#include "mikumikustudio/frame_limits.hpp"
// ===========================================================================
// VA 0x0044AAA0 - HandleLButtonDblClk  (original: sub_44AAA0)
// ===========================================================================
// WM_LBUTTONDBLCLK handler (sole caller: WndProc 0x4C3A10 @ 0x4C41BB).
//
// Guard: byte this+0xA0274 (655988) set -> no-op.
//
// Body (gated on compact-panel geometry: this+8 (height) in (144,160),
// this+4 (width) in (100, sidebar-18) with byte this+0x330 clear):
//   1. Model IK sweep - for each of the 100 model slots (this+0x780):
//      if any IK display flag byte (model+0x2D98 array, count model+0x2D84)
//      is non-zero, snapshot the selection (sub_4A0080, ecx = model,
//      stack = app+0x980 frame value) then zero the whole flag array.
//   2. Frame text sync to the frame-number edit box (control 0x1A1 = 417):
//      this+0x980 = this+0x97C + (this+4 - 100) / 13 (signed div by 13);
//      EM_SETSEL (0xB1) to end of the old text, WM_SETTEXT (0xC2) with
//      sprintf_s("%d").  GetDlgItem is re-fetched before every call like
//      the original.
//   3. Per-slot frame apply: sub_4B4260(model, frame, this+0xA0CC4) for
//      every loaded slot; the active slot (this+0x910, byte) additionally
//      gets the panel-state sync sub_4A02C0(model).
//   4. Mode branch on byte this+0x2F8 (kByteOptflag0):
//        set   -> ReloadModels (0x42E640) + RefreshLightPanel/RefreshSelfShadowPanel/ApplyGravityTrack
//                 + ApplyAccessoryTrack per non-null slot of the 255-slot array at
//                 this+0x9DD70 + SyncAccessoryEditPanel
//        clear, byte this+0x9ED98 set -> zero floats this+0x308/+0x30C,
//                 same reload chain, then if this+0xA0430 >= 0:
//                 Sub4970B0(model at that slot) and SetPhysicsMode (0x4A9220)
//                 (model, 0, slots, this+0xA0CC4); then PostModelReload
//                 (0x41A650)
//        clear, byte 0x9ED98 clear -> EnableWindow(ctrl 400, TRUE) and
//                 EnableWindow(ctrl 401, FALSE)
//   5. Scroll clamp: this+0x97C = frame - (sidebar-84)/26 (unsigned
//      compare, clamp at 0), then PanelPaint (0x414610).
//   6. Timeline strip: gated on byte this+0xA06CC - TimelineDrawTicks
//      (0x4C2A00, frame offset + sidebar width), InvalidateRect of
//      {6, 95, sidebar-3, 146}; then gated on byte this+0xA0196 with the
//      pre-store value of byte this+0xA03E9 == 0: SetFrameNormalized
//      (0x4C2B80, this+0xA4424) and sub_4C3530 (timer-reset, ecx =
//      this+0xCC subsystem) with (double)(unsigned)(frame-1) / 30.0
//      (negative frame adds 2^32f first), clamped >= 0.
//   7. Tail: if this+0x91C == 1 -> AviBgOverlayRefresh; byte this+0x9EDB5 = 1.
//
// Reference: ../translated/MikuMikuDance/fcn_0044aaa0.cpp
//   NOTE: the translated file is a register-tracking re-render that deviates
//   in places (arg lists of the model helpers, IK field offsets); this port
//   follows the IDA decompilation of MikuMikuDance.exe 0x44AAA0.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {

// Forward declarations for functions ported in other translation units
// (declared here with their original VAs; bodies in the files noted).
void PanelPaint(MMDApp* app);                                   // VA 0x00414610 (ui_panel_paint.cpp)
void TimelineDrawTicks(int frameOffset, int width);             // VA 0x004C2A00 (ui_timeline_gfx.cpp)
void SetFrameNormalized(int frame);                             // VA 0x004C2B80 (ui_timeline_gfx.cpp)

