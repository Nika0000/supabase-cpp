# supabase-cpp

A C++20 client for Supabase with an API shaped like `supabase-js`. It uses libcurl for HTTP and WebSocket connections and `nlohmann::json` for JSON values.

## What works

| Area | Status |
| --- | --- |
| Database | PostgREST queries, filters, mutations, typed results, and RPC |
| Auth | Sign-in, sign-up, OAuth URL and PKCE flows, session refresh, MFA, and admin operations |
| Edge Functions | Invoke functions and stream responses |
| Realtime | Postgres changes, broadcast, presence, reconnect, and channel rejoin |
| Storage and GraphQL | Public API stubs; calls return `NotImplemented` |

Realtime uses libcurl's WebSocket API and requires libcurl 7.86 or newer. With an older libcurl, WebSocket connection attempts return `NotImplemented`.

## Use in a CMake project

The library requires a C++20 compiler, libcurl, and `nlohmann_json`. Make those dependencies available to CMake, then add the library as a subdirectory:

```cmake
add_subdirectory(extern/supabase-cpp)
target_link_libraries(your_app PRIVATE supabase::supabase)
```

For a standalone build, use CMake from this repository's root. Tests and the example are optional and disabled by default:

```sh
cmake -S . -B build -DSUPABASE_BUILD_TESTS=ON -DSUPABASE_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The complete example is in [`examples/basic.cpp`](examples/basic.cpp).

## Quick start

Use your Supabase project URL and anon key. Each operation returns `supabase::Result<T>`; check it before accessing `value()`.

```cpp
#include <cstdio>
#include <supabase/supabase.hpp>

int main()
{
    auto client = supabase::createClient("https://your-project.supabase.co", "your-anon-key");
    auto rows = client.from("games")
                    .select("id, name")
                    .eq("published", true)
                    .order("name")
                    .limit(10)
                    .execute();

    if (!rows)
    {
        std::fprintf(stderr, "query failed: %s\n", rows.error().message.c_str());
        return 1;
    }

    std::printf("%s\n", rows.value().data.dump(2).c_str());
}
```

Other entry points use the same client:

```cpp
auto signedIn = client.auth().signInWithPassword({ .email = "me@example.com", .password = "secret" });
auto sum = client.rpc("add", { { "a", 2 }, { "b", 3 } }).execute<int>();
auto reply = client.functions().invoke("hello", { .json = supabase::Json { { "name", "world" } } });
auto room = client.realtime().channel("games");
```

Realtime event callbacks run on a worker thread. Register them before calling `room->subscribe()` and keep the channel alive while you need events.

## Configuration

Pass `supabase::ClientOptions` as the third argument to `createClient` when you need custom behavior:

- `transport`: inject an `http::Transport` implementation, including a mock for tests.
- `socketFactory`: inject the WebSocket implementation used by Realtime.
- `curl.caBundle` or `curl.caBundleBlob`: provide a CA bundle when the platform has no suitable system store.
- `storage`: persist sessions through an `auth::SessionStorage` implementation; the default is in-memory storage.
- `autoRefreshToken`: control automatic refresh before authenticated requests.

The public API returns `Result<T>` or `Result<void>` for fallible operations. It does not throw exceptions to report request failures.

## API notes

| `supabase-js` | `supabase-cpp` |
| --- | --- |
| `from('t').select('*').eq('a', 1)` | `from("t").select("*").eq("a", 1).execute()` |
| `.delete()` | `.remove()` |
| `.not('a', 'eq', 1)` / `.or('...')` | `.not_("a", "eq", 1)` / `.or_("...")` |
| `.single()` / `.maybeSingle()` | Same names |
| `rpc('fn', args)` | `rpc("fn", args).execute()` |
| `auth.signInWithOAuth()` | `auth().getOAuthSignInUrl()`; the caller opens the URL |
| `auth.onAuthStateChange()` | `auth().onAuthStateChange()`; returns an RAII subscription |
| `functions.invoke()` | `functions().invoke()` or `invokeStream()` |