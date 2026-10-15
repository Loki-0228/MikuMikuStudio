#include <native_ui/update.hpp>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace native_ui {
void UpdateCoordinator::Request(const UpdateRequest& request) {
    if (!request.flags || request.flags & ~63u) throw std::invalid_argument("invalid update flags");
    auto& entry = entries_[request.id];
    if (entry.known && (request.windowGeneration < entry.window || (request.windowGeneration == entry.window && request.geometryRevision < entry.geometry))) return;
    entry.known = true; entry.window = request.windowGeneration; entry.geometry = request.geometryRevision;
    if (entry.pending) {
        auto& prior = *entry.pending;
        if (request.windowGeneration < prior.windowGeneration ||
            (request.windowGeneration == prior.windowGeneration && request.geometryRevision < prior.geometryRevision)) return;
        auto merged = request;
        if (request.windowGeneration == prior.windowGeneration) {
            merged.flags |= prior.flags;
            merged.deadline = std::min(merged.deadline, prior.deadline);
        }
        entry.pending = merged;
    } else entry.pending = request;
    entry.failed = false;
}
void UpdateCoordinator::Available(uint64_t id, bool value) { entries_[id].available = value; }
void UpdateCoordinator::Forget(uint64_t id) { entries_.erase(id); }
bool UpdateCoordinator::Failed(uint64_t id) const { auto it = entries_.find(id); return it != entries_.end() && it->second.failed; }
std::optional<uint64_t> UpdateCoordinator::Deadline() const {
    std::optional<uint64_t> deadline;
    for (const auto& [id, entry] : entries_) if (entry.available && !entry.failed && entry.pending)
        if (!deadline || entry.pending->deadline < *deadline) deadline = entry.pending->deadline;
    return deadline;
}
size_t UpdateCoordinator::Drain(uint64_t now, size_t budget, const std::function<UpdateOutcome(const UpdateRequest&)>& process) {
    if (draining_ || !budget) return 0;
    draining_ = true; struct Guard { bool& value; ~Guard() { value = false; } } guard{draining_};
    std::vector<UpdateRequest> batch;
    batch.reserve(std::min(budget, entries_.size()));
    for (auto& [id, entry] : entries_) if (entry.available && !entry.failed && entry.pending && entry.pending->deadline <= now) {
        batch.push_back(*entry.pending); entry.pending.reset(); if (batch.size() == budget) break;
    }
    for (const auto& request : batch) {
        auto current = entries_.find(request.id); if (current == entries_.end()) continue;
        if (current->second.window != request.windowGeneration) continue;
        if (current->second.geometry != request.geometryRevision) {
            // The newer geometry replaces coordinates, not unconsumed work.
            // This entry was frozen before another callback superseded it.
            if (current->second.pending) {
                current->second.pending->flags |= request.flags;
                current->second.pending->deadline = std::min(current->second.pending->deadline, request.deadline);
            } else {
                auto merged = request; merged.geometryRevision = current->second.geometry; current->second.pending = merged;
            }
            continue;
        }
        if (!current->second.available) { Request(request); continue; }
        UpdateOutcome result;
        try { result = process(request); } catch (...) { result = {UpdateResult::Failed}; }
        auto it = entries_.find(request.id); if (it == entries_.end()) continue;
        auto& entry = it->second;
        if (result.result == UpdateResult::Failed) {
            // A newer explicit request is already a retry decision. Otherwise
            // retain the failed work and wait for a new external request.
            const uint32_t remaining = request.flags & ~result.consumed;
            if (!entry.pending) { entry.pending = request; entry.pending->flags = remaining; entry.failed = true; }
            else if (entry.pending->windowGeneration == request.windowGeneration) entry.pending->flags |= remaining;
            continue;
        }
        uint32_t remaining = request.flags & ~result.consumed;
        if (remaining) {
            auto retry = request; retry.flags = remaining; retry.deadline = std::max(now + 1, result.retryDeadline);
            if (entry.pending) {
                auto& next = *entry.pending;
                if (next.windowGeneration == retry.windowGeneration) { next.flags |= retry.flags; next.deadline = std::min(next.deadline, retry.deadline); }
                // A newer window generation never inherits old work.
            } else entry.pending = retry;
        }
    }
    return batch.size();
}
}
