#pragma once

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

/// Options for `createClient`, mirroring supabase-js `SupabaseClientOptions`.
struct ClientOptions
{
    std::string schema = "public";
    http::Headers headers;                         ///< extra headers sent with every request
    std::shared_ptr<http::Transport> transport;    ///< defaults to a curl transport
    ws::SocketFactory socketFactory;               ///< realtime websocket, defaults to curl
    http::CurlOptions curl;                        ///< used only when `transport` is null; also configures the default websocket
    std::shared_ptr<auth::SessionStorage> storage; ///< defaults to in-memory
    bool autoRefreshToken = true;
    std::chrono::milliseconds timeout { 30000 };
};

}
