#include "ui_refresh_dependencies.hpp"
#include <algorithm>

namespace mikumikustudio::plugins {
namespace {
bool Same(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) noexcept {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
}
void UiRefreshDependencies::Advance(Owner& owner) noexcept {
    // Zero permanently closes this owner's graph after counter exhaustion.
    if (owner.revision) owner.revision = owner.revision == UINT64_MAX ? 0 : owner.revision + 1;
}
uint64_t UiRefreshDependencies::Revision(const RefreshClient& client) const noexcept {
    const auto owner = owners_.find(&client);
    return owner == owners_.end() ? 1 : owner->second.revision;
}
bool UiRefreshDependencies::HasDependents(const RefreshClient& client, const MmsUiTargetRefV1& upstream) const noexcept {
    return std::any_of(edges_.begin(), edges_.end(), [&](const auto& item) {
        return item.second.owner == &client && Same(item.second.upstream, upstream);
    });
}
MmsResult UiRefreshDependencies::Add(const RefreshClient& client, const MmsUiTargetRefV1& upstream,
    const MmsUiTargetRefV1& dependent, MmsHandle& out) {
    if (upstream.element == dependent.element) return MMS_E_CONFLICT;
    for (const auto& item : edges_) if (item.second.owner == &client && Same(item.second.upstream, upstream) &&
        Same(item.second.dependent, dependent)) return MMS_E_CONFLICT;
    const auto old = owners_.find(&client);
    if ((old != owners_.end() && (!old->second.revision || old->second.revision == UINT64_MAX || old->second.count >= kPerOwner)) ||
        edges_.size() >= kGlobal || next_ == UINT64_MAX) return MMS_E_BUSY;
    std::vector<MmsUiTargetRefV1> reachable{dependent};
    for (size_t position = 0; position < reachable.size(); ++position) {
        if (Same(reachable[position], upstream)) return MMS_E_CONFLICT;
        for (const auto& item : edges_) {
            const auto& edge = item.second;
            if (edge.owner != &client || !Same(edge.upstream, reachable[position])) continue;
            if (std::none_of(reachable.begin(), reachable.end(), [&](const auto& ref) { return Same(ref, edge.dependent); }))
                reachable.push_back(edge.dependent);
        }
    }
    const auto inserted = owners_.try_emplace(&client);
    const auto handle = next_;
    try { edges_.emplace(handle, Edge{&client, upstream, dependent}); }
    catch (...) { if (inserted.second) owners_.erase(inserted.first); throw; }
    ++next_; ++inserted.first->second.count; Advance(inserted.first->second); out = handle; return MMS_OK;
}
MmsResult UiRefreshDependencies::Remove(const RefreshClient& client, MmsHandle handle) {
    const auto edge = edges_.find(handle);
    if (edge == edges_.end()) return MMS_E_NOT_FOUND;
    if (edge->second.owner != &client) return MMS_E_ARGUMENT;
    auto& owner = owners_.at(&client);
    edges_.erase(edge); --owner.count; Advance(owner); return MMS_OK;
}
MmsResult UiRefreshDependencies::Capture(const RefreshClient& client, const MmsUiTargetRefV1& upstream,
    std::vector<MmsUiTargetRefV1>& out, uint64_t& revision) const {
    const auto version = Revision(client); if (!version) return MMS_E_BUSY;
    std::vector<MmsUiTargetRefV1> vertices{upstream};
    for (size_t position = 0; position < vertices.size(); ++position) {
        for (const auto& item : edges_) {
            const auto& edge = item.second;
            if (edge.owner != &client || !Same(edge.upstream, vertices[position])) continue;
            if (std::none_of(vertices.begin(), vertices.end(), [&](const auto& ref) { return Same(ref, edge.dependent); })) {
                if (vertices.size() > kMembers) return MMS_E_BUSY;
                vertices.push_back(edge.dependent);
            }
        }
    }
    if (vertices.size() == 1) return MMS_E_UNSUPPORTED;
    std::vector<size_t> incoming(vertices.size(), 0);
    for (const auto& item : edges_) {
        const auto& edge = item.second; if (edge.owner != &client) continue;
        const auto from = std::find_if(vertices.begin(), vertices.end(), [&](const auto& ref) { return Same(ref, edge.upstream); });
        if (from == vertices.end()) continue;
        const auto to = std::find_if(vertices.begin(), vertices.end(), [&](const auto& ref) { return Same(ref, edge.dependent); });
        if (to != vertices.end()) ++incoming[size_t(to - vertices.begin())];
    }
    std::vector<bool> emitted(vertices.size(), false);
    std::vector<MmsUiTargetRefV1> candidate; candidate.reserve(vertices.size() - 1);
    for (size_t position = 0; position < vertices.size(); ++position) {
        size_t chosen = vertices.size();
        for (size_t i = 0; i < vertices.size(); ++i) if (!emitted[i] && !incoming[i] &&
            (chosen == vertices.size() || vertices[i].element < vertices[chosen].element)) chosen = i;
        if (chosen == vertices.size()) return MMS_E_CONFLICT;
        emitted[chosen] = true;
        if (chosen) candidate.push_back(vertices[chosen]);
        for (const auto& item : edges_) {
            const auto& edge = item.second;
            if (edge.owner != &client || !Same(edge.upstream, vertices[chosen])) continue;
            const auto to = std::find_if(vertices.begin(), vertices.end(), [&](const auto& ref) { return Same(ref, edge.dependent); });
            if (to != vertices.end()) --incoming[size_t(to - vertices.begin())];
        }
    }
    out.swap(candidate); revision = version; return MMS_OK;
}
void UiRefreshDependencies::InvalidateTarget(MmsHandle element) noexcept {
    for (auto it = edges_.begin(); it != edges_.end();) {
        if (it->second.upstream.element == element || it->second.dependent.element == element) {
            auto& owner = owners_.find(it->second.owner)->second;
            --owner.count; Advance(owner); it = edges_.erase(it);
        } else ++it;
    }
}
void UiRefreshDependencies::Retire(const RefreshClient& client) noexcept {
    for (auto it = edges_.begin(); it != edges_.end();) {
        if (it->second.owner == &client) it = edges_.erase(it); else ++it;
    }
    owners_.erase(&client);
}
void UiRefreshDependencies::Clear() noexcept {
    for (auto& owner : owners_) { owner.second.count = 0; Advance(owner.second); }
    edges_.clear();
}
}
