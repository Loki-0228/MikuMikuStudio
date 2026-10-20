#include "native_model_outline.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
// The journal's authorized publication phase (undo/redo always, and a caller
// that wraps its commit in the object-operation publication guard) may hold the
// scene-mutation and UI locks. Without that explicit authorization the locks
// still reject. No dialog, selection, capture or camera-mode check belongs
// here; those gates belong to the caller.
bool Ready(MMDApp& app, bool replay) noexcept {
    const bool authorized = replay && NativeObjectPublicationAuthorized(&app);
    return !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        (!app.SceneMutationInProgress() || authorized) && (!bake::UiLocked() || authorized) &&
        !SceneSaveBlocksEdits();
}
// Editing comparisons use the raw float bytes. A negative zero, a NaN payload
// or a subnormal must not be treated as an equal authored value.
bool SameBits(float left, float right) noexcept {
    return !std::memcmp(&left, &right, sizeof(float));
}
bool GenerationIs(const void* object, std::uint64_t generation) noexcept {
    // SceneObjectIdentity may throw while the identity registry is being torn
    // down. Candidate callbacks are noexcept, so a failed lookup is mismatch.
    try {
        return SceneObjectIdentity(object).generation == generation;
    } catch (...) {
        return false;
    }
}
std::uint64_t Add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}
}
struct NativeModelOutlinePlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    MMDApp& app;
    SharedHistory* const history;
    const unsigned slot;
    unsigned char* const object;
    const MmsObjectRef initial;
    const std::shared_ptr<RetainedSceneObject> owner;
    const std::uint64_t binding, scene, revision;
    const float before;
    float after;
    std::uint64_t bookkeeping = 0, serial = 1;
    bool preview = false, applied = false, published = false, cancelled = false;

    State(MMDApp& value, SharedHistory& journal, unsigned index, std::uint64_t currentRevision)
        : app(value), history(&journal), slot(index), object(value.ModelSlot(static_cast<int>(index))),
          initial(SceneObjectIdentity(object)), owner(RetainSceneObject(value, MMS_OBJECT_MODEL, index)),
          binding(BoundNativePoseGeneration(&value)), scene(value.SceneGeneration()),
          revision(currentRevision), before(mdl::Mdl(object)->edgeScale), after(before) {}

    // The bound pointer is checked before the cached journal is dereferenced:
    // the binding (and with it the journal) can be destroyed while a wrapper or
    // a retired node still holds this state.
    bool Bound() const noexcept { return BoundNativePoseHistory(&app) == history; }
    // Same live object in the original scene. The stable id survives deletion
    // and a later controlled undo restore changes only the generation.
    bool Present() const noexcept {
        return owner && owner->Present() && app.ModelSlot(static_cast<int>(slot)) == object &&
            SceneObjectPresent(object, initial.id) && app.SceneGeneration() == scene;
    }
    float Live() const noexcept { return mdl::Mdl(object)->edgeScale; }
    bool Changed() const noexcept { return !SameBits(before, after); }

    // Begin-only domain: the target may not have been replaced, rebound, edited
    // by another journal entry or moved to a new scene epoch.
    bool InitialCurrent() const noexcept {
        if (!Bound() || !Present() || !GenerationIs(object, initial.generation)) return false;
        if (BoundNativePoseGeneration(&app) != binding) return false;
        std::uint64_t current = 0;
        return !history->Revision(current) && current == revision;
    }

    // Called after PrepareNative reserved its Adjustment token. This
    // path is deliberately allocation-free: no Boundary, no candidate, no
    // messages, and exactly one field write.
    MmsResult PreviewValue(float value) noexcept {
        if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        if (published || cancelled) return MMS_E_STALE;
        if (!Bound() || !Present() || !GenerationIs(object, initial.generation)) return MMS_E_STALE;
        if (BoundNativePoseGeneration(&app) != binding) return MMS_E_STALE;
        std::uint64_t current = 0;
        if (history->Revision(current) || current != revision) return MMS_E_STALE;
        if (!SameBits(Live(), preview ? after : before)) return MMS_E_STALE;
        after = value;
        preview = true;
        mdl::Mdl(object)->edgeScale = after;
        return MMS_OK;
    }

    // Cancel never dereferences the journal. Dropping the wrapper before any
    // Preview must not invalidate a staged HistoryChange: the caller may still
    // reserve and publish it. Only a live preview this state wrote is ours to
    // cancel, and that cancels any candidate prepared from it as well.
    bool Rollback() noexcept {
        if (published) return false;
        if (!preview) return true;
        cancelled = true;
        if (!Present() || !SameBits(Live(), after)) return false;
        mdl::Mdl(object)->edgeScale = before;
        after = before;
        preview = false;
        return true;
    }

    std::uint64_t RecordBytes() const noexcept {
        return sizeof(State) + 4 * sizeof(void*);
    }

    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> source;
        const bool forward, initial;
        const std::uint64_t serial, binding, generation;
        const float expected;
        mutable resource_budget::Collector resources;
        std::uint64_t bytes = 0;
        bool consumed = false;

        Candidate(std::shared_ptr<State> value, bool direction)
            : source(std::move(value)), forward(direction), initial(!source->published),
              serial(source->serial), binding(BoundNativePoseGeneration(&source->app)),
              generation(SceneObjectIdentity(source->object).generation),
              expected(source->published ? (direction ? source->before : source->after)
                                         : (source->preview ? source->after : source->before)) {}
        bool IsCurrent() const noexcept override {
            if (consumed || source->cancelled || source->serial != serial || forward == source->applied) return false;
            if (!source->Bound() || !source->Present()) return false;
            if (BoundNativePoseGeneration(&source->app) != binding) return false;
            if (!GenerationIs(source->object, generation)) return false;
            if (!Ready(source->app, true)) return false;
            if (!source->owner->CoversBookkeeping(resources)) return false;
            const float live = source->Live();
            // The first candidate is prepared before the first Preview and must
            // accept every later controlled after written by the same state.
            if (initial) return SameBits(live, source->preview ? source->after : expected);
            return SameBits(live, expected);
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& state = *source;
            mdl::Mdl(state.object)->edgeScale = forward ? state.after : state.before;
            state.preview = false;
            state.applied = forward;
            state.published = true;
            ++state.serial;
            consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& value) const noexcept override {
            value = bytes;
            return true;
        }
    };

    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (cancelled || forward == applied || !Bound() || !Present()) return MMS_E_STALE;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            if (published) {
                // Undo expects the applied after, redo expects the restored
                // before. The first prepare has no applied direction yet.
                if (!SameBits(Live(), forward ? before : after)) return MMS_E_STALE;
            } else {
                if (!forward || !InitialCurrent()) return MMS_E_STALE;
                if (!SameBits(Live(), preview ? after : before)) return MMS_E_STALE;
            }
            if (!Ready(app, true)) return MMS_E_BUSY;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward);
            if (const auto result = owner->MeasureBookkeeping(next->resources)) return result;
            if (!next->resources.Result().Complete()) return MMS_E_NOT_READY;
            next->bytes = resource_budget::Collector::Sum(RecordBytes(), next->resources.Result().totalBytes);
            if (!next->IsCurrent()) return MMS_E_STALE;
            out = std::move(next);
            return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override {
        return Add(RecordBytes(), bookkeeping);
    }
    bool RequiresPhysicsReset() const noexcept override { return false; }
};

