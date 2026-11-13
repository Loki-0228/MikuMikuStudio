#include "ui_registry.hpp"
#include <atomic>
#include <cmath>
#include <limits>

namespace mikumikustudio::plugins {
namespace {
// The host may replace its registry while an older native gesture still has
// a cleanup callback. A process-wide token cannot unlock a replacement host.
std::atomic<uint64_t> lastHostLayoutToken{0};
uint64_t ReserveHostLayoutToken() noexcept {
    auto previous = lastHostLayoutToken.load(std::memory_order_relaxed);
    while (previous != UINT64_MAX) {
        if (lastHostLayoutToken.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) return previous + 1;
    }
    return 0;
}
void AdvancePreview(LayoutPreviewInfo& info) noexcept {
    if (info.revision != UINT64_MAX) ++info.revision;
    if (info.revision == UINT64_MAX && info.state == LayoutPreviewState::Active) {
        info.state = LayoutPreviewState::Revoked;
        info.reasons |= MMS_LAYOUT_SESSION_REVISION_EXHAUSTED;
    }
}
bool SameText(MmsString a, MmsString b) noexcept {
    return a.bytes == b.bytes && (!a.bytes || !std::memcmp(a.data, b.data, size_t(a.bytes)));
}
auto FindElement(const std::map<std::string, ElementRecord>& tree, MmsHandle handle) {
    return std::find_if(tree.begin(), tree.end(), [=](const auto& e) { return e.second.handle == handle; });
}
auto FindId(const std::map<std::string, ElementRecord>& tree, MmsString id) {
    return std::find_if(tree.begin(), tree.end(), [&](const auto& e) { return SameText(Text(e.first), id); });
}
uint32_t PreviewMount(const ElementRecord& element, const std::map<std::string, ElementRecord>& tree) noexcept {
    if (element.nativeObserved || (element.desc.capabilities & MMS_UI_DOCK)) return 0;
    if (element.desc.kind == MMS_UI_PANEL) {
        const auto parent = FindId(tree, element.desc.parentId);
        return parent != tree.end() && parent->first == "mms.workspace.tabs.extensions" &&
            !parent->second.nativeObserved && parent->second.desc.kind == MMS_UI_GROUP ? HostLayoutWorkspace : 0;
    }
    if (element.desc.kind != MMS_UI_BUTTON && element.desc.kind != MMS_UI_INPUT &&
        element.desc.kind != MMS_UI_GROUP && element.desc.kind != MMS_UI_SEPARATOR) return 0;
    auto parent = element.desc.parentId;
    for (size_t depth = 0; depth <= tree.size(); ++depth) {
        const auto found = FindId(tree, parent);
        if (found == tree.end()) return 0;
        if (found->first == "mms.toolbar.main.extensions") return HostLayoutToolbar;
        if (found->first == "mms.properties.main.extensions") return HostLayoutProperties;
        if (found->first == "mms.sidebar.left.extensions") return HostLayoutLeft;
        if (found->first == "mms.sidebar.right.extensions") return HostLayoutRight;
        if (found->second.nativeObserved || found->second.desc.kind != MMS_UI_GROUP) return 0;
        parent = found->second.desc.parentId;
    }
    return 0;
}
template<class T> MmsResult OutputHeader(const T* output, uint16_t minor = 0) noexcept {
    if (!output || output->h.size < sizeof(T)) return MMS_E_ARGUMENT;
    return output->h.major == 1 && output->h.minor == minor ? MMS_OK : MMS_E_VERSION;
}
template<class T> MmsResult InputSpan(MmsSpan span, uint32_t version = 1) noexcept {
    if (!span.data || !span.count || span.count > 64 || span.stride != sizeof(T)) return MMS_E_ARGUMENT;
    return span.recordVersion == version ? MMS_OK : MMS_E_VERSION;
}
template<class T> MmsResult InputRecord(MmsSpan span, size_t index, T& value, uint16_t minor = 0) noexcept {
    std::memcpy(&value, static_cast<const unsigned char*>(span.data) + index * sizeof(T), sizeof(T));
    if (value.h.size != sizeof(T)) return MMS_E_ARGUMENT;
    return value.h.major == 1 && value.h.minor == minor ? MMS_OK : MMS_E_VERSION;
}
}

MmsUiLayoutSessionApiV1 UiRegistry::LayoutSessionApi(UiClient& client, uint32_t minor) {
    if (minor > 1) return {};
    MmsUiLayoutSessionApiV1 api{}; api.h = Header<MmsUiLayoutSessionApiV1>(); api.h.minor = static_cast<uint16_t>(minor); api.context = &client;
#define SESSION_CALL(...) return Boundary([&]() -> MmsResult { \
    auto* client = static_cast<UiClient*>(context); \
    if (!client || !client->registry) return MMS_E_ARGUMENT; \
    auto& registry = *client->registry; \
    if (const auto result = registry.Check(*client)) return result; \
    __VA_ARGS__; })
    static const auto capabilities = [](void* context, MmsUiLayoutSessionCapsV1* out, uint32_t mounts) -> MmsResult {
        SESSION_CALL(
            if (const auto result = OutputHeader(out)) return result;
            const MmsUiLayoutSessionCapsV1 value{Header<MmsUiLayoutSessionCapsV1>(), 7, mounts, 64, 32, 256, 0};
            *out = value; return MMS_OK;
        );
    };
    api.queryCapabilities = minor
        ? +[](void* context, MmsUiLayoutSessionCapsV1* out) -> MmsResult { return capabilities(context, out, HostLayoutAll); }
        : +[](void* context, MmsUiLayoutSessionCapsV1* out) -> MmsResult { return capabilities(context, out, 15); };
    static const auto queryTarget = [](void* context, MmsHandle handle, MmsUiLayoutSessionTargetInfoV1* out, uint32_t mounts) -> MmsResult {
        SESSION_CALL(
            if (const auto result = OutputHeader(out)) return result;
            const auto found = FindElement(registry.elements, handle);
            if (found == registry.elements.end()) return MMS_E_NOT_FOUND;
            const auto& element = found->second;
            if (!SameText(element.desc.ownerPlugin, Text(client->id))) return MMS_E_ARGUMENT;
            const auto mount = PreviewMount(element, registry.elements);
            if (!(mount & mounts)) return MMS_E_UNSUPPORTED;
            const auto caps = element.desc.capabilities;
            const uint32_t fields = ((caps & MMS_UI_HIDE) ? PreviewVisible : 0u) |
                ((caps & MMS_UI_RESIZE) ? PreviewSize : 0u) | ((caps & MMS_UI_LAYOUT) ? PreviewOrder : 0u);
            if (!fields) return MMS_E_UNSUPPORTED;
            const MmsUiLayoutSessionTargetInfoV1 value{Header<MmsUiLayoutSessionTargetInfoV1>(), handle, fields, mount};
            *out = value; return MMS_OK;
        );
    };
    api.queryTarget = minor
        ? +[](void* context, MmsHandle handle, MmsUiLayoutSessionTargetInfoV1* out) -> MmsResult { return queryTarget(context, handle, out, HostLayoutAll); }
        : +[](void* context, MmsHandle handle, MmsUiLayoutSessionTargetInfoV1* out) -> MmsResult { return queryTarget(context, handle, out, 15); };
    static const auto begin = [](void* context, MmsSpan targets, MmsHandle* out, uint32_t mounts) -> MmsResult {
        SESSION_CALL(
            if (const auto result = registry.Check(*client, true)) return result;
            if (!out) return MMS_E_ARGUMENT;
            if (const auto result = InputSpan<MmsUiLayoutSessionTargetV1>(targets)) return result;
            std::vector<LayoutPreviewValue> values; values.reserve(size_t(targets.count));
            for (size_t i = 0; i < targets.count; ++i) {
                MmsUiLayoutSessionTargetV1 target{};
                if (const auto result = InputRecord(targets, i, target)) return result;
                if (target.reserved || !target.fields || (target.fields & ~7u)) return MMS_E_ARGUMENT;
                const auto found = FindElement(registry.elements, target.element);
                if (found != registry.elements.end() && (PreviewMount(found->second, registry.elements) & ~mounts))
                    return MMS_E_UNSUPPORTED;
                values.push_back({target.element, target.fields});
            }
            MmsHandle handle = 0;
            if (const auto result = registry.BeginLayoutPreview(*client, values, handle)) return result;
            *out = handle; return MMS_OK;
        );
    };
    api.begin = minor
        ? +[](void* context, MmsSpan targets, MmsHandle* out) -> MmsResult { return begin(context, targets, out, HostLayoutAll); }
        : +[](void* context, MmsSpan targets, MmsHandle* out) -> MmsResult { return begin(context, targets, out, 15); };
    api.apply = [](void* context, MmsHandle handle, uint64_t revision, MmsSpan input) -> MmsResult {
        SESSION_CALL(
            if (const auto result = registry.Check(*client, true)) return result;
            if (const auto result = InputSpan<MmsUiLayoutSessionValueV1>(input)) return result;
            std::vector<LayoutPreviewValue> values; values.reserve(size_t(input.count));
            for (size_t i = 0; i < input.count; ++i) {
                MmsUiLayoutSessionValueV1 value{};
                if (const auto result = InputRecord(input, i, value)) return result;
                if (value.reserved || !value.fields || (value.fields & ~7u) || value.visible > 1 ||
                    (!(value.fields & PreviewVisible) && value.visible) ||
                    (!(value.fields & PreviewSize) && (value.width != 0 || value.height != 0)) ||
                    (!(value.fields & PreviewOrder) && value.order)) return MMS_E_ARGUMENT;
                values.push_back({value.element, value.fields, value.visible != 0, value.width, value.height, value.order});
            }
            return registry.ApplyLayoutPreview(*client, handle, revision, values);
        );
    };
    api.query = [](void* context, MmsHandle handle, MmsUiLayoutSessionInfoV1* out) -> MmsResult {
        SESSION_CALL(
            if (const auto result = OutputHeader(out)) return result;
            const auto found = registry.layoutPreviews_.find(handle);
            if (found == registry.layoutPreviews_.end() || found->second.owner != client) return MMS_E_NOT_FOUND;
            const auto& session = found->second;
            uint32_t active = 0;
            if (session.info.state == LayoutPreviewState::Active)
                for (const auto& value : session.values) if (value.fields) ++active;
            const MmsUiLayoutSessionInfoV1 value{Header<MmsUiLayoutSessionInfoV1>(), session.info.revision,
                uint32_t(session.info.state), session.info.reasons, uint32_t(session.values.size()), active};
            *out = value; return MMS_OK;
        );
    };
    api.queryClaim = [](void* context, MmsHandle handle, uint64_t revision, uint32_t index, MmsUiLayoutSessionTargetV1* out) -> MmsResult {
        SESSION_CALL(
            if (const auto result = OutputHeader(out)) return result;
            const auto found = registry.layoutPreviews_.find(handle);
            if (found == registry.layoutPreviews_.end() || found->second.owner != client) return MMS_E_NOT_FOUND;
            const auto& session = found->second;
            if (session.info.revision != revision) return MMS_E_STALE;
            if (index >= session.values.size()) return MMS_E_ARGUMENT;
            const auto& claim = session.values[index];
            if (session.info.state == LayoutPreviewState::Active && (claim.fields & PreviewParent)) return MMS_E_UNSUPPORTED;
            const MmsUiLayoutSessionTargetV1 value{Header<MmsUiLayoutSessionTargetV1>(), claim.element,
                session.info.state == LayoutPreviewState::Active ? claim.fields : 0u, 0};
            *out = value; return MMS_OK;
        );
    };
    api.rollback = [](void* context, MmsHandle handle, uint64_t revision) -> MmsResult {
        SESSION_CALL(return registry.EndLayoutPreview(*client, handle, revision, false));
    };
    api.commit = [](void* context, MmsHandle handle, uint64_t revision) -> MmsResult {
        SESSION_CALL(return registry.EndLayoutPreview(*client, handle, revision, true));
    };
    api.release = [](void* context, MmsHandle handle) -> MmsResult {
        SESSION_CALL(return registry.ReleaseLayoutPreview(*client, handle));
    };
