#include "native_picture_history.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "background_identity.hpp"
#include "native_picture_gesture.hpp"
#include "native_pose_scope.hpp"
#include "native_object_commands.hpp"
#include "native_history_commands.hpp"
#include "history_runtime.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/background_picture_activation.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/picture_menu.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include <commdlg.h>
#include <array>
#include <cstring>
#include <cwchar>
#include <filesystem>

namespace mikumikustudio::plugins {
namespace {
namespace identity = background_identity;
using Asset = media::BackgroundPictureAssetPtr;
using Activation = media::PreparedPictureActivation;
using Presentation = media::PicturePresentation;
constexpr auto pictureEffects = HistoryEffects::ProjectData | HistoryEffects::Viewport;
std::uint64_t Add(std::uint64_t a, std::uint64_t b) noexcept { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
Presentation View(MMDApp& app) noexcept {
    return {app.PictureOffsetX(), app.PictureOffsetY(), app.PictureScale(), app.PictureBackgroundEnabled() != 0};
}
MmsResult Status(media::PictureActivationResult result) noexcept {
    switch (result) {
    case media::PictureActivationResult::Ok: return MMS_OK;
    case media::PictureActivationResult::Invalid: return MMS_E_ARGUMENT;
    case media::PictureActivationResult::Stale: return MMS_E_STALE;
    case media::PictureActivationResult::NotReady: return MMS_E_NOT_READY;
    case media::PictureActivationResult::TooLarge: return MMS_E_MEMORY;
    case media::PictureActivationResult::NoMemory: return MMS_E_MEMORY;
    default: return MMS_E_FAILED;
    }
}
bool Ready(MMDApp& app, bool replay = false) noexcept {
    const bool own = replay && NativeObjectPublicationAuthorized(&app);
    return (!app.SceneMutationInProgress() || own) && (!bake::UiLocked() || own) && !SceneSaveBlocksEdits() &&
        !NativeObjectUiPending(&app) && !app.PlaybackActive() && !app.FrameStepPlayback() &&
        !app.state.recordPlaybackStartPending && !GetCapture() && !app.RecordingWindow() && !app.FrameRangeDialog() &&
        !app.state.frameCopyDialog && !app.GravitySettingDialog() && !app.GroundShadowColorDialog() &&
        !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen && !NativeModelOutlineDialogBlocks(app) &&
        app.Renderer() && app.Renderer()->device;
}
void Error(MmsResult result) noexcept {
    if (!result) return;
    runtime_log::Write("PLUGIN_ABI picture edit refused: %d", result);
    MessageBeep(MB_ICONWARNING);
}
thread_local bool picking = false;
}

struct NativePicturePlan::State final : HistoryChange, std::enable_shared_from_this<State> {
    MMDApp& app;
    SharedHistory* const history;
    const std::uint64_t instance, scene, binding;
    std::uint64_t revision = 0, serial = 1, originalTicket = 0, originalPictureMutationSerial = 0;
    IDirect3DTexture9* originalTexture = nullptr;
    std::array<wchar_t, 256> originalPath{};
    std::int32_t width = 0, height = 0;
    Asset before, after;
    Presentation original, beforeView, afterView;
    std::unique_ptr<Activation> initial;
    bool read = false, changed = false, visibility = false, published = false, applied = false;
    State(MMDApp& a, SharedHistory& h) : app(a), history(&h), instance(identity::Instance(&a)),
        scene(a.SceneGeneration()), binding(BoundNativePoseGeneration(&a)) {}
    bool Structure() const noexcept {
        return identity::IsOwner(&app, instance) && app.SceneGeneration() == scene && BoundNativePoseHistory(&app) == history;
    }
    bool MatchesSnapshot(const Asset& expected, const identity::PictureSnapshot& snapshot) const noexcept {
        if (snapshot.asset != expected) return false;
        if (!expected) return !app.PictureBackgroundTexture() && !app.PictureBackgroundPath()[0] && !app.PictureBackgroundEnabled();
        return app.PictureBackgroundTexture() && app.PictureWidth() == expected->Width() &&
            app.PictureHeight() == expected->Height() && !std::wcsncmp(app.PictureBackgroundPath(), expected->Path(), 256);
    }
    bool Matches(const Asset& expected) const noexcept {
        if (!Structure()) return false;
        identity::PictureSnapshot snapshot;
        return identity::QueryPictureSnapshot(&app, instance, snapshot) && MatchesSnapshot(expected, snapshot);
    }
    bool InitialCurrent() const noexcept {
        identity::PictureSnapshot snapshot;
        if (!Structure() || BoundNativePoseGeneration(&app) != binding ||
            !identity::QueryPictureSnapshot(&app, instance, snapshot) || snapshot.ticket != originalTicket ||
            snapshot.pictureMutationSerial != originalPictureMutationSerial || !MatchesSnapshot(before, snapshot) ||
            app.PictureBackgroundTexture() != originalTexture || app.PictureWidth() != width || app.PictureHeight() != height ||
            app.PictureOffsetX() != original.x || app.PictureOffsetY() != original.y ||
            std::memcmp(&app.PictureScale(), &original.scale, sizeof(float)) ||
            bool(app.PictureBackgroundEnabled()) != original.visible ||
            std::memcmp(app.PictureBackgroundPath(), originalPath.data(), sizeof originalPath)) return false;
        std::uint64_t now = 0;
        return history->Revision(now) == MMS_OK && now == revision;
    }
    MmsResult ReadReady() const noexcept {
        if (!InitialCurrent()) return MMS_E_STALE;
        if (auto result = history->CheckNativeNavigation()) return result;
        if (auto result = history->CheckNativeEdit()) return result;
        return Ready(app) ? MMS_OK : MMS_E_BUSY;
    }
    struct Candidate final : PreparedHistoryChange {
        std::shared_ptr<State> source;
        const bool forward, first;
        const std::uint64_t serial, binding;
        Presentation leaving;
        std::unique_ptr<Activation> activation;
        std::uint64_t bytes = 0;
        bool consumed = false;
        Candidate(std::shared_ptr<State> s, bool f) : source(std::move(s)), forward(f), first(!source->published),
            serial(source->serial), binding(BoundNativePoseGeneration(&source->app)), leaving(View(source->app)) {}
        bool IsCurrent() const noexcept override {
            return !consumed && source->serial == serial && forward != source->applied && source->Structure() &&
                BoundNativePoseGeneration(&source->app) == binding && Ready(source->app, !first) &&
                (first ? source->InitialCurrent() : source->Matches(forward ? source->before : source->after)) &&
                activation && activation->IsCurrent();
        }
        void Publish() noexcept override {
            if (!IsCurrent() || !activation->Publish()) std::terminate();
            auto& s = *source;
            // Unregistered presentation adjustments belong to the picture being
            // left. Remember them only when the replacement really commits.
            if (!s.visibility) {
                if (forward) s.beforeView = leaving; else s.afterView = leaving;
            }
            s.app.PendingPictureUiTicket() = identity::CurrentPictureTicket(&s.app, s.instance);
            s.published = true; s.applied = forward; ++s.serial; consumed = true;
        }
        bool TargetRetainedBytes(std::uint64_t& out) const noexcept override { out = bytes; return true; }
    };
    MmsResult PrepareChange(bool forward, bool retain, std::unique_ptr<PreparedHistoryChange>& out) {
        out.reset();
        return Boundary([&]() -> MmsResult {
            if (!read || !changed || forward == applied || !Structure() ||
                (!published && !InitialCurrent()) || (published && !Matches(forward ? before : after))) return MMS_E_STALE;
            if (!Ready(app, published)) return MMS_E_BUSY;
            if (serial == UINT64_MAX) return MMS_E_NOT_READY;
            auto candidate = std::make_unique<Candidate>(shared_from_this(), forward);
            if (!published && initial) candidate->activation = std::move(initial);
            else {
                auto presentation = forward ? afterView : beforeView;
                if (visibility) {
                    presentation = View(app);
                    presentation.visible = forward ? afterView.visible : beforeView.visible;
                }
                // An unrecorded visibility adjustment only borrows the existing
                // source/texture. Lowering the journal budget must not prevent
                // hiding that resident image; no new GPU payload is allocated.
                const auto presentationLimit = retain ? history->MaxBytes() : UINT64_MAX;
                const auto result = visibility ? Activation::PreparePresentation(&app, presentation, presentationLimit, candidate->activation) :
                    Activation::Prepare(&app, forward ? after : before, presentation, history->MaxBytes(), candidate->activation);
                if (auto status = Status(result)) return status;
            }
            candidate->bytes = Add(RetainedBytes(), candidate->activation->ResourceBytes());
            if ((retain || !visibility) && (candidate->bytes == UINT64_MAX || candidate->bytes > history->MaxBytes())) return MMS_E_MEMORY;
            if (!candidate->IsCurrent()) return MMS_E_STALE;
            out = std::move(candidate); return MMS_OK;
        });
    }
    MmsResult Prepare(bool forward, std::unique_ptr<PreparedHistoryChange>& out) override {
        return PrepareChange(forward, true, out);
    }
    MmsResult PrepareInitial(SharedHistory& target, bool retain, std::unique_ptr<PreparedHistoryChange>& out) override {
        if (&target != history) return MMS_E_STALE;
        return PrepareChange(true, retain, out);
    }
    std::uint64_t RetainedBytes() const noexcept override {
        auto bytes = std::uint64_t(sizeof(State) + 4 * sizeof(void*));
        if (before) bytes = Add(bytes, before->RetainedBytes());
        if (after && after != before) {
            bytes = Add(bytes, media::BackgroundPictureAsset::OverheadBytes());
            if (!before || before->Source() != after->Source()) bytes = Add(bytes, after->Source()->RetainedBytes());
        }
        return bytes;
    }
    bool RequiresPhysicsReset() const noexcept override { return false; }
    HistoryEffects Effects() const noexcept override { return pictureEffects; }
};

MmsResult NativePicturePlan::Begin(MMDApp& app) noexcept {
    // End the live draft before taking the replacement/visibility snapshot,
    // including a request whose later decode or preparation will fail.
    CancelPictureGesture(app);
    if (state_) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        auto* history = BoundNativePoseHistory(&app);
        if (!history) return MMS_E_NOT_READY;
        if (auto result = history->CheckNativeNavigation()) return result;
        if (auto result = history->CheckNativeEdit()) return result;
        if (!Ready(app)) return MMS_E_BUSY;
        const auto instance = identity::Instance(&app);
        if (!identity::IsOwner(&app, instance)) return MMS_E_NOT_READY;
        auto state = std::make_shared<State>(app, *history);
        identity::PictureSnapshot snapshot;
        bool mutating = false;
        if (!identity::QueryPictureSnapshot(&app, instance, snapshot, &mutating))
            return mutating ? MMS_E_BUSY : MMS_E_NOT_READY;
        state->originalTicket = snapshot.ticket;
        state->originalPictureMutationSerial = snapshot.pictureMutationSerial;
        state->before = std::move(snapshot.asset);
        state->originalTexture = app.PictureBackgroundTexture();
        state->width = app.PictureWidth(); state->height = app.PictureHeight();
        state->original = state->beforeView = state->afterView = View(app);
        std::memcpy(state->originalPath.data(), app.PictureBackgroundPath(), sizeof state->originalPath);
        if (wcsnlen_s(state->originalPath.data(), state->originalPath.size()) >= state->originalPath.size()) return MMS_E_NOT_READY;
        if (auto result = history->Revision(state->revision)) return result;
        if (!state->InitialCurrent()) return MMS_E_NOT_READY;
        state_ = std::move(state); return MMS_OK;
    });
}
MmsResult NativePicturePlan::Read(const wchar_t* path) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    return Boundary([&]() -> MmsResult {
        if (auto status = state_->ReadReady()) return status;
        state_->read = false; state_->changed = false;
        state_->initial.reset(); state_->after.reset();
        if (auto status = state_->ReadReady()) return status;
        std::unique_ptr<Activation> candidate;
        if (auto status = Status(Activation::PrepareImport(&state_->app, path, state_->history->MaxBytes(), candidate))) return status;
        if (auto status = state_->ReadReady()) return status;
        state_->after = candidate->TargetAsset(); state_->afterView = candidate->TargetPresentation();
        state_->initial = std::move(candidate); state_->visibility = false; state_->changed = true; state_->read = true;
        return MMS_OK;
    });
}
MmsResult NativePicturePlan::Clear() noexcept {
    if (!state_) return MMS_E_NOT_READY;
    if (auto result = state_->ReadReady()) return result;
    state_->read = false; state_->changed = false; state_->initial.reset();
    if (auto result = state_->ReadReady()) return result;
    state_->after.reset(); state_->afterView = {};
    state_->visibility = false; state_->changed = bool(state_->before); state_->read = true; return MMS_OK;
}
MmsResult NativePicturePlan::Visibility(bool visible) noexcept {
    if (!state_) return MMS_E_NOT_READY;
    if (auto result = state_->ReadReady()) return result;
    if (!state_->before && visible) return MMS_E_NOT_READY;
    state_->read = false; state_->changed = false; state_->initial.reset();
    if (auto result = state_->ReadReady()) return result;
    state_->after = state_->before; state_->afterView = state_->original;
    state_->afterView.visible = visible; state_->visibility = true;
    state_->changed = state_->original.visible != visible; state_->read = true; return MMS_OK;
}
MmsResult NativePicturePlan::Take(std::shared_ptr<HistoryChange>& out) noexcept {
    if (out) return MMS_E_ARGUMENT;
    if (!state_ || !state_->read) return MMS_E_NOT_READY;
    if (auto result = state_->ReadReady()) return result;
    if (state_->changed) out = state_;
    return MMS_OK;
}
namespace {
MmsResult Commit(MMDApp& app, NativePicturePlan& plan, bool& changed, const char* label,
    SharedHistory::NativeIntent intent) noexcept {
    changed = false;
    return Boundary([&]() -> MmsResult {
        std::shared_ptr<HistoryChange> change;
        if (auto result = plan.Take(change)) return result;
        if (!change) return MMS_OK;
        auto* history = BoundNativePoseHistory(&app);
        if (!history) return MMS_E_STALE;
        std::unique_ptr<SharedHistory::NativeTransaction> transaction;
        if (auto result = history->PrepareNative(label, std::move(change), transaction, intent)) return result;
        if (auto result = history->CommitNative(std::move(transaction))) return result;
        changed = true;
        // Control notifications can synchronously reenter. Keep the same gate
        // as the other native editors across the entire post-commit refresh.
        Boundary([&]() -> MmsResult {
            bake::ScopedUiInputGate gate;
            PublishBoundHistoryEffects(app, *history, pictureEffects, false);
            return MMS_OK;
        });
        return MMS_OK;
    });
}
}
MmsResult ImportBoundPicture(MMDApp& app, const wchar_t* path, bool& changed) noexcept {
    changed = false; NativePicturePlan plan;
    if (auto result = plan.Begin(app)) return result;
    if (auto result = plan.Read(path)) return result;
    return Commit(app, plan, changed, "Import background picture", SharedHistory::NativeIntent::ExplicitCommit);
}
MmsResult ClearBoundPicture(MMDApp& app, bool& changed) noexcept {
    changed = false; NativePicturePlan plan;
    if (auto result = plan.Begin(app)) return result;
    if (auto result = plan.Clear()) return result;
    return Commit(app, plan, changed, "Remove background picture", SharedHistory::NativeIntent::ExplicitCommit);
}
MmsResult SetBoundPictureVisibility(MMDApp& app, bool visible, bool& changed) noexcept {
    changed = false; NativePicturePlan plan;
    if (auto result = plan.Begin(app)) return result;
    if (auto result = plan.Visibility(visible)) return result;
    return Commit(app, plan, changed, "Background picture visibility", SharedHistory::NativeIntent::Adjustment);
}
bool TryLoadBoundPicture(MMDApp* app, const wchar_t* path) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    bool changed = false; Error(ImportBoundPicture(*app, path, changed)); return true;
}
bool TryToggleBoundPicture(MMDApp* app) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    if (!app->PictureBackgroundTexture()) return true;
    bool changed = false; Error(SetBoundPictureVisibility(*app, !app->PictureBackgroundEnabled(), changed)); return true;
}
bool TryClearBoundPicture(MMDApp* app) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    bool changed = false; Error(ClearBoundPicture(*app, changed)); return true;
}
bool TryPickBoundPicture(MMDApp* app) noexcept {
    if (!app || !BoundNativePoseHistory(app)) return false;
    if (picking) return true;
    struct Picking { Picking() { picking = true; } ~Picking() { picking = false; } } pickingScope;
    Error(Boundary([&]() -> MmsResult {
        NativePicturePlan plan;
        if (auto result = plan.Begin(*app)) return result;
        const auto menu = GetMenuState(GetMenu(app->MainWindow()), 301, MF_BYCOMMAND);
        const bool remember = menu != UINT(-1) && (menu & MF_CHECKED);
        auto initial = remember ? std::filesystem::path(app->DirBg()) : std::filesystem::path(L"UserFile/BackGround");
        if (initial.is_relative()) initial = std::filesystem::path(app->ExeDir()) / initial;
        const auto directory = std::filesystem::absolute(initial).lexically_normal().native();
        std::array<wchar_t, 32768> originalDirectory{};
        if (!GetCurrentDirectoryW(DWORD(originalDirectory.size()), originalDirectory.data())) return MMS_E_FAILED;
        struct Directory { const wchar_t* path; ~Directory() { SetCurrentDirectoryW(path); } } restore{originalDirectory.data()};
        wchar_t path[256]{};
        OPENFILENAMEW picker{}; picker.lStructSize = sizeof picker;
        picker.hwndOwner = app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow();
        picker.lpstrFilter = L"Pictures (*.bmp;*.jpg;*.png;*.tga;*.dds;*.dib;*.pfm;*.hdr)\0*.bmp;*.jpg;*.png;*.tga;*.dds;*.dib;*.pfm;*.hdr\0\0";
        picker.lpstrFile = path; picker.nMaxFile = 256; picker.lpstrInitialDir = directory.c_str();
        picker.lpstrDefExt = L"bmp"; picker.lpstrTitle = app->EnglishUI() == 2 ? L"载入背景图片" :
            app->EnglishUI() ? L"Load background picture" : L"背景画像を読み込む";
        picker.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        app->state.enterKeyState = 1;
        const bool selected = GetUiOpenFileNameW(&picker) != FALSE;
        if (!SetCurrentDirectoryW(originalDirectory.data())) return MMS_E_FAILED;
        if (!selected) return MMS_OK;
        const auto folder = remember ? std::filesystem::path(path).parent_path().native() : std::wstring{};
        if (folder.size() >= kPathFieldCapacity) return MMS_E_ARGUMENT;
        if (auto result = plan.Read(path)) return result;
        bool changed = false;
        const auto result = Commit(*app, plan, changed, "Import background picture", SharedHistory::NativeIntent::ExplicitCommit);
        if (!result && changed && remember) wcscpy_s(app->DirBg(), kPathFieldCapacity, folder.c_str());
        return result;
    }));
    return true;
}

