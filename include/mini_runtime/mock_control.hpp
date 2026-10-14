#pragma once
#include "core_types.hpp"
#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
namespace mini_runtime::testing {
enum class MockMode { Async, Immediate, Reject, Fail, Duplicate, RejectWithCallback };
enum class MockPoint { BeforeExecute, BeforeCallback, AfterCallback, BeforeSubmitReturn, QuiesceEntered };
struct MockStats {
    size_t initialization_calls=0, cleanup_calls=0, callbacks=0, pending=0, active=0,
           threads_started=0, threads_joined=0;
};
class MockBackend;
class MockControl {
    friend class MockBackend;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::array<bool,5> held_{}, reached_{};
    MockMode mode_=MockMode::Async;
    bool fail_=false;
    MockStats stats_;
    size_t submitting_=0;
    std::optional<CommandDescriptor> last_;
    std::function<void()> enter_, leave_;
    void point(MockPoint p) {
        std::unique_lock lock(mutex_); auto i=static_cast<size_t>(p);
        reached_[i]=true; cv_.notify_all();
        cv_.wait(lock,[&]{return !held_[i];});
    }
public:
    void set_mode(MockMode m) { std::lock_guard lock(mutex_); mode_=m; }
    void fail_initialize(bool b) { std::lock_guard lock(mutex_); fail_=b; }
    void hold(MockPoint p) { std::lock_guard lock(mutex_); auto i=static_cast<size_t>(p); held_[i]=true; reached_[i]=false; }
    void wait_reached(MockPoint p) { std::unique_lock lock(mutex_); cv_.wait(lock,[&]{return reached_[static_cast<size_t>(p)];}); }
    void release(MockPoint p) { std::lock_guard lock(mutex_); held_[static_cast<size_t>(p)]=false; cv_.notify_all(); }
    void wait_idle() { std::unique_lock lock(mutex_); cv_.wait(lock,[&]{return !stats_.pending && !stats_.active && !submitting_;}); }
    // Install before work; hooks must not throw and must not be changed during work.
    void set_callback_hooks(std::function<void()> enter,std::function<void()> leave) {
        std::lock_guard lock(mutex_); enter_=std::move(enter); leave_=std::move(leave);
    }
    MockStats stats() { std::lock_guard lock(mutex_); return stats_; }
    std::optional<CommandDescriptor> last_command() { std::lock_guard lock(mutex_); return last_; }
};
}
