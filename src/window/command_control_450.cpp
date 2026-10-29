#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/ui_translation.hpp"
// ===========================================================================
// CommandDispatch family: control notifications 450..499 (0x47E8A0)
// ===========================================================================
// Cases 450..499 (0x1C2..0x1F3) of the 368-case switch in CommandDispatch
// (0x47E8A0): editor-panel control notifications (buttons / combos / toggles
// of the accessory, bone-selection and physics panels), plus value echo and
// panel refresh chains.
//
// Case map (id -> behaviour -> original VA).  Index byte_48F650[id-0xC8]
// selects the jump-table entry jpt_47E903 (0x48F30C); targets below are the
// `jumptable 0047E903 case N` labels confirmed by IDA:
//
//   id  hex  VA            behaviour
//   --- ---  ------------  ---------------------------------------------------
//   450 1C2  def_47E903    default (no-op for this id - see default note)
//   451 1C3  0x00486431    camera reset: pos/rot floats 0, gate app+0xA0430
//                          (<0: posY 10, camangle -45; else 0), Refresh(-1),
//                          PostViewRefresh
//   452 1C4  0x0048681D    clear frame selection (4 arrays + 255 bone-frame
//                          tables), Refresh(-1), RegisterCameraState,
//                          PanelPaint, SelectionReeval
//   453 1C5  0x0047E99F    opt flag byte 0x2F9 = 0; PostModelReload2,
//                          HandleWindowSize, InvalidateRect
//   454 1C6  0x0047E9C8    opt flag byte 0x2F9 = 1; same refresh chain
//   455-466 1C7-1D2  def_47E903   default (no-op)
//   467 1D3  0x0048648F    accessory reset: pos (-0.5,-1,0.5), pose blob
//                          memset + kind 3, color 0.6, scene vtable 0xCC/0xD4,
//                          echo %3d / %+3.1f into 461-466, TBM_SETPOS
//                          455-460, Refresh(-2)
//   468 1D4  0x004868C2    clear frame selection (like 452), Refresh(-2),
//                          RegisterLightState, PanelPaint
//   469 1D5  0x0047E9F1    opt flag byte 0x2FA = 0; same refresh chain
//   470 1D6  0x0047EA06    opt flag byte 0x2FA = 1; same refresh chain
//   471 1D7  def_47E903    default (no-op)
//   472 1D8  0x00486A59    load accessory (OPENFILENAMEW, EN/JP filter,
//                          menu-0x12D gate for initial dir, LoadAccessoryFile)
//   473 1D9  0x00486BA7    delete accessory: CB_GETCURSEL(0x1D7) lookup in
//                          slot table 0x9DD70, confirm MessageBox (EN/JP),
//                          free + rebuild slot (0x927C0 blob), index fixup,
//                          combo rebuild, EnableMenuItem 0xF9, RebuildCameraModePanel,
//                          Refresh(-1), PostLanguageSweep2
//   474,475 1DA,1DB  def_47E903   default (no-op)
//   476 1DC  0x00486F91    toggle accessory display byte 0x210,
//                          BM_SETCHECK(0x1DC), Refresh(byte 0x9E170)
//   477 1DD  0x0048A199    toggle accessory flag byte 0x49E (then default,
//                          which is a no-op for this id)
//   478-485 1DE-1E5  def_47E903   default (no-op)
//   486 1E6  0x00487011    toggle accessory flag byte 0x49C,
//                          BM_SETCHECK(0x1E6), Refresh(byte 0x9E170)
//   487 1E7  0x0048696B    per-accessory checkbox sync 0x1DE..0x1E4 via
//                          CommitEditControl, clear frame selection, Refresh(byte
//                          0x9E170), RegisterAccessoryKey, PanelPaint
//   488 1E8  0x0047EA2F    opt flag byte 0x2FB = 0; PostModelReload2,
//                          HandleWindowSize, InvalidateRect
//   489 1E9  0x0047EA58    opt flag byte 0x2FB = 1; same refresh chain
//   490 1EA  0x0047ECD6    MMS: merged select / box-select cycle.  original:
//                          radio 0x1EA, app+0x914 = checked(0x1EA) ? 0 : 2
//   491 1EB  0x0047ED57    MMS: bone scale mode.  original: radio 0x1EB,
//                          app+0x914 = checked(0x1EB) ? 1 : 2
//   492 1EC  0x0047EDDA    move: app+0x914 = checked(0x1EC) ? 4 : 2
//   493 1ED  0x0047EE5E    rotate: app+0x914 = checked(0x1ED) ? 3 : 2
//                          (all four: RefreshBoneModePanel writes the check
//                          states and the merged caption from app+0x914)
//   494 1EE  0x0047FB24    select all bones (flags at model+0x26BC, byte
//                          +0x1E4 type / word +0x1F4 & 0x400 gate), store mode 0
//                          (was: check radio 0x1EA, re-dispatch WM_COMMAND(0x1EA)),
//                          PostLanguageSweep + PostLanguageSweep2
//   495 1EF  0x0047F3FA    insert current-frame keyframes for all selected
//                          bones (bone-0 seed, D3DXMatrixRotationQuaternion
//                          bake for parent-less bones, identity for parented,
//                          change detection, frame-flag 0x2D98 marks,
//                          PostViewRefresh)
//   496 1F0  0x00481926    bone copy: count selected bones, enable 0x1F1/
//                          0x1F2, allocate 0x30-stride record array (app+0x350)
//                          with name/pos/quat snapshots
//   497 1F1  0x00481B4B    bone paste: clear selection, paste-counter 0x31B4
//                          wrap at 0x1E, undo records at model+0x26FC
//                          (0x24-stride), name-match paste loop, SetFocus,
//                          undo-flag 0x9EDB5 = 1
//   498 1F2  0x0048203D    bone paste (delete-tag variant, name-tag filter);
//                          full port in the case body below.  Needle bytes
//                          and the second search pointer remain unverified
//                          against the binary (docs/PORTING_STATUS.md:
//                          BONE-498 / BONE-498-NAME).
//   499 1F3  def_47E903    default (no-op)
//
// Default handler def_47E903 (0x482897): only acts when HIWORD(notify)==1
// (BN_CLICKED) and the sending control equals GetDlgItem(hwnd, 0x1B4) (then
// ApplyModelComboSelection when app+0xA0B50 == 0); otherwise the control-HWND compare chain
// (0x1B4, 0x1BB, ...) is walked and always misses for the ids in this family
// (450/452/455-466/471/474/475/478-485/499 are all > 0x1B4/0x1BB), so the
// default is a no-op here.  `notify` (= HIWORD of the original wParam) is
// therefore unused by the family; the parameter is kept for the shared
// dispatch signature.
//
// Fidelity notes:
//   * All float math follows the original x87 shape (double intermediates,
//     float stores).  Constants: flt_52964C = -0.5f, flt_5295E8 = -1.0f,
//     flt_52960C = 0.5f, flt_52C9A4 = 0.6f, flt_52A1E4 = 10.0f,
//     flt_52A1E8 = -45.0f, dbl_52B9E8 = 256.0, dbl_52B8E0 = 100.0.
//   * App-state members are named state fields; the accessory / model
//     field offsets used by this family are declared below as
//     file-local constants.
//
// Reference: ../translated/MikuMikuDance/fcn_0047e8a0.cpp
//   (the translated file covers only cases 200..0xDC; this port follows the
//   IDA decompilation of MikuMikuDance.exe 0x47E8A0 directly)
// =========================================================================//
#include "mikumikustudio/animation_curve_host.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include <vector>
#include <Windows.h>
#include <commctrl.h>

