#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
// ===========================================================================
// VA 0x00471342..0x00473127 - ConsumeEditKeys  (main-pump edit-key family)
// ===========================================================================
// The non-letter half of the main pump's per-frame keyboard consumption
// (sub_46B090, x64 twin sub_7FF7CB4474F0).  The letter half is already
// ported as ConsumeLetterHotkeys (key_ladder.cpp); this file ports the
// eight edit-key segments that surround it in the original pump, in
// original execution order:
//
//   G12 x86 0x471342..0x471EB5 | x64 0x44DEF6..0x44ED16
//       Panel focus chain: TAB / Shift+TAB walks focus between the panel
//       edits (select-all + optional MessageBeep on arrival), the arrow
//       keys park focus on the panel root, and the whole walk computes
//       the pump-wide "focus not in a panel edit" flag (r13 at 0x44ED2E)
//       that gates the segments below.  This port is a SUPERSET of
//       key_ladder.cpp's FocusInPanelEdit probe list - the original chain
//       also clears the flag for 0x198, 0x1A1, 0x1A8..0x1AA, 0x1CD..0x1D2,
//       0x1DE..0x1E5, 0x1FA, 0x1FF, 0x204, 0x209 and 0x231.
//   G6  x86 0x4721C9          | x64 0x44F06A..0x44F106
//       ']' (VK 221) flips the interpolation-reset checkbox 0x212 (530).
//   G8  x86 0x472246..0x472313 | x64 0x44F106..0x44F1E8
//       DELETE rebuilds the model-edit state (DeleteMarkedKeyframes) unless focus is
//       inside one of the five frame edits.
//   G5  x86 0x47244E..0x472524 (TAB) / 0x47252B..0x472609 (VK 226)
//                           | x64 0x44F32D..0x44F42E / 0x44F42E..0x44F52F
//       TAB and the Japanese henkan key cycle the model/accessory combo
//       0x1B4 (436), wrapping at both ends, then apply it (ApplyModelComboSelection).
//   G7  x86 0x472633..0x472698 | x64 0x44F52F..0x44F594 / 0x44F594..0x44F5D4
//       Alt+Enter toggles fullscreen (flip 0xA0274 + ApplyFullscreenWindowState +
//       PostDeviceReset); ESC leaves it (same two calls).
//   G2  x86 0x472B76..0x472D1E | x64 0x44FB0A..0x44FCDB
//       Enter in camera/accessory mode: dirty 0xA0B0D, clear the selected
//       marks on the four global key tables and every accessory track,
//       re-register each ticked global track and every active accessory,
//       then PanelPaint + SelectionReeval.
//   G3  x86 0x473067..0x473092 | x64 0x4500DF..0x450111
//       Enter otherwise re-dispatches the register-frame button command
//       0x1F4 (case 500) on the main window - NOTE the original runs G2
//       AND G3 back to back in camera mode (case 500 has no mode gate of
//       its own; it registers the selected model's pose).
//   G4  x86 0x4730F8..0x473127 | x64 0x450186..0x4501B5
//       ESC while a frame-step recording is running ends it (FinishAviRecord).
//
// The x86 and x64 compilers schedule these blocks differently inside the
// pump (x86 interleaves G5/G7 between the letter blocks, x64 groups them
// before G2); the relative order inside this family is preserved.
//
// Gate inputs, verified on both binaries (poll cells x64 = x86 + 4):
//   RETURN cell x86 +0xBC (MMDAppState::enterKeyState) / x64 +0xC0 - the same cell
//   the ~20 command handlers poke with 1 to fake an Enter; consuming it
//   here closes that chain.
//   MENU(Alt) +0xC4/+0xC8, ESC +0x2C/+0x30, TAB +0x80/+0x84, VK221
//   +0x78/+0x7C, VK226 +0x7C/+0x80, DELETE +0xB8/+0xBC, SHIFT +0x24/+0x28.
//   viewportActive 0x9EDD1/0x9FCDD and frameStep 0x9ED90/0x9FC98 are
//   maintained by MouseInteractionBegin (frame_modes.cpp); "playing" is
//   dword +0x330 in x86 and byte +0x368 in x64 (PlaybackActive);
//   optflag[0] byte +0x2F8/+0x328; the modal gate is 0xA0B50/0xA1B98
//   (FrameRangeDialog); fullscreen byte 0xA0274/0xA11E4; the main window
//   HWND 0xA06B8/0xA16C8; the floating viewport 0xA0D38/0xA1DE0.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "device_lost_wait.hpp"

