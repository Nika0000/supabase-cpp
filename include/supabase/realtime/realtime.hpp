#pragma once

#include <supabase/context.hpp>
#include <supabase/json.hpp>
#include <supabase/result.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace supabase::realtime
{

namespace detail
{
    class Core;
}

/// Receives the event payload. Runs on the realtime worker thread; keep it short.
using EventCallback = std::function<void(const Json& payload)>;

enum class ChannelState
{
    Closed,
    Joining,
    Joined,
    Errored
};

/// Mirrors supabase-js `REALTIME_SUBSCRIBE_STATES`.
enum class SubscribeStatus
{
    Subscribed,
    TimedOut,
    Closed,
    ChannelError
};

/// `message` carries the server's reason for TimedOut/ChannelError, empty otherwise.
using SubscribeCallback = std::function<void(SubscribeStatus status, const std::string& message)>;

enum class PresenceEvent
{
    Sync, ///< payload: full presence state `{ key: { metas: [...] } }`
    Join, ///< payload: `{ key, newPresences }`
    Leave ///< payload: `{ key, leftPresences }`
};

struct ChannelOptions
{
    bool broadcastSelf = false; ///< receive your own broadcasts
    bool broadcastAck  = false; ///< server acknowledges broadcasts
    std::string presenceKey;    ///< empty lets the server pick one
    bool isPrivate = false;     ///< private channel (authorized through RLS)
};

/// `event` is `*`, `INSERT`, `UPDATE` or `DELETE`; `filter` is e.g. `id=eq.1`.
struct PostgresChangesFilter
{
    std::string event  = "*";
    std::string schema = "public";
    std::string table;
    std::string filter;
};

/// Phoenix channel. Mirrors supabase-js `RealtimeChannel`. Create with `RealtimeClient::channel`.
/// Register handlers before `subscribe`. Thread-safe.
class RealtimeChannel
{
  public:
    ~RealtimeChannel();
    RealtimeChannel(const RealtimeChannel&)            = delete;
    RealtimeChannel& operator=(const RealtimeChannel&) = delete;

    /// Row changes. The payload is `{ schema, table, commit_timestamp, eventType, new, old, errors }`.
    RealtimeChannel& onPostgresChanges(PostgresChangesFilter filter, EventCallback callback);
    /// Broadcast messages with the given event name (`*` for all). The payload is `{ type, event, payload }`.
    RealtimeChannel& onBroadcast(std::string_view event, EventCallback callback);
    RealtimeChannel& onPresence(PresenceEvent event, EventCallback callback);
    /// Any raw Phoenix event by name (`system`, `phx_close`, ...), payload as received.
    RealtimeChannel& on(std::string_view event, EventCallback callback);

    /// Connects the client if needed and joins the channel asynchronously; the outcome arrives through `callback`.
    /// The channel rejoins by itself after a dropped connection.
    [[nodiscard]] Result<void> subscribe(SubscribeCallback callback = {});
    [[nodiscard]] Result<void> unsubscribe();

    /// Sends a broadcast message. The channel must be joined.
    [[nodiscard]] Result<void> send(std::string_view event, const Json& payload);
    [[nodiscard]] Result<void> track(const Json& payload);
    [[nodiscard]] Result<void> untrack();
    /// Current presence state `{ key: { metas: [...] } }`.
    [[nodiscard]] Json presenceState() const;

    [[nodiscard]] ChannelState state() const;
    /// Full topic including the `realtime:` prefix.
    [[nodiscard]] const std::string& topic() const noexcept;

  private:
    friend class detail::Core;
    struct Impl;
    RealtimeChannel(std::weak_ptr<detail::Core> core, std::string topic, ChannelOptions options);
    std::unique_ptr<Impl> impl_;
};

/// Realtime client. Mirrors supabase-js `RealtimeClient`. One websocket and one worker thread per client,
/// started on `connect()` or the first `subscribe()` and shared by all channels. Copies share state.
class RealtimeClient
{
  public:
    explicit RealtimeClient(std::shared_ptr<const Context> ctx);

    /// `realtime().channel("room-1")` joins topic `realtime:room-1`. Returns the existing channel for a repeated name.
    [[nodiscard]] std::shared_ptr<RealtimeChannel> channel(std::string_view name, ChannelOptions options = {}) const;

    /// Starts the connection in the background and keeps it alive with automatic reconnects.
    [[nodiscard]] Result<void> connect() const;
    /// Stops the worker and closes every channel. `connect()` may be called again.
    void disconnect() const;
    [[nodiscard]] bool isConnected() const;

    /// Pushes a new access token to joined channels; empty re-reads the client's current token.
    void setAuth(std::string token = {}) const;

    /// Unsubscribes and forgets the channel.
    Result<void> removeChannel(const std::shared_ptr<RealtimeChannel>& channel) const;
    void removeAllChannels() const;

  private:
    std::shared_ptr<detail::Core> core_;
};

}
