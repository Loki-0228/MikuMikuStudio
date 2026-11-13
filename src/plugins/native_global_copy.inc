#include "native_global_copy.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/app_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model_layout.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
namespace global_copy_detail {

using Status = frame_clipboard::GlobalCopyStatus;
constexpr int accessorySlotCount = 255;
static_assert(kModelSlotCount <= 255, "global copy model reference capacity");

bool SameIdentity(const MmsObjectRef& left, const MmsObjectRef& right) noexcept {
    return left.id.lo == right.id.lo && left.id.hi == right.id.hi &&
        left.generation == right.generation;
}

void CaptureAccessory(const MMDApp& app, GlobalCopySnapshot& snapshot,
                      std::uint8_t slot) noexcept {
    auto& ref = snapshot.accessoryReferences[slot];
    if (ref.referenced) return;
    ref.referenced = true;
    ref.slot = slot;
    if (slot >= accessorySlotCount) return;
    ref.pointer = app.AccessorySlot(slot);
    ref.available = ref.pointer && FindSceneObjectIdentity(ref.pointer, ref.identity);
}

void CaptureModel(const MMDApp& app, GlobalCopySnapshot& snapshot,
                  std::int32_t slot) noexcept {
    if (slot == -1) return;
    if (slot < 0 || slot >= kModelSlotCount) {
        auto& ref = snapshot.invalidModelReference;
        if (!ref.referenced) {
            ref.referenced = true;
            ref.slot = slot;
        }
        return;
    }
    auto& ref = snapshot.modelReferences[slot];
    if (ref.referenced) return;
    ref.referenced = true;
    ref.slot = slot;
    ref.pointer = app.ModelSlot(slot);
    ref.available = ref.pointer && FindSceneObjectIdentity(ref.pointer, ref.identity);
    if (!ref.available) return;
    // This pointer came from the live slot, with an existing live identity.
    // The table is a structural stamp; no bone contents or indices are read.
    const auto* model = reinterpret_cast<const mdl::ModelRecord*>(ref.pointer);
    ref.boneTable = model->boneTable;
    ref.boneCount = model->boneCount;
}

bool EnvelopeCurrent(const MMDApp& app, const GlobalCopySnapshot& snapshot) noexcept {
    const auto& state = app.GlobalClipboardCopy();
    return state.status == Status::Captured && state.snapshot.get() == &snapshot &&
        state.attempt == snapshot.attempt && state.sceneGeneration == snapshot.sceneGeneration &&
        state.sceneGeneration == app.SceneGeneration();
}

MmsResult Fail(frame_clipboard::GlobalCopyState& state, MmsResult error) noexcept {
    state.snapshot.reset();
    state.status = Status::Failed;
    state.error = error;
    state.preparing = false;
    return error;
}

}
}

bool BeginGlobalCopy(MMDApp& app) noexcept {
    auto& state = app.GlobalClipboardCopy();
    global_copy_detail::Fail(state, MMS_E_NOT_READY);
    state.sceneGeneration = app.SceneGeneration();
    if (state.attempt == (std::numeric_limits<std::uint64_t>::max)()) return false;
    ++state.attempt;
    state.preparing = true;
    return true;
}

void CompleteEmptyGlobalCopy(MMDApp& app) noexcept {
    auto& state = app.GlobalClipboardCopy();
    if (!state.preparing || state.sceneGeneration != app.SceneGeneration()) {
        global_copy_detail::Fail(state, MMS_E_NOT_READY);
        return;
    }
    state.snapshot.reset();
    state.status = frame_clipboard::GlobalCopyStatus::Empty;
    state.error = MMS_OK;
    state.preparing = false;
}

