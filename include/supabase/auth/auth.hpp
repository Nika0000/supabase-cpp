#pragma once

/**
 * @file supabase/auth/auth.hpp
 * @brief Supabase Auth types, session storage, and synchronous GoTrue operations.
 */

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

/// @brief Identity and metadata returned by the authentication server.
/// Missing string fields are empty. User-editable metadata must not be used as
/// trusted authorization data; enforce permissions on the server.
struct User
{
    std::string id;                      ///< Unique user identifier.
    std::string aud;                     ///< Authentication audience.
    std::string role;                    ///< Role reported by the authentication server.
    std::string email;                   ///< Email address, empty when unavailable.
    std::string phone;                   ///< Phone number, empty when unavailable.
    std::string created_at;              ///< Account creation timestamp as returned by the server.
    Json app_metadata  = Json::object(); ///< Application metadata managed by the server or administrators.
    Json user_metadata = Json::object(); ///< Metadata that the user can update.
    Json raw;                            ///< Complete server object, including fields not modeled here.
};

/// @brief Credentials and user data for an authenticated session.
/// Access and refresh tokens are secrets. Protect persisted sessions and do not
/// include tokens in logs. A local Session does not establish server validity.
struct Session
{
    std::string access_token;           ///< Bearer credential used for authenticated requests.
    std::string refresh_token;          ///< Credential used to obtain a replacement session.
    std::string token_type  = "bearer"; ///< Token type reported by the server; defaults to "bearer".
    std::int64_t expires_in = 0;        ///< Token lifetime in seconds from the response; not a live countdown.
    std::int64_t expires_at = 0;        ///< Expiry as Unix seconds; zero when unspecified.
    User user;                          ///< User associated with this session.
};

/// @brief Deserialize a user and retain the complete input in User::raw.
void from_json(const Json& j, User& user);
/// @brief Deserialize a session, deriving expires_at from expires_in when needed.
void from_json(const Json& j, Session& session);
/// @brief Serialize session credentials, expiry, and the user's raw server object.
void to_json(Json& j, const Session& session);

/// @brief Result data from registration, sign-in, or OTP verification.
/// Success does not always include a session: email confirmation may be pending.
/// Check the optional fields before using them.
struct AuthResponse
{
    std::optional<User> user;       ///< User data, when included in the response.
    std::optional<Session> session; ///< Authenticated session, when issued by the server.
};

/// @brief Registration credentials; provide exactly one of email or phone.
struct SignUpCredentials
{
    std::string email;      ///< Email address for email registration; leave phone empty.
    std::string phone;      ///< Phone number for phone registration; leave email empty.
    std::string password;   ///< Password subject to the project's server-side policy.
    Json data;              ///< Initial user metadata; null omits it from the request.
    std::string redirectTo; ///< Optional confirmation redirect URL allowed by the project.
};

/// @brief Password sign-in credentials; provide exactly one of email or phone.
struct PasswordCredentials
{
    std::string email;    ///< Email address; leave phone empty when supplied.
    std::string phone;    ///< Phone number; leave email empty when supplied.
    std::string password; ///< Account password.
};

/// @brief Request an email link or phone OTP; provide exactly one contact field.
struct OtpCredentials
{
    std::string email;            ///< Email destination; leave phone empty when supplied.
    std::string phone;            ///< Phone destination; leave email empty when supplied.
    bool shouldCreateUser = true; ///< Whether the server may register a new user for this request.
    std::string redirectTo;       ///< Optional redirect URL for the email flow.
    Json data;                    ///< User metadata sent with the request; null omits it.
};

/// @brief Verify either a token hash or a code delivered to an email or phone.
/// A nonempty tokenHash takes precedence over token, email, and phone.
struct VerifyOtpParams
{
    std::string email;     ///< Email associated with token; mutually exclusive with phone.
    std::string phone;     ///< Phone associated with token; mutually exclusive with email.
    std::string token;     ///< Received code; required when tokenHash is empty.
    std::string tokenHash; ///< Hashed token from an authentication link; replaces the contact/code form.
    /// "sms", "phone_change", "signup", "invite", "magiclink", "recovery", "email_change" or "email".
    std::string type;
    std::string redirectTo; ///< Optional redirect URL forwarded to the verification endpoint.
};

