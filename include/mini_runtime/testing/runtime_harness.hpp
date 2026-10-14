#pragma once
#include "../runtime.hpp"
namespace mini_runtime::testing {
struct RuntimeTestOptions {
    detail::StartupDependencies startup;
    detail::IAllocationGate* allocation_gate = nullptr;
    uint64_t next_event_id=1, next_job_id=1, next_token=1;
    std::function<void(std::stop_token)> before_schedule;
    // Installed before start; must not throw. Empty for native Runtime.
    std::function<void()> after_dispatch;
};
inline std::unique_ptr<Runtime> make_runtime(std::shared_ptr<IBackendFactory> factory,RuntimeTestOptions options) {
    return std::unique_ptr<Runtime>(new Runtime(std::move(factory),std::move(options.startup),
        options.allocation_gate,options.next_event_id,options.next_job_id,options.next_token,
        std::move(options.before_schedule),std::move(options.after_dispatch)));
}
inline detail::CoreStats inspect(const Runtime& runtime) { return runtime.store_.inspect(); }
inline std::optional<detail::NodeState> node_state(const Runtime& runtime,EventHandle h) { return runtime.store_.node_state(h); }
}
