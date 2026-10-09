#pragma once

/**
 * @file supabase/storage/storage.hpp
 * @brief Supabase Storage bucket management, object operations, and access URLs.
 */

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

/// @brief Bucket metadata returned by the Storage service.
/// Missing string fields are empty; absent limits remain unset.
struct Bucket
{
    std::string id;                            ///< Bucket identifier used by StorageClient::from().
    std::string name;                          ///< Bucket name reported by the server.
    bool isPublic = false;                     ///< Whether the bucket permits public object access.
    std::optional<std::int64_t> fileSizeLimit; ///< Maximum object size in bytes, when reported.
    std::vector<std::string> allowedMimeTypes; ///< Allowed MIME types included in the response.
    std::string createdAt;                     ///< Creation timestamp as returned by the server.
    std::string updatedAt;                     ///< Last update timestamp as returned by the server.
};

/// @brief Settings sent when creating or updating a bucket.
/// isPublic is always sent. An unset size limit and an empty MIME list are omitted;
/// these defaults do not explicitly clear existing restrictions during an update.
struct BucketOptions
{
    bool isPublic = false;                     ///< Public-access setting; defaults to false.
    std::optional<std::int64_t> fileSizeLimit; ///< Optional upload size limit in bytes, validated by the server.
    std::vector<std::string> allowedMimeTypes; ///< Nonempty list of permitted MIME types to send.
};

/// @brief Upload/update headers and cancellation for an object request.
struct FileOptions
{
    std::string contentType  = "application/octet-stream"; ///< Content-Type header for the supplied bytes.
    std::string cacheControl = "3600";                     ///< Cache lifetime in seconds, sent as Cache-Control: max-age=<value>.
    bool upsert              = false;                      ///< x-upsert header requesting replacement when permitted by the server.
    http::CancelToken cancel;                              ///< Optional cancellation flag; set true to request transport abort.
};

/// @brief Object identifiers returned after a successful upload or update.
struct UploadResult
{
    std::string id;       ///< Server Id field, empty when not returned.
    std::string path;     ///< Original path supplied by the caller, relative to the bucket.
    std::string fullPath; ///< Server Key field, or bucket/path when that field is unavailable.
};

/// @brief Object or folder entry returned by listing or deletion operations.
struct FileObject
{
    std::string name;           ///< Entry name reported by the server.
    std::string id;             ///< Object identifier; empty when absent, including folder entries.
    std::string updatedAt;      ///< Last update timestamp, empty when absent.
    std::string createdAt;      ///< Creation timestamp, empty when absent.
    std::string lastAccessedAt; ///< Last access timestamp, empty when absent.
    Json metadata;              ///< Server object metadata; null when absent, including folder entries.
};

/// @brief Server-side pagination, ordering, and search for an object listing.
struct ListOptions
{
    int limit              = 100;    ///< Requested maximum entries; validated by the server.
    int offset             = 0;      ///< Requested listing offset; defaults to the first page.
    std::string sortColumn = "name"; ///< Field forwarded in sortBy.column.
    bool ascending         = true;   ///< Ascending when true, descending when false.
    std::string search;              ///< Search text forwarded to the listing endpoint.
};

/// @brief Object path and its time-limited access URL.
/// The URL includes access credentials. Protect it from unintended disclosure.
struct SignedUrl
{
    std::string path;      ///< Object path relative to its bucket.
    std::string signedUrl; ///< Absolute signed URL; may be empty for an unsuccessful batch entry.
};

/// @brief Image transformation parameters forwarded to the authenticated render endpoint.
/// Optional dimensions/quality and nonempty string fields are sent as query
/// parameters. Availability, supported values, and limits are enforced by the service.
struct TransformOptions
{
    std::optional<int> width;   ///< Optional target width in pixels.
    std::optional<int> height;  ///< Optional target height in pixels.
    std::string resize;         ///< Resize mode, such as "cover", "contain", or "fill".
    std::string format;         ///< Output format option forwarded to the server, such as "origin" or "avif".
    std::optional<int> quality; ///< Optional output quality forwarded without local validation.
};

/**
 * @brief Synchronous object operations bound to one Storage bucket.
 *
 * Obtain this value handle through StorageClient::from(). It retains the shared
 * request context and uses the project's transport, timeout, API key, and bearer
 * provider. Resolving authentication may refresh the current user session before
 * a Storage request is sent. Object paths are relative to this bucket; URL routes
 * encode individual path segments while preserving '/' separators.
 *
 * Permission checks, object conflicts, and upload/transform limits are enforced
 * by the service. Transport errors are propagated. Non-2xx responses produce
 * Error with code "StorageApiError" and the HTTP status, taking the message from
 * a JSON "message" or "error" field when available, otherwise from the response
 * body. Methods requiring JSON return errc::Parse for invalid JSON; upload/update
 * metadata handling is described separately below.
 *
 * Examples assume an initialized supabase::Client named `client` and are
 * independent snippets intended for use inside a function. Include
 * <supabase/supabase.hpp> for the complete public API.
 */