// Editor-panel refresh helpers, ported in other translation units.
void RefreshLightPanel(MMDApp* app);                                    // VA 0x00411070 (ui_frame_refresh.cpp)
void RefreshSelfShadowPanel(MMDApp* app);                                    // VA 0x00411B90 (ui_frame_refresh.cpp)
void ApplyGravityTrack(MMDApp* app);                                    // VA 0x00412330 (track_apply.cpp)
void SyncAccessoryEditPanel(MMDApp* app);                                    // VA 0x004134E0 (ui_frame_refresh.cpp)
void ApplyAccessoryTrack(MMDApp* app, int idx);                           // VA 0x00413120 (accessory_paste.cpp)
void SnapshotPoseBeforeFrameChange(unsigned char* model, int frame);  // VA 0x004A0080, was Sub4A0080 (model_keyframe_edit.cpp)
void SyncModelEditControls(unsigned char* model);                     // VA 0x004A02C0, was Sub4A02C0 (model_keyframe_edit.cpp)
int SeekModelFrame(unsigned char* model, int frame, int physicsMode);  // VA 0x004B4260 (model_frame_seek.cpp)
void WaveRestartAt(void* sub, double v);                       // VA 0x004C3530, was Sub4C3530 (subsystem_init.cpp)
void AviBgOverlayRefresh(MMDApp* app);                                    // VA 0x004168D0 (bg_overlay.cpp)

// VA 0x0044D940 - row/panel mode toggle (combobox-driven rebuild of the
// accessory/camera panel + menu enable state).  Full port lives in
// src/window/ui_model_reload.cpp (verified 2026-09 branch-by-branch
// against the x64 twin sub_7FF7CB486B10); G5 of the pump's TAB/VK226
// combo cycle (src/app/pump_edit_keys.cpp) is a new caller.
void ApplyModelComboSelection(MMDApp* app);

