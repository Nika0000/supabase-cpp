// End-to-end tests against a live (local) Supabase stack.
// Environment: SUPABASE_URL, SUPABASE_ANON_KEY, SUPABASE_SERVICE_KEY.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>

#include <supabase/supabase.hpp>

#include <mutex>
#include <string>

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

#define CHECK_OK(result)                                                                                                                   \
    do                                                                                                                                     \
    {                                                                                                                                      \
        if (!(result))                                                                                                                     \
        {                                                                                                                                  \
            std::printf("FAIL %s:%d  %s: %s\n", __FILE__, __LINE__, #result, (result).error().message.c_str());                            \
            ++failures;                                                                                                                    \
        }                                                                                                                                  \
    } while (false)

using namespace supabase;
using namespace std::chrono_literals;

std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value ? value : "";
}

std::string uniqueSuffix() { return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()); }

/// One-shot wait for a value delivered from the realtime worker thread.
class Latch
{
  public:
    void set(Json value)
    {
        {
            std::lock_guard lock(mutex_);
            value_ = std::move(value);
            set_   = true;
        }
        cv_.notify_all();
    }

    bool wait(std::chrono::seconds timeout)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return set_; });
    }

    Json value() const
    {
        std::lock_guard lock(mutex_);
        return value_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Json value_;
    bool set_ = false;
};

void postgrestCrud(Client& client)
{
    auto inserted = client.from("todos").insert(Json { { "title", "first" } }).select().single().execute();
    CHECK_OK(inserted);
    if (!inserted)
        return;
    const auto id = inserted.value().data["id"];
    CHECK(inserted.value().data["title"] == "first");
    CHECK(inserted.value().data["done"] == false);

    auto updated = client.from("todos").update(Json { { "done", true } }).eq("id", id).select().single().execute();
    CHECK_OK(updated);
    if (updated)
        CHECK(updated.value().data["done"] == true);

    auto counted = client.from("todos").select("*", postgrest::CountMode::Exact).eq("id", id).execute();
    CHECK_OK(counted);
    if (counted)
    {
        CHECK(counted.value().data.size() == 1);
        CHECK(counted.value().count == 1);
    }

    auto removed = client.from("todos").remove().eq("id", id).execute();
    CHECK_OK(removed);

    auto gone = client.from("todos").select().eq("id", id).maybeSingle().execute();
    CHECK_OK(gone);
    if (gone)
        CHECK(gone.value().data.is_null());

    auto missing = client.from("no_such_table").select().execute();
    CHECK(!missing.ok());
    if (!missing)
        CHECK(missing.error().isHttp());
}

void rpcCall(Client& client)
{
    auto sum = client.rpc("add_numbers", Json { { "a", 2 }, { "b", 3 } }).execute();
    CHECK_OK(sum);
    if (sum)
        CHECK(sum.value().data == 5);
}

void authFlow(Client& client, const std::string& serviceKey)
{
    const auto email           = "it-" + uniqueSuffix() + "@example.com";
    const std::string password = "correct-horse-battery";

    auto admin   = client.auth().admin(serviceKey);
    auto created = admin.createUser({ .email = email, .password = password, .emailConfirm = true });
    CHECK_OK(created);
    if (!created)
        return;

    auto signedIn = client.auth().signInWithPassword({ .email = email, .password = password });
    CHECK_OK(signedIn);
    if (signedIn)
    {
        CHECK(signedIn.value().session.has_value());
        CHECK(signedIn.value().user && signedIn.value().user->email == email);
    }

    auto wrong = client.auth().signInWithPassword({ .email = email, .password = "nope" });
    CHECK(!wrong.ok());

    auto signedOut = client.auth().signOut();
    CHECK_OK(signedOut);
    auto deleted = admin.deleteUser(created.value().id);
    CHECK_OK(deleted);
}

void storageRoundTrip(Client& client)
{
    const auto bucket   = "it-" + uniqueSuffix();
    const auto& storage = client.storage();

    auto made = storage.createBucket(bucket);
    CHECK_OK(made);
    auto files = storage.from(bucket);

    auto up = files.upload("dir/hello.txt", "hello world", { .contentType = "text/plain" });
    CHECK_OK(up);

    auto down = files.download("dir/hello.txt");
    CHECK_OK(down);
    if (down)
        CHECK(down.value() == "hello world");

    auto listed = files.list("dir");
    CHECK_OK(listed);
    if (listed)
        CHECK(listed.value().size() == 1);

    auto signedUrl = files.createSignedUrl("dir/hello.txt", 60);
    CHECK_OK(signedUrl);

    auto moved = files.move("dir/hello.txt", "dir/moved.txt");
    CHECK_OK(moved);
    auto removed = files.remove({ "dir/moved.txt" });
    CHECK_OK(removed);
    auto dropped = storage.deleteBucket(bucket);
    CHECK_OK(dropped);
}

