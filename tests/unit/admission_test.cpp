#include "mini_runtime/work_store.hpp"
#include <cassert>
#include <barrier>
#include <cstdlib>
#include <thread>
#include <vector>
static thread_local long budget=-1;
void* operator new(std::size_t n) {
    if(budget==0) throw std::bad_alloc();
    if(budget>0) --budget;
    if(auto p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
using namespace mini_runtime;
using namespace mini_runtime::detail;
struct Sink:ICompletionSink {
    SubmissionContext* seen=nullptr;
    void complete(SubmissionContext& c,BackendResultCode) noexcept override {seen=&c;}
};
struct Gate:IAllocationGate {
    int deny=-1; unsigned visited=0;
    bool allow(AdmissionPoint p) noexcept override {visited|=1u<<static_cast<int>(p);return static_cast<int>(p)!=deny;}
};
CommandDescriptor noop(){return {{"test.noop",1},{},{},{},{}};}
struct Fixture {
    std::mutex mutex;
    EventRegistry events{mutex,71}; OperationRegistry ops; Sink sink; Gate gate;
    WorkStore store{mutex,events,ops,&gate};
    Fixture(){assert(ops.register_operations({{{"test.noop",1},{},false,false}}));store.attach_sink(sink);}
};
void one(const CoreStats& s){assert(s.jobs==1&&s.submissions==1&&s.ready==1&&s.contexts_live==1&&s.external_events==1&&s.live_event_records==1&&s.completion_queue==0);}
int main(){
    {std::mutex m;EventRegistry e(m,1);OperationRegistry o;WorkStore s(m,e,o);auto r=s.admit(noop());assert(!r&&r.error().code==ErrorCode::INVALID_STATE);assert(s.empty());}
    for(int p=0;p<6;++p){Fixture f;auto old=f.store.admit(noop());assert(old);f.gate.deny=p;auto r=f.store.admit(noop());assert(!r&&r.error().code==ErrorCode::OUT_OF_MEMORY);one(f.store.inspect());assert(f.store.node_state(old.value())==NodeState::EMPTY);assert(f.events.query(old.value()));}
    bool reached_success=false;int failures=0;
    for(long n=0;n<40;++n){Fixture f;assert(f.store.admit(noop()));auto c=noop();budget=n;auto r=f.store.admit(c);budget=-1;if(r){reached_success=true;break;}++failures;assert(r.error().code==ErrorCode::OUT_OF_MEMORY);one(f.store.inspect());}
    assert(reached_success&&failures>=6);
    {Fixture f;for(int p=0;p<6;++p){auto c=noop();if(p==0)c.operation.name="unknown";if(p==1)c.operation.version=2;if(p==2)c.parameters.push_back({"p",true});if(p==3)c.inputs.push_back(1);if(p==4)c.outputs.push_back(1);if(p==5)c.dependencies.push_back(1);assert(!f.store.admit(c));assert(f.store.empty());}assert(f.store.admit(noop()));assert(f.gate.visited==63);one(f.store.inspect());assert(f.store.has_ready()&&!f.store.has_completion());}
    {Fixture f;constexpr int count=8;std::barrier start(count);std::vector<std::thread> threads;std::vector<EventHandle> handles(count);for(int i=0;i<count;++i)threads.emplace_back([&,i]{start.arrive_and_wait();auto r=f.store.admit(noop());assert(r);handles[i]=r.value();});for(auto& t:threads)t.join();auto s=f.store.inspect();assert(s.jobs==count&&s.ready==count&&s.submissions==count&&s.contexts_live==count);for(int i=0;i<count;++i)for(int j=0;j<i;++j)assert(handles[i].event_id!=handles[j].event_id);}
    const auto max=std::numeric_limits<uint64_t>::max();
    for(int p=0;p<3;++p){std::mutex m;EventRegistry e(m,9,p==2?max-1:1);OperationRegistry o;assert(o.register_operations({{{"test.noop",1},{},false,false}}));Sink sink;WorkStore s(m,e,o,nullptr,p==0?max-1:1,p==1?max-1:1);s.attach_sink(sink);assert(s.admit(noop()));auto r=s.admit(noop());assert(!r&&r.error().code==(p==0?ErrorCode::JOB_ID_EXHAUSTED:p==1?ErrorCode::SUBMISSION_ID_EXHAUSTED:ErrorCode::EVENT_ID_EXHAUSTED));one(s.inspect());}
    // Direct ownership graph exercise: callback owns Context, not vice versa.
    {std::mutex m;EventRegistry e(m,8);auto h=e.create().value();auto counts=std::make_shared<ContextCounters>();std::weak_ptr<SubmissionContext> weak;Sink sink;CompletionCallback cb;CompletionNode* node;
     {auto source=noop();auto owned=std::make_shared<const CommandDescriptor>(source);auto c=std::make_shared<SubmissionContext>(1,2,owned,e.acquire(h).value(),h,counts);weak=c;node=&c->node;source.operation.name="mutated";cb=[c,&sink](BackendResultCode code){assert(c->command->operation.name=="test.noop");sink.complete(*c,code);};assert(node->owner==c.get());}
     cb(BackendResultCode::Success);assert(&sink.seen->node==node&&counts->live==1);cb={};assert(weak.expired()&&counts->live==0);}
}
