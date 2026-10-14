#pragma once
#include "work_store.hpp"

namespace mini_runtime::detail {
class CompletionIngress final : public ICompletionSink {
    WorkStore& store_;
public:
    explicit CompletionIngress(WorkStore& store) noexcept : store_(store) {}
    void complete(SubmissionContext& context, BackendResultCode code) noexcept override {
        std::lock_guard lock(store_.coordinator_);
        auto& node = context.node;
        if (context.callback_seen || node.state == NodeState::PROCESSING ||
            node.state == NodeState::RETIRED || node.state == NodeState::QUEUED) {
            context.counters->duplicates.fetch_add(1);
            return;
        }
        context.callback_seen = true;
        if (context.phase == SubmissionPhase::REJECTED) return;
        // Only dispatched contexts can receive a backend callback.
        if (context.phase != SubmissionPhase::SUBMITTING &&
            context.phase != SubmissionPhase::ACCEPTED) return;
        node.payload = code == BackendResultCode::Success
                     ? CompletionReason::Success : CompletionReason::Failure;
        if (context.phase == SubmissionPhase::SUBMITTING)
            node.state = NodeState::STAGED;
        else
            store_.enqueue_locked(node);
    }
};

class Dispatcher {
    WorkStore& store_;
    IExecutionBackend& backend_;
public:
    Dispatcher(WorkStore& store, IExecutionBackend& backend) noexcept
        : store_(store), backend_(backend) {}
    bool dispatch_one() {
        std::shared_ptr<SubmissionContext> context;
        BackendCommand command{};
        CompletionCallback callback;
        {
            std::lock_guard lock(store_.coordinator_);
            if (store_.ready_.empty()) return false;
            auto& job = store_.jobs_.at(store_.ready_.front());
            context = job.context; // Handoff lease survives even immediate completion.
            context->phase = SubmissionPhase::SUBMITTING;
            command = {context->job_id, context->submission_token, std::move(job.command)};
            callback = std::move(job.callback);
            store_.ready_.pop_front();
        }
        const auto decision = backend_.submit(command, std::move(callback));
        {
            std::lock_guard lock(store_.coordinator_);
            auto& node = context->node;
            if (decision == SubmitDecision::Accepted) {
                context->phase = SubmissionPhase::ACCEPTED;
                if (node.state == NodeState::STAGED) store_.enqueue_locked(node);
            } else {
                context->phase = SubmissionPhase::REJECTED;
                // The approved duplicate counter also diagnoses a callback on rejected work.
                if (context->callback_seen) context->counters->duplicates.fetch_add(1);
                node.payload = CompletionReason::Rejected;
                store_.enqueue_locked(node);
            }
        }
        return true;
    }
};
}
