#include "pkce.hpp"

#include <chrono>

#include <supabase/auth/auth.hpp>
#include <supabase/postgrest/builder.hpp> // urlEncode

#include <mutex>
#include <utility>
#include <vector>

namespace supabase::auth
{

namespace
{
    constexpr std::int64_t kRefreshMarginSeconds = 30;

    std::int64_t nowSeconds()
    {
        using namespace std::chrono;
        return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    }

    std::string str(const Json& j, const char* key)
    {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::int64_t integer(const Json& j, const char* key)
    {
        const auto it = j.find(key);
        return it != j.end() && it->is_number_integer() ? it->get<std::int64_t>() : 0;
    }

    Json object(const Json& j, const char* key)
    {
        const auto it = j.find(key);
        return it != j.end() && it->is_object() ? *it : Json::object();
    }

    /// GoTrue error bodies use several shapes across versions.
    Error toError(const http::Response& response)
    {
        Error error     = makeError(errc::Http, response.body, response.status);
        const Json body = Json::parse(response.body, nullptr, false);
        if (!body.is_object())
            return error;
        for (const char* key : { "error_code", "code", "error" })
        {
            if (auto value = str(body, key); !value.empty())
            {
                error.code = std::move(value);
                break;
            }
        }
        for (const char* key : { "msg", "message", "error_description" })
        {
            if (auto value = str(body, key); !value.empty())
            {
                error.message = std::move(value);
                break;
            }
        }
        return error;
    }

    void putIfSet(Json& body, const char* key, const std::string& value)
    {
        if (!value.empty())
            body[key] = value;
    }
}

void from_json(const Json& j, User& user)
{
    user.id            = str(j, "id");
    user.aud           = str(j, "aud");
    user.role          = str(j, "role");
    user.email         = str(j, "email");
    user.phone         = str(j, "phone");
    user.created_at    = str(j, "created_at");
    user.app_metadata  = object(j, "app_metadata");
    user.user_metadata = object(j, "user_metadata");
    user.raw           = j;
}

void from_json(const Json& j, Session& session)
{
    session.access_token  = str(j, "access_token");
    session.refresh_token = str(j, "refresh_token");
    if (auto type = str(j, "token_type"); !type.empty())
        session.token_type = std::move(type);
    session.expires_in = integer(j, "expires_in");
    session.expires_at = integer(j, "expires_at");
    if (session.expires_at == 0 && session.expires_in > 0)
        session.expires_at = nowSeconds() + session.expires_in;
    if (const auto it = j.find("user"); it != j.end() && it->is_object())
        session.user = it->get<User>();
}

void to_json(Json& j, const Session& session)
{
    j = Json {
        { "access_token", session.access_token }, { "refresh_token", session.refresh_token },
        { "token_type", session.token_type },     { "expires_in", session.expires_in },
        { "expires_at", session.expires_at },     { "user", session.user.raw.is_null() ? Json::object() : session.user.raw },
    };
}

std::optional<std::string> MemorySessionStorage::load() { return m_value; }
void MemorySessionStorage::save(const std::string& serialized) { m_value = serialized; }
void MemorySessionStorage::clear() { m_value.reset(); }

Subscription::Subscription(std::function<void()> unsubscribe) : m_unsubscribe(std::move(unsubscribe)) {}

Subscription::~Subscription() { unsubscribe(); }

Subscription::Subscription(Subscription&& other) noexcept : m_unsubscribe(std::exchange(other.m_unsubscribe, nullptr)) {}

Subscription& Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other)
    {
        unsubscribe();
        m_unsubscribe = std::exchange(other.m_unsubscribe, nullptr);
    }
    return *this;
}

void Subscription::unsubscribe()
{
    if (auto fn = std::exchange(m_unsubscribe, nullptr))
        fn();
}

struct AuthClient::Impl
{
    std::shared_ptr<Context> ctx;
    std::shared_ptr<SessionStorage> storage;
    bool autoRefresh = true;

    std::mutex mutex;                                      // guards session, loaded, listeners
    std::mutex refreshMutex;                               // serializes network refreshes
    std::optional<std::pair<std::string, Error>> rejected; // last refresh token rejected, guarded by refreshMutex
    std::optional<Session> session;
    bool loaded = false;
    std::map<int, AuthCallback> listeners;
    int nextListener = 0;
    std::string codeVerifier; // PKCE, guarded by mutex

