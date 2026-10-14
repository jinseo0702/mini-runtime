#include "mini_runtime/event_registry.hpp"
#include <cassert>
#include <barrier>
#include <cstdlib>
#include <future>
#include <vector>
using namespace mini_runtime;
using namespace mini_runtime::detail;
thread_local bool deny = false;
void* operator new(std::size_t n) {
    if (deny) throw std::bad_alloc{};
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
template<class T> void invalid(const Result<T,RuntimeError>& r) {
    assert(!r && r.error().code == ErrorCode::INVALID_EVENT);
}
int main() {
    std::mutex m, other;
    EventRegistry r(m), foreign(other, 23);
    assert(!r.create()); assert(!r.bind_runtime_id(0));
    assert(r.bind_runtime_id(22)); assert(!r.bind_runtime_id(22));
    auto h = r.create().value();
    auto pending = r.query(h); assert(pending && pending.value().status == EventStatus::PENDING);
    invalid(r.query({23,h.event_id})); invalid(foreign.query(h));
    invalid(r.acquire({22,0})); invalid(r.release_event({23,h.event_id}));
    auto pin = r.acquire(h).value();
    std::barrier ready(9);
    std::vector<std::future<Result<EventSnapshot,RuntimeError>>> futures;
    for (int i=0; i<8; ++i) futures.push_back(std::async(std::launch::async,[&,p=pin]{
        ready.arrive_and_wait(); return r.wait_pinned(p);
    }));
    ready.arrive_and_wait();
    assert(r.release_event(h)); invalid(r.release_event(h)); invalid(r.wait(h));
    assert(r.external_size()==0 && r.live_records()==1);
    assert(r.complete(pin,EventStatus::FAILED,make_error(ErrorCode::BACKEND_FAILURE,"failure",91)));
    assert(!r.complete(pin,EventStatus::COMPLETED,std::nullopt));
    for (auto& f:futures) {
        auto s=f.get(); assert(s && s.value().status==EventStatus::FAILED);
        assert(s.value().failure->job_id==91 && s.value().failure->message=="failure");
    }
    pin.reset(); assert(r.live_records()==0);
    auto h2=r.create().value(); assert(h2.event_id>h.event_id);
    auto p2=r.acquire(h2).value();
    std::string message(1024,'z');
    assert(r.complete(p2,EventStatus::FAILED,make_error(ErrorCode::BACKEND_FAILURE,message,7)));
    auto copy=r.query(h2); copy.value().failure->message="mutated";
    assert(r.query(h2).value().failure->message==message);
    deny=true; auto oom=r.query(h2); auto woom=r.wait(h2); deny=false;
    assert(!oom && oom.error().code==ErrorCode::OUT_OF_MEMORY);
    assert(!woom && woom.error().code==ErrorCode::OUT_OF_MEMORY);
    assert(r.wait(h2).value().failure->message==message);
    assert(r.release_event(h2)); p2.reset(); assert(r.live_records()==0);
    deny=true; auto noalloc=r.create(); deny=false;
    assert(!noalloc && noalloc.error().code==ErrorCode::OUT_OF_MEMORY);
    assert(r.external_size()==0 && r.live_records()==0);
    auto h3=r.create().value(); assert(h3.event_id>h2.event_id+1);
    auto p3=r.acquire(h3).value();
    assert(r.complete(p3,EventStatus::CANCELLED,make_error(ErrorCode::BACKEND_FAILURE,"ignored",8)));
    auto cancelled=r.query(h3); assert(cancelled.value().failure->code==ErrorCode::SHUTDOWN_CANCELLED);
    assert(cancelled.value().failure->job_id==8); assert(r.release_event(h3)); p3.reset();
    EventRegistry exhausted(other,24,std::numeric_limits<uint64_t>::max()-1);
    auto last=exhausted.create(); assert(last);
    auto end=exhausted.create(); assert(!end && end.error().code==ErrorCode::EVENT_ID_EXHAUSTED);
    assert(exhausted.release_event(last.value())); assert(!exhausted.create());
    auto race=r.create().value(); auto rp=r.acquire(race).value(); std::barrier start(2);
    auto reader=std::async(std::launch::async,[&]{start.arrive_and_wait();
        for(int i=0;i<2000;++i){auto s=r.query(race); assert(s);
            assert(s.value().status==EventStatus::PENDING || s.value().status==EventStatus::COMPLETED);
            assert(!s.value().failure);}
    });
    start.arrive_and_wait(); assert(r.complete(rp,EventStatus::COMPLETED,make_error(ErrorCode::BACKEND_FAILURE,"discard")));
    reader.get(); assert(r.wait(race).value().status==EventStatus::COMPLETED);
    { std::lock_guard lock(m); assert(r.external_size_locked()==1);
      assert(r.query_locked(race)); assert(r.release_event_locked(race)); }
    rp.reset(); assert(r.live_records()==0);
}
