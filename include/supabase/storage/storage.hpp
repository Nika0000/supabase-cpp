#pragma once

#include <cstdint>

#include <supabase/context.hpp>
#include <supabase/json.hpp>
#include <supabase/result.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace supabase::storage
{

struct Bucket
{
    std::string id;
    std::string name;
    bool isPublic = false;
    std::optional<std::int64_t> fileSizeLimit;
    std::vector<std::string> allowedMimeTypes;
    std::string createdAt;
    std::string updatedAt;
};

struct BucketOptions
{
    bool isPublic = false;
    std::optional<std::int64_t> fileSizeLimit; ///< bytes
    std::vector<std::string> allowedMimeTypes;
};

struct FileOptions
{
    std::string contentType  = "application/octet-stream";
    std::string cacheControl = "3600"; ///< seconds
    bool upsert              = false;
    http::CancelToken cancel;
};

struct UploadResult
{
    std::string id;
    std::string path;
    std::string fullPath; ///< `bucket/path`
};

struct FileObject
{
    std::string name;
    std::string id; ///< empty for folders
    std::string updatedAt;
    std::string createdAt;
    std::string lastAccessedAt;
    Json metadata; ///< null for folders
};

struct ListOptions
{
    int limit              = 100;
    int offset             = 0;
    std::string sortColumn = "name";
    bool ascending         = true;
    std::string search;
};

struct SignedUrl
{
    std::string path;
    std::string signedUrl;
};

struct TransformOptions
{
    std::optional<int> width;
    std::optional<int> height;
    std::string resize; ///< cover | contain | fill
    std::string format; ///< origin | avif
    std::optional<int> quality;
};

/// Per-bucket file operations. Mirrors supabase-js `StorageFileApi`.
class StorageFileApi
{
  public:
    StorageFileApi(std::shared_ptr<const Context> ctx, std::string bucket);

    /// POST. Fails with 409 when the object exists and `options.upsert` is false.
    [[nodiscard]] Result<UploadResult> upload(std::string_view path, std::string_view data, const FileOptions& options = {}) const;
    /// PUT. Replaces an existing object.
    [[nodiscard]] Result<UploadResult> update(std::string_view path, std::string_view data, const FileOptions& options = {}) const;
    [[nodiscard]] Result<std::string> download(std::string_view path, const TransformOptions* transform = nullptr) const;
    /// Removes objects; returns the deleted ones.
    [[nodiscard]] Result<std::vector<FileObject>> remove(const std::vector<std::string>& paths) const;
    [[nodiscard]] Result<std::vector<FileObject>> list(std::string_view folder = {}, const ListOptions& options = {}) const;
    [[nodiscard]] Result<void> move(std::string_view from, std::string_view to) const;
    [[nodiscard]] Result<void> copy(std::string_view from, std::string_view to) const;
    /// `expiresInSeconds` is the signed URL lifetime.
    [[nodiscard]] Result<SignedUrl> createSignedUrl(std::string_view path, int expiresInSeconds) const;
    [[nodiscard]] Result<std::vector<SignedUrl>> createSignedUrls(const std::vector<std::string>& paths, int expiresInSeconds) const;
    /// Pure URL construction for public buckets; no request is made.
    [[nodiscard]] std::string getPublicUrl(std::string_view path, bool download = false) const;

  private:
    [[nodiscard]] Result<UploadResult>
    put(http::Method method, std::string_view path, std::string_view data, const FileOptions& options) const;

    std::shared_ptr<const Context> m_ctx;
    std::string m_bucket;
};

/// Storage client. Mirrors supabase-js `storage`.
class StorageClient
{
  public:
    explicit StorageClient(std::shared_ptr<const Context> ctx);
    [[nodiscard]] StorageFileApi from(std::string_view bucket) const;

    [[nodiscard]] Result<std::vector<Bucket>> listBuckets() const;
    [[nodiscard]] Result<Bucket> getBucket(std::string_view id) const;
    [[nodiscard]] Result<void> createBucket(std::string_view id, const BucketOptions& options = {}) const;
    [[nodiscard]] Result<void> updateBucket(std::string_view id, const BucketOptions& options) const;
    [[nodiscard]] Result<void> emptyBucket(std::string_view id) const;
    [[nodiscard]] Result<void> deleteBucket(std::string_view id) const;

  private:
    std::shared_ptr<const Context> m_ctx;
};

}
