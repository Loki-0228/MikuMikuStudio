// Hidden full-window regression: PMM camera mode retains a loaded model target;
// rotation-only bones accept and reset a modified numeric translation draft.
#include "mikumikustudio/bone_parent_coordinates.hpp"
#include "mikumikustudio/background_save.hpp"
#include "../src/plugins/native_pose_scope.hpp"
void CheckModelEditControls(mikumikustudio::MMDApp* app, const wchar_t* directory) {
    using namespace mikumikustudio;
    const auto root = std::filesystem::absolute(directory);
    std::filesystem::create_directories(root);
    const auto path = root / L"camera-mode.pmm";
    const auto combo = GetDlgItem(app->MainWindow(), panel::kMainComboModel);
    Check(combo != nullptr, "model edit regression has the real model combo");
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    ApplyModelComboSelection(app);
    Check(app->CameraMode() != 0, "fixture saves in camera mode");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str());
    Check(SaveSceneFile(app), "save real camera-mode PMM");
    background_save::Wait(app);
    Check(std::filesystem::exists(path), "synchronous save publishes camera-mode PMM before load");
    app->MainModelComboSelection() = 0;
    LoadSceneFile();
    Check(app->CameraMode() != 0 && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0,
        "PMM retains camera mode on load");
    const int remembered = app->MainModelComboSelection();
    Check(remembered > 0 && remembered < SendMessageW(combo, CB_GETCOUNT, 0, 0),
        "loaded camera-mode PMM remembers an available model before a toggle can open a picker");
    // With MMS_EXTRA_MODEL, retain a different target from SelectedModelSlot
    // too. A language refresh must preserve object identity, not just a row.
    if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 2)
        app->MainModelComboSelection() = remembered == 1 ? 2 : 1;
    const auto targetSlot = SendMessageW(combo, CB_GETITEMDATA, app->MainModelComboSelection(), 0);
    Check(targetSlot >= 0 && targetSlot < kModelSlotCount && app->ModelSlot(static_cast<int>(targetSlot)),
        "remembered camera-mode target resolves to real model item data");
    auto* const target = app->ModelSlot(static_cast<int>(targetSlot));
    auto* const targetRecord = mdl::Mdl(target);
    Check(targetRecord->boneCount > 0 && targetRecord->boneTable, "language refresh fixture has a real editable bone");
    auto& translation = targetRecord->boneTable[0].trans[0];
    const auto originalTranslation = translation;
    const auto originalDirty = static_cast<unsigned char>(app->SceneModified());
    translation += .125f;app->SceneModified() = 1;
    const auto draft = translation;
    const auto selected = app->SelectedModelSlot();
    const auto frame = app->CurrentFrame();
    const auto registered = app->LastRegisteredFrame();
    const auto revision = app->SceneRevision();
    const auto generation = app->SceneGeneration();
    const auto cameraX = app->CameraPositionX(), cameraY = app->CameraPositionY(), cameraZ = app->CameraPositionZ();
    const auto language = app->EnglishUI();
    const auto undoEnabled = IsWindowEnabled(GetDlgItem(app->MainWindow(), panel::kUndoButton));
    const auto redoEnabled = IsWindowEnabled(GetDlgItem(app->MainWindow(), panel::kRedoButton));
#if !MMD_ORIGINAL_PROFILE
    const auto* history = plugins::BoundNativePoseHistory(app);
    const auto historyGeneration = plugins::BoundNativePoseGeneration(app);
    const auto undos = history ? history->UndoCount() : 0;
    const auto redos = history ? history->RedoCount() : 0;
#endif
    for (const auto nextLanguage : {0, 1, 2, static_cast<int>(language)}) {
        app->EnglishUI() = static_cast<unsigned char>(nextLanguage);
        LocalizeUI(app);
        const auto row = app->MainModelComboSelection();
        const auto slot = SendMessageW(combo, CB_GETITEMDATA, row, 0);
        Check(app->CameraMode() != 0 && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0 &&
              row > 0 && slot >= 0 && slot < kModelSlotCount && app->ModelSlot(static_cast<int>(slot)) == target,
            "switching UI language keeps the remembered real model while the visible selector stays in camera mode");
        Check(app->SelectedModelSlot() == selected && app->CurrentFrame() == frame && app->LastRegisteredFrame() == registered &&
              app->SceneRevision() == revision && app->SceneGeneration() == generation && app->SceneModified() && translation == draft &&
              app->CameraPositionX() == cameraX && app->CameraPositionY() == cameraY && app->CameraPositionZ() == cameraZ,
            "selector localization preserves current object, transport, camera and unregistered dirty bone draft");
        Check(IsWindowEnabled(GetDlgItem(app->MainWindow(), panel::kUndoButton)) == undoEnabled &&
              IsWindowEnabled(GetDlgItem(app->MainWindow(), panel::kRedoButton)) == redoEnabled,
            "selector localization preserves actual Undo and Redo availability");
#if !MMD_ORIGINAL_PROFILE
        Check(plugins::BoundNativePoseHistory(app) == history && plugins::BoundNativePoseGeneration(app) == historyGeneration &&
              (!history || (history->UndoCount() == undos && history->RedoCount() == redos)),
            "selector localization neither changes the common journal nor replaces its binding");
#endif
    }
    translation = originalTranslation;app->SceneModified() = originalDirty;
    SendMessageW(GetDlgItem(app->MainWindow(), panel::kModelEditToggle), BM_CLICK, 0, 0);
    Check(app->CameraMode() == 0 && app->SelectedModel() == target,
        "To model switches to the remembered loaded object without opening a file picker");
    auto* model = app->SelectedModel();
    auto* record = mdl::Mdl(model);
    int bone = -1;
    for (unsigned i = 0; i < record->boneCount; ++i) {
        if (record->boneTable[i].type == mdl::BoneType::RotateMove &&
            !(record->boneTable[i].flags & mdl::kBoneFlagMovable) &&
            record->boneTable[i].parent >= 0) { bone = static_cast<int>(i); break; }
    }
    Check(bone >= 0, "loaded model has a parented rotation-only bone");
    std::memset(record->boneSelection, 0, record->boneCount);
    record->selectedBone = bone;
    record->boneSelection[bone] = 1;
    for (int axis = 0; axis < 3; ++axis) {
        const auto edit = GetDlgItem(app->MainWindow(), panel::kReadoutPosXEdit + axis);
        char input[64]{};
        std::snprintf(input, sizeof(input), "%.6f", BoneParentBindOffset(model, bone, axis) + 2.5f);
        SetWindowTextA(edit, input);
        SendMessageW(edit, EM_SETMODIFY, TRUE, 0);
        CommitEditControlTail(app, edit);
        Check(std::abs(record->boneTable[bone].trans[axis] - 2.5f) < .00001f,
            "real modified numeric control moves a rotation-only bone");
        SendMessageW(GetDlgItem(app->MainWindow(), 537 + axis), BM_CLICK, 0, 0);
        Check(record->boneTable[bone].trans[axis] == 0,
            "real XYZ button resets the actual rotation-only bone displacement");
        app->state.messageSeen = 1;
        FrameDriver(app);
        Check(record->boneTable[bone].trans[axis] == 0 && !SendMessageW(edit, EM_GETMODIFY, 0, 0),
            "rendering cannot restore the previous translation draft after reset");
    }
    Check(!IsWindowVisible(app->MainWindow()), "model edit regression stays hidden");
    std::puts("PASS camera-mode PMM toggle and real rotation-only XYZ reset");
}