#undef SESSION_CALL
    return api;
}

MmsUiLayoutSessionApiV1_2 UiRegistry::LayoutSessionApi12(UiClient& client) {
    MmsUiLayoutSessionApiV1_2 api{};
    const auto prefix = LayoutSessionApi(client, 1);
    std::memcpy(&api, &prefix, sizeof(prefix));
    api.h = Header<MmsUiLayoutSessionApiV1_2>(); api.h.minor = 2;
#define SESSION_EX_CALL(...) return Boundary([&]() -> MmsResult { \
    auto* client = static_cast<UiClient*>(context); \
    if (!client || !client->registry) return MMS_E_ARGUMENT; \
    auto& registry = *client->registry; \
    if (const auto result = registry.Check(*client)) return result; \
    __VA_ARGS__; })
    api.queryCapabilitiesEx = [](void* context, MmsUiLayoutSessionCapsV1* out) -> MmsResult {
        SESSION_EX_CALL(
            if (const auto result = OutputHeader(out)) return result;
            *out = {Header<MmsUiLayoutSessionCapsV1>(), 15, HostLayoutAll, 64, 32, 256, 0};
            return MMS_OK;
        );
    };
    api.queryTargetEx = [](void* context, MmsHandle handle, MmsUiLayoutSessionTargetInfoV1* out) -> MmsResult {
        SESSION_EX_CALL(
            if (const auto result = OutputHeader(out)) return result;
            const auto found = FindElement(registry.elements, handle);
            if (found == registry.elements.end()) return MMS_E_NOT_FOUND;
            const auto& element = found->second;
            if (!SameText(element.desc.ownerPlugin, Text(client->id))) return MMS_E_ARGUMENT;
            const auto mount = PreviewMount(element, registry.elements);
            if (!mount) return MMS_E_UNSUPPORTED;
            const auto caps = element.desc.capabilities;
            const uint32_t fields = ((caps & MMS_UI_HIDE) ? PreviewVisible : 0u) |
                ((caps & MMS_UI_RESIZE) ? PreviewSize : 0u) | ((caps & MMS_UI_LAYOUT) ? PreviewOrder : 0u) |
                ((mount != HostLayoutWorkspace && (caps & MMS_UI_MOVE)) ? PreviewParent : 0u);
            if (!fields) return MMS_E_UNSUPPORTED;
            *out = {Header<MmsUiLayoutSessionTargetInfoV1>(), handle, fields, mount}; return MMS_OK;
        );
    };
    api.beginEx = [](void* context, MmsSpan input, MmsHandle* out) -> MmsResult {
        SESSION_EX_CALL(
            if (const auto result = registry.Check(*client, true)) return result;
            if (!out) return MMS_E_ARGUMENT;
            if (const auto result = InputSpan<MmsUiLayoutSessionTargetV1_2>(input, 2)) return result;
            std::vector<LayoutPreviewValue> values; values.reserve(size_t(input.count));
            for (size_t index = 0; index < input.count; ++index) {
                MmsUiLayoutSessionTargetV1_2 target{};
                if (const auto result = InputRecord(input, index, target, 2)) return result;
                if (target.reserved || !target.fields || (target.fields & ~15u) ||
                    ((target.fields & PreviewParent) ? !target.parent : target.parent != 0)) return MMS_E_ARGUMENT;
                LayoutPreviewValue value{}; value.element = target.element; value.fields = target.fields; value.parent = target.parent;
                values.push_back(value);
            }
            MmsHandle handle = 0;
            if (const auto result = registry.BeginLayoutPreview(*client, values, handle)) return result;
            *out = handle; return MMS_OK;
        );
    };
    api.applyEx = [](void* context, MmsHandle handle, uint64_t revision, MmsSpan input) -> MmsResult {
        SESSION_EX_CALL(
            if (const auto result = registry.Check(*client, true)) return result;
            if (const auto result = InputSpan<MmsUiLayoutSessionValueV1_2>(input, 2)) return result;
            std::vector<LayoutPreviewValue> values; values.reserve(size_t(input.count));
            for (size_t index = 0; index < input.count; ++index) {
                MmsUiLayoutSessionValueV1_2 value{};
                if (const auto result = InputRecord(input, index, value, 2)) return result;
                if (value.reserved || !value.fields || (value.fields & ~15u) || value.visible > 1 ||
                    (!(value.fields & PreviewVisible) && value.visible) ||
                    (!(value.fields & PreviewSize) && (value.width != 0 || value.height != 0)) ||
                    (!(value.fields & PreviewOrder) && value.order) ||
                    ((value.fields & PreviewParent) ? !value.parent : value.parent != 0)) return MMS_E_ARGUMENT;
                values.push_back({value.element, value.fields, value.visible != 0, value.width, value.height, value.order, value.parent});
            }
            return registry.ApplyLayoutPreview(*client, handle, revision, values);
        );
    };
    api.queryClaimEx = [](void* context, MmsHandle handle, uint64_t revision, uint32_t index,
        MmsUiLayoutSessionTargetV1_2* out) -> MmsResult {
        SESSION_EX_CALL(
            if (const auto result = OutputHeader(out, 2)) return result;
            const auto found = registry.layoutPreviews_.find(handle);
            if (found == registry.layoutPreviews_.end() || found->second.owner != client) return MMS_E_NOT_FOUND;
            const auto& session = found->second;
            if (session.info.revision != revision) return MMS_E_STALE;
            if (index >= session.values.size()) return MMS_E_ARGUMENT;
            const auto& claim = session.values[index];
            const auto fields = session.info.state == LayoutPreviewState::Active ? claim.fields : 0u;
            MmsUiLayoutSessionTargetV1_2 value{}; value.h = Header<MmsUiLayoutSessionTargetV1_2>(); value.h.minor = 2;
            value.element = claim.element; value.fields = fields; value.parent = (fields & PreviewParent) ? claim.parent : 0;
            *out = value; return MMS_OK;
        );
    };
