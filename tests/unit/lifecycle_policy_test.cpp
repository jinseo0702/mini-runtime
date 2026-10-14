#include "mini_runtime/lifecycle_policy.hpp"

#include <array>
#include <cassert>
#include <optional>
#include <type_traits>

using mini_runtime::ErrorCode;
using mini_runtime::RuntimeState;
using mini_runtime::check_start;
using mini_runtime::check_shutdown;

static_assert(std::is_same_v<decltype(check_start(RuntimeState::CREATED)),
                             std::optional<ErrorCode>>);
static_assert(std::is_same_v<decltype(check_shutdown(RuntimeState::CREATED)),
                             std::optional<ErrorCode>>);
static_assert(noexcept(check_start(RuntimeState::CREATED)));
static_assert(noexcept(check_shutdown(RuntimeState::CREATED)));

int main() {
    struct Row {
        RuntimeState state;
        std::optional<ErrorCode> start;
        std::optional<ErrorCode> shutdown;
    };
    const std::array<Row, 7> table{{
        {RuntimeState::CREATED, std::nullopt, ErrorCode::INVALID_STATE},
        {RuntimeState::STARTING, ErrorCode::START_IN_PROGRESS, ErrorCode::START_IN_PROGRESS},
        {RuntimeState::RUNNING, ErrorCode::ALREADY_RUNNING, std::nullopt},
        {RuntimeState::START_FAILED, ErrorCode::INVALID_STATE, ErrorCode::INVALID_STATE},
        {RuntimeState::SHUTTING_DOWN, ErrorCode::INVALID_STATE, ErrorCode::SHUTDOWN_IN_PROGRESS},
        {RuntimeState::DRAINING, ErrorCode::INVALID_STATE, ErrorCode::SHUTDOWN_IN_PROGRESS},
        {RuntimeState::STOPPED, ErrorCode::INVALID_STATE, ErrorCode::ALREADY_STOPPED}
    }};
    for (const auto& row : table) {
        auto state = row.state;
        for (int repetition = 0; repetition < 10; ++repetition) {
            assert(check_start(state) == row.start);
            assert(state == row.state);
            assert(check_shutdown(state) == row.shutdown);
            assert(state == row.state);
        }
    }
}
