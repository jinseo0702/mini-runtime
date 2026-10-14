#include "mini_runtime/dispatch.hpp"
#include "mini_runtime/completion_processor.hpp"
#include <barrier>
#include "mini_runtime/mock_backend.hpp"
#include <cassert>
#include <cstdlib>
#include <future>
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
int main(){
    {Fixture f;Backend b(f.mutex);Dispatcher d(f.store,b);CompletionProcessor p(f.store);
     assert(!p.process_one());
     for(auto reason:{CompletionReason::Success,CompletionReason::Failure,CompletionReason::Rejected,CompletionReason::Cancelled}){
        auto h=f.store.admit(noop()).value();assert(d.dispatch_one());
        enter();b.saved(BackendResultCode::Success);leave();
        // Controlled queued payload injection covers shutdown's preallocated cancellation node.
        {std::lock_guard lock(f.mutex);f.tap.seen->node.payload=reason;}
        auto record=f.events.acquire(h).value();std::barrier gate(9);
        std::vector<std::future<Result<EventSnapshot,RuntimeError>>> waits;
        for(int i=0;i<8;++i)waits.push_back(std::async(std::launch::async,[&,record]{gate.arrive_and_wait();return f.events.wait_pinned(record);}));
        gate.arrive_and_wait();assert(f.events.release_event(h));
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
        for(int i=0;i<3;++i){assert(f.events.query(h).value().status==expected);assert(f.events.wait(h).value().status==expected);}
        assert(f.store.inspect().finalizations==1);assert(f.events.release_event(h));assert(!f.events.release_event(h));
        assert(f.store.inspect().contexts_live==0);assert(f.events.live_records()==0);backend.value()->cleanup();
    }
}
