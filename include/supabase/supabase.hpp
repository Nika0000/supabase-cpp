#pragma once

/**
 * @file supabase/supabase.hpp
 * @brief Public umbrella header for the native Supabase C++ client.
 *
 * Include this header to access Client, configuration, authentication, PostgREST,
 * Storage, Functions, Realtime, and the shared JSON/result/transport types.
 * The GraphQL entry point is also exposed, but its current execution methods
 * report ErrorCode::NotImplemented.
 *
 * Construct a client with your project URL and anon/publishable API key.
 * Construction is local; database builders send requests when execute() is
 * called. Inspect Result before accessing its value. Service methods document
 * their own response types, authentication requirements, and execution model.
 *
 * @code{.cpp}
 * #include <supabase/supabase.hpp>
 *
 * void loadGames()
 * {
 *     auto client = supabase::createClient("https://your-project.supabase.co", "your-publishable-key");
 *     auto result = client.from("games").select("id, name").eq("published", true).execute();
 *     if (!result)
 *     {
 *         // Handle result.error().
 *         return;
 *     }
 *     // Consume result.value() using the PostgREST response API.
 * }
 * @endcode
 *
 * @see Client for service ownership and lifetime rules.
 * @see ClientOptions for custom transports, session storage, and timeouts.
 */
#include <supabase/client.hpp>