/// @brief Credentials for exchanging a third-party identity token for a session.
struct IdTokenCredentials
{
    std::string provider;    ///< Provider identifier, such as "google" or "apple".
    std::string token;       ///< Identity token obtained from the provider.
    std::string nonce;       ///< Optional nonce expected by the identity-token flow.
    std::string accessToken; ///< Optional provider access token; not the Supabase session token.
};

/// @brief Options used to construct a browser OAuth authorization URL.
struct OAuthOptions
{
    std::string redirectTo;                         ///< Application callback URL allowed by the project.
    std::string scopes;                             ///< Provider scopes in the provider's expected format.
    std::map<std::string, std::string> queryParams; ///< Additional authorization parameters; URL-encoded by the client.
    bool pkce = false;                              ///< Save a verifier for exchangeCodeForSession() on this auth instance.
};

/// @brief Editable authentication account fields.
/// Unset optional fields are omitted. A supplied empty string is sent to the
/// server. Non-null data is forwarded as user metadata.
struct UserAttributes
{
    std::optional<std::string> email;    ///< New email address; confirmation follows the project's policy.
    std::optional<std::string> phone;    ///< New phone number; confirmation follows the project's policy.
    std::optional<std::string> password; ///< New account password.
    Json data;                           ///< User metadata; null leaves metadata out of the request.
};

/// @brief Multi-factor authentication factor returned by GoTrue.
struct Factor
{
    std::string id;            ///< Factor identifier used for challenge, verification, and removal.
    std::string friendly_name; ///< Display name chosen during enrollment.
    std::string factor_type;   ///< Factor type, such as "totp" or "phone".
    std::string status;        ///< Enrollment status, such as "verified" or "unverified".
};

/// @brief Deserialize a factor, accepting either factor_type or the legacy type field.
void from_json(const Json& j, Factor& factor);

/// @brief Options for enrolling a new MFA factor.
struct MfaEnrollParams
{
    std::string factorType = "totp"; ///< Requested factor type; defaults to TOTP.
    std::string friendlyName;        ///< Optional display name.
    std::string issuer;              ///< Optional issuer label for the authenticator setup.
    std::string phone;               ///< Phone number for phone factors; omitted when empty.
};

/// @brief Enrolled factor and setup material for a TOTP authenticator.
/// TOTP fields are empty when absent from the response. Treat secret and uri as
/// credentials and display them only as part of the enrollment flow.
struct MfaEnrollResponse
{
    Factor factor;       ///< Newly enrolled factor; complete a challenge to verify it.
    std::string qr_code; ///< TOTP QR code as an SVG data URI supplied by the server.
    std::string secret;  ///< TOTP shared secret for manual authenticator setup.
    std::string uri;     ///< TOTP otpauth:// setup URI.
};

/// @brief Challenge that must be completed with a factor's verification code.
struct MfaChallenge
{
    std::string id;              ///< Challenge identifier passed to AuthClient::Mfa::verify().
    std::int64_t expires_at = 0; ///< Challenge expiry as Unix seconds, when supplied.
};

/// @brief All enrolled factors and filtered lists of verified factors.
struct MfaFactors
{
    std::vector<Factor> all;   ///< Every factor included in the server response.
    std::vector<Factor> totp;  ///< Verified TOTP factors.
    std::vector<Factor> phone; ///< Verified phone factors.
};

/// @brief Current and available authentication assurance levels, typically "aal1" or "aal2".
/// These values are derived from local session data, not a server authorization check.
struct AuthenticatorAssuranceLevel
{
    std::string currentLevel; ///< Session JWT's aal claim, defaulting to "aal1" when absent.
    std::string nextLevel;    ///< "aal2" when local user data contains a verified factor; otherwise currentLevel.
    std::vector<std::string> currentAuthenticationMethods; ///< Authentication methods parsed from the JWT's amr claim.
};

