// Full-window regression: PMM camera mode must retain a loaded model target,
// and rotation-only bones must accept/reset manual translation through buttons.
#include "mikumikustudio/bone_parent_coordinates.hpp"
void CheckModelEditControls(mikumikustudio::MMDApp* app, const wchar_t* directory) {
    using namespace mikumikustudio;
    const auto root = std::filesystem::absolute(directory);
    std::filesystem::create_directories(root);
    const auto path = root / L"camera-mode.pmm";
    auto* combo = GetDlgItem(app->MainWindow(), panel::kMainComboModel);
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    ApplyModelComboSelection(app);
    Check(app->CameraMode() != 0, "fixture saves in camera mode");
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, path.c_str());
    SaveSceneFile(app);
    Check(std::filesystem::exists(path), "save real camera-mode PMM");
    app->MainModelComboSelection() = 0;
    LoadSceneFile();
    Check(app->CameraMode() != 0 && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0,
        "PMM retains camera mode on load");
    SendMessageW(GetDlgItem(app->MainWindow(), panel::kModelEditToggle), BM_CLICK, 0, 0);
    Check(app->CameraMode() == 0 && app->SelectedModel() != nullptr,
        "To model switches to loaded model without opening file picker");
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
        HWND edit = GetDlgItem(app->MainWindow(), panel::kReadoutPosXEdit + axis);
        char input[64]{};
        std::snprintf(input, sizeof(input), "%.6f", BoneParentBindOffset(model, bone, axis) + 2.5f);
        SetWindowTextA(edit, input);
        CommitEditControlTail(app, edit);
        Check(std::abs(record->boneTable[bone].trans[axis] - 2.5f) < .00001f,
            "real numeric control moves rotation-only bone");
        SendMessageW(GetDlgItem(app->MainWindow(), 537 + axis), BM_CLICK, 0, 0);
        Check(record->boneTable[bone].trans[axis] == 0,
            "real XYZ button resets rotation-only bone");
        app->state.messageSeen = 1;
        FrameDriver(app);
        Check(record->boneTable[bone].trans[axis] == 0,
            "rendering cannot restore the previous typed displacement");
    }
    Check(!IsWindowVisible(app->MainWindow()), "model edit regression stays hidden");
    std::puts("PASS camera-mode PMM toggle and real rotation-only XYZ reset");
}
