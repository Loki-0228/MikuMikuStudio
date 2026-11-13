// MAINTENANCE: regression-keep; video export must not append defaults to frame ranges.
// Real dialog resources and AviOutDlgProc run on an inactive private desktop.
// Codec enumeration and the successful pick are controlled. Cancellation
// also runs the native picker. The overwrite case uses an owned file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#include <MinHook.h>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/dshow_recorder.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mf_h264_recorder.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/wave_audio_context.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace mikumikustudio {
INT_PTR __stdcall AviOutDlgProc(HWND, UINT, WPARAM, LPARAM);
int FillCodecCombo(DShowRecorder*, HWND, char);
void CmdFileMenu(MMDApp*, HWND, std::uint16_t, std::uint16_t);
}
using namespace mikumikustudio;
namespace {
unsigned checks = 0, failures = 0, codecCalls = 0;
unsigned pathWarnings = 0, invalidParameters = 0;
const wchar_t* rejectedPath = nullptr;
const wchar_t* overwritePath = nullptr;
unsigned overwritePrompts = 0;
unsigned backgroundWarnings = 0;
const wchar_t* protectedBackground = nullptr;
int overwriteAnswer = IDNO;
MMDApp* replaceSceneOnOverwrite = nullptr;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
void Require(bool value, const char* label) {
    Check(value, label);
    if (!value) std::exit(2);
}

// A controlled codec row isolates the range dialog from installed encoders.
// IDOK still executes the real AVI/MP4 choice and output-path handling.
int FixtureCodec(DShowRecorder* recorder, HWND combo, char) {
    ++codecCalls;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const auto row = SendMessageW(combo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(MMD_ORIGINAL_PROFILE ? L"AVI Raw" : L"H.264 (MP4)"));
    const int codec = MMD_ORIGINAL_PROFILE ? -1 : kBuiltInH264Codec;
    SendMessageW(combo, CB_SETITEMDATA, row, codec);
    SendMessageW(combo, CB_SETCURSEL, row, 0);
    if (recorder) recorder->selectedCodec = codec;
    return static_cast<int>(row);
}
int WINAPI UnexpectedMessageA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("FAIL unexpected MessageBoxA: %s\n", text ? text : "");
    std::exit(2);
}
int WINAPI UnexpectedMessageW(HWND, LPCWSTR text, LPCWSTR, UINT flags) {
    if (protectedBackground && text && std::wcsstr(text, protectedBackground) &&
        std::wcsstr(text, L"background video") && flags == (MB_OK | MB_ICONWARNING)) {
        ++backgroundWarnings;
        return IDOK;
    }
    if (rejectedPath && text && std::wcsstr(text, rejectedPath) && std::wcsstr(text, L"Win32 206")) {
        ++pathWarnings;
        return IDOK;
    }
    if (overwritePath && text && std::wcsstr(text, overwritePath) &&
        flags == (MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2)) {
        ++overwritePrompts;
        if (replaceSceneOnOverwrite) replaceSceneOnOverwrite->AdvanceSceneGeneration();
        return overwriteAnswer;
    }
    std::fwprintf(stderr, L"FAIL unexpected MessageBoxW: %ls\n", text ? text : L"");
    std::exit(2);
}
void CountInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    ++invalidParameters;
}
template<class F> void Hook(F target, F replacement) {
    Require(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement), nullptr) == MH_OK,
            "install isolated dialog dependency or unexpected-message guard");
}

