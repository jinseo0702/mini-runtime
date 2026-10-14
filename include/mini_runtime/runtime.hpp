#pragma once
#include "startup.hpp"
#include "dispatch.hpp"
#include "completion_processor.hpp"
namespace mini_runtime {
class Runtime;
namespace testing {
struct RuntimeTestOptions;
std::unique_ptr<Runtime> make_runtime(std::shared_ptr<IBackendFactory>, RuntimeTestOptions);
detail::CoreStats inspect(const Runtime&);
std::optional<detail::NodeState> node_state(const Runtime&,EventHandle);
}
class Runtime {
    friend std::unique_ptr<Runtime> testing::make_runtime(std::shared_ptr<IBackendFactory>,testing::RuntimeTestOptions);
    friend detail::CoreStats testing::inspect(const Runtime&);
    friend std::optional<detail::NodeState> testing::node_state(const Runtime&,EventHandle);
    // All core state uses this lock; backend calls and joins are always unlocked.
    mutable std::mutex coordinator_;
    detail::EventRegistry events_;
    detail::StartupSession startup_;
    detail::WorkStore store_;
    detail::CompletionIngress ingress_;
    detail::CompletionProcessor processor_;
    std::unique_ptr<detail::Dispatcher> dispatcher_;
    std::function<void(std::stop_token)> before_schedule_;
    detail::StartupDependencies configure(std::shared_ptr<IBackendFactory> factory,detail::StartupDependencies deps) {
        deps.factory = std::move(factory);
        auto initialized = std::move(deps.initialized);
        deps.initialized = [this,initialized=std::move(initialized)](uint64_t id) {
            auto bound = events_.bind_runtime_id(id);
            if (!bound) return bound;
            dispatcher_ = std::make_unique<detail::Dispatcher>(store_,*startup_.backend());
            return initialized ? initialized(id) : Result<void,RuntimeError>::ok();
        };
        deps.work = [this](detail::WorkerRole role,std::stop_token stop) {
            if (role == detail::WorkerRole::Scheduler) {
                while (store_.wait_ready(stop)) {
                    if (before_schedule_) before_schedule_(stop);
                    if (!stop.stop_requested()) dispatcher_->dispatch_one();
                }
            } else {
                while (store_.wait_completion(stop)) processor_.process_one();
            }
        };
        return deps;
    }
    Runtime(std::shared_ptr<IBackendFactory> factory,detail::StartupDependencies deps,
            detail::IAllocationGate* gate,uint64_t event,uint64_t job,uint64_t token,
            std::function<void(std::stop_token)> before)
        : events_(coordinator_,0,event), startup_(coordinator_,configure(std::move(factory),std::move(deps))),
          store_(coordinator_,events_,startup_.operations(),gate,job,token), ingress_(store_),
          processor_(store_), before_schedule_(std::move(before)) { store_.attach_sink(ingress_); }
public:
    explicit Runtime(std::shared_ptr<IBackendFactory> factory)
        : Runtime(std::move(factory),{},nullptr,1,1,1,{}) {}
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
    // Quiescence includes callback return AND callable release, so ingress/store
    // remain alive throughout normal shutdown, even after terminal publication.
    ~Runtime() noexcept { if (state() == RuntimeState::RUNNING) (void)shutdown(); }
    Result<void,RuntimeError> start() { return startup_.start(); }
    RuntimeState state() const { return startup_.state(); }
    uint64_t runtime_id() const { return startup_.runtime_id(); }
    Result<EventHandle,RuntimeError> submit(const CommandDescriptor& command) {
        std::lock_guard lock(coordinator_);
        if (startup_.state_locked() != RuntimeState::RUNNING)
            return Result<EventHandle,RuntimeError>::err(make_error(ErrorCode::INVALID_STATE,{}));
        return store_.admit_locked(command);
    }
    Result<EventSnapshot,RuntimeError> query(EventHandle h) { return events_.query(h); }
    Result<EventSnapshot,RuntimeError> wait(EventHandle h) { return events_.wait(h); }
    Result<void,RuntimeError> release_event(EventHandle h) { return events_.release_event(h); }
    Result<void,RuntimeError> shutdown() {
        {
            std::lock_guard lock(coordinator_);
            auto closing = startup_.begin_shutdown_locked();
            if (!closing) return closing;
            store_.cancel_ready_locked();
        }
        (void)startup_.begin_draining();
        startup_.stop_scheduler();
        startup_.backend()->quiesce();
        store_.wait_drained();
        startup_.stop_completion();
        dispatcher_.reset();
        startup_.cleanup_backend();
        return startup_.complete_shutdown();
    }
};
}
