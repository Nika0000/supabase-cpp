#pragma once

/**
 * @file supabase/config.hpp
 * @brief Construction options for a Supabase client and its shared services.
 */

#include <chrono>

#include <supabase/http.hpp>
#include <supabase/ws.hpp>

#include <memory>
#include <string>

namespace supabase
{

namespace auth
{
    class SessionStorage;
}

/**
 * @brief Project-wide options consumed by Client construction or createClient().
 *
 * Defaults select the public PostgREST schema, curl-backed HTTP and WebSocket
 * providers, in-memory session storage, automatic on-demand token refresh, and
 * a 30-second HTTP request timeout. Creating the client does not send a request
 * or start the Realtime worker.
 *
 * Client construction takes these options by value. Changing the original
 * options afterward does not reconfigure the client. Injected transports and
 * storage remain shared objects, while a socket factory creates separate sockets
 * for Realtime connection attempts.
 *
 * @par Example: configure a schema and request timeout
 * @code{.cpp}
 * supabase::ClientOptions options;
 * options.schema = "app";
 * options.headers = { { "X-Application-Version", "1.0" } };
 * options.timeout = std::chrono::milliseconds { 10000 };
 * auto client = supabase::createClient(
 *     "https://your-project.supabase.co",
 *     "your-publishable-key",
 *     options);
 * @endcode
 * The example is intended for use inside a function with
 * <supabase/supabase.hpp> included. The schema must be exposed by the server.
 *
 * @see Client
 * @see createClient
 */
struct ClientOptions
{
    /// @brief Default database schema for PostgREST queries and RPC calls.
    /// Sent through Accept-Profile or Content-Profile according to the method.
    /// Auth, Storage, Edge Functions, and Realtime keep their own API endpoints.
    /// Use Client::schema() for a query entry point targeting another schema.
    std::string schema = "public";

    /// @brief Additional headers for API requests and the Realtime WebSocket handshake.
    /// HTTP services append these to their generated API-key and authorization
    /// headers. Duplicate names are retained rather than replaced or deduplicated.
    /// Header values are retained in the shared context during construction.
    http::Headers headers;

    /// @brief Shared HTTP transport for Auth, PostgREST, Edge Functions, and Storage.
    /// Null selects http::makeCurlTransport(curl). A supplied transport must meet
    /// http::Transport's concurrency and streaming contract and honor request settings.
    /// Supplied transports use their own configuration; this client's curl settings are not applied to them.
    /// This setting does not select the Realtime WebSocket implementation.
    std::shared_ptr<http::Transport> transport;

    /// @brief Factory for the Realtime worker's WebSocket connections.
    /// An empty factory selects ws::makeCurlSocketFactory(curl). A custom factory
    /// supplies its own socket implementation and configuration. It may be called
    /// again after reconnects; each returned socket is driven by the Realtime worker.
    /// HTTP transport selection is independent of this factory.
    ws::SocketFactory socketFactory;

    /// @brief Settings for whichever curl-backed providers are selected by default.
    /// Used by the HTTP provider when transport is null and by the WebSocket
    /// provider when socketFactory is empty, independently. Each provider uses
    /// the settings it supports; custom providers configure themselves.
    /// HTTP request timeouts are supplied separately through timeout below.
    http::CurlOptions curl;

    /// @brief Persistence for the serialized authentication session.
    /// Null selects auth::MemorySessionStorage. The session is loaded lazily and
    /// saved or cleared on auth changes. Implementations must protect credentials
    /// and satisfy auth::SessionStorage's threading contract, including shared use.
    /// Never persist session tokens as unprotected application configuration.
    std::shared_ptr<auth::SessionStorage> storage;

    /// @brief Enable automatic refresh before operations needing an expiring session.
    /// True permits a blocking refresh when a refresh token and expiry are available.
    /// Refresh is on demand; no background refresh thread is started. False leaves
    /// automatic refresh disabled while AuthClient::refreshSession() remains available.
    bool autoRefreshToken = true;

    /// @brief Timeout forwarded to HTTP requests; defaults to 30 seconds.
    /// The default curl transport applies it per transfer attempt. Auth refresh
    /// and retries can make a complete operation exceed this duration. Realtime
    /// connection, join, and heartbeat timing is managed independently.
    std::chrono::milliseconds timeout { 30000 };
};

}
