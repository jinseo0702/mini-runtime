#include "mini_runtime/lifecycle_gate.hpp"
#include <array>
#include <barrier>
#include <cassert>
#include <thread>
#include <vector>
using namespace mini_runtime;
using Outcome = Result<void, RuntimeError>;
static void success(const Outcome& r) {
    assert(r.has_value() && static_cast<bool>(r));
    r.value();
}
static void reject(const Outcome& r, ErrorCode code) {
    assert(!r.has_value() && !static_cast<bool>(r));
    assert(r.error().code == code);
}
static void observe(LifecycleGate& g, RuntimeState s,
                    std::optional<ErrorCode> start, std::optional<ErrorCode> stop) {
    const auto r = Result<RuntimeState, RuntimeError>::ok(g.state());
    assert(r.has_value() && static_cast<bool>(r));
    assert(r.value() == s);
    assert(check_start(r.value()) == start);
    assert(check_shutdown(r.value()) == stop);
}
static void rejected_operations(LifecycleGate& g, RuntimeState s) {
    if (auto e = check_start(s)) reject(g.begin_start(), *e);
    if (auto e = check_shutdown(s)) reject(g.begin_shutdown(), *e);
    if (s != RuntimeState::STARTING) {
        reject(g.complete_start(true), ErrorCode::INVALID_STATE);
        reject(g.complete_start(false), ErrorCode::INVALID_STATE);
    }
    if (s != RuntimeState::SHUTTING_DOWN) reject(g.begin_draining(), ErrorCode::INVALID_STATE);
    if (s != RuntimeState::DRAINING) reject(g.complete_shutdown(), ErrorCode::INVALID_STATE);
    assert(g.state() == s);
}
static void sequential(bool ready) {
    std::mutex coordinator; // Outlives Gate; never held across Gate calls.
    LifecycleGate g(coordinator);
    observe(g, RuntimeState::CREATED, std::nullopt, ErrorCode::INVALID_STATE);
    rejected_operations(g, RuntimeState::CREATED);
    success(g.begin_start());
    observe(g, RuntimeState::STARTING, ErrorCode::START_IN_PROGRESS, ErrorCode::START_IN_PROGRESS);
    rejected_operations(g, RuntimeState::STARTING);
    success(g.complete_start(ready)); // Test injection, not actual Runtime readiness.
    if (!ready) {
        observe(g, RuntimeState::START_FAILED, ErrorCode::INVALID_STATE, ErrorCode::INVALID_STATE);
        rejected_operations(g, RuntimeState::START_FAILED);
        return;
    }
    observe(g, RuntimeState::RUNNING, ErrorCode::ALREADY_RUNNING, std::nullopt);
    rejected_operations(g, RuntimeState::RUNNING);
    success(g.begin_shutdown());
    observe(g, RuntimeState::SHUTTING_DOWN, ErrorCode::INVALID_STATE, ErrorCode::SHUTDOWN_IN_PROGRESS);
    rejected_operations(g, RuntimeState::SHUTTING_DOWN);
    success(g.begin_draining());
    observe(g, RuntimeState::DRAINING, ErrorCode::INVALID_STATE, ErrorCode::SHUTDOWN_IN_PROGRESS);
    rejected_operations(g, RuntimeState::DRAINING);
    success(g.complete_shutdown());
    observe(g, RuntimeState::STOPPED, ErrorCode::INVALID_STATE, ErrorCode::ALREADY_STOPPED);
    rejected_operations(g, RuntimeState::STOPPED);
}
static void concurrent() {
    std::mutex coordinator;
    LifecycleGate g(coordinator);
    for (bool shutdown : {false, true}) {
        std::barrier launch(9);
        std::array<int, 8> winners{};
        std::array<std::optional<ErrorCode>, 8> errors{};
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < winners.size(); ++i) threads.emplace_back([&, i] {
            launch.arrive_and_wait();
            const auto r = shutdown ? g.begin_shutdown() : g.begin_start();
            if (r) { success(r); winners[i] = 1; }
            else {
                errors[i] = r.error().code;
                reject(r, shutdown ? ErrorCode::SHUTDOWN_IN_PROGRESS : ErrorCode::START_IN_PROGRESS);
            }
            assert(g.state() == (shutdown ? RuntimeState::SHUTTING_DOWN : RuntimeState::STARTING));
        });
        launch.arrive_and_wait();
        for (auto& t : threads) t.join();
        // Workers write distinct non-packed slots; main reads only after join.
        int total = 0;
        for (std::size_t i = 0; i < winners.size(); ++i) {
            total += winners[i];
            if (winners[i]) assert(!errors[i]);
            else assert(errors[i] == (shutdown ? ErrorCode::SHUTDOWN_IN_PROGRESS : ErrorCode::START_IN_PROGRESS));
        }
        assert(total == 1);
        if (!shutdown) success(g.complete_start(true)); // All competitors joined.
    }
    success(g.begin_draining());
    success(g.complete_shutdown());
    observe(g, RuntimeState::STOPPED, ErrorCode::INVALID_STATE, ErrorCode::ALREADY_STOPPED);
}
int main() {
    sequential(true);
    sequential(false);
    const auto e = make_error(ErrorCode::BACKEND_INIT_FAILED, "injected diagnostic", uint64_t{42});
    auto r = Result<RuntimeState, RuntimeError>::err(e);
    const auto copy = r;
    const auto moved = std::move(r);
    for (const auto* p : {&copy, &moved}) {
        assert(!p->has_value() && !static_cast<bool>(*p));
        assert(p->error().code == e.code);
        assert(p->error().job_id == e.job_id);
    }
    const auto v = Outcome::err(e);
    reject(v, e.code);
    assert(v.error().job_id == e.job_id); // Never branch on message text.
    concurrent();
}
