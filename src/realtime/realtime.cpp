#include <condition_variable>
#include <cstdint>

#include <supabase/realtime/realtime.hpp>
#include <supabase/ws.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Phoenix channels protocol v1 over a text websocket (`vsn=1.0.0`).
// Lock order: RealtimeChannel::Impl::mu, then Core::m_mu. Core never calls into a channel while holding m_mu.
namespace supabase::realtime
{

namespace
{
    using Clock = std::chrono::steady_clock;
    using Calls = std::vector<std::function<void()>>;

    constexpr auto kHeartbeatInterval = std::chrono::seconds(25);
    constexpr auto kJoinTimeout       = std::chrono::seconds(10);
    constexpr auto kRejoinDelay       = std::chrono::seconds(2);
    constexpr auto kConnectTimeout    = std::chrono::seconds(10);
    constexpr auto kPollInterval      = std::chrono::milliseconds(50);

    constexpr std::array<std::chrono::milliseconds, 4> kReconnectBackoff = {
        std::chrono::milliseconds(1000),
        std::chrono::milliseconds(2000),
        std::chrono::milliseconds(5000),
        std::chrono::milliseconds(10000),
    };

    std::string str(const Json& object, const char* key)
    {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    Json field(const Json& object, const char* key, const Json& fallback = Json::object())
    {
        const auto it = object.find(key);
        return it != object.end() && !it->is_null() ? *it : fallback;
    }

    std::string dump(const Json& json) { return json.dump(-1, ' ', false, Json::error_handler_t::replace); }

    std::string
    makeFrame(const std::string& topic, std::string_view event, Json payload, const std::string& ref, const std::string& joinRef = {})
    {
        Json frame = { { "topic", topic }, { "event", event }, { "payload", std::move(payload) }, { "ref", ref } };
        if (!joinRef.empty())
            frame["join_ref"] = joinRef;
        return dump(frame);
    }

    void runCalls(const Calls& calls)
    {
        for (const auto& call : calls)
        {
            try
            {
                call();
            }
            catch (...)
            {
                // A throwing user callback must not kill the worker.
            }
        }
    }

    struct Binding
    {
        enum class Kind
        {
            Postgres,
            Broadcast,
            Presence,
            Raw
        };

        Kind kind = Kind::Raw;
        PostgresChangesFilter filter;
        std::string event;
        PresenceEvent presence = PresenceEvent::Sync;
        std::int64_t id        = -1; ///< server-assigned, Postgres only
        EventCallback callback;
    };
}

namespace detail
{
    class Core : public std::enable_shared_from_this<Core>
    {
      public:
        explicit Core(std::shared_ptr<const Context> ctx) : m_ctx(std::move(ctx)) {}
        ~Core()
        {
            stop();
            if (m_worker.joinable()) // destroyed from the worker's own callback
                m_worker.detach();
        }

        std::shared_ptr<RealtimeChannel> channel(std::string_view name, ChannelOptions options);
        Result<void> start();
        void stop();
        void push(std::string frame);
        void setAuth(std::string token);
        Result<void> removeChannel(const std::shared_ptr<RealtimeChannel>& channel);
        void removeAll();

        std::string nextRef() { return std::to_string(++m_ref); }
        std::string token() const;
        bool connected() const noexcept { return m_connected; }

      private:
        std::vector<std::shared_ptr<RealtimeChannel>> channels() const;
        std::string url() const;
        void run();
        void session(ws::Socket& socket);
        void dispatch(std::string_view raw);
        void closeChannels();

        std::shared_ptr<const Context> m_ctx;
        std::mutex m_lifeMu; ///< serializes start/stop
        std::thread m_worker;
        std::atomic<std::thread::id> m_workerId; ///< lets callbacks on the worker skip m_lifeMu
        std::atomic_bool m_stop { false };
        std::atomic_bool m_connected { false };
        std::atomic<std::uint64_t> m_ref { 0 };

