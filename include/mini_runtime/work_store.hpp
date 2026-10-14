#pragma once
#include "backend.hpp"
#include "event_registry.hpp"
#include <deque>

namespace mini_runtime::detail {
enum class NodeState { EMPTY, STAGED, QUEUED, PROCESSING, RETIRED };
enum class SubmissionPhase { READY, SUBMITTING, ACCEPTED, REJECTED };
enum class CompletionReason { Success, Failure, Rejected, Cancelled };
struct SubmissionContext;
struct CompletionNode {
    CompletionNode* next = nullptr;
    SubmissionContext* const owner;
    NodeState state = NodeState::EMPTY;
    CompletionReason payload = CompletionReason::Success;
    explicit CompletionNode(SubmissionContext* p) noexcept : owner(p) {}
    CompletionNode(const CompletionNode&) = delete;
    CompletionNode& operator=(const CompletionNode&) = delete;
};
struct ContextCounters {
    std::atomic<size_t> live{0}, finalizations{0}, duplicates{0};
};
struct SubmissionContext {
    const uint64_t job_id, submission_token;
    const std::shared_ptr<const CommandDescriptor> command;
    const std::shared_ptr<EventRecord> event;
    const EventHandle handle;
    const std::shared_ptr<ContextCounters> counters;
    CompletionNode node;
    SubmissionPhase phase = SubmissionPhase::READY;
    bool callback_seen = false;
    SubmissionContext(uint64_t j, uint64_t t, std::shared_ptr<const CommandDescriptor> c,
                      std::shared_ptr<EventRecord> e, EventHandle h,
                      std::shared_ptr<ContextCounters> counts) noexcept
        : job_id(j), submission_token(t), command(std::move(c)), event(std::move(e)),
          handle(h), counters(std::move(counts)), node(this) { counters->live.fetch_add(1); }
    ~SubmissionContext() { counters->live.fetch_sub(1); }
    SubmissionContext(const SubmissionContext&) = delete;
    SubmissionContext& operator=(const SubmissionContext&) = delete;
};
struct Job {
    std::shared_ptr<const CommandDescriptor> command;
    std::shared_ptr<SubmissionContext> context;
    CompletionCallback callback;
};
class ICompletionSink {
public:
    virtual ~ICompletionSink() = default;
    virtual void complete(SubmissionContext&, BackendResultCode) noexcept = 0;
};
enum class AdmissionPoint { Command, Event, Context, JobRegistry, SubmissionRegistry, ReadyQueue };
class IAllocationGate {
public:
    virtual ~IAllocationGate() = default;
    virtual bool allow(AdmissionPoint) noexcept = 0;
};
struct CoreStats {
    size_t jobs, submissions, ready, completion_queue, contexts_live, finalizations,
           duplicate_callbacks, external_events, live_event_records;
};
class CompletionIngress;
class Dispatcher;
class CompletionProcessor;
class WorkStore {
    friend class CompletionIngress;
    friend class Dispatcher;
    friend class CompletionProcessor;
    std::mutex& coordinator_;
    EventRegistry& events_;
    OperationRegistry& operations_;
    IAllocationGate* gate_;
    ICompletionSink* sink_ = nullptr;
    uint64_t next_job_id_, next_token_;
    std::shared_ptr<ContextCounters> counters_ = std::make_shared<ContextCounters>();
    std::unordered_map<uint64_t, Job> jobs_;
    std::unordered_map<uint64_t, std::shared_ptr<SubmissionContext>> submissions_;
    std::deque<uint64_t> ready_;
    CompletionNode* completion_head_ = nullptr;
    CompletionNode* completion_tail_ = nullptr;
    size_t completion_size_ = 0;
    void allocation(AdmissionPoint p) { if (gate_ && !gate_->allow(p)) throw std::bad_alloc(); }
    static Result<EventHandle,RuntimeError> fail(ErrorCode c) {
        return Result<EventHandle,RuntimeError>::err(make_error(c, {}));
    }
public:
    // Sink must outlive this store and every backend callable; destruction requires quiescence/joins.
    WorkStore(std::mutex& m, EventRegistry& e, OperationRegistry& o,
              IAllocationGate* g=nullptr, uint64_t j=1, uint64_t t=1)
        : coordinator_(m), events_(e), operations_(o), gate_(g), next_job_id_(j), next_token_(t) {}
    WorkStore(const WorkStore&) = delete;
    WorkStore& operator=(const WorkStore&) = delete;
    void attach_sink(ICompletionSink& sink) {
        std::lock_guard lock(coordinator_);
        if (sink_) throw std::logic_error("Sink already attached");
        sink_ = &sink;
    }
    Result<EventHandle,RuntimeError> admit(const CommandDescriptor& c) {
        std::lock_guard lock(coordinator_); return admit_locked(c);
    }
    Result<EventHandle,RuntimeError> admit_locked(const CommandDescriptor& c) {
        if (!sink_) return fail(ErrorCode::INVALID_STATE);
        auto valid = operations_.validate(c);
        if (!valid) return Result<EventHandle,RuntimeError>::err(std::move(valid.error()));
        const auto max = std::numeric_limits<uint64_t>::max();
        if (!next_job_id_ || next_job_id_ == max) return fail(ErrorCode::JOB_ID_EXHAUSTED);
        if (!next_token_ || next_token_ == max) return fail(ErrorCode::SUBMISSION_ID_EXHAUSTED);
        const auto j = next_job_id_++, t = next_token_++;
        std::optional<EventHandle> handle;
        try {
            allocation(AdmissionPoint::Command);
            auto command = std::make_shared<const CommandDescriptor>(c);
            allocation(AdmissionPoint::Event);
            auto created = events_.create_locked();
            if (!created) return Result<EventHandle,RuntimeError>::err(std::move(created.error()));
            handle = created.value();
            auto record = events_.acquire_locked(*handle).value();
            allocation(AdmissionPoint::Context);
            auto context = std::make_shared<SubmissionContext>(j,t,command,record,*handle,counters_);
            CompletionCallback callback = [context, sink=sink_](BackendResultCode code) noexcept {
                sink->complete(*context,code);
            };
            allocation(AdmissionPoint::JobRegistry);
            jobs_.emplace(j, Job{command,context,std::move(callback)});
            allocation(AdmissionPoint::SubmissionRegistry);
            submissions_.emplace(t,context);
            allocation(AdmissionPoint::ReadyQueue);
            ready_.push_back(j); // Final commit; no throwing operations follow.
            return Result<EventHandle,RuntimeError>::ok(*handle);
        } catch (const std::bad_alloc&) {
            submissions_.erase(t);
            jobs_.erase(j);
            if (handle) events_.release_event_locked(*handle);
            return fail(ErrorCode::OUT_OF_MEMORY);
        }
    }
    CoreStats inspect() const {
        std::lock_guard lock(coordinator_);
        return {jobs_.size(),submissions_.size(),ready_.size(),completion_size_,
                counters_->live.load(),counters_->finalizations.load(),counters_->duplicates.load(),
                events_.external_size_locked(),events_.live_records_locked()};
    }
    std::optional<NodeState> node_state(EventHandle h) const {
        std::lock_guard lock(coordinator_);
        for (const auto& [id,c] : submissions_) {
            (void)id;
            if (c->handle.runtime_id == h.runtime_id && c->handle.event_id == h.event_id) return c->node.state;
        }
        return std::nullopt;
    }
    bool has_ready() const { std::lock_guard lock(coordinator_); return !ready_.empty(); }
    bool has_completion() const { std::lock_guard lock(coordinator_); return completion_head_ != nullptr; }
    bool empty() const {
        std::lock_guard lock(coordinator_);
        return jobs_.empty() && submissions_.empty() && ready_.empty() && !completion_head_;
    }
};
}
