#pragma once

#include <supabase/context.hpp>
#include <supabase/result.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace supabase::storage
{

/// Planned: per-bucket file operations (upload, download, list, remove, signed URLs). Not implemented yet.
class StorageFileApi
{
  public:
    StorageFileApi(std::shared_ptr<const Context> ctx, std::string bucket);

    [[nodiscard]] Result<void> upload(std::string_view path, std::string_view data) const;
    [[nodiscard]] Result<std::string> download(std::string_view path) const;
    [[nodiscard]] Result<void> remove(std::string_view path) const;

  private:
    std::shared_ptr<const Context> ctx_;
    std::string bucket_;
};

/// Storage client. Mirrors supabase-js `storage`. Every call returns `NotImplemented` for now.
class StorageClient
{
  public:
    explicit StorageClient(std::shared_ptr<const Context> ctx);
    [[nodiscard]] StorageFileApi from(std::string_view bucket) const;

  private:
    std::shared_ptr<const Context> ctx_;
};

}