/// @brief Account attributes accepted by admin creation and update operations.
/// Only nonempty strings and metadata, and confirmation flags set to true, are
/// sent. Defaults leave those fields out of an update request.
struct AdminCreateUserAttributes
{
    std::string email;                  ///< Account email address.
    std::string phone;                  ///< Account phone number.
    std::string password;               ///< Account password.
    bool emailConfirm = false;          ///< Mark the email confirmed when true; false is omitted.
    bool phoneConfirm = false;          ///< Mark the phone confirmed when true; false is omitted.
    Json userMetadata = Json::object(); ///< User-editable metadata to send when nonempty.
    Json appMetadata  = Json::object(); ///< Administrative application metadata to send when nonempty.
};

/// @brief Pagination parameters for an administrative user listing.
struct AdminListUsersParams
{
    int page    = 1;  ///< One-based page number, forwarded to the server.
    int perPage = 50; ///< Requested page size, subject to the server's limits.
};

/// @brief User page returned by the administrative listing endpoint.
struct AdminListUsersResponse
{
    std::vector<User> users; ///< Users included in this page.
    int total = 0;           ///< Server total when nonzero; otherwise the number of users in this page.
};

/// @brief Parameters for generating a link to deliver through an application mailer.
struct AdminGenerateLinkParams
{
    /// Required link type: "signup", "invite", "magiclink", "recovery",
    /// "email_change_current", or "email_change_new".
    std::string type;
    std::string email;      ///< Required email address for the link.
    std::string password;   ///< Optional password for the sign-up flow.
    std::string newEmail;   ///< Optional new address for an email-change flow.
    std::string redirectTo; ///< Optional destination after link verification.
    Json data;              ///< User metadata forwarded only when it is a JSON object.
};

/// @brief Generated link, verification material, and associated user data.
/// Link and token fields grant access to an authentication flow; do not log them.
struct AdminGenerateLinkResponse
{
    std::string action_link;       ///< Complete authentication link for delivery to the user.
    std::string email_otp;         ///< Verification code, when returned by the server.
    std::string hashed_token;      ///< Token hash for a custom verification flow.
    std::string verification_type; ///< Verification type associated with the generated link.
    User user;                     ///< User data parsed from the response.
};

/// @brief Server revocation scope for AuthClient::signOut().
/// Both scopes clear this client's local session, even when revocation fails.
enum class SignOutScope
{
    Global, ///< Revoke all of the user's sessions; the default sign-out scope.
    Local   ///< Revoke only the current session on the server.
};

/// @brief Session lifecycle events delivered to authentication subscribers.
enum class AuthEvent
{
    InitialSession,      ///< Current local snapshot, delivered synchronously during subscription.
    SignedIn,            ///< A sign-in or explicit setSession() stored a session.
    SignedOut,           ///< Local credentials were cleared; the callback receives no session.
    TokenRefreshed,      ///< A replacement session was stored after token refresh.
    UserUpdated,         ///< An account update stored new user data in the session.
    MfaChallengeVerified ///< MFA verification stored a session with the server's updated assurance level.
};

/// @brief Callback receiving an auth event and its associated session snapshot.
/// Callbacks run synchronously on the operation's thread. Keep them short and
/// defer further auth calls until they return; refresh callbacks may run while
/// token refresh is serialized. Captured state must outlive in-flight callbacks.
using AuthCallback = std::function<void(AuthEvent, const std::optional<Session>&)>;

/// @brief Persistence interface for serialized sessions.
/// Implementations may be called from any thread. Protect stored credentials and
/// synchronize access if the same storage is shared by multiple clients. Storage
/// operations run while auth state is locked and must not call back into auth.
class SessionStorage
{
  public:
    virtual ~SessionStorage() = default;

    /// @brief Load the previously saved session, or return std::nullopt when absent.
    virtual std::optional<std::string> load() = 0;
    /// @brief Replace the stored session with the supplied JSON serialization.
    virtual void save(const std::string& serialized) = 0;
    /// @brief Remove the stored session; called when local credentials are cleared.
    virtual void clear() = 0;
};

/// @brief Default session storage that keeps credentials only in process memory.
/// Direct concurrent access requires external synchronization; AuthClient
/// serializes access through its own session-state lock.
class MemorySessionStorage final : public SessionStorage
{
  public:
    /// @brief Return a copy of the currently stored serialization, if present.
    std::optional<std::string> load() override;
    /// @brief Replace the in-memory serialization.
    void save(const std::string& serialized) override;
    /// @brief Discard the in-memory serialization.
    void clear() override;

