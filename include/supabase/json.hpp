#pragma once

/// @file supabase/json.hpp
/// @brief Native JSON value type used by Supabase requests and responses.

#include <nlohmann/json.hpp>

namespace supabase
{
/**
 * @brief Alias for nlohmann::json with its native parsing and conversion behavior.
 *
 * Stores objects, arrays, strings, numbers, booleans, and null. This alias adds no
 * validation or error mapping. Normal parse(), get<T>(), and checked access can
 * throw nlohmann JSON exceptions. SDK methods document where those exceptions
 * are converted into Result errors.
 *
 * For non-throwing parse-error detection, pass false as parse()'s allow_exceptions
 * argument and check is_discarded(). Valid JSON null is distinct from a discarded
 * value. Typed conversion uses native get<T>() and user-provided from_json functions.
 *
 * @code{.cpp}
 * auto payload = supabase::Json { { "name", "world" }, { "enabled", true } };
 * auto parsed = supabase::Json::parse("{\"id\":42}", nullptr, false);
 * if (!parsed.is_discarded())
 * {
 *     auto id = parsed.at("id").get<int>();
 *     // Consume id; checked access/conversion can still throw for incompatible data.
 * }
 * @endcode
 * The example is intended for use inside a function. Include <supabase/json.hpp>.
 */
using Json = nlohmann::json;
}
