#include "native_model_source.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "history_runtime.hpp"
#include "native_object_commands.hpp"
#include "native_pose_scope.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <memory>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
constexpr std::size_t kModelPathChars = 256;
using ModelPath = std::array<wchar_t, kModelPathChars>;

// The application edit gate stays authoritative. Only the original
// publication window may relax the UI lock and scene-mutation exclusion; a
// save-blocked serialization is refused unconditionally.
bool Ready(MMDApp& app, bool publication) noexcept {
    const bool authorized = publication && NativeObjectPublicationAuthorized(&app);
    return (!app.SceneMutationInProgress() || authorized) && (!bake::UiLocked() || authorized) &&
        !SceneSaveBlocksEdits() && !NativeObjectUiPending(&app) && !GetCapture() &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.RecordingWindow() && !app.FrameRangeDialog() && !app.state.frameCopyDialog &&
        !app.GravitySettingDialog() && !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() &&
        !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app) && app.MainWindow() &&
        GetWindowThreadProcessId(app.MainWindow(), nullptr) == GetCurrentThreadId();
}

bool TextValid(const wchar_t* text, std::size_t length) noexcept {
    for (std::size_t i = 0; i < length; ++i) {
        const auto value = std::uint16_t(text[i]);
        if (!value) return false;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (++i == length || std::uint16_t(text[i]) < 0xdc00 || std::uint16_t(text[i]) > 0xdfff) return false;
        } else if (value >= 0xdc00 && value <= 0xdfff) return false;
    }
    return true;
}

bool Terminated(const ModelPath& path) noexcept {
    return wcsnlen_s(path.data(), path.size()) < path.size();
}

std::uint64_t ObjectGeneration(const unsigned char* object) noexcept {
    MmsObjectRef identity{};
    return FindSceneObjectIdentity(object, identity) ? identity.generation : 0;
}

bool SameId(const MmsObjectRef& identity, const MmsId& id) noexcept {
    return identity.id.lo == id.lo && identity.id.hi == id.hi;
}
}

struct NativeModelSourcePlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    MMDApp& app;
    SharedHistory* const journal;
    SharedHistory* const permission;
    const unsigned slot;
    unsigned char* const object;
    const MmsObjectRef initial;
    const std::uint64_t binding, scene, sceneRevision, revision;
    std::shared_ptr<RetainedSceneObject> owner;
    ModelPath before{}, after{};
    std::uint64_t serial = 1, retainedBytes = 0;
    bool taken = false, applied = false, published = false;

    State(MMDApp& value, unsigned slotValue, SharedHistory* journalValue, SharedHistory* permissionValue,
          std::uint64_t revisionValue)
        : app(value), journal(journalValue), permission(permissionValue), slot(slotValue),
          object(value.ModelSlot(slotValue)), initial(SceneObjectIdentity(object)),
          binding(BoundNativePoseGeneration(&value)), scene(value.SceneGeneration()),
          sceneRevision(value.SceneRevision()), revision(revisionValue) {}

    // Structural source check. Identity is verified through the live scene
    // membership before the model object is dereferenced. The first direction
    // additionally pins the original binding and object generation; later
    // preparations accept a restored object with a new generation.
    bool Source(bool forward, bool first) const noexcept {
        if (BoundNativePoseHistory(&app) != journal) return false;
        if (!journal && NativeHistory(&app) != permission) return false;
        if (app.SceneGeneration() != scene || app.ModelSlot(slot) != object) return false;
        if (!owner || !owner->Present()) return false;
        MmsObjectRef identity{};
        if (!FindSceneObjectIdentity(object, identity) || !SameId(identity, initial.id)) return false;
        if (!SceneObjectPresent(object, initial.id)) return false;
        if (first && (BoundNativePoseGeneration(&app) != binding || identity.generation != initial.generation))
            return false;
        const ModelPath& expected = forward ? before : after;
        return std::memcmp(mdl::Mdl(object)->path, expected.data(), sizeof(expected)) == 0;
    }

    bool InitialVersionsCurrent() const noexcept {
        if (app.SceneRevision() != sceneRevision) return false;
        if (!permission) return true;
        std::uint64_t current = 0;
        return permission->Revision(current) == MMS_OK && current == revision;
    }

    bool InitialCurrent() const noexcept {
        return Source(true, true) && InitialVersionsCurrent();
    }

    void Measure(resource_budget::Collector& collector) const {
        collector.Add(this, sizeof(*this) + 4 * sizeof(void*), 1);
    }

    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> state;
        const bool forward, first;
        const std::uint64_t serial, binding, generation, revision;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;

        Candidate(std::shared_ptr<State> source, bool direction, std::uint64_t currentRevision)
            : state(std::move(source)), forward(direction), first(!state->published), serial(state->serial),
              binding(BoundNativePoseGeneration(&state->app)), generation(ObjectGeneration(state->object)),
              revision(currentRevision) {}

        // No-allocation revalidation: the resource ledger is verified with
        // CoversBookkeeping, which cannot insert new charges.
        bool Metadata() const noexcept {
            const auto& source = *state;
            if (consumed || source.serial != serial || forward == source.applied || !source.taken) return false;
            if (!source.Source(forward, first)) return false;
            if (BoundNativePoseGeneration(&source.app) != binding || ObjectGeneration(source.object) != generation)
                return false;
            if (first && !source.InitialVersionsCurrent()) return false;
            if (!Ready(source.app, true)) return false;
            if (source.permission) {
                std::uint64_t current = 0;
                if (source.permission->Revision(current) != MMS_OK || current != revision) return false;
            }
            return true;
        }

        bool IsCurrent() const noexcept override {
            return Metadata() && state->owner && state->owner->CoversBookkeeping(resources);
        }

        void Publish() noexcept override {
            // The caller validated a fresh IsCurrent immediately before this
            // void publication. Keep only no-call scalar invariants here: no
            // metadata/source/version query, owner access, Win32/COM/file call
            // or allocation is permitted inside Publish.
            if (consumed || state->serial != serial || forward == state->applied || !state->taken)
                std::terminate();
            const ModelPath& expected = forward ? state->after : state->before;
            std::memcpy(mdl::Mdl(state->object)->path, expected.data(), sizeof(expected));
            state->applied = forward; state->published = true; ++state->serial; consumed = true;
        }

        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };

    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!taken || forward == applied) return MMS_E_STALE;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            const bool first = !published;
            if (!Source(forward, first)) return MMS_E_STALE;
            if (first && !InitialVersionsCurrent()) return MMS_E_STALE;
            if (!Ready(app, true)) return MMS_E_BUSY;
            std::uint64_t currentRevision = 0;
            if (permission) if (const auto result = permission->Revision(currentRevision)) return result;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward, currentRevision);
            next->resources.Add(this, sizeof(*this) + 4 * sizeof(void*), 1);
            if (const auto result = owner->MeasureBookkeeping(next->resources)) return result;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            next->bytes = next->resources.Result().totalBytes;
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next);
            return MMS_OK;
        });
    }

    std::uint64_t RetainedBytes() const noexcept override { return retainedBytes; }
    bool RequiresPhysicsReset() const noexcept override { return false; }
    HistoryEffects Effects() const noexcept override { return HistoryEffects::ProjectData; }
};

