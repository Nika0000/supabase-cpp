// Dependency-free tests driven by a mock transport. No network access.
#include <chrono>
#include <condition_variable>
#include <cstdio>

#include <supabase/supabase.hpp>

#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace
{
int failures = 0;

#define CHECK(cond)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(cond))                                                                                                                       \
        {                                                                                                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                                    \
            ++failures;                                                                                                                    \
        }                                                                                                                                  \
    } while (false)

using namespace supabase;

class MockTransport final : public http::Transport
{
  public:
    Result<http::Response> send(const http::Request& request, http::ChunkCallback onChunk) override
    {
        requests.push_back(request);
        if (replies.empty())
            return makeError(errc::Network, "no reply queued");
        auto reply = std::move(replies.front());
        replies.pop_front();
        if (onChunk)
            onChunk(reply.body);
        return reply;
    }

    void reply(int status, std::string body, http::Headers headers = {})
    {
        replies.push_back(http::Response { status, std::move(headers), std::move(body) });
    }

    std::vector<http::Request> requests;
    std::deque<http::Response> replies;
};

std::string headerOf(const http::Request& request, std::string_view name)
{
    for (const auto& [key, value] : request.headers)
    {
        if (key == name)
            return value;
    }
    return {};
}

Client makeClient(const std::shared_ptr<MockTransport>& transport)
{
    ClientOptions options;
    options.transport = transport;
    return createClient("https://x.supabase.co/", "anon", options);
}

void selectBuildsUrl()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, R"([{"id":1}])", { { "Content-Range", "0-0/42" } });

    auto result = client.from("games")
                      .select("id, name", postgrest::CountMode::Exact)
                      .eq("title", "a b")
                      .in("id", { 1, 2 })
                      .order("id", { .ascending = false })
                      .limit(5)
                      .execute();

    CHECK(result.ok());
    CHECK(result.value().data.size() == 1);
    CHECK(result.value().count == 42);
    const auto& request = mock->requests.at(0);
    CHECK(request.method == http::Method::Get);
    CHECK(request.url == "https://x.supabase.co/rest/v1/games?select=id%2Cname&title=eq.a%20b&id=in.%281%2C2%29&order=id.desc&limit=5");
    CHECK(headerOf(request, "apikey") == "anon");
    CHECK(headerOf(request, "Authorization") == "Bearer anon");
    CHECK(headerOf(request, "Prefer") == "count=exact");
}

void insertSendsBodyAndPrefer()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(201, R"([{"id":9}])");

    auto result = client.from("games").insert({ { "name", "x" } }).select().single().execute();
    CHECK(result.ok());
    const auto& request = mock->requests.at(0);
    CHECK(request.method == http::Method::Post);
    CHECK(request.body == R"({"name":"x"})");
    CHECK(headerOf(request, "Prefer") == "return=representation");
    CHECK(headerOf(request, "Accept") == "application/vnd.pgrst.object+json");
    CHECK(headerOf(request, "Content-Profile") == "public");
}

void errorIsMapped()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(404, R"({"code":"42P01","message":"no table","details":"d","hint":"h"})");

    auto result = client.from("nope").select().execute();
    CHECK(!result.ok());
    CHECK(result.error().code == "42P01");
    CHECK(result.error().message == "no table");
    CHECK(result.error().status == 404);
    CHECK(result.error().hint == "h");
}

void maybeSingleHandlesEmptyAndMany()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, "[]");
    mock->reply(200, R"([{"a":1},{"a":2}])");

    auto none = client.from("t").select().maybeSingle().execute();
    CHECK(none.ok() && none.value().data.is_null());
    auto many = client.from("t").select().maybeSingle().execute();
    CHECK(!many.ok() && many.error().code == "PGRST116");
}

