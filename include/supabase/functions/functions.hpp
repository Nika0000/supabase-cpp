#pragma once

#include <supabase/context.hpp>
#include <supabase/json.hpp>
#include <supabase/result.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace supabase::functions
{

struct InvokeOptions
{
    http::Method method = http::Method::Post;
    std::optional<Json> json; ///< sent as application/json
    std::string body;         ///< raw body, used when `json` is empty
    std::string contentType;  ///< override for raw bodies
    http::Headers headers;
    std::string region; ///< e.g. "eu-west-1"; sent as x-region
    http::CancelToken cancel;
};

struct FunctionResponse
{
    int status = 0;
    http::Headers headers;
    std::string body;

    /// Parse the body as JSON; null Json when it is not valid JSON.
    [[nodiscard]] Json json() const { return Json::parse(body, nullptr, false); }
};

/// Edge Functions client. Mirrors supabase-js `functions`.
class FunctionsClient
{
  public:
    explicit FunctionsClient(std::shared_ptr<const Context> ctx);

    /// Non-2xx statuses are returned as `Error` (code "FunctionsHttpError").
    [[nodiscard]] Result<FunctionResponse> invoke(std::string_view name, const InvokeOptions& options = {}) const;

    /// Streams the response body (SSE etc.) to `onChunk`; return false to stop.
    [[nodiscard]] Result<int> invokeStream(std::string_view name, const InvokeOptions& options, http::ChunkCallback onChunk) const;

  private:
    [[nodiscard]] http::Request build(std::string_view name, const InvokeOptions& options) const;

    std::shared_ptr<const Context> m_ctx;
};

}
