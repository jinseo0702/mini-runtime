#include "mini_runtime/result.hpp"
#include <cassert>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
using mini_runtime::Result;
using R = Result<int, std::string>;
static_assert(!std::is_default_constructible_v<R>);
static_assert(!std::is_copy_assignable_v<R> && !std::is_move_assignable_v<R>);
static_assert(std::is_copy_constructible_v<R>);
static_assert(std::is_nothrow_move_constructible_v<R>);
static_assert(!std::is_convertible_v<R, bool>);
static_assert(std::is_same_v<decltype(std::declval<R&>().value()), int&>);
static_assert(std::is_same_v<decltype(std::declval<const R&>().value()), const int&>);
static_assert(std::is_same_v<decltype(std::declval<R&>().error()), std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const R&>().error()), const std::string&>);
using M = Result<std::unique_ptr<int>, std::unique_ptr<int>>;
static_assert(!std::is_copy_constructible_v<M>);
template<class F> void rejects(F f) {
    bool caught = false;
    try { f(); } catch (const std::logic_error&) { caught = true; }
    assert(caught);
}
struct Tracked {
    static inline int live = 0;
    int value;
    explicit Tracked(int v) : value(v) { ++live; }
    Tracked(const Tracked& v) : value(v.value) { ++live; }
    Tracked(Tracked&& v) noexcept : value(v.value) { ++live; }
    ~Tracked() { --live; }
};
struct ThrowCopy {
    ThrowCopy() = default;
    ThrowCopy(ThrowCopy&&) noexcept = default;
    ThrowCopy(const ThrowCopy&) { throw 42; }
};
int main() {
    auto ok = R::ok(7);
    auto err = R::err("bad");
    assert(ok.has_value() && static_cast<bool>(ok));
    assert(!err.has_value() && !static_cast<bool>(err));
    ok.value() = 8;
    err.error() = "changed";
    const R& co = ok;
    const R& ce = err;
    assert(co.value() == 8 && ce.error() == "changed");
    rejects([&] { (void)ok.error(); });
    rejects([&] { (void)co.error(); });
    rejects([&] { (void)err.value(); });
    rejects([&] { (void)ce.value(); });
    R copied(ok), copied_error(err), moved(std::move(err));
    assert(copied.value() == 8 && copied_error.error() == "changed");
    assert(!err.has_value() && moved.error() == "changed");
    auto same_ok = Result<int, int>::ok(1);
    auto same_err = Result<int, int>::err(2);
    assert(same_ok.has_value() && !same_err.has_value());
    assert(same_ok.value() == 1 && same_err.error() == 2);
    auto mo = M::ok(std::make_unique<int>(3));
    auto me = M::err(std::make_unique<int>(4));
    M moved_ok(std::move(mo)), moved_err(std::move(me));
    assert(mo.has_value() && !me.has_value());
    assert(!mo.value() && !me.error());
    assert(*moved_ok.value() == 3 && *moved_err.error() == 4);
    {
        using L = Result<Tracked, Tracked>;
        auto a = L::ok(Tracked(10));
        auto b = L::err(Tracked(20));
        L c(a), d(b), e(std::move(a)), f(std::move(b));
        assert(Tracked::live == 6);
        assert(a.has_value() && !b.has_value());
        assert(c.value().value == 10 && d.error().value == 20);
        assert(e.value().value == 10 && f.error().value == 20);
    }
    assert(Tracked::live == 0);
    auto throwing = Result<ThrowCopy, int>::ok(ThrowCopy{});
    bool caught = false;
    try { auto copy = throwing; (void)copy; } catch (int v) { caught = v == 42; }
    assert(caught && throwing.has_value());
    caught = false;
    ThrowCopy source;
    try { auto copy = Result<ThrowCopy, int>::ok(source); (void)copy; }
    catch (int v) { caught = v == 42; }
    assert(caught);
}
