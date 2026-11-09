#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/recording_state.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "recording_ack_wait.hpp"
#include <wrl/client.h>
// ===========================================================================
// VA 0x0046EEE0..0x0046F575 - playback / frame-step catch-up section of the
// FrameDriver (original: sub_46B090)
// ===========================================================================
// Field-exact port of the timeline->physics cursor machinery.  State (all
// verified from the live disassembly):
//   app+0x9E64C  physics cursor, seconds (float).  Advanced only here, in
//                1/60 steps (dbl_52EA08 = 1/60), then clamped to the target.
//   app+0x9E654  playback start time (frameA / 30.0, dbl_52BA68 = 30.0);
//                written by the play/seek handlers (CommandDispatch 408 at
//                0x487510/0x487536, FrameDriver section 7 at 0x479CF9, loop
//                restart at 0x46F32A) - NOT advanced during playback.
//   app+0x9E658  playback end time (frameB / 30.0).
//   app+0x9EDA8/AC  T0: wall-clock anchor (64-bit timeGetTime pair) reset at
//                play start / loop restart; the playing target is
//                9E654 + (TimeNow - T0) * MilliToSec.
//   app+0x9ED94  per-pass counter N: incremented at 0x46EFB7 every
//                FrameDriver pass, reset to 0 by section 7 (0x479D5A), so
//                N >= 1 whenever the frame-step target formula runs.
//   app+0x330    "frame advanced" master gate (set by play/seek handlers).
//   app+0x9ED90  frame-step mode flag (1 = paused stepping).
//
// Flow (0x46EEE0..0x46F575):
//   1. frame counter display: " %d" or, when 0xA0B4C (English/recording
//      display) is set, " %dframe recording(until %dframe)"; the shown
//      frame is frameA + (N*30)/measuredFps (0xA0B08).  Skipped when
//      0xA0D61 is set; N += 1 either way (0x46EFB7).
//   2. 0x46EFBE: app+0xA0D6C == 0 -> skip the whole section (jump to the
//      post-physics 0x46FEC4; the same gate also covers the accessory and
//      physics blocks downstream).
//   3. 0x46EFE8: app+0x330 == 0 -> nothing to advance, jump to 0x46F575.
//   4. 0x46EFEE: frame-step mode (0x9ED90 != 0):
//        var_14C8 = 1/fps (dead bookkeeping, never read);
//        target   = (frameA + 30.0/fps) / (N*30.0)          (0x46F011..54)
//        gate     : (uint)(int)(frameA + 30.0/fps) < (uint)(frameB+1)
//                   -> BLOCK 1 (0x46F12C), else the recording wait (0x46F070).
//   5. playing (0x9ED90 == 0): target = 9E654 + elapsed (0x46F264);
//      end(0x9E658) >= target -> BLOCK 2 (0x46F3D6), else the past-end
//      stop/loop path (0x46F2AF).
//   BLOCK 1 (0x46F12C..0x46F25B): cursor += 1/60; while target > cursor:
//     sub_4175A0(app, 1) [timeline advance] - ordered per-model pass
//     [morph 0x4970B0 + SetPhysicsMode(0, models, count)] - kinematic sync
//     0x4B22F0 in REVERSE slot order - stepSimulation(1/60, 10, 1/60) -
//     cursor += 1/60.  No selection skip.  Then cursor = target.
//   BLOCK 2 (0x46F3D6..0x46F562): same, but the ordered pass and the
//     FORWARD-order kinematic sync SKIP the selected model
//     (var_14A1 && app+0x2F8 == 0 && app+0x910 == slot).  Then
//     cursor = target and sub_4175A0(app, 0) (0x46F56C).
//   Past end (0x46F2AF): cursor = target (beyond end, verbatim); audio stop
//     pair 0x4C2680/0x4C2760 when 0xA06CC; loop (0x341): reread the frame
//     edit 0x199 -> atol -> 9E654 = frame/30, count = app+0xA0670,
//     cursor = 9E654, 0x433A40, audio reseek, T0 = now, sub_4175A0(0);
//     stop: 0x330 = 0, 0x4341E0, BM_SETCHECK(0) on item 0x198, and the
//     final sub_4175A0(0) is SKIPPED (jump straight to 0x46F575).
//
// FPU condition-code translation (exact):
//   "test ah,41h / jnz skip"  = skip unless target > t1 (NaN skips too) ->
//                               plain float > in C++.
//   "test ah,5 / jp  block2"  = end >= target (NaN also jumps) ->
//                               !(end < target).
//   "test ah,5 / jnp loop"    = continue while cursor < target (NaN exits)
//                               -> plain float < in C++.
//   1/60 additions: x86 runs them in double and rounds to float on store
//   (fstp dword); the x64 twin (sub_7FF7CB4474F0 region 0x7FF7CB44BA81
//   etc.) is a plain addss against the .rdata float 0x3C888889 - the
//   port follows the x64 single-precision form (behavior basis).
//
// The var_14A1 local ("selection active") is threaded in from FrameDriver
// where the original computes it (0x46DCA6..0x46DCCD): zeroed, then filled
// by the app callback at 0xA03D4 when 0x9ED90 == 0 && byte 0xA03B8 != 0.
//
// Port scope notes (docs/ARCHITECTURE.md section 8):
//   - sub_4175A0 (PlaybackPoseAdvance) is fully ported in
//     src/app/timeline_advance.cpp (per-model AdvanceModelKeyframes + camera/clip/
//     shadow/light/accessory key tracks); verified equivalent by the
//     AllStar play/stop A/B (post-stop kinematic-pose hashes identical
//     across every model slot).
//     Its second parameter is an integer flag (push 1 / push 0, compared
//     as a byte at 0x4175D7 - IDA's float prototype is wrong: the stack
//     slot is merely reused as a double scratch later in the function).
//   - The phase-15 null guards were reclaimed in phase 19: the Bullet
//     world and the app+0xA06C0 capture object are created by the ported
//     init path (SceneConstruct at WM_CREATE / InitMainWindowAndD3D's
//     0x408EA0 zeroing of the 0x6C object) before the message loop can
//     run the FrameDriver, matching the original's direct dereferences.
//     The recording wait itself stays unreachable until the frame-range
//     dialog (0x40F2F0) arms frame-step mode.
//   - flt_52B9F0 = 4294967296.0f (2^32) used as the unsigned-int-to-double
//     fixup; dbl_52BA68 = 30.0; dbl_52EA08 = 1/60; flt_52EA00 = 1/60f.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
#include "mikumikustudio/recording_state.hpp"

