// Opt-in production-host regression: <model.pmx> <motion.vmd>.
// Simulated 600 Hz calls through frame 660; no performance/FPS claim is made.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "../src/app/playback_physics_samples.hpp"
#include <MinHook.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <vector>

using namespace mikumikustudio;
namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL %s\n", message); std::exit(1); }
}
LRESULT CALLBACK Hide(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) reinterpret_cast<CBT_CREATEWND*>(lp)->lpcs->style &= ~WS_VISIBLE;
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
decltype(&ShowWindow) showWindow;
BOOL WINAPI KeepHidden(HWND window, int) { return showWindow(window, SW_HIDE); }
int WINAPI DialogA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("Unexpected dialog: %s\n", text); std::exit(2);
}
int WINAPI DialogW(HWND, LPCWSTR text, LPCWSTR title, UINT flags) {
    // The explicitly supplied motion is intentionally retargeted to this model.
    if ((flags & MB_TYPEMASK) == MB_OKCANCEL && std::wcscmp(title, L"Load motion data") == 0)
        return IDOK;
    std::fwprintf(stderr, L"Unexpected dialog: %ls: %ls\n", title, text); std::exit(2);
}
template<class F> void Hook(F original, F replacement, F* trampoline) {
    Check(MH_CreateHook(reinterpret_cast<void*>(original), reinterpret_cast<void*>(replacement),
                       reinterpret_cast<void**>(trampoline)) == MH_OK, "create host visibility hook");
    Check(MH_EnableHook(reinterpret_cast<void*>(original)) == MH_OK, "enable host visibility hook");
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Check(message.message != WM_QUIT, "host remains alive");
        if (message.message == WM_PAINT) ValidateRect(message.hwnd, nullptr);
        else { TranslateMessage(&message); DispatchMessageW(&message); }
    }
}
}
int wmain(int argc, wchar_t** argv) {
    Check(argc == 3, "model and motion paths required");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Check(MH_Initialize() == MH_OK, "initialize hooks");
    Hook(&ShowWindow, &KeepHidden, &showWindow);
    decltype(&MessageBoxA) boxA; decltype(&MessageBoxW) boxW;
    Hook(&MessageBoxA, &DialogA, &boxA); Hook(&MessageBoxW, &DialogW, &boxW);
    const auto hidden = SetWindowsHookExW(WH_CBT, Hide, nullptr, GetCurrentThreadId());
    auto app = std::make_unique<MMDApp>(); g_Block = app.get();
    app->state = MMDAppState{}; app->InitDefaults(); app->EnglishUI() = 1;
    Check(InitMainWindowAndD3D(app.get(), GetModuleHandleW(nullptr), SW_HIDE), "initialize production host");
    EnableD3D12Native(false);
    LoadModelFile(app.get(), argv[1], false);
    app->SelectedModelSlot() = 0; app->state.optflag[0] = 0;
    Check(app->SelectedModel() != nullptr, "load supplied PMX");
    Check(ImportVmdFile(app.get(), argv[2]), "load supplied VMD");
    auto& model = *mdl::Mdl(app->SelectedModel());
    Check(model.maxFrame >= 660, "motion covers the reported interval");
    app->PlaybackTiming().interpolate = true;
    app->PlaybackPhysicsMode() = 2; app->FpsLimit() = 1000;
    app->DeltaTime() = 1.f / 60; app->MessageSeen() = 10;
    FrameDriver(app.get()); Pump();
    app->PlaybackStartsAtCurrentFrame() = 0;
    app->TimeNowLow() = 100000; app->TimeNowHigh() = 0;
    SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStartFrameEdit), L"0");
    SetWindowTextW(GetDlgItem(app->MainWindow(), panel::kPlayStopFrameEdit), L"700");
    CommandDispatch(GetDlgItem(app->MainWindow(), panel::kPlayButton), MAKEWPARAM(408, BN_CLICKED));
    Check(app->PlaybackActive(), "start normal playback");
    std::vector<float> previous(model.boneCount * 16);
    unsigned samples = 0, changed = 0, held = 0;
    for (unsigned sample = 0; sample <= 13200; ++sample) {
        Pump();
        app->TimeNowLow() = 100000 + unsigned(std::lround(sample * 1000.0 / 600));
        app->TimeNowHigh() = 0; app->DeltaTime() = 1.f / 600; app->MessageSeen() = 10;
        const auto before = app->frameRate.presentedFrames;
        FrameDriver(app.get());
        auto& clock = app->PlaybackPhysicsClock();
        Check(clock.visualSampleReady && clock.samples && clock.samples->Covers(app->PlaybackCursorSeconds()),
              "every candidate is bracketed by completed physics samples");
        Check(clock.Completed(60) - app->PlaybackCursorSeconds() <= 1.0 / 60 + 1e-9,
              "physics precomputation stays within one independent tick");
        if (app->PlaybackCursorSeconds() >= 20) {
            ++samples; bool moved = false;
            for (unsigned b = 0; b < model.boneCount; ++b) for (unsigned c = 0; c < 16; ++c) {
                const float value = model.boneTable[b].matInit[c];
                Check(std::isfinite(value), "display bone matrices are finite");
                moved = moved || std::abs(previous[b * 16 + c] - value) > 1e-6f;
                previous[b * 16 + c] = value;
            }
            if (moved) ++changed;
            if (before == app->frameRate.presentedFrames) ++held;
            // Reusing the same timestamp must retain the displayed image and FPS.
            const auto presented = app->frameRate.presentedFrames;
            FrameDriver(app.get());
            Check(app->frameRate.presentedFrames == presented, "duplicate timestamp does not present or inflate FPS");
        }
        if (sample % 1200 == 0) std::printf("frame=%.3f physics_steps=%llu presented=%llu\n",
            app->PlaybackCursorSeconds() * 30, clock.step, app->frameRate.presentedFrames);
    }
    Check(changed > 1000 && samples >= 1200, "reported interval has intermediate moving poses above 60 Hz");
    std::printf("PASS frames=600..660 samples=%u changed=%u held=%u bones=%u rigids=%u\n",
        samples, changed, held, model.boneCount, model.rigidCount);
    app->PlaybackActive() = 0; StopPlayback(app.get());
    ShutdownCleanup(app.get()); g_Block = nullptr; UnhookWindowsHookEx(hidden);
}