struct RangeCase {
    const char* name;
    const wchar_t* mainStart;
    const wchar_t* mainEnd;
    unsigned lastFrame;
    int audioBytes;
    unsigned audioBytesPerSecond;
    int expectedStart;
    int expectedEnd;
};
const RangeCase cases[] = {
    {"reported-1100-plus-1", L"0", L"1100", 1, 0, 0, 0, 1100},
    {"explicit-plus-zero", L"", L"1100", 0, 0, 0, 0, 1100},
    {"explicit-short-range", L"35", L"120", 3000, 60000, 300, 35, 120},
    {"issue-48-playback-range", L"6559", L"7448", 8000, 0, 0, 6559, 7448},
    {"pasted-playback-range", L" 6559 ", L"7448 ", 8000, 0, 0, 6559, 7448},
    {"relative-stop", L"35", L"-2", 120, 0, 0, 35, 119},
    {"negative-start-clamps-to-zero", L"-10", L"120", 0, 0, 0, 0, 120},
    {"zero-falls-back-to-motion", L"10", L"0", 1100, 0, 0, 10, 1100},
    {"empty-falls-back-to-motion", L"3", L"", 42, 0, 0, 3, 42},
    {"audio-rounded-up", L"5", L"0", 5, 1001, 300, 5, 101},
    {"motion-longer-than-audio", L"0", L"0", 300, 1001, 300, 0, 300},
    {"invalid-audio-rate", L"0", L"0", 7, 1001, 0, 0, 7},
    {"start-past-fallback-end", L"10", L"", 7, 0, 0, 10, 10},
};

struct Context {
    MMDApp* app;
    const RangeCase* range;
    unsigned opening;
    bool cancel;
    const wchar_t* expectedFps;
    const wchar_t* enteredFps;
    bool initialized = false;
};
std::wstring Text(HWND dialog, int id) {
    wchar_t text[96]{};
    GetDlgItemTextW(dialog, id, text, static_cast<int>(std::size(text)));
    return text;
}
void CheckChineseFpsLabel(HWND dialog) {
    constexpr wchar_t unit[] = L"帧/秒";
    const HWND label = FindWindowExW(dialog, nullptr, L"Static", unit);
    Require(label != nullptr, "find the translated frame-rate unit");
    HDC dc = GetDC(label);
    Require(dc != nullptr, "get a device context for the frame-rate label");
    const auto font = reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0));
    const HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SIZE extent{};
    const bool measured = GetTextExtentPoint32W(dc, unit, static_cast<int>(std::size(unit) - 1), &extent) != 0;
    if (previous) SelectObject(dc, previous);
    ReleaseDC(label, dc);
    RECT bounds{};
    Check(measured && GetClientRect(label, &bounds) && bounds.right >= extent.cx,
          "the Chinese frame-rate unit fits its control");
}
INT_PTR CALLBACK Probe(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message != WM_INITDIALOG)
        return context ? AviOutDlgProc(dialog, message, wp, lp) : FALSE;
    context = reinterpret_cast<Context*>(lp);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
    AviOutDlgProc(dialog, WM_INITDIALOG, wp, 0);
    context->initialized = true;
    if (context->app->EnglishUI() == 2) CheckChineseFpsLabel(dialog);
    const auto actual = Text(dialog, panel::kAviEndFrameEdit);
    std::printf("CASE language=%u name=%s opening=%u end='%ls' expected=%d action=%s\n",
                unsigned(context->app->EnglishUI()), context->range->name,
                context->opening, actual.c_str(), context->range->expectedEnd,
                context->cancel ? "cancel" : "ok");
    std::printf("DIMENSIONS render=%dx%d width='%ls' height='%ls' fps='%ls'\n",
                context->app->RenderWidth(), context->app->RenderHeight(),
                Text(dialog, panel::kAviWidthEdit).c_str(),
                Text(dialog, panel::kAviHeightEdit).c_str(),
                Text(dialog, panel::kAviFpsEdit).c_str());
    Check(actual == std::to_wstring(context->range->expectedEnd), "default end is one complete range value");
    Check(Text(dialog, panel::kAviStartFrameEdit) == std::to_wstring(context->range->expectedStart),
          "default start uses the current playback range");
    Check(Text(dialog, panel::kAviFpsEdit) == context->expectedFps,
          "default FPS uses the last confirmed video frame rate");
    Check(Text(dialog, panel::kAviWidthEdit) == std::to_wstring(context->app->RenderWidth()) &&
          Text(dialog, panel::kAviHeightEdit) == std::to_wstring(context->app->RenderHeight()),
          "initial dimensions match the render size without inserted digits");
    Check(context->app->AviRecordStartFrame() == 303 &&
          context->app->AviRecordEndFrame() == 404 && context->app->AviRecordFps() == 24,
          "initialization does not commit the pending frame range or FPS");
    Check(!IsWindowVisible(dialog), "dialog stays hidden before its first display");
    if (context->cancel) {
        SetDlgItemInt(dialog, panel::kAviStartFrameEdit, 22, FALSE);
        SetDlgItemInt(dialog, panel::kAviEndFrameEdit, 99999, FALSE);
        SetDlgItemTextW(dialog, panel::kAviFpsEdit, L"99");
    } else if (context->enteredFps) {
        SetDlgItemTextW(dialog, panel::kAviFpsEdit, context->enteredFps);
    }
    // EndDialog runs inside initialization, before the modal loop can show it.
    // No menu 223 continuation is invoked, so no recorder or output file starts.
    const auto promptCount = overwritePrompts;
    const std::wstring pendingPath = context->app->AviOutputPath();
    AviOutDlgProc(dialog, WM_COMMAND, context->cancel ? IDCANCEL : IDOK, 0);
    if (overwritePath) {
        Check(overwritePrompts == promptCount + 1, "ask before writing the existing destination with the codec's extension");
        if (overwritePrompts == promptCount + 1) {
            Check(context->app->AviOutputPath() == pendingPath &&
                  context->app->AviRecordStartFrame() == 303 &&
                  context->app->AviRecordEndFrame() == 404 &&
                  context->app->AviRecordFps() == 24, "declining preserves the chosen path and prior recording settings");
            overwriteAnswer = IDYES;
            AviOutDlgProc(dialog, WM_COMMAND, IDOK, 0);
            Check(overwritePrompts == promptCount + 2, "retry in the same settings window confirms the final path again");
        }
    }
    return TRUE;
}

