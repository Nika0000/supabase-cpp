#include <supabase/functions/functions.hpp>

namespace supabase::functions
{

namespace
{
    Error toError(const http::Response& response)
    {
        Error error     = makeError("FunctionsHttpError", response.body, response.status);
        const Json body = Json::parse(response.body, nullptr, false);
        if (body.is_object())
        {
            for (const char* key : { "error", "message" })
            {
                const auto it = body.find(key);
                if (it != body.end() && it->is_string())
                {
                    error.message = it->get<std::string>();
                    break;
                }
            }
        }
        return error;
    }
}

FunctionsClient::FunctionsClient(std::shared_ptr<const Context> ctx) : m_ctx(std::move(ctx)) {}

http::Request FunctionsClient::build(std::string_view name, const InvokeOptions& options) const
{
    http::Request request;
    request.method  = options.method;
    request.url     = m_ctx->endpoint("/functions/v1/") + std::string(name);
    request.timeout = m_ctx->timeout;
    request.cancel  = options.cancel;
    request.headers = m_ctx->baseHeaders();
    if (!options.region.empty())
        request.headers.emplace_back("x-region", options.region);

    if (options.json)
    {
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = options.json->dump();
    }
    else if (!options.body.empty())
    {
        request.headers.emplace_back("Content-Type", options.contentType.empty() ? "text/plain" : options.contentType);
        request.body = options.body;
    }
    request.headers.insert(request.headers.end(), options.headers.begin(), options.headers.end());
    return request;
}

Result<FunctionResponse> FunctionsClient::invoke(std::string_view name, const InvokeOptions& options) const
{
    auto sent = m_ctx->transport->send(build(name, options));
    if (!sent)
        return sent.error();
    http::Response& response = sent.value();
    if (!response.ok())
        return toError(response);
    return FunctionResponse { response.status, std::move(response.headers), std::move(response.body) };
}

Result<int> FunctionsClient::invokeStream(std::string_view name, const InvokeOptions& options, http::ChunkCallback onChunk) const
{
    if (!onChunk)
        return makeError(errc::InvalidArgument, "onChunk is required");
    auto sent = m_ctx->transport->send(build(name, options), std::move(onChunk));
    if (!sent)
        return sent.error();
    if (!sent.value().ok())
        return makeError("FunctionsHttpError", "function returned HTTP " + std::to_string(sent.value().status), sent.value().status);
    return sent.value().status;
}

}
