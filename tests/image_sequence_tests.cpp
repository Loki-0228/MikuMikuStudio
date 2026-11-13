// Real dialog, Direct3D readback, image encoders and recording lifecycle.
// Run in a disposable directory. --hidden-production allows GPU testing on
// the input desktop with visibility/activation blocked for all test windows.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/default_effect.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/antialias_motion.hpp"
#include "mikumikustudio/transparent_capture.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_layout.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/ui_scale.hpp"
#include "ui_scale_popup_fixture.hpp"
#include "export_save_workflow_checks.hpp"
#include "../src/window/image_sequence_dialog.hpp"
#include "../src/window/still_image_picker.hpp"
#include <MinHook.h>
#include "plugins/coordinator_fixture.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" float ExpGetFrameTime();

using namespace mikumikustudio;
namespace seq = mikumikustudio::image_sequence;
namespace fs = std::filesystem;
namespace sequence_lifecycle {
bool AcceptWriteError(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags);
bool AcceptSaveFailure(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags);
}
namespace {
unsigned checks = 0, warnings = 0;
bool expectWarning = false;
bool expectStillDestinationChanged = false;
std::wstring lastStillWarning;
bool dialogOnly = false;
fs::path output;
// Command 276 rejects a path in two ways the generic warning fixture must not
// absorb: a controlled picker failure and a chosen display name longer than
// the production path field. Both stay inactive outside their own command call.
enum class StillPickerFailure { None, ShowError, LongResult };
StillPickerFailure stillPickerFailure = StillPickerFailure::None;
unsigned stillWarnings = 0;
bool stillCommandActive = false;
DWORD stillCommandThread = 0;
IFileSaveDialog* stillCommandPicker = nullptr;
const wchar_t* StillPathWarningText() {
    const unsigned char language = g_Block ? g_Block->EnglishUI() : 2;
    return language == 2 ? L"无法选择图片保存路径。请重试，或选择较短的路径。" :
        language == 1 ? L"Could not select the image output path. Please retry or choose a shorter path." :
        L"画像の保存先を選択できません。再試行するか、短いパスを選択してください。";
}
void Check(bool value, const char* reason) {
    ++checks;
    if (!value) { std::printf("FAIL %s\n", reason); std::exit(1); }
}
int WINAPI MessageW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    if (sequence_lifecycle::AcceptSaveFailure(owner, text, title, flags)) return IDOK;
    if (sequence_lifecycle::AcceptWriteError(owner, text, title, flags)) return IDOK;
    if (stillPickerFailure != StillPickerFailure::None) {
        const bool matched = stillCommandActive && GetCurrentThreadId() == stillCommandThread &&
            stillCommandPicker != nullptr && g_Block && owner == g_Block->MainWindow() && title && text &&
            std::wcscmp(title, L"MMS") == 0 && flags == (MB_OK | MB_ICONWARNING) &&
            std::wcscmp(text, StillPathWarningText()) == 0;
        if (matched) { ++stillWarnings; return IDOK; }
        std::fwprintf(stderr, L"FAIL unexpected rejected-path message: title=%ls text=%ls flags=0x%x\n",
            title ? title : L"", text ? text : L"", flags);
        std::exit(2);
    }
    if (expectWarning) {
        lastStillWarning = text ? text : L"";
        if (expectStillDestinationChanged)
            Check(text && std::wcscmp(text, L"保存位置的文件在导出期间发生了变化。请重新选择保存位置。") == 0,
                  "report the destination change and tell the user how to retry");
        ++warnings;
        return IDOK;
    }
    std::fwprintf(stderr, L"FAIL unexpected message: %ls\n", text); std::exit(2);
}
int WINAPI MessageA(HWND, LPCSTR text, LPCSTR, UINT) {
    if (expectWarning) { ++warnings; return IDOK; }
    std::printf("FAIL unexpected message: %s\n", text); std::exit(2);
}
decltype(&ShowWindow) originalShowWindow;
BOOL WINAPI Hidden(HWND window, int) { return originalShowWindow(window, SW_HIDE); }
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
std::wstring Read(HWND dialog, int id) {
    wchar_t text[512]{}; GetDlgItemTextW(dialog, id, text, 512); return text;
}
void Capture(HWND dialog, const fs::path& path) {
    if (dialogOnly) {
        originalShowWindow(dialog, SW_SHOWNOACTIVATE);
        UpdateWindow(dialog);
    }
    RECT rect{}; GetWindowRect(dialog, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetWindowDC(dialog), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(memory, bitmap);
    SendMessageW(dialog, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + width * height * 4;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    stream.write(static_cast<char*>(pixels), width * height * 4);
    Check(bool(stream), "save dialog screenshot");
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(dialog, dc);
}
seq::DialogContext* dialogContext = nullptr;
bool cancelDialog = false;
INT_PTR CALLBACK Probe(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message != WM_INITDIALOG) return seq::DialogProc(dialog, message, wp, lp);
    seq::DialogProc(dialog, message, wp, lp);
    auto& c = *dialogContext;
    Check(Read(dialog, panel::kAviWidthEdit) == L"1920" && Read(dialog, panel::kAviHeightEdit) == L"1080", "dialog dimensions initialized once");
    Check(SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETCOUNT, 0, 0) == 7, "seven single-image formats");
    wchar_t selected[32]{};
    const auto selection = SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETCURSEL, 0, 0);
    SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(selected));
    Check(wcscmp(selected, L"PNG") == 0, "PNG is the displayed default format");
    Check(Read(dialog, seq::Example).find(L"frame_0001.png") != std::wstring::npos, "500-frame filename uses four digits");
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, 499, FALSE);
    Check(Read(dialog, seq::Example).find(L"frame_001.png") != std::wstring::npos, "preview shrinks below leading-five threshold");
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, 500, FALSE);
    SetDlgItemTextW(dialog, seq::Prefix, L"镜头A_");
    Check(Read(dialog, seq::Example).find(L"镜头A_0001.png") != std::wstring::npos, "Unicode prefix preview");
    Capture(dialog, output / (L"dialog-" + std::to_wstring(c.app->EnglishUI()) + L".bmp"));
    if (cancelDialog) {
        seq::DialogProc(dialog, WM_COMMAND, IDCANCEL, 0);
        return TRUE;
    }
    SetDlgItemInt(dialog, panel::kAviWidthEdit, 640, FALSE);
    Check(Read(dialog, panel::kAviHeightEdit) == L"360", "size editing preserves video aspect ratio");
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"nan");
    expectWarning = true; seq::DialogProc(dialog, WM_COMMAND, IDOK, 0); expectWarning = false;
    Check(!seq::Active(c.app), "invalid FPS does not arm export");
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"29.97");
    CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, BST_CHECKED);
    seq::DialogProc(dialog, WM_COMMAND, IDOK, 0);
    return TRUE;
}
void DialogTests(MMDApp* app) {
    for (int language = 0; language < 3; ++language) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        for (bool cancel : {false, true}) {
            seq::DialogContext c{app, app->SceneGeneration()}; c.options.directory = output.wstring();
            c.options.width = 1920; c.options.height = 1080; c.options.end = 500;
            dialogContext = &c; cancelDialog = cancel;
            const auto result = UiDialogBoxParamA(GetModuleHandleW(nullptr), MAKEINTRESOURCEA(seq::kDialog),
                app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&c));
            Check(result == (cancel ? IDCANCEL : IDOK), "real dialog completes through production callback");
            Check(seq::Active(app) == !cancel, "cancel does not prepare export");
            if (!cancel) Check(c.options.width == 640 && c.options.height == 360 &&
                c.options.prefix == L"镜头A_" && c.options.fps == 29.97f && c.options.closeAfterExport,
                "dialog commits all displayed settings");
            seq::Abort(app);
        }
        seq::RefreshMenu(app); seq::RefreshMenu(app);
        HMENU file = GetSubMenu(GetMenu(app->MainWindow()), 0);
        unsigned count = 0;
        for (int i = 0; i < GetMenuItemCount(file); ++i) count += GetMenuItemID(file, i) == seq::kMenuCommand;
        Check(count == 1, "menu survives language refresh without duplication");
        wchar_t menuText[128]{};
        GetMenuStringW(file, seq::kMenuCommand, menuText, 128, MF_BYCOMMAND);
        if (language == 2) Check(std::wstring(menuText) == L"导出图片序列(&I)…", "Chinese sequence export naming is consistent");
    }
    Check(warnings == 3, "all three languages reject invalid FPS");
}
void NamesAndValidation(MMDApp* app) {
    seq::Options o; o.directory = output.wstring(); o.width = 64; o.height = 48;
    const std::pair<int, const wchar_t*> cases[] = {
        {0,L"frame_1.png"}, {4,L"frame_1.png"}, {5,L"frame_01.png"},
        {49,L"frame_01.png"}, {50,L"frame_001.png"},
        {499,L"frame_001.png"}, {500,L"frame_0001.png"},
        {1100,L"frame_0001.png"}, {4999,L"frame_0001.png"},
        {5000,L"frame_00001.png"}, {10000,L"frame_00001.png"},
    };
    for (const auto& item : cases) {
        o.end = item.first;
        Check(fs::path(seq::FramePath(o, 1)).filename() == item.second, "leading-digit rounding boundary");
    }
    o.end = 499; o.fps = 240;
    Check(fs::path(seq::FramePath(o, 1)).filename() == L"frame_0001.png", "high FPS has enough numbering digits");
    std::wstring error;
    for (const auto* prefix : {L"../bad", L"bad\\name", L"bad:", L"bad*", L"trailing.", L"trailing "}) {
        o.prefix = prefix; Check(!seq::Validate(app, o, error) && !error.empty(), "invalid filename rejected");
    }
    o.prefix = L""; Check(seq::Validate(app, o, error), "empty prefix permitted");
    o.fps = 0; Check(!seq::Validate(app, o, error), "zero FPS rejected");
    o.fps = 30; o.start = 600; Check(!seq::Validate(app, o, error), "reversed range rejected");
}
void Start(MMDApp* app, const seq::Options& o) {
    std::wstring error;
    if (!seq::Prepare(app, o, error)) std::fwprintf(stderr, L"%ls\n", error.c_str());
    Check(seq::Active(app), "prepare real export");
    app->AviOutputWidth() = o.width; app->AviOutputHeight() = o.height;
    app->AviRecordStartFrame() = o.start; app->AviRecordEndFrame() = o.end;
    app->AviRecordFps() = o.fps; app->AviIncludeWave() = 0; app->AviStereoOutput() = 0;
    app->AviCloseAfterExport() = false;
    StartAviRecordWindow(app);
    Check(app->RecordingWindow() && app->state.recordPlaybackStartPending, "start through video recording entry");
}
void RunExport(MMDApp* app, seq::Options o, bool cancel = false) {
    const int oldW = app->RenderWidth(), oldH = app->RenderHeight();
    const int oldScreenW = app->Renderer()->screenWidth, oldScreenH = app->Renderer()->screenHeight;
    Start(app, o);
    const auto firstFrame=coordinator_fixture::frames, firstPoll=coordinator_fixture::polls;
    const auto started=GetTickCount64();
    unsigned steps = 0;
    while (seq::Active(app) && steps++ < 150) {
        app->state.messageSeen = 1; coordinator_fixture::Drive(app);
        if (cancel && steps == 5) FinishAviRecord(app);
    }
    std::printf("TRACE export fps=%.3f steps=%u frames=%llu polls=%llu elapsed_ms=%llu active=%d recorded=%u\n",
        o.fps,steps,static_cast<unsigned long long>(coordinator_fixture::frames-firstFrame),
        static_cast<unsigned long long>(coordinator_fixture::polls-firstPoll),
        static_cast<unsigned long long>(GetTickCount64()-started),seq::Active(app),app->state.recordedFrameCount);
    Check(!seq::Active(app) && !app->RecordingWindow() && !app->FrameStepPlayback(), "export releases recording state");
    Check(app->RenderWidth() == oldW && app->RenderHeight() == oldH, "render resolution restored");
    Check(app->Renderer()->screenWidth == oldScreenW && app->Renderer()->screenHeight == oldScreenH, "device preview resolution restored");
    std::uint64_t files = 0;
    for (const auto& file : fs::directory_iterator(o.directory)) {
        if (file.path().filename().wstring().find(o.prefix) == 0 && file.path().extension() == seq::kFormats[o.format].extension) ++files;
    }
    Check(cancel ? files > 0 && files < seq::FrameCount(o) : files == seq::FrameCount(o), "inclusive range and output FPS produce correct image count");
    for (std::uint64_t number = 1; number <= files; ++number) Check(fs::file_size(seq::FramePath(o, number)) > 0, "complete consecutive images");
    std::printf("PASS format=%ls fps=%.3f range=%d..%d images=%llu cancel=%d\n",
        seq::kFormats[o.format].name, o.fps, o.start, o.end, static_cast<unsigned long long>(files), cancel);
}
unsigned folderPickers = 0, settingsDialogs = 0;
bool cancelFolder = false, cancelSettings = false;
enum class SequencePickerAction { None, Reenter, ReplaceScene, ReplaceInSettings, ReplaceInBrowse };
SequencePickerAction sequencePickerAction = SequencePickerAction::None;
unsigned nestedVideoPickers = 0;
std::wstring menuPrefix;
std::vector<unsigned char> ImagePixels(const fs::path&, UINT*, UINT*);
BOOL WINAPI RejectNestedVideoPicker(OPENFILENAMEW*) {
    ++nestedVideoPickers;
    return FALSE;
}
HRESULT STDMETHODCALLTYPE ChooseFolder(IFileOpenDialog* picker, HWND owner) {
    ++folderPickers;
    const bool browsing = sequencePickerAction == SequencePickerAction::ReplaceInBrowse && folderPickers == 2;
    Check(browsing ? GetWindow(owner, GW_OWNER) == g_Block->MainWindow() : owner == g_Block->MainWindow(),
          "folder picker belongs to its menu or settings window");
    FILEOPENDIALOGOPTIONS options{};
    Check(SUCCEEDED(picker->GetOptions(&options)) && (options & FOS_PICKFOLDERS), "menu requests a folder picker");
    if (sequencePickerAction == SequencePickerAction::Reenter) {
        sequencePickerAction = SequencePickerAction::None;
        const auto outerPickers = folderPickers;
        SendMessageW(owner, WM_COMMAND, seq::kMenuCommand, 0);
        Check(folderPickers == outerPickers, "nested sequence command cannot open another folder picker");
        SendMessageW(owner, WM_COMMAND, 223, 0);
        Check(nestedVideoPickers == 0, "nested video command cannot open a competing file picker");
    } else if (sequencePickerAction == SequencePickerAction::ReplaceScene || browsing) {
        Check(ResetAppState(g_Block), "replace the scene through the production reset in the folder picker");
    }
    return cancelFolder ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK;
}
HRESULT STDMETHODCALLTYPE ChosenFolder(IFileOpenDialog*, IShellItem** item) {
    return SHCreateItemFromParsingName(output.c_str(), nullptr, IID_PPV_ARGS(item));
}
INT_PTR CALLBACK MenuSettingsProbe(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message != WM_INITDIALOG) return seq::DialogProc(dialog, message, wp, lp);
    seq::DialogProc(dialog, message, wp, lp);
    Check(Read(dialog, panel::kAviEndFrameEdit) == std::to_wstring(seq::SceneEndFrame(g_Block)),
        "menu initializes the automatically detected scene range");
    if (cancelSettings) { seq::DialogProc(dialog, WM_COMMAND, IDCANCEL, 0); return TRUE; }
    auto* context = reinterpret_cast<seq::DialogContext*>(lp);
    context->updating = true;
    SetDlgItemInt(dialog, panel::kAviWidthEdit, 64, FALSE);
    SetDlgItemInt(dialog, panel::kAviHeightEdit, 48, FALSE);
    SetDlgItemInt(dialog, panel::kAviStartFrameEdit, 0, FALSE);
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, 1, FALSE);
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"30");
    SetDlgItemTextW(dialog, seq::Prefix, menuPrefix.c_str());
    CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, BST_UNCHECKED);
    context->updating = false;
    if (sequencePickerAction == SequencePickerAction::ReplaceInBrowse) {
        seq::DialogProc(dialog, WM_COMMAND, seq::Browse, 0);
        return TRUE;
    }
    if (sequencePickerAction == SequencePickerAction::ReplaceInSettings)
        Check(ResetAppState(g_Block), "replace the scene through the production reset in settings");
    seq::DialogProc(dialog, WM_COMMAND, IDOK, 0);
    return TRUE;
}
decltype(&UiDialogBoxParamA) originalMenuDialog = nullptr;
INT_PTR MenuDialog(HINSTANCE instance, LPCSTR name, HWND owner, DLGPROC proc, LPARAM parameter) {
    if (proc == seq::DialogProc) { ++settingsDialogs; proc = MenuSettingsProbe; }
    return originalMenuDialog(instance, name, owner, proc, parameter);
}
void MenuEntryTests(MMDApp* app, bool sceneBoundaryOnly = false) {
    wchar_t modelPath[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_MENU_SEQUENCE_MODEL", modelPath, std::size(modelPath));
    if (length && !sceneBoundaryOnly) {
        Check(length < std::size(modelPath) && fs::is_regular_file(modelPath), "menu export model fixture exists");
        LoadModelFile(app, modelPath, false);
        Check(app->SelectedModel() != nullptr, "load menu export model fixture");
    }
    // Exercise the real WM_COMMAND entry and shell setup; only replace the
    // user's folder selection and dialog input so the host stays hidden.
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    Check(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&picker))), "create shell picker for menu regression");
    auto** table = *reinterpret_cast<void***>(picker.Get());
    void* show = table[3]; void* result = table[20];
    Check(MH_CreateHook(show, &ChooseFolder, nullptr) == MH_OK &&
        MH_CreateHook(result, &ChosenFolder, nullptr) == MH_OK &&
        MH_CreateHook(reinterpret_cast<void*>(&UiDialogBoxParamA), &MenuDialog,
            reinterpret_cast<void**>(&originalMenuDialog)) == MH_OK, "hook user input for menu export");
    MH_EnableHook(show); MH_EnableHook(result); MH_EnableHook(reinterpret_cast<void*>(&UiDialogBoxParamA));
    const auto camera = app->CameraMode();
    if (sceneBoundaryOnly) {
        app->CameraMode() = 1;
        auto* videoPicker = reinterpret_cast<void*>(&GetSaveFileNameW);
        Check(MH_CreateHook(videoPicker, &RejectNestedVideoPicker, nullptr) == MH_OK &&
              MH_EnableHook(videoPicker) == MH_OK, "observe any competing video file picker");
        for (const auto action : {SequencePickerAction::ReplaceScene, SequencePickerAction::ReplaceInSettings,
                                  SequencePickerAction::ReplaceInBrowse}) {
            sequencePickerAction = action;
            menuPrefix = L"stale_sequence_";
            folderPickers = settingsDialogs = 0;
            const auto oldGeneration = app->SceneGeneration();
            const auto originalStart = app->AviRecordStartFrame();
            const auto originalWidth = app->AviOutputWidth();
            SendMessageW(app->MainWindow(), WM_COMMAND, seq::kMenuCommand, 0);
            Check(app->SceneGeneration() != oldGeneration &&
                folderPickers == (action == SequencePickerAction::ReplaceInBrowse ? 2u : 1u) &&
                settingsDialogs == unsigned(action != SequencePickerAction::ReplaceScene),
                "a scene switch exits the appropriate modal boundary");
            Check(!seq::Active(app) && !app->RecordingWindow() && !app->state.recordPlaybackStartPending &&
                app->AviRecordStartFrame() == originalStart && app->AviOutputWidth() == originalWidth &&
                !fs::exists(output / L"stale_sequence_1.png"),
                "stale settings cannot export or change the replacement project");
        }
        sequencePickerAction = SequencePickerAction::Reenter;
    }
    for (int mode : {0, 1}) {
        if (!mode && !app->SelectedModel()) { std::puts("SKIP model-mode menu export: set MMS_MENU_SEQUENCE_MODEL"); continue; }
        app->CameraMode() = mode;
        menuPrefix = L"menu_camera_" + std::to_wstring(mode) + L"_";
        folderPickers = settingsDialogs = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, seq::kMenuCommand, 0);
        std::printf("MENU camera=%d folder_pickers=%u settings=%u recording=%d\n",
            mode, folderPickers, settingsDialogs, seq::Active(app));
        Check(folderPickers == 1 && settingsDialogs == 1 && seq::Active(app) && app->RecordingWindow(),
            "image sequence menu starts export in model and camera/accessory modes");
        unsigned steps = 0;
        while (seq::Active(app) && steps++ < 150) { app->state.messageSeen = 1; coordinator_fixture::Drive(app); }
        Check(!seq::Active(app) && !app->RecordingWindow(), "menu export finishes recording");
        for (int frame : {1, 2}) {
            const auto path = output / (menuPrefix + std::to_wstring(frame) + L".png");
            Check(fs::file_size(path) > 0, "menu export writes consecutive PNG images");
            if (sceneBoundaryOnly) {
                UINT width = 0, height = 0;
                const auto pixels = ImagePixels(path, &width, &height);
                Check(width == 64 && height == 48 && pixels.size() == 64 * 48 * 4,
                    "retry produces a complete decodable image at the requested dimensions");
            }
        }
        for (bool folder : {true, false}) {
            folderPickers = settingsDialogs = 0; cancelFolder = folder; cancelSettings = !folder;
            SendMessageW(app->MainWindow(), WM_COMMAND, seq::kMenuCommand, 0);
            Check(folderPickers == 1 && settingsDialogs == unsigned(!folder) &&
                !seq::Active(app) && !app->RecordingWindow(), "canceling either menu dialog leaves export inactive");
        }
        cancelFolder = cancelSettings = false;
        folderPickers = settingsDialogs = 0;
        const auto playback = app->PlaybackActive(); app->PlaybackActive() = 1;
        SendMessageW(app->MainWindow(), WM_COMMAND, seq::kMenuCommand, 0);
        app->PlaybackActive() = playback;
        Check(folderPickers == 0 && settingsDialogs == 0, "playback still blocks menu export");
    }
    app->CameraMode() = camera;
    for (void* hook : {show, result, reinterpret_cast<void*>(&UiDialogBoxParamA)}) {
        MH_DisableHook(hook); MH_RemoveHook(hook);
    }
    if (length && !sceneBoundaryOnly) ResetAppState(app);
    if (sceneBoundaryOnly) {
        auto* videoPicker = reinterpret_cast<void*>(&GetSaveFileNameW);
        MH_DisableHook(videoPicker);
        MH_RemoveHook(videoPicker);
        std::puts("PASS image sequence modal scene boundary and retry");
    }
}
fs::path stillPath;
bool cancelStill = false;
bool reenterStillOnShow = false;
bool resetStillSceneOnShow = false;
unsigned stillPickers = 0;
unsigned stillSetup = 0;
UINT selectedStillType = 1;
HRESULT (STDMETHODCALLTYPE* originalStillTypes)(IFileSaveDialog*, UINT, const COMDLG_FILTERSPEC*);
HRESULT (STDMETHODCALLTYPE* originalStillExtension)(IFileSaveDialog*, LPCWSTR);
HRESULT (STDMETHODCALLTYPE* originalStillTitle)(IFileSaveDialog*, LPCWSTR);
HRESULT (STDMETHODCALLTYPE* originalStillShow)(IFileSaveDialog*, HWND) = nullptr;
HRESULT (STDMETHODCALLTYPE* originalStillResult)(IFileSaveDialog*, IShellItem**) = nullptr;
// The production command creates its own picker object; only that instance may
// be rejected, and only on the thread that dispatched the controlled command.
// Every other call keeps the original shell behavior.
IFileSaveDialog* stillTemplatePicker = nullptr;
void NoteStillCommandPicker(IFileSaveDialog* picker) {
    if (!stillCommandActive || GetCurrentThreadId() != stillCommandThread) return;
    if (stillCommandPicker == nullptr) { stillCommandPicker = picker; return; }
    Check(stillCommandPicker == picker, "one production picker instance serves the controlled command");
}
bool IsStillCommandPicker(IFileSaveDialog* picker) {
    return stillCommandActive && GetCurrentThreadId() == stillCommandThread && picker == stillCommandPicker;
}
HRESULT STDMETHODCALLTYPE StillTypes(IFileSaveDialog* picker, UINT count, const COMDLG_FILTERSPEC* types) {
    NoteStillCommandPicker(picker);
    const wchar_t* expected[] = {L"*.png", L"*.jpg;*.jpeg", L"*.bmp", L"*.dds", L"*.dib", L"*.pfm", L"*.hdr"};
    Check(count == std::size(expected), "modern still-image picker retains all seven image types");
    for (UINT i = 0; i < count; ++i)
        Check(std::wcscmp(types[i].pszSpec, expected[i]) == 0, "image filter order and JPEG aliases are retained");
    stillSetup |= 1;
    return originalStillTypes(picker, count, types);
}
HRESULT STDMETHODCALLTYPE StillExtension(IFileSaveDialog* picker, LPCWSTR extension) {
    NoteStillCommandPicker(picker);
    Check(std::wcscmp(extension, L"png") == 0, "default extension enables shell file-type extension switching");
    stillSetup |= 2;
    return originalStillExtension(picker, extension);
}
HRESULT STDMETHODCALLTYPE StillTitle(IFileSaveDialog* picker, LPCWSTR title) {
    NoteStillCommandPicker(picker);
    const wchar_t* expected = g_Block->EnglishUI() == 2 ? L"导出图片" :
        g_Block->EnglishUI() == 1 ? L"render to picture file" : L"画像ファイル出力";
    Check(std::wcscmp(title, expected) == 0, "still-image save dialog title follows the UI language");
    stillSetup |= 4;
    return originalStillTitle(picker, title);
}
// Rejection-path bookkeeping. The display-name replacement is installed on the
// exact shell item the production code is about to examine, so other COM items
// keep their ordinary names and later probes start from a clean slate.
unsigned stillShowFailureCalls = 0;
unsigned stillLongItemsCreated = 0;
unsigned stillLongDisplayCalls = 0;
std::wstring stillLongPath;
IShellItem* stillLongItem = nullptr;
HRESULT (STDMETHODCALLTYPE* originalStillDisplayName)(IShellItem*, SIGDN, PWSTR*) = nullptr;
HRESULT STDMETHODCALLTYPE LongStillDisplayName(IShellItem* item, SIGDN form, PWSTR* name) {
    if (stillPickerFailure == StillPickerFailure::LongResult && item == stillLongItem &&
        stillCommandActive && GetCurrentThreadId() == stillCommandThread &&
        form == SIGDN_FILESYSPATH && name != nullptr) {
        ++stillLongDisplayCalls;
        const std::size_t bytes = (stillLongPath.size() + 1) * sizeof(wchar_t);
        auto* value = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
        if (value == nullptr) return E_OUTOFMEMORY;
        std::memcpy(value, stillLongPath.c_str(), bytes);
        *name = value;
        return S_OK;
    }
    return originalStillDisplayName(item, form, name);
}
HRESULT STDMETHODCALLTYPE StillSaveDialog(IFileSaveDialog* picker, HWND owner) {
    if (stillPickerFailure != StillPickerFailure::None && !IsStillCommandPicker(picker))
        return originalStillShow(picker, owner);
    ++stillPickers;
    Check(owner == g_Block->MainWindow() && stillSetup == 7, "modern save picker is fully configured before Show");
    stillSetup = 0;
    DWORD options = 0;
    const DWORD required = FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    Check(SUCCEEDED(picker->GetOptions(&options)) && (options & required) == required,
        "modern save picker preserves overwrite confirmation and filesystem paths");
    UINT type = 0;
    Check(SUCCEEDED(picker->GetFileTypeIndex(&type)) && type == 1, "still-image picker starts with PNG selected");
    if (cancelStill) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (reenterStillOnShow) {
        // This Show has consumed stillSetup. A nested picker can run its own setup
        // and must cancel before it touches the outer picker's result fixture.
        reenterStillOnShow = false;
        cancelStill = true;
        SendMessageW(owner, WM_COMMAND, 276, 0);
        cancelStill = false;
    }
    Check(SUCCEEDED(picker->SetFileTypeIndex(selectedStillType)) &&
        SUCCEEDED(picker->GetFileTypeIndex(&type)) && type == selectedStillType,
        "modern save picker accepts a different image type");
    if (resetStillSceneOnShow) {
        resetStillSceneOnShow = false;
        Check(ResetAppState(g_Block), "replace the scene while the still picker is active");
    }
    if (stillPickerFailure == StillPickerFailure::ShowError) {
        Check(IsStillCommandPicker(picker), "the rejected Show belongs to the bound production picker");
        ++stillShowFailureCalls;
        return E_FAIL;  // explicit non-cancel failure; the command must stay retryable
    }
    if (stillPickerFailure == StillPickerFailure::LongResult) return S_OK;
    // Only replace user input; the production code receives a real filesystem shell item.
    std::ofstream placeholder(stillPath, std::ios::binary);
    Check(bool(placeholder), "create shell result fixture");
    return S_OK;
}
HRESULT STDMETHODCALLTYPE StillSaveResult(IFileSaveDialog* picker, IShellItem** item) {
    if (stillPickerFailure == StillPickerFailure::LongResult) {
        if (!IsStillCommandPicker(picker)) return originalStillResult(picker, item);
        // A real shell item for the private output directory, with only its
        // filesystem display name replaced. The production code still performs
        // GetResult/GetDisplayName and its own capacity check.
        const HRESULT created = SHCreateItemFromParsingName(output.c_str(), nullptr, IID_PPV_ARGS(item));
        if (SUCCEEDED(created)) {
            ++stillLongItemsCreated;
            stillLongItem = *item;
        }
        return created;
    }
    const HRESULT hr = SHCreateItemFromParsingName(stillPath.c_str(), nullptr, IID_PPV_ARGS(item));
    Check(fs::remove(stillPath), "remove shell placeholder before the production capture starts");
    return hr;
}
struct StillSaveDialogProbe {
    Microsoft::WRL::ComPtr<IFileSaveDialog> picker;
    Microsoft::WRL::ComPtr<IShellItem> shellItem;
    void* hooks[6]{};
    StillSaveDialogProbe() {
        cancelStill = false; stillSetup = 0; selectedStillType = 1;
        stillPickerFailure = StillPickerFailure::None; stillWarnings = 0;
        stillShowFailureCalls = stillLongItemsCreated = stillLongDisplayCalls = 0;
        stillLongPath.clear(); stillLongItem = nullptr;
        stillCommandActive = false; stillCommandThread = 0; stillCommandPicker = nullptr;
        Check(SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&picker))), "create modern save picker for still-image regression");
        stillTemplatePicker = picker.Get();
        // A real filesystem item supplies the class whose display name is
        // replaced; the rejection path creates its item the same way.
        Check(SUCCEEDED(SHCreateItemFromParsingName(output.c_str(), nullptr, IID_PPV_ARGS(&shellItem))),
            "create filesystem shell item for the rejected result path");
        auto** table = *reinterpret_cast<void***>(picker.Get());
        auto** itemTable = *reinterpret_cast<void***>(shellItem.Get());
        void* show = table[3]; void* types = table[4]; void* title = table[17];
        void* result = table[20]; void* extension = table[22];
        void* display = itemTable[5];  // IShellItem::GetDisplayName
        Check(MH_CreateHook(show, &StillSaveDialog, reinterpret_cast<void**>(&originalStillShow)) == MH_OK &&
            MH_CreateHook(result, &StillSaveResult, reinterpret_cast<void**>(&originalStillResult)) == MH_OK &&
            MH_CreateHook(types, &StillTypes, reinterpret_cast<void**>(&originalStillTypes)) == MH_OK &&
            MH_CreateHook(title, &StillTitle, reinterpret_cast<void**>(&originalStillTitle)) == MH_OK &&
            MH_CreateHook(extension, &StillExtension, reinterpret_cast<void**>(&originalStillExtension)) == MH_OK &&
            MH_CreateHook(display, &LongStillDisplayName,
                reinterpret_cast<void**>(&originalStillDisplayName)) == MH_OK,
            "control modern still-image save selection");
        hooks[0] = show; hooks[1] = result; hooks[2] = types; hooks[3] = title;
        hooks[4] = extension; hooks[5] = display;
        for (void* hook : hooks)
            Check(MH_EnableHook(hook) == MH_OK, "enable modern still-image dialog probe");
    }
    ~StillSaveDialogProbe() {
        stillPickerFailure = StillPickerFailure::None;
        stillLongItem = nullptr; stillCommandActive = false;
        stillCommandThread = 0; stillCommandPicker = nullptr;
        if (stillTemplatePicker == picker.Get()) stillTemplatePicker = nullptr;
        cancelStill = false; stillSetup = 0;
        for (void* hook : hooks) { MH_DisableHook(hook); MH_RemoveHook(hook); }
    }
};
std::vector<std::wstring> StillOutputEntries() {
    std::vector<std::wstring> entries;
    for (const auto& entry : fs::directory_iterator(output)) entries.push_back(entry.path().filename().wstring());
    std::sort(entries.begin(), entries.end());
    return entries;
}
std::wstring LongStillResultPath(unsigned language) {
    // Exactly 256 wchar without the terminator: the production field rejects
    // >= 256, so this also distinguishes an accidental > 256 change.
    const std::wstring prefix = output.wstring() + L"\\returned_still_" + std::to_wstring(language) + L"_";
    Check(prefix.size() <= 252, "the rejected result path prefix leaves room for its exact length");
    std::wstring path = prefix;
    while (path.size() < 252) path.push_back(L'x');
    path += L".png";
    Check(path.size() == 0x100, "the rejected result path is exactly 256 wchar without a terminator");
    return path;
}
// The remembered-directory fields are fixed-width arrays: state.dirUser is 1000
// wchar and the capture path field is 256 wchar. Snapshot and compare the whole
// field so a write beyond the terminator is detected too.
constexpr std::size_t kStillDirUserField = 1000;
constexpr std::size_t kStillCapturePathField = 256;
std::vector<wchar_t> StillWideField(const wchar_t* field, std::size_t count) {
    return std::vector<wchar_t>(field, field + count);
}
bool StillFieldUnchanged(const wchar_t* field, const std::vector<wchar_t>& snapshot) {
    return !snapshot.empty() && std::memcmp(field, snapshot.data(), snapshot.size() * sizeof(wchar_t)) == 0;
}
struct StillSaveState {
    std::vector<wchar_t> directory;
    std::vector<wchar_t> capturePath;
    std::vector<std::wstring> entries;
    unsigned warnings = 0;
    HMENU menu = nullptr;
    UINT remember = 0;
};
fs::path StillRememberedDirectory() { return output / L"still_remembered"; }
void EnsureStillRememberedDirectory() {
    Check(fs::create_directories(StillRememberedDirectory()) || fs::is_directory(StillRememberedDirectory()),
        "create the remembered directory fixture inside the private output directory");
}
HMENU StillRememberMenu() {
    HMENU bar = GetMenu(g_Block->MainWindow());
    if (bar == nullptr) return nullptr;
    if (GetMenuState(bar, 0x12D, MF_BYCOMMAND) != static_cast<UINT>(-1)) return bar;
    for (int i = 0; i < GetMenuItemCount(bar); ++i) {
        HMENU popup = GetSubMenu(bar, i);
        if (popup != nullptr && GetMenuState(popup, 0x12D, MF_BYCOMMAND) != static_cast<UINT>(-1)) return popup;
    }
    return nullptr;
}
StillSaveState SnapshotStillSaveState(MMDApp* app) {
    StillSaveState state;
    state.directory = StillWideField(app->DirUser(), kStillDirUserField);
    state.capturePath = StillWideField(app->CaptureSavePath(), kStillCapturePathField);
    state.entries = StillOutputEntries();
    state.warnings = warnings;
    state.menu = StillRememberMenu();
    state.remember = state.menu ? GetMenuState(state.menu, 0x12D, MF_BYCOMMAND) : static_cast<UINT>(-1);
    Check(state.menu != nullptr && state.remember != static_cast<UINT>(-1), "the remember-directory menu gate exists");
    return state;
}
std::vector<wchar_t> EnableStillRememberedDirectory(MMDApp* app, const fs::path& directory) {
    Check(fs::is_directory(directory), "the remembered directory fixture is a real directory");
    Check(wcscpy_s(app->DirUser(), kStillDirUserField, directory.c_str()) == 0, "seed a valid remembered directory");
    HMENU menu = StillRememberMenu();
    Check(menu != nullptr, "locate the remember-directory menu gate");
    CheckMenuItem(menu, 0x12D, MF_BYCOMMAND | MF_CHECKED);
    Check((GetMenuState(menu, 0x12D, MF_BYCOMMAND) & MF_CHECKED) != 0,
        "enable the remember-directory gate before the controlled command");
    return StillWideField(app->DirUser(), kStillDirUserField);
}
void RestoreStillSaveState(MMDApp* app, const StillSaveState& state) {
    std::memcpy(app->DirUser(), state.directory.data(), state.directory.size() * sizeof(wchar_t));
    CheckMenuItem(state.menu, 0x12D, MF_BYCOMMAND | (state.remember & MF_CHECKED));
    Check((GetMenuState(state.menu, 0x12D, MF_BYCOMMAND) & MF_CHECKED) == (state.remember & MF_CHECKED),
        "restore the remember-directory gate");
}
void RunStillRejection(MMDApp* app, unsigned language, StillPickerFailure failure) {
    EnsureStillRememberedDirectory();
    const auto state = SnapshotStillSaveState(app);
    const auto remembered = EnableStillRememberedDirectory(app, StillRememberedDirectory());
    stillPath = output / (L"rejected_still_" + std::to_wstring(language) + L".png");
    stillPickers = 0; stillSetup = 0; stillWarnings = 0;
    stillShowFailureCalls = stillLongItemsCreated = stillLongDisplayCalls = 0;
    stillLongPath = LongStillResultPath(language);
    selectedStillType = 3; cancelStill = false;
    stillPickerFailure = failure;
    stillCommandPicker = nullptr;
    stillCommandActive = true; stillCommandThread = GetCurrentThreadId();
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    const bool boundOwnPicker = stillCommandPicker != nullptr && stillCommandPicker != stillTemplatePicker;
    stillCommandActive = false;
    stillPickerFailure = StillPickerFailure::None;
    stillLongItem = nullptr;
    Check(boundOwnPicker, "the rejected command bound the production picker instance it created");
    Check(stillPickers == 1, "a rejected still path goes through the real save picker");
    if (failure == StillPickerFailure::ShowError) {
        Check(stillShowFailureCalls == 1 && stillLongItemsCreated == 0 && stillLongDisplayCalls == 0,
            "the injected non-cancel picker failure reaches Show exactly once");
    } else {
        Check(stillLongItemsCreated == 1 && stillLongDisplayCalls == 1 && stillShowFailureCalls == 0 &&
            stillLongPath.size() == 0x100 && std::wcslen(stillLongPath.c_str()) == 0x100,
            "the chosen item reports a filesystem display name at the production path-field boundary");
    }
    Check(stillWarnings == 1, "every rejected still path reports exactly one warning");
    Check(warnings == state.warnings, "rejected still path is not absorbed by the generic warning expectation");
    Check(!app->RecordingWindow() && !seq::StillActive(app), "rejected still path creates no recording state");
    Check(StillFieldUnchanged(app->DirUser(), remembered),
        "rejected still path leaves the seeded remembered directory field untouched");
    Check(StillFieldUnchanged(app->CaptureSavePath(), state.capturePath),
        "rejected still path leaves the full capture path field untouched");
    Check((GetMenuState(state.menu, 0x12D, MF_BYCOMMAND) & MF_CHECKED) != 0,
        "rejected still path leaves the remember-directory gate enabled");
    Check(!fs::exists(stillPath) && StillOutputEntries() == state.entries,
        "rejected still path writes no output file");
    RestoreStillSaveState(app, state);
    std::printf("PASS still-rejection language=%u mode=%s warnings=%u pickers=%u\n", language,
        failure == StillPickerFailure::ShowError ? "show" : "long-result", stillWarnings, stillPickers);
}
void RunStillRecovery(MMDApp* app, unsigned language, const wchar_t* label) {
    const unsigned oldWarnings = warnings, oldStillWarnings = stillWarnings;
    const auto entries = StillOutputEntries();
    const fs::path path = output / (L"retry_still_" + std::to_wstring(language) + L"_" + label + L".png");
    stillPath = path;
    stillPickers = 0; stillSetup = 0;
    selectedStillType = 1; cancelStill = false;
    stillPickerFailure = StillPickerFailure::None;
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    Check(stillPickers == 1 && app->RecordingWindow() && seq::StillActive(app),
        "a rejected still path is retryable through the real command");
    for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
        app->state.messageSeen = 1; coordinator_fixture::Drive(app);
    }
    Check(!app->RecordingWindow() && !seq::StillActive(app), "retried still export releases the recording state");
    Check(path.wstring().size() < kStillCapturePathField, "the retry path fits the production path field");
    Check(fs::is_regular_file(path) && fs::file_size(path) > 0, "retried still export writes the short chosen path");
    Check(_wcsicmp(app->CaptureSavePath(), path.c_str()) == 0, "retried still export commits the chosen path");
    auto expected = entries;
    const auto name = path.filename().wstring();
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
        expected.push_back(name);
        std::sort(expected.begin(), expected.end());
    }
    Check(warnings == oldWarnings && stillWarnings == oldStillWarnings && StillOutputEntries() == expected,
        "retried still export adds only its own file and reports no extra warning");
    std::printf("PASS still-recovery language=%u file=%ls\n", language, name.c_str());
}
void StillSceneBoundaryTests(MMDApp* app) {
    StillSaveDialogProbe probe;
    EnsureStillRememberedDirectory();
    const auto initial = SnapshotStillSaveState(app);
    const auto remembered = EnableStillRememberedDirectory(app, StillRememberedDirectory());
    const auto generation = app->SceneGeneration();
    stillPath = output / L"still-from-replaced-project.png";
    resetStillSceneOnShow = true;
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    Check(!resetStillSceneOnShow && stillPickers == 1 && app->SceneGeneration() != generation,
        "the old still picker returns a path after the production scene reset");
    Check(!app->RecordingWindow() && !seq::StillActive(app) && !app->state.timelineAdvanceRequested,
        "the replaced project does not enter still capture");
    Check(StillFieldUnchanged(app->DirUser(), remembered) &&
        StillFieldUnchanged(app->CaptureSavePath(), initial.capturePath) &&
        StillOutputEntries() == initial.entries && warnings == initial.warnings,
        "the stale picker does not change the destination or write output");
    RestoreStillSaveState(app, initial);
    app->RenderWidth() = 64;
    app->RenderHeight() = 48;
    const auto language = app->EnglishUI();
    RunStillRecovery(app, language, L"scene");
    const auto retried = output / (L"retry_still_" + std::to_wstring(language) + L"_scene.png");
    UINT width = 0, height = 0;
    const auto pixels = ImagePixels(retried, &width, &height);
    Check(width == 64 && height == 48 && pixels.size() == 64 * 48 * 4,
        "the current project exports a decodable PNG after retry");
    std::puts("PASS still export scene boundary and retry");
}
void RunStillRememberedDirectorySave(MMDApp* app) {
    const auto state = SnapshotStillSaveState(app);
    const auto currentDirectory = fs::current_path();
    auto& projectDirectory = app->PathWorkspace().projectDirectory;
    const auto originalProjectRoot = StillWideField(projectDirectory, std::size(projectDirectory));
    auto projectRoot = output;
    projectRoot.make_preferred();
    auto directory = output / L"still_save";
    directory.make_preferred();
    const auto path = directory / L"saved.png";
    Check(path.native().size() < kStillCapturePathField &&
        projectRoot.native().size() < std::size(projectDirectory),
        "the still-save fixture paths fit the production fields");
    const bool createdDirectory = fs::create_directory(directory);
    Check(fs::is_directory(directory) && !fs::exists(path), "use an unused still-save fixture path");

    Check(wcscpy_s(projectDirectory, std::size(projectDirectory), projectRoot.c_str()) == 0,
        "seed the original project resource root");
    const auto seededProjectRoot = StillWideField(projectDirectory, std::size(projectDirectory));
    EnableStillRememberedDirectory(app, StillRememberedDirectory());
    stillPath = path;
    stillPickers = 0; stillSetup = 0;
    selectedStillType = 1; cancelStill = false;
    SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
    Check(stillPickers == 1 && app->RecordingWindow() && seq::StillActive(app),
        "the remember-directory save starts through the real still command");
    for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
        app->state.messageSeen = 1; coordinator_fixture::Drive(app);
    }
    Check(!app->RecordingWindow() && !seq::StillActive(app),
        "the remember-directory save finishes the still capture");
    const bool saved = fs::is_regular_file(path) && fs::file_size(path) > 0;
    const bool rememberedOutput = _wcsicmp(app->DirUser(), directory.c_str()) == 0;
    const bool projectRootUnchanged = StillFieldUnchanged(projectDirectory, seededProjectRoot);

    std::memcpy(projectDirectory, originalProjectRoot.data(), sizeof projectDirectory);
    std::memcpy(app->CaptureSavePath(), state.capturePath.data(), state.capturePath.size() * sizeof(wchar_t));
    RestoreStillSaveState(app, state);
    fs::current_path(currentDirectory);
    fs::remove(path);
    if (createdDirectory) fs::remove(directory);

    Check(saved && warnings == state.warnings, "the remember-directory save writes the chosen PNG without warnings");
    Check(rememberedOutput, "successful still save remembers the chosen output directory");
    Check(projectRootUnchanged, "successful still save preserves the project resource root");
    std::printf("PASS still-remember-directory project root preserved\n");
}
void StillImageLanguageTests(MMDApp* app) {
    StillSaveDialogProbe probe;
    const int language = app->EnglishUI(), frame = app->CurrentFrame();
    const int width = app->RenderWidth(), height = app->RenderHeight();
    app->RenderWidth() = 64; app->RenderHeight() = 48;
    EnsureStillRememberedDirectory();
    const wchar_t* formats[] = {L"画像出力中（%d フレーム）", L"Exporting image (frame %d)", L"正在导出图片（第 %d 帧）"};
    for (int mode : {0, 1, 2}) {
        app->EnglishUI() = mode;
        for (int current : {0, 137}) {
            app->CurrentFrame() = current; stillPickers = 0;
            selectedStillType = current ? static_cast<UINT>(mode + 1) : 1;
            const wchar_t* suffix = selectedStillType == 2 ? L".jpeg" : selectedStillType == 3 ? L".bmp" : L".png";
            stillPath = output / (L"单张_" + std::to_wstring(mode) + L"_" + std::to_wstring(current) + suffix);
            SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
            const HWND window = app->RecordingWindow();
            Check(stillPickers == 1 && window && IsWindowUnicode(window), "still-image menu creates a Unicode export window");
            wchar_t expected[96]{}, actual[128]{};
            swprintf_s(expected, formats[mode], current);
            GetWindowTextW(window, actual, std::size(actual));
            Check(std::wcscmp(actual, expected) == 0, "still-image window preserves localized title and current frame");
            // WM_SETTEXT/WM_GETTEXT must also stay Unicode after creation,
            // including characters outside the system ANSI code page.
            const wchar_t* unicode = L"图片・画像・image \U0001F3AC";
            SetWindowTextW(window, unicode); GetWindowTextW(window, actual, std::size(actual));
            Check(std::wcscmp(actual, unicode) == 0, "export window caption updates preserve Unicode");
            SetWindowTextW(window, expected);
            for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
                app->state.messageSeen = 1; coordinator_fixture::Drive(app);
            }
            Check(!app->RecordingWindow() && fs::is_regular_file(stillPath) && fs::file_size(stillPath) > 0,
                "localized still-image export completes with the chosen file type");
            std::printf("PASS still-image language=%d frame=%d type=%u Unicode caption and output\n", mode, current, selectedStillType);
        }
        const auto canceledState = SnapshotStillSaveState(app);
        const auto canceledRemembered = EnableStillRememberedDirectory(app, StillRememberedDirectory());
        stillPath = output / (L"canceled_still_" + std::to_wstring(mode) + L".png");
        cancelStill = true; stillPickers = 0;
        SendMessageW(app->MainWindow(), WM_COMMAND, 276, 0);
        Check(stillPickers == 1 && !app->RecordingWindow(), "canceling still-image selection creates no export window");
        Check(warnings == canceledState.warnings && !seq::StillActive(app),
            "canceling still-image selection reports no warning and starts no capture");
        Check(StillFieldUnchanged(app->DirUser(), canceledRemembered) &&
            StillFieldUnchanged(app->CaptureSavePath(), canceledState.capturePath) &&
            (GetMenuState(canceledState.menu, 0x12D, MF_BYCOMMAND) & MF_CHECKED) != 0 &&
            !fs::exists(stillPath) && StillOutputEntries() == canceledState.entries,
            "canceling still-image selection keeps save state and output unchanged");
        cancelStill = false;
        RestoreStillSaveState(app, canceledState);
        // Non-cancel rejections in each UI language must warn once, leave no
        // capture state, and stay retryable for the next real short-path save.
        for (StillPickerFailure failure : {StillPickerFailure::ShowError, StillPickerFailure::LongResult}) {
            RunStillRejection(app, static_cast<unsigned>(mode), failure);
            RunStillRecovery(app, static_cast<unsigned>(mode),
                failure == StillPickerFailure::ShowError ? L"show" : L"long");
        }
    }
    RunStillRememberedDirectorySave(app);
    app->EnglishUI() = language; app->CurrentFrame() = frame;
    app->RenderWidth() = width; app->RenderHeight() = height;
}
#include "transparent_image_checks.hpp"
#include "transparent_capture_failure_checks.hpp"
#include "antialias_image_sequence_checks.hpp"
#include "image_sequence_continuity_checks.hpp"
#include "image_export_render_state_checks.inc"