bool RefreshBoundPictureUi(MMDApp& app) noexcept {
    const auto pending = app.PendingPictureUiTicket();
    if (!pending) return true;
    const auto instance = identity::Instance(&app), scene = app.SceneGeneration();
    bool mutating = false;
    const auto active = identity::CurrentPictureTicket(&app, instance, &mutating);
    if (mutating) return false; // a failed candidate may restore this same ticket
    if (active != pending) {
        if (app.PendingPictureUiTicket() == pending) app.PendingPictureUiTicket() = 0;
        return true;
    }
    const auto current = [&]() noexcept {
        return identity::IsOwner(&app, instance) && app.SceneGeneration() == scene &&
            app.PendingPictureUiTicket() == pending && identity::IsCurrent(&app, identity::Kind::Picture, pending);
    };
    // Another owner may enter the process-wide mutation gate between queries.
    // Keep the obligation; the next non-busy entry can confirm real retirement.
    if (!current()) return false;
    if (!app.MainWindow() || !IsWindow(app.MainWindow()) || !app.Renderer()) return false;
    return Boundary([&]() -> MmsResult {
        bake::ScopedUiInputGate gate;
        const auto menu = GetMenu(app.MainWindow());
        if (!current()) return MMS_E_STALE;
        if (CheckMenuItem(menu, 233, MF_BYCOMMAND | (app.PictureBackgroundEnabled() ? MF_CHECKED : MF_UNCHECKED)) == DWORD(-1)) return MMS_E_FAILED;
        if (!current()) return MMS_E_STALE;
        if (!picture_menu::EnsureItem(&app)) return MMS_E_FAILED;
        if (!current()) return MMS_E_STALE;
        if (app.PictureBackgroundTexture() && !TryPicBgOverlayRefresh(&app)) return MMS_E_FAILED;
        if (!current()) return MMS_E_STALE;
        updates::RequestViewport(&app, false);
        if (!current()) return MMS_E_STALE;
        app.PendingPictureUiTicket() = 0;
        return MMS_OK;
    }) == MMS_OK;
}
void ClearLegacyPicture(MMDApp* app) noexcept {
    if (!app) return;
    CancelPictureGesture(*app);
    const auto instance = identity::Instance(app);
    if (!identity::IsOwner(app, instance)) return;
    {
        identity::Mutation mutation(app, true, false, false, true);
        IDirect3DTexture9* previous = nullptr;
        if (!mutation.ClearPicture(app, app->PictureBackgroundTexture(), previous, app->PictureBackgroundPath(),
            app->PictureWidth(), app->PictureHeight(), app->PictureOffsetX(), app->PictureOffsetY(),
            app->PictureScale(), app->PictureBackgroundEnabled())) return;
        app->SceneModified() = 1;
        if (previous) {
            bake::ScopedUiInputGate inputGate;
            previous->Release();
        }
        if (!mutation.Current() || !identity::IsOwner(app, instance)) return;
    }
    app->PendingPictureUiTicket() = identity::Acquire(app, identity::Kind::Picture);
    RefreshBoundPictureUi(*app);
}
}
#endif
