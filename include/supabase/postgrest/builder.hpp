#pragma once

#include <cstdint>

#include <supabase/context.hpp>
#include <supabase/json.hpp>
#include <supabase/result.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace supabase::postgrest
{

enum class CountMode
{
    None,
    Exact,
    Planned,
    Estimated
};

/// Result of a PostgREST call. `T` is `Json` unless a typed `execute<T>()` was used.
template <typename T> struct Response
{
    T data {};
    int status = 0;
    std::optional<std::int64_t> count; ///< set when a count mode was requested
};

struct OrderOptions
{
    bool ascending = true;
    std::optional<bool> nullsFirst;
};

struct UpsertOptions
{
    std::string onConflict;
    bool ignoreDuplicates = false;
    CountMode count       = CountMode::None;
};

struct WriteOptions
{
    CountMode count = CountMode::None;
};

struct RpcOptions
{
    bool head       = false; ///< HEAD request, count only
    bool get        = false; ///< GET with args as query params (for read-only functions)
    CountMode count = CountMode::None;
};

/// Chainable filter/modifier builder. Mirrors supabase-js `PostgrestFilterBuilder`.
/// Methods mutate and return `*this`, so both `auto q = ...; q.eq(..)` and one-expression chains work.
class FilterBuilder
{
  public:
    enum class Kind
    {
        Select,
        Insert,
        Update,
        Upsert,
        Delete,
        Rpc
    };

    FilterBuilder(std::shared_ptr<const Context> ctx, std::string path, http::Method method);

    // Filters
    FilterBuilder& eq(std::string_view column, const Json& value);
    FilterBuilder& neq(std::string_view column, const Json& value);
    FilterBuilder& gt(std::string_view column, const Json& value);
    FilterBuilder& gte(std::string_view column, const Json& value);
    FilterBuilder& lt(std::string_view column, const Json& value);
    FilterBuilder& lte(std::string_view column, const Json& value);
    FilterBuilder& like(std::string_view column, std::string_view pattern);
    FilterBuilder& ilike(std::string_view column, std::string_view pattern);
    FilterBuilder& is(std::string_view column, const Json& value); ///< null / true / false
    FilterBuilder& in(std::string_view column, const std::vector<Json>& values);
    FilterBuilder& contains(std::string_view column, const Json& value);
    FilterBuilder& containedBy(std::string_view column, const Json& value);
    FilterBuilder& overlaps(std::string_view column, const Json& value);
    FilterBuilder& textSearch(std::string_view column, std::string_view query, std::string_view config = {});
    FilterBuilder& match(const Json& columns); ///< {"a":1,"b":2} -> a=eq.1&b=eq.2
    FilterBuilder& not_(std::string_view column, std::string_view op, const Json& value);
    FilterBuilder& or_(std::string_view filters); ///< "a.eq.1,b.eq.2"
    FilterBuilder& filter(std::string_view column, std::string_view op, const Json& value);

    // Modifiers
    FilterBuilder& select(std::string_view columns = "*"); ///< columns to return after a write
    FilterBuilder& order(std::string_view column, OrderOptions options = {});
    FilterBuilder& limit(std::int64_t count);
    FilterBuilder& range(std::int64_t from, std::int64_t to);
    FilterBuilder& single();      ///< exactly one row, else error
    FilterBuilder& maybeSingle(); ///< zero or one row, null data when empty
    FilterBuilder& count(CountMode mode);

    // Execution
    [[nodiscard]] Result<Response<Json>> execute() const;
    template <typename T> [[nodiscard]] Result<Response<T>> execute() const
    {
        auto raw = execute();
        if (!raw)
            return raw.error();
        try
        {
            return Response<T> { raw.value().data.template get<T>(), raw.value().status, raw.value().count };
        }
        catch (const Json::exception& e)
        {
            return makeError(errc::Parse, e.what(), raw.value().status);
        }
    }

    /// Request pieces, exposed for testing and custom transports.
    [[nodiscard]] http::Request buildRequest() const;

    // Used by QueryBuilder/Client
    FilterBuilder& setBody(Json body);
    FilterBuilder& setPrefer(std::string_view token);
    FilterBuilder& addParam(std::string name, std::string value);

  private:
    FilterBuilder& addFilter(std::string_view column, std::string_view op, std::string_view value);

    std::shared_ptr<const Context> m_ctx;
    std::string m_path;
    http::Method m_method;
    std::vector<std::pair<std::string, std::string>> m_params;
    std::vector<std::string> m_prefer;
    std::optional<Json> m_body;
    std::string m_select;
    std::optional<std::pair<std::int64_t, std::int64_t>> m_range;
    CountMode m_count  = CountMode::None;
    bool m_single      = false;
    bool m_maybeSingle = false;
};

/// Entry point returned by `Client::from(table)`.
class QueryBuilder
{
  public:
    QueryBuilder(std::shared_ptr<const Context> ctx, std::string table);

    [[nodiscard]] FilterBuilder select(std::string_view columns = "*", CountMode count = CountMode::None) const;
    [[nodiscard]] FilterBuilder insert(const Json& values, WriteOptions options = {}) const;
    [[nodiscard]] FilterBuilder upsert(const Json& values, UpsertOptions options = {}) const;
    [[nodiscard]] FilterBuilder update(const Json& values, WriteOptions options = {}) const;
    [[nodiscard]] FilterBuilder remove(WriteOptions options = {}) const; ///< supabase-js `delete`

  private:
    std::shared_ptr<const Context> m_ctx;
    std::string m_table;
};

/// Builder for `client.rpc(fn, args)`.
[[nodiscard]] FilterBuilder makeRpc(std::shared_ptr<const Context> ctx, std::string_view fn, const Json& args, RpcOptions options);

/// Percent-encode for URL query components.
[[nodiscard]] std::string urlEncode(std::string_view text);

}