namespace mikumikustudio {

// Global-track register backends, defined with these signatures in
// command_control_400.cpp / ui_frame_refresh.cpp / command_view_menu.cpp
// (not yet registered in ported_funcs.hpp; declared locally like
// key_ladder.cpp does for its cross-TU callees).
void RegisterCameraState(MMDApp* app, int frame);    // VA 0x00410560, was Sub410560
void RegisterLightState(MMDApp* app, int frame);     // VA 0x00411630, was Sub411630
void RegisterSelfShadowState(MMDApp* app, int frame);  // VA 0x00411DF0, was Sub411DF0
                                                     // (ui_frame_refresh.cpp)
void RegisterGravityKeyCurrent(MMDApp* app, std::int32_t frame);  // VA 0x00412B20, was Sub412B20

namespace {

// The 0x220..0x226 / 0x22A probes run against the floating viewport
// window when one exists (x64 0x44EB00..0x44EB23 / 0x44ECD7).
HWND PanelEditOwner(MMDApp* app) {
    HWND owner = app->FloatingWindow();
    return owner != nullptr ? owner : static_cast<HWND>(app->Hwnd());
}

// Common focus-OK gate recomputed by every segment exactly as the pump
// does (focus == main window || viewport input active byte).
bool FocusOk(MMDApp* app, HWND focus) {
    return focus == static_cast<HWND>(app->Hwnd()) ||
           app->ViewportInputActive() != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// G12 - panel focus chain (x86 0x471342..0x471EB5, x64 0x44DEF6..0x44ED16).
// `focus` is the GetFocus() snapshot the pump caches before any block runs
// (x64 0x44DEF8).  Returns true when the focus ended up being inside one
// of the panel edits, i.e. the inverse of the r13 flag the segments below
// receive as focusNotInPanelEdit.
// ---------------------------------------------------------------------------
bool PumpPanelFocusChain(MMDApp* app, HWND focus) {
    auto& state = app->state;
    const HWND main = static_cast<HWND>(app->Hwnd());
    const HWND owner = PanelEditOwner(app);
    const bool tab = state.tabKeyState == 1;             // +0x80 cell
    const bool shift = state.shiftModifierState == 3;    // +0x24 cell
    const bool cameraMode = state.optflag[0] != 0;       // +0x2F8 byte
    bool notInEdit = true;                               // bl at 0x44DEF6

    const auto focusIs = [&](HWND on, int id) {
        return GetDlgItem(on, id) == focus;
    };
    // SetFocus + optional select-all + optional beep; the original
    // re-fetches GetDlgItem before every call, kept.
    const auto moveTo = [&](HWND on, int id, bool selectAll, bool beep) {
        SetFocus(GetDlgItem(on, id));
        if (selectAll) {
            const LRESULT len =
                GetWindowTextLengthA(GetDlgItem(on, id));
            SendMessageA(GetDlgItem(on, id), EM_SETSEL, 0, len);
        }
        if (beep)
            MessageBeep(0);
    };
    // The 0x1DA / 0x1C1 / 0x1DB / 0x1C2 rows park focus on the panel root
    // control (id 0) when any arrow key fires (order: LEFT, RIGHT, UP,
    // DOWN - independent ifs, several can fire in one frame).
    const auto arrowsToPanelRoot = [&]() {
        // The panel root control has no id in this port (every main-panel
        // control starts at 400), so GetDlgItem(main, 0) is NULL and
        // SetFocus(NULL) cleared the thread focus - which killed the letter
        // ladder and the Delete gate in one go.  Fall back to the main window.
        const HWND root = GetDlgItem(main, 0);
        const HWND target = root != nullptr ? root : main;
        if (state.leftKeyState == 1)
            SetFocus(target);
        if (state.rightKeyState == 1)
            SetFocus(target);
        if (state.upKeyState == 1)
            SetFocus(target);
        if (state.downKeyState == 1)
            SetFocus(target);
    };

    // ---- 0x44DF19: frame-scale edits 0x199 <-> 0x19A ----------------------
    if (focusIs(main, 0x199)) {
        if (tab)
            moveTo(main, 0x19A, true, false);
        notInEdit = false;
    }
    if (focusIs(main, 0x19A)) {
        if (tab)
            moveTo(main, 0x199, true, false);
        notInEdit = false;
    }
    // ---- 0x44E00F: play-button row edit 0x198 -> 0x199 (with beep) --------
    if (focusIs(main, 0x198)) {
        if (tab)
            moveTo(main, 0x199, true, true);
        notInEdit = false;
    }
    // ---- 0x44E0B1: frame edit 0x1A1 is gated but has no TAB move ----------
    if (focusIs(main, 0x1A1))
        notInEdit = false;
    // ---- 0x44E0C8 / 0x44E142: 0x1A9 <-> 0x1AA -----------------------------
    if (focusIs(main, 0x1A9)) {
        if (tab)
            moveTo(main, 0x1AA, true, false);
        notInEdit = false;
    }
    if (focusIs(main, 0x1AA)) {
        if (tab)
            moveTo(main, 0x1A9, true, false);
        notInEdit = false;
    }
    // ---- 0x44E1C9: 0x19F -> 0x1A9 (camera mode, select-all) or 0x1A8 ------
    if (focusIs(main, 0x19F)) {
        if (tab) {
            if (cameraMode)
                moveTo(main, 0x1A9, true, true);
            else
                moveTo(main, 0x1A8, false, true);
        }
        notInEdit = false;
    }
    // ---- 0x44E281: 0x1A8 -> 0x1A9 ----------------------------------------
    if (focusIs(main, 0x1A8)) {
        if (tab)
            moveTo(main, 0x1A9, true, false);
        notInEdit = false;
    }

    // ---- 0x44E326 loop: 0x1CD..0x1D2, wrapping 0x1D2 <-> 0x1CD -----------
    for (int id = 0x1CD; id <= 0x1D2; ++id) {
        if (!focusIs(main, id))
            continue;
        if (tab) {
            const int target = shift
                ? (id == 0x1CD ? 0x1D2 : id - 1)
                : (id == 0x1D2 ? 0x1CD : id + 1);
            moveTo(main, target, true, false);
        }
        notInEdit = false;
    }
    // ---- 0x44E4A4 loop: 0x1DE..0x1E5, wrapping 0x1E5 <-> 0x1DE -----------
    for (int id = 0x1DE; id <= 0x1E5; ++id) {
        if (!focusIs(main, id))
            continue;
        if (tab) {
            const int target = shift
                ? (id == 0x1DE ? 0x1E5 : id - 1)
                : (id == 0x1E5 ? 0x1DE : id + 1);
            moveTo(main, target, true, false);
        }
        notInEdit = false;
    }

    // ---- 0x44E5D0..0x44E65E: probes without TAB moves (bl only) ----------
    if (focusIs(main, 0x1FA) || focusIs(main, 0x1FF) ||
        focusIs(main, 0x204) || focusIs(main, 0x209) ||
        focusIs(main, 0x231))
        notInEdit = false;

    // ---- 0x44E659: 0x1E5 - forward TAB commits the edit -------------------
    if (focusIs(main, 0x1E5)) {
        if (tab) {
            if (shift) {
                moveTo(main, 0x1E4, true, false);
            } else {
                moveTo(main, 0x1DE, true, false);
                CommitEditControl(app, GetDlgItem(main, panel::kAccScaleYEdit));     // 0x463640
            }
        }
        notInEdit = false;
    }

    // ---- 0x44E766 / 0x44E837 / 0x44E908 / 0x44EA1F: camera rows ----------
    // 0x1DA -> 0x1DB (no select-all), 0x1C1 -> 0x1C2 (no select-all),
    // 0x1DB -> 0x1DE (select-all), 0x1C2 -> 0x1C1 (no select-all); each
    // beeps and hands the arrow keys to the panel root.
    if (focusIs(main, 0x1DA)) {
        if (tab)
            moveTo(main, 0x1DB, false, true);
        arrowsToPanelRoot();
        notInEdit = false;
    }
    if (focusIs(main, 0x1C1)) {
        if (tab)
            moveTo(main, 0x1C2, false, true);
        arrowsToPanelRoot();
        notInEdit = false;
    }
    if (focusIs(main, 0x1DB)) {
        if (tab)
            moveTo(main, 0x1DE, true, true);
        arrowsToPanelRoot();
        notInEdit = false;
    }
    if (focusIs(main, 0x1C2)) {
        if (tab)
            moveTo(main, 0x1C1, false, true);
        arrowsToPanelRoot();
        notInEdit = false;
    }

    // ---- 0x44EAF0 loop: 0x220..0x226 on the panel owner ------------------
    // The chain end depends on the mode: 0x226 in camera/accessory mode,
    // 0x225 in model mode (the extra edit only exists there).
    const int chainEnd = cameraMode ? 0x226 : 0x225;
    for (int id = 0x220; id <= 0x226; ++id) {
        if (!focusIs(owner, id))
            continue;
        if (tab) {
            const int target = shift
                ? (id == 0x220 ? chainEnd : id - 1)
                : (id == chainEnd ? 0x220 : id + 1);
            moveTo(owner, target, true, false);
        }
        notInEdit = false;
    }
    // ---- 0x44ECD7: frame edit 0x22A on the panel owner -------------------
    if (focusIs(owner, 0x22A))
        notInEdit = false;

    return !notInEdit;
}

// ---------------------------------------------------------------------------
// G6 - ']' interpolation toggle (x86 0x4721C9, x64 0x44F06A..0x44F106).
// Gate: focusOK && VK221 pressed && !playing && !panel-edit focus.
// Body: flip checkbox 0x212 (530, the register-reset checkbox probed by
// RegisterCameraState).
// ---------------------------------------------------------------------------
void PumpInterpolationToggle(MMDApp* app, HWND focus,
                             bool focusNotInPanelEdit) {
    if (!(FocusOk(app, focus) && app->state.keyState221 == 1 &&
          app->PlaybackActive() == 0 && focusNotInPanelEdit))
        return;
    const HWND main = static_cast<HWND>(app->Hwnd());
    const HWND box = GetDlgItem(main, panel::kPhysicsFrameCheckbox);
    const LRESULT checked = SendMessageA(box, BM_GETCHECK, 0, 0);
    SendMessageA(box, BM_SETCHECK, checked == 1 ? 0 : 1, 0);
}

// ---------------------------------------------------------------------------
// G8 - DELETE model-edit rebuild (x86 0x472246..0x472313,
// x64 0x44F106..0x44F1E8).
// Gate: focus == main && DELETE pressed && !playing && !panel-edit focus.
// Body: a FRESH GetFocus (the chain above may have moved it after the
// gate's snapshot was taken) must not sit in any of the five frame edits
// {0x1AA, 0x1A9, 0x1A1, 0x19A, 0x199}, then DeleteMarkedKeyframes rebuilds the
// model-edit state.
// ---------------------------------------------------------------------------
// The gate itself is a pure function so the rule is assertable without a
// window (tests/panel_key_tests.cpp).  `focus == main` alone was the reported
// failure: after the user clicks ANY panel control (the bone panel's
// 全選択/初期化/登録 buttons, the radio rows, a combo, the slider ...) the focus
// is that control, not the main window, so the Delete key went dead exactly in
// the "select all bones, then delete" workflow.  The window half is therefore
// now the same rule the letter ladder uses - the main/floating window, any
// window inside them, or a cleared focus (FocusAllowsLetterHotkeys) - while a
// focused text field and the five frame edits keep the key (text editing).
bool DeleteShortcutAllowed(const MMDApp* app, bool focusAcceptsKeys,
                           bool focusInFrameEdit) noexcept {
    if (app == nullptr) {
        return false;
    }
    if (!focusAcceptsKeys || focusInFrameEdit) {
        return false;
    }
    if (app->state.deleteKeyState != 1) {          // +0xB8/x64 +0xBC cell
        return false;
    }
    if (app->PlaybackActive() != 0) {              // +0x330
        return false;
    }
    return true;
}

void PumpDeleteRebuild(MMDApp* app, HWND focus, bool focusNotInPanelEdit) {
    const HWND main = static_cast<HWND>(app->Hwnd());
    const HWND current = GetFocus();                      // 0x44F13F
    if (curve_editor::OwnsWindow(main, focus) || curve_editor::OwnsWindow(main, current)) return;
    static const int kFrameEdits[] = {0x1AA, 0x1A9, 0x1A1, 0x19A, 0x199};
    bool inFrameEdit = false;
    for (int id : kFrameEdits) {
        if (GetDlgItem(main, id) == current) {
            inFrameEdit = true;
            break;
        }
    }
    const bool focusAcceptsKeys = FocusAllowsLetterHotkeys(
        main, static_cast<HWND>(app->FloatingWindow()), current);
    // The panel-edit sweep still owns its half of the rule.
    if (!focusNotInPanelEdit) {
        return;
    }
    if (!DeleteShortcutAllowed(app, focusAcceptsKeys, inFrameEdit)) {
        return;
    }
    // Explicit timeline marks define the deletion. Only fall back to the
    // viewport's selected bones when no timeline keys are selected.
    if (unsigned char* model = app->SelectedModel(); model && app->CameraMode() == 0) {
        const auto marked = [](const auto* keys, std::size_t capacity) {
            for (std::size_t i = 0; keys && i < capacity; ++i)
                if (keys[i].allocated != 0) return true;
            return false;
        };
        const auto& record = *mdl::Mdl(model);
        if (!marked(record.boneKeys, mdl::BoneKeyCapacity(record)) &&
            !marked(record.morphKeys, mdl::MorphKeyCapacity(record.morphKeys)) &&
            !marked(record.displayKeys, mdl::KeyPoolCapacity(record.displayKeys))) {
            MarkSelectedBoneKeysAtFrame(model,
                                        static_cast<int>(app->CurrentFrame()));
        }
    }
    if(negative_frames::Command(app,423))return;
    DeleteMarkedKeyframes(app);                                       // 0x4316B0
}

// ---------------------------------------------------------------------------
// G5 - TAB / VK226 combo cycle (x86 0x47244E..0x472524 and
// 0x47252B..0x472609, x64 0x44F32D..0x44F42E and 0x44F42E..0x44F52F).
// Gate per half: focusOK && cell pressed && !playing && !panel-edit focus
// && frame-range dialog closed (0xA0B50).  Body: step the model combo
// 0x1B4 (436) selection - Shift steps back with a wrap to count-1,
// plain steps forward with a wrap to 0 - then CB_SETCURSEL and apply via
// ApplyModelComboSelection (ui_model_reload.cpp).
// ---------------------------------------------------------------------------
void PumpTabCycle(MMDApp* app, HWND focus, bool focusNotInPanelEdit) {
    const bool gate = FocusOk(app, focus) &&
                      app->PlaybackActive() == 0 &&
                      focusNotInPanelEdit &&
                      app->FrameRangeDialog() == nullptr;
    const auto cycle = [&](bool cellPressed) {
        if (!gate || !cellPressed)
            return;
        const HWND main = static_cast<HWND>(app->Hwnd());
        const HWND combo = GetDlgItem(main, panel::kMainComboModel);
        int index;
        if (app->state.shiftModifierState == 3) {
            index = static_cast<int>(
                         SendMessageA(combo, CB_GETCURSEL, 0, 0)) - 1;
            if (index < 0)
                index = static_cast<int>(
                            SendMessageA(combo, CB_GETCOUNT, 0, 0)) - 1;
        } else {
            index = static_cast<int>(
                         SendMessageA(combo, CB_GETCURSEL, 0, 0)) + 1;
            if (index >= static_cast<int>(
                             SendMessageA(combo, CB_GETCOUNT, 0, 0)))
                index = 0;
        }
        SendMessageA(combo, CB_SETCURSEL, index, 0);
        ApplyModelComboSelection(app);                                   // 0x44D940
    };
    cycle(app->state.tabKeyState == 1);                   // x64 0x44F32D
    cycle(app->state.keyState226 == 1);                   // x64 0x44F42E
}

// ---------------------------------------------------------------------------
// G7 - Legacy ESC polling fallback. Alt+Enter is handled by WM_SYSKEYDOWN
// in MainWndProc so short presses and focus in child controls are reliable.
// ---------------------------------------------------------------------------
void PumpFullscreenKeys(MMDApp* app, HWND /*focus*/,
                        bool focusNotInPanelEdit) {
    auto& state = app->state;
    // Alt+Enter is consumed by WM_SYSKEYDOWN; polling it too can toggle twice.
    if (app->FullscreenMode() != 0 &&
        app->ViewportInputActive() != 0 &&
        state.escKeyState == 1 &&
        focusNotInPanelEdit) {
        app->FullscreenMode() = 0;                         // 0xA0274
        ApplyFullscreenWindowState(app);                                    // 0x4629D0
        PostDeviceReset(app);                              // 0x440DB0
    }
}

// ---------------------------------------------------------------------------
// G2 + G3 - Enter register frame (x86 0x472B76..0x472D1E and
// 0x473067..0x473092, x64 0x44FB0A..0x44FCDB and 0x4500DF..0x450111).
// G2 (camera/accessory mode, optflag[0] != 0): dirty 0xA0B0D, clear the
// selected marks on all 10000 records of the camera (+0x48, stride 0x54),
// light (+0x24 / 0x28), self-shadow (+0x14 / 0x18) and gravity (+0x21 /
// 0x24) tables and on all 255 accessory tracks (flag +0x18, stride 0x3C,
// 10000 records), re-register each ticked track byte 0xA03E4..0xA03E7 and
// every accessory whose object active flag (+0x4AC) is set, then
// PanelPaint + SelectionReeval.  G3 then re-dispatches the register
// command 0x1F4 (case 500) unconditionally on the next Enter - the
// original fires both in camera mode.
// ---------------------------------------------------------------------------
void PumpEnterRegisterFrame(MMDApp* app, HWND focus,
                            bool focusNotInPanelEdit) {
    auto& state = app->state;
    const HWND main = static_cast<HWND>(app->Hwnd());
    if (curve_editor::OwnsWindow(main, focus)) return;
    const bool enterPressed = state.enterKeyState == 1;               // +0xBC cell
    const bool altHeld = state.menuKeyState == 3;          // +0xC4 cell

    // ---- G2: camera/accessory mode register ------------------------------
    if (FocusOk(app, focus) && app->PlaybackActive() == 0 &&
        state.optflag[0] != 0 && focusNotInPanelEdit &&
        enterPressed && !altHeld) {
        app->SceneModified() = 1;                          // 0xA0B0D
        for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->CameraKeys()); i < capacity; ++i)
            app->CameraKeys()[i].selected = 0;
        for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->LightKeys()); i < capacity; ++i)
            app->LightKeys()[i].selected = 0;
        for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->ShadowKeys()); i < capacity; ++i)
            app->ShadowKeys()[i].selected = 0;
        for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(app->GravityKeys()); i < capacity; ++i)
            app->GravityKeys()[i].selected = 0;
        for (int slot = 0; slot < 0xFF; ++slot)
            for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(app->AccessoryKeys(slot))); i < poolCapacity; ++i)
                app->AccessoryKeys(slot)[i].selected = 0;  // 0x384 table

        const std::int32_t frame = app->CurrentFrame();    // 0x980
        if (app->GlobalTrackSelected(GlobalTimelineTrack::Camera) != 0)
            RegisterCameraState(app, frame);               // 0x410560
        if (app->GlobalTrackSelected(GlobalTimelineTrack::Light) != 0)
            RegisterLightState(app, frame);                // 0x411630
        if (app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow) != 0)
            RegisterSelfShadowState(app, frame);           // 0x411DF0
        if (app->GlobalTrackSelected(GlobalTimelineTrack::Gravity) != 0)
            RegisterGravityKeyCurrent(app, frame);         // 0x412B20
        for (int slot = 0; slot < 0xFF; ++slot) {
            mdl::AccessoryRecord* accessory = app->AccessorySlot(slot);
            if (accessory != nullptr && accessory->rowSelected != 0)
                RegisterAccessoryKey(app, frame, slot);               // 0x413CB0
        }
        PanelPaint(app);                                   // 0x414610
        SelectionReeval(app);                              // 0x430510
    }

    // ---- G3: model mode register button (no extra gates) -----------------
    if (enterPressed && !altHeld)
        // raw command id kept (no macro): 0x1F4 is the original's
        // register-button command, re-dispatched into the WM_COMMAND switch.
        SendMessageA(main, WM_COMMAND, 0x1F4, 0);          // case 500
}

