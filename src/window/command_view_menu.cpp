#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/original_import.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/recording_state.hpp"
#include "../app/recording_ack_wait.hpp"
#include "../plugins/native_model_physics.hpp"
#include "../plugins/native_floor_history.hpp"
// ===========================================================================
// CommandDispatch family: View/Edit option-menu cases 251..303 (0x47E8A0)
// ===========================================================================
// Cases 251..302 (0xFB..0x12E) of the 368-case switch in CommandDispatch
// (0x47E8A0): display/physics option toggles (byte-flag flips), menu check
// sync (CheckMenuItem) and enable state (EnableMenuItem), refresh tails
// (0x42C810 / 0x40CAC0 / 0x414610 PanelPaint), modal editor dialogs
// (DialogBoxParamA / CreateDialogParamA), the bone/camera rotation dialogs
// (case 300) and the render-to-picture / save-enhanced-model / open-oni
// file dialogs (cases 276/263/296).  Case 278 reloads the toon texture set
// (0x424DC0, InitToonTextures).
//
// Calling convention of the original (sub_47E8A0): thiscall, this = app
// (ebp = ecx at 0x47E8F4); arg_0 = sending control HWND (only used by the
// default handler's notify==1 compare chain, which can never match a menu
// id); arg_4 = wParam (id = LOWORD).  All `hwnd` uses in this family read
// app+0xA06B8 (the main window), i.e. the `hwnd` parameter of the port.
//
// Case map (id -> behaviour -> original VA).  Index byte_48F650[id-0xC8]
// selects the jump-table entry jpt_47E903 (0x48F30C); targets below are the
// `jumptable 0047E903 case N` labels confirmed by IDA:
//
//   id  hex  VA            behaviour
//   --- ---  ------------  ---------------------------------------------------
//   251 0FB  0x0048A1CC    gate app+0x2F8: return if set; 0xBC=1; modal
//                          "frame control" dialog (0x294 EN / 0x27C JP,
//                          sub_44D3F0); return when it closes with 2,
//                          else default (no-op)
//   252 0FC  0x0048A219    same gate/flag; dialog 0x295 EN / 0x283 JP
//                          (sub_44D510); same close handling
//   253 0FD  0x00489FC8    if app+0xA0B44 (dialog) or app+0x2F8 set:
//                          default (no-op); else CreateDialogParamA
//                          (0x296 EN / 0x285 JP, sub_44C7F0), store
//                          app+0xA0B44, ShowWindow(5) + UpdateWindow
//   254 0FE  0x0048A26A    transparent ground-shadow blend at app+0x9ED9A
//                          with CheckMenuItem(GetMenu(hwnd), 0xFE, 0/8);
//                          independent of background AVI menu 0xD8
//   255 0FF  0x0048A883    0x54=1, sub_439E40(app), dirty 0xA0B0D=1
//   256 100  0x0048A89B    0x5C=1, sub_43A650(app), dirty 0xA0B0D=1
//   257 101  0x0048A8B3    0x64=1, sub_43B720(app), dirty 0xA0B0D=1
//   258 102  0x0048A8CB    0x68=1, sub_43BB30(app), dirty 0xA0B0D=1
//   259 103  0x0048A8E3    if app+0xA0B50 or app+0x2F8 set: default no-op;
//                          else CreateDialogParamA (0x29A EN / 0x299 JP,
//                          sub_464BD0) stored at app+0xA0B50, show+update
//   260 104  0x0048A862    toggle English UI byte 0xA0B4C (setz), then
//                          LocalizeUI (0x441AD0) + sub_40B5A0 (0x40B5A0)
//   261 105  0x0048A958    gate app+0x2F8; 0xBC=1; modal "enhance model"
//                          dialog (0x2AA EN / 0x2A9 JP, sub_43C9A0);
//                          if closed with 2 return; MessageBoxA
//                          "Please preserve the edit result as a new model
//                          by 'save enhanced model'." (JP 0x52E724), caption
//                          "enhance model" (JP 0x52E764), MB_TOPMOST;
//                          0xA0B64=1
//   262 106  0x0048A9EC    if app+0xA0B74 or app+0x2F8 set: default no-op /
//                          return; 0x48=1, 0xBC=1; CreateDialogParamA
//                          (0x2AC EN / 0x2AB JP, sub_465020) at app+0xA0B74
//   263 107  0x0048AA6F    "save enhanced model": SetCurrentDirectoryW(exe),
//                          swprintf_s(kFmt529688), OPENFILENAMEW (lStruct
//                          0x4C, filter "Polygon Model files(*.pmd)" /
//                          "*.pmd" / "All Files(*.*)", nFilterIndex 1,
//                          Flags 6, defext "pmd", title "save enhanced
//                          model" / JP 0x52F650, initial dir = DirModel
//                          when menu 0x12D checked else "UserFile\Model");
//                          dir-copy gate like 0x12D -> ExtractDirFromPath
//                          (app+0x9F338) -> CopyDirPathW(DirModel);
//                          sub_41EC10(app, path) (save model file)
//   264 108  0x0048ABB0    0x40=1; toggle byte app+0xA0CC8 with
//                          CheckMenuItem 0x108
//   265 109  0x0048ACA7    app+0xA0CC4 = 1; check 0x109, uncheck
//                          0x10D/0x10E/0x110; ModelBonePhysicsModeApply(0x4B2210)
//                          over the 100 model slots; 0x9EDB5=1
//   266 10A  0x0048AEE1    if app+0xA0CCC set: return; 0x4C=1, 0xBC=1;
//                          CreateDialogParamA (0x323 EN / 0x322 JP,
//                          sub_479E90) at app+0xA0CCC
//   267 10B  0x0048AF57    0x44=1; MessageBoxA "Bullet Physics Library
//                          Ver.2.75\n\nhttp://www.bulletphysics.com/",
//                          caption "about physical engine" (JP 0x52F5E0),
//                          MB_TOPMOST
//   268 10C  0x0048AECE    0x54=1, 0x9EDB5=1
//   269 10D  0x0048AC0E    app+0xA0CC4 = 2; check 0x10D, uncheck
//                          0x109/0x10E/0x110; kinematic sync loop;
//                          0x9EDB5=1
//   270 10E  0x0048AD38    app+0xA0CC4 = 3; check 0x10E, uncheck
//                          0x10D/0x109/0x110; sync loop; 0x9EDB5=1
//   271 10F  0x0048AE60    0x60=1; menu 0x10F (IK display) toggled from its
//                          GetMenuState bit 8; byte app+0xA066D mirrors
//   272 110  0x0048ADCC    app+0xA0CC4 = 0; check 0x110, uncheck
//                          0x10D/0x109/0x10E; sync loop (no 0x9EDB5)
//   273 111  0x0048DBFF    gate app+0x2F8; 0x48=1; per-bone selection:
//                          sel[i] = (frames[i*0x25C+0x1EC] != 0),
//                          curBone = i when visible; PostLanguageSweep +
//                          PostLanguageSweep2
//   274 112  0x0048DC96    gate app+0x2F8; 0x30=1; clear frame-selection
//                          flags of morph (+0x10/0x14), other (+0x14/0x1C)
//                          and bone (+0x38/0x3C) tables, then mark the
//                          bone frames of every visible bone and its
//                          parent chain (+0x38 when +0x39==0); PanelPaint
//   275 113  0x0048DE30    0x38=1, 0xBC=1; modal dialog 0x32D EN / 0x32C JP
//                          (sub_461CE0) - no close handling (always return)
//   276 114  0x0048AFA4    "render to picture file": SetCurrentDirectoryW,
//                          swprintf_s(kFmt529688), OPENFILENAMEW ("All
//                          picture format" / JP 0x52F218, defext "bmp",
//                          title "render to picture file" / JP 0x52F1D0,
//                          initial dir DirUser / "UserFile" on menu 0x12D);
//                          dir-copy gate, wcscpy_s(app+0x9F134, path);
//                          grow locale render size (0x1D4E4/0x1D4E8 ->
//                          0x1D4FC/0x1D500) + PostDeviceReset when render
//                          W/H grew; CreateWindowExA "RecWindow" class
//                          (title 0x52F1C0 "描画中(0 frame)", style
//                          0x80C80000, X = sidebar+0x32, Y = 0x64,
//                          AdjustWindowRect 0x80C00000) stored at
//                          app+0xA0D24; CreateWindow-failed MessageBox
//                          (EN/JP 0x52DFF8); scene vtable+0x94 call when
//                          locale+0x1D4F8 == 0; AviBgOverlayRefresh when (0x9EB84==3
//                          || 0x91C==1) && 0x9E400!=0; RefreshMainWindowViewport;
//                          InvalidateRect(&app+0xA0D40); 0xA03B7=1
//   277 115  0x0048B2F7    0x58=1; menu 0x115 (shadow) toggled from
//                          GetMenuState bit 8; scene vtable+0xE4(scene,
//                          0xA1, on/off)
//   278 116  0x0048B401    InitToonTextures (0x424DC0), sub_4076E0(locale
//                          0xA06C4) - toon texture reload
//   279 117  0x0048B393    0x60=1; menu 0x117 (self shadow) toggle;
//                          byte app+0xA0188 mirrors
//   280 118  0x0048B416    app+0xA0D38 ? SaveFlagSubsystem (0x461FA0)
//                          : InitFlagSubsystem (0x461E00)
//   281 119  0x0048B433    0x6C=1; menu 0x119 (transparent window) toggle;
//                          SetWindowPos(app+0xA0D38, HWND_TOPMOST /
//                          HWND_NOTOPMOST, 0,0,0,0, 0x43)
//   282 11A  0x0048B624    0x40=1; toggle byte app+0xA0194 with
//                          CheckMenuItem 0x11A
//   283 11B  0x0048B682    0x44=1; toggle byte app+0xA0195 with
//                          CheckMenuItem 0x11B
//   284 11C  0x0048B6E0    0x6C=1; toggle byte app+0xA0196 with
//                          CheckMenuItem 0x11C
//   285 11D  0x0048B73E    0x6C=1; toggle byte app+0xA0197 with
//                          CheckMenuItem 0x11D; sub048+0x44 -> ptr,
//                          ptr+0xD4 = 1 / -1
//   286 11E  0x0048B7B8    0x48=1, 0xBC=1; ChooseColorA (lStruct 0x24,
//                          rgbResult from app+0xA0198/9C/A0, Flags 3,
//                          cust colors app+0xA01A4); write R/G/B back,
//                          SetModelColor(model, r, g, b) over the 100 slots
//   287 11F  0x0048B8CF    toggle byte app+0xA01E4 with CheckMenuItem 0x11F
//   288 120  0x0048A08F    0xBC=1; modal dialog 0x325 EN / 0x324 JP
//                          (sub_4641F0); return when closed
//   289 121  0x0048A0DF    0x38=1, 0xBC=1; modal dialog 0x32B EN / 0x32A JP
//                          (sub_42E370); return when closed
//   290 122  0x0048B926    app+0xA0274=1; sub_4629D0 (0x4629D0),
//                          PostDeviceReset (0x440DB0)
//   291 123  0x0048B93E    0x5C=1; app+0xA03B8 ? DisableKinect (0x42A020)
//                          : OpenNiInit (0x429CB0) with 0
//   292 124  0x0048B964    0x38=1; auto-record toggle (byte app+0xA0D68):
//                          off->on: CheckMenuItem 0x124, SetTimer(hwnd,
//                          0x65, 0x5DC), 0x9EDB5=1, model+0x38FD =
//                          app+0xA03EA per slot; on->off: KillTimer(0x65),
//                          uncheck, 0xA0B0D=1, 0x9EDB5=1
//   293 125  0x0048BA1E    toggle byte app+0xA03DC with CheckMenuItem 0x125
//   294 126  0x0048BA75    0x74=1; toggle byte app+0xA03DD with
//                          CheckMenuItem 0x126
//   295 127  0x0048BAD3    0x40=1; toggle byte app+0xA03DE with
//                          CheckMenuItem 0x127
//   296 128  0x0048BB31    "open oni data": app+0x2F8 set -> MessageBox
//                          "Please select model!" (JP 0x52E2F4), caption
//                          "open oni data" / "oni"; else GetOpenFileNameW
//                          ("oni files(*.oni)", defext "oni", Flags
//                          0x1000, initial dir "UserFile", owner
//                          app+0xA0D38 ?: hwnd); DisableKinect when
//                          0xA03B8; WideToSjisPath (0x407910) then
//                          OpenNiInit (0x429CB0)
//   297 129  0x0048BC9B    0x40=1; toggle byte app+0xA03E9 with
//                          CheckMenuItem 0x129
//   298 12A  0x0048BD18    menu 0x12A (2D/3D render) toggle; locale
//                          byte +0x1D570 = on/off; InitRenderStates
//                          (0x406E90) on the locale subobject
//   299 12B  0x0048BDA1    menu 0x12B toggle; BM_SETCHECK(0xF1, on/off)
//                          into GetDlgItem(app+0xA0D38 ?: hwnd, 0x228);
//                          byte app+0xA4420 mirrors
//   300 12C  0x0048A2C1    "rotation" dialog: 0xBC=1.  app+0x2F8 set:
//                          camera path - posx/y/z + cam2/3/4 + camangle
//                          through degree/radian conversion (dbl_52B768
//                          pi, dbl_52B760 180.0, dbl_52E678 (float)pi),
//                          dialog 0x298 EN / 0x289 JP (sub_40FBC0),
//                          writeback, dirty, PostViewRefresh.  else bone
//                          path: cur bone frames pos/quat snapshot,
//                          D3DXMatrixRotationQuaternion + asin/atan2
//                          extraction (0x40A690/0x40A6B0) + quadrant
//                          fixes (+-pi on m01/m13 when fabs(cos(t3))
//                          < flt_52B740), clamp, dialog 0x297 EN /
//                          0x288 JP (sub_40F860), PushBoneEditUndo, pos/angle
//                          writeback, RotationZ*X*Y matrix, quaternion
//                          writeback, frameFlag, dirty, PostViewRefresh
//   301 12D  0x0048D700    menu 0x12D (enhanced mode / accessory column)
//                          toggle only
//   302 12E  0x0048E0A6    "reset rotation": 0x34=1.  app+0x2F8 set:
//                          cam2/3/4 = 0, dirty, PostViewRefresh.  else
//                          cur bone (2D90 != -1): PushBoneEditUndo, identity
//                          matrix, D3DXQuaternionRotationMatrix into the
//                          bone quat, frameFlag[cur]=1, dirty,
//                          PostViewRefresh (tail shared with case 300)
//
// Fidelity notes:
//   * All flags/menu calls follow the original byte-for-byte (CheckMenuItem
//     flags 0/8, SetWindowPos flags 0x43, BM_SETCHECK 0/1, MB_TOPMOST
//     0x40000).  The `def_47E903` default handler only acts when
//     HIWORD(wParam) == BN_CLICKED and the control equals
//     GetDlgItem(hwnd, 0x1B4) - a no-op for every menu id in this family,
//     so `break` == the original's `jmp def_47E903`.
//   * `jmp loc_48F2E0` (function epilogue) is `return`.
//   * Float math follows the original x87 shape (double intermediates,
//     float stores).  Constants are bit-exact: dbl_52B768 = 0x400921FB
//     00000000 (pi truncated to 0x921FB mantissa), dbl_52B760 = 180.0,
//     dbl_52E678 = 0x400921FC80000000 (= (double)(float)pi),
//     flt_52B740 = 0x358637BD, flt_52B73C = 0xC0490FD8 (-pi float),
//     flt_52B738 = 0x40490FD8 (+pi float).
//   * Dialog procedures of this family are ported below (BoneFrameMultiplyDlgProc
//     .. ModelDisplayOrderDlgProc with their original VAs); helpers they call
//     are ported either later in this TU or in the translation units noted at
//     their declarations (call sites preserved; stubs.cpp is off-limits).
//
// Reference: ../translated/MikuMikuDance/fcn_0047e8a0.cpp
//   (the translated file covers only cases 200..0xDC; this port follows the
//   IDA decompilation of MikuMikuDance.exe 0x47E8A0 directly)
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cwchar>
#include <btBulletDynamicsCommon.h>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/scene_history_ui.hpp"
#include "mikumikustudio/model_order_projection.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_parent_coordinates.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/pmm_project_settings.hpp"
#include "mikumikustudio/view_options.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../plugins/native_pose_axis.hpp"
#include "../plugins/native_pose_scope.hpp"
#include "../plugins/native_global_commands.hpp"
#include "../plugins/native_model_order.hpp"
#include "../plugins/native_model_outline_ui.hpp"
#include "../plugins/native_model_outline_color.hpp"
#include "../plugins/native_model_names.hpp"
#include "../plugins/native_model_toon.hpp"
#include "../plugins/native_texture_reload.hpp"
#include "../plugins/native_model_multiply.hpp"
#include "../plugins/native_physics_mode.hpp"
#include "../plugins/native_history_commands.hpp"
#include "../plugins/native_object_commands.hpp"
#include "mikumikustudio/runtime_log.hpp"
#endif
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/plugin_storage.hpp"

#include "dialog_scaffold.hpp"
#include "still_image_picker.hpp"

