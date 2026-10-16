#include "native_object_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_history_commands.hpp"
#include "native_pose_scope.hpp"
#include "history_runtime.hpp"
#include "scene_object_history.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/physics_scene.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/plugin_runtime.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/scene_history_ui.hpp"
#include "mikudancestudio/scene_ownership.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/ui_language.hpp"
#include "mikudancestudio/ui_message.hpp"
#include <commctrl.h>
#include <array>
#include <cstdio>

namespace mikudancestudio::plugins {
namespace {
class Operation;
thread_local Operation* operation = nullptr;
thread_local MMDApp* pendingUi = nullptr;
struct SelectionToken {
    MmsId model{}, accessory{}, scopeAccessory{};
    bool cameraMode = true;
    int globalScope = 0;
};
thread_local SelectionToken pendingSelection;
thread_local bool hasPendingSelection = false;
constexpr UINT_PTR kOperationSubclass = 0x4d4d536e;

bool Ready(MMDApp& app, bool pluginCall = false) noexcept {
    return !operation && !bake::UiLocked() && !SceneSaveInProgress() &&
        !app.SceneMutationInProgress() && (!InCallback() || pluginCall) &&
        !app.PlaybackActive() && !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.FrameRangeDialog() && !app.state.frameCopyDialog && !app.RecordingWindow() &&
        !GetCapture() && app.MainWindow() &&
        GetWindowThreadProcessId(app.MainWindow(), nullptr) == GetCurrentThreadId();
}
// This is deliberately separate from the journal gate. Message boxes and UI
// refreshes must keep native/menu callbacks out while journal access is denied.
class Operation {
    struct Window { HWND handle; bool close = false; };
    std::vector<Window> windows_;
    std::unique_ptr<bake::ScopedUiLock> uiLock_;
    std::unique_ptr<ScopedSceneMutation> mutation_;
    static LRESULT CALLBACK Filter(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                   UINT_PTR, DWORD_PTR data) {
        auto& self = *reinterpret_cast<Operation*>(data);
        if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE)) {
            for (auto& item : self.windows_) if (item.handle == window) item.close = true;
            return 0;
        }
        if (message == WM_COMMAND || message == WM_NOTIFY || message == WM_HSCROLL || message == WM_VSCROLL ||
            (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)) return 0;
        return DefSubclassProc(window, message, wp, lp);
    }
    void Release() noexcept {
        // The guard remains visible through synchronous paint/enable messages.
        mutation_.reset(); uiLock_.reset();
        for (auto& item : windows_) if (IsWindow(item.handle)) {
            RemoveWindowSubclass(item.handle, Filter, kOperationSubclass);
            if (item.close) PostMessageW(item.handle, WM_CLOSE, 0, 0);
        }
        operation = nullptr;
    }
public:
    MMDApp& app;
    bool publishing = false;
    explicit Operation(MMDApp& value) : app(value) {
        if (operation) throw std::invalid_argument("nested object operation");
        // Only MMS's command windows need an input filter. Existing modeless
        // editors are rejected before entry; the confirmation is created later.
        windows_.push_back({app.MainWindow()});
        if (app.FloatingWindow() && app.FloatingWindow() != app.MainWindow())
            windows_.push_back({app.FloatingWindow()});
        operation = this;
        try {
            for (const auto& item : windows_)
                if (!SetWindowSubclass(item.handle, Filter, kOperationSubclass, reinterpret_cast<DWORD_PTR>(this)))
                    throw std::runtime_error("object operation input guard");
            uiLock_ = std::make_unique<bake::ScopedUiLock>(nullptr);
            mutation_ = std::make_unique<ScopedSceneMutation>(app);
        } catch (...) { Release(); throw; }
    }
    ~Operation() { Release(); }
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
};
struct Publication {
    Operation& operation;
    explicit Publication(Operation& value) : operation(value) { operation.publishing = true; }
    ~Publication() { operation.publishing = false; }
};
bool EditorsOpen(MMDApp& app) noexcept {
    // These native dialogs hold writable scene pointers/scratch state. A
    // curve editor uses checked identities and receives a read notification.
    return app.FrameRangeDialog() || app.state.frameCopyDialog ||
        app.GravitySettingDialog() || app.GroundShadowColorDialog() || app.AccessoryOrderArray() ||
        app.state.accessoryEditDialogOpen || app.state.edgeThicknessDialog;
}
bool ValidOrder(const MMDApp& app) noexcept {
    std::array<bool, kModelSlotCount + 1> models{};
    std::array<bool, kModelSlotCount> accessories{};
    unsigned modelCount = 0, accessoryCount = 0;
    for (unsigned slot = 0; slot < kModelSlotCount; ++slot) {
        if (const auto* model = mdl::Mdl(app.ModelSlot(slot))) {
            const auto order = model->comboSelIndex;
            if (order < 1 || order > kModelSlotCount || models[order]) return false;
            models[order] = true; ++modelCount;
        }
        if (const auto* accessory = app.AccessorySlot(slot)) {
            const auto order = accessory->order;
            if (order >= kModelSlotCount || accessories[order]) return false;
            accessories[order] = true; ++accessoryCount;
        }
    }
    for (unsigned order = 1; order <= modelCount; ++order) if (!models[order]) return false;
    for (unsigned order = 0; order < accessoryCount; ++order) if (!accessories[order]) return false;
    return true;
}
SelectionToken CaptureSelectionToken(MMDApp& app) {
    const auto selection = scene_history_ui::CaptureSelection(app);
    SelectionToken token;
    if (selection.model) token.model = SceneObjectIdentity(selection.model).id;
    if (selection.accessory) token.accessory = SceneObjectIdentity(selection.accessory).id;
    if (selection.scopeAccessory) token.scopeAccessory = SceneObjectIdentity(selection.scopeAccessory).id;
    token.cameraMode = selection.cameraMode; token.globalScope = selection.globalScope;
    return token;
}
scene_history_ui::Selection ResolveSelection(const MMDApp& app, const SelectionToken& token) noexcept {
    scene_history_ui::Selection selection;
    selection.cameraMode = token.cameraMode; selection.globalScope = token.globalScope;
    // Resolve IDs against live scene pointers only. A queued retry owns no
    // pointer to a potentially released object, and address reuse cannot bind it.
    for (unsigned slot = 0; slot < kModelSlotCount; ++slot) {
        if (auto* model = app.ModelSlot(slot); model && SceneObjectPresent(model, token.model)) selection.model = model;
        if (auto* accessory = app.AccessorySlot(slot)) {
            if (SceneObjectPresent(accessory, token.accessory)) selection.accessory = accessory;
            if (SceneObjectPresent(accessory, token.scopeAccessory)) selection.scopeAccessory = accessory;
        }
    }
    return selection;
}
void ScheduleUi(MMDApp& app, const SelectionToken* token = nullptr) noexcept {
    pendingUi = &app;
    if (token) { pendingSelection = *token; hasPendingSelection = true; }
    RequestBoundHistoryUiRefresh(&app);
}
void SafeSelection(MMDApp& app) noexcept {
    // This must precede even the first allocation/control read of UI refresh:
    // an allocation failure must not leave WM_PAINT pointing at a removed model.
    if (!app.CameraMode() && (app.SelectedModelSlot() >= kModelSlotCount || !app.SelectedModel())) {
        app.CameraMode() = 1; app.SetSelectedModelSlot(0); app.MainModelComboSelection() = 0;
        app.EditMode() = ViewportEditMode::None;
    }
    if (app.SelectedAccessorySlot() >= kModelSlotCount || !app.AccessorySlot(app.SelectedAccessorySlot())) {
        app.SelectedAccessorySlot() = 0;
        for (unsigned i = 0; i < kModelSlotCount; ++i)
            if (app.AccessorySlot(i) && app.AccessorySlot(i)->order == 0) { app.SelectedAccessorySlot() = std::uint8_t(i); break; }
    }
}
void AccessoryReadout(MMDApp& app) {
    const auto main = app.MainWindow();
    const auto* accessory = app.AccessorySlot(app.SelectedAccessorySlot());
    for (int id : {473, 474, 475, 476, 477, 478, 479, 480, 481, 482, 483, 484, 485, 486, 487})
        EnableWindow(GetDlgItem(main, id), accessory != nullptr);
    for (int id : {476, 477, 486})
        SendDlgItemMessageW(main, id, BM_SETCHECK, accessory &&
            (id == 476 ? accessory->visible : id == 477 ? accessory->additiveBlend : accessory->shadowEnabled)
                ? BST_CHECKED : BST_UNCHECKED, 0);
    for (int id = 478; id <= 485; ++id) {
        char text[96]{};
        if (accessory) {
            const auto value = id < 481 ? double(accessory->position[id - 478]) :
                id < 484 ? double(accessory->rotation[id - 481]) * 180.0 / 3.141592025756836 :
                id == 484 ? double(accessory->scale) : double(accessory->opacity);
            sprintf_s(text, id == 485 ? "%3.2f" : "%3.4f", value);
        }
        SetDlgItemTextA(main, id, text);
    }
}
bool RefreshUi(MMDApp& app, SharedHistory& history, const scene_history_ui::Selection& selection) {
    const auto main = app.MainWindow();
    const auto previousModels = SendDlgItemMessageW(main, 436, CB_GETCOUNT, 0, 0);
    const auto previousAccessories = SendDlgItemMessageW(main, 471, CB_GETCOUNT, 0, 0);
    const auto result = scene_history_ui::RefreshObjectLists(app, selection, history.UndoCount() != 0, history.RedoCount() != 0);
    if (result != scene_history_ui::Result::Ok) {
        ScheduleUi(app);
        runtime_log::Write("PLUGIN_ABI object list refresh pending: %d", static_cast<int>(result));
        return false;
    }
    const bool topologyChanged = pendingUi == &app ||
        previousModels != SendDlgItemMessageW(main, 436, CB_GETCOUNT, 0, 0) ||
        previousAccessories != SendDlgItemMessageW(main, 471, CB_GETCOUNT, 0, 0) ||
        selection.cameraMode != bool(app.CameraMode());
    // Deleting the edited model selects camera mode; no seek/evaluation or
    // registration handler is called. Remaining author poses/marks stay live.
    if (topologyChanged) {
        PostModelReload2(&app);
        EnsureInterpChannelCombo(&app);
        if (app.CameraMode() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
        SetModelExtensionMenuEnabled(&app, !app.CameraMode() && app.SelectedModel() != nullptr);
        EnableMenuItem(GetMenu(main), 249, MF_BYCOMMAND |
            (SendDlgItemMessageW(main, 471, CB_GETCOUNT, 0, 0) > 0 ? MF_ENABLED : MF_GRAYED));
    }
    AccessoryReadout(app);
    EnableWindow(GetDlgItem(main, panel::kUndoButton), history.UndoCount() != 0);
    EnableWindow(GetDlgItem(main, panel::kRedoButton), history.RedoCount() != 0);
    if (app.CurveDC()) SelectionReeval(&app);
    InvalidateRect(main, nullptr, FALSE);
    RefreshBoundPoseReadout(app);
    curve_editor::NotifyOwnedChanged(main);
    if (pendingUi == &app) { pendingUi = nullptr; hasPendingSelection = false; }
    return true;
}
bool RefreshPublishedUi(MMDApp& app, SharedHistory& history, const SelectionToken& selection) noexcept {
    const auto result = Boundary([&]() -> MmsResult { return RefreshUi(app, history, ResolveSelection(app, selection)) ? MMS_OK : MMS_E_FAILED; });
    if (result != MMS_OK) ScheduleUi(app, &selection);
    return result == MMS_OK;
}
bool TryDelete(MMDApp* app, HWND window, bool model) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!Ready(*app) || EditorsOpen(*app) || NativeObjectUiPending(app)) return true;
    // Check callback/pending-transaction admission before displaying a modal.
    if (history->Enter() != MMS_OK) return true;
    history->Leave();
    const auto result = Boundary([&]() -> MmsResult {
        Operation scope(*app);
        if (!ValidOrder(*app)) return MMS_E_STALE;
        for (int id : {436, 471, 449, 474, 450, 475, 434})
            if (!GetDlgItem(app->MainWindow(), id)) return MMS_E_NOT_READY;
        const auto selected = SendDlgItemMessageW(window, model ? 436 : 471, CB_GETCURSEL, 0, 0);
        if (selected < (model ? 1 : 0)) return MMS_E_NOT_FOUND;
        unsigned slot = kModelSlotCount;
        for (unsigned i = 0; i < kModelSlotCount; ++i) {
            const bool match = model ? (app->ModelSlot(i) && mdl::Mdl(app->ModelSlot(i))->comboSelIndex == selected) :
                (app->AccessorySlot(i) && app->AccessorySlot(i)->order == selected);
            if (match) { if (slot != kModelSlotCount) return MMS_E_STALE; slot = i; }
        }
        if (slot == kModelSlotCount) return MMS_E_NOT_FOUND;
        const void* pointer = model ? static_cast<void*>(app->ModelSlot(slot)) : static_cast<void*>(app->AccessorySlot(slot));
        const auto identity = SceneObjectIdentity(pointer);
        const auto name = model ? text_encoding::ModelName(*mdl::Mdl(app->ModelSlot(slot)), UiUsesEnglishNames(app->EnglishUI())) :
            text_encoding::Display(app->AccessorySlot(slot)->name);
        const auto flags = MB_OKCANCEL | (app->FloatingWindow() ? MB_TOPMOST : 0);
        if (ShowUiMessage(app, window, model ? UiMessage::DeleteModelWithHistory : UiMessage::DeleteAccessoryWithHistory, flags, name.c_str()) != IDOK)
            return MMS_OK;
        const void* current = model ? static_cast<void*>(app->ModelSlot(slot)) : static_cast<void*>(app->AccessorySlot(slot));
        if (current != pointer || !SceneObjectPresent(pointer, identity.id) ||
            SceneObjectIdentity(pointer).generation != identity.generation || EditorsOpen(*app) || !ValidOrder(*app)) return MMS_E_STALE;
        const auto selection = CaptureSelectionToken(*app);
        // This owner remains alive through the post-publication control calls.
        auto retained = RetainSceneObject(*app, model ? MMS_OBJECT_MODEL : MMS_OBJECT_ACCESSORY, slot);
        {
            Publication allow(scope);
            auto change = retained->CaptureRemoval();
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (auto r = history->PrepareNative(model ? "Delete model" : "Delete accessory", std::move(change), transaction)) return r;
            if (auto r = history->CommitNative(std::move(transaction))) return r;
        }
        SafeSelection(*app);
        app->SceneModified() = 1; app->RecordedFrameCountByte1() = 1; app->PhysicsResetPending() = 1;
        RefreshPublishedUi(*app, *history, selection);
        return MMS_OK;
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI object deletion refused: %d", result);
    return true;
}
bool TryImport(MMDApp* app, const wchar_t* path, bool model, bool showInfo) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    if (!path || !Ready(*app) || EditorsOpen(*app) || NativeObjectUiPending(app)) return true;
    if (history->Enter() != MMS_OK) return true;
    history->Leave();
    const auto result = Boundary([&]() -> MmsResult {
        Operation scope(*app);
        if (!ValidOrder(*app) || !app->Renderer()) return MMS_E_NOT_READY;
        unsigned slot = kModelSlotCount, count = 0;
        for (unsigned i = 0; i < kModelSlotCount; ++i) {
            const void* object = model ? static_cast<void*>(app->ModelSlot(i)) : app->AccessorySlot(i);
            if (object) ++count;
            else if (slot == kModelSlotCount) slot = i;
        }
        if (slot == kModelSlotCount) return MMS_E_NOT_READY;
        wchar_t absolute[256]{};
        const auto length = GetFullPathNameW(path, 256, absolute, nullptr);
        if (!length || length >= 256) return MMS_E_ARGUMENT;
        import_texture_cache::Preparation texturePreparation(*app->Renderer());
        std::shared_ptr<import_texture_cache::Retained> textures;
        std::unique_ptr<import_texture_cache::Publication> texturePublication;
        std::shared_ptr<HistoryChange> change;
        void* pointer = nullptr;
        if (model) {
            if (!app->Physics()) return MMS_E_NOT_READY;
            auto privateScene = std::make_shared<history_physics::ImportScene>(*app->Physics());
            model_import::Prepared prepared;
            const auto loaded = model_import::Prepare(*app, absolute, showInfo, privateScene->Scene(), texturePreparation, prepared);
            if (loaded != model_import::Result::Ready)
                return loaded == model_import::Result::Memory ? MMS_E_MEMORY : MMS_E_NOT_READY;
            if (!model_import::ShowMessages(app->MainWindow(), prepared.Messages())) return MMS_OK;
            auto& record = *mdl::Mdl(prepared.Object());
            record.comboSelIndex = record.comboSelIndex2 = static_cast<uint8_t>(count + 1);
            record.displayState = app->state.characterTransparentMode;
            auto physics = privateScene->Prepare();
            if (auto r = texturePreparation.Finish(textures, texturePublication)) return r;
            pointer = prepared.Object();
            change = RetainedSceneObject::ImportModel(*app, slot, prepared, privateScene,
                std::move(physics), std::move(textures), std::move(texturePublication));
        } else {
            accessory_import::Prepared prepared;
            const auto loaded = accessory_import::Prepare(*app, absolute, texturePreparation, prepared);
            if (loaded != accessory_import::Result::Ready)
                return loaded == accessory_import::Result::Memory ? MMS_E_MEMORY : MMS_E_NOT_READY;
            prepared.Object()->order = static_cast<uint8_t>(count);
            if (auto r = texturePreparation.Finish(textures, texturePublication)) return r;
            pointer = prepared.Object();
            change = RetainedSceneObject::ImportAccessory(*app, slot, prepared, std::move(textures), std::move(texturePublication));
        }
        auto selection = CaptureSelectionToken(*app);
        {
            Publication allow(scope);
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (auto r = history->PrepareNative(model ? "Import model" : "Import accessory", std::move(change), transaction)) return r;
            if (auto r = history->CommitNative(std::move(transaction))) return r;
        }
        const auto id = SceneObjectIdentity(pointer).id;
        if (model) { selection.model = id; selection.cameraMode = false; app->EditMode() = ViewportEditMode::Bone; }
        else selection.accessory = id;
        app->SceneModified() = 1; app->RecordedFrameCountByte1() = 1; app->PhysicsResetPending() = 1;
        RefreshPublishedUi(*app, *history, selection);
        return MMS_OK;
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI object import refused: %d", result);
    return true;
}
}
bool NativeObjectOperationActive(const MMDApp* app) noexcept { return operation && &operation->app == app; }
bool NativeObjectPublicationAuthorized(const MMDApp* app) noexcept {
    return NativeObjectOperationActive(app) && operation->publishing;
}
bool NativeObjectUiPending(const MMDApp* app) noexcept { return pendingUi == app && app; }
void ForgetBoundObjectUi(const MMDApp* app) noexcept {
    if (pendingUi == app) { pendingUi = nullptr; hasPendingSelection = false; }
}
bool TryDeleteBoundModel(MMDApp* app, HWND window) noexcept { return TryDelete(app, window, true); }
bool TryDeleteBoundAccessory(MMDApp* app, HWND window) noexcept { return TryDelete(app, window, false); }
bool TryImportBoundModel(MMDApp* app, const wchar_t* path, bool showInfo) noexcept { return TryImport(app, path, true, showInfo); }
bool TryImportBoundAccessory(MMDApp* app, const wchar_t* path) noexcept { return TryImport(app, path, false, false); }
bool RefreshBoundObjectUi(MMDApp& app, SharedHistory& history) noexcept {
    if (!Ready(app)) return false;
    return Boundary([&]() -> MmsResult {
        Operation scope(app);
        const auto selection = pendingUi == &app && hasPendingSelection ? pendingSelection : CaptureSelectionToken(app);
        return RefreshPublishedUi(app, history, selection) ? MMS_OK : MMS_E_FAILED;
    }) == MMS_OK;
}
MmsResult RunBoundObjectHistory(MMDApp& app, SharedHistory& history, bool redo, bool deferUi) noexcept {
    if (!Ready(app, deferUi) || EditorsOpen(app) || NativeObjectUiPending(&app)) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        Operation scope(app);
        const auto selection = CaptureSelectionToken(app);
        std::array<const void*, kModelSlotCount * 2> objects{};
        for (unsigned i = 0; i < kModelSlotCount; ++i) {
            objects[i] = app.ModelSlot(i); objects[i + kModelSlotCount] = app.AccessorySlot(i);
        }
        {
            Publication allow(scope);
            if (auto result = history.History(redo)) return result;
        }
        bool changedObjects = false;
        for (unsigned i = 0; i < kModelSlotCount; ++i)
            changedObjects = changedObjects || objects[i] != app.ModelSlot(i) || objects[i + kModelSlotCount] != app.AccessorySlot(i);
        if (changedObjects) SafeSelection(app);
        if (deferUi) {
            // A plugin may pump before its invocation returns. Make native
            // selection safe now, but leave every control update to the host.
            if (changedObjects) ScheduleUi(app, &selection);
        } else {
            PublishBoundPoseEdit(app, history, true);
            if (changedObjects) RefreshPublishedUi(app, history, selection);
            else curve_editor::NotifyOwnedChanged(app.MainWindow());
        }
        return MMS_OK;
    });
}
}
#endif
