#pragma once

/**
 * @file supabase/postgrest/builder.hpp
 * @brief Chainable PostgREST queries, writes, filters, and database function calls.
 */

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

/// @brief Server count strategy requested through the Prefer header.
/// Availability of a numeric total depends on the server's Content-Range response.
enum class CountMode
{
    None,     ///< Omit the count preference.
    Exact,    ///< Request an exact total with count=exact.
    Planned,  ///< Request a planner-based estimate with count=planned.
    Estimated ///< Request the server's estimated count strategy with count=estimated.
};

/// @brief Data, HTTP status, and an optional total from a successful PostgREST call.
/// @tparam T Json for execute(), or the conversion target supplied to execute<T>().
template <typename T> struct Response
{
    T data {};                         ///< Parsed or converted response data; JSON data is null for an empty body.
    int status = 0;                    ///< HTTP status; successful execution returns a 2xx status.
    std::optional<std::int64_t> count; ///< Numeric Content-Range total; unset when absent, unknown, or invalid.
};

/// @brief Sort direction and optional null placement for an order term.
struct OrderOptions
{
    bool ascending = true;          ///< Ascending when true, descending when false.
    std::optional<bool> nullsFirst; ///< true: nulls first; false: nulls last; unset: use the database default.
};

/// @brief Conflict resolution and count options for an upsert request.
struct UpsertOptions
{
    std::string onConflict;                  ///< Optional conflict column expression forwarded as on_conflict.
    bool ignoreDuplicates = false;           ///< Ignore conflicting rows when true; otherwise request merging.
    CountMode count       = CountMode::None; ///< Server count preference for the write.
};

/// @brief Count preference for insert, update, and delete operations.
struct WriteOptions
{
    CountMode count = CountMode::None; ///< Requested server count strategy.
};

/// @brief Method and count options for a PostgREST database function call.
/// head takes precedence over get when both are true. Otherwise the default is POST.
struct RpcOptions
{
    bool head       = false;           ///< Use HEAD to request response metadata without a body.
    bool get        = false;           ///< Use GET with arguments in query parameters for a read-only function.
    CountMode count = CountMode::None; ///< Requested server count strategy.
};

/**
 * @brief Mutable PostgREST filter and response-shaping builder.
 *
 * Obtain a builder from QueryBuilder operations or supabase::Client::rpc().
 * Filter and modifier methods change this object and return *this; saved
 * references borrow the builder and must not outlive it. Query construction is
 * local. execute() sends a new synchronous request on every call and does not
 * consume the builder.
 *
 * Column arguments accept PostgREST column or JSON-path expressions. Comparison
 * operands use raw string contents or the JSON spelling of non-string values.
 * Query names and values are URL-encoded when the request is built. Operators,
 * expressions, and pagination values are interpreted by the server; the builder
 * does not validate their database meaning.
 *
 * Requests share the owning client's transport, timeout, schema, and bearer
 * provider. Building or executing a request can refresh an expiring auth session.
 * Mutators act on the same object; use separate builder copies for independent
 * query chains or concurrent modification.
 *
 * @par Example: filter, sort, and fetch one page
 * @code{.cpp}
 * auto result = client.from("games")
 *                     .select("id, name", supabase::postgrest::CountMode::Exact)
 *                     .eq("published", true)
 *                     .order("name")
 *                     .range(0, 9)
 *                     .execute();
 * if (result)
 * {
 *     // Read result->data, result->status, and the optional result->count.
 * }
 * else
 * {
 *     // Handle result.error().code, .message, .details, and .hint.
 * }
 * @endcode
 * Examples assume an initialized supabase::Client named `client` and are
 * independent snippets intended for use inside a function. Include
 * <supabase/supabase.hpp> for the complete public API.
 */
class FilterBuilder
{
  public:
    /// @brief Names for PostgREST operation categories.
    enum class Kind
    {
        Select, ///< Read rows.
        Insert, ///< Insert rows.
        Update, ///< Update rows.
        Upsert, ///< Insert with conflict resolution.
        Delete, ///< Delete rows.
        Rpc     ///< Call a database function.
    };

