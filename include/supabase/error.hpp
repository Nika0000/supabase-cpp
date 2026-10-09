#pragma once

/// @file supabase/error.hpp
/// @brief Request diagnostics and common SDK error codes.

#include <string>
#include <string_view>
#include <utility>

namespace supabase
{

/// @brief Diagnostic value returned by fallible SDK operations through Result<T>.
/// Codes may come from errc, a service-specific mapper, or the server. Preserve
/// unknown codes rather than assuming a closed enum. HTTP status is contextual:
/// parse/cardinality errors can carry a successful response's status.
struct Error
{
    std::string code;    ///< Machine-readable SDK, service, or server code.
    std::string message; ///< Human-readable diagnostic; may contain a raw server response.
    int status = 0;      ///< Associated HTTP status; zero when no status is available.
    std::string details; ///< Additional server details, when extracted by the service.
    std::string hint;    ///< Server guidance, when extracted by the service.

    /// @brief Check whether an HTTP status is attached to this diagnostic.
    /// @return true when status is nonzero, including a 2xx status on a local parse error.
    /// @note Does not classify the status as an HTTP failure or check its range.
    [[nodiscard]] bool isHttp() const noexcept { return status != 0; }
};

/// @brief Common SDK error strings for comparisons with Error::code.
/// Services may return other codes; these constants are not an exhaustive list.
namespace errc
{
    /// Transport or connection failure not classified as timeout or cancellation.
    inline constexpr std::string_view Network = "NetworkError";
    /// Transport timeout.
    inline constexpr std::string_view Timeout = "Timeout";
    /// Request cancellation or an unavailable/destroyed operation owner.
    inline constexpr std::string_view Cancelled = "Cancelled";
    /// Response parsing or supported JSON conversion failure.
    inline constexpr std::string_view Parse = "ParseError";
    /// Locally rejected argument or operation state.
    inline constexpr std::string_view InvalidArgument = "InvalidArgument";
    /// Required authentication session or refresh credentials are unavailable.
    inline constexpr std::string_view NoSession = "AuthSessionMissing";
    /// Operation or configured backend capability is not implemented.
    inline constexpr std::string_view NotImplemented = "NotImplemented";
    /// Generic HTTP response error when no more specific code was extracted.
    inline constexpr std::string_view Http = "HttpError";
}

/// @brief Create a diagnostic with empty details and hint fields.
/// @param code SDK or service code, copied into owned storage.
/// @param message Diagnostic text, moved into the result.
/// @param status Associated HTTP status, or zero when unavailable.
/// @return Error value; no exception is thrown merely to report this diagnostic.
/// @code{.cpp}
/// auto error = supabase::makeError(supabase::errc::InvalidArgument, "a nonempty identifier is required");
/// @endcode
[[nodiscard]] inline Error makeError(std::string_view code, std::string message, int status = 0)
{
    return Error { .code = std::string(code), .message = std::move(message), .status = status, .details = {}, .hint = {} };
}

}
