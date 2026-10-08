#pragma once

#include <chrono>

#include <supabase/http.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace supabase::ws
{

/// Minimal text-message websocket. A socket is driven from one thread at a time.
class Socket
{
  public:
    virtual ~Socket() = default;
    /// `url` uses ws:// or wss://.
    virtual Result<void> connect(const std::string& url, const http::Headers& headers, std::chrono::milliseconds timeout) = 0;
    virtual Result<void> send(std::string_view text) = 0;
    /// Waits up to `wait` for one complete message; an empty optional means nothing arrived.
    /// A close frame or a lost connection is an Error.
    virtual Result<std::optional<std::string>> receive(std::chrono::milliseconds wait) = 0;
    virtual void close() noexcept = 0;
};

using SocketFactory = std::function<std::unique_ptr<Socket>()>;

/// curl-backed websocket (needs libcurl >= 7.86; older builds fail `connect` with `NotImplemented`).
/// Honors caBundle, caBundleBlob, userAgent, proxy, connectTimeout and verifyPeer.
[[nodiscard]] SocketFactory makeCurlSocketFactory(http::CurlOptions options = {});

}