namespace mikumikustudio {

// Gravity keyframe registrar, defined below at mikumikustudio scope (the
// pump's Enter register-frame block in pump_edit_keys.cpp also calls it).
void RegisterGravityKeyCurrent(MMDApp* app, std::int32_t frame);  // VA 0x00412B20, was Sub412B20

// ---- dialog helpers moved to dedicated TUs (original VAs kept) ----------
// physics-model editor (menu 262, dialog 0x2AC): physics_model_dialog.cpp
void AddRigidBody(HWND hDlg);                    // was Sub45F480, VA 0x0045F480
LRESULT CALLBACK PhysicsEditSubclassProc(HWND, UINT, WPARAM, LPARAM);  // was Sub41EC50, VA 0x0041EC50
void InitPhysicsModelDialog(HWND hDlg);          // was Sub45F670, VA 0x0045F670
void CollectBodyEdits(MMDApp* app);              // was Sub41FF30, VA 0x0041FF30
void CollectJointEdits(MMDApp* app);             // was Sub4204F0, VA 0x004204F0
void AddJoint(HWND hDlg);                        // was Sub43CA50, VA 0x0043CA50
void ApplyBodyRecordToEdits(HWND hDlg, int idx);  // was Sub43CCD0, VA 0x0043CCD0
void ApplyJointRecordToEdits(HWND hDlg, int idx);  // was Sub4214A0, VA 0x004214A0
void FlipPhysicsDialogPage(HWND hDlg, int flag);  // was Sub421C20, VA 0x00421C20
void UpdateShapeControls(HWND hDlg, int mode, int idx);  // was Sub421CE0, VA 0x00421CE0
void CommitPhysicsEdits(HWND hDlg);              // was Sub4220F0, VA 0x004220F0
void PickPivotBone(HWND hDlg);                   // was Sub420DC0, VA 0x00420DC0
INT_PTR CALLBACK PhysicsModelDlgProc(HWND, UINT, WPARAM, LPARAM);  // was Sub465020, VA 0x00465020
// model-edge / English-name dialog (menu 259): model_edge_dialog.cpp
void InitModelEdgeDialog(HWND hDlg);             // was Sub43C430, VA 0x0043C430
void ApplyModelEdgeDialog(HWND hDlg);            // was Sub45F240, VA 0x0045F240
void SelectModelEdgeBone(HWND hDlg);             // was Sub45F050, VA 0x0045F050
void SelectModelEdgeMorph(HWND hDlg);            // was Sub45EF10, VA 0x0045EF10
void SelectModelEdgeGroup(HWND hDlg);             // was Sub45EDC0, VA 0x0045EDC0
void CollectEnglishNameEdit(MMDApp* app, HWND hEdit, int idx);  // was Sub43BED0, VA 0x0043BED0 (collector)
// enhance-model IO (toon collect / save model): enhance_model_io.cpp
int CollectToonFileNames(HWND hDlg);             // was Sub41EA20, VA 0x0041EA20
// (SaveEnhancedModel / SetModelColor declared in ported_funcs.hpp; RefreshMenuLanguage below)
// misc dialog bodies: misc_dialogs.cpp
bool ApplyBoneFrameScaleAdd(MMDApp* app, HWND hDlg);
void ApplyMorphScaleAdd(MMDApp* app, HWND hDlg);  // was Sub43E680, VA 0x0043E680
void InitGravityDialog(HWND hDlg);               // was Sub423160, VA 0x00423160
void ApplyPhysicsOnOff(int on);                  // was Sub4403C0, VA 0x004403C0

// Defined in stubs.cpp - declared here (before the anonymous namespace) so
// the dialog procs inside it can call it.
void SeekSelectedModelToCurrentFrame(MMDApp* app);  // VA 0x004220C0 (model_frame_seek.cpp)

namespace {

// Model-field offsets (model = slot array app+0x780 [byte app+0x910]).
                                                // stride: parent dword@0x30,
                                                // stored pos 0x140..0x148,
                                                // stored quat 0x14C..0x158,
                                                // visible byte +0x1EC
// Bit-exact original constants (see fidelity notes in the header).
// C++17 has no std::bit_cast; MSVC's __builtin_bit_cast is accepted in
// constant expressions, so the static_asserts pin each literal below to
// the original .rdata bit pattern.
constexpr double kDbl52B768 = static_cast<double>(3.141592f);  // pi (truncated)
static_assert(__builtin_bit_cast(std::uint64_t, kDbl52B768) ==
                  0x400921FB00000000ull,
              "dbl_52B768 bit-exact");
constexpr double kDbl52B760 = 180.0;                           // 180.0
static_assert(__builtin_bit_cast(std::uint64_t, kDbl52B760) ==
                  0x4066800000000000ull,
              "dbl_52B760 bit-exact");
// 3.141594886779785 is the shortest round-trip decimal for dbl_52E678.
constexpr double kDbl52E678 = 3.141594886779785;               // (double)(float)pi
static_assert(__builtin_bit_cast(std::uint64_t, kDbl52E678) ==
                  0x400921FC80000000ull,
              "dbl_52E678 bit-exact");
constexpr float kFlt52B740 = 1e-6f;  // ~1e-6 threshold
static_assert(__builtin_bit_cast(std::uint32_t, kFlt52B740) == 0x358637BDu,
              "flt_52B740 bit-exact");
constexpr float kFlt52B73C = -3.141592f;  // -pi float
static_assert(__builtin_bit_cast(std::uint32_t, kFlt52B73C) == 0xC0490FD8u,
              "flt_52B73C bit-exact");
constexpr float kFlt52B738 = 3.141592f;  // +pi float
static_assert(__builtin_bit_cast(std::uint32_t, kFlt52B738) == 0x40490FD8u,
              "flt_52B738 bit-exact");

// 0x529688: swprintf_s format L"\0\0%s%s" - the leading NULs make the
// original call (which passes NO varargs) write an empty string; the %s
// slots are never consumed.  Kept byte-identical (same idiom as
// command_control_450.cpp case 472).

// Active model = slot array at app+0x780 indexed by byte app+0x910.
static unsigned char* ActiveModel(MMDApp* app) {
    return app->SelectedModel();
}

// 0x40A680 / 0x40A690 / 0x40A6B0 / 0x40A6D0 - the D3DX angle helpers of the
// bone-rotation dialog (case 300).  sub_40A6B0 dispatches to the CRT
// atan2 (cintrin table entry "atan2" at 0x544C40): arg0 = x, arg1 = y;
// every result is rounded to float before it is stored.
double AngleFabs(float x) { return static_cast<double>(std::fabs(x)); }
double AngleAsin(float x) { return static_cast<double>(std::asin(x)); }
double AngleCos(float x)  { return static_cast<double>(std::cos(x)); }
double AngleAtan2(float y, float x) {
    return static_cast<double>(static_cast<float>(std::atan2(y, x)));
}

// Use the COM interface instead of byte offsets into the x86 vtable.
void SetMultisampleAntialias(MMDApp* app, bool enabled) {
    app->Renderer()->device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, enabled ? TRUE : FALSE);
}

// Write back the selected bone's rotation and mark its pending edit.
// dirty = 1, PostViewRefresh.
void BoneRotationWriteback(MMDApp* app, const d3dx::D3DXMATRIXF& m) {
    unsigned char* model = ActiveModel(app);
    const std::int32_t cur = mdl::Mdl(model)->selectedBone;
    mdl::BoneRecord* const bones = mdl::Bones(model);
    float quat[4];
    d3dx::Get().quatFromMatrix(quat, &m);
    std::memcpy(bones[cur].rotQuat, quat, sizeof(quat));
    unsigned char* frameFlag = mdl::Mdl(model)->bonePhysicsState;
    frameFlag[cur] = 1;
    app->SceneModified() = 1;
    PostViewRefresh(app);
}

// ---------------------------------------------------------------------------
// Dialog procedures of this family, ported from the MikuMikuDance.exe
// decompilation (each carries its original VA).  They reach the app state via
// the global g_Block and share the rotation temp floats app+0xA0B28..0xA0B40.
// Helpers they call are ported either later in this TU or in the translation
// units noted at their declarations (call sites preserved; stubs.cpp is
// off-limits).
// ---------------------------------------------------------------------------

// ---- JP strings, byte-exact Shift-JIS as in the binary ------------------
// 0x52E724: "編集結果を'強化モデル保存'で保存してモデルとして保存して下さい"
static const char kMsgEnhanceModelJp[] =
    "\x95\xD2\x8F\x57\x8C\x8B\x89\xCA\x82\xCD\x27\x8A\x67\x92\xA3"
    "\x83\x82\x83\x66\x83\x8B\x95\xDB\x91\xB6\x27\x82\xC5\x90\x56"
    "\x82\xB5\x82\xA2\x83\x82\x83\x66\x83\x8B\x82\xC6\x82\xB5\x82"
    "\xC4\x95\xDB\x91\xB6\x82\xB5\x82\xC4\x89\xBA\x82\xB3\x82\xA2";
// 0x52E764: "モデル強化"
static const char kCaptionEnhanceModelJp[] =
    "\x83\x82\x83\x66\x83\x8B\x8A\x67\x92\xA3";
// 0x52F5E0: "物理エンジンについて"
static const char kCaptionPhysEngineJp[] =
    "\x95\xA8\x97\x9D\x83\x47\x83\x93\x83\x57\x83\x93\x82\xC9\x82\xC2"
    "\x82\xA2\x82\xC4";
// 0x52DFF8: "描画ウィンドウ作成"
static const char kCaptionCreateWndJp[] =
    "\x98\x5E\x89\xE6\x83\x45\x83\x42\x83\x93\x83\x68\x83\x45\x8D\xEC"
    "\x90\xAC";
// 0x52E2F4: "モデルを選択していないとoniデータは読めません"
static const char kMsgOpenOniJp[] =
    "\x83\x4A\x83\x81\x83\x89\x81\x45\x8F\xC6\x96\xBE\x83\x82\x81\x5B"
    "\x83\x68\x82\xC5\x82\xCD" "oni" "\x83\x66\x81\x5B\x83\x5E\x82\xCD"
    "\x93\xC7\x82\xDF\x82\xDC\x82\xB9\x82\xF1";

// 0x52F650: JP title "拡張モデル保存"
static const wchar_t kTitleSaveModelJp[] =
    L"\x62E1\x5F35\x30E2\x30C7\x30EB\x4FDD\x5B58";
// 0x52F218: JP picture filter label "書きの画像フォーマット" (label is
// followed by a double NUL in the binary - the pattern list is empty).
static const wchar_t kFilterPicJp[] =
    L"\x5168\x3066\x306E\x5BFE\x5FDC\x30D5\x30A9\x30FC\x30DE\x30C3\x30C8"
    L"\x00\x00"
    L"*.bmp;*.jpg;*.png;*.dds;*.dib;*.pfm;*.hdr\x00\x00"
    L"Bmp files(*.bmp)\x00\x00*.bmp\x00\x00";
// 0x52F1D0: JP title "画像ファイル出力"
static const wchar_t kTitleRenderJp[] =
    L"\x753B\x50CF\x30D5\x30A1\x30A4\x30EB\x51FA\x529B";
// 0x52DC84: JP title "ファイルを開く"
static const wchar_t kTitleOpenJp[] =
    L"\x30D5\x30A1\x30A4\x30EB\x3092\x958B\x304F";

// ---- dialog-helper call targets (ported below in this TU, or in the files
//      noted on each declaration; e.g. SeekSelectedModelToCurrentFrame is
//      defined in src/model/model_frame_seek.cpp) ----

// Forward declarations (bodies below / later in this TU).
void SetEdgeThickness(MMDApp* app, float thickness);      // VA 0x0041E980, was Sub41E980
void SetGravityChannel(MMDApp* app, int channel, float value);  // VA 0x0045FD80, was Sub45FD80
// VA 0x0042E270 - edit-646 (0x286) subclass of the edge-thickness dialog
// (253): on WM_KEYDOWN+VK_RETURN it reads the edit text, applies it through
// the thickness setter (0x41E980) and parks the percent value on trackbar
// 647 (TBM_SETPOS, dbl 0x52B8E0 = 100.0); anything else goes to the saved
// original procedure (app+0xA0B48, captured when case 253 subclasses).
LRESULT CALLBACK EdgeThicknessEditSubclassProc(  // was Sub42E270, VA 0x0042E270
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const auto dialog=GetParent(hwnd);
    auto* app=reinterpret_cast<MMDApp*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(!app)app=g_Block;
#if !MMD_ORIGINAL_PROFILE
    // The registered controller handles Enter before this legacy procedure.
    // Failed controller initialization must never fall back to a direct write.
    if((plugins::BoundNativePoseHistory(app)||plugins::ControlledModelOutlineDialog(dialog))&&msg==WM_KEYDOWN&&wParam==VK_RETURN)return 0;
#endif
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {  // 0x100 / 0x0D
        const HWND dlg = app->state.edgeThicknessDialog;
        if (hwnd == GetDlgItem(dlg, panel::kEdgeThicknessEdit /*646*/)) {
            char buf[8];
            GetWindowTextA(hwnd, buf, 8);
            const float v = static_cast<float>(atof(buf));
            SetEdgeThickness(app, v);                   // 0x41E980
            SendMessageA(GetDlgItem(dlg, panel::kEdgeThicknessSlider /*647*/),
                         TBM_SETPOS, 1,
                         static_cast<int>(v * 100.0));  // dbl 0x52B8E0
            return 0;
        }
    }
    return CallWindowProcA(app->EdgeThicknessEditProc(),
                           hwnd, msg, wParam, lParam);
}
// VA 0x0041E950 - edge-thickness getter: camera mode (byte 0x2F8) reports a
// constant 1.0, model mode the active model's thickness float (+0x31C0).
// (Template caption "thickness of edge line" / エッジ太さ設定, scale 0..2 -
// model+0x31C0 is the per-model edge thickness, not a FPS.)
float GetEdgeThickness(MMDApp* app) {  // was Sub41E950, VA 0x0041E950
    if (app->state.optflag[0] != 0)
        return 1.0f;
    unsigned char* model = app->SelectedModel();
    return model ? mdl::Mdl(model)->edgeScale : 1.0f;
}
// VA 0x0041E980 - edge-thickness setter (model mode only): marks the
// in-dialog and dirty flags, then stores into the active model's thickness
// float (+0x31C0).
void SetEdgeThickness(MMDApp* app, float thickness) {  // was Sub41E980, VA 0x0041E980
#if !MMD_ORIGINAL_PROFILE
    if(plugins::BoundNativePoseHistory(app)||plugins::ControlledModelOutlineDialog(app->state.edgeThicknessDialog))return; // No legacy write-through after a controlled refusal.
#endif
    if (app->state.optflag[0] != 0)
        return;
    app->state.messageSeen = 1;
    app->SceneModified() = 1;
    unsigned char* model = app->SelectedModel();
    if (model) mdl::Mdl(model)->edgeScale = thickness;
}
// VA 0x0041E9C0 - enhance-model dialog init: mirrors the ten 100-byte SJIS
// name slots of the active model (+0x33D8) into edits 709..718.
void FillEnhanceModelNameEdits(MMDApp* app, HWND hDlg) {  // was Sub41E9C0, VA 0x0041E9C0
    unsigned char* model = app->SelectedModel();
    for (int i = 0; i < 10; ++i) {
        std::wstring name;
        const char* bytes = mdl::Mdl(model)->pmdToonFileNames[i];
        if (!text_encoding::Decode(bytes, 932, name))
            name = text_encoding::Display(bytes);
        SetWindowTextW(GetDlgItem(hDlg, panel::kToon01Edit + i), name.c_str());
    }
}
// VA 0x0045ECC0 - model-edge dialog (259) edit subclass: WM_KEYDOWN +
// VK_RETURN sets the in-dialog flag (app+0xA0D6C) and dispatches to the
// edge collector (0x43BED0) with 0/1/2/3 for edits 0x29B/0x2A0/0x2A4/
// 0x2A8 (edit 0x2A0 additionally runs the language sweep 0x42F1E0);
// everything else goes to the saved original proc (app+0xA0B54).
LRESULT CALLBACK ModelEdgeEditSubclassProc(  // was Sub45ECC0, VA 0x0045ECC0
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MMDApp* app = g_Block;
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        app->state.messageSeen = 1;
        const HWND dlg = app->FrameRangeDialog();
        if (hwnd == GetDlgItem(dlg, panel::kModelNameEnEdit)) {
            CollectEnglishNameEdit(app, hwnd, 0);                        // 0x43BED0
            return 0;
        }
        if (hwnd == GetDlgItem(dlg, panel::kBoneNameEnEdit)) {
            CollectEnglishNameEdit(app, hwnd, 1);
            PostLanguageSweep(app);                         // 0x42F1E0
            return 0;
        }
        if (hwnd == GetDlgItem(dlg, panel::kMorphNameEnEdit)) {
            CollectEnglishNameEdit(app, hwnd, 2);
            return 0;
        }
        if (hwnd == GetDlgItem(dlg, panel::kGroupNameEnEdit)) {
            CollectEnglishNameEdit(app, hwnd, 3);
            return 0;
        }
    }
    return CallWindowProcA(app->ModelEdgeEditProc(),
                           hwnd, msg, wParam, lParam);
}
// Display order is also the native model-selector row identity. Calculation
// order is independent (comboSelIndex2, consumed by physics_frame.cpp).
struct ModelOrderDialogState {
    MMDApp& app;
    bool calculate;
    int count = 0;
    std::int32_t* order = nullptr; // [0] unused; entries hold slot + 1
    std::array<unsigned char*, kModelSlotCount> models{};
    std::array<unsigned char, kModelSlotCount> draw{}, calculation{};
    bool uiNeedsRepair = false, listNeedsRepair = false, closeRequested = false;
    bool restoreCanUndo = false, restoreCanRedo = false;
    scene_history_ui::Selection restoreSelection{};
#if !MMD_ORIGINAL_PROFILE
    plugins::NativeModelOrderPlan historyPlan;
    bool bound = false;
#endif
    void ClearScratch() noexcept {
        if (app.AccessoryOrderArray() == order) app.AccessoryOrderArray() = nullptr;
        std::free(order); order = nullptr;
    }
    ~ModelOrderDialogState() {
        ClearScratch();
    }
    bool IsCurrent() const noexcept {
        for (int slot = 0; slot < kModelSlotCount; ++slot) {
            auto* model = app.ModelSlot(slot);
            if (model != models[slot]) return false;
            if (model && (mdl::Mdl(model)->comboSelIndex != draw[slot] ||
                          mdl::Mdl(model)->comboSelIndex2 != calculation[slot])) return false;
        }
        return app.AccessoryOrderArray() == order;
    }
};

bool ModelOrderEditReady(MMDApp& app) {
    if (bake::UiLocked() || app.SceneMutationInProgress()) return false;
#if !MMD_ORIGINAL_PROFILE
    if (auto* history = plugins::BoundNativePoseHistory(&app))
        if (history->CheckNativeNavigation() != MMS_OK) return false;
#endif
    return true;
}

bool InitModelOrderRows(ModelOrderDialogState& state, HWND dialog) {
    if (state.app.AccessoryOrderArray()) return false;
    // List captions are derived from the objects, never copied from a possibly
    // stale selector. The two dialogs choose different native order fields.
    bake::ScopedUiInputGate gate;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* model = state.app.ModelSlot(slot);
        state.models[slot] = model;
        if (!model) continue;
        for (int previous = 0; previous < slot; ++previous) if (state.models[previous] == model) return false;
        ++state.count;
        state.draw[slot] = mdl::Mdl(model)->comboSelIndex;
        state.calculation[slot] = mdl::Mdl(model)->comboSelIndex2;
    }
    state.order = static_cast<std::int32_t*>(
        std::calloc(static_cast<std::size_t>(state.count) + 1, sizeof(std::int32_t)));
    if (!state.order) return false;
    state.app.AccessoryOrderArray() = state.order;
    try {
        const auto projected = model_order::Project<kModelSlotCount>([&](std::size_t slot) noexcept {
            return state.models[slot] ? int(state.calculate ? state.calculation[slot] : state.draw[slot]) : -1;
        });
        if (projected.count != static_cast<std::size_t>(state.count)) return false;
        for (int row = 1; row <= state.count; ++row) {
            const int found = projected.rows[row] - 1;
            state.order[row] = projected.rows[row];
            const auto name = text_encoding::ModelLabel(*mdl::Mdl(state.models[found]),
                UiUsesEnglishNames(state.app.EnglishUI()));
            if (SendDlgItemMessageW(dialog, panel::kOrderListBox, LB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(name.c_str())) != row - 1) return false;
        }
    } catch (...) { return false; }
    return state.IsCurrent();
}

bool RepairModelOrderUi(ModelOrderDialogState& state) {
    if (!state.uiNeedsRepair) return true;
    bake::ScopedUiInputGate gate;
    state.uiNeedsRepair = scene_history_ui::RefreshObjectLists(state.app, state.restoreSelection,
        state.restoreCanUndo, state.restoreCanRedo) != scene_history_ui::Result::Ok;
    return !state.uiNeedsRepair;
}

bool RepairModelOrderRows(ModelOrderDialogState& state, HWND dialog) {
    if (!state.listNeedsRepair) return true;
    if (!state.IsCurrent()) return false;
    bake::ScopedUiInputGate gate;
    SendDlgItemMessageW(dialog, panel::kOrderListBox, LB_RESETCONTENT, 0, 0);
    try {
        for (int row = 1; row <= state.count; ++row) {
            const auto stored = state.order[row];
            if (stored <= 0 || stored > kModelSlotCount) return false;
            const auto slot = static_cast<unsigned>(stored - 1);
            if (!state.models[slot]) return false;
            const auto name = text_encoding::ModelLabel(*mdl::Mdl(state.models[slot]),
                UiUsesEnglishNames(state.app.EnglishUI()));
            if (SendDlgItemMessageW(dialog, panel::kOrderListBox, LB_INSERTSTRING, row - 1,
                    reinterpret_cast<LPARAM>(name.c_str())) != row - 1) return false;
        }
    } catch (...) { return false; }
    state.listNeedsRepair = false;
    return true;
}

bool ApplyModelOrderDialog(ModelOrderDialogState& state) {
    auto& app = state.app;
    if (!ModelOrderEditReady(app) || !RepairModelOrderUi(state) || !state.IsCurrent()) return false;
    std::array<bool, kModelSlotCount> used{};
    std::array<mdl::ModelRecord*, kModelSlotCount> targets{};
    bool changed = false;
    for (int row = 1; row <= state.count; ++row) {
        const auto stored = state.order[row];
        if (stored <= 0 || stored > kModelSlotCount) return false;
        const auto slot = static_cast<unsigned>(stored - 1);
        if (!state.models[slot] || used[slot]) return false;
        used[slot] = true;
        targets[static_cast<std::size_t>(row) - 1] = mdl::Mdl(state.models[slot]);
        changed |= (state.calculate ? state.calculation[slot] : state.draw[slot]) != row;
    }
#if !MMD_ORIGINAL_PROFILE
    if (state.bound)
        return state.historyPlan.Read(state.order, static_cast<std::size_t>(state.count)) == MMS_OK;
#endif
    // No callbacks occur between the order-byte writes. Native control reads
    // and refreshes share the guard against synchronous input/close reentry.
    bake::ScopedUiInputGate gate;
    const auto selection = scene_history_ui::CaptureSelection(app);
    const bool canUndo = IsWindowEnabled(GetDlgItem(app.MainWindow(), panel::kUndoButton)) != FALSE;
    const bool canRedo = IsWindowEnabled(GetDlgItem(app.MainWindow(), panel::kRedoButton)) != FALSE;
    for (int row = 1; row <= state.count; ++row) {
        auto& model = *targets[static_cast<std::size_t>(row) - 1];
        (state.calculate ? model.comboSelIndex2 : model.comboSelIndex) = static_cast<unsigned char>(row);
    }
    if (!state.calculate && scene_history_ui::RefreshObjectLists(app, selection, canUndo, canRedo) !=
            scene_history_ui::Result::Ok) {
        for (int slot = 0; slot < kModelSlotCount; ++slot)
            if (state.models[slot]) mdl::Mdl(state.models[slot])->comboSelIndex = state.draw[slot];
        // A failed native control write must not publish a different order.
        // Cancel/retry cannot release the modal owner until the restored
        // mapping is complete, including repeated CB_ERRSPACE failures.
        state.restoreSelection = selection;
        state.restoreCanUndo = canUndo; state.restoreCanRedo = canRedo;
        state.uiNeedsRepair = true;
        RepairModelOrderUi(state);
        return false;
    }
    if (changed) {
        app.SceneModified() = 1;
        if (!state.calculate) InvalidateEffectCurrentObjectCache();
        InvalidateRect(app.MainWindow(), nullptr, FALSE);
    }
    return true;
}