        mutable std::mutex m_mu; ///< guards the members below
        std::condition_variable m_wake;
        std::vector<std::string> m_outbox;
        std::vector<std::shared_ptr<RealtimeChannel>> m_channels;
        std::string m_authOverride;

        std::string m_heartbeatRef; ///< worker thread only
    };
}

struct RealtimeChannel::Impl
{
    Impl(std::weak_ptr<detail::Core> core, std::string topic, ChannelOptions options)
        : core(std::move(core)), topic(std::move(topic)), options(std::move(options))
    {
    }

    std::weak_ptr<detail::Core> core;
    const std::string topic;
    const ChannelOptions options;

    mutable std::mutex mu;
    std::vector<Binding> bindings;
    ChannelState state = ChannelState::Closed;
    bool wantJoin      = false; ///< the user asked to be subscribed
    bool joinPending   = false; ///< a phx_join must be (re)sent
    std::string joinRef;
    Clock::time_point joinDeadline;
    Clock::time_point rejoinAt;
    SubscribeCallback onStatus;
    Json presence = Json::object();

    void addBinding(Binding binding)
    {
        std::lock_guard lock(mu);
        bindings.push_back(std::move(binding));
    }

    static Calls statusCall(const SubscribeCallback& callback, SubscribeStatus status, std::string message = {})
    {
        Calls calls;
        if (callback)
            calls.emplace_back([callback, status, message = std::move(message)] { callback(status, message); });
        return calls;
    }

    std::optional<std::string> takeJoinFrame(Clock::time_point now, detail::Core& owner)
    {
        std::lock_guard lock(mu);
        if (!wantJoin || !joinPending || now < rejoinAt)
            return std::nullopt;

        Json postgres = Json::array();
        for (const auto& binding : bindings)
        {
            if (binding.kind != Binding::Kind::Postgres)
                continue;
            Json entry = { { "event", binding.filter.event }, { "schema", binding.filter.schema } };
            if (!binding.filter.table.empty())
                entry["table"] = binding.filter.table;
            if (!binding.filter.filter.empty())
                entry["filter"] = binding.filter.filter;
            postgres.push_back(std::move(entry));
        }
        Json config = { { "broadcast", { { "self", options.broadcastSelf }, { "ack", options.broadcastAck } } },
                        { "presence", { { "key", options.presenceKey } } },
                        { "postgres_changes", std::move(postgres) },
                        { "private", options.isPrivate } };

        joinPending  = false;
        state        = ChannelState::Joining;
        joinRef      = owner.nextRef();
        joinDeadline = now + kJoinTimeout;
        return makeFrame(topic, "phx_join", { { "config", std::move(config) }, { "access_token", owner.token() } }, joinRef, joinRef);
    }

    void checkTimeout(Clock::time_point now)
    {
        Calls calls;
        {
            std::lock_guard lock(mu);
            if (state != ChannelState::Joining || joinRef.empty() || now < joinDeadline)
                return;
            state = ChannelState::Errored;
            joinRef.clear();
            joinPending = wantJoin;
            rejoinAt    = now + kRejoinDelay;
            calls       = statusCall(onStatus, SubscribeStatus::TimedOut, "join timed out");
        }
        runCalls(calls);
    }

    void onDisconnected()
    {
        std::lock_guard lock(mu);
        if (!wantJoin)
            return;
        state       = ChannelState::Errored;
        joinPending = true;
        rejoinAt    = Clock::time_point {};
        joinRef.clear();
    }

    void presenceCalls(PresenceEvent event, const Json& payload, Calls& calls) const
    {
        auto shared = std::make_shared<const Json>(payload);
        for (const auto& binding : bindings)
            if (binding.kind == Binding::Kind::Presence && binding.presence == event)
                calls.emplace_back([callback = binding.callback, shared] { callback(*shared); });
    }

