#include <supabase/client.hpp>

namespace supabase
{

namespace
{
    std::shared_ptr<Context> makeContext(std::string url, std::string anonKey, ClientOptions& options)
    {
        while (!url.empty() && url.back() == '/')
            url.pop_back();

        auto ctx     = std::make_shared<Context>();
        ctx->url     = std::move(url);
        ctx->anonKey = std::move(anonKey);
        ctx->schema  = std::move(options.schema);
        ctx->headers = std::move(options.headers);
        ctx->timeout = options.timeout;
        // Capture WebSocket curl settings before moving them into the default HTTP
        // provider. An injected HTTP transport does not suppress the default socket factory.
        ctx->socketFactory = options.socketFactory ? std::move(options.socketFactory) : ws::makeCurlSocketFactory(options.curl);
        ctx->transport     = options.transport ? std::move(options.transport) : http::makeCurlTransport(std::move(options.curl));
        return ctx;
    }
}

Client::Client(std::string url, std::string anonKey, ClientOptions options)
    : m_ctx(makeContext(std::move(url), std::move(anonKey), options)),
      m_auth(std::make_unique<auth::AuthClient>(m_ctx, std::move(options.storage), options.autoRefreshToken)), m_functions(m_ctx),
      m_storage(m_ctx), m_realtime(m_ctx), m_graphql(m_ctx)
{
}

postgrest::QueryBuilder SchemaClient::from(std::string_view table) const { return postgrest::QueryBuilder(m_ctx, std::string(table)); }

postgrest::FilterBuilder SchemaClient::rpc(std::string_view fn, const Json& args, postgrest::RpcOptions options) const
{
    return postgrest::makeRpc(m_ctx, fn, args, options);
}

SchemaClient Client::schema(std::string_view name) const
{
    // Separate the schema setting while preserving shared transport and bearer resolution.
    auto scoped    = std::make_shared<Context>(*m_ctx);
    scoped->schema = std::string(name);
    return SchemaClient(std::move(scoped));
}

postgrest::QueryBuilder Client::from(std::string_view table) const { return postgrest::QueryBuilder(m_ctx, std::string(table)); }

postgrest::FilterBuilder Client::rpc(std::string_view fn, const Json& args, postgrest::RpcOptions options) const
{
    return postgrest::makeRpc(m_ctx, fn, args, options);
}

}