void functionsInvoke(Client& client)
{
    auto reply = client.functions().invoke("echo", { .json = Json { { "name", "world" } } });
    CHECK_OK(reply);
    if (reply)
    {
        CHECK(reply.value().status == 200);
        CHECK(Json::parse(reply.value().body, nullptr, false)["echo"]["name"] == "world");
    }
}

void realtimePostgresChanges(Client& client)
{
    Latch subscribed, postgresReady, inserted;
    auto channel = client.realtime().channel("it-changes-" + uniqueSuffix());
    channel->on(
        "system",
        [&](const Json& status)
        {
            if (status.value("extension", std::string {}) == "postgres_changes" && status.value("status", std::string {}) == "ok")
                postgresReady.set(status);
        }
    );
    channel->onPostgresChanges(
        { .event = "INSERT", .table = "todos" },
        [&](const Json& change)
        {
            if (change["new"]["title"] == "from-realtime")
                inserted.set(change);
        }
    );
    auto sub = channel->subscribe([&](realtime::SubscribeStatus status, const std::string&)
                                  { subscribed.set(Json { { "status", static_cast<int>(status) } }); });
    CHECK_OK(sub);
    CHECK(subscribed.wait(15s));
    CHECK(subscribed.value()["status"] == static_cast<int>(realtime::SubscribeStatus::Subscribed));
    CHECK(channel->state() == realtime::ChannelState::Joined);
    // A channel join can complete before Realtime has attached its Postgres subscription.
    const bool ready = postgresReady.wait(30s);
    CHECK(ready);
    if (!ready)
    {
        auto left = client.realtime().removeChannel(channel);
        CHECK_OK(left);
        return;
    }

    auto row = client.from("todos").insert(Json { { "title", "from-realtime" } }).select().single().execute();
    CHECK_OK(row);
    const bool received = inserted.wait(15s);
    CHECK(received);
    if (received)
        CHECK(inserted.value()["eventType"] == "INSERT");

    if (row)
    {
        auto cleaned = client.from("todos").remove().eq("id", row.value().data["id"]).execute();
        CHECK_OK(cleaned);
    }
    auto left = client.realtime().removeChannel(channel);
    CHECK_OK(left);
}

void realtimeBroadcast(Client& client)
{
    Latch subscribed, received;
    auto channel = client.realtime().channel("it-broadcast-" + uniqueSuffix(), { .broadcastSelf = true });
    channel->onBroadcast("ping", [&](const Json& message) { received.set(message); });
    auto sub = channel->subscribe([&](realtime::SubscribeStatus status, const std::string&)
                                  { subscribed.set(Json { { "status", static_cast<int>(status) } }); });
    CHECK_OK(sub);
    CHECK(subscribed.wait(15s));

    auto sent = channel->send("ping", Json { { "n", 42 } });
    CHECK_OK(sent);
    CHECK(received.wait(15s));
    CHECK(received.value()["payload"]["n"] == 42);

    auto left = client.realtime().removeChannel(channel);
    CHECK_OK(left);
}
}

int main()
{
    const auto url        = env("SUPABASE_URL");
    const auto anonKey    = env("SUPABASE_ANON_KEY");
    const auto serviceKey = env("SUPABASE_SERVICE_KEY");
    if (url.empty() || anonKey.empty() || serviceKey.empty())
    {
        std::printf("SUPABASE_URL, SUPABASE_ANON_KEY and SUPABASE_SERVICE_KEY must be set\n");
        return 2;
    }

    auto client     = createClient(url, anonKey);
    auto privileged = createClient(url, serviceKey); // storage bucket management needs the service role

    postgrestCrud(client);
    rpcCall(client);
    authFlow(client, serviceKey);
    storageRoundTrip(privileged);
    functionsInvoke(client);
    realtimePostgresChanges(client);
    realtimeBroadcast(client);
    client.realtime().disconnect();

    std::printf(failures == 0 ? "ok\n" : "%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