MmsResult CaptureGlobalCopySnapshot(MMDApp& app, std::uint32_t nativeFirst) noexcept {
    using Status = frame_clipboard::GlobalCopyStatus;
    auto& state = app.GlobalClipboardCopy();
    // Never reconstruct a begin from the clipboard, playhead or selection.
    const bool begun = state.preparing && state.status == Status::Failed && state.error == MMS_E_NOT_READY &&
        !state.snapshot && state.attempt != 0;
    const auto attempt = state.attempt;
    const auto sceneGeneration = state.sceneGeneration;
    global_copy_detail::Fail(state, MMS_E_NOT_READY);
    if (!begun) return MMS_E_NOT_READY;
    if (sceneGeneration != app.SceneGeneration())
        return global_copy_detail::Fail(state, MMS_E_STALE);

    try {
        auto snapshot = std::make_shared<GlobalCopySnapshot>();
        snapshot->nativeFirst = nativeFirst;
        snapshot->attempt = attempt;
        snapshot->sceneGeneration = sceneGeneration;
        if (const auto result = CaptureGlobalPastePayload(app, snapshot->payload))
            return global_copy_detail::Fail(state, result);
        snapshot->payload.frameOffset = 0;
        for (const auto& key : snapshot->payload.cameras)
            global_copy_detail::CaptureModel(app, *snapshot, key.parentModel);
        for (const auto& key : snapshot->payload.accessories) {
            global_copy_detail::CaptureAccessory(app, *snapshot, key.slot);
            global_copy_detail::CaptureModel(app, *snapshot, key.parentModel);
        }

        std::vector<curve_host::CopyAuthoringTarget> targets;
        const auto add = [&](curve_host::Kind kind, int slot = 0) {
            if (std::none_of(targets.begin(), targets.end(), [&](const auto& target) {
                return target.kind == kind && target.slot == slot;
            })) targets.push_back({kind, slot});
        };
        if (!snapshot->payload.cameras.empty()) add(curve_host::Kind::Camera);
        if (!snapshot->payload.lights.empty()) add(curve_host::Kind::Light);
        if (!snapshot->payload.shadows.empty()) add(curve_host::Kind::Shadow);
        if (!snapshot->payload.gravity.empty()) add(curve_host::Kind::Gravity);
        for (const auto& key : snapshot->payload.accessories) {
            // An unavailable reference remains raw payload for admission.
            // Do not dereference it or invent authoring at copy time.
            if (snapshot->accessoryReferences[key.slot].available)
                add(curve_host::Kind::Accessory, key.slot);
        }
        std::wstring error;
        if (!targets.empty() && !curve_host::CaptureCopyAuthoring(app, targets, snapshot->authoring, error))
            return global_copy_detail::Fail(state, MMS_E_ARGUMENT);

        if (state.attempt != attempt || state.sceneGeneration != sceneGeneration ||
            app.SceneGeneration() != sceneGeneration || state.status != Status::Failed ||
            state.error != MMS_E_NOT_READY || state.snapshot || state.preparing)
            return MMS_E_STALE; // A newer attempt must keep its own outcome.
        // Publish only after every owned field and reference has been captured.
        // No mutable alias survives this conversion; publication cannot allocate.
        state.snapshot = std::move(snapshot);
        state.error = MMS_OK;
        state.status = Status::Captured;
        return MMS_OK;
    } catch (const std::bad_alloc&) {
        return global_copy_detail::Fail(state, MMS_E_MEMORY);
    } catch (...) {
        return global_copy_detail::Fail(state, MMS_E_FAILED);
    }
}

frame_clipboard::GlobalCopyStatus ReadGlobalCopySnapshot(
    const MMDApp& app, std::shared_ptr<const GlobalCopySnapshot>& out) noexcept {
    const auto& state = app.GlobalClipboardCopy();
    // Copy before clearing out so passing an existing snapshot handle is safe.
    const auto snapshot = state.snapshot;
    if (snapshot && global_copy_detail::EnvelopeCurrent(app, *snapshot)) out = snapshot;
    else {
        out.reset();
        if (state.status == frame_clipboard::GlobalCopyStatus::Captured)
            return frame_clipboard::GlobalCopyStatus::Failed;
    }
    return state.status;
}

bool GlobalCopySnapshotCurrent(const MMDApp& app, const GlobalCopySnapshot& snapshot) noexcept {
    if (!global_copy_detail::EnvelopeCurrent(app, snapshot) ||
        snapshot.invalidModelReference.referenced) return false;
    for (const auto& ref : snapshot.accessoryReferences) {
        if (!ref.referenced) continue;
        if (!ref.available || !ref.pointer || ref.slot < 0 ||
            ref.slot >= global_copy_detail::accessorySlotCount ||
            app.AccessorySlot(ref.slot) != ref.pointer) return false;
        MmsObjectRef identity{};
        if (!FindSceneObjectIdentity(ref.pointer, identity) ||
            !global_copy_detail::SameIdentity(identity, ref.identity)) return false;
    }
    for (const auto& ref : snapshot.modelReferences) {
        if (!ref.referenced) continue;
        if (!ref.available || !ref.pointer || ref.slot < 0 || ref.slot >= kModelSlotCount ||
            app.ModelSlot(ref.slot) != ref.pointer) return false;
        MmsObjectRef identity{};
        if (!FindSceneObjectIdentity(ref.pointer, identity) ||
            !global_copy_detail::SameIdentity(identity, ref.identity)) return false;
        // Slot and identity checks precede dereferencing the borrowed pointer.
        const auto* model = reinterpret_cast<const mdl::ModelRecord*>(ref.pointer);
        if (model->boneTable != ref.boneTable || model->boneCount != ref.boneCount) return false;
    }
    return true;
}

}
#endif