    // Called with `mu` held.
    void applyPresenceState(const Json& incoming, Calls& calls)
    {
        const Json previous = std::move(presence);
        presence            = incoming.is_object() ? incoming : Json::object();
        for (const auto& [key, value] : presence.items())
            if (!previous.contains(key))
                presenceCalls(PresenceEvent::Join, { { "key", key }, { "newPresences", field(value, "metas", Json::array()) } }, calls);
        for (const auto& [key, value] : previous.items())
            if (!presence.contains(key))
                presenceCalls(PresenceEvent::Leave, { { "key", key }, { "leftPresences", field(value, "metas", Json::array()) } }, calls);
        presenceCalls(PresenceEvent::Sync, presence, calls);
    }

    // Called with `mu` held.
    void applyPresenceDiff(const Json& joins, const Json& leaves, Calls& calls)
    {
        if (joins.is_object())
            for (const auto& [key, value] : joins.items())
            {
                const Json metas = field(value, "metas", Json::array());
                Json& current    = presence[key];
                if (!current.is_object())
                    current = { { "metas", Json::array() } };
                Json& list = current["metas"];
                for (const auto& meta : metas)
                {
                    const bool known = std::any_of(
                        list.begin(),
                        list.end(),
                        [&](const Json& existing)
                        { return !str(meta, "phx_ref").empty() && str(existing, "phx_ref") == str(meta, "phx_ref"); }
                    );
                    if (!known)
                        list.push_back(meta);
                }
                presenceCalls(PresenceEvent::Join, { { "key", key }, { "newPresences", metas } }, calls);
            }
        if (leaves.is_object())
            for (const auto& [key, value] : leaves.items())
            {
                const Json metas = field(value, "metas", Json::array());
                if (presence.contains(key) && presence[key].is_object())
                {
                    Json& list = presence[key]["metas"];
                    Json kept  = Json::array();
                    for (const auto& existing : list)
                    {
                        const bool left = std::any_of(
                            metas.begin(), metas.end(), [&](const Json& meta) { return str(meta, "phx_ref") == str(existing, "phx_ref"); }
                        );
                        if (!left)
                            kept.push_back(existing);
                    }
                    if (kept.empty())
                        presence.erase(key);
                    else
                        list = std::move(kept);
                }
                presenceCalls(PresenceEvent::Leave, { { "key", key }, { "leftPresences", metas } }, calls);
            }
        presenceCalls(PresenceEvent::Sync, presence, calls);
    }

    // Called with `mu` held.
    void onJoinReply(const Json& payload, Calls& calls)
    {
        const Json response = field(payload, "response");
        if (str(payload, "status") != "ok")
        {
            state              = ChannelState::Errored;
            wantJoin           = false; // a rejected join (bad filter, RLS) will not heal by retrying
            std::string reason = str(response, "reason");
            if (reason.empty())
                reason = str(response, "message");
            if (reason.empty())
                reason = dump(response);
            calls = statusCall(onStatus, SubscribeStatus::ChannelError, std::move(reason));
            return;
        }
        state               = ChannelState::Joined;
        const Json assigned = field(response, "postgres_changes", Json::array());
        std::size_t index   = 0;
        for (auto& binding : bindings)
        {
            if (binding.kind != Binding::Kind::Postgres)
                continue;
            if (assigned.is_array() && index < assigned.size() && assigned[index].is_object())
                binding.id = assigned[index].value("id", static_cast<std::int64_t>(-1));
            ++index;
        }
        calls = statusCall(onStatus, SubscribeStatus::Subscribed);
    }

