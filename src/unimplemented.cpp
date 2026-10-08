#include <supabase/graphql/graphql.hpp>

// Placeholders for roadmap modules. The public shapes are fixed; implementations land later.
namespace supabase
{

namespace
{
    Error notImplemented(std::string_view what) { return makeError(errc::NotImplemented, std::string(what) + " is not implemented yet"); }
}

namespace graphql
{
    GraphQLClient::GraphQLClient(std::shared_ptr<const Context> ctx) : ctx_(std::move(ctx)) {}
    Result<Json> GraphQLClient::query(std::string_view, const Json&) const { return notImplemented("graphql.query"); }
}

}