#include <commdlg.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {

struct BoneCopyRecord {
    char name[20];
    float position[3];
    float rotation[4];
};
static_assert(sizeof(BoneCopyRecord) == 0x30,
              "bone copy record ABI");

// 32-bit multiply with the original's overflow idiom (mul/seto/neg/or):
// returns 0xFFFFFFFF when the product overflows (cases 496/497/498).
static std::uint32_t MulOrMax(std::uint32_t a, std::uint32_t b) {
    const std::uint64_t r = static_cast<std::uint64_t>(a) * b;
    return r > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(r);
}

// Active model = slot array at this+0x780 indexed by byte this+0x910.
// (sub_47E8A0 pattern; the original re-derives it on every use.)
static unsigned char* ActiveModel(MMDApp* app) {
    return app->SelectedModel();
}

// IDirect3DDevice9 slot 0xCC is SetLight(0, D3DLIGHT9*).
void ApplySceneLight(MMDApp* app) {
    app->Renderer()->device->SetLight(0, &app->SceneLight());
}

// IDirect3DDevice9 slot 0xD4 is LightEnable(0, TRUE).
void EnableSceneLight(MMDApp* app) {
    app->Renderer()->device->LightEnable(0, TRUE);
}

// Japanese strings, byte-exact Shift-JIS as in the binary.
// 0x530648: "アクセサリ：%sを削除します\nアクセサリのフレームデータも全て削除されます\n(この操作は元に戻す事はできません)\n\n削除してもよろしいですか？"
static const char kMsgDelAccessoryJp[] =
    "\x83\x41\x83\x4E\x83\x5A\x83\x54\x83\x8A\x81\x46"
    "%s"
    "\x82\xF0\x8D\xED\x8F\x9C\x82\xB5\x82\xDC\x82\xB7\n"
    "\x83\x41\x83\x4E\x83\x5A\x83\x54\x83\x8A\x82\xCC\x83\x74\x83\x8C"
    "\x81\x5B\x83\x80\x83\x66\x81\x5B\x83\x5E\x82\xE0\x91\x53\x82\xC4"
    "\x8D\xED\x8F\x9C\x82\xB3\x82\xEA\x82\xDC\x82\xB7\n"
    "\x28\x82\xB1\x82\xCC\x91\x80\x8D\xEC\x82\xCD\x8C\xB3\x82\xC9"
    "\x96\xDF\x82\xB7\x8E\x96\x82\xCD\x82\xC5\x82\xAB\x82\xDC\x82\xB9"
    "\x82\xF1\x29\n\n"
    "\x8D\xED\x8F\x9C\x82\xB5\x82\xC4\x82\xE0\x82\xE6\x82\xEB\x82\xB5"
    "\x82\xA2\x82\xC5\x82\xB7\x82\xA9\x81\x48";
// 0x530638: "アクセサリ削除"
static const char kCaptionDelAccessoryJp[] =
    "\x83\x41\x83\x4E\x83\x5A\x83\x54\x83\x8A\x8D\xED\x8F\x9C";

// 0x529688: swprintf_s format L"\0\0%s%s" - the leading NULs make the
// original call (which passes NO varargs) write an empty string; the %s
// slots are never consumed.  Kept byte-identical; the two dummy args below
// are never read (only silence the compiler's format-string warning).
static const wchar_t kFmt529688[] = L"\x0\x0%s%s";

// 0x52D948: JP open-filter for accessories (double-NUL terminated):
// "読込可能ファイル(*.x,*.vac)"
static const wchar_t kFilterAccJp[] =
    L"\x8AAD\x8FBC\x53EF\x80FD\x30D5\x30A1\x30A4\x30EB"
    L"(*.x,*.vac)\x00*.x;*.vac\x00x files(*.x)\x00*.x\x00"
    L"vac files(*.vac)\x00";
// 0x52DC84: JP dialog title "ファイルを開く"
static const wchar_t kTitleOpenJp[] = L"\x30D5\x30A1\x30A4\x30EB\x3092\x958B\x304F";

// strstr needles of case 498.  x64 control flow verified (2026-09-07,
// tools/x64_disasm_case498.py): the mirror-paste body 0x14004425E does
//   pass 1: strstr(bones[k1].name, 左); find the 右-tagged bone whose
//           suffix after the tag matches (0x14004456C..0x140044617)
//   pass 2 (fallback): strstr(bones[k1].name, 右); find the 左-tagged
//           bone whose suffix matches (0x14004461D..0x1400446D9)
//   fallback: target = k1 (0x1400446D9)
// and mirrors via xmm6 = -0.0f pool @0x140132B80.  The needles are the
// SJIS pair 左 (0x14012D9BC, \x8D\xB6) and 右 (0x14012D9C0, \x89\x45);
// モデル削除/ボーン削除 bytes do not occur in the x64 image.  The x86
// 0x530BFC image bytes remain unverified (no x86 binary at hand).
static const char kNeedle530BF8[] = "\x89\x45";   // 右 @0x14012D9C0
static const char kNeedle530BFC[] = "\x8D\xB6";   // 左 @0x14012D9BC

// ---------------------------------------------------------------------------
// External targets ported in other translation units (declared here with
// their original VAs; bodies in the files noted on each line).
// ---------------------------------------------------------------------------
void RefreshRequest(int area);                     // VA 0x00440AC0 (ui_refresh.cpp)
void PanelPaint(MMDApp* app);                      // VA 0x00414610 (ui_panel_paint.cpp)
void SelectionReeval(MMDApp* app);                 // VA 0x00430510 (ui_selection_reeval.cpp)
void CopyDirPathW(wchar_t* dest, const wchar_t* src); // VA 0x0042AE20 (media_load.cpp) path copy

