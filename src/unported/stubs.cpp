// ===========================================================================
// Late-port functions (ledger: 418 ported / 0 stub)
// MAINTENANCE: production-keep despite the historical path; C06 in
// docs/records/development/workspace.md tracks a possible move, not deletion.
// ===========================================================================
// Everything defined with a body below is a verified full port of its
// original VA (bodies were kept here when they had no natural subsystem
// file yet); comments still record each original VA and behaviour notes.
// NOTE on the directory name: "src/unported/" is historical - zero stubs
// remain.  Every no-op twin that used to live here was deleted once its
// real body landed elsewhere, and the comments below record where each
// port lives.  The file stays at this path (moving it would only churn
// CMakeLists.txt and every cross-reference for no behavioral gain).
// The porting ledger (docs/PORTING_STATUS.md) is the authoritative tracker.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/image_sequence_export.hpp"
#include "mikudancestudio/transparent_capture.hpp"
#include "mikudancestudio/wic_mov_recorder.hpp"
#include <Windows.h>
#include <d3d9.h>
#include <mmsystem.h>   // timeEndPeriod

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <locale.h>
#include <new>

#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/globals.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/mf_h264_recorder.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/recording_state.hpp"
#include "../app/device_lost_wait.hpp"

#include "mikudancestudio/texture_lookup.hpp"

