#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>
namespace mini_runtime {
struct OperationKey {
    std::string name;
    uint32_t version;
    bool operator==(const OperationKey&) const = default;
};
enum class ParameterType { Integer, Number, Boolean, String };
using ParameterValue = std::variant<int64_t, double, bool, std::string>;
struct Parameter { std::string name; ParameterValue value; };
struct CommandDescriptor {
    OperationKey operation;
    std::vector<Parameter> parameters;
    std::vector<uint64_t> inputs, outputs, dependencies;
};
struct ParameterSchema {
    std::string name;
    ParameterType type;
    bool required;
    bool operator==(const ParameterSchema&) const = default;
};
struct OperationSchema {
    OperationKey key;
    std::vector<ParameterSchema> parameters;
    bool allows_buffers = false;
    bool allows_dependencies = false;
    bool operator==(const OperationSchema&) const = default;
};
enum class BackendResultCode { Success, Failure };
enum class SubmitDecision { Accepted, Rejected };
struct BackendCommand {
    uint64_t job_id, submission_token;
    std::shared_ptr<const CommandDescriptor> command;
};
using CompletionCallback = std::function<void(BackendResultCode)>;
}