void Run(MMDApp* app, const RangeCase& range, unsigned opening,
         const wchar_t* expectedFps, const wchar_t* enteredFps = nullptr, bool cancel = false) {
    // Reopening also covers an even output size with a nonstandard aspect ratio.
    app->RenderWidth() = opening == 1 ? 1918 : 1920;
    app->RenderHeight() = 1080;
    WaveAudioContext audio{};
    audio.dataSize = range.audioBytes;
    audio.format.nAvgBytesPerSec = range.audioBytesPerSecond;
    wcscpy_s(audio.path, L"fixture.wav");
    app->Audio() = range.audioBytes ? &audio : nullptr;
    app->LastRegisteredFrame() = range.lastFrame;
    SetDlgItemTextW(app->MainWindow(), panel::kPlayStartFrameEdit, range.mainStart);
    SetDlgItemTextW(app->MainWindow(), panel::kPlayStopFrameEdit, range.mainEnd);
    app->AviRecordStartFrame() = 303;
    app->AviRecordEndFrame() = 404;
    app->AviRecordFps() = 24;
    wcscpy_s(app->AviOutputPath(), 256, L"video-export-range-test.avi");
    const std::wstring oldPath = app->AviOutputPath();
    Context context{app, &range, opening, cancel, expectedFps, enteredFps};
    // These are the production JP / EN resources; Chinese uses the EN template
    // and the actual dialog's LocalizeChineseDialog call, as menu 223 does.
    const auto resource = MAKEINTRESOURCEA(app->EnglishUI() ? 654 : 608);
    const INT_PTR result = UiDialogBoxParamA(GetModuleHandleW(nullptr), resource,
        app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&context));
    Require(context.initialized && result != -1, "create and initialize the real video-export dialog");
    Check(result == (cancel ? IDCANCEL : IDOK), "production command closes the dialog with the expected result");
    if (cancel) {
        Check(app->AviRecordStartFrame() == 303 && app->AviRecordEndFrame() == 404 &&
              app->AviRecordFps() == 24 && oldPath == app->AviOutputPath(),
              "cancel preserves committed range, FPS and output path");
    } else {
        std::printf("COMMIT start=%d end=%d fps=%g\n", app->AviRecordStartFrame(),
                    app->AviRecordEndFrame(), app->AviRecordFps());
        Check(app->AviRecordStartFrame() == range.expectedStart &&
              app->AviRecordEndFrame() == range.expectedEnd &&
              app->AviRecordFps() == std::wcstof(enteredFps ? enteredFps : expectedFps, nullptr),
              "IDOK reads the displayed range and selected FPS");
        Check(app->AviOutputWidth() == app->RenderWidth() && app->AviOutputHeight() == app->RenderHeight(),
              "IDOK reads the original render dimensions without inserted digits");
        Check(std::wstring(app->AviOutputPath()) == (MMD_ORIGINAL_PROFILE
                ? L"video-export-range-test.avi" : L"video-export-range-test.mp4"),
              "IDOK follows the profile's real AVI or MP4 path selection");
    }
    Check(app->Renderer()->device == nullptr && app->Recorder()->graph == nullptr &&
          app->Recorder()->framePush == nullptr, "range test creates no GPU or recording graph");
    app->Audio() = nullptr;
}