    // --- session state ---------------------------------------------------

    void loadLocked()
    {
        if (loaded)
            return;
        loaded         = true;
        const auto raw = storage->load();
        if (!raw)
            return;
        const Json parsed = Json::parse(*raw, nullptr, false);
        if (parsed.is_object() && !str(parsed, "access_token").empty())
            session = parsed.get<Session>();
    }

    std::optional<Session> snapshot()
    {
        const std::lock_guard lock(mutex);
        loadLocked();
        return session;
    }

    void emit(AuthEvent event, const std::optional<Session>& current)
    {
        std::vector<AuthCallback> callbacks;
        {
            const std::lock_guard lock(mutex);
            for (const auto& entry : listeners)
                callbacks.push_back(entry.second);
        }
        for (const auto& callback : callbacks)
            callback(event, current);
    }

    void store(const Session& next, AuthEvent event)
    {
        {
            const std::lock_guard lock(mutex);
            loaded  = true;
            session = next;
            storage->save(Json(next).dump());
        }
        emit(event, next);
    }

    void clear()
    {
        {
            const std::lock_guard lock(mutex);
            loaded = true;
            session.reset();
            storage->clear();
        }
        emit(AuthEvent::SignedOut, std::nullopt);
    }

    [[nodiscard]] bool needsRefresh(const Session& s) const
    {
        return s.expires_at != 0 && s.expires_at - nowSeconds() < kRefreshMarginSeconds;
    }

    /// `bearer` empty -> anon key.
    Result<Json> call(http::Method method, const std::string& path, const Json* body, const std::string& bearer)
    {
        http::Request request;
        request.method  = method;
        request.url     = ctx->endpoint(path);
        request.timeout = ctx->timeout;
        request.headers.emplace_back("apikey", ctx->anonKey);
        request.headers.emplace_back("Authorization", "Bearer " + (bearer.empty() ? ctx->anonKey : bearer));
        request.headers.insert(request.headers.end(), ctx->headers.begin(), ctx->headers.end());
        request.headers.emplace_back("Accept", "application/json");
        if (body != nullptr)
        {
            request.headers.emplace_back("Content-Type", "application/json");
            request.body = body->dump();
        }

        auto sent = ctx->transport->send(request);
        if (!sent)
            return sent.error();
        if (!sent.value().ok())
            return toError(sent.value());
        if (sent.value().body.empty())
            return Json::object();
        Json parsed = Json::parse(sent.value().body, nullptr, false);
        if (parsed.is_discarded())
            return makeError(errc::Parse, "invalid JSON in auth response", sent.value().status);
        return parsed;
    }

    Result<Session> grant(const std::string& type, const Json& body, AuthEvent event)
    {
        auto reply = call(http::Method::Post, "/auth/v1/token?grant_type=" + type, &body, {});
        if (!reply)
            return reply.error();
        auto next = reply.value().get<Session>();
        if (next.access_token.empty())
            return makeError(errc::Parse, "token response had no access_token");
        store(next, event);
        return next;
    }

    Result<Session> refresh(const std::string& token)
    {
        const std::lock_guard serialize(refreshMutex);
        // Another thread may have refreshed while we waited.
        if (const auto current = snapshot(); current && current->refresh_token != token)
            return *current;
        // A queued caller holding the token that was just rejected must not sign out again.
        if (rejected && rejected->first == token)
            return rejected->second;
        auto result = grant("refresh_token", Json { { "refresh_token", token } }, AuthEvent::TokenRefreshed);
        if (!result && (result.error().status == 400 || result.error().status == 401))
        {
            // Definitive rejection: the refresh token is unusable. Transient errors keep the session.
            rejected = std::make_pair(token, result.error());
            if (const auto current = snapshot(); current && current->refresh_token == token)
                clear();
        }
        return result;
    }

    std::optional<Session> freshSession()
    {
        auto current = snapshot();
        if (!current || !autoRefresh || current->refresh_token.empty() || !needsRefresh(*current))
            return current;
        if (auto refreshed = refresh(current->refresh_token))
            return refreshed.value();
        return snapshot(); // keep the stale session; the server will answer 401 if truly expired
    }

    std::string bearerToken()
    {
        const auto current = freshSession();
        return current ? current->access_token : ctx->anonKey;
    }
};

