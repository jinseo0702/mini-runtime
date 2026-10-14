#include "mini_runtime/testing/runtime_harness.hpp"
#include "mini_runtime/mock_backend.hpp"
#include <cassert>
#include <barrier>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
using namespace mini_runtime;
using namespace mini_runtime::testing;
struct Handoffs {
    std::mutex mutex;
    std::condition_variable cv;
    size_t count = 0;
    void arrive() noexcept { std::lock_guard lock(mutex); ++count; cv.notify_all(); }
    void wait(size_t target) { std::unique_lock lock(mutex); cv.wait(lock,[&]{return count >= target;}); }
};
void empty(const Runtime& runtime) {
    const auto s = inspect(runtime);
    if (s.jobs || s.submissions || s.ready || s.completion_queue || s.contexts_live || s.external_events || s.live_event_records)
        std::cerr << "nonzero jobs=" << s.jobs << " submissions=" << s.submissions << " ready=" << s.ready
                  << " completion_queue=" << s.completion_queue << " contexts=" << s.contexts_live
                  << " external_events=" << s.external_events << " live_event_records=" << s.live_event_records
                  << " finalizations=" << s.finalizations << std::endl;
    assert(s.jobs == 0 && s.submissions == 0 && s.ready == 0);
    assert(s.completion_queue == 0 && s.contexts_live == 0);
    assert(s.external_events == 0 && s.live_event_records == 0);
}
CommandDescriptor noop() { return {{"test.noop",1},{},{},{},{}}; }
void completed(Runtime& r, EventHandle h) {
    auto w = r.wait(h); assert(w);
    assert(w.value().status == EventStatus::COMPLETED && !w.value().failure);
    auto q = r.query(h); assert(q);
    assert(q.value().status == EventStatus::COMPLETED && !q.value().failure);
    assert(r.release_event(h));
}
int main() {
    Handoffs handoffs; // Outlives runtime, including destruction.
    auto c = std::make_shared<MockControl>();
    RuntimeTestOptions options;
    options.after_dispatch = [&]() noexcept { handoffs.arrive(); };
    auto r = make_runtime(std::make_shared<MockBackendFactory>(c),std::move(options));
    assert(r->start());
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i=0;i<10000;++i) {
        auto h = r->submit(noop()); assert(h); completed(*r,h.value());
        handoffs.wait(i+1); c->wait_idle(); empty(*r);
    }
    assert(inspect(*r).finalizations == 10000 && c->stats().callbacks == 10000);
    const auto serial_end = std::chrono::steady_clock::now();
    std::barrier start(5);
    std::vector<std::thread> callers;
    for (int t=0;t<4;++t) callers.emplace_back([&]{
        start.arrive_and_wait();
        for (int i=0;i<250;++i) {
            auto h = r->submit(noop()); assert(h);
            auto q = r->query(h.value()); assert(q);
            assert(q.value().status == EventStatus::PENDING || q.value().status == EventStatus::COMPLETED);
            completed(*r,h.value());
        }
    });
    start.arrive_and_wait(); for (auto& caller : callers) caller.join();
    handoffs.wait(11000); c->wait_idle(); empty(*r);
    assert(inspect(*r).finalizations == 11000 && c->stats().callbacks == 11000);
    const auto concurrent_end = std::chrono::steady_clock::now();
    // Accepted backend work drains while shutdown overlaps an application waiter.
    c->hold(MockPoint::BeforeCallback);
    auto h = r->submit(noop()); assert(h);
    c->wait_reached(MockPoint::BeforeCallback);
    auto waiter = std::async(std::launch::async,[&]{ completed(*r,h.value()); });
    c->hold(MockPoint::QuiesceEntered);
    auto shutdown = std::async(std::launch::async,[&]{return r->shutdown();});
    c->wait_reached(MockPoint::QuiesceEntered);
    c->release(MockPoint::BeforeCallback); c->release(MockPoint::QuiesceEntered);
    waiter.get(); assert(shutdown.get()); handoffs.wait(11001);
    assert(r->state() == RuntimeState::STOPPED); empty(*r);
    assert(inspect(*r).finalizations == 11001 && c->stats().callbacks == 11001);
    assert(c->stats().cleanup_calls == 1 && c->stats().threads_started == c->stats().threads_joined);
    // Deterministic READY cancellation; no caller survives runtime destruction.
    {
        std::mutex mutex; std::condition_variable_any cv; bool reached = false;
        auto control = std::make_shared<MockControl>(); RuntimeTestOptions gated;
        gated.before_schedule = [&](std::stop_token stop) {
            std::unique_lock lock(mutex); reached = true; cv.notify_all();
            cv.wait(lock,stop,[]{return false;});
        };
        auto ready = make_runtime(std::make_shared<MockBackendFactory>(control),std::move(gated));
        assert(ready->start()); auto event = ready->submit(noop()); assert(event);
        { std::unique_lock lock(mutex); cv.wait(lock,[&]{return reached;}); }
        assert(inspect(*ready).ready == 1);
        auto waiting = std::async(std::launch::async,[&]{return ready->wait(event.value());});
        assert(ready->shutdown()); auto result = waiting.get(); assert(result);
        assert(result.value().status == EventStatus::CANCELLED);
        assert(result.value().failure && result.value().failure->code == ErrorCode::SHUTDOWN_CANCELLED);
        assert(ready->release_event(event.value())); empty(*ready);
        assert(ready->state() == RuntimeState::STOPPED);
        assert(control->stats().cleanup_calls == 1);
        assert(control->stats().threads_started == control->stats().threads_joined);
    }
    std::cout << "baseline serial_cycles=10000 concurrent_cycles=1000 callers=4 serial_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(serial_end-begin).count()
              << " concurrent_us=" << std::chrono::duration_cast<std::chrono::microseconds>(concurrent_end-serial_end).count()
              << " finalizations=" << inspect(*r).finalizations << " callbacks=" << c->stats().callbacks
              << " live_resources=0 cleanup=" << c->stats().cleanup_calls
              << " threads_joined=" << c->stats().threads_joined << " SLA=none allocation_coverage=observed_counters_only\n";
}
