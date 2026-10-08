#include <cctype>
#include <charconv>

#include <supabase/postgrest/builder.hpp>

#include <algorithm>
#include <system_error>

namespace supabase::postgrest
{

namespace
{
    constexpr std::string_view kHex = "0123456789ABCDEF";

    bool needsQuoting(std::string_view text) { return text.find_first_of(",()\"\\ :") != std::string_view::npos; }

    std::string quote(std::string_view text)
    {
        std::string out = "\"";
        for (const char c : text)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        out += '"';
        return out;
    }

    /// Scalar -> filter text. Strings are raw, null/bool/number use their JSON spelling.
    std::string scalar(const Json& value) { return value.is_string() ? value.get<std::string>() : value.dump(); }

    /// Element inside `in.(...)` or a `{...}` pg array literal.
    std::string element(const Json& value)
    {
        const std::string text = scalar(value);
        return value.is_string() && needsQuoting(text) ? quote(text) : text;
    }

    std::string join(const std::vector<Json>& values)
    {
        std::string out;
        for (const auto& v : values)
        {
            if (!out.empty())
                out += ',';
            out += element(v);
        }
        return out;
    }

    /// `cs`/`cd`/`ov` operands: arrays become pg array literals, objects stay JSON (jsonb).
    std::string collection(const Json& value)
    {
        if (value.is_array())
            return "{" + join(value.get<std::vector<Json>>()) + "}";
        return scalar(value);
    }

    std::string_view countToken(CountMode mode)
    {
        switch (mode)
        {
            case CountMode::Exact:
                return "count=exact";
            case CountMode::Planned:
                return "count=planned";
            case CountMode::Estimated:
                return "count=estimated";
            case CountMode::None:
                break;
        }
        return {};
    }

    /// Drop whitespace outside double quotes, as supabase-js does for `select`.
    std::string compactColumns(std::string_view columns)
    {
        std::string out;
        bool quoted = false;
        for (const char c : columns)
        {
            if (c == '"')
                quoted = !quoted;
            if (quoted || !std::isspace(static_cast<unsigned char>(c)))
                out += c;
        }
        return out;
    }

    bool isWrite(http::Method method) { return method != http::Method::Get && method != http::Method::Head; }

    std::optional<std::int64_t> parseTotal(std::string_view contentRange)
    {
        const auto slash = contentRange.find('/');
        if (slash == std::string_view::npos)
            return std::nullopt;
        const auto total     = contentRange.substr(slash + 1);
        std::int64_t value   = 0;
        const auto [ptr, ec] = std::from_chars(total.data(), total.data() + total.size(), value);
        if (ec != std::errc() || ptr != total.data() + total.size())
            return std::nullopt;
        return value;
    }

    Error httpError(const http::Response& response)
    {
        Error error     = makeError(errc::Http, response.body, response.status);
        const Json body = Json::parse(response.body, nullptr, false);
        if (!body.is_object())
            return error;
        const auto text
            = [&](const char* key) { return body.contains(key) && body[key].is_string() ? body[key].get<std::string>() : std::string(); };
        if (auto code = text("code"); !code.empty())
            error.code = std::move(code);
        if (auto message = text("message"); !message.empty())
            error.message = std::move(message);
        error.details = text("details");
        error.hint    = text("hint");
        return error;
    }
}

std::string urlEncode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += c;
            continue;
        }
        out += '%';
        out += kHex[byte >> 4];
        out += kHex[byte & 0x0F];
    }
    return out;
}

FilterBuilder::FilterBuilder(std::shared_ptr<const Context> ctx, std::string path, http::Method method)
    : ctx_(std::move(ctx)), path_(std::move(path)), method_(method)
{
}

FilterBuilder& FilterBuilder::addFilter(std::string_view column, std::string_view op, std::string_view value)
{
    params_.emplace_back(std::string(column), std::string(op) + "." + std::string(value));
    return *this;
}

FilterBuilder& FilterBuilder::addParam(std::string name, std::string value)
{
    params_.emplace_back(std::move(name), std::move(value));
    return *this;
}

FilterBuilder& FilterBuilder::setBody(Json body)
{
    body_ = std::move(body);
    return *this;
}

FilterBuilder& FilterBuilder::setPrefer(std::string_view token)
{
    prefer_.emplace_back(token);
    return *this;
}

