#pragma once

/// @file supabase/context.hpp
/// @brief Internal shared request settings and bearer-token resolution.

#include <chrono>

#include <supabase/http.hpp>
#include <supabase/ws.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace supabase
{

/// @brief Internal context retained by native services and database builders.
/// Client construction normalizes the URL and selects providers. SchemaClient
/// uses a context copy with another schema while sharing transport and bearer
/// resolution. Configure through ClientOptions rather than mutating a live context;
/// public fields have no synchronization for concurrent modification.
struct Context
{
    std::string url;                             ///< Project base URL, without trailing slashes when created by Client.
    std::string anonKey;                         ///< Project API key used in apikey headers and as the default bearer.
    std::string schema = "public";               ///< Database schema used in PostgREST profile headers.
    http::Headers headers;                       ///< Extra HTTP API and WebSocket handshake headers; duplicate names are retained.
    std::shared_ptr<http::Transport> transport;  ///< Shared HTTP transport; required for request execution.
    ws::SocketFactory socketFactory;             ///< Realtime connection factory, called by its worker.
    std::chrono::milliseconds timeout { 30000 }; ///< Timeout forwarded to HTTP requests.

    /// @brief Optional provider installed by AuthClient for current bearer credentials.
    /// May refresh an expiring session synchronously. The native provider refers
    /// weakly to auth state and falls back to the project key when that state is gone.
    std::function<std::string()> bearer;

    /// @brief Resolve credentials through bearer, or return anonKey if no provider is installed.
    /// @return Token string copied from the provider or project key.
    /// @note A configured provider's empty result is returned as-is; it is not replaced.
    /// Resolution can perform a blocking auth refresh and does not return Result<T>.
    [[nodiscard]] std::string token() const { return bearer ? bearer() : anonKey; }

    /// @brief Build API-key, bearer authorization, and extra HTTP headers.
    /// @return New list containing apikey, Authorization: Bearer <token>, then headers.
    /// @note Calls token(); may refresh auth. Header names are not deduplicated.
    [[nodiscard]] http::Headers baseHeaders() const;

    /// @brief Concatenate the project base URL and a supplied endpoint path.
    /// @param path Path normally starting with '/', including any already-encoded query.
    /// @return Literal url + path; no separator insertion, URL encoding, or validation occurs.
    [[nodiscard]] std::string endpoint(std::string_view path) const { return url + std::string(path); }
};

}