AuthClient::AuthClient(std::shared_ptr<Context> ctx, std::shared_ptr<SessionStorage> storage, bool autoRefreshToken)
    : m_impl(std::make_shared<Impl>())
{
    m_impl->ctx         = std::move(ctx);
    m_impl->storage     = storage ? std::move(storage) : std::make_shared<MemorySessionStorage>();
    m_impl->autoRefresh = autoRefreshToken;

    // Weak capture: Context outlives neither the client nor creates a reference cycle.
    m_impl->ctx->bearer = [weak = std::weak_ptr<Impl>(m_impl), anon = m_impl->ctx->anonKey]()
    {
        const auto impl = weak.lock();
        return impl ? impl->bearerToken() : anon;
    };
}

namespace
{
    /// Either a full session (has access_token) or just a user (confirmation pending).
    AuthResponse toAuthResponse(const Json& body)
    {
        AuthResponse out;
        if (!str(body, "access_token").empty())
        {
            out.session = body.get<Session>();
            out.user    = out.session->user;
        }
        else if (!str(body, "id").empty())
        {
            out.user = body.get<User>();
        }
        return out;
    }

    std::string withRedirect(std::string path, std::string_view redirectTo)
    {
        if (!redirectTo.empty())
            path += "?redirect_to=" + postgrest::urlEncode(redirectTo);
        return path;
    }
}

Result<AuthResponse> AuthClient::signUp(const SignUpCredentials& credentials)
{
    if (credentials.email.empty() == credentials.phone.empty())
        return makeError(errc::InvalidArgument, "provide exactly one of email or phone");
    Json body = Json::object();
    putIfSet(body, "email", credentials.email);
    putIfSet(body, "phone", credentials.phone);
    body["password"] = credentials.password;
    if (!credentials.data.is_null())
        body["data"] = credentials.data;

    auto reply = m_impl->call(http::Method::Post, withRedirect("/auth/v1/signup", credentials.redirectTo), &body, {});
    if (!reply)
        return reply.error();
    auto response = toAuthResponse(reply.value());
    if (response.session)
        m_impl->store(*response.session, AuthEvent::SignedIn);
    return response;
}

Result<AuthResponse> AuthClient::signInWithPassword(const PasswordCredentials& credentials)
{
    if (credentials.email.empty() == credentials.phone.empty())
        return makeError(errc::InvalidArgument, "provide exactly one of email or phone");
    Json body = Json::object();
    putIfSet(body, "email", credentials.email);
    putIfSet(body, "phone", credentials.phone);
    body["password"] = credentials.password;

    auto session = m_impl->grant("password", body, AuthEvent::SignedIn);
    if (!session)
        return session.error();
    return AuthResponse { session.value().user, session.value() };
}

Result<AuthResponse> AuthClient::signInWithIdToken(const IdTokenCredentials& credentials)
{
    Json body = { { "provider", credentials.provider }, { "id_token", credentials.token } };
    putIfSet(body, "nonce", credentials.nonce);
    putIfSet(body, "access_token", credentials.accessToken);

    auto session = m_impl->grant("id_token", body, AuthEvent::SignedIn);
    if (!session)
        return session.error();
    return AuthResponse { session.value().user, session.value() };
}

Result<void> AuthClient::signInWithOtp(const OtpCredentials& credentials)
{
    if (credentials.email.empty() == credentials.phone.empty())
        return makeError(errc::InvalidArgument, "provide exactly one of email or phone");
    Json body = { { "create_user", credentials.shouldCreateUser } };
    putIfSet(body, "email", credentials.email);
    putIfSet(body, "phone", credentials.phone);
    if (!credentials.data.is_null())
        body["data"] = credentials.data;

    auto reply = m_impl->call(http::Method::Post, withRedirect("/auth/v1/otp", credentials.redirectTo), &body, {});
    if (!reply)
        return reply.error();
    return {};
}

