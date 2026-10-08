#pragma once

#include <chrono>

#include <supabase/http.hpp>
#include <supabase/ws.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace supabase
{

/// Shared per-client state handed to every sub-client. Treat as internal.
struct Context
{
    std::string url; ///< project URL without trailing slash
    std::string anonKey;
    std::string schema = "public";
    http::Headers headers;
    std::shared_ptr<http::Transport> transport;
    ws::SocketFactory socketFactory; ///< used by realtime
    std::chrono::milliseconds timeout { 30000 };
    /// Returns the bearer token for the current user, or the anon key when signed out.
    std::function<std::string()> bearer;

    [[nodiscard]] std::string token() const { return bearer ? bearer() : anonKey; }
    /// `apikey`, `Authorization` and user headers.
    [[nodiscard]] http::Headers baseHeaders() const;
    [[nodiscard]] std::string endpoint(std::string_view path) const { return url + std::string(path); }
};

}
