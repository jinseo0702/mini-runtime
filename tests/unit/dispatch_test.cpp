#include "mini_runtime/dispatch.hpp"
#include "mini_runtime/mock_backend.hpp"
#include <cassert>
#include <cstdlib>
#include <future>
static thread_local bool inject=false;
static thread_local size_t requests=0;
void* operator new(std::size_t n){if(inject){++requests;throw std::bad_alloc();}if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
using namespace mini_runtime;
using namespace mini_runtime::detail;
using namespace mini_runtime::testing;
CommandDescriptor noop(){return {{"test.noop",1},{},{},{},{}};}
void enter(){requests=0;inject=true;}
void leave(){inject=false;assert(requests==0);}
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
    {Fixture f;Backend b(f.mutex);Dispatcher d(f.store,b);assert(!d.dispatch_one());
     for(int i=0;i<3;++i){auto c=noop();assert(f.store.admit(c));c.operation.name="changed";}
     for(int i=0;i<3;++i){assert(d.dispatch_one());enter();b.saved(BackendResultCode::Success);b.saved(BackendResultCode::Failure);leave();}
     assert(!d.dispatch_one());auto s=f.store.inspect();assert(s.completion_queue==3&&s.duplicate_callbacks==3&&s.finalizations==0);b.quiesce();}
    for(auto mode:{MockMode::Immediate,MockMode::Duplicate,MockMode::RejectWithCallback,MockMode::Reject,MockMode::Async,MockMode::Fail}){
        Fixture f;auto control=std::make_shared<MockControl>();control->set_mode(mode);control->set_callback_hooks(enter,leave);
        const bool early=mode==MockMode::Immediate||mode==MockMode::Duplicate||mode==MockMode::RejectWithCallback;
        if(early)control->hold(MockPoint::BeforeSubmitReturn);
        else if(mode!=MockMode::Reject)control->hold(MockPoint::BeforeCallback);
        MockBackendFactory factory(control);auto backend=factory.create();assert(backend&&backend.value()->initialize());Dispatcher d(f.store,*backend.value());
        auto h=f.store.admit(noop());assert(h);
        if(early){auto future=std::async(std::launch::async,[&]{return d.dispatch_one();});control->wait_reached(MockPoint::BeforeSubmitReturn);
            assert(f.store.node_state(h.value())==NodeState::STAGED);assert(!f.store.has_completion());assert(f.events.query(h.value()).value().status==EventStatus::PENDING);
            control->release(MockPoint::BeforeSubmitReturn);assert(future.get());
        }else {assert(d.dispatch_one());if(mode!=MockMode::Reject){control->wait_reached(MockPoint::BeforeCallback);assert(f.store.node_state(h.value())==NodeState::EMPTY);control->release(MockPoint::BeforeCallback);}}
        backend.value()->quiesce();backend.value()->cleanup();auto s=f.store.inspect();assert(s.completion_queue==1&&s.finalizations==0);assert(s.duplicate_callbacks==((mode==MockMode::Duplicate||mode==MockMode::RejectWithCallback)?1u:0u));assert(f.events.query(h.value()).value().status==EventStatus::PENDING);
        if(mode!=MockMode::Reject){assert(f.tap.seen);assert(f.tap.seen->node.payload==
            (mode==MockMode::Fail?CompletionReason::Failure:mode==MockMode::RejectWithCallback?CompletionReason::Rejected:CompletionReason::Success));}
    }
    // Direct context checks exercise fixed payload and late callback defenses without T07.
    {Fixture f;auto h=f.events.create().value();auto counts=std::make_shared<ContextCounters>();auto command=std::make_shared<const CommandDescriptor>(noop());
     SubmissionContext c(1,1,command,f.events.acquire(h).value(),h,counts);
     for(auto state:{NodeState::PROCESSING,NodeState::RETIRED}){c.node.state=state;c.phase=SubmissionPhase::ACCEPTED;enter();f.ingress.complete(c,BackendResultCode::Failure);leave();assert(c.node.state==state);}
     assert(counts->duplicates==2);c.node.state=NodeState::EMPTY;c.phase=SubmissionPhase::SUBMITTING;
     enter();f.ingress.complete(c,BackendResultCode::Failure);leave();assert(c.node.state==NodeState::STAGED&&c.node.payload==CompletionReason::Failure);
     c.phase=SubmissionPhase::REJECTED;enter();f.ingress.complete(c,BackendResultCode::Success);leave();assert(counts->duplicates==3);assert(!f.store.has_completion());}
}