#undef SESSION_EX_CALL
    return api;
}

MmsResult UiRegistry::BeginHostLayout(uint32_t mounts, uint64_t& out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (callbackDepth_) return MMS_E_BUSY;
    if (!mounts || (mounts & ~uint32_t(HostLayoutAll))) return MMS_E_ARGUMENT;
    const auto token = ReserveHostLayoutToken();
    if (!token) return MMS_E_BUSY;
    // Fixed slots and in-place masks cannot fail after ownership is acquired.
    // Publish the barrier before Changed can reenter plugin or native work.
    for (unsigned slot = 0; slot != 5; ++slot) if (mounts & (1u << slot)) hostLayoutTokens_[slot] = token;
    for (auto& [id, session] : layoutPreviews_) if (session.info.state == LayoutPreviewState::Active) {
        bool changed = false, remaining = false;
        for (auto& value : session.values) {
            if (value.fields) {
                const auto element = FindElement(elements, value.element);
                if (element != elements.end() && (PreviewMount(element->second, elements) & mounts)) {
                    value.fields = 0; changed = true;
                }
            }
            remaining |= value.fields != 0;
        }
        if (changed) {
            session.info.reasons |= MMS_LAYOUT_SESSION_HOST_INTERACTION;
            AdvancePreview(session.info);
            if (!remaining) session.info.state = LayoutPreviewState::Revoked;
        }
    }
    ReconcileParentPreviews(elements);
    Changed();
    // Reentrant native work can replace/end this gesture during notification.
    // The takeover already happened; only release our surviving slots, never
    // resurrect previews or clear a newer gesture's ownership.
    if (!IsHostLayoutCurrent(mounts, token)) { EndHostLayout(token); return MMS_E_STALE; }
    out = token;
    return MMS_OK;
}
void UiRegistry::EndHostLayout(uint64_t token) noexcept {
    if (GetCurrentThreadId() != thread_ || !token) return;
    for (auto& current : hostLayoutTokens_) if (current == token) current = 0;
}
bool UiRegistry::IsHostLayoutCurrent(uint32_t mounts, uint64_t token) const noexcept {
    if (GetCurrentThreadId() != thread_ || !token || !mounts || (mounts & ~uint32_t(HostLayoutAll))) return false;
    for (unsigned slot = 0; slot != 5; ++slot) if ((mounts & (1u << slot)) && hostLayoutTokens_[slot] != token) return false;
    return true;
}
bool UiRegistry::GeometryReady() const noexcept {
    return GetCurrentThreadId() == thread_ && geometryReady_ && geometryUiRevision_ == revision_;
}

