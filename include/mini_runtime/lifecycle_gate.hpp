#pragma once
#include <mutex>
#include "mini_runtime/lifecycle_policy.hpp"
#include "mini_runtime/result.hpp"

namespace mini_runtime {
// The mutex owner must outlive the gate. Reentry while holding that same
// nonrecursive mutex is unsupported. No execution resources are owned here.
class LifecycleGate {
    std::mutex& coordinator_;
    RuntimeState state_ = RuntimeState::CREATED;
public:
    explicit LifecycleGate(std::mutex& coordinator) : coordinator_(coordinator) {}
    LifecycleGate(const LifecycleGate&) = delete;
    LifecycleGate(LifecycleGate&&) = delete;
    LifecycleGate& operator=(const LifecycleGate&) = delete;
    LifecycleGate& operator=(LifecycleGate&&) = delete;
    RuntimeState state() const {
        const std::lock_guard<std::mutex> lock(coordinator_);
        return state_;
    }
    Result<void, RuntimeError> begin_start() {
        const std::lock_guard<std::mutex> lock(coordinator_);
        if (auto error = check_start(state_))
            return Result<void, RuntimeError>::err(make_error(*error, "Cannot start"));
        state_ = RuntimeState::STARTING;
        return Result<void, RuntimeError>::ok();
    }
    // Caller confirms actual backend/thread readiness before passing true.
    Result<void, RuntimeError> complete_start(bool success) {
        const std::lock_guard<std::mutex> lock(coordinator_);
        if (state_ != RuntimeState::STARTING)
            return Result<void, RuntimeError>::err(make_error(ErrorCode::INVALID_STATE, "Not starting"));
        state_ = success ? RuntimeState::RUNNING : RuntimeState::START_FAILED;
        return Result<void, RuntimeError>::ok();
    }
    Result<void, RuntimeError> begin_shutdown() {
        const std::lock_guard<std::mutex> lock(coordinator_);
        if (auto error = check_shutdown(state_))
            return Result<void, RuntimeError>::err(make_error(*error, "Cannot shutdown"));
        state_ = RuntimeState::SHUTTING_DOWN;
        return Result<void, RuntimeError>::ok();
    }
    Result<void, RuntimeError> begin_draining() {
        const std::lock_guard<std::mutex> lock(coordinator_);
        if (state_ != RuntimeState::SHUTTING_DOWN)
            return Result<void, RuntimeError>::err(make_error(ErrorCode::INVALID_STATE, "Not shutting down"));
        state_ = RuntimeState::DRAINING;
        return Result<void, RuntimeError>::ok();
    }
    Result<void, RuntimeError> complete_shutdown() {
        const std::lock_guard<std::mutex> lock(coordinator_);
        if (state_ != RuntimeState::DRAINING)
            return Result<void, RuntimeError>::err(make_error(ErrorCode::INVALID_STATE, "Not draining"));
        state_ = RuntimeState::STOPPED;
        return Result<void, RuntimeError>::ok();
    }
};
} // namespace mini_runtime