  private:
    std::optional<std::string> m_value;
};

/// @brief Move-only RAII handle for an authentication listener.
/// Retain the handle while events are needed. Destruction removes the listener;
/// callbacks already copied for dispatch may still finish after unsubscription.
class Subscription
{
  public:
    /// @brief Construct an inactive subscription.
    Subscription() = default;
    /// @brief Own an unsubscribe action, invoked at most once by this handle.
    Subscription(std::function<void()> unsubscribe);
    /// @brief Unsubscribe if the handle is still active.
    ~Subscription();
    /// @brief Transfer ownership of the unsubscribe action from another handle.
    Subscription(Subscription&& other) noexcept;
    /// @brief Unsubscribe this handle, then take ownership of another handle's action.
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    /// @brief Remove the listener; repeated calls on this handle have no effect.
    /// @note This does not wait for callbacks that are already in flight.
    void unsubscribe();

  private:
    std::function<void()> m_unsubscribe;
};

/**
 * @brief Synchronous Supabase Auth client backed by GoTrue.
 *
 * Obtain this API from supabase::Client::auth(). Authentication state, storage,
 * and event listeners are shared with the handles returned by mfa() and admin().
 * Request methods block on the configured HTTP transport and return Result<T>;
 * check success before accessing a value or an error.
 *
 * Automatic refresh is performed on demand before session-dependent operations.
 * There is no background refresh thread. Coordinate overlapping sign-in,
 * sign-out, and account updates at the application level; internal locking does
 * not make a sequence of separate operations atomic.
 *
 * @par Example: sign in with an email and password
 * @code{.cpp}
 * auto result = client.auth().signInWithPassword({
 *     .email = "player@example.com",
 *     .password = "user-supplied-password",
 * });
 * if (result && result->session)
 * {
 *     // The stored session and its user are available through result->session.
 * }
 * else if (!result)
 * {
 *     // Handle result.error().code, .message, and .status.
 * }
 * @endcode
 * Examples assume an initialized supabase::Client named `client` and run inside
 * a function. Include <supabase/supabase.hpp> for the complete public API.
 */
class AuthClient
{
  private:
    struct Impl;

  public:
    /// @brief Create an auth client using a shared request context.
    /// @param ctx Non-null context with the project URL, API key, and HTTP transport.
    /// @param storage Session persistence; null selects MemorySessionStorage.
    /// @param autoRefreshToken Refresh expiring sessions on demand when true.
    /// @note The stored session is loaded lazily when first needed.
    AuthClient(std::shared_ptr<Context> ctx, std::shared_ptr<SessionStorage> storage, bool autoRefreshToken);

    /// @brief Register a user with an email address or phone number and password.
    /// @param credentials Exactly one contact field, a password, and optional metadata or redirect.
    /// @return User data and an optional session, or a validation, transport, or server error.
    /// @note When a session is returned, it is stored and SignedIn is emitted.
    /// Confirmation requirements can produce a successful response without a session.
    Result<AuthResponse> signUp(const SignUpCredentials& credentials);

    /// @brief Sign in an existing user with an email or phone and a password.
    /// @param credentials Exactly one of email or phone, together with the account password.
    /// @return The authenticated user and session, or an error.
    /// @note Success persists the session and emits SignedIn before returning.
    Result<AuthResponse> signInWithPassword(const PasswordCredentials& credentials);

    /// @brief Exchange a provider identity token for a Supabase session.
    /// @param credentials Provider, identity token, and any required nonce or provider access token.
    /// @return The authenticated user and session, or an error from the exchange.
    /// @note Success persists the session and emits SignedIn. Provider validation is performed by the server.
    Result<AuthResponse> signInWithIdToken(const IdTokenCredentials& credentials);

    /// @brief Request an email sign-in link or a phone verification code.
    /// @param credentials Exactly one contact field and the options for user creation and delivery.
    /// @return Success when the request is accepted, or an error.
    /// @note This request does not create a local session. Complete the flow with verifyOtp()
    /// or the application's authentication-link handler.
    Result<void> signInWithOtp(const OtpCredentials& credentials);