EffectiveElementLayout UiRegistry::EffectiveLayout(const ElementRecord& element) const noexcept {
    EffectiveElementLayout result{element.visible, element.width, element.height, element.effectiveHints};
    for (const auto& [id, session] : layoutPreviews_) {
        if (session.info.state != LayoutPreviewState::Active || !session.applied) continue;
        for (const auto& value : session.values) if (value.element == element.handle) {
            if (value.fields & PreviewVisible) result.visible = value.visible;
            if (value.fields & PreviewSize) { result.width = value.width; result.height = value.height; }
            if (value.fields & PreviewOrder) result.hints.order = value.order;
        }
    }
    return result;
}

MmsString UiRegistry::PreviewParentOf(const ElementRecord& element,
    const std::map<std::string, ElementRecord>& tree, const std::vector<LayoutPreviewValue>* candidate) const noexcept {
    const auto selected = [&](const std::vector<LayoutPreviewValue>& values, MmsString& out) {
        for (const auto& value : values) if (value.element == element.handle && (value.fields & PreviewParent)) {
            const auto parent = FindElement(tree, value.parent);
            out = parent == tree.end() ? MmsString{} : parent->second.desc.id;
            return true;
        }
        return false;
    };
    MmsString result{};
    if (candidate && selected(*candidate, result)) return result;
    for (const auto& [handle, session] : layoutPreviews_)
        if (session.info.state == LayoutPreviewState::Active && session.applied && selected(session.values, result)) return result;
    return element.desc.parentId;
}
MmsString UiRegistry::EffectiveParent(const ElementRecord& element) const noexcept {
    return PreviewParentOf(element, elements);
}
bool UiRegistry::ValidParentPreview(const ElementRecord& element, MmsHandle parent,
    const std::map<std::string, ElementRecord>& tree, const std::vector<LayoutPreviewValue>* candidate) const noexcept {
    const auto mount = PreviewMount(element, tree);
    if (!mount || mount == HostLayoutWorkspace || !(element.desc.capabilities & MMS_UI_MOVE)) return false;
    const auto target = FindElement(tree, parent);
    if (target == tree.end() || parent == element.handle || target->second.nativeObserved) return false;
    const auto isRoot = [](const std::string& id) {
        return id == "mms.toolbar.main.extensions" || id == "mms.properties.main.extensions" ||
            id == "mms.sidebar.left.extensions" || id == "mms.sidebar.right.extensions";
    };
    if (!isRoot(target->first) && (target->second.desc.kind != MMS_UI_GROUP ||
        !(target->second.desc.capabilities & MMS_UI_CONTAINER) ||
        !SameText(target->second.desc.ownerPlugin, element.desc.ownerPlugin))) return false;
    auto current = FindElement(tree, element.handle);
    for (size_t depth = 0; depth <= tree.size(); ++depth) {
        if (current == tree.end() || current->second.nativeObserved) return false;
        if (isRoot(current->first)) {
            const uint32_t actual = current->first == "mms.toolbar.main.extensions" ? HostLayoutToolbar :
                current->first == "mms.properties.main.extensions" ? HostLayoutProperties :
                current->first == "mms.sidebar.left.extensions" ? HostLayoutLeft : HostLayoutRight;
            return actual == mount && current->second.desc.kind == MMS_UI_TOOLBAR;
        }
        if (depth && (current->second.desc.kind != MMS_UI_GROUP ||
            !SameText(current->second.desc.ownerPlugin, element.desc.ownerPlugin))) return false;
        current = FindId(tree, PreviewParentOf(current->second, tree, candidate));
    }
    return false; // Includes a batch-created or cross-session cycle.
}
std::vector<UiRegistry::LayoutParentDependency> UiRegistry::CaptureParentDependencies(
    const std::vector<LayoutPreviewValue>& values) const {
    std::vector<LayoutParentDependency> result;
    for (const auto& value : values) if (value.fields & PreviewParent) {
        // Retain both the saved ancestry and the proposed effective ancestry.
        // Keeping arenas prevents pointer reuse from disguising re-registration.
        for (bool effective : {false, true}) {
            auto current = FindElement(elements, effective ? value.parent : value.element);
            for (size_t depth = 0; current != elements.end() && depth <= elements.size(); ++depth) {
                const auto& e = current->second;
                result.push_back({value.element, e.handle, e.arena, e.desc.parentId});
                current = FindId(elements, effective ? PreviewParentOf(e, elements, &values) : e.desc.parentId);
            }
        }
    }
    return result;
}
void UiRegistry::ReconcileParentPreviews(const std::map<std::string, ElementRecord>& tree) noexcept {
    // Dropping an override can expose a saved ancestor relationship. Recheck
    // every remaining override until no dependent cycle or invalid root remains.
    bool again;
    do {
        again = false;
        for (auto& [id, session] : layoutPreviews_) if (session.info.state == LayoutPreviewState::Active) {
            bool changed = false, remaining = false;
            for (auto& value : session.values) {
                if (value.fields & PreviewParent) {
                    bool valid = true;
                    for (const auto& dependency : session.parents) if (dependency.element == value.element) {
                        const auto found = FindElement(tree, dependency.dependency);
                        if (found == tree.end() || found->second.arena != dependency.arena ||
                            !SameText(found->second.desc.parentId, dependency.parent)) { valid = false; break; }
                    }
                    if (valid && session.applied) {
                        const auto source = FindElement(tree, value.element);
                        valid = source != tree.end() && ValidParentPreview(source->second, value.parent, tree);
                    }
                    if (!valid) { value.fields &= ~uint32_t(PreviewParent); changed = again = true; }
                }
                remaining |= value.fields != 0;
            }
            if (changed) {
                session.info.reasons |= MMS_LAYOUT_SESSION_TARGET_CHANGED;
                AdvancePreview(session.info);
                if (!remaining) session.info.state = LayoutPreviewState::Revoked;
            }
        }
    } while (again);
}