void rpcPostsArgs()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, "5");

    auto result = client.rpc("add", { { "a", 2 }, { "b", 3 } }).execute<int>();
    CHECK(result.ok() && result.value().data == 5);
    CHECK(mock->requests.at(0).url == "https://x.supabase.co/rest/v1/rpc/add");
    CHECK(mock->requests.at(0).method == http::Method::Post);
}

void signInStoresSessionAndAuthorizesRequests()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, R"({"access_token":"tok","refresh_token":"r","expires_in":3600,"user":{"id":"u1","email":"a@b.c"}})");
    mock->reply(200, "[]");

    int events        = 0;
    auto subscription = client.auth().onAuthStateChange([&](auth::AuthEvent, const std::optional<auth::Session>&) { ++events; });
    auto response     = client.auth().signInWithPassword({ .email = "a@b.c", .password = "pw" });

    CHECK(response.ok());
    CHECK(response.value().user->id == "u1");
    CHECK(mock->requests.at(0).url == "https://x.supabase.co/auth/v1/token?grant_type=password");
    CHECK(events == 2); // InitialSession + SignedIn

    auto rows = client.from("t").select().execute();
    CHECK(rows.ok());
    CHECK(headerOf(mock->requests.at(1), "Authorization") == "Bearer tok");
}

void signInRejectsAmbiguousIdentity()
{
    auto mock     = std::make_shared<MockTransport>();
    auto client   = makeClient(mock);
    auto response = client.auth().signInWithPassword({ .password = "pw" });
    CHECK(!response.ok() && response.error().code == errc::InvalidArgument);
    CHECK(mock->requests.empty());
}

void signOutClearsSessionEvenOnServerRejection()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, R"({"access_token":"tok","refresh_token":"r","expires_in":3600,"user":{"id":"u1"}})");
    mock->reply(401, R"({"msg":"expired"})");

    CHECK(client.auth().signInWithPassword({ .email = "a@b.c", .password = "pw" }).ok());
    CHECK(client.auth().signOut().ok());
    CHECK(!client.auth().getSession().has_value());
}

void pkceExchangeUsesSavedVerifier()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, R"({"access_token":"tok","refresh_token":"r","expires_in":3600,"user":{"id":"u1"}})");

    CHECK(!client.auth().exchangeCodeForSession("code").ok()); // no verifier yet
    const auto url = client.auth().getOAuthSignInUrl("github", { .pkce = true });
    const auto at  = url.find("code_challenge=");
    CHECK(at != std::string::npos);
    CHECK(url.find("&code_challenge_method=s256") == at + 15 + 43); // SHA-256 -> 43 base64url chars

    CHECK(client.auth().exchangeCodeForSession("code").ok());
    CHECK(mock->requests.at(0).url == "https://x.supabase.co/auth/v1/token?grant_type=pkce");
    const auto body = Json::parse(mock->requests.at(0).body);
    CHECK(body["auth_code"] == "code" && body["code_verifier"].get<std::string>().size() == 64);
    CHECK(!client.auth().exchangeCodeForSession("code").ok()); // verifier is single-use
}

void mfaVerifyUpgradesSession()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    client.auth().setSession({ .access_token = "a1", .refresh_token = "r1", .user = { .id = "u1" } });
    mock->reply(200, R"({"id":"c1","expires_at":99})");
    mock->reply(200, R"({"access_token":"a2","refresh_token":"r2","expires_in":3600})");

    auto session = client.auth().mfa().challengeAndVerify("f1", "123456");

    CHECK(session.ok() && session.value().access_token == "a2");
    CHECK(session.value().user.id == "u1");
    CHECK(mock->requests.at(0).url == "https://x.supabase.co/auth/v1/factors/f1/challenge");
    CHECK(mock->requests.at(1).url == "https://x.supabase.co/auth/v1/factors/f1/verify");
    CHECK(headerOf(mock->requests.at(1), "Authorization") == "Bearer a1");
    CHECK(client.auth().getSession()->access_token == "a2");
}

