#include <cstdio>

#include <supabase/supabase.hpp>

int main()
{
    auto client = supabase::createClient("https://your-project.supabase.co", "your-anon-key");

    auto signedIn = client.auth().signInWithPassword({ .email = "me@example.com", .password = "secret" });
    if (!signedIn)
    {
        std::printf("sign-in failed: %s\n", signedIn.error().message.c_str());
        return 1;
    }

    auto games = client.from("games").select("id, name").eq("published", true).order("name").limit(10).execute();
    if (games)
        std::printf("%s\n", games.value().data.dump(2).c_str());

    auto reply = client.functions().invoke("hello", { .json = supabase::Json { { "name", "world" } } });
    if (reply)
        std::printf("%s\n", reply.value().body.c_str());

    auto room = client.realtime().channel("games");
    room->onPostgresChanges({ .event = "INSERT", .table = "games" },
        [](const supabase::Json& change) { std::printf("new row: %s\n", change["new"].dump().c_str()); });
    if (auto subscribed = room->subscribe(); !subscribed)
        std::printf("realtime failed: %s\n", subscribed.error().message.c_str());
    // Events arrive on a worker thread; keep the process alive as long as you need them.
    return 0;
}
