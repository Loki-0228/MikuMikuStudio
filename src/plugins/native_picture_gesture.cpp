#include "native_picture_gesture.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "background_gesture_core.hpp"
#include "background_identity.hpp"
#include "history_runtime.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "native_picture_history.hpp"
#include "native_pose_scope.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include <atomic>
#include <cmath>
#include <cwchar>

namespace mikumikustudio {
void FinishPictureGestureInteraction(MMDApp*, int operation) noexcept;
}
namespace mikumikustudio::plugins {
namespace {
namespace identity = background_identity;
namespace core = background_gesture_core;
using Values = core::Values;
constexpr auto effects = HistoryEffects::ProjectData | HistoryEffects::Viewport;
identity::PlacementFields Fields(MMDApp& app) noexcept {
    return {&app.PictureOffsetX(), &app.PictureOffsetY(), &app.PictureScale()};
}
identity::Placement Placement(Values value) noexcept { return {value.x, value.y, value.scale}; }
Values Value(identity::Placement value) noexcept { return {value.x, value.y, value.scale}; }
unsigned Mask(Values a, Values b) noexcept {
    return (a.x != b.x ? 1u : 0u) | (a.y != b.y ? 2u : 0u) | (!core::Same(a.scale, b.scale) ? 4u : 0u);
}
bool Finite(Values value) noexcept { return std::isfinite(value.scale) && value.scale > 0; }
bool Visible(MMDApp& app) noexcept {
    return app.PictureBackgroundEnabled() && app.PictureBackgroundTexture() &&
        app.PictureWidth() > 0 && app.PictureHeight() > 0 &&
        !(app.AviBackgroundEnabled() == 1 && app.AviFrameWidth() > 0 && app.AviFrameHeight() > 0);
}
bool Ready(MMDApp& app, HWND owner, bool replay = false) noexcept {
    const bool own = replay && NativeObjectPublicationAuthorized(&app);
    const auto capture = GetCapture();
    return (!owner || (IsWindow(owner) && GetWindowThreadProcessId(owner, nullptr) == GetCurrentThreadId())) &&
        (!capture || (owner && capture == owner)) &&
        (!app.SceneMutationInProgress() || own) && (!bake::UiLocked() || own) && !SceneSaveBlocksEdits() &&
        !NativeObjectUiPending(&app) && !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !app.RecordingWindow() && !app.FrameRangeDialog() &&
        !app.state.frameCopyDialog && !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app) &&
        app.Renderer() && app.Renderer()->device;
}
struct Change final : HistoryChange, std::enable_shared_from_this<Change> {
    MMDApp& app;
    SharedHistory* const history;
    const std::uint64_t instance, scene, binding, assetId, initialTicket;
    IDirect3DTexture9* const initialTexture; // identity comparison only, never dereferenced
    const HWND owner;
    const Values before;
    Values after;
    std::uint64_t generation = 1;
    unsigned mask = 0;
    bool initial = true, applied = false;
    Change(MMDApp& a, SharedHistory& h, HWND window, std::uint64_t i, std::uint64_t id,
        std::uint64_t ticket, Values value) noexcept : app(a), history(&h), instance(i),
        scene(a.SceneGeneration()), binding(BoundNativePoseGeneration(&a)), assetId(id), initialTicket(ticket),
        initialTexture(a.PictureBackgroundTexture()), owner(window), before(value), after(value) {}
    bool Structure() const noexcept {
        return identity::IsOwner(&app, instance) && app.SceneGeneration() == scene && BoundNativePoseHistory(&app) == history;
    }
    bool Read(std::uint64_t& ticket, Values& value) const noexcept {
        if (!Structure()) return false;
        const auto current = identity::CurrentPictureTicket(&app, instance);
        const auto asset = identity::CurrentPictureAsset(&app, instance, current);
        if (!asset || asset->Id() != assetId || !app.PictureBackgroundTexture() ||
            app.PictureWidth() != asset->Width() || app.PictureHeight() != asset->Height() ||
            std::wcsncmp(app.PictureBackgroundPath(), asset->Path(), 256)) return false;
        identity::Placement observed{};
        if (identity::PlacementAccess(&app, identity::Kind::Picture, instance, current, Fields(app), observed) !=
            identity::PlacementResult::Ok) return false;
        ticket = current; value = Value(observed); return true;
    }
    bool InitialDomain() const noexcept {
        return Structure() && BoundNativePoseGeneration(&app) == binding &&
            app.PictureBackgroundTexture() == initialTexture &&
            identity::CurrentPictureTicket(&app, instance) == initialTicket;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<Change> source;
        const bool forward, first;
        const std::uint64_t generation, binding, ticket;
        const Values expected;
        bool consumed = false;
        Candidate(std::shared_ptr<Change> value, bool direction, std::uint64_t t, Values v) noexcept :
            source(std::move(value)), forward(direction), first(source->initial), generation(source->generation),
            binding(BoundNativePoseGeneration(&source->app)), ticket(t), expected(v) {}
        bool IsCurrent() const noexcept override {
            auto& s = *source;
            std::uint64_t current = 0; Values observed{};
            return !consumed && s.generation == generation && forward != s.applied && s.Structure() &&
                BoundNativePoseGeneration(&s.app) == binding && Ready(s.app, first ? s.owner : nullptr, !first) &&
                (!first || (s.InitialDomain() && Visible(s.app))) &&
                s.Read(current, observed) && current == ticket &&
                core::Same(observed, first ? s.after : expected);
        }
        void Publish() noexcept override {
            if (!IsCurrent()) std::terminate();
            auto& s = *source;
            const auto old = Placement(first ? s.after : expected);
            auto replacement = old;
            const auto target = forward ? s.after : s.before;
            if (s.mask & 1) replacement.x = target.x;
            if (s.mask & 2) replacement.y = target.y;
            if (s.mask & 4) replacement.scale = target.scale;
            identity::Placement observed{};
            // Compare all fields, but preserve every field outside this edit.
            // No callback can run between validation and the locked exchange.
            if (identity::PlacementAccess(&s.app, identity::Kind::Picture, s.instance, ticket,
                Fields(s.app), observed, &old, &replacement, 7) != identity::PlacementResult::Ok) std::terminate();
            s.app.PendingPictureUiTicket() = ticket;
            s.initial = false; s.applied = forward; ++s.generation; consumed = true;
        }
    };
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        return Boundary([&]() -> MmsResult {
            if (forward == applied || !Structure()) return MMS_E_STALE;
            if (generation == UINT64_MAX) return MMS_E_NOT_READY;
            if (!Ready(app, initial ? owner : nullptr, !initial)) return MMS_E_BUSY;
            std::uint64_t ticket = 0; Values observed{};
            if (!Read(ticket, observed) || (initial && (!InitialDomain() || !Visible(app) || !core::Same(observed, after))))
                return MMS_E_STALE;
            if (!Finite(observed)) return MMS_E_ARGUMENT;
            auto next = std::make_unique<Candidate>(shared_from_this(), forward, ticket, observed);
            out = std::move(next); return MMS_OK;
        });
    }
    std::uint64_t RetainedBytes() const noexcept override { return sizeof(*this) + 4 * sizeof(void*); }
    bool RequiresPhysicsReset() const noexcept override { return false; }
    HistoryEffects Effects() const noexcept override { return effects; }
};
struct Session final {
    std::shared_ptr<Change> change;
    std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    core::Gesture state;
    std::uint64_t serial = 0, revision = 0;
    int operation = 0;
    bool Current() const noexcept {
        if (!change->InitialDomain()) return false;
        std::uint64_t current = 0, ticket = 0; Values observed{};
        return change->history->Revision(current) == MMS_OK && current == revision &&
            change->Read(ticket, observed) && ticket == change->initialTicket && core::Same(observed, change->after);
    }
};
thread_local std::shared_ptr<Session> active;
thread_local unsigned working = 0;
struct Work final { Work() noexcept { ++working; } ~Work() { --working; } };
std::atomic<std::uint64_t> serials{0};
std::uint64_t NextSerial() noexcept {
    auto old = serials.load(std::memory_order_relaxed);
    while (old != UINT64_MAX) if (serials.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) return old + 1;
    return 0;
}
// Detach first, then dispose of the journal/capture while the outer Work
// barrier still prevents callbacks from starting another session.
void Finish(const std::shared_ptr<Session>& session, bool cancel) noexcept {
    auto& s = *session->change;
    std::uint64_t ticket = 0; Values observed{};
    const bool domain = s.InitialDomain() && s.Read(ticket, observed) && ticket == s.initialTicket;
    core::Detached receipt{};
    if (session->state.Detach(GetCurrentThreadId(), domain ? core::Domain{s.instance, ticket} : core::Domain{},
        observed, cancel, receipt) != core::Result::Ok) std::terminate();
    if (domain && receipt.restoreMask) {
        const auto expected = Placement(observed); auto replacement = expected;
        if (receipt.restoreMask & 1) replacement.x = receipt.restore.x;
        if (receipt.restoreMask & 2) replacement.y = receipt.restore.y;
        if (receipt.restoreMask & 4) replacement.scale = receipt.restore.scale;
        identity::Placement actual{};
        if (identity::PlacementAccess(&s.app, identity::Kind::Picture, s.instance, ticket,
            Fields(s.app), actual, &expected, &replacement, 7) == identity::PlacementResult::Ok)
            s.app.PendingPictureUiTicket() = ticket;
    }
    session->transaction.reset();
    if (identity::IsOwner(&s.app, s.instance)) {
        FinishPictureGestureInteraction(&s.app, session->operation);
        if (s.owner && GetCapture() == s.owner) ReleaseCapture();
    }
    if (session->state.FinishDetach(GetCurrentThreadId(), receipt.serial) != core::Result::Ok) std::terminate();
}
}
bool PictureGestureActive(const MMDApp& app) noexcept { return active && &active->change->app == &app; }
std::uint64_t PictureGestureSerial(const MMDApp& app) noexcept { return PictureGestureActive(app) ? active->serial : 0; }
int PictureGestureOperation(const MMDApp& app) noexcept { return PictureGestureActive(app) ? active->operation : 0; }
HWND PictureGestureOwner(const MMDApp& app) noexcept { return PictureGestureActive(app) ? active->change->owner : nullptr; }
MmsResult BeginPictureGesture(MMDApp& app, SharedHistory& history, HWND owner, int operation) noexcept {
    if (operation != 1 && operation != 2) return MMS_E_ARGUMENT;
    if (!owner || !IsWindow(owner) || GetWindowThreadProcessId(owner, nullptr) != GetCurrentThreadId()) return MMS_E_ARGUMENT;
    const auto instance = identity::Instance(&app);
    if (!identity::IsOwner(&app, instance) || BoundNativePoseHistory(&app) != &history) return MMS_E_NOT_READY;
    if (active || working || !Ready(app, owner)) return MMS_E_BUSY;
    if (!Visible(app)) return MMS_E_NOT_READY;
    Work work;
    ScopedSceneCloseDeferral close(app);
    return Boundary([&]() -> MmsResult {
        if (auto result = history.CheckNativeNavigation()) return result;
        if (auto result = history.CheckNativeEdit()) return result;
        const auto ticket = identity::Acquire(&app, identity::Kind::Picture);
        const auto asset = identity::CurrentPictureAsset(&app, instance, ticket);
        if (!asset) return MMS_E_NOT_READY;
        identity::Placement observed{};
        if (identity::PlacementAccess(&app, identity::Kind::Picture, instance, ticket, Fields(app), observed) !=
            identity::PlacementResult::Ok) return MMS_E_STALE;
        if (!Finite(Value(observed))) return MMS_E_ARGUMENT;
        auto next = std::make_shared<Session>();
        next->change = std::make_shared<Change>(app, history, owner, instance, asset->Id(), ticket, Value(observed));
        next->operation = operation; next->serial = NextSerial();
        if (!next->serial) return MMS_E_NOT_READY;
        if (auto result = history.Revision(next->revision)) return result;
        if (auto result = history.PrepareNative("Drag background picture", next->change, next->transaction,
            SharedHistory::NativeIntent::Adjustment)) return result;
        if (!next->Current() || !Ready(app, owner) || !Visible(app)) return MMS_E_STALE;
        if (next->state.Begin(GetCurrentThreadId(), {instance, ticket}, Value(observed),
            reinterpret_cast<std::uintptr_t>(next->transaction.get())) != core::Result::Ok) return MMS_E_NOT_READY;
        active = std::move(next); return MMS_OK;
    });
}
MmsResult PreviewPictureGesture(MMDApp& app, std::int32_t x, std::int32_t y, float scale) noexcept {
    if (!PictureGestureActive(app)) return MMS_E_NOT_READY;
    if (working) return MMS_E_BUSY;
    const auto current = active; auto& s = *current->change;
    if (!Finite({x, y, scale})) return MMS_E_ARGUMENT;
    if (!Ready(app, s.owner) || !Visible(app)) return MMS_E_BUSY;
    if (!current->Current()) return MMS_E_STALE;
    const auto previous = s.after; const Values next{x, y, scale};
    const core::Domain domain{s.instance, s.initialTicket};
    if (current->state.ReservePreview(GetCurrentThreadId(), domain, previous) != core::Result::Ok) return MMS_E_STALE;
    const auto expected = Placement(previous), replacement = Placement(next); identity::Placement observed{};
    if (identity::PlacementAccess(&app, identity::Kind::Picture, s.instance, s.initialTicket,
        Fields(app), observed, &expected, &replacement, 7) != identity::PlacementResult::Ok) {
        current->state.AbortPreview(GetCurrentThreadId()); return MMS_E_STALE;
    }
    if (current->state.AcceptPreview(GetCurrentThreadId(), domain, previous, next) != core::Result::Ok) std::terminate();
    s.after = next; s.mask = Mask(s.before, next); app.PendingPictureUiTicket() = s.initialTicket;
    return MMS_OK;
}
void RefreshPictureGesture(MMDApp& app) noexcept {
    if (!RefreshBoundPictureUi(app)) RequestBoundHistoryUiRefresh(&app, HistoryEffects::Viewport);
}
bool CancelPictureGesture(MMDApp& app) noexcept {
    if (!PictureGestureActive(app) || working) return false;
    Work work;
    auto current = std::move(active);
    ScopedSceneCloseDeferral close(app);
    bake::ScopedUiInputGate gate;
    Finish(current, true);
    if (identity::IsOwner(&app, current->change->instance)) RefreshPictureGesture(app);
    return true;
}
MmsResult EndPictureGesture(MMDApp& app, bool& changed) noexcept {
    changed = false;
    if (!PictureGestureActive(app)) return MMS_E_NOT_READY;
    if (working) return MMS_E_BUSY;
    Work work;
    auto current = std::move(active); auto& s = *current->change;
    ScopedSceneCloseDeferral close(app);
    MmsResult result = MMS_E_STALE;
    if (Ready(app, s.owner) && Visible(app) && current->Current()) {
        result = !s.mask ? MMS_OK : Boundary([&]() -> MmsResult {
            return s.history->CommitNative(std::move(current->transaction));
        });
        changed = result == MMS_OK && s.mask != 0;
    }
    bake::ScopedUiInputGate gate;
    Finish(current, !changed);
    if (s.Structure() && BoundNativePoseGeneration(&app) == s.binding) {
        if (changed) PublishBoundHistoryEffects(app, *s.history, effects, false);
        else RefreshPictureGesture(app);
    }
    return result;
}
}
#endif