void HandleLButtonDblClk(MMDApp* app) {
    if (app->FullscreenMode() != 0)                             // 655988 (0xA0274)
        return;

    const HWND hwnd = static_cast<HWND>(app->state.hwnd);  // 657080
    app->CameraAttachmentTransformSuppressed() = 0;
    RECT rect;
    GetClientRect(hwnd, &rect);                                 // output never read in the original

    // compact-panel geometry gate (asm order: width < sidebar-18, height
    // < 160, height > 144, width > 100, then byte 0x330 clear)
    if (app->MouseY() > 144 &&
        app->MouseY() < 160 &&
        app->MouseX() <
            app->SidebarWidth() - 18 &&
        app->MouseX() > 100 &&
        app->PlaybackActive() == 0) {

        // ---- 1. model IK sweep: snapshot + clear any set IK flags ---------
        // x64 twin sub_7FF7CB45D790 sweeps all 255 slots (count-down from
        // 0xFF at 0x7FF7CB45D840).
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* model = app->ModelSlot(i);
            if (model == nullptr)
                continue;
            const std::int32_t flagCount = mdl::Mdl(model)->boneCount;
            if (flagCount <= 0)
                continue;
            unsigned char* ikFlags = mdl::Mdl(model)->bonePhysicsState;
            int idx = 0;
            while (ikFlags[idx] == 0) {
                if (++idx >= flagCount)
                    break;
            }
            if (idx < flagCount) {
                // original: sub_4A0080(ecx = model, stack = app+2432 frame)
                SnapshotPoseBeforeFrameChange(model, app->state.currentFrame);
                for (int j = 0;
                     j < static_cast<int>(mdl::Mdl(model)->boneCount); ++j)
                    mdl::Mdl(model)->bonePhysicsState[j] = 0;
            }
        }

        // ---- 2. frame number -> edit box (control 0x1A1 = 417) ------------
        // this+0x980 = this+0x97C + (this+4 - 100) / 13
        const auto nextFrame = std::int64_t(app->state.timelineStartFrame) + (std::int64_t(app->MouseX()) - 100) / 13;
        if (nextFrame < 0 || nextFrame > frame_limits::maximum) { MessageBeep(MB_ICONWARNING); return; }
        app->state.currentFrame = static_cast<int>(nextFrame);
        const LRESULT textLen = GetWindowTextLengthA(GetDlgItem(hwnd, panel::kCurrentFrameEdit));
        SendMessageA(GetDlgItem(hwnd, panel::kCurrentFrameEdit), EM_SETSEL, 0, textLen);
        char frameText[256];                                    // 0x100
        sprintf_s(frameText, 0x100u, "%d",
                  app->state.currentFrame);
        SendMessageA(GetDlgItem(hwnd, panel::kCurrentFrameEdit), EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>(frameText));

        // ---- 3. per-slot bone-frame apply + active-slot panel sync --------
        // x64 twin sub_7FF7CB45D790: apply loop runs v16 < 255
        // (0x7FF7CB45D988..0x7FF7CB45D9CB).
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* model = app->ModelSlot(i);
            if (model == nullptr)
                continue;
            // original: sub_4B4260(ecx = model, frame = app+2432,
            //            app+0xA0CC4)
            SeekModelFrame(model,
                      app->state.currentFrame,
                      app->PlaybackPhysicsMode());
            if (i == static_cast<int>(app->SelectedModelSlot())) {
                // original: sub_4A02C0(ecx = model)
                SyncModelEditControls(model);
            }
        }

        // ---- 4. mode branch: model row / accessory row / light row --------
        if (app->state.optflag[0] != 0) {  // 760 (0x2F8)
            ReloadModels(app);                                    // 0x42E640
            RefreshLightPanel(app);                                       // 0x411070
            RefreshSelfShadowPanel(app);                                       // 0x411B90
            ApplyGravityTrack(app);                                       // 0x412330
            for (int i = 0; i < 255; ++i) {                       // 0xFF slots
                if (app->AccessorySlot(i) != nullptr)
                    ApplyAccessoryTrack(app, i);                            // 0x413120
            }
            SyncAccessoryEditPanel(app);                                       // 0x4134E0
        } else if (app->state.followCameraEnabled != 0) {  // 650648
            // The eye is the rendered viewpoint and must be preserved across this reload.
            // Clearing the offsets here shifted the view on every mouse move, which
            // caused the drift observed when dragging with the move/rotate tools.
            // Refer to the eye-ownership note in src/app/timeline_advance.cpp.
            ReloadModels(app);                                    // 0x42E640
            RefreshLightPanel(app);
            RefreshSelfShadowPanel(app);
            ApplyGravityTrack(app);
            for (int i = 0; i < 255; ++i) {
                if (app->AccessorySlot(i) != nullptr)
                    ApplyAccessoryTrack(app, i);
            }
            SyncAccessoryEditPanel(app);
            const std::int32_t sel =
                app->CameraParentModel();
            if (sel >= 0) {
                unsigned char* selModel = app->ModelSlot(sel);
                // original: sub_4970B0(ecx = model at slot sel)
                ModelApplyMorphs(selModel);                       // 0x4970B0
                SetPhysicsMode(selModel, 0,
                               app->ModelSlots(),
                               app->NativePhysicsMode());                    // 0x4A9220
            }
            // A timeline click only changes the tracked pose. The render path
            // resolves the current parent bone as its pivot, just as it does
            // in camera mode. PostModelReload is reserved for one-time mode or
            // reference changes; calling it on every click rewrites the eye in
            // model mode and causes the view to drift relative to camera mode.
        } else {
            EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);            // 0x190
            EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);           // 0x191
        }

        // ---- 5. scroll clamp + panel repaint -------------------------------
        const std::int32_t pages =
            (app->SidebarWidth() - 84) / 26;
        const std::uint32_t frameU =
            static_cast<std::uint32_t>(app->state.currentFrame);
        if (frameU <= static_cast<std::uint32_t>(pages))          // unsigned compare (jbe)
            app->state.timelineStartFrame = 0;       // 2428 (0x97C)
        else
            app->state.timelineStartFrame =
                static_cast<std::int32_t>(frameU - static_cast<std::uint32_t>(pages));
        PanelPaint(app);                                          // 0x414610

        // ---- 6. timeline strip redraw + normalized-time seek ---------------
        if (app->state.waveEnabled != 0) {   // 657100 (0xA06CC)
            TimelineDrawTicks(app->state.timelineStartFrame,
                              app->SidebarWidth());
            RECT rc;
            rc.left = 6;
            rc.top = 95;
            rc.right = app->SidebarWidth() - 3;
            rc.bottom = 146;
            InvalidateRect(hwnd, &rc, 0);
            if (app->state.wavPlaysOnFrameMove != 0) {  // 655766 (0xA0196)
                // gate flag read BEFORE the 0xA02B6 store (asm zf capture)
                const bool gate =
                    app->state.automaticFrameAdvanceEnabled == 0;  // 656361 (0xA03E9)
                app->AudioSeekReady() = 1;  // 0xA02B6
                if (gate) {
                    SetFrameNormalized(app->FrameNormalization());  // 0x4C2B80
                    const std::int32_t frameMinus1 =
                        app->state.currentFrame - 1;
                    // fild + (negative ? fadd 2^32f) + fdiv 30.0 ==
                    // (double)(unsigned)v31 / 30.0
                    double t = static_cast<double>(static_cast<std::uint32_t>(frameMinus1)) / 30.0;
                    if (t < 0.0)                                  // fldz/fcom clamp, never fires
                        t = 0.0;
                    // original: sub_4C3530(ecx = app+0xCC subsystem, t)
                    WaveRestartAt(app->Audio(), t);
                }
            }
        }

        // ---- 7. tail ---------------------------------------------------------
        if (app->state.aviBackgroundEnabled == 1)      // 2332 (0x91C)
            AviBgOverlayRefresh(app);                                       // 0x4168D0
        app->PhysicsResetPending() = 1;
    }
}

