#include <supabase/context.hpp>

namespace supabase
{

http::Headers Context::baseHeaders() const
{
    http::Headers out;
    out.reserve(headers.size() + 2);
    out.emplace_back("apikey", anonKey);
    out.emplace_back("Authorization", "Bearer " + token());
    out.insert(out.end(), headers.begin(), headers.end());
    return out;
}

}
