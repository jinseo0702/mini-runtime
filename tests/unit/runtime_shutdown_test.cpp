#include "mini_runtime/testing/runtime_harness.hpp"
#include "mini_runtime/mock_backend.hpp"
#include <cassert>
#include <future>
#include <array>
using namespace mini_runtime;
int main() {
    auto control = std::make_shared<testing::MockControl>();
    Runtime runtime(std::make_shared<testing::MockBackendFactory>(control));
    CommandDescriptor command{{"test.noop",1},{},{},{},{}};
    assert(!runtime.submit(command));
    assert(!runtime.shutdown());
    assert(runtime.start());
    assert(!runtime.start());
    auto event = runtime.submit(command); assert(event);
    auto result = runtime.wait(event.value()); assert(result);
    assert(result.value().status == EventStatus::COMPLETED);
    assert(runtime.shutdown());
    assert(runtime.state() == RuntimeState::STOPPED);
    assert(runtime.query(event.value()));
    assert(!runtime.shutdown());
    assert(runtime.release_event(event.value()));
    assert(!runtime.query(event.value()));
    {
        auto c = std::make_shared<testing::MockControl>();
        testing::RuntimeTestOptions options;
        std::mutex mutex;
        std::condition_variable_any cv;
        bool reached = false;
        options.before_schedule = [&](std::stop_token stop) {
            std::unique_lock lock(mutex);
            reached = true; cv.notify_all();
            cv.wait(lock,stop,[]{return false;});
        };
        auto r = testing::make_runtime(std::make_shared<testing::MockBackendFactory>(c),std::move(options));
        assert(r->start());
        auto h = r->submit(command); assert(h);
        { std::unique_lock lock(mutex); cv.wait(lock,[&]{return reached;}); }
        assert(testing::inspect(*r).ready == 1);
        std::array<std::future<Result<EventSnapshot,RuntimeError>>,8> waiters;
        for (auto& waiter : waiters) waiter = std::async(std::launch::async,[&]{return r->wait(h.value());});
        assert(r->shutdown());
        for (auto& waiter : waiters) {
            auto snapshot = waiter.get(); assert(snapshot);
            assert(snapshot.value().status == EventStatus::CANCELLED);
            assert(snapshot.value().failure->code == ErrorCode::SHUTDOWN_CANCELLED);
        }
        assert(r->release_event(h.value()));
        assert(testing::inspect(*r).jobs == 0);
    }
    {
        auto c = std::make_shared<testing::MockControl>();
        c->hold(testing::MockPoint::AfterCallback);
        c->hold(testing::MockPoint::QuiesceEntered);
        auto r = std::make_unique<Runtime>(std::make_shared<testing::MockBackendFactory>(c));
        assert(r->start()); auto h = r->submit(command); assert(h);
        c->wait_reached(testing::MockPoint::AfterCallback);
        assert(r->wait(h.value()));
        auto destroy = std::async(std::launch::async,[&]{r.reset();});
        c->wait_reached(testing::MockPoint::QuiesceEntered);
        assert(destroy.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
        c->release(testing::MockPoint::QuiesceEntered);
        c->release(testing::MockPoint::AfterCallback);
        destroy.get(); assert(c->stats().cleanup_calls == 1);
    }
    {
        auto c = std::make_shared<testing::MockControl>();
        c->set_mode(testing::MockMode::Immediate);
        c->hold(testing::MockPoint::BeforeSubmitReturn);
        c->hold(testing::MockPoint::QuiesceEntered);
        Runtime r(std::make_shared<testing::MockBackendFactory>(c));
        assert(r.start()); auto h = r.submit(command); assert(h);
        c->wait_reached(testing::MockPoint::BeforeSubmitReturn);
        assert(testing::inspect(r).completion_queue == 0);
        auto shutdown = std::async(std::launch::async,[&]{return r.shutdown();});
        c->release(testing::MockPoint::BeforeSubmitReturn);
        c->wait_reached(testing::MockPoint::QuiesceEntered);
        auto duplicate = r.shutdown(); assert(!duplicate);
        assert(duplicate.error().code == ErrorCode::SHUTDOWN_IN_PROGRESS);
        c->release(testing::MockPoint::QuiesceEntered);
        assert(shutdown.get()); assert(r.wait(h.value()).value().status == EventStatus::COMPLETED);
        assert(r.release_event(h.value()));
    }

    for (auto mode : {testing::MockMode::Fail,testing::MockMode::Reject,testing::MockMode::Duplicate}) {
        auto c = std::make_shared<testing::MockControl>(); c->set_mode(mode);
        Runtime r(std::make_shared<testing::MockBackendFactory>(c)); assert(r.start());
        auto invalid = command; invalid.inputs.push_back(1); assert(!r.submit(invalid));
        auto h = r.submit(command); assert(h); auto result = r.wait(h.value()); assert(result);
        if (mode == testing::MockMode::Duplicate) assert(result.value().status == EventStatus::COMPLETED);
        else {
            assert(result.value().status == EventStatus::FAILED);
            assert(result.value().failure->code == (mode == testing::MockMode::Reject ? ErrorCode::BACKEND_REJECTED : ErrorCode::BACKEND_FAILURE));
        }
        assert(r.shutdown()); assert(r.release_event(h.value()));
    }
    {
        auto c = std::make_shared<testing::MockControl>(); c->fail_initialize(true);
        Runtime r(std::make_shared<testing::MockBackendFactory>(c)); assert(!r.start());
        assert(r.state() == RuntimeState::START_FAILED); assert(!r.shutdown());
    }

    // Deterministically pin all eight references before external release; Runtime
    // delegates wait to this same registry implementation.
    {
        std::mutex mutex;
        detail::EventRegistry registry(mutex,123);
        auto h = registry.create(); assert(h);
        std::array<std::shared_ptr<detail::EventRecord>,8> pins;
        for (auto& pin : pins) { auto acquired = registry.acquire(h.value()); assert(acquired); pin = acquired.value(); }
        std::array<std::future<Result<EventSnapshot,RuntimeError>>,8> waiters;
        for (size_t i=0;i<8;++i) waiters[i] = std::async(std::launch::async,[&,i]{return registry.wait_pinned(pins[i]);});
        assert(registry.release_event(h.value())); assert(!registry.query(h.value()));
        assert(registry.complete(pins[0],EventStatus::COMPLETED,std::nullopt));
        for (auto& waiter : waiters) { auto snapshot = waiter.get(); assert(snapshot && snapshot.value().status == EventStatus::COMPLETED); }
    }

}