MmsResult NativeModelOutlinePlan::Begin(MMDApp& app, unsigned slot) noexcept {
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history) return MMS_E_NOT_READY;
        if (slot >= static_cast<unsigned>(kModelSlotCount)) return MMS_E_ARGUMENT;
        unsigned char* object = app.ModelSlot(static_cast<int>(slot));
        if (!object) return MMS_E_NOT_READY;
        // A pointer visible through two slots has no unambiguous undo target.
        for (unsigned other = 0; other < static_cast<unsigned>(kModelSlotCount); ++other)
            if (other != slot && app.ModelSlot(static_cast<int>(other)) == object) return MMS_E_STALE;
        if (!std::isfinite(mdl::Mdl(object)->edgeScale)) return MMS_E_ARGUMENT;
        if (const auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app, false)) return MMS_E_BUSY;
        std::uint64_t revision = 0;
        if (const auto result = history->Revision(revision)) return result;
        auto state = std::make_shared<State>(app, *history, slot, revision);
        if (!state->InitialCurrent()) return MMS_E_STALE;
        resource_budget::Collector resources;
        if (const auto result = state->owner->MeasureBookkeeping(resources)) return result;
        if (!resources.Result().Complete()) return MMS_E_NOT_READY;
        state->bookkeeping = resources.Result().totalBytes;
        state_ = std::move(state);
        return MMS_OK;
    });
}

MmsResult NativeModelOutlinePlan::Set(float after) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    if (!std::isfinite(after)) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        auto& state = *state_;
        if (state.cancelled || state.preview || state.published) return MMS_E_STALE;
        if (!state.InitialCurrent()) return MMS_E_STALE;
        // Reject a foreign writer that changed the field after Begin. The
        // staged value is intentionally not clamped: edit 646 historically
        // accepts negative and above-two text.
        if (!SameBits(state.Live(), state.before)) return MMS_E_STALE;
        state.after = after;
        return MMS_OK;
    });
}

MmsResult NativeModelOutlinePlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        auto& state = *state_;
        if (state.cancelled || state.published) return MMS_E_STALE;
        if (!state.InitialCurrent()) return MMS_E_STALE;
        if (!SameBits(state.Live(), state.preview ? state.after : state.before)) return MMS_E_STALE;
        // The wrapper keeps its reference: Preview and Cancel still need the
        // state after the journal has taken its own copy.
        if (state.Changed()) out = state_;
        return MMS_OK;
    });
}

MmsResult NativeModelOutlinePlan::Preview(float after) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return state_->PreviewValue(after);
}

bool NativeModelOutlinePlan::Cancel() noexcept {
    auto state = std::move(state_);
    if (!state) return true;
    if (state->published) return false;
    const bool owed = state->preview;
    const bool restored = state->Rollback();
    return !owed || restored;
}

bool NativeModelOutlinePlan::Changed() const noexcept {
    return state_ && state_->Changed();
}

NativeModelOutlinePlan::~NativeModelOutlinePlan() { Cancel(); }
}
#endif