    /// @brief Verify a delivered OTP or the token hash from an authentication link.
    /// @param params Nonempty type and either tokenHash, or token with exactly one contact field.
    /// @return User data and an optional session, or a validation, transport, or server error.
    /// @note tokenHash takes precedence. A returned session is stored and emits SignedIn.
    /// @code{.cpp}
    /// auto result = client.auth().verifyOtp({
    ///     .email = "player@example.com",
    ///     .token = "user-entered-code",
    ///     .type = "email",
    /// });
    /// @endcode
    Result<AuthResponse> verifyOtp(const VerifyOtpParams& params);

    /// @brief Construct the provider authorization URL for a browser OAuth flow.
    /// @param provider Provider identifier, such as "google" or "github".
    /// @param options Redirect URL, scopes, additional query parameters, and PKCE selection.
    /// @return URL for the application to open in a browser; no HTTP request is made here.
    /// @note With PKCE enabled, one verifier is held in memory on this auth instance.
    /// Starting another PKCE flow replaces it. The application handles the redirect
    /// and completes the exchange on the same auth instance.
    /// @code{.cpp}
    /// auto url = client.auth().getOAuthSignInUrl("google", {
    ///     .redirectTo = "myapp://auth/callback",
    ///     .pkce = true,
    /// });
    /// // Open url in a browser and pass the callback's code to exchangeCodeForSession().
    /// @endcode
    [[nodiscard]] std::string getOAuthSignInUrl(std::string_view provider, const OAuthOptions& options = {}) const;

    /// @brief Complete a PKCE flow by exchanging the redirect's authorization code.
    /// @param authCode Nonempty code from the OAuth callback query string.
    /// @return The stored session, or an error if the code, saved verifier, or exchange is invalid.
    /// @pre getOAuthSignInUrl() was called with OAuthOptions::pkce on this auth instance.
    /// @note The verifier is consumed at the start of the call, including failed attempts.
    /// Success persists the session and emits SignedIn.
    Result<Session> exchangeCodeForSession(std::string_view authCode);

    /// @brief Request a replacement session using a refresh token.
    /// @param refreshToken Token to use; empty selects the current session's refresh token.
    /// @return Refreshed session, or an error; no available token produces errc::NoSession.
    /// @note A successful exchange persists the session and emits TokenRefreshed.
    /// A refresh rejection with HTTP 400 or 401 clears the matching local session
    /// and emits SignedOut. Other failures retain the current session.
    Result<Session> refreshSession(std::string_view refreshToken = {});

    /// @brief Install a supplied session locally without contacting the server.
    /// @param session Session with a nonempty access_token and any available user and refresh data.
    /// @return The stored session, or errc::InvalidArgument when the access token is empty.
    /// @note Persists the session and emits SignedIn. Tokens and user data are not verified by this call.
    Result<Session> setSession(Session session);

    /// @brief Revoke server sessions and clear this client's local credentials.
    /// @param scope Global revokes all of the user's sessions; Local revokes only the current session.
    /// @return Success when revocation succeeds, no local session exists, or the server
    /// reports HTTP 401, 403, or 404; other server and transport failures are returned.
    /// @note Both scopes clear persisted credentials and emit SignedOut even on error.
    /// @code{.cpp}
    /// auto result = client.auth().signOut(supabase::auth::SignOutScope::Local);
    /// // Local credentials are cleared even if result contains a revocation error.
    /// @endcode
    Result<void> signOut(SignOutScope scope = SignOutScope::Global);

    /// @brief Fetch the current user from the authentication server.
    /// @return Server user data, errc::NoSession when signed out, or a request error.
    /// @note May refresh the session first. The returned user does not replace the local
    /// session's user snapshot, and this call does not emit UserUpdated.
    Result<User> getUser();

    /// @brief Update the signed-in user's authentication account fields.
    /// @param attributes Optional email, phone, password, or user metadata fields to send.
    /// @return Updated server user data, errc::NoSession when signed out, or a request error.
    /// @note Success stores the returned user in the current session and emits UserUpdated.
    /// Contact changes may require confirmation according to the project's policy.
    Result<User> updateUser(const UserAttributes& attributes);

