#include "ui_refresh.hpp"
#include "ui_registry.hpp"
#include <cmath>
#include <limits>

namespace mikudancestudio::plugins {
namespace {
constexpr std::size_t kTickets = 256, kGlobalTickets = 4096, kRects = 32;
constexpr std::size_t kBytes = 256 * 1024, kGlobalBytes = 4 * 1024 * 1024, kBatch = 64;
constexpr std::uint32_t kSupported = MMS_UI_REFRESH_REPAINT | MMS_UI_REFRESH_STATE;
bool SameRef(const MmsUiTargetRefV1& a, const MmsUiTargetRefV1& b) noexcept {
    return a.element == b.element && a.instanceGeneration == b.instanceGeneration && a.projectEpoch == b.projectEpoch;
}
struct Flight {
    RefreshClient& client;
    explicit Flight(RefreshClient& c) : client(c) { client.inFlight.fetch_add(1); }
    ~Flight() { client.inFlight.fetch_sub(1); }
};
MmsResult Invalidate(const RefreshPresentation& target, bool whole, const std::vector<MmsUiDirtyRectV1>& rects) {
    if (!IsWindow(target.window)) return MMS_E_STALE;
    if (whole) return InvalidateRect(target.window, nullptr, FALSE) ? MMS_OK : MMS_E_FAILED;
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) return MMS_E_MEMORY;
    struct RegionGuard { HRGN value; ~RegionGuard() { DeleteObject(value); } } guard{region};
    const double scale = target.dpi / 96.0;
    bool any = false;
    for (const auto& r : rects) {
        const double left = std::clamp(double(r.x), 0.0, double(target.width));
        const double top = std::clamp(double(r.y), 0.0, double(target.height));
        const double right = std::clamp(double(r.x) + r.width, 0.0, double(target.width));
        const double bottom = std::clamp(double(r.y) + r.height, 0.0, double(target.height));
        if (r.width == 0 || r.height == 0 || right <= left || bottom <= top) continue;
        HRGN part = CreateRectRgn(int(std::floor(left * scale)), int(std::floor(top * scale)),
            int(std::ceil(right * scale)), int(std::ceil(bottom * scale)));
        if (!part) return MMS_E_MEMORY;
        const auto combined = CombineRgn(region, region, part, RGN_OR); DeleteObject(part);
        if (combined == ERROR) return MMS_E_MEMORY;
        any = true;
    }
    return !any || InvalidateRgn(target.window, region, FALSE) ? MMS_OK : MMS_E_FAILED;
}
}
struct UiRefreshQueue::Target {
    RefreshPresentation presentation;
    std::uint64_t generation = 0, geometry = 0;
    bool suspended = false;
    bool Ready() const noexcept { return !suspended && presentation.window && presentation.width > 0 && presentation.height > 0; }
};
struct UiRefreshQueue::Ticket {
    RefreshClient* owner = nullptr;
    MmsUiTargetRefV1 target{};
    std::uint64_t expectedGeometry = 0, key = 0;
    std::uint32_t kinds = MMS_UI_REFRESH_REPAINT;
    std::vector<MmsUiDirtyRectV1> rects;
    bool whole = false, released = false;
    MmsUiRefreshStateV1 state{Header<MmsUiRefreshStateV1>(), MMS_UI_REFRESH_PENDING, 0, MMS_OK, 0, 0, 0};
    std::size_t Bytes() const noexcept { return sizeof(Ticket) + rects.capacity() * sizeof(MmsUiDirtyRectV1); }
    void Finish(std::uint32_t status, MmsResult result, std::uint64_t geometry = 0, std::uint32_t completed = 0) noexcept {
        state.status = status; state.result = result; state.geometryRevision = geometry;
        state.completedKinds = status == MMS_UI_REFRESH_COMPLETED ? kinds : completed;
        std::vector<MmsUiDirtyRectV1>().swap(rects);
    }
};
UiRefreshQueue::UiRefreshQueue(UiRegistry& registry) : registry_(registry) {}
UiRefreshQueue::~UiRefreshQueue() = default;
void UiRefreshQueue::SetWake(std::function<bool()> wake) { wake_ = std::move(wake); }
MmsUiRefreshApiV1 UiRefreshQueue::Api(RefreshClient& client) {
    MmsUiRefreshApiV1 api{}; api.h = Header<MmsUiRefreshApiV1>(); api.context = &client;
#define REFRESH_CALL(body) auto& c = *static_cast<RefreshClient*>(p); Flight flight(c); return Boundary([&]() -> MmsResult { body; })
    api.getCapabilities = [](void* p, MmsUiRefreshCapabilitiesV1* out) { REFRESH_CALL(return c.queue->Capabilities(c, out)); };
    api.getTarget = [](void* p, MmsHandle h, MmsUiRefreshTargetV1* out) { REFRESH_CALL(return c.queue->GetTarget(c, h, out)); };
    api.request = [](void* p, const MmsUiRefreshRequestV1* request, MmsHandle* out) { REFRESH_CALL(return c.queue->Request(c, request, out)); };
    api.query = [](void* p, MmsHandle h, MmsUiRefreshStateV1* out) { REFRESH_CALL(return c.queue->Query(c, h, out)); };
    api.cancel = [](void* p, MmsHandle h) { REFRESH_CALL(return c.queue->Cancel(c, h)); };
    api.release = [](void* p, MmsHandle h) { REFRESH_CALL(return c.queue->Release(c, h)); };
#undef REFRESH_CALL
    return api;
}
MmsResult UiRefreshQueue::Capabilities(RefreshClient& client, MmsUiRefreshCapabilitiesV1* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!Valid(out)) return MMS_E_ARGUMENT;
    if (client.stopping.load()) return MMS_E_NOT_READY;
    *out = {Header<MmsUiRefreshCapabilitiesV1>(), kSupported, MMS_UI_REFRESH_SELF,
        std::uint32_t(kTickets), std::uint32_t(kRects), kBytes, std::uint32_t(kBatch), 0};
    return MMS_OK;
}
MmsResult UiRefreshQueue::GetTarget(RefreshClient& client, MmsHandle handle, MmsUiRefreshTargetV1* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!Valid(out) || !handle) return MMS_E_ARGUMENT;
    if (client.stopping.load()) return MMS_E_NOT_READY;
    const ElementRecord* element = nullptr;
    for (const auto& entry : registry_.elements) if (entry.second.handle == handle) { element = &entry.second; break; }
    if (!element) return MMS_E_STALE;
    if (String(element->desc.ownerPlugin) != client.owner || client.owner == "mms") return MMS_E_ARGUMENT;
    if (element->desc.kind != MMS_UI_BUTTON && element->desc.kind != MMS_UI_PANEL) return MMS_E_UNSUPPORTED;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = targets_.find(handle); if (found == targets_.end()) return MMS_E_NOT_READY;
    const auto& t = found->second;
    if (t.presentation.owner != client.owner) return MMS_E_STALE;
    *out = {Header<MmsUiRefreshTargetV1>(), {Header<MmsUiTargetRefV1>(), handle, t.generation, epoch_}, t.geometry,
        std::uint32_t(MMS_UI_REFRESH_REPAINT | (t.presentation.stateRefresh ? MMS_UI_REFRESH_STATE : 0)), MMS_UI_REFRESH_SELF, std::uint32_t(t.Ready()), std::uint32_t(t.presentation.visible),
        t.presentation.width, t.presentation.height};
    return MMS_OK;
}
MmsResult UiRefreshQueue::Request(RefreshClient& client, const MmsUiRefreshRequestV1* request, MmsHandle* out) {
    if (!Valid(request) || !Valid(&request->target) || !out || !request->target.element ||
        !request->target.instanceGeneration || !request->target.projectEpoch || !request->kinds ||
        request->kinds & ~15u || request->flags || request->reserved) return MMS_E_ARGUMENT;
    if (request->kinds & ~kSupported) return MMS_E_UNSUPPORTED;
    if (request->scope != MMS_UI_REFRESH_SELF) return request->scope == MMS_UI_REFRESH_SUBTREE ||
        request->scope == MMS_UI_REFRESH_DEPENDENTS ? MMS_E_UNSUPPORTED : MMS_E_ARGUMENT;
    if (request->binding || request->sourceRevision) return MMS_E_ARGUMENT;
    const auto& span = request->dirtyRects;
    if (!(request->kinds & MMS_UI_REFRESH_REPAINT) && span.count) return MMS_E_ARGUMENT;
    if (span.count > kRects) return MMS_E_BUSY;
    if (span.count && (!span.data || span.stride < sizeof(MmsUiDirtyRectV1) || span.recordVersion != 1 ||
        !request->expectedGeometryRevision)) return MMS_E_ARGUMENT;
    Ticket ticket; ticket.owner = &client; ticket.target = request->target;
    ticket.kinds = request->kinds;
    ticket.expectedGeometry = request->expectedGeometryRevision; ticket.key = request->coalesceKey; ticket.whole = span.count == 0;
    ticket.rects.reserve(std::size_t(span.count));
    for (std::uint64_t index = 0; index < span.count; ++index) {
        MmsUiDirtyRectV1 rect{};
        std::memcpy(&rect, static_cast<const unsigned char*>(span.data) + std::size_t(index * span.stride), sizeof(rect));
        for (float value : {rect.x, rect.y, rect.width, rect.height})
            if (!std::isfinite(value) || std::abs(value) > 1048576) return MMS_E_ARGUMENT;
        if (rect.width < 0 || rect.height < 0) return MMS_E_ARGUMENT;
        ticket.rects.push_back(rect);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.stopping.load()) return MMS_E_NOT_READY;
    if ((ticket.kinds & MMS_UI_REFRESH_STATE) && evaluatingState_ && GetCurrentThreadId() == thread_) return MMS_E_BUSY;
    if (ticket.target.projectEpoch != epoch_) return MMS_E_STALE;
    const auto target = targets_.find(ticket.target.element);
    if (target == targets_.end() || target->second.generation != ticket.target.instanceGeneration) return MMS_E_STALE;
    if (target->second.presentation.owner != client.owner || client.owner == "mms") return MMS_E_ARGUMENT;
    if ((ticket.kinds & MMS_UI_REFRESH_STATE) && !target->second.presentation.stateRefresh) return MMS_E_UNSUPPORTED;
    if (ticket.expectedGeometry && ticket.expectedGeometry != target->second.geometry) return MMS_E_STALE;
    if (!target->second.Ready()) return MMS_E_NOT_READY;
    std::size_t count = 0, bytes = 0, globalBytes = 0; bool pending = false;
    for (const auto& item : tickets_) {
        globalBytes += item.second.Bytes();
        if (item.second.owner == &client) { ++count; bytes += item.second.Bytes(); }
        pending |= item.second.state.status == MMS_UI_REFRESH_PENDING;
    }
    if (count >= kTickets || tickets_.size() >= kGlobalTickets || ticket.Bytes() > kBytes - bytes ||
        ticket.Bytes() > kGlobalBytes - globalBytes || next_ == UINT64_MAX) return MMS_E_BUSY;
    const auto handle = next_++;
    tickets_.emplace(handle, std::move(ticket));
    // This internal wake is a non-reentrant PostMessage signal, never a plugin
    // callback. Holding the lock prevents UI consumption before failure rollback.
    bool signalled = true;
    if (!pending && wake_) { try { signalled = wake_(); } catch (...) { signalled = false; } }
    if (!signalled) { tickets_.erase(handle); return MMS_E_FAILED; }
    *out = handle; return MMS_OK;
}
MmsResult UiRefreshQueue::Query(RefreshClient& client, MmsHandle handle, MmsUiRefreshStateV1* out) {
    if (!Valid(out)) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_); const auto found = tickets_.find(handle);
    if (found == tickets_.end() || found->second.owner != &client || found->second.released) return MMS_E_NOT_FOUND;
    *out = found->second.state; return MMS_OK;
}
MmsResult UiRefreshQueue::Cancel(RefreshClient& client, MmsHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_); const auto found = tickets_.find(handle);
    if (found == tickets_.end() || found->second.owner != &client || found->second.released) return MMS_E_NOT_FOUND;
    if (found->second.state.status == MMS_UI_REFRESH_RUNNING) return MMS_E_BUSY;
    if (found->second.state.status == MMS_UI_REFRESH_PENDING) found->second.Finish(MMS_UI_REFRESH_CANCELLED, MMS_E_CANCELLED);
    return MMS_OK;
}
MmsResult UiRefreshQueue::Release(RefreshClient& client, MmsHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_); const auto found = tickets_.find(handle);
    if (found == tickets_.end() || found->second.owner != &client || found->second.released) return MMS_E_NOT_FOUND;
    if (found->second.state.status == MMS_UI_REFRESH_PENDING || found->second.state.status == MMS_UI_REFRESH_RUNNING)
        found->second.released = true;
    else tickets_.erase(found);
    return MMS_OK;
}
void UiRefreshQueue::SyncTargets(const std::vector<RefreshPresentation>& presentations) {
    if (GetCurrentThreadId() != thread_) throw std::invalid_argument("refresh presentation thread");
    if (presentations.size() > 8192) throw std::bad_alloc();
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<MmsHandle, Target> candidate;
    for (const auto& p : presentations) {
        if (!p.element || !p.instance || p.owner.empty() || !p.window || p.dpi < 48 || p.dpi > 960 ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.width) || !std::isfinite(p.height) ||
            p.width < 0 || p.height < 0 || p.width > 1048576 || p.height > 1048576) throw std::invalid_argument("refresh presentation");
        const auto old = targets_.find(p.element);
        Target target; target.presentation = p;
        const bool sameInstance = old != targets_.end() && old->second.presentation.owner == p.owner &&
            old->second.presentation.instance == p.instance && old->second.presentation.window == p.window;
        if (!nextGeneration_ || !nextGeometry_) throw std::bad_alloc();
        target.generation = sameInstance ? old->second.generation : nextGeneration_++;
        const bool sameGeometry = sameInstance && old->second.presentation.x == p.x && old->second.presentation.y == p.y &&
            old->second.presentation.width == p.width && old->second.presentation.height == p.height &&
            old->second.presentation.dpi == p.dpi && old->second.presentation.visible == p.visible;
        target.geometry = sameGeometry ? old->second.geometry : nextGeometry_++;
        if (!candidate.emplace(p.element, std::move(target)).second) throw std::invalid_argument("duplicate refresh target");
    }
    targets_.swap(candidate);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& ticket = it->second; const auto target = targets_.find(ticket.target.element);
        if (ticket.state.status == MMS_UI_REFRESH_PENDING && (target == targets_.end() ||
            target->second.generation != ticket.target.instanceGeneration ||
            (ticket.expectedGeometry && ticket.expectedGeometry != target->second.geometry)))
            ticket.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
        if (ticket.released && ticket.state.status != MMS_UI_REFRESH_PENDING && ticket.state.status != MMS_UI_REFRESH_RUNNING)
            it = tickets_.erase(it); else ++it;
    }
}
void UiRefreshQueue::InvalidateTarget(MmsHandle element) noexcept {
    std::lock_guard<std::mutex> lock(mutex_); targets_.erase(element);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& ticket = it->second;
        if (ticket.target.element == element && ticket.state.status == MMS_UI_REFRESH_PENDING) ticket.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
        if (ticket.released && ticket.state.status != MMS_UI_REFRESH_PENDING && ticket.state.status != MMS_UI_REFRESH_RUNNING)
            it = tickets_.erase(it); else ++it;
    }
}
void UiRefreshQueue::SuspendTargets() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& target : targets_) target.second.suspended = true;
}
void UiRefreshQueue::Drain() {
    if (GetCurrentThreadId() != thread_ || registry_.InCallback()) return;
    struct Group { Target target; RefreshClient* owner; MmsUiTargetRefV1 ref; std::uint64_t key; std::uint32_t kinds;
        std::vector<MmsHandle> tickets; std::vector<MmsUiDirtyRectV1> rects; bool whole = false, fixedGeometry = false; };
    std::vector<Group> groups;
    {
        std::lock_guard<std::mutex> lock(mutex_); if (draining_) return;
        std::size_t count = 0;
        for (auto it = tickets_.begin(); it != tickets_.end() && count < kBatch;) {
            auto& ticket = it->second;
            if (ticket.state.status != MMS_UI_REFRESH_PENDING) { ++it; continue; }
            ++count; MmsResult result = MMS_OK; const auto target = targets_.find(ticket.target.element);
            if (ticket.owner->stopping.load()) result = MMS_E_CANCELLED;
            else if (ticket.target.projectEpoch != epoch_ || target == targets_.end() ||
                target->second.generation != ticket.target.instanceGeneration ||
                (ticket.expectedGeometry && ticket.expectedGeometry != target->second.geometry)) result = MMS_E_STALE;
            else if (!target->second.Ready()) result = MMS_E_NOT_READY;
            if (result) {
                ticket.Finish(result == MMS_E_CANCELLED ? MMS_UI_REFRESH_CANCELLED : MMS_UI_REFRESH_FAILED, result);
                if (ticket.released) it = tickets_.erase(it); else ++it;
                continue;
            }
            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) {
                return g.owner == ticket.owner && SameRef(g.ref, ticket.target) && g.key == ticket.key && g.kinds == ticket.kinds &&
                    g.fixedGeometry == (ticket.expectedGeometry != 0);
            });
            if (group == groups.end()) {
                groups.push_back({target->second, ticket.owner, ticket.target, ticket.key, ticket.kinds, {}, {}, false, ticket.expectedGeometry != 0}); group = std::prev(groups.end());
            }
            group->tickets.push_back(it->first); group->whole |= ticket.whole;
            group->fixedGeometry |= ticket.expectedGeometry != 0;
            group->rects.insert(group->rects.end(), ticket.rects.begin(), ticket.rects.end());
            ++it;
        }
        for (const auto& group : groups) for (auto handle : group.tickets) tickets_.at(handle).state.status = MMS_UI_REFRESH_RUNNING;
        draining_ = true;
    }
    // No queue lock crosses read-only plugin callbacks or native window calls.
    // Every group is checked again: an earlier callback may have pumped messages.
    for (auto& group : groups) {
        std::uint32_t completed = 0;
        const auto result = Boundary([&]() -> MmsResult {
            const auto validate = [&](bool strict) -> MmsResult {
                std::lock_guard<std::mutex> lock(mutex_);
                if (group.owner->stopping.load()) return MMS_E_CANCELLED;
                const auto target = targets_.find(group.ref.element);
                if (group.ref.projectEpoch != epoch_ || target == targets_.end() || target->second.generation != group.ref.instanceGeneration ||
                    (strict && target->second.geometry != group.target.geometry)) return MMS_E_STALE;
                if (!target->second.Ready()) return MMS_E_NOT_READY;
                if ((group.kinds & MMS_UI_REFRESH_STATE) && !target->second.presentation.stateRefresh) return MMS_E_UNSUPPORTED;
                if (!strict) group.target = target->second;
                return MMS_OK;
            };
            if (const auto r = validate(group.fixedGeometry)) return r;
            if (group.kinds & MMS_UI_REFRESH_STATE) {
                { std::lock_guard<std::mutex> lock(mutex_); evaluatingState_ = true; }
                struct StateGuard { UiRefreshQueue& queue; ~StateGuard() {
                    std::lock_guard<std::mutex> lock(queue.mutex_); queue.evaluatingState_ = false;
                } } guard{*this};
                // A provider may disable its own HWND while answering getState.
                // Remember ownership before that callback: USER32 may clear focus
                // as soon as the HWND becomes disabled.
                const HWND focusedBeforeState = GetFocus();
                const auto r = registry_.WithElementState(group.ref.element, [&](const MmsCommandStateV1& state) -> MmsResult {
                    if (const auto check = validate(true)) return check;
                    const bool visibilityChanged = bool(state.visible) != group.target.presentation.stateVisible;
                    // Only an existing reserved slot can change visibility without
                    // measurement, sibling reflow or viewport changes.
                    if (visibilityChanged && !group.target.presentation.stateVisibility) return MMS_E_UNSUPPORTED;
                    if (visibilityChanged) if (const auto check = registry_.CheckVisibilityPublication(group.ref.element)) return check;
                    HWND window = group.target.presentation.window;
                    if (!IsWindow(window)) return MMS_E_STALE;
                    const bool losingAvailability = !state.enabled || !(state.visible && group.target.presentation.layoutVisible);
                    const auto relocateFocus = [&]() -> MmsResult {
                        const HWND focus = GetFocus();
                        // Keep an explicit new focus chosen by a provider/native
                        // callback, but recover focus that USER32 cleared itself.
                        if ((focus != window && (focus || focusedBeforeState != window)) ||
                            (!losingAvailability && IsWindowEnabled(window) && IsWindowVisible(window))) return MMS_OK;
                        const HWND parent = GetParent(window);
                        const auto eligible = [&](HWND candidate) {
                            return candidate && candidate != window && IsWindow(candidate) &&
                                IsWindowVisible(candidate) && IsWindowEnabled(candidate) &&
                                (GetWindowLongPtrW(candidate, GWL_STYLE) & WS_TABSTOP);
                        };
                        HWND next = parent ? GetNextDlgTabItem(parent, window, FALSE) : nullptr;
                        if (!eligible(next)) {
                            // Custom control-parent containers are not dialogs.
                            // Fall back to their actual sibling tab order if the
                            // dialog helper returns the container or no tab stop.
                            const HWND first = parent ? GetWindow(parent, GW_CHILD) : nullptr;
                            HWND child = GetWindow(window, GW_HWNDNEXT);
                            if (!child) child = first;
                            for (unsigned visited = 0; child && child != window && visited < 8192; ++visited) {
                                if (eligible(child)) { next = child; break; }
                                child = GetWindow(child, GW_HWNDNEXT); if (!child) child = first;
                            }
                        }
                        if (!eligible(next)) {
                            next = parent;
                            while (next && (!IsWindowVisible(next) || !IsWindowEnabled(next))) next = GetParent(next);
                        }
                        if (const auto check = validate(true)) return check;
                        if (!IsWindow(window)) return MMS_E_STALE;
                        if (GetFocus() == window || (!GetFocus() && focusedBeforeState == window)) SetFocus(next);
                        if (const auto check = validate(true)) return check;
                        if (!IsWindow(window)) return MMS_E_STALE;
                        return GetFocus() == window || (next && !GetFocus()) ? MMS_E_FAILED : MMS_OK;
                    };
                    // Choose a valid keyboard target while the old HWND is still
                    // enabled. EnableWindow/ShowWindow are allowed to clear its
                    // focus; their callbacks may subsequently choose another one.
                    if (losingAvailability) if (const auto check = relocateFocus()) return check;
                    if (bool(IsWindowEnabled(window)) != bool(state.enabled)) EnableWindow(window, state.enabled);
                    if (const auto check = validate(true)) return check;
                    if (!IsWindow(window)) return MMS_E_STALE;
                    const auto checked = state.checked ? BST_CHECKED : BST_UNCHECKED;
                    if (SendMessageW(window, BM_GETCHECK, 0, 0) != checked) SendMessageW(window, BM_SETCHECK, checked, 0);
                    if (const auto check = validate(true)) return check;
                    if (!IsWindow(window)) return MMS_E_STALE;
                    if (visibilityChanged) {
                        const bool shown = state.visible && group.target.presentation.layoutVisible;
                        if (bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != shown) ShowWindow(window, shown ? SW_SHOWNA : SW_HIDE);
                        const auto focusResult = relocateFocus();
                        if (const auto check = focusResult ? focusResult : validate(true)) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            auto target = targets_.find(group.ref.element);
                            if (target != targets_.end() && target->second.generation == group.ref.instanceGeneration) target->second.suspended = true;
                            return check;
                        }
                        if (!IsWindow(window)) return MMS_E_STALE;
                        if (bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != shown) return MMS_E_FAILED;
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto& target = targets_.at(group.ref.element);
                        const auto result = registry_.PublishVisibility(group.ref.element, shown);
                        if (result) { target.suspended = true; return result; }
                        if (target.presentation.visible != shown) target.geometry = nextGeometry_++;
                        target.presentation.visible = shown; target.presentation.stateVisible = state.visible != 0;
                        group.target.geometry = target.geometry;
                        group.target.presentation.visible = shown; group.target.presentation.stateVisible = state.visible != 0;
                    }
                    if (const auto check = relocateFocus()) return check;
                    const bool applied = bool(IsWindowEnabled(window)) == bool(state.enabled) && SendMessageW(window, BM_GETCHECK, 0, 0) == checked;
                    if (const auto check = validate(true)) return check;
                    return IsWindow(window) ? (applied ? MMS_OK : MMS_E_FAILED) : MMS_E_STALE;
                });
                if (r) return r;
                completed |= MMS_UI_REFRESH_STATE;
            }
            if (group.kinds & MMS_UI_REFRESH_REPAINT) {
                if (const auto r = validate(group.fixedGeometry || (group.kinds & MMS_UI_REFRESH_STATE))) return r;
                const auto r = Invalidate(group.target.presentation, group.whole, group.rects);
                if (!r) completed |= MMS_UI_REFRESH_REPAINT;
                return r;
            }
            return MMS_OK;
        });
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto handle : group.tickets) {
            auto found = tickets_.find(handle); if (found == tickets_.end()) continue;
            found->second.Finish(result == MMS_E_CANCELLED ? MMS_UI_REFRESH_CANCELLED : result ? MMS_UI_REFRESH_FAILED : MMS_UI_REFRESH_COMPLETED, result, group.target.geometry, completed);
            if (found->second.released) tickets_.erase(found);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_); draining_ = false;
}
bool UiRefreshQueue::Pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(tickets_.begin(), tickets_.end(), [](const auto& t) { return t.second.state.status == MMS_UI_REFRESH_PENDING; });
}
void UiRefreshQueue::Retire(RefreshClient& client) noexcept {
    client.stopping.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        if (t.owner == &client && t.state.status == MMS_UI_REFRESH_PENDING) t.Finish(MMS_UI_REFRESH_CANCELLED, MMS_E_CANCELLED);
        if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING) it = tickets_.erase(it); else ++it;
    }
}
bool UiRefreshQueue::HasLeases(const RefreshClient& client) const {
    if (client.inFlight.load()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    return client.inFlight.load() || std::any_of(tickets_.begin(), tickets_.end(), [&](const auto& t) { return t.second.owner == &client; });
}
void UiRefreshQueue::FailPending(MmsResult result) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        if (t.state.status == MMS_UI_REFRESH_PENDING) t.Finish(MMS_UI_REFRESH_FAILED, result);
        if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING) it = tickets_.erase(it); else ++it;
    }
}
void UiRefreshQueue::AdvanceProjectEpoch() noexcept {
    std::lock_guard<std::mutex> lock(mutex_); ++epoch_;
    for (auto it = tickets_.begin(); it != tickets_.end();) {
        auto& t = it->second;
        if (t.state.status == MMS_UI_REFRESH_PENDING) t.Finish(MMS_UI_REFRESH_FAILED, MMS_E_STALE);
        if (t.released && t.state.status != MMS_UI_REFRESH_PENDING && t.state.status != MMS_UI_REFRESH_RUNNING) it = tickets_.erase(it); else ++it;
    }
}
}
