#include "mini_runtime/mock_backend.hpp"
#include "mini_runtime/runtime.hpp"

#include <iostream>
#include <memory>
#include <string_view>

namespace {
int report_error(std::string_view stage, const mini_runtime::RuntimeError& error) {
    std::cerr << stage << " failed: code=" << static_cast<int>(error.code)
              << " message=" << error.message << '\n';
    return 1;
}
}

int main() {
    using namespace mini_runtime;

    auto control = std::make_shared<testing::MockControl>();
    auto factory = std::make_shared<testing::MockBackendFactory>(control);
    Runtime runtime(factory);

    auto started = runtime.start();
    if (!started) return report_error("start", started.error());
    std::cout << "Runtime started: runtime_id=" << runtime.runtime_id() << '\n';

    // This baseline supports test.noop v1 with all other fields empty.
    CommandDescriptor command{{"test.noop", 1}, {}, {}, {}, {}};
    auto submitted = runtime.submit(command);
    if (!submitted) return report_error("submit", submitted.error());
    const auto event = submitted.value();
    std::cout << "Submitted test.noop v1: event_id=" << event.event_id << '\n';

    auto completed = runtime.wait(event);
    if (!completed) return report_error("wait", completed.error());
    if (completed.value().status != EventStatus::COMPLETED) {
        if (completed.value().failure)
            return report_error("operation", *completed.value().failure);
        std::cerr << "Operation did not complete successfully\n";
        return 1;
    }
    std::cout << "Event completed: COMPLETED\n";

    auto released = runtime.release_event(event);
    if (!released) return report_error("release_event", released.error());

    auto stopped = runtime.shutdown();
    if (!stopped) return report_error("shutdown", stopped.error());
    if (runtime.state() != RuntimeState::STOPPED) {
        std::cerr << "Runtime did not reach STOPPED\n";
        return 1;
    }
    std::cout << "Event released; runtime stopped: STOPPED\n";
}
