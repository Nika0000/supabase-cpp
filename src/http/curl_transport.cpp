#include <curl/curl.h>

#include <supabase/http.hpp>

#include <algorithm>
#include <mutex>
#include <thread>

namespace supabase::http
{

namespace
{
    void ensureCurlGlobalInit()
    {
        static const bool kInitialized = (curl_global_init(CURL_GLOBAL_DEFAULT), true);
        (void) kInitialized;
    }

    struct Transfer
    {
        Response* response           = nullptr;
        const ChunkCallback* onChunk = nullptr;
        const CancelToken* cancel    = nullptr;
        bool stoppedByCallback       = false;
    };

    std::size_t onBody(char* data, std::size_t size, std::size_t count, void* user)
    {
        auto& transfer          = *static_cast<Transfer*>(user);
        const std::size_t bytes = size * count;
        if (transfer.onChunk != nullptr && *transfer.onChunk)
        {
            if (!(*transfer.onChunk)(std::string_view(data, bytes)))
            {
                transfer.stoppedByCallback = true;
                return 0;
            }
            return bytes;
        }
        transfer.response->body.append(data, bytes);
        return bytes;
    }

    std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* user)
    {
        auto& transfer          = *static_cast<Transfer*>(user);
        const std::size_t bytes = size * count;

        std::string_view line(data, bytes);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        {
            line.remove_suffix(1);
        }

        if (line.starts_with("HTTP/"))
        {
            transfer.response->headers.clear(); // new response block (redirect / 100-continue)
            return bytes;
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
        {
            return bytes;
        }

        auto value = line.substr(colon + 1);

        while (!value.empty() && value.front() == ' ')
        {
            value.remove_prefix(1);
        }

        transfer.response->headers.emplace_back(std::string(line.substr(0, colon)), std::string(value));
        return bytes;
    }

    int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const auto& transfer = *static_cast<const Transfer*>(user);
        return transfer.cancel != nullptr && *transfer.cancel && (*transfer.cancel)->load() ? 1 : 0;
    }

    bool isIdempotent(Method method) { return method != Method::Post && method != Method::Patch; }

    bool isRetryableStatus(int status) { return status == 502 || status == 503 || status == 504; }

    class CurlTransport final : public Transport
    {
      public:
        explicit CurlTransport(CurlOptions options) : m_options(std::move(options)) { ensureCurlGlobalInit(); }

        ~CurlTransport() override
        {
            for (CURL* handle : m_pool)
            {
                curl_easy_cleanup(handle);
            }
        }

        CurlTransport(const CurlTransport&)            = delete;
        CurlTransport& operator=(const CurlTransport&) = delete;

        Result<Response> send(const Request& request, ChunkCallback onChunk) override
        {
            const bool canRetry = !onChunk && isIdempotent(request.method);
            const int attempts  = 1 + (canRetry ? std::max<int>(0, m_options.maxRetries) : 0);

            for (int attempt = 1;; ++attempt)
            {
                auto result = attemptOnce(request, onChunk);
                const bool retry
                    = attempt < attempts && (result ? isRetryableStatus(result.value().status) : result.error().code == errc::Network);
                if (!retry)
                    return result;
                if (request.cancel && request.cancel->load())
                    return makeError(errc::Cancelled, "request cancelled");
                std::this_thread::sleep_for(std::chrono::milliseconds(200 << (attempt - 1)));
            }
        }

      private:
        CURL* acquire()
        {
            {
                const std::lock_guard lock(m_mutex);
                if (!m_pool.empty())
                {
                    CURL* handle = m_pool.back();
                    m_pool.pop_back();
                    return handle;
                }
            }
            return curl_easy_init();
        }

        void release(CURL* handle)
        {
            const std::lock_guard lock(m_mutex);
            if (m_pool.size() < kMaxPooled)
                m_pool.push_back(handle);
            else
                curl_easy_cleanup(handle);
        }

        Result<Response> attemptOnce(const Request& request, const ChunkCallback& onChunk)
        {
            CURL* curl = acquire();
            if (curl == nullptr)
                return makeError(errc::Network, "curl_easy_init failed");
            curl_easy_reset(curl);

            Response response;
            Transfer transfer { .response = &response, .onChunk = &onChunk, .cancel = &request.cancel, .stoppedByCallback = false };

            curl_slist* headers = nullptr;
            for (const auto& [name, value] : request.headers)
                headers = curl_slist_append(headers, (name + ": " + value).c_str());

            const std::string_view method = toString(request.method);
            const std::string methodName(method);

            curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, m_options.userAgent.c_str());
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(m_options.connectTimeout.count()));
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_options.verifyPeer ? 1L : 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_options.verifyPeer ? 2L : 0L);
#if LIBCURL_VERSION_NUM >= 0x074D00 // 7.77.0
            if (!m_options.caBundleBlob.empty())
            {
                curl_blob blob {};
                blob.data  = const_cast<char*>(m_options.caBundleBlob.data());
                blob.len   = m_options.caBundleBlob.size();
                blob.flags = CURL_BLOB_COPY;
                curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
            }
            else
#endif
                if (!m_options.caBundle.empty())
                curl_easy_setopt(curl, CURLOPT_CAINFO, m_options.caBundle.c_str());
            if (const auto pins = curlPinnedKeys(m_options); !pins.empty())
                curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, pins.c_str());
            if (!m_options.proxy.empty())
                curl_easy_setopt(curl, CURLOPT_PROXY, m_options.proxy.c_str());

            if (request.method == Method::Head)
                curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            else
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, methodName.c_str());
            if (!request.body.empty())
            {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            }

            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

            const CURLcode code = curl_easy_perform(curl);
            long status         = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            curl_slist_free_all(headers);
            release(curl);

            if (transfer.stoppedByCallback)
            {
                response.status = static_cast<int>(status);
                return response;
            }
            if (code != CURLE_OK)
                return toError(code, request);
            response.status = static_cast<int>(status);
            return response;
        }

        static Error toError(CURLcode code, const Request& request)
        {
            if (code == CURLE_ABORTED_BY_CALLBACK || (request.cancel && request.cancel->load()))
                return makeError(errc::Cancelled, "request cancelled");
            if (code == CURLE_OPERATION_TIMEDOUT)
                return makeError(errc::Timeout, curl_easy_strerror(code));
            return makeError(errc::Network, curl_easy_strerror(code));
        }

        static constexpr std::size_t kMaxPooled = 8;

        CurlOptions m_options;
        std::mutex m_mutex;
        std::vector<CURL*> m_pool;
    };
}

std::shared_ptr<Transport> makeCurlTransport(CurlOptions options) { return std::make_shared<CurlTransport>(std::move(options)); }

}