    /// @brief Construct a builder for a relative API path and HTTP method.
    /// @param ctx Non-null shared context containing the request transport and project settings.
    /// @param path Endpoint path under the project URL; appended verbatim.
    /// @param method HTTP method used by execute().
    /// @note Usually created by QueryBuilder or makeRpc(); construction sends no request.
    FilterBuilder(std::shared_ptr<const Context> ctx, std::string path, http::Method method);

    /// @brief Match rows whose column equals the supplied value (eq).
    /// @param column Column or JSON-path expression to compare.
    /// @param value Comparison operand in the column's expected type.
    /// @return This builder with the filter appended.
    FilterBuilder& eq(std::string_view column, const Json& value);

    /// @brief Match rows whose column differs from the supplied value (neq).
    /// @param column Column or JSON-path expression to compare.
    /// @param value Comparison operand.
    /// @return This builder with the filter appended.
    FilterBuilder& neq(std::string_view column, const Json& value);

    /// @brief Match rows whose column is greater than the supplied value (gt).
    /// @param column Column or JSON-path expression to compare.
    /// @param value Lower comparison bound, excluded from the match.
    /// @return This builder with the filter appended.
    FilterBuilder& gt(std::string_view column, const Json& value);

    /// @brief Match rows whose column is greater than or equal to the supplied value (gte).
    /// @param column Column or JSON-path expression to compare.
    /// @param value Inclusive lower comparison bound.
    /// @return This builder with the filter appended.
    FilterBuilder& gte(std::string_view column, const Json& value);

    /// @brief Match rows whose column is less than the supplied value (lt).
    /// @param column Column or JSON-path expression to compare.
    /// @param value Upper comparison bound, excluded from the match.
    /// @return This builder with the filter appended.
    FilterBuilder& lt(std::string_view column, const Json& value);

    /// @brief Match rows whose column is less than or equal to the supplied value (lte).
    /// @param column Column or JSON-path expression to compare.
    /// @param value Inclusive upper comparison bound.
    /// @return This builder with the filter appended.
    FilterBuilder& lte(std::string_view column, const Json& value);

    /// @brief Apply a case-sensitive SQL LIKE pattern (like).
    /// @param column Text column to compare.
    /// @param pattern Pattern forwarded to PostgREST, such as "Space%".
    /// @return This builder with the filter appended.
    FilterBuilder& like(std::string_view column, std::string_view pattern);

    /// @brief Apply a case-insensitive SQL LIKE pattern (ilike).
    /// @param column Text column to compare.
    /// @param pattern Pattern forwarded to PostgREST, such as "space%".
    /// @return This builder with the filter appended.
    FilterBuilder& ilike(std::string_view column, std::string_view pattern);

    /// @brief Apply an IS filter for a null or boolean value.
    /// @param column Column or JSON-path expression to test.
    /// @param value JSON null, true, or false; validation is left to the server.
    /// @return This builder with the filter appended.
    FilterBuilder& is(std::string_view column, const Json& value);

    /// @brief Match rows whose column is a member of a supplied value list (in).
    /// @param column Column or JSON-path expression to compare.
    /// @param values Values serialized into a parenthesized PostgREST list.
    /// @return This builder with the filter appended.
    /// @note String elements containing reserved delimiters are quoted and escaped.
    FilterBuilder& in(std::string_view column, const std::vector<Json>& values);

    /// @brief Match rows whose column contains the supplied operand (cs).
    /// @param column Column to compare using the server's containment operator.
    /// @param value Array, JSON object, or raw string operand appropriate for the column.
    /// @return This builder with the filter appended.
    /// @note Arrays become PostgreSQL array literals; objects retain their JSON representation.
    FilterBuilder& contains(std::string_view column, const Json& value);

    /// @brief Match rows whose column is contained by the supplied operand (cd).
    /// @param column Column to compare using the server's containment operator.
    /// @param value Array, JSON object, or raw string operand appropriate for the column.
    /// @return This builder with the filter appended.
    /// @note Uses the same collection serialization as contains().
    FilterBuilder& containedBy(std::string_view column, const Json& value);

