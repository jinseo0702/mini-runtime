#pragma once

#include <optional>
#include "mini_runtime/runtime_error.hpp"

namespace mini_runtime {

enum class RuntimeState {
    CREATED, STARTING, RUNNING, START_FAILED,
    SHUTTING_DOWN, DRAINING, STOPPED
};

inline std::optional<ErrorCode> check_start(RuntimeState state) noexcept {
    switch (state) {
    case RuntimeState::CREATED: return std::nullopt;
    case RuntimeState::STARTING: return ErrorCode::START_IN_PROGRESS;
    case RuntimeState::RUNNING: return ErrorCode::ALREADY_RUNNING;
    case RuntimeState::START_FAILED:
    case RuntimeState::SHUTTING_DOWN:
    case RuntimeState::DRAINING:
    case RuntimeState::STOPPED: return ErrorCode::INVALID_STATE;
    }
    return ErrorCode::INVALID_STATE;
}

inline std::optional<ErrorCode> check_shutdown(RuntimeState state) noexcept {
    switch (state) {
    case RuntimeState::CREATED: return ErrorCode::INVALID_STATE;
    case RuntimeState::STARTING: return ErrorCode::START_IN_PROGRESS;
    case RuntimeState::RUNNING: return std::nullopt;
    case RuntimeState::START_FAILED: return ErrorCode::INVALID_STATE;
    case RuntimeState::SHUTTING_DOWN:
    case RuntimeState::DRAINING: return ErrorCode::SHUTDOWN_IN_PROGRESS;
    case RuntimeState::STOPPED: return ErrorCode::ALREADY_STOPPED;
    }
    return ErrorCode::INVALID_STATE;
}

} // namespace mini_runtime