#if !MMD_ORIGINAL_PROFILE
bool BeginBoundModelOrder(ModelOrderDialogState& state) {
    if (!plugins::BoundNativePoseHistory(&state.app)) return true;
    const auto result = state.historyPlan.Begin(state.app,
        state.calculate ? plugins::ModelOrderKind::Calculate : plugins::ModelOrderKind::Draw);
    if (result != MMS_OK) {
        runtime_log::Write("PLUGIN_ABI model order capture refused: %d", result); return false;
    }
    state.bound = true; return true;
}
void FinishBoundModelOrder(ModelOrderDialogState& state, INT_PTR response) {
    if (!state.bound || response != IDOK) return;
    state.ClearScratch();
    std::shared_ptr<plugins::HistoryChange> change;
    auto result = state.historyPlan.Take(change);
    if (result == MMS_OK && change) {
        auto* history = plugins::BoundNativePoseHistory(&state.app);
        result = history ? plugins::CommitBoundModelOrder(state.app, *history, std::move(change), state.calculate) : MMS_E_STALE;
    }
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI model order commit refused: %d", result);
}
#endif

// VA 0x00466370 - gravity-setting dialog (266) edit subclass: WM_KEYDOWN +
// VK_RETURN reads the edit (0x2C5..0x2C9 = 709..713) as a float and applies
// it through 0x45FD80 with channel 0..4; edits 710..712 additionally park
// the percent value on their trackbars 637..639 (TBM_SETPOS, dbl 0x52B8E0
// = 100.0). Common-controls subclassing keeps the original procedure per
// control without truncating pointers or sharing another dialog's scratch.
LRESULT CALLBACK GravitySettingEditSubclassProc(  // was Sub466370, VA 0x00466370
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData) {
    auto* app = reinterpret_cast<MMDApp*>(refData);
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        const HWND dlg = app->GravitySettingDialog();
        char buf[0x100];
        for (int ch = 0; ch < 5; ++ch) {
            if (hwnd != GetDlgItem(dlg, panel::kGravityAccelEdit + ch))
                continue;
            GetWindowTextA(hwnd, buf, 0x100);
            const float v = static_cast<float>(atof(buf));
            if (ch >= 1 && ch <= 3)
                SendMessageA(GetDlgItem(dlg, panel::kGravityDirXSlider + ch - 1),
                             TBM_SETPOS, 1,
                             static_cast<int>(v * 100.0));  // dbl 0x52B8E0
            SetGravityChannel(app, ch, v);                   // 0x45FD80
            return 0;
        }
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, GravitySettingEditSubclassProc, subclassId);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
// VA 0x0045FD80 - gravity-setting dialog channel setter.  Channels 0..3
// write the gravity cluster (magnitude / direction X / Y / Z, app+0x9EDC4 /
// 0x9EDB8 / 0x9EDBC / 0x9EDC0), normalise the direction (D3DXVec3Normalize,
// in place), scale it by magnitude * dbl 0x52C170 (10.0) and push it into
// the physics world setGravity (world = *(app+0x9EDB0)->world, vtable
// slot 0x34/4 = 13).  An all-zero direction gets Y nudged to
// flt 0x529624 = 0.1f first.  Channel 4 stores (int)value into app+0x9EDC8.
// Ends with RefreshRequest(-4) on every path.
void SetGravityChannel(MMDApp* app, int channel, float value) {  // was Sub45FD80, VA 0x0045FD80
    switch (channel) {
    case 0:
        app->state.gravityMagnitude = value;
        if (app->state.gravityX == 0.0f &&
            app->state.gravityY == 0.0f &&
            app->state.gravityZ == 0.0f)
            app->state.gravityY = 0.1f;  // flt 0x529624
        break;
    case 1:
        app->state.gravityX = value;
        if (value == 0.0f && app->state.gravityY == 0.0f &&
            app->state.gravityZ == 0.0f)
            app->state.gravityY = 0.1f;
        break;
    case 2:
        app->state.gravityY = value;
        if (app->state.gravityX == 0.0f &&
            app->state.gravityZ == 0.0f && value == 0.0f)
            app->state.gravityY = 0.1f;
        break;
    case 3:
        app->state.gravityZ = value;
        if (app->state.gravityX == 0.0f &&
            app->state.gravityY == 0.0f && value == 0.0f)
            app->state.gravityY = 0.1f;
        break;
    default:
        if (channel == 4)
            app->state.gravityNoise =
                static_cast<std::int32_t>(value);
        RefreshRequest(-4);                                 // 0x440AC0
        return;
    }

    // common tail: normalise, scale by magnitude * 10, setGravity
    float dir[3] = {app->state.gravityX,
                    app->state.gravityY,
                    app->state.gravityZ};
    if (d3dx::Get().vec3Normalize != nullptr && d3dx::Get().Load())
        d3dx::Get().vec3Normalize(dir, dir);
    else {  // documented fallback (same formula; 1/sqrt)
        const float inv = 1.0f / std::sqrt(dir[0] * dir[0] +
                                           dir[1] * dir[1] +
                                           dir[2] * dir[2]);
        dir[0] *= inv;
        dir[1] *= inv;
        dir[2] *= inv;
    }
    const float mag = app->state.gravityMagnitude;
    float vec[4] = {static_cast<float>(dir[0] * mag * 10.0),
                    static_cast<float>(dir[1] * mag * 10.0),
                    static_cast<float>(dir[2] * mag * 10.0), 0.0f};
    PhysicsScene* physicsScene = app->Physics();
    if (physicsScene != nullptr && physicsScene->world != nullptr)
        physicsScene->world->setGravity(btVector3(vec[0], vec[1], vec[2]));
    RefreshRequest(-4);                                     // 0x440AC0
}
// VA 0x00460080 - gravity-setting dialog OK ("register"): dirty flag, clear the
// selection marks of the four global frame tables (camera 0x374 +0x48,
// light 0x378 +0x24, self-shadow 0x37C +0x14, gravity 0x380 +0x21) and of
// all 255 accessory tracks (0x384 blobs, +0x18, 0x3C stride), rebuild via
// 0x412B20 at the current frame, then RefreshRequest(-4) + PanelPaint.
void ApplyGravitySettingDialog(MMDApp* app) {  // was Sub460080, VA 0x00460080
    app->SceneModified() = 1;
    mdl::CameraKey* camera = app->CameraKeys();
    mdl::LightKey* light = app->LightKeys();
    mdl::SelfShadowKey* shadow = app->ShadowKeys();
    mdl::GravityKey* gravity = app->GravityKeys();
    if (camera == nullptr || light == nullptr || shadow == nullptr ||
        gravity == nullptr)
        return;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(camera); i < capacity; ++i) camera[i].selected = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(light); i < capacity; ++i) light[i].selected = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(shadow); i < capacity; ++i) shadow[i].selected = 0;
    for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(gravity); i < capacity; ++i) gravity[i].selected = 0;
    for (int slot = 0; slot < 255; ++slot) {
        mdl::AccessoryKey* acc = app->AccessoryKeys(slot);
        if (acc == nullptr)
            continue;
        for (std::size_t i = 0, capacity = mdl::KeyPoolCapacity(acc); i < capacity; ++i)
            acc[i].selected = 0;
    }
    RegisterGravityKeyCurrent(app, app->state.currentFrame);  // 0x412B20
    RefreshRequest(-4);                                       // 0x440AC0
    PanelPaint(app);                                          // 0x414610
}
// Newly-required unported dependencies (0x45ECC0 / 0x460080 call sites) -
// file-local stub bodies so the call sites link (stubs.cpp must not be
// modified).  CollectEnglishNameEdit's real body now lives in model_edge_dialog.cpp;
// only the forward declaration at the top of this TU is needed.
}  // namespace (anonymous) - RegisterGravityKeyCurrent is consumed by pump_edit_keys.cpp
// and stays at mikumikustudio scope.
// VA 0x00412B20 (x64 twin sub_7FF7CB47DA60, confirmed on the x64 binary;
// the pre-existing "frame-table rebuild" label was wrong): the gravity
// keyframe registrar.  Walks the gravity track's next-chain (app+0x380,
// 36-byte GravityKey records, 10000 slots) to the first record with
// frame >= target: exact hit overwrites the live gravity cluster
// (magnitude / direction xyz / noise / noise-mode byte, selected = 1)
// into that record and returns; otherwise a fresh slot is taken from the
// frame == 0 holes (records 1..9999; record 1 is checked first) and
// spliced in - before the found record, or appended after the chain's
// dead end - with lastRegisteredFrame bumped to max(current, frame).
// No free slot raises the "You cannot regist over %dpoint." box
// (limit 10000, EN/JP by englishUI) and returns without registering.
// 0x460080 (ApplyGravitySettingDialog above) calls this at the current frame.
void RegisterGravityKeyCurrent(MMDApp* app, std::int32_t frame) {
    if(frame<0){negative_frames::RegisterGlobal(app,negative_frames::Global::Gravity,frame);return;}  // VA 0x00412B20, was Sub412B20
    // JP overflow strings (0x52B918 / 0x52B908, SJIS byte-exact - the
    // same pair key_registrars.cpp uses for the model-track registrars)
    static const char kJpOverflow[] =
        "\x93\x6f\x98\x5e\x83\x7c\x83\x43\x83\x93\x83\x67\x90\x94\x82\xaa%d"
        "\x8c\xc2\x82\xf0\x89\x7a\x82\xa6\x82\xdc\x82\xb5\x82\xbd\n"
        "\x82\xb1\x82\xea\x88\xc8\x8f\xe3\x82\xcc\x93\x6f\x98\x5e\x82\xcd\x8d"
        "\x73\x82\xa6\x82\xdc\x82\xb5\x82\xf1\n"
        "\x81\x75\xcc\xda\xb0\xd1\x95\xd2\x8f\x57\x81\x76\x82\xcc\x81\x75\x95"
        "\x73\x97\x70\xcc\xda\xb0\xd1\x8d\xed\x8f\x9c\x81\x76\x82\xf0\x8e\xc0"
        "\x8d\x73\x82\xb5\x82\xc4\x89\xba\x82\xb3\x82\xa2";
    static const char kJpFrameRegTitle[] =  // "フレーム登録"
        "\xcc\xda\xb0\xd1\x93\x6f\x98\x5e";

    mdl::GravityKey* keys = app->GravityKeys();
    const std::uint32_t target = static_cast<std::uint32_t>(frame);

    // Walk the chain from record 0 for the first frame >= target; a dead
    // end leaves `current` on the chain's last record (append there).
    std::uint32_t current = 0;
    bool spliceBefore = keys[0].frame >= target;
    if (!spliceBefore) {
        std::uint32_t node = 0;
        for (;;) {
            const std::uint32_t next = keys[node].next;
            if (next == 0)
                break;
            current = next;
            node = next;
            if (keys[current].frame >= target) {
                spliceBefore = true;
                break;
            }
        }
    }

    // Exact hit: overwrite the live cluster into the record.
    if (spliceBefore && keys[current].frame == target) {
        mdl::GravityKey& key = keys[current];
        key.acceleration = app->state.gravityMagnitude;
        key.direction[0] = app->state.gravityX;
        key.direction[1] = app->state.gravityY;
        key.direction[2] = app->state.gravityZ;
        key.noise = app->state.gravityNoise;
        key.noiseEnabled = app->GravityNoiseEnabled();
        key.selected = 1;
        curve_host::NotifyLegacyRegistered(app, curve_host::Kind::Gravity, 0, frame);
        return;
    }

    std::uint32_t freeIndex = 0;
    if (!mdl::KeyPoolFreeSlot(app->GravityKeys(), freeIndex)) {
        MessageBoxW(static_cast<HWND>(app->Hwnd()),
            app->EnglishUI() == 2 ? L"无法扩展关键帧存储，本次注册已取消。" :
            app->EnglishUI() == 0 ? L"メモリを確保できないため、登録を中止しました。" :
            L"Unable to allocate keyframe storage. Registration was cancelled.",
            L"MikuMikuStudio", MB_OK | MB_ICONERROR);
        return;
    }
    keys = app->GravityKeys();
    mdl::GravityKey& added = keys[freeIndex];
    if (spliceBefore) {
        const std::uint32_t previous = keys[current].previous;
        keys[previous].next = freeIndex;
        added.previous = previous;
        keys[current].previous = freeIndex;
        added.next = current;
    } else {
        keys[current].next = freeIndex;
        added.previous = current;
        // added.next keeps the zeroed hole's value, as the original does.
    }
    added.frame = target;
    added.acceleration = app->state.gravityMagnitude;
    added.direction[0] = app->state.gravityX;
    added.direction[1] = app->state.gravityY;
    added.direction[2] = app->state.gravityZ;
    added.noise = app->state.gravityNoise;
    added.noiseEnabled = app->GravityNoiseEnabled();
    added.selected = 1;
    if (target > app->state.lastRegisteredFrame)
        app->state.lastRegisteredFrame = target;
    curve_host::NotifyLegacyRegistered(app, curve_host::Kind::Gravity, 0, frame);
}