void StillImageReentryTests(MMDApp* app) {
    StillSaveDialogProbe probe;
    const int width = app->RenderWidth(), height = app->RenderHeight();
    app->RenderWidth() = 64; app->RenderHeight() = 48;
    const fs::path outerPath = output / L"still_reentry.png";
    const HWND main = app->MainWindow();
    stillPath = outerPath;
    stillPickers = 0;
    reenterStillOnShow = true;
    SendMessageW(main, WM_COMMAND, 276, 0);
    const unsigned startupPickers = stillPickers;
    const HWND window = app->RecordingWindow();
    Check(!reenterStillOnShow && window && seq::StillActive(app),
        "the outer still command starts a capture after sending the nested command from Show");
    Check(_wcsicmp(app->CaptureSavePath(), outerPath.c_str()) == 0 && !fs::exists(outerPath),
        "the outer still picker commits its own path before the PNG is written");
    const auto capturePath = StillWideField(app->CaptureSavePath(), kStillCapturePathField);
    auto expectedEntries = StillOutputEntries();
    expectedEntries.push_back(outerPath.filename().wstring());
    std::sort(expectedEntries.begin(), expectedEntries.end());

    // Dispatch only the queued command while the outer recording is active.
    // Cancel any unexpected picker so the original window can still finish in RED.
    stillPath = output / L"still_reentry_blocked.png";
    cancelStill = true;
    MSG command{};
    Check(PostMessageW(main, WM_COMMAND, 276, 0) &&
        PeekMessageW(&command, main, WM_COMMAND, WM_COMMAND, PM_REMOVE) &&
        command.hwnd == main && command.wParam == 276 && command.lParam == 0,
        "queue a real still command during the outer capture");
    DispatchMessageW(&command);
    cancelStill = false;
    const bool captureUnchanged = app->RecordingWindow() == window && seq::StillActive(app) &&
        StillFieldUnchanged(app->CaptureSavePath(), capturePath);

    for (unsigned step = 0; app->RecordingWindow() && step < 12; ++step) {
        app->state.messageSeen = 1; coordinator_fixture::Drive(app);
    }
    Check(!app->RecordingWindow() && !seq::StillActive(app) && !IsWindow(window),
        "the outer still capture finishes and releases its recording window");
    UINT pngWidth = 0, pngHeight = 0;
    ImagePixels(outerPath, &pngWidth, &pngHeight);
    constexpr unsigned char pngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    char signature[sizeof pngSignature]{};
    std::ifstream png(outerPath, std::ios::binary);
    Check(bool(png.read(signature, sizeof signature)) && std::memcmp(signature, pngSignature, sizeof signature) == 0,
        "the outer capture writes a PNG");
    Check(pngWidth == 64 && pngHeight == 48 && StillOutputEntries() == expectedEntries,
        "the outer capture produces one 64x48 image at its chosen path");
    Check(startupPickers == 1, "a nested still command during Show cannot open a second picker");
    Check(stillPickers == startupPickers && captureUnchanged,
        "a queued still command during capture cannot open another picker or retarget the outer capture");
    app->RenderWidth() = width; app->RenderHeight() = height;
}
#include "still_shell_host_checks.inc"
}

