#include <cctype>

#include <supabase/http.hpp>

#include <algorithm>

namespace supabase::http
{

std::string_view toString(Method method) noexcept
{
    switch (method)
    {
        case Method::Get:
            return "GET";
        case Method::Post:
            return "POST";
        case Method::Put:
            return "PUT";
        case Method::Patch:
            return "PATCH";
        case Method::Delete:
            return "DELETE";
        case Method::Head:
            return "HEAD";
    }
    return "GET";
}

std::string Response::header(std::string_view name) const
{
    const auto equalsIgnoreCase = [name](const std::string& other)
    {
        return std::equal(
            name.begin(),
            name.end(),
            other.begin(),
            other.end(),
            [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }
        );
    };
    for (const auto& [key, value] : headers)
    {
        if (equalsIgnoreCase(key))
            return value;
    }
    return {};
}

}
