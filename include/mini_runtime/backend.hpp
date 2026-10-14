#pragma once
#include "core_types.hpp"
#include "result.hpp"
#include "runtime_error.hpp"
#include <algorithm>
#include <cstddef>
#include <new>
namespace mini_runtime {
// No backend call or join occurs under Coordinator. initialize emits no callbacks.
// Accepted retains the owned command and callable through exactly one callback;
// Rejected emits zero callbacks.
class IExecutionBackend {
public:
    virtual ~IExecutionBackend() = default;
    virtual Result<void, RuntimeError> initialize() = 0;
    virtual const std::vector<OperationSchema>& operations() const noexcept = 0;
    virtual SubmitDecision submit(const BackendCommand&, CompletionCallback) noexcept = 0;
    // Idempotent: wait for callbacks to RETURN and callable references to be
    // released; no later callback is permitted. Separate from cleanup.
    virtual void quiesce() noexcept = 0;
    virtual void cleanup() noexcept = 0; // Idempotent resource cleanup.
};
class IBackendFactory {
public:
    virtual ~IBackendFactory() = default;
    virtual Result<std::unique_ptr<IExecutionBackend>, RuntimeError> create() = 0;
};
namespace detail {
// Caller supplies synchronization using the Runtime Coordinator.
class OperationRegistry {
    std::vector<OperationSchema> schemas_;
public:
    Result<void, RuntimeError> register_operations(const std::vector<OperationSchema>& batch) {
        try {
            auto candidate = schemas_;
            for (const auto& schema : batch) {
                auto found = std::find_if(candidate.begin(), candidate.end(),
                    [&](const auto& existing) { return existing.key == schema.key; });
                if (found != candidate.end()) {
                    if (*found != schema)
                        return Result<void, RuntimeError>::err(
                            make_error(ErrorCode::SCHEMA_CONFLICT, "Conflicting schema"));
                } else {
                    candidate.push_back(schema);
                }
            }
            schemas_.swap(candidate);
            return Result<void, RuntimeError>::ok();
        } catch (const std::bad_alloc&) {
            return Result<void, RuntimeError>::err(
                make_error(ErrorCode::OUT_OF_MEMORY, "Registration allocation failed"));
        }
    }
    Result<void, RuntimeError> validate(const CommandDescriptor& command) {
        try {
            if (!contains(command.operation))
                return Result<void, RuntimeError>::err(
                    make_error(ErrorCode::UNKNOWN_OPERATION, "Unregistered operation"));
            if (command.operation.name != "test.noop" || command.operation.version != 1 ||
                !command.parameters.empty() || !command.inputs.empty() ||
                !command.outputs.empty() || !command.dependencies.empty())
                return Result<void, RuntimeError>::err(
                    make_error(ErrorCode::INVALID_PARAMETER, "Unsupported command contents"));
            return Result<void, RuntimeError>::ok();
        } catch (const std::bad_alloc&) {
            return Result<void, RuntimeError>::err(
                make_error(ErrorCode::OUT_OF_MEMORY, "Validation allocation failed"));
        }
    }
    bool contains(const OperationKey& key) {
        return std::any_of(schemas_.begin(), schemas_.end(),
            [&](const auto& schema) { return schema.key == key; });
    }
    void clear() noexcept { schemas_.clear(); }
    size_t size() { return schemas_.size(); }
};
}
}