// ===========================================================================
// Image-sequence menu lifecycle matrix (--sequence-lifecycle-only).
//
// This mode enters through the real menu command, folder picker, settings
// dialog and recording window. Only this fixture's own picker result, dialog
// input and cancellation confirmation are replaced; the production sequence
// writer, finalizer, host window procedure and every default-mode assertion
// stay untouched. It runs only when the explicit mode is requested.
// ===========================================================================
namespace sequence_lifecycle {
using RealDialogBox = decltype(&UiDialogBoxParamA);
using RealWriteFrame = decltype(&seq::WriteFrame);
using RealFinish = decltype(&seq::Finish);
using RealUiMessage = decltype(&ShowUiMessage);
using RealPostMessage = decltype(&PostMessageW);
using RealFolderShow = HRESULT (STDMETHODCALLTYPE*)(IFileOpenDialog*, HWND);
using RealFolderResult = HRESULT (STDMETHODCALLTYPE*)(IFileOpenDialog*, IShellItem**);

RealDialogBox realDialogBox = nullptr;
RealWriteFrame realWriteFrame = nullptr;
RealFinish realFinish = nullptr;
RealUiMessage realUiMessage = nullptr;
RealPostMessage realPostMessage = nullptr;
RealFolderShow realFolderShow = nullptr;
RealFolderResult realFolderResult = nullptr;
void* pickerShowTarget = nullptr;
void* pickerResultTarget = nullptr;
WNDPROC previousMainWndProc = nullptr;
decltype(&GetUiSaveFileNameW) realProjectPicker = nullptr;
export_save_workflow::SaveRetry projectSave;

enum class WriteFault { None, FolderRemoved, LateCollision };

struct Probe {
    MMDApp* app = nullptr;
    HWND main = nullptr;
    HWND recording = nullptr;
    std::wstring prefix;
    fs::path directory, savedDirectory, failedFrame;
    WriteFault fault = WriteFault::None;
    std::wstring writeError;
    std::vector<std::vector<unsigned char>> savedFrames;
    unsigned errors = 0;
    int fps = 0, first = 0, last = 0;
    bool close = false;
    bool caseActive = false;
    bool cancelInProgress = false;
    bool forwardClose = false;
    unsigned writes = 0, finishes = 0, confirmations = 0;
    unsigned posts = 0, deliveries = 0, quits = 0;
    unsigned pickerShows = 0, pickerResults = 0, dialogs = 0;
    bool commandActive = false;
    DWORD commandThread = 0;
    HWND commandOwner = nullptr;
    IFileOpenDialog* commandPicker = nullptr;
    bool finishReturned = false, finishSucceeded = false;
    bool timingOk = true, deltaOk = true;
    int savedWidth = 0, savedHeight = 0, savedScreenWidth = 0, savedScreenHeight = 0;
    RECT savedPresentation{};
    float savedHudScale = 1;
    unsigned savedDirty = 0, savedModelDirty = 0;
    bool checkInterfaceScale = false;
    bool checkProjectSave = false;
} probe;

struct CaseStats {
    unsigned writes = 0, decoded = 0, posts = 0, deliveries = 0, confirmations = 0, quits = 0;
    bool finishReturned = false, finishSucceeded = false, timingOk = true, deltaOk = true;
};

struct CancelResult {
    CaseStats cancel;
    CaseStats retry;
};

bool Stopped(MMDApp* app) {
    return app != nullptr && app->RecordingWindow() == nullptr && !seq::Active(app) &&
        !app->PlaybackActive() && !app->FrameStepPlayback() &&
        app->state.recordPlaybackStartPending == 0 && !app->AviRenderResolutionOverridden() &&
        app->CaptureRenderTarget() == nullptr && app->CaptureSystemSurface() == nullptr &&
        app->CaptureReadbackPixels() == nullptr;
}

void CheckNaturalClose() {
    Check(probe.close && !probe.cancelInProgress,
        "close is requested only by an opted-in natural sequence success");
    Check(probe.finishReturned && probe.finishSucceeded && probe.finishes == 1,
        "close follows the real successful sequence finalizer return");
    Check(Stopped(probe.app), "close follows recording, playback and capture teardown");
    Check(probe.app->RenderWidth() == probe.savedWidth && probe.app->RenderHeight() == probe.savedHeight,
        "close follows editor resolution restoration");
    Check(probe.app->Renderer()->screenWidth == probe.savedScreenWidth &&
        probe.app->Renderer()->screenHeight == probe.savedScreenHeight,
        "close follows preview resolution restoration");
    Check(probe.recording != nullptr && !IsWindow(probe.recording),
        "close follows destruction of the real recording window");
    Check(probe.app->MainWindow() == probe.main, "close belongs to the recording host window");
}

// Check the editor view and the project's dirty flags after recording teardown.
void CheckRestored(MMDApp* app) {
    Check(app->RenderWidth() == probe.savedWidth && app->RenderHeight() == probe.savedHeight,
        "export restores the saved editor dimensions");
    Check(app->Renderer()->screenWidth == probe.savedScreenWidth &&
        app->Renderer()->screenHeight == probe.savedScreenHeight,
        "export restores the saved preview dimensions");
    Check(app->SceneModified() == probe.savedDirty && app->EnhancedModelDirty() == probe.savedModelDirty,
        "export preserves the fixture project's dirty flags");
    if (probe.checkInterfaceScale) {
        const auto present = FramePresentationRect(app);
        Check(ui_scale::Percent() == 200 && EqualRect(&present, &probe.savedPresentation) &&
            FrameOverlayScale(app) == probe.savedHudScale,
            "export restores the same editor presentation and HUD at 200 percent");
    }
}

// The fixture host procedure records the single controlled delivery. It never
// forwards WM_CLOSE except in the final real-close case, so the isolated host
// stays alive for decoding and the following cases.
LRESULT CALLBACK MainWindowSink(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE) {
        ++probe.deliveries;
        Check(probe.caseActive && probe.posts == 1, "the close delivery follows one posted request");
        CheckNaturalClose();
        Check(probe.deliveries == 1, "the fixture receives no duplicate close delivery");
        if (!probe.forwardClose) return 0;
    }
    return CallWindowProcW(previousMainWndProc, window, message, wp, lp);
}