void mfaAalReadsJwtClaims()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    // payload: {"aal":"aal1","amr":[{"method":"password"}]}
    const std::string jwt = "h.eyJhYWwiOiJhYWwxIiwiYW1yIjpbeyJtZXRob2QiOiJwYXNzd29yZCJ9XX0.s";
    client.auth().setSession(
        { .access_token = jwt, .user = { .raw = Json { { "factors", Json::array({ { { "status", "verified" } } }) } } } }
    );

    auto level = client.auth().mfa().getAuthenticatorAssuranceLevel();

    CHECK(level.ok());
    CHECK(level.value().currentLevel == "aal1" && level.value().nextLevel == "aal2");
    CHECK(level.value().currentAuthenticationMethods.size() == 1 && level.value().currentAuthenticationMethods[0] == "password");
}

void adminUsesServiceKey()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, R"({"users":[{"id":"u1"},{"id":"u2"}],"total":7})");
    mock->reply(200, "{}");

    auto admin = client.auth().admin("svc");
    auto users = admin.listUsers({ .page = 2, .perPage = 10 });
    auto gone  = admin.deleteUser("u1", true);

    CHECK(users.ok() && users.value().users.size() == 2 && users.value().total == 7);
    CHECK(mock->requests.at(0).url == "https://x.supabase.co/auth/v1/admin/users?page=2&per_page=10");
    CHECK(headerOf(mock->requests.at(0), "Authorization") == "Bearer svc");
    CHECK(gone.ok() && mock->requests.at(1).method == http::Method::Delete);
    CHECK(mock->requests.at(1).url == "https://x.supabase.co/auth/v1/admin/users/u1");
}

void mfaRequiresSession()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    CHECK(client.auth().mfa().enroll().error().code == errc::NoSession);
}

void functionsInvoke()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    mock->reply(200, R"({"ok":true})");
    mock->reply(500, R"({"error":"boom"})");

    functions::InvokeOptions options;
    options.json = Json { { "q", 1 } };
    auto ok      = client.functions().invoke("hello", options);
    CHECK(ok.ok() && ok.value().json()["ok"] == true);
    CHECK(mock->requests.at(0).url == "https://x.supabase.co/functions/v1/hello");

    auto bad = client.functions().invoke("hello", options);
    CHECK(!bad.ok() && bad.error().code == "FunctionsHttpError" && bad.error().message == "boom");
}

struct SocketState
{
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> inbox;
    std::vector<Json> sent;
    std::string url;

    void push(const Json& frame)
    {
        std::lock_guard lock(mu);
        inbox.push_back(frame.dump());
        cv.notify_all();
    }

    std::vector<Json> frames()
    {
        std::lock_guard lock(mu);
        return sent;
    }

    /// First sent frame with this event, or null.
    Json find(std::string_view event)
    {
        for (auto& frame : frames())
            if (frame.value("event", "") == event)
                return frame;
        return nullptr;
    }
};

class MockSocket final : public ws::Socket
{
  public:
    explicit MockSocket(std::shared_ptr<SocketState> state) : state_(std::move(state)) {}

    Result<void> connect(const std::string& url, const http::Headers&, std::chrono::milliseconds) override
    {
        std::lock_guard lock(state_->mu);
        state_->url = url;
        return {};
    }

    Result<void> send(std::string_view text) override
    {
        std::lock_guard lock(state_->mu);
        state_->sent.push_back(Json::parse(text));
        return {};
    }

    Result<std::optional<std::string>> receive(std::chrono::milliseconds wait) override
    {
        std::unique_lock lock(state_->mu);
        state_->cv.wait_for(lock, wait, [&] { return !state_->inbox.empty(); });
        if (state_->inbox.empty())
            return std::optional<std::string> {};
        std::optional<std::string> message(std::move(state_->inbox.front()));
        state_->inbox.pop_front();
        return message;
    }

    void close() noexcept override {}

  private:
    std::shared_ptr<SocketState> state_;
};