namespace {  // (resumed)

// ---- JP strings of the dialogs, byte-exact -------------------------------
// 0x52E628: "ON (X印)" (SJIS 0x88F3 = 印); 0x52E624: "OFF".
static const char kOnXMarkJp[] = "ON (X\x88\xF3)";
static const char kOff[] = "OFF";
// "fine shadow mode" notice (x64 0x7FF7CB54D6A0 / 0x7FF7CB54D6B0): JP
// caption = the Fshadow button's two-kanji label + "モード"; JP text =
// "この命令は画面右上の「<Fshadow label>」ボタンに取って変わりました"
// (SJIS, byte-exact).
static const char kCaptionFineShadowJp[] =
    "\x94\xFC\x89\x65\x83\x82\x81\x5B\x83\x68";
static const char kMsgFineShadowJp[] =
    "\x82\xB1\x82\xCC\x89\x42\x82\xB5\x83\x52\x83\x7D\x83\x93\x83\x68\x82\xCD"
    "\x89\xE6\x96\xCA\x89\x45\x8F\xE3\x82\xCC\x81\x75\x94\xFC\x81\x40\x89\x65"
    "\x81\x76\x83\x7B\x83\x5E\x83\x93\x82\xC9\x8E\xE6\x82\xC1\x82\xC4\x95\xCF"
    "\x82\xED\x82\xE8\x82\xDC\x82\xB5\x82\xBD";

// ===========================================================================
// 0x44D3F0 - case 251 bone-frame multiply dialog (template caption
// "multiply of bone position-angle" / ﾎﾞｰﾝﾌﾚｰﾑ位置角度補正; 0x294 EN /
// 0x27C JP).  WM_INITDIALOG: top-most the dialog when the accessory column
// exists (app+0xA0D38), prefill the 12 edit boxes 686..697 (even columns
// "1.0", odd columns "0.0" via EM_REPLACESEL), focus 686 and select all.
// WM_COMMAND: id 1 -> apply selected bone keys + EndDialog(1); id 2 -> EndDialog(2).
// Reference: MikuMikuDance.exe sub_44D3F0.
// ===========================================================================
// Extend the existing dialog below its angle rows. The original resource and
// original-profile window keep their native layout.
static void AddBoneMultiplyScaleRows(MMDApp* app, HWND dialog) {
    if (MMD_ORIGINAL_PROFILE) return;
    RECT extra{0, 0, 0, 60}, window{};
    MapDialogRect(dialog, &extra);
    GetWindowRect(dialog, &window);
    const int height = window.bottom - window.top + extra.bottom;
    int top = window.top - extra.bottom / 2;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor))
        top = (std::max)(int(monitor.rcWork.top),
            (std::min)(top, int(monitor.rcWork.bottom) - height));
    SetWindowPos(dialog, nullptr, window.left, top, window.right - window.left, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    for (int id : {IDOK, IDCANCEL}) {
        HWND button = GetDlgItem(dialog, id);
        RECT rect{}; GetWindowRect(button, &rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(button, nullptr, rect.left, rect.top + extra.bottom, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(
        GetDlgItem(dialog, panel::kBoneMulPosXScaleEdit), WM_GETFONT, 0, 0));
    const auto add = [&](const wchar_t* kind, const wchar_t* text, int id,
                         int x, int y, int width, int height, DWORD style) {
        RECT rect{x, y, x + width, y + height}; MapDialogRect(dialog, &rect);
        HWND child = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE)), nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return child;
    };
    const wchar_t* label = app->EnglishUI() == kUiChinese ? L"缩放" :
        app->EnglishUI() ? L"scale" : L"拡大率";
    add(L"STATIC", label, -1, 8, 117, 80, 12, SS_LEFT);
    HWND previous = GetDlgItem(dialog, panel::kBoneMulPosXOffsetEdit + 10);
    for (int axis = 0; axis < 3; ++axis) {
        const int y = 130 + axis * 15;
        const wchar_t* names[] = {L"X *", L"Y *", L"Z *"};
        add(L"STATIC", names[axis], -1, 26, y + 2, 13, 12, SS_LEFT);
        add(L"STATIC", L"+", -1, 74, y + 2, 7, 12, SS_LEFT);
        for (int column = 0; column < 2; ++column) {
            HWND edit = add(L"EDIT", column ? L"0.0" : L"1.0",
                panel::kBoneMulScaleXFactorEdit + 2 * axis + column,
                column ? 82 : 40, y, 30, 12,
                WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL);
            SetWindowPos(edit, previous, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            previous = edit;
        }
    }
}

#if !MMD_ORIGINAL_PROFILE
struct ModelMultiplySession { MMDApp* app; plugins::NativeModelMultiplyPlan* plan; };
static ModelMultiplySession* MultiplySession(HWND dialog, UINT message, LPARAM parameter) {
    auto* session = message == WM_INITDIALOG ? reinterpret_cast<ModelMultiplySession*>(parameter) :
        reinterpret_cast<ModelMultiplySession*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(session));
    return session;
}
static void MultiplyRefused(MMDApp* app, HWND dialog, MmsResult result) {
    const int language = app->EnglishUI();
    const bool argument = result == MMS_E_ARGUMENT;
    const wchar_t* text = language == kUiChinese ? (argument ? L"请输入有效数字。计算结果必须有限，且在当前通道支持范围内。" :
        L"当前编辑状态已改变或暂时不可用。请取消本次调整后重试。") : language ?
        (argument ? L"Enter finite valid numbers within the channel's supported range." :
        L"The editing state has changed or is unavailable. Cancel this adjustment and try again.") :
        (argument ? L"チャンネルの対応範囲内で、有限の有効な数値を入力してください。" :
        L"編集状態が変更されたか、現在利用できません。キャンセルしてから再試行してください。");
    MessageBoxW(dialog, text, language == kUiChinese ? L"关键帧调整" : language ? L"Keyframe adjustment" : L"フレーム補正",
        MB_OK | MB_ICONWARNING);
}
#endif
INT_PTR CALLBACK BoneFrameMultiplyDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {  // was Sub44D3F0, VA 0x0044D3F0
    (void)lParam;
    MMDApp* app = g_Block;
#if !MMD_ORIGINAL_PROFILE
    auto* session = MultiplySession(hDlg, msg, lParam);
    if (session) app = session->app;
#endif
    if (msg == WM_INITDIALOG) {
        MakeDialogTopmostIfRequested(app, hDlg);
        LocalizeChineseDialog(app, hDlg);
        AddBoneMultiplyScaleRows(app, hDlg);
        for (int i = 0; i < 12; i += 2) {
            PrefillEdit(hDlg, panel::kBoneMulPosXScaleEdit + i, "1.0");
        }
        for (int j = 0; j < 12; j += 2) {
            PrefillEdit(hDlg, panel::kBoneMulPosXOffsetEdit + j, "0.0");
        }
        SelectAllEdit(hDlg, panel::kBoneMulPosXScaleEdit);
        return 0;
    }
    if (msg == WM_COMMAND) {
        const WORD id = LOWORD(wParam);
        if (id == 1) {
#if !MMD_ORIGINAL_PROFILE
            if (session) {
                if (const auto result = session->plan->ReadDialog(hDlg)) { MultiplyRefused(app, hDlg, result); return TRUE; }
            } else
#endif
            if (!ApplyBoneFrameScaleAdd(app, hDlg)) {
                const int language = app->EnglishUI();
                MessageBoxW(hDlg, language == kUiChinese ?
                    L"请输入有效数字。所有选中帧的缩放结果必须大于 0，且在支持范围内。" :
                    language ? L"Enter valid numbers. The resulting scale of every selected key must be positive and within the supported range." :
                    L"有効な数値を入力してください。選択した全フレームの拡大率は正の値で、対応範囲内である必要があります。",
                    language == kUiChinese ? L"骨骼帧调整" : language ? L"Bone frame adjustment" : L"ボーンフレーム補正",
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(hDlg, 1);
            return 0;
        }
        if (id == 2) {
            EndDialog(hDlg, 2);
            return 0;
        }
    }
    return 0;
}

// ===========================================================================
// 0x44D510 - case 252 facial-multiply dialog (template caption "multiply of
// facial expression" / 表情大きさ補正; 0x295 EN / 0x283 JP).
// WM_INITDIALOG: same top-most gate, but only edits 686 ("1.0") and 687
// ("0.0") are prefilled; focus 686 + select all.
// WM_COMMAND: id 1 -> sub_43E680 apply + EndDialog(1); id 2 -> EndDialog(2).
// Reference: MikuMikuDance.exe sub_44D510.
// ===========================================================================
INT_PTR CALLBACK FacialMultiplyDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                       LPARAM lParam) {  // was Sub44D510, VA 0x0044D510
    (void)lParam;
    MMDApp* app = g_Block;
#if !MMD_ORIGINAL_PROFILE
    auto* session = MultiplySession(hDlg, msg, lParam);
    if (session) app = session->app;
#endif
    if (msg == WM_INITDIALOG) {
        MakeDialogTopmostIfRequested(app, hDlg);
        PrefillEdit(hDlg, panel::kMorphMulScaleEdit, "1.0");
        PrefillEdit(hDlg, panel::kMorphMulOffsetEdit, "0.0");
        SelectAllEdit(hDlg, panel::kMorphMulScaleEdit);
        return 0;
    }
    if (msg == WM_COMMAND) {
        const WORD id = LOWORD(wParam);
        if (id == 1) {
#if !MMD_ORIGINAL_PROFILE
            if (session) {
                if (const auto result = session->plan->ReadDialog(hDlg)) { MultiplyRefused(app, hDlg, result); return TRUE; }
            } else
#endif
            ApplyMorphScaleAdd(app, hDlg);  // 0x43E680
            EndDialog(hDlg, 1);
            return 0;
        }
        if (id == 2) {
            EndDialog(hDlg, 2);
            return 0;
        }
    }
    return 0;
}

// ===========================================================================
// 0x44C7F0 - case 253 edge-thickness dialog (template caption "thickness of
// edge line" / エッジ太さ設定, scale 0..2; 0x296 EN / 0x285 JP), modeless.
// WM_INITDIALOG: top-most gate; subclass the thickness edit 646 with
// sub_42E270 (old proc saved at app+0xA0B48); fill 646 with "%3.2f" of
// sub_41E950 (current thickness) and set trackbar 647 (range 0..200, tick
// 0x64, pos thickness*100).
// WM_COMMAND id 2: DestroyWindow + clear app+0xA0B44, return 1.
// WM_HSCROLL: TBM_GETPOS/100 -> sub_41E980, refresh 646, return 1.
// Reference: MikuMikuDance.exe sub_44C7F0.
// ===========================================================================
namespace {
// Trackbar thumb bounds are cached until painting. Compute all labels from
// the channel and thumb size without temporarily moving the slider.
void LayoutEdgeThicknessScale(HWND dialog) {
    HWND slider=GetDlgItem(dialog,panel::kEdgeThicknessSlider);
    if(!slider)return;
    const int minimum=static_cast<int>(SendMessageW(slider,TBM_GETRANGEMIN,0,0));
    const int maximum=static_cast<int>(SendMessageW(slider,TBM_GETRANGEMAX,0,0));
    if(maximum<=minimum)return;
    RECT channel{},thumb{};
    SendMessageW(slider,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
    SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
    const int thumbWidth=thumb.right-thumb.left;
    const int start=channel.left+thumbWidth/2;
    const int end=channel.right-(thumbWidth+1)/2;
    const int values[]={minimum,minimum+(maximum-minimum)/2,maximum};
    for(int index=0;index<3;++index){
        HWND label=GetDlgItem(dialog,1646+index);
        if(!label)continue;
        POINT centre={start+MulDiv(end-start,values[index]-minimum,maximum-minimum),0};
        MapWindowPoints(slider,dialog,&centre,1);
        RECT bounds{};GetWindowRect(label,&bounds);
        MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&bounds),2);
        SetWindowPos(label,nullptr,centre.x-(bounds.right-bounds.left)/2,bounds.top,0,0,
                     SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    }
}
}
INT_PTR CALLBACK EdgeThicknessDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {  // was Sub44C7F0, VA 0x0044C7F0
    auto* app=reinterpret_cast<MMDApp*>(GetWindowLongPtrW(hDlg,DWLP_USER));
    if(msg==WM_INITDIALOG){app=lParam?reinterpret_cast<MMDApp*>(lParam):g_Block;SetWindowLongPtrW(hDlg,DWLP_USER,reinterpret_cast<LONG_PTR>(app));app->state.edgeThicknessDialog=hDlg;}
    if(!app)return 0;
#if !MMD_ORIGINAL_PROFILE
    INT_PTR controlled=0;
    if(plugins::HandleBoundModelOutlineDialog(*app,hDlg,msg,wParam,lParam,controlled))return controlled;
#endif
    if(msg==WM_DESTROY&&app->state.edgeThicknessDialog==hDlg)app->state.edgeThicknessDialog=nullptr;
    switch (msg) {
    case WM_INITDIALOG: {
        MakeDialogTopmostIfRequested(app, hDlg);
        app->EdgeThicknessEditProc() =
            reinterpret_cast<WNDPROC>(
                GetWindowLongPtrA(GetDlgItem(hDlg, panel::kEdgeThicknessEdit), GWLP_WNDPROC));
        SetWindowLongPtrA(GetDlgItem(hDlg, panel::kEdgeThicknessEdit), GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(EdgeThicknessEditSubclassProc));
        const float fps = GetEdgeThickness(app);  // 0x41E950
        char buf[0x100];
        sprintf_s(buf, 0x100, "%3.2f", static_cast<double>(fps));
        PrefillEdit(hDlg, panel::kEdgeThicknessEdit, buf);
        SendMessageA(GetDlgItem(hDlg, panel::kEdgeThicknessSlider), TBM_SETRANGEMIN, 0, 0);
        SendMessageA(GetDlgItem(hDlg, panel::kEdgeThicknessSlider), TBM_SETRANGEMAX, 0, 200);
        SendMessageA(GetDlgItem(hDlg, panel::kEdgeThicknessSlider), TBM_SETTICFREQ, 0x64, 0);
        SendMessageA(GetDlgItem(hDlg, panel::kEdgeThicknessSlider), TBM_SETPOS, 1,
                     std::isfinite(fps)?int((std::clamp)(double(fps)*100.0,0.0,200.0)):0);
        LayoutEdgeThicknessScale(hDlg);
#if !MMD_ORIGINAL_PROFILE
        plugins::InitializeBoundModelOutline(*app,hDlg);
#endif
        break;
    }
    case WM_SIZE:
    case WM_THEMECHANGED:
    case WM_DPICHANGED:
        LayoutEdgeThicknessScale(hDlg);
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == 2) {
            DestroyWindow(hDlg);
            return 1;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 1;
    case WM_HSCROLL: {
        const double v =
            static_cast<double>(
                SendMessageA(GetDlgItem(hDlg, panel::kEdgeThicknessSlider), TBM_GETPOS, 0, 0)) /
            100.0;
        SetEdgeThickness(app, static_cast<float>(v));  // 0x41E980
        char buf[0x100];
        sprintf_s(buf, 0x100, "%3.2f", v);
        SendMessageA(GetDlgItem(hDlg, panel::kEdgeThicknessEdit), EM_SETSEL, 0,
                     GetWindowTextLengthA(GetDlgItem(hDlg, panel::kEdgeThicknessEdit)));
        SendMessageA(GetDlgItem(hDlg, panel::kEdgeThicknessEdit), EM_REPLACESEL, 0,
                     (LPARAM)buf);
        return 1;
    }
    default:
        break;
    }
    return 0;
}

// ===========================================================================
// 0x43C9A0 - case 261 "enhance model" dialog (template caption "toon
// texture" / トゥーンテクスチャ; 0x2AA EN / 0x2A9 JP).
// WM_INITDIALOG: top-most gate + sub_41E9C0 init.
// WM_COMMAND: id 1 -> EndDialog(1) when sub_41EA20 succeeds else EndDialog(2);
// id 2 -> EndDialog(2).
// Reference: MikuMikuDance.exe sub_43C9A0.
// ===========================================================================
INT_PTR CALLBACK EnhanceModelDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {  // was Sub43C9A0, VA 0x0043C9A0
    (void)lParam;
    if (msg == WM_INITDIALOG) {
        MMDApp* app = g_Block;
        MakeDialogTopmostIfRequested(app, hDlg);
        LocalizeChineseDialog(app, hDlg);
        FillEnhanceModelNameEdits(app, hDlg);  // 0x41E9C0
        return 0;
    }
    if (msg != WM_COMMAND) {
        return 0;
    }
    const WORD id = LOWORD(wParam);
    if (id == 1) {
        if (CollectToonFileNames(hDlg) != 0) {  // 0x41EA20
            EndDialog(hDlg, 1);
        } else {
            EndDialog(hDlg, 2);
        }
        return 0;
    }
    if (id == 2) {
        EndDialog(hDlg, 2);
        return 0;
    }
    return 0;
}

// ===========================================================================
// 0x464BD0 - case 259 model-edge dialog (0x29A EN / 0x299 JP), modeless at
// app+0xA0B50.  WM_INITDIALOG: subclass edits 667/672/676/680 with
// sub_45ECC0 (old proc of 667 saved at app+0xA0B54), sub_43C430 init.
// WM_COMMAND: sets app+0xA0B6C = 1.  id 1/2 close (sub_45F240 apply,
// DestroyWindow, app+0xA0B50 = 0, re-enable main-window combo 0x1B4; id 1
// additionally shows the "preserve edit result" box and sets app+0xA0B64).
// ids 0x29E/0x29F/0x2A2/0x2A3/0x2A6/679 are the prev/next buttons of the
// three combos 669/673/677 (CB_SETCURSEL wrap-around + refresh helper).
// CBN_SELCHANGE of 669/673/677 refreshes the matching combo helper.
// Reference: MikuMikuDance.exe sub_464BD0.
// ===========================================================================
INT_PTR CALLBACK ModelEdgeDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                  LPARAM lParam) {  // was Sub464BD0, VA 0x00464BD0
    MMDApp* app = g_Block;
    const HWND hCtrl = reinterpret_cast<HWND>(lParam);
    if (msg == WM_INITDIALOG) {
        app->ModelEdgeEditProc() =
            reinterpret_cast<WNDPROC>(
                GetWindowLongPtrA(GetDlgItem(hDlg, panel::kModelNameEnEdit), GWLP_WNDPROC));
        SetWindowLongPtrA(GetDlgItem(hDlg, panel::kModelNameEnEdit), GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(ModelEdgeEditSubclassProc));
        SetWindowLongPtrA(GetDlgItem(hDlg, panel::kBoneNameEnEdit), GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(ModelEdgeEditSubclassProc));
        SetWindowLongPtrA(GetDlgItem(hDlg, panel::kMorphNameEnEdit), GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(ModelEdgeEditSubclassProc));
        SetWindowLongPtrA(GetDlgItem(hDlg, panel::kGroupNameEnEdit), GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(ModelEdgeEditSubclassProc));
        InitModelEdgeDialog(hDlg);  // 0x43C430
        return 0;
    }
    if (msg != WM_COMMAND) {
        return 0;
    }
    // dirty write: the frame-edit dialog deliberately invalidates the
    // cached time (original blob reuse of app+0xA0B6C == timeNowHigh)
    reinterpret_cast<std::int32_t&>(app->state.timeNowHigh) = 1;
    switch (LOWORD(wParam)) {
    case 1:
    case 2:
        ApplyModelEdgeDialog(hDlg);  // 0x45F240
        DestroyWindow(hDlg);
        app->FrameRangeDialog() = nullptr;
        EnableWindow(GetDlgItem(app->state.hwnd, panel::kMainComboModel), TRUE);
        if (LOWORD(wParam) == 1) {
            if (app->state.englishUI != 0) {
                LegacyUiMessageBoxA(app->state.hwnd,
                            "Please preserve the edit result as a new model by "
                            "'save enhanced model'.",
                            "enhance model", 0);
            } else {
                LegacyUiMessageBoxA(app->state.hwnd, kMsgEnhanceModelJp,
                            kCaptionEnhanceModelJp, 0);
            }
            app->EnhancedModelDirty() = 1;
        }
        return 1;
    case 0x29E: {  // combo 669 next button
        const int cur = app->ModelEdgeComboCursor(0);
        const int count = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, panel::kBoneNameCombo), CB_GETCOUNT, 0, 0));
        SendMessageA(GetDlgItem(hDlg, panel::kBoneNameCombo), CB_SETCURSEL,
                     cur + 1 >= count ? 0 : cur + 1, 0);
        SelectModelEdgeBone(hDlg);  // 0x45F050
        return 0;
    }
    case 0x29F: {  // combo 669 prev button
        int sel = app->ModelEdgeComboCursor(0) - 1;
        if (sel < 0) {
            sel = static_cast<int>(
                      SendMessageA(GetDlgItem(hDlg, panel::kBoneNameCombo), CB_GETCOUNT,
                                   0, 0)) -
                  1;
        }
        SendMessageA(GetDlgItem(hDlg, panel::kBoneNameCombo), CB_SETCURSEL, sel, 0);
        SelectModelEdgeBone(hDlg);  // 0x45F050 (LABEL_16)
        return 0;
    }
    case 0x2A2: {  // combo 673 next button
        const int cur = app->ModelEdgeComboCursor(1);
        const int count = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, panel::kMorphNameCombo), CB_GETCOUNT, 0, 0));
        SendMessageA(GetDlgItem(hDlg, panel::kMorphNameCombo), CB_SETCURSEL,
                     cur + 1 >= count ? 0 : cur + 1, 0);
        SelectModelEdgeMorph(hDlg);  // 0x45EF10
        return 0;
    }
    case 0x2A3: {  // combo 673 prev button
        int sel = app->ModelEdgeComboCursor(1) - 1;
        if (sel < 0) {
            sel = static_cast<int>(
                      SendMessageA(GetDlgItem(hDlg, panel::kMorphNameCombo), CB_GETCOUNT,
                                   0, 0)) -
                  1;
        }
        SendMessageA(GetDlgItem(hDlg, panel::kMorphNameCombo), CB_SETCURSEL, sel, 0);
        SelectModelEdgeMorph(hDlg);  // 0x45EF10 (LABEL_23)
        return 0;
    }
    case 0x2A6: {  // combo 677 next button
        const int cur = app->ModelEdgeComboCursor(2);
        const int count = static_cast<int>(
            SendMessageA(GetDlgItem(hDlg, panel::kGroupNameCombo), CB_GETCOUNT, 0, 0));
        SendMessageA(GetDlgItem(hDlg, panel::kGroupNameCombo), CB_SETCURSEL,
                     cur + 1 >= count ? 0 : cur + 1, 0);
        SelectModelEdgeGroup(hDlg);  // 0x45EDC0
        return 0;
    }
    case 679: {  // combo 677 prev button
        int sel = app->ModelEdgeComboCursor(2) - 1;
        if (sel < 0) {
            sel = static_cast<int>(
                      SendMessageA(GetDlgItem(hDlg, panel::kGroupNameCombo), CB_GETCOUNT,
                                   0, 0)) -
                  1;
        }
        SendMessageA(GetDlgItem(hDlg, panel::kGroupNameCombo), CB_SETCURSEL, sel, 0);
        SelectModelEdgeGroup(hDlg);  // 0x45EDC0
        return 0;
    }
    default:
        break;
    }
    if (HIWORD(wParam) == 1 /*CBN_SELCHANGE*/) {
        if (hCtrl == GetDlgItem(hDlg, panel::kBoneNameCombo)) {
            SelectModelEdgeBone(hDlg);  // 0x45F050 (LABEL_16)
            return 0;
        }
        if (hCtrl == GetDlgItem(hDlg, panel::kMorphNameCombo)) {
            SelectModelEdgeMorph(hDlg);  // 0x45EF10 (LABEL_23)
            return 0;
        }
        if (hCtrl == GetDlgItem(hDlg, panel::kGroupNameCombo)) {
            SelectModelEdgeGroup(hDlg);  // 0x45EDC0
        }
    }
    return 0;
}

