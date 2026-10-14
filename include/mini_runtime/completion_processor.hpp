#pragma once
#include "work_store.hpp"

namespace mini_runtime::detail {
class CompletionProcessor {
    WorkStore& store_;
public:
    explicit CompletionProcessor(WorkStore& store) noexcept : store_(store) {}
    bool process_one() {
        std::lock_guard lock(store_.coordinator_);
        auto* node = store_.completion_head_;
        if (!node || node->state != NodeState::QUEUED) return false;
        // Acquire ownership before detaching or erasing either registry.
        auto context = store_.submissions_.at(node->owner->submission_token);
        store_.completion_head_ = node->next;
        if (!store_.completion_head_) store_.completion_tail_ = nullptr;
        --store_.completion_size_;
        node->next = nullptr;
        node->state = NodeState::PROCESSING;
        EventStatus status = EventStatus::COMPLETED;
        std::optional<RuntimeError> failure;
        switch (node->payload) {
        case CompletionReason::Success: break;
        case CompletionReason::Failure:
            status = EventStatus::FAILED;
            failure = make_error(ErrorCode::BACKEND_FAILURE, "Backend execution failed", context->job_id);
            break;
        case CompletionReason::Rejected:
            status = EventStatus::FAILED;
            failure = make_error(ErrorCode::BACKEND_REJECTED, "Backend submission rejected", context->job_id);
            break;
        case CompletionReason::Cancelled:
            status = EventStatus::CANCELLED;
            failure = make_error(ErrorCode::SHUTDOWN_CANCELLED, "Submission cancelled during shutdown", context->job_id);
            break;
        }
        store_.events_.complete_locked(context->event, status, std::move(failure));
        store_.jobs_.erase(context->job_id);
        store_.submissions_.erase(context->submission_token);
        node->state = NodeState::RETIRED;
        context->counters->finalizations.fetch_add(1);
        return true;
    }
};
}