BOOL WINAPI PostHook(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CLOSE && window == probe.main) {
        Check(probe.caseActive, "no close is posted outside a controlled lifecycle case");
        ++probe.posts;
        Check(window == probe.app->MainWindow(), "close is posted to the real main window");
        Check(probe.posts == 1, "one natural success posts exactly one close request");
        CheckNaturalClose();
    }
    return realPostMessage(window, message, wp, lp);
}

void ArmWriteFault();

bool WriteFrameHook(MMDApp* app, IDirect3DSurface9* surface, std::wstring& error) {
    if (!probe.caseActive || app != probe.app) return realWriteFrame(app, surface, error);
    const double expected = static_cast<double>(probe.first) / 30.0 +
        static_cast<double>(probe.writes) / static_cast<double>(probe.fps);
    const double cursor = app->PlaybackCursorSeconds();
    const double delta = app->DeltaTime();
    const double wantedDelta = 1.0 / static_cast<double>(probe.fps);
    const bool timeline = std::abs(cursor - expected) < 0.0001;
    const bool rate = std::abs(delta - wantedDelta) < 0.00001;
    if (!timeline || !rate)
        std::printf("SEQUENCE-LIFECYCLE timing writes=%u cursor=%.9f expected=%.9f delta=%.9f expected_delta=%.9f\n",
            probe.writes, cursor, expected, delta, wantedDelta);
    probe.timingOk = probe.timingOk && timeline;
    probe.deltaOk = probe.deltaOk && rate;
    Check(probe.posts == 0 && probe.deliveries == 0,
        "no close is requested while sequence samples are written");
    const bool written = realWriteFrame(app, surface, error);
    if (probe.fault != WriteFault::None && probe.writes == 3) {
        Check(!written && !error.empty(), "the real writer rejects the fourth output");
        Check(error.find(probe.failedFrame.wstring()) != std::wstring::npos,
            "the writer identifies the fourth frame's output path");
        Check(error.rfind(probe.fault == WriteFault::FolderRemoved ? L"无法写入图片" : L"已存在同名文件", 0) == 0,
            "the writer distinguishes an unavailable directory from an occupied filename");
        probe.writeError = error;
        return written;
    }
    Check(written, "the real image-sequence writer accepts the rendered sample");
    ++probe.writes;
    if (probe.fault != WriteFault::None && probe.writes == 3) ArmWriteFault();
    return written;
}

