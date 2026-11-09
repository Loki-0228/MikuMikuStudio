// Playback uses canonical frame/30 time; physical integration advances only
// at fixed simulation ticks, while output and display sample independently.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "btBulletDynamicsCommon.h"

#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {
namespace {

// fild dword + "test/jge/fadd flt_52B9F0" (x86) / zero-extended
// cvtsi2ss r64 (x64, e.g. 0x7FF7CB44B96B): signed dword reinterpreted as
// unsigned before the float conversion.
double UnsigFlt(std::int32_t v) {
    return static_cast<double>(static_cast<std::uint32_t>(v));
}

inline double& Cursor(MMDApp* app) {
    return app->PlaybackCursorSeconds();
}

// Recording wait (0x46F070..0x46F127): capture one frame through the
// app+0xA06C0 +0x68 interface (vtable slot 4, E_FAIL aborts), pump messages,
// sleep 1ms, until the byte behind app+0x9EDD4 turns non-zero; then sleep
// 500ms and finish with 0x464A00.  Returns true when the section should be
// treated as finished via the 0x46F575 exit (always the case here).
void FrameStepBeyondEnd(MMDApp* app) {
    auto& s = *app;
    DShowRecorder* recorder = s.Recorder();
    IPushSource* push =
        recorder != nullptr
            ? static_cast<IPushSource*>(recorder->framePush)
            : nullptr;
    unsigned char* flagPtr = s.RecordingCompletionFlag();

    // The original guards BOTH capture calls with a null test on the
    // +0x68 interface (0x46F079 test/jz and 0x46F0A4 test/jz): with no
    // AVI-dump dialog opened the interface stays null and the calls are
    // skipped entirely.  Omitting the guards null-dereferences on the
    // first frame-step click past the end.
    auto capture = [&]() -> long {                          // 0x46F070/9B
#ifdef MIKUMIKUSTUDIO_DIAG
        static int capCount = 0;
#endif
        if (push == nullptr)
            return 0;
        // IPushSource methods are STDMETHODCALLTYPE: `this` is the first
        // stack argument (NOT a cdecl call - the 8-byte cleanup mismatch
        // corrupted the caller's stack).
#ifdef MIKUMIKUSTUDIO_DIAG
        if (getenv("MIKUMIKUSTUDIO_TRACE_REC")) {
            if (++capCount <= 5 || capCount % 60 == 0) {
                FILE* tf = fopen(getenv("MIKUMIKUSTUDIO_TRACE_REC"), "a");
                if (tf) {
                    fprintf(tf,
                            "capture#%d push=%p flag=%d\n",
                            capCount, static_cast<void*>(push),
                            (int)*flagPtr);
                    fclose(tf);
                }
            }
        }
#endif
        return push->GetStreamingState(flagPtr);
    };

    capture();
    while (*flagPtr == 0) {                                 // 0x46F08C
        if (push == nullptr)                                // 0x46F0A4 jz
            break;
        if (capture() == static_cast<long>(0x80004005))     // E_FAIL
            break;                                          // 0x46F0B1
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);                         // 0x46F0D8
            DispatchMessageA(&msg);
        }
        Sleep(1);                                           // 0x46F102
    }
    Sleep(500);                                             // 0x46F115
    FinishAviRecord(app, true);                                    // 0x46F122
}

// Past-end path while playing (0x46F2AF..0x46F3D1).
void PlayingBeyondEnd(MMDApp* app) {
    auto& s = *app;
    WaveAudioContext* audio = s.Audio();                    // app+0xCC
    if (s.WaveEnabled() != 0) {
        CloseDataFile(audio);                                // 0x46F2C4
        WaveStartPlayback(audio);                            // 0x4C2760
    }

    if (s.PlaybackLoopEnabled() != 0) {
        char text[256]{};
        GetWindowTextA(GetDlgItem(static_cast<HWND>(s.Hwnd()), panel::kPlayStartFrameEdit),
                       text, sizeof text);
        std::int32_t frame = 0;
        if (!frame_limits::ParsePlaybackBound(text, frame)) { s.PlaybackActive() = 0; StopPlayback(app); return; }
        // x64 0x7FF7CB44BC3E..0x7FF7CB44BC4B: cvtsi2ss of the
        // zero-extended frame, then divss by the 30.0f slot.
        s.PlaybackStartSeconds() = UnsigFlt(frame) / 30.0f;
        s.PlaybackPhysicsMode() = s.SavedPlaybackPhysicsMode();
        Cursor(app) = s.PlaybackStartSeconds();
        UpdateBoneFrames(app);                              // 0x433A40
        if (s.WaveEnabled() != 0 &&
            s.AutomaticFrameAdvanceEnabled() == 0) {
            SetFrameNormalized(
                s.FrameNormalization());
            WaveSeekAndFeed(audio, static_cast<double>(
                                   s.PlaybackStartSeconds() / playback_timing::Speed(s.PlaybackTiming())));
        }
        s.PlaybackClockAnchorLow() =
            s.TimeNowLow();
        s.PlaybackClockAnchorHigh() = s.TimeNowHigh();
        PlaybackPoseAdvance(app, 0);                        // 0x46F56C
        return;
    }

    // stop playback (0x46F3A5..0x46F3D1); the final 0x4175A0(0) is skipped.
    s.PlaybackActive() = 0;
    StopPlayback(app);                                         // 0x4341E0
    SendMessageA(GetDlgItem(static_cast<HWND>(s.Hwnd()), panel::kPlayButton),
                 BM_SETCHECK, 0, 0);               // 0x46F3CB
}

}  // namespace

