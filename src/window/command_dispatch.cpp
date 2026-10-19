#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/force_paste.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/frame_rate_correction.hpp"
#include "mikumikustudio/sequence_scatter.hpp"
#include "mikumikustudio/native_media.hpp"
// ===========================================================================
// VA 0x0047E8A0 - CommandDispatch  (original: sub_47E8A0, 0x10A6A = 68KB)
// ===========================================================================
// WM_COMMAND target from MainWndProc (0x004C3A10): a ~368-case switch on
// LOWORD(wParam).  The switch body is split by id family into the files
// below (each keeps its own case -> VA mapping in the header comment):
//   0x000..0x0FF  File menu          200..250  command_file_menu.cpp
//   0x0FB..0x134  View/option menu   251..308  command_view_menu.cpp
//   0x190..0x1C1  control notif.     400..449  command_control_400.cpp
//   0x1C2..0x1F3  control notif.     450..499  command_control_450.cpp
//   0x1F4..0x237  control notif.     500..567  command_control_500.cpp
// The 19 cases implemented directly below (200 / 0xC9..0xDC) predate the
// family split and stay here; everything else funnels to the families.
//
// The original default region def_47E903 (0x482897; x64 dispatcher
// sub_7FF7CB45F550 default at 0x7ff7cb472c40) is NOT a no-op: ids without a
// jump-table entry whose HIWORD(wParam) == 1 (CBN_SELCHANGE) are dispatched
// by control HWND - lParam is compared against GetDlgItem(hwnd, id) of the
// combos 0x1B4/0x1BB/0x1D7/0x1C1/0x1C2/0x1DA/0x1DB/0x1F8/0x1FD/0x202/0x207/
// 0x1B1/0x1B2 in this fixed order.  That chain is ported below as
// DefaultSelChangeChain (control 0x1B4 heads the original chain but stays
// ported as family case 436 in command_control_400.cpp, equivalent
// semantics).  x86 handler VA per control:
//   0x1B4 0x4828AA  0x1BB 0x48E214  0x1D7 0x48E2A4  0x1C1 0x48E37C
//   0x1C2 0x48E62F  0x1DA 0x48E75E  0x1DB 0x48E8FD  0x1F8 0x48EA48
//   0x1FD 0x48EC56  0x202 0x48EE66  0x207 0x48F066  0x1B1 0x48F263
//   0x1B2 0x48F27E
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/skinning_menu.hpp"
#include "mikumikustudio/physics_bake.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "viewport_corner_menu.hpp"