    void handle(const Json& message)
    {
        const std::string event = str(message, "event");
        const Json payload      = field(message, "payload");
        Calls calls;
        {
            std::lock_guard lock(mu);
            if (event == "phx_reply")
            {
                if (state == ChannelState::Joining && !joinRef.empty() && str(message, "ref") == joinRef)
                    onJoinReply(payload, calls);
            }
            else if (event == "phx_close")
            {
                if (wantJoin)
                    calls = statusCall(onStatus, SubscribeStatus::Closed);
                wantJoin = false;
                state    = ChannelState::Closed;
            }
            else if (event == "phx_error")
            {
                if (wantJoin)
                {
                    state       = ChannelState::Errored;
                    joinPending = true;
                    rejoinAt    = Clock::now() + kRejoinDelay;
                    joinRef.clear();
                    calls = statusCall(onStatus, SubscribeStatus::ChannelError, "channel crashed on the server");
                }
            }
            else if (event == "postgres_changes")
            {
                const Json data        = field(payload, "data");
                const Json ids         = field(payload, "ids", Json::array());
                const std::string type = str(data, "type");
                auto mapped            = std::make_shared<const Json>(Json { { "schema", str(data, "schema") },
                                                                             { "table", str(data, "table") },
                                                                             { "commit_timestamp", str(data, "commit_timestamp") },
                                                                             { "eventType", type },
                                                                             { "new", field(data, "record") },
                                                                             { "old", field(data, "old_record") },
                                                                             { "errors", field(data, "errors", Json()) } });
                for (const auto& binding : bindings)
                {
                    if (binding.kind != Binding::Kind::Postgres || (binding.filter.event != "*" && binding.filter.event != type))
                        continue;
                    const bool matches
                        = ids.is_array() && std::any_of(ids.begin(), ids.end(), [&](const Json& id) { return id == binding.id; });
                    if (matches)
                        calls.emplace_back([callback = binding.callback, mapped] { callback(*mapped); });
                }
            }
            else if (event == "broadcast")
            {
                const std::string name = str(payload, "event");
                auto shared            = std::make_shared<const Json>(payload);
                for (const auto& binding : bindings)
                    if (binding.kind == Binding::Kind::Broadcast && (binding.event == "*" || binding.event == name))
                        calls.emplace_back([callback = binding.callback, shared] { callback(*shared); });
            }
            else if (event == "presence_state")
            {
                applyPresenceState(payload, calls);
            }
            else if (event == "presence_diff")
            {
                applyPresenceDiff(field(payload, "joins"), field(payload, "leaves"), calls);
            }

            auto shared = std::make_shared<const Json>(payload);
            for (const auto& binding : bindings)
                if (binding.kind == Binding::Kind::Raw && (binding.event == "*" || binding.event == event))
                    calls.emplace_back([callback = binding.callback, shared] { callback(*shared); });
        }
        runCalls(calls);
    }
};

RealtimeChannel::RealtimeChannel(std::weak_ptr<detail::Core> core, std::string topic, ChannelOptions options)
    : m_impl(std::make_unique<Impl>(std::move(core), std::move(topic), std::move(options)))
{
}

RealtimeChannel::~RealtimeChannel() = default;

RealtimeChannel& RealtimeChannel::onPostgresChanges(PostgresChangesFilter filter, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Postgres;
    binding.filter   = std::move(filter);
    binding.callback = std::move(callback);
    m_impl->addBinding(std::move(binding));
    return *this;
}

RealtimeChannel& RealtimeChannel::onBroadcast(std::string_view event, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Broadcast;
    binding.event    = std::string(event);
    binding.callback = std::move(callback);
    m_impl->addBinding(std::move(binding));
    return *this;
}

RealtimeChannel& RealtimeChannel::onPresence(PresenceEvent event, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Presence;
    binding.presence = event;
    binding.callback = std::move(callback);
    m_impl->addBinding(std::move(binding));
    return *this;
}

RealtimeChannel& RealtimeChannel::on(std::string_view event, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Raw;
    binding.event    = std::string(event);
    binding.callback = std::move(callback);
    m_impl->addBinding(std::move(binding));
    return *this;
}

Result<void> RealtimeChannel::subscribe(SubscribeCallback callback)
{
    const auto core = m_impl->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    {
        std::lock_guard lock(m_impl->mu);
        if (m_impl->wantJoin)
            return makeError(errc::InvalidArgument, "channel is already subscribed");
        m_impl->wantJoin    = true;
        m_impl->joinPending = true;
        m_impl->state       = ChannelState::Joining;
        m_impl->rejoinAt    = Clock::time_point {};
        m_impl->joinRef.clear();
        m_impl->onStatus = std::move(callback);
    }
    auto started = core->start();
    if (!started.ok())
    {
        std::lock_guard lock(m_impl->mu);
        m_impl->wantJoin    = false;
        m_impl->joinPending = false;
        m_impl->state       = ChannelState::Closed;
    }
    return started;
}

Result<void> RealtimeChannel::unsubscribe()
{
    const auto core = m_impl->core.lock();
    SubscribeCallback callback;
    std::string leaveFrame;
    {
        std::lock_guard lock(m_impl->mu);
        if (!m_impl->wantJoin && m_impl->state == ChannelState::Closed)
            return {};
        if (core && !m_impl->joinRef.empty())
            leaveFrame = makeFrame(m_impl->topic, "phx_leave", Json::object(), core->nextRef(), m_impl->joinRef);
        m_impl->wantJoin    = false;
        m_impl->joinPending = false;
        m_impl->state       = ChannelState::Closed;
        m_impl->joinRef.clear();
        m_impl->presence = Json::object();
        callback         = m_impl->onStatus;
    }
    if (core && !leaveFrame.empty())
        core->push(std::move(leaveFrame));
    runCalls(Impl::statusCall(callback, SubscribeStatus::Closed));
    return {};
}

Result<void> RealtimeChannel::send(std::string_view event, const Json& payload)
{
    const auto core = m_impl->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    std::string frame;
    {
        std::lock_guard lock(m_impl->mu);
        if (m_impl->state != ChannelState::Joined)
            return makeError(errc::InvalidArgument, "channel is not joined");
        frame = makeFrame(
            m_impl->topic,
            "broadcast",
            { { "type", "broadcast" }, { "event", event }, { "payload", payload } },
            core->nextRef(),
            m_impl->joinRef
        );
    }
    core->push(std::move(frame));
    return {};
}

Result<void> RealtimeChannel::track(const Json& payload)
{
    const auto core = m_impl->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    std::string frame;
    {
        std::lock_guard lock(m_impl->mu);
        if (m_impl->state != ChannelState::Joined)
            return makeError(errc::InvalidArgument, "channel is not joined");
        frame = makeFrame(
            m_impl->topic,
            "presence",
            { { "type", "presence" }, { "event", "track" }, { "payload", payload } },
            core->nextRef(),
            m_impl->joinRef
        );
    }
    core->push(std::move(frame));
    return {};
}

Result<void> RealtimeChannel::untrack()
{
    const auto core = m_impl->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    std::string frame;
    {
        std::lock_guard lock(m_impl->mu);
        if (m_impl->state != ChannelState::Joined)
            return makeError(errc::InvalidArgument, "channel is not joined");
        frame = makeFrame(m_impl->topic, "presence", { { "type", "presence" }, { "event", "untrack" } }, core->nextRef(), m_impl->joinRef);
    }
    core->push(std::move(frame));
    return {};
}

Json RealtimeChannel::presenceState() const
{
    std::lock_guard lock(m_impl->mu);
    return m_impl->presence;
}

ChannelState RealtimeChannel::state() const
{
    std::lock_guard lock(m_impl->mu);
    return m_impl->state;
}

const std::string& RealtimeChannel::topic() const noexcept { return m_impl->topic; }

namespace detail
{
    std::shared_ptr<RealtimeChannel> Core::channel(std::string_view name, ChannelOptions options)
    {
        std::string topic = name.rfind("realtime:", 0) == 0 ? std::string(name) : "realtime:" + std::string(name);
        std::lock_guard lock(m_mu);
        for (const auto& existing : m_channels)
            if (existing->topic() == topic)
                return existing;
        std::shared_ptr<RealtimeChannel> created(new RealtimeChannel(weak_from_this(), std::move(topic), std::move(options)));
        m_channels.push_back(created);
        return created;
    }