// Menus 288 and 289 use the same one-based scratch representation and actual
// list controls. Only the selected native order field differs.
INT_PTR ModelOrderDlgProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<ModelOrderDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    const bool close = message == WM_CLOSE || (message == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE) ||
        (message == WM_COMMAND && LOWORD(wp) == IDCANCEL);
    if (state && bake::UiLocked() && (close || message == WM_COMMAND)) {
        state->closeRequested |= close;
        return TRUE;
    }
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<ModelOrderDialogState*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        if (!state || !InitModelOrderRows(*state, dialog)) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (state->closeRequested) { EndDialog(dialog, IDCANCEL); return TRUE; }
        MakeDialogTopmostIfRequested(&state->app, dialog);
        return FALSE;
    }
    if (!state) return FALSE;
    // X and Alt+F4 must use the same repair gate as the Cancel button.
    if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE)) {
        if (RepairModelOrderUi(*state)) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    const WORD command = LOWORD(wp);
    if (command == 630 || command == 631) {
        if (!RepairModelOrderRows(*state, dialog)) return TRUE;
        const int row = static_cast<int>(SendDlgItemMessageW(dialog, panel::kOrderListBox, LB_GETCURSEL, 0, 0));
        if (row < 0 || row >= state->count) return TRUE;
        const int target = row + (command == 630 ? -1 : 1);
        if (target < 0 || target >= state->count) return TRUE;
        try {
            bake::ScopedUiInputGate gate;
            HWND list = GetDlgItem(dialog, panel::kOrderListBox);
            const auto size = SendMessageW(list, LB_GETTEXTLEN, row, 0);
            if (size < 0) return TRUE;
            std::wstring name(static_cast<std::size_t>(size) + 1, L'\0');
            if (SendMessageW(list, LB_GETTEXT, row, reinterpret_cast<LPARAM>(name.data())) != size) return TRUE;
            // Allocate the duplicate before deleting its original. Failure to
            // insert leaves every original row and scratch identity intact.
            const int insertAt = target < row ? target : target + 1;
            const auto inserted = SendMessageW(list, LB_INSERTSTRING, insertAt, reinterpret_cast<LPARAM>(name.c_str()));
            if (inserted < 0) return TRUE;
            const int removeAt = insertAt <= row ? row + 1 : row;
            if (inserted != insertAt || SendMessageW(list, LB_DELETESTRING, removeAt, 0) == LB_ERR) {
                if (SendMessageW(list, LB_DELETESTRING, inserted, 0) == LB_ERR) {
                    state->listNeedsRepair = true;
                    RepairModelOrderRows(*state, dialog);
                }
                SendMessageW(list, LB_SETCURSEL, row, 0);
                return TRUE;
            }
            SendMessageW(list, LB_SETCURSEL, target, 0);
            const auto previous = state->order[row + 1];
            state->order[row + 1] = state->order[target + 1];
            state->order[target + 1] = previous;
            if (state->closeRequested) {
                state->closeRequested = false;
                PostMessageW(dialog, WM_CLOSE, 0, 0);
            }
        } catch (...) { return TRUE; }
        return TRUE;
    }
    if (command == 632) {
        if (RepairModelOrderRows(*state, dialog) && ApplyModelOrderDialog(*state)) EndDialog(dialog, IDOK);
        else if (state->closeRequested && RepairModelOrderUi(*state)) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (command == IDCANCEL) {
        if (RepairModelOrderUi(*state)) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK ModelCalculateOrderDlgProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    return ModelOrderDlgProc(dialog, message, wp, lp);
}


// ===========================================================================
// 0x479E90 - case 266 gravity-setting dialog (template caption "gravity
// setting" / 重力設定; 0x323 EN / 0x322 JP), modeless
// at app+0xA0CCC.  WM_INITDIALOG: top-most gate; subclass edits 709..713
// with sub_466370 (old proc of 709 saved at app+0xA0C50); sub_423160 init.
// WM_COMMAND: checkbox 731 mirrors app+0xA0CD4 and enables edit 713;
// EN_UPDATE on 709..713 -> atof + sub_45FD80(idx, f) (710..712 also sync
// their trackbars 637..639); id 1 -> sub_460080; id 2 -> DestroyWindow,
// app+0xA0CCC = 0, sub_412330, return 1.  WM_HSCROLL on trackbars
// 637/638/639 -> TBM_GETPOS/100 -> "%3.2f" into 710/711/712 + sub_45FD80.
// Reference: MikuMikuDance.exe sub_479E90.
// ===========================================================================
INT_PTR CALLBACK GravitySettingDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                       LPARAM lParam) {  // was Sub479E90, VA 0x00479E90
    MMDApp* app = g_Block;
    const HWND hCtrl = reinterpret_cast<HWND>(lParam);
    if (msg == WM_INITDIALOG) {
        LocalizeChineseDialog(app, hDlg);
        MakeDialogTopmostIfRequested(app, hDlg);
        for (int i = 709; i <= 713; ++i) {
            SetWindowSubclass(GetDlgItem(hDlg, i), GravitySettingEditSubclassProc,
                              0, reinterpret_cast<DWORD_PTR>(app));
        }
        InitGravityDialog(hDlg);  // 0x423160
        return 0;
    }
    if (msg == WM_COMMAND) {
        const WORD id = LOWORD(wParam);
        if (id == 731) {
            if (IsDlgButtonChecked(hDlg, panel::kGravityNoiseCheckbox) == 1) {
                app->state.gravityNoiseEnabled = 1;
                EnableWindow(GetDlgItem(hDlg, panel::kGravityNoiseEdit), TRUE);
            } else {
                app->state.gravityNoiseEnabled = 0;
                EnableWindow(GetDlgItem(hDlg, panel::kGravityNoiseEdit), FALSE);
            }
        } else if (HIWORD(wParam) == 768 /*EN_UPDATE*/) {
            char buf[0x100];
            if (hCtrl == GetDlgItem(app->GravitySettingDialog(), panel::kGravityAccelEdit)) {
                GetWindowTextA(hCtrl, buf, 256);
                SetGravityChannel(app,0, static_cast<float>(atof(buf)));  // 0x45FD80
            } else if (hCtrl == GetDlgItem(app->GravitySettingDialog(), panel::kGravityDirXEdit)) {
                GetWindowTextA(hCtrl, buf, 256);
                const float f = static_cast<float>(atof(buf));
                SendMessageA(GetDlgItem(app->GravitySettingDialog(), panel::kNumInputPosXEdit),
                             TBM_SETPOS, 1, (int)(f * 100.0));
                SetGravityChannel(app,1, f);  // 0x45FD80
            } else if (hCtrl == GetDlgItem(app->GravitySettingDialog(), panel::kGravityDirYEdit)) {
                GetWindowTextA(hCtrl, buf, 256);
                const float f = static_cast<float>(atof(buf));
                SendMessageA(GetDlgItem(app->GravitySettingDialog(), panel::kNumInputPosYEdit),
                             TBM_SETPOS, 1, (int)(f * 100.0));
                SetGravityChannel(app,2, f);  // 0x45FD80
            } else if (hCtrl == GetDlgItem(app->GravitySettingDialog(), panel::kGravityDirZEdit)) {
                GetWindowTextA(hCtrl, buf, 256);
                const float f = static_cast<float>(atof(buf));
                SendMessageA(GetDlgItem(app->GravitySettingDialog(), panel::kNumInputPosZEdit),
                             TBM_SETPOS, 1, (int)(f * 100.0));
                SetGravityChannel(app,3, f);  // 0x45FD80
            } else if (hCtrl == GetDlgItem(app->GravitySettingDialog(), panel::kGravityNoiseEdit)) {
                GetWindowTextA(hCtrl, buf, 256);
                SetGravityChannel(app,4, static_cast<float>(atof(buf)));  // 0x45FD80
            }
        } else if (id == 1) {
            ApplyGravitySettingDialog(app);  // 0x460080
        } else if (id == 2) {
            DestroyWindow(hDlg);
            app->GravitySettingDialog() = 0;
            ApplyGravityTrack(app);  // 0x412330
            return 1;
        }
        return 0;
    }
    if (msg != WM_HSCROLL) {
        return 0;
    }
    if (hCtrl == GetDlgItem(hDlg, panel::kNumInputPosXEdit)) {
        const double v =
            static_cast<double>(
                SendMessageA(hCtrl, TBM_GETPOS, 0, 0)) /
            100.0;
        char buf[0x100];
        sprintf_s(buf, 0x100, "%3.2f", v);
        SetWindowTextA(GetDlgItem(hDlg, panel::kGravityDirXEdit), buf);
        SetGravityChannel(app,1, static_cast<float>(v));  // 0x45FD80
    } else if (hCtrl == GetDlgItem(hDlg, panel::kNumInputPosYEdit)) {
        const double v =
            static_cast<double>(
                SendMessageA(hCtrl, TBM_GETPOS, 0, 0)) /
            100.0;
        char buf[0x100];
        sprintf_s(buf, 0x100, "%3.2f", v);
        SetWindowTextA(GetDlgItem(hDlg, panel::kGravityDirYEdit), buf);
        SetGravityChannel(app,2, static_cast<float>(v));  // 0x45FD80
    } else if (hCtrl == GetDlgItem(hDlg, panel::kNumInputPosZEdit)) {
        const double v =
            static_cast<double>(
                SendMessageA(hCtrl, TBM_GETPOS, 0, 0)) /
            100.0;
        char buf[0x100];
        sprintf_s(buf, 0x100, "%3.2f", v);
        SetWindowTextA(GetDlgItem(hDlg, panel::kGravityDirZEdit), buf);
        SetGravityChannel(app,3, static_cast<float>(v));  // 0x45FD80
    }
    return 1;
}

// ===========================================================================
// 0x461CE0 - case 275 physics ON/OFF frame dialog (template caption "change
// physics ON/OFF frame" / 物理ON/OFFフレーム変換; 0x32D EN / 0x32C JP).
// WM_INITDIALOG: CB_RESETCONTENT(669), CB_ADDSTRING "ON (X mark)" / JP
// "ON (X印)" then "OFF", CB_SETCURSEL(0).  WM_COMMAND: id 1 -> sub_4403C0
// (CB_GETCURSEL != 0) then EndDialog(1); id 2 -> EndDialog(1) as well (the
// caller of case 275 never inspects the result).
// Reference: MikuMikuDance.exe sub_461CE0.
// ===========================================================================
INT_PTR CALLBACK PhysicsOnOffFrameDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {  // was Sub461CE0, VA 0x00461CE0
    (void)lParam;
    if (msg == WM_INITDIALOG) {
        LocalizeChineseDialog(g_Block, hDlg);
        SendMessageA(GetDlgItem(hDlg, panel::kBoneNameCombo), CB_RESETCONTENT, 0, 0);
        const char* first =
            g_Block->state.englishUI != 0
                ? "ON (X mark)"
                : kOnXMarkJp;
        AddUiComboText(g_Block, GetDlgItem(hDlg, panel::kBoneNameCombo), first);
        AddUiComboText(g_Block, GetDlgItem(hDlg, panel::kBoneNameCombo), kOff);
        SendMessageA(GetDlgItem(hDlg, panel::kBoneNameCombo), CB_SETCURSEL, 0, 0);
        return 0;
    }
    if (msg == WM_COMMAND) {
        const WORD id = LOWORD(wParam);
        if (id == 1) {
            if (SendMessageA(GetDlgItem(hDlg, panel::kBoneNameCombo), CB_GETCURSEL, 0,
                             0) != 0) {
                ApplyPhysicsOnOff(1);  // 0x4403C0
            } else {
                ApplyPhysicsOnOff(0);  // 0x4403C0
            }
            EndDialog(hDlg, 1);
            return 0;
        }
        if (id == 2) {
            EndDialog(hDlg, 1);
            return 0;
        }
    }
    return 0;
}

// ===========================================================================
// 0x40FBC0 - case 300 camera numeric-input dialog (template caption
// "numeric input(camera)" / 数値入力(カメラ); 0x298 EN / 0x289 JP).
// WM_INITDIALOG: top-most gate; the seven temp floats app+0xA0B28..0xA0B40
// (set by the case-300 camera path: posx/y/z, -cam2.., cam3.., cam4..,
// -camangle) are written as "%7.3f" into edits 637..642 + 644 via
// EM_REPLACESEL; focus 637 + select all.
// WM_COMMAND: id 1 -> read all seven edits back into the temps (atof),
// EndDialog(1); id 2 -> EndDialog(2).  The caller converts back to radians.
// Reference: MikuMikuDance.exe sub_40FBC0.
// ===========================================================================
// Validate the complete dialog before publishing any of its values.
bool ReadNumericDialog(HWND dialog, MMDApp* app, int count) {
    constexpr int ids[] = {637,638,639,640,641,642,644};
    float values[7]{};
    for(int i=0;i<count;++i) {
        char text[128]{};GetDlgItemTextA(dialog,ids[i],text,sizeof(text));
        char* end=nullptr;const double value=std::strtod(text,&end);
        const bool empty=end==text;
        while(*end&&std::isspace(static_cast<unsigned char>(*end))) ++end;
        if(empty||*end||!std::isfinite(value)||std::fabs(value)>(std::numeric_limits<float>::max)()) {
            SetFocus(GetDlgItem(dialog,ids[i]));
            SendDlgItemMessageW(dialog,ids[i],EM_SETSEL,0,-1);
            MessageBeep(MB_ICONWARNING);return false;
        }
        values[i]=static_cast<float>(value);
    }
    for(int i=0;i<count;++i) app->RotationDialogTemp(i)=values[i];
    return true;
}

INT_PTR CALLBACK CameraNumericInputDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                           LPARAM lParam) {  // was Sub40FBC0, VA 0x0040FBC0
    (void)lParam;
    MMDApp* app = g_Block;
    if (msg == WM_INITDIALOG) {
        MakeDialogTopmostIfRequested(app, hDlg);
        static const std::size_t kEditIds[7] = {637, 638, 639, 640, 641, 642, 644};
        char buf[64];
        for (int i = 0; i < 7; ++i) {
            sprintf_s(buf, sizeof(buf), "%.9g",
                      static_cast<double>(app->RotationDialogTemp(i)));
            PrefillEdit(hDlg, static_cast<int>(kEditIds[i]), buf);
        }
        SelectAllEdit(hDlg, panel::kNumInputPosXEdit);
        return 0;
    }
    if (msg == WM_COMMAND) {
        const WORD id = LOWORD(wParam);
        if (id == 1) {
            if (ReadNumericDialog(hDlg,app,7)) EndDialog(hDlg, 1);
        } else if (id == 2) {
            EndDialog(hDlg, 2);
        }
    }
    return 0;
}

// ===========================================================================
// 0x40F860 - case 300 bone numeric-input dialog (template caption "numeric
// input(bone)" / 数値入力(ボーン); 0x297 EN / 0x288 JP).
// WM_INITDIALOG: top-most gate; the six temp floats app+0xA0B28..0xA0B3C
// (pos x/y/z + the three euler angles in degrees, set by the case-300 bone
// path) are written as "%7.3f" into edits 637..642; focus 637 + select all.
// WM_COMMAND: id 1 -> read the six edits back (atof), EndDialog(1);
// id 2 -> EndDialog(2).  The caller converts back to radians.
// Reference: MikuMikuDance.exe sub_40F860.
// ===========================================================================
INT_PTR CALLBACK BoneNumericInputDlgProc(HWND hDlg, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {  // was Sub40F860, VA 0x0040F860
    (void)lParam;
    MMDApp* app = g_Block;
    if (msg == WM_INITDIALOG) {
        MakeDialogTopmostIfRequested(app, hDlg);
        static const std::size_t kEditIds[6] = {637, 638, 639, 640, 641, 642};
        char buf[64];
        for (int i = 0; i < 6; ++i) {
            sprintf_s(buf, sizeof(buf), "%.9g",
                      static_cast<double>(app->RotationDialogTemp(i)));
            PrefillEdit(hDlg, static_cast<int>(kEditIds[i]), buf);
        }
        SelectAllEdit(hDlg, panel::kNumInputPosXEdit);
        return 0;
    }
    if (msg == WM_COMMAND) {
        const WORD id = LOWORD(wParam);
        if (id == 1) {
            if (ReadNumericDialog(hDlg,app,6)) EndDialog(hDlg, 1);
        } else if (id == 2) {
            EndDialog(hDlg, 2);
        }
    }
    return 0;
}

// Menu 288: publish the display/selector permutation and refresh by object
// identity. Selecting the same model again would reset live editing state.
INT_PTR CALLBACK ModelDisplayOrderDlgProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    return ModelOrderDlgProc(dialog, message, wp, lp);
}

}  // namespace

// ===========================================================================
// "fine shadow mode" notice - the Ctrl+S branch of the 'S' letter hotkey
// (x64 main pump sub_7FF7CB4474F0 @ 0x7FF7CB44F625).
// ===========================================================================
// The branch is NOT a menu command case: it lives in the main pump's letter
// hotkey ladder (x86 0x4726B2, right after the 0x46FF02 key-poll call;
// ported in src/app/key_ladder.cpp, which calls this from its 'G' block).
// Original block:
//   gate: (GetFocus() == main window || separate-window byte 0x9FCDD) &&
//         dialogFlags[7] == 1 ('G' pressed this frame - the cell the
//         PollKey letter table assigns to 'g'/'G') &&
//         GetFocus() != the frame-number edit 0x22A
//   - Shift held (shiftModifierState == 3): toggle the byte at
//     x64 app+0xA1DC8 (shadow render-mode toggle; x86 twin VA unrecovered)
//   - Ctrl held (ctrlModifierState == 3, the "fine" modifier - NOT the
//     selection mode): THIS notice
//   - plain 'S': frame-number edit 0x22A read-back, seek and echo into
//     0x1A1 (same body as command case 553)
// EN: caption "fine shadow mode" (0x7FF7CB54D658), text "This command is
// changed for \"Fshadow\" button." (0x7FF7CB54D670); JP: byte_7FF7CB54D6A0 /
// byte_7FF7CB54D6B0 (constants above), gated by the English-UI byte.
// =========================================================================//
void FineShadowModeNotice(MMDApp* app) {
    const bool english = app->state.englishUI != 0;
    LegacyUiMessageBoxA(app->state.hwnd,
                english
                    ? "This command is changed for \"Fshadow\" button."
                    : kMsgFineShadowJp,
                english ? "fine shadow mode" : kCaptionFineShadowJp,
                0);
}

// (defined in effect_api.cpp; declared at namespace scope - an
// anonymous-namespace declaration would be an undefined internal entity)
void WideToSjisPath(char* dst, const wchar_t* src, rsize_t size);  // 0x407910

namespace {

// The persistence module keeps the record predicate to itself, so the shape is
// mirrored here to answer one question: would saving the project right now
// already store `selected` as the sort limit?  A limit record is usable when it
// is one byte inside the enum range; any other content is a value from a newer
// writer or a damaged record.
bool UsableLimitRecord(const pmm_extension::Record* record) {
    return record != nullptr && record->value.size() == 1 &&
           record->value[0] <=
               static_cast<std::uint8_t>(TransparentSortPrecision::Off);
}

// True when picking `selected` still has work to do in the project file, even
// though the running value is already `selected`: a limit byte this build
// cannot read, or one that differs from the selection, is replaced rather than
// carried over.  Only the precision field is consulted; the retired toggle of
// the earlier test format is not migrated here and stays inert in the file.  A
// missing record and a limit byte equal to the selection both normalize
// themselves on save, so they need no edit flag.  The caller asks only while
// this field has not been edited yet: after an edit the raw records are stale
// by design and must not be consulted again.
bool StoredLimitNeedsRewrite(const MMDApp& app,
                             TransparentSortPrecision selected) {
    const auto& document = app.PmmProjectSettings().document;
    const auto* limit =
        pmm_extension::Find(document, pmm_settings::kSortingPrecisionKey);
    return limit != nullptr &&
           (!UsableLimitRecord(limit) ||
            limit->value[0] != static_cast<std::uint8_t>(selected));
}

}  // namespace

namespace {
class StillPickerEvents final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileDialogEvents> {
public:
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem* item, FDE_OVERWRITE_RESPONSE* response) override {
        *response = FDEOR_DEFAULT;
        PWSTR path = nullptr;
        const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        path_.reset(path);
        identity_.reset();
        if (SUCCEEDED(result)) identity_ = image_sequence::ReadStillFileIdentity(path);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override {
        identity_.reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override {
        identity_.reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        *response = FDESVR_DEFAULT;
        return S_OK;
    }
    std::optional<image_sequence::StillFileIdentity> ForPath(const wchar_t* path) const {
        if (identity_ && CompareStringOrdinal(path_.get(), -1, path, -1, TRUE) == CSTR_EQUAL)
            return identity_;
        return std::nullopt;
    }

private:
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path_{nullptr, &CoTaskMemFree};
    std::optional<image_sequence::StillFileIdentity> identity_;
};
}

HRESULT ChooseStillImagePath(MMDApp* app, HWND owner, wchar_t* file, std::size_t capacity,
                             std::optional<image_sequence::StillFileIdentity>& overwrite) {
    overwrite.reset();
    Microsoft::WRL::ComPtr<IFileSaveDialog> picker;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&picker));
    if (FAILED(hr)) return hr;
    DWORD options = 0;
    hr = picker->GetOptions(&options);
    if (SUCCEEDED(hr)) hr = picker->SetOptions(options | FOS_FORCEFILESYSTEM |
        FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    const COMDLG_FILTERSPEC types[] = {
        {L"PNG (*.png)", L"*.png"}, {L"JPEG (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"},
        {L"BMP (*.bmp)", L"*.bmp"}, {L"DDS (*.dds)", L"*.dds"},
        {L"DIB (*.dib)", L"*.dib"}, {L"PFM (*.pfm)", L"*.pfm"},
        {L"HDR (*.hdr)", L"*.hdr"},
    };
    if (SUCCEEDED(hr)) hr = picker->SetFileTypes(static_cast<UINT>(std::size(types)), types);
    if (SUCCEEDED(hr)) hr = picker->SetFileTypeIndex(1);
    // Set before Show so the shell updates the extension when the file type changes.
    if (SUCCEEDED(hr)) hr = picker->SetDefaultExtension(L"png");
    if (SUCCEEDED(hr)) hr = picker->SetTitle(LocalizedUiTextW(
        app->EnglishUI() ? L"render to picture file" : kTitleRenderJp));
    const std::wstring directory = (GetMenuState(GetMenu(owner), 0x12D, 0) & 8) != 0
        ? app->DirUser() : std::wstring(app->ExeDir()) + L"\\UserFile";
    Microsoft::WRL::ComPtr<IShellItem> folder;
    if (SUCCEEDED(hr) && SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(),
            nullptr, IID_PPV_ARGS(&folder))))
        hr = picker->SetFolder(folder.Get());
    if (FAILED(hr)) return hr;
    auto events = Microsoft::WRL::Make<StillPickerEvents>();
    if (!events) return E_OUTOFMEMORY;
    DWORD cookie = 0;
    hr = picker->Advise(events.Get(), &cookie);
    if (FAILED(hr)) return hr;
    hr = picker->Show(owner);
    picker->Unadvise(cookie);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IShellItem> result;
    hr = picker->GetResult(&result);
    PWSTR path = nullptr;
    if (SUCCEEDED(hr)) hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (SUCCEEDED(hr)) {
        // The capture pipeline still stores paths in a fixed-size field.
        if (std::wcslen(path) >= capacity) hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
        else {
            wcscpy_s(file, capacity, path);
            overwrite = events->ForPath(path);
        }
    }
    CoTaskMemFree(path);
    return hr;
}

