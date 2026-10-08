#pragma once

#include <supabase/error.hpp>

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace supabase
{

/// Value-or-Error, a minimal std::expected stand-in that builds on every supported toolchain.
template <typename T> class [[nodiscard]] Result
{
  public:
    Result(T value) : m_data(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : m_data(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return m_data.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<0>(m_data); }
    [[nodiscard]] const T& value() const& { return std::get<0>(m_data); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(m_data)); }
    [[nodiscard]] const Error& error() const { return std::get<1>(m_data); }

    T* operator->() { return &std::get<0>(m_data); }
    const T* operator->() const { return &std::get<0>(m_data); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    [[nodiscard]] T valueOr(T fallback) const& { return ok() ? value() : std::move(fallback); }

    /// Transform the value, propagating the error unchanged.
    template <typename F> [[nodiscard]] auto map(F&& fn) const& -> Result<std::invoke_result_t<F, const T&>>
    {
        if (!ok())
            return error();
        return std::forward<F>(fn)(value());
    }

  private:
    std::variant<T, Error> m_data;
};

template <> class [[nodiscard]] Result<void>
{
  public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return !m_error.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const { return *m_error; }

  private:
    std::optional<Error> m_error;
};

}
