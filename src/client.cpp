#include <supabase/client.hpp>

namespace supabase
{

namespace
{
    std::shared_ptr<Context> makeContext(std::string url, std::string anonKey, ClientOptions& options)
    {
        while (!url.empty() && url.back() == '/')
            url.pop_back();

        auto ctx           = std::make_shared<Context>();
        ctx->url           = std::move(url);
        ctx->anonKey       = std::move(anonKey);
        ctx->schema        = std::move(options.schema);
        ctx->headers       = std::move(options.headers);
        ctx->timeout       = options.timeout;
        ctx->socketFactory = options.socketFactory ? std::move(options.socketFactory) : ws::makeCurlSocketFactory(options.curl);
        ctx->transport     = options.transport ? std::move(options.transport) : http::makeCurlTransport(std::move(options.curl));
        return ctx;
    }
}

Client::Client(std::string url, std::string anonKey, ClientOptions options)
    : ctx_(makeContext(std::move(url), std::move(anonKey), options)),
      auth_(std::make_unique<auth::AuthClient>(ctx_, std::move(options.storage), options.autoRefreshToken)), functions_(ctx_),
      storage_(ctx_), realtime_(ctx_), graphql_(ctx_)
{
}

postgrest::QueryBuilder Client::from(std::string_view table) const { return postgrest::QueryBuilder(ctx_, std::string(table)); }

postgrest::FilterBuilder Client::rpc(std::string_view fn, const Json& args, postgrest::RpcOptions options) const
{
    return postgrest::makeRpc(ctx_, fn, args, options);
}

}
