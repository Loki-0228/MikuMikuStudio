// ===========================================================================
// VA 0x0046FF35..0x004739E2 - ConsumeLetterHotkeys  (main-pump letter ladder)
// ===========================================================================
// The main pump (sub_46B090, x64 twin sub_7FF7CB4474F0) polls the keyboard
// at 0x46FF02 (frame_modes.cpp MouseInteractionBegin / model_query_gaps.cpp
// Sub42D3A0) and then walks a consumption ladder that checks each letter's
// pressed-this-frame cell in the runtime LetterHotkeyState (values: 0 idle,
// 1 pressed, 2 released, 3 held; only ==1 is consumed here) and dispatches
// the equivalent of a menu command.  This port runs the ladder from
// FrameDriver right after MouseInteractionBegin.
//
// Common gate, recomputed per block in the original (x86 registers ebx=app,
// var_14C4=GetFocus(), var_14D0="focus not in a panel edit"):
//   focusOK       = foreground belongs to the app, and focus accepts
//                   shortcuts (including panel buttons and cleared focus).
//                   Use the same rule as the keyboard sampling layer.
//   focusInEdit   = focus is one of the panel edits 0x1C1 / 0x1C2 / 0x1DA /
//                   0x1DB / 0x220..0x226 / 0x22A on (floating ? floating :
//                   main) - the controls whose TAB/caret sections clear the
//                   original's flag (x64 0x44E766..0x44ED12)
//   shift / ctrl  = shiftModifierState / ctrlModifierState == 3 (held)
// Block-specific extra gates are noted inline (playbackActive == 0 is the
// "frame advanced" gate app+0x330; FrameStepPlayback() is byte 0x9ED90;
// optflag[0] == 0 selects model/bone mode).
//
// SLOT MAPPING (verified against both binaries): the x86 poll fills
// cell +0x4C (slot 7) with 'g' and +0x50 (slot 8) with 's' (0x42D4CE /
// 0x42D4E5), and the ladder's three-way "seek frame / fine shadow" block
// is gated on slot 7 - i.e. on the G key, exactly as MMD 9.32 documents
// (G: go to frame, Shift+G: self-shadow map, Ctrl+G: the "fine shadow
// mode" notice; S: select unregistered bones, Ctrl+S: save + beep).
// The poll tables (frame_modes.cpp / model_query_gaps.cpp kLetterKeys)
// carry the same binary pairing after the 2026-09 s/g transpose fix.
//
// The non-letter segments that surround the ladder in the pump are ported
// in sibling files and wired from FrameDriver / this ladder: pump_edit_keys.cpp
// carries the panel focus sweep (0x471342..0x471EB5), the ']' (VK 221)
// interpolation toggle 0x4721C9, the DELETE-key model-edit rebuild 0x472246
// (poll cell +0xB8), the TAB/VK226 combo cycling, Alt+Enter fullscreen, the
// Enter register-frame blocks and the ESC handling; pump_navigation.cpp
// carries the right/middle-button drags, the arrow-key camera/light repeat
// section 0x47283F..0x472A49 and the numpad view presets 0x47314B...
// (called below at their binary positions).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/runtime_log.hpp"