Result<AuthResponse> AuthClient::verifyOtp(const VerifyOtpParams& params)
{
    if (params.type.empty())
        return makeError(errc::InvalidArgument, "type is required");
    Json body = { { "type", params.type } };
    if (!params.tokenHash.empty())
    {
        body["token_hash"] = params.tokenHash;
    }
    else
    {
        if (params.token.empty() || params.email.empty() == params.phone.empty())
            return makeError(errc::InvalidArgument, "provide tokenHash, or token with exactly one of email or phone");
        body["token"] = params.token;
        putIfSet(body, "email", params.email);
        putIfSet(body, "phone", params.phone);
    }

    auto reply = m_impl->call(http::Method::Post, withRedirect("/auth/v1/verify", params.redirectTo), &body, {});
    if (!reply)
        return reply.error();
    auto response = toAuthResponse(reply.value());
    if (response.session)
        m_impl->store(*response.session, AuthEvent::SignedIn);
    return response;
}

std::string AuthClient::getOAuthSignInUrl(std::string_view provider, const OAuthOptions& options) const
{
    std::string url = m_impl->ctx->endpoint("/auth/v1/authorize?provider=") + postgrest::urlEncode(provider);
    if (!options.redirectTo.empty())
        url += "&redirect_to=" + postgrest::urlEncode(options.redirectTo);
    if (!options.scopes.empty())
        url += "&scopes=" + postgrest::urlEncode(options.scopes);
    for (const auto& [key, value] : options.queryParams)
        url += "&" + postgrest::urlEncode(key) + "=" + postgrest::urlEncode(value);
    if (options.pkce)
    {
        auto verifier = pkce::generateVerifier();
        url += "&code_challenge=" + pkce::challengeS256(verifier) + "&code_challenge_method=s256";
        const std::lock_guard lock(m_impl->mutex);
        m_impl->codeVerifier = std::move(verifier);
    }
    return url;
}

Result<Session> AuthClient::exchangeCodeForSession(std::string_view authCode)
{
    std::string verifier;
    {
        const std::lock_guard lock(m_impl->mutex);
        verifier = std::exchange(m_impl->codeVerifier, std::string());
    }
    if (verifier.empty())
        return makeError(errc::InvalidArgument, "no PKCE code verifier; call getOAuthSignInUrl with pkce first");
    if (authCode.empty())
        return makeError(errc::InvalidArgument, "auth code is required");
    return m_impl->grant("pkce", Json { { "auth_code", std::string(authCode) }, { "code_verifier", verifier } }, AuthEvent::SignedIn);
}

Result<Session> AuthClient::refreshSession(std::string_view refreshToken)
{
    std::string token(refreshToken);
    if (token.empty())
    {
        const auto current = m_impl->snapshot();
        if (!current || current->refresh_token.empty())
            return makeError(errc::NoSession, "no session to refresh");
        token = current->refresh_token;
    }
    return m_impl->refresh(token);
}

Result<Session> AuthClient::setSession(Session session)
{
    if (session.access_token.empty())
        return makeError(errc::InvalidArgument, "access_token is required");
    m_impl->store(session, AuthEvent::SignedIn);
    return session;
}

Result<void> AuthClient::signOut(SignOutScope scope)
{
    const auto current = m_impl->snapshot();
    Result<void> outcome;
    if (current)
    {
        auto reply = m_impl->call(
            http::Method::Post,
            scope == SignOutScope::Local ? "/auth/v1/logout?scope=local" : "/auth/v1/logout",
            nullptr,
            current->access_token
        );
        // An already-invalid token means we are effectively signed out; only surface other failures.
        if (!reply && reply.error().status != 401 && reply.error().status != 403 && reply.error().status != 404)
            outcome = reply.error();
    }
    m_impl->clear();
    return outcome;
}

Result<User> AuthClient::getUser()
{
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    auto reply = m_impl->call(http::Method::Get, "/auth/v1/user", nullptr, current->access_token);
    if (!reply)
        return reply.error();
    return reply.value().get<User>();
}

Result<User> AuthClient::updateUser(const UserAttributes& attributes)
{
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    Json body = Json::object();
    if (attributes.email)
        body["email"] = *attributes.email;
    if (attributes.phone)
        body["phone"] = *attributes.phone;
    if (attributes.password)
        body["password"] = *attributes.password;
    if (!attributes.data.is_null())
        body["data"] = attributes.data;

    auto reply = m_impl->call(http::Method::Put, "/auth/v1/user", &body, current->access_token);
    if (!reply)
        return reply.error();
    auto user = reply.value().get<User>();

    Session updated = *current;
    updated.user    = user;
    m_impl->store(updated, AuthEvent::UserUpdated);
    return user;
}