    Result<void> Core::start()
    {
        if (m_workerId == std::this_thread::get_id()) // called from a callback
            return m_stop ? Result<void>(makeError(errc::InvalidArgument, "realtime is disconnecting")) : Result<void>();
        std::lock_guard life(m_lifeMu);
        if (m_worker.joinable())
        {
            if (!m_stop)
                return {};
            m_worker.join();
            m_workerId = std::thread::id();
        }
        if (!m_ctx->socketFactory)
            return makeError(errc::NotImplemented, "no websocket factory configured");
        m_stop     = false;
        m_worker   = std::thread([this] { run(); });
        m_workerId = m_worker.get_id();
        return {};
    }

    void Core::stop()
    {
        const bool onWorker = m_workerId == std::this_thread::get_id();
        std::unique_lock life(m_lifeMu, std::defer_lock);
        if (!onWorker)
            life.lock();
        m_stop = true;
        {
            std::lock_guard lock(m_mu); // pairs with the wait predicate so the wake-up is not lost
        }
        m_wake.notify_all();
        if (onWorker || !m_worker.joinable())
            return; // from a callback the loop exits on its own; the next start/stop joins
        m_worker.join();
        m_workerId = std::thread::id();
    }

    void Core::push(std::string frame)
    {
        std::lock_guard lock(m_mu);
        m_outbox.push_back(std::move(frame));
    }

