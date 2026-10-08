#pragma once

#include <chrono>

#include <supabase/result.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace supabase::http
{

enum class Method
{
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Head
};

[[nodiscard]] std::string_view toString(Method method) noexcept;

using Headers = std::vector<std::pair<std::string, std::string>>;

/// Set to true from any thread to abort an in-flight request.
using CancelToken = std::shared_ptr<std::atomic_bool>;

struct Request
{
    Method method = Method::Get;
    std::string url;
    Headers headers;
    std::string body;
    std::chrono::milliseconds timeout { 30000 };
    CancelToken cancel;
};

struct Response
{
    int status = 0;
    Headers headers;
    std::string body;

    [[nodiscard]] bool ok() const noexcept { return status >= 200 && status < 300; }
    /// Case-insensitive header lookup, empty if absent.
    [[nodiscard]] std::string header(std::string_view name) const;
};

/// Receives body chunks as they arrive. Return false to stop the transfer.
using ChunkCallback = std::function<bool(std::string_view)>;

/// Transport abstraction. Implementations must be thread-safe. Non-2xx statuses are NOT errors here;
/// only transport failures (DNS, TLS, timeout, cancel) return an Error.
class Transport
{
  public:
    virtual ~Transport() = default;
    /// When `onChunk` is set the body is streamed to it and Response::body stays empty.
    virtual Result<Response> send(const Request& request, ChunkCallback onChunk = {}) = 0;
};

struct CurlOptions
{
    std::string caBundle; ///< PEM file path; empty uses curl's default store
    std::string caBundleBlob; ///< In-memory PEM data (needs curl >= 7.77); takes precedence over caBundle
    std::string userAgent = "supabase-cpp";
    std::string proxy;
    std::chrono::milliseconds connectTimeout { 10000 };
    int maxRetries  = 2; ///< retries for network errors and 502/503/504 on idempotent methods
    bool verifyPeer = true;
};

[[nodiscard]] std::shared_ptr<Transport> makeCurlTransport(CurlOptions options = {});

/// Runs `transport->send` on a worker thread. The transport is kept alive until the call finishes.
/// Abort early via `Request::cancel`; the future then resolves with the transport's cancel Error.
[[nodiscard]] std::future<Result<Response>> sendAsync(std::shared_ptr<Transport> transport, Request request);

/// Callback flavour of sendAsync; `done` runs on the worker thread.
void sendAsync(std::shared_ptr<Transport> transport, Request request, std::function<void(Result<Response>)> done);

}