void CheckOutputPathBoundary(MMDApp* app, const wchar_t* fps) {
    const auto& range = cases[0];
    SetDlgItemTextW(app->MainWindow(), panel::kPlayStartFrameEdit, range.mainStart);
    SetDlgItemTextW(app->MainWindow(), panel::kPlayStopFrameEdit, range.mainEnd);
    const auto resource = MAKEINTRESOURCEA(app->EnglishUI() ? 654 : 608);
    const std::wstring longestStem(kPathFieldCapacity - 5, L'x');
    const std::wstring tooLong = longestStem + L"x.x";
    const auto oldHandler = _set_thread_local_invalid_parameter_handler(CountInvalidParameter);
    const auto run = [&](const std::wstring& path) {
        wcscpy_s(app->AviOutputPath(), kPathFieldCapacity, path.c_str());
        app->AviRecordStartFrame() = 303;
        app->AviRecordEndFrame() = 404;
        app->AviRecordFps() = 24;
        Context context{app, &range, 0, false, fps, nullptr};
        const auto result = UiDialogBoxParamA(GetModuleHandleW(nullptr), resource,
            app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&context));
        Require(context.initialized, "initialize the output-path boundary dialog");
        return result;
    };
    const auto warningsBefore = pathWarnings;
    const auto invalidBefore = invalidParameters;
    rejectedPath = tooLong.c_str();
    Check(run(tooLong) == IDCANCEL, "a path that will overflow after extension selection cancels export");
    rejectedPath = nullptr;
    Check(pathWarnings == warningsBefore + 1 && invalidParameters == invalidBefore,
          "show one path-specific warning without calling the CRT invalid-parameter handler");
    Check(app->AviOutputPath() == tooLong && app->AviRecordStartFrame() == 303 &&
          app->AviRecordEndFrame() == 404 && app->AviRecordFps() == 24,
          "the rejected path and committed recording settings remain intact");
    Check(run(longestStem + L".x") == IDOK && app->AviOutputPath() ==
          longestStem + (MMD_ORIGINAL_PROFILE ? L".avi" : L".mp4"),
          "a retry at the exact field limit commits the correct container extension");
    _set_thread_local_invalid_parameter_handler(oldHandler);
}

