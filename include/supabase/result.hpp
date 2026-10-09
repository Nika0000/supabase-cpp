#pragma once

/// @file supabase/result.hpp
/// @brief Value-or-error results for synchronous SDK operations.

#include <supabase/error.hpp>

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace supabase
{

/**
 * @brief Owning value-or-Error result implemented with std::variant.
 * @tparam T Success value type; void uses the specialization below.
 *
 * Check ok() or the explicit boolean conversion before accessing either branch.
 * Accessors return borrowed references/pointers except valueOr(), which returns
 * an owned value. References remain valid only while their stored value exists;
 * replacing, moving from, or destroying the result can affect them.
 *
 * This is a small C++20 result type, not the full std::expected interface. A move
 * does not explicitly reset the source result's success/error classification.
 *
 * @code{.cpp}
 * supabase::Result<int> result = 42;
 * if (result)
 * {
 *     auto value = result.value();
 *     // Consume value.
 * }
 * else
 * {
 *     // Handle result.error().code and result.error().message.
 * }
 * @endcode
 */
template <typename T> class [[nodiscard]] Result
{
  public:
    /// @brief Construct a success result by moving the supplied value into storage.
    Result(T value) : m_data(std::in_place_index<0>, std::move(value)) {}
    /// @brief Construct a failure result by moving the supplied diagnostic into storage.
    Result(Error error) : m_data(std::in_place_index<1>, std::move(error)) {}

    /// @brief Test whether the success alternative is currently active.
    /// @return true for a stored T; false for an Error or a valueless variant.
    [[nodiscard]] bool ok() const noexcept { return m_data.index() == 0; }
    /// @brief Explicit success test, suitable for if statements and boolean conditions.
    explicit operator bool() const noexcept { return ok(); }

    /// @brief Borrow mutable success data from an lvalue result.
    /// @pre ok() is true; accessing another alternative throws std::bad_variant_access.
    [[nodiscard]] T& value() & { return std::get<0>(m_data); }
    /// @brief Borrow immutable success data from a const lvalue result.
    /// @pre ok() is true; accessing another alternative throws std::bad_variant_access.
    [[nodiscard]] const T& value() const& { return std::get<0>(m_data); }
    /// @brief Obtain an rvalue reference for moving success data out of the result.
    /// @pre ok() is true. The caller must move/copy the value before its owner is destroyed.
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(m_data)); }
    /// @brief Borrow the stored failure diagnostic.
    /// @pre The Error alternative is active; success or a valueless variant throws std::bad_variant_access.
    [[nodiscard]] const Error& error() const { return std::get<1>(m_data); }

    /// @brief Access mutable success data through pointer syntax.
    /// @pre ok() is true; otherwise std::bad_variant_access is thrown.
    T* operator->() { return &std::get<0>(m_data); }
    /// @brief Access immutable success data through pointer syntax.
    /// @pre ok() is true; otherwise std::bad_variant_access is thrown.
    const T* operator->() const { return &std::get<0>(m_data); }
    /// @brief Dereference mutable success data without moving it.
    /// @pre ok() is true.
    T& operator*() & { return value(); }
    /// @brief Dereference immutable success data without moving it.
    /// @pre ok() is true.
    const T& operator*() const& { return value(); }

    /// @brief Return a copy of success data or move the supplied fallback into the return value.
    /// @param fallback Value taken by value even when the result is successful.
    /// @return Owned success copy or fallback; the result is unchanged.
    /// @note Requires copying T from const storage; it is not a lazy fallback operation.
    [[nodiscard]] T valueOr(T fallback) const& { return ok() ? value() : std::move(fallback); }

    /// @brief Transform const success data while preserving a failure diagnostic.
    /// @tparam F Callable accepting const T& and returning a non-void value suitable for Result storage.
    /// @param fn Invoked once on success; not invoked on failure.
    /// @return Result of the callable's return type, or a copy of the original Error.
    /// @note Exceptions from the callable propagate; this method does not map them to Error.
    /// A void-returning callable is unsupported by this implementation.
    /// @code{.cpp}
    /// auto doubled = supabase::Result<int>(21).map([](const int& value) { return value * 2; });
    /// @endcode
    template <typename F> [[nodiscard]] auto map(F&& fn) const& -> Result<std::invoke_result_t<F, const T&>>
    {
        if (!ok())
            return error();
        return std::forward<F>(fn)(value());
    }

  private:
    std::variant<T, Error> m_data;
};

/// @brief Success-or-Error result for operations without a success payload.
/// Default construction means success. Test ok() before error(): unlike Result<T>,
/// error() dereferences optional storage without checking, so calling it on success
/// has undefined behavior. The specialization provides no value(), valueOr(), or map().
template <> class [[nodiscard]] Result<void>
{
  public:
    /// @brief Construct a successful completion result.
    Result() = default;
    /// @brief Construct a failed completion result with an owned diagnostic.
    Result(Error error) : m_error(std::move(error)) {}

    /// @brief Test whether no error is stored.
    [[nodiscard]] bool ok() const noexcept { return !m_error.has_value(); }
    /// @brief Explicit success test for boolean conditions.
    explicit operator bool() const noexcept { return ok(); }
    /// @brief Borrow the stored failure diagnostic.
    /// @pre ok() is false; accessing an empty optional is undefined behavior.
    [[nodiscard]] const Error& error() const { return *m_error; }

  private:
    std::optional<Error> m_error;
};

}