    /// @brief Request a password recovery email.
    /// @param email Nonempty account email address.
    /// @param redirectTo Optional allowed application URL for the recovery redirect.
    /// @return Success when the request is accepted, or a validation, transport, or server error.
    /// @note This call does not change the password or install a session. Handle the
    /// recovery flow in the application and use updateUser() to set the new password.
    Result<void> resetPasswordForEmail(std::string_view email, std::string_view redirectTo = {});

    /// @brief Read a copy of the current local session, refreshing it on demand.
    /// @return Session snapshot, or std::nullopt when no local session remains.
    /// @note With automatic refresh enabled, an expiring session with a refresh token
    /// can trigger a blocking request. Transient refresh failures retain the old snapshot;
    /// HTTP 400/401 rejection clears the matching local session. Use refreshSession() to inspect refresh errors.
    /// A returned session is not proof that its credentials are accepted by the server.
    [[nodiscard]] std::optional<Session> getSession();

    /// @brief Subscribe to the current local session and subsequent authentication changes.
    /// @param callback Nonempty callback invoked synchronously on the operation's thread.
    /// @return Move-only handle that removes the listener on destruction or unsubscribe().
    /// @note InitialSession is delivered before this call returns, without a token refresh.
    /// Further callbacks may overlap when operations run on different threads. Defer
    /// auth calls until callbacks return, and keep captured state valid for in-flight calls.
    /// @code{.cpp}
    /// auto subscription = client.auth().onAuthStateChange(
    ///     [](supabase::auth::AuthEvent event, const std::optional<supabase::auth::Session>& session)
    ///     {
    ///         // Schedule application state changes; session is empty after SignedOut.
    ///     });
    /// // Retain subscription for as long as the listener is needed.
    /// @endcode
    [[nodiscard]] Subscription onAuthStateChange(AuthCallback callback);

    /// @brief MFA enrollment, challenge, and verification operations for the current user.
    /// Obtain this handle through AuthClient::mfa(). Operations require a local
    /// signed-in session, may refresh it, and report request failures through Result<T>.
    class Mfa
    {
      public:
        /// @brief Enroll a new MFA factor without verifying it.
        /// @param params Factor type and optional display name, issuer, or phone number.
        /// @return Factor data and any TOTP setup material, or an error.
        /// @note Complete challenge() and verify() to verify the enrolled factor.
        Result<MfaEnrollResponse> enroll(const MfaEnrollParams& params = {});

        /// @brief Create a verification challenge for an enrolled factor.
        /// @param factorId Nonempty factor identifier returned by enrollment or listFactors().
        /// @param channel Optional delivery channel for a phone factor; omitted when empty.
        /// @return Challenge identifier and expiry, or an error.
        Result<MfaChallenge> challenge(std::string_view factorId, std::string_view channel = {});

        /// @brief Verify an MFA challenge and store the session returned by the server.
        /// @param factorId Nonempty identifier of the challenged factor.
        /// @param challengeId Nonempty identifier returned by challenge().
        /// @param code Nonempty authenticator or delivered verification code.
        /// @return Updated session, or a validation, transport, or server error.
        /// @note Success persists the new session and emits MfaChallengeVerified.
        Result<Session> verify(std::string_view factorId, std::string_view challengeId, std::string_view code);

        /// @brief Create a challenge without an explicit channel and verify it with a code.
        /// @param factorId Factor to challenge.
        /// @param code Authenticator code to verify against the newly created challenge.
        /// @return Updated session, or the first challenge or verification error.
        /// @note Performs two requests. For phone delivery, use challenge() with the
        /// required channel, obtain the delivered code, and then call verify().
        /// @code{.cpp}
        /// auto result = client.auth().mfa().challengeAndVerify("enrolled-factor-id", "user-entered-code");
        /// @endcode
        Result<Session> challengeAndVerify(std::string_view factorId, std::string_view code);

        /// @brief Remove an enrolled factor from the current user's account.
        /// @param factorId Nonempty identifier of the factor to remove.
        /// @return Removed factor identifier from the response, or an error.
        /// @note Removal does not update the local user snapshot or emit an additional auth event.
        Result<std::string> unenroll(std::string_view factorId);