// ---------------------------------------------------------------------------
// Dependencies ported in other translation units (call sites preserved via
// these file-local declarations; bodies in the files noted on each line).
// ---------------------------------------------------------------------------
void PushBoneEditUndo(MMDApp* app);                       // VA 0x0042D6E0 (bone_edit_undo.cpp)
void RebuildCameraModePanel(MMDApp* app);                       // VA 0x0044D780 (ui_model_reload.cpp)
void SyncAccessoryEditPanel(MMDApp* app);                       // VA 0x004134E0 (ui_frame_refresh.cpp)
void RegisterAccessoryKey(MMDApp* app, int frame, int slot);   // VA 0x00413CB0 (render/accessory.cpp)
void RegisterCameraState(MMDApp* app, int frame);  // VA 0x00410560, was Sub410560 (command_control_400.cpp)
void RegisterLightState(MMDApp* app, int frame);   // VA 0x00411630, was Sub411630 (command_control_400.cpp)
void CommitEditControl(MMDApp* app, HWND hwnd);            // VA 0x00463640 (ui_edit_commit.cpp)
void DeleteAccessory(mdl::AccessoryRecord* accessory, int flag);  // VA 0x0040A6F0, was Sub40A6F0 (render/accessory.cpp)
void IdentityCtor(void* obj);                         // VA 0x004C46F0 (stubs.cpp; original is `return this`)
void* ConstructArrayElements(void* block, std::uint32_t elementSize,
                             std::uint32_t count,
                             void* ctor);  // VA 0x00401150, was Sub401150 (accessory_paste.cpp)

// MMS addition (no original VA): the merged select / box-select cycle of
// control 490, defined in localize_ui.cpp next to the caption table.
ViewportEditMode NextBoneSelectMode(ViewportEditMode current);

