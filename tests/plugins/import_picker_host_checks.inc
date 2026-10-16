// Include in sidebar_resize_tests.cpp after Hook and before edit_history_host_checks.
// Run only while the real host has its explicit common-history binding.
#include "../src/plugins/native_pose_scope.hpp"
namespace import_picker_host_checks {
std::wstring expectedInitial, selectedFile, navigatedDirectory;
unsigned pickerCalls = 0;
BOOL Pick(LPOPENFILENAMEW ofn) {
    ++pickerCalls;
    Check((ofn->Flags & (OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR)) ==
              (OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR),
          "controlled import picker requires existing files and preserves cwd");
    Check(ofn->lpstrInitialDir && expectedInitial == ofn->lpstrInitialDir &&
          std::filesystem::path(ofn->lpstrInitialDir).is_absolute(),
          "controlled import picker receives an absolute executable-relative initial directory");
    Check(SetCurrentDirectoryW(navigatedDirectory.c_str()) != FALSE,
          "picker stub simulates a common dialog changing the process directory");
    if (selectedFile.empty()) return FALSE;
    Check(selectedFile.size() < ofn->nMaxFile, "controlled picker failure fixture fits native path field");
    wcscpy_s(ofn->lpstrFile, ofn->nMaxFile, selectedFile.c_str()); return TRUE;
}
void Run(MMDApp* app) {
    auto* history = plugins::BoundNativePoseHistory(app);
    Check(history != nullptr, "picker probe requires explicit common-history binding");
    auto* model = mdl::Mdl(app->ModelSlot(0));
    Check(model && model->boneCount && model->boneTable, "picker probe has a real authored model pose");
    const auto main = app->MainWindow(); const auto menu = GetMenu(main);
    const auto previousMenu = GetMenuState(menu, 0x12D, MF_BYCOMMAND);
    Check(previousMenu != UINT(-1), "picker directory preference exists");
    const std::wstring modelDirectory(app->DirModel()), accessoryDirectory(app->DirAccs());
    wcscpy_s(app->DirModel(), kPathFieldCapacity, L"history-relative-models");
    wcscpy_s(app->DirAccs(), kPathFieldCapacity, L"history-relative-accessories");
    const auto oldCwd = std::filesystem::current_path();
    const auto temporaryDirectory = std::filesystem::temp_directory_path();
    Check(SetCurrentDirectoryW(temporaryDirectory.c_str()) != FALSE, "picker probe starts outside the executable directory");
    const auto isolatedCwd = std::filesystem::current_path();
    const auto navigation = isolatedCwd / (L"mms-picker-navigation-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    Check(std::filesystem::create_directory(navigation), "picker probe creates its isolated navigation directory");
    navigatedDirectory = navigation.native();
    const PathResolutionWorkspace workspace = app->PathWorkspace();
    const auto frame = app->CurrentFrame(); const auto dirty = app->SceneModified();
    const auto selectedModel = app->SelectedModelSlot(); const auto selectedAccessory = app->SelectedAccessorySlot();
    const auto camera = app->CameraMode(); const auto enter = app->state.enterKeyState;
    const auto undo = history->UndoCount(), redo = history->RedoCount();
    const float savedPose = model->boneTable[0].trans[0]; model->boneTable[0].trans[0] = 123.75f;
    Hook(&GetUiOpenFileNameW, &Pick);
    for (bool remembered : {false, true}) {
        CheckMenuItem(menu, 0x12D, MF_BYCOMMAND | (remembered ? MF_CHECKED : MF_UNCHECKED));
        for (unsigned command : {435u, 472u}) {
            const bool loadModel = command == 435;
            const wchar_t* relative = remembered ? (loadModel ? app->DirModel() : app->DirAccs()) :
                (loadModel ? L"UserFile\\Model" : L"UserFile\\Accessory");
            expectedInitial = (std::filesystem::path(app->ExeDir()) / relative).lexically_normal().native();
            for (bool acceptMissing : {false, true}) {
                selectedFile = acceptMissing ? (std::filesystem::path(app->ExeDir()) /
                    (loadModel ? L"__missing_history_picker__.pmx" : L"__missing_history_picker__.x")).native() : L"";
                Check(selectedFile.empty() || GetFileAttributesW(selectedFile.c_str()) == INVALID_FILE_ATTRIBUTES,
                      "picker failed-import input is absent");
                const auto before = pickerCalls;
                SendMessageW(main, WM_COMMAND, command, 0);
                Check(pickerCalls == before + 1, "real 435/472 command invokes the controlled picker");
                Check(std::filesystem::current_path() == isolatedCwd &&
                      std::memcmp(&workspace, &app->PathWorkspace(), sizeof workspace) == 0 &&
                      !wcscmp(app->DirModel(), L"history-relative-models") &&
                      !wcscmp(app->DirAccs(), L"history-relative-accessories"),
                      "picker cancellation and failed import preserve cwd workspace and remembered directories");
                Check(app->CurrentFrame() == frame && app->SceneModified() == dirty &&
                      app->SelectedModelSlot() == selectedModel && app->SelectedAccessorySlot() == selectedAccessory &&
                      app->CameraMode() == camera && app->state.enterKeyState == enter &&
                      model->boneTable[0].trans[0] == 123.75f &&
                      history->UndoCount() == undo && history->RedoCount() == redo,
                      "picker cancellation and failed import do not seek dirty select or change shared history");
            }
        }
    }
    MH_DisableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));
    MH_RemoveHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));
    model->boneTable[0].trans[0] = savedPose;
    wcscpy_s(app->DirModel(), kPathFieldCapacity, modelDirectory.c_str());
    wcscpy_s(app->DirAccs(), kPathFieldCapacity, accessoryDirectory.c_str());
    CheckMenuItem(menu, 0x12D, MF_BYCOMMAND | (previousMenu & MF_CHECKED));
    Check(SetCurrentDirectoryW(oldCwd.c_str()) != FALSE, "picker probe restores its original directory");
    Check(std::filesystem::remove(navigation), "picker probe removes its empty navigation directory");
    std::puts("PASS actual 435/472 controlled pickers preserve pose paths and history on cancel/failure");
}
}