    /// @brief Match rows whose column overlaps the supplied operand (ov).
    /// @param column Column to compare using the server's overlap operator.
    /// @param value Collection operand; arrays become PostgreSQL array literals.
    /// @return This builder with the filter appended.
    FilterBuilder& overlaps(std::string_view column, const Json& value);

    /// @brief Apply the PostgREST full-text search operator fts.
    /// @param column Searchable column expression.
    /// @param query Full-text query expression interpreted by the server.
    /// @param config Optional search configuration, such as "english"; empty uses fts without a configuration.
    /// @return This builder with an fts or fts(config) filter appended.
    FilterBuilder& textSearch(std::string_view column, std::string_view query, std::string_view config = {});

    /// @brief Append an equality filter for every supplied object entry.
    /// @param columns Object mapping column expressions to equality operands.
    /// @return This builder with the filters appended; an empty object adds none.
    /// @note For example, {"published": true, "genre": "puzzle"} adds two eq filters.
    FilterBuilder& match(const Json& columns);

    /// @brief Append the negated form of a PostgREST operator.
    /// @param column Column or JSON-path expression to compare.
    /// @param op Operator without the not prefix, such as "eq" or "in".
    /// @param value Operand serialized as a scalar; use raw PostgREST syntax for collection operators.
    /// @return This builder with a not.op filter appended.
    /// @note For a negated membership test, use not_("id", "in", "(1,2)").
    FilterBuilder& not_(std::string_view column, std::string_view op, const Json& value);

    /// @brief Append a group of alternatives using PostgREST's OR syntax.
    /// @param filters Raw comma-separated filter expressions, without the outer parentheses.
    /// @return This builder with an or parameter containing the parenthesized expressions.
    /// @note For example, "status.eq.queued,status.eq.running". The expression is
    /// URL-encoded when built; its operator grammar is not rewritten or validated.
    FilterBuilder& or_(std::string_view filters);

    /// @brief Append a filter using an explicit PostgREST operator.
    /// @param column Column or JSON-path expression to compare.
    /// @param op Operator text forwarded to the server.
    /// @param value Scalar operand; string values can provide raw operator-specific syntax.
    /// @return This builder with an op.value filter appended.
    FilterBuilder& filter(std::string_view column, std::string_view op, const Json& value);

    /// @brief Set the returned column expression and request row data after a write.
    /// @param columns PostgREST select expression; defaults to all columns.
    /// @return This builder with the selected columns replaced.
    /// @note Whitespace outside quoted identifiers is removed. For methods other
    /// than GET or HEAD, this also appends Prefer: return=representation.
    FilterBuilder& select(std::string_view columns = "*");

    /// @brief Append a sort term to the request's order expression.
    /// @param column Column or expression to sort by.
    /// @param options Direction and optional null placement.
    /// @return This builder with the sort term appended.
    /// @note Repeated calls form a comma-separated order expression in call order.
    FilterBuilder& order(std::string_view column, OrderOptions options = {});

    /// @brief Append a requested maximum number of result rows.
    /// @param count Row limit forwarded to the server without local range validation.
    /// @return This builder with a limit parameter appended.
    /// @note Repeated calls append parameters; they do not replace a previous limit.
    FilterBuilder& limit(std::int64_t count);

    /// @brief Set an inclusive row window using offset and limit query parameters.
    /// @param from Zero-based first row offset.
    /// @param to Inclusive last row index; range(0, 9) requests ten rows.
    /// @return This builder with any previous range replaced.
    /// @note Ordering determines which rows fall in the window. Values are not
    /// checked locally, and separate limit() parameters remain in the request.
    FilterBuilder& range(std::int64_t from, std::int64_t to);

    /// @brief Request the server's singular JSON representation for exactly one row.
    /// @return This builder in singular-response mode.
    /// @note Sends Accept: application/vnd.pgrst.object+json. Cardinality errors
    /// are reported by the server. Replaces any earlier maybeSingle() selection.
    FilterBuilder& single();