namespace mikumikustudio {

// Family entry points (defined in the per-family TUs listed above).
void CmdFileMenu(MMDApp* app, HWND hwnd, std::uint16_t id, std::uint16_t notify);
void CmdViewMenu(MMDApp* app, HWND hwnd, std::uint16_t id, std::uint16_t notify);
void CmdControl400(MMDApp* app, HWND hwnd, std::uint16_t id, std::uint16_t notify);
void CmdControl450(MMDApp* app, HWND hwnd, std::uint16_t id, std::uint16_t notify);
void CmdControl500(MMDApp* app, HWND hwnd, std::uint16_t id, std::uint16_t notify);

namespace {

// Model combo-order byte (model+0x2D7C): the value matched against the
// cursor of the parent-model combos 0x1B4/0x1C1/0x1DA (case 437's lookup;
// same constant as the 400 family).
constexpr std::size_t kModelComboOrder2D7C = 0x2D7C;

// Accessory timeline-row selection flag (x86 acc+0x4AC, x64 acc+0x4BC): set
// on the accessory chosen in combo 0x1D7, cleared on all others, and read by
// the (not yet ported) pump Enter-registration block.  No promoted accessor
// exists yet - the byte lives in the record's reserved tail.
// rowSelected lives on AccessoryRecord (accessory_layout.hpp).

// Shared body of the 0x1C1/0x1DA model-found branches (0x48e439..0x48e4ed /
// 0x48e7f5..0x48e8db): repopulate a bone combo with the model's selectable
// bones (type byte +0x1E4 == 8 or < 7; EN name +0x14 when the english-UI
// byte 0xA0B4C is set, JP name +0x0 otherwise) and set the cursor to 0.
// The original's byte is two-valued, so its `!= 0` test means "English UI";
// the port's third language (2 = Chinese) is not English, hence the shared
// UiUsesEnglishNames() test - with `!= 0` the Chinese UI listed English bone
// names here while the morph combos stayed on the model's own names.
void RepopulateBoneCombo(MMDApp* app, HWND combo, std::int32_t modelSlot) {
    FillBoneCombo(combo, mdl::Mdl(app->ModelSlot(modelSlot)),
                  UiUsesEnglishNames(app->EnglishUI()));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

// Shared body of the four facial morph-row combos 0x1F8/0x1FD/0x202/0x207
// (0x48ea48 / 0x48ec56 / 0x48ee66 / 0x48f066): each Unicode combo item
// carries its morph index, independent of translated or duplicate names.
// The selected item's index is stored into
// selectedMorphs[lane] (0x2D9C+4*lane); the row slider receives
// TBM_SETPOS (int)(v*100) and the edit the "%5.4f" echo.  Every
// display-frame row (model+0x26DC, 0x2E stride) then gets its selected byte
// (+0x2C) set when its target word (+0x2A) equals the new morph index,
// cleared otherwise.  Tail: the two language sweeps.
void MorphRowSelect(MMDApp* app, HWND hwnd, HWND combo, std::size_t lane,
                    int spinId, int textId) {
    char text[0x100];
    const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || app->SelectedModel() == nullptr) return;
    const mdl::ModelRecord* record = mdl::Mdl(app->SelectedModel());
    const LRESULT index = SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    if (record->morphs == nullptr || index < 0 ||
        static_cast<std::uint64_t>(index) >= record->morphCount) return;
    mdl::Mdl(app->SelectedModel())->selectedMorphs[lane] = static_cast<std::int32_t>(index);
    const float v = record->morphs[index].value;
    SendMessageA(GetDlgItem(hwnd, spinId), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<std::int32_t>(v * 100.0)));  // dbl_52B8E0
    sprintf_s(text, 0x100u, "%5.4f", static_cast<double>(v));
    SetWindowTextA(GetDlgItem(hwnd, textId), text);
    // display-frame row sweep (0x48ec01..0x48ec41)
    mdl::ModelRecord* live = mdl::Mdl(app->SelectedModel());
    mdl::FrameGroup* groups = live->displayFrames;
    for (std::uint32_t i = 0; i < mikumikustudio::mdl::FacialFrameCount(*live); ++i) {
        groups[i].selected = (mikumikustudio::mdl::FrameTargetIndex(groups[i]) == index) ? 1 : 0;
    }
    PostLanguageSweep(app);   // 0x42F1E0 (x64 sub_7FF7CB47F7F0)
    PostLanguageSweep2(app);  // 0x40D070 (x64 sub_7FF7CB440CE0)
}

// The default-chain compare chain (def_47E903 @ 0x482897, x64 default at
// 0x7ff7cb472c40).  Returns true when a control matched and its handler ran;
// every matched path ends at the SetFocus(hwnd) exit (0x48f2d3) except
// 0x1DA's "no accessory selected" bail (0x48f2e0), which keeps the focus.
bool DefaultSelChangeChain(MMDApp* app, HWND hwnd, HWND ctrl) {
    // ---- 0x1BB (0x48e214): IK list combo -> on/off radio pair sync.  The
    // flag byte of the 24-byte-stride IK table model+0x26C0 (the same entry
    // cases 444/445 write) picks between radios 0x1BC (on) / 0x1BD (off).
    if (ctrl == GetDlgItem(hwnd, panel::kIkChainCombo)) {
        const LRESULT sel = SendMessageA(GetDlgItem(hwnd, panel::kIkChainCombo),
                                         CB_GETCURSEL, 0, 0);
        mdl::IkChain* chains = mdl::Mdl(app->SelectedModel())->ikChains;
        if (chains != nullptr) {
            CheckRadioButton(hwnd, 0x1BC, 0x1BD,
                             chains[sel].enabled != 0 ? 0x1BC : 0x1BD);
        }
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1D7 (0x48e2a4): main accessory combo.  Finds the accessory
    // (order byte +0x49D) whose combo entry was picked, drops the four
    // global-row selection bytes 0xA03E4..0xA03E7 and every accessory row
    // flag, marks the new accessory, and re-syncs its edit panel
    // (0x4134E0).  Both paths end with the two language sweeps.
    if (ctrl == GetDlgItem(hwnd, panel::kAccessoryCombo)) {
        const LRESULT sel = SendMessageA(GetDlgItem(hwnd, panel::kAccessoryCombo),
                                         CB_GETCURSEL, 0, 0);
        std::int32_t slot = 0;
        bool found = false;
        for (; slot < 0xFF; ++slot) {
            mdl::AccessoryRecord* acc = app->AccessorySlot(slot);
            if (acc != nullptr &&
                static_cast<int>(acc->order) == static_cast<int>(sel)) {
                found = true;
                break;
            }
        }
        if (found) {
            app->GlobalTrackSelected(GlobalTimelineTrack::Camera) = 0;
            app->GlobalTrackSelected(GlobalTimelineTrack::Light) = 0;
            app->GlobalTrackSelected(GlobalTimelineTrack::SelfShadow) = 0;
            app->GlobalTrackSelected(GlobalTimelineTrack::Gravity) = 0;
            for (std::int32_t i = 0; i < 0xFF; ++i) {
                mdl::AccessoryRecord* acc = app->AccessorySlot(i);
                if (acc != nullptr) {
                    acc->rowSelected = 0;
                }
            }
            app->SelectedAccessorySlot() =
                static_cast<std::uint8_t>(slot);
            app->AccessorySlot(slot)->rowSelected = 1;
            SyncAccessoryEditPanel(app);  // 0x4134E0 accessory edit panel sync
        }
        PostLanguageSweep(app);
        PostLanguageSweep2(app);
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1C1 (0x48e37c): camera manipulation parent MODEL combo.
    // Cursor > 0 selects the model (combo-order byte 0x2D7C), rebuilds the
    // 0x1C2 bone list from its selectable bones and resets the parent bone;
    // cursor <= 0 detaches, keeping the attach bone's world position
    // (matInit * position, x87 double intermediates), zeroing the rotation
    // and setting the distance to -20.0f (0x52F0B4).  Tail:
    // RefreshRequest(-1) + PostViewRefresh.
    if (ctrl == GetDlgItem(hwnd, panel::kMainComboNormal)) {
        const LRESULT sel = SendMessageA(GetDlgItem(hwnd, panel::kMainComboNormal),
                                         CB_GETCURSEL, 0, 0);
        HWND boneCombo = GetDlgItem(hwnd, panel::kBoneRegisterCombo);
        SendMessageA(boneCombo, CB_RESETCONTENT, 0, 0);
        if (sel > 0) {
            std::int32_t slot = 0;
            bool found = false;
            // 槽扫描界 255：x64 0x7FF7CB472E70 处 mov r13d,0FFh 后按 8 字节
            // 步进扫槽数组（x86 原版才是 0x64）
            for (; slot < kModelSlotCount; ++slot) {
                unsigned char* model = app->ModelSlot(slot);
                if (model != nullptr &&
                    mdl::Mdl(model)->comboSelIndex ==
                        static_cast<int>(sel)) {
                    found = true;
                    break;
                }
            }
            if (found) {
                if (app->CameraParentModel() < 0) {
                    // Start at a visible distance from the bone. A zero relative
                    // Z puts the eye on its origin and clips the binding target.
                    app->CameraPositionAxis(0) = 0.0f;
                    app->CameraPositionAxis(1) = 0.0f;
                    const float distance=app->CameraDistance();
                    app->CameraPositionAxis(2) =
                        std::isfinite(distance)&&distance<-0.1f?distance:-45.0f;
                    app->CameraRotation()[0] = 0.0f;
                    app->CameraRotation()[1] = 0.0f;
                    app->CameraRotation()[2] = 0.0f;
                }
                app->CameraParentModel() = slot;
                RepopulateBoneCombo(app, boneCombo, slot);
                const int bone=SelectedComboBone(boneCombo,mdl::Mdl(app->ModelSlot(slot)));
                app->CameraParentBone() = bone>=0?bone:0;
            }
        } else {
            const std::int32_t parent = app->CameraParentModel();
            if (parent >= 0) {
                const mdl::BoneRecord& bone =
                    mdl::Mdl(app->ModelSlot(parent))
                        ->boneTable[app->CameraParentBone()];
                const double px = bone.position[0];
                const double py = bone.position[1];
                const double pz = bone.position[2];
                app->CameraPositionAxis(0) = static_cast<float>(
                    py * bone.matInit[4] + px * bone.matInit[0] +
                    pz * bone.matInit[8] + bone.matInit[12]);
                app->CameraPositionAxis(1) = static_cast<float>(
                    py * bone.matInit[5] + px * bone.matInit[1] +
                    pz * bone.matInit[9] + bone.matInit[13]);
                app->CameraPositionAxis(2) = static_cast<float>(
                    py * bone.matInit[6] + px * bone.matInit[2] +
                    pz * bone.matInit[10] + bone.matInit[14]);
                app->CameraDistance() = -20.0f;
                app->CameraRotation()[0] = 0.0f;
                app->CameraRotation()[1] = 0.0f;
                app->CameraRotation()[2] = 0.0f;
            }
            SendMessageA(GetDlgItem(hwnd, panel::kBoneRegisterCombo),
                         CB_RESETCONTENT, 0, 0);
            app->CameraParentModel() = -1;
        }
        RefreshRequest(-1);    // 0x440AC0 (x64 sub_7FF7CB4BF4F0)
        PostViewRefresh(app);  // 0x40D130 (x64 sub_7FF7CB440DD0)
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1C2 (0x48e62f): camera manipulation parent BONE combo.  The
    // item data stores the bone index into the parent-bone field (0xA0434) and
    // refreshes the timeline row selection.
    if (ctrl == GetDlgItem(hwnd, panel::kBoneRegisterCombo)) {
        const std::int32_t parent = app->CameraParentModel();
        if (parent >= 0 && parent < kModelSlotCount) {
            const std::int32_t index =
                SelectedComboBone(ctrl, mdl::Mdl(app->ModelSlot(parent)));
            if (index >= 0) {
                app->CameraParentBone() = index;
                RefreshRequest(-1);
            }
        }
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1DA (0x48e75e): accessory parent MODEL combo (accessory from
    // the 0x9E170 slot byte).  Cursor > 0 re-parents to the model found by
    // its 0x2D7C order byte, rebuilding the 0x1DB bone list; cursor <= 0
    // detaches (parentModel = -1).  With no selected accessory the whole
    // handler bails without touching the focus (0x48f2e0).
    if (ctrl == GetDlgItem(hwnd, panel::kMainComboGround)) {
        mdl::AccessoryRecord* acc =
            app->AccessorySlot(app->SelectedAccessorySlot());
        if (acc == nullptr) {
            return true;
        }
        const LRESULT sel = SendMessageA(GetDlgItem(hwnd, panel::kMainComboGround),
                                         CB_GETCURSEL, 0, 0);
        HWND boneCombo = GetDlgItem(hwnd, panel::kAttachBoneCombo);
        SendMessageA(boneCombo, CB_RESETCONTENT, 0, 0);
        if (sel > 0) {
            std::int32_t slot = 0;
            bool found = false;
            // 槽扫描界 255：x64 0x7FF7CB4732F7 处 mov r13d,0FFh（同 0x1C1）
            for (; slot < kModelSlotCount; ++slot) {
                unsigned char* model = app->ModelSlot(slot);
                if (model != nullptr &&
                    mdl::Mdl(model)->comboSelIndex ==
                        static_cast<int>(sel)) {
                    found = true;
                    break;
                }
            }
            if (found) {
                acc->parentModel = slot;
                RepopulateBoneCombo(app, boneCombo, slot);
                acc->parentBone = 0;
                RefreshRequest(
                    static_cast<int>(app->SelectedAccessorySlot()));
            }
        } else {
            acc->parentModel = -1;
        }
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1DB (0x48e8fd): accessory parent BONE combo. Item data stores
    // the bone index into acc+0x234, independent of duplicate/translated names.
    if (ctrl == GetDlgItem(hwnd, panel::kAttachBoneCombo)) {
        mdl::AccessoryRecord* acc =
            app->AccessorySlot(app->SelectedAccessorySlot());
        if (acc != nullptr && acc->parentModel >= 0 && acc->parentModel < kModelSlotCount) {
            const std::int32_t index =
                SelectedComboBone(ctrl, mdl::Mdl(app->ModelSlot(acc->parentModel)));
            if (index >= 0) {
                acc->parentBone = index;
                RefreshRequest(
                    static_cast<int>(app->SelectedAccessorySlot()));
            }
        }
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1F8 / 0x1FD / 0x202 / 0x207: facial morph row combos (the
    // slider/edit partners 0x1F9/0x1FA etc. never emit WM_COMMAND).
    if (ctrl == GetDlgItem(hwnd, panel::kMorphCombo0)) {
        MorphRowSelect(app, hwnd, ctrl, 0, 0x1F9, 0x1FA);
        SetFocus(hwnd);
        return true;
    }
    if (ctrl == GetDlgItem(hwnd, panel::kMorphCombo1)) {
        MorphRowSelect(app, hwnd, ctrl, 1, 0x1FE, 0x1FF);
        SetFocus(hwnd);
        return true;
    }
    if (ctrl == GetDlgItem(hwnd, panel::kMorphCombo2)) {
        MorphRowSelect(app, hwnd, ctrl, 2, 0x203, 0x204);
        SetFocus(hwnd);
        return true;
    }
    if (ctrl == GetDlgItem(hwnd, panel::kMorphCombo3)) {
        MorphRowSelect(app, hwnd, ctrl, 3, 0x208, 0x209);
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1B1 (0x48f263): light-colour interpolation row combo ->
    // SelectionReeval (the x64 chain jumps straight into case 431's tail
    // at 0x7ff7cb463665 for the same effect).
    if (ctrl == GetDlgItem(hwnd, panel::kInterpCurveCombo)) {
        SelectionReeval(app);
        SetFocus(hwnd);
        return true;
    }

    // ---- 0x1B2 (0x48f27e): selection-target combo.  The cursor is stored
    // on the active model (0x4CCF0, restored when the model is re-selected;
    // cleared to 3 on scene load).
    if (ctrl == GetDlgItem(hwnd, panel::kRegisterScopeCombo)) {
        unsigned char* model = app->SelectedModel();
        if (model != nullptr) {
            const LRESULT sel = SendMessageA(GetDlgItem(hwnd, panel::kRegisterScopeCombo),
                                             CB_GETCURSEL, 0, 0);
            mdl::Mdl(model)->frameRegistrationSelection =
                static_cast<std::int32_t>(sel);
        }
        SetFocus(hwnd);
        return true;
    }

    return false;
}

}  // namespace

void CommandDispatch(HWND ctrl, WPARAM wParam) {
    runtime_log::Write("COMMAND id=%u notification=%u", LOWORD(wParam), HIWORD(wParam));
    MMDApp* app = g_Block;
    auto& s = *app;
    HWND hwnd = static_cast<HWND>(s.Hwnd());
    struct CurveSyncOnExit {
        HWND owner;
        ~CurveSyncOnExit() { curve_editor::NotifyOwnedChanged(owner); }
    } curveSync{hwnd};
    const std::uint16_t id = LOWORD(wParam);
    const std::uint16_t notification = HIWORD(wParam);
    if (ctrl == nullptr && notification <= 1 && image_sequence::HandleCommand(app, id)) return;
    if (ctrl == nullptr && notification <= 1 && plugin_menu::HandleCommand(app, id)) return;
    if (ctrl == nullptr && notification == 0 && bake::HandleCommand(app, id)) return;
    if (ctrl == nullptr && notification == 0 && frame_rate_correction::HandleCommand(app, id)) return;
    if (ctrl == nullptr && notification == 0 && sequence_scatter::HandleCommand(app, id)) return;
    if (ctrl == nullptr && notification == 0 && HandleSkinningCommand(app, id)) return;
    // MMS addition: the Help menu's dock-corner item (command 32810) cycles
    // one corner on each ordinary menu click.
    if (ctrl == nullptr && notification == 0 && viewport_corner_menu::HandleCommand(app, id)) return;
    if (ctrl == nullptr && notification == 0 && autosave::HandleCommand(app, id)) return;
    if (ctrl == nullptr && notification == 0 && viewport_render_scale::HandleCommand(app, id)) return;

    switch (id) {
    case 200:        // File: Exit (0x48BCF9) - flag + WM_CLOSE
        s.state.dialogFlags[0] = 1;
        SendMessageA(hwnd, WM_CLOSE, 0, 0);
        break;

    case 0xC9: {     // Help: About (x64 0x7FF7CB472BAD)
        // Raise the dialog-in-flight flags (x64 app+0x58/+0xC0 =
        // dialogFlags[9]/enterKeyState), compose the banner into a 256-byte ANSI
        // buffer - the Japanese branch carries the Shift-JIS bytes of the
        // author's name (樋口優, 0x7FF7CB54F980) - and pop it over the
        // floating viewport window when one exists (x64 app+0xA1DE0).
        s.state.dialogFlags[9] = 1;
        s.state.enterKeyState = 1;
        char text[256];
        sprintf_s(text, 256,
                  s.EnglishUI() != 0
                      ? "MikuMikuStudio Ver.%4.2f\n  (DirectX9 Version)\n\n"
                        "programmed by Yu Higuchi"
                      : "MikuMikuStudio Ver.%4.2f\n  (DirectX9 Version)\n\n"
                        "programmed by \x94\xF3\x8C\xFB\x97\x44",
                  9.32);
        const HWND owner = s.state.floatingWindow != 0
                               ? reinterpret_cast<HWND>(s.state.floatingWindow)
                               : hwnd;
        LegacyUiMessageBoxA(owner, text, "About", MB_OK);
        break;
    }

    case 0xCA:      // File: load VPD pose
        CmdLoadPose(app);
        break;
    case 0xCB:      // File: save VPD pose (selected-bones check inside)
        CmdSavePose(app);
        break;
    case 0xCC:      // File: New - reset scene state
        CmdResetState(app);
        break;
    case 0xCD:      // File: open PMM (dirty confirm + dialog)
        CmdOpenScene(app);
        break;
    case 0xCE:      // File: open WAV
        CmdOpenWave(app);
        break;
    case 0xCF:      // File: save PMM (0x489AF7) - overwrite via stored path,
        //          falling into the save-as flow (case 0xD0) when unnamed
        if (app->EnvFileName()[0] != L'\0') {
            SaveSceneFile(app);
        } else {
            CmdSaveScene(app);
        }
        break;
    case 0xD0:      // File: save PMM as (0x489B12 GetSaveFileNameW flow)
        CmdSaveScene(app);
        break;
    case 0xD1:      // File: load VMD motion
        CmdLoadMotion(app);
        break;
    case 0xD2:      // File: save VMD motion
        CmdSaveMotion(app);
        break;
    case 0xD3: {    // View: information display toggle (0x47EAC7/0x47EB00)
        auto& flag = s.state.fpsOverlayEnabled;
        if (flag != 0) {
            flag = 0;
            CheckMenuItem(GetMenu(hwnd), 0xD3, MF_UNCHECKED);
        } else {
            flag = 1;
            s.state.fpsOverlayElapsedSeconds = 0.0f;   // timer cluster reset
            s.state.fpsOverlayFrameCount = 0;
            CheckMenuItem(GetMenu(hwnd), 0xD3, MF_CHECKED);
        }
        // 0x47EB56/0x47EB6F: sync the 0x227 checkbox - owned by the
        // floating window (0xA0D38) when open, else the main window.
        HWND owner3 = static_cast<HWND>(s.FloatingWindow());
        if (owner3 == nullptr) owner3 = hwnd;
        SendMessageA(GetDlgItem(owner3, panel::kInfoCheckbox), BM_SETCHECK,
                     s.state.fpsOverlayEnabled, 0);
        break;
    }
    case 0xD5:      // Background: load AVI file
        CmdLoadAvi(app);
        break;
    case 0xD6: {    // View: character transparent mode (0x487FA0)
        auto& flag = s.state.characterTransparentMode;
        flag = flag ? 0 : 1;
        CheckMenuItem(GetMenu(hwnd), 0xD6, flag ? MF_CHECKED : MF_UNCHECKED);
        // 0x487FF5..0x48803C: push the new flag into every loaded model's
        // displayState (+0x2D8C)
        for (int slot = 0; slot < kModelSlotCount; ++slot) {
            unsigned char* model = s.ModelSlot(slot);
            if (model != nullptr)
                mdl::Mdl(model)->displayState = flag;
        }
        break;
    }
    case 0xD7: {    // View: coordinate axis display (0x47FBF5)
        auto& flag = s.state.groundGridEnabled;
        flag = flag ? 0 : 1;
        CheckMenuItem(GetMenu(hwnd), 0xD7, flag ? MF_CHECKED : MF_UNCHECKED);
        // 0x47FC4F/0x47FC8B: sync the 0x22D checkbox - owned by the
        // floating window (0xA0D38) when open, else the main window.
        HWND owner7 = static_cast<HWND>(s.FloatingWindow());
        if (owner7 == nullptr) owner7 = hwnd;
        SendMessageA(GetDlgItem(owner7, panel::kCoordAxisCheckbox), BM_SETCHECK,
                     flag, 0);
        break;
    }
    case 0xD8: {    // Background: AVI display (0x4871F9)
        // Modern video has a Media Foundation reader instead of an AVI stream.
        auto& flag = s.state.aviBackgroundEnabled;
        if (flag == 1) {
            flag = 0;
            CheckMenuItem(GetMenu(hwnd), 0xD8, MF_UNCHECKED);
        } else if (s.state.aviStream != 0 || media::HasVideo(app)) {
            flag = 1;
            CheckMenuItem(GetMenu(hwnd), 0xD8, MF_CHECKED);
            AviBgOverlayRefresh(app);
        }
        break;
    }
    case 0xD9:      // Edit: select all bone frames (0x4831EA)
        SelectFrameGroup(app, 0);
        break;
    case 0xDA:      // Edit: select all disp/IK/OP frames (0x4832C8)
        SelectFrameGroup(app, 1);
        break;
    case 0xDC:      // Edit: select all facial frames (0x483258)
        SelectFrameGroup(app, 2);
        break;

    default:
        // Original default region def_47E903 (0x482897): a CBN_SELCHANGE
        // notification is dispatched by control HWND through the combo chain
        // ported above (control 0x1B4 = id 436 stays in the 400 family);
        // everything else funnels into the per-family TUs by id range (each
        // family switches on its own cases; unlisted ids and the remaining
        // 309..399 are no-ops, matching def_47E903 - except the port-added view
        // ids 304..308 (the transparent sort limit submenu: the command id
        // minus 304 is the stored enum value, so 304..307 are Low..Maximum and
        // 308 is Off).  Id 303 keeps a compatibility command that swaps Off and
        // Standard and has no menu item any more; it stays inside the view
        // range as well.
        if (notification == 1 /*CBN_SELCHANGE*/ &&
            DefaultSelChangeChain(app, hwnd, ctrl)) {
            break;
        }
        if (id >= 200 && id <= 250) {
            CmdFileMenu(app, hwnd, id, notification);
        } else if (id >= 251 && id <= 311) {  // includes model-language settings
            CmdViewMenu(app, hwnd, id, notification);
        } else if (id >= 400 && id <= 449) {
            CmdControl400(app, hwnd, id, notification);
        } else if (id >= 450 && id <= 499) {
            CmdControl450(app, hwnd, id, notification);
        } else if (id >= 500 && id <= 567) {
            CmdControl500(app, hwnd, id, notification);
        } else if ((id == 571 || id == 574 || id == 576 || id == panel::kAdaptiveShadowCheckbox || id == panel::kShadowFilterCheckbox) &&
                   notification == BN_CLICKED) {
            // MMS: the 缩放 X / Y / Z reset buttons (command_control_500.cpp).
            CmdControl500(app, hwnd, id, notification);
        }
        break;
    }
    RefreshForcePasteControls(app);
}

}  // namespace mikumikustudio
