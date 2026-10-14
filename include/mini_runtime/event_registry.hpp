#pragma once
#include "result.hpp"
#include "runtime_error.hpp"
#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace mini_runtime {
struct EventHandle { uint64_t runtime_id, event_id; };
enum class EventStatus { PENDING, COMPLETED, FAILED, CANCELLED };
struct EventSnapshot { EventStatus status; std::optional<RuntimeError> failure; };
namespace detail {
// Shared accounting outlives the registry if an internal owner retains a record.
class EventRecord {
    friend class EventRegistry;
    std::shared_ptr<std::atomic<size_t>> live_;
    EventSnapshot snapshot_{EventStatus::PENDING, std::nullopt};
    std::condition_variable changed_;
public:
    explicit EventRecord(std::shared_ptr<std::atomic<size_t>> live) : live_(std::move(live)) {
        live_->fetch_add(1);
    }
    ~EventRecord() { live_->fetch_sub(1); }
    EventRecord(const EventRecord&) = delete;
    EventRecord& operator=(const EventRecord&) = delete;
};
class EventRegistry {
    std::mutex& coordinator_;
    uint64_t runtime_id_, next_event_id_;
    std::shared_ptr<std::atomic<size_t>> live_ = std::make_shared<std::atomic<size_t>>(0);
    std::unordered_map<uint64_t, std::shared_ptr<EventRecord>> records_;
    static RuntimeError error(ErrorCode code) noexcept { return make_error(code, {}); }
    bool owns(const std::shared_ptr<EventRecord>& p) const noexcept {
        return p && p->live_ == live_;
    }
    Result<EventSnapshot,RuntimeError> snapshot_locked(const std::shared_ptr<EventRecord>& p) {
        try { return Result<EventSnapshot,RuntimeError>::ok(p->snapshot_); }
        catch (const std::bad_alloc&) {
            return Result<EventSnapshot,RuntimeError>::err(error(ErrorCode::OUT_OF_MEMORY));
        }
    }
    Result<EventSnapshot,RuntimeError> wait_impl(std::shared_ptr<EventRecord> p,
                                                std::unique_lock<std::mutex>& lock) {
        if (!owns(p)) return Result<EventSnapshot,RuntimeError>::err(error(ErrorCode::INVALID_EVENT));
        p->changed_.wait(lock, [&]{ return p->snapshot_.status != EventStatus::PENDING; });
        return snapshot_locked(p);
    }
public:
    explicit EventRegistry(std::mutex& coordinator, uint64_t runtime_id=0, uint64_t next_event_id=1)
        : coordinator_(coordinator), runtime_id_(runtime_id), next_event_id_(next_event_id) {}
    EventRegistry(const EventRegistry&) = delete;
    EventRegistry& operator=(const EventRegistry&) = delete;
    Result<void,RuntimeError> bind_runtime_id_locked(uint64_t id) {
        if (runtime_id_ != 0 || !records_.empty() || live_records_locked() != 0 ||
            id == 0 || id == std::numeric_limits<uint64_t>::max())
            return Result<void,RuntimeError>::err(error(ErrorCode::INVALID_STATE));
        runtime_id_ = id;
        return Result<void,RuntimeError>::ok();
    }
    Result<void,RuntimeError> bind_runtime_id(uint64_t id) {
        std::lock_guard lock(coordinator_); return bind_runtime_id_locked(id);
    }
    Result<EventHandle,RuntimeError> create_locked() {
        if (runtime_id_ == 0 || runtime_id_ == std::numeric_limits<uint64_t>::max())
            return Result<EventHandle,RuntimeError>::err(error(ErrorCode::INVALID_STATE));
        if (next_event_id_ == 0 || next_event_id_ == std::numeric_limits<uint64_t>::max())
            return Result<EventHandle,RuntimeError>::err(error(ErrorCode::EVENT_ID_EXHAUSTED));
        const auto id = next_event_id_++;
        try {
            auto p = std::make_shared<EventRecord>(live_);
            records_.emplace(id, std::move(p));
            return Result<EventHandle,RuntimeError>::ok({runtime_id_, id});
        } catch (const std::bad_alloc&) {
            return Result<EventHandle,RuntimeError>::err(error(ErrorCode::OUT_OF_MEMORY));
        }
    }
    Result<EventHandle,RuntimeError> create() {
        std::lock_guard lock(coordinator_); return create_locked();
    }
    Result<std::shared_ptr<EventRecord>,RuntimeError> acquire_locked(EventHandle h) {
        auto it = records_.find(h.event_id);
        if (h.runtime_id != runtime_id_ || h.runtime_id == 0 || it == records_.end())
            return Result<std::shared_ptr<EventRecord>,RuntimeError>::err(error(ErrorCode::INVALID_EVENT));
        return Result<std::shared_ptr<EventRecord>,RuntimeError>::ok(it->second);
    }
    Result<std::shared_ptr<EventRecord>,RuntimeError> acquire(EventHandle h) {
        std::lock_guard lock(coordinator_); return acquire_locked(h);
    }
    Result<EventSnapshot,RuntimeError> query_locked(EventHandle h) {
        auto p = acquire_locked(h);
        if (!p) return Result<EventSnapshot,RuntimeError>::err(std::move(p.error()));
        return snapshot_locked(p.value());
    }
    Result<EventSnapshot,RuntimeError> query(EventHandle h) {
        std::lock_guard lock(coordinator_); return query_locked(h);
    }
    Result<EventSnapshot,RuntimeError> wait(EventHandle h) {
        std::unique_lock lock(coordinator_);
        auto p = acquire_locked(h);
        if (!p) return Result<EventSnapshot,RuntimeError>::err(std::move(p.error()));
        return wait_impl(std::move(p.value()), lock);
    }
    Result<EventSnapshot,RuntimeError> wait_pinned(std::shared_ptr<EventRecord> p) {
        std::unique_lock lock(coordinator_); return wait_impl(std::move(p), lock);
    }
    Result<void,RuntimeError> release_event_locked(EventHandle h) {
        auto p = acquire_locked(h);
        if (!p) return Result<void,RuntimeError>::err(std::move(p.error()));
        records_.erase(h.event_id);
        return Result<void,RuntimeError>::ok();
    }
    Result<void,RuntimeError> release_event(EventHandle h) {
        std::lock_guard lock(coordinator_); return release_event_locked(h);
    }
    bool complete_locked(const std::shared_ptr<EventRecord>& p, EventStatus status,
                         std::optional<RuntimeError> failure) {
        if (!owns(p) || status == EventStatus::PENDING || p->snapshot_.status != EventStatus::PENDING)
            return false;
        if (status == EventStatus::COMPLETED) failure.reset();
        if (status == EventStatus::CANCELLED) {
            auto job = failure ? failure->job_id : std::nullopt;
            failure = make_error(ErrorCode::SHUTDOWN_CANCELLED, {}, job);
        }
        p->snapshot_.failure = std::move(failure);
        p->snapshot_.status = status;
        p->changed_.notify_all();
        return true;
    }
    bool complete(const std::shared_ptr<EventRecord>& p, EventStatus status,
                  std::optional<RuntimeError> failure) {
        std::lock_guard lock(coordinator_); return complete_locked(p, status, std::move(failure));
    }
    size_t external_size_locked() const noexcept { return records_.size(); }
    size_t live_records_locked() const noexcept { return live_->load(); }
    size_t external_size() const { std::lock_guard lock(coordinator_); return external_size_locked(); }
    size_t live_records() const { std::lock_guard lock(coordinator_); return live_records_locked(); }
};
} // namespace detail
} // namespace mini_runtime
