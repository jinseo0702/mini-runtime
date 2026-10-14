#include "mini_runtime/result.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

using mini_runtime::Result;
using R = Result<void, std::string>;
using M = Result<void, std::unique_ptr<int>>;
static_assert(std::is_same_v<decltype(std::declval<const R&>().value()), void>);
static_assert(std::is_same_v<decltype(std::declval<R&>().error()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const R&>().error()), const std::string&>);
static_assert(!std::is_convertible_v<R, bool>);
static_assert(noexcept(std::declval<const R&>().has_value()));
static_assert(noexcept(static_cast<bool>(std::declval<const R&>())));
static_assert(!std::is_default_constructible_v<R>);
static_assert(std::is_copy_constructible_v<R>);
static_assert(std::is_nothrow_move_constructible_v<R>);
static_assert(!std::is_copy_assignable_v<R> && !std::is_move_assignable_v<R>);
static_assert(!std::is_copy_constructible_v<M>);
static_assert(std::is_nothrow_move_constructible_v<M>);
static_assert(!std::is_default_constructible_v<M>);
static_assert(!std::is_copy_assignable_v<M> && !std::is_move_assignable_v<M>);

template <class F>
void logic_error(F action) {
    bool caught = false;
    try { action(); } catch (const std::logic_error&) { caught = true; }
    assert(caught);
}

struct Tracked {
    static inline int live = 0;
    int value;
    explicit Tracked(int v) : value(v) { ++live; }
    Tracked(const Tracked& other) : value(other.value) { ++live; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
    ~Tracked() { --live; }
};

int main() {
    auto success = R::ok();
    const auto& cs = success;
    assert(cs.has_value() && static_cast<bool>(cs));
    success.value();
    cs.value();
    logic_error([&] { (void)success.error(); });
    logic_error([&] { (void)cs.error(); });
    auto copied_success = success;
    auto moved_success = std::move(success);
    copied_success.value();
    moved_success.value();
    success.value();

    auto failure = R::err("failure");
    const auto& cf = failure;
    assert(!cf.has_value() && !static_cast<bool>(cf));
    assert(cf.error() == "failure");
    failure.error() = "changed";
    logic_error([&] { failure.value(); });
    logic_error([&] { cf.value(); });
    auto copied_failure = failure;
    failure.error() = "independent";
    assert(copied_failure.error() == "changed");
    auto moved_failure = std::move(copied_failure);
    assert(!moved_failure && moved_failure.error() == "changed");
    assert(!copied_failure.has_value());
    logic_error([&] { copied_failure.value(); });

    auto unique = M::err(std::make_unique<int>(42));
    auto moved_unique = std::move(unique);
    assert(!moved_unique && *moved_unique.error() == 42);
    assert(!unique.has_value() && !unique.error());
    auto unique_success = M::ok();
    auto moved_unique_success = std::move(unique_success);
    moved_unique_success.value();

    assert(Tracked::live == 0);
    {
        auto empty = Result<void, Tracked>::ok();
        empty.value();
        assert(Tracked::live == 0);
        auto original = Result<void, Tracked>::err(Tracked(7));
        assert(Tracked::live == 1);
        auto copy = original;
        auto move = std::move(original);
        assert(Tracked::live == 3);
        assert(copy.error().value == 7 && move.error().value == 7);
    }
    assert(Tracked::live == 0);
}
