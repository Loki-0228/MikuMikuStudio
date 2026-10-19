// ===========================================================================
// VA 0x0044C5D0 - FrameRangeDlgProc  (original: sub_44C5D0, 0x218 bytes)
// VA 0x0047A3F0 - SelectNavDlgProc  (original: sub_47A3F0, 0x1B5 bytes)
// ===========================================================================
// sub_44C5D0 is the modal "frame range" dialog proc (template reached from
// CommandDispatch case 424, 0x0048A6xx DialogBoxParamA): on
// WM_INITDIALOG it mirrors the two main-window range edits 425/426 into
// its own edits 686/687 (atol + "%d" round-trip), presets edit 605 with
// the .rdata literal "1.0" (0x52D740), checks the three radio/check
// boxes 688..690 (BM_SETCHECK 1) and focuses edit 686 with an all-select
// (EM_SETSEL 0..len, after a transient focus to 605 + EM_SETSEL 0..3,
// verbatim).  OK (id 1) applies through sub_43E970 then EndDialog(1);
// Cancel (id 2) EndDialog(2).  When the alternate-dialog slot
// app+0xA0D38 is non-null the dialog is raised to the top of the z
// order (SetWindowPos HWND_TOPMOST, NOSIZE|NOMOVE; the Hex-Rays
// "HWND_MESSAGE|2" rendering is wrong, x64 0x7FF7CB475FE5 is
// or rdx,-1 with flags 3).
//
// sub_47A3F0 is the selection-list navigation dialog proc (case 402,
// 0x0048A990 CreateDialogParamA family): WM_INITDIALOG fills the list
// (sub_466630); ids 670/671 walk the combo 669 selection back/forward
// within CB_GETCOUNT bounds (CB_GETCURSEL 0x147 / CB_GETCOUNT 0x146 /
// CB_SETCURSEL 0x14E) with sub_461C20 refreshing after each move;
// id 632 -> sub_4256C0(app), id 630 -> sub_43D610; click notifications
// (HIWORD==1) from 669/673/677 route to sub_461C20 / sub_43D2E0(hDlg,0)
// / sub_43D560; id 2 cancels with EndDialog(1).
//
// The selection-dialog call targets remain pending.  The frame-range apply
// target is ported in frame_range_apply.cpp.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_translation.hpp"

namespace mikumikustudio {

// ---- pending dependencies (stubs.cpp / dialog_select_ops.cpp) -------------
// All of the 0x466630/0x461C20/0x43Dxxx family are __thiscall on Block in
// the original (caller loads ECX from the Block global), so the HWND-only
// stub signatures were missing the MMDApp* first parameter.  The bodies are
// ported in dialog_select_ops.cpp with these corrected signatures.
void InitSelectNavDialog(MMDApp* app, HWND hDlg);    // was Sub466630, VA 0x00466630
void SyncSelectAttachCombo(MMDApp* app, HWND hDlg);  // was Sub461C20, VA 0x00461C20
void ResetModelSelection(MMDApp* app);               // was Sub4256C0, VA 0x004256C0
void ApplyBoneAttach(MMDApp* app, HWND hDlg);        // was Sub43D610, VA 0x0043D610
void RebuildTargetBoneList(MMDApp* app, HWND hDlg, int keepSelection);  // was Sub43D2E0, VA 0x0043D2E0
void CommitTargetBonePick(MMDApp* app, HWND hDlg);   // was Sub43D560, VA 0x0043D560
void ApplyFrameRangeScale(HWND hDlg);                // VA 0x0043E970, was Sub43E970
                                                   // (frame_range_apply.cpp)

// ===========================================================================
// VA 0x0044C5D0 - FrameRangeDlgProc
// ===========================================================================
INT_PTR CALLBACK FrameRangeDlgProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam) {  // was Sub44C5D0
    (void)lParam;
    char text[256];

    if (Msg == WM_INITDIALOG) {                     // 0x44C5F8
        MMDApp* app = g_Block;
        LocalizeChineseDialog(app, hDlg);
        if (app->state.floatingWindow != 0)   // 0xA0D38
            SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOSIZE | SWP_NOMOVE);  // x64 0x7FF7CB475FE5: or rdx,-1, flags 3

        const HWND main = reinterpret_cast<HWND>(
            app->state.hwnd);
        GetWindowTextA(GetDlgItem(main, panel::kRangeStartEdit), text, sizeof text);           // 0x44C68E
        sprintf_s(text, 0x100, "%d", atol(text));
        SendMessageA(GetDlgItem(hDlg, panel::kScaleFromEdit), EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>(text));
        GetWindowTextA(GetDlgItem(main, panel::kRangeStopEdit), text, sizeof text);           // 0x44C6EB
        sprintf_s(text, 0x100, "%d", atol(text));
        SendMessageA(GetDlgItem(hDlg, panel::kScaleToEdit), EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>(text));

        SendMessageA(GetDlgItem(hDlg, panel::kScaleRateEdit), EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>("1.0"));  // 0x52D740
        SetFocus(GetDlgItem(hDlg, panel::kScaleRateEdit));                           // 0x44C74C
        SendMessageA(GetDlgItem(hDlg, panel::kScaleRateEdit), EM_SETSEL, 0, 3);
        SendMessageA(GetDlgItem(hDlg, panel::kScaleBoneCheckbox), BM_SETCHECK, 1, 0);
        SendMessageA(GetDlgItem(hDlg, panel::kScaleMorphCheckbox), BM_SETCHECK, 1, 0);
        SendMessageA(GetDlgItem(hDlg, panel::kScaleDispIkCheckbox), BM_SETCHECK, 1, 0);

        SetFocus(GetDlgItem(hDlg, panel::kScaleFromEdit));                           // 0x44C7A7
        const HWND edit = GetDlgItem(hDlg, panel::kScaleFromEdit);
        SendMessageA(edit, EM_SETSEL, 0, GetWindowTextLengthA(edit));
        return 0;
    }
    if (Msg == WM_COMMAND) {                        // 0x44C5FD
        if (LOWORD(wParam) == 1) {                  // OK
            ApplyFrameRangeScale(hDlg);              // 0x0043E970
            EndDialog(hDlg, 1);
        } else if (LOWORD(wParam) == 2) {           // Cancel
            EndDialog(hDlg, 2);
        }
    }
    return 0;
}

