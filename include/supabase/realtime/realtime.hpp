#pragma once

/**
 * @file supabase/realtime/realtime.hpp
 * @brief WebSocket channels for database changes, broadcasts, and presence.
 */

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

/// @brief Handler receiving a channel event's JSON payload on the Realtime worker.
/// The reference is borrowed for the duration of the call; copy data retained
/// afterward. Keep handlers short and dispatch UI work to the appropriate thread.
/// Handler exceptions are caught and discarded by the dispatcher.
using EventCallback = std::function<void(const Json& payload)>;

/// @brief Local channel lifecycle state, independent of the socket's connection state.
enum class ChannelState
{
    Closed,  ///< Not currently requesting a subscription, or explicitly closed.
    Joining, ///< Subscription requested or a join reply is pending.
    Joined,  ///< Server accepted the channel join.
    Errored  ///< Join failed, timed out, or the connection was interrupted.
};

/// @brief Subscription outcomes, corresponding to supabase-js REALTIME_SUBSCRIBE_STATES.
enum class SubscribeStatus
{
    Subscribed,  ///< Server accepted a join or rejoin.
    TimedOut,    ///< A channel join reply did not arrive before the local deadline.
    Closed,      ///< Channel closed locally or was closed by the server.
    ChannelError ///< Server rejected the join or reported a channel error.
};

/// @brief Handler receiving subscription outcomes and an optional diagnostic message.
/// TimedOut and ChannelError include a local or server reason; other outcomes use
/// an empty message. Server events and timeouts run on the worker, while explicit
/// unsubscribe() invokes Closed synchronously on its calling thread. Callbacks
/// can therefore overlap; captured state must remain valid for in-flight calls.
/// Callback exceptions are caught and discarded by the dispatcher.
using SubscribeCallback = std::function<void(SubscribeStatus status, const std::string& message)>;

/// @brief Presence notifications derived from the server's state and diff messages.
enum class PresenceEvent
{
    Sync, ///< Full current state: { key: { metas: [...] } }.
    Join, ///< Added presence metadata: { key, newPresences }.
    Leave ///< Removed presence metadata: { key, leftPresences }.
};

/// @brief Immutable channel configuration sent when the channel joins.
struct ChannelOptions
{
    bool broadcastSelf = false; ///< Ask the server to deliver broadcasts sent by this channel back to it.
    bool broadcastAck  = false; ///< Request server broadcast acknowledgements; send() still returns after queuing.
    std::string presenceKey;    ///< Presence identifier; empty lets the server choose one.
    bool isPrivate = false;     ///< Request a private channel subject to server-side Realtime authorization.
};

/// @brief Server subscription filter for PostgreSQL row-change events.
/// Register before subscribing so the filter is included in the join request.
/// The server validates the filter and determines which changes are authorized.
struct PostgresChangesFilter
{
    std::string event  = "*";      ///< "*", "INSERT", "UPDATE", or "DELETE".
    std::string schema = "public"; ///< Database schema; defaults to public.
    std::string table;             ///< Optional table name; omitted from the join filter when empty.
    std::string filter;            ///< Optional server filter expression, such as "id=eq.1".
};

/**
 * @brief Phoenix channel for database changes, broadcasts, and presence.
 *
 * Create channels through RealtimeClient::channel(). The client retains registered
 * channels, so releasing an application shared_ptr does not unsubscribe. Use
 * unsubscribe() to leave, or RealtimeClient::removeChannel() to leave and remove
 * the registration. Channels refer weakly to the shared Realtime core and do not
 * keep its socket or worker alive after the last client owner is destroyed.
 *
 * Public channel state and handler registration are synchronized. Handlers run
 * outside the channel lock and may call channel operations. Register PostgreSQL
 * filters before subscribe(); their server binding is established during joining.
 * Each registration appends a handler rather than replacing existing handlers.
 *
 * @par Example: receive broadcasts and observe the asynchronous join
 * @code{.cpp}
 * auto channel = client.realtime().channel("room-1");
 * channel->onBroadcast("message", [](const supabase::Json& event)
 * {
 *     // Consume event["payload"] on this worker, or copy it for another thread.
 * });
 * auto started = channel->subscribe(
 *     [](supabase::realtime::SubscribeStatus status, const std::string& message)
 *     {
 *         // Use Subscribed to enable sends; report errors using status and message.
 *     });
 * if (!started)
 * {
 *     // Handle an immediate startup error; success alone does not mean Joined.
 * }
 * @endcode
 * Examples assume an initialized supabase::Client named `client` and run inside
 * a function. Include <supabase/supabase.hpp> for the complete public API.
 */