namespace mikudancestudio {

// Full-port callees defined in other TUs (local declarations).
void RefreshAfterFrameApply(MMDApp* app);      // VA 0x432FA0 (ui_frame_step.cpp),
                                                // was Sub432FA0

// ---- frame-driver tail targets (0x0046B090) -------------------------------
// TimelineAdvance (0x460130, actually the frame-capture/screenshot tail)
// is a full port in src/app/frame_driver.cpp.
// PostDeviceReset (0x440DB0) is a full port in src/render/device_reset.cpp.
// 0x433A40 UpdateBoneFrames is ported in src/app/playback_state.cpp.

// ---- playback catch-up dependencies (0x0046EEE0..0x0046F575) -------------
// VA 0x004A31D0 (per-model keyframe application) is ported in
// src/model/model_keyframe_advance.cpp together with its helpers
// 0x4A05A0 (VMD easing) and 0x499B50 (bone physics-mode notify).

// ---------------------------------------------------------------------------
// VA 0x00464A00 - FinishAviRecord: recording-wait epilogue (called at 0x46F122
// after the frame-step wait finishes, from RecWndProc's IDYES and the AVI
// dump flow).  Full port:
//   timeEndPeriod(1); stop-playback restore (0x4341E0); the 0x9EDD4 flag
//   byte poke; TeardownDShowGraph (0x409320) on the recorder at +0xA06C0;
//   lazy GetBackBuffer refetch (vtable +0x48) and SetRenderTarget(0, bb)
//   when the wrapper byte at +0x1D4F8 equals 0 (vtable +0x94); the separate-window
//   branch (byte 0xA0D61 = state.aviStereoOutput; the x64 reference reads
//   app+0xA1E0D at 0x140089561): clear 0x9FE74, call ApplyFullscreenWindowState
//   fullscreen restore, PostDeviceReset, clear aviStereoOutput; otherwise the
//   device-vs-window size check (adopt window size + PostDeviceReset) and
//   DestroyWindow of the wait window 0xA0D24; enable control 408; viewport
//   refresh (0x42C810) + Sub432FA0 + PostViewRefresh; Release() the two COM
//   objects at 650120/650124; free the buffer at 652084; ShowWindow(SW_SHOW)
//   the main and separate windows when aviStereoOutput is clear. That last
//   guard is a tautology in the original as well: the x64 reference clears the
//   same byte at 0x14008960B and tests it at 0x14008969C, so both paths leave
//   it zero and the two ShowWindow calls always run. Defect FB-15.
// ---------------------------------------------------------------------------
void FinishAviRecord(MMDApp* app, bool reachedRequestedEnd) {  // was Sub464A00
    recording_state::Guard finishing(app, recording_state::Operation::Finish);
    if (!finishing) return;
    auto& s = *app;
    timeEndPeriod(1);                                       // 0x464A07
    s.state.playbackActive = 0;             // 0x464A11
    StopPlayback(app);                                         // 0x464A17
    unsigned char* flag = s.RecordingCompletionFlag();      // 0x464A1C
    if (flag != nullptr)
        *flag = 1;                                          // 0x464A28
    const auto expectedFrames = recording_state::OutputFrameCount(
        s.AviRecordStartFrame(), s.AviRecordEndFrame(), s.AviRecordFps());
    const bool mfRecording = IsBuiltInH264Recording(s.Recorder());
    std::wstring sequenceError;
    const bool sequenceComplete = image_sequence::Finish(app, reachedRequestedEnd, sequenceError);
    const bool mfComplete = mfRecording &&
        FinishBuiltInH264Recording(s.Recorder(), expectedFrames);
    const bool movRecording = IsPngMovRecording(s.Recorder());
    std::wstring movError;
    bool movComplete = false;
    if (movRecording) {
        if (reachedRequestedEnd) movComplete = FinishPngMovRecording(s.Recorder(), expectedFrames, movError);
        else AbortPngMovRecording(s.Recorder());
    }
    ReleaseTransparentCapture(app);
    image_sequence::EndStill(app);
    bool dshowComplete = false;
    TeardownDShowGraph(s.Recorder(), &dshowComplete);           // 0x464A31
    D3DRenderer* r = s.Renderer();
    if (r != nullptr && r->device != nullptr && !device_lost_wait::Pending(app)) {
        IDirect3DDevice9* device = r->device;               // 0x1D4E0
        IDirect3DSurface9** backBuffer = &r->backbufferSurface;
        if (*backBuffer == nullptr)                         // 0x464A3C
            device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
                                  backBuffer);              // 0x464A5A
        if (r->multisampleAvailable == 0)
            device->SetRenderTarget(0, *backBuffer);        // 0x464A81
    }
    if (s.state.aviStereoOutput != 0) {                     // 0xA0D61
        s.state.fullscreenMode = 0;                    // 0x464B0B
        ApplyFullscreenWindowState(app);                                     // 0x464B11
        PostDeviceReset(app);                               // 0x464B18
        s.state.aviStereoOutput = 0;                    // 0x464B1D
    } else {
        const std::int32_t winW = s.state.recRTW;
        const std::int32_t winH = s.state.recRTH;
        if (r != nullptr &&
            (r->screenWidth != winW || r->screenHeight != winH)) {
            r->presentParameters.BackBufferWidth = winW;   // 0x1D4FC
            r->presentParameters.BackBufferHeight = winH;  // 0x1D500
            r->screenWidth = winW;                          // 0x1D4E4
            r->screenHeight = winH;                         // 0x1D4E8
            PostDeviceReset(app);                           // 0x464AF5
        }
        DestroyWindow(s.RecordingWindow());                 // 0x464B01
    }
    s.RecordingWindow() = nullptr;                          // 0x464B31
    if (s.AviRenderResolutionOverridden()) {
        s.RenderWidth() = s.AviSavedRenderWidth();
        s.RenderHeight() = s.AviSavedRenderHeight();
        s.AviRenderResolutionOverridden() = false;
    }
    EnableWindow(GetDlgItem(static_cast<HWND>(s.Hwnd()), panel::kPlayButton), 1);  // 0x464B3E
    if (!device_lost_wait::Pending(app))
        RefreshMainWindowViewport(app);                         // 0x464B46
    RefreshAfterFrameApply(app);                            // 0x464B4D
    PostViewRefresh(app);                                   // 0x464B54
    if (IDirect3DSurface9* com = s.CaptureRenderTarget()) {        // 0x464B69
        com->Release();
        s.CaptureRenderTarget() = nullptr;
    }
    if (IDirect3DSurface9* com = s.CaptureSystemSurface()) {       // 0x464B81
        com->Release();
        s.CaptureSystemSurface() = nullptr;
    }
    // The pixel buffer is allocated with ::operator new(capW * capH * 4) at
    // 0x46E88D (record_readback.cpp), but this epilogue freed it with the CRT free
    // while ShutdownCleanup freed the same field with
    // ::operator delete (0x462D0E) - two different releases for one block.
    // free() on memory from ::operator new is undefined behaviour even though the
    // MSVC CRT heap happens to make it work, so this release now matches the
    // allocation; the x64 reference likewise releases the buffer through a delete
    // form (operator delete[] at 0x14000BE08). Both release points clear the
    // slot, so neither can release the block twice.
    // Defect AB-05.
    if (void* buf = s.state.captureReadbackPixels) {                 // 0x464B94
        ::operator delete(buf);
        s.state.captureReadbackPixels = nullptr;
    }
    if (s.state.aviStereoOutput == 0) {                 // 0x464BA2
        ShowWindow(static_cast<HWND>(s.Hwnd()), SW_SHOW);   // 5
        HWND separate = s.FloatingWindow();
        if (separate != nullptr)
            ShowWindow(separate, SW_SHOW);                  // 0x464BC8
    }
    // Keep the recording guard through ShowWindow's synchronous focus
    // restoration, so stale edit text cannot seek or register a frame.
    s.state.recordPlaybackStartPending = 0;
    s.state.frameStepPlayback = 0;
    if (!movError.empty()) MessageBoxW(static_cast<HWND>(s.Hwnd()), movError.c_str(), L"MOV export", MB_OK | MB_ICONERROR);
    if (!sequenceError.empty()) MessageBoxW(s.MainWindow(), sequenceError.c_str(),
        image_sequence::Text(app, L"导出图片序列", L"Image sequence export", L"連番画像出力"), MB_OK | MB_ICONERROR);
    if (reachedRequestedEnd && (mfComplete || dshowComplete || movComplete || sequenceComplete) &&
        s.AviCloseAfterExport())
        PostMessageW(static_cast<HWND>(s.Hwnd()), WM_CLOSE, 0, 0);
}

