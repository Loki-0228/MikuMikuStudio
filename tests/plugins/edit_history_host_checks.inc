// Actual hidden host, DLL service negotiation, menu dispatch and one journal.
#include "../src/plugins/native_pose_scope.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
#include "../src/plugins/native_key_commands.hpp"
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
    app->CameraMode() = 0; app->SelectedModelSlot() = 0; model.selectedBone = 0;
    std::memset(model.boneSelection, 0, model.boneCount); model.boneSelection[0] = 1;
    model.boneTable[0].trans[1] = 4.f; model.bonePhysicsState[0] = 1;
    Shutdown();
    Check(InitializeHistoryTestRuntime(app), "explicit host binding exposes the same common journal");
    auto* history = NativeHistory(app);
    Check(history && BoundNativePoseHistory(app) == history && !history->UndoCount(), "native and plugin history share ownership");
    const auto library = GetModuleHandleW(L"manager_edit_probe.dll");
    auto value = reinterpret_cast<int(*)()>(GetProcAddress(library, "GetProbeValue"));
    auto flags = reinterpret_cast<uint64_t(*)()>(GetProcAddress(library, "GetProbeFlags"));
    auto cancels = reinterpret_cast<int(*)()>(GetProcAddress(library, "GetProbeCancels"));
    Check(value && flags && cancels && (flags() & 1) && !(flags() & 2), "controlled host negotiates edit service");
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
    MixedModelRange(app, *history);
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
    const auto beforeClose = cancels(); Menu(app, L"Edit probe pending");
    wchar_t dllPath[32768]{}; Check(GetModuleFileNameW(library, dllPath, 32768) > 0, "loaded probe path available");
    const auto lease = LoadLibraryW(dllPath); Check(lease != nullptr, "retain read-only probe exports through shutdown");
    Shutdown();
    Check(NativeHistory(app) == nullptr && BoundNativePoseHistory(app) == nullptr && cancels() == beforeClose + 1 && value() == -1,
          "shutdown cancels pending edits before destroying the plugin and common host");
    FreeLibrary(lease);
    app->SceneModified() = 0;
    std::puts("PASS production plugin/native common history, revision, project and shutdown");
}
}