class RealtimeChannel
{
  public:
    /// @brief Destroy this channel's local state after its final shared owner releases it.
    /// @note Destruction is not a substitute for explicit unsubscription or removal.
    ~RealtimeChannel();
    RealtimeChannel(const RealtimeChannel&)            = delete;
    RealtimeChannel& operator=(const RealtimeChannel&) = delete;

    /// @brief Register a handler for authorized PostgreSQL row changes.
    /// @param filter Event, schema, optional table, and optional server filter expression.
    /// @param callback Handler receiving { schema, table, commit_timestamp, eventType, new, old, errors }.
    /// @return This channel with the binding appended.
    /// @note Register before subscribe() so the server receives and assigns the filter.
    /// Record contents depend on what the server supplies; missing records become empty objects.
    /// @code{.cpp}
    /// channel->onPostgresChanges({
    ///     .event = "INSERT",
    ///     .schema = "public",
    ///     .table = "games",
    /// }, [](const supabase::Json& change)
    /// {
    ///     // Consume change["new"] or copy it before returning.
    /// });
    /// @endcode
    RealtimeChannel& onPostgresChanges(PostgresChangesFilter filter, EventCallback callback);

    /// @brief Register a handler for broadcast messages with a matching event name.
    /// @param event Broadcast event name, or "*" for all names.
    /// @param callback Handler receiving the envelope { type, event, payload }.
    /// @return This channel with the binding appended.
    RealtimeChannel& onBroadcast(std::string_view event, EventCallback callback);

    /// @brief Register a handler for presence state, joins, or leaves.
    /// @param event Presence notification type and its corresponding payload shape.
    /// @param callback Handler receiving the derived presence payload.
    /// @return This channel with the binding appended.
    /// @note Local state is updated before handlers run; presenceState() returns a copy of it.
    RealtimeChannel& onPresence(PresenceEvent event, EventCallback callback);

    /// @brief Register a handler for raw Phoenix channel events.
    /// @param event Event name, such as "system" or "phx_close"; "*" matches all channel events.
    /// @param callback Handler receiving the event payload as supplied by the server.
    /// @return This channel with the binding appended.
    /// @note Raw handlers run alongside typed handlers for the same incoming message.
    RealtimeChannel& on(std::string_view event, EventCallback callback);

    /// @brief Request an asynchronous channel join and start the shared worker if needed.
    /// @param callback Optional status handler retained for the subscription and any rejoins.
    /// @return Success when startup is accepted, errc::InvalidArgument when already
    /// subscribed, errc::Cancelled if the core was destroyed, or a worker startup error.
    /// @note Success does not mean Joined; wait for SubscribeStatus::Subscribed.
    /// Socket interruptions and join timeouts trigger automatic retry. A rejected
    /// join or a server phx_close stops automatic joining and requires a new subscribe().
    [[nodiscard]] Result<void> subscribe(SubscribeCallback callback = {});

    /// @brief Close this channel locally and queue a leave frame when possible.
    /// @return Success after local closure; the server's leave acknowledgement is not awaited.
    /// @note Clears presence state and disables automatic rejoin. Closed is reported
    /// synchronously on the calling thread when an active or errored channel is closed.
    /// Calling again on a closed, inactive channel has no effect. Handlers remain registered,
    /// and callbacks already in flight may finish. The shared socket stays running.
    [[nodiscard]] Result<void> unsubscribe();

    /// @brief Queue a broadcast message on a joined channel.
    /// @param event Broadcast event name.
    /// @param payload JSON data placed in the broadcast envelope's payload field.
    /// @return Success after queuing, errc::InvalidArgument if not joined, or errc::Cancelled if the core is gone.
    /// @note Does not wait for transmission or a server acknowledgement, even with broadcastAck enabled.
    /// Queued messages can be lost when a connection is replaced.
    /// @code{.cpp}
    /// auto result = channel->send("message", supabase::Json { { "text", "Hello" } });
    /// // Call after Subscribed and check result for local state errors.
    /// @endcode
    [[nodiscard]] Result<void> send(std::string_view event, const Json& payload);

    /// @brief Queue presence metadata for this joined channel.
    /// @param payload JSON metadata to publish to the presence service.
    /// @return Success after queuing, or the same local state/core errors as send().
    /// @note Does not update presenceState() immediately; that snapshot follows server events.
    /// Tracking is not automatically replayed after a reconnect; track again after Subscribed.
    [[nodiscard]] Result<void> track(const Json& payload);

    /// @brief Queue a request to remove this joined channel's tracked presence.
    /// @return Success after queuing, or the same local state/core errors as send().
    /// @note Local presence changes when the server publishes an updated state or diff.
    [[nodiscard]] Result<void> untrack();