#if !MMD_ORIGINAL_PROFILE
namespace plugins {
bool TryEditBoundPoseNumeric(MMDApp* app, HWND owner) noexcept {
    // The existing dialog uses one application temp array. Refuse recursive
    // entry while its modal pump is active rather than overwriting that array.
    static thread_local bool active = false;
    if (active) return true;
    if (!app) return false;
    auto* history = BoundNativePoseHistory(app);
    if (!history) return false;
    Boundary([&]() -> MmsResult {
        if (g_Block != app) return MMS_E_NOT_READY;
        struct Guard { bool& active; explicit Guard(bool& a) : active(a) { active = true; } ~Guard() { active = false; } } guard(active);
        const auto run = [&](auto& edit, int resource, DLGPROC procedure) -> MmsResult {
            if (const auto result = edit.Begin(*app, *history)) return result;
            const auto values = edit.Values();
            for (unsigned i = 0; i < values.size(); ++i) app->RotationDialogTemp(i) = values[i];
            app->state.enterKeyState = 1;
            const auto result = UiDialogBoxParamA(static_cast<HINSTANCE>(app->HInstance()),
                MAKEINTRESOURCEA(resource), owner, procedure, 0);
            if (result != IDOK) return MMS_E_CANCELLED;
            if (g_Block != app) return MMS_E_STALE;
            auto input = values;
            for (unsigned i = 0; i < input.size(); ++i) input[i] = app->RotationDialogTemp(i);
            return edit.Apply(input);
        };
        if (app->CameraMode()) {
            CameraNumericEdit edit;
            return run(edit, app->EnglishUI() ? 0x298 : 0x289, CameraNumericInputDlgProc);
        }
        BoneNumericEdit edit;
        return run(edit, app->EnglishUI() ? 0x297 : 0x288, BoneNumericInputDlgProc);
    });
    return true;
}
}
#endif

#if !MMD_ORIGINAL_PROFILE
static void RunBoundModelMultiply(MMDApp& app, HWND owner, plugins::ModelMultiplyKind kind) {
    static thread_local bool dialogActive = false;
    if (dialogActive) return;
    const auto result = plugins::Boundary([&]() -> MmsResult {
        auto* history = plugins::BoundNativePoseHistory(&app);
        if (!history) return MMS_E_NOT_READY;
        plugins::NativeModelMultiplyPlan plan;
        if (auto status = plan.Begin(app, kind)) return status;
        const auto generation = plugins::BoundNativePoseGeneration(&app);
        struct DialogScope {
            bool& active;
            explicit DialogScope(bool& value) : active(value) { active = true; }
            ~DialogScope() { active = false; }
        } scope(dialogActive);
        ModelMultiplySession session{&app, &plan};
        const bool bone = kind == plugins::ModelMultiplyKind::Bone;
        app.state.enterKeyState = 1;
        const auto response = UiDialogBoxParamA(static_cast<HINSTANCE>(app.HInstance()),
            MAKEINTRESOURCEA(bone ? (app.EnglishUI() ? 0x294 : 0x27C) : (app.EnglishUI() ? 0x295 : 0x283)),
            owner, bone ? BoneFrameMultiplyDlgProc : FacialMultiplyDlgProc, reinterpret_cast<LPARAM>(&session));
        if (response == -1) return MMS_E_FAILED;
        if (response != 1) return MMS_OK;
        if (plugins::BoundNativePoseHistory(&app) != history || plugins::BoundNativePoseGeneration(&app) != generation)
            return MMS_E_STALE;
        std::shared_ptr<plugins::HistoryChange> change;
        if (auto status = plan.Take(change)) return status;
        if (!change) return MMS_OK;
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        if (auto status = history->PrepareNative(bone ? "Multiply bone keyframes" : "Multiply morph keyframes", std::move(change),
            transaction, plugins::SharedHistory::NativeIntent::ExplicitCommit)) return status;
        if (auto status = history->CommitNative(std::move(transaction))) return status;
        // Refresh after publication is optional and may dispatch Win32 work.
        // Its failure must not report a committed multiply as an aborted edit.
        plugins::Boundary([&]() -> MmsResult {
            bake::ScopedUiInputGate gate;
            plugins::PublishBoundPoseEdit(app, *history, history->LastPublicationRequiresPhysicsReset());
            curve_host::NotifyModelRegistered(&app, !bone);
            if (app.Renderer() && app.PanelDC()) { PanelPaint(&app); SelectionReeval(&app); }
            return MMS_OK;
        });
        return MMS_OK;
    });
    if (result != MMS_OK) runtime_log::Write("PLUGIN_ABI model multiply refused: %d", result);
}
#endif
void CmdViewMenu(MMDApp* app, HWND hwnd, std::uint16_t id,
                 std::uint16_t notify) {
    (void)notify;  // see header: the default handler's notify gate can never
                   // match any id of this family
    HINSTANCE hInst = static_cast<HINSTANCE>(app->HInstance());
    const bool english = app->state.englishUI != 0;

#if MMD_ORIGINAL_PROFILE
    if (id == 259 || id == 261 || id == 262 || id == 263) {
        if (!app->SelectedModel()) return;
        if (!original_import::CanEdit(*mdl::Mdl(app->SelectedModel()))) {
            original_import::Refuse(hwnd, app->EnglishUI(), true);
            return;
        }
    }
#endif

    switch (id) {
    // ------------------------------------------------------------------
    // 251 (0x0048A1CC): bone-frame multiply dialog (0x294 EN / 0x27C JP).
    // Gate: app+0x2F8 must be clear; closes with 2 -> return, else the
    // default handler (no-op for menu ids).
    // ------------------------------------------------------------------
    case 251: {
        if (app->state.optflag[0] != 0) {
            return;
        }
#if !MMD_ORIGINAL_PROFILE
        if (plugins::BoundNativePoseHistory(app)) { RunBoundModelMultiply(*app, hwnd, plugins::ModelMultiplyKind::Bone); return; }
#endif
        app->state.enterKeyState = 1;
        if (UiDialogBoxParamA(hInst,
                            MAKEINTRESOURCEA(english ? 0x294 : 0x27C),
                            hwnd, BoneFrameMultiplyDlgProc, 0) == 2) {
            return;
        }
        break;
    }

    // ------------------------------------------------------------------
    // 252 (0x0048A219): facial-multiply dialog 0x295 EN / 0x283 JP
    //                    (sub_44D510).
    // ------------------------------------------------------------------
    case 252: {
        if (app->state.optflag[0] != 0) {
            return;
        }
#if !MMD_ORIGINAL_PROFILE
        if (plugins::BoundNativePoseHistory(app)) { RunBoundModelMultiply(*app, hwnd, plugins::ModelMultiplyKind::Morph); return; }
#endif
        app->state.enterKeyState = 1;
        if (UiDialogBoxParamA(hInst,
                            MAKEINTRESOURCEA(english ? 0x295 : 0x283),
                            hwnd, FacialMultiplyDlgProc, 0) == 2) {
            return;
        }
        break;
    }

    // ------------------------------------------------------------------
    // 253 (0x00489FC8): edge-thickness dialog (0x296 EN / 0x285 JP,
    // sub_44C7F0) as a modeless window stored at app+0xA0B44.
    // ------------------------------------------------------------------
    case 253: {
        if (app->state.edgeThicknessDialog != nullptr) {
            break;  // jnz def_47E903 (no-op)
        }
        if (app->state.optflag[0] != 0) {
            break;
        }
        HWND dlg = CreateUiDialogParamA(
            hInst, MAKEINTRESOURCEA(english ? 0x296 : 0x285),
            hwnd, EdgeThicknessDlgProc, reinterpret_cast<LPARAM>(app));
        app->state.edgeThicknessDialog = IsWindow(dlg)?dlg:nullptr;
        if(!app->state.edgeThicknessDialog)break;
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        break;
    }

    // ------------------------------------------------------------------
    // 254 (0x0048A26A): toggle transparent ground-shadow blending
    // and menu item 0xFE. This flag is independent of background video.
    // ------------------------------------------------------------------
    case 254: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryToggleBoundProjectedShadowFlag(app, 1)) break;
#endif
        if (app->state.projectedShadowBlendEnabled != 0) {
            app->state.projectedShadowBlendEnabled = 0;
            CheckMenuItem(GetMenu(hwnd), 0xFE, MF_UNCHECKED);
        } else {
            app->state.projectedShadowBlendEnabled = 1;
            CheckMenuItem(GetMenu(hwnd), 0xFE, MF_CHECKED);
        }
        break;
    }

    // ------------------------------------------------------------------
    // 255..258 (0x0048A883/0x48A89B/0x48A8B3/0x48A8CB): open the four
    // frame-edit dialogs: flag 0x54/0x5C/0x64/0x68 = 1, helper call,
    // dirty 0xA0B0D = 1.  Table-driven: the four bodies are identical
    // except for the flag index and the frame-line helper called.
    // ------------------------------------------------------------------
    case 255:
    case 256:
    case 257:
    case 258: {
        static const struct {
            int id;
            int flagIdx;
            // Every frame-line op returns the number of key records it
            // moved/removed (0 = nothing to do; defect 7).
            int (*frameLineOp)(MMDApp*);
        } kFrameEditDialogs[] = {
            {255, 9, InsertBoneCameraFrameLine},    // 0x439E40
            {256, 11, DeleteBoneCameraFrameLine},   // 0x43A650
            {257, 13, InsertFacialLightFrameLine},  // 0x43B720
            {258, 14, DeleteFacialLightFrameLine},  // 0x43BB30
        };
        for (const auto& entry : kFrameEditDialogs) {
            if (entry.id == id) {
                app->state.dialogFlags[entry.flagIdx] = 1;
                entry.frameLineOp(app);
                app->SceneModified() = 1;
                return;
            }
        }
        break;
    }

    // ------------------------------------------------------------------
    // 259 (0x0048A8E3): model-edge dialog (0x29A EN / 0x299 JP,
    // sub_464BD0) at app+0xA0B50.
    // ------------------------------------------------------------------
    case 259: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryEditBoundModelNames(app, hwnd)) return;
#endif
        if (app->FrameRangeDialog() != nullptr) {
            break;
        }
        if (app->state.optflag[0] != 0) {
            break;
        }
        HWND dlg = CreateUiDialogParamA(
            hInst, MAKEINTRESOURCEA(english ? 0x29A : 0x299),
            hwnd, ModelEdgeDlgProc, 0);
        app->FrameRangeDialog() = dlg;
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        return;
    }

    // ------------------------------------------------------------------
    // 260 (0x0048A862): toggle English UI (byte 0xA0B4C), then the
    // language sweep (0x441AD0 LocalizeUI) and UI refresh (0x40B5A0).
    // ------------------------------------------------------------------
    case 260: {
        const std::uint8_t cur =
            app->state.englishUI;
        // Cycle Japanese(0) -> English(1) -> Chinese(2) -> Japanese(0).
        app->state.englishUI = static_cast<std::uint8_t>((cur + 1) % 3);
        LocalizeUI(app);                             // 0x441AD0
        plugins::PluginNotifyLanguageChanged(app);
        return;
    }

    // ------------------------------------------------------------------
    // 261 (0x0048A958): enhance-model dialog (0x2AA EN / 0x2A9 JP,
    // sub_43C9A0); on close (result != 2) the save reminder box,
    // then 0xA0B64 = 1.
    // ------------------------------------------------------------------
    case 261: {
        if (app->state.optflag[0] != 0) {
            return;
        }
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryEditBoundModelToon(app, hwnd)) return;
#endif
        app->state.enterKeyState = 1;
        const INT_PTR r = UiDialogBoxParamA(
            hInst, MAKEINTRESOURCEA(english ? 0x2AA : 0x2A9),
            hwnd, EnhanceModelDlgProc, 0);
        if (r == 2) {
            return;
        }
        if (english) {
            LegacyUiMessageBoxA(hwnd,
                        "Please preserve the edit result as a new model by "
                        "'save enhanced model'.",
                        "enhance model", MB_TOPMOST);
        } else {
            LegacyUiMessageBoxA(hwnd, kMsgEnhanceModelJp, kCaptionEnhanceModelJp,
                        MB_TOPMOST);
        }
        app->EnhancedModelDirty() = 1;
        return;
    }

    // ------------------------------------------------------------------
    // 262 (0x0048A9EC): frame-edit dialog (0x2AC EN / 0x2AB JP,
    // sub_465020) at app+0xA0B74.
    // ------------------------------------------------------------------
    case 262: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryEditBoundModelPhysics(app, hwnd)) return;
#endif
        if (app->state.frameCopyDialog != nullptr) {
            break;
        }
        if (app->state.optflag[0] != 0) {
            return;
        }
        app->state.dialogFlags[6] = 1;
        app->state.enterKeyState = 1;
        HWND dlg = CreateUiDialogParamW(
            hInst, MAKEINTRESOURCEW(english ? 0x2AC : 0x2AB),
            hwnd, PhysicsModelDlgProc, 0);
        app->state.frameCopyDialog = dlg;
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        return;
    }

    // ------------------------------------------------------------------
    // 263 (0x0048AA6F): "save enhanced model" file dialog.  Filter
    // "Polygon Model files(*.pmd)\0*.pmd\0All Files(*.*)\0\0" (0x52F688),
    // title EN "save enhanced model" / JP 0x52F650, defext "pmd",
    // Flags 6, nFilterIndex 1; initial dir = DirModel when menu 0x12D is
    // checked else "UserFile\Model" (0x52DCBC); on success the menu gate
    // copies the directory into DirModel, then sub_41EC10 saves the file.
    // ------------------------------------------------------------------
    case 263: {
#if !MMD_ORIGINAL_PROFILE
        plugins::TrySaveEnhancedModelDialog(app, hwnd);
        return;
#else
        app->state.dialogFlags[12] = 1;
        app->state.enterKeyState = 1;
        SetCurrentDirectoryW(app->ExeDir());
        wchar_t fileBuf[0x100];
        fileBuf[0] = L'\0';
        OPENFILENAMEW ofn;
        std::memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.nFilterIndex = 1;
        ofn.lpstrFilter =
            L"Polygon Model files(*.pmd)\x00\x00*.pmd\x00\x00"
            L"All Files(*.*)\x00\x00";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = 0x100;
        ofn.Flags = (OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY);
        if ((GetMenuState(GetMenu(hwnd), 0x12D, 0) & 8) != 0) {
            ofn.lpstrInitialDir = app->DirModel();
        } else {
            ofn.lpstrInitialDir = L"UserFile\\Model";
        }
        ofn.lpstrDefExt = L"pmd";
        wchar_t fileTitle[0x100];
        ofn.nMaxFileTitle = 0x100;
        ofn.lpstrFileTitle = fileTitle;
        ofn.lpstrTitle = english ? L"save enhanced model" : kTitleSaveModelJp;
        if (!GetUiSaveFileNameW(&ofn)) {
            return;
        }
        if ((GetMenuState(GetMenu(hwnd), 0x12D, 0) & 8) != 0) {
            wchar_t* dir = ExtractDirFromPath(
                app->PathWorkspace().projectDirectory,
                fileBuf, kPathFieldCapacity);
            CopyDirPathW(app->DirModel(), dir);
        }
        SaveEnhancedModel(app, fileBuf);                     // 0x41EC10
        return;
#endif
    }

    // ------------------------------------------------------------------
    // 264 (0x0048ABB0): toggle byte app+0xA0CC8 with CheckMenuItem 0x108.
    // ------------------------------------------------------------------
    case 264: {
        app->state.dialogFlags[4] = 1;
        if (app->state.rigidBodyDisplayEnabled != 0) {
            app->state.rigidBodyDisplayEnabled = 0;
            CheckMenuItem(GetMenu(hwnd), 0x108, MF_UNCHECKED);
        } else {
            app->state.rigidBodyDisplayEnabled = 1;
            CheckMenuItem(GetMenu(hwnd), 0x108, MF_CHECKED);
        }
        return;
    }

    // ------------------------------------------------------------------
    // 265/269/270/272 (0x0048ACA7/0x48AC0E/0x48AD38/0x48ADCC): view-mode
    // radios - app+0xA0CC4 = 1/2/3/0, the matching menu item
    // (0x109/0x10D/0x10E/0x110) is checked, the other three unchecked,
    // then ModelBonePhysicsModeApply (0x4B2210, thiscall(model, app+0xA0CC4))
    // over the 100 model slots.  Cases 265/269/270 set 0x9EDB5 = 1.
    // ------------------------------------------------------------------
    case 265: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TrySetBoundPhysicsMode(app, 1)) return;
#endif
        app->PlaybackPhysicsMode() = 1;
        CheckMenuItem(GetMenu(hwnd), 0x10D, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x109, MF_CHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x10E, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x110, MF_UNCHECKED);
        // x64 dispatcher sub_7FF7CB45F550 case 265 twin walks all 255 slots
        // (mov r13d, 0FFh ... dec r13 at 0x7FF7CB46F309..0x7FF7CB46F32D).
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* model = app->ModelSlot(i);
            if (model != nullptr) {
                ModelBonePhysicsModeApply(model, app->PlaybackPhysicsMode());
            }
        }
        app->PhysicsResetPending() = 1;
        return;
    }

    case 269: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TrySetBoundPhysicsMode(app, 2)) return;
#endif
        app->PlaybackPhysicsMode() = 2;
        CheckMenuItem(GetMenu(hwnd), 0x10D, MF_CHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x109, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x10E, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x110, MF_UNCHECKED);
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* model = app->ModelSlot(i);
            if (model != nullptr) {
                ModelBonePhysicsModeApply(model, app->PlaybackPhysicsMode());
            }
        }
        app->PhysicsResetPending() = 1;
        return;
    }

    case 270: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TrySetBoundPhysicsMode(app, 3)) return;
#endif
        app->PlaybackPhysicsMode() = 3;
        CheckMenuItem(GetMenu(hwnd), 0x10D, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x109, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x10E, MF_CHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x110, MF_UNCHECKED);
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* model = app->ModelSlot(i);
            if (model != nullptr) {
                ModelBonePhysicsModeApply(model, app->PlaybackPhysicsMode());
            }
        }
        app->PhysicsResetPending() = 1;
        return;
    }

    case 272: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TrySetBoundPhysicsMode(app, 0)) return;