// Physics-cursor catch-up of the FrameDriver (0x46EEE0..0x46F575).
// selActive is the var_14A1 local computed in FrameDriver (0x46DCA6).
void PlaybackCatchup(MMDApp* app, unsigned char selActive) {
    for (int slot=0; slot<kModelSlotCount; ++slot)
        if (auto* model=app->ModelSlot(slot)) bone_scale::RestoreTransforms(model);
    auto& s = *app;

    // ---- 1. frame counter display + N++ (0x46EEE0..0x46EFB7) ------------
    // x64 0x7FF7CB44B850..0x7FF7CB44B8A4, all single precision: N*30 as an
    // INTEGER (imul eax, 1Eh) then movd/cvtdq2ss, divss by the fps float,
    // addss the zero-extended frameA, cvttss2si r32.
    const bool recording = s.RecordingWindow() != nullptr &&
        s.FrameStepPlayback() != 0 && s.PlaybackActive() != 0 &&
        s.state.recordPlaybackStartPending == 0;
    if (recording && s.state.aviStereoOutput == 0) {
        const std::int32_t n = s.state.recordedFrameCount;
        const std::int32_t frameA = s.AviRecordStartFrame();
        const float fps = s.AviRecordFps();
        const int shown = static_cast<int>(
            static_cast<double>(n) * s.PlaybackTiming().playbackFps / fps + UnsigFlt(frameA));
        wchar_t buf[0x100];
        if (image_sequence::Active(app)) {
            swprintf_s(buf, image_sequence::Text(app,
                L"正在导出图片序列：第 %d 张（时间轴 %d / %d 帧）",
                L"Exporting image %d (timeline frame %d / %d)",
                L"連番画像出力中：%d 枚（フレーム %d / %d）"),
                n + 1, shown, s.AviRecordEndFrame());
        } else if (s.state.englishUI == 2) {
            swprintf_s(buf, 0x100, L"正在录制：第 %d 帧（结束帧 %d）",
                       shown, s.AviRecordEndFrame());
        } else if (s.state.englishUI != 0) {
            swprintf_s(buf, 0x100, L" %dframe recording(until %dframe)",
                       shown, s.AviRecordEndFrame());
        } else {
            swprintf_s(buf, 0x100, L" %d", shown);
        }
        SetWindowTextW(s.RecordingWindow(), buf);
    }
    if (s.FrameStepPlayback() != 0 && s.PlaybackActive() != 0 &&
        s.state.recordPlaybackStartPending == 0)
        s.state.recordedFrameCount += 1;

    // ---- 2./3. gates (0x46EFBE..0x46EFEE) --------------------------------
    if (s.state.messageSeen == 0)
        return;                                    // -> 0x46FEC4 (post)
    // Idle editing retains a wall-time budget. Playback consumes fixed
    // canonical ticks below and leaves no render-dependent remainder.
    g_CatchupDtBudget = s.state.deltaTime;                   // 0xA06BC
    if (s.PlaybackActive() == 0)
        return;                                    // -> 0x46F575

    const double speed = playback_timing::Speed(s.PlaybackTiming());
    double target;
    bool reachedEnd = false;
    if (s.FrameStepPlayback()) {
        const double frame = s.AviRecordStartFrame() +
            double(s.state.recordedFrameCount) * s.PlaybackTiming().playbackFps / s.AviRecordFps();
        if (frame >= double(s.AviRecordEndFrame()) + 1) {
            FrameStepBeyondEnd(app);
            return;
        }
        target = frame / 30.0;
    } else {
        const std::uint64_t now = (std::uint64_t(s.TimeNowHigh()) << 32) | s.TimeNowLow();
        const std::uint64_t anchor = (std::uint64_t(s.PlaybackClockAnchorHigh()) << 32) |
            s.PlaybackClockAnchorLow();
        std::int64_t elapsed = static_cast<std::int64_t>(now - anchor);
        if (now < anchor) elapsed += std::int64_t(1) << 32;
        target = s.PlaybackStartSeconds() + double(elapsed) * s.MilliToSec() * speed;
        if (target > s.PlaybackEndSeconds()) {
            target = s.PlaybackEndSeconds();
            reachedEnd = true;
        }
    }

    auto& clock = s.PlaybackPhysicsClock();
    if (!clock.active || target < clock.origin) clock.Reset(s.PlaybackStartSeconds());
    const double rate = playback_timing::PhysicsRate(s.PlaybackTiming());
    const float savedDelta = s.DeltaTime();
    clock.stepping = true;
    const bool simulate = s.NativePhysicsMode() > 0 && s.Physics() && s.Physics()->world;
    while (simulate && clock.Due(target, rate)) {
        Cursor(app) = clock.Next(rate);
        s.DeltaTime() = static_cast<float>(1.0 / rate);
        g_CatchupDtBudget = s.DeltaTime();
        PlaybackPoseAdvance(app, 0);
        PhysicsFrame(app, selActive);
        ++clock.ticks;
    }
    clock.stepping = false;
    s.DeltaTime() = savedDelta;
    g_CatchupDtBudget = 0;
    Cursor(app) = target;
    PlaybackPoseAdvance(app, 0);
    if (reachedEnd) PlayingBeyondEnd(app);
}

}  // namespace mikumikustudio