bool AcceptWriteError(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    if (!probe.caseActive || probe.writeError.empty()) return false;
    Check(owner == probe.main && text && probe.writeError == text && title &&
        std::wcscmp(title, L"导出图片序列") == 0 && flags == (MB_OK | MB_ICONERROR),
        "the editor reports the real write error to its own window");
    Check(Stopped(probe.app) && probe.finishReturned && !probe.finishSucceeded,
        "the write error is shown after incomplete recording teardown");
    Check(++probe.errors == 1, "the export displays its write error exactly once");
    return true;
}

bool AcceptSaveFailure(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    return projectSave.AcceptFailure(owner, text, title, flags);
}

BOOL ProjectSavePicker(LPOPENFILENAMEW picker) {
    return projectSave.OwnsPicker(picker) ? projectSave.Pick(picker) : realProjectPicker(picker);
}

bool FinishHook(MMDApp* app, bool reachedRequestedEnd, std::wstring& error) {
    if (!probe.caseActive || app != probe.app) return realFinish(app, reachedRequestedEnd, error);
    Check(probe.posts == 0, "no close is requested before the real finalizer returns");
    ++probe.finishes;
    const bool complete = realFinish(app, reachedRequestedEnd, error);
    probe.finishReturned = true;
    probe.finishSucceeded = complete;
    return complete;
}

int UiMessageHook(MMDApp* app, HWND owner, UiMessage message, UINT flags, const wchar_t* detail) {
    if (probe.caseActive && probe.cancelInProgress && app == probe.app &&
        owner == probe.recording && message == UiMessage::StopRecording &&
        flags == (MB_YESNO | MB_ICONQUESTION)) {
        ++probe.confirmations;
        return IDYES;
    }
    return realUiMessage(app, owner, message, flags, detail);
}

HRESULT STDMETHODCALLTYPE FolderShowHook(IFileOpenDialog* picker, HWND owner) {
    // Only the picker created by this fixture's own menu command, on the
    // dispatching thread and with the production owner, may be replaced.
    // Every other Show keeps the real shell implementation.
    if (!probe.commandActive || GetCurrentThreadId() != probe.commandThread ||
        owner != probe.commandOwner || probe.commandPicker != nullptr)
        return realFolderShow(picker, owner);
    probe.commandPicker = picker;
    ++probe.pickerShows;
    Check(owner == probe.app->MainWindow(), "the sequence folder picker belongs to the main window");
    FILEOPENDIALOGOPTIONS options{};
    Check(SUCCEEDED(picker->GetOptions(&options)) && (options & FOS_PICKFOLDERS) != 0,
        "the real menu folder picker requests folders");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FolderResultHook(IFileOpenDialog* picker, IShellItem** item) {
    // GetResult is replaced only for the exact picker instance this command
    // bound; everything else keeps the real shell result.
    if (!probe.commandActive || GetCurrentThreadId() != probe.commandThread ||
        probe.commandPicker == nullptr || picker != probe.commandPicker)
        return realFolderResult(picker, item);
    ++probe.pickerResults;
    return SHCreateItemFromParsingName(probe.directory.c_str(), nullptr, IID_PPV_ARGS(item));
}

INT_PTR CALLBACK SettingsDialogProbe(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message != WM_INITDIALOG) return seq::DialogProc(dialog, message, wp, lp);
    seq::DialogProc(dialog, message, wp, lp);
    auto* context = reinterpret_cast<seq::DialogContext*>(lp);
    Check(context != nullptr, "the real settings dialog carries its production context");
    Check(Read(dialog, panel::kAviEndFrameEdit) == std::to_wstring(seq::SceneEndFrame(probe.app)),
        "the sequence dialog initializes the automatically detected scene range");
    Check(static_cast<int>(SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETCOUNT, 0, 0)) == 7,
        "the real sequence dialog lists all seven formats");
    wchar_t selected[32]{};
    const auto selection = SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETCURSEL, 0, 0);
    SendDlgItemMessageW(dialog, seq::FormatCombo, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(selected));
    Check(std::wcscmp(selected, L"PNG") == 0, "the real sequence dialog defaults to PNG");
    // Only this fixture's input is replaced. Production receives the requested
    // size, nonzero start frame, range, FPS, prefix and close choice unchanged.
    context->updating = true;
    SetDlgItemInt(dialog, panel::kAviWidthEdit, 64, FALSE);
    SetDlgItemInt(dialog, panel::kAviHeightEdit, 48, FALSE);
    SetDlgItemInt(dialog, panel::kAviStartFrameEdit, probe.first, FALSE);
    SetDlgItemInt(dialog, panel::kAviEndFrameEdit, probe.last, FALSE);
    SetDlgItemTextW(dialog, panel::kAviFpsEdit, probe.fps == 24 ? L"24" : L"60");
    SetDlgItemTextW(dialog, seq::Prefix, probe.prefix.c_str());
    SendDlgItemMessageW(dialog, seq::FormatCombo, CB_SETCURSEL, 2, 0);
    CheckDlgButton(dialog, panel::kAviCloseAfterExportCheckbox, probe.close ? BST_CHECKED : BST_UNCHECKED);
    context->updating = false;
    if (probe.fault != WriteFault::None && probe.savedDirty) {
        seq::DialogProc(dialog, WM_COMMAND, MAKEWPARAM(panel::kAviCloseAfterExportCheckbox, BN_CLICKED), 0);
        const auto warning = GetDlgItem(dialog, panel::kExportUnsavedWarning);
        const auto save = GetDlgItem(dialog, panel::kExportSaveProjectButton);
        Check((GetWindowLongPtrW(warning, GWL_STYLE) & WS_VISIBLE) &&
            (GetWindowLongPtrW(save, GWL_STYLE) & WS_VISIBLE) && IsWindowEnabled(save),
            "the dirty project with close enabled shows the warning and save action");
    }
    Check(Read(dialog, panel::kAviWidthEdit) == L"64" && Read(dialog, panel::kAviHeightEdit) == L"48" &&
        Read(dialog, panel::kAviStartFrameEdit) == std::to_wstring(probe.first),
        "the controlled dialog input keeps the requested size and nonzero start frame");
    if (probe.checkProjectSave) {
        using Parameter = export_save_workflow::Parameter;
        projectSave.Drive(*probe.app, dialog, probe.directory / L"project-save",
            {{panel::kAviWidthEdit}, {panel::kAviHeightEdit}, {panel::kAviStartFrameEdit},
             {panel::kAviEndFrameEdit}, {panel::kAviFpsEdit}, {seq::Prefix}, {seq::FormatCombo, Parameter::Choice},
             {panel::kAviCloseAfterExportCheckbox, Parameter::CheckBox}}, Check);
        probe.savedDirty = probe.app->SceneModified();
    }
    seq::DialogProc(dialog, WM_COMMAND, IDOK, 0);
    return TRUE;
}

