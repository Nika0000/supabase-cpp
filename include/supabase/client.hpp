#pragma once

/// @file supabase/client.hpp
/// @brief Project client, service accessors, and schema-scoped database entry points.

#include <supabase/auth/auth.hpp>
#include <supabase/config.hpp>
#include <supabase/functions/functions.hpp>
#include <supabase/graphql/graphql.hpp>
#include <supabase/postgrest/builder.hpp>
#include <supabase/realtime/realtime.hpp>
#include <supabase/storage/storage.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace supabase
{

/// @brief PostgREST entry points bound to one exposed database schema.
/// Retains a shared context and transport, using the original auth implementation's
/// bearer provider. After that implementation is destroyed, authentication falls
/// back to the project API key. Copies retain the same scoped context.
class SchemaClient
{
  public:
    /// @brief Retain a schema-specific request context without making a request.
    /// @param ctx Non-null context, normally supplied by Client::schema().
    explicit SchemaClient(std::shared_ptr<const Context> ctx) : m_ctx(std::move(ctx)) {}

    /// @brief Start a table or view operation in this entry point's schema.
    /// @param table Relation name appended verbatim to the PostgREST path.
    /// @return QueryBuilder retaining the scoped context; execution is deferred.
    [[nodiscard]] postgrest::QueryBuilder from(std::string_view table) const;

    /// @brief Start a database function call in this entry point's schema.
    /// @param fn Function name forwarded without local validation.
    /// @param args JSON arguments; POST sends JSON, GET/HEAD use object entries as query parameters.
    /// @param options Method and count preference.
    /// @return FilterBuilder; call execute() to perform the request.
    [[nodiscard]] postgrest::FilterBuilder
    rpc(std::string_view fn, const Json& args = Json::object(), postgrest::RpcOptions options = {}) const;

  private:
    std::shared_ptr<const Context> m_ctx;
};

/**
 * @brief Project client owning authentication and the native service entry points.
 *
 * Move-only: services share a request context and transport, while the client owns
 * one AuthClient. Returned service references borrow their objects; do not use
 * them after destruction or replacement of the referenced object. Database
 * builders and schema entry points retain their contexts independently.
 * After moving a client, obtain service references from the destination client.
 *
 * Construction does not send a request or start Realtime. Database queries execute
 * explicitly; Auth, Storage, and Functions request methods are synchronous, while
 * Realtime starts its worker on connect() or channel subscription.
 *
 * @code{.cpp}
 * auto client = supabase::createClient("https://your-project.supabase.co", "your-publishable-key");
 * auto result = client.from("games").select("id, name").eq("published", true).execute();
 * if (!result)
 * {
 *     // Handle result.error() before accessing response data.
 * }
 * @endcode
 * Include <supabase/supabase.hpp>; examples are intended for use inside a function.
 */
class Client
{
  public:
    /// @brief Configure a project and construct its shared service context.
    /// @param url Project base URL; trailing slashes are removed without further validation.
    /// @param anonKey Project API key; client applications use an anon/publishable key.
    /// @param options Schema, transports, storage, refresh, and timeout settings, taken by value.
    /// @note Credentials and server permissions are checked when requests are executed.
    Client(std::string url, std::string anonKey, ClientOptions options = {});

    /// @brief Select another database schema without changing this client's default.
    /// @param name Exposed schema name sent in Accept-Profile or Content-Profile headers.
    /// @return Entry point sharing transport and bearer resolution with this client.
    /// @code{.cpp}
    /// auto result = client.schema("app").from("games").select().execute();
    /// @endcode
    [[nodiscard]] SchemaClient schema(std::string_view name) const;

    /// @brief Start an operation on a table or view in the default schema.
    /// @param table Relation name appended verbatim to the PostgREST path.
    /// @return QueryBuilder supporting reads and writes; no request is sent here.

    [[nodiscard]] postgrest::QueryBuilder from(std::string_view table) const;
    /// @brief Start a database function call in the default schema.
    /// @param fn Function name appended verbatim to the RPC path.
    /// @param args JSON arguments serialized according to the selected request method.
    /// @param options Method selection and count preference; defaults to POST.
    /// @return FilterBuilder supporting further modifiers and explicit execution.
    [[nodiscard]] postgrest::FilterBuilder
    rpc(std::string_view fn, const Json& args = Json::object(), postgrest::RpcOptions options = {}) const;

    /// @brief Access the owned native authentication instance.
    /// @return Borrowed mutable reference for sign-in, sessions, account updates, and auth events.
    [[nodiscard]] auth::AuthClient& auth() { return *m_auth; }

    /// @brief Access synchronous Edge Function invocation and streaming.
    /// @return Borrowed service reference sharing project authentication and HTTP transport.
    [[nodiscard]] const functions::FunctionsClient& functions() const { return m_functions; }

    /// @brief Access bucket management and per-bucket object operations.
    /// @return Borrowed Storage service reference.
    [[nodiscard]] const storage::StorageClient& storage() const { return m_storage; }

    /// @brief Access WebSocket channels, broadcasts, presence, and database change subscriptions.
    /// @return Borrowed Realtime client reference; obtaining it does not start the worker.
    [[nodiscard]] const realtime::RealtimeClient& realtime() const { return m_realtime; }

    /// @brief Access the reserved GraphQL interface.
    /// @return Borrowed GraphQL client; query() currently returns errc::NotImplemented.
    [[nodiscard]] const graphql::GraphQLClient& graphql() const { return m_graphql; }

  private:
    std::shared_ptr<Context> m_ctx;
    std::unique_ptr<auth::AuthClient> m_auth;
    functions::FunctionsClient m_functions;
    storage::StorageClient m_storage;
    realtime::RealtimeClient m_realtime;
    graphql::GraphQLClient m_graphql;
};

/// @brief Construct a project client using the supabase-js-style factory name.
/// @param url Project base URL, normalized by Client construction.
/// @param anonKey Project API key; never distribute a service-role key in a client app.
/// @param options Construction settings transferred to the new client.
/// @return Move-only Client value; no request is sent by this factory.
[[nodiscard]] inline Client createClient(std::string url, std::string anonKey, ClientOptions options = {})
{
    return { std::move(url), std::move(anonKey), std::move(options) };
}

}