class StorageFileApi
{
  public:
    /// @brief Bind a bucket identifier to a shared project context.
    /// @param ctx Non-null context with project settings, authentication, and HTTP transport.
    /// @param bucket Bucket identifier used by subsequent file operations.
    /// @note Construction is local; the bucket's existence and permissions are not checked.
    StorageFileApi(std::shared_ptr<const Context> ctx, std::string bucket);

    /// @brief Upload object bytes using a POST request.
    /// @param path Destination object path relative to this bucket.
    /// @param data Object bytes, including any embedded nulls; copied into the request body.
    /// @param options Content type, cache lifetime, upsert preference, and optional cancellation flag.
    /// @return Requested path and available server identifiers, or a transport/server error.
    /// @note Existing-object conflicts and overwrite permissions are decided by the server.
    /// Missing or invalid response metadata retains HTTP success with an empty id and
    /// a bucket/path fallback when Key is unavailable.
    /// @code{.cpp}
    /// auto result = client.storage().from("documents").upload("notes/readme.txt", "Hello", {
    ///     .contentType = "text/plain; charset=utf-8",
    /// });
    /// if (!result)
    /// {
    ///     // Handle result.error().code, .message, and .status.
    /// }
    /// @endcode
    [[nodiscard]] Result<UploadResult> upload(std::string_view path, std::string_view data, const FileOptions& options = {}) const;

    /// @brief Write replacement object bytes using a PUT request.
    /// @param path Object path relative to this bucket.
    /// @param data Replacement bytes copied into the request body.
    /// @param options The same headers and cancellation options used by upload().
    /// @return Requested path and available server identifiers, or a transport/server error.
    /// @note Uses the same response metadata fallback as upload(); existence and permissions are server-controlled.
    [[nodiscard]] Result<UploadResult> update(std::string_view path, std::string_view data, const FileOptions& options = {}) const;

    /// @brief Download the complete object or authenticated image-render response.
    /// @param path Object path relative to this bucket.
    /// @param transform Optional image transformation parameters, borrowed for this call.
    /// @return Complete response bytes, which may contain nulls, or a transport/server error.
    /// @note A non-null transform selects /render/image/authenticated/ even when all
    /// its fields are unset. Null selects the regular /object/ endpoint. Data is buffered.
    /// @code{.cpp}
    /// auto result = client.storage().from("documents").download("notes/readme.txt");
    /// if (result)
    /// {
    ///     // Consume the bytes in result.value(); no text or JSON decoding is performed.
    /// }
    /// @endcode
    [[nodiscard]] Result<std::string> download(std::string_view path, const TransformOptions* transform = nullptr) const;

    /// @brief Request deletion of a set of objects from this bucket.
    /// @param paths Object paths forwarded in the request's prefixes array.
    /// @return Entries parsed from the server's deletion response, or an error.
    /// @note A valid non-array JSON response yields an empty result list.
    [[nodiscard]] Result<std::vector<FileObject>> remove(const std::vector<std::string>& paths) const;

    /// @brief Fetch one page of object or folder entries under a prefix.
    /// @param folder Prefix forwarded to the listing endpoint; empty requests the bucket root.
    /// @param options Requested page size, offset, sorting, and search text.
    /// @return Listed entries, or a transport/server/JSON parse error.
    /// @note Does not automatically fetch further pages or recurse into folders.
    /// A valid non-array JSON response yields an empty result list.
    /// @code{.cpp}
    /// auto result = client.storage().from("documents").list("notes", {
    ///     .limit = 20,
    ///     .offset = 0,
    ///     .sortColumn = "name",
    ///     .ascending = true,
    /// });
    /// @endcode
    [[nodiscard]] Result<std::vector<FileObject>> list(std::string_view folder = {}, const ListOptions& options = {}) const;

    /// @brief Request an object move within this bucket.
    /// @param from Source object path relative to the bucket.
    /// @param to Destination object path relative to the same bucket.
    /// @return Success on HTTP 2xx, or a transport/server error; response data is discarded.
    [[nodiscard]] Result<void> move(std::string_view from, std::string_view to) const;

    /// @brief Request an object copy within this bucket.
    /// @param from Source object path relative to the bucket.
    /// @param to Destination object path relative to the same bucket.
    /// @return Success on HTTP 2xx, or a transport/server error; response data is discarded.
    [[nodiscard]] Result<void> copy(std::string_view from, std::string_view to) const;

    /// @brief Request a time-limited access URL for one object.
    /// @param path Object path relative to this bucket.
    /// @param expiresInSeconds Requested URL lifetime in seconds, forwarded without local validation.
    /// @return Original path and an absolute URL assembled from the server's signedURL field, or an error.
    /// @note Invalid JSON or a missing, empty, or non-string signedURL produces errc::Parse.
    /// @code{.cpp}
    /// auto result = client.storage().from("documents").createSignedUrl("notes/readme.txt", 60);
    /// if (result)
    /// {
    ///     // Provide result->signedUrl to the intended recipient for temporary access.
    /// }
    /// @endcode
    [[nodiscard]] Result<SignedUrl> createSignedUrl(std::string_view path, int expiresInSeconds) const;