// ---------------------------------------------------------------------------
// G4 - ESC stops a frame-step recording (x86 0x4730F8..0x473127,
// x64 0x450186..0x4501B5).  Gate: viewport active && frame-step flag set
// && ESC pressed && !panel-edit focus (no focus-OK, no playing gate).
// ---------------------------------------------------------------------------
void PumpEscStop(MMDApp* app, bool focusNotInPanelEdit) {
    if (app->ViewportInputActive() != 0 &&
        app->FrameStepPlayback() != 0 &&
        app->state.escKeyState == 1 &&
        focusNotInPanelEdit)
        FinishAviRecord(app);                                    // 0x464A00
}

// ---------------------------------------------------------------------------
// Sequencer - the pump's execution order for this family.  Wire into
// FrameDriver right after MouseInteractionBegin and before
// ConsumeLetterHotkeys (frame_driver.cpp): G12 must run first because it
// both moves focus and computes the focusNotInPanelEdit flag the other
// segments consume, and the letters do not touch any cell consumed here
// (RETURN / ESC / TAB / VK221 / VK226 / DELETE).
// ---------------------------------------------------------------------------
void ConsumeEditKeys(MMDApp* app) {
    const HWND focus = GetFocus();                         // 0x44DEF8
    // The curve editor owns Enter/Delete/Tab itself. The legacy viewport flag
    // can remain set after it opens, so FocusOk alone cannot isolate input.
    if (curve_editor::OwnsWindow(app->Hwnd(), focus) ||
        curve_editor::OwnsWindow(app->Hwnd(), GetForegroundWindow())) return;
    const bool focusNotInPanelEdit =
        !PumpPanelFocusChain(app, focus);                  // r13 @ 0x44ED2E
    PumpInterpolationToggle(app, focus, focusNotInPanelEdit);  // G6
    PumpDeleteRebuild(app, focus, focusNotInPanelEdit);        // G8
    PumpTabCycle(app, focus, focusNotInPanelEdit);             // G5
    PumpFullscreenKeys(app, focus, focusNotInPanelEdit);       // G7
    if (device_lost_wait::Pending(app)) {
        // The same ESC can exit fullscreen and cancel recording. Preserve
        // cancellation/cleanup even when the fullscreen Reset has failed.
        PumpEscStop(app, focusNotInPanelEdit);
        return;
    }
    PumpEnterRegisterFrame(app, focus, focusNotInPanelEdit);   // G2 + G3
    PumpEscStop(app, focusNotInPanelEdit);                     // G4
}

}  // namespace mikumikustudio