    /// @brief Normalize an array response containing zero or one row into a single value.
    /// @return This builder in optional-single-response mode.
    /// @note Empty arrays become JSON null; one-element arrays become their element.
    /// Larger arrays produce error code "PGRST116" with the response's HTTP status.
    /// Non-array data is retained as received. Replaces any earlier single() selection.
    FilterBuilder& maybeSingle();

    /// @brief Set the server count preference for this request.
    /// @param mode Count strategy; None omits the preference generated by this method.
    /// @return This builder with any previous count mode replaced.
    /// @note Response::count is parsed from Content-Range when a numeric total is available.
    FilterBuilder& count(CountMode mode);

    /// @brief Execute the configured request synchronously and parse its JSON response.
    /// @return Parsed data, HTTP status, and optional total on HTTP 2xx; otherwise an Error.
    /// @note An empty successful body yields JSON null. Invalid JSON produces errc::Parse.
    /// Transport errors are propagated. Server error objects supply code, message,
    /// details, and hint when available; otherwise errc::Http and the response body are used.
    /// maybeSingle() normalization is applied after parsing. Each call sends another request.
    [[nodiscard]] Result<Response<Json>> execute() const;

    /// @brief Execute the request and convert its parsed JSON data to a C++ type.
    /// @tparam T Type supported by Json::get<T>(), including any user-provided from_json conversion.
    /// @return Converted data with the original status and count, or an execution/conversion error.
    /// @note JSON conversion exceptions become errc::Parse with the response status.
    /// Conversion must handle the response shape, including null when the body is empty
    /// or maybeSingle() returns no row. Non-JSON exceptions from user conversions propagate.
    /// @code{.cpp}
    /// auto result = client.from("games").select("id, name").execute<std::vector<supabase::Json>>();
    /// @endcode
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

    /// @brief Build the HTTP request for inspection or use with a custom transport.
    /// @return Request with the encoded URL, schema headers, preferences, auth headers, and JSON body.
    /// @note Does not send the PostgREST request, but resolving the bearer token can
    /// trigger an auth refresh request. Uses the shared context's timeout.
    /// Sending this request directly bypasses execute() parsing and cardinality handling.
    [[nodiscard]] http::Request buildRequest() const;

    /// @brief Replace the JSON body used by the request.
    /// @param body JSON value to serialize, including null if explicitly supplied.
    /// @return This builder with its previous body replaced.
    /// @note Low-level operation used by QueryBuilder and makeRpc(); generates Content-Type: application/json.
    FilterBuilder& setBody(Json body);

    /// @brief Append a token to the request's Prefer header.
    /// @param token Preference text, such as "return=representation".
    /// @return This builder with the preference appended.
    /// @note Tokens are joined with commas without conflict resolution or deduplication.
    FilterBuilder& setPrefer(std::string_view token);

    /// @brief Append an unencoded query name/value pair for request construction.
    /// @param name Parameter name to URL-encode when the request is built.
    /// @param value Parameter value to URL-encode when the request is built.
    /// @return This builder with the parameter appended.
    /// @note Duplicate names are retained; no PostgREST operator prefix is added.
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

/**
 * @brief Entry point for operations on a PostgREST table or view.
 *
 * Obtain this object through supabase::Client::from() or SchemaClient::from().
 * Each operation creates an independent FilterBuilder retaining the shared
 * request context. No database request is sent until execute() is called on
 * that builder. The schema comes from the context; use Client::schema() to
 * obtain entry points for another exposed database schema.
 *
 * Writes do not request row representations by default. Chain
 * FilterBuilder::select() when the server should return affected rows.
 */
class QueryBuilder
{
  public:
    /// @brief Bind a table or view name to a shared request context.
    /// @param ctx Non-null shared context containing the schema and transport.
    /// @param table Relation name appended verbatim to /rest/v1/; not URL-encoded or validated here.
    QueryBuilder(std::shared_ptr<const Context> ctx, std::string table);

    /// @brief Start a GET request selecting rows from the relation.
    /// @param columns PostgREST column or embedding expression; defaults to all columns.
    /// @param count Optional server count strategy.
    /// @return New builder accepting filters, sorting, pagination, and response modifiers.
    [[nodiscard]] FilterBuilder select(std::string_view columns = "*", CountMode count = CountMode::None) const;

