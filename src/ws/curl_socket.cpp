#include <curl/curl.h>

#include <supabase/ws.hpp>

#include <array>
#include <thread>

namespace supabase::ws
{

namespace
{
#if LIBCURL_VERSION_NUM >= 0x075600 // 7.86.0, first release with the websocket API
    using Clock = std::chrono::steady_clock;

    constexpr auto kPollInterval = std::chrono::milliseconds(5);

    class CurlSocket final : public Socket
    {
      public:
        explicit CurlSocket(http::CurlOptions options) : m_options(std::move(options)) {}
        ~CurlSocket() override { close(); }

        Result<void> connect(const std::string& url, const http::Headers& headers, std::chrono::milliseconds timeout) override
        {
            close();
            curl_global_init(CURL_GLOBAL_DEFAULT);
            m_easy = curl_easy_init();
            if (m_easy == nullptr)
                return makeError(errc::Network, "curl_easy_init failed");

            curl_slist* list = nullptr;
            for (const auto& [name, value] : headers)
                list = curl_slist_append(list, (name + ": " + value).c_str());

            curl_easy_setopt(m_easy, CURLOPT_URL, url.c_str());
            curl_easy_setopt(m_easy, CURLOPT_CONNECT_ONLY, 2L);
            curl_easy_setopt(m_easy, CURLOPT_HTTPHEADER, list);
            curl_easy_setopt(m_easy, CURLOPT_USERAGENT, m_options.userAgent.c_str());
            curl_easy_setopt(m_easy, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(m_easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout.count()));
            curl_easy_setopt(m_easy, CURLOPT_SSL_VERIFYPEER, m_options.verifyPeer ? 1L : 0L);
            curl_easy_setopt(m_easy, CURLOPT_SSL_VERIFYHOST, m_options.verifyPeer ? 2L : 0L);
#if LIBCURL_VERSION_NUM >= 0x074D00 // 7.77.0
            if (!m_options.caBundleBlob.empty())
            {
                curl_blob blob {};
                blob.data  = const_cast<char*>(m_options.caBundleBlob.data());
                blob.len   = m_options.caBundleBlob.size();
                blob.flags = CURL_BLOB_COPY;
                curl_easy_setopt(m_easy, CURLOPT_CAINFO_BLOB, &blob);
            }
            else
#endif
                if (!m_options.caBundle.empty())
                curl_easy_setopt(m_easy, CURLOPT_CAINFO, m_options.caBundle.c_str());
            if (!m_options.proxy.empty())
                curl_easy_setopt(m_easy, CURLOPT_PROXY, m_options.proxy.c_str());

            const CURLcode code = curl_easy_perform(m_easy);
            curl_slist_free_all(list); // curl does not keep the list after the handshake
            if (code != CURLE_OK)
            {
                const auto errorCode = code == CURLE_OPERATION_TIMEDOUT ? errc::Timeout : errc::Network;
                std::string message  = curl_easy_strerror(code);
                close();
                return makeError(errorCode, std::move(message));
            }
            return {};
        }

        Result<void> send(std::string_view text) override
        {
            if (m_easy == nullptr)
                return makeError(errc::Network, "websocket is not connected");
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            while (!text.empty())
            {
                std::size_t sent    = 0;
                const CURLcode code = curl_ws_send(m_easy, text.data(), text.size(), &sent, 0, CURLWS_TEXT);
                if (code == CURLE_AGAIN)
                {
                    if (Clock::now() >= deadline)
                        return makeError(errc::Timeout, "websocket send timed out");
                    std::this_thread::sleep_for(kPollInterval);
                    continue;
                }
                if (code != CURLE_OK)
                    return makeError(errc::Network, curl_easy_strerror(code));
                text.remove_prefix(sent); // curl resumes a partially sent frame with the remaining bytes
            }
            return {};
        }

        Result<std::optional<std::string>> receive(std::chrono::milliseconds wait) override
        {
            if (m_easy == nullptr)
            {
                return makeError(errc::Network, "websocket is not connected");
            }

            const auto deadline = Clock::now() + wait;
            std::array<char, 16 * 1024> buffer {};

            for (;;)
            {
                std::size_t received      = 0;
                const curl_ws_frame* meta = nullptr;
                const CURLcode code       = curl_ws_recv(m_easy, buffer.data(), buffer.size(), &received, &meta);
                if (code == CURLE_AGAIN)
                {
                    if (Clock::now() >= deadline)
                        return std::optional<std::string> {};
                    std::this_thread::sleep_for(kPollInterval);
                    continue;
                }
                if (code != CURLE_OK)
                    return makeError(errc::Network, curl_easy_strerror(code));
                if (meta == nullptr)
                    continue;
                if ((meta->flags & CURLWS_CLOSE) != 0)
                    return makeError(errc::Network, "websocket closed by peer");
                if ((meta->flags & (CURLWS_PING | CURLWS_PONG)) != 0)
                    continue; // curl answers pings itself
                m_pending.append(buffer.data(), received);
                if (meta->bytesleft == 0 && (meta->flags & CURLWS_CONT) == 0)
                {
                    std::optional<std::string> message(std::move(m_pending));
                    m_pending.clear();
                    return message;
                }
            }
        }

        void close() noexcept override
        {
            if (m_easy == nullptr)
                return;
            std::size_t sent = 0;
            curl_ws_send(m_easy, "", 0, &sent, 0, CURLWS_CLOSE);
            curl_easy_cleanup(m_easy);
            m_easy = nullptr;
            m_pending.clear();
        }

      private:
        http::CurlOptions m_options;
        CURL* m_easy = nullptr;
        std::string m_pending;
    };
#else
    class CurlSocket final : public Socket
    {
      public:
        explicit CurlSocket(http::CurlOptions) {}
        Result<void> connect(const std::string&, const http::Headers&, std::chrono::milliseconds) override
        {
            return makeError(errc::NotImplemented, "websocket support needs libcurl >= 7.86");
        }
        Result<void> send(std::string_view) override { return makeError(errc::Network, "websocket is not connected"); }
        Result<std::optional<std::string>> receive(std::chrono::milliseconds) override
        {
            return makeError(errc::Network, "websocket is not connected");
        }
        void close() noexcept override {}
    };
#endif
}

SocketFactory makeCurlSocketFactory(http::CurlOptions options)
{
    return [options = std::move(options)]() -> std::unique_ptr<Socket> { return std::make_unique<CurlSocket>(options); };
}

}
