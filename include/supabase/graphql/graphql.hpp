#pragma once

/**
 * @file supabase/graphql/graphql.hpp
 * @brief Reserved API for the Supabase pg_graphql endpoint.
 * @note GraphQL requests are not implemented in this version of the SDK.
 */

#include <supabase/context.hpp>
#include <supabase/json.hpp>
#include <supabase/result.hpp>

#include <memory>
#include <string_view>

namespace supabase::graphql
{

/**
 * @brief Placeholder client for the Supabase /graphql/v1 endpoint.
 *
 * Obtain this API through supabase::Client::graphql(). The client retains the
 * shared project context, but query() currently returns errc::NotImplemented
 * for every call. It does not send a request, inspect the document, or change
 * authentication state.
 *
 * @par Example: handle the current unsupported operation
 * @code{.cpp}
 * auto result = client.graphql().query("query { __typename }");
 * if (!result && result.error().code == supabase::errc::NotImplemented)
 * {
 *     // GraphQL execution is unavailable in this SDK version.
 * }
 * @endcode
 * The example assumes an initialized supabase::Client named `client` and is
 * intended for use inside a function. Include <supabase/supabase.hpp> for the
 * complete public API.
 */
class GraphQLClient
{
  public:
    /// @brief Retain a shared project context for the reserved GraphQL interface.
    /// @param ctx Shared request context supplied by the owning Supabase client.
    /// @note Construction does not make an HTTP request.
    explicit GraphQLClient(std::shared_ptr<const Context> ctx);

    /// @brief Report that GraphQL execution is not implemented.
    /// @param document GraphQL document reserved for future execution; currently ignored.
    /// @param variables Variable bindings reserved for future execution; currently ignored.
    /// @return Always an Error with code errc::NotImplemented and HTTP status zero.
    /// @note Empty or malformed inputs also return NotImplemented. No server
    /// request, document validation, or JSON response parsing occurs.
    [[nodiscard]] Result<Json> query(std::string_view document, const Json& variables = Json::object()) const;

  private:
    std::shared_ptr<const Context> m_ctx;
};

}
