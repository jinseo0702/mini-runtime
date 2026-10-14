#include "mini_runtime/mock_backend.hpp"
#include <cassert>
#include <future>
#include <atomic>
using namespace mini_runtime;
using namespace mini_runtime::testing;
int main() {
    auto command=std::make_shared<const CommandDescriptor>(CommandDescriptor{{"test.noop",1},{},{},{},{}});
    auto failed=std::make_shared<MockControl>(); failed->fail_initialize(true);
    { auto b=MockBackendFactory(failed).create(); assert(b); auto r=b.value()->initialize(); assert(!r && r.error().code==ErrorCode::BACKEND_INIT_FAILED); }
    assert(failed->stats().threads_started==0 && failed->stats().cleanup_calls==1);
    for(auto mode:{MockMode::Async,MockMode::Immediate,MockMode::Reject,MockMode::Fail,MockMode::Duplicate,MockMode::RejectWithCallback}) {
        auto c=std::make_shared<MockControl>(); c->set_mode(mode);
        std::atomic<int> entered=0,left=0,calls=0,failures=0;
        c->set_callback_hooks([&]{++entered;},[&]{++left;});
        { auto b=MockBackendFactory(c).create(); assert(b && b.value()->initialize());
          auto owner=std::make_shared<int>(42); std::weak_ptr<int> weak=owner;
          CompletionCallback cb=[owner,&calls,&failures](BackendResultCode code){assert(*owner==42); ++calls; if(code==BackendResultCode::Failure) ++failures;}; owner.reset();
          auto d=b.value()->submit({1,2,command},std::move(cb));
          assert(d==((mode==MockMode::Reject || mode==MockMode::RejectWithCallback)?SubmitDecision::Rejected:SubmitDecision::Accepted));
          b.value()->quiesce(); b.value()->quiesce(); assert(weak.expired());
          int expected=mode==MockMode::Reject?0:mode==MockMode::Duplicate?2:1;
          assert(calls==expected && entered==expected && left==expected);
          assert(failures==(mode==MockMode::Fail?1:0));
          assert(c->last_command()->operation==command->operation);
          b.value()->cleanup(); b.value()->cleanup(); }
        auto s=c->stats(); assert(s.callbacks==static_cast<size_t>(calls) && s.pending==0 && s.active==0 && s.cleanup_calls==1 && s.threads_started==1 && s.threads_joined==1);
    }
    for(auto point:{MockPoint::BeforeExecute,MockPoint::BeforeCallback,MockPoint::AfterCallback,MockPoint::BeforeSubmitReturn}) {
        auto c=std::make_shared<MockControl>(); c->hold(point);
        auto b=MockBackendFactory(c).create(); assert(b && b.value()->initialize());
        auto owned_command=std::make_shared<const CommandDescriptor>(*command); std::weak_ptr<const CommandDescriptor> wc=owned_command;
        auto owner=std::make_shared<int>(1); std::weak_ptr<int> weak=owner;
        CompletionCallback cb=[owner](BackendResultCode){assert(*owner==1);}; owner.reset();
        auto submit=std::async(std::launch::async,[&,cmd=std::move(owned_command),cb=std::move(cb)]() mutable {
            auto d=b.value()->submit({3,4,cmd},std::move(cb)); cmd.reset(); return d;
        });
        c->wait_reached(point);
        if(point!=MockPoint::BeforeSubmitReturn) assert(!weak.expired() && !wc.expired());
        c->hold(MockPoint::QuiesceEntered);
        auto q=std::async(std::launch::async,[&]{b.value()->quiesce();});
        c->wait_reached(MockPoint::QuiesceEntered);
        assert(q.wait_for(std::chrono::seconds(0))==std::future_status::timeout);
        c->release(MockPoint::QuiesceEntered); c->release(point);
        assert(submit.get()==SubmitDecision::Accepted); q.get(); c->wait_idle();
        assert(weak.expired() && wc.expired());
    }
    // Callback itself remains blocked independently of the post-callback gate.
    auto c=std::make_shared<MockControl>(); auto b=MockBackendFactory(c).create(); assert(b && b.value()->initialize());
    std::promise<void> entered,release; auto gate=release.get_future().share();
    b.value()->submit({1,1,command},[&](BackendResultCode){entered.set_value(); gate.wait();});
    entered.get_future().get(); c->hold(MockPoint::QuiesceEntered);
    auto q=std::async(std::launch::async,[&]{b.value()->quiesce();}); c->wait_reached(MockPoint::QuiesceEntered);
    assert(q.wait_for(std::chrono::seconds(0))==std::future_status::timeout);
    c->release(MockPoint::QuiesceEntered); release.set_value(); q.get();
}