// ===========================================================================
// VA 0x0047A3F0 - SelectNavDlgProc
// ===========================================================================
INT_PTR CALLBACK SelectNavDlgProc(HWND hDlg, UINT Msg, WPARAM wParam, LPARAM lParam) {  // was Sub47A3F0
    if (Msg == WM_INITDIALOG) {                     // 0x47A3F9
        InitSelectNavDialog(g_Block, hDlg);                   // 0x00466630 list fill
        return 0;
    }
    if (Msg != WM_COMMAND)
        return 0;

    if (LOWORD(wParam) == 2) {                      // 0x47A40F cancel
        EndDialog(hDlg, 1);
        return 0;
    }
    if (LOWORD(wParam) == 671) {                    // 0x47A42A previous
        HWND combo = GetDlgItem(hDlg, panel::kBoneNameCombo);
        const LRESULT cur = SendMessageA(combo, CB_GETCURSEL, 0, 0);
        if (cur > 0) {
            SendMessageA(combo, CB_SETCURSEL, cur - 1, 0);
            SyncSelectAttachCombo(g_Block, hDlg);
        }
        return 0;
    }
    if (LOWORD(wParam) == 670) {                    // 0x47A47A next
        HWND combo = GetDlgItem(hDlg, panel::kBoneNameCombo);
        const LRESULT cur = SendMessageA(combo, CB_GETCURSEL, 0, 0);
        if (cur < SendMessageA(combo, CB_GETCOUNT, 0, 0) - 1) {
            SendMessageA(combo, CB_SETCURSEL, cur + 1, 0);
            SyncSelectAttachCombo(g_Block, hDlg);
        }
        return 0;
    }
    if (LOWORD(wParam) == 632) {                    // 0x47A4DD
        ResetModelSelection(g_Block);
        return 0;
    }
    if (LOWORD(wParam) == 630) {                    // 0x47A4F6
        ApplyBoneAttach(g_Block, hDlg);
        return 0;
    }
    if (HIWORD(wParam) == 1) {                      // 0x47A516 click
        const HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (ctrl == GetDlgItem(hDlg, panel::kBoneNameCombo)) {
            SyncSelectAttachCombo(g_Block, hDlg);
            return 0;
        }
        if (ctrl == GetDlgItem(hDlg, panel::kMorphNameCombo)) {
            RebuildTargetBoneList(g_Block, hDlg, 0);
            return 0;
        }
        if (ctrl == GetDlgItem(hDlg, panel::kGroupNameCombo)) {
            CommitTargetBonePick(g_Block, hDlg);
            return 0;
        }
    }
    return 0;
}

}  // namespace mikumikustudio