    std::string Core::token() const
    {
        {
            std::lock_guard lock(m_mu);
            if (!m_authOverride.empty())
                return m_authOverride;
        }
        return m_ctx->token();
    }

    void Core::setAuth(std::string token)
    {
        {
            std::lock_guard lock(m_mu);
            m_authOverride = std::move(token);
        }
        const std::string current = this->token();
        for (const auto& channel : channels())
        {
            std::string frame;
            {
                std::lock_guard lock(channel->m_impl->mu);
                if (channel->m_impl->state != ChannelState::Joined)
                    continue;
                frame = makeFrame(
                    channel->m_impl->topic, "access_token", { { "access_token", current } }, nextRef(), channel->m_impl->joinRef
                );
            }
            push(std::move(frame));
        }
    }

    Result<void> Core::removeChannel(const std::shared_ptr<RealtimeChannel>& channel)
    {
        if (!channel)
            return makeError(errc::InvalidArgument, "channel is null");
        auto result = channel->unsubscribe();
        std::lock_guard lock(m_mu);
        m_channels.erase(std::remove(m_channels.begin(), m_channels.end(), channel), m_channels.end());
        return result;
    }

    void Core::removeAll()
    {
        for (const auto& channel : channels())
            (void) removeChannel(channel);
    }

    std::vector<std::shared_ptr<RealtimeChannel>> Core::channels() const
    {
        std::lock_guard lock(m_mu);
        return m_channels;
    }

    std::string Core::url() const
    {
        std::string base = m_ctx->url;
        if (base.rfind("https://", 0) == 0)
            base.replace(0, 8, "wss://");
        else if (base.rfind("http://", 0) == 0)
            base.replace(0, 7, "ws://");
        return base + "/realtime/v1/websocket?apikey=" + m_ctx->anonKey + "&vsn=1.0.0";
    }