Result<void> AuthClient::resetPasswordForEmail(std::string_view email, std::string_view redirectTo)
{
    if (email.empty())
        return makeError(errc::InvalidArgument, "email is required");
    const Json body = { { "email", std::string(email) } };
    auto reply      = m_impl->call(http::Method::Post, withRedirect("/auth/v1/recover", redirectTo), &body, {});
    if (!reply)
        return reply.error();
    return {};
}

std::optional<Session> AuthClient::getSession() { return m_impl->freshSession(); }

Subscription AuthClient::onAuthStateChange(AuthCallback callback)
{
    int id = 0;
    {
        const std::lock_guard lock(m_impl->mutex);
        id = m_impl->nextListener++;
        m_impl->listeners.emplace(id, callback);
    }
    callback(AuthEvent::InitialSession, m_impl->snapshot());

    return Subscription(
        [weak = std::weak_ptr<Impl>(m_impl), id]
        {
            if (const auto impl = weak.lock())
            {
                const std::lock_guard lock(impl->mutex);
                impl->listeners.erase(id);
            }
        }
    );
}

void from_json(const Json& j, Factor& factor)
{
    factor.id            = str(j, "id");
    factor.friendly_name = str(j, "friendly_name");
    factor.factor_type   = str(j, "factor_type");
    if (factor.factor_type.empty())
        factor.factor_type = str(j, "type");
    factor.status = str(j, "status");
}

namespace
{
    std::string base64UrlDecode(std::string_view text)
    {
        std::string out;
        std::uint32_t buffer = 0;
        int bits             = 0;
        for (const char c : text)
        {
            int v = -1;
            if (c >= 'A' && c <= 'Z')
                v = c - 'A';
            else if (c >= 'a' && c <= 'z')
                v = c - 'a' + 26;
            else if (c >= '0' && c <= '9')
                v = c - '0' + 52;
            else if (c == '-' || c == '+')
                v = 62;
            else if (c == '_' || c == '/')
                v = 63;
            else
                continue; // padding or junk
            buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            }
        }
        return out;
    }

    Json jwtPayload(const std::string& jwt)
    {
        const auto first = jwt.find('.');
        if (first == std::string::npos)
            return Json::object();
        const auto second = jwt.find('.', first + 1);
        if (second == std::string::npos)
            return Json::object();
        const Json payload = Json::parse(base64UrlDecode(std::string_view(jwt).substr(first + 1, second - first - 1)), nullptr, false);
        return payload.is_object() ? payload : Json::object();
    }

    std::string idPath(std::string_view prefix, std::string_view id, std::string_view suffix = {})
    {
        return std::string(prefix) + postgrest::urlEncode(id) + std::string(suffix);
    }

    Json adminAttributes(const AdminCreateUserAttributes& a)
    {
        Json body = Json::object();
        putIfSet(body, "email", a.email);
        putIfSet(body, "phone", a.phone);
        putIfSet(body, "password", a.password);
        if (a.emailConfirm)
            body["email_confirm"] = true;
        if (a.phoneConfirm)
            body["phone_confirm"] = true;
        if (!a.userMetadata.empty())
            body["user_metadata"] = a.userMetadata;
        if (!a.appMetadata.empty())
            body["app_metadata"] = a.appMetadata;
        return body;
    }
}

AuthClient::Mfa AuthClient::mfa() const
{
    Mfa out;
    out.m_impl = m_impl;
    return out;
}

AuthClient::Admin AuthClient::admin(std::string serviceRoleKey) const
{
    Admin out;
    out.m_impl       = m_impl;
    out.m_serviceKey = std::move(serviceRoleKey);
    return out;
}

Result<MfaEnrollResponse> AuthClient::Mfa::enroll(const MfaEnrollParams& params)
{
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    Json body = { { "factor_type", params.factorType } };
    putIfSet(body, "friendly_name", params.friendlyName);
    putIfSet(body, "issuer", params.issuer);
    putIfSet(body, "phone", params.phone);

    auto reply = m_impl->call(http::Method::Post, "/auth/v1/factors", &body, current->access_token);
    if (!reply)
        return reply.error();
    MfaEnrollResponse out;
    out.factor = reply.value().get<Factor>();
    if (out.factor.friendly_name.empty())
        out.factor.friendly_name = params.friendlyName;
    const Json totp = object(reply.value(), "totp");
    out.qr_code     = str(totp, "qr_code");
    out.secret      = str(totp, "secret");
    out.uri         = str(totp, "uri");
    return out;
}

