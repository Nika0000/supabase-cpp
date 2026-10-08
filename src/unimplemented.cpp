#include <supabase/graphql/graphql.hpp>
#include <supabase/storage/storage.hpp>

// Placeholders for roadmap modules. The public shapes are fixed; implementations land later.
namespace supabase
{

namespace
{
    Error notImplemented(std::string_view what) { return makeError(errc::NotImplemented, std::string(what) + " is not implemented yet"); }
}

namespace storage
{
    StorageFileApi::StorageFileApi(std::shared_ptr<const Context> ctx, std::string bucket)
        : ctx_(std::move(ctx)), bucket_(std::move(bucket))
    {
    }
    Result<void> StorageFileApi::upload(std::string_view, std::string_view) const { return notImplemented("storage.upload"); }
    Result<std::string> StorageFileApi::download(std::string_view) const { return notImplemented("storage.download"); }
    Result<void> StorageFileApi::remove(std::string_view) const { return notImplemented("storage.remove"); }

    StorageClient::StorageClient(std::shared_ptr<const Context> ctx) : ctx_(std::move(ctx)) {}
    StorageFileApi StorageClient::from(std::string_view bucket) const { return StorageFileApi(ctx_, std::string(bucket)); }
}

namespace graphql
{
    GraphQLClient::GraphQLClient(std::shared_ptr<const Context> ctx) : ctx_(std::move(ctx)) {}
    Result<Json> GraphQLClient::query(std::string_view, const Json&) const { return notImplemented("graphql.query"); }
}

}
