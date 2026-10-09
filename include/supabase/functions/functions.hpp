#pragma once

/**
 * @file supabase/functions/functions.hpp
 * @brief Synchronous invocation and response streaming for Supabase Edge Functions.
 */

#include <supabase/context.hpp>
#include <supabase/json.hpp>
#include <supabase/result.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace supabase::functions
{

/// @brief Request options for an Edge Function invocation.
/// A populated json value takes precedence over body and contentType, including
/// when that value is JSON null. Otherwise a nonempty raw body is sent with its
/// specified content type, defaulting to "text/plain".
struct InvokeOptions
{
    http::Method method = http::Method::Post; ///< HTTP method; defaults to POST.
    std::optional<Json> json;                 ///< Optional JSON payload, serialized with Content-Type: application/json.
    std::string body;                         ///< Raw request bytes used when json has no value.
    std::string contentType;                  ///< Content type for a nonempty raw body; empty selects text/plain.
    http::Headers headers;                    ///< Additional headers appended after shared and generated headers; duplicates are retained.
    std::string region;                       ///< Optional execution region, such as "eu-west-1", sent as x-region.
    http::CancelToken cancel;                 ///< Optional shared cancellation flag; set true to request that the transport abort.
};

/// @brief Complete response from a successful buffered invocation.
/// Returned for HTTP 2xx responses. The body retains the received bytes and is
/// not automatically parsed based on Content-Type.
struct FunctionResponse
{
    int status = 0;        ///< HTTP response status; zero for a default-constructed response.
    http::Headers headers; ///< Response headers supplied by the transport, including any duplicate names.
    std::string body;      ///< Complete response body, which may be empty, text, JSON, or binary data.

    /// @brief Parse the current response body as JSON without throwing for parse errors.
    /// @return Parsed JSON value, or a discarded value when the body is invalid or empty.
    /// @note Check is_discarded() for parse failure. A valid JSON null value is
    /// distinct from failure. Each call parses the body again.
    [[nodiscard]] Json json() const { return Json::parse(body, nullptr, false); }
};

/**
 * @brief Client for invoking deployed Supabase Edge Functions.
 *
 * Obtain this API through supabase::Client::functions(). Requests use the shared
 * project URL, HTTP transport, timeout, API key, and bearer-token provider.
 * With an active session, the bearer token belongs to the current user;
 * otherwise the project API key is used. Resolving the token may refresh an
 * expiring session before the function request is sent.
 *
 * invoke() buffers the response; invokeStream() delivers body bytes to a callback.
 * Both methods block on the calling thread until the transport finishes. Check
 * Result<T> before accessing its value or error.
 *
 * Examples assume an initialized supabase::Client named `client` and are
 * independent snippets intended for use inside a function. Include
 * <supabase/supabase.hpp> for the complete public API.
 */
class FunctionsClient
{
  public:
    /// @brief Create a functions client retaining a shared request context.
    /// @param ctx Non-null context with the project URL, API key, and HTTP transport.
    /// @note Usually obtained from supabase::Client::functions() rather than constructed directly.
    explicit FunctionsClient(std::shared_ptr<const Context> ctx);

    /// @brief Invoke a deployed function and buffer its complete response body.
    /// @param name Function name appended verbatim to the project's /functions/v1/ path.
    /// @param options Request method, payload, headers, region, and optional cancellation flag.
    /// @return Status, headers, and body for HTTP 2xx; transport errors are propagated.
    /// Other HTTP statuses produce Error with code "FunctionsHttpError" and the
    /// response status. The message uses a JSON string field named "error" or
    /// "message" when available, otherwise the complete response body.
    /// @note The function name is neither validated nor URL-encoded. Timeout comes
    /// from the shared context. A JSON payload generates Content-Type: application/json;
    /// for an empty raw body, no Content-Type header is generated.
    /// @par Example: invoke with JSON and inspect the response
    /// @code{.cpp}
    /// auto result = client.functions().invoke("hello", {
    ///     .json = supabase::Json { { "name", "world" } },
    /// });
    /// if (!result)
    /// {
    ///     // Handle result.error().code, .message, and .status.
    /// }
    /// else
    /// {
    ///     auto data = result->json();
    ///     if (data.is_discarded())
    ///     {
    ///         // The successful response body is empty or is not valid JSON.
    ///     }
    ///     else
    ///     {
    ///         // Consume the parsed JSON value in data.
    ///     }
    /// }
    /// @endcode
    /// @par Example: send raw bytes with an explicit content type
    /// @code{.cpp}
    /// auto result = client.functions().invoke("process-text", {
    ///     .body = "raw request text",
    ///     .contentType = "text/plain; charset=utf-8",
    /// });
    /// @endcode
    [[nodiscard]] Result<FunctionResponse> invoke(std::string_view name, const InvokeOptions& options = {}) const;

    /// @brief Invoke a function and deliver response bytes as the transport receives them.
    /// @param name Function name appended verbatim to the project's /functions/v1/ path.
    /// @param options Request options with the same payload and header rules as invoke().
    /// @param onChunk Nonempty callback; return true to continue receiving or false to stop.
    /// @return HTTP 2xx status on success, errc::InvalidArgument for an empty callback,
    /// or a transport error. Other HTTP statuses produce "FunctionsHttpError"
    /// with the response status and a generic HTTP status message.
    /// @note Chunks are arbitrary byte ranges, not complete JSON values or SSE events.
    /// The supplied string_view is borrowed; copy bytes that must survive the callback.
    /// The default transport calls the callback synchronously on the invocation thread.
    /// Body chunks may arrive before a non-2xx status is reported; check the final
    /// result even after receiving data.
    /// @note The default curl transport does not retry streamed requests. Returning
    /// false stops reception normally and can still produce a successful 2xx result.
    /// Cancellation through options.cancel produces errc::Cancelled when observed
    /// by that transport. No response body or headers are returned by this method.
    /// @code{.cpp}
    /// auto result = client.functions().invokeStream(
    ///     "stream-events",
    ///     {
    ///         .method = supabase::http::Method::Get,
    ///         .headers = { { "Accept", "text/event-stream" } },
    ///     },
    ///     [](std::string_view chunk)
    ///     {
    ///         // Feed chunk to the application's SSE parser; retain partial events.
    ///         return true;
    ///     });
    /// if (!result)
    /// {
    ///     // Handle the stream error; some bytes may already have been received.
    /// }
    /// @endcode
    [[nodiscard]] Result<int> invokeStream(std::string_view name, const InvokeOptions& options, http::ChunkCallback onChunk) const;

  private:
    [[nodiscard]] http::Request build(std::string_view name, const InvokeOptions& options) const;

    std::shared_ptr<const Context> m_ctx;
};

}