FilterBuilder& FilterBuilder::eq(std::string_view c, const Json& v) { return addFilter(c, "eq", scalar(v)); }
FilterBuilder& FilterBuilder::neq(std::string_view c, const Json& v) { return addFilter(c, "neq", scalar(v)); }
FilterBuilder& FilterBuilder::gt(std::string_view c, const Json& v) { return addFilter(c, "gt", scalar(v)); }
FilterBuilder& FilterBuilder::gte(std::string_view c, const Json& v) { return addFilter(c, "gte", scalar(v)); }
FilterBuilder& FilterBuilder::lt(std::string_view c, const Json& v) { return addFilter(c, "lt", scalar(v)); }
FilterBuilder& FilterBuilder::lte(std::string_view c, const Json& v) { return addFilter(c, "lte", scalar(v)); }
FilterBuilder& FilterBuilder::like(std::string_view c, std::string_view p) { return addFilter(c, "like", p); }
FilterBuilder& FilterBuilder::ilike(std::string_view c, std::string_view p) { return addFilter(c, "ilike", p); }
FilterBuilder& FilterBuilder::is(std::string_view c, const Json& v) { return addFilter(c, "is", scalar(v)); }
FilterBuilder& FilterBuilder::in(std::string_view c, const std::vector<Json>& v) { return addFilter(c, "in", "(" + join(v) + ")"); }
FilterBuilder& FilterBuilder::contains(std::string_view c, const Json& v) { return addFilter(c, "cs", collection(v)); }
FilterBuilder& FilterBuilder::containedBy(std::string_view c, const Json& v) { return addFilter(c, "cd", collection(v)); }
FilterBuilder& FilterBuilder::overlaps(std::string_view c, const Json& v) { return addFilter(c, "ov", collection(v)); }

FilterBuilder& FilterBuilder::textSearch(std::string_view column, std::string_view query, std::string_view config)
{
    const std::string op = config.empty() ? "fts" : "fts(" + std::string(config) + ")";
    return addFilter(column, op, query);
}

FilterBuilder& FilterBuilder::match(const Json& columns)
{
    for (const auto& [key, value] : columns.items())
        eq(key, value);
    return *this;
}

FilterBuilder& FilterBuilder::not_(std::string_view column, std::string_view op, const Json& value)
{
    return addFilter(column, "not", std::string(op) + "." + scalar(value));
}

FilterBuilder& FilterBuilder::or_(std::string_view filters) { return addParam("or", "(" + std::string(filters) + ")"); }

FilterBuilder& FilterBuilder::filter(std::string_view column, std::string_view op, const Json& value)
{
    return addFilter(column, op, scalar(value));
}

FilterBuilder& FilterBuilder::select(std::string_view columns)
{
    select_ = compactColumns(columns);
    if (isWrite(method_))
        prefer_.emplace_back("return=representation");
    return *this;
}

FilterBuilder& FilterBuilder::order(std::string_view column, OrderOptions options)
{
    std::string term = std::string(column) + (options.ascending ? ".asc" : ".desc");
    if (options.nullsFirst)
        term += *options.nullsFirst ? ".nullsfirst" : ".nullslast";

    const auto it = std::find_if(params_.begin(), params_.end(), [](const auto& p) { return p.first == "order"; });
    if (it == params_.end())
        params_.emplace_back("order", std::move(term));
    else
        it->second += "," + term;
    return *this;
}

FilterBuilder& FilterBuilder::limit(std::int64_t count) { return addParam("limit", std::to_string(count)); }

FilterBuilder& FilterBuilder::range(std::int64_t from, std::int64_t to)
{
    range_ = std::pair { from, to };
    return *this;
}

FilterBuilder& FilterBuilder::single()
{
    single_      = true;
    maybeSingle_ = false;
    return *this;
}

FilterBuilder& FilterBuilder::maybeSingle()
{
    maybeSingle_ = true;
    single_      = false;
    return *this;
}

FilterBuilder& FilterBuilder::count(CountMode mode)
{
    count_ = mode;
    return *this;
}