    /// @brief Start a POST request inserting rows into the relation.
    /// @param values JSON object for one row or array of objects for multiple rows, interpreted by the server.
    /// @param options Optional server count strategy.
    /// @return New write builder; chain select() to request inserted row data.
    /// @code{.cpp}
    /// auto result = client.from("games")
    ///                     .insert(supabase::Json { { "name", "New game" } })
    ///                     .select("id, name")
    ///                     .single()
    ///                     .execute();
    /// @endcode
    [[nodiscard]] FilterBuilder insert(const Json& values, WriteOptions options = {}) const;

    /// @brief Start an insert request with server-side conflict resolution.
    /// @param values JSON object or array of row objects to insert or merge.
    /// @param options Conflict column expression, duplicate policy, and optional count strategy.
    /// @return New builder with a resolution preference and any supplied on_conflict parameter.
    /// @note The default requests merge-duplicates; ignoreDuplicates requests ignore-duplicates.
    /// Conflict constraints and row validity are checked by the database.
    /// @code{.cpp}
    /// auto result = client.from("games")
    ///                     .upsert(supabase::Json { { "slug", "new-game" }, { "name", "New game" } }, {
    ///                         .onConflict = "slug",
    ///                     })
    ///                     .select()
    ///                     .execute();
    /// @endcode
    [[nodiscard]] FilterBuilder upsert(const Json& values, UpsertOptions options = {}) const;

    /// @brief Start a PATCH request applying field changes to matching rows.
    /// @param values JSON object containing the fields to update.
    /// @param options Optional server count strategy.
    /// @return New write builder whose filters determine the rows to update.
    /// @note No row filter is added automatically; chain filters to express the intended match.
    /// @code{.cpp}
    /// auto result = client.from("games")
    ///                     .update(supabase::Json { { "published", true } })
    ///                     .eq("id", 42)
    ///                     .select("id, published")
    ///                     .single()
    ///                     .execute();
    /// @endcode
    [[nodiscard]] FilterBuilder update(const Json& values, WriteOptions options = {}) const;

    /// @brief Start a DELETE request removing matching rows.
    /// @param options Optional server count strategy.
    /// @return New write builder whose filters determine the rows to delete.
    /// @note Corresponds to supabase-js delete(). No row filter is added automatically;
    /// chain filters to express the intended match and select() to request deleted row data.
    /// @code{.cpp}
    /// auto result = client.from("games").remove().eq("id", 42).execute();
    /// @endcode
    [[nodiscard]] FilterBuilder remove(WriteOptions options = {}) const;

  private:
    std::shared_ptr<const Context> m_ctx;
    std::string m_table;
};

/// @brief Construct a PostgREST database function call used by Client::rpc().
/// @param ctx Non-null shared context providing the schema, transport, and authentication.
/// @param fn Database function name appended verbatim to /rest/v1/rpc/.
/// @param args Function arguments; POST sends JSON, replacing null with an empty object.
/// GET/HEAD use object entries as query parameters; array values become PostgreSQL
/// array literals. Non-object args add no parameters for those methods.
/// @param options Select HEAD, GET, or default POST and an optional count strategy.
/// @return New builder accepting further filters and response modifiers; no request is sent here.
/// @note HEAD takes precedence over GET. Use Client::rpc() for the normal application entry point.
/// @code{.cpp}
/// auto result = client.rpc("search_games", supabase::Json { { "search_text", "space" } })
///                     .limit(10)
///                     .execute();
/// @endcode
[[nodiscard]] FilterBuilder makeRpc(std::shared_ptr<const Context> ctx, std::string_view fn, const Json& args, RpcOptions options);

/// @brief Percent-encode a value for use as a URL query name or component.
/// @param text Unencoded input bytes.
/// @return Encoded string, using uppercase hexadecimal escapes for encoded bytes.
/// @note Used by request construction; this function does not validate PostgREST
/// grammar or encode an entire URL as a structured address.
[[nodiscard]] std::string urlEncode(std::string_view text);

}
