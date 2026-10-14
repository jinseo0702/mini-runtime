#pragma once
#include "backend.hpp"
#include "lifecycle_policy.hpp"
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
namespace mini_runtime::detail {
enum class WorkerRole { Completion, Scheduler };
class IRuntimeIdSource {
public:
    virtual ~IRuntimeIdSource() = default;
    virtual Result<uint64_t,RuntimeError> next() = 0;
};
class NativeRuntimeIdSource : public IRuntimeIdSource {
    inline static std::atomic<uint64_t> next_{1};
public:
    Result<uint64_t,RuntimeError> next() override {
        auto n = next_.load();
        while (n != UINT64_MAX) {
            if (next_.compare_exchange_weak(n,n+1)) return Result<uint64_t,RuntimeError>::ok(n);
        }
        return Result<uint64_t,RuntimeError>::err(make_error(ErrorCode::RUNTIME_ID_EXHAUSTED,"Runtime IDs exhausted"));
    }
};
struct StartupDependencies {
    std::shared_ptr<IBackendFactory> factory;
    std::function<Result<std::thread,RuntimeError>(WorkerRole,std::function<void()>)> launch;
    std::function<bool(WorkerRole)> ready_gate;
    std::function<void(WorkerRole,std::stop_token)> work;
    std::shared_ptr<IRuntimeIdSource> ids;
    std::function<Result<void,RuntimeError>(uint64_t)> initialized;
};
class StartupSession {
    using R = Result<void,RuntimeError>;
    std::mutex& coordinator_;
    StartupDependencies deps_;
    RuntimeState state_ = RuntimeState::CREATED;
    std::unique_ptr<IExecutionBackend> backend_;
    OperationRegistry operations_;
    uint64_t id_ = 0;
    std::thread completion_, scheduler_;
    std::stop_source completion_stop_, scheduler_stop_;
    std::condition_variable_any cv_;
    bool reported_[2]{}, ready_[2]{}, run_ = false;
    static size_t index(WorkerRole r) { return r == WorkerRole::Completion ? 0 : 1; }
    static R error(ErrorCode c) { return R::err(make_error(c,"Startup session failure")); }
    void entry(WorkerRole role, std::stop_token stop) noexcept {
        bool ready = false;
        try { ready = !deps_.ready_gate || deps_.ready_gate(role); } catch (...) {}
        {
            std::unique_lock lock(coordinator_);
            reported_[index(role)] = true;
            ready_[index(role)] = ready;
            cv_.notify_all();
            if (!ready) return;
            cv_.wait(lock,stop,[&]{return run_;});
            if (stop.stop_requested()) return;
        }
        try {
            if (deps_.work) deps_.work(role,stop);
            else {
                std::unique_lock lock(coordinator_);
                cv_.wait(lock,stop,[]{return false;});
            }
        } catch (...) { /* Thread entry never propagates user exceptions. */ }
    }
    R launch(WorkerRole role) {
        auto token = role == WorkerRole::Completion ? completion_stop_.get_token() : scheduler_stop_.get_token();
        std::function<void()> body = [this,role,token]{entry(role,token);};
        auto result = deps_.launch ? deps_.launch(role,std::move(body)) :
            Result<std::thread,RuntimeError>::ok(std::thread(std::move(body)));
        if (!result) return error(ErrorCode::THREAD_INIT_FAILED);
        auto& thread = role == WorkerRole::Completion ? completion_ : scheduler_;
        thread = std::move(result.value());
        if (!thread.joinable()) return error(ErrorCode::THREAD_INIT_FAILED);
        std::unique_lock lock(coordinator_);
        cv_.wait(lock,[&]{return reported_[index(role)];});
        return ready_[index(role)] ? R::ok() : error(ErrorCode::THREAD_INIT_FAILED);
    }
    R fail(RuntimeError e) {
        // Wake both before any join, then reverse startup order.
        scheduler_stop_.request_stop(); completion_stop_.request_stop(); cv_.notify_all();
        stop_scheduler(); stop_completion(); cleanup_backend();
        { std::lock_guard lock(coordinator_); state_ = RuntimeState::START_FAILED; }
        return R::err(std::move(e));
    }
public:
    StartupSession(std::mutex& coordinator, StartupDependencies deps)
        : coordinator_(coordinator), deps_(std::move(deps)) {}
    StartupSession(const StartupSession&) = delete;
    StartupSession& operator=(const StartupSession&) = delete;
    ~StartupSession() noexcept {
        scheduler_stop_.request_stop(); completion_stop_.request_stop(); cv_.notify_all();
        stop_scheduler(); stop_completion(); cleanup_backend();
    }
    R start() {
        { std::lock_guard lock(coordinator_);
          if (auto e = check_start(state_)) return error(*e);
          state_ = RuntimeState::STARTING; }
        bool launching = false;
        try {
            if (!deps_.factory) return fail(make_error(ErrorCode::BACKEND_INIT_FAILED,"Missing factory"));
            auto b = deps_.factory->create();
            if (!b) return fail(std::move(b.error()));
            backend_ = std::move(b.value());
            if (!backend_) return fail(make_error(ErrorCode::BACKEND_INIT_FAILED,"Null backend"));
            auto init = backend_->initialize();
            if (!init) return fail(std::move(init.error()));
            auto reg = operations_.register_operations(backend_->operations());
            if (!reg) return fail(std::move(reg.error()));
            auto valid = operations_.validate(CommandDescriptor{{"test.noop",1},{},{},{},{}});
            if (!valid) return fail(std::move(valid.error()));
            NativeRuntimeIdSource native;
            auto id = deps_.ids ? deps_.ids->next() : native.next();
            if (!id) return fail(std::move(id.error()));
            if (id.value() == 0 || id.value() == UINT64_MAX)
                return fail(make_error(ErrorCode::RUNTIME_ID_EXHAUSTED,"Invalid runtime ID"));
            { std::lock_guard lock(coordinator_); id_ = id.value(); }
            if (deps_.initialized) {
                auto initialized = deps_.initialized(id_);
                if (!initialized) return fail(std::move(initialized.error()));
            }
            launching = true;
            auto c = launch(WorkerRole::Completion);
            if (!c) return fail(std::move(c.error()));
            auto s = launch(WorkerRole::Scheduler);
            if (!s) return fail(std::move(s.error()));
            { std::lock_guard lock(coordinator_); state_ = RuntimeState::RUNNING; run_ = true; }
            cv_.notify_all();
            return R::ok();
        } catch (const std::bad_alloc&) {
            return fail(make_error(launching ? ErrorCode::THREAD_INIT_FAILED : ErrorCode::OUT_OF_MEMORY,"Allocation failure"));
        } catch (...) {
            return fail(make_error(launching ? ErrorCode::THREAD_INIT_FAILED : ErrorCode::BACKEND_INIT_FAILED,"Initialization exception"));
        }
    }
    RuntimeState state() const { std::lock_guard lock(coordinator_); return state_; }
    RuntimeState state_locked() const { return state_; }
    uint64_t runtime_id() const { std::lock_guard lock(coordinator_); return id_; }
    IExecutionBackend* backend() { return backend_.get(); }
    OperationRegistry& operations() { return operations_; }
    size_t worker_count() const { return size_t(completion_.joinable()) + size_t(scheduler_.joinable()); }
    R begin_shutdown() {
        std::lock_guard lock(coordinator_);
        return begin_shutdown_locked();
    }
    R begin_shutdown_locked() {
        if (auto e = check_shutdown(state_)) return error(*e);
        state_ = RuntimeState::SHUTTING_DOWN; return R::ok();
    }
    R begin_draining() {
        std::lock_guard lock(coordinator_);
        if (state_ != RuntimeState::SHUTTING_DOWN) return error(ErrorCode::INVALID_STATE);
        state_ = RuntimeState::DRAINING; return R::ok();
    }
    R complete_shutdown() {
        std::lock_guard lock(coordinator_);
        if (state_ != RuntimeState::DRAINING) return error(ErrorCode::INVALID_STATE);
        state_ = RuntimeState::STOPPED; return R::ok();
    }
    void stop_scheduler() noexcept {
        scheduler_stop_.request_stop(); cv_.notify_all();
        if (scheduler_.joinable()) scheduler_.join();
    }
    void stop_completion() noexcept {
        completion_stop_.request_stop(); cv_.notify_all();
        if (completion_.joinable()) completion_.join();
    }
    void cleanup_backend() noexcept {
        if (backend_) { backend_->quiesce(); backend_->cleanup(); backend_.reset(); }
        operations_.clear();
    }
};
}