void CmdControl450(MMDApp* app, HWND hwnd, std::uint16_t id,
                   std::uint16_t notify) {
    // These bone operations may arrive from the frame-loop shortcut path,
    // even when there is no loaded/selected model. Guard before undo or UI
    // side effects, not just before the first dereference in case 494.
    if (id >= 494 && id <= 498 &&
        (app->SelectedModelSlot() >= kModelSlotCount || ActiveModel(app) == nullptr))
        return;
    (void)notify;  // see header: the default handler's notify gate can never
                   // match any id of this family
    switch (id) {
    // ------------------------------------------------------------------
    // 451 (0x00486431): camera reset - position/rotation floats cleared,
    // camera angle gated on app+0xA0430 (camera-cluster flag, default -1).
    // ------------------------------------------------------------------
    case 451: {
        app->CameraPositionX() = 0.0f;
        app->CameraPositionZ() = 0.0f;
        app->CameraPitch() = 0.0f;
        app->CameraYaw() = 0.0f;
        app->CameraRoll() = 0.0f;
        if (app->CameraParentModel() < 0) {
            app->CameraPositionY() = 10.0f;
            app->CameraDistance() = -45.0f;
        } else {
            app->CameraPositionY() = 0.0f;
            app->CameraDistance() = 0.0f;
        }
        RefreshRequest(-1);
        PostViewRefresh(app);
        break;
    }

    // ------------------------------------------------------------------
    // 453 / 454 (0x0047E99F / 0x0047E9C8): UI option flag byte 0x2F9 =
    // 0 / 1, then PostModelReload2 + HandleWindowSize + InvalidateRect.
    // 469 / 470 (0x0047E9F1 / 0x0047EA06): flag byte 0x2FA, same refresh
    // chain.  488 / 489 (0x0047EA2F / 0x0047EA58): flag byte 0x2FB, same
    // refresh chain.  Table-driven: the six bodies are identical except
    // for the flag index and the value stored.
    // ------------------------------------------------------------------
    case 453:
    case 454:
    case 469:
    case 470:
    case 488:
    case 489: {
        static const struct {
            int id;
            int flagIdx;
            unsigned char value;  // optflag is unsigned char[7]
        } kOptFlagRadios[] = {
            {453, 1, 0}, {454, 1, 1},  // 0x2F9
            {469, 2, 0}, {470, 2, 1},  // 0x2FA
            {488, 3, 0}, {489, 3, 1},  // 0x2FB
        };
        for (const auto& radio : kOptFlagRadios) {
            if (radio.id == id) {
                app->state.optflag[radio.flagIdx] = radio.value;
                break;
            }
        }
        PostModelReload2(app);
        HandleWindowSize(app);
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }

    // ------------------------------------------------------------------
    // 452 (0x0048681D): clear frame selection; RefreshRequest(-1),
    // RegisterCameraState(app, app+0x980), PanelPaint, SelectionReeval.
    // ------------------------------------------------------------------
    case 452: {
        app->SceneModified() = 1;
        ClearGlobalFrameSelection(app);
        RefreshRequest(-1);
        RegisterCameraState(app, app->state.currentFrame);
        PanelPaint(app);
        SelectionReeval(app);
        break;
    }

    // ------------------------------------------------------------------
    // 467 (0x0048648F): accessory reset - position (-0.5,-1.0,0.5),
    // pose blob memset + kind 3, color 0.6, scene vtable 0xCC/0xD4,
    // %3d / %+3.1f echoes into 461-466, TBM_SETPOS 455-460, Refresh(-2).
    // ------------------------------------------------------------------
    case 467: {
        float* const direction = app->LightDirection();
        direction[0] = -0.5f;
        direction[1] = -1.0f;
        direction[2] = 0.5f;
        D3DLIGHT9& light = app->SceneLight();
        light = {};
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Direction = {direction[0], direction[1], direction[2]};
        light.Diffuse = {};
        // 0.602f is bit-exact with the x64 immediate 0x3F1A1CAC
        // (0x7FF7CB46A492..0x7FF7CB46A4BC, written to all three light
        // channels and copied into Specular); same constant as the
        // light-key default in command_control_400.cpp ResetLightRecord.
        const float col = 0.602f;  // flt_52C9A4
        app->LightColor()[0] = col;
        app->LightColor()[1] = col;
        app->LightColor()[2] = col;
        light.Ambient = {col, col, col, 0.0f};
        light.Specular = {col, col, col, 0.0f};
        ApplySceneLight(app);
        EnableSceneLight(app);

        char buf[256];
        for (int ch = 0; ch < 3; ++ch) {
            sprintf_s(buf, 0x100, "%3d",
                      static_cast<int>(static_cast<double>(
                                           app->LightColor()[ch]) *
                                       256.0));  // dbl_52B9E8
            SetWindowTextA(GetDlgItem(hwnd, 0x1CD + ch), buf);  // 461..463
        }
        for (int ax = 0; ax < 3; ++ax) {
            sprintf_s(buf, 0x100, "%+3.1f",
                      static_cast<double>(direction[ax]));
            SetWindowTextA(GetDlgItem(hwnd, 0x1D0 + ax), buf);  // 464..466
        }
        for (int ch = 0; ch < 3; ++ch) {
            SendMessageA(GetDlgItem(hwnd, panel::kLightColorSliderR + ch), TBM_SETPOS,
                         1, static_cast<LPARAM>(static_cast<int>(
                                static_cast<double>(
                                    app->LightColor()[ch]) *
                                256.0)));  // sliders 455..457
        }
        for (int ax = 0; ax < 3; ++ax) {
            SendMessageA(GetDlgItem(hwnd, panel::kLightDirSliderX + ax), TBM_SETPOS,
                         1, static_cast<LPARAM>(static_cast<int>(
                                static_cast<double>(
                                    direction[ax]) *
                                100.0)));  // sliders 458..460, dbl_52B8E0
        }
        RefreshRequest(-2);
        break;
    }

    // ------------------------------------------------------------------
    // 468 (0x004868C2): clear frame selection (like 452); Refresh(-2),
    // RegisterLightState(app, app+0x980), PanelPaint.
    // ------------------------------------------------------------------
    case 468: {
        app->SceneModified() = 1;
        ClearGlobalFrameSelection(app);
        RefreshRequest(-2);
        RegisterLightState(app, app->state.currentFrame);
        PanelPaint(app);
        break;
    }

    // ------------------------------------------------------------------
    // 472 (0x00486A59): load accessory via OPENFILENAMEW; initial dir
    // gated on menu item 0x12D, EN/JP filter + title, defext "x";
    // dir-copy into app+0xA1D10 when the menu gate is set, then
    // LoadAccessoryFile(app, path).
    // ------------------------------------------------------------------
    case 472: {
        SetCurrentDirectoryW(app->ExeDir());  // 0xA06CE
        wchar_t fileBuf[0x100];
        swprintf_s(fileBuf, 0x100, kFmt529688, L"", L"");
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = app->state.floatingWindow != 0
                            ? reinterpret_cast<HWND>(
                                  app->state.floatingWindow)
                            : hwnd;
        if (app->state.englishUI != 0) {
            ofn.lpstrFilter = L"accessory files(*.x,*.vac)\0*.x;*.vac\0";
            ofn.lpstrTitle = L"open file";
        } else {
            ofn.lpstrFilter = kFilterAccJp;
            ofn.lpstrTitle = kTitleOpenJp;
        }
        if ((GetMenuState(GetMenu(hwnd), 0x12D, 0) & 8) != 0) {
            ofn.lpstrInitialDir = app->DirAccs();  // 0xA1D10
        } else {
            ofn.lpstrInitialDir = L"UserFile\\Accessory";
        }
        ofn.lpstrDefExt = L"x";
        wchar_t fileTitle[0x100];
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = 0x100;
        ofn.lpstrFileTitle = fileTitle;
        ofn.nMaxFileTitle = 0x100;
        ofn.Flags = OFN_FILEMUSTEXIST;
        if (!GetUiOpenFileNameW(&ofn)) {
            break;
        }
        if ((GetMenuState(GetMenu(hwnd), 0x12D, 0) & 8) != 0) {
            // ExtractDirFromPath(app+0x9F338, fileBuf) then copy into DirAccs
            wchar_t* dir =
                ExtractDirFromPath(app->PathWorkspace().projectDirectory,
                                   fileBuf, kPathFieldCapacity);
            CopyDirPathW(app->DirAccs(), dir);
        }
        // original thiscall: LoadAccessoryFile(app, fileBuf)
        LoadAccessoryFile(fileBuf);
        break;
    }

    // ------------------------------------------------------------------
    // 473 (0x00486BA7): delete accessory selected in combo 0x1D7.
    // CB_GETCURSEL(0x1D7) -> find slot (table 0x9DD70, id byte acc+0x49D);
    // confirm box (EN/JP, MB_OKCANCEL + 0x40000 when app+0xA0D38 != 0);
    // dispose slot blob, allocate fresh 0x927C0 blob, fix up indices,
    // rebuild combos 0x1D7/0x1DA/0x1DB, clear checkbox labels 0x1DE..0x1E4,
    // EnableMenuItem 0xF9, RebuildCameraModePanel, Refresh(-1), PostLanguageSweep2.
    // ------------------------------------------------------------------
    case 473: {
        const LRESULT sel =
            SendMessageA(GetDlgItem(hwnd, panel::kAccessoryCombo), CB_GETCURSEL, 0, 0);
        std::int32_t found = -1;
        for (int i = 0; i < 0xFF; ++i) {
            mdl::AccessoryRecord* acc = app->AccessorySlot(i);
            if (acc != nullptr &&
                static_cast<int>(acc->order) == static_cast<int>(sel)) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            break;
        }
        app->state.enterKeyState = 1;  // 0xBC
        mdl::AccessoryRecord* acc = app->AccessorySlot(found);

        const auto name = text_encoding::Display(acc->name);
        const std::uint32_t flags = app->state.floatingWindow != 0
            ? (MB_OKCANCEL | MB_TOPMOST) : MB_OKCANCEL;
        if (ShowUiMessage(app, hwnd, UiMessage::DeleteAccessory, flags, name.c_str()) != IDOK) {
            break;
        }

        const std::int32_t oldSel = acc->order;
        // dispose the accessory blob
        if (acc != nullptr) {
            DeleteAccessory(acc, 1);
        }
        curve_host::ForgetAccessory(app, found);
        app->AccessorySlot(found) = nullptr;
        // dispose the previous frame blob of this slot
        if (app->AccessoryKeys(found) != nullptr) {
            mdl::KeyPoolDestroy(app->AccessoryKeys(found));
            app->AccessoryKeys(found) = nullptr;
        }
        // fresh 0x927C0 frame blob with an identity frame seeded
        app->SceneModified() = 1;
        auto* keys = mdl::KeyPoolCreate<mdl::AccessoryKey>();
        app->AccessoryKeys(found) = keys;
        std::memset(keys, 0, sizeof(mdl::AccessoryKey) * mdl::kTimelineKeyCapacity);
        keys[0].visible = 1;
        keys[0].parentModel = -1;
        keys[0].scale = 1.0f;
        keys[0].opacity = 1.0f;
        // combo rebuild: 0x1D7 select = oldSel, 0x1B2 index shifted
        SendMessageA(GetDlgItem(hwnd, panel::kAccessoryCombo), CB_DELETESTRING,
                     static_cast<WPARAM>(sel), 0);
        SendMessageA(GetDlgItem(hwnd, panel::kRegisterScopeCombo), CB_DELETESTRING,
                     static_cast<WPARAM>(sel + 4), 0);
        // fix indices above the deleted one
        for (int i = 0; i < 0xFF; ++i) {
            mdl::AccessoryRecord* p = app->AccessorySlot(i);
            if (p != nullptr && static_cast<int>(p->order) > oldSel) {
                --p->order;
            }
        }
        // Choose the last displayed accessory, independently of reused storage slots.
        std::int32_t lastSlot = -1;
        int lastOrder = -1;
        for (int i = 0; i < 0xFF; ++i) {
            mdl::AccessoryRecord* p = app->AccessorySlot(i);
            if (p != nullptr && p->order > lastOrder) {
                lastSlot = i;
                lastOrder = p->order;
            }
        }
        SendMessageA(GetDlgItem(hwnd, panel::kAccessoryCombo), CB_SETCURSEL, lastOrder, 0);
        if (lastSlot < 0) {
            app->SelectedAccessorySlot() = 0;
            SendMessageA(GetDlgItem(hwnd, panel::kMainComboGround), CB_SETCURSEL, -1, 0);
        } else {
            app->SelectedAccessorySlot() = static_cast<std::uint8_t>(lastSlot);
        }
        SendMessageA(GetDlgItem(hwnd, panel::kAttachBoneCombo), CB_RESETCONTENT, 0, 0);
        // clear the checkbox labels 0x1DE..0x1E4
        char empty[0x100];
        strcpy_s(empty, 0x100, "");
        for (int ctl = 0x1DE; ctl <= 0x1E4; ++ctl) {
            HWND edit = GetDlgItem(hwnd, ctl);
            SendMessageA(edit, EM_SETSEL, 0,
                         static_cast<LPARAM>(GetWindowTextLengthA(edit)));
            SendMessageA(edit, EM_REPLACESEL, 0,
                         reinterpret_cast<LPARAM>(empty));
        }
        if (oldSel < app->AccessoryRenderSplitOrder())
            --app->AccessoryRenderSplitOrder();
        EnableMenuItem(GetMenu(hwnd), 0xF9, MF_BYCOMMAND |
            (lastSlot < 0 ? MF_GRAYED : MF_ENABLED));
        RebuildCameraModePanel(app);
        SendMessageA(GetDlgItem(hwnd, panel::kInterpCurveCombo), CB_DELETESTRING, 5, 0);
        SendMessageA(GetDlgItem(hwnd, panel::kInterpCurveCombo), CB_DELETESTRING, 4, 0);
        PostModelReload2(app);
        app->GlobalTrackSelected(GlobalTimelineTrack::Camera) = 0;
        RefreshRequest(-1);
        InvalidateRect(hwnd, nullptr, FALSE);
        PostLanguageSweep2(app);
        if (lastSlot >= 0) {
            // Use the normal selection notification to synchronize the timeline
            // highlight, parent controls and transform fields with the new target.
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(panel::kAccessoryCombo, CBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(GetDlgItem(hwnd, panel::kAccessoryCombo)));
        }
        break;
    }

    // ------------------------------------------------------------------
    // 476 (0x00486F91): toggle accessory display flag byte acc+0x210,
    // BM_SETCHECK into checkbox 0x1DC, Refresh(byte 0x9E170).
    // ------------------------------------------------------------------
    case 476: {
        const std::uint8_t idx = app->state.selectedObjectSlot;
        mdl::AccessoryRecord* acc = app->AccessorySlot(idx);
        if (acc == nullptr) {
            break;
        }
        if (acc->visible != 0) {
            acc->visible = 0;
            SendMessageA(GetDlgItem(hwnd, panel::kAccessoryVisibleCheckbox), BM_SETCHECK, 0, 0);
        } else {
            acc->visible = 1;
            SendMessageA(GetDlgItem(hwnd, panel::kAccessoryVisibleCheckbox), BM_SETCHECK, 1, 0);
        }
        RefreshRequest(idx);
        break;
    }

    // ------------------------------------------------------------------
    // 477 (0x0048A199): toggle accessory flag byte acc+0x49E; the case
    // then falls into def_47E903, which is a no-op for this id (see the
    // default note) - equivalent to break here.
    // ------------------------------------------------------------------
    case 477: {
        const std::uint8_t idx = app->state.selectedObjectSlot;
        mdl::AccessoryRecord* acc = app->AccessorySlot(idx);
        if (acc == nullptr) {
            break;
        }
        acc->additiveBlend = acc->additiveBlend == 0 ? 1 : 0;
        break;
    }

    // ------------------------------------------------------------------
    // 486 (0x00487011): toggle accessory flag byte acc+0x49C,
    // BM_SETCHECK into checkbox 0x1E6, Refresh(byte 0x9E170).
    // ------------------------------------------------------------------
    case 486: {
        const std::uint8_t idx = app->state.selectedObjectSlot;
        mdl::AccessoryRecord* acc = app->AccessorySlot(idx);
        if (acc == nullptr) {
            break;
        }
        if (acc->shadowEnabled != 0) {
            acc->shadowEnabled = 0;
            SendMessageA(GetDlgItem(hwnd, panel::kAccessoryShadowCheckbox), BM_SETCHECK, 0, 0);
        } else {
            acc->shadowEnabled = 1;
            SendMessageA(GetDlgItem(hwnd, panel::kAccessoryShadowCheckbox), BM_SETCHECK, 1, 0);
        }
        RefreshRequest(idx);
        break;
    }

    // ------------------------------------------------------------------
    // 487 (0x0048696B): sync the per-accessory checkboxes 0x1DE..0x1E4
    // (CommitEditControl), clear the frame selection, then Refresh(byte 0x9E170)
    // + RegisterAccessoryKey(app, app+0x980, byte 0x9E170) + PanelPaint.
    // ------------------------------------------------------------------
    case 487: {
        for (int ctl = 0x1DE; ctl <= 0x1E4; ++ctl) {
            CommitEditControl(app, GetDlgItem(hwnd, ctl));
        }
        app->SceneModified() = 1;
        const std::uint8_t idx = app->state.selectedObjectSlot;
        if (app->AccessorySlot(idx) == nullptr) {
            break;
        }
        app->SceneModified() = 1;
        ClearGlobalFrameSelection(app);
        RefreshRequest(idx);
        RegisterAccessoryKey(app, app->state.currentFrame, idx);
        PanelPaint(app);
        break;
    }

    // ------------------------------------------------------------------
    // 490..493 (0x0047ECD6 / 0x0047ED57 / 0x0047EDDA / 0x0047EE5E):
    // mode buttons 0x1EA..0x1ED.  The clicked button stays checked; the other
    // three are unchecked and app+0x914 stores the mode
    // (0/1/3/4 when the corresponding button is checked, 2 when none).
    //
    // MMS change: 490 and 491 are no longer two independent radios.  490 is the
    // merged select / box-select cycle that the model panel shows in the first
    // cell of its mode row, and 491 is the bone scale mode.  RefreshBoneModePanel
    // writes both check states and the merged caption from app+0x914 afterwards,
    // so the four cases below only store the mode.  The two programmatic
    // re-dispatches of WM_COMMAND 0x1EA (this file's case 494 and
    // command_control_500.cpp's case 501) now set the mode directly, which is
    // what lets the cycle key on the stored mode instead of the check state.
    // ------------------------------------------------------------------
    case 490: {
        // select <-> none.  Box selection is Shift + drag inside select mode
        // (frame_modes.cpp), so the button no longer cycles through it.
        app->EditMode() = NextBoneSelectMode(app->EditMode());
        RefreshBoneModePanel(app);
        break;
    }

    case 491: {
#if MMD_ORIGINAL_PROFILE
        app->EditMode() = IsDlgButtonChecked(hwnd, panel::kBoxSelectRadio) != 0
            ? ViewportEditMode::BoneBox : ViewportEditMode::None;
#else
        // MMS addition: bone scale mode.  The viewport draws the scale handles
        // and bone_scale::Enabled gates them out of the camera/accessory panel,
        // during playback and without a valid bone selection.
        app->EditMode() = IsDlgButtonChecked(hwnd, panel::kBoxSelectRadio) != 0
            ? ViewportEditMode::Scale : ViewportEditMode::None;
#endif
        RefreshBoneModePanel(app);
        break;
    }

    case 492: {
        app->EditMode() = IsDlgButtonChecked(hwnd, panel::kBoneMoveRadio) != 0
            ? ViewportEditMode::Light : ViewportEditMode::None;
        RefreshBoneModePanel(app);
        break;
    }

    case 493: {
        app->EditMode() = IsDlgButtonChecked(hwnd, panel::kBoneRotateRadio) != 0
            ? ViewportEditMode::Camera : ViewportEditMode::None;
        RefreshBoneModePanel(app);
        break;
    }

    // ------------------------------------------------------------------
    // 494 (0x0047FB24): select all bones.  A bone is selected when
    // (flag word +0x1F4 has bit 0x400 and type byte +0x1E4 == 4) or
    // type == 8 or type <= 6; then store select mode (was: check radio 0x1EA
    // and re-dispatch WM_COMMAND(0x1EA)) and run PostLanguageSweep +
    // PostLanguageSweep2.
    // ------------------------------------------------------------------
    case 494: {
        unsigned char* model = ActiveModel(app);
        auto* const modelRecord = mikumikustudio::mdl::Mdl(model);
        const std::int32_t boneCount = modelRecord->boneCount;
        mikumikustudio::mdl::BoneRecord* const bones = modelRecord->boneTable;
        unsigned char* const sel = modelRecord->boneSelection;
        if (boneCount > 0 && (bones == nullptr || sel == nullptr))
            break;
        for (std::int32_t i = 0; i < boneCount; ++i) {
            const mdl::BoneType type = bones[i].type;
            const std::uint16_t flag = bones[i].flags;
            const bool hit =
                ((flag & mdl::kBoneFlagFixedAxis) == mdl::kBoneFlagFixedAxis &&
                 type == mdl::BoneType::UnderIk) ||
                type == mdl::BoneType::FixedAxis ||
                type <= mdl::BoneType::Effector;
            if (hit) {
                sel[i] = 1;
            }
        }
        // MMS change: the original checked radio 0x1EA and re-dispatched
        // WM_COMMAND 0x1EA.  Control 490 is now the merged select /
        // box-select cycle, so that re-dispatch would advance the cycle (a user
        // in box-select mode would land on "none" after selecting all).  Store
        // the mode and let RefreshBoneModePanel write the buttons instead.
        app->EditMode() = ViewportEditMode::Bone;
        RefreshBoneModePanel(app);
        PostLanguageSweep(app);
        PostLanguageSweep2(app);
        break;
    }

    // ------------------------------------------------------------------
    // Reset selected poses without registering or replacing their keyframes.
    // ------------------------------------------------------------------
    case 495: {
        auto* model=ActiveModel(app);
        auto* record=mdl::Mdl(model);
        if(!record || record->boneCount<=0 || !record->boneTable ||
           !record->boneSelection || !record->bonePhysicsState)break;
        const HWND owner=app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow();
        for(int id=panel::kReadoutPosXEdit;id<=panel::kReadoutRotZEdit;++id)
            SendDlgItemMessageA(owner,id,EM_SETMODIFY,FALSE,0);
        PushBoneEditUndo(app);
        for(int i=0;i<record->boneCount;++i) {
            if(!record->boneSelection[i])continue;
            auto& bone=record->boneTable[i];
            for(float& value:bone.trans)value=0;
            for(int axis=0;axis<3;++axis)bone.rotQuat[axis]=0;
            bone.rotQuat[3]=1;
            bone_scale::SetPose(model,i,{});
            record->bonePhysicsState[i]=1;
        }
        app->SceneModified()=1;
        PostViewRefresh(app);
        break;
    }

    // ------------------------------------------------------------------
    // 496 (0x00481926): bone copy - count the selected bones, enable the
    // paste controls 0x1F1/0x1F2, snapshot each selected bone's name,
    // stored pos (0x140..0x148) and stored quat (0x14C..0x158) into a
    // 0x30-stride record array at app+0x350 (name[20], pos[3], quat[4]).
    // ------------------------------------------------------------------
    case 496: {
        unsigned char* model = ActiveModel(app);
        auto* modelRecord = mikumikustudio::mdl::Mdl(model);
        const std::int32_t boneCount = modelRecord->boneCount;
        std::int32_t count = 0;
        if (boneCount > 0) {
            unsigned char* sel = modelRecord->boneSelection;
            for (std::int32_t i = 0; i < boneCount; ++i) {
                if (sel[i] != 0) {
                    ++count;
                }
            }
        }
        if (count == 0) {
            break;  // jz def_47E903 (no-op)
        }
        EnableWindow(GetDlgItem(hwnd, panel::kBonePasteButton), TRUE);
        EnableWindow(GetDlgItem(hwnd, panel::kBoneReversePasteButton), TRUE);
        app->state.copiedBoneCount = count;
        auto& records =
            reinterpret_cast<BoneCopyRecord*&>(app->BoneCopyRecords());
        if (records != nullptr) {
            free(records);
            records = nullptr;
        }
        auto* block = static_cast<BoneCopyRecord*>(::operator new(
            MulOrMax(static_cast<std::uint32_t>(count),
                     sizeof(BoneCopyRecord))));
        if (block != nullptr) {
            ConstructArrayElements(block, sizeof(BoneCopyRecord),
                                   static_cast<std::uint32_t>(count),
                                   &IdentityCtor);
        }
        records = block;
        memset(block, 0,
               static_cast<std::size_t>(count) * sizeof(BoneCopyRecord));
        app->state.copiedBoneCount = 0;  // fill cursor
        model = ActiveModel(app);
        modelRecord = mikumikustudio::mdl::Mdl(model);
        const std::int32_t nBones = modelRecord->boneCount;
        mikumikustudio::mdl::BoneRecord* const bones = modelRecord->boneTable;
        unsigned char* sel = modelRecord->boneSelection;
        std::int32_t cursor = 0;
        for (std::int32_t i = 0; i < nBones; ++i) {
            if (sel[i] == 0) {
                continue;
            }
            bone_scale::CopyClipboard(app, cursor, bone_scale::Pose(model, i), true);
            BoneCopyRecord& rec = records[cursor];
            strcpy_s(rec.name, bones[i].name);
            memcpy(rec.position, bones[i].trans, sizeof rec.position);
            memcpy(rec.rotation, bones[i].rotQuat, sizeof rec.rotation);
            ++cursor;
            // 0x481B1E updates the shared copy count after every emitted
            // record.  Paste (497/498) gates directly on this field.
            app->state.copiedBoneCount = cursor;
        }
        break;
    }

    // ------------------------------------------------------------------
    // 497 (0x00481B4B): bone paste - clear the selection, bump the paste
    // counter (model+0x31B4, wraps at 0x1E), allocate a 0x24-stride undo
    // record blob (model+0x26FC, 28-byte stride per paste step) and paste
    // each copied bone by exact name match: undo data (old pos/quat) into
    // the record, new values into the bone frames, selection + frame
    // flags, current bone = pasted index, then the language sweeps.
    // ------------------------------------------------------------------
    case 497: {
        if (app->state.copiedBoneCount == 0) {
            break;  // jz def_47E903 (no-op)
        }
        unsigned char* model = ActiveModel(app);
        auto* modelRecord = mikumikustudio::mdl::Mdl(model);
        const std::int32_t boneCount = modelRecord->boneCount;
        unsigned char* sel = modelRecord->boneSelection;
        for (std::int32_t i = 0; i < boneCount; ++i) {
            sel[i] = 0;
        }
        EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
        EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
        modelRecord->undoDirty = 1;
        modelRecord->redoDirty = 0;
        ++modelRecord->undoState[0];
        if (modelRecord->undoState[0] >= 0x1E) {
            modelRecord->undoState[0] = 0;
        }
        const std::int32_t pasteIdx = modelRecord->undoState[0];
        modelRecord->undoState[1] = pasteIdx;
        bone_scale::CaptureUndo(model);
        auto& undo = modelRecord->undoRings[0].slots[pasteIdx];
        undo.operation = 1;
        // count stored 28*(pasteIdx+0x164) dwords past the undo-table base
        undo.dirty = 0;
        unsigned char* pasteBlob =
            reinterpret_cast<unsigned char*>(undo.bonePose);
        if (pasteBlob != nullptr) {
            free(pasteBlob);
            undo.bonePose = nullptr;
        }
        const std::int32_t count =
            app->state.copiedBoneCount;
        pasteBlob = static_cast<unsigned char*>(::operator new(
            MulOrMax(static_cast<std::uint32_t>(count), 0x24u)));
        if (pasteBlob != nullptr) {
            ConstructArrayElements(pasteBlob, 0x24,
                                   static_cast<std::uint32_t>(count),
                                   &IdentityCtor);
        }
        undo.bonePose = reinterpret_cast<mikumikustudio::mdl::BonePoseSnapshot*>(
            pasteBlob);
        memset(pasteBlob, 0, static_cast<std::size_t>(count) * 0x24u);
        std::vector<unsigned char> captured(static_cast<std::size_t>(boneCount), 0);

        auto& records =
            reinterpret_cast<BoneCopyRecord*&>(app->BoneCopyRecords());
        for (std::int32_t j = 0; j < count; ++j) {
            model = ActiveModel(app);
            modelRecord = mikumikustudio::mdl::Mdl(model);
            const std::int32_t nBones = modelRecord->boneCount;
            sel = modelRecord->boneSelection;
            mikumikustudio::mdl::BoneRecord* const bones = modelRecord->boneTable;
            unsigned char* frameFlag = modelRecord->bonePhysicsState;
            const BoneCopyRecord& src = records[j];
            // exact name match (the original's inline 2-byte-step compare
            // is byte-identical to strcmp)
            std::int32_t found = -1;
            for (std::int32_t k = 0; k < nBones; ++k) {
                if (strcmp(bones[k].name, src.name) == 0) {
                    found = k;
                    break;
                }
            }
            if (found < 0) {
                continue;
            }
            mikumikustudio::mdl::BoneRecord* bone = &bones[found];
            sel[found] = 1;
            if (!captured[found]) {
                captured[found] = 1;
                auto& rec = undo.bonePose[undo.dirty++];
                rec.boneIndex = found;
                memcpy(rec.position, bone->trans, sizeof rec.position);
                memcpy(rec.rotation, bone->rotQuat, sizeof rec.rotation);
                rec.physicsDisabled = frameFlag[found];
            }
            // new pos from the copy record
            memcpy(bone->trans, src.position, sizeof bone->trans);
            // old quat into the undo record, new quat into the bone
            memcpy(bone->rotQuat, src.rotation, sizeof bone->rotQuat);
            bone_scale::SetPose(model, found, bone_scale::Clipboard(app, j, true));
            frameFlag[found] = 1;
            modelRecord->selectedBone = found;
            PostLanguageSweep(app);
            PostLanguageSweep2(app);
        }
        SetFocus(hwnd);
        app->PhysicsResetPending() = 1;
        break;
    }

    // ------------------------------------------------------------------
    // 498 (0x0048203D): bone paste, left/right mirror variant (x64 body
    // 0x14004425E, verified 2026-09-07 - was misnamed "delete-tag").  Same
    // undo-record bookkeeping as 497; the paste target is the opposite-side
    // bone resolved through the 左/右 name tags (kNeedle530BFC/kNeedle530BF8)
    // instead of a plain name match, and the pasted transform is mirrored
    // (X position and quat Y/Z negated; x64 uses -0.0f from the 0x140132B80
    // pool).  When both tag searches fail, the exact-name match index (k1)
    // is used as the fallback target.
    // ------------------------------------------------------------------
    case 498: {
        if (app->state.copiedBoneCount == 0) {
            break;  // jz def_47E903 (no-op)
        }
        unsigned char* model = ActiveModel(app);
        auto* modelRecord = mikumikustudio::mdl::Mdl(model);
        const std::int32_t boneCount = modelRecord->boneCount;
        unsigned char* sel = modelRecord->boneSelection;
        for (std::int32_t i = 0; i < boneCount; ++i) {
            sel[i] = 0;
        }
        EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
        EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
        modelRecord->undoDirty = 1;
        modelRecord->redoDirty = 0;
        ++modelRecord->undoState[0];
        if (modelRecord->undoState[0] >= 0x1E) {
            modelRecord->undoState[0] = 0;
        }
        const std::int32_t pasteIdx = modelRecord->undoState[0];
        modelRecord->undoState[1] = pasteIdx;
        bone_scale::CaptureUndo(model);
        auto& undo = modelRecord->undoRings[0].slots[pasteIdx];
        undo.operation = 1;
        undo.dirty = 0;
        unsigned char* pasteBlob =
            reinterpret_cast<unsigned char*>(undo.bonePose);
        if (pasteBlob != nullptr) {
            free(pasteBlob);
            undo.bonePose = nullptr;
        }
        const std::int32_t count =
            app->state.copiedBoneCount;
        pasteBlob = static_cast<unsigned char*>(::operator new(
            MulOrMax(static_cast<std::uint32_t>(count), 0x24u)));
        if (pasteBlob != nullptr) {
            ConstructArrayElements(pasteBlob, 0x24,
                                   static_cast<std::uint32_t>(count),
                                   &IdentityCtor);
        }
        undo.bonePose = reinterpret_cast<mikumikustudio::mdl::BonePoseSnapshot*>(
            pasteBlob);
        memset(pasteBlob, 0, static_cast<std::size_t>(count) * 0x24u);
        std::vector<unsigned char> captured(static_cast<std::size_t>(boneCount), 0);

        auto& records =
            reinterpret_cast<BoneCopyRecord*&>(app->BoneCopyRecords());
        for (std::int32_t j = 0; j < count; ++j) {
            model = ActiveModel(app);
            modelRecord = mikumikustudio::mdl::Mdl(model);
            const std::int32_t nBones = modelRecord->boneCount;
            sel = modelRecord->boneSelection;
            mikumikustudio::mdl::BoneRecord* const bones = modelRecord->boneTable;
            unsigned char* frameFlag = modelRecord->bonePhysicsState;
            const BoneCopyRecord& src = records[j];
            // step 1: exact name match -> k1
            std::int32_t k1 = -1;
            for (std::int32_t k = 0; k < nBones; ++k) {
                if (strcmp(bones[k].name, src.name) == 0) {
                    k1 = k;
                    break;
                }
            }
            if (k1 < 0) {
                continue;  // no exact-name bone: record skipped
            }
            std::int32_t target = -1;
            const char* tag1 = strstr(bones[k1].name, kNeedle530BFC);
            if (tag1 != nullptr) {
                // pass 1 (x64 0x14004456C..0x140044617): k1 carries the 左
                // tag; find the 右-tagged bone whose suffix matches
                const char* suffix = tag1 + 2;
                for (std::int32_t k = 0; k < nBones; ++k) {
                    const char* q = strstr(bones[k].name, kNeedle530BF8);
                    if (q != nullptr && strcmp(q + 2, suffix) == 0) {
                        target = k;
                        break;
                    }
                }
            }
            if (target < 0) {
                // pass 2 (x64 0x14004461D..0x1400446D9): k1 carries the 右
                // tag; find the 左-tagged bone whose suffix matches.  (The
                // old x86-derived raw-offset search from (char*)bones + j
                // does not exist in the x64 image.)
                const char* suffix = strstr(bones[k1].name, kNeedle530BF8);
                if (suffix != nullptr) {
                    suffix += 2;
                    for (std::int32_t k = 0; k < nBones; ++k) {
                        const char* q = strstr(bones[k].name, kNeedle530BFC);
                        if (q != nullptr && strcmp(q + 2, suffix) == 0) {
                            target = k;
                            break;
                        }
                    }
                }
            }
            if (target < 0) {
                target = k1;  // fallback to the exact-name match
            }
            mikumikustudio::mdl::BoneRecord* bone = &bones[target];
            sel[target] = 1;
            if (!captured[target]) {
                captured[target] = 1;
                auto& rec = undo.bonePose[undo.dirty++];
                rec.boneIndex = target;
                memcpy(rec.position, bone->trans, sizeof rec.position);
                memcpy(rec.rotation, bone->rotQuat, sizeof rec.rotation);
                rec.physicsDisabled = frameFlag[target];
            }
            // new pos from the copy record - X negated (fchs)
            bone->trans[0] = -src.position[0];
            bone->trans[1] = src.position[1];
            bone->trans[2] = src.position[2];
            // new quat from the copy record - Y and Z negated (fchs)
            bone->rotQuat[0] = src.rotation[0];
            bone->rotQuat[1] = -src.rotation[1];
            bone->rotQuat[2] = -src.rotation[2];
            bone->rotQuat[3] = src.rotation[3];
            bone_scale::SetPose(model, static_cast<int>(bone - bones), bone_scale::Clipboard(app, j, true, true));
            frameFlag[target] = 1;
            modelRecord->selectedBone = target;
            PostLanguageSweep(app);
            PostLanguageSweep2(app);
        }
        SetFocus(hwnd);
        app->PhysicsResetPending() = 1;
        break;
    }

    default:
        // def_47E903 (0x482897): only reachable actions are gated on
        // notify==BN_CLICKED and the sending control being 0x1B4/0x1BB..., a
        // compare chain that can never match ids 450/452/455-466/471/474/
        // 475/478-485/499 - no-op for the whole family.
        break;
    }
}

}  // namespace mikumikustudio