    void Core::run()
    {
        std::size_t attempt = 0;
        while (!m_stop)
        {
            auto socket = m_ctx->socketFactory();
            auto opened = socket ? socket->connect(url(), m_ctx->headers, kConnectTimeout)
                                 : Result<void>(makeError(errc::Network, "websocket factory returned null"));
            if (opened.ok())
            {
                attempt     = 0;
                m_connected = true;
                session(*socket);
                m_connected = false;
                socket->close();
                for (const auto& channel : channels())
                    channel->m_impl->onDisconnected();
            }
            if (m_stop)
                break;
            std::unique_lock lock(m_mu);
            m_wake.wait_for(
                lock, kReconnectBackoff[std::min<std::size_t>(attempt++, kReconnectBackoff.size() - 1)], [this] { return m_stop.load(); }
            );
        }
        closeChannels();
    }

    void Core::session(ws::Socket& socket)
    {
        {
            std::lock_guard lock(m_mu);
            m_outbox.clear(); // anything queued for the previous connection is stale
        }
        m_heartbeatRef.clear();
        auto nextHeartbeat = Clock::now() + kHeartbeatInterval;

        while (!m_stop)
        {
            std::vector<std::string> out;
            {
                std::lock_guard lock(m_mu);
                out.swap(m_outbox);
            }
            const auto now = Clock::now();
            for (const auto& channel : channels())
            {
                channel->m_impl->checkTimeout(now);
                if (auto join = channel->m_impl->takeJoinFrame(now, *this))
                    out.push_back(std::move(*join));
            }
            if (now >= nextHeartbeat)
            {
                if (!m_heartbeatRef.empty())
                    return; // previous heartbeat was never answered: the link is dead
                m_heartbeatRef = nextRef();
                out.push_back(makeFrame("phoenix", "heartbeat", Json::object(), m_heartbeatRef));
                nextHeartbeat = now + kHeartbeatInterval;
            }
            for (const auto& frame : out)
                if (!socket.send(frame).ok())
                    return;

            auto received = socket.receive(kPollInterval);
            if (!received.ok())
                return;
            if (received.value().has_value())
                dispatch(*received.value());
        }
    }

    void Core::dispatch(std::string_view raw)
    {
        const Json message = Json::parse(raw.begin(), raw.end(), nullptr, false);
        if (!message.is_object())
            return;
        const std::string topic = str(message, "topic");
        if (topic == "phoenix")
        {
            if (str(message, "event") == "phx_reply" && str(message, "ref") == m_heartbeatRef)
                m_heartbeatRef.clear();
            return;
        }
        for (const auto& channel : channels())
            if (channel->m_impl->topic == topic)
            {
                channel->m_impl->handle(message);
                return;
            }
    }

    void Core::closeChannels()
    {
        for (const auto& channel : channels())
        {
            SubscribeCallback callback;
            bool wasActive = false;

            {
                std::lock_guard lock(channel->m_impl->mu);
                wasActive                    = channel->m_impl->wantJoin;
                callback                     = channel->m_impl->onStatus;
                channel->m_impl->wantJoin    = false;
                channel->m_impl->joinPending = false;
                channel->m_impl->state       = ChannelState::Closed;
                channel->m_impl->joinRef.clear();
            }

            if (wasActive)
                runCalls(RealtimeChannel::Impl::statusCall(callback, SubscribeStatus::Closed));
        }
    }
}

RealtimeClient::RealtimeClient(std::shared_ptr<const Context> ctx) : m_core(std::make_shared<detail::Core>(std::move(ctx))) {}

std::shared_ptr<RealtimeChannel> RealtimeClient::channel(std::string_view name, ChannelOptions options) const
{
    return m_core->channel(name, std::move(options));
}

Result<void> RealtimeClient::connect() const { return m_core->start(); }
void RealtimeClient::disconnect() const { m_core->stop(); }
bool RealtimeClient::isConnected() const { return m_core->connected(); }
void RealtimeClient::setAuth(std::string token) const { m_core->setAuth(std::move(token)); }
Result<void> RealtimeClient::removeChannel(const std::shared_ptr<RealtimeChannel>& channel) const { return m_core->removeChannel(channel); }
void RealtimeClient::removeAllChannels() const { m_core->removeAll(); }

}
