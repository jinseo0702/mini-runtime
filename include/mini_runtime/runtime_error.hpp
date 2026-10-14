#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mini_runtime {
enum class ErrorCode {
    INVALID_STATE, START_IN_PROGRESS, ALREADY_RUNNING,
    SHUTDOWN_IN_PROGRESS, ALREADY_STOPPED,
    INVALID_EVENT, INVALID_PARAMETER, UNKNOWN_OPERATION, OUT_OF_MEMORY,
    BACKEND_INIT_FAILED, THREAD_INIT_FAILED, BACKEND_REJECTED,
    BACKEND_FAILURE, SHUTDOWN_CANCELLED,
    SCHEMA_CONFLICT, RUNTIME_ID_EXHAUSTED, EVENT_ID_EXHAUSTED,
    JOB_ID_EXHAUSTED, SUBMISSION_ID_EXHAUSTED
};
struct RuntimeError {
    ErrorCode code;
    std::string message;
    std::optional<uint64_t> job_id;
};
inline RuntimeError make_error(ErrorCode code, std::string_view message,
                               std::optional<uint64_t> job_id = std::nullopt) noexcept {
    try {
        return RuntimeError{code, std::string(message), job_id};
    } catch (...) {
        return RuntimeError{code, std::string{}, job_id};
    }
}
} // namespace mini_runtime
