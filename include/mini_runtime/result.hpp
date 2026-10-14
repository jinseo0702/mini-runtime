#pragma once

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace mini_runtime {

template <class T, class E>
class Result {
    static_assert(!std::is_reference_v<T> && !std::is_void_v<T>);
    static_assert(!std::is_reference_v<E> && !std::is_void_v<E>);
    static_assert(std::is_nothrow_move_constructible_v<T>);
    static_assert(std::is_nothrow_move_constructible_v<E>);

    std::variant<T, E> payload_;

    template <std::size_t I, class V>
    explicit Result(std::in_place_index_t<I> tag, V&& value)
        : payload_(tag, std::forward<V>(value)) {}

public:
    Result() = delete;
    Result(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) = delete;
    Result& operator=(Result&&) = delete;

    static Result ok(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }
    static Result err(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }
    bool has_value() const noexcept { return payload_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        if (!has_value()) throw std::logic_error("Result has no value");
        return std::get<0>(payload_);
    }
    const T& value() const & {
        if (!has_value()) throw std::logic_error("Result has no value");
        return std::get<0>(payload_);
    }
    E& error() & {
        if (has_value()) throw std::logic_error("Result has no error");
        return std::get<1>(payload_);
    }
    const E& error() const & {
        if (has_value()) throw std::logic_error("Result has no error");
        return std::get<1>(payload_);
    }
};

template <class E>
class Result<void, E> {
    static_assert(!std::is_reference_v<E> && !std::is_void_v<E>);
    static_assert(std::is_nothrow_move_constructible_v<E>);

    std::variant<std::monostate, E> payload_;

    explicit Result(std::in_place_index_t<0> tag) : payload_(tag) {}
    explicit Result(std::in_place_index_t<1> tag, E&& error)
        : payload_(tag, std::move(error)) {}

public:
    Result() = delete;
    Result(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(const Result&) = delete;
    Result& operator=(Result&&) = delete;

    static Result ok() { return Result(std::in_place_index<0>); }
    static Result err(E error) {
        return Result(std::in_place_index<1>, std::move(error));
    }
    bool has_value() const noexcept { return payload_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const {
        if (!has_value()) throw std::logic_error("Result has no value");
    }
    E& error() & {
        if (has_value()) throw std::logic_error("Result has no error");
        return std::get<1>(payload_);
    }
    const E& error() const & {
        if (has_value()) throw std::logic_error("Result has no error");
        return std::get<1>(payload_);
    }
};

} // namespace mini_runtime