namespace mikumikustudio {

// command_view_menu.cpp - the Ctrl+G branch body (x86 0x4726EE, x64
// 0x7FF7CB44F625).  Defined at namespace scope there; declared here to
// avoid touching ported_funcs.hpp.
void FineShadowModeNotice(MMDApp* app);

// ui_frame_step.cpp - frame-apply chain; declared locally like
// command_control_500.cpp does (not in ported_funcs.hpp).
void RefreshAfterFrameApply(MMDApp* app);                 // VA 0x432FA0, was Sub432FA0

// pump_navigation.cpp - arrow-key navigation and numpad presets, called
// from the ladder at their binary positions (see the call sites below).
void ConsumeArrowKeyNavigation(MMDApp* app);              // VA 0x47283F
void ConsumeNumpadViewPresets(MMDApp* app);               // VA 0x47314B

namespace {

// Port dialogFlags slots as written by frame_modes.cpp kLetterKeys.
enum LetterSlot {
    kSlotX = 0, kSlotZ = 1, kSlotC = 2, kSlotV = 3,
    kSlotD = 4,  kSlotA = 5,  kSlotB = 6,
    kSlotG = 7,  // receives 'g'/'G' (binary pairing, see header note)
    kSlotS = 8,  // receives 's'/'S'
    kSlotI = 9,  kSlotH = 10, kSlotK = 11, kSlotP = 12, kSlotU = 13,
    kSlotJ = 14, kSlotF = 15, kSlotR = 16, kSlotL = 17,
};

// The 0x22A read and the edit-focus probe run on the floating viewport
// window when one exists (x86 0x472731 / 0x471E67).
HWND LadderOwner(MMDApp* app) {
    HWND owner = app->FloatingWindow();
    return owner != nullptr ? owner : static_cast<HWND>(app->Hwnd());
}

// The panel edits whose focus suppresses the ladder (see header).
bool FocusInPanelEdit(MMDApp* app, HWND focus) {
    const HWND owner = LadderOwner(app);
    static const int kEdits[] = {
        0x1C1, 0x1C2, 0x1DA, 0x1DB,
        0x220, 0x221, 0x222, 0x223, 0x224, 0x225, 0x226, 0x22A,
    };
    for (int id : kEdits) {
        if (focus != nullptr && GetDlgItem(owner, id) == focus)
            return true;
    }
    return false;
}

// The ladder re-dispatches menu command ids through the main window
// (SendMessageA(main, WM_COMMAND, id, 0) at e.g. 0x471F84).
void SendMenuCommand(MMDApp* app, int id) {
    runtime_log::Write("COMMAND_SOURCE source=letter-hotkey id=%d", id);
    SendMessageA(static_cast<HWND>(app->Hwnd()), WM_COMMAND, id, 0);
}

}  // namespace

// Dispatch an eligible input snapshot. The pump checks foreground ownership
// below; keeping focus explicit also lets tests exercise the real I/K/F routes
// without activating a window over the user's current application.
void DispatchLetterHotkeys(MMDApp* app, HWND focus) {
    auto& state = app->state;
    const HWND main = static_cast<HWND>(app->Hwnd());
    const bool focusOK = FocusAllowsLetterHotkeys(main, app->FloatingWindow(), focus);
    const bool focusInEdit = FocusInPanelEdit(app, focus);
    const bool shift = state.shiftModifierState == 3;      // +0x24
    const bool ctrl = state.ctrlModifierState == 3;        // +0xC0
    const bool playing = app->PlaybackActive() != 0;       // +0x330
    const bool modelMode = state.optflag[0] == 0 &&
        app->SelectedModelSlot() < kModelSlotCount && app->SelectedModel() != nullptr;
    const auto pressed = [app](int slot) {
        return app->LetterHotkeys().Pressed(slot);
    };

    // ---- 'B' (0x471EB5): toggle background color black/white ------------
    // Body = command case 282 minus its dialogFlags[4] write: flip
    // app+0xA0194 and mirror menu item 0x11A.
    if (focusOK && !focusInEdit && pressed(kSlotB)) {
        HMENU menu = GetMenu(main);
        if (state.blackBackgroundEnabled != 0) {
            state.blackBackgroundEnabled = 0;
            CheckMenuItem(menu, 0x11A, MF_UNCHECKED);
        } else {
            state.blackBackgroundEnabled = 1;
            CheckMenuItem(menu, 0x11A, MF_CHECKED);
        }
    }

    // ---- 'V' (0x471F37): transparent models / outline / paste ------------
    if (focusOK && !focusInEdit && pressed(kSlotV)) {
        if (ctrl && !playing) {
            SendMenuCommand(app, 0x1A5);                   // case 421 paste
        } else if (shift) {
            // Body = command case 283 minus dialogFlags[5]: flip the
            // outline-suppression byte and menu 0x11B.
            HMENU menu = GetMenu(main);
            if (state.modelNonDisplayMode != 0) {
                state.modelNonDisplayMode = 0;
                CheckMenuItem(menu, 0x11B, MF_UNCHECKED);
            } else {
                state.modelNonDisplayMode = 1;
                CheckMenuItem(menu, 0x11B, MF_CHECKED);
            }
        } else {
            // Body = command case 0xD6 minus the command bookkeeping: flip
            // app+0x9EB7E, mirror menu 0xD6 and push the flag into every
            // loaded model's displayState (the pump walks the whole slot
            // array, kModelSlotCount wide in both layouts).
            HMENU menu = GetMenu(main);
            state.characterTransparentMode = state.characterTransparentMode != 0 ? 0 : 1;
            CheckMenuItem(menu, 0xD6,
                          state.characterTransparentMode != 0 ? MF_CHECKED : MF_UNCHECKED);
            for (int slot = 0; slot < kModelSlotCount; ++slot) {
                unsigned char* model = app->ModelSlot(slot);
                if (model != nullptr)
                    mdl::Mdl(model)->displayState = state.characterTransparentMode;
            }
        }
    }

    // ---- 'C' (0x4720B7): copy frames / bone-select toggle ----------------
    if (focusOK && !playing && !focusInEdit && pressed(kSlotC)) {
        if (ctrl) {
            SendMenuCommand(app, 0x1A4);                   // case 420 copy
        } else if (modelMode) {
            // MMS: control 490 is the merged select / box-select cycle button,
            // so its check state no longer separates "select" from "box
            // select".  Key the toggle on the stored mode, which keeps the
            // original meaning: select <-> none, and from box select back to
            // plain select.
            app->EditMode() = app->EditMode() == ViewportEditMode::Bone
                                  ? ViewportEditMode::None
                                  : ViewportEditMode::Bone;
            RefreshBoneModePanel(app);
        }
    }

    // ---- 'H' (0x472324): register all facials (clear all frames, 0xE5) ---
    if (focusOK && modelMode && !playing && !focusInEdit &&
        pressed(kSlotH)) {
        SendMenuCommand(app, 0xE5);                        // case 229
    }

    // ---- 'I' (0x472375): insert bone/camera frame line -------------------
    if (focusOK && !playing && !focusInEdit && pressed(kSlotI))
        InsertBoneCameraFrameLine(app);                                    // 0x439E40

    // ---- 'K' (0x4723AB): delete bone/camera frame line -------------------
    if (focusOK && !playing && !focusInEdit && pressed(kSlotK))
        DeleteBoneCameraFrameLine(app);                                    // 0x43A650

    // ---- 'U' (0x4723E1): insert facial/light frame line ------------------
    if (focusOK && !playing && !focusInEdit && pressed(kSlotU))
        InsertFacialLightFrameLine(app);                                    // 0x43B720

    // ---- 'J' (0x472417): delete facial/light frame line ------------------
    if (focusOK && !playing && !focusInEdit && pressed(kSlotJ))
        DeleteFacialLightFrameLine(app);                                    // 0x43BB30

    // ---- 'G' (0x4726B2): seek frame / self-shadow / fine-shadow notice ----
    // Matches the original three-way block, gated on the 'g'/'G' cell.
    // Plain G reads the go-to-frame edit 0x22A back and seeks to that frame,
    // then echoes the result into 0x1A1 (the same body as command case 553);
    // Shift+G toggles the shadow render-mode byte app+0xA1DC8
    // (selfShadowCompositionEnabled);
    // Ctrl+G raises the "fine shadow mode" notice (x86 0x4726EE, x64 0x7FF7CB44F625).
    // The port previously cycled the transform axis here,
    // duplicating the 'L' block below and leaving FineShadowModeNotice without a caller.
    // Defect AB-02.
    if (focusOK && !focusInEdit && pressed(kSlotG)) {
        if (ctrl) {
            FineShadowModeNotice(app);
        } else if (shift) {
            state.selfShadowCompositionEnabled =
                state.selfShadowCompositionEnabled != 0 ? 0 : 1;
        } else {
            char buf[0x100];
            GetWindowTextA(GetDlgItem(main, panel::kGotoFrameEdit), buf, 0xA);
            long v = atol(buf);
            if (v < 0)
                v = 0;
            state.currentFrame = static_cast<std::int32_t>(v);
            RefreshAfterFrameApply(app);                   // 0x432FA0
            PostViewRefresh(app);
            sprintf_s(buf, 0x100u, "%d", state.currentFrame);
            SetWindowTextA(GetDlgItem(main, panel::kCurrentFrameEdit), buf);
        }
    }
    // ---- 'L' (0x472804): cycle the transform-channel selector ------------
    // ++app+0x9ED9C; wraps modulo 3 in camera/accessory mode, modulo 2 in
    // model mode (consumed by the mouse-wheel channel chain,
    // ModeCameraAdjust / frame_modes.cpp coordinateSystem).
    // The port wrapped at 3 unconditionally, so model mode could reach a third
    // channel the original never selects there. The original wraps the
    // counter at 2 when the model-mode byte is zero and at 3 otherwise
    // (x64 0x14002F765 increments app+0x9FCA4, tests app+0x328, then
    // compares against 2 or 3 - the same byte gates the C/H/X/Z blocks).
    // Defect AB-03.
    if (focusOK && !focusInEdit && pressed(kSlotL)) {
        ++state.coordinateSystem;
        // Model mode wraps at 2 (matching the original's model-mode byte gate). Outside
        // it, the third state is accessory editing, which requires an existing accessory;
        // CoordinateStateLimit reports 2 while the list is empty.
        const int limit = modelMode ? 2 : CoordinateStateLimit(app);
        if (state.coordinateSystem >= limit)
            state.coordinateSystem = 0;
    }

    // x86 0x47283F..0x472A49: the arrow-key camera/light navigation (with
    // hold auto-repeat) sits here in the pump, between 'L' and Ctrl+'S'.
    ConsumeArrowKeyNavigation(app);                 // pump_navigation.cpp

    // ---- Ctrl+'S' (0x472A65): save + bell (no edit-focus gate) -----------
    if (focusOK && pressed(kSlotS) && ctrl) {
        SendMenuCommand(app, 0xCF);                        // case 0xCF save
        MessageBeep(0x40);
    }

    // ---- 'P' (0x472A95): play/stop via the play button 0x198 -------------
    // Button 408 is a push-look auto checkbox (0x50001003 = BS_AUTOCHECKBOX |
    // BS_PUSHLIKE, ui_controls.inc), its check mark mirrors the playback flag
    // (+0x330), and command 408 itself toggles: the stop branch runs when
    // PlaybackActive() != 0.  The gate here therefore must NOT test !playing:
    // with that test the key went dead the moment playback started, so a
    // second P could not stop it.  P now acts exactly like a click on the
    // button - start when stopped, stop while playing.
    //
    // The key itself is sampled IME independently (keyboard_input.cpp): with
    // a Chinese/Japanese input method switched on, GetKeyboardState alone
    // never reported the press, which is the other half of "P cannot stop
    // playback" (the user had to switch the IME to English first).
    if (focusOK && app->FrameStepPlayback() == 0 && !focusInEdit &&
        pressed(kSlotP)) {
        const bool starting = app->PlaybackActive() == 0;
        SendMessageA(GetDlgItem(main, panel::kPlayButton), BM_SETCHECK,
                     starting ? 1 : 0, 0);
        SendMenuCommand(app, 0x198);                 // case 408 play/stop
        SetFocus(main);
    }

    // ---- 'F' (0x472B2A): paste keyframes to another bone (0xFA) ----------
    if (focusOK && app->FrameStepPlayback() == 0 && !focusInEdit &&
        pressed(kSlotF)) {
        SendMenuCommand(app, 0xFA);                        // case 250
    }

    // ---- 'X' (0x472D5C): redo / camera mode ------------------------------
    if (focusOK && modelMode && !playing && !focusInEdit && pressed(kSlotX)) {
        unsigned char* model = app->SelectedModel();
        if (ctrl && model != nullptr &&
            mdl::Mdl(model)->redoDirty != 0) {
            // Redo one ring entry (0x4A2490), then enable the undo button
            // / disable redo exactly like the pump tail, and re-seek the
            // frame when the new ring head is not a pose-only record.
            // The original passes &app+0x980 so the redo can move the
            // current frame itself.
            RedoModelEdit(model, &app->CurrentFrame(), app->PlaybackPhysicsMode());
            auto* record = mdl::Mdl(model);
            if (record->undoState[0] == record->undoState[1]) {
                EnableWindow(GetDlgItem(main, panel::kRedoButton), FALSE);
                record->redoDirty = 0;
            }
            EnableWindow(GetDlgItem(main, panel::kUndoButton), TRUE);
            record->undoDirty = 1;
            if (record->undoRings[0].slots[record->undoState[0]].operation
                    != 1) {
                SeekModelFrame(model, static_cast<int>(app->CurrentFrame()),
                          app->PlaybackPhysicsMode());
            }
            PanelPaint(app);                               // 0x414610
            SelectionReeval(app);                          // 0x430510
            app->SceneModified() = 1;
            app->PhysicsResetPending() = 1;
        } else if (!ctrl) {
            // MMS: RefreshBoneModePanel writes all four mode buttons and the
            // merged select / box-select caption from the stored mode.
            app->EditMode() = ViewportEditMode::Camera;    // 0x914 = 3
            RefreshBoneModePanel(app);
        }
    }

    // ---- 'Z' (0x472ED7): undo / light mode -------------------------------
    if (focusOK && modelMode && !playing && !focusInEdit && pressed(kSlotZ)) {
        unsigned char* model = app->SelectedModel();
        if (ctrl && model != nullptr &&
            mdl::Mdl(model)->undoDirty != 0) {
            // Undo one ring entry (0x4A1870), then the mirror-image button
            // tail of the X branch.  Like the redo, the frame pointer aims
            // straight at app+0x980.
            UndoModelEdit(model, &app->CurrentFrame(), app->PlaybackPhysicsMode());
            auto* record = mdl::Mdl(model);
            if (record->undoRings[0].slots[record->undoState[0]].operation
                    == 0) {
                EnableWindow(GetDlgItem(main, panel::kUndoButton), FALSE);
                record->undoDirty = 0;
            }
            if (record->undoState[0] == record->undoState[1]) {
                EnableWindow(GetDlgItem(main, panel::kUndoButton), FALSE);
                record->undoDirty = 0;
            }
            EnableWindow(GetDlgItem(main, panel::kRedoButton), TRUE);
            record->redoDirty = 1;
            PanelPaint(app);                               // 0x414610
            SelectionReeval(app);                          // 0x430510
            app->SceneModified() = 1;
            app->PhysicsResetPending() = 1;
        } else if (!ctrl) {
            app->EditMode() = ViewportEditMode::Light;     // 0x914 = 4
            RefreshBoneModePanel(app);
        }
    }

    // ---- 'A' (0x473092): select all bones (0x1EE) ------------------------
    // A model and foreground, non-edit focus are required for bone commands.
    if (focusOK && modelMode && !playing && !focusInEdit && pressed(kSlotA))
        SendMenuCommand(app, 0x1EE);                       // case 494

    // ---- plain 'S' (0x4730B1): select unregistered bones (0x1F5) ---------
    if (focusOK && modelMode && !playing && !focusInEdit && pressed(kSlotS) && !ctrl)
        SendMenuCommand(app, 0x1F5);                       // case 501

    // ---- 'D' (0x4730D9): model-offset dialog (0xDB) ----------------------
    if (focusOK && !focusInEdit && pressed(kSlotD))
        SendMenuCommand(app, 0xDB);                        // case 219

    // x86 0x47314B..0x4739E2: the numpad view presets run after the ESC
    // stop block (pump_edit_keys.cpp) and before 'R', per the pump order.
    ConsumeNumpadViewPresets(app);                  // pump_navigation.cpp

    // ---- 'R' (0x473989): frame control dialog (0xFB) ---------------------
    if (focusOK && modelMode && app->FrameStepPlayback() == 0 &&
        !focusInEdit && pressed(kSlotR)) {
        SendMenuCommand(app, 0xFB);                        // case 251
    }
}

void ConsumeLetterHotkeys(MMDApp* app) {
    if (!LetterHotkeyInputAllowed(app)) return;
    DispatchLetterHotkeys(app, GetFocus());
}

}  // namespace mikumikustudio