bool UiRegistry::HasLayoutPreviewFields(MmsHandle element, uint32_t fields) const noexcept {
    if (!element || !fields || (fields & ~15u)) return false;
    for (const auto& [id, session] : layoutPreviews_) {
        if (session.info.state != LayoutPreviewState::Active || !session.applied) continue;
        for (const auto& value : session.values)
            if (value.element == element && (value.fields & fields)) return true;
    }
    return false;
}

ControlLayoutVisibility UiRegistry::ControlVisibility(const ElementRecord& element) const noexcept {
    auto current = FindElement(elements, element.handle);
    bool temporary = false, persistent = false;
    const auto onSavedPath = [&](MmsHandle target) noexcept {
        auto saved = FindElement(elements, element.handle);
        for (size_t depth = 0; saved != elements.end() && depth <= elements.size(); ++depth) {
            if (saved->second.handle == target) return true;
            saved = FindId(elements, saved->second.desc.parentId);
        }
        return false;
    };
    for (size_t depth = 0; depth <= elements.size(); ++depth) {
        if (current == elements.end()) return ControlLayoutVisibility::PersistentHidden;
        const auto& record = current->second;
        const bool anchor = current->first == "mms.toolbar.main.extensions" ||
            current->first == "mms.properties.main.extensions" ||
            current->first == "mms.sidebar.left.extensions" ||
            current->first == "mms.sidebar.right.extensions";
        if (record.nativeObserved || (anchor ? record.desc.kind != MMS_UI_TOOLBAR :
            (depth ? record.desc.kind != MMS_UI_GROUP :
                record.desc.kind != MMS_UI_BUTTON && record.desc.kind != MMS_UI_INPUT &&
                record.desc.kind != MMS_UI_GROUP && record.desc.kind != MMS_UI_SEPARATOR)))
            return ControlLayoutVisibility::PersistentHidden;

        if (!EffectiveLayout(record).visible) {
            // The active override owns the hide even when the saved baseline
            // is also hidden. A persistent hide higher up still takes priority.
            if (!HasLayoutPreviewFields(record.handle, PreviewVisible) && onSavedPath(record.handle)) persistent = true;
            else temporary = true; // Includes hidden ancestors introduced only by Parent.
        }
        if (anchor) return persistent ? ControlLayoutVisibility::PersistentHidden :
            temporary ? ControlLayoutVisibility::TemporaryHidden : ControlLayoutVisibility::Visible;
        current = FindId(elements, EffectiveParent(record));
    }
    // A broken or cyclic parent chain cannot expose a native control.
    return ControlLayoutVisibility::PersistentHidden;
}

