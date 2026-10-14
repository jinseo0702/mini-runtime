#include "mini_runtime/runtime_error.hpp"
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

namespace {
bool fail_allocation = false;
unsigned failed_allocations = 0;
}
void* operator new(std::size_t size) {
    if (fail_allocation) {
        ++failed_allocations;
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    using namespace mini_runtime;
    static_assert(std::is_aggregate_v<RuntimeError>);
    static_assert(std::is_same_v<decltype(RuntimeError::code), ErrorCode>);
    static_assert(std::is_same_v<decltype(RuntimeError::message), std::string>);
    static_assert(std::is_same_v<decltype(RuntimeError::job_id), std::optional<uint64_t>>);
    using Factory = RuntimeError (*)(ErrorCode, std::string_view, std::optional<uint64_t>) noexcept;
    static_assert(std::is_same_v<decltype(&make_error), Factory>);
    static_assert(noexcept(make_error(ErrorCode::OUT_OF_MEMORY, {})));
    constexpr ErrorCode codes[] = {
        ErrorCode::INVALID_STATE, ErrorCode::START_IN_PROGRESS, ErrorCode::ALREADY_RUNNING,
        ErrorCode::SHUTDOWN_IN_PROGRESS, ErrorCode::ALREADY_STOPPED,
        ErrorCode::INVALID_EVENT, ErrorCode::INVALID_PARAMETER, ErrorCode::UNKNOWN_OPERATION,
        ErrorCode::OUT_OF_MEMORY, ErrorCode::BACKEND_INIT_FAILED, ErrorCode::THREAD_INIT_FAILED,
        ErrorCode::BACKEND_REJECTED, ErrorCode::BACKEND_FAILURE, ErrorCode::SHUTDOWN_CANCELLED
    };
    for (auto code : codes) {
        auto error = make_error(code, "diagnostic");
        assert(error.code == code && error.message == "diagnostic" && !error.job_id);
    }
    auto empty = make_error(ErrorCode::INVALID_EVENT, {}, 0);
    assert(empty.code == ErrorCode::INVALID_EVENT && empty.message.empty() && empty.job_id == 0);
    std::string source(8192, 'x');
    source[10] = '\0';
    auto owned = make_error(ErrorCode::BACKEND_FAILURE, source, std::numeric_limits<uint64_t>::max());
    assert(owned.message == source);
    source[0] = 'y';
    assert(owned.message[0] == 'x');
    assert(owned.job_id == std::numeric_limits<uint64_t>::max());
    for (auto code : codes) {
        for (auto id : {std::optional<uint64_t>{}, std::optional<uint64_t>{0},
                        std::optional<uint64_t>{std::numeric_limits<uint64_t>::max()}}) {
            const auto before = failed_allocations;
            fail_allocation = true;
            auto fallback = make_error(code, source, id);
            fail_allocation = false;
            assert(failed_allocations > before);
            assert(fallback.code == code && fallback.job_id == id && fallback.message.empty());
        }
    }
    auto recovered = make_error(ErrorCode::INVALID_PARAMETER, source);
    assert(recovered.message == source && !recovered.job_id);
}
