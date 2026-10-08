#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace supabase
{

/// Error returned by every fallible call. `status` is the HTTP status (0 for transport failures).
struct Error
{
    std::string code;
    std::string message;
    int status = 0;
    std::string details;
    std::string hint;

    [[nodiscard]] bool isHttp() const noexcept { return status != 0; }
};

namespace errc
{
    inline constexpr std::string_view Network         = "NetworkError";
    inline constexpr std::string_view Timeout         = "Timeout";
    inline constexpr std::string_view Cancelled       = "Cancelled";
    inline constexpr std::string_view Parse           = "ParseError";
    inline constexpr std::string_view InvalidArgument = "InvalidArgument";
    inline constexpr std::string_view NoSession       = "AuthSessionMissing";
    inline constexpr std::string_view NotImplemented  = "NotImplemented";
    inline constexpr std::string_view Http            = "HttpError";
}

[[nodiscard]] inline Error makeError(std::string_view code, std::string message, int status = 0)
{
    return Error { std::string(code), std::move(message), status, {}, {} };
}

}