MmsResult UiRegistry::ValidateLayoutPreview(UiClient& client, const std::vector<LayoutPreviewValue>& values, bool apply) const {
    if (client.registry != this || values.empty() || values.size() > 64) return MMS_E_ARGUMENT;
    for (size_t i = 0; i < values.size(); ++i) {
        const auto& value = values[i];
        if (!value.element || !value.fields || (value.fields & ~15u) || (!(value.fields & PreviewParent) && value.parent)) return MMS_E_ARGUMENT;
        for (size_t j = 0; j < i; ++j) if (values[j].element == value.element) return MMS_E_ARGUMENT;
        const auto found = FindElement(elements, value.element);
        if (found == elements.end()) return MMS_E_NOT_FOUND;
        const auto& e = found->second;
        if (!SameText(e.desc.ownerPlugin, Text(client.id))) return MMS_E_ARGUMENT;
        const auto mount = PreviewMount(e, elements);
        if (!mount) return MMS_E_UNSUPPORTED;
        for (unsigned slot = 0; slot != 5; ++slot) if ((mount & (1u << slot)) && hostLayoutTokens_[slot]) return MMS_E_BUSY;
        if (((value.fields & PreviewVisible) && !(e.desc.capabilities & MMS_UI_HIDE)) ||
            ((value.fields & PreviewSize) && !(e.desc.capabilities & MMS_UI_RESIZE)) ||
            ((value.fields & PreviewOrder) && !(e.desc.capabilities & MMS_UI_LAYOUT))) return MMS_E_UNSUPPORTED;
        if (value.fields & PreviewParent) {
            if (!(e.desc.capabilities & MMS_UI_MOVE) || mount == HostLayoutWorkspace) return MMS_E_UNSUPPORTED;
            if (!ValidParentPreview(e, value.parent, elements, &values)) return MMS_E_ARGUMENT;
        }
        if (apply && (value.fields & PreviewSize)) {
            const auto& hints = e.effectiveHints;
            const float sizes[]{value.width, value.height};
            for (unsigned axis = 0; axis != 2; ++axis) if (!std::isfinite(sizes[axis]) || sizes[axis] <= 0 || sizes[axis] > 32768 ||
                sizes[axis] < hints.minimum[axis] || (hints.maximum[axis] && sizes[axis] > hints.maximum[axis])) return MMS_E_ARGUMENT;
        }
    }
    // An otherwise valid batch may invalidate a different active group's
    // override. Refuse the whole batch rather than silently cancelling it.
    for (const auto& [id, session] : layoutPreviews_) if (session.info.state == LayoutPreviewState::Active && session.applied)
        for (const auto& claim : session.values) if (claim.fields & PreviewParent) {
            if (std::any_of(values.begin(), values.end(), [&](const auto& v) { return v.element == claim.element && (v.fields & PreviewParent); })) continue;
            const auto source = FindElement(elements, claim.element);
            if (source == elements.end() || !ValidParentPreview(source->second, claim.parent, elements, &values)) return MMS_E_ARGUMENT;
        }
    return MMS_OK;
}