http::Request FilterBuilder::buildRequest() const
{
    http::Request request;
    request.method  = method_;
    request.timeout = ctx_->timeout;

    std::string query;
    const auto append = [&query](std::string_view name, std::string_view value)
    {
        query += query.empty() ? "" : "&";
        query += urlEncode(name) + "=" + urlEncode(value);
    };
    if (!select_.empty())
        append("select", select_);
    for (const auto& [name, value] : params_)
        append(name, value);
    if (range_)
    {
        append("offset", std::to_string(range_->first));
        append("limit", std::to_string(range_->second - range_->first + 1));
    }
    request.url = ctx_->endpoint(path_) + (query.empty() ? "" : "?" + query);

    request.headers = ctx_->baseHeaders();
    request.headers.emplace_back(isWrite(method_) ? "Content-Profile" : "Accept-Profile", ctx_->schema);
    request.headers.emplace_back("Accept", single_ ? "application/vnd.pgrst.object+json" : "application/json");

    std::string prefer;
    const auto addPrefer = [&prefer](std::string_view token)
    {
        if (token.empty())
            return;
        prefer += prefer.empty() ? "" : ",";
        prefer += token;
    };
    for (const auto& token : prefer_)
        addPrefer(token);
    addPrefer(countToken(count_));
    if (!prefer.empty())
        request.headers.emplace_back("Prefer", std::move(prefer));

    if (body_)
    {
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = body_->dump();
    }
    return request;
}

Result<Response<Json>> FilterBuilder::execute() const
{
    auto sent = ctx_->transport->send(buildRequest());
    if (!sent)
        return sent.error();
    const http::Response& response = sent.value();
    if (!response.ok())
        return httpError(response);

    Response<Json> out;
    out.status = response.status;
    out.count  = parseTotal(response.header("Content-Range"));
    if (!response.body.empty())
    {
        out.data = Json::parse(response.body, nullptr, false);
        if (out.data.is_discarded())
            return makeError(errc::Parse, "invalid JSON in response", response.status);
    }

    if (maybeSingle_ && out.data.is_array())
    {
        if (out.data.size() > 1)
        {
            Error error   = makeError("PGRST116", "multiple rows returned for maybeSingle()", response.status);
            error.details = "Results contain " + std::to_string(out.data.size()) + " rows";
            return error;
        }
        out.data = out.data.empty() ? Json(nullptr) : out.data.front();
    }
    return out;
}

QueryBuilder::QueryBuilder(std::shared_ptr<const Context> ctx, std::string table) : ctx_(std::move(ctx)), table_(std::move(table)) {}

FilterBuilder QueryBuilder::select(std::string_view columns, CountMode count) const
{
    FilterBuilder builder(ctx_, "/rest/v1/" + table_, http::Method::Get);
    builder.select(columns).count(count);
    return builder;
}

FilterBuilder QueryBuilder::insert(const Json& values, WriteOptions options) const
{
    FilterBuilder builder(ctx_, "/rest/v1/" + table_, http::Method::Post);
    builder.setBody(values).count(options.count);
    return builder;
}

FilterBuilder QueryBuilder::upsert(const Json& values, UpsertOptions options) const
{
    FilterBuilder builder(ctx_, "/rest/v1/" + table_, http::Method::Post);
    builder.setBody(values).count(options.count);
    builder.setPrefer(options.ignoreDuplicates ? "resolution=ignore-duplicates" : "resolution=merge-duplicates");
    if (!options.onConflict.empty())
        builder.addParam("on_conflict", options.onConflict);
    return builder;
}

FilterBuilder QueryBuilder::update(const Json& values, WriteOptions options) const
{
    FilterBuilder builder(ctx_, "/rest/v1/" + table_, http::Method::Patch);
    builder.setBody(values).count(options.count);
    return builder;
}

FilterBuilder QueryBuilder::remove(WriteOptions options) const
{
    FilterBuilder builder(ctx_, "/rest/v1/" + table_, http::Method::Delete);
    builder.count(options.count);
    return builder;
}

FilterBuilder makeRpc(std::shared_ptr<const Context> ctx, std::string_view fn, const Json& args, RpcOptions options)
{
    const auto method = options.head ? http::Method::Head : options.get ? http::Method::Get : http::Method::Post;
    FilterBuilder builder(std::move(ctx), "/rest/v1/rpc/" + std::string(fn), method);
    builder.count(options.count);
    if (method == http::Method::Post)
    {
        builder.setBody(args.is_null() ? Json::object() : args);
    }
    else if (args.is_object())
    {
        for (const auto& [key, value] : args.items())
            builder.addParam(key, value.is_array() ? collection(value) : scalar(value));
    }
    return builder;
}

}