#endif
        app->PlaybackPhysicsMode() = 0;
        CheckMenuItem(GetMenu(hwnd), 0x10D, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x109, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x10E, MF_UNCHECKED);
        CheckMenuItem(GetMenu(hwnd), 0x110, MF_CHECKED);
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* model = app->ModelSlot(i);
            if (model != nullptr) {
                ModelBonePhysicsModeApply(model, app->PlaybackPhysicsMode());
            }
        }
        return;
    }

    // ------------------------------------------------------------------
    // 266 (0x0048AEE1): gravity-setting dialog (0x323 EN / 0x322 JP,
    // sub_479E90) at app+0xA0CCC.
    // ------------------------------------------------------------------
    case 266: {
        if (app->GravitySettingDialog() != 0) {
            return;
        }
        app->state.dialogFlags[7] = 1;
        app->state.enterKeyState = 1;
        HWND dlg = CreateUiDialogParamA(
            hInst, MAKEINTRESOURCEA(english ? 0x323 : 0x322),
            hwnd, GravitySettingDlgProc, 0);
        app->GravitySettingDialog() = dlg;
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        return;
    }

    // ------------------------------------------------------------------
    // 267 (0x0048AF57): physical-engine about box.
    // ------------------------------------------------------------------
    case 267: {
        app->state.dialogFlags[5] = 1;
        if (english) {
            LegacyUiMessageBoxA(hwnd,
                        "Bullet Physics Library Ver.2.75\n\n"
                        "http://www.bulletphysics.com/",
                        "about physical engine", MB_TOPMOST);
        } else {
            LegacyUiMessageBoxA(hwnd,
                        "Bullet Physics Library Ver.2.75\n\n"
                        "http://www.bulletphysics.com/",
                        kCaptionPhysEngineJp, MB_TOPMOST);
        }
        return;
    }

    // ------------------------------------------------------------------
    // 268 (0x0048AECE): 0x54 = 1, undo-available byte 0x9EDB5 = 1.
    // ------------------------------------------------------------------
    case 268:
        app->state.dialogFlags[9] = 1;
        app->PhysicsResetPending() = 1;
        return;

    // ------------------------------------------------------------------
    // 271 (0x0048AE60): toggle menu 0x10F (IK display) from its own
    // GetMenuState bit 8; byte app+0xA066D mirrors the checked state.
    // ------------------------------------------------------------------
    case 271: {
        app->state.dialogFlags[12] = 1;
        if ((GetMenuState(GetMenu(hwnd), 0x10F, 0) & 8) == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x10F, MF_CHECKED);
            app->state.playbackAlwaysOnOffMode = 1;
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x10F, MF_UNCHECKED);
            app->state.playbackAlwaysOnOffMode = 0;
        }
        return;
    }

    // ------------------------------------------------------------------
    // 273 (0x0048DBFF): bone visibility -> selection: sel[i] = 1 and
    // curBone = i for every bone whose frame visibility byte +0x1EC is
    // set, then the language sweeps (0x42F1E0 / 0x40D070).
    // ------------------------------------------------------------------
    case 273: {
        if (app->state.optflag[0] != 0) {
            return;
        }
        app->state.dialogFlags[6] = 1;
        unsigned char* model = ActiveModel(app);
        const std::int32_t boneCount =
            static_cast<std::int32_t>(mdl::Mdl(model)->boneCount);
        if (boneCount > 0) {
            mdl::BoneRecord* const bones = mdl::Bones(model);
            unsigned char* sel = mdl::Mdl(model)->boneSelection;
            for (std::int32_t i = 0; i < boneCount; ++i) {
                if (bones[i].hasRigidBody != 0) {
                    sel[i] = 1;
                    mdl::Mdl(model)->selectedBone = i;
                } else {
                    sel[i] = 0;
                }
            }
        }
        PostLanguageSweep(app);   // 0x42F1E0
        PostLanguageSweep2(app);  // 0x40D070
        return;
    }

    // ------------------------------------------------------------------
    // 274 (0x0048DC96): clear the frame-selection flags (morph +0x10 /
    // 0x14-step, other +0x14 / 0x1C-step, bone +0x38 / 0x3C-step), then
    // mark physics-enabled keys on the tracks of bones with rigid bodies.
    // PanelPaint tail.
    // ------------------------------------------------------------------
    case 274: {
        if (app->state.optflag[0] != 0) {
            return;
        }
        app->state.dialogFlags[0] = 1;
        unsigned char* model = ActiveModel(app);
        if (model == nullptr) return;
        auto& record = *mdl::Mdl(model);
        if (!record.boneTable || !record.boneKeys ||
            !record.morphKeys || !record.displayKeys) return;
        // Model pointers and DisplayKey both grow on x64. Fixed x86 byte
        // offsets here used to read invalid pointers and corrupt key data.
        const auto morphCapacity1 = mdl::MorphKeyCapacity(record);
        for (std::size_t i = 0; i < morphCapacity1; ++i)
            record.morphKeys[i].allocated = 0;
        for (std::size_t i = 0; i < mdl::KeyPoolCapacity(mdl::DisplayKeys(model)); ++i)
            record.displayKeys[i].allocated = 0;
        // Pool capacity of this record, resolved once for both sweeps below.
        const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(record);
        for (std::size_t i = 0; i < boneKeyCapacity; ++i)
            record.boneKeys[i].allocated = 0;
        for (std::uint32_t bone = 0;
             bone < record.boneCount && bone < boneKeyCapacity; ++bone) {
            if (record.boneTable[bone].hasRigidBody == 0) continue;
            std::uint32_t key = bone;
            // Walk this bone's key chain, not a bone parent chain. Increasing
            // frame numbers and bounded links keep this traversal finite.
            for (;;) {
                auto& current = record.boneKeys[key];
                if (current.physicsDisabled == 0) current.allocated = 1;
                const auto next = current.next;
                if (next < record.boneCount || next >= boneKeyCapacity ||
                    record.boneKeys[next].frame <= current.frame) break;
                key = next;
            }
        }
        PanelPaint(app);  // 0x414610
        return;
    }

    // ------------------------------------------------------------------
    // 275 (0x0048DE30): modal dialog 0x32D EN / 0x32C JP (sub_461CE0);
    // no close handling in the original (always returns).
    // ------------------------------------------------------------------
    case 275:
        app->state.dialogFlags[2] = 1;
        app->state.enterKeyState = 1;
        UiDialogBoxParamA(hInst,
                        MAKEINTRESOURCEA(english ? 0x32D : 0x32C),
                        hwnd, PhysicsOnOffFrameDlgProc, 0);
        return;

    // ------------------------------------------------------------------
    // 276 (0x0048AFA4): "render to picture file".  Save dialog with the
    // picture filter (EN 0x52F3F8 / JP 0x52F218, label followed by an
    // empty pattern list as in the binary), defext "bmp", title "render
    // to picture file" / JP 0x52F1D0, initial dir DirUser / "UserFile";
    // dir gate 0x12D like 263; the chosen path goes to app+0x9F134 and
    // the render target grows (locale screenWidth/screenHeight, 0x1D4E4/
    // 0x1D4E8 -> presentParameters.BackBufferWidth/Height, 0x1D4FC/
    // 0x1D500 + PostDeviceReset) when wider/taller; then the recording
    // window ("RecWindow" class, title 0x52F1C0) is created at
    // app+0xA0D24 and the refresh tail runs.
    // ------------------------------------------------------------------
    case 276: {
        const auto generation = app->SceneGeneration();
        const auto canStart = [&] {
            return IsWindow(hwnd) && app->MainWindow() == hwnd &&
                app->SceneGeneration() == generation && !app->sceneRecoveryPending &&
                !app->SceneMutationInProgress() && !app->RecordingWindow();
        };
        if (!canStart()) return;
#if !MMD_ORIGINAL_PROFILE
        // Release the start guard before replaying a deferred close.
        ScopedSceneCloseDeferral deferClose(*app);
#endif
        recording_state::WindowStopGuard starting(app, recording_state::Operation::Start);
        if (!starting) return;

        app->state.dialogFlags[6] = 1;
        app->state.enterKeyState = 1;
        SetCurrentDirectoryW(app->ExeDir());
        wchar_t fileBuf[0x100]{};
        std::optional<image_sequence::StillFileIdentity> overwrite;
        const HRESULT picked = ChooseStillImagePath(app, hwnd, fileBuf, std::size(fileBuf), overwrite);
        if (!canStart()) return;
        if (FAILED(picked)) {
            if (picked != HRESULT_FROM_WIN32(ERROR_CANCELLED))
                MessageBoxW(hwnd, image_sequence::Text(app,
                    L"无法选择图片保存路径。请重试，或选择较短的路径。",
                    L"Could not select the image output path. Please retry or choose a shorter path.",
                    L"画像の保存先を選択できません。再試行するか、短いパスを選択してください。"),
                    L"MMS", MB_OK | MB_ICONWARNING);
            return;
        }
        if (app->SelfShadowMode() > 0 && !recording_state::CheckEffectReady(app)) return;
        if ((GetMenuState(GetMenu(hwnd), 0x12D, 0) & 8) != 0) {
            wchar_t directory[kPathFieldCapacity]{};
            CopyDirPathW(app->DirUser(), ExtractDirFromPath(directory, fileBuf, std::size(directory)));
        }
        wcscpy_s(app->CaptureSavePath(), 0x100, fileBuf);
        image_sequence::BeginStill(app, image_sequence::FormatFromPath(fileBuf), overwrite);
        D3DRenderer* locale = app->Renderer();
        const std::int32_t oldW = locale->screenWidth;   // 0x1D4E4
        const std::int32_t oldH = locale->screenHeight;  // 0x1D4E8
        // TimelineAdvance restores the backbuffer from recRTW/recRTH after
        // the still image has been captured. Unlike video recording, this
        // path never initializes those fields; leaving them at zero makes
        // PostDeviceReset pick the desktop-sized default backbuffer.
        app->state.recRTW = oldW;
        app->state.recRTH = oldH;
        const std::int32_t renderW =
            app->RenderWidth();
        const std::int32_t renderH =
            app->RenderHeight();
        if (renderW > oldW || renderH > oldH) {
            locale->presentParameters.BackBufferWidth = renderW;   // 0x1D4FC
            locale->presentParameters.BackBufferHeight = renderH;  // 0x1D500
            locale->screenWidth = renderW;                         // 0x1D4E4
            locale->screenHeight = renderH;                        // 0x1D4E8
            if (!PostDeviceReset(app)) {  // 0x440DB0
                image_sequence::CancelStillCapture(app);
                MessageBoxW(hwnd, image_sequence::Text(app,
                    L"无法创建图片导出目标，本次导出已取消。",
                    L"Could not create the image export target. Export was canceled.",
                    L"画像出力先を作成できません。出力を中止しました。"), L"MMS", MB_OK | MB_ICONERROR);
                return;
            }
        }
        RECT rc;
        rc.left = 0;
        rc.top = 0;
        rc.right = renderW;
        rc.bottom = renderH;
        AdjustWindowRect(&rc, WS_POPUP | WS_CAPTION, FALSE);
        // A Shift-JIS title passed to an ANSI window was decoded using the
        // system code page and ignored the selected UI language.
        wchar_t title[96]{};
        swprintf_s(title, app->EnglishUI() == 2 ? L"正在导出图片（第 %d 帧）" :
            english ? L"Exporting image (frame %d)" : L"画像出力中（%d フレーム）",
            app->CurrentFrame());
        HWND recWnd = CreateWindowExW(
            0, L"RecWindow", title, 0x80C80000,
            app->SidebarWidth() + 0x32, 0x64,
            rc.right - rc.left, rc.bottom - rc.top, hwnd, nullptr, hInst,
            nullptr);
        app->RecordingWindow() = recWnd;                    // 0xA0D24
        if (recWnd == nullptr) {
            image_sequence::CancelStillCapture(app);
            LegacyUiMessageBoxA(hwnd, "CreateWindow failed",
                        english ? "create main window" : kCaptionCreateWndJp,
                        MB_TOPMOST);
            return;
        }
        ShowWindow(recWnd, SW_SHOW);
        UpdateWindow(recWnd);
        if (locale->multisampleAvailable == 0 && !image_sequence::Transparent(app) &&
            !EnsureCaptureRenderTarget(app, true)) {
            CancelExportAfterCaptureTargetFailure(app);
            return;
        }
        const bool needRefresh =
            app->CaptureMode() == ScreenCaptureMode::BackgroundRefresh ||
            app->state.aviBackgroundEnabled == 1;
        if (needRefresh && app->state.aviStream != 0) {
            AviBgOverlayRefresh(app);  // 0x4168D0
        }
        RefreshMainWindowViewport(app);  // 0x42C810
        InvalidateRect(hwnd, &app->ViewportRect(), FALSE);
        app->state.timelineAdvanceRequested = 1;
        return;
    }

    // ------------------------------------------------------------------
    // 277 (0x0048B2F7): shadow toggle - menu 0x115 from GetMenuState bit
    // 8, scene vtable+0xE4(scene, 0xA1, on/off).
    // ------------------------------------------------------------------
    case 277: {
#if !MMD_ORIGINAL_PROFILE
        std::wstring error;
        if (!antialias::Toggle(app, error))
            MessageBoxW(hwnd, error.c_str(), antialias::DialogTitle(app), MB_OK | MB_ICONERROR);
#else
        app->state.dialogFlags[10] = 1;
        if ((GetMenuState(GetMenu(hwnd), 0x115, 0) & 8) == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x115, MF_CHECKED);
            SetMultisampleAntialias(app, true);
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x115, MF_UNCHECKED);
            SetMultisampleAntialias(app, false);
        }
#endif
        return;
    }

    // ------------------------------------------------------------------
    // 278 (0x0048B401): reload the toon texture set (0x424DC0
    // InitToonTextures), then the locale refresh sub_4076E0.
    // ------------------------------------------------------------------
    case 278:
#if !MMD_ORIGINAL_PROFILE
        plugins::TryReloadNativeTextures(app);
#else
        InitToonTextures(app);  // 0x424DC0
        ReloadTextureCache(app->Renderer());  // 0x4076E0
#endif
        return;

    // ------------------------------------------------------------------
    // 279 (0x0048B393): self-shadow toggle - menu 0x117, byte
    // app+0xA0188 mirrors.
    // ------------------------------------------------------------------
    case 279: {
        app->state.dialogFlags[12] = 1;
        if ((GetMenuState(GetMenu(hwnd), 0x117, 0) & 8) == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x117, MF_CHECKED);
            app->state.selfShadowEnabled = 1;
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x117, MF_UNCHECKED);
            app->state.selfShadowEnabled = 0;
        }
        return;
    }

    // ------------------------------------------------------------------
    // 280 (0x0048B416): flag-subsystem save/init keyed on app+0xA0D38.
    // ------------------------------------------------------------------
    case 280:
        if (app->state.floatingWindow != 0) {
            SaveFlagSubsystem(app);  // 0x461FA0
        } else {
            InitFlagSubsystem(app);  // 0x461E00
        }
        return;

    // ------------------------------------------------------------------
    // 281 (0x0048B433): transparent-window toggle - menu 0x119 plus
    // SetWindowPos of the accessory column (app+0xA0D38) to
    // HWND_TOPMOST / HWND_NOTOPMOST with flags 0x43.
    // ------------------------------------------------------------------
    case 281: {
        app->state.dialogFlags[15] = 1;
        if ((GetMenuState(GetMenu(hwnd), 0x119, 0) & 8) == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x119, MF_CHECKED);
            if (app->state.floatingWindow != 0) {
                SetWindowPos(app->state.floatingWindow,
                             HWND_TOPMOST, 0, 0, 0, 0, 0x43);
            }
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x119, MF_UNCHECKED);
            if (app->state.floatingWindow != 0) {
                SetWindowPos(app->state.floatingWindow,
                             HWND_NOTOPMOST, 0, 0, 0, 0, 0x43);
            }
        }
        return;
    }

    // ------------------------------------------------------------------
    // 282..285 (0x0048B624/0x48B682/0x48B6E0/0x48B73E): option toggles -
    // bytes app+0xA0194..0xA0197 mirrored by menu items 0x11A..0x11D.
    // Case 285 additionally stores 1 / -1 into scene->groundBody -> +0xD4.
    // ------------------------------------------------------------------
    case 282: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryToggleBoundBlackBackground(app)) return;
#endif
        app->state.dialogFlags[4] = 1;
        if (app->state.blackBackgroundEnabled != 0) {
            app->state.blackBackgroundEnabled = 0;
            CheckMenuItem(GetMenu(hwnd), 0x11A, MF_UNCHECKED);
        } else {
            app->state.blackBackgroundEnabled = 1;
            CheckMenuItem(GetMenu(hwnd), 0x11A, MF_CHECKED);
        }
        return;
    }

    case 283: {
        app->state.dialogFlags[5] = 1;
        if (app->state.modelNonDisplayMode != 0) {
            app->state.modelNonDisplayMode = 0;
            CheckMenuItem(GetMenu(hwnd), 0x11B, MF_UNCHECKED);
        } else {
            app->state.modelNonDisplayMode = 1;
            CheckMenuItem(GetMenu(hwnd), 0x11B, MF_CHECKED);
        }
        return;
    }

    case 284: {
        app->state.dialogFlags[15] = 1;
        if (app->state.wavPlaysOnFrameMove != 0) {
            app->state.wavPlaysOnFrameMove = 0;
            CheckMenuItem(GetMenu(hwnd), 0x11C, MF_UNCHECKED);
        } else {
            app->state.wavPlaysOnFrameMove = 1;
            CheckMenuItem(GetMenu(hwnd), 0x11C, MF_CHECKED);
        }
        return;
    }

    case 285: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryToggleBoundFloor(app)) return;
#endif
        app->state.dialogFlags[15] = 1;
        auto* target = app->Physics()->groundBody;
        if (app->state.floorVisible != 0) {
            app->state.floorVisible = 0;
            CheckMenuItem(GetMenu(hwnd), 0x11D, MF_UNCHECKED);
            if (target) target->setCollisionFlags(-1);
        } else {
            app->state.floorVisible = 1;
            CheckMenuItem(GetMenu(hwnd), 0x11D, MF_CHECKED);
            if (target) target->setCollisionFlags(btCollisionObject::CF_STATIC_OBJECT);
        }
        return;
    }

    // ------------------------------------------------------------------
    // 286 (0x0048B7B8): background-color picker - ChooseColorA with the
    // current R/G/B from app+0xA0198/0xA019C/0xA01A0, custom colors at
    // app+0xA01A4, flags 3; the new color is written back and applied
    // via SetModelColor(model, r, g, b) over the 100 model slots.
    // ------------------------------------------------------------------
    case 286: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryEditBoundModelOutlineColor(app, hwnd)) return;
