// Hidden production-window regression: real register buttons, PMM writer and
// clipboard commands must keep negative keyframes working end to end.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <MinHook.h>
#include <Windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
fs::path root;
void Check(bool good, const char* reason) {
    ++checks;
    if (!good) { std::printf("FAIL %s\n", reason); std::exit(1); }
}
int WINAPI MessageW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected message: %ls\n", text); std::exit(2);
}
int WINAPI MessageA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("FAIL unexpected message: %s\n", text); std::exit(2);
}
BOOL WINAPI NoBeep(UINT) { return TRUE; }
decltype(&ShowWindow) originalShow;
BOOL WINAPI Hide(HWND window, int) { return originalShow(window, SW_HIDE); }
LRESULT CALLBACK HideCreated(int code, WPARAM, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, 0, 0);
}
}

namespace mikumikustudio {
void CmdControl400(MMDApp*, HWND, std::uint16_t, std::uint16_t);
void StepFrame(MMDApp*, bool);
void RefreshAfterFrameApply(MMDApp*);
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(argc == 2, "isolated artifact directory required");
    root = fs::absolute(argv[1]);
    fs::create_directories(root);
    SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_AUTOSAVE_CONFIG", (root / L"autosave.ini").c_str());
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", (root / L"recent.txt").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / L"profile").c_str());
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "initialize hidden test guards");
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&MessageW), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(&MessageA), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBeep), reinterpret_cast<void*>(&NoBeep), nullptr);
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow), reinterpret_cast<void*>(&Hide),
        reinterpret_cast<void**>(&originalShow)) == MH_OK, "hide all test windows");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable background guards");
    auto hook = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Check(hook != nullptr, "prevent activation");
    auto* app = new MMDApp;
    g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production app");
    SetWindowPos(app->MainWindow(), nullptr, 0, 0, 1600, 800, SWP_NOACTIVATE | SWP_NOZORDER);
    app->EnglishUI() = 1;
    LoadModelFile(app, (root / L"fixture" / L"fixture.pmd").c_str(), false);
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneCount >= 1, "load real model fixture");
    app->CameraMode() = 0;
    auto* model = app->ModelSlot(0);
    auto* record = mdl::Mdl(model);
    const int bone = 0;
    std::memset(record->boneSelection, 0, record->boneCount);
    record->selectedBone = bone;
    record->boneSelection[bone] = 1;

    app->CurrentFrame() = 0;
    RefreshAfterFrameApply(app);
    std::array<float, 3> zeroPosition{};
    std::copy_n(record->boneTable[bone].trans, 3, zeroPosition.begin());
    record->boneTable[bone].trans[0] += 9;
    record->bonePhysicsState[bone] = 1;
    StepFrame(app, false);
    Check(app->CurrentFrame() == -1, "left at zero enters a signed negative frame");
    Check(std::abs(record->boneTable[bone].trans[0] - zeroPosition[0]) < 1e-5,
        "left at zero discards an unregistered bone displacement");
    StepFrame(app, true);
    Check(app->CurrentFrame() == 0, "right from negative returns to frame zero");

    app->CurrentFrame() = -20;
    RefreshAfterFrameApply(app);
    record->boneTable[bone].trans[0] = zeroPosition[0] + 4;
    RegisterSelectedBoneKeys(model, -20, 0);
    Check(negative_frames::Count(app) >= 1, "real register backend stores a negative model key");
    const auto registeredCount = negative_frames::Count(app);
    SendMessageW(GetDlgItem(app->MainWindow(), 537), BM_CLICK, 0, 0);
    Check(record->boneTable[bone].trans[0] == 0, "XYZ reset also works on a negative frame");
    CmdControl400(app, app->MainWindow(), 400, 0);
    Check(std::abs(record->boneTable[bone].trans[0] - (zeroPosition[0] + 4)) < 1e-4 &&
        negative_frames::Count(app) == registeredCount,
        "undo after negative-frame XYZ reset restores the pose without undoing the registered key");
    app->CurrentFrame() = -10;
    RefreshAfterFrameApply(app);
    Check(std::abs(record->boneTable[bone].trans[0] - (zeroPosition[0] + 2)) < 1e-4,
        "negative bone interpolation reaches the frame-zero pose");

    SetDlgItemTextA(app->MainWindow(), panel::kPlayStartFrameEdit, "0");
    SetDlgItemTextA(app->MainWindow(), panel::kPlayStopFrameEdit, "-1");
    const auto savedPath = root / L"negative-model-keys.pmm";
    wcscpy_s(app->EnvFileName(), kPathFieldCapacity, savedPath.c_str());
    SaveSceneFile(app);
    Check(fs::exists(savedPath), "save negative keys through the real PMM writer");
    LoadSceneFile();
    Check(app->CurrentFrame() == -10 && negative_frames::Count(app) >= 1,
        "real PMM reload restores the signed cursor and negative tracks");
    model = app->ModelSlot(0);
    record = mdl::Mdl(model);
    SeekModelFrame(model, -20, 0);
    Check(std::abs(record->boneTable[bone].trans[0] - (zeroPosition[0] + 4)) < 1e-4,
        "reloaded negative bone key evaluates exactly");
    char endpoint[32]{};
    GetDlgItemTextA(app->MainWindow(), panel::kPlayStopFrameEdit, endpoint, sizeof(endpoint));
    Check(std::string(endpoint) == "-1", "signed playback endpoint survives the extension roundtrip");

    SetDlgItemTextA(app->MainWindow(), panel::kRangeStartEdit, "-20");
    SetDlgItemTextA(app->MainWindow(), panel::kRangeStopEdit, "-20");
    CmdControl400(app, app->MainWindow(), 415, 0);
    Check(IsWindowEnabled(GetDlgItem(app->MainWindow(), 420)),
        "negative range selection enables the actual copy control");
    SendMessageW(GetDlgItem(app->MainWindow(), 420), BM_CLICK, 0, 0);
    const auto beforePaste = negative_frames::Count(app);
    Check(IsWindowEnabled(GetDlgItem(app->MainWindow(), 421)),
        "negative clipboard enables the actual paste control");
    app->CurrentFrame() = -40;
    SendMessageW(GetDlgItem(app->MainWindow(), 421), BM_CLICK, 0, 0);
    Check(negative_frames::Count(app) > beforePaste, "real range select, copy and paste retain negative keys");
    app->CurrentFrame() = -40;
    RefreshAfterFrameApply(app);
    Check(std::abs(record->boneTable[bone].trans[0] - (zeroPosition[0] + 4)) < 1e-4,
        "pasted negative key evaluates at the signed destination");
    app->CurrentFrame() = 5;
    SendMessageW(GetDlgItem(app->MainWindow(), 421), BM_CLICK, 0, 0);
    SeekModelFrame(model, 5, 0);
    Check(std::abs(record->boneTable[bone].trans[0] - (zeroPosition[0] + 4)) < 1e-4,
        "negative clipboard pastes into the native positive key pool");
    SetDlgItemTextA(app->MainWindow(), panel::kRangeStartEdit, "5");
    SetDlgItemTextA(app->MainWindow(), panel::kRangeStopEdit, "5");
    CmdControl400(app, app->MainWindow(), 415, 0);
    CmdControl400(app, app->MainWindow(), 420, 0);
    app->CurrentFrame() = -60;
    CmdControl400(app, app->MainWindow(), 421, 0);
    RefreshAfterFrameApply(app);
    Check(std::abs(record->boneTable[bone].trans[0] - (zeroPosition[0] + 4)) < 1e-4,
        "native positive clipboard pastes into the negative key store");
    app->PlaybackStartsAtCurrentFrame() = 1;
    CmdControl400(app, app->MainWindow(), 408, 0);
    Check(app->PlaybackStartSeconds() >= 0 && app->PlaybackEndSeconds() >= 0,
        "playback from a negative cursor never starts below zero");
    if (app->PlaybackActive()) CmdControl400(app, app->MainWindow(), 408, 0);
    Check(!IsWindowVisible(app->MainWindow()), "negative host regression stays hidden");
    std::printf("PASS negative frame host checks=%u\n", checks);
    ShutdownCleanup(app);
    g_Block = nullptr;
    delete app;
    UnhookWindowsHookEx(hook);
    return 0;
}
