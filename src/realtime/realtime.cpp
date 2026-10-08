#include <condition_variable>
#include <cstdint>

#include <supabase/realtime/realtime.hpp>
#include <supabase/ws.hpp>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Phoenix channels protocol v1 over a text websocket (`vsn=1.0.0`).
// Lock order: RealtimeChannel::Impl::mu, then Core::mu_. Core never calls into a channel while holding mu_.
namespace supabase::realtime
{

namespace
{
    using Clock = std::chrono::steady_clock;
    using Calls = std::vector<std::function<void()>>;

    constexpr auto kHeartbeatInterval                       = std::chrono::seconds(25);
    constexpr auto kJoinTimeout                             = std::chrono::seconds(10);
    constexpr auto kRejoinDelay                             = std::chrono::seconds(2);
    constexpr auto kConnectTimeout                          = std::chrono::seconds(10);
    constexpr auto kPollInterval                            = std::chrono::milliseconds(50);
    constexpr std::chrono::milliseconds kReconnectBackoff[] = {
        std::chrono::milliseconds(1000), std::chrono::milliseconds(2000), std::chrono::milliseconds(5000), std::chrono::milliseconds(10000)
    };

    std::string str(const Json& object, const char* key)
    {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    Json field(const Json& object, const char* key, Json fallback = Json::object())
    {
        const auto it = object.find(key);
        return it != object.end() && !it->is_null() ? *it : std::move(fallback);
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
        explicit Core(std::shared_ptr<const Context> ctx) : ctx_(std::move(ctx)) {}
        ~Core()
        {
            stop();
            if (worker_.joinable()) // destroyed from the worker's own callback
                worker_.detach();
        }

        std::shared_ptr<RealtimeChannel> channel(std::string_view name, ChannelOptions options);
        Result<void> start();
        void stop();
        void push(std::string frame);
        void setAuth(std::string token);
        Result<void> removeChannel(const std::shared_ptr<RealtimeChannel>& channel);
        void removeAll();

        std::string nextRef() { return std::to_string(++ref_); }
        std::string token() const;
        bool connected() const noexcept { return connected_; }

      private:
        std::vector<std::shared_ptr<RealtimeChannel>> channels() const;
        std::string url() const;
        void run();
        void session(ws::Socket& socket);
        void dispatch(std::string_view raw);
        void closeChannels();

        std::shared_ptr<const Context> ctx_;
        std::mutex lifeMu_; ///< serializes start/stop
        std::thread worker_;
        std::atomic<std::thread::id> workerId_ {}; ///< lets callbacks on the worker skip lifeMu_
        std::atomic_bool stop_ { false };
        std::atomic_bool connected_ { false };
        std::atomic<std::uint64_t> ref_ { 0 };

        mutable std::mutex mu_; ///< guards the members below
        std::condition_variable wake_;
        std::vector<std::string> outbox_;
        std::vector<std::shared_ptr<RealtimeChannel>> channels_;
        std::string authOverride_;

        std::string heartbeatRef_; ///< worker thread only
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
    Clock::time_point joinDeadline {};
    Clock::time_point rejoinAt {};
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
    : impl_(std::make_unique<Impl>(std::move(core), std::move(topic), std::move(options)))
{
}

RealtimeChannel::~RealtimeChannel() = default;

RealtimeChannel& RealtimeChannel::onPostgresChanges(PostgresChangesFilter filter, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Postgres;
    binding.filter   = std::move(filter);
    binding.callback = std::move(callback);
    impl_->addBinding(std::move(binding));
    return *this;
}

RealtimeChannel& RealtimeChannel::onBroadcast(std::string_view event, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Broadcast;
    binding.event    = std::string(event);
    binding.callback = std::move(callback);
    impl_->addBinding(std::move(binding));
    return *this;
}

RealtimeChannel& RealtimeChannel::onPresence(PresenceEvent event, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Presence;
    binding.presence = event;
    binding.callback = std::move(callback);
    impl_->addBinding(std::move(binding));
    return *this;
}

RealtimeChannel& RealtimeChannel::on(std::string_view event, EventCallback callback)
{
    Binding binding;
    binding.kind     = Binding::Kind::Raw;
    binding.event    = std::string(event);
    binding.callback = std::move(callback);
    impl_->addBinding(std::move(binding));
    return *this;
}

Result<void> RealtimeChannel::subscribe(SubscribeCallback callback)
{
    const auto core = impl_->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    {
        std::lock_guard lock(impl_->mu);
        if (impl_->wantJoin)
            return makeError(errc::InvalidArgument, "channel is already subscribed");
        impl_->wantJoin    = true;
        impl_->joinPending = true;
        impl_->state       = ChannelState::Joining;
        impl_->rejoinAt    = Clock::time_point {};
        impl_->joinRef.clear();
        impl_->onStatus = std::move(callback);
    }
    auto started = core->start();
    if (!started.ok())
    {
        std::lock_guard lock(impl_->mu);
        impl_->wantJoin    = false;
        impl_->joinPending = false;
        impl_->state       = ChannelState::Closed;
    }
    return started;
}

Result<void> RealtimeChannel::unsubscribe()
{
    const auto core = impl_->core.lock();
    SubscribeCallback callback;
    std::string leaveFrame;
    {
        std::lock_guard lock(impl_->mu);
        if (!impl_->wantJoin && impl_->state == ChannelState::Closed)
            return {};
        if (core && !impl_->joinRef.empty())
            leaveFrame = makeFrame(impl_->topic, "phx_leave", Json::object(), core->nextRef(), impl_->joinRef);
        impl_->wantJoin    = false;
        impl_->joinPending = false;
        impl_->state       = ChannelState::Closed;
        impl_->joinRef.clear();
        impl_->presence = Json::object();
        callback        = impl_->onStatus;
    }
    if (core && !leaveFrame.empty())
        core->push(std::move(leaveFrame));
    runCalls(Impl::statusCall(callback, SubscribeStatus::Closed));
    return {};
}

Result<void> RealtimeChannel::send(std::string_view event, const Json& payload)
{
    const auto core = impl_->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    std::string frame;
    {
        std::lock_guard lock(impl_->mu);
        if (impl_->state != ChannelState::Joined)
            return makeError(errc::InvalidArgument, "channel is not joined");
        frame = makeFrame(
            impl_->topic,
            "broadcast",
            { { "type", "broadcast" }, { "event", event }, { "payload", payload } },
            core->nextRef(),
            impl_->joinRef
        );
    }
    core->push(std::move(frame));
    return {};
}

Result<void> RealtimeChannel::track(const Json& payload)
{
    const auto core = impl_->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    std::string frame;
    {
        std::lock_guard lock(impl_->mu);
        if (impl_->state != ChannelState::Joined)
            return makeError(errc::InvalidArgument, "channel is not joined");
        frame = makeFrame(
            impl_->topic,
            "presence",
            { { "type", "presence" }, { "event", "track" }, { "payload", payload } },
            core->nextRef(),
            impl_->joinRef
        );
    }
    core->push(std::move(frame));
    return {};
}

Result<void> RealtimeChannel::untrack()
{
    const auto core = impl_->core.lock();
    if (!core)
        return makeError(errc::Cancelled, "realtime client was destroyed");
    std::string frame;
    {
        std::lock_guard lock(impl_->mu);
        if (impl_->state != ChannelState::Joined)
            return makeError(errc::InvalidArgument, "channel is not joined");
        frame = makeFrame(impl_->topic, "presence", { { "type", "presence" }, { "event", "untrack" } }, core->nextRef(), impl_->joinRef);
    }
    core->push(std::move(frame));
    return {};
}

Json RealtimeChannel::presenceState() const
{
    std::lock_guard lock(impl_->mu);
    return impl_->presence;
}

ChannelState RealtimeChannel::state() const
{
    std::lock_guard lock(impl_->mu);
    return impl_->state;
}

const std::string& RealtimeChannel::topic() const noexcept { return impl_->topic; }

namespace detail
{
    std::shared_ptr<RealtimeChannel> Core::channel(std::string_view name, ChannelOptions options)
    {
        std::string topic = name.rfind("realtime:", 0) == 0 ? std::string(name) : "realtime:" + std::string(name);
        std::lock_guard lock(mu_);
        for (const auto& existing : channels_)
            if (existing->topic() == topic)
                return existing;
        std::shared_ptr<RealtimeChannel> created(new RealtimeChannel(weak_from_this(), std::move(topic), std::move(options)));
        channels_.push_back(created);
        return created;
    }

    Result<void> Core::start()
    {
        if (workerId_ == std::this_thread::get_id()) // called from a callback
            return stop_ ? Result<void>(makeError(errc::InvalidArgument, "realtime is disconnecting")) : Result<void>();
        std::lock_guard life(lifeMu_);
        if (worker_.joinable())
        {
            if (!stop_)
                return {};
            worker_.join();
            workerId_ = std::thread::id();
        }
        if (!ctx_->socketFactory)
            return makeError(errc::NotImplemented, "no websocket factory configured");
        stop_     = false;
        worker_   = std::thread([this] { run(); });
        workerId_ = worker_.get_id();
        return {};
    }

    void Core::stop()
    {
        const bool onWorker = workerId_ == std::this_thread::get_id();
        std::unique_lock life(lifeMu_, std::defer_lock);
        if (!onWorker)
            life.lock();
        stop_ = true;
        {
            std::lock_guard lock(mu_); // pairs with the wait predicate so the wake-up is not lost
        }
        wake_.notify_all();
        if (onWorker || !worker_.joinable())
            return; // from a callback the loop exits on its own; the next start/stop joins
        worker_.join();
        workerId_ = std::thread::id();
    }

    void Core::push(std::string frame)
    {
        std::lock_guard lock(mu_);
        outbox_.push_back(std::move(frame));
    }

    std::string Core::token() const
    {
        {
            std::lock_guard lock(mu_);
            if (!authOverride_.empty())
                return authOverride_;
        }
        return ctx_->token();
    }

    void Core::setAuth(std::string token)
    {
        {
            std::lock_guard lock(mu_);
            authOverride_ = std::move(token);
        }
        const std::string current = this->token();
        for (const auto& channel : channels())
        {
            std::string frame;
            {
                std::lock_guard lock(channel->impl_->mu);
                if (channel->impl_->state != ChannelState::Joined)
                    continue;
                frame
                    = makeFrame(channel->impl_->topic, "access_token", { { "access_token", current } }, nextRef(), channel->impl_->joinRef);
            }
            push(std::move(frame));
        }
    }

    Result<void> Core::removeChannel(const std::shared_ptr<RealtimeChannel>& channel)
    {
        if (!channel)
            return makeError(errc::InvalidArgument, "channel is null");
        auto result = channel->unsubscribe();
        std::lock_guard lock(mu_);
        channels_.erase(std::remove(channels_.begin(), channels_.end(), channel), channels_.end());
        return result;
    }

    void Core::removeAll()
    {
        for (const auto& channel : channels())
            (void) removeChannel(channel);
    }

    std::vector<std::shared_ptr<RealtimeChannel>> Core::channels() const
    {
        std::lock_guard lock(mu_);
        return channels_;
    }

    std::string Core::url() const
    {
        std::string base = ctx_->url;
        if (base.rfind("https://", 0) == 0)
            base.replace(0, 8, "wss://");
        else if (base.rfind("http://", 0) == 0)
            base.replace(0, 7, "ws://");
        return base + "/realtime/v1/websocket?apikey=" + ctx_->anonKey + "&vsn=1.0.0";
    }

    void Core::run()
    {
        std::size_t attempt = 0;
        while (!stop_)
        {
            auto socket = ctx_->socketFactory();
            auto opened = socket ? socket->connect(url(), ctx_->headers, kConnectTimeout)
                                 : Result<void>(makeError(errc::Network, "websocket factory returned null"));
            if (opened.ok())
            {
                attempt    = 0;
                connected_ = true;
                session(*socket);
                connected_ = false;
                socket->close();
                for (const auto& channel : channels())
                    channel->impl_->onDisconnected();
            }
            if (stop_)
                break;
            std::unique_lock lock(mu_);
            wake_.wait_for(
                lock, kReconnectBackoff[std::min<std::size_t>(attempt++, std::size(kReconnectBackoff) - 1)], [this] { return stop_.load(); }
            );
        }
        closeChannels();
    }

    void Core::session(ws::Socket& socket)
    {
        {
            std::lock_guard lock(mu_);
            outbox_.clear(); // anything queued for the previous connection is stale
        }
        heartbeatRef_.clear();
        auto nextHeartbeat = Clock::now() + kHeartbeatInterval;

        while (!stop_)
        {
            std::vector<std::string> out;
            {
                std::lock_guard lock(mu_);
                out.swap(outbox_);
            }
            const auto now = Clock::now();
            for (const auto& channel : channels())
            {
                channel->impl_->checkTimeout(now);
                if (auto join = channel->impl_->takeJoinFrame(now, *this))
                    out.push_back(std::move(*join));
            }
            if (now >= nextHeartbeat)
            {
                if (!heartbeatRef_.empty())
                    return; // previous heartbeat was never answered: the link is dead
                heartbeatRef_ = nextRef();
                out.push_back(makeFrame("phoenix", "heartbeat", Json::object(), heartbeatRef_));
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
            if (str(message, "event") == "phx_reply" && str(message, "ref") == heartbeatRef_)
                heartbeatRef_.clear();
            return;
        }
        for (const auto& channel : channels())
            if (channel->impl_->topic == topic)
            {
                channel->impl_->handle(message);
                return;
            }
    }

    void Core::closeChannels()
    {
        for (const auto& channel : channels())
        {
            SubscribeCallback callback;
            bool wasActive;
            {
                std::lock_guard lock(channel->impl_->mu);
                wasActive                   = channel->impl_->wantJoin;
                callback                    = channel->impl_->onStatus;
                channel->impl_->wantJoin    = false;
                channel->impl_->joinPending = false;
                channel->impl_->state       = ChannelState::Closed;
                channel->impl_->joinRef.clear();
            }
            if (wasActive)
                runCalls(RealtimeChannel::Impl::statusCall(callback, SubscribeStatus::Closed));
        }
    }
}

RealtimeClient::RealtimeClient(std::shared_ptr<const Context> ctx) : core_(std::make_shared<detail::Core>(std::move(ctx))) {}

std::shared_ptr<RealtimeChannel> RealtimeClient::channel(std::string_view name, ChannelOptions options) const
{
    return core_->channel(name, std::move(options));
}

Result<void> RealtimeClient::connect() const { return core_->start(); }
void RealtimeClient::disconnect() const { core_->stop(); }
bool RealtimeClient::isConnected() const { return core_->connected(); }
void RealtimeClient::setAuth(std::string token) const { core_->setAuth(std::move(token)); }
Result<void> RealtimeClient::removeChannel(const std::shared_ptr<RealtimeChannel>& channel) const { return core_->removeChannel(channel); }
void RealtimeClient::removeAllChannels() const { core_->removeAll(); }

}