// StopPlayback (0x4341E0 stop playback) and the Sub4C2760/4C34A0
// audio-timer helpers are defined further below (400-family). 0x463640
// (edit commit) is ported in src/window/ui_edit_commit.cpp; 0x4C2680/
// 0x4C2A00/0x4C2B80 live in ui_refresh.cpp / ui_timeline_gfx.cpp.

// ---- lifecycle --------------------------------------------------------------
// VA 0x00462C40 (0x6A8) - DirectX teardown before free(Block) - full port in
// src/app/shutdown_cleanup.cpp (with 0x409320/0x4096C0/0x4030F0/0x4C2C40/
// 0x406BE0/0x4CB4F0).

// VA 0x0042A110 - model count query behind ExpGetPmdNum.  Original walks the
// 100-slot model array at +0x780 in an unrolled 20x5 loop counting non-null
// entries (the loop body is the compiler's unroll of `for i in 0..100`).
// x64 twin: the ExpGetPmdNum export body itself (sub_7FF7CB4FB5F0) counts
// non-null entries in a 255-count do/while over app+0xBE8.
int GetPmdNum(MMDApp* app) {
    int count = 0;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        if (app->ModelSlot(slot) != nullptr)
            ++count;
    }
    return count;
}

// ---- subsystem inits reached from InitMainWindowAndD3D (0x0047A5B0) ------
// 0x406D40 - render-subsystem construction/reset for the 0x1D574 object.
// The original defaults three device-state gates that the render passes
// key on: +120164 = 1 (stencil shadow passes), +120024 = 1 (MaxAnisotropy
// seed, replaced by InitD3D caps), +120176 = 1 (anisotropic sampler path).
// Leaving this a no-op silently disabled the stencil shadow compositing
// and the anisotropic filters, which the d3d9 call-trace A/B surfaced as
// the missing STENCILFUNC/REF/PASS trios on the port side.
void RendererInit(D3DRenderer* r) {
    InvalidateTextureLookup();
    r->d3d9 = nullptr;
    r->device = nullptr;
    r->lineVertexBuffer = nullptr;
    r->captureSurface = nullptr;
    r->backbufferSurface = nullptr;
    r->depthStencilSurface = nullptr;
    r->hdrTexture = nullptr;
    r->spriteTexture = nullptr;
    r->shadowSurface = nullptr;
    r->shadowDepthSurface = nullptr;
    r->effect = nullptr;
    r->screenWidth = 0;
    r->screenHeight = 0;
    r->multisampleAvailable = 0;
    r->postProcessEnabled = 0;
    // 0x406D94: the 10000-entry resource pool keeps two of every three
    // dwords zeroed (the tag is left untouched).
    for (auto& entry : r->resourcePool) {
        entry.heapBuffer = nullptr;
        entry.comObject = nullptr;
    }
    r->d3dInitialized = 1;
    r->shaderModelCaps = 1;
    r->runtimeToggle = 1;
    r->localeTable[0] = _create_locale(0, "Locale");
    r->localeTable[1] = _create_locale(0, ".OCP");
    r->localeTable[2] = _create_locale(0, ".ACP");
    r->localeTable[3] = _create_locale(0, "JPN");
    // 0x406E18..0x406E42: nvapi stereo probe chain.  sub_4C6940 loads
    // nvapi.dll and runs NvAPI_Initialize; on success sub_4CAF10(1) queries
    // the stereo caps interface (0xBE7692EC) and sub_4CB210 the support
    // gate (0x239C4545).  stereoEnabled = 1 only when every step returns 0.
    // On drivers without 3D Vision support the caps call fails and the byte
    // stays 0 - matching the measured original on this machine - but on a
    // stereo-capable driver the port now follows the original instead of
    // hard-disabling.
    {
        int status = NvapiInitChain();                        // 0x406E18
        if (status != 0)
            status = NvapiStereoCaps(1);                      // 0x406E28
        if (status != 0) {
            r->stereoEnabled = 0;                             // 0x406E2F
        } else {
            r->stereoEnabled = (NvapiStereoSupportGate() == 0) ? 1 : 0;  // 0x406E42
        }
    }
}
// FontSubInit (0x408E70) is ported in src/io/pmm_load_shell.cpp.
// InitFlagSubsystem (0x461E00) / SaveFlagSubsystem (0x461FA0) - the
// separate ("Mic") window create/destroy pair - are ported in
// src/window/mic_window.cpp.
// Sub025CInit (0x4C2450) / Sub048Init (0x401360) / WaveSeekAndFeed (0x4C34A0)
// are ported in src/app/subsystem_init.cpp; 0x4C2760 is WaveStartPlayback
// in src/media/wave_audio.cpp.
// 0x458F80 LoadSceneFile + 0x450000 v2 body live in src/io/pmm_load_*.cpp.

