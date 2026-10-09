#pragma once

/// @file supabase/ws.hpp
/// @brief Text WebSocket transport interface and curl socket factory.

#include <chrono>

#include <supabase/http.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace supabase::ws
{

/// @brief Synchronous text-message WebSocket interface used by Realtime.
/// Drive each socket from one thread at a time, including close(). The interface
/// does not start a background reader or provide concurrent send/receive safety.
/// Socket ownership is independent of the factory that created it.
class Socket
{
  public:
    virtual ~Socket() = default;
    /// @brief Establish a WebSocket connection and complete its handshake.
    /// @param url Destination using ws:// or wss://.
    /// @param headers Additional HTTP handshake headers.
    /// @param timeout Connection-establishment timeout.
    /// @return Success after connecting, or a connection/implementation error.
    /// @note The curl implementation closes any previous connection first.
    virtual Result<void> connect(const std::string& url, const http::Headers& headers, std::chrono::milliseconds timeout) = 0;

    /// @brief Send text over an established connection.
    /// @param text Borrowed message bytes, consumed before the call returns.
    /// @return Success after writing, or a transport error; success is not an
    /// acknowledgement from the peer.
    virtual Result<void> send(std::string_view text) = 0;

    /// @brief Poll for one complete message, retaining partial data between calls.
    /// @param wait Polling wait used while no data is available.
    /// @return Owned message, an empty optional when the polling wait expires, or
    /// an error for a peer close or lost connection. An engaged optional containing
    /// an empty string represents an empty message, not a polling timeout.
    /// @note Curl handles ping/pong frames internally. This polling wait is not a
    /// hard deadline for assembling a message when fragments keep arriving.
    virtual Result<std::optional<std::string>> receive(std::chrono::milliseconds wait) = 0;

    /// @brief Release the connection and pending receive data without throwing.
    /// The curl implementation permits repeated calls; do not call concurrently
    /// with another operation on the same socket.
    virtual void close() noexcept = 0;
};

/// @brief Callable producing a separately owned, initially disconnected socket.
/// Realtime accepts a factory so each connection can obtain its own transport.
using SocketFactory = std::function<std::unique_ptr<Socket>()>;

/// @brief Create a factory for curl-backed WebSockets without opening a connection.
/// @param options Copied TLS trust-store, user-agent, proxy, and peer-verification
/// settings. HTTP retry settings and CurlOptions::connectTimeout are not used;
/// Socket::connect() supplies the connection timeout.
/// @return Factory producing a new independently owned socket on each call.
/// @note Requires libcurl >= 7.86 at build time. Older builds return sockets whose
/// connect() reports ErrorCode::NotImplemented. In-memory CA data requires curl
/// >= 7.77 and takes precedence over a configured CA file when supported.
/// @code{.cpp}
/// auto factory = supabase::ws::makeCurlSocketFactory();
/// auto socket = factory();
/// auto connected = socket->connect("wss://example.com/socket", {}, std::chrono::seconds(10));
/// if (connected)
/// {
///     auto message = socket->receive(std::chrono::milliseconds(100));
///     if (message && message.value().has_value())
///     {
///         // Consume *message.value() before polling again.
///     }
/// }
/// socket->close();
/// @endcode
[[nodiscard]] SocketFactory makeCurlSocketFactory(http::CurlOptions options = {});

}