INT_PTR DialogBoxHook(HINSTANCE instance, LPCSTR name, HWND owner, DLGPROC proc, LPARAM parameter) {
    if (probe.commandActive && GetCurrentThreadId() == probe.commandThread &&
        owner == probe.commandOwner && proc == seq::DialogProc) {
        ++probe.dialogs;
        proc = SettingsDialogProbe;
    }
    return realDialogBox(instance, name, owner, proc, parameter);
}

// A menu command owns exactly one picker instance and one settings dialog on
// the dispatching thread. The local scope clears the binding even on failure.
struct CommandScope {
    CommandScope() {
        probe.commandActive = true;
        probe.commandThread = GetCurrentThreadId();
        probe.commandOwner = probe.main;
        probe.commandPicker = nullptr;
    }
    ~CommandScope() {
        probe.commandActive = false;
        probe.commandThread = 0;
        probe.commandOwner = nullptr;
        probe.commandPicker = nullptr;
    }
};

void InstallHooks(MMDApp* app) {
    probe.app = app;
    probe.main = app->MainWindow();
    Check(probe.main != nullptr, "the isolated host owns a main window");
    previousMainWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(probe.main, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&MainWindowSink)));
    Check(previousMainWndProc != nullptr, "install the controlled close sink on the real main window");
    Check(MH_CreateHook(reinterpret_cast<void*>(&PostMessageW), reinterpret_cast<void*>(&PostHook),
        reinterpret_cast<void**>(&realPostMessage)) == MH_OK, "observe real close requests");
    Check(MH_CreateHook(reinterpret_cast<void*>(&seq::WriteFrame), reinterpret_cast<void*>(&WriteFrameHook),
        reinterpret_cast<void**>(&realWriteFrame)) == MH_OK, "observe the real sequence writer");
    Check(MH_CreateHook(reinterpret_cast<void*>(&seq::Finish), reinterpret_cast<void*>(&FinishHook),
        reinterpret_cast<void**>(&realFinish)) == MH_OK, "observe the real sequence finalizer");
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowUiMessage), reinterpret_cast<void*>(&UiMessageHook),
        reinterpret_cast<void**>(&realUiMessage)) == MH_OK, "control the recording-window confirmation");
    Check(MH_CreateHook(reinterpret_cast<void*>(&UiDialogBoxParamA), reinterpret_cast<void*>(&DialogBoxHook),
        reinterpret_cast<void**>(&realDialogBox)) == MH_OK, "control only this fixture's settings dialog input");
    if (probe.checkProjectSave) {
        const auto target = reinterpret_cast<void*>(&GetUiSaveFileNameW);
        Check(MH_CreateHook(target, reinterpret_cast<void*>(&ProjectSavePicker),
            reinterpret_cast<void**>(&realProjectPicker)) == MH_OK && MH_EnableHook(target) == MH_OK,
            "control the project path only for the sequence settings dialog");
    }
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    Check(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&picker))), "create a real shell picker to locate its vtable");
    auto** table = *reinterpret_cast<void***>(picker.Get());
    pickerShowTarget = table[3]; pickerResultTarget = table[20];
    Check(MH_CreateHook(pickerShowTarget, reinterpret_cast<void*>(&FolderShowHook),
        reinterpret_cast<void**>(&realFolderShow)) == MH_OK &&
        MH_CreateHook(pickerResultTarget, reinterpret_cast<void*>(&FolderResultHook),
            reinterpret_cast<void**>(&realFolderResult)) == MH_OK,
        "replace only this fixture's picker selection");
    void* targets[] = {
        reinterpret_cast<void*>(&PostMessageW), reinterpret_cast<void*>(&seq::WriteFrame),
        reinterpret_cast<void*>(&seq::Finish), reinterpret_cast<void*>(&ShowUiMessage),
        reinterpret_cast<void*>(&UiDialogBoxParamA), pickerShowTarget, pickerResultTarget,
    };
    for (void* target : targets) Check(MH_EnableHook(target) == MH_OK, "enable sequence lifecycle observation");
}

void RemoveHooks() {
    void* targets[] = {
        reinterpret_cast<void*>(&PostMessageW), reinterpret_cast<void*>(&seq::WriteFrame),
        reinterpret_cast<void*>(&seq::Finish), reinterpret_cast<void*>(&ShowUiMessage),
        reinterpret_cast<void*>(&UiDialogBoxParamA), pickerShowTarget, pickerResultTarget,
    };
    for (void* target : targets) { MH_DisableHook(target); MH_RemoveHook(target); }
    if (probe.checkProjectSave) {
        const auto target = reinterpret_cast<void*>(&GetUiSaveFileNameW);
        Check(MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK, "remove the project picker hook");
    }
    if (previousMainWndProc != nullptr && probe.main != nullptr && IsWindow(probe.main))
        SetWindowLongPtrW(probe.main, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousMainWndProc));
    previousMainWndProc = nullptr;
}

bool PumpMessages() {
    MSG message{};
    for (int count = 0; count < 100; ++count) {
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) return true;
        if (message.message == WM_QUIT) {
            Check(message.wParam == 0, "WM_QUIT carries the normal zero exit code");
            ++probe.quits;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (probe.main != nullptr && !IsWindow(probe.main)) return false;
    }
    return true;
}

void IdlePump(MMDApp* app, int frames) {
    for (int idle = 0; idle < frames && probe.main != nullptr && IsWindow(probe.main); ++idle) {
        if (!PumpMessages()) break;
        app->DeltaTime() = 1.f / 30;
        app->state.messageSeen = 1;
        coordinator_fixture::Drive(app);
    }
}

void DriveNatural(MMDApp* app) {
    for (int step = 0; step < 400 && app->RecordingWindow() != nullptr; ++step) {
        if (!PumpMessages()) return;
        if (app->RecordingWindow() == nullptr || !IsWindow(probe.main)) return;
        app->DeltaTime() = 1.f / 30;
        app->state.messageSeen = 1;
        coordinator_fixture::Drive(app);
        if (!IsWindow(probe.main)) return;
    }
}

void DriveCancel(MMDApp* app) {
    for (int step = 0; step < 400 && app->RecordingWindow() != nullptr; ++step) {
        MSG message{};
        for (int pump = 0; pump < 100 && app->RecordingWindow() != nullptr && probe.writes < 3 &&
             PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++pump) {
            if (message.message == WM_QUIT) {
                Check(message.wParam == 0, "WM_QUIT carries the normal zero exit code");
                ++probe.quits;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            Check(IsWindow(probe.main), "the host window survives until the cancellation request");
        }
        if (probe.writes >= 3 || app->RecordingWindow() == nullptr) break;
        app->DeltaTime() = 1.f / 30;
        app->state.messageSeen = 1;
        coordinator_fixture::Drive(app);
    }
    Check(probe.writes == 3, "cancellation observes exactly three accepted samples at the sampling boundary");
    Check(app->RecordingWindow() == probe.recording,
        "cancellation is requested while the real recording window is still active");
    probe.cancelInProgress = true;
    SendMessageW(probe.recording, WM_KEYDOWN, VK_ESCAPE, 0);
    probe.cancelInProgress = false;
}

fs::path NumberedPath(const std::wstring& prefix, unsigned number) {
    wchar_t digits[16]{};
    swprintf_s(digits, L"%02u", number);
    return probe.directory / (prefix + digits + L".png");
}

std::vector<unsigned char> ReadBytes(const fs::path& path) {
    const auto size = static_cast<std::size_t>(fs::file_size(path));
    std::vector<unsigned char> bytes(size);
    std::ifstream stream(path, std::ios::binary);
    Check(bool(stream), "open the sequence image for byte comparison");
    if (size != 0) stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    Check(bool(stream) && static_cast<std::size_t>(stream.gcount()) == size,
        "read the complete sequence image bytes");
    return bytes;
}

unsigned CountPrefixed(const std::wstring& prefix) {
    unsigned count = 0;
    for (const auto& entry : fs::directory_iterator(probe.directory)) {
        const auto name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) != 0) continue;
        Check(entry.path().extension() == L".png", "an independent sequence prefix produces PNG files only");
        ++count;
    }
    return count;
}

void VerifyImages(const std::wstring& prefix, unsigned expected, unsigned existing = 0) {
    Check(CountPrefixed(prefix) == expected + existing, "one independent prefix has exactly the expected files");
    for (unsigned number = 1; number <= expected; ++number) {
        const auto path = NumberedPath(prefix, number);
        Check(fs::is_regular_file(path), "the sequence image exists at its two-digit frame name");
        UINT width = 0, height = 0;
        const auto pixels = ImagePixels(path, &width, &height);
        Check(width == 64 && height == 48, "the sequence image keeps the requested 64x48 size");
        Check(pixels.size() == std::size_t(64) * 48 * 4, "the sequence image decodes to a complete pixel payload");
    }
    for (const auto& entry : fs::directory_iterator(probe.directory))
        Check(entry.path().extension() != L".tmp", "no temporary sequence frame remains in the output directory");
}

void ResetCase(int fps, int first, int last, bool close, const std::wstring& prefix,
               const fs::path& directory = output) {
    probe.fps = fps; probe.first = first; probe.last = last; probe.close = close;
    probe.prefix = prefix; probe.recording = nullptr;
    probe.directory = directory;
    probe.savedDirectory.clear();
    probe.failedFrame.clear();
    probe.fault = WriteFault::None;
    probe.writeError.clear();
    probe.savedFrames.clear();
    probe.errors = 0;
    probe.writes = probe.finishes = probe.confirmations = 0;
    probe.posts = probe.deliveries = probe.quits = 0;
    probe.finishReturned = probe.finishSucceeded = false;
    probe.timingOk = probe.deltaOk = true;
    probe.savedWidth = probe.app->RenderWidth();
    probe.savedHeight = probe.app->RenderHeight();
    probe.savedScreenWidth = probe.app->Renderer()->screenWidth;
    probe.savedScreenHeight = probe.app->Renderer()->screenHeight;
    probe.savedDirty = probe.app->SceneModified();
    probe.savedModelDirty = probe.app->EnhancedModelDirty();
    if (probe.checkInterfaceScale) {
        probe.savedPresentation = FramePresentationRect(probe.app);
        probe.savedHudScale = FrameOverlayScale(probe.app);
        Check(ui_scale::Percent() == 200 && probe.savedHudScale > 1 && !IsRectEmpty(&probe.savedPresentation),
            "the editor starts the export with a real enlarged HUD");
    }
    probe.caseActive = true;
}

CaseStats CaptureStats() {
    CaseStats stats;
    stats.writes = probe.writes;
    stats.posts = probe.posts;
    stats.deliveries = probe.deliveries;
    stats.confirmations = probe.confirmations;
    stats.quits = probe.quits;
    stats.finishReturned = probe.finishReturned;
    stats.finishSucceeded = probe.finishSucceeded;
    stats.timingOk = probe.timingOk;
    stats.deltaOk = probe.deltaOk;
    return stats;
}