void CheckOverwriteAfterCodecChange(MMDApp* app, const wchar_t* fps) {
    namespace fs = std::filesystem;
    const auto directory = fs::absolute(L"video-container-probe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(app->EnglishUI()));
    Require(fs::create_directory(directory), "create an independent destination directory");
    const auto requested = directory / (MMD_ORIGINAL_PROFILE ? L"已有.mp4" : L"已有.avi");
    const auto destination = directory / (MMD_ORIGINAL_PROFILE ? L"已有.avi" : L"已有.mp4");
    Require(requested.native().size() < kPathFieldCapacity, "destination fits the legacy path field");
    std::ofstream(destination, std::ios::binary) << "existing destination";
    Require(fs::file_size(destination) == sizeof("existing destination") - 1, "write the existing destination sentinel");
    wcscpy_s(app->AviOutputPath(), kPathFieldCapacity, requested.c_str());
    app->AviRecordStartFrame() = 303;
    app->AviRecordEndFrame() = 404;
    app->AviRecordFps() = 24;
    overwritePath = destination.c_str();
    overwriteAnswer = IDNO;
    const auto& range = cases[0];
    Context context{app, &range, 0, false, fps, nullptr};
    const auto resource = MAKEINTRESOURCEA(app->EnglishUI() ? 654 : 608);
    const auto result = UiDialogBoxParamA(GetModuleHandleW(nullptr), resource,
        app->MainWindow(), Probe, reinterpret_cast<LPARAM>(&context));
    overwritePath = nullptr;
    Check(context.initialized && result == IDOK && destination.native() == app->AviOutputPath(),
          "accepting commits the exact confirmed filename");
    std::ifstream file(destination, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(file), {}};
    Check(bytes == "existing destination" && !fs::exists(requested),
          "path confirmation never touches the picked file or the existing destination");
}

enum class VideoModalAction { Cancel, ReplaceInPicker, ReplaceInSettings, ReplaceInOverwrite,
                              ReplaceInDeclinedOverwrite, Accept, ChangeProtectedBackground };
struct ShellCancelProbe {
    MMDApp* app;
    const wchar_t* path;
    VideoModalAction action = VideoModalAction::Cancel;
    bool sawPicker = false;
    bool sawSettings = false;
    const wchar_t* alternate = nullptr;
    HWND settings = nullptr;
    bool sawAlternate = false;
};
ShellCancelProbe* activeShellProbe = nullptr;
decltype(&GetUiSaveFileNameW) realSaveFileName = nullptr;
unsigned staleSaveCalls = 0, videoStarts = 0;
bool ObserveSaveAs(MMDApp*, HWND) { ++staleSaveCalls; return false; }
void ObserveVideoStart(MMDApp*, const recording_output_path::Selection*) { ++videoStarts; }

BOOL ControlledVideoPick(LPOPENFILENAMEW parameters) {
    auto* probe = activeShellProbe;
    if (!probe || !probe->path) return realSaveFileName(parameters);
    if (probe->alternate && parameters->hwndOwner == probe->settings) {
        probe->sawAlternate = true;
        return wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, probe->alternate) == 0;
    }
    Require(parameters->hwndOwner == probe->app->MainWindow() &&
            ((parameters->Flags & OFN_OVERWRITEPROMPT) != 0) == bool(MMD_ORIGINAL_PROFILE) &&
            wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, probe->path) == 0,
            "return a controlled successful choice for the video settings cancellation case");
    probe->sawPicker = true;
    if (probe->action == VideoModalAction::ReplaceInPicker) probe->app->AdvanceSceneGeneration();
    return TRUE;
}