Result<MfaChallenge> AuthClient::Mfa::challenge(std::string_view factorId, std::string_view channel)
{
    if (factorId.empty())
        return makeError(errc::InvalidArgument, "factor id is required");
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    Json body = Json::object();
    if (!channel.empty())
        body["channel"] = std::string(channel);

    auto reply = m_impl->call(http::Method::Post, idPath("/auth/v1/factors/", factorId, "/challenge"), &body, current->access_token);
    if (!reply)
        return reply.error();
    return MfaChallenge { str(reply.value(), "id"), integer(reply.value(), "expires_at") };
}

Result<Session> AuthClient::Mfa::verify(std::string_view factorId, std::string_view challengeId, std::string_view code)
{
    if (factorId.empty() || challengeId.empty() || code.empty())
        return makeError(errc::InvalidArgument, "factor id, challenge id and code are required");
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    const Json body = { { "challenge_id", std::string(challengeId) }, { "code", std::string(code) } };

    auto reply = m_impl->call(http::Method::Post, idPath("/auth/v1/factors/", factorId, "/verify"), &body, current->access_token);
    if (!reply)
        return reply.error();
    auto next = reply.value().get<Session>();
    if (next.access_token.empty())
        return makeError(errc::Parse, "verify response had no access_token");
    if (next.user.id.empty())
        next.user = current->user;
    m_impl->store(next, AuthEvent::MfaChallengeVerified);
    return next;
}

Result<Session> AuthClient::Mfa::challengeAndVerify(std::string_view factorId, std::string_view code)
{
    auto issued = challenge(factorId);
    if (!issued)
        return issued.error();
    return verify(factorId, issued.value().id, code);
}

Result<std::string> AuthClient::Mfa::unenroll(std::string_view factorId)
{
    if (factorId.empty())
        return makeError(errc::InvalidArgument, "factor id is required");
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    auto reply = m_impl->call(http::Method::Delete, idPath("/auth/v1/factors/", factorId), nullptr, current->access_token);
    if (!reply)
        return reply.error();
    return str(reply.value(), "id");
}

Result<MfaFactors> AuthClient::Mfa::listFactors()
{
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    auto reply = m_impl->call(http::Method::Get, "/auth/v1/user", nullptr, current->access_token);
    if (!reply)
        return reply.error();
    MfaFactors out;
    const auto it = reply.value().find("factors");
    if (it == reply.value().end() || !it->is_array())
        return out;
    for (const auto& item : *it)
    {
        auto factor = item.get<Factor>();
        out.all.push_back(factor);
        if (factor.status != "verified")
            continue;
        if (factor.factor_type == "totp")
            out.totp.push_back(factor);
        else if (factor.factor_type == "phone")
            out.phone.push_back(factor);
    }
    return out;
}

Result<AuthenticatorAssuranceLevel> AuthClient::Mfa::getAuthenticatorAssuranceLevel()
{
    const auto current = m_impl->freshSession();
    if (!current)
        return makeError(errc::NoSession, "no active session", 401);
    const Json payload = jwtPayload(current->access_token);

    AuthenticatorAssuranceLevel out;
    out.currentLevel = str(payload, "aal");
    if (out.currentLevel.empty())
        out.currentLevel = "aal1";
    out.nextLevel = out.currentLevel;
    if (const auto amr = payload.find("amr"); amr != payload.end() && amr->is_array())
    {
        for (const auto& entry : *amr)
        {
            // Entries are {"method": "...", "timestamp": ...}; older tokens use plain strings.
            if (entry.is_object())
                out.currentAuthenticationMethods.push_back(str(entry, "method"));
            else if (entry.is_string())
                out.currentAuthenticationMethods.push_back(entry.get<std::string>());
        }
    }
    if (const auto factors = current->user.raw.find("factors"); factors != current->user.raw.end() && factors->is_array())
    {
        for (const auto& factor : *factors)
        {
            if (str(factor, "status") == "verified")
            {
                out.nextLevel = "aal2";
                break;
            }
        }
    }
    return out;
}

