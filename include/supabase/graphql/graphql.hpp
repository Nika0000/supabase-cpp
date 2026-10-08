#pragma once

#include <supabase/context.hpp>
#include <supabase/json.hpp>
#include <supabase/result.hpp>

#include <memory>
#include <string_view>

namespace supabase::graphql
{

/// pg_graphql client (`/graphql/v1`). Not implemented yet; returns `NotImplemented`.
class GraphQLClient
{
  public:
    explicit GraphQLClient(std::shared_ptr<const Context> ctx);
    [[nodiscard]] Result<Json> query(std::string_view document, const Json& variables = Json::object()) const;

  private:
    std::shared_ptr<const Context> ctx_;
};

}
