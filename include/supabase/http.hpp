#pragma once

/// @file supabase/http.hpp
/// @brief HTTP requests, responses, streaming, cancellation, and transport factories.

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

/// @brief HTTP verbs supported by the request model.
enum class Method
{
    Get,
    Post,
    Put,
    Patch,
    Delete,
    Head
};

/// @brief Return the uppercase wire spelling of a method.
/// @return Static string view; an unrecognized enum value falls back to "GET".
[[nodiscard]] std::string_view toString(Method method) noexcept;

/// @brief Ordered header name/value pairs, preserving duplicate names.
using Headers = std::vector<std::pair<std::string, std::string>>;

/// @brief Shared cooperative cancellation flag.
/// Initialize to false, then store true from any thread to request cancellation.
/// A null token disables cancellation. Copies share the flag; observing cancellation
/// depends on the transport and does not synchronously wait for completion.
using CancelToken = std::shared_ptr<std::atomic_bool>;

/// @brief Owned request data passed to a transport.
/// Raw transports do not add project authentication or JSON content headers.
struct Request
{
    /// HTTP verb; defaults to GET.
    Method method = Method::Get;
    /// Complete destination URL.
    std::string url;
    /// Outgoing headers, including any application credentials or content type.
    Headers headers;
    /// Request bytes; may contain embedded null characters.
    std::string body;
    /// Curl total timeout per attempt; retries can extend the overall duration.
    std::chrono::milliseconds timeout { 30000 };
    /// Optional cooperative cancellation flag.
    CancelToken cancel;
};

/// @brief HTTP response, including unsuccessful HTTP status codes.
struct Response
{
    /// HTTP status code; zero means no status has been assigned.
    int status = 0;
    /// Received headers in transport order.
    Headers headers;
    /// Buffered response bytes; empty when a chunk callback is used.
    std::string body;

    /// @brief Whether the HTTP status is in the successful range [200, 300).
    [[nodiscard]] bool ok() const noexcept { return status >= 200 && status < 300; }
    /// @brief Look up the first header with a case-insensitive matching name.
    /// @param name Header name to search for.
    /// @return Copied value, or an empty string if absent or present with no value.
    /// Duplicate header values are not combined.
    [[nodiscard]] std::string header(std::string_view name) const;
};

/// @brief Consumer of arbitrary response-body chunks, not complete messages.
/// The view borrows transport memory and is valid only during the callback. Copy
/// bytes that must outlive the call. With curl, this runs on the sending thread
/// and must not throw. Return false to stop reception; curl returns the partial
/// response as a successful Result rather than a cancellation error.
using ChunkCallback = std::function<bool(std::string_view)>;

/// @brief Thread-safe interface for synchronous HTTP transfers.
/// Implementations must support concurrent send() calls. Non-2xx HTTP statuses
/// remain Response values; transport failures such as DNS, TLS, timeout, or
/// cancellation produce an Error. Check both the Result and Response::ok().
class Transport
{
  public:
    virtual ~Transport() = default;

    /// @brief Perform a request, optionally delivering its body incrementally.
    /// @param request Request data, borrowed until this call returns.
    /// @param onChunk Optional body consumer; Response::body stays empty when set.
    /// @return Response including its HTTP status, or a transport error.
    /// @note Curl does not retry streaming requests. Callbacks run synchronously
    /// within this call and must not throw.
    virtual Result<Response> send(const Request& request, ChunkCallback onChunk = {}) = 0;
};

/// @brief Configuration copied into the curl HTTP transport.
struct CurlOptions
{
    /// PEM trust-store file path; empty uses curl's default trust store.
    std::string caBundle;
    /// In-memory PEM trust store. With libcurl >= 7.77, overrides caBundle when
    /// nonempty; older builds use caBundle instead.
    std::string caBundleBlob;
    /// Base64 SPKI SHA-256 pins; a connection must match one when nonempty.
    std::vector<std::string> pinnedSha256;
    /// User-Agent sent with requests.
    std::string userAgent = "supabase-cpp";
    /// Proxy URL; empty leaves proxy selection to curl and its environment.
    std::string proxy;
    /// Maximum connection-establishment time for each HTTP attempt.
    std::chrono::milliseconds connectTimeout { 10000 };
    /// Additional attempts for GET, HEAD, PUT, and DELETE on Network errors or
    /// HTTP 502/503/504. Negative values act as zero. POST, PATCH, streaming,
    /// timeout, and cancellation failures are not retried.
    int maxRetries = 2;
    /// Verify the TLS certificate and hostname using the configured trust store.
    bool verifyPeer = true;
};

/// Value for CURLOPT_PINNEDPUBLICKEY built from `pinnedSha256`; empty when no pins are set.
[[nodiscard]] inline std::string curlPinnedKeys(const CurlOptions& options)
{
    std::string out;
    for (const auto& pin : options.pinnedSha256)
    {
        if (!out.empty())
            out += ';';
        out += "sha256//" + pin;
    }
    return out;
}

/// @brief Create a shared curl transport without sending a request.
/// @param options Transport settings copied into the implementation.
/// @return Thread-safe transport with pooled curl handles.
/// @code{.cpp}
/// auto transport = supabase::http::makeCurlTransport();
/// supabase::http::Request request;
/// request.url = "https://example.com/health";
/// auto result = transport->send(request);
/// if (result && result.value().ok())
/// {
///     const auto& body = result.value().body;
///     // Consume the buffered response.
/// }
/// @endcode
[[nodiscard]] std::shared_ptr<Transport> makeCurlTransport(CurlOptions options = {});

/// @brief Run a buffered request on a detached worker and return its future.
/// @param transport Transport retained until the request completes.
/// @param request Owned request snapshot, including any shared cancellation flag.
/// @return Future containing the response or transport error. A null transport
/// produces a ready future containing ErrorCode::InvalidArgument.
/// @note Transport exceptions are converted to Network errors. Cancellation is
/// cooperative and depends on the transport; destroying the future does not cancel.
/// @code{.cpp}
/// supabase::http::Request request;
/// request.url = "https://example.com/health";
/// request.cancel = std::make_shared<std::atomic_bool>(false);
/// auto pending = supabase::http::sendAsync(supabase::http::makeCurlTransport(), request);
/// // request.cancel->store(true); // Request cancellation if needed.
/// auto result = pending.get(); // Wait for completion; inspect status separately.
/// @endcode
[[nodiscard]] std::future<Result<Response>> sendAsync(std::shared_ptr<Transport> transport, Request request);

/// @brief Run a buffered request and deliver its result to a completion callback.
/// @param transport Transport retained by the detached worker.
/// @param request Owned request snapshot.
/// @param done Completion handler, normally invoked on the worker thread. It must
/// not throw; captured objects must remain valid until completion.
/// @note A null transport or empty handler is ignored without a callback. Worker
/// startup failure reporting may occur on the calling thread. Transport exceptions
/// become Network errors. No join handle is returned; cancel through Request::cancel.
void sendAsync(std::shared_ptr<Transport> transport, Request request, std::function<void(Result<Response>)> done);

}
