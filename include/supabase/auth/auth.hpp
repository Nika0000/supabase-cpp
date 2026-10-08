#pragma once

#include <cstdint>

#include <supabase/context.hpp>
#include <supabase/postgrest/builder.hpp>
#include <supabase/result.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace supabase::auth
{

struct User
{
    std::string id;
    std::string aud;
    std::string role;
    std::string email;
    std::string phone;
    std::string created_at;
    Json app_metadata  = Json::object();
    Json user_metadata = Json::object();
    Json raw; ///< the full server object
};

struct Session
{
    std::string access_token;
    std::string refresh_token;
    std::string token_type  = "bearer";
    std::int64_t expires_in = 0;
    std::int64_t expires_at = 0; ///< unix seconds
    User user;
};

void from_json(const Json& j, User& user);
void from_json(const Json& j, Session& session);
void to_json(Json& j, const Session& session);

/// Sign-up/sign-in response. `session` is empty when email confirmation is pending.
struct AuthResponse
{
    std::optional<User> user;
    std::optional<Session> session;
};

struct SignUpCredentials
{
    std::string email;
    std::string phone;
    std::string password;
    Json data; ///< stored as user_metadata
    std::string redirectTo;
};

struct PasswordCredentials
{
    std::string email;
    std::string phone;
    std::string password;
};

struct OtpCredentials
{
    std::string email;
    std::string phone;
    bool shouldCreateUser = true;
    std::string redirectTo;
    Json data;
};

struct IdTokenCredentials
{
    std::string provider; ///< "google", "apple", ...
    std::string token;
    std::string nonce;
    std::string accessToken;
};

struct OAuthOptions
{
    std::string redirectTo;
    std::string scopes;
    std::map<std::string, std::string> queryParams;
    bool pkce = false; ///< use the PKCE flow; finish with `exchangeCodeForSession`
};

struct UserAttributes
{
    std::optional<std::string> email;
    std::optional<std::string> phone;
    std::optional<std::string> password;
    Json data;
};

/// MFA factor as returned by GoTrue.
struct Factor
{
    std::string id;
    std::string friendly_name;
    std::string factor_type; ///< "totp" or "phone"
    std::string status;      ///< "verified" or "unverified"
};

void from_json(const Json& j, Factor& factor);

struct MfaEnrollParams
{
    std::string factorType = "totp";
    std::string friendlyName;
    std::string issuer;
    std::string phone; ///< phone factors only
};

struct MfaEnrollResponse
{
    Factor factor;
    std::string qr_code; ///< totp: SVG data URI
    std::string secret;  ///< totp: shared secret
    std::string uri;     ///< totp: otpauth:// URI
};

struct MfaChallenge
{
    std::string id;
    std::int64_t expires_at = 0;
};

struct MfaFactors
{
    std::vector<Factor> all;
    std::vector<Factor> totp;  ///< verified only
    std::vector<Factor> phone; ///< verified only
};

/// "aal1" / "aal2" as in supabase-js `getAuthenticatorAssuranceLevel`.
struct AuthenticatorAssuranceLevel
{
    std::string currentLevel;
    std::string nextLevel;
    std::vector<std::string> currentAuthenticationMethods; ///< JWT `amr` methods
};

struct AdminCreateUserAttributes
{
    std::string email;
    std::string phone;
    std::string password;
    bool emailConfirm = false;
    bool phoneConfirm = false;
    Json userMetadata = Json::object();
    Json appMetadata  = Json::object();
};

struct AdminListUsersParams
{
    int page    = 1;
    int perPage = 50;
};

struct AdminListUsersResponse
{
    std::vector<User> users;
    int total = 0; ///< body `total` when the server sends it, else users.size()
};

/// supabase-js `generateLink` types: signup, invite, magiclink, recovery, email_change_current, email_change_new.
struct AdminGenerateLinkParams
{
    std::string type;
    std::string email;
    std::string password;
    std::string newEmail;
    std::string redirectTo;
    Json data;
};

struct AdminGenerateLinkResponse
{
    std::string action_link;
    std::string email_otp;
    std::string hashed_token;
    std::string verification_type;
    User user;
};

enum class AuthEvent
{
    InitialSession,
    SignedIn,
    SignedOut,
    TokenRefreshed,
    UserUpdated,
    MfaChallengeVerified
};

using AuthCallback = std::function<void(AuthEvent, const std::optional<Session>&)>;

/// Persists the serialized session. Implementations may be called from any thread.
class SessionStorage
{
  public:
    virtual ~SessionStorage()                        = default;
    virtual std::optional<std::string> load()        = 0;
    virtual void save(const std::string& serialized) = 0;
    virtual void clear()                             = 0;
};

class MemorySessionStorage final : public SessionStorage
{
  public:
    std::optional<std::string> load() override;
    void save(const std::string& serialized) override;
    void clear() override;

  private:
    std::optional<std::string> value_;
};

/// RAII handle returned by `onAuthStateChange`; unsubscribes on destruction.
class Subscription
{
  public:
    Subscription() = default;
    Subscription(std::function<void()> unsubscribe);
    ~Subscription();
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    void unsubscribe();

  private:
    std::function<void()> unsubscribe_;
};

/// GoTrue client. Mirrors supabase-js `auth`. Thread-safe.
class AuthClient
{
  private:
    struct Impl;

  public:
    AuthClient(std::shared_ptr<Context> ctx, std::shared_ptr<SessionStorage> storage, bool autoRefreshToken);

    Result<AuthResponse> signUp(const SignUpCredentials& credentials);
    Result<AuthResponse> signInWithPassword(const PasswordCredentials& credentials);
    Result<AuthResponse> signInWithIdToken(const IdTokenCredentials& credentials);
    Result<void> signInWithOtp(const OtpCredentials& credentials);
    /// Returns the provider authorize URL to open in a browser; the caller handles the redirect.
    /// With `options.pkce` the code verifier is kept in memory until `exchangeCodeForSession`.
    [[nodiscard]] std::string getOAuthSignInUrl(std::string_view provider, const OAuthOptions& options = {}) const;

    /// PKCE: swaps the `code` from the redirect for a session, using the verifier saved by `getOAuthSignInUrl`.
    Result<Session> exchangeCodeForSession(std::string_view authCode);

    Result<Session> refreshSession(std::string_view refreshToken = {});
    Result<Session> setSession(Session session);
    Result<void> signOut();
    Result<User> getUser();
    Result<User> updateUser(const UserAttributes& attributes);
    Result<void> resetPasswordForEmail(std::string_view email, std::string_view redirectTo = {});

    /// Current session, refreshed first when it is about to expire (if auto-refresh is on).
    [[nodiscard]] std::optional<Session> getSession();

    /// Fires `InitialSession` immediately, then every change, until the handle is destroyed.
    [[nodiscard]] Subscription onAuthStateChange(AuthCallback callback);

    /// MFA enrollment and verification. Requires a signed-in session.
    class Mfa
    {
      public:
        Result<MfaEnrollResponse> enroll(const MfaEnrollParams& params = {});
        Result<MfaChallenge> challenge(std::string_view factorId, std::string_view channel = {});
        /// Upgrades the stored session to aal2 on success.
        Result<Session> verify(std::string_view factorId, std::string_view challengeId, std::string_view code);
        Result<Session> challengeAndVerify(std::string_view factorId, std::string_view code);
        Result<std::string> unenroll(std::string_view factorId);
        Result<MfaFactors> listFactors();
        Result<AuthenticatorAssuranceLevel> getAuthenticatorAssuranceLevel();

      private:
        friend class AuthClient;
        std::shared_ptr<Impl> impl_;
    };

    /// GoTrue admin API. Needs the service-role key; never ship that key in a client app.
    class Admin
    {
      public:
        Result<AdminListUsersResponse> listUsers(const AdminListUsersParams& params = {});
        Result<User> getUserById(std::string_view id);
        Result<User> createUser(const AdminCreateUserAttributes& attributes);
        Result<User> updateUserById(std::string_view id, const AdminCreateUserAttributes& attributes);
        Result<void> deleteUser(std::string_view id, bool shouldSoftDelete = false);
        Result<User> inviteUserByEmail(std::string_view email, std::string_view redirectTo = {}, const Json& data = nullptr);
        Result<AdminGenerateLinkResponse> generateLink(const AdminGenerateLinkParams& params);
        Result<void> signOut(std::string_view jwt);

      private:
        friend class AuthClient;
        std::shared_ptr<Impl> impl_;
        std::string serviceKey_;
    };

    [[nodiscard]] Mfa mfa() const;
    [[nodiscard]] Admin admin(std::string serviceRoleKey) const;

  private:
    std::shared_ptr<Impl> impl_;
};

}