Result<AdminListUsersResponse> AuthClient::Admin::listUsers(const AdminListUsersParams& params)
{
    const std::string path = "/auth/v1/admin/users?page=" + std::to_string(params.page) + "&per_page=" + std::to_string(params.perPage);
    auto reply             = m_impl->call(http::Method::Get, path, nullptr, m_serviceKey);
    if (!reply)
        return reply.error();
    AdminListUsersResponse out;
    if (const auto it = reply.value().find("users"); it != reply.value().end() && it->is_array())
    {
        for (const auto& item : *it)
            out.users.push_back(item.get<User>());
    }
    out.total = static_cast<int>(integer(reply.value(), "total"));
    if (out.total == 0)
        out.total = static_cast<int>(out.users.size());
    return out;
}

Result<User> AuthClient::Admin::getUserById(std::string_view id)
{
    if (id.empty())
        return makeError(errc::InvalidArgument, "user id is required");
    auto reply = m_impl->call(http::Method::Get, idPath("/auth/v1/admin/users/", id), nullptr, m_serviceKey);
    if (!reply)
        return reply.error();
    return reply.value().get<User>();
}

Result<User> AuthClient::Admin::createUser(const AdminCreateUserAttributes& attributes)
{
    const Json body = adminAttributes(attributes);
    auto reply      = m_impl->call(http::Method::Post, "/auth/v1/admin/users", &body, m_serviceKey);
    if (!reply)
        return reply.error();
    return reply.value().get<User>();
}

Result<User> AuthClient::Admin::updateUserById(std::string_view id, const AdminCreateUserAttributes& attributes)
{
    if (id.empty())
        return makeError(errc::InvalidArgument, "user id is required");
    const Json body = adminAttributes(attributes);
    auto reply      = m_impl->call(http::Method::Put, idPath("/auth/v1/admin/users/", id), &body, m_serviceKey);
    if (!reply)
        return reply.error();
    return reply.value().get<User>();
}

Result<void> AuthClient::Admin::deleteUser(std::string_view id, bool shouldSoftDelete)
{
    if (id.empty())
        return makeError(errc::InvalidArgument, "user id is required");
    const Json body = { { "should_soft_delete", shouldSoftDelete } };
    auto reply      = m_impl->call(http::Method::Delete, idPath("/auth/v1/admin/users/", id), &body, m_serviceKey);
    if (!reply)
        return reply.error();
    return {};
}

Result<User> AuthClient::Admin::inviteUserByEmail(std::string_view email, std::string_view redirectTo, const Json& data)
{
    if (email.empty())
        return makeError(errc::InvalidArgument, "email is required");
    Json body = { { "email", std::string(email) } };
    if (data.is_object())
        body["data"] = data;
    auto reply = m_impl->call(http::Method::Post, withRedirect("/auth/v1/invite", redirectTo), &body, m_serviceKey);
    if (!reply)
        return reply.error();
    return reply.value().get<User>();
}

Result<AdminGenerateLinkResponse> AuthClient::Admin::generateLink(const AdminGenerateLinkParams& params)
{
    if (params.type.empty() || params.email.empty())
        return makeError(errc::InvalidArgument, "type and email are required");
    Json body = { { "type", params.type }, { "email", params.email } };
    putIfSet(body, "password", params.password);
    putIfSet(body, "new_email", params.newEmail);
    putIfSet(body, "redirect_to", params.redirectTo);
    if (params.data.is_object())
        body["data"] = params.data;

    auto reply = m_impl->call(http::Method::Post, "/auth/v1/admin/generate_link", &body, m_serviceKey);
    if (!reply)
        return reply.error();
    AdminGenerateLinkResponse out;
    out.action_link       = str(reply.value(), "action_link");
    out.email_otp         = str(reply.value(), "email_otp");
    out.hashed_token      = str(reply.value(), "hashed_token");
    out.verification_type = str(reply.value(), "verification_type");
    out.user              = reply.value().get<User>();
    return out;
}

Result<void> AuthClient::Admin::signOut(std::string_view jwt)
{
    if (jwt.empty())
        return makeError(errc::InvalidArgument, "jwt is required");
    auto reply = m_impl->call(http::Method::Post, "/auth/v1/logout?scope=global", nullptr, std::string(jwt));
    if (!reply)
        return reply.error();
    return {};
}

}