BOOL CALLBACK DriveShellWindow(HWND window, LPARAM data) {
    auto& probe = *reinterpret_cast<ShellCancelProbe*>(data);
    const auto owner = GetWindow(window, GW_OWNER);
    wchar_t type[24]{};
    if (!GetClassNameW(window, type, static_cast<int>(std::size(type))) ||
        std::wcscmp(type, L"#32770") != 0) return TRUE;
    if (owner != probe.app->MainWindow()) return TRUE;
    if (GetDlgItem(window, panel::kAviStartFrameEdit)) {
        if (probe.sawSettings) return TRUE;
        probe.sawSettings = true;
        Check(std::wcscmp(probe.app->AviOutputPath(), L"previous-output.avi") == 0,
              "the candidate path stays private while settings are open");
        SetDlgItemInt(window, panel::kAviStartFrameEdit, 45, FALSE);
        SetDlgItemInt(window, panel::kAviEndFrameEdit, 47, FALSE);
        if (probe.action == VideoModalAction::ChangeProtectedBackground) {
            probe.settings = window;
            const auto starts = videoStarts;
            const auto warnings = backgroundWarnings;
            SendMessageW(window, WM_COMMAND, IDOK, 0);
            Require(backgroundWarnings == warnings + 1 && videoStarts == starts &&
                    std::wstring(probe.app->AviOutputPath()) == L"previous-output.avi",
                    "the current background is refused before committing parameters or starting output");
            const HWND change = GetDlgItem(window, panel::kVideoChangeDestinationButton);
            Require(change && (GetWindowLongPtrW(change, GWL_STYLE) & WS_TABSTOP),
                    "the protected output can be changed from the same keyboard-accessible dialog");
            SendMessageW(change, BM_CLICK, 0, 0);
            Require(probe.sawAlternate && GetDlgItemInt(window, panel::kAviStartFrameEdit, nullptr, FALSE) == 45 &&
                    GetDlgItemInt(window, panel::kAviEndFrameEdit, nullptr, FALSE) == 47,
                    "changing destination preserves the entered frame range");
        }
        if (probe.action == VideoModalAction::ReplaceInSettings) {
            probe.app->AdvanceSceneGeneration();
            SendMessageW(window, WM_COMMAND, panel::kExportSaveProjectButton, 0);
        }
        // EndDialog takes effect after this callback returns, so stale messages
        // sent to the settings window here must all remain harmless.
        SendMessageW(window, WM_COMMAND, probe.action == VideoModalAction::Cancel ? IDCANCEL : IDOK, 0);
    } else {
        if (probe.path || probe.sawPicker) return TRUE;
        probe.sawPicker = true;
        std::puts("SHELL cancelling the real Windows picker");
        SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    }
    return FALSE;
}

void CALLBACK DriveShellTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (activeShellProbe)
        EnumThreadWindows(GetCurrentThreadId(), &DriveShellWindow,
            reinterpret_cast<LPARAM>(activeShellProbe));
}