template <typename Pred> bool waitUntil(Pred pred)
{
    for (int i = 0; i < 400; ++i)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

Client makeRealtimeClient(const std::shared_ptr<SocketState>& state)
{
    ClientOptions options;
    options.transport     = std::make_shared<MockTransport>();
    options.socketFactory = [state]() -> std::unique_ptr<ws::Socket> { return std::make_unique<MockSocket>(state); };
    return createClient("https://x.supabase.co", "anon", options);
}

Json frame(std::string topic, std::string event, Json payload, std::string ref = {})
{
    return { { "topic", topic }, { "event", event }, { "payload", std::move(payload) }, { "ref", ref } };
}

void realtimeChannelLifecycle()
{
    auto state  = std::make_shared<SocketState>();
    auto client = makeRealtimeClient(state);

    std::mutex mu;
    std::vector<Json> changes, broadcasts, syncs, joins;
    std::vector<realtime::SubscribeStatus> statuses;
    auto record = [&](std::vector<Json>& into)
    {
        return [&](const Json& payload)
        {
            std::lock_guard lock(mu);
            into.push_back(payload);
        };
    };

    auto channel = client.realtime().channel("room");
    CHECK(channel->topic() == "realtime:room");
    CHECK(client.realtime().channel("room") == channel);
    channel->onPostgresChanges({ "INSERT", "public", "games", "id=eq.1" }, record(changes))
        .onBroadcast("ping", record(broadcasts))
        .onPresence(realtime::PresenceEvent::Sync, record(syncs))
        .onPresence(realtime::PresenceEvent::Join, record(joins));
    CHECK(channel
              ->subscribe(
                  [&](realtime::SubscribeStatus status, const std::string&)
                  {
                      std::lock_guard lock(mu);
                      statuses.push_back(status);
                  }
              )
              .ok());

    CHECK(waitUntil([&] { return !state->find("phx_join").is_null(); }));
    const Json join = state->find("phx_join");
    CHECK(state->url == "wss://x.supabase.co/realtime/v1/websocket?apikey=anon&vsn=1.0.0");
    CHECK(join.value("topic", "") == "realtime:room");
    CHECK(join["payload"]["access_token"] == "anon");
    CHECK(join["payload"]["config"]["postgres_changes"][0]["table"] == "games");
    CHECK(join["payload"]["config"]["postgres_changes"][0]["filter"] == "id=eq.1");

    const std::string ref = join["ref"];
    state->push(frame(
        "realtime:room",
        "phx_reply",
        { { "status", "ok" }, { "response", { { "postgres_changes", Json::array({ Json { { "id", 42 } } }) } } } },
        ref
    ));
    CHECK(waitUntil([&] { return channel->state() == realtime::ChannelState::Joined; }));
    CHECK(waitUntil(
        [&]
        {
            std::lock_guard lock(mu);
            return statuses.size() == 1;
        }
    ));
    CHECK(statuses[0] == realtime::SubscribeStatus::Subscribed);

    // Wrong id and wrong event type are filtered; the later broadcast proves they were already processed.
    const auto change = [](int id, const char* type)
    {
        return Json { { "ids", Json::array({ id }) },
                      { "data", { { "schema", "public" }, { "table", "games" }, { "type", type }, { "record", { { "id", 1 } } } } } };
    };
    state->push(frame("realtime:room", "postgres_changes", change(7, "INSERT")));
    state->push(frame("realtime:room", "postgres_changes", change(42, "UPDATE")));
    state->push(frame("realtime:room", "postgres_changes", change(42, "INSERT")));
    state->push(frame("realtime:room", "broadcast", { { "type", "broadcast" }, { "event", "ping" }, { "payload", { { "n", 1 } } } }));
    CHECK(waitUntil(
        [&]
        {
            std::lock_guard lock(mu);
            return !broadcasts.empty();
        }
    ));
    {
        std::lock_guard lock(mu);
        CHECK(changes.size() == 1);
        CHECK(changes[0]["eventType"] == "INSERT");
        CHECK(changes[0]["new"]["id"] == 1);
        CHECK(changes[0]["table"] == "games");
        CHECK(broadcasts[0]["payload"]["n"] == 1);
    }

    const auto metas = [](const char* phxRef) { return Json { { "metas", Json::array({ Json { { "phx_ref", phxRef } } }) } }; };
    state->push(frame("realtime:room", "presence_state", { { "u1", metas("a") } }));
    state->push(frame("realtime:room", "presence_diff", { { "joins", { { "u2", metas("b") } } }, { "leaves", { { "u1", metas("a") } } } }));
    CHECK(waitUntil(
        [&]
        {
            std::lock_guard lock(mu);
            return syncs.size() == 2;
        }
    ));
    const Json presence = channel->presenceState();
    CHECK(!presence.contains("u1"));
    CHECK(presence["u2"]["metas"].size() == 1);
    {
        std::lock_guard lock(mu);
        CHECK(joins.size() == 2); // u1 from the snapshot, u2 from the diff
    }

    CHECK(channel->send("ping", { { "n", 2 } }).ok());
    CHECK(channel->track({ { "name", "a" } }).ok());
    CHECK(waitUntil([&] { return !state->find("broadcast").is_null() && !state->find("presence").is_null(); }));
    CHECK(state->find("broadcast")["payload"]["payload"]["n"] == 2);
    CHECK(state->find("presence")["payload"]["event"] == "track");

    CHECK(channel->unsubscribe().ok());
    CHECK(channel->state() == realtime::ChannelState::Closed);
    CHECK(waitUntil([&] { return !state->find("phx_leave").is_null(); }));
    CHECK(channel->send("ping", Json::object()).error().code == errc::InvalidArgument);
    client.realtime().disconnect();
}

void realtimeJoinErrorIsReported()
{
    auto state  = std::make_shared<SocketState>();
    auto client = makeRealtimeClient(state);

    std::mutex mu;
    std::string message;
    bool failed  = false;
    auto channel = client.realtime().channel("bad");
    CHECK(channel
              ->subscribe(
                  [&](realtime::SubscribeStatus status, const std::string& reason)
                  {
                      std::lock_guard lock(mu);
                      failed  = status == realtime::SubscribeStatus::ChannelError;
                      message = reason;
                  }
              )
              .ok());
    CHECK(channel->subscribe().error().code == errc::InvalidArgument);
    CHECK(waitUntil([&] { return !state->find("phx_join").is_null(); }));
    const std::string ref = state->find("phx_join")["ref"];
    state->push(frame("realtime:bad", "phx_reply", { { "status", "error" }, { "response", { { "reason", "no such table" } } } }, ref));
    CHECK(waitUntil(
        [&]
        {
            std::lock_guard lock(mu);
            return failed;
        }
    ));
    CHECK(message == "no such table");
    CHECK(channel->state() == realtime::ChannelState::Errored);
}

void stubsReportNotImplemented()
{
    auto mock   = std::make_shared<MockTransport>();
    auto client = makeClient(mock);
    CHECK(client.graphql().query("{ a }").error().code == errc::NotImplemented);
}
}

int main()
{
    selectBuildsUrl();
    insertSendsBodyAndPrefer();
    errorIsMapped();
    maybeSingleHandlesEmptyAndMany();
    rpcPostsArgs();
    signInStoresSessionAndAuthorizesRequests();
    signInRejectsAmbiguousIdentity();
    signOutClearsSessionEvenOnServerRejection();
    pkceExchangeUsesSavedVerifier();
    mfaVerifyUpgradesSession();
    mfaAalReadsJwtClaims();
    adminUsesServiceKey();
    mfaRequiresSession();
    functionsInvoke();
    realtimeChannelLifecycle();
    realtimeJoinErrorIsReported();
    stubsReportNotImplemented();
    std::printf(failures == 0 ? "ok\n" : "%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
