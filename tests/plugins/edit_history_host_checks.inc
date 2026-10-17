// Actual hidden host, DLL service negotiation, menu dispatch and one journal.
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/plugins/native_global_commands.hpp"
#include "../src/plugins/native_morph_commands.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include <shlobj.h>
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "../src/plugins/native_key_commands.hpp"
#include "../src/plugins/native_object_commands.hpp"
#include "../src/plugins/native_frame_lines.hpp"
#include "../src/plugins/native_morph_keys.hpp"
#include "../src/plugins/native_display_keys.hpp"
#include "../src/plugins/native_global_paste.hpp"
#include "../src/plugins/native_model_paste.hpp"
#include "../src/plugins/native_vmd_source.hpp"
#include "../src/plugins/native_model_vmd_source.hpp"
#include "../src/plugins/native_viewport_history.hpp"
#include "../src/plugins/scene_object_history.hpp"
#include "mikudancestudio/accessory_motion_io.hpp"
#include "mikudancestudio/clipboard_layout.hpp"
#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/ui_language.hpp"
#include "mikudancestudio/system_settings_ui.hpp"
namespace mikudancestudio { void DispatchSeparateWindowCommand(MMDApp*, unsigned short); }
namespace edit_history_host_checks {
UINT FindMenuCommand(HMENU menu, const wchar_t* label) {
    for (int index = 0; menu && index < GetMenuItemCount(menu); ++index) {
        wchar_t text[256]{};
        GetMenuStringW(menu, index, text, 256, MF_BYPOSITION);
        if (!wcscmp(text, label)) return GetMenuItemID(menu, index);
        if (const auto found = FindMenuCommand(GetSubMenu(menu, index), label)) return found;
    }
    return 0;
}
void Menu(MMDApp* app, const wchar_t* label) {
    plugins::Refresh(app);
    const auto id = FindMenuCommand(GetMenu(app->MainWindow()), label);
    Check(id && id != UINT(-1), "registered edit probe menu exists");
    SendMessageW(app->MainWindow(), WM_COMMAND, id, 0);
}
void RotationReset(MMDApp* app, plugins::SharedHistory& history, int (*value)()) {
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    std::array<float, 4> original{};
    std::memcpy(original.data(), model.boneTable[0].rotQuat, sizeof(original));
    const auto dirty = model.bonePhysicsState[0];
    const auto position = model.boneTable[0].trans[1];
    const auto ring = model.undoState[0];
    const std::array<float, 4> rotated{0, 0, .6f, .8f}, identity{0, 0, 0, 1};
    std::memcpy(model.boneTable[0].rotQuat, rotated.data(), sizeof(rotated));
    model.bonePhysicsState[0] = 0;
    Menu(app, L"Edit probe prepared"); app->SceneModified() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, 302, 0);
    Check(value() == 0 && !history.UndoCount() && !app->SceneModified() &&
        !std::memcmp(model.boneTable[0].rotQuat, rotated.data(), sizeof(rotated)) && model.undoState[0] == ring,
        "pending real plugin transaction blocks rotation reset without old-ring fallback");
    Menu(app, L"Edit probe cancel");
    Menu(app, L"Edit probe once");
    SendMessageW(app->MainWindow(), WM_COMMAND, 302, 0);
    Check(value() == 1 && history.UndoCount() == 2 &&
        !std::memcmp(model.boneTable[0].rotQuat, identity.data(), sizeof(identity)) &&
        model.boneTable[0].trans[1] == position && model.undoState[0] == ring,
        "actual view-menu rotation reset joins the plugin journal without old-ring or position changes");
    Menu(app, L"Edit probe once"); Menu(app, L"Edit probe undo");
    app->SceneModified() = 0; app->RecordedFrameCountByte1() = 0;
    SendMessageW(app->MainWindow(), WM_COMMAND, 302, 0);
    Check(value() == 1 && history.UndoCount() == 2 && history.RedoCount() == 1 &&
        !app->SceneModified() && !app->RecordedFrameCountByte1(),
        "production identity reset preserves the later plugin redo branch");
    Menu(app, L"Edit probe redo");
    Check(value() == 2 && history.UndoCount() == 3, "plugin redo remains usable after no-op rotation reset");
    const auto saved = std::filesystem::absolute(L"rotation-reset.pmm");
    Check(SaveSceneCopy(app, saved.c_str()) && history.UndoCount() == 3,
        "PMM save accepts committed rotation reset without discarding common history");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(value() == 1 && !std::memcmp(model.boneTable[0].rotQuat, rotated.data(), sizeof(rotated)) &&
        model.bonePhysicsState[0] == 0, "real plugin undo restores the exact quaternion and pending flag");
    SendMessageW(app->MainWindow(), WM_COMMAND, 400, 0);
    app->CameraMode() = 1;
    Menu(app, L"Edit probe redo"); SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0);
    Menu(app, L"Edit probe redo");
    Check(value() == 2 && !std::memcmp(model.boneTable[0].rotQuat, identity.data(), sizeof(identity)),
        "camera-mode native and plugin redo preserve shared rotation-reset ordering");
    Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo"); Menu(app, L"Edit probe undo");
    Check(value() == 0 && history.Reset() == MMS_OK, "rotation-reset probe restores plugin state and clears its setup history");
    app->CameraMode() = 0;
    std::memcpy(model.boneTable[0].rotQuat, original.data(), sizeof(original));
    model.bonePhysicsState[0] = dirty;
}
#include "bone_numeric_host_checks.inc"
#include "camera_reset_host_checks.inc"
#include "camera_numeric_host_checks.inc"
#include "camera_axis_host_checks.inc"
#include "camera_text_host_checks.inc"
#include "light_text_host_checks.inc"
#include "fov_text_host_checks.inc"
#include "camera_projection_host_checks.inc"
#include "shadow_parameter_host_checks.inc"
void MixedModelDeletion(MMDApp* app, plugins::SharedHistory& history) {
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    Check(model.boneCount && model.morphCount && model.boneCount < mdl::BoneKeyCapacity(model.boneKeys) &&
          model.morphCount < mdl::MorphKeyCapacity(model.morphKeys) && mdl::KeyPoolCapacity(model.displayKeys) > 1,
          "real model has all three owned key families");
    // Fixture setup precedes this journal's first operation. Keep the original
    // unregistered pose so the subsequent native/plugin test has the same input.
    for (std::size_t i = 0; i < mdl::BoneKeyCapacity(model.boneKeys); ++i) model.boneKeys[i].allocated = 0;
    for (std::size_t i = 0; i < mdl::MorphKeyCapacity(model.morphKeys); ++i) model.morphKeys[i].allocated = 0;
    for (std::size_t i = 0; i < mdl::KeyPoolCapacity(model.displayKeys); ++i) model.displayKeys[i].allocated = 0;
    const auto boneSlot = model.boneCount, morphSlot = model.morphCount;
    auto& bone = model.boneKeys[boneSlot]; bone = model.boneKeys[0];
    bone.frame = 10; bone.previous = bone.next = 0; bone.allocated = 1;
    model.boneKeys[0].next = boneSlot;
    auto& morph = model.morphKeys[morphSlot]; morph = model.morphKeys[0];
    morph.frame = 10; morph.previous = morph.next = 0; morph.value = .75f; morph.allocated = 1;
    model.morphKeys[0].next = morphSlot; model.morphKeys[0].value = .25f; model.morphs[0].value = .75f;
    auto& display = model.displayKeys[1]; display.frame = 10; display.previous = display.next = 0;
    display.visible = 0; display.allocated = 1; model.displayKeys[0].next = 1; model.displayKeys[0].visible = 1;
    if (model.ikChainCount) std::memcpy(display.ikStates, model.displayKeys[0].ikStates, model.ikChainCount);
    if (model.boneOrderCount) std::memcpy(display.selectorStates, model.displayKeys[0].selectorStates,
                                       model.boneOrderCount * sizeof(mdl::BoneReference));
    model.loadComplete = 0; app->CurrentFrame() = 10;
    unsigned reads = 0; float readValue = 0;
    curve_editor::Session session; session.id = "model-key-refresh";
    curve_editor::Source source; source.id = "morph"; source.label = L"Test morph";
    source.read = [&](std::vector<curve_editor::Channel>& channels, std::wstring&) {
        ++reads; readValue = model.morphs[0].value;
        curve_editor::Channel channel; channel.id = "weight"; channel.label = L"Weight";
        animation_curve::Key key; key.frame = 0; key.value = readValue; channel.track.keys.push_back(key);
        channels = {channel}; return true;
    };
    session.sources.push_back(std::move(source));
    Check(curve_editor::Open(app->MainWindow(), std::move(session), false) != nullptr && reads,
          "hidden morph curve editor reads its initial source");
    const auto refreshed = [&](unsigned before, float expected) {
        const auto until = GetTickCount64() + 1000;
        while (reads == before && GetTickCount64() < until) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (reads == before) Sleep(1);
        }
        Check(reads > before && readValue == expected, "model-key history refreshes the open morph curve source");
    };
    auto beforeRead = reads;
    SendMessageW(app->MainWindow(), WM_COMMAND, 423, 0);
    Check(history.UndoCount() == 1 && !bone.frame && !morph.frame && !display.frame &&
          !model.boneKeys[0].next && !model.morphKeys[0].next && !model.displayKeys[0].next &&
          model.morphs[0].value == .25f && model.loadComplete == 1,
          "actual delete button atomically publishes bone morph display keys and current derived state");
    refreshed(beforeRead, .25f); beforeRead = reads;
    app->CameraMode() = 1;
    SendMessageW(app->MainWindow(), WM_COMMAND, 400, 0);
    Check(!history.UndoCount() && history.RedoCount() == 1 && bone.frame == 10 && morph.frame == 10 &&
          display.frame == 10 && model.morphs[0].value == .75f && model.loadComplete == 0,
          "one actual undo restores every model-key family");
    refreshed(beforeRead, .75f); beforeRead = reads;
    app->CurrentFrame() = 20; Menu(app, L"Edit probe redo");
    Check(history.UndoCount() == 1 && !bone.frame && !morph.frame && !display.frame &&
          model.morphs[0].value == .25f && model.loadComplete == 1,
          "plugin redo replays native model-key deletion at the current frame");
    refreshed(beforeRead, .25f);
    Menu(app, L"Edit probe undo");
    Check(bone.frame == 10 && morph.frame == 10 && display.frame == 10 && history.Reset() == MMS_OK,
          "restore fixture tracks and clear isolated setup history");
    curve_editor::CloseOwned(app->MainWindow());
    app->CurrentFrame() = 0; app->CameraMode() = 0;
}
void MixedModelRange(MMDApp* app, plugins::SharedHistory& history) {
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    const auto dialog = CreateWindowExW(0, L"STATIC", L"Range probe", WS_CHILD, 0, 0, 200, 100,
        app->MainWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(dialog != nullptr, "hidden range dialog created");
    for (const auto& entry : std::array<std::pair<int, const wchar_t*>, 3>{{
        {panel::kScaleFromEdit, L"0"}, {panel::kScaleToEdit, L"20"}, {panel::kScaleRateEdit, L"2"}}})
        Check(CreateWindowExW(0, L"EDIT", entry.second, WS_CHILD, 0, 0, 40, 20, dialog,
            reinterpret_cast<HMENU>(INT_PTR(entry.first)), GetModuleHandleW(nullptr), nullptr) != nullptr, "range number field created");
    for (int id : {panel::kScaleBoneCheckbox, panel::kScaleMorphCheckbox, panel::kScaleDispIkCheckbox}) {
        Check(CreateWindowExW(0, L"BUTTON", L"family", WS_CHILD | BS_CHECKBOX, 0, 0, 40, 20, dialog,
            reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr) != nullptr, "range family checkbox created");
        CheckDlgButton(dialog, id, BST_CHECKED);
    }
    Check(plugins::TryScaleBoundBoneRange(app, dialog), "controlled range route consumes the three-family dialog");
    DestroyWindow(dialog);
    const auto frames = [&](unsigned frame) {
        return model.boneKeys[model.boneKeys[0].next].frame == frame &&
            model.morphKeys[model.morphKeys[0].next].frame == frame && model.displayKeys[model.displayKeys[0].next].frame == frame;
    };
    Check(history.UndoCount() == 1 && frames(20) && model.maxFrame >= 20 && app->LastRegisteredFrame() >= 20,
          "three-family range publishes one record and one shared timeline maximum");
    app->CurrentFrame() = 15; Menu(app, L"Edit probe undo");
    Check(!history.UndoCount() && frames(10) && model.morphs[0].value == .75f && model.loadComplete == 0,
          "plugin undo restores every range track and current-frame values");
    SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0);
    Check(history.UndoCount() == 1 && frames(20) && model.morphs[0].value == .625f && model.loadComplete == 1,
          "native redo applies the mixed range with current-frame morph and display sampling");
    Menu(app, L"Edit probe undo");
    Check(history.Reset() == MMS_OK, "clear isolated range setup history");
    app->CurrentFrame() = 0;
}
void ModelInterpolation(MMDApp* app, plugins::SharedHistory& history) {
    auto& model = *mdl::Mdl(app->ModelSlot(0));
    auto& key = model.boneKeys[model.boneKeys[0].next];
    const auto combo = GetDlgItem(app->MainWindow(), panel::kInterpCurveCombo);
    Check(combo && SendMessageW(combo, CB_GETCOUNT, 0, 0) >= 6, "real model interpolation selector has five channels and all");
    const auto selection = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    const std::array<unsigned char, 4> original{key.interpolation[0], key.interpolation[4], key.interpolation[8], key.interpolation[12]};
    const auto a = app->state.lightA[0], b = app->state.lightB[0], c = app->state.lightC[0], d = app->state.lightD[0];
    const auto matches = [&](const std::array<unsigned char, 4>& curve) {
        return key.interpolation[0] == curve[0] && key.interpolation[4] == curve[1] &&
               key.interpolation[8] == curve[2] && key.interpolation[12] == curve[3];
    };
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    app->state.lightA[0] = 25; app->state.lightB[0] = 30; app->state.lightC[0] = 80; app->state.lightD[0] = 100;
    SendMessageW(app->MainWindow(), WM_COMMAND, 431, 0);
    Check(history.UndoCount() == 1 && matches({25, 30, 80, 100}) && model.boneTable[0].trans[1] == 4.f && model.bonePhysicsState[0],
          "actual interpolation paste creates one node and preserves the unregistered pose");
    SendMessageW(app->MainWindow(), WM_COMMAND, 431, 0);
    Check(history.UndoCount() == 1, "repeating an identical interpolation paste creates no history");
    app->CurrentFrame() = 17; Menu(app, L"Edit probe undo");
    Check(matches(original) && model.boneTable[0].trans[1] == 4.f && history.RedoCount() == 1,
          "plugin undo restores the original interpolation without seeking the changed playhead");
    SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0);
    Check(matches({25, 30, 80, 100}) && model.boneTable[0].trans[1] == 4.f,
          "native redo restores interpolation while preserving the current pose");
    SendMessageW(app->MainWindow(), WM_COMMAND, 432, 0);
    Check(history.UndoCount() == 2 && matches({20, 20, 107, 107}), "actual interpolation reset joins the same journal");
    Menu(app, L"Edit probe undo");
    Check(matches({25, 30, 80, 100}), "plugin undo restores the curve before reset");
    Menu(app, L"Edit probe undo");
    Check(matches(original) && history.Reset() == MMS_OK, "restore the original curve and clear isolated interpolation history");
    app->state.lightA[0] = a; app->state.lightB[0] = b; app->state.lightC[0] = c; app->state.lightD[0] = d;
    SendMessageW(combo, CB_SETCURSEL, selection, 0); app->CurrentFrame() = 0;
}
struct DeleteConfirmation {
    HHOOK hook = nullptr;
    HWND owner = nullptr;
    std::wstring expectedTitle, expectedText;
    UINT expectedFlags = 0;
    unsigned seen = 0, allowed = 0;
    static thread_local DeleteConfirmation* active;
    static bool AllowMessageBox(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
        if (!active || active->allowed || owner != active->owner || !text || !title ||
            flags != active->expectedFlags || title != active->expectedTitle || text != active->expectedText) return false;
        ++active->allowed;
        return true;
    }
    static LRESULT CALLBACK HideAndAccept(int code, WPARAM wp, LPARAM lp) {
        if (code == HCBT_ACTIVATE && active) {
            const auto window = reinterpret_cast<HWND>(wp);
            wchar_t type[32]{}; GetClassNameW(window, type, 32);
            wchar_t title[128]{}; GetWindowTextW(window, title, 128);
            if (!wcscmp(type, L"#32770") && GetWindow(window, GW_OWNER) == active->owner && title == active->expectedTitle) {
                ++active->seen; ShowWindow(window, SW_HIDE);
                Check(GetDlgItem(window, IDOK) && GetDlgItem(window, IDCANCEL), "real object deletion uses an OK/cancel confirmation");
                PostMessageW(window, WM_COMMAND, IDOK, 0);
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    DeleteConfirmation(MMDApp* app, bool model) : owner(app->MainWindow()),
        expectedFlags(MB_OKCANCEL | (app->FloatingWindow() ? MB_TOPMOST : 0)) {
        const auto language = app->EnglishUI() <= 2 ? app->EnglishUI() : 1;
        const wchar_t* titles[2][3] = {{L"アクセサリ削除", L"Delete accessory", L"删除配件"},
            {L"モデル削除", L"Delete model", L"删除模型"}};
        const wchar_t* bodies[2][3] = {
            {L"アクセサリと全フレームのデータを削除しますか？\nこの操作は元に戻せます。",
             L"Delete the accessory and all its frame data?\nYou can undo this operation.", L"确定删除配件及其全部帧数据吗？\n此操作可以撤销。"},
            {L"モデルと全フレームのデータを削除しますか？\nこの操作は元に戻せます。",
             L"Delete the model and all its frame data?\nYou can undo this operation.", L"确定删除模型及其全部帧数据吗？\n此操作可以撤销。"}};
        expectedTitle = titles[model][language]; expectedText = bodies[model][language];
        const auto name = model ? text_encoding::ModelName(*mdl::Mdl(app->ModelSlot(0)), UiUsesEnglishNames(app->EnglishUI())) :
            text_encoding::Display(app->AccessorySlot(0)->name);
        if (!name.empty()) { expectedText += L"\n\n"; expectedText += name; }
        Check(!active && !expectedMessageBoxW, "no nested host deletion confirmation"); active = this;
        expectedMessageBoxW = AllowMessageBox;
        Check(!AllowMessageBox(nullptr, expectedText.c_str(), expectedTitle.c_str(), expectedFlags) &&
            !AllowMessageBox(owner, L"unexpected body", expectedTitle.c_str(), expectedFlags) &&
            !AllowMessageBox(owner, expectedText.c_str(), L"unexpected title", expectedFlags) &&
            !AllowMessageBox(owner, expectedText.c_str(), expectedTitle.c_str(), expectedFlags | MB_ICONWARNING),
            "confirmation allowance rejects other owners, text, titles and flags");
        hook = SetWindowsHookExW(WH_CBT, HideAndAccept, nullptr, GetCurrentThreadId());
        Check(hook != nullptr, "install hidden host deletion confirmation hook");
    }
    ~DeleteConfirmation() { UnhookWindowsHookEx(hook); expectedMessageBoxW = nullptr; active = nullptr; }
};
thread_local DeleteConfirmation* DeleteConfirmation::active = nullptr;
void FlushObjectUi(MMDApp* app) {
    const auto until = GetTickCount64() + 2000;
    while (plugins::NativeObjectUiPending(app) && GetTickCount64() < until) {
        MSG message{}; unsigned remaining = 64;
        while (remaining-- && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (plugins::NativeObjectUiPending(app)) Sleep(1);
    }
    Check(!plugins::NativeObjectUiPending(app), "real deferred host refresh completes after plugin invocation");
}
void ObjectMenuDeletion(MMDApp* app, plugins::SharedHistory& history, const MmsSceneApiV1& api,
                        const wchar_t* modelPath, const wchar_t* accessoryPath) {
    using namespace plugins;
    std::puts("RUN actual model/accessory deletion menus and plugin history"); std::fflush(stdout);
    auto* model = app->ModelSlot(0); auto* accessory = app->AccessorySlot(0);
    Check(model && accessory && !history.UndoCount(), "real resource deletion begins with both loaded objects and an empty journal");
    auto& record = *mdl::Mdl(model);
    auto* vertexBuffer = record.vertexBuffer; auto* mesh = accessory->mesh; auto* keys = app->AccessoryKeys(0);
    const auto poseY = record.boneTable[0].trans[1]; const auto dirty = record.bonePhysicsState[0];
    const auto frame = app->CurrentFrame(); const auto selectedModel = app->SelectedModelSlot();
    const auto originalModel = SceneObjectIdentity(model), originalAccessory = SceneObjectIdentity(accessory);
    const auto modelSource = std::filesystem::absolute(modelPath), accessorySource = std::filesystem::absolute(accessoryPath);
    Check(std::filesystem::equivalent(modelSource.parent_path(), std::filesystem::current_path()) &&
        std::filesystem::equivalent(accessorySource.parent_path(), std::filesystem::current_path()), "offline source test stays inside its isolated fixture directory");
    struct Offline {
        std::filesystem::path source, moved;
        bool active = true;
        explicit Offline(std::filesystem::path value) : source(std::move(value)), moved(source.wstring() + L".history-offline") {
            std::filesystem::rename(source, moved);
        }
        void Restore() {
            std::filesystem::rename(moved, source); active = false;
            Check(std::filesystem::exists(source), "restore isolated source file for later PMM reopen checks");
        }
        ~Offline() { if (active) { std::error_code error; std::filesystem::rename(moved, source, error); } }
    } offlineModel(modelSource), offlineAccessory(accessorySource);
    Check(!std::filesystem::exists(modelSource) && !std::filesystem::exists(accessorySource), "model and accessory source files are unavailable before deletion");
    MmsSceneQueryV1 query{}; query.h = Header<MmsSceneQueryV1>(); query.flags = MMS_SCENE_CURRENT_TIME;
    MmsSnapshotV1 retainedSnapshot{}; MmsSpan retainedObjects{};
    Check(api.acquire(api.context, &query, &retainedSnapshot) == MMS_OK &&
        api.objects(api.context, retainedSnapshot.handle, &retainedObjects) == MMS_OK && retainedObjects.count == 2,
        "public scene snapshot sees both objects before actual menu deletion");
    const auto counts = [&](unsigned models, unsigned accessories) {
        return SendDlgItemMessageW(app->MainWindow(), 436, CB_GETCOUNT, 0, 0) == LRESULT(models + 1) &&
            SendDlgItemMessageW(app->MainWindow(), 471, CB_GETCOUNT, 0, 0) == LRESULT(accessories);
    };
    const auto sceneCount = [&](unsigned expected) {
        MmsSnapshotV1 snapshot{}; MmsSpan objects{};
        Check(api.acquire(api.context, &query, &snapshot) == MMS_OK && api.objects(api.context, snapshot.handle, &objects) == MMS_OK &&
            objects.count == expected, "public scene object count follows actual menu history publication");
        api.release(api.context, snapshot.handle);
    };
    const auto stale = [&](MmsObjectRef object) {
        auto selected = query; selected.objects = {&object, 1, sizeof(object), 1}; MmsSnapshotV1 snapshot{};
        Check(api.acquire(api.context, &selected, &snapshot) == MMS_E_STALE,
            "public scene rejects deleted or restored object's previous generation");
    };
    const auto samePose = [&] {
        return record.boneTable[0].trans[1] == poseY && record.bonePhysicsState[0] == dirty &&
            app->CurrentFrame() == frame && record.vertexBuffer == vertexBuffer && accessory->mesh == mesh;
    };
    app->CameraMode() = 0; app->SetSelectedModelSlot(0);
    SendDlgItemMessageW(app->MainWindow(), 436, CB_SETCURSEL, record.comboSelIndex, 0);
    { DeleteConfirmation confirmation(app, true); SendMessageW(app->MainWindow(), WM_COMMAND, 437, 0);
      Check(confirmation.allowed == 1 && confirmation.seen == 1 &&
          !DeleteConfirmation::AllowMessageBox(confirmation.owner, confirmation.expectedText.c_str(), confirmation.expectedTitle.c_str(), confirmation.expectedFlags),
          "actual model-delete button presents exactly one strictly permitted real confirmation"); }
    Check(!app->ModelSlot(0) && history.UndoCount() == 1 && app->CameraMode() && counts(0, 1),
        "actual 437 deletes the measured real model and updates the PMM model list");
    sceneCount(1); stale(originalModel);
    Check(api.objects(api.context, retainedSnapshot.handle, &retainedObjects) == MMS_OK && retainedObjects.count == 2,
        "old scene snapshot remains readable after real model deletion");
    Menu(app, L"Edit probe undo");
    Check(app->ModelSlot(0) == model && history.RedoCount() == 1 && NativeObjectUiPending(app),
        "actual plugin undo restores retained model before deferred control refresh");
    FlushObjectUi(app);
    const auto restoredModel = SceneObjectIdentity(model);
    Check(Same(originalModel.id, restoredModel.id) && originalModel.generation != restoredModel.generation &&
        counts(1, 1) && samePose(), "plugin undo keeps model ID/resources/pose and renews generation without source IO or seek");
    stale(originalModel); sceneCount(2);
    Menu(app, L"Edit probe redo"); FlushObjectUi(app);
    Check(!app->ModelSlot(0) && history.UndoCount() == 1 && counts(0, 1), "actual plugin redo removes model and refreshes native lists");
    Menu(app, L"Edit probe undo"); FlushObjectUi(app);
    Check(app->ModelSlot(0) == model && samePose() && history.Reset() == MMS_OK, "restore model and clear isolated model deletion history");

    app->SelectedAccessorySlot() = 0;
    SendDlgItemMessageW(app->MainWindow(), 471, CB_SETCURSEL, 0, 0);
    { DeleteConfirmation confirmation(app, false); SendMessageW(app->MainWindow(), WM_COMMAND, 473, 0);
      Check(confirmation.allowed == 1 && confirmation.seen == 1 &&
          !DeleteConfirmation::AllowMessageBox(confirmation.owner, confirmation.expectedText.c_str(), confirmation.expectedTitle.c_str(), confirmation.expectedFlags),
          "actual accessory-delete button presents exactly one strictly permitted real confirmation"); }
    Check(!app->AccessorySlot(0) && history.UndoCount() == 1 && counts(1, 0), "actual 473 deletes the measured X mesh and updates the PMM accessory list");
    stale(originalAccessory); sceneCount(1);
    Menu(app, L"Edit probe undo"); FlushObjectUi(app);
    const auto restoredAccessory = SceneObjectIdentity(accessory);
    Check(app->AccessorySlot(0) == accessory && app->AccessoryKeys(0) == keys && accessory->mesh == mesh && counts(1, 1) &&
        Same(originalAccessory.id, restoredAccessory.id) && originalAccessory.generation != restoredAccessory.generation,
        "actual plugin undo restores original X mesh/key pool and stable ID while source file is offline");
    Menu(app, L"Edit probe redo"); FlushObjectUi(app);
    Check(!app->AccessorySlot(0) && counts(1, 0), "actual plugin redo removes X object without disk access");
    Menu(app, L"Edit probe undo"); FlushObjectUi(app);
    Check(app->AccessorySlot(0) == accessory && app->ModelSlot(0) == model && counts(1, 1) && samePose() &&
        history.Reset() == MMS_OK, "restore both objects and clear isolated resource deletion history");
    api.release(api.context, retainedSnapshot.handle);
    app->CameraMode() = 0; app->SetSelectedModelSlot(selectedModel);
    app->MainModelComboSelection() = record.comboSelIndex;
    SendDlgItemMessageW(app->MainWindow(), 436, CB_SETCURSEL, record.comboSelIndex, 0);
    PostModelReload2(app);
    EnsureInterpChannelCombo(app);
    offlineAccessory.Restore(); offlineModel.Restore();
    std::puts("PASS actual 437/473, real plugin undo/redo, offline sources, stable scene IDs and deferred lists");
}
#include "accessory_text_host_checks.inc"
#include "accessory_flag_host_checks.inc"
#include "morph_text_host_checks.inc"
#include "grid_display_host_checks.inc"
#include "projected_shadow_host_checks.inc"
#include "vpd_import_host_checks.inc"
#include "shadow_ambient_host_checks.inc"
#include "shadow_slider_host_checks.inc"
#include "fov_slider_host_checks.inc"
#include "light_slider_host_checks.inc"
#include "morph_slider_host_checks.inc"
#include "system_settings_host_checks.inc"
#include "frame_line_host_checks.inc"
#include "key_paste_host_checks.inc"
#include "morph_paste_host_checks.inc"
#include "display_paste_host_checks.inc"
#include "global_paste_host_checks.inc"
#include "model_paste_host_checks.inc"
#include "vmd_global_host_checks.inc"
#include "camera_crash_vmd_host_checks.inc"
#include "vmd_accessory_host_checks.inc"
#include "vmd_model_host_checks.inc"
#include "vmd_model_scale_host_checks.inc"
#include "viewport_history_host_checks.inc"
#include "sparse_registration_host_checks.inc"
void Run(MMDApp* app) {
    using namespace plugins;
    const auto initial = GetModuleHandleW(L"manager_edit_probe.dll");
    Check(initial != nullptr, "normal startup loads the edit probe DLL");
    auto initialFlags = reinterpret_cast<uint64_t(*)()>(GetProcAddress(initial, "GetProbeFlags"));
    Check(initialFlags && (initialFlags() & 2) && !(initialFlags() & 1),
          "ordinary host keeps mms.edit unavailable");
    wchar_t path[32768]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", path, 32768) > 0, "edit host model fixture supplied");
    LoadModelFile(app, path, false);
    auto* raw = app->ModelSlot(0); Check(raw != nullptr, "load real model before controlled history binding");
    auto& model = *mdl::Mdl(raw);
    Check(model.boneCount && model.bonePhysicsState && model.boneSelection, "model has editable bones");
    wchar_t accessoryPath[32768]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_ACCESSORY", accessoryPath, 32768) > 0, "edit host accessory fixture supplied");
    LoadAccessoryFile(accessoryPath);
    LoadAccessoryFile(accessoryPath);
    Check(app->AccessorySlot(1),"second accessory for duplicate-name VMD matching");
    Check(app->AccessorySlot(0) && app->AccessorySlot(0)->mesh, "load real X object before controlled history binding");
    app->CameraMode() = 0; app->SelectedModelSlot() = 0; model.selectedBone = 0;
    std::memset(model.boneSelection, 0, model.boneCount); model.boneSelection[0] = 1;
    model.boneTable[0].trans[1] = 4.f; model.bonePhysicsState[0] = 1;
    Shutdown();
    Check(InitializeHistoryTestRuntime(app), "explicit host binding exposes the same common journal");
    auto* history = NativeHistory(app);
    Check(history && BoundNativePoseHistory(app) == history && !history->UndoCount(), "native and plugin history share ownership");
    Check(history->SetMode(SharedHistory::RecordingMode::AllOperations)==MMS_OK,"explicitly opt migration probes into all-operation history");
    const auto library = GetModuleHandleW(L"manager_edit_probe.dll");
    auto value = reinterpret_cast<int(*)()>(GetProcAddress(library, "GetProbeValue"));
    auto flags = reinterpret_cast<uint64_t(*)()>(GetProcAddress(library, "GetProbeFlags"));
    auto cancels = reinterpret_cast<int(*)()>(GetProcAddress(library, "GetProbeCancels"));
    Check(value && flags && cancels && (flags() & 1) && !(flags() & 2), "controlled host negotiates edit service");
    system_settings_host_checks::Run(app,*history,value);
    frame_line_host_checks::Run(app,*history,value);
    key_paste_host_checks::Run(app,*history,value);
    morph_paste_host_checks::Run(app,*history,value);
    display_paste_host_checks::Run(app,*history,value);
    global_paste_host_checks::Run(app,*history,value);
    model_paste_host_checks::Run(app,*history,value);
    vmd_global_host_checks::Run(app,*history,value);
    camera_crash_vmd_host_checks::Run(app,*history,value);
    vmd_accessory_host_checks::Run(app,*history,value);
    vmd_model_host_checks::Run(app,*history,value);
    vmd_model_scale_host_checks::Run(app,*history,value);
    viewport_history_host_checks::Run(app,*history,value);
    sparse_registration_host_checks::Run(app,*history,value);
    {
        auto extra=RetainSceneObject(*app,MMS_OBJECT_ACCESSORY,1);
        auto removal=extra->CaptureRemoval();std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(history->PrepareNative("Remove duplicate-name test accessory",removal,token)==MMS_OK&&history->CommitNative(std::move(token))==MMS_OK,
            "remove the additional VMD fixture through the normal retained-object transaction");
        Check(!app->AccessorySlot(1)&&history->Reset()==MMS_OK,"restore the original two-object fixture for existing object-menu tests");
        history->Collect();
    }
    Check(RefreshBoundObjectUi(*app,*history),"refresh original accessory list after the duplicate-name fixture");
    auto scene = CreateSceneStore(app); SceneClient client; auto api = scene->Api(client);
    const auto revisionMatches = [&] {
        MmsSceneQueryV1 query{}; query.h = Header<MmsSceneQueryV1>(); query.flags = MMS_SCENE_CURRENT_TIME;
        MmsSnapshotV1 snapshot{}; uint64_t revision = 0;
        Check(history->Revision(revision) == MMS_OK && api.acquire(api.context, &query, &snapshot) == MMS_OK,
              "capture edit revision through the public scene service");
        const bool same = snapshot.sceneRevision == revision; api.release(api.context, snapshot.handle);
        Check(same, "scene revision equals the journal revision after mixed edits");
    };
    MixedModelDeletion(app, *history);
    RotationReset(app, *history, value);
    NumericPose(app, *history, value);
    CameraRotationReset(app, *history, value);
    CameraNumericPose(app, *history, value);
    CameraAxisPose(app, *history, value);
    CameraTextPose(app, *history, value);
    LightTextPose(app, *history, value);
    light_slider_host_checks::Run(app, *history, value);
    FovTextPose(app, *history, value);
    fov_slider_host_checks::Run(app, *history, value);
    CameraProjectionMode(app, *history, value);
    AccessoryTextPose(app, *history, value);
    AccessoryFlagParameters(app, *history, value);
    ShadowParameters(app, *history, value);
    MorphTextPose(app, *history, value);
    morph_slider_host_checks::Run(app, *history, value);
    GridDisplayCommands(app, *history, value);
    ProjectedShadowFlags(app, *history, value);
    vpd_import_host_checks::Run(app, *history, value);
    shadow_ambient_host_checks::Run(app, *history, value);
    ShadowRangeSlider(app, *history, value);
    MixedModelRange(app, *history);
    ModelInterpolation(app, *history);
    ObjectMenuDeletion(app, *history, api, path, accessoryPath);
    revisionMatches(); app->SceneModified() = 0;
    app->PlaybackActive() = 1; Menu(app, L"Edit probe once"); app->PlaybackActive() = 0;
    app->FrameStepPlayback() = 1; Menu(app, L"Edit probe once"); app->FrameStepPlayback() = 0;
    app->state.recordPlaybackStartPending = true; Menu(app, L"Edit probe once"); app->state.recordPlaybackStartPending = false;
    Check(value() == 0 && !history->UndoCount() && !app->SceneModified(),
          "plugin edits are refused during playback, frame stepping and pending recording");
    const auto undoButton = GetDlgItem(app->MainWindow(), panel::kUndoButton);
    Check(undoButton != nullptr, "native undo control exists"); EnableWindow(undoButton, FALSE);
    Menu(app, L"Edit probe pump-once");
    Check(value() == 1 && history->UndoCount() == 1 && app->SceneModified(), "plugin commit creates one step and marks project dirty");
    const auto deadline = GetTickCount64() + 1000;
    while (!IsWindowEnabled(undoButton) && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (!IsWindowEnabled(undoButton)) Sleep(1);
    }
    Check(IsWindowEnabled(undoButton), "deferred history controls recover after a plugin pumps its queued wake");
    SendMessageW(app->MainWindow(), WM_COMMAND, 495, 0);
    Check(model.boneTable[0].trans[1] == 0 && history->UndoCount() == 2, "actual native reset joins the plugin journal");
    revisionMatches(); Menu(app, L"Edit probe once");
    Check(value() == 2 && history->UndoCount() == 3, "plugin reads the current revision after native editing");
    SendMessageW(app->MainWindow(), WM_COMMAND, 400, 0);
    Check(value() == 1 && model.boneTable[0].trans[1] == 0, "native undo reverses the last plugin edit");
    Menu(app, L"Edit probe undo");
    Check(value() == 1 && model.boneTable[0].trans[1] == 4.f, "plugin undo reverses the preceding native edit");
    SendMessageW(app->MainWindow(), WM_COMMAND, 400, 0);
    Check(value() == 0 && !history->UndoCount() && history->RedoCount() == 3, "mixed undo reaches the original state");
    app->CameraMode() = 1;
    Menu(app, L"Edit probe redo");
    SendMessageW(app->MainWindow(), WM_COMMAND, 401, 0);
    Menu(app, L"Edit probe redo");
    Check(value() == 2 && model.boneTable[0].trans[1] == 0 && history->UndoCount() == 3,
          "mixed redo is independent of current camera mode");
    revisionMatches(); Menu(app, L"Edit probe twice");
    Check(value() == 4 && history->UndoCount() == 5, "one plugin command can deliberately create two undo steps");
    const auto count = history->UndoCount(); const auto beforeFailure = cancels();
    Menu(app, L"Edit probe prepare-fail");
    Check(value() == 4 && history->UndoCount() == count && cancels() == beforeFailure + 1,
          "failed plugin prepare cancels without advancing shared history");
    const auto project = std::filesystem::absolute(L"edit-history.pmm");
    Check(SaveSceneCopy(app, project.c_str()), "actual PMM save accepts committed plugin state");
    const auto beforeReset = cancels(); Menu(app, L"Edit probe prepared");
    Check(!ResetPluginProject(app) && app->ModelSlot(0) == raw && value() == 4 && history->UndoCount() == count,
          "project reset refuses a pending edit before changing scene or history");
    Menu(app, L"Edit probe cancel");
    ResetAppState(app);
    Check(cancels() == beforeReset + 1 && value() == 0 && !history->UndoCount() && !history->RedoCount(),
          "actual new project clears history after the plugin cancels its prepared edit");
    wcscpy_s(app->state.envFileName, project.c_str()); LoadSceneFile();
    Check(app->ModelSlot(0) && value() == 4 && !history->UndoCount(), "PMM reopen restores plugin data with empty history");
    revisionMatches();
    Menu(app, L"Edit probe once");
    Check(value() == 5 && history->UndoCount() == 1, "editing continues from the reopened project revision");
    vmd_accessory_host_checks::RunPersistence(app);
    const auto beforeClose = cancels(); Menu(app, L"Edit probe pending");
    wchar_t dllPath[32768]{}; Check(GetModuleFileNameW(library, dllPath, 32768) > 0, "loaded probe path available");
    const auto lease = LoadLibraryW(dllPath); Check(lease != nullptr, "retain read-only probe exports through shutdown");
    Shutdown();
    Check(NativeHistory(app) == nullptr && BoundNativePoseHistory(app) == nullptr && cancels() == beforeClose + 1 && value() == -1,
          "shutdown cancels pending edits before destroying the plugin and common host");
    FreeLibrary(lease);
    viewport_history_host_checks::RunLifecycle(app);
    app->SceneModified() = 0;
    std::puts("PASS production plugin/native common history, revision, project and shutdown");
}
}
