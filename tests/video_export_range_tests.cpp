// MAINTENANCE: regression-keep; video export must not append defaults to frame ranges.
// Real dialog resources and AviOutDlgProc run on a runner-owned private desktop.
// Only codec enumeration is replaced; no GPU, recording graph or file is created.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/dshow_recorder.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mf_h264_recorder.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/wave_audio_context.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>

namespace mikumikustudio {
INT_PTR __stdcall AviOutDlgProc(HWND, UINT, WPARAM, LPARAM);
int FillCodecCombo(DShowRecorder*, HWND, char);
}
using namespace mikumikustudio;
namespace {
unsigned checks = 0, failures = 0, codecCalls = 0;
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
int WINAPI UnexpectedMessageW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"FAIL unexpected MessageBoxW: %ls\n", text ? text : L"");
    std::exit(2);
}
template<class F> void Hook(F target, F replacement) {
    Require(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement), nullptr) == MH_OK,
            "install isolated dialog dependency or unexpected-message guard");
}

struct RangeCase {
    const char* name;
    const wchar_t* mainEnd;
    unsigned lastFrame;
    int audioBytes;
    unsigned audioBytesPerSecond;
    unsigned expected;
};
const RangeCase cases[] = {
    {"reported-1100-plus-1", L"1100", 1, 0, 0, 1100},
    {"explicit-plus-zero", L"1100", 0, 0, 0, 1100},
    {"explicit-short-range", L"120", 3000, 60000, 300, 120},
    {"zero-falls-back-to-motion", L"0", 1100, 0, 0, 1100},
    {"empty-falls-back-to-motion", L"", 42, 0, 0, 42},
    {"audio-rounded-up", L"0", 5, 1001, 300, 101},
    {"motion-longer-than-audio", L"0", 300, 1001, 300, 300},
    {"invalid-audio-rate", L"0", 7, 1001, 0, 7},
};

struct Context {
    MMDApp* app;
    const RangeCase* range;
    unsigned opening;
    bool cancel;
    bool initialized = false;
};
std::wstring Text(HWND dialog, int id) {
    wchar_t text[96]{};
    GetDlgItemTextW(dialog, id, text, static_cast<int>(std::size(text)));
    return text;
}
INT_PTR CALLBACK Probe(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message != WM_INITDIALOG)
        return context ? AviOutDlgProc(dialog, message, wp, lp) : FALSE;
    context = reinterpret_cast<Context*>(lp);
    SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
    AviOutDlgProc(dialog, WM_INITDIALOG, wp, 0);
    context->initialized = true;
    const auto actual = Text(dialog, panel::kAviEndFrameEdit);
    std::printf("CASE language=%u name=%s opening=%u end='%ls' expected=%u action=%s\n",
                unsigned(context->app->EnglishUI()), context->range->name,
                context->opening, actual.c_str(), context->range->expected,
                context->cancel ? "cancel" : "ok");
    std::printf("DIMENSIONS render=%dx%d width='%ls' height='%ls' fps='%ls'\n",
                context->app->RenderWidth(), context->app->RenderHeight(),
                Text(dialog, panel::kAviWidthEdit).c_str(),
                Text(dialog, panel::kAviHeightEdit).c_str(),
                Text(dialog, panel::kAviFpsEdit).c_str());
    Check(actual == std::to_wstring(context->range->expected), "default end is one complete range value");
    Check(Text(dialog, panel::kAviStartFrameEdit) == L"0", "default start remains zero");
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
    }
    // EndDialog runs inside initialization, before the modal loop can show it.
    // No menu 223 continuation is invoked, so no recorder or output file starts.
    AviOutDlgProc(dialog, WM_COMMAND, context->cancel ? IDCANCEL : IDOK, 0);
    return TRUE;
}

void Run(MMDApp* app, const RangeCase& range, unsigned opening, bool cancel) {
    // Reopening also covers an even output size with a nonstandard aspect ratio.
    app->RenderWidth() = opening == 1 ? 1918 : 1920;
    app->RenderHeight() = 1080;
    WaveAudioContext audio{};
    audio.dataSize = range.audioBytes;
    audio.format.nAvgBytesPerSec = range.audioBytesPerSecond;
    wcscpy_s(audio.path, L"fixture.wav");
    app->Audio() = range.audioBytes ? &audio : nullptr;
    app->LastRegisteredFrame() = range.lastFrame;
    SetDlgItemTextW(app->MainWindow(), panel::kPlayStopFrameEdit, range.mainEnd);
    app->AviRecordStartFrame() = 303;
    app->AviRecordEndFrame() = 404;
    app->AviRecordFps() = 24;
    wcscpy_s(app->AviOutputPath(), 256, L"video-export-range-test.avi");
    const std::wstring oldPath = app->AviOutputPath();
    Context context{app, &range, opening, cancel};
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
        std::printf("COMMIT start=%d end=%d fps=%.0f\n", app->AviRecordStartFrame(),
                    app->AviRecordEndFrame(), app->AviRecordFps());
        Check(app->AviRecordStartFrame() == 0 && app->AviRecordEndFrame() == static_cast<int>(range.expected) &&
              app->AviRecordFps() == 30, "IDOK reads the displayed range without extra digits");
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
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    wchar_t desktopName[256]{};
    DWORD needed = 0;
    Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
        desktopName, sizeof desktopName, &needed) && desktopName[0] && _wcsicmp(desktopName, L"Default") != 0,
        "run only on a runner-owned private desktop; never switch the input desktop");
    Require(MH_Initialize() == MH_OK, "initialize test hooks");
    Hook(&FillCodecCombo, &FixtureCodec);
    Hook(&MessageBoxA, &UnexpectedMessageA);
    Hook(&MessageBoxW, &UnexpectedMessageW);
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
    Require(CreateWindowExW(0, L"EDIT", L"0", WS_CHILD, 0, 0, 50, 20, owner,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kPlayStopFrameEdit)),
        GetModuleHandleW(nullptr), nullptr) != nullptr,
        "create the main playback-range control");
    for (unsigned language = 0; language != 3; ++language) {
        app->EnglishUI() = static_cast<unsigned char>(language);
        for (const auto& range : cases)
            for (unsigned opening = 0; opening != 2; ++opening) Run(app.get(), range, opening, false);
        Run(app.get(), cases[0], 2, true);
    }
    Check(codecCalls == 51, "all languages and repeated openings initialize through the real production callback");
    Check(!IsWindowVisible(owner), "owner remains hidden");
    DestroyWindow(owner);
    app->MainWindow() = nullptr;
    app->Renderer() = nullptr;
    app->Recorder() = nullptr;
    g_Block = nullptr;
    g_FrameScale = savedFrameScale;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    std::printf("video-export-range profile=%s checks=%u failures=%u dialogs=%u\n",
                MMD_ORIGINAL_PROFILE ? "mmd932" : "full", checks, failures, codecCalls);
    return failures ? 1 : 0;
}