namespace mikumikustudio {
namespace {

inline double& Cursor(MMDApp* app) {
    return app->PlaybackCursorSeconds();
}

btDiscreteDynamicsWorld* PlayWorld(MMDApp* app) {
    PhysicsScene* scene = app->Physics();
    if (scene == nullptr)
        return nullptr;
    return scene->world;   // slot 64 / 0x40
}

// Recording wait (0x46F070..0x46F127): capture one frame through the
// app+0xA06C0 +0x68 interface (vtable slot 4, E_FAIL aborts), pump messages,
// sleep 1ms, until the byte behind app+0x9EDD4 turns non-zero; then sleep
// 500ms and finish with 0x464A00.  Returns true when the section should be
// treated as finished via the 0x46F575 exit (always the case here).
void FrameStepBeyondEnd(MMDApp* app) {
    if (!app || recording_state::Stopping(app) ||
        recording_state::Guard::Active(app->Recorder(), recording_state::Operation::GraphTeardown)) return;
    auto& s = *app;
    DShowRecorder* recorder = s.Recorder();
    Microsoft::WRL::ComPtr<IPushSource> pushOwner =
        recorder != nullptr
            ? static_cast<IPushSource*>(recorder->framePush)
            : nullptr;
    IPushSource* push = pushOwner.Get();
    unsigned char* flagPtr = s.RecordingCompletionFlag();

    // The graph's end-of-stream drain handles the last downstream delivery.
    // Retain the source while message dispatch can stop or replace recording.
    if (push) {
        const auto window = s.RecordingWindow();
        const auto frameStep = s.FrameStepPlayback();
        const auto current = [&] {
            return !recording_state::Stopping(app) && s.Recorder() == recorder &&
                recorder->framePush == push && s.RecordingWindow() == window &&
                s.FrameStepPlayback() == frameStep && s.RecordingCompletionFlag() == flagPtr;
        };
        const auto result = recording_state::WaitForAck(push, flagPtr, current,
            [&] { return recording_state::PumpRecordingMessages(current); });
        if (result != recording_state::AckResult::Ready) {
            if (result != recording_state::AckResult::Cancelled && current()) {
                runtime_log::Write("RECORDING final acknowledgement failed result=%d", static_cast<int>(result));
                FinishAviRecord(app);
            }
            return;
        }
    }
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
        s.PlaybackStartSeconds() = (std::max)(frame, 0) / 30.0;
        s.PlaybackPhysicsMode() = s.SavedPlaybackPhysicsMode();
        Cursor(app) = s.PlaybackStartSeconds();
        UpdateBoneFrames(app);                              // 0x433A40
        if (s.WaveEnabled() != 0 &&
            s.AutomaticFrameAdvanceEnabled() == 0) {
            SetFrameNormalized(
                s.FrameNormalization());
            WaveSeekAndFeed(audio, s.PlaybackStartSeconds() / playback_timing::Speed(s.PlaybackTiming()));
        }
        s.PlaybackClockAnchorLow() =
            s.TimeNowLow();
        s.PlaybackClockAnchorHigh() = s.TimeNowHigh();
        PreparePlaybackPhysics(app, 0);
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

void PreparePlaybackPhysics(MMDApp* app, unsigned char selActive) {
    auto& clock = app->PlaybackPhysicsClock();
    if (!clock.initialPosePending || app->NativePhysicsMode() <= 0 || !PlayWorld(app)) return;
    const double target = Cursor(app);
    Cursor(app) = clock.origin;
    app->PhysicsResetPending() = 1;
    app->state.gravityNoiseTimer = 0;
    AdvancePlaybackPhysicsTracks(app);
    StepPlaybackPhysics(app, selActive, 0.f);
    clock.initialPosePending = false;
    Cursor(app) = target;
}

namespace {
double AdvanceFixedPhysics(MMDApp* app, unsigned char selActive, double target) {
    if (app->NativePhysicsMode() <= 0 || !PlayWorld(app)) return target;
    PreparePlaybackPhysics(app, selActive);
    auto& clock = app->PlaybackPhysicsClock();
    const int rate = playback_timing::PhysicsRate(app->PlaybackTiming());
    const float seconds = 1.f / rate;
    // A suspended real-time window may be many frames behind. Keep the next
    // step pending across pumps instead of skipping solver input samples.
    unsigned budget = app->FrameStepPlayback() ? UINT_MAX : 8192;
    while (budget-- && clock.Due(target, rate)) {
        Cursor(app) = clock.Next(rate);
        ++clock.step;
        AdvancePlaybackPhysicsTracks(app);
        StepPlaybackPhysics(app, selActive, seconds);
    }
    return clock.Due(target, rate) ? Cursor(app) : target;
}
}

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
        const int shown = static_cast<int>(n * double(s.PlaybackTiming().playbackFps) / fps + frameA);
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
    // The idle physics pass still uses the wall-clock delta. Active playback
    // instead advances at the independent fixed simulation timestamps.
    g_CatchupDtBudget = s.state.deltaTime;                   // 0xA06BC
    if (s.PlaybackActive() == 0)
        return;                                    // -> 0x46F575

    if (s.FrameStepPlayback() != 0) {
        // Output samples have their own rate, independent of the rate used
        // to map elapsed seconds to authored frames.
        const std::int32_t n = s.state.recordedFrameCount;
        const std::int32_t frameA = s.AviRecordStartFrame();
        const float fps = s.AviRecordFps();
        const int playbackFps = s.PlaybackTiming().playbackFps;
        const auto samples = recording_state::OutputFrameCount(frameA, s.AviRecordEndFrame(), fps, playbackFps);
        if (std::uint64_t(n) >= samples) {
            FrameStepBeyondEnd(app);
            return;
        }
        const double frame = frameA + n * double(playbackFps) / fps;
        Cursor(app) = AdvanceFixedPhysics(app, 0, frame / playback_timing::kTimelineFps);
        PlaybackPoseAdvance(app, 0);
        return;
    }

    // ---- 5. playing (0x46F264..) ------------------------------------------
    // MIKUMIKUSTUDIO_TRACE_PLAY: per-substep bone dump for the first model, first
    // 40 catch-up substeps after play starts (diagnostic only; compiled
    // under MIKUMIKUSTUDIO_DIAG).
#ifdef MIKUMIKUSTUDIO_DIAG
    const int count = s.NativePhysicsMode();
    const bool selSkip = selActive != 0;
    static int tracePlayLeft = -1;
    static int tracePassLeft = -1;
    if (const char* tp = getenv("MIKUMIKUSTUDIO_TRACE_PLAY")) {
        if (tracePlayLeft < 0) {
            FILE* tf = fopen(tp, "a");
            if (tf) {
                fprintf(tf, "play-start end=%f cursor=%f count=%d\n",
                        s.PlaybackEndSeconds(), Cursor(app),
                        count);
                fclose(tf);
            }
            tracePlayLeft = 400;
            tracePassLeft = 100;
            unsigned char* mdl = s.ModelSlot(0);
            FILE* tf2 = fopen(tp, "a");
            if (tf2 && mdl != nullptr) {
                const std::int32_t boneCnt = mdl::Mdl(mdl)->boneCount;
                unsigned char* const active =
                    mdl::Mdl(mdl)->boneTrackActive;
                std::uint32_t* const cursors =
                    mdl::Mdl(mdl)->boneKeyCursors;
                mikumikustudio::mdl::BoneRecord* const bones = mikumikustudio::mdl::Bones(mdl);
                const mdl::BoneKey* const keyList = mdl::BoneKeys(mdl);
                fprintf(tf2, "loadstate boneCnt=%d\n", boneCnt);
                for (int b = 0; b < boneCnt && b < 12; ++b)
                    fprintf(tf2,
                            "bone%d active=%d cursor=%d quat=%08X "
                            "keyframe=%d nextframe=%d\n",
                            b, active ? active[b] : -1,
                            cursors ? cursors[b] : -1,
                            *reinterpret_cast<std::uint32_t*>(
                                &bones[b].rotQuat[0]),
                            (keyList && cursors)
                                ? static_cast<int>(keyList[cursors[b]].frame)
                                : -1,
                            (keyList && cursors &&
                             keyList[cursors[b]].next != 0)
                                ? static_cast<int>(
                                      keyList[keyList[cursors[b]].next].frame)
                                : -1);
                fclose(tf2);
            } else if (tf2) {
                fclose(tf2);
            }
        }
    }
#endif
    const std::uint64_t now =
        (static_cast<std::uint64_t>(s.TimeNowHigh()) << 32) | s.TimeNowLow();
    const std::uint64_t t0 =
        (static_cast<std::uint64_t>(s.PlaybackClockAnchorHigh()) << 32) |
        s.PlaybackClockAnchorLow();
    // x64 0x7FF7CB44BB86..0x7FF7CB44BBB3, all single precision: the 64-bit
    // tick delta goes straight into a float (cvtsi2ss rax), then mulss by
    // MilliToSec and addss the start seconds.  The old port widened
    // everything to double and rounded once at the store.
    // The tick comes from the 32-bit timeGetTime counter, which wraps
    // every 49.7 days. When that wrap falls between the anchor and
    // now, the 64-bit subtraction below yields about -2^32 ms: the target
    // lands far below the cursor, the cursor is written negative, and the
    // anchor is never renewed, so the timeline stays stuck until playback is
    // stopped and restarted. The original subtracts exactly the same way
    // (sub rax, [r12 + 0x9fcb0] at 0x7FF7CB44BB92, then cvtsi2ss), so this
    // port deliberately deviates and treats one wrapped 2^32 period as
    // elapsed time, keeping elapsed time continuous across the wrap.
    // Defect AB-04.
    std::int64_t elapsed =
        static_cast<std::int64_t>(now - t0);
    if (now < t0)
        elapsed += static_cast<std::int64_t>(1) << 32;
    constexpr double milliseconds = 0.001;
    const double target = s.PlaybackStartSeconds() + elapsed * milliseconds *
        playback_timing::Speed(s.PlaybackTiming());
    const double end = s.PlaybackEndSeconds();

#ifdef MIKUMIKUSTUDIO_DIAG
    if (tracePassLeft > 0) {
        --tracePassLeft;
        if (FILE* tf = fopen(getenv("MIKUMIKUSTUDIO_TRACE_PLAY"), "a")) {
            fprintf(tf,
                    "pass cursor=%.5f target=%.5f end=%.5f elapsed=%.3f "
                    "b330=%d sel=%d\n",
                    Cursor(app), target, end, static_cast<double>(elapsed),
                    s.PlaybackActive(), selSkip);
            fclose(tf);
        }
    }
#endif

    const double last = (std::min)(target, end);
    Cursor(app) = AdvanceFixedPhysics(app, selActive, last);
    PlaybackPoseAdvance(app, 0);
    if (target > end && Cursor(app) >= last) PlayingBeyondEnd(app);
    return;
}

}  // namespace mikumikustudio
