
#include <supabase/postgrest/builder.hpp> // urlEncode
#include <supabase/storage/storage.hpp>

#include <algorithm>

namespace supabase::storage
{

namespace
{
    constexpr std::string_view kRoot = "/storage/v1";

    std::string str(const Json& object, const char* key)
    {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    /// Percent-encodes each path segment but keeps the separators.
    std::string encodePath(std::string_view path)
    {
        std::string out;
        size_t start = 0;
        while (start <= path.size())
        {
            const size_t end = std::min(path.find('/', start), path.size());
            out += postgrest::urlEncode(path.substr(start, end - start));
            if (end == path.size())
                break;
            out += '/';
            start = end + 1;
        }
        return out;
    }

    Error toError(const http::Response& response)
    {
        Error error     = makeError("StorageApiError", response.body, response.status);
        const Json body = Json::parse(response.body, nullptr, false);
        if (body.is_object())
        {
            if (const auto message = str(body, "message"); !message.empty())
                error.message = message;
            else if (const auto text = str(body, "error"); !text.empty())
                error.message = text;
        }
        return error;
    }

    Result<http::Response> run(const Context& ctx, http::Request request, const http::CancelToken& cancel = {})
    {
        request.timeout = ctx.timeout;
        request.cancel  = cancel;
        auto sent       = ctx.transport->send(request);
        if (!sent)
            return sent.error();
        if (!sent.value().ok())
            return toError(sent.value());
        return std::move(sent.value());
    }

    http::Request makeRequest(const Context& ctx, http::Method method, const std::string& route)
    {
        http::Request request;
        request.method  = method;
        request.url     = ctx.endpoint(kRoot) + route;
        request.headers = ctx.baseHeaders();
        return request;
    }

    http::Request jsonRequest(const Context& ctx, http::Method method, const std::string& route, const Json& body)
    {
        http::Request request = makeRequest(ctx, method, route);
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = body.dump();
        return request;
    }

    Result<Json> parseBody(const http::Response& response)
    {
        Json body = Json::parse(response.body, nullptr, false);
        if (body.is_discarded())
            return makeError(errc::Parse, "invalid JSON in storage response", response.status);
        return body;
    }

    Bucket toBucket(const Json& json)
    {
        Bucket bucket;
        bucket.id        = str(json, "id");
        bucket.name      = str(json, "name");
        bucket.createdAt = str(json, "created_at");
        bucket.updatedAt = str(json, "updated_at");
        bucket.isPublic  = json.value("public", false);
        if (const auto it = json.find("file_size_limit"); it != json.end() && it->is_number_integer())
            bucket.fileSizeLimit = it->get<std::int64_t>();
        if (const auto it = json.find("allowed_mime_types"); it != json.end() && it->is_array())
        {
            for (const auto& mime : *it)
            {
                if (mime.is_string())
                    bucket.allowedMimeTypes.push_back(mime.get<std::string>());
            }
        }
        return bucket;
    }

    FileObject toFileObject(const Json& json)
    {
        FileObject file;
        file.name           = str(json, "name");
        file.id             = str(json, "id");
        file.updatedAt      = str(json, "updated_at");
        file.createdAt      = str(json, "created_at");
        file.lastAccessedAt = str(json, "last_accessed_at");
        if (const auto it = json.find("metadata"); it != json.end())
            file.metadata = *it;
        return file;
    }

    Result<std::vector<FileObject>> toFileObjects(const http::Response& response)
    {
        auto body = parseBody(response);
        if (!body)
            return body.error();
        std::vector<FileObject> files;
        if (body.value().is_array())
        {
            for (const auto& item : body.value())
                files.push_back(toFileObject(item));
        }
        return files;
    }

    Json bucketBody(const BucketOptions& options)
    {
        Json body = { { "public", options.isPublic } };
        if (options.fileSizeLimit)
            body["file_size_limit"] = *options.fileSizeLimit;
        if (!options.allowedMimeTypes.empty())
            body["allowed_mime_types"] = options.allowedMimeTypes;
        return body;
    }

    std::string transformQuery(const TransformOptions& transform)
    {
        std::string query;
        const auto add = [&query](std::string_view key, const std::string& value)
        {
            query += query.empty() ? '?' : '&';
            query += std::string(key) + "=" + postgrest::urlEncode(value);
        };
        if (transform.width)
            add("width", std::to_string(*transform.width));
        if (transform.height)
            add("height", std::to_string(*transform.height));
        if (!transform.resize.empty())
            add("resize", transform.resize);
        if (!transform.format.empty())
            add("format", transform.format);
        if (transform.quality)
            add("quality", std::to_string(*transform.quality));
        return query;
    }

    Result<void> discard(const Result<http::Response>& response)
    {
        if (!response)
            return response.error();
        return {};
    }
}

StorageFileApi::StorageFileApi(std::shared_ptr<const Context> ctx, std::string bucket) : m_ctx(std::move(ctx)), m_bucket(std::move(bucket))
{
}

Result<UploadResult>
StorageFileApi::put(http::Method method, std::string_view path, std::string_view data, const FileOptions& options) const
{
    const std::string key = std::string(path);
    http::Request request = makeRequest(*m_ctx, method, "/object/" + postgrest::urlEncode(m_bucket) + "/" + encodePath(key));
    request.headers.emplace_back("Content-Type", options.contentType);
    request.headers.emplace_back("cache-control", "max-age=" + options.cacheControl);
    request.headers.emplace_back("x-upsert", options.upsert ? "true" : "false");
    request.body = std::string(data);

    auto response = run(*m_ctx, std::move(request), options.cancel);
    if (!response)
        return response.error();
    const Json body = Json::parse(response.value().body, nullptr, false);
    UploadResult result;
    result.path = key;
    if (body.is_object())
    {
        result.id       = str(body, "Id");
        result.fullPath = str(body, "Key");
    }
    if (result.fullPath.empty())
        result.fullPath = m_bucket + "/" + key;
    return result;
}

Result<UploadResult> StorageFileApi::upload(std::string_view path, std::string_view data, const FileOptions& options) const
{
    return put(http::Method::Post, path, data, options);
}

Result<UploadResult> StorageFileApi::update(std::string_view path, std::string_view data, const FileOptions& options) const
{
    return put(http::Method::Put, path, data, options);
}

Result<std::string> StorageFileApi::download(std::string_view path, const TransformOptions* transform) const
{
    std::string route = transform ? "/render/image/authenticated/" : "/object/";
    route += postgrest::urlEncode(m_bucket) + "/" + encodePath(path);
    if (transform)
        route += transformQuery(*transform);

    auto response = run(*m_ctx, makeRequest(*m_ctx, http::Method::Get, route));
    if (!response)
        return response.error();
    return std::move(response.value().body);
}

Result<std::vector<FileObject>> StorageFileApi::remove(const std::vector<std::string>& paths) const
{
    auto response
        = run(*m_ctx, jsonRequest(*m_ctx, http::Method::Delete, "/object/" + postgrest::urlEncode(m_bucket), { { "prefixes", paths } }));
    if (!response)
        return response.error();
    return toFileObjects(response.value());
}

Result<std::vector<FileObject>> StorageFileApi::list(std::string_view folder, const ListOptions& options) const
{
    const Json payload = {
        { "prefix", std::string(folder) },
        { "limit", options.limit },
        { "offset", options.offset },
        { "sortBy", { { "column", options.sortColumn }, { "order", options.ascending ? "asc" : "desc" } } },
        { "search", options.search },
    };

    auto response = run(*m_ctx, jsonRequest(*m_ctx, http::Method::Post, "/object/list/" + postgrest::urlEncode(m_bucket), payload));
    if (!response)
        return response.error();
    return toFileObjects(response.value());
}

Result<void> StorageFileApi::move(std::string_view from, std::string_view to) const
{
    const Json payload = { { "bucketId", m_bucket }, { "sourceKey", std::string(from) }, { "destinationKey", std::string(to) } };
    return discard(run(*m_ctx, jsonRequest(*m_ctx, http::Method::Post, "/object/move", payload)));
}

Result<void> StorageFileApi::copy(std::string_view from, std::string_view to) const
{
    const Json payload = { { "bucketId", m_bucket }, { "sourceKey", std::string(from) }, { "destinationKey", std::string(to) } };
    return discard(run(*m_ctx, jsonRequest(*m_ctx, http::Method::Post, "/object/copy", payload)));
}

Result<SignedUrl> StorageFileApi::createSignedUrl(std::string_view path, int expiresInSeconds) const
{
    const std::string key = std::string(path);
    auto response         = run(
        *m_ctx,
        jsonRequest(
            *m_ctx,
            http::Method::Post,
            "/object/sign/" + postgrest::urlEncode(m_bucket) + "/" + encodePath(key),
            { { "expiresIn", expiresInSeconds } }
        )
    );
    if (!response)
        return response.error();
    auto body = parseBody(response.value());
    if (!body)
        return body.error();
    const std::string relative = body.value().is_object() ? str(body.value(), "signedURL") : std::string();
    if (relative.empty())
        return makeError(errc::Parse, "missing signedURL in storage response", response.value().status);
    return SignedUrl { key, m_ctx->endpoint(kRoot) + relative };
}

Result<std::vector<SignedUrl>> StorageFileApi::createSignedUrls(const std::vector<std::string>& paths, int expiresInSeconds) const
{
    auto response = run(
        *m_ctx,
        jsonRequest(
            *m_ctx,
            http::Method::Post,
            "/object/sign/" + postgrest::urlEncode(m_bucket),
            { { "expiresIn", expiresInSeconds }, { "paths", paths } }
        )
    );
    if (!response)
        return response.error();
    auto body = parseBody(response.value());
    if (!body)
        return body.error();
    std::vector<SignedUrl> urls;
    if (body.value().is_array())
    {
        for (const auto& item : body.value())
        {
            const std::string relative = str(item, "signedURL");
            urls.push_back({ str(item, "path"), relative.empty() ? std::string() : m_ctx->endpoint(kRoot) + relative });
        }
    }
    return urls;
}

std::string StorageFileApi::getPublicUrl(std::string_view path, bool download) const
{
    std::string url = m_ctx->endpoint(kRoot) + "/object/public/" + postgrest::urlEncode(m_bucket) + "/" + encodePath(path);
    if (download)
        url += "?download=";
    return url;
}

StorageClient::StorageClient(std::shared_ptr<const Context> ctx) : m_ctx(std::move(ctx)) {}

StorageFileApi StorageClient::from(std::string_view bucket) const { return StorageFileApi(m_ctx, std::string(bucket)); }

Result<std::vector<Bucket>> StorageClient::listBuckets() const
{
    auto response = run(*m_ctx, makeRequest(*m_ctx, http::Method::Get, "/bucket"));
    if (!response)
        return response.error();
    auto body = parseBody(response.value());
    if (!body)
        return body.error();
    std::vector<Bucket> buckets;
    if (body.value().is_array())
    {
        for (const auto& item : body.value())
            buckets.push_back(toBucket(item));
    }
    return buckets;
}

Result<Bucket> StorageClient::getBucket(std::string_view id) const
{
    auto response = run(*m_ctx, makeRequest(*m_ctx, http::Method::Get, "/bucket/" + postgrest::urlEncode(id)));
    if (!response)
        return response.error();
    auto body = parseBody(response.value());
    if (!body)
        return body.error();
    return toBucket(body.value());
}

Result<void> StorageClient::createBucket(std::string_view id, const BucketOptions& options) const
{
    Json body    = bucketBody(options);
    body["id"]   = std::string(id);
    body["name"] = std::string(id);
    return discard(run(*m_ctx, jsonRequest(*m_ctx, http::Method::Post, "/bucket", body)));
}

Result<void> StorageClient::updateBucket(std::string_view id, const BucketOptions& options) const
{
    return discard(run(*m_ctx, jsonRequest(*m_ctx, http::Method::Put, "/bucket/" + postgrest::urlEncode(id), bucketBody(options))));
}

Result<void> StorageClient::emptyBucket(std::string_view id) const
{
    return discard(run(*m_ctx, jsonRequest(*m_ctx, http::Method::Post, "/bucket/" + postgrest::urlEncode(id) + "/empty", Json::object())));
}

Result<void> StorageClient::deleteBucket(std::string_view id) const
{
    return discard(run(*m_ctx, makeRequest(*m_ctx, http::Method::Delete, "/bucket/" + postgrest::urlEncode(id))));
}

}