MmsResult NativeModelSourcePlan::Begin(MMDApp& app, unsigned slot) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        if (slot >= unsigned(kModelSlotCount) || !app.ModelSlot(slot)) return MMS_E_ARGUMENT;
        if (!Ready(app, false)) return MMS_E_BUSY;
        auto* journal = BoundNativePoseHistory(&app);
        auto* permission = journal ? journal : NativeHistory(&app);
        if (permission) {
            if (const auto result = permission->CheckNativeEdit()) return result;
        } else if (InCallback()) {
            // Without a bound or direct manager journal there is no edit gate
            // to consult; a plugin callback must never mint one implicitly.
            return MMS_E_BUSY;
        }
        std::uint64_t revision = 0;
        if (permission) if (const auto result = permission->Revision(revision)) return result;
        auto state = std::make_shared<State>(app, slot, journal, permission, revision);
        state->owner = RetainSceneObject(app, MMS_OBJECT_MODEL, slot);
        std::memcpy(state->before.data(), mdl::Mdl(state->object)->path, sizeof(state->before));
        state->after = state->before;
        if (!Terminated(state->before)) return MMS_E_NOT_READY;
        if (!state->InitialCurrent()) return MMS_E_STALE;
        state_ = std::move(state);
        return MMS_OK;
    });
}

MmsResult NativeModelSourcePlan::Set(const wchar_t* path) noexcept {
    if (!path) return MMS_E_ARGUMENT;
    if (!state_ || state_->taken) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& state = *state_;
        if (!state.InitialCurrent()) return MMS_E_STALE;
        if (!Ready(state.app, true)) return MMS_E_BUSY;
        const auto length = wcsnlen_s(path, kModelPathChars);
        if (length == 0 || length >= kModelPathChars || !TextValid(path, length)) return MMS_E_ARGUMENT;
        // wcscpy_s semantics: overwrite the string and its terminator only.
        // Bytes after the new terminator keep the captured old tail, and an
        // identical string leaves the complete old buffer untouched.
        ModelPath next = state.before;
        const auto prefix = (length + 1) * sizeof(wchar_t);
        if (std::memcmp(next.data(), path, prefix) != 0) std::memcpy(next.data(), path, prefix);
        if (!state.InitialCurrent()) return MMS_E_STALE;
        state.after = std::move(next);
        return MMS_OK;
    });
}

MmsResult NativeModelSourcePlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || state_->taken) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& state = *state_;
        if (!state.owner || !state.InitialCurrent()) return MMS_E_STALE;
        if (!Ready(state.app, true)) return MMS_E_BUSY;
        // Freeze the complete retained cost once; every later preparation
        // remeasures the dynamic owner and reports the same full charge.
        resource_budget::Collector resources;
        state.Measure(resources);
        if (const auto result = state.owner->MeasureBookkeeping(resources)) return result;
        if (!resources.Result().Complete()) return MMS_E_NOT_READY;
        if (!state.InitialCurrent()) return MMS_E_STALE;
        state.retainedBytes = resources.Result().totalBytes;
        state.taken = true;
        if (std::memcmp(state.before.data(), state.after.data(), sizeof(state.before)) != 0) out = state_;
        return MMS_OK;
    });
}

bool NativeModelSourcePlan::IsCurrent() const noexcept {
    const auto* state = state_.get();
    if (!state) return false;
    // Validate the captured pointers and generations before dereferencing any
    // captured journal. The current selection is intentionally irrelevant.
    if (BoundNativePoseHistory(&state->app) != state->journal) return false;
    if (!state->journal && NativeHistory(&state->app) != state->permission) return false;
    if (BoundNativePoseGeneration(&state->app) != state->binding) return false;
    if (!state->Source(true, true) || !state->InitialVersionsCurrent()) return false;
    return Ready(state->app, true);
}

unsigned char* NativeModelSourcePlan::Object() const noexcept {
    return state_ ? state_->object : nullptr;
}

SharedHistory* NativeModelSourcePlan::Journal() const noexcept {
    return state_ ? state_->journal : nullptr;
}

SharedHistory* NativeModelSourcePlan::PermissionHistory() const noexcept {
    return state_ ? state_->permission : nullptr;
}
}
#endif