    /// @brief Read a synchronized copy of the last received presence state.
    /// @return Object with the shape { key: { metas: [...] } }.
    /// @note unsubscribe() clears the snapshot. Connection loss or disconnect() can
    /// leave the previous snapshot available until new server state arrives.
    [[nodiscard]] Json presenceState() const;

    /// @brief Read the synchronized local lifecycle state.
    /// @return Current state; Joined reflects a successful join, not guaranteed socket availability.
    [[nodiscard]] ChannelState state() const;

    /// @brief Access the immutable full Phoenix topic.
    /// @return Borrowed topic string including the realtime: prefix, valid while this channel exists.
    [[nodiscard]] const std::string& topic() const noexcept;

  private:
    friend class detail::Core;
    struct Impl;
    RealtimeChannel(std::weak_ptr<detail::Core> core, std::string topic, ChannelOptions options);
    std::unique_ptr<Impl> m_impl;
};

/**
 * @brief Shared WebSocket connection and registry of Phoenix channels.
 *
 * Obtain this API through supabase::Client::realtime(). One socket and one worker
 * serve all registered channels, starting on connect() or the first subscribe().
 * RealtimeClient copies share this core, channel registry, and auth override;
 * disconnecting or removing channels through one copy affects the others.
 * Channels hold a weak reference to the core, so they do not extend its lifetime.
 *
 * The worker sends heartbeats and reconnects after connection loss. Connection
 * startup and channel joining are asynchronous. Observe isConnected() for socket
 * state and subscription callbacks for channel readiness. PostgreSQL events,
 * broadcasts, and presence delivery are subject to the server's configuration
 * and permissions; queued outgoing messages have no delivery guarantee.
 */
class RealtimeClient
{
  public:
    /// @brief Create a Realtime core retaining the shared project context.
    /// @param ctx Non-null context with project settings, bearer provider, and WebSocket factory.
    /// @note Construction does not start a worker or connect a socket.
    explicit RealtimeClient(std::shared_ptr<const Context> ctx);

    /// @brief Retrieve or register a channel by its full topic.
    /// @param name Channel name; realtime: is prepended when not already present.
    /// @param options Configuration for a newly created channel.
    /// @return Shared channel retained in this client's registry; no join is performed.
    /// @note Repeated names return the existing object and retain its original options.
    /// Removing the registration allows a later call to create a new channel with new options.
    [[nodiscard]] std::shared_ptr<RealtimeChannel> channel(std::string_view name, ChannelOptions options = {}) const;

    /// @brief Start the background connection worker, or retain the already-running worker.
    /// @return Success after startup is accepted, errc::NotImplemented when no socket
    /// factory exists, or errc::InvalidArgument when restarting from a stopping worker callback.
    /// @note Socket connection errors occur later and are retried by the worker;
    /// a successful return does not establish a connection or join a channel.
    [[nodiscard]] Result<void> connect() const;

    /// @brief Stop the connection worker and close active registered channels locally.
    /// @note Off the worker thread, this waits for the worker to finish. From a
    /// worker callback it requests shutdown and returns without joining itself.
    /// Closed status callbacks are delivered by the worker. Channel objects and
    /// bindings remain registered; connect() and subscribe() can be called again.
    void disconnect() const;

    /// @brief Read whether the shared worker currently has an open WebSocket session.
    /// @return Socket connection flag; independent of whether any channel has joined.
    [[nodiscard]] bool isConnected() const;

    /// @brief Set the Realtime bearer override and queue token updates for joined channels.
    /// @param token Nonempty override used for joins and updates; empty restores the shared bearer provider.
    /// @note Does not change the AuthClient's stored session. Resolving an empty
    /// override can refresh auth on the calling thread. Updates are queued without
    /// awaiting server confirmation; new joins use the selected token.
    void setAuth(std::string token = {}) const;

    /// @brief Unsubscribe a channel and remove its registration from this client.
    /// @param channel Channel obtained from this client; a null pointer is invalid.
    /// @return Unsubscription result, or errc::InvalidArgument for a null pointer.
    /// @note External shared_ptrs keep the removed object alive, but it is no longer
    /// serviced by the registry. Obtain a new channel() before subscribing again.
    /// The shared connection remains running.
    Result<void> removeChannel(const std::shared_ptr<RealtimeChannel>& channel) const;

    /// @brief Unsubscribe and remove the current snapshot of registered channels.
    /// @note Does not stop the shared worker. Use disconnect() to stop the socket as well.
    void removeAllChannels() const;

  private:
    std::shared_ptr<detail::Core> m_core;
};

}
