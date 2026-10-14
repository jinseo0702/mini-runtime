#pragma once
#include "backend.hpp"
#include "mock_control.hpp"
#include <deque>
#include <thread>
namespace mini_runtime::testing {
class MockBackend final : public IExecutionBackend {
    struct Work { BackendCommand command; CompletionCallback callback; MockMode mode; };
    std::shared_ptr<MockControl> c_;
    std::deque<Work> queue_;
    std::thread worker_;
    bool initialized_=false, closed_=false, stop_=false, cleaned_=false;
    // Serializes initialization and cleanup/join, independently of callback state.
    std::mutex lifecycle_;
    const std::vector<OperationSchema> operations_{{{"test.noop",1},{},false,false}};
    void execute(Work& w) noexcept {
        c_->point(MockPoint::BeforeExecute);
        const int count=w.mode==MockMode::Duplicate ? 2 : 1;
        for(int i=0;i<count;++i) {
            c_->point(MockPoint::BeforeCallback);
            { std::lock_guard lock(c_->mutex_); ++c_->stats_.callbacks; }
            if(c_->enter_) c_->enter_();
            w.callback(w.mode==MockMode::Fail ? BackendResultCode::Failure : BackendResultCode::Success);
            if(c_->leave_) c_->leave_();
            c_->point(MockPoint::AfterCallback);
        }
        w.callback={}; w.command.command.reset();
    }
    void run() noexcept {
        for(;;) {
            std::unique_lock lock(c_->mutex_);
            c_->cv_.wait(lock,[&]{return stop_ || !queue_.empty();});
            if(queue_.empty() && stop_) return;
            Work work=std::move(queue_.front()); queue_.pop_front();
            --c_->stats_.pending; ++c_->stats_.active; lock.unlock();
            execute(work);
            lock.lock(); --c_->stats_.active; c_->cv_.notify_all();
        }
    }
public:
    explicit MockBackend(std::shared_ptr<MockControl> c):c_(std::move(c)) {}
    ~MockBackend() override { cleanup(); }
    Result<void,RuntimeError> initialize() override {
        std::lock_guard life(lifecycle_); std::lock_guard lock(c_->mutex_);
        if(initialized_) return Result<void,RuntimeError>::ok();
        ++c_->stats_.initialization_calls;
        if(c_->fail_ || cleaned_) return Result<void,RuntimeError>::err(make_error(ErrorCode::BACKEND_INIT_FAILED,"Mock initialization failed"));
        try { worker_=std::thread([this]{run();}); }
        catch(...) { return Result<void,RuntimeError>::err(make_error(ErrorCode::BACKEND_INIT_FAILED,"Mock worker failed")); }
        initialized_=true; ++c_->stats_.threads_started;
        return Result<void,RuntimeError>::ok();
    }
    const std::vector<OperationSchema>& operations() const noexcept override { return operations_; }
    SubmitDecision submit(const BackendCommand& command,CompletionCallback callback) noexcept override {
        MockMode mode;
        {
            std::lock_guard lock(c_->mutex_);
            if(!initialized_ || closed_) return SubmitDecision::Rejected;
            mode=c_->mode_;
            try { if(command.command) c_->last_=*command.command; else c_->last_.reset(); }
            catch(...) { return SubmitDecision::Rejected; }
            ++c_->submitting_;
            if(mode==MockMode::Async || mode==MockMode::Fail) {
                try { queue_.push_back({command,std::move(callback),mode}); }
                catch(...) { --c_->submitting_; c_->cv_.notify_all(); return SubmitDecision::Rejected; }
                ++c_->stats_.pending; c_->cv_.notify_all();
            } else if(mode!=MockMode::Reject) ++c_->stats_.active;
        }
        if(mode!=MockMode::Async && mode!=MockMode::Fail) {
            if(mode!=MockMode::Reject) {
                Work work{command,std::move(callback),mode}; execute(work);
                std::lock_guard lock(c_->mutex_); --c_->stats_.active;
            }
            callback={};
        }
        callback={}; // Drop even a moved-from callable before publishing submit completion.
        c_->point(MockPoint::BeforeSubmitReturn);
        { std::lock_guard lock(c_->mutex_); --c_->submitting_; c_->cv_.notify_all(); }
        return mode==MockMode::Reject || mode==MockMode::RejectWithCallback ? SubmitDecision::Rejected : SubmitDecision::Accepted;
    }
    void quiesce() noexcept override {
        { std::lock_guard lock(c_->mutex_); closed_=true; }
        c_->point(MockPoint::QuiesceEntered); c_->wait_idle();
    }
    void cleanup() noexcept override {
        std::lock_guard life(lifecycle_);
        if(cleaned_) return;
        quiesce();
        { std::lock_guard lock(c_->mutex_); stop_=true; c_->cv_.notify_all(); }
        if(worker_.joinable()) { worker_.join(); std::lock_guard lock(c_->mutex_); ++c_->stats_.threads_joined; }
        { std::lock_guard lock(c_->mutex_); ++c_->stats_.cleanup_calls; cleaned_=true; }
    }
};
class MockBackendFactory final : public IBackendFactory {
    std::shared_ptr<MockControl> control_;
public:
    explicit MockBackendFactory(std::shared_ptr<MockControl> c):control_(std::move(c)) {}
    Result<std::unique_ptr<IExecutionBackend>,RuntimeError> create() override {
        try { return Result<std::unique_ptr<IExecutionBackend>,RuntimeError>::ok(std::make_unique<MockBackend>(control_)); }
        catch(const std::bad_alloc&) { return Result<std::unique_ptr<IExecutionBackend>,RuntimeError>::err(make_error(ErrorCode::OUT_OF_MEMORY,"Mock construction failed")); }
    }
};
}
