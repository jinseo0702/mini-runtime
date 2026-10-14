#include "mini_runtime/startup.hpp"
#include "mini_runtime/mock_backend.hpp"
#include <cassert>
#include <future>
using namespace mini_runtime;
using namespace mini_runtime::detail;
using namespace mini_runtime::testing;
struct Exhausted : IRuntimeIdSource {
    Result<uint64_t,RuntimeError> next() override {
        return Result<uint64_t,RuntimeError>::err(make_error(ErrorCode::RUNTIME_ID_EXHAUSTED,"exhausted"));
    }
};
struct FaultBackend : IExecutionBackend {
    std::function<void()> observe;
    std::vector<OperationSchema> schemas;
    explicit FaultBackend(std::function<void()> f) : observe(std::move(f)) {
        schemas.push_back({{"test.noop",1},{},false,false});
        schemas.push_back({{"test.noop",1},{},true,false});
    }
    Result<void,RuntimeError> initialize() override { observe(); return Result<void,RuntimeError>::ok(); }
    const std::vector<OperationSchema>& operations() const noexcept override { return schemas; }
    SubmitDecision submit(const BackendCommand&,CompletionCallback) noexcept override { return SubmitDecision::Rejected; }
    void quiesce() noexcept override { observe(); }
    void cleanup() noexcept override { observe(); }
};
struct FaultFactory : IBackendFactory {
    bool fail;
    std::function<void()> observe;
    FaultFactory(bool f,std::function<void()> o) : fail(f),observe(std::move(o)) {}
    Result<std::unique_ptr<IExecutionBackend>,RuntimeError> create() override {
        observe();
        if (fail) return Result<std::unique_ptr<IExecutionBackend>,RuntimeError>::err(make_error(ErrorCode::BACKEND_INIT_FAILED,"factory"));
        return Result<std::unique_ptr<IExecutionBackend>,RuntimeError>::ok(std::make_unique<FaultBackend>(observe));
    }
};
int main() {
    std::mutex coordinator;
    auto control = std::make_shared<MockControl>();
    StartupDependencies deps;
    deps.factory = std::make_shared<MockBackendFactory>(control);
    std::promise<void> arrived, release;
    auto released = release.get_future().share();
    std::atomic<int> workers{0};
    StartupSession* session = nullptr;
    deps.ready_gate = [&](WorkerRole role) {
        assert(session->state() == RuntimeState::STARTING);
        assert(session->backend());
        assert(session->operations().size() != 0);
        assert(workers == 0);
        if (role == WorkerRole::Scheduler) { arrived.set_value(); released.wait(); }
        return true;
    };
    deps.initialized = [&](uint64_t id) {
        assert(id != 0); assert(session->state() == RuntimeState::STARTING);
        return Result<void,RuntimeError>::ok();
    };
    deps.work = [&](WorkerRole, std::stop_token stop) {
        assert(session->state() == RuntimeState::RUNNING);
        workers.fetch_add(1);
        workers.notify_all();
        std::mutex m; std::condition_variable_any cv;
        std::unique_lock lock(m); cv.wait(lock,stop,[]{return false;});
    };
    StartupSession s(coordinator,deps); session = &s;
    assert(s.runtime_id() == 0);
    auto start = std::async(std::launch::async,[&]{return s.start();});
    arrived.get_future().wait();
    assert(s.start().error().code == ErrorCode::START_IN_PROGRESS);
    assert(workers == 0);
    release.set_value(); assert(start.get());
    while (workers.load() != 2) { auto n = workers.load(); if (n != 2) workers.wait(n); }
    assert(s.worker_count() == 2);
    assert(s.start().error().code == ErrorCode::ALREADY_RUNNING);
    assert(s.begin_shutdown()); assert(s.begin_draining());
    s.stop_scheduler(); s.stop_completion(); s.cleanup_backend();
    assert(s.complete_shutdown()); assert(!s.backend()); assert(s.worker_count() == 0);
    assert(s.begin_shutdown().error().code == ErrorCode::ALREADY_STOPPED);
    const auto consumed = s.runtime_id();
    for (int fault = 0; fault != 6; ++fault) {
        auto c = std::make_shared<MockControl>();
        StartupDependencies d; d.factory = std::make_shared<MockBackendFactory>(c);
        if (fault == 0) d.factory.reset();
        if (fault == 1) c->fail_initialize(true);
        if (fault == 2) d.ids = std::make_shared<Exhausted>();
        if (fault == 3) d.initialized = [](uint64_t) { return Result<void,RuntimeError>::err(make_error(ErrorCode::INVALID_STATE,"hook")); };
        if (fault == 4) d.ready_gate = [](WorkerRole r){return r != WorkerRole::Scheduler;};
        if (fault == 5) d.launch = [](WorkerRole r,std::function<void()> f) {
            if (r == WorkerRole::Scheduler) return Result<std::thread,RuntimeError>::err(make_error(ErrorCode::THREAD_INIT_FAILED,"launch"));
            return Result<std::thread,RuntimeError>::ok(std::thread(std::move(f)));
        };
        StartupSession failed(coordinator,std::move(d));
        assert(!failed.start()); assert(failed.state() == RuntimeState::START_FAILED);
        assert(!failed.backend()); assert(failed.operations().size() == 0); assert(failed.worker_count() == 0);
        assert(failed.start().error().code == ErrorCode::INVALID_STATE);
        assert(failed.begin_shutdown().error().code == ErrorCode::INVALID_STATE);
    }
    for (bool factory_failure : {false,true}) {
        StartupSession* observing = nullptr;
        int calls = 0;
        StartupDependencies d;
        d.factory = std::make_shared<FaultFactory>(factory_failure,[&]{
            assert(observing->state() == RuntimeState::STARTING); ++calls;
        });
        StartupSession failed(coordinator,std::move(d)); observing = &failed;
        auto result = failed.start();
        assert(!result);
        assert(result.error().code == (factory_failure ? ErrorCode::BACKEND_INIT_FAILED : ErrorCode::SCHEMA_CONFLICT));
        assert(calls == (factory_failure ? 1 : 4));
        assert(failed.state() == RuntimeState::START_FAILED && !failed.backend());
    }
    NativeRuntimeIdSource a,b;
    auto first = a.next(); auto second = b.next();
    assert(first && second && first.value() > consumed && second.value() == first.value()+1);
}
