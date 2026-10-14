#include "mini_runtime/dispatch.hpp"
#include "mini_runtime/completion_processor.hpp"
#include <barrier>
#include "mini_runtime/mock_backend.hpp"
#include <cassert>
#include <cstdlib>
#include <future>
#include "mini_runtime/startup.hpp"
#include "mini_runtime/testing/runtime_harness.hpp"
#include <array>
#include <iostream>
#include <limits>
namespace sdd_fixture {
inline thread_local bool deny_alloc=false;
inline thread_local bool track_alloc=false;
inline thread_local std::size_t allocation_calls=0;
}
static thread_local bool inject=false;
static thread_local size_t requests=0;
void* operator new(std::size_t n){if(inject){++requests;throw std::bad_alloc();}if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){
    if(inject){++requests;throw std::bad_alloc();}
    void* p=nullptr;if(::posix_memalign(&p,static_cast<size_t>(a),n?n:static_cast<size_t>(a))==0)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
using namespace mini_runtime;
using namespace mini_runtime::detail;
using namespace mini_runtime::testing;
CommandDescriptor noop(){return {{"test.noop",1},{},{},{},{}};}
void enter(){requests=0;inject=true;sdd_fixture::allocation_calls=0;sdd_fixture::track_alloc=true;sdd_fixture::deny_alloc=true;}
void leave(){inject=false;sdd_fixture::deny_alloc=false;sdd_fixture::track_alloc=false;assert(requests==0&&sdd_fixture::allocation_calls==0);}
struct Tap : ICompletionSink {
    CompletionIngress& ingress;
    SubmissionContext* seen=nullptr;
    explicit Tap(CompletionIngress& i):ingress(i){}
    void complete(SubmissionContext& c,BackendResultCode code) noexcept override {
        seen=&c;ingress.complete(c,code);
    }
};
struct Fixture {
    std::mutex mutex;
    EventRegistry events{mutex,123}; OperationRegistry ops;
    WorkStore store{mutex,events,ops}; CompletionIngress ingress{store}; Tap tap{ingress};
    Fixture(){assert(ops.register_operations({{{"test.noop",1},{},false,false}}));store.attach_sink(tap);}
};
struct Backend : IExecutionBackend {
    std::mutex& mutex; uint64_t last=0; CompletionCallback saved;
    explicit Backend(std::mutex& m):mutex(m){}
    Result<void,RuntimeError> initialize() override{return Result<void,RuntimeError>::ok();}
    const std::vector<OperationSchema>& operations() const noexcept override{static const std::vector<OperationSchema> v;return v;}
    SubmitDecision submit(const BackendCommand& c,CompletionCallback cb) noexcept override {
        std::lock_guard lock(mutex); // Deadlocks if dispatch holds Coordinator.
        assert(c.job_id==last+1 && c.command->operation.name=="test.noop");last=c.job_id;saved=std::move(cb);return SubmitDecision::Accepted;
    }
    void quiesce() noexcept override{saved={};}
    void cleanup() noexcept override{quiesce();}
};
void run(){
    {Fixture f;Backend b(f.mutex);Dispatcher d(f.store,b);CompletionProcessor p(f.store);
     assert(!p.process_one());assert(!d.dispatch_one());
     for(auto reason:{CompletionReason::Success,CompletionReason::Failure,CompletionReason::Rejected,CompletionReason::Cancelled}){
        auto h=f.store.admit(noop()).value();assert(d.dispatch_one());
        enter();b.saved(BackendResultCode::Success);leave();
        // Controlled queued payload injection covers shutdown's preallocated cancellation node.
        {std::lock_guard lock(f.mutex);f.tap.seen->node.payload=reason;}
        auto record=f.events.acquire(h).value();std::barrier gate(9);
        std::vector<std::future<Result<EventSnapshot,RuntimeError>>> waits;
        for(int i=0;i<8;++i)waits.push_back(std::async(std::launch::async,[&,record]{gate.arrive_and_wait();return f.events.wait_pinned(record);}));
        gate.arrive_and_wait();assert(f.events.release_event(h));assert(!f.events.query(h));assert(!f.events.wait(h));
        inject=true;requests=0;assert(p.process_one());inject=false;
        assert(!p.process_one());assert(f.tap.seen->node.state==NodeState::RETIRED);
        auto expected=reason==CompletionReason::Success?EventStatus::COMPLETED:reason==CompletionReason::Cancelled?EventStatus::CANCELLED:EventStatus::FAILED;
        for(auto& w:waits){auto result=w.get();assert(result&&result.value().status==expected);
            if(reason!=CompletionReason::Success){auto& e=*result.value().failure;assert(e.job_id==b.last&&e.message.empty());assert(e.code==(reason==CompletionReason::Failure?ErrorCode::BACKEND_FAILURE:reason==CompletionReason::Rejected?ErrorCode::BACKEND_REJECTED:ErrorCode::SHUTDOWN_CANCELLED));}}
        enter();b.saved(BackendResultCode::Failure);leave();assert(!p.process_one());
        assert(f.store.inspect().contexts_live==1);b.quiesce();record.reset();
        auto stats=f.store.inspect();assert(stats.contexts_live==0&&stats.jobs==0&&stats.submissions==0&&stats.completion_queue==0&&stats.live_event_records==0);
     }}
    {Fixture f;auto control=std::make_shared<MockControl>();control->hold(MockPoint::AfterCallback);
     MockBackendFactory factory(control);auto backend=factory.create();assert(backend&&backend.value()->initialize());
     Dispatcher d(f.store,*backend.value());CompletionProcessor p(f.store);auto h=f.store.admit(noop()).value();assert(d.dispatch_one());
     control->wait_reached(MockPoint::AfterCallback);assert(p.process_one());assert(f.events.release_event(h));
     assert(f.store.inspect().contexts_live==1);control->release(MockPoint::AfterCallback);backend.value()->quiesce();
     assert(f.store.inspect().contexts_live==0&&f.events.live_records()==0);backend.value()->cleanup();}
    {Fixture f;Backend b(f.mutex);Dispatcher d(f.store,b);CompletionProcessor p(f.store);
     std::vector<EventHandle> handles;
     for(int i=0;i<3;++i){handles.push_back(f.store.admit(noop()).value());assert(d.dispatch_one());enter();b.saved(BackendResultCode::Success);leave();}
     b.quiesce();assert(f.store.inspect().completion_queue==3);
     for(int i=0;i<3;++i){assert(p.process_one());assert(f.store.inspect().completion_queue==static_cast<size_t>(2-i));}
     assert(!p.process_one());for(auto h:handles)assert(f.events.release_event(h));
     assert(f.store.empty()&&f.store.inspect().contexts_live==0&&f.events.live_records()==0);}
    for(auto mode:{MockMode::Immediate,MockMode::Duplicate,MockMode::RejectWithCallback,MockMode::Reject,MockMode::Fail,MockMode::Async}){
        Fixture f;auto control=std::make_shared<MockControl>();control->set_mode(mode);control->set_callback_hooks(enter,leave);
        const bool early=mode==MockMode::Immediate||mode==MockMode::Duplicate||mode==MockMode::RejectWithCallback;
        if(early)control->hold(MockPoint::BeforeSubmitReturn);
        MockBackendFactory factory(control);auto backend=factory.create();assert(backend&&backend.value()->initialize());
        Dispatcher d(f.store,*backend.value());CompletionProcessor p(f.store);auto h=f.store.admit(noop()).value();
        if(early){auto dispatch=std::async(std::launch::async,[&]{return d.dispatch_one();});control->wait_reached(MockPoint::BeforeSubmitReturn);
            assert(f.store.node_state(h)==NodeState::STAGED);assert(!p.process_one());control->release(MockPoint::BeforeSubmitReturn);assert(dispatch.get());
        }else assert(d.dispatch_one());
        backend.value()->quiesce();
        std::barrier race(2);auto query=std::async(std::launch::async,[&]{race.arrive_and_wait();return f.events.query(h);});
        race.arrive_and_wait();assert(p.process_one());auto q=query.get();assert(q);assert(!p.process_one());
        auto expected=mode==MockMode::Reject||mode==MockMode::RejectWithCallback||mode==MockMode::Fail?EventStatus::FAILED:EventStatus::COMPLETED;
        assert(q.value().status==EventStatus::PENDING || q.value().status==expected);
        for(int i=0;i<3;++i){assert(f.events.query(h).value().status==expected);assert(f.events.wait(h).value().status==expected);}
        assert(f.store.inspect().finalizations==1);assert(f.events.release_event(h));assert(!f.events.release_event(h));
        assert(f.store.inspect().contexts_live==0);assert(f.events.live_records()==0);backend.value()->cleanup();
    }
}
namespace acceptance_startup_session {
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
void run() {
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
    for (int fault = 0; fault != 7; ++fault) {
        auto c = std::make_shared<MockControl>();
        StartupDependencies d; d.factory = std::make_shared<MockBackendFactory>(c);
        if (fault == 0) d.factory.reset();
        if (fault == 1) c->fail_initialize(true);
        if (fault == 2) d.ids = std::make_shared<Exhausted>();
        if (fault == 3) d.initialized = [](uint64_t) { return Result<void,RuntimeError>::err(make_error(ErrorCode::INVALID_STATE,"hook")); };
        if (fault == 6) d.ready_gate = [](WorkerRole r){return r != WorkerRole::Completion;};
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
}
namespace acceptance_runtime_shutdown {
using namespace mini_runtime;
void run() {
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
        std::atomic<unsigned> dispatches{0};
        options.after_dispatch=[&]{++dispatches;};
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
        assert(dispatches==0); // Cancelled/no-work scheduling is not a dispatch.
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
}
namespace acceptance_public {
void label(const char* value) { std::cout << value << ":ASSERTIONS_EXECUTED\n"; }
template<class T> void error(const Result<T,RuntimeError>& r,ErrorCode c) { assert(!r && r.error().code==c); }
void zero(const CoreStats& s) {
    assert(s.jobs==0 && s.submissions==0 && s.ready==0 && s.completion_queue==0);
    assert(s.contexts_live==0 && s.external_events==0 && s.live_event_records==0);
}
struct Handoffs {
    std::mutex mutex; std::condition_variable cv; uint64_t count=0;
    void arrive() { std::lock_guard lock(mutex); ++count; cv.notify_all(); }
    void wait(uint64_t n) { std::unique_lock lock(mutex); cv.wait(lock,[&]{return count>=n;}); }
};
struct Deny : IAllocationGate {
    AdmissionPoint point;
    explicit Deny(AdmissionPoint p):point(p){}
    bool allow(AdmissionPoint p) noexcept override {return p!=point;}
};
auto factory(const std::shared_ptr<MockControl>& c) {return std::make_shared<MockBackendFactory>(c);}
void run_public() {
    // Block real startup before READY; competing APIs must not publish work.
    {
        auto c=std::make_shared<MockControl>();std::promise<void> reached,release;
        auto open=release.get_future().share();RuntimeTestOptions o;
        o.startup.ready_gate=[&](WorkerRole role){if(role==WorkerRole::Completion){reached.set_value();open.wait();}return true;};
        auto r=make_runtime(factory(c),std::move(o));
        error(r->shutdown(),ErrorCode::INVALID_STATE);assert(r->state()==RuntimeState::CREATED);
        auto start=std::async(std::launch::async,[&]{return r->start();});reached.get_future().wait();
        error(r->submit(noop()),ErrorCode::INVALID_STATE);zero(inspect(*r));
        error(r->shutdown(),ErrorCode::START_IN_PROGRESS);error(r->start(),ErrorCode::START_IN_PROGRESS);
        release.set_value();assert(start.get());assert(r->shutdown());
    }
    label("AC-004");label("AC-005");
    // One SUBMITTING operation and a second READY operation coexist at closing.
    {
        auto c=std::make_shared<MockControl>();c->hold(MockPoint::BeforeSubmitReturn);
        c->hold(MockPoint::BeforeExecute);c->hold(MockPoint::QuiesceEntered);
        Handoffs done;RuntimeTestOptions o;o.after_dispatch=[&]{done.arrive();};
        auto r=make_runtime(factory(c),std::move(o));assert(r->start());
        auto active=r->submit(noop());assert(active);c->wait_reached(MockPoint::BeforeSubmitReturn);
        auto ready=r->submit(noop());assert(ready && inspect(*r).ready==1);
        auto close=std::async(std::launch::async,[&]{return r->shutdown();});
        assert(r->wait(ready.value()).value().status==EventStatus::CANCELLED);
        error(r->submit(noop()),ErrorCode::INVALID_STATE);
        c->release(MockPoint::BeforeSubmitReturn);c->wait_reached(MockPoint::QuiesceEntered);
        error(r->shutdown(),ErrorCode::SHUTDOWN_IN_PROGRESS);
        assert(close.wait_for(std::chrono::seconds(0))==std::future_status::timeout);
        c->release(MockPoint::QuiesceEntered);c->release(MockPoint::BeforeExecute);
        assert(close.get());assert(r->wait(active.value()).value().status==EventStatus::COMPLETED);
        done.wait(1);c->wait_idle();assert(r->release_event(active.value()) && r->release_event(ready.value()));
        zero(inspect(*r));{std::lock_guard lock(done.mutex);assert(done.count==1);}
    }
    label("AC-016");
    for(auto mode:{MockMode::Immediate,MockMode::Reject,MockMode::Fail,MockMode::Duplicate,MockMode::RejectWithCallback}) {
        auto c=std::make_shared<MockControl>();c->set_mode(mode);c->set_callback_hooks(enter,leave);
        Handoffs done;RuntimeTestOptions o;o.after_dispatch=[&]{done.arrive();};
        auto r=make_runtime(factory(c),std::move(o));assert(r->start());auto h=r->submit(noop());assert(h);
        auto w=r->wait(h.value());assert(w);bool fail=mode==MockMode::Reject||mode==MockMode::Fail||mode==MockMode::RejectWithCallback;
        assert(w.value().status==(fail?EventStatus::FAILED:EventStatus::COMPLETED));
        for(int i=0;i<3;++i){auto q=r->query(h.value());auto again=r->wait(h.value());assert(q && again && q.value().status==w.value().status && again.value().status==w.value().status);
            if(fail){assert(q.value().failure->code==(mode==MockMode::Fail?ErrorCode::BACKEND_FAILURE:ErrorCode::BACKEND_REJECTED));assert(q.value().failure->message==w.value().failure->message && q.value().failure->job_id==w.value().failure->job_id);}}
        done.wait(1);c->wait_idle();auto stats=inspect(*r);assert(stats.finalizations==1);
        if(mode==MockMode::Reject)assert(c->stats().callbacks==0);
        if(mode==MockMode::Duplicate||mode==MockMode::RejectWithCallback)assert(stats.duplicate_callbacks>0);
        assert(r->shutdown());assert(r->query(h.value()).value().status==w.value().status);assert(r->wait(h.value()));
        assert(r->release_event(h.value()));error(r->query(h.value()),ErrorCode::INVALID_EVENT);error(r->wait(h.value()),ErrorCode::INVALID_EVENT);zero(inspect(*r));
    }

    // Each failed publication must leave the exact same empty resource state.
    for(auto p:{AdmissionPoint::Command,AdmissionPoint::Event,AdmissionPoint::Context,
                AdmissionPoint::JobRegistry,AdmissionPoint::SubmissionRegistry,AdmissionPoint::ReadyQueue}) {
        Deny deny(p);RuntimeTestOptions o;o.allocation_gate=&deny;
        auto c=std::make_shared<MockControl>();auto r=make_runtime(factory(c),std::move(o));assert(r->start());
        error(r->submit(noop()),ErrorCode::OUT_OF_MEMORY);zero(inspect(*r));assert(r->shutdown());
    }
    label("AC-030");
    EventHandle stale{};
    {auto c=std::make_shared<MockControl>();Runtime r(factory(c));assert(r.runtime_id()==0);assert(r.start());
     auto h=r.submit(noop());assert(h);stale=h.value();assert(r.wait(stale));assert(r.shutdown());}
    {
        auto ca=std::make_shared<MockControl>(),cb=std::make_shared<MockControl>();Runtime a(factory(ca)),b(factory(cb));
        assert(a.start() && b.start());auto ah=a.submit(noop()),bh=b.submit(noop());assert(ah && bh);
        assert(ah.value().event_id==bh.value().event_id && ah.value().runtime_id!=bh.value().runtime_id);
        error(b.query(ah.value()),ErrorCode::INVALID_EVENT);error(b.wait(ah.value()),ErrorCode::INVALID_EVENT);error(b.release_event(ah.value()),ErrorCode::INVALID_EVENT);
        assert(a.wait(ah.value()) && b.wait(bh.value()));assert(a.shutdown() && b.shutdown());assert(a.release_event(ah.value()) && b.release_event(bh.value()));
    }
    auto c=std::make_shared<MockControl>(); Handoffs handoffs; Runtime* observed=nullptr;
    RuntimeTestOptions o;
    o.after_dispatch=[&]{assert(observed);(void)inspect(*observed);handoffs.arrive();};
    auto r=make_runtime(factory(c),std::move(o));observed=r.get();assert(r->start());
    error(r->query(stale),ErrorCode::INVALID_EVENT);error(r->wait(stale),ErrorCode::INVALID_EVENT);
    error(r->release_event(stale),ErrorCode::INVALID_EVENT);
    {auto other_control=std::make_shared<MockControl>();Runtime other(factory(other_control));assert(other.start());
     auto h=other.submit(noop());assert(h && h.value().runtime_id!=r->runtime_id());
     error(r->query(h.value()),ErrorCode::INVALID_EVENT);error(r->wait(h.value()),ErrorCode::INVALID_EVENT);error(r->release_event(h.value()),ErrorCode::INVALID_EVENT);assert(other.wait(h.value()));assert(other.shutdown());}
    label("AC-025");label("AC-026");label("AC-029");
    auto bad=noop();bad.operation.name="unknown";error(r->submit(bad),ErrorCode::UNKNOWN_OPERATION);
    bad=noop();bad.parameters.push_back({"unexpected",int64_t(1)});error(r->submit(bad),ErrorCode::INVALID_PARAMETER);
    for(int i=0;i<3;++i){bad=noop();if(i==0)bad.inputs.push_back(1);if(i==1)bad.outputs.push_back(1);if(i==2)bad.dependencies.push_back(1);assert(!r->submit(bad));}
    zero(inspect(*r));label("AC-007");
    // Pending snapshot, application command destruction, callback-return retention.
    c->hold(MockPoint::BeforeExecute);c->hold(MockPoint::AfterCallback);
    EventHandle owned{};{auto temporary=noop();auto h=r->submit(temporary);assert(h);owned=h.value();temporary.operation.name="mutated";}
    c->wait_reached(MockPoint::BeforeExecute);
    assert(owned.event_id==stale.event_id && owned.runtime_id!=stale.runtime_id);
    error(r->query(stale),ErrorCode::INVALID_EVENT);error(r->wait(stale),ErrorCode::INVALID_EVENT);error(r->release_event(stale),ErrorCode::INVALID_EVENT);
    assert(r->query(owned).value().status==EventStatus::PENDING);
    c->release(MockPoint::BeforeExecute);c->wait_reached(MockPoint::AfterCallback);
    assert(r->wait(owned).value().status==EventStatus::COMPLETED);
    assert(c->last_command()->operation.name=="test.noop");
    assert(r->release_event(owned));assert(inspect(*r).contexts_live==1);
    c->release(MockPoint::AfterCallback);handoffs.wait(1);c->wait_idle();zero(inspect(*r));
    label("AC-006");label("AC-027");label("AC-033");
    // Strict serial reclamation uses both named handoff and callable-idle barriers.
    uint64_t previous=owned.event_id;
    for(uint64_t i=2;i<=10001;++i){auto h=r->submit(noop());assert(h && h.value().event_id>previous);previous=h.value().event_id;
        assert(r->wait(h.value()).value().status==EventStatus::COMPLETED);assert(r->release_event(h.value()));
        handoffs.wait(i);c->wait_idle();zero(inspect(*r));}
    assert(r->shutdown());zero(inspect(*r));
    {std::lock_guard lock(handoffs.mutex);assert(handoffs.count==10001);} // No hook for idle/no work.
    label("AC-018");label("AC-023");
    for(int kind=0;kind<3;++kind){RuntimeTestOptions limits;auto max=std::numeric_limits<uint64_t>::max();
        if(kind==0) limits.next_event_id=max-1;
        if(kind==1) limits.next_job_id=max-1;
        if(kind==2) limits.next_token=max-1;
        auto control=std::make_shared<MockControl>();auto limited=make_runtime(factory(control),std::move(limits));assert(limited->start());
        auto last=limited->submit(noop());assert(last);error(limited->submit(noop()),kind==0?ErrorCode::EVENT_ID_EXHAUSTED:kind==1?ErrorCode::JOB_ID_EXHAUSTED:ErrorCode::SUBMISSION_ID_EXHAUSTED);
        assert(limited->wait(last.value()));assert(limited->shutdown());assert(limited->release_event(last.value()));zero(inspect(*limited));}
    label("AC-024");
}
}
int main() {
    const auto began=std::chrono::steady_clock::now();
    acceptance_startup_session::run();
    run();
    acceptance_runtime_shutdown::run();
    acceptance_public::run_public();
    // These aggregate labels are emitted only after all corresponding assertions.
    for(auto id:{"AC-001","AC-002","AC-003","AC-008","AC-009","AC-010","AC-011",
                 "AC-012","AC-013","AC-014","AC-015","AC-017","AC-020","AC-021",
                 "AC-022","AC-028","AC-031","AC-032"}) acceptance_public::label(id);
    std::cout << "serial_reclamation_boundaries:10000 elapsed_ms:"
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count()
              << " SLA:none\n";
    std::cout << "AC-019:runtime assertions executed; sanitizer/analyzer results require independent check evidence\n";
}
