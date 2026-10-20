#include "native_object_commands.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_accessory_settings.hpp"
#include "native_history_commands.hpp"
#include "native_camera_follow_ui.hpp"
#include "native_global_commands.hpp"
#include "native_morph_commands.hpp"
#include "native_pose_scope.hpp"
#include "history_runtime.hpp"
#include "scene_object_history.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/scene_history_ui.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <commctrl.h>
#include <array>
#include <cstdio>

namespace mikumikustudio { INT_PTR __stdcall AccessorySettingsDlgProc(HWND, UINT, WPARAM, LPARAM); }
namespace mikumikustudio::plugins {
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
struct AccessorySettingsSession {
    MMDApp& app;
    AccessorySettingsPlan plan;
    HWND dialog = nullptr;
    MmsResult result = MMS_E_CANCELLED;
};
thread_local AccessorySettingsSession* accessorySettings = nullptr;
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
bool ValidOrder(const MMDApp& app, bool repairModelRanks = false) noexcept {
    std::array<bool, kModelSlotCount + 1> models{};
    std::array<bool, kModelSlotCount> accessories{};
    unsigned modelCount = 0, accessoryCount = 0;
    for (unsigned slot = 0; slot < kModelSlotCount; ++slot) {
        if (const auto* model = mdl::Mdl(app.ModelSlot(slot))) {
            const auto order = model->comboSelIndex;
            if (!repairModelRanks && (order < 1 || order > kModelSlotCount || models[order])) return false;
            models[order] = true; ++modelCount;
        }
        if (const auto* accessory = app.AccessorySlot(slot)) {
            const auto order = accessory->order;
            if (order >= kModelSlotCount || accessories[order]) return false;
            accessories[order] = true; ++accessoryCount;
        }
    }
    if (!repairModelRanks)
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
        const auto edit = GetDlgItem(main, id);
        if (!SendMessageW(edit, EM_GETMODIFY, 0, 0)) SetWindowTextA(edit, text);
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
        SetModelExtensionMenuEnabled(&app, !app.CameraMode() && app.SelectedModel() != nullptr);
    }
    // Metadata edits keep the same object pointers/counts, but order/name
    // changes invalidate the cached timeline labels and row-to-slot mapping.
    // Rebuild only the camera-mode list; this path never seeks or evaluates.
    if (app.CameraMode() && app.Renderer() && app.PanelDC()) PostLanguageSweep(&app);
    EnableMenuItem(GetMenu(main), 249, MF_BYCOMMAND |
        (SendDlgItemMessageW(main, 471, CB_GETCOUNT, 0, 0) > 0 ? MF_ENABLED : MF_GRAYED));
    AccessoryReadout(app);
    EnableWindow(GetDlgItem(main, panel::kUndoButton), history.UndoCount() != 0);
    EnableWindow(GetDlgItem(main, panel::kRedoButton), history.RedoCount() != 0);
    if (app.CurveDC()) SelectionReeval(&app);
    InvalidateRect(main, nullptr, FALSE);
    RefreshBoundPoseReadout(app);
    RefreshBoundGlobalReadout(app);
    RefreshBoundMorphReadout(app);
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
bool BoundAccessorySettingsDialog(const MMDApp* app, HWND dialog) noexcept {
    return accessorySettings && &accessorySettings->app == app && accessorySettings->dialog == dialog &&
        NativeObjectOperationActive(app) && !NativeObjectPublicationAuthorized(app);
}
bool InitializeBoundAccessorySettings(MMDApp* app, HWND dialog, int& count) noexcept {
    if (!accessorySettings || &accessorySettings->app != app || accessorySettings->dialog ||
        !NativeObjectOperationActive(app) || GetParent(dialog) != app->MainWindow()) return false;
    accessorySettings->dialog = dialog;
    const auto result = Boundary([&]() -> MmsResult {
        std::array<mdl::AccessoryRecord*, kModelSlotCount> order{};
        count = 0;
        for (unsigned slot = 0; slot < kModelSlotCount; ++slot) if (auto* object = app->AccessorySlot(slot)) {
            if (object->order >= kModelSlotCount || order[object->order]) return MMS_E_STALE;
            order[object->order] = object; ++count;
        }
        if (!count || app->AccessoryOrderArray()) return MMS_E_NOT_READY;
        app->AccessoryOrderArray() = std::calloc(std::size_t(count), sizeof(mdl::AccessoryRecord*));
        if (!app->AccessoryOrderArray()) return MMS_E_MEMORY;
        auto** scratch = static_cast<mdl::AccessoryRecord**>(app->AccessoryOrderArray());
        const auto list = GetDlgItem(dialog, panel::kOrderListBox);
        for (int row = 0; row < count; ++row) {
            if (!order[row]) return MMS_E_STALE;
            scratch[row] = order[row];
            const auto text = text_encoding::Display(order[row]->name);
            if (SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())) != row) return MMS_E_MEMORY;
        }
        if (!SetDlgItemInt(dialog, panel::kAccessoryOrderEdit, app->AccessoryRenderSplitOrder(), FALSE)) return MMS_E_FAILED;
        SendMessageW(list, LB_SETCURSEL, WPARAM(-1), 0);
        return MMS_OK;
    });
    if (result) accessorySettings->result = result;
    return result == MMS_OK;
}
bool AcceptBoundAccessorySettings(MMDApp* app, HWND dialog) noexcept {
    if (!BoundNativePoseHistory(app)) return false;
    if (!BoundAccessorySettingsDialog(app, dialog)) { EndDialog(dialog, IDCANCEL); return true; }
    accessorySettings->result = accessorySettings->plan.ReadDialog(dialog);
    EndDialog(dialog, accessorySettings->result == MMS_OK ? 1 : IDCANCEL);
    return true;
}
bool TryEditBoundAccessorySettings(MMDApp* app) noexcept {
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app); if (!history) return false;
    if (!Ready(*app) || EditorsOpen(*app) || NativeObjectUiPending(app) || accessorySettings) return true;
    if (history->CheckNativeEdit() != MMS_OK) return true;
    const auto result = Boundary([&]() -> MmsResult {
        Operation scope(*app); AccessorySettingsSession session{*app};
        if (auto r = session.plan.Begin(*app)) return r;
        const auto selection = CaptureSelectionToken(*app);
        accessorySettings = &session;
        struct Cleanup {
            MMDApp& app;
            ~Cleanup() { accessorySettings = nullptr; std::free(app.AccessoryOrderArray()); app.AccessoryOrderArray() = nullptr; }
        } cleanup{*app};
        const auto response = UiDialogBoxParamA(static_cast<HINSTANCE>(app->HInstance()),
            MAKEINTRESOURCEA(app->EnglishUI() ? 0x293 : 0x273), app->MainWindow(), AccessorySettingsDlgProc, 0);
        // Scratch data is no longer an active editor when entering the journal.
        std::free(app->AccessoryOrderArray()); app->AccessoryOrderArray() = nullptr;
        accessorySettings = nullptr;
        if (response == -1) return MMS_E_FAILED;
        if (response != 1) return session.result == MMS_E_CANCELLED ? MMS_OK : session.result;
        if (session.result != MMS_OK) return session.result;
        std::shared_ptr<HistoryChange> change;
        if (auto r = session.plan.Take(change)) return r;
        if (!change) return MMS_OK;
        {
            Publication allow(scope);
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (auto r = history->PrepareNative("Accessory settings", std::move(change), transaction)) return r;
            if (auto r = history->CommitNative(std::move(transaction))) return r;
        }
        app->SceneModified() = 1;
        RefreshPublishedUi(*app, *history, selection);
        return MMS_OK;
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI accessory settings refused: %d", result);
    return true;
}
bool RefreshBoundObjectUi(MMDApp& app, SharedHistory& history) noexcept {
    if (!Ready(app)) return false;
    return Boundary([&]() -> MmsResult {
        Operation scope(app);
        const auto selection = pendingUi == &app && hasPendingSelection ? pendingSelection : CaptureSelectionToken(app);
        return RefreshPublishedUi(app, history, selection) ? MMS_OK : MMS_E_FAILED;
    }) == MMS_OK;
}
MmsResult CommitBoundModelOrder(MMDApp& app, SharedHistory& history,
    std::shared_ptr<HistoryChange> change, bool calculate) noexcept {
    if (!change) return MMS_OK;
    if (BoundNativePoseHistory(&app) != &history || !Ready(app) || EditorsOpen(app) || NativeObjectUiPending(&app)) return MMS_E_BUSY;
    if (!ValidOrder(app, true)) return MMS_E_STALE;
    if (const auto result = history.CheckNativeEdit()) return result;
    return Boundary([&]() -> MmsResult {
        Operation scope(app);
        const auto selection = CaptureSelectionToken(app);
        if (!ValidOrder(app, true)) return MMS_E_STALE;
        {
            Publication allow(scope);
            std::unique_ptr<SharedHistory::NativeTransaction> transaction;
            if (auto result = history.PrepareNative(calculate ? "Model calculation order" : "Model draw order",
                    std::move(change), transaction, SharedHistory::NativeIntent::ExplicitCommit)) return result;
            if (auto result = history.CommitNative(std::move(transaction))) return result;
        }
        app.SceneModified() = 1;
        if (!calculate) InvalidateEffectCurrentObjectCache();
        RefreshPublishedUi(app, history, selection);
        return MMS_OK;
    });
}
MmsResult RunBoundObjectHistory(MMDApp& app, SharedHistory& history, bool redo, bool deferUi) noexcept {
    if (!Ready(app, deferUi) || EditorsOpen(app) || NativeObjectUiPending(&app)) return MMS_E_BUSY;
    return Boundary([&]() -> MmsResult {
        // A pending draft may need storage. Capture it before the journal
        // publishes, so failure cannot hide a successfully applied traversal.
        ScopedFollowReadoutDrafts drafts(app);
        Operation scope(app);
        const auto selection = CaptureSelectionToken(app);
        std::array<const void*, kModelSlotCount * 2> objects{};
        std::array<std::uint8_t, kModelSlotCount> modelDrawOrder{};
        struct AccessoryUi { std::uint8_t order = 0; std::array<char, 100> name{}; };
        std::array<AccessoryUi, kModelSlotCount> accessoryUi{};
        const auto split = app.AccessoryRenderSplitOrder();
        for (unsigned i = 0; i < kModelSlotCount; ++i) {
            objects[i] = app.ModelSlot(i); objects[i + kModelSlotCount] = app.AccessorySlot(i);
            if (const auto* model = mdl::Mdl(app.ModelSlot(i))) modelDrawOrder[i] = model->comboSelIndex;
            if (const auto* a = app.AccessorySlot(i)) {
                accessoryUi[i].order = a->order;
                std::memcpy(accessoryUi[i].name.data(), a->name, sizeof a->name);
            }
        }
        {
            Publication allow(scope);
            if (auto result = history.History(redo)) return result;
        }
        bool changedObjects = false;
        for (unsigned i = 0; i < kModelSlotCount; ++i)
            changedObjects = changedObjects || objects[i] != app.ModelSlot(i) || objects[i + kModelSlotCount] != app.AccessorySlot(i);
        bool changedLists = changedObjects || split != app.AccessoryRenderSplitOrder();
        for (unsigned i = 0; i < kModelSlotCount && !changedLists; ++i)
            if (const auto* model = mdl::Mdl(app.ModelSlot(i))) changedLists = model->comboSelIndex != modelDrawOrder[i];
        for (unsigned i = 0; i < kModelSlotCount && !changedLists; ++i) if (const auto* a = app.AccessorySlot(i))
            changedLists = a->order != accessoryUi[i].order || std::memcmp(a->name, accessoryUi[i].name.data(), sizeof a->name);
        if (changedObjects) SafeSelection(app);
        if (deferUi) {
            // A plugin may pump before its invocation returns. Make native
            // selection safe now, but leave every control update to the host.
            if (changedLists) ScheduleUi(app, &selection);
        } else {
            PublishBoundPoseEdit(app, history, history.LastPublicationRequiresPhysicsReset());
            if (changedLists) RefreshPublishedUi(app, history, selection);
            else {
                // The journal already published its local track evaluation.
                // Refresh values without seeking, while Operation still blocks
                // control notifications from starting another scene edit.
                RefreshBoundGlobalReadout(app);
                RefreshBoundMorphReadout(app);
                const auto notified = Boundary([&]() -> MmsResult {
                    curve_editor::NotifyOwnedChanged(app.MainWindow()); return MMS_OK;
                });
                // A pending-source allocation must not turn an applied undo
                // into a failed command that a caller could repeat.
                if (notified != MMS_OK) ScheduleUi(app, &selection);
            }
        }
        return MMS_OK;
    });
}
}
#endif