    /// @brief Request time-limited access URLs for multiple objects in one call.
    /// @param paths Object paths relative to this bucket.
    /// @param expiresInSeconds Requested URL lifetime in seconds, forwarded to the service.
    /// @return Parsed response entries, or a request/JSON parse error.
    /// @note Entries missing signedURL retain an empty signedUrl; individual error
    /// fields are not represented by SignedUrl. Check each entry before using it.
    /// A valid non-array JSON response yields an empty result list.
    [[nodiscard]] Result<std::vector<SignedUrl>> createSignedUrls(const std::vector<std::string>& paths, int expiresInSeconds) const;

    /// @brief Construct the public object URL without contacting the service.
    /// @param path Object path relative to this bucket, encoded per path segment.
    /// @param download Append an empty download query parameter to request attachment handling.
    /// @return URL under /storage/v1/object/public/ with no access token.
    /// @note Does not verify that the object exists or the bucket permits public access.
    /// @code{.cpp}
    /// auto url = client.storage().from("public-images").getPublicUrl("covers/game.png");
    /// @endcode
    [[nodiscard]] std::string getPublicUrl(std::string_view path, bool download = false) const;

  private:
    [[nodiscard]] Result<UploadResult>
    put(http::Method method, std::string_view path, std::string_view data, const FileOptions& options) const;

    std::shared_ptr<const Context> m_ctx;
    std::string m_bucket;
};

/**
 * @brief Bucket management and per-bucket object entry points for Supabase Storage.
 *
 * Obtain this API through supabase::Client::storage(). Bucket requests are
 * synchronous and use the shared project's timeout, transport, and authentication.
 * The service enforces bucket-management permissions. Request errors follow the
 * StorageApiError and transport-error behavior described by StorageFileApi.
 *
 * BucketOptions is not a partial-update model for visibility: isPublic is always
 * sent. Set it explicitly when preserving a bucket's existing public setting.
 */
class StorageClient
{
  public:
    /// @brief Retain the shared request context for Storage operations.
    /// @param ctx Non-null context containing project settings, authentication, and HTTP transport.
    /// @note Construction does not contact the service.
    explicit StorageClient(std::shared_ptr<const Context> ctx);

    /// @brief Create a value handle for file operations in a bucket.
    /// @param bucket Bucket identifier, encoded when used in URL routes.
    /// @return StorageFileApi retaining the shared context; no request or bucket validation occurs.
    [[nodiscard]] StorageFileApi from(std::string_view bucket) const;

    /// @brief Fetch bucket metadata visible to the current credentials.
    /// @return Bucket entries, or a transport/server/JSON parse error.
    /// @note A valid non-array JSON response yields an empty result list.
    [[nodiscard]] Result<std::vector<Bucket>> listBuckets() const;

    /// @brief Fetch metadata for one bucket.
    /// @param id Bucket identifier, URL-encoded by the client.
    /// @return Parsed bucket metadata, or a transport/server/JSON parse error.
    [[nodiscard]] Result<Bucket> getBucket(std::string_view id) const;

    /// @brief Create a bucket with the requested access and upload settings.
    /// @param id Identifier sent as both the bucket's id and name.
    /// @param options Visibility, optional size limit, and optional MIME restrictions.
    /// @return Success on HTTP 2xx, or a transport/server error.
    /// @note Default options request a private bucket. No metadata response is parsed.
    /// @code{.cpp}
    /// auto result = client.storage().createBucket("documents", {
    ///     .isPublic = false,
    ///     .fileSizeLimit = 5 * 1024 * 1024,
    ///     .allowedMimeTypes = { "text/plain", "application/pdf" },
    /// });
    /// @endcode
    [[nodiscard]] Result<void> createBucket(std::string_view id, const BucketOptions& options = {}) const;

    /// @brief Update a bucket's visibility and any supplied upload restrictions.
    /// @param id Identifier of the bucket to update.
    /// @param options isPublic is always sent; unset fileSizeLimit and empty allowedMimeTypes are omitted.
    /// @return Success on HTTP 2xx, or a transport/server error.
    [[nodiscard]] Result<void> updateBucket(std::string_view id, const BucketOptions& options) const;

    /// @brief Request removal of a bucket's contents while retaining the bucket.
    /// @param id Bucket identifier.
    /// @return Success on HTTP 2xx, or a transport/server error.
    [[nodiscard]] Result<void> emptyBucket(std::string_view id) const;

    /// @brief Request deletion of a bucket.
    /// @param id Bucket identifier.
    /// @return Success on HTTP 2xx, or a transport/server error.
    /// @note Does not empty the bucket first; the service decides whether deletion is permitted.
    [[nodiscard]] Result<void> deleteBucket(std::string_view id) const;

  private:
    std::shared_ptr<const Context> m_ctx;
};

}