#endif
        app->state.dialogFlags[6] = 1;
        app->state.enterKeyState = 1;
        CHOOSECOLORA cc;
        std::memset(&cc, 0, sizeof(cc));
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = hwnd;
        const std::uint8_t r =
            app->state.modelOutlineColorRed;
        const std::uint16_t g =
            app->state.modelOutlineColorGreen;
        const std::uint8_t b =
            app->state.modelOutlineColorBlue;
        cc.rgbResult = (static_cast<COLORREF>(b) << 16) |
                       (static_cast<COLORREF>(g) << 8) | r;
        // Keep the original sixteen custom colors without indexing across the
        // separate twelve-item array and its four-item trailing storage.
        std::array<COLORREF, 16> customColors{};
        static_assert(sizeof(app->state.customColorTable) <= sizeof(customColors));
        std::memcpy(customColors.data(), app->state.customColorTable, sizeof(app->state.customColorTable));
        static_assert(sizeof(app->state.customColorTable) + sizeof(app->state.pad209) == sizeof(customColors));
        std::memcpy(reinterpret_cast<unsigned char*>(customColors.data()) + sizeof(app->state.customColorTable),
            &app->state.pad209, sizeof(app->state.pad209));
        cc.lpCustColors = customColors.data();
        cc.Flags = 3;  // CC_RGBINIT | CC_FULLOPEN
        if (!ChooseColorA(&cc)) {
            return;
        }
        std::memcpy(app->state.customColorTable, customColors.data(), sizeof(app->state.customColorTable));
        std::memcpy(&app->state.pad209, reinterpret_cast<const unsigned char*>(customColors.data()) + sizeof(app->state.customColorTable), sizeof(app->state.pad209));
        const std::uint8_t nr = cc.rgbResult & 0xFF;
        const std::uint8_t ng = (cc.rgbResult >> 8) & 0xFF;
        const std::uint8_t nb = (cc.rgbResult >> 16) & 0xFF;
        app->state.modelOutlineColorRed = nr;
        app->state.modelOutlineColorGreen = ng;
        app->state.modelOutlineColorBlue = nb;
        // x64 dispatcher twin (0x7FF7CB4701EF..0x7FF7CB470228) walks all
        // 255 slots before each SetModelColor (sub_7FF7CB4F2240) call.
        for (int i = 0; i < kModelSlotCount; ++i) {
            unsigned char* model = app->ModelSlot(i);
            if (model != nullptr) {
                SetModelColor(reinterpret_cast<MMDApp*>(model), nr, ng, nb);
            }
        }
        return;
    }

    // ------------------------------------------------------------------
    // 287 (0x0048B8CF): toggle byte app+0xA01E4 with CheckMenuItem 0x11F.
    // ------------------------------------------------------------------
    case 287: {
        if (app->WireframeRenderingEnabled() != 0) {
            CheckMenuItem(GetMenu(hwnd), 0x11F, MF_UNCHECKED);
            app->WireframeRenderingEnabled() = 0;
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x11F, MF_CHECKED);
            app->WireframeRenderingEnabled() = 1;
        }
        return;
    }

    // ------------------------------------------------------------------
    // 288 (0x0048A08F): modal dialog 0x325 EN / 0x324 JP (sub_4641F0);
    // return when closed.
    // ------------------------------------------------------------------
    case 288: {
        if (app->AccessoryOrderArray() || !ModelOrderEditReady(*app)) return;
        ModelOrderDialogState state{*app, false};
#if !MMD_ORIGINAL_PROFILE
        if (!BeginBoundModelOrder(state)) return;
#endif
        app->state.enterKeyState = 1;
        const auto response = UiDialogBoxParamA(hInst,
                        MAKEINTRESOURCEA(english ? 0x325 : 0x324),
                        hwnd, ModelDisplayOrderDlgProc, reinterpret_cast<LPARAM>(&state));
#if !MMD_ORIGINAL_PROFILE
        FinishBoundModelOrder(state, response);
#else
        (void)response;
#endif
        return;
    }

    // ------------------------------------------------------------------
    // 289 (0x0048A0DF): modal dialog 0x32B EN / 0x32A JP (sub_42E370);
    // return when closed.
    // ------------------------------------------------------------------
    case 289: {
        if (app->AccessoryOrderArray() || !ModelOrderEditReady(*app)) return;
        ModelOrderDialogState state{*app, true};
#if !MMD_ORIGINAL_PROFILE
        if (!BeginBoundModelOrder(state)) return;
#endif
        app->state.dialogFlags[2] = 1;
        app->state.enterKeyState = 1;
        const auto response = UiDialogBoxParamA(hInst,
                        MAKEINTRESOURCEA(english ? 0x32B : 0x32A),
                        hwnd, ModelCalculateOrderDlgProc, reinterpret_cast<LPARAM>(&state));
#if !MMD_ORIGINAL_PROFILE
        FinishBoundModelOrder(state, response);
#else
        (void)response;
#endif
        return;
    }

    // ------------------------------------------------------------------
    // 290 (0x0048B926): full scene reload - 0xA0274 = 1,
    // sub_4629D0 (0x4629D0), PostDeviceReset (0x440DB0).
    // ------------------------------------------------------------------
    case 290:
        app->FullscreenMode() = 1;
        ApplyFullscreenWindowState(app);  // 0x4629D0
        PostDeviceReset(app);  // 0x440DB0
        return;

    // ------------------------------------------------------------------
    // 291 (0x0048B93E): open-ONI gate - app+0xA03B8 selects between the
    // plugin loader (0x42A020) and the OpenNi init (0x429CB0, arg 0).
    // ------------------------------------------------------------------
    case 291:
        app->state.dialogFlags[11] = 1;
        if (app->state.depthDeviceEnabled != 0) {
            DisableKinect(app);  // 0x42A020
        } else {
            OpenNiInit(app, nullptr);  // 0x429CB0
        }
        return;

    // ------------------------------------------------------------------
    // 292 (0x0048B964): auto-frame-record toggle (byte app+0xA0D68):
    // on - CheckMenuItem 0x124, SetTimer(hwnd, 0x65, 0x5DC, 0), per-slot
    // model+0x38FD = app+0xA03EA, 0x9EDB5 = 1; off - KillTimer(0x65),
    // uncheck, dirty + 0x9EDB5 = 1.
    // ------------------------------------------------------------------
    case 292: {
        app->state.dialogFlags[2] = 1;
        if (app->state.autoRepeat == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x124, MF_CHECKED);
            app->state.autoRepeat = 1;
            SetTimer(hwnd, 0x65, 0x5DC, nullptr);
            app->PhysicsResetPending() = 1;
            // x64 dispatcher twin (0x7FF7CB470326..0x7FF7CB47034F) walks all
            // 255 slots (movzx eax,[rbx+0A137Eh]; mov [rcx+3CA5h],al).
            for (int i = 0; i < kModelSlotCount; ++i) {
                unsigned char* model = app->ModelSlot(i);
                if (model != nullptr) {
                    mdl::Mdl(model)->openniVersion = app->state.openniVersion;
                }
            }
        } else {
            KillTimer(hwnd, 0x65);
            CheckMenuItem(GetMenu(hwnd), 0x124, MF_UNCHECKED);
            app->state.autoRepeat = 0;
            app->SceneModified() = 1;
            app->PhysicsResetPending() = 1;
        }
        return;
    }

    // ------------------------------------------------------------------
    // 293/294/295 (0x0048BA1E/0x48BA75/0x48BAD3): option toggles - bytes
    // app+0xA03DC/0xA03DD/0xA03DE mirrored by menu items 0x125..0x127.
    // ------------------------------------------------------------------
    case 293: {
        if (app->state.kinectMirrorEnabled != 0) {
            CheckMenuItem(GetMenu(hwnd), 0x125, MF_UNCHECKED);
            app->state.kinectMirrorEnabled = 0;
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x125, MF_CHECKED);
            app->state.kinectMirrorEnabled = 1;
        }
        return;
    }

    case 294: {
        app->state.dialogFlags[17] = 1;
        if (app->state.kinectInitLostBone != 0) {
            CheckMenuItem(GetMenu(hwnd), 0x126, MF_UNCHECKED);
            app->state.kinectInitLostBone = 0;
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x126, MF_CHECKED);
            app->state.kinectInitLostBone = 1;
        }
        return;
    }

    case 295: {
        app->state.dialogFlags[4] = 1;
        if (app->state.depthTextureCompositionEnabled != 0) {
            CheckMenuItem(GetMenu(hwnd), 0x127, MF_UNCHECKED);
            app->state.depthTextureCompositionEnabled = 0;
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x127, MF_CHECKED);
            app->state.depthTextureCompositionEnabled = 1;
        }
        return;
    }

    // ------------------------------------------------------------------
    // 296 (0x0048BB31): "open oni data" - model-edit gate
    // (app+0x2F8): message box "Please select model!" / JP 0x52E2F4,
    // caption "open oni data" / "oni"; else the oni file dialog
    // (filter 0x52F168, defext "oni", Flags 0x1000, initial dir
    // "UserFile", owner app+0xA0D38 ?: hwnd), DisableKinect when
    // app+0xA03B8, wide->SJIS conversion (0x407910) and OpenNiInit
    // (0x429CB0).
    // ------------------------------------------------------------------
    case 296: {
        if (app->state.optflag[0] != 0) {
            if (english) {
                LegacyUiMessageBoxA(hwnd, "Please select model!", "open oni data",
                            MB_TOPMOST);
            } else {
                LegacyUiMessageBoxA(hwnd, kMsgOpenOniJp, "oni",
                            MB_TOPMOST);
            }
            return;
        }
        SetCurrentDirectoryW(app->ExeDir());
        wchar_t fileBuf[0x100];
        fileBuf[0] = L'\0';
        OPENFILENAMEW ofn;
        std::memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = app->state.floatingWindow != 0
                            ? app->state.floatingWindow
                            : hwnd;
        ofn.lpstrFilter =
            L"oni files(*.oni)\x00\x00*.oni\x00\x00All Files(*.*)\x00\x00";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = 0x100;
        ofn.Flags = OFN_FILEMUSTEXIST;
        ofn.lpstrInitialDir = L"UserFile";
        ofn.lpstrDefExt = L"oni";
        wchar_t fileTitle[0x100];
        ofn.nMaxFileTitle = 0x100;
        ofn.lpstrFileTitle = fileTitle;
        ofn.lpstrTitle = english ? L"open file data" : kTitleOpenJp;
        if (!GetUiOpenFileNameW(&ofn)) {
            return;
        }
        if (app->state.depthDeviceEnabled != 0) {
            DisableKinect(app);  // 0x42A020
        }
        char sjisPath[0x100];
        WideToSjisPath(sjisPath, fileBuf, 0x100);  // 0x407910
        OpenNiInit(app, sjisPath);  // 0x429CB0
        return;
    }

    // ------------------------------------------------------------------
    // 297 (0x0048BC9B): toggle byte app+0xA03E9 with CheckMenuItem 0x129.
    // ------------------------------------------------------------------
    case 297: {
        app->state.dialogFlags[4] = 1;
        if (app->state.automaticFrameAdvanceEnabled != 0) {
            app->state.automaticFrameAdvanceEnabled = 0;
            CheckMenuItem(GetMenu(hwnd), 0x129, MF_UNCHECKED);
        } else {
            app->state.automaticFrameAdvanceEnabled = 1;
            CheckMenuItem(GetMenu(hwnd), 0x129, MF_CHECKED);
        }
        return;
    }

    // ------------------------------------------------------------------
    // 298 (0x0048BD18): render-mode toggle - menu 0x12A, locale byte
    // runtimeToggle (+0x1D570) mirrors, InitRenderStates (0x406E90) on the
    // locale subobject (the ported helper takes the app).
    // ------------------------------------------------------------------
    case 298: {
        D3DRenderer* locale = app->Renderer();
        if ((GetMenuState(GetMenu(hwnd), 0x12A, 0) & 8) == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x12A, MF_CHECKED);
            locale->runtimeToggle = 1;  // 0x1D570
            InitRenderStates(app);  // 0x406E90 (orig: thiscall(locale))
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x12A, MF_UNCHECKED);
            locale->runtimeToggle = 0;  // 0x1D570
            InitRenderStates(app);
        }
        return;
    }

    // ------------------------------------------------------------------
    // 299 (0x0048BDA1): accessory-column toggle - menu 0x12B, checkbox
    // 0x228 of the accessory window (app+0xA0D38 ?: main) via
    // BM_SETCHECK, byte app+0xA4420 mirrors.
    // ------------------------------------------------------------------
    case 299: {
        HWND owner = app->state.floatingWindow != 0
                         ? app->state.floatingWindow
                         : hwnd;
        if ((GetMenuState(GetMenu(hwnd), 0x12B, 0) & 8) == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x12B, MF_CHECKED);
            SendMessageA(GetDlgItem(owner, panel::kFrameVolumeCheckbox), BM_SETCHECK, 1, 0);
            app->FrameVolumeControlEnabled() = 1;
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x12B, MF_UNCHECKED);
            SendMessageA(GetDlgItem(owner, panel::kFrameVolumeCheckbox), BM_SETCHECK, 0, 0);
            app->FrameVolumeControlEnabled() = 0;
        }
        return;
    }

    // ------------------------------------------------------------------
    // 300 (0x0048A2C1): rotation dialog.  Camera path when app+0x2F8 is
    // set (posx/y/z + cam2/3/4 + camangle through the pi/180 conversion,
    // dialog 0x298 EN / 0x289 JP, writeback, dirty, PostViewRefresh);
    // bone path otherwise (quaternion extraction via D3DXMatrixRotation-
    // Quaternion + asin/atan2 (0x40A690/0x40A6B0), quadrant fixes,
    // clamp, dialog 0x297 EN / 0x288 JP, PushBoneEditUndo, pos/angle writeback,
    // RotationZ*X*Y matrix, quaternion writeback, frameFlag, dirty,
    // PostViewRefresh).  The 16-float temp block app+0xA0B28..0xA0B40 is
    // shared with the dialog procs in the original.
    // ------------------------------------------------------------------
    case 300: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryEditBoundPoseNumeric(app, hwnd)) return;
#endif
        constexpr float toDegrees=180.0f/3.14159265358979323846f;
        constexpr float toRadians=3.14159265358979323846f/180.0f;
        app->state.enterKeyState=1;
        if(app->state.optflag[0]!=0) {
            const float values[]={app->CameraPositionX(),app->CameraPositionY(),
                app->CameraPositionZ(),
                -app->CameraPitch()*toDegrees,app->CameraYaw()*toDegrees,
                app->CameraRoll()*toDegrees,-app->CameraDistance()};
            for(int i=0;i<7;++i) app->RotationDialogTemp(i)=std::isfinite(values[i])?values[i]:0.0f;
            if(UiDialogBoxParamA(hInst,MAKEINTRESOURCEA(english?0x298:0x289),
                hwnd,CameraNumericInputDlgProc,0)!=IDOK) return;
            app->CameraPositionX()=app->RotationDialogTemp(0);
            app->CameraPositionY()=app->RotationDialogTemp(1);
            app->CameraPositionZ()=app->RotationDialogTemp(2);
            app->CameraPitch()=-app->RotationDialogTemp(3)*toRadians;
            app->CameraYaw()=app->RotationDialogTemp(4)*toRadians;
            app->CameraRoll()=app->RotationDialogTemp(5)*toRadians;
            app->CameraDistance()=-app->RotationDialogTemp(6);
            app->SceneModified()=1;
            RefreshRequest(-1);PostViewRefresh(app);return;
        }
        unsigned char* model=ActiveModel(app);
        if(!model) return;
        const int boneIndex=mdl::Mdl(model)->selectedBone;
        if(boneIndex<0||boneIndex>=static_cast<int>(mdl::Mdl(model)->boneCount)) return;
        auto* bone=mdl::Bones(model)+boneIndex;
        d3dx::D3DXMATRIXF matrix{};
        d3dx::Get().matrixRotationQuaternion(&matrix,bone->rotQuat);
        // Invert the same row-vector Rz * Rx * Ry composition as the toolbar.
        const float sineX=(std::max)(-1.0f,(std::min)(1.0f,-matrix.m[2][1]));
        const float x=std::asin(sineX);
        float y=0,z=0;
        if(std::fabs(std::cos(x))>1e-6f) {
            y=std::atan2(matrix.m[2][0],matrix.m[2][2]);
            z=std::atan2(matrix.m[0][1],matrix.m[1][1]);
        } else {
            y=std::atan2(-matrix.m[0][2],matrix.m[0][0]);
        }
        const float values[]={bone->trans[0]+BoneParentBindOffset(model,boneIndex,0),
            bone->trans[1]+BoneParentBindOffset(model,boneIndex,1),
            bone->trans[2]+BoneParentBindOffset(model,boneIndex,2),
            x*toDegrees,-y*toDegrees,-z*toDegrees};
        for(int i=0;i<6;++i) app->RotationDialogTemp(i)=std::isfinite(values[i])?values[i]:0.0f;
        if(UiDialogBoxParamA(hInst,MAKEINTRESOURCEA(english?0x297:0x288),
            hwnd,BoneNumericInputDlgProc,0)!=IDOK) return;
        PushBoneEditUndo(app);
        for(int i=0;i<3;++i) bone->trans[i]=app->RotationDialogTemp(i)-BoneParentBindOffset(model,boneIndex,i);
        d3dx::D3DXMATRIXF rotation{},temporary{};
        d3dx::Get().rotZ(&rotation,-app->RotationDialogTemp(5)*toRadians);
        d3dx::Get().rotX(&temporary,app->RotationDialogTemp(3)*toRadians);
        d3dx::Get().multiply(&rotation,&rotation,&temporary);
        d3dx::Get().rotY(&temporary,-app->RotationDialogTemp(4)*toRadians);
        d3dx::Get().multiply(&rotation,&rotation,&temporary);
        BoneRotationWriteback(app,rotation);return;
    }

    // ------------------------------------------------------------------
    // 301 (0x0048D700): menu 0x12D (enhanced-mode) toggle only.
    // ------------------------------------------------------------------
    case 301: {
        if ((GetMenuState(GetMenu(hwnd), 0x12D, 0) & 8) == 0) {
            CheckMenuItem(GetMenu(hwnd), 0x12D, MF_CHECKED);
        } else {
            CheckMenuItem(GetMenu(hwnd), 0x12D, MF_UNCHECKED);
        }
        return;
    }

    // ------------------------------------------------------------------
    // 302 (0x0048E0A6): reset rotation.  app+0x2F8 set: cam2/3/4 = 0,
    // dirty, PostViewRefresh.  else: current bone (2D90 != -1),
    // PushBoneEditUndo, identity matrix, quaternion writeback via the shared
    // tail (0x48A805), frameFlag[cur] = 1, dirty, PostViewRefresh.
    // ------------------------------------------------------------------
    case 302: {
#if !MMD_ORIGINAL_PROFILE
        if (plugins::TryResetBoundCameraRotation(app)) return;
        if (plugins::TryResetBoundBoneRotation(app)) return;
        if (plugins::TryResetUnboundBoneRotation(app)) return;
#endif
        app->state.dialogFlags[1] = 1;
        if (app->state.optflag[0] != 0) {
            app->CameraPitch() = 0.0f;
            app->CameraYaw() = 0.0f;
            app->CameraRoll() = 0.0f;
            app->SceneModified() = 1;
            PostViewRefresh(app);
            return;
        }
        unsigned char* model = ActiveModel(app);
        if (mdl::Mdl(model)->selectedBone == -1) {
            return;
        }
        PushBoneEditUndo(app);  // 0x42D6E0
        d3dx::D3DXMATRIXF ident{};
        for (int i = 0; i < 4; ++i) {
            ident.m[i][i] = 1.0f;
        }
        BoneRotationWriteback(app, ident);
        return;
    }

    // 303 (port addition, kept for compatibility): the old "enable automatic
    // transparent material sorting" toggle.  It has no menu item any more and
    // only moves between the two values it always meant: Off while sorting is
    // on, Standard while it is off.  Any other stored value turns into Off.
    case kAutoTransparentMaterialSortingMenuId: {
#if MMD_ORIGINAL_PROFILE
        app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
        return;
#else
        auto& precision = app->TransparentMaterialSortPrecision();
        precision = precision == TransparentSortPrecision::Off
                        ? TransparentSortPrecision::Standard
                        : TransparentSortPrecision::Off;
        pmm_settings::SortingPrecisionChanged(*app);
        RefreshTransparentSortPrecisionMenu(app);  // move the check mark
        RefreshMainWindowViewport(app);  // 0x42C810 viewport refresh
        InvalidateRect(hwnd, &app->ViewportRect(), FALSE);
        return;
#endif
    }

    // ------------------------------------------------------------------
    // 304..308 (port addition): the five transparent sort limit values
    // reached from the sort-limit submenu.  The command id minus
    // kTransparentSortPrecisionMenuFirst is the stored enum value, so
    // Low=304 .. Maximum=307 and Off=308: the enum is the only state and the
    // handler never depends on menu text.  Picking the stored value again is a
    // no-op, with the one exception described at StoredLimitNeedsRewrite above:
    // a project file holding a limit byte this build cannot read still needs
    // the user's pick to be written. The separate scope toggle uses id 309.
    // ------------------------------------------------------------------
    case kTransparentSortPrecisionMenuFirst:
    case kTransparentSortPrecisionMenuFirst + 1:
    case kTransparentSortPrecisionMenuFirst + 2:
    case kTransparentSortPrecisionMenuFirst + 3:
    case kTransparentSortPrecisionMenuOff: {
#if MMD_ORIGINAL_PROFILE
        app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
        return;
#else
        const auto precision = static_cast<TransparentSortPrecision>(
            id - kTransparentSortPrecisionMenuFirst);
        // The raw records are consulted only while this field has never been
        // edited: after the first edit they still hold the pre-edit bytes until
        // the project is saved, and re-picking the same value must not mark the
        // project dirty a second time.
        const bool marksEdit =
            app->TransparentMaterialSortPrecision() != precision ||
            (!app->PmmProjectSettings().sortingPrecisionEdited &&
             StoredLimitNeedsRewrite(*app, precision));
        if (!marksEdit) {
            return;
        }
        app->TransparentMaterialSortPrecision() = precision;
        pmm_settings::SortingPrecisionChanged(*app);
        RefreshTransparentSortPrecisionMenu(app);  // move the check mark
        RefreshMainWindowViewport(app);  // 0x42C810 viewport refresh
        InvalidateRect(hwnd, &app->ViewportRect(), FALSE);
        return;
#endif
    }

    case kTransparentSortCrossObjectsMenuId: {
#if MMD_ORIGINAL_PROFILE
        app->TransparentSortCrossObjects() = false;
#else
        app->TransparentSortCrossObjects() = !app->TransparentSortCrossObjects();
        pmm_settings::SortingCrossObjectsChanged(*app);
        RefreshTransparentSortPrecisionMenu(app);
        RefreshMainWindowViewport(app);
        InvalidateRect(hwnd, &app->ViewportRect(), FALSE);
#endif
        return;
    }

    case 310: {
        const unsigned char next = static_cast<unsigned char>(
            (gModelInfoLanguagePreference + 1) % 3);
        SetModelInfoLanguagePreference(next);
        SaveModelInfoLanguagePreference();
        LocalizeUI(app);
        return;
    }

    case 311:
        SetModelNameAutoTranslation(!gModelNameAutoTranslation);
        SaveModelInfoLanguagePreference();
        LocalizeUI(app);
        return;

    default:
        // def_47E903 (0x482897): only reachable actions are gated on
        // notify == BN_CLICKED and the sending control being
        // GetDlgItem(hwnd, 0x1B4) - a no-op for every menu id 251..302.
        break;
    }
}

}  // namespace mikumikustudio