        /// @brief Fetch the current user's enrolled factors from the server.
        /// @return All factors and verified TOTP/phone subsets, or an error.
        /// @note A response without a factors array yields empty lists. This call
        /// does not replace the user snapshot stored in the local session.
        Result<MfaFactors> listFactors();

        /// @brief Derive assurance levels and authentication methods from local session data.
        /// @return Current level, available next level, and methods, or errc::NoSession.
        /// @note May refresh the session first. JWT claims are decoded without signature
        /// verification, and available factors come from the local user snapshot.
        /// Enforce assurance requirements on the server rather than trusting this result.
        Result<AuthenticatorAssuranceLevel> getAuthenticatorAssuranceLevel();

      private:
        friend class AuthClient;
        std::shared_ptr<Impl> m_impl;
    };

    /// @brief Privileged GoTrue account management for a trusted server environment.
    /// Obtain this handle through AuthClient::admin(). User-management requests
    /// use the supplied service-role key and do not require a signed-in user.
    /// Never distribute that key in a client application. These operations do
    /// not install sessions or emit auth events on the calling client.
    class Admin
    {
      public:
        /// @brief Fetch one page of users from the administrative endpoint.
        /// @param params One-based page and requested page size, forwarded to the server.
        /// @return User page and server total when available, or a request error.
        Result<AdminListUsersResponse> listUsers(const AdminListUsersParams& params = {});

        /// @brief Fetch an account by its user identifier.
        /// @param id Nonempty user identifier.
        /// @return Server user data, or a validation, transport, or server error.
        Result<User> getUserById(std::string_view id);

        /// @brief Create an account using administrative privileges.
        /// @param attributes Account credentials, confirmation flags, and metadata to send.
        /// @return Created user data, or a request error.
        Result<User> createUser(const AdminCreateUserAttributes& attributes);

        /// @brief Update an account by its user identifier.
        /// @param id Nonempty user identifier.
        /// @param attributes Fields to send; empty strings/metadata and false confirmation flags are omitted.
        /// @return Updated user data, or a validation, transport, or server error.
        Result<User> updateUserById(std::string_view id, const AdminCreateUserAttributes& attributes);

        /// @brief Request deletion of an account.
        /// @param id Nonempty user identifier.
        /// @param shouldSoftDelete Forward the server's soft-deletion option when true.
        /// @return Success when deletion is accepted, or an error.
        Result<void> deleteUser(std::string_view id, bool shouldSoftDelete = false);

        /// @brief Request an invitation email for an account.
        /// @param email Nonempty email address to invite.
        /// @param redirectTo Optional destination after the invitation is accepted.
        /// @param data User metadata forwarded only when it is a JSON object.
        /// @return Invited user data, or a validation, transport, or server error.
        Result<User> inviteUserByEmail(std::string_view email, std::string_view redirectTo = {}, const Json& data = nullptr);

        /// @brief Generate an authentication link for delivery by the application.
        /// @param params Nonempty type and email, with any additional flow-specific fields.
        /// @return Link, verification material, and parsed user data, or an error.
        /// @note Delivery of the returned link is the application's responsibility.
        Result<AdminGenerateLinkResponse> generateLink(const AdminGenerateLinkParams& params);

        /// @brief Request global server revocation using the supplied user's access token.
        /// @param jwt Nonempty user access token sent as the request's bearer credential.
        /// @return Success when revocation succeeds, or a validation, transport, or server error.
        /// @note This method sends jwt rather than the handle's service-role key.
        /// It does not clear this client's local session. Use AuthClient::signOut()
        /// when local credentials must also be removed.
        Result<void> signOut(std::string_view jwt);

      private:
        friend class AuthClient;
        std::shared_ptr<Impl> m_impl;
        std::string m_serviceKey;
    };

    /// @brief Access MFA operations sharing this auth instance's session and transport.
    /// @return A handle retaining the shared auth implementation; no request is made.
    [[nodiscard]] Mfa mfa() const;

    /// @brief Access privileged account management with a service-role key.
    /// @param serviceRoleKey Server-only credential retained by the returned handle.
    /// @return An admin handle sharing the transport; no request or key validation is performed here.
    [[nodiscard]] Admin admin(std::string serviceRoleKey) const;

  private:
    std::shared_ptr<Impl> m_impl;
};

}