MmsResult UiRegistry::BeginLayoutPreview(UiClient& client, const std::vector<LayoutPreviewValue>& targets, MmsHandle& out) {
    return Boundary([&]() -> MmsResult {
        if (const auto result = Check(client, true)) return result;
        if (const auto result = ValidateLayoutPreview(client, targets, false)) return result;
        size_t owned = 0;
        for (const auto& [id, session] : layoutPreviews_) {
            if (session.owner == &client) ++owned;
            if (session.info.state != LayoutPreviewState::Active) continue;
            for (const auto& claim : session.values) for (const auto& target : targets)
                if (claim.element == target.element && (claim.fields & target.fields)) return MMS_E_CONFLICT;
        }
        if (owned >= 32 || layoutPreviews_.size() >= 256 || next_ == UINT64_MAX) return MMS_E_BUSY;
        LayoutPreview candidate; candidate.owner = &client; candidate.values = targets;
        candidate.parents = CaptureParentDependencies(targets);
        const auto handle = next_;
        layoutPreviews_.emplace(handle, std::move(candidate)); ++next_; out = handle;
        return MMS_OK;
    });
}

MmsResult UiRegistry::ApplyLayoutPreview(UiClient& client, MmsHandle handle, uint64_t expected, const std::vector<LayoutPreviewValue>& values) {
    return Boundary([&]() -> MmsResult {
        if (const auto result = Check(client, true)) return result;
        const auto found = layoutPreviews_.find(handle);
        if (found == layoutPreviews_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        auto& session = found->second;
        if (session.info.state != LayoutPreviewState::Active) return MMS_E_NOT_READY;
        if (session.info.revision != expected) return MMS_E_STALE;
        // Reserve the final revision for a terminal commit/rollback. A
        // successful Apply must remain visible, never immediately revoke.
        if (session.info.revision >= UINT64_MAX - 1) return MMS_E_BUSY;
        size_t index = 0;
        for (const auto& claim : session.values) if (claim.fields) {
            if (index == values.size() || values[index].element != claim.element || values[index].fields != claim.fields) return MMS_E_ARGUMENT;
            ++index;
        }
        if (index != values.size()) return MMS_E_ARGUMENT;
        if (const auto result = ValidateLayoutPreview(client, values, true)) return result;
        auto candidate = session.values;
        index = 0;
        for (auto& claim : candidate) if (claim.fields) claim = values[index++];
        auto parents = CaptureParentDependencies(candidate);
        session.parents.swap(parents);
        session.values.swap(candidate); session.applied = true; AdvancePreview(session.info);
        // Invalidate geometry through the same revision and native scheduler.
        // Preferences, their revision, layout history and scene state stay put.
        Changed(); return MMS_OK;
    });
}

MmsResult UiRegistry::QueryLayoutPreview(UiClient& client, MmsHandle handle, LayoutPreviewInfo& out) const {
    if (const auto result = Check(client)) return result;
    const auto found = layoutPreviews_.find(handle);
    if (found == layoutPreviews_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    out = found->second.info; return MMS_OK;
}

MmsResult UiRegistry::EndLayoutPreview(UiClient& client, MmsHandle handle, uint64_t expected, bool commit) {
    return Boundary([&]() -> MmsResult {
        if (const auto result = Check(client, commit)) return result;
        if (callbackDepth_) return MMS_E_BUSY;
        const auto found = layoutPreviews_.find(handle);
        if (found == layoutPreviews_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        auto& session = found->second;
        if (session.info.state != LayoutPreviewState::Active) return MMS_E_NOT_READY;
        if (session.info.revision != expected) return MMS_E_STALE;
        if (session.info.revision == UINT64_MAX) return MMS_E_BUSY;
        if (!commit || !session.applied) {
            session.info.state = commit ? LayoutPreviewState::Committed : LayoutPreviewState::RolledBack;
            ++session.info.revision; ReconcileParentPreviews(elements); Changed(); return MMS_OK;
        }
        // Build an ordinary layout transaction. Failure consumes only this
        // private transaction; the active preview and history stay unchanged.
        MmsHandle transaction = 0;
        if (const auto result = Begin(client, revision_, &transaction)) return result;
        struct Cleanup {
            MmsUiApiV1 api; MmsHandle transaction;
            ~Cleanup() { api.cancelLayout(api.context, transaction); }
        } cleanup{Api(client), transaction};
        for (const auto& value : session.values) {
            if (!value.fields) continue;
            const auto element = FindElement(elements, value.element);
            if (element == elements.end()) return MMS_E_STALE;
            const auto& e = element->second;
            MmsLayoutOpV1 op{}; op.h = Header<MmsLayoutOpV1>(); op.elementId = e.desc.id; op.expectedGeneration = e.desc.generation;
            if (value.fields & PreviewParent) {
                const auto parent = FindElement(elements, value.parent);
                if (parent == elements.end()) return MMS_E_STALE;
                op.operation = MMS_LAYOUT_MOVE; op.parentId = parent->second.desc.id;
                if (const auto result = Apply(client, transaction, &op)) return result;
            }
            if (value.fields & PreviewVisible) {
                op.operation = MMS_LAYOUT_VISIBLE; op.value.visible = value.visible;
                if (const auto result = Apply(client, transaction, &op)) return result;
            }
            if (value.fields & PreviewSize) {
                op.operation = MMS_LAYOUT_SIZE; op.value.size = {value.width, value.height};
                if (const auto result = Apply(client, transaction, &op)) return result;
            }
            if (value.fields & PreviewOrder) {
                auto hints = e.effectiveHints; hints.order = value.order;
                if (const auto result = StageHints(client, transaction, e.desc.id, e.desc.generation, &hints)) return result;
            }
        }
        return Commit(client, transaction, handle);
    });
}

MmsResult UiRegistry::ReleaseLayoutPreview(UiClient& client, MmsHandle handle) {
    if (const auto result = Check(client)) return result;
    if (callbackDepth_) return MMS_E_BUSY;
    const auto found = layoutPreviews_.find(handle);
    if (found == layoutPreviews_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    if (found->second.info.state == LayoutPreviewState::Active) return MMS_E_BUSY;
    layoutPreviews_.erase(found); return MMS_OK;
}

void UiRegistry::RevokeLayoutPreviewFields(MmsHandle target, uint32_t fields, uint32_t reason) noexcept {
    for (auto& [id, session] : layoutPreviews_) if (session.info.state == LayoutPreviewState::Active) {
        bool changed = false, remaining = false;
        for (auto& value : session.values) {
            if ((!target || target == value.element) && (value.fields & fields)) { value.fields &= ~fields; changed = true; }
            if (target && (value.fields & PreviewParent) && ((fields & PreviewParent) || (reason & MMS_LAYOUT_SESSION_TARGET_CHANGED)))
                for (const auto& dependency : session.parents) if (dependency.element == value.element && dependency.dependency == target) {
                    value.fields &= ~uint32_t(PreviewParent); changed = true; break;
                }
            remaining |= value.fields != 0;
        }
        if (changed) {
            session.info.reasons |= reason;
            AdvancePreview(session.info);
            if (!remaining) session.info.state = LayoutPreviewState::Revoked;
        }
    }
}
void UiRegistry::RevokeAllLayoutPreviews() noexcept { RevokeLayoutPreviewFields(0, 15); }
void UiRegistry::RevokeLayoutPreviews(UiClient& client) noexcept {
    if (GetCurrentThreadId() != thread_ || callbackDepth_) return;
    bool changed = false;
    for (auto& [id, session] : layoutPreviews_) if (session.owner == &client && session.info.state == LayoutPreviewState::Active) {
        session.info.reasons |= MMS_LAYOUT_SESSION_STOPPING;
        AdvancePreview(session.info); session.info.state = LayoutPreviewState::Revoked;
        changed = true;
    }
    if (changed) { ReconcileParentPreviews(elements); Changed(); }
}

void UiRegistry::ReconcileLayoutPreviews(const std::map<std::string, ElementRecord>& next, MmsHandle committedPreview) noexcept {
    if (std::none_of(layoutPreviews_.begin(), layoutPreviews_.end(), [](const auto& entry) {
        return entry.second.info.state == LayoutPreviewState::Active;
    })) return;
    for (const auto& [id, before] : elements) {
        const auto found = next.find(id);
        if (found == next.end() || found->second.handle != before.handle ||
            (PreviewMount(before, elements) && !PreviewMount(found->second, next))) {
            RevokeLayoutPreviewFields(before.handle, 15, MMS_LAYOUT_SESSION_TARGET_CHANGED); continue;
        }
        const auto& after = found->second;
        uint32_t fields = before.visible != after.visible ? PreviewVisible : 0;
        uint32_t reason = MMS_LAYOUT_SESSION_PERSISTENT_EDIT;
        if (before.width != after.width || before.height != after.height ||
            std::memcmp(before.effectiveHints.minimum, after.effectiveHints.minimum, sizeof(after.effectiveHints.minimum)) ||
            std::memcmp(before.effectiveHints.maximum, after.effectiveHints.maximum, sizeof(after.effectiveHints.maximum))) fields |= PreviewSize;
        if (before.effectiveHints.order != after.effectiveHints.order) fields |= PreviewOrder;
        const auto& current = elements;
        auto ancestor = current.find(id);
        for (size_t depth = 0; ancestor != elements.end() && depth <= elements.size(); ++depth) {
            const auto changed = next.find(ancestor->first);
            if (changed == next.end() || changed->second.handle != ancestor->second.handle) {
                fields = 15; reason |= MMS_LAYOUT_SESSION_TARGET_CHANGED; break;
            }
            if (!SameText(ancestor->second.desc.parentId, changed->second.desc.parentId)) {
                const auto committed = layoutPreviews_.find(committedPreview);
                const bool ownParent = committed != layoutPreviews_.end() &&
                    std::any_of(committed->second.values.begin(), committed->second.values.end(), [&](const auto& value) {
                        return value.element == ancestor->second.handle && (value.fields & PreviewParent);
                    });
                if (!ownParent) { fields = 15; reason |= MMS_LAYOUT_SESSION_TARGET_CHANGED; break; }
                fields |= PreviewParent;
            }
            ancestor = FindId(elements, ancestor->second.desc.parentId);
        }
        if (fields) RevokeLayoutPreviewFields(before.handle, fields, reason);
    }
    ReconcileParentPreviews(next);
}
}