// ---- message targets referenced by MainWndProc (0x004C3A10) --------------

// ---- file loaders reached from the command dialogs ------------------------
// 0x418A10 LoadVpdFile / 0x418750 SaveVpdFile: src/io/vpd_file.cpp.
// ResetAppState (0x44E540 scene reset) is a full port in
// src/app/reset_app_state.cpp.
// 0x418500 LoadWaveFile: full port in src/media/wave_audio.cpp; the old
// path-arg twin here was dead code (all callers take the app object) and
// was removed.
// 0x41B080 SaveSceneFile is ported in src/io/pmm_save.cpp.
// VA 0x00434B60 (VMD motion load) is ported in src/model/vmd_load.cpp.
// 0x419370 SaveVmdFile is ported in src/io/vmd_save.cpp.
// 0x433250 LoadAviFile: full port in src/media/media_load.cpp; the old
// path-arg twin here was dead code (all callers take the app object) and
// was removed.
// SelectFrameGroup (0xD9/DA/DC) is ported in src/window/frame_line_edit.cpp.

// ---- morph color re-eval (placeholder until the UI phase ports 0x430510) --

// ---- editor-panel refresh helpers (wave-3 call targets) -------------------
// Prototypes are `int __thiscall(_DWORD)` (this = app) in the IDB; names are
// provisional address-style placeholders - renamed in the finishing phase.
// Signatures marked (model,...) take the model pointer (slot array element),
// matching the call sites ported from the wave-3/4 TUs.
// 0x412330 (gravity-track apply + physics dialog refresh) and
// 0x413120 (accessory key-track apply) are ported in
// src/model/track_apply.cpp.
// Sub416280 (interp-curve drag handler) is ported in
// src/window/ui_selection_reeval.cpp.
// RelayoutSidebarControls (0x442EB0) is ported in ui_windowsize.cpp.
// VA 0x004B4260 (frame seek / pose rebuild) is ported in
// src/model/model_frame_seek.cpp.
// WaveRestartAt (was Sub4C3530; 0x4C3530 audio-timer restart) is ported in
// src/app/subsystem_init.cpp.
// 0x4168D0 (AVI background frame update) is ported in
// src/render/bg_overlay.cpp.  [Defect RB-13]
// CompareFunction (0x40EC70) is ported in src/window/ui_editor_click.cpp.
// RefreshMainWindowViewport (0x42C810) is ported in src/window/ui_viewport_refresh.cpp.
// Sub40CAC0 (0x40CAC0) is ported in src/window/ui_viewport_layout.cpp -
// viewport overlay layout for ids 536..557 (was the silent no-op that kept
// those 22 controls stacked at their creation-time fallback coordinates).

