#pragma once

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

/// PostgREST entry points bound to one schema. Shares the owning client's auth state,
/// transport and token refresh (falls back to the anon key once that client is gone).
class SchemaClient
{
  public:
    explicit SchemaClient(std::shared_ptr<const Context> ctx) : m_ctx(std::move(ctx)) {}

    [[nodiscard]] postgrest::QueryBuilder from(std::string_view table) const;
    [[nodiscard]] postgrest::FilterBuilder
    rpc(std::string_view fn, const Json& args = Json::object(), postgrest::RpcOptions options = {}) const;

  private:
    std::shared_ptr<const Context> m_ctx;
};

/// Supabase client. Mirrors supabase-js `SupabaseClient`. Cheap to move; sub-clients share state.
class Client
{
  public:
    Client(std::string url, std::string anonKey, ClientOptions options = {});

    /// supabase-js `client.schema(name)`: `from()`/`rpc()` against another PostgREST schema
    /// (`Accept-Profile`/`Content-Profile`) using the same auth session.
    [[nodiscard]] SchemaClient schema(std::string_view name) const;

    /// `client.from("games").select("*").eq("id", 1).execute()`
    [[nodiscard]] postgrest::QueryBuilder from(std::string_view table) const;
    /// `client.rpc("fn", {{"arg", 1}}).execute()`
    [[nodiscard]] postgrest::FilterBuilder
    rpc(std::string_view fn, const Json& args = Json::object(), postgrest::RpcOptions options = {}) const;

    [[nodiscard]] auth::AuthClient& auth() { return *m_auth; }
    [[nodiscard]] const functions::FunctionsClient& functions() const { return m_functions; }
    [[nodiscard]] const storage::StorageClient& storage() const { return m_storage; }
    [[nodiscard]] const realtime::RealtimeClient& realtime() const { return m_realtime; }
    [[nodiscard]] const graphql::GraphQLClient& graphql() const { return m_graphql; }

  private:
    std::shared_ptr<Context> m_ctx;
    std::unique_ptr<auth::AuthClient> m_auth;
    functions::FunctionsClient m_functions;
    storage::StorageClient m_storage;
    realtime::RealtimeClient m_realtime;
    graphql::GraphQLClient m_graphql;
};

/// supabase-js style factory.
[[nodiscard]] inline Client createClient(std::string url, std::string anonKey, ClientOptions options = {})
{
    return { std::move(url), std::move(anonKey), std::move(options) };
}

}
