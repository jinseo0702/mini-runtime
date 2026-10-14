#include "mini_runtime/lifecycle_gate.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>
using namespace mini_runtime;
static_assert(!std::is_copy_constructible_v<LifecycleGate>);
static_assert(!std::is_move_constructible_v<LifecycleGate>);
static_assert(!std::is_copy_assignable_v<LifecycleGate>);
static_assert(!std::is_move_assignable_v<LifecycleGate>);

void reach(LifecycleGate& g, RuntimeState s) {
    assert(g.state() == RuntimeState::CREATED);
    if (s == RuntimeState::CREATED) return;
    assert(g.begin_start());
    assert(g.state() == RuntimeState::STARTING); // No automatic readiness.
    if (s == RuntimeState::STARTING) return;
    // Inject a readiness result, not an actual Runtime implementation.
    assert(g.complete_start(s != RuntimeState::START_FAILED));
    if (s == RuntimeState::RUNNING || s == RuntimeState::START_FAILED) return;
    assert(g.begin_shutdown());
    if (s == RuntimeState::SHUTTING_DOWN) return;
    assert(g.begin_draining());
    if (s == RuntimeState::DRAINING) return;
    assert(g.complete_shutdown());
}
int main() {
    constexpr std::array states{RuntimeState::CREATED, RuntimeState::STARTING,
        RuntimeState::RUNNING, RuntimeState::START_FAILED,
        RuntimeState::SHUTTING_DOWN, RuntimeState::DRAINING, RuntimeState::STOPPED};
    constexpr std::array<std::optional<ErrorCode>, 7> start_errors{
        std::nullopt, ErrorCode::START_IN_PROGRESS, ErrorCode::ALREADY_RUNNING,
        ErrorCode::INVALID_STATE, ErrorCode::INVALID_STATE,
        ErrorCode::INVALID_STATE, ErrorCode::INVALID_STATE};
    constexpr std::array<std::optional<ErrorCode>, 7> shutdown_errors{
        ErrorCode::INVALID_STATE, ErrorCode::START_IN_PROGRESS, std::nullopt,
        ErrorCode::INVALID_STATE, ErrorCode::SHUTDOWN_IN_PROGRESS,
        ErrorCode::SHUTDOWN_IN_PROGRESS, ErrorCode::ALREADY_STOPPED};
    for (std::size_t i = 0; i < states.size(); ++i) {
        for (int op = 0; op < 6; ++op) {
            std::mutex mutex;
            LifecycleGate g(mutex);
            reach(g, states[i]);
            auto expected = states[i];
            std::optional<ErrorCode> error;
            auto result = [&]() {
                switch (op) {
                case 0:
                    error = start_errors[i];
                    if (!error) expected = RuntimeState::STARTING;
                    return g.begin_start();
                case 1:
                    error = shutdown_errors[i];
                    if (!error) expected = RuntimeState::SHUTTING_DOWN;
                    return g.begin_shutdown();
                case 2: case 3:
                    if (states[i] != RuntimeState::STARTING) error = ErrorCode::INVALID_STATE;
                    else expected = op == 2 ? RuntimeState::RUNNING : RuntimeState::START_FAILED;
                    return g.complete_start(op == 2);
                case 4:
                    if (states[i] != RuntimeState::SHUTTING_DOWN) error = ErrorCode::INVALID_STATE;
                    else expected = RuntimeState::DRAINING;
                    return g.begin_draining();
                default:
                    if (states[i] != RuntimeState::DRAINING) error = ErrorCode::INVALID_STATE;
                    else expected = RuntimeState::STOPPED;
                    return g.complete_shutdown();
                }
            }();
            assert(result.has_value() == !error.has_value());
            if (error) assert(result.error().code == *error);
            else result.value();
            assert(g.state() == expected);
        }
    }
    std::mutex mutex;
    LifecycleGate g(mutex);
    for (bool shutdown : {false, true}) {
        std::barrier launch(9);
        std::atomic<int> successes{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
            launch.arrive_and_wait();
            auto result = shutdown ? g.begin_shutdown() : g.begin_start();
            if (result) ++successes;
            else assert(result.error().code == (shutdown ? ErrorCode::SHUTDOWN_IN_PROGRESS : ErrorCode::START_IN_PROGRESS));
            auto observed = g.state();
            assert(observed == (shutdown ? RuntimeState::SHUTTING_DOWN : RuntimeState::STARTING));
        });
        launch.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        assert(successes == 1);
        if (!shutdown) assert(g.complete_start(true));
    }
    // A handshake establishes the worker has started while the external lock
    // is held; a bounded future wait checks blocking, not a sleep-only probe.
    for (bool read : {false, true}) {
        std::unique_lock held(mutex);
        std::promise<void> entered;
        auto ready = entered.get_future();
        auto operation = std::async(std::launch::async, [&] {
            entered.set_value();
            if (read) return g.state() == RuntimeState::SHUTTING_DOWN;
            return g.begin_shutdown().error().code == ErrorCode::SHUTDOWN_IN_PROGRESS;
        });
        ready.wait();
        assert(operation.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
        held.unlock();
        assert(operation.get()); // joins the async thread
    }
}