void StartMenu(MMDApp* app) {
    const unsigned shows = probe.pickerShows, results = probe.pickerResults, dialogs = probe.dialogs;
    Check(probe.prefix.size() < 128, "the fixture prefix fits the production prefix field");
    {
        const CommandScope command;
        SendMessageW(probe.main, WM_COMMAND, seq::kMenuCommand, 0);
        Check(probe.commandPicker != nullptr, "the menu command bound its own production folder picker");
    }
    Check(probe.pickerShows == shows + 1 && probe.pickerResults == results + 1,
        "the menu command runs the real folder picker exactly once");
    Check(probe.dialogs == dialogs + 1, "the menu command runs the real settings dialog exactly once");
    Check(seq::Active(app), "the menu prepares an active image-sequence session");
    Check(app->RecordingWindow() != nullptr && app->state.recordPlaybackStartPending != 0,
        "the menu starts the real recording window");
    if (probe.checkInterfaceScale) {
        Check(app->RenderWidth() == 64 && app->RenderHeight() == 48 &&
            ui_scale::Percent() == 200 && FrameOverlayScale(app) <= 1,
            "capture uses the requested pixels and keeps the editor scale out of the recording HUD");
    }
    probe.recording = app->RecordingWindow();
}

CaseStats RunNatural(MMDApp* app, int fps, int first, int last, unsigned expected, bool close,
                     const std::wstring& prefix, const fs::path& directory = output) {
    ResetCase(fps, first, last, close, prefix, directory);
    StartMenu(app);
    DriveNatural(app);
    Check(!app->RecordingWindow() && !seq::Active(app), "natural completion releases the recording session");
    Check(Stopped(app), "natural completion performs the full recording teardown");
    Check(probe.recording != nullptr && !IsWindow(probe.recording),
        "natural completion destroys its recording window");
    Check(probe.writes == expected, "real writer count matches the independent short-range oracle");
    Check(probe.timingOk && probe.deltaOk, "samples advance from the nonzero start frame at the output FPS");
    Check(probe.finishes == 1 && probe.finishReturned && probe.finishSucceeded,
        "natural completion reports success from the real finalizer");
    Check(probe.confirmations == 0, "natural completion asks no cancel confirmation");
    IdlePump(app, 8);
    const unsigned wanted = close ? 1u : 0u;
    Check(probe.posts == wanted && probe.deliveries == wanted,
        "natural completion produces the exact close request and delivery counts");
    VerifyImages(prefix, expected);
    IdlePump(app, 4);
    Check(probe.posts == wanted && probe.deliveries == wanted && Stopped(app),
        "delayed messages neither repeat the close nor restart recording");
    Check(probe.quits == 0, "a natural export case receives no WM_QUIT");
    CheckRestored(app);
    CaseStats stats = CaptureStats();
    stats.decoded = expected;
    probe.caseActive = false;
    return stats;
}

CancelResult RunCancel(MMDApp* app, int fps, int first, int last, const std::wstring& prefix,
                       const std::wstring& retryPrefix, unsigned retryExpected) {
    ResetCase(fps, first, last, true, prefix);
    StartMenu(app);
    DriveCancel(app);
    Check(probe.writes == 3, "cancellation keeps exactly three accepted samples");
    Check(probe.confirmations == 1, "cancellation uses exactly one real recording-window confirmation");
    Check(probe.finishes == 1 && probe.finishReturned && !probe.finishSucceeded,
        "cancellation reports incomplete from the real finalizer");
    Check(!app->RecordingWindow() && !seq::Active(app), "cancellation releases the recording session");
    Check(Stopped(app), "cancellation performs the full recording teardown");
    Check(probe.recording != nullptr && !IsWindow(probe.recording), "cancellation destroys its recording window");
    Check(probe.posts == 0 && probe.deliveries == 0,
        "cancellation requests no close even when close-after-export is enabled");
    Check(probe.timingOk && probe.deltaOk, "canceled samples advance from the nonzero start frame at the output FPS");
    IdlePump(app, 8);
    Check(probe.posts == 0 && probe.deliveries == 0, "delayed messages do not close after cancellation");
    Check(probe.quits == 0, "a canceled export case receives no WM_QUIT");
    CheckRestored(app);
    VerifyImages(prefix, 3);
    std::vector<std::vector<unsigned char>> before;
    for (unsigned number = 1; number <= 3; ++number)
        before.push_back(ReadBytes(NumberedPath(prefix, number)));
    CancelResult result;
    result.cancel = CaptureStats();
    result.cancel.decoded = 3;
    probe.caseActive = false;
    // Retry immediately through the very same real menu command with a fresh
    // prefix. The three canceled files must stay byte-identical.
    result.retry = RunNatural(app, fps, 17, 22, retryExpected, false, retryPrefix);
    Check(CountPrefixed(prefix) == 3, "the canceled prefix keeps exactly three frames after the retry");
    for (unsigned number = 1; number <= 3; ++number)
        Check(ReadBytes(NumberedPath(prefix, number)) == before[number - 1],
            "retrying the export leaves the canceled frame bytes unchanged");
    probe.caseActive = false;
    return result;
}

constexpr char kExistingFile[] = "existing fixture file";

void WriteExistingFile(const fs::path& path) {
    Check(!fs::exists(path), "the simulated user's file uses a fresh fixture path");
    std::ofstream stream(path, std::ios::binary);
    stream.write(kExistingFile, sizeof kExistingFile - 1);
    stream.close();
    Check(bool(stream), "create an existing file in the isolated fixture");
}

void CheckExistingFile(const fs::path& path) {
    const auto bytes = ReadBytes(path);
    Check(bytes.size() == sizeof kExistingFile - 1 &&
        std::equal(bytes.begin(), bytes.end(), kExistingFile),
        "the existing file's bytes are preserved");
}

void ArmWriteFault() {
    for (unsigned n = 1; n <= 3; ++n)
        probe.savedFrames.push_back(ReadBytes(NumberedPath(probe.prefix, n)));
    if (probe.fault == WriteFault::FolderRemoved) {
        // Move only this case's own output directory after three real writes.
        // The next production GetTempFileNameW sees a missing path.
        probe.savedDirectory = probe.directory;
        probe.savedDirectory += L"-moved";
        Check(probe.directory.parent_path() == output && !fs::exists(probe.savedDirectory),
            "move only the owned case directory to a fresh sibling");
        Check(MoveFileW(probe.directory.c_str(), probe.savedDirectory.c_str()) &&
            !fs::exists(probe.directory), "make the destination unavailable between frames");
    } else {
        WriteExistingFile(probe.failedFrame);
    }
}

void RunWriteFault(MMDApp* app, WriteFault fault) {
    const wchar_t* label = fault == WriteFault::FolderRemoved ? L"missing-folder" : L"late-collision";
    const auto destination = output / label;
    const auto retry = output / (std::wstring(label) + L"-retry");
    Check(fs::create_directory(destination) && fs::create_directory(retry),
        "create fresh source and retry directories");
    WriteExistingFile(destination / L"keep.txt");
    const std::wstring prefix = L"seq_fault_";
    const std::wstring projectPath = app->EnvFileName();
    const auto projectGeneration = app->SceneGeneration();
    ResetCase(24, 17, 22, true, prefix, destination);
    probe.fault = fault;
    probe.savedDirectory = destination;
    probe.failedFrame = NumberedPath(prefix, 4);
    StartMenu(app);
    DriveNatural(app);
    IdlePump(app, 8);
    Check(probe.writes == 3 && probe.timingOk && probe.deltaOk,
        "three frames are written with correct timing before the real fourth-write failure");
    Check(probe.errors == 1 && probe.finishes == 1 && probe.finishReturned && !probe.finishSucceeded,
        "one real write error ends recording as incomplete");
    Check(Stopped(app) && probe.recording && !IsWindow(probe.recording),
        "the failure destroys the recording window and releases capture");
    Check(IsWindow(probe.main) && !probe.posts && !probe.deliveries && !probe.quits && !probe.confirmations,
        "the editor stays open with no close request or cancellation prompt");
    CheckRestored(app);

    const auto preserved = probe.savedDirectory;
    const auto frames = probe.savedFrames;
    const auto beforeDirty = probe.savedDirty;
    const auto beforeModelDirty = probe.savedModelDirty;
    const auto codePosition = probe.writeError.rfind(L" (0x");
    const auto code = codePosition == std::wstring::npos ? std::wstring{} : probe.writeError.substr(codePosition);
    Check(fault != WriteFault::FolderRemoved || !code.empty(), "the write warning includes a Win32 failure code");
    probe.caseActive = false;
    const auto retryStats = RunNatural(app, 24, 17, 22, 5, false, prefix, retry);
    Check(projectPath == app->EnvFileName() && app->SceneGeneration() == projectGeneration &&
        app->SceneModified() == beforeDirty && app->EnhancedModelDirty() == beforeModelDirty,
        "the failure and retry keep the same project and its dirty flags");

    probe.directory = preserved;
    CheckExistingFile(preserved / L"keep.txt");
    for (unsigned n = 1; n <= 3; ++n)
        Check(ReadBytes(NumberedPath(prefix, n)) == frames[n - 1],
            "the output already published before the failure stays byte-identical after retry");
    if (fault == WriteFault::LateCollision) {
        CheckExistingFile(NumberedPath(prefix, 4));
        Check(!fs::exists(NumberedPath(prefix, 5)), "no sample is written after the collision");
    }
    VerifyImages(prefix, 3, fault == WriteFault::LateCollision ? 1 : 0);
    std::printf("SEQUENCE-WRITE-FAILURE case=%ls writes=3 errors=1 closePosts=0 retryDecoded=%u dirty=%u%ls\n",
        label, retryStats.decoded, beforeDirty, code.c_str());
}

bool RunFinalClose(MMDApp* app) {
    const std::wstring prefix = L"seq_life_final_";
    probe.forwardClose = true;
    std::printf("SEQUENCE-LIFECYCLE begin case=final-real-close fps=24 range=17..22 expected=5 close=1 cancel=0\n");
    ResetCase(24, 17, 22, true, prefix);
    StartMenu(app);
    DriveNatural(app);
    Check(!app->RecordingWindow() && !seq::Active(app), "the final natural export releases the recording session");
    Check(Stopped(app), "the final natural export performs the full recording teardown");
    Check(probe.recording != nullptr && !IsWindow(probe.recording),
        "the final natural export destroys its recording window");
    Check(probe.writes == 5, "the final export writes the independent five-frame oracle");
    Check(probe.timingOk && probe.deltaOk, "the final export samples follow the requested start frame and FPS");
    Check(probe.finishes == 1 && probe.finishReturned && probe.finishSucceeded,
        "the final export finalizer reports success");
    Check(probe.confirmations == 0, "the final export asks no cancel confirmation");
    VerifyImages(prefix, 5);
    // The final close must consume the same clean project the matrix started
    // with; never clear the flags here to hide a leaked modification.
    CheckRestored(app);
    Check(app->SceneModified() == 0 && app->EnhancedModelDirty() == 0,
        "the final close is a saved-project close without an unsaved prompt");
    // Only this final case lets the real host consume WM_CLOSE. The sink still
    // records the one delivery and asserts that teardown already completed.
    const auto deadline = GetTickCount64() + 15000;
    while (probe.quits == 0 && GetTickCount64() < deadline) {
        MSG message{};
        bool dispatched = false;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                Check(message.wParam == 0, "WM_QUIT carries the normal zero exit code");
                ++probe.quits;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            dispatched = true;
            if (!IsWindow(probe.main)) break;
        }
        if (!dispatched && probe.quits == 0) Sleep(1);
    }
    const bool mainGone = !IsWindow(probe.main);
    const bool recordingGone = !IsWindow(probe.recording);
    const bool quit = probe.quits == 1;
    if (mainGone && recordingGone && quit)
        Check(probe.posts == 1 && probe.deliveries == 1,
            "the final natural success posts and delivers one real close request");
    else
        std::printf("SEQUENCE-LIFECYCLE blocked=real-close mainAlive=%d recordingAlive=%d quit=%u posts=%u deliveries=%u\n",
            mainGone ? 0 : 1, recordingGone ? 0 : 1, probe.quits, probe.posts, probe.deliveries);
    std::printf("SEQUENCE-LIFECYCLE end case=final-real-close fps=24 range=17..22 expected=5 writes=%u decoded=5 finish=%d closePost=%u closeDelivery=%u confirm=%u retry=0 realQuit=%u\n",
        probe.writes, probe.finishSucceeded ? 1 : 0, probe.posts, probe.deliveries,
        probe.confirmations, probe.quits);
    probe.caseActive = false;
    probe.forwardClose = false;
    return mainGone && recordingGone && quit;
}