// ---- command-family helpers (wave-4 call targets) --------------------------
// 0x417130 (picture background quad rebuild) is ported in
// src/render/bg_overlay.cpp.  [Defect RB-13]
// 0x435FE0 (VSQ load / auto lipsync) is a full port in
// src/io/vsq_load.cpp (LoadVsqFile); the old stub here was removed.
// 0x4337A0 background picture load (was Sub4337A0) is a full port in
// src/media/media_load.cpp (LoadBackgroundPicture).
// 0x42AE40 CopyPathW lives in src/media/media_load.cpp; the twin stub here
// was dead code (callers use CopyPathW) and was removed.
// 0x464760 StartAviRecordFullscreen / 0x45E820 StartAviRecordWindow are
// full ports in src/app/avi_record_start.cpp (callers there use the real
// names); the old no-op twins here were dead code and were removed.
// Sub414110 (0x414110) is ported in src/window/accessory_paste.cpp.
// 0x4C46F0: the original is the identity ctor `return this;` (verified on
// the decompile) - callers ignore the return value, so an empty body is
// the faithful port, not a placeholder.
void IdentityCtor(void* obj)            { (void)obj; }  // was Sub4C46F0
// 0x441070 JumpNextKeyframe / 0x4414C0 JumpPrevKeyframe (timeline
// registration jumps) are full ports in src/app/app_gap_bodies.cpp; the old
// no-op stubs here were removed (callers rewired in command_control_500.cpp).
// ApplyCameraReferenceModeChange (was Sub41ACD0; 0x41ACD0 camera-reference
// switch re-anchor) is ported in
// src/app/frame_modes.cpp.
// PushBoneEditUndo (0x42D6E0) is ported in src/model/bone_edit_undo.cpp.
// Sub401150 (0x401150 array ctor) is ported in src/window/accessory_paste.cpp.
// Shared, lossless legacy path encoder (independent of the UI/CRT locale).
void WideToSjisPath(char* dst, const wchar_t* src, std::size_t size) {
    text_encoding::EncodePath(dst, size, src);
}
// 0x4341E0 stop-playback restore is ported in src/app/playback_state.cpp.
// Sub40A710 (0x40A710 dispose+free wrapper) is ported in
// src/model/model_dispose.cpp; SeekSelectedModelToCurrentFrame (0x4220C0 seek+flag) in
// src/model/model_frame_seek.cpp.

// ---- VMD loader dependencies (0x00434B60; registrars pending phases) ----
// PanelPaint (0x414610) is ported in src/window/ui_panel_paint.cpp; the old
// duplicate stub here used to win the link (stubs.obj is always pulled in for
// the other placeholders), silently blacking out the timeline panel.
// ---- dialog-proc dependencies (dialog_procs.cpp; VAs recorded) -----------
// 0x466630/0x461C20/0x4256C0/0x43D2E0/0x43D560/0x43D610 (select-dialog
// family, real signatures thiscall(app, hDlg...)) are ported in
// src/window/dialog_select_ops.cpp.

// 0x410AA0/0x411900/0x4120B0 global-track key registrars: real (app, rec)
// overloads live in src/window/command_control_400.cpp.

// ---- v2 loader dependencies (0x00450000 phase 2) --------------------------
// ClearTimelineAndCurveDCs (0x40AE00) is ported in src/window/ui_init.cpp; it initializes
// the timeline and interpolation-curve GDI caches.  RefillBoneRegisterCombo (bone-register
// combo refill) is also a full port there.
// 0x4C4700 accessory-track dtor (was Sub4C4700) is DisposeAccessory in
// src/render/accessory.cpp.
// 0x0040FF80 PMM model dialogs now live in src/window/ui_message.cpp.
}  // namespace mikudancestudio