// ===========================================================================
// VA 0x004632F0 - HandleMouseActivate  (original: sub_4632F0)
// ===========================================================================
// WM_MOUSEACTIVATE handler (callers: WndProc 0x4C3A10 @ 0x4C41A6 and
// RecWndProc 0x466A10 @ 0x466C6E).  The original returns an HWND/BOOL
// (sub_44D940's / SetForegroundWindow's / SetCursorPos's result, or
// GetClientRect's BOOL when byte 0x330 is set); the project's void
// signature drops it (WndProc returns 0).
//
// Guard: byte this+0x330 (816) set -> no-op.
//
// Body (this+4 / this+8 are the window width / height fields):
//   fg = GetForegroundWindow();  cached = this+0xA0D38 (658744)
//   byte this+0x2F8 (kByteOptflag0) set:
//     if fg == cached OR this+8 <= clientBottom - 158:
//       CB_SETCURSEL (0x14E) on combo 0x1B4 (436) with wParam =
//       this+0xA042C (656428), then sub_44D940 (0x44D940, panel mode
//       rebuild) and return
//   byte 0x2F8 clear:
//     if this+8 > clientBottom - 158 AND fg != cached:
//       CB_SETCURSEL(436, 0), sub_44D940, return
//     else if this+0x914 (2324) != 0:
//       checkboxes 0x1EB (491), 0x1EC (492), 0x1ED (493) -> BM_SETCHECK 0,
//       0x1EA (490) -> BM_SETCHECK 1; this+0x914 = 0; cursor re-target
//       when this+8 > (int)((double)this+0xA0D4C - f*80.0) &&
//       this+4 > (int)((double)this+0xA0D48 - f*127.0) &&
//       this+4 < this+0xA0D48 && this+8 < this+0xA0D4C, where f =
//       float at locale-sub + 0x1D4F0 (120048): Point = (40,150) ->
//       ClientToScreen -> SetCursorPos; byte this+0x9F12C (651564) = 1;
//       SetForegroundWindow(hwnd)
//     else (this+0x914 == 0):
//       all four checkboxes -> BM_SETCHECK 0; this+0x914 = 2;
//       Point = (this+0xA0D48 - (int)(f*110.0),
//                this+0xA0D4C - (int)(f*65.0)); ClientToScreen against
//       cached window (also SetForegroundWindow(cached)) when non-null,
//       else against the main hwnd; SetCursorPos; byte 0x9F12C = 1.
//
// Reference: ../translated/MikuMikuDance/fcn_004632f0.cpp
//   NOTE: the translated file is a register-tracking re-render whose
//   control flow does not match the binary (it invents tick-count paths
//   and merges the branches); this port follows the IDA decompilation of
//   MikuMikuDance.exe 0x4632F0.
// =========================================================================//
void HandleMouseActivate(MMDApp* app) {
    const HWND hwnd = static_cast<HWND>(app->state.hwnd);  // 657080
    RECT rect;
    GetClientRect(hwnd, &rect);
    if (app->PlaybackActive() != 0)
        return;

    const HWND fg = GetForegroundWindow();
    const HWND cached = app->FloatingWindow();  // 658744 (0xA0D38)

    if (app->state.optflag[0] != 0) {    // 760 (0x2F8)
        // mode row: keep the combo selection in sync with the panel state
        if (fg == cached || app->MouseY() <= rect.bottom - 158) {
            SendMessageA(GetDlgItem(hwnd, panel::kMainComboModel), CB_SETCURSEL,
                         ResolveModelComboSelection(app), 0);
            ApplyModelComboSelection(app);                                       // 0x44D940 (ui_model_reload.cpp)
            return;
        }
    } else if (app->MouseY() > rect.bottom - 158 &&
               fg != cached) {
        SendMessageA(GetDlgItem(hwnd, panel::kMainComboModel), CB_SETCURSEL, 0, 0);
        ApplyModelComboSelection(app);                                           // 0x44D940 (ui_model_reload.cpp)
        return;
    } else if (app->EditMode() != ViewportEditMode::Bone) {
        // panel already active: RefreshBoneModePanel writes the mode buttons
        // (491 scale, 492 move, 493 rotate off, 490 select on) and the merged
        // select / box-select caption from the stored mode.
        app->EditMode() = ViewportEditMode::Bone;
        RefreshBoneModePanel(app);

        const float scale =
            app->Renderer()->viewScale;  // locale-sub + 0x1D4F0
        const std::int32_t hideLeft =
            app->state.hideLeft;      // 658760 (0xA0D48)
        const std::int32_t hideBottom =
            app->state.hideBottom;    // 658764 (0xA0D4C)
        const std::int32_t winW = app->MouseX();
        const std::int32_t winH = app->MouseY();

        // fild/fmul/fsubp/ftol2_sse: truncation of the double expression
        const bool inside =
            winH > static_cast<std::int32_t>(static_cast<double>(hideBottom) -
                                             static_cast<double>(scale) * 80.0) &&
            winW > static_cast<std::int32_t>(static_cast<double>(hideLeft) -
                                             static_cast<double>(scale) * 127.0);
        if ((inside && winW < hideLeft) && winH < hideBottom) {
            POINT pt;
            pt.x = 40;                                            // 0x28
            pt.y = 150;                                           // 0x96
            ClientToScreen(hwnd, &pt);
            SetCursorPos(pt.x, pt.y);
            app->state.separateWindowMouseSeen = 1;      // 651564 (0x9F12C)
            SetForegroundWindow(hwnd);
        }
    } else {
        // panel inactive: all four mode buttons off, cursor parked at the
        // scaled panel corner
        app->EditMode() = ViewportEditMode::None;
        RefreshBoneModePanel(app);

        const float scale =
            app->Renderer()->viewScale;  // locale-sub + 0x1D4F0
        POINT pt;
        pt.x = app->state.hideLeft -
               static_cast<std::int32_t>(static_cast<double>(scale) * 110.0);
        pt.y = app->state.hideBottom -
               static_cast<std::int32_t>(static_cast<double>(scale) * 65.0);
        if (cached != nullptr) {
            ClientToScreen(cached, &pt);
            SetForegroundWindow(cached);
        } else {
            ClientToScreen(hwnd, &pt);
        }
        SetCursorPos(pt.x, pt.y);
        app->state.separateWindowMouseSeen = 1;          // 651564 (0x9F12C)
    }
}

}  // namespace mikumikustudio