bool Run(MMDApp* app, bool checkInterfaceScale, bool checkWriteFailures, bool checkProjectSave) {
    wchar_t modelPath[32768]{};
    const auto length = GetEnvironmentVariableW(L"MMS_MENU_SEQUENCE_MODEL", modelPath, std::size(modelPath));
    Check(length > 0 && length < std::size(modelPath), "the parent supplies the synthetic sequence model fixture");
    Check(fs::is_regular_file(modelPath), "the synthetic sequence model fixture is a real file");
    LoadModelFile(app, modelPath, false);
    Check(app->SelectedModel() != nullptr, "the synthetic sequence model fixture loads");
    app->CameraMode() = 1;
    app->state.followCameraEnabled = 0;
    app->state.sceneModified = 0;
    app->EnhancedModelDirty() = 0;
    probe.checkInterfaceScale = checkInterfaceScale;
    probe.checkProjectSave = checkProjectSave;
    if (checkInterfaceScale) {
        Check(SetWindowPos(app->MainWindow(), nullptr, 0, 0, 1200, 820,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "size the hidden editor for the UI-scale fixture");
        Check(ui_scale_popup_fixture::Choose(app->MainWindow(), 200), "select 200 percent through the real Help popup");
    }
    InstallHooks(app);
    if (checkWriteFailures || checkProjectSave) {
        if (!DefaultEffectReadyForCapture(app)) {
            std::puts("SEQUENCE-WRITE-FAILURE waiting for the production self-shadow shader");
            const auto deadline = GetTickCount64() + 540000;
            while (!DefaultEffectReadyForCapture(app)) {
                Check(GetTickCount64() < deadline, "the self-shadow shader becomes ready before the write-failure test");
                Sleep(100);
            }
            std::puts("SEQUENCE-WRITE-FAILURE production self-shadow shader ready");
        }
        if (checkProjectSave) {
            app->SceneModified() = 1;
            const bool closed = RunFinalClose(app);
            RemoveHooks();
            return closed;
        }
        RunWriteFault(app, WriteFault::LateCollision);
        app->SceneModified() = 1;
        RunWriteFault(app, WriteFault::FolderRemoved);
        RemoveHooks();
        return true;
    }
    unsigned decodedTotal = 0;
    for (int fps : {24, 60}) {
        const unsigned shortCount = fps == 24 ? 5u : 12u;
        const std::wstring tag = std::to_wstring(fps);
        const std::wstring closePrefix = L"seq_life_" + tag + L"_close_";
        const std::wstring keepPrefix = L"seq_life_" + tag + L"_keep_";
        const std::wstring cancelPrefix = L"seq_life_" + tag + L"_cancel_";
        const std::wstring retryPrefix = L"seq_life_" + tag + L"_retry_";
        std::printf("SEQUENCE-LIFECYCLE begin case=%ls-close fps=%d range=17..22 expected=%u close=1 cancel=0\n",
            tag.c_str(), fps, shortCount);
        const CaseStats closeStats = RunNatural(app, fps, 17, 22, shortCount, true, closePrefix);
        decodedTotal += closeStats.decoded;
        std::printf("SEQUENCE-LIFECYCLE end case=%ls-close fps=%d range=17..22 expected=%u writes=%u decoded=%u finish=%d closePost=%u closeDelivery=%u confirm=%u retry=0 realQuit=%u\n",
            tag.c_str(), fps, shortCount, closeStats.writes, closeStats.decoded,
            closeStats.finishSucceeded ? 1 : 0, closeStats.posts, closeStats.deliveries,
            closeStats.confirmations, closeStats.quits);
        std::printf("SEQUENCE-LIFECYCLE begin case=%ls-keep fps=%d range=17..22 expected=%u close=0 cancel=0\n",
            tag.c_str(), fps, shortCount);
        const CaseStats keepStats = RunNatural(app, fps, 17, 22, shortCount, false, keepPrefix);
        decodedTotal += keepStats.decoded;
        std::printf("SEQUENCE-LIFECYCLE end case=%ls-keep fps=%d range=17..22 expected=%u writes=%u decoded=%u finish=%d closePost=%u closeDelivery=%u confirm=%u retry=0 realQuit=%u\n",
            tag.c_str(), fps, shortCount, keepStats.writes, keepStats.decoded,
            keepStats.finishSucceeded ? 1 : 0, keepStats.posts, keepStats.deliveries,
            keepStats.confirmations, keepStats.quits);
        std::printf("SEQUENCE-LIFECYCLE begin case=%ls-cancel fps=%d range=17..46 expected=3 close=1 cancel=1\n",
            tag.c_str(), fps);
        const CancelResult cancelResult = RunCancel(app, fps, 17, 46, cancelPrefix, retryPrefix, shortCount);
        decodedTotal += cancelResult.cancel.decoded + cancelResult.retry.decoded;
        std::printf("SEQUENCE-LIFECYCLE retry case=%ls-cancel fps=%d range=17..22 expected=%u writes=%u decoded=%u finish=%d closePost=%u closeDelivery=%u confirm=%u\n",
            tag.c_str(), fps, shortCount, cancelResult.retry.writes, cancelResult.retry.decoded,
            cancelResult.retry.finishSucceeded ? 1 : 0, cancelResult.retry.posts,
            cancelResult.retry.deliveries, cancelResult.retry.confirmations);
        std::printf("SEQUENCE-LIFECYCLE end case=%ls-cancel fps=%d range=17..46 expected=3 writes=%u decoded=%u finish=%d closePost=%u closeDelivery=%u confirm=%u retry=%u realQuit=%u\n",
            tag.c_str(), fps, cancelResult.cancel.writes, cancelResult.cancel.decoded,
            cancelResult.cancel.finishSucceeded ? 1 : 0, cancelResult.cancel.posts,
            cancelResult.cancel.deliveries, cancelResult.cancel.confirmations,
            cancelResult.retry.decoded, cancelResult.cancel.quits);
    }
    const bool realClosed = RunFinalClose(app);
    decodedTotal += 5;
    RemoveHooks();
    std::printf("SEQUENCE-LIFECYCLE SUMMARY pass=%d coordinator=%d checks=%u decoded=%u\n",
        realClosed ? 1 : 0, coordinator_fixture::Enabled() ? 1 : 0, checks, decodedTotal);
    return realClosed;
}
} // namespace sequence_lifecycle

static int RunImageSequenceTests(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(argc == 2 || argc == 3, "isolated artifact directory required"); output = fs::absolute(argv[1]);
    fs::create_directories(output);
    const bool scaledSequence = argc == 3 && wcscmp(argv[2], L"--sequence-ui-scale-only") == 0;
    const bool writeFailures = argc == 3 && wcscmp(argv[2], L"--sequence-write-failures") == 0;
    const bool saveRetry = argc == 3 && wcscmp(argv[2], L"--sequence-save-retry") == 0;
    const bool sceneBoundary = argc == 3 && wcscmp(argv[2], L"--sequence-modal-scene") == 0;
    const bool stillBoundary = argc == 3 && wcscmp(argv[2], L"--still-modal-scene") == 0;
    wchar_t desktop[256]{}; DWORD bytes = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof desktop, &bytes) &&
        (_wcsicmp(desktop, L"Default") != 0 || (argc == 3 &&
            (wcscmp(argv[2], L"--hidden-production") == 0 || wcscmp(argv[2], L"--still-only") == 0 ||
                wcscmp(argv[2], L"--still-failures-only") == 0 || wcscmp(argv[2], L"--still-overwrite-only") == 0 ||
                wcscmp(argv[2], L"--still-overwrite-failure") == 0 ||
                wcscmp(argv[2], L"--sequence-lifecycle-only") == 0 || scaledSequence || writeFailures || saveRetry ||
                sceneBoundary || stillBoundary))),
        "private desktop or explicit hidden GPU test required");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "initialize message guards");
    if (argc == 3 && wcscmp(argv[2], L"--still-shell-only") == 0) {
        still_shell::RunOnPrivateDesktop();
        Check(MH_Uninitialize() == MH_OK, "release the private Shell fixture hooks");
        return 0;
    }
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&MessageW), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(&MessageA), nullptr);
    Check(MH_CreateHook(reinterpret_cast<void*>(&ShowWindow), reinterpret_cast<void*>(&Hidden), reinterpret_cast<void**>(&originalShowWindow)) == MH_OK, "install visibility guard");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable visibility guard");
    auto hide = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Check(hide != nullptr, "install activation guard");
    auto* app = new MMDApp; g_Block = app; app->state = MMDAppState{}; app->InitDefaults(); app->EnglishUI() = 2;
    dialogOnly = argc == 3 && wcscmp(argv[2], L"--dialogs-only") == 0;
    if (dialogOnly) {
        app->MainWindow() = CreateWindowW(L"STATIC", L"Image sequence dialog fixture", WS_POPUP,
            0, 0, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(app->MainWindow() != nullptr, "create hidden dialog owner");
        SetMenu(app->MainWindow(), LoadMenuW(GetModuleHandleW(nullptr), L"SAMPLE02E"));
        DialogTests(app);
        DestroyWindow(app->MainWindow()); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        std::printf("PASS image-sequence visible private-dialog checks=%u\n", checks);
        return 0;
    }
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize production renderer");
    Check(!IsWindowVisible(app->MainWindow()), "production host remains hidden");
    if (stillBoundary) {
        StillSceneBoundaryTests(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        return 0;
    }
    if (sceneBoundary) {
        MenuEntryTests(app, true);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        return 0;
    }
    if (argc == 3 && wcscmp(argv[2], L"--still-overwrite-failure") == 0) {
        still_shell::RunOverwriteFailure(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        std::puts("PASS still overwrite failures preserve the image and allow a retry");
        return 0;
    }
    if (argc == 3 && wcscmp(argv[2], L"--still-overwrite-only") == 0) {
        still_shell::RunCapture(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        std::puts("PASS still export overwrites the Shell-approved target with a real JPEG");
        return 0;
    }
    if (argc == 3 && wcscmp(argv[2], L"--still-failures-only") == 0) {
        TransparentStillFailures(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        std::printf("PASS automatic still-image failure checks=%u\n", checks);
        return 0;
    }
    if (argc == 3 && wcscmp(argv[2], L"--still-only") == 0) {
        StillImageLanguageTests(app);
        const auto pixels = ImagePixels(output / L"单张_0_0.png");
        unsigned clear = 0;
        for (std::size_t i = 3; i < pixels.size(); i += 4) clear += pixels[i] == 0;
        Check(clear > 10, "single-image PNG background becomes transparent automatically");
        StillImageReentryTests(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        std::printf("PASS automatic still-image checks=%u\n", checks);
        return 0;
    }
    if (scaledSequence || writeFailures || saveRetry || (argc == 3 && wcscmp(argv[2], L"--sequence-lifecycle-only") == 0)) {
        if (!sequence_lifecycle::Run(app, scaledSequence, writeFailures, saveRetry)) {
            std::printf("FAIL image-sequence lifecycle real close was not observed\n");
            return 1;
        }
        ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
        std::printf("PASS image-sequence lifecycle checks=%u\n", checks);
        return 0;
    }
    NamesAndValidation(app); SceneRangeTests(app); DialogTests(app); app->EnglishUI() = 2;
    MenuEntryTests(app);
    CheckImageExportRenderState(app);
    CameraAndContinuityTests(app);
    StillImageLanguageTests(app);
    TransparentImages(app);
    TransparentStillFailures(app);
    seq::Options o; o.directory = output.wstring(); o.width = 64; o.height = 48; o.end = 1;
    for (int format = 0; format < std::size(seq::kFormats); ++format) {
        o.format = format; o.prefix = L"中文_" + std::to_wstring(format) + L"_";
        RunExport(app, o);
        std::wstring error;
        const auto originalSize = fs::file_size(seq::FramePath(o, 1));
        Check(!seq::Prepare(app, o, error) && !error.empty() && !seq::Active(app), "existing image blocks export");
        Check(fs::file_size(seq::FramePath(o, 1)) == originalSize, "existing image unchanged");
    }
    o.format = 2; o.start = 10; o.end = 39;
    for (float fps : {24.f, 30.f, 60.f, 29.97f}) {
        o.fps = fps; o.prefix = L"fps_" + std::to_wstring(fps) + L"_"; RunExport(app, o);
    }
    o.fps = 30; o.prefix = L"cancel_"; RunExport(app, o, true);
    o.prefix = L"collision_"; std::wstring error;
    Check(seq::Prepare(app, o, error), "prepare before late collision");
    { std::ofstream sentinel(fs::path(seq::FramePath(o, 1))); sentinel << "keep"; }
    Check(!seq::WriteFrame(app, app->Renderer()->backbufferSurface, error) && !error.empty(), "late collision stops output");
    Check(fs::file_size(seq::FramePath(o, 1)) == 4, "late collision cannot overwrite image");
    seq::Abort(app);
    o.prefix = L"incomplete_"; Check(seq::Prepare(app, o, error), "prepare incomplete sequence");
    Check(!seq::Finish(app, true, error) && !error.empty() && !seq::Active(app), "incomplete sequence cannot report success");
    CheckTemporalImageSequence(app);
    for (const auto& entry : fs::directory_iterator(output)) Check(entry.path().extension() != L".tmp", "no temporary frames remain");
    ShutdownCleanup(app); g_Block = nullptr; delete app; UnhookWindowsHookEx(hide);
    std::printf("PASS image-sequence checks=%u\n", checks);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || std::wcscmp(argv[2], L"--still-shell-only") != 0)
        return RunImageSequenceTests(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto previous = GetThreadDesktop(GetCurrentThreadId());
    const auto name = L"MmsStillExport-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const auto desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(desktop && SetThreadDesktop(desktop), "attach to a fresh private desktop");
    BOOL input = TRUE;
    DWORD bytes = 0;
    Check(GetUserObjectInformationW(desktop, UOI_IO, &input, sizeof input, &bytes) && !input,
        "the test desktop is not the input desktop");
    std::puts("STILL-SHELL private desktop confirmed inactive");
    const int result = RunImageSequenceTests(argc, argv);
    Check(SetThreadDesktop(previous) && CloseDesktop(desktop), "release the owned still export desktop");
    std::puts("STILL-SHELL private desktop released");
    return result;
}