void CheckMenuCancellation(MMDApp* app) {
    namespace fs = std::filesystem;
    const auto directory = fs::current_path() / L"video-shell-pick";
    Require(fs::create_directory(directory), "create a separate directory for the real picker");
    const auto target = directory / (MMD_ORIGINAL_PROFILE ? L"视频.avi" : L"视频.mp4");
    const auto remembered = fs::current_path().wstring();
    Require(remembered.size() < kPathFieldCapacity && target.native().size() < kPathFieldCapacity,
            "test paths fit the output and initial-directory fields");
    wcscpy_s(app->ExeDir(), kPathFieldCapacity, remembered.c_str());
    wcscpy_s(app->DirUser(), kPathFieldCapacity, remembered.c_str());
    wcscpy_s(app->AviOutputPath(), kPathFieldCapacity, L"previous-output.avi");
    app->HInstance() = GetModuleHandleW(nullptr);
    app->EnglishUI() = 1;
    const HMENU menu = CreateMenu();
    Require(menu && AppendMenuW(menu, MF_STRING | MF_CHECKED, 0x12D, L"Remember location") &&
        SetMenu(app->MainWindow(), menu), "enable remembering the location on the isolated owner");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Require(SUCCEEDED(com), "initialize COM for the real Windows file picker");
    for (const wchar_t* path : {static_cast<const wchar_t*>(nullptr), target.c_str()}) {
        ShellCancelProbe probe{app, path};
        activeShellProbe = &probe;
        const auto timer = SetTimer(nullptr, 0, 50, &DriveShellTimer);
        Require(timer != 0, "drive only windows owned by the inactive test desktop");
        CmdFileMenu(app, app->MainWindow(), 223, 0);
        KillTimer(nullptr, timer);
        activeShellProbe = nullptr;
        Check(probe.sawPicker && probe.sawSettings == (path != nullptr),
              "real Shell cancellation stops immediately and the controlled pick opens settings");
        Check(std::wcscmp(app->AviOutputPath(), L"previous-output.avi") == 0 &&
              app->DirUser() == remembered && !fs::exists(target) && !app->Recorder()->graph,
              "cancelling either dialog preserves the previous path and directory without starting output");
    }
    auto requested = target;
    requested.replace_extension(L".tmp");
    std::ofstream(target, std::ios::binary) << "existing video";
    const auto baselineStarts = videoStarts;
    for (const auto action : {VideoModalAction::ReplaceInPicker, VideoModalAction::ReplaceInSettings,
                             VideoModalAction::ReplaceInOverwrite, VideoModalAction::ReplaceInDeclinedOverwrite,
                             VideoModalAction::Accept}) {
        const bool accept = action == VideoModalAction::Accept;
        const bool replaceOnPrompt = action == VideoModalAction::ReplaceInOverwrite ||
                                     action == VideoModalAction::ReplaceInDeclinedOverwrite;
        const bool confirm = replaceOnPrompt || accept;
        const auto generation = app->SceneGeneration();
        app->AviRecordStartFrame() = 303;
        const auto promptCount = overwritePrompts;
        overwritePath = confirm ? target.c_str() : nullptr;
        overwriteAnswer = action == VideoModalAction::ReplaceInDeclinedOverwrite ? IDNO : IDYES;
        replaceSceneOnOverwrite = replaceOnPrompt ? app : nullptr;
        ShellCancelProbe probe{app, requested.c_str(), action};
        activeShellProbe = &probe;
        const auto timer = SetTimer(nullptr, 0, 50, &DriveShellTimer);
        Require(timer != 0, "drive the settings for the scene boundary cases");
        CmdFileMenu(app, app->MainWindow(), 223, 0);
        KillTimer(nullptr, timer);
        activeShellProbe = nullptr;
        replaceSceneOnOverwrite = nullptr;
        overwritePath = nullptr;
        Check(probe.sawPicker && probe.sawSettings == (action != VideoModalAction::ReplaceInPicker),
              "scene replacement stops at the expected modal boundary");
        Check((app->SceneGeneration() != generation) == !accept && overwritePrompts == promptCount + unsigned(confirm),
              "the expected prompt and scene transition occurred");
        Check(staleSaveCalls == 0 && videoStarts == baselineStarts + unsigned(accept),
              "stale settings cannot save the replacement project or start video");
        Check(app->AviRecordStartFrame() == (accept ? 45 : 303) &&
              std::wstring(app->AviOutputPath()) == (accept ? target.native() : L"previous-output.avi") &&
              std::wstring(app->DirUser()) == (accept ? directory.native() : remembered),
              "only the confirmed current settings publish the output path, frame range and directory");
    }
    const auto alternate = directory / (MMD_ORIGINAL_PROFILE ? L"alternate.avi" : L"alternate.mp4");
    const auto projectDirectory = (directory / L"project-resources").wstring();
    wcscpy_s(app->PathWorkspace().projectDirectory, kPathFieldCapacity, projectDirectory.c_str());
    wcscpy_s(app->AviOutputPath(), kPathFieldCapacity, L"previous-output.avi");
    wcscpy_s(app->AviBackgroundPath(), kPathFieldCapacity, target.c_str());
    protectedBackground = target.c_str();
    ShellCancelProbe probe{app, requested.c_str(), VideoModalAction::ChangeProtectedBackground};
    probe.alternate = alternate.c_str();
    activeShellProbe = &probe;
    const auto timer = SetTimer(nullptr, 0, 50, &DriveShellTimer);
    Require(timer != 0, "drive the background protection case in the real settings");
    CmdFileMenu(app, app->MainWindow(), 223, 0);
    KillTimer(nullptr, timer);
    activeShellProbe = nullptr;
    protectedBackground = nullptr;
    Check(probe.sawAlternate && videoStarts == baselineStarts + 2 &&
          app->AviOutputPath() == alternate.native() && app->AviRecordStartFrame() == 45 &&
          app->AviRecordEndFrame() == 47 && app->PathWorkspace().projectDirectory == projectDirectory,
          "only the new destination starts export and the project resource directory stays intact");
    app->AviBackgroundPath()[0] = L'\0';
    std::ifstream saved(target, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    Check(bytes == "existing video" && !fs::exists(requested), "settings never write the fixture video");
    if (!failures) std::puts("PASS video settings scene boundary and retry");
    CoUninitialize();
    SetMenu(app->MainWindow(), nullptr);
    DestroyMenu(menu);
}

DWORD WINAPI RunOnDesktop(void* desktop) {
    Require(SetThreadDesktop(static_cast<HDESK>(desktop)) != FALSE,
            "attach the dialog thread to the inactive private desktop");
    Require(MH_Initialize() == MH_OK, "initialize test hooks");
    Hook(&FillCodecCombo, &FixtureCodec);
    Hook(&MessageBoxA, &UnexpectedMessageA);
    Hook(&MessageBoxW, &UnexpectedMessageW);
    Hook(&SaveSceneAs, &ObserveSaveAs);
    Hook(&StartAviRecordWindow, &ObserveVideoStart);
    Require(MH_CreateHook(reinterpret_cast<void*>(&GetUiSaveFileNameW), reinterpret_cast<void*>(&ControlledVideoPick),
        reinterpret_cast<void**>(&realSaveFileName)) == MH_OK, "observe the picker and control the successful selection case");
    Require(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable isolated codec and message guards");
    auto app = std::make_unique<MMDApp>();
    auto renderer = std::make_unique<D3DRenderer>();
    app->state = MMDAppState{};
    DShowRecorder recorder{};
    app->Renderer() = renderer.get();
    app->Recorder() = &recorder;
    g_Block = app.get();
    const float savedFrameScale = g_FrameScale;
    g_FrameScale = 30;
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Video range fixture",
        WS_POPUP, 0, 0, 400, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(owner != nullptr, "create hidden owner");
    app->MainWindow() = owner;
    for (const int id : {panel::kPlayStartFrameEdit, panel::kPlayStopFrameEdit}) {
        Require(CreateWindowExW(0, L"EDIT", L"0", WS_CHILD, 0, 0, 50, 20, owner,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr) != nullptr,
            "create a main playback-range control");
    }
    const wchar_t* expectedFps = L"30";
    for (unsigned language = 0; language != 3; ++language) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        for (const auto& range : cases) {
            Run(app.get(), range, 0, expectedFps, L"60");
            Run(app.get(), range, 1, L"60", L"29.97");
            expectedFps = L"29.97";
        }
        Run(app.get(), cases[0], 2, expectedFps, nullptr, true);
        Run(app.get(), cases[0], 3, expectedFps);
        CheckOutputPathBoundary(app.get(), expectedFps);
        CheckOverwriteAfterCodecChange(app.get(), expectedFps);
    }
    CheckMenuCancellation(app.get());
    Check(codecCalls == 3 * (2 * std::size(cases) + 5) + 6,
          "all languages and repeated openings initialize through the real production callback");
    Check(!IsWindowVisible(owner), "owner remains hidden");
    DestroyWindow(owner);
    app->MainWindow() = nullptr;
    app->Renderer() = nullptr;
    app->Recorder() = nullptr;
    g_Block = nullptr;
    g_FrameScale = savedFrameScale;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    return 0;
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    wchar_t desktopName[80]{};
    swprintf_s(desktopName, L"MMS-video-export-range-%lu", GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Require(desktop != nullptr, "create an inactive video-export desktop");
    const HANDLE thread = CreateThread(nullptr, 0, RunOnDesktop, desktop, 0, nullptr);
    Require(thread && WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0,
            "run the dialog regression within thirty seconds");
    DWORD exitCode = 1;
    Require(GetExitCodeThread(thread, &exitCode) && exitCode == 0,
            "complete the private dialog thread");
    CloseHandle(thread);
    // Windows input helpers can keep the inactive desktop open until process exit.
    Check(CloseDesktop(desktop) != FALSE || GetLastError() == ERROR_BUSY,
          "release the inactive video-export desktop");
    std::printf("video-export-range profile=%s checks=%u failures=%u dialogs=%u\n",
                MMD_ORIGINAL_PROFILE ? "mmd932" : "full", checks, failures, codecCalls);
    return failures ? 1 : 0;
}
