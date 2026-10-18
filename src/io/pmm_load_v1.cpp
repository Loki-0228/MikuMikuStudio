#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmv4mms.hpp"
#include "mikudancestudio/native_media.hpp"
#include "mikudancestudio/ui_language.hpp"
#include "mikudancestudio/ui_translation.hpp"
#include "mikudancestudio/native_mesh.hpp"
// ===========================================================================
// VA 0x0045916D..0x0045E7F7 - LoadSceneV1  (the inline v1 (.pmm "0001")
//                                     loader body of sub_458F80)
// ===========================================================================
// Entered from the load shell sub_458F80 when the 30-byte header version
// field reads "0001" (all three stock sample scenes).  The body owns the fd
// on every path: _close happens at 0x45E11D on success and at 0x45A7CA /
// 0x45C5D5 on the two abort paths.
//
// Layout (all addresses inside sub_458F80):
//   0x45916D  dispose 100 model slots + free the four global tracks
//             (0x374/0x378/0x37C/0x380) + free the 255 accessory objects
//             and tracks (0x9DD70 / 0x384)
//   0x45922D  scene-state reset run (same field set as v2 plus A046C/A0470)
//   0x4592F6  menu/checkbox reset (0xF7, 0x217), A0D2C=flt_52A1D8, A0D30=0,
//             AVI teardown trio, 0x91C=0, AVI-path clear, 0x9E3F0 release
//   0x4593F9  header reads: render w/h, edit flag (A06C8 only when
//             A0D38==0 - no A0D3C variant), fov, and 6 option bytes.  Defect B3-29.
//   0x459526  UI clear run: 7 edits 0x1DE..0x1E4, uncheck 0x1B8/0x1B9/0x1DD,
//             CheckMenuItem 0xFE, EnableMenuItem 0x120/0x121 grayed
//   0x45964A  9ED9A=0; slot byte -> 0x910; model count; combo resets
//             0x1B4/0x1DA/0x1C1/0x1C2 + initial strings; per-model 20-byte
//             name pre-pass into 0x1B4/0x1DA/0x1C1
//   0x459861  per-model loop: slot byte, allocate/init ModelRecord,
//             ModelInitDefaults, 20-byte name + 0x100 path, ResolveAnsiUserFile,
//             ModelLoadPMD; on failure MessageBoxA + GetOpenFileNameW retry
//             (cancel and retry failure both keep the empty slot and go on
//             with the state load; the original aborted on cancel)
//             then the direct-index state load into the model fields
//             (0x2D7C disp count/order, 0x2D8D, 0x2D90 + 0x2D9C..0x2DA8 W4s,
//             0x26D4 group count + 0x26D0/0x65 flags, 0x31AC/0x31B0,
//             dense+sparse bone keys 0x26E0/0x3C, morph keys 0x26E4/0x14,
//             physics keys 0x26E8/0x1C with bool arrays +0x10 / flag +0x14,
//             display-frame records 0x26BC/0x25C with the duplicated +0x14C
//             read and the 0x2D98/0x2D94 bool bytes, morph current 0x26C4/
//             0x88+0x30, IK current 0x26C0/0x18+0x12), EnableMenuItem
//             0x120/0x121 enable
//   0x45A6D3  combo 0x1B1 register list (5 entries in model mode incl.
//             PostLoadInit + CB_SETCURSEL(model 0x2D7C); 7 entries in camera
//             mode), CB_SETCURSEL(0x1B1, 3)
//   0x45AA93  track re-allocation (camera 840000 / light 400000 / selection
//             240000 / shadow 360000 + defaults; camera +0x4C dword = -1
//             sweep; 255 accessory tracks 600000) - v1 allocates through
//             the 0x4C6889 malloc-family helper
//   0x45AC10  camera track read (record 0 + sparse, 84-byte records with a
//             SHORTER field set than v2: 0x28 bytes + 4x4 interp bytes +
//             flag +0x40 + dword +0x44 + flag +0x48) + camera misc W4s +
//             frame UI (BM_SETCHECK 0x1BE uses 0x31C==0 - inverted vs v2)
//   0x45B090  light track read (40-byte records, same shape as v2) + light
//             misc + rgb/direction slider UI
//   0x45B761  9E170 byte + 9DA48 dword; combo 0x1D7/0x1DB resets; accessory
//             block (0x4B0 objects, GetOpenFileNameW retry with .x/.vac
//             filter; cancel and retry failure both keep the empty
//             accessory record and continue (the original closed + reset),
//             ResetAppState + return (NO HandleWindowSize); 60-byte track
//             records with the transparency quirk byte)
//   0x45C390  current accessory UI (CB_SETCURSEL 0x1D7, parent-model frame
//             entries with the (<7 || ==8) filter, SyncAccessoryEditPanel)
//   0x45C48C  config block: 0x980/0x97C/0x9E16C, frame edit 0x1A1, refresh
//             chain, radio 0x914 switch (v1 map: 0->0x1EA, 1->0x1EB,
//             2->none, 3->0x1ED, 4->0x1EC), checkbox bytes 0x340..0x342/
//             0x9ED99 + edits 0x199/0x19A, ClearTimelineAndCurveDCs, wave path (0xD0),
//             AVI block (0x91C read-before-test; w1/w2/w3 -> 9E414/9E418/
//             9E41C), picture block (9E428; p1/p2/p3 -> 9E434/9E438/9E43C),
//             0x31E/0x31D/0x918 menu checks with owner checkboxes
//             0x227/0x22D, FPS menu (1000/30 float compares), 0x9EB84
//             screen-mode switch, physics defaults, CB_SETCURSEL 0x14E on
//             0x1C1/0x1C2
//   0x45D278  read-gated tail (the v1 loader CHECKS _read results here):
//             A0B20 chain (shadow distance copies, per-model 0x31BE bytes,
//             BM_SETCHECK 0x1B9/0x1DD, 0x31C0 sweep, 9ED9A menu, A0CC4
//             physics-mode menu, gravity reads, A0CD4, A0D30/A0188 +
//             selection track 0x37C 24-byte records, model 0x37C0 bytes,
//             A0198..A01A0 color sweep, 0x11A, camera parent reregister +
//             A0430/A0434 + RefillBoneRegisterCombo + 0x1C2 strcmp loop, 16 config dwords
//             0xA0438..0xA0474, 0xF7/0x217, A0478, 0x11D scene +0xD4,
//             per-model 0x2D7D byte sweep)
//   0x45E11D  _close(fd)
//   0x45E149  success tail: shadow-mode gate (RefreshSelfShadowPanel), menu 0x117,
//             BM_SETCHECK 0x1B8, light direction to the physics scene
//             (D3DXVec3Normalize + vtable[13]), window title
//             ("MikuMikuDance [%s]"), two key-chain integrity boxes
//             (physics 0x26E8/0x1C chain and per-bone 0x26E0/0x3C chains -
//             v1 compares the chain link against 0, not the root index),
//             combo 0x1B2 population (camera/light/s shadow/gravity +
//             accessory names; else model re-select -> 0x910 +
//             PostLoadInit), RefreshSeparateWindowViewport + InvalidateRect(child), edit-mode
//             repaint (RefreshLightPanel + PanelPaint), InvalidateRect, RelayoutSidebarControls,
//             9EDB5=1, A442C=1, PostViewRefresh
//
// Split: the body is divided into static LoadSceneV1_* segment functions
// along this VA map (each carries its address-range banner); the shared
// frame - window handles, slot arrays, scratch buffers, track pointers -
// lives in PmmV1LoadContext.  The read-gated tail stays ONE function
// with the original nesting.
//
// Deviations: the v1-only quirks are kept and commented inline (inverted
//   0x1BE checkbox sense, duplicated +0x14C read, radio case 2 leaving all
//   four radios unchecked, physics-mode default leaving the menus untouched,
//   sprintf boxes whose format takes more arguments than the original
//   passed - reproduced argument-for-argument).
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <btBulletDynamicsCommon.h>
#include <cmath>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <cstdlib>
#include <io.h>

#include "mikudancestudio/text_encoding.hpp"
#include "mikudancestudio/accessory_layout.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/recent_projects.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/bone_combo.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/scene_ownership.hpp"
#include "mikudancestudio/panel_controls.hpp"

// Shared PMM record readers, the Rd helper and the byte-identical halves of
// the loader string tables live here (see the header comment for the
// v1/v2 parameterization points).
#include "pmm_io_common.hpp"

#pragma comment(lib, "avifil32.lib")

namespace mikudancestudio {

using namespace pmm_io;

namespace {

// flt_52A1D8 (0x3C3851EC = 0.01125f) - the float the original seeds into
// A0D2C/physicsInterval at 0x45934D.  C++17 has no std::bit_cast; MSVC's
// __builtin_bit_cast is accepted in constant expressions, so the
// static_assert pins the literal to the original .rdata bit pattern.
constexpr float kPhysicsIntervalDefault = 0.01125f;
static_assert(
    __builtin_bit_cast(std::uint32_t, kPhysicsIntervalDefault) == 0x3C3851ECu,
    "flt_52A1D8 bit-exact");

// 0x4C6889 allocation helper of the original (malloc-family; the tracks it
// returns are released with free() on the next load / shutdown).
inline unsigned char* AllocBytes(std::size_t n) {
    return static_cast<unsigned char*>(std::malloc(n));
}

// ---- UI strings: see pmm_io_common.hpp (byte-identical halves; the wide
// .rdata mirrors and kNon moved there). ------------------------------------


// Porting-era trace under MIKUDANCESTUDIO_PMM_TRACE_DIR (CMake option
// MIKUDANCESTUDIO_DIAG, default OFF); the OFF stub keeps the call sites
// valid and inlines away to nothing.
#ifdef MIKUDANCESTUDIO_DIAG
void LogV1Stage(const char* stage, long pos, std::int32_t a = 0,
                std::int32_t b = 0) {
    const char* dir = std::getenv("MIKUDANCESTUDIO_PMM_TRACE_DIR");
    if (dir == nullptr || dir[0] == '\0')
        return;
    char p[MAX_PATH];
    sprintf_s(p, "%s\\pmm_v1_load.log", dir);
    FILE* stream = nullptr;
    if (fopen_s(&stream, p, "ab") != 0 || stream == nullptr)
        return;
    fprintf(stream, "stage=%s file_pos=%ld a=%ld b=%ld\r\n", stage, pos,
            static_cast<long>(a), static_cast<long>(b));
    fclose(stream);
}
#else
inline void LogV1Stage(const char*, long, std::int32_t = 0,
                       std::int32_t = 0) {}
#endif

constexpr std::size_t kTrackCam =
    sizeof(mdl::CameraKey) * mdl::kTimelineKeyCapacity;      // 0xCD140
constexpr std::size_t kTrackLight =
    sizeof(mdl::LightKey) * mdl::kTimelineKeyCapacity;       // 0x61A80
constexpr std::size_t kTrackSel =
    sizeof(mdl::SelfShadowKey) * mdl::kTimelineKeyCapacity;  // 0x3A980
constexpr std::size_t kTrackShadow =
    sizeof(mdl::GravityKey) * mdl::kTimelineKeyCapacity;     // 0x57E40
constexpr std::size_t kTrackAcc =
    sizeof(mdl::AccessoryKey) * mdl::kTimelineKeyCapacity;   // 0x927C0

}  // namespace

namespace {

// ---- SJIS box texts (byte-exact; VA recorded) ----------------------------
// kJpChainCapPhys / kJpCannotOpenModel / kJpOpenCaption are byte-identical
// to their v2 copies and live in pmm_io_common.hpp.  Wave5-C IDA verdict
// (x86 original): the four kept below are byte-identical to their v2
// copies too - single .rdata originals (0x52D770 / 0x52D788 / 0x52D848 /
// 0x52DAE0) referenced by both loader bodies (v1 sites 0x45E4FC /
// 0x45E4DA / 0x45E37A / 0x45B9B7; v2 sites 0x458B54 / 0x458B32 /
// 0x458A22 / 0x455B8C); the v2 file's former 以下 / ブ / 替 / no-し直
// readings were transcription errors, corrected in Wave5-C.  Per-body
// duplicates stay until the loader split lands, then belong in
// pmm_io_common.hpp.
const char kJpChainCapDisp[] = "\x83Z\x81[\x83u\x83\x66\x81[\x83^\x82\xCC\x88\xD9\x8F\xED";   // 0x52D770
const char kJpChainFmtPhys[] =
    "\x22%s\x22\x83\x82\x83\x66\x83\x8B\x81\x41\x83{\x81[\x83"
    "\x93\x22%s\x22\x82\xCC\x83t\x83\x8C\x81[\x83\x80\x83\x66"
    "\x81[\x83^\x82\xC9\x88\xD9\x8F\xED\x82\xAA\x8C\xA9\x82\xC2"
    "\x82\xA9\x82\xE8\x82\xDC\x82\xB5\x82\xBD\x0A\x0A\x88\xD9"
    "\x8F\xED\x82\xC8\x83t\x83\x8C\x81[\x83\x80(\x83t\x83\x8C"
    "\x81[\x83\x80\x94\xD4\x8D\x86%d\x88\xC8\x8D~)\x82\xF0\x8D"
    "\xED\x8F\x9C\x82\xB5\x82\xDC\x82\xB7\x0A%d\x83t\x83\x8C\x81"
    "[\x83\x80\x88\xC8\x8D~\x82\xCC%s\x82\xCC\x83\x82\x81[\x83"
    "V\x83\x87\x83\x93\x82\xF0\x8D\xC4\x93x\x90\xDD\x92\xE8\x82"
    "\xB5\x92\xBC\x82\xB5\x82\xC4\x89\xBA\x82\xB3\x82\xA2";   // 0x52D788
const char kJpChainFmtDisp[] =
    "\x22%s\x22\x83\x82\x83\x66\x83\x8B\x81\x41\x22\x95\x5C\x8E"
    "\xA6\xA5IK\xA5\x8AO\x90\x65\x22\x82\xCC\x83t\x83\x8C\x81"
    "[\x83\x80\x83\x66\x81[\x83^\x82\xC9\x88\xD9\x8F\xED\x82\xAA"
    "\x8C\xA9\x82\xC2\x82\xA9\x82\xE8\x82\xDC\x82\xB5\x82\xBD"
    "\x0A\x0A\x88\xD9\x8F\xED\x82\xC8\x22\x95\x5C\x8E\xA6\xA5"
    "IK\xA5\x8AO\x90\x65\x22\x83t\x83\x8C\x81[\x83\x80\x81i\x83"
    "t\x83\x8C\x81[\x83\x80\x94\xD4\x8D\x86%d\x88\xC8\x8D~\x81"
    "j\x82\xF0\x8D\xED\x8F\x9C\x82\xB5\x82\xDC\x82\xB7\x0A\x90"
    "\x5C\x82\xB5\x96\xF3\x82\xA0\x82\xE8\x82\xDC\x82\xB9\x82"
    "\xF1\x82\xAA\x81\x41%s\x83\x82\x83\x66\x83\x8B\x82\xCC%d"
    "\x83t\x83\x8C\x81[\x83\x80\x88\xC8\x8D~\x82\xCC\x22\x95\x5C"
    "\x8E\xA6\xA5IK\x22\x82\xF0\x8D\xC4\x93x\x90\xDD\x92\xE8\x82"
    "\xB5\x92\xBC\x82\xB5\x82\xC4\x89\xBA\x82\xB3\x82\xA2";   // 0x52D848
const char kJpModelFailFmt[] =
    "%s\x82\xCC\x83\x82\x83\x66\x83\x8B\x83t\x83@\x83\x43\x83"
    "\x8B\x82\xAA\x8C\xA9\x82\xC2\x82\xA9\x82\xE8\x82\xDC\x82"
    "\xB9\x82\xF1\x0A\x0A\x83\x82\x83\x66\x83\x8B\x83t\x83@\x83"
    "\x43\x83\x8B\x82\xCC\x8F\xEA\x8F\x8A\x82\xF0\x8Ew\x92\xE8"
    "\x82\xB5\x82\xC4\x89\xBA\x82\xB3\x82\xA2\x0A\x0A(\x83|\x83"
    "\x8A\x83S\x83\x93\x92\xB8\x93_\x90\x94\x82\xE2\x83{\x81["
    "\x83\x93\x8D\x5C\x91\xA2\x81\x41\x95\x5C\x8F\xEE\x82\xCC"
    "\x8E\xED\x97\xDE\x93\x99\x91S\x82\xAD\x93\xAF\x82\xB6\x83"
    "\x82\x83\x66\x83\x8B\x83t\x83@\x83\x43\x83\x8B\x82\xC5\x82"
    "\xC8\x82\xA2\x82\xC6\x0A\x93\xC7\x82\xDD\x8D\x9E\x82\xDD"
    "\x83G\x83\x89\x81[\x82\xAA\x94\xAD\x90\xB6\x82\xB5MMD\x82"
    "\xAA\x8B\xAD\x90\xA7\x8FI\x97\xB9\x82\xB7\x82\xE9\x89\xC2"
    "\x94\x5C\x90\xAB\x82\xAA\x82\xA0\x82\xE8\x82\xDC\x82\xB7"
    "\x82\xCC\x82\xC5\x92\x8D\x88\xD3\x82\xB5\x82\xC4\x89\xBA"
    "\x82\xB3\x82\xA2)";   // 0x52DE00
const char kJpCannotOpenAcc[] =
    "\x83\x41\x83N\x83Z\x83T\x83\x8A\x83t\x83@\x83\x43\x83\x8B"
    "(%s)\x82\xAA\x8C\xA9\x82\xC2\x82\xA9\x82\xE8\x82\xDC\x82"
    "\xB9\x82\xF1\x0A\x0A%s\x82\xCC\x8F\xEA\x8F\x8A\x82\xF0\x8E"
    "w\x92\xE8\x82\xB5\x82\xC4\x89\xBA\x82\xB3\x82\xA2(\x91\xE3"
    "\x91\xD6\x89\xC2)";   // 0x52DAE0

}  // namespace

namespace {

// Per-load frame shared by the LoadSceneV1 segment functions: the
// original keeps every one of these in sub_458F80's inline v1 body
// frame; the split passes them as one context (same pattern as the v2
// loader's PmmV2LoadContext).
struct PmmV1LoadContext {
    MMDApp* s;
    HWND main;
    PathResolutionWorkspace* paths;
    D3DRenderer* wrap;
    unsigned char** slots;
    mdl::AccessoryRecord** accs;
    mdl::AccessoryKey** accTracks;
    // shared scratch buffers (the 0x218/0x318/0x380/0xdb8 frame slots of
    // the original stack)
    char text[0x100];          // name/EM_REPLACESEL scratch
    char box[0x3E8];           // 1000-byte message-box text (0x459994)
    char mbPath[0x100];        // accessory/background path scratch
    wchar_t widePath[0x100];   // model path
    wchar_t wideTmp[0x100];    // accessory path
    wchar_t ofnFile[0x100];    // GetOpenFileName buffer
    wchar_t ofnTitle[0x100];
    wchar_t wndText[0x100];    // window-title sprintf target
    char lbText[0x100];        // CB_GETLBTEXT buffer
    // the re-allocated global tracks this body reads back (0x45AA93
    // segment; the gravity track stays local - nothing reads it)
    mdl::CameraKey* cameraKeys = nullptr;
    mdl::LightKey* lightKeys = nullptr;
    mdl::SelfShadowKey* selfShadowKeys = nullptr;

    explicit PmmV1LoadContext(MMDApp* app)
        : s(app),
          main(reinterpret_cast<HWND>(app->Hwnd())),
          paths(&app->PathWorkspace()),
          wrap(app->Renderer()),
          slots(app->ModelSlots()),
          accs(app->AccessorySlots()),
          accTracks(app->AccessoryKeyTracks()) {}
};

// abort helper (0x45A7C5) - no longer reached from a cancelled locate-file
// dialog, which now skips that file:
// close + ResetAppState + HandleWindowSize + return; the split-out twin
// of the abortLoad lambda the inline body carried.
static void AbortV1Load(MMDApp* s, int fd) {
    pmm_io::Close(fd);                                              // 0x45A7CA
    ResetAppState(s);                                        // 0x45A7D4
    HandleWindowSize(s);                                     // 0x45A7DB
}

// ---- 0x45916D..0x459630: dispose + scene-state reset + menu/checkbox
// reset + AVI teardown + header-field reads + UI clear run -------------
static void LoadSceneV1_DisposeAndHeader(PmmV1LoadContext& ctx, int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    char* const text = ctx.text;

    // ---- dispose models + tracks + accessories (0x45916D..0x459257) ------
    ReleaseSceneModels(*s);                                      // 0x45916D
    ReleaseGlobalTimelineTracks(*s);                              // 0x459196
    ReleaseAccessoriesAndTracks(*s);                              // 0x459221
    // ---- scene-state reset run (0x45922D..0x4592F5) ----------------------
    s->state.accessoryRenderSplitOrder = 0;
    s->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
    s->state.mainModelComboSelection = 0;
    s->state.cameraParentModel = -1;
    s->state.cameraParentBone = 0;
    s->state.cameraAttachmentBasis[14] = 0;               // 0xA0470
    s->state.cameraAttachmentBasis[13] = 0;               // 0xA046C
    s->state.cameraAttachmentBasis[12] = 0.0f;                    // 0xA0468
    s->state.cameraAttachmentBasis[11] = 0.0f;                    // 0xA0464
    s->state.cameraAttachmentBasis[9] = 0.0f;                     // 0xA045C
    s->state.cameraAttachmentBasis[8] = 0.0f;                     // 0xA0458
    s->state.cameraAttachmentBasis[7] = 0.0f;                     // 0xA0454
    s->state.cameraAttachmentBasis[6] = 0.0f;                     // 0xA0450
    s->state.cameraAttachmentBasis[4] = 0.0f;                     // 0xA0448
    s->state.cameraAttachmentBasis[3] = 0.0f;                     // 0xA0444
    s->state.cameraAttachmentBasis[2] = 0.0f;                     // 0xA0440
    s->state.cameraAttachmentBasis[1] = 0.0f;                     // 0xA043C
    s->state.cameraAttachmentBasis[15] = 1.0f;                    // 0xA0474
    s->state.cameraAttachmentBasis[10] = 1.0f;                     // 0xA0460
    s->state.cameraAttachmentBasis[5] = 1.0f;                 // 0xA044C
    s->state.cameraAttachmentBasis[0] = 1.0f;                    // 0xA0438
    s->state.followCameraEnabled = 0;                // 0x9ED98
    // ---- menu/checkbox reset (0x4592F6..0x45933F) ------------------------
    CheckMenuItem(GetMenu(main), 0xF7, 0);                       // 0x459316
    SendMessageA(GetDlgItem(main, panel::kFollowCameraCheckbox), BM_SETCHECK, 0, 0);    // 0x45933A

    s->state.physicsInterval = kPhysicsIntervalDefault;           // 0x45934D
    s->state.selfShadowMode = 0;                 // 0x45935C

    // ---- AVI teardown trio (0x459369..0x45939D) --------------------------
    media::ReleaseVideo(s);
    if (s->AviFrameReader() != nullptr) {
        AVIStreamGetFrameClose(s->AviFrameReader());
        s->AviFrameReader() = nullptr;
    }
    if (s->AviStream() != nullptr) {
        AVIStreamRelease(s->AviStream());
        s->AviStream() = nullptr;
    }
    if (s->AviFile() != nullptr) {
        AVIFileRelease(s->AviFile());
        s->AviFile() = nullptr;
    }
    // v1 additionally clears the AVI gate dword before clearing the path.
    s->AviBackgroundEnabled() = 0;                               // 0x4593A8
    swprintf_s(s->AviBackgroundPath(), 0x100, L"");              // 0x4593C7

    if (s->AviBackgroundTexture() != nullptr) {                   // 0x4593DC
        s->AviBackgroundTexture()->Release();
        s->AviBackgroundTexture() = nullptr;
    }

    // ---- header-field reads (0x4593F9..0x459501) -------------------------
    Rd(fd, &s->state.renderW, 4);       // 0x4593F9
    Rd(fd, &s->state.renderH, 4);       // 0x45940C
    {
        std::int32_t editFlag = 0;
        Rd(fd, &editFlag, 4);                                    // 0x459420
        // v1: no A0D3C variant - the flag lands in A06C8 only when the
        // accessory column is hidden (A0D38 == 0).
        if (s->state.floatingWindow == 0)
            s->state.sidebarWidth = editFlag;   // 0xA06C8
    }
    Rd(fd, &s->state.cameraFov, 4);          // 0x459449
    // v1 reads SIX option bytes (0x2F8..0x2FD), one less than v2; the
    // Ghidra merge of the 5th/6th read hides one (calls 0x459455..0x4594FC).
    for (int f = 0; f < 6; ++f) {
        unsigned char b = 0;
        Rd(fd, &b, 1);                                           // 0x459455..
        s->state.optflag[f] = (b == 1) ? 1 : 0;       // 0x2F8..2FD
    }
    strcpy_s(text, 0x100, "");                                   // 0x459526

    // ---- UI clear run (0x45953F..0x459630) -------------------------------
    for (int id = 478; id <= 484; ++id) {  // 0x1DE..0x1E4 edits
        HWND item = GetDlgItem(main, id);
        const int len = GetWindowTextLengthA(item);
        SendMessageA(item, EM_SETSEL, 0, len);
        SendMessageA(item, EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>(text));
    }
    SendMessageA(GetDlgItem(main, panel::kShadowCheckbox), BM_SETCHECK, 0, 0);     // 0x4595A8
    SendMessageA(GetDlgItem(main, panel::kAddBlendCheckbox), BM_SETCHECK, 0, 0);     // 0x4595C2
    SendMessageA(GetDlgItem(main, panel::kAccessoryAddBlendCheckbox), BM_SETCHECK, 0, 0);     // 0x4595DC
    CheckMenuItem(GetMenu(main), 0xFE, 0);                       // 0x4595F3
    EnableMenuItem(GetMenu(main), 0x120, 1);                     // 0x45960E
    EnableMenuItem(GetMenu(main), 0x121, 1);                     // 0x459629
}


// ---- 0x45964A..0x459841: 9ED9A=0, slot byte, model count, combo resets
// + the per-model 20-byte name pre-pass; returns the model count ------
static unsigned char LoadSceneV1_ModelPrePass(PmmV1LoadContext& ctx,
                                            int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    char* const text = ctx.text;

    // ---- model count / combo resets (0x45964A..0x45979E) ----------------
    s->state.projectedShadowBlendEnabled = 0;                   // 0x459640
    Rd(fd, &s->SelectedModelSlot(), 1);                            // 0x45964A
    unsigned char modelCount = 0;
    Rd(fd, &modelCount, 1);                                      // 0x45965B

    SendMessageA(GetDlgItem(main, panel::kMainComboModel), CB_RESETCONTENT, 0, 0); // 0x459675
    if (s->EnglishUI() == 0)
        SendMessageW(GetDlgItem(main, panel::kMainComboModel), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kWCamLight));       // 0x4596BA
    else
        SendMessageA(GetDlgItem(main, panel::kMainComboModel), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>("camera/light/accessory"));
    SendMessageA(GetDlgItem(main, panel::kMainComboGround), CB_RESETCONTENT, 0, 0); // 0x4596D8
    if (s->EnglishUI() == 0)
        SendMessageW(GetDlgItem(main, panel::kMainComboGround), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kWGround));         // 0x45971D
    else
        SendMessageA(GetDlgItem(main, panel::kMainComboGround), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>("ground"));
    SendMessageA(GetDlgItem(main, panel::kMainComboNormal), CB_RESETCONTENT, 0, 0); // 0x45973B
    if (s->EnglishUI() == 0)
        SendMessageW(GetDlgItem(main, panel::kMainComboNormal), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kWNashi));          // 0x459780
    else
        SendMessageA(GetDlgItem(main, panel::kMainComboNormal), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kNon));
    SendMessageA(GetDlgItem(main, panel::kBoneRegisterCombo), CB_RESETCONTENT, 0, 0); // 0x45979E

    // per-model 20-byte name pre-pass (0x4597C6..0x459841)
    for (unsigned char i = 0; i < modelCount; ++i) {
        pmm_io::ReadText(fd, text, 0x14, 20);
        text[20] = 0;                                      // 0x4597C6
        SendMessageA(GetDlgItem(main, panel::kMainComboModel), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text));
        SendMessageA(GetDlgItem(main, panel::kMainComboGround), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text));
        SendMessageA(GetDlgItem(main, panel::kMainComboNormal), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text));
    }
    return modelCount;
}


// ---- 0x459861..0x45A6B4: one per-model record - allocate/init, load
// the PMD (MessageBoxA + GetOpenFileNameW retry; a cancelled picker keeps
// the empty slot and the state load continues), then
// the direct-index state load.  Returns false after AbortV1Load ran.
static bool LoadSceneV1_ModelBlock(PmmV1LoadContext& ctx, int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    PathResolutionWorkspace& paths = *ctx.paths;
    D3DRenderer* const wrap = ctx.wrap;
    unsigned char** const slots = ctx.slots;
    char* const box = ctx.box;
    char* const mbPath = ctx.mbPath;
    wchar_t* const widePath = ctx.widePath;
    wchar_t* const ofnFile = ctx.ofnFile;
    wchar_t* const ofnTitle = ctx.ofnTitle;

            unsigned char slotByte = 0;
            Rd(fd, &slotByte, 1);                                // 0x459861
            if (slotByte >= kModelSlotCount || slots[slotByte] != nullptr)
                throw std::runtime_error("Invalid or duplicate PMM model slot");
            unsigned char* nm =
                static_cast<unsigned char*>(operator new(mdl::kSize));
            if (nm != nullptr) IdentityCtor(nm);                    // 0x459888
            // Deliberate hardening: the original reads this byte directly
            // from the file and never checks it against the model slot
            // count (verified at 0x1400835E1 / 0x1400835E8), so a slot beyond
            // the end overwrites adjacent application state. The port keeps
            // the record in a local and writes it to slots[] only when the
            // byte is in range, leaving the stream position unchanged.
            // Defect B3-05.
            const bool slotInRange =
                static_cast<int>(slotByte) < kModelSlotCount;
            unsigned char* const model = nm;
            if (slotInRange)
                slots[slotByte] = nm;                            // 0x459897
            std::memset(model, 0, mdl::kSize);                   // 0x4598BC
            ModelInitDefaults(model);                            // 0x4598D0

            char name20[0x14];
            pmm_io::ReadText(fd, name20, 0x14, 20);                                // 0x4598E4
            pmm_io::ReadFixedText(fd, mbPath, 0x100, 255);                               // 0x4598FB
            ResolveAnsiUserFile(reinterpret_cast<unsigned char*>(wrap),
                                mbPath, widePath, 0x100,
                                paths);                           // 0x459925

            LogV1Stage("model-resolved", _tell(fd), slotByte);
            bool loaded = ModelLoadPMD(                          // 0x459965
                model, main, widePath, wrap,
                static_cast<int>(
                    reinterpret_cast<std::uintptr_t>(s->state.exeDir)),
                0, s->EnglishUI(), s->Physics(),
                paths);
            if (!loaded) {                                       // 0x45998B
                sprintf_s(box, 0x3E8,
                          s->EnglishUI() != 0
                              ? "Cannot open the model file:%s"
                                "\n\nPlease select the same model"
                              : kJpModelFailFmt,
                          name20);
                LegacyUiMessageBoxA(main, box,
                            s->EnglishUI() != 0 ? "open file"
                                                : kJpOpenCaption,
                            0);                                  // 0x4599ED
                SetCurrentDirectoryW(reinterpret_cast<const wchar_t*>(
                    s->state.exeDir));                         // 0x4599FA
                swprintf_s(ofnFile, 0x100, L"");                 // 0x459A11
                OPENFILENAMEW ofn;
                std::memset(&ofn, 0, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);                          // 0x459A22
                ofn.hwndOwner =
                    s->state.floatingWindow
                        ? reinterpret_cast<HWND>(
                              s->state.floatingWindow)
                        : main;
                ofn.lpstrFilter = kWFilterModel;
                ofn.lpstrFile = ofnFile;
                ofn.nMaxFile = 0x100;
                ofn.Flags = OFN_FILEMUSTEXIST;
                ofn.lpstrInitialDir =
                    (GetMenuState(GetMenu(main), 0x12D, 0) & 8)
                        ? reinterpret_cast<LPCWSTR>(s->state.dirModel)
                        : kWUserModel;
                ofn.lpstrDefExt = L"pmd;pmx";
                ofn.nMaxFileTitle = 0x100;
                ofn.lpstrFileTitle = ofnTitle;
                ofn.lpstrTitle =
                    s->EnglishUI() != 0
                        ? L"load model"
                        : reinterpret_cast<LPCWSTR>(kWOpenFile);
                if (!GetUiOpenFileNameW(&ofn)) {                   // 0x459B09
                    // Closing the picker without a file skips this model's
                    // file: the slot keeps its initialised (empty) record and
                    // the per-model state load below still runs, exactly like
                    // the failed-retry path - instead of aborting the load.
                } else {
                if (GetMenuState(GetMenu(main), 0x12D, 0) & 8) {
                    ExtractDirFromPath(ofnTitle, ofnFile,
                                      sizeof(ctx.ofnTitle)
                                          / sizeof(ctx.ofnTitle[0]));       // 0x459B3F
                    wcscpy_s(reinterpret_cast<wchar_t*>(
                                 s->state.dirModel),
                             0x3E8, ofnTitle);                   // 0x459B51
                }
                if (!ModelLoadPMD(                               // 0x459B94
                        model, main, ofnFile, wrap,
                        static_cast<int>(reinterpret_cast<
                            std::uintptr_t>(s->state.exeDir)),
                        0, s->EnglishUI(),
                        s->Physics(),
                        paths)) {
                    // retry failed: box + CONTINUE (no abort)   // 0x459BCC
                    LegacyUiMessageBoxA(
                        main,
                        s->EnglishUI() != 0 ? "Cannot open the model file"
                                           : kJpCannotOpenModel,
                        s->EnglishUI() != 0 ? "open file" : kJpOpenCaption,
                        0);
                }
                }
            }

            LogV1Stage("model-loaded", _tell(fd), loaded ? 1 : 0);
            // ---- state load (direct indices, no remap records) ----------
            mdl::ModelRecord* const record = mdl::Mdl(model);
            // The two PMM display-order bytes are adjacent in the original
            // x86 allocation.  Keep that file layout while naming the
            // x64 members instead of indexing the model allocation.
            Rd(fd, &record->comboSelIndex, 1);                   // 0x459BEB
            record->comboSelIndex2 = record->comboSelIndex;
            s->state.mainModelComboSelection =
                record->comboSelIndex;
            {
                unsigned char b = 0;
                Rd(fd, &b, 1);                                   // 0x459C2D
                mikudancestudio::mdl::Mdl(model)->loadComplete = (b == 1) ? 1 : 0;
            }
            Rd(fd, &record->selectedBone, sizeof(record->selectedBone));
            for (std::int32_t& selectedMorph :
                 mdl::Mdl(model)->selectedMorphs)
                Rd(fd, &selectedMorph, sizeof(selectedMorph));   // 0x459CAB
            if (record->selectedBone < 0 || std::uint32_t(record->selectedBone) >= record->boneCount)
                record->selectedBone = -1;
            for (auto& morph : record->selectedMorphs)
                if (morph < 0 || std::uint32_t(morph) >= record->morphCount) morph = -1;
            // The PMM stores one visibility byte per display group.  In the
            // original x86 object these lived at model+0x26D0/0x26D4.  Those
            // are pointer-sized fields, so using the literal offsets on x64
            // reads a different member (and previously dereferenced null).
            // Consume every serialized byte, but only apply records that the
            // model actually owns; a project may refer to an older model
            // revision with a different display-group count.
            unsigned char serializedGroupCount = 0;
            Rd(fd, &serializedGroupCount, 1);                    // 0x459CD7
            mdl::DisplayGroup* const displayGroups =
                mdl::DisplayGroups(model);
            const unsigned char loadedGroupCount =
                mdl::Mdl(model)->groupCount;
            for (unsigned char g = 0; g < serializedGroupCount; ++g) {
                unsigned char b = 0;
                Rd(fd, &b, 1);                                   // 0x459D07
                if (displayGroups != nullptr && g < loadedGroupCount)
                    displayGroups[g].flags = (b == 1) ? 1 : 0;
            }
            LogV1Stage("misc-done", _tell(fd),
                        static_cast<int>(mikudancestudio::mdl::Mdl(model)->boneCount),
                        static_cast<int>(mikudancestudio::mdl::Mdl(model)->morphCount));
            Rd(fd, &record->boneListPos, sizeof(record->boneListPos));
            Rd(fd, &record->maxFrame, sizeof(record->maxFrame));

            // dense bone keys (0x26E0 array, 0x3C stride)   0x459DF4..0x459EDE
            {
                const std::int32_t boneCnt =
                    static_cast<std::int32_t>(record->boneCount);
                mdl::BoneKey* keys = mdl::BoneKeys(model);
                for (std::int32_t i = 0; i < boneCnt; ++i) {
                    ReadPmmBoneKey<PmmStream::V1>(fd, keys[i]);
                }
                // Sparse records name a bone-key pool index, not a timeline
                // frame.  The first boneCount entries are the dense heads.
                // (0x459F18..0x45A0D6)
                std::int32_t cnt = 0;
                pmm_io::ReadCount(fd, cnt);
                // This section carries explicit slot indices, so it is the one
                // load path that can name a slot past the original pool: the
                // pool grows on demand instead of dropping such a record.  The
                // capacity is cached and only re-read after a growth.
                std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*record);
                for (std::int32_t i = 0; i < cnt; ++i) {
                    std::int32_t keyIndex = 0;
                    Rd(fd, &keyIndex, 4);
                    // Deliberate hardening: the original applies this
                    // file-supplied index directly to the bone-key pool
                    // without comparing it against the pool capacity (the
                    // loader region 0x140082D00..0x140085000 contains no
                    // capacity check), so an out-of-range index writes
                    // outside the pool.  The port refuses a negative index and
                    // grows the pool for an index past the current capacity;
                    // when the pool cannot grow the record is consumed and
                    // discarded, leaving the stream position unchanged.
                    // Defect B3-03.
                    if (keyIndex < 0) {
                        mdl::BoneKey discardedKey{};
                        ReadPmmBoneKey<PmmStream::V1>(fd, discardedKey);
                        continue;
                    }
                    if (static_cast<std::size_t>(keyIndex) >= boneKeyCapacity) {
                        if (!mdl::BoneKeyPoolEnsure(
                                *record,
                                static_cast<std::uint64_t>(keyIndex) + 1)) {
                            mdl::BoneKey discardedKey{};
                            ReadPmmBoneKey<PmmStream::V1>(fd, discardedKey);
                            continue;
                        }
                        keys = mdl::BoneKeys(model);
                        boneKeyCapacity = mdl::BoneKeyCapacity(*record);
                    }
                    ReadPmmBoneKey<PmmStream::V1>(fd, keys[keyIndex]);
                }
            }
            // dense + sparse morph keys (0x26E4 array, 0x14 stride)
            {
                const std::int32_t morphCnt =
                    static_cast<std::int32_t>(record->morphCount);
                mdl::MorphKey* keys = mdl::MorphKeys(model);
                for (std::int32_t i = 0; i < morphCnt; ++i)      // 0x45A0D7
                    ReadPmmMorphKey(fd, keys[i]);
                std::int32_t cnt = 0;
                pmm_io::ReadCount(fd, cnt);                                 // 0x45A107
                for (std::int32_t i = 0; i < cnt; ++i) {
                    std::int32_t keyIndex = 0;
                    Rd(fd, &keyIndex, 4);
                    if (keyIndex < morphCnt || std::uint64_t(keyIndex) >= mdl::kMorphKeyCapacityLimit) {
                        mdl::MorphKey discardedKey{};
                        ReadPmmMorphKey(fd, discardedKey);
                        continue;
                    }
                    if (!mdl::MorphKeyPoolEnsure(*record, std::uint64_t(keyIndex) + 1))
                        throw std::bad_alloc();
                    keys = mdl::MorphKeys(model);
                    ReadPmmMorphKey(fd, keys[keyIndex]);
                }
            }

            LogV1Stage("bonekeys-done", _tell(fd));
            // physics key records (0x26E8 array, 0x1C stride; record 0
            // keeps the IK bool-array pointer at +0x10 and the flag at +0x14)
            {
                mdl::DisplayKey*& keys = mdl::DisplayKeys(model);
                ReadFrame(fd, keys[0].frame);
                Rd(fd, &keys[0].previous, 4);
                Rd(fd, &keys[0].next, 4);
                unsigned char visible = 0;
                Rd(fd, &visible, 1);
                keys[0].visible = visible == 1;
                for (std::int32_t i = 0;
                     i < static_cast<std::int32_t>(record->ikChainCount); ++i) {
                    unsigned char b = 0;
                    Rd(fd, &b, 1);                               // 0x45A1A1
                    mdl::IkStates(keys[0])[i] = b == 1;
                }
                {
                    unsigned char b = 0;
                    Rd(fd, &b, 1);                               // 0x45A207
                    keys[0].allocated = b == 1;
                }
                std::int32_t cnt = 0;
                pmm_io::ReadCount(fd, cnt);                                 // 0x45A23E
                for (std::int32_t i = 0; i < cnt; ++i) {
                    std::int32_t keyIndex = 0;
                    Rd(fd, &keyIndex, 4);
                    // Deliberate hardening: the original takes this
                    // index from the file and applies it directly to the display-key pool
                    // without checking it against the pool capacity (the
                    // loader region 0x140082D00..0x140085000 contains no
                    // capacity check), so an index beyond the end reads
                    // and writes past the pool (including via the IK
                    // state pointer held in the out-of-range record).  The
                    // port consumes the record and discards it, leaving the
                    // stream position unchanged.
                    // Defect B3-03.
                    if (keyIndex < 0) throw std::runtime_error("Negative PMM key slot");
                        if (!mdl::DisplayKeyPoolEnsure(*mdl::Mdl(model), std::uint64_t(keyIndex) + 1)) throw std::bad_alloc();
                    mdl::DisplayKey& key = keys[keyIndex];
                    ReadFrame(fd, key.frame);
                    Rd(fd, &key.previous, 4);
                    Rd(fd, &key.next, 4);
                    unsigned char visible = 0;
                    Rd(fd, &visible, 1);
                    key.visible = visible == 1;
                    for (std::int32_t j = 0;
                         j < static_cast<std::int32_t>(record->ikChainCount);
                         ++j) {
                        unsigned char b = 0;
                        Rd(fd, &b, 1);                           // 0x45A2C1
                        mdl::IkStates(key)[j] = b == 1;
                    }
                    unsigned char b = 0;
                    Rd(fd, &b, 1);                               // 0x45A320
                    key.allocated = b == 1;
                }
            }
            LogV1Stage("physkeys-done", _tell(fd));
            // Current bone pose. The first quaternion component is read
            // twice, matching the original stream consumption order.
            {
                const std::int32_t boneCnt =
                    static_cast<std::int32_t>(record->boneCount);
                mdl::BoneRecord* const bones = mdl::Bones(model);
                for (std::int32_t i = 0; i < boneCnt; ++i) {
                    mdl::BoneRecord& bone = bones[i];
                    Rd(fd, &bone.trans[0], sizeof(bone.trans[0]));
                    Rd(fd, &bone.trans[1], sizeof(bone.trans[1]));
                    Rd(fd, &bone.trans[2], sizeof(bone.trans[2]));
                    Rd(fd, &bone.rotQuat[0], sizeof(bone.rotQuat[0]));
                    Rd(fd, &bone.rotQuat[1], sizeof(bone.rotQuat[1]));
                    Rd(fd, &bone.rotQuat[2], sizeof(bone.rotQuat[2]));
                    Rd(fd, &bone.rotQuat[3], sizeof(bone.rotQuat[3]));
                    Rd(fd, &bone.rotQuat[0], sizeof(bone.rotQuat[0]));
                    RepairBonePose(bone.trans, bone.rotQuat);
                    unsigned char b = 0;
                    Rd(fd, &b, 1);                               // 0x45A4E8
                    record->bonePhysicsState[i] = (b == 1) ? 1 : 0;
                    Rd(fd, &b, 1);                               // 0x45A539
                    record->boneSelection[i] = (b == 1) ? 1 : 0;
                }
            }
            // Current morph values.
            mdl::MorphRecord* const morphs = mdl::Morphs(model);
            for (std::int32_t i = 0;
                 i < static_cast<std::int32_t>(record->morphCount); ++i)
                Rd(fd, &morphs[i].value, sizeof(morphs[i].value));
            // Current IK enabled flags.
            mdl::IkChain* const ikChains = mdl::IkChains(model);
            for (std::int32_t i = 0;
                 i < static_cast<std::int32_t>(record->ikChainCount); ++i) {
                unsigned char b = 0;
                Rd(fd, &b, 1);
                ikChains[i].enabled = (b == 1) ? 1 : 0;
            }
            LogV1Stage("state-done", _tell(fd));
            EnableMenuItem(GetMenu(main), 0x120, 0);             // 0x45A682
            EnableMenuItem(GetMenu(main), 0x121, 0);             // 0x45A6A2
            if (!slotInRange) {
                // The record never reached slots[], so release it here the same way
                // that ReleaseSceneModels releases a stored model.
                if (model != nullptr) {
                    ModelDispose(model);
                    std::free(model);
                }
            }
    return true;
}


// ---- 0x45A6D3..0x45AA89: register-combo 0x1B1 (5 model-mode entries
// incl. reselect, or 7 camera-mode entries) ----------------------------
static void LoadSceneV1_RegisterCombo(PmmV1LoadContext& ctx) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    unsigned char** const slots = ctx.slots;

    if (s->SelectedModelSlot() >= kModelSlotCount) s->SetSelectedModelSlot(0);
    if (!slots[s->SelectedModelSlot()]) s->state.optflag[0] = 1;

    // ---- register-combo 0x1B1 (0x45A6D3..0x45AA89) -----------------------
    SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_RESETCONTENT, 0, 0); // 0x45A6D3
    if (s->state.optflag[0] == 0) {
        // model-edit mode: 5 entries + reselect current model
        if (s->EnglishUI() == 0) {
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWXMove));      // 0x45A9A3
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWYMove));
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWZMove));
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWRot));
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWAll));
        } else {
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("x axis move"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("y axis move"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("z axis move"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("rotation"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("all"));        // 0x52D594
        }
        SendMessageA(GetDlgItem(main, panel::kMainComboModel), CB_SETCURSEL,       // 0x45AA59
                     mikudancestudio::mdl::Mdl(
                         slots[s->SelectedModelSlot()])->comboSelIndex,
                     0);
        PostLoadInit(slots[s->SelectedModelSlot()]);                 // 0x45AA6F
    } else {
        // camera mode: 7 entries (distance + view angle included)
        if (s->EnglishUI() == 0) {
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWXMove));      // 0x45A7FA
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWYMove));
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWZMove));
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWRot));
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWDist));        // 0x45A87E
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWViewAng));
            SendMessageW(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(kWAll));
        } else {
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("x axis move"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("y axis move"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("z axis move"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("rotation"));
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("distance"));   // 0x52D5A4
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("view angle")); // 0x52D598
            SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>("all"));        // 0x52D594
        }
        SendMessageA(GetDlgItem(main, panel::kMainComboModel), CB_SETCURSEL, 0, 0);// 0x45A8DE
    }
    SendMessageA(GetDlgItem(main, panel::kInterpCurveCombo), CB_SETCURSEL, 3, 0);    // 0x45AA86

}


// ---- 0x45AA93..0x45ABCC: track re-allocation through the 0x4C6889
// malloc-family helper + defaults + 255 accessory tracks --------------
static void LoadSceneV1_ReallocateTracks(PmmV1LoadContext& ctx) {
    auto* const s = ctx.s;
    D3DRenderer* const wrap = ctx.wrap;
    mdl::AccessoryRecord** const accs = ctx.accs;
    mdl::AccessoryKey** const accTracks = ctx.accTracks;

    // ---- track re-allocation (0x45AA93..0x45ABCC) -------------------------
    ctx.cameraKeys = mdl::KeyPoolCreate<mdl::CameraKey>();                                  // 0x45AA93
    s->CameraKeys() = ctx.cameraKeys;
    std::memset(ctx.cameraKeys, 0, kTrackCam);                        // 0x45AAA6
    ctx.lightKeys = mdl::KeyPoolCreate<mdl::LightKey>();                                // 0x45AAB0
    s->LightKeys() = ctx.lightKeys;
    std::memset(ctx.lightKeys, 0, kTrackLight);
    ctx.selfShadowKeys = mdl::KeyPoolCreate<mdl::SelfShadowKey>();                                  // 0x45AACD
    s->ShadowKeys() = ctx.selfShadowKeys;
    std::memset(ctx.selfShadowKeys, 0, kTrackSel);
    auto* const gravityKeys = mdl::KeyPoolCreate<mdl::GravityKey>();                               // 0x45AAEA
    s->GravityKeys() = gravityKeys;
    std::memset(gravityKeys, 0, kTrackShadow);
    ctx.selfShadowKeys[0].mode = wrap->postProcessEnabled ? 1 : 0;   // 0x45AB29
    ctx.selfShadowKeys[0].distance = 0.0112500004f;                   // flt_52A1D8
    s->state.selfShadowMode = 1;                 // 0x45AB46
    s->state.selfShadowEnabled = 0;
    gravityKeys[0].noise = 10;
    gravityKeys[0].acceleration = 9.8000002f;                    // 0x52A1DC
    gravityKeys[0].direction[1] = -1.0f;                         // 0x5295E8
    for (std::size_t i = 0; i < mdl::kTimelineKeyCapacity; ++i)   // 0x45AB84
        ctx.cameraKeys[i].parentModel = -1;
    for (int i = 0; i < 255; ++i) {                              // 0x45ABB0
        auto* const keys = mdl::KeyPoolCreate<mdl::AccessoryKey>();
        accTracks[i] = keys;
        std::memset(keys, 0, kTrackAcc);                          // 0x45ABC3
        keys[0].visible = 1;
        keys[0].parentModel = -1;
        keys[0].scale = 1.0f;
        keys[0].opacity = 1.0f;
        accs[i] = nullptr;
    }
}


// ---- 0x45AC10..0x45B090: camera track read (shorter v1 field set) +
// camera misc + frame UI (inverted 0x1BE sense) ------------------------
static void LoadSceneV1_CameraTrack(PmmV1LoadContext& ctx, int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    char* const text = ctx.text;
    mdl::CameraKey*& cameraKeys = ctx.cameraKeys;

    // ---- camera track read (0x45AC10..0x45ACF7) ---------------------------
    {
        // v1 camera record: 0x28 bytes, 6x4 interpolation bytes (the
        // 0x45AC96 loop bound is 6, as in v2), flag +0x40, dword +0x44,
        // flag +0x48 (no +0x4C/+0x50 dwords here - those come from the
        // config tail reregister pass).  Shape lives in
        // pmm_io_common.hpp ReadPmmCameraKey.
        ReadPmmCameraKey<PmmStream::V1>(fd, cameraKeys[0]);
        std::int32_t cnt = 0;
        pmm_io::ReadCount(fd, cnt);                                         // 0x45AD51
        for (std::int32_t i = 0; i < cnt; ++i) {
            std::int32_t timelineFrame = 0;
            Rd(fd, &timelineFrame, 4);
            // Deliberate hardening: the original applies this file-supplied
            // frame straight to the camera key track without comparing it
            // against the track capacity (0x2710 = 10000 appears only as a
            // traversal count and the loader region holds no capacity
            // comparison), so a frame past the end writes outside the track.
            // The port consumes the record and drops it, leaving the stream
            // position unchanged.
            // Defect B3-04.
            if (timelineFrame < 0) throw std::runtime_error("Negative PMM key slot");
            if (!mdl::KeyPoolEnsure(cameraKeys, std::uint64_t(timelineFrame) + 1)) throw std::bad_alloc();
            s->CameraKeys() = cameraKeys;
            ReadPmmCameraKey<PmmStream::V1>(fd, cameraKeys[timelineFrame]);
        }
    }
    LogV1Stage("camtrack-done", _tell(fd));
    // camera misc (0x45AEE7..0x45AF93)
    Rd(fd, &s->state.cameraPosX, 4);
    Rd(fd, &s->state.cameraPosY, 4);
    Rd(fd, &s->state.cameraPosZ, 4);
    Rd(fd, &s->state.viewOffsetX, 4);
    Rd(fd, &s->state.viewOffsetY, 4);
    Rd(fd, &s->state.cameraDistance, 4);      // 0xA08DC
    Rd(fd, &s->state.cameraPitch, 4);
    Rd(fd, &s->state.cameraYaw, 4);
    Rd(fd, &s->state.cameraRoll, 4);
    {
        unsigned char b = 0;
        Rd(fd, &b, 1);                                           // 0x45AF93
        s->state.cameraPerspective = (b == 1) ? 1 : 0;
    }
    // frame UI (0x45AFB8..0x45B07D); the original converts the fov float
    // with __ftol for both the range and the "%3d" text.
    {
        const int frame =
            static_cast<int>(s->state.cameraFov);    // 0x9E1E8
        // 0x405 = TBM_SETPOS (WM_USER+5, trackbar): wParam 1 = redraw,
        // lParam = position (0x45AFBF..0x45AFCE).
        SendMessageA(GetDlgItem(main, panel::kFovSlider), TBM_SETPOS, 1,
                     frame);                                     // 0x45AFCE
        HWND item = GetDlgItem(main, panel::kFovEdit);
        SendMessageA(item, EM_SETSEL, 0, GetWindowTextLengthA(item));
        sprintf_s(text, 0x100, "%3d", frame);                    // 0x45B022
        SendMessageA(item, EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>(text));
        // v1 checks the box when 0x31C == 0 (v2 uses != 0).
        SendMessageA(GetDlgItem(main, panel::kPerspectiveCheckbox), BM_SETCHECK,
                     s->state.cameraPerspective == 0 ? 1 : 0,
                     0);                                         // 0x45B07A
    }

    LogV1Stage("cammisc-done", _tell(fd));
}


// ---- 0x45B090..0x45B761: light track read + light misc + rgb/direction
// slider UI --------------------------------------------------------------
static void LoadSceneV1_LightTrack(PmmV1LoadContext& ctx, int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    char* const text = ctx.text;
    mdl::LightKey*& lightKeys = ctx.lightKeys;

    // ---- light track read (0x45B090..0x45B2D2) ----------------------------
    {
        // Record shape (37 stream bytes) lives in pmm_io_common.hpp
        // ReadPmmLightKey - byte-identical to the v2 reader.
        ReadPmmLightKey(fd, lightKeys[0]);
        std::int32_t cnt = 0;
        pmm_io::ReadCount(fd, cnt);                                         // 0x45B185
        for (std::int32_t i = 0; i < cnt; ++i) {
            std::int32_t timelineFrame = 0;
            Rd(fd, &timelineFrame, 4);
            // Deliberate hardening: the original writes this file-supplied
            // frame directly to the light key track without checking it
            // against the track capacity (0x2710 = 10000 appears only as a
            // traversal count, and the loader region performs no capacity
            // check), so a frame beyond the end writes outside the track.
            // The port consumes the record and discards it, leaving the
            // stream position unchanged.
            // Defect B3-04.
            if (timelineFrame < 0) throw std::runtime_error("Negative PMM key slot");
            if (!mdl::KeyPoolEnsure(lightKeys, std::uint64_t(timelineFrame) + 1)) throw std::bad_alloc();
            s->LightKeys() = lightKeys;
            ReadPmmLightKey(fd, lightKeys[timelineFrame]);
        }
    }
    // light misc (0x45B326..0x45B385)
    Rd(fd, s->LightColor() + 0, sizeof(float));
    Rd(fd, s->LightColor() + 1, sizeof(float));
    Rd(fd, s->LightColor() + 2, sizeof(float));
    Rd(fd, s->LightDirection() + 0, sizeof(float));
    Rd(fd, s->LightDirection() + 1, sizeof(float));
    Rd(fd, s->LightDirection() + 2, sizeof(float));

    LogV1Stage("light-done", _tell(fd));
    // rgb/direction slider UI (0x45B399..0x45B74E)
    {
        const int sliderIds[] = {455, 456, 457, 458, 459, 460};  // 0x1C7..0x1CC
        const int editIds[] = {461, 462, 463, 464, 465, 466};    // 0x1CD..0x1D2
        const float values[] = {
            s->LightColor()[0], s->LightColor()[1], s->LightColor()[2],
            s->LightDirection()[0], s->LightDirection()[1],
            s->LightDirection()[2]};
        for (int k = 0; k < 6; ++k) {
            const double scale = k < 3 ? 256.0 : 100.0;
            // 0x405 = TBM_SETPOS, same shape as the frame slider.
            SendMessageA(GetDlgItem(main, sliderIds[k]),
                         TBM_SETPOS, 1,
                         static_cast<int>(values[k] * scale));    // 0x45B3AF..
            HWND item = GetDlgItem(main, editIds[k]);
            SendMessageA(item, EM_SETSEL, 0, GetWindowTextLengthA(item));
            if (k < 3)
                sprintf_s(text, 0x100, "%3d",
                          static_cast<int>(values[k] * 256.0));  // 0x45B4DB..
            else
                sprintf_s(text, 0x100, "%+3.1f", values[k]);     // 0x45B643..
            SendMessageA(item, EM_REPLACESEL, 0,
                         reinterpret_cast<LPARAM>(text));
        }
    }

}


// ---- 0x45B761..0x45C479: accessory block (retry dialog; a cancelled
// picker keeps the empty accessory and goes on, retry failure ->
// close+ResetAppState WITHOUT HandleWindowSize) + UI; false = aborted ----
static bool LoadSceneV1_AccessoryBlock(PmmV1LoadContext& ctx, int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    PathResolutionWorkspace& paths = *ctx.paths;
    D3DRenderer* const wrap = ctx.wrap;
    mdl::AccessoryRecord** const accs = ctx.accs;
    mdl::AccessoryKey** const accTracks = ctx.accTracks;
    unsigned char** const slots = ctx.slots;
    char* const text = ctx.text;
    char* const mbPath = ctx.mbPath;
    wchar_t* const wideTmp = ctx.wideTmp;
    wchar_t* const ofnFile = ctx.ofnFile;
    wchar_t* const ofnTitle = ctx.ofnTitle;

    // ---- accessory block (0x45B761..0x45C479) -----------------------------
    Rd(fd, &s->SelectedAccessorySlot(), 1);                       // 0x45B761
    Rd(fd, &s->DisplayObjectListScrollPosition(), 4);             // 0x45B774
    SendMessageA(GetDlgItem(main, panel::kAccessoryCombo), CB_RESETCONTENT, 0, 0);// 0x45B78E
    SendMessageA(GetDlgItem(main, panel::kAttachBoneCombo), CB_RESETCONTENT, 0, 0);// 0x45B7A8
    unsigned char accCount = 0;
    Rd(fd, &accCount, 1);                                        // 0x45B7BC
    EnableMenuItem(GetMenu(main), 0xF9, accCount == 0 ? 1 : 0);  // 0x45B7EA
    // shadow name list (0x45B814..0x45B837)
    for (unsigned char i = 0; i < accCount; ++i) {
        pmm_io::ReadFixedText(fd, text, 100, 99);
        SendMessageA(GetDlgItem(main, panel::kAccessoryCombo), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text));
    }
    LogV1Stage("acc-head-done", _tell(fd), accCount);
    char accName[100];
    for (unsigned char i = 0; i < accCount; ++i) {
        unsigned char accSlot = 0;
        Rd(fd, &accSlot, 1);                                     // 0x45B867
        // An invalid slot has no object to pair with the shadow-list entry.
        // Continuing would preserve a phantom count and save an unreadable PMM.
        if (accSlot >= kModelSlotCount)
            throw std::runtime_error("Invalid PMM accessory slot");
        if (accs[accSlot] != nullptr)
            throw std::runtime_error("Duplicate PMM accessory slot");
        auto* acc = static_cast<mdl::AccessoryRecord*>(operator new(
            sizeof(mdl::AccessoryRecord)));                     // 0x45B871
        if (acc != nullptr) IdentityCtor(acc);                      // 0x45B88E
        accs[accSlot] = acc;
        std::memset(accs[accSlot], 0, sizeof(mdl::AccessoryRecord));
        Sub04B0Init(accs[accSlot]);                              // 0x45B8D6
        pmm_io::ReadFixedText(fd, accName, 100, 99);                                    // 0x45B8EA
        pmm_io::ReadFixedText(fd, mbPath, 0x100, 255);                                   // 0x45B901
        ResolveAnsiUserFile(reinterpret_cast<unsigned char*>(wrap),
                            mbPath, wideTmp, 0x100,
                            paths);                               // 0x45B92B
        if (!LoadAccessoryObject(s, accs[accSlot], wideTmp)) {   // 0x45B961
            sprintf_s(text, 0x100,
                      s->EnglishUI() != 0
                          ? "Cannot open file:%s.\n\nPlease select "
                            "accessory of %s."
                          : kJpCannotOpenAcc,
                      accName, accName);                         // 0x45B9CE
            LegacyUiMessageBoxA(main, text,
                        s->EnglishUI() != 0 ? "open file" : kJpOpenCaption,
                        0);                                      // 0x45B9ED
            SetCurrentDirectoryW(reinterpret_cast<const wchar_t*>(
                s->state.exeDir));                             // 0x45B9FA
            swprintf_s(ofnFile, 0x100, L"");                     // 0x45BA11
            OPENFILENAMEW ofn;
            std::memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);                              // 0x45BA22
            ofn.hwndOwner =
                s->state.floatingWindow
                    ? reinterpret_cast<HWND>(
                          s->state.floatingWindow)
                    : main;
            ofn.lpstrFilter =
                s->EnglishUI() != 0 ? kWFilterAccEn : kWFilterAccJp;
            ofn.lpstrFile = ofnFile;
            ofn.nMaxFile = 0x100;
            ofn.Flags = OFN_FILEMUSTEXIST;
            ofn.lpstrInitialDir =
                (GetMenuState(GetMenu(main), 0x12D, 0) & 8)
                    ? reinterpret_cast<LPCWSTR>(s->state.dirAccs)
                    : kWUserAcc;
            ofn.lpstrDefExt = L"x";                              // 0x5297D8
            ofn.nMaxFileTitle = 0x100;
            ofn.lpstrFileTitle = ofnTitle;
            ofn.lpstrTitle =
                s->EnglishUI() != 0
                    ? L"open file"                               // 0x52DA00
                    : reinterpret_cast<LPCWSTR>(kWOpenFile);
            if (!GetUiOpenFileNameW(&ofn)) {                       // 0x45BB0D
                // Closing the picker without a file skips this accessory's
                // file: the accessory keeps its initialised (empty) record and
                // its remaining stream records are still consumed below,
                // instead of aborting the whole project load.
            } else {
            if (GetMenuState(GetMenu(main), 0x12D, 0) & 8) {
                ExtractDirFromPath(ofnTitle, ofnFile,
                                      sizeof(ctx.ofnTitle)
                                          / sizeof(ctx.ofnTitle[0]));           // 0x45BB47
                wcscpy_s(reinterpret_cast<wchar_t*>(
                             s->state.dirAccs),
                         0x3E8, ofnTitle);                       // 0x45BB59
            }
            if (!LoadAccessoryObject(s, accs[accSlot], ofnFile)) {
                pmm_io::Close(fd);                                      // 0x45C5D5
                ResetAppState(s);                                // 0x45C5DF
                // NOTE: no HandleWindowSize on this path (unlike 0x45A7C5).
                return false;
            }
            }
        }

        LogV1Stage("acc-obj-done", _tell(fd), accSlot);
        mdl::AccessoryRecord& accessory = *mdl::Accessory(accs[accSlot]);
        Rd(fd, &accessory.order, 1);                              // 0x45BBB8
        accName[sizeof accName - 1] = '\0';
        const auto displayName = text_encoding::LegacyFilename(accName, accessory.sourcePath);
        text_encoding::EncodeDisplay(accessory.name, sizeof accessory.name, displayName.c_str());
        // Replace the shadow-list label using the resolved Unicode filename.
        HWND accessoryCombo = GetDlgItem(main, panel::kAccessoryCombo);
        SendMessageW(accessoryCombo, CB_DELETESTRING, accessory.order, 0);
        SendMessageW(accessoryCombo, CB_INSERTSTRING, accessory.order,
                     reinterpret_cast<LPARAM>(displayName.c_str()));// 0x45BBDA
        // accessory track record 0 + sparse keys (0x45BBF3..0x45C16F);
        // record shape (55 stream bytes, transparency quirk included) lives
        // in pmm_io_common.hpp ReadPmmAccessoryKey - byte-identical to v2.
        auto*& accessoryKeys = accTracks[accSlot];
        ReadPmmAccessoryKey(fd, accessoryKeys[0]);
        {
            std::int32_t cnt = 0;
            pmm_io::ReadCount(fd, cnt);                                     // 0x45BE2B
            for (std::int32_t k = 0; k < cnt; ++k) {
                std::int32_t timelineFrame = 0;
                Rd(fd, &timelineFrame, 4);
                // Deliberate hardening: the original applies this
                // file-supplied frame straight to the accessory key track
                // without comparing it against the track capacity (0x2710 =
                // 10000 appears only as a traversal count and the loader
                // region holds no capacity comparison), so a frame past the
                // end writes outside the track.  The port consumes the record
                // and drops it, leaving the stream position unchanged.
                // Defect B3-04.
                if (timelineFrame < 0) throw std::runtime_error("Negative PMM key slot");
            if (!mdl::KeyPoolEnsure(accessoryKeys, std::uint64_t(timelineFrame) + 1)) throw std::bad_alloc();
                ReadPmmAccessoryKey(fd, accessoryKeys[timelineFrame]);
            }
        }
        {
            unsigned char b = 0;                                 // 0x45C17F
            Rd(fd, &b, 1);
            accessory.visible = b & 1;
            accessory.opacity =
                static_cast<float>(100 - (b >> 1)) / 100.0f;
        }
        Rd(fd, &accessory.parentModel, 4);                        // 0x45C20B
        Rd(fd, &accessory.parentBone, 4);
        Rd(fd, &accessory.rotation[0], 4);
        Rd(fd, &accessory.rotation[1], 4);
        Rd(fd, &accessory.rotation[2], 4);
        Rd(fd, &accessory.scale, 4);
        Rd(fd, &accessory.position[0], 4);                        // 0x45C2C2
        Rd(fd, &accessory.position[1], 4);                        // 0x45C2E0
        Rd(fd, &accessory.position[2], 4);                        // 0x45C2FE
        {
            // v1 has only ONE trailing flag byte here (call 0x45C30A ->
            // 0x49C); the v2 body's second byte (0x49E) does not exist in
            // this format and would desync the stream by one byte per
            // accessory.
            unsigned char b = 0;
            Rd(fd, &b, 1);                                       // 0x45C30A
            accessory.shadowEnabled = (b == 1) ? 1 : 0;
        }
    }
    // current accessory selection UI (0x45C357..0x45C479)
    {
        const unsigned char cur = s->SelectedAccessorySlot();
        // Deliberate hardening: the selected object byte is read from the file
        // and is never bounds-checked against the 255-entry accessory array
        // (the loader region 0x140082D00..0x140085000 performs no capacity check),
        // so a value of 255 would read lastRegisteredFrame as a record pointer.
        // The port keeps the UI block on an in-range slot.
        // Defect B3-10.
        if (static_cast<int>(cur) < 255 && accs[cur] != nullptr) {
            mdl::AccessoryRecord& accessory = *mdl::Accessory(accs[cur]);
            SendMessageA(GetDlgItem(main, panel::kAccessoryCombo), CB_SETCURSEL,  // 0x45C390
                         accessory.order, 0);
            const std::int32_t parentSlot = accessory.parentModel;
            unsigned char* pm = parentSlot >= 0 && parentSlot < kModelSlotCount
                ? slots[parentSlot] : nullptr;
            if (pm != nullptr &&
                static_cast<std::int32_t>(
                    mikudancestudio::mdl::Mdl(pm)->boneCount) > 0) {
                FillBoneCombo(GetDlgItem(main, panel::kAttachBoneCombo),
                              mdl::Mdl(pm), UiUsesEnglishNames(s->EnglishUI()));
            }
            SyncAccessoryEditPanel(s);                                        // 0x45C479
        }
    }

    LogV1Stage("accessories-done", _tell(fd));
    return true;
}


// ---- 0x45C48C..0x45D278: config block head, register radios, checkbox
// bytes, wave/AVI/picture blocks, menu checks, FPS + capture-mode
// switches, physics defaults --------------------------------------------
static void LoadSceneV1_ConfigBlock(PmmV1LoadContext& ctx, int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    PathResolutionWorkspace& paths = *ctx.paths;
    D3DRenderer* const wrap = ctx.wrap;
    char* const text = ctx.text;
    char* const mbPath = ctx.mbPath;

    // ---- config block head (0x45C48C..0x45C553) ---------------------------
    Rd(fd, &s->state.currentFrame, 4);
    Rd(fd, &s->state.timelineStartFrame, 4);
    Rd(fd, &s->state.lastRegisteredFrame, 4);
    {
        HWND item = GetDlgItem(main, panel::kCurrentFrameEdit);
        SendMessageA(item, EM_SETSEL, 0, GetWindowTextLengthA(item));
        sprintf_s(text, 0x100, "%d",
                  s->state.currentFrame);         // 0x45C501
        SendMessageA(item, EM_REPLACESEL, 0,
                     reinterpret_cast<LPARAM>(text));
    }
    PostModelReload2(s);                                         // 0x45C52B
    PostLanguageSweep(s);                                        // 0x45C532
    HandleWindowSize(s);                                         // 0x45C539
    ApplyModelComboSelection(s);                                                // 0x45C540

    // register radios (0x45C553..0x45C766); v1 map: 0 -> 0x1EA, 1 -> 0x1EB,
    // 2 -> none of them checked, 3 -> 0x1ED, 4 -> 0x1EC.
    //
    // MMS: control 490 is the merged select / box-select cycle button and 491
    // is the bone scale mode, so the per-mode BM_SETCHECK series is written by
    // RefreshBoneModePanel instead.  The stored byte keeps its meaning and the
    // box-select state (1) still comes back as box select.
    std::int32_t savedEditMode = 0;
    Rd(fd, &savedEditMode, 4);                                  // 0x45C553
    s->EditMode() = static_cast<ViewportEditMode>(savedEditMode);
    RefreshBoneModePanel(s);
    {
        unsigned char b = 0;
        Rd(fd, &b, 1);                                           // 0x45C779
        s->state.cameraReferenceMode = b;
        SendMessageA(GetDlgItem(main, panel::kCameraRefModelCheckbox), BM_SETCHECK,
                     b == 1 ? 1 : 0, 0);
        SendMessageA(GetDlgItem(main, panel::kCameraRefBoneCheckbox), BM_SETCHECK,
                     b == 2 ? 1 : 0, 0);
        Rd(fd, &b, 1);                                           // 0x45C821
        s->state.playbackLoopEnabled = b ? 1 : 0;
        SendMessageA(GetDlgItem(main, 411), BM_SETCHECK, b ? 1 : 0, 0);
        Rd(fd, &b, 1);                                           // 0x45C879
        s->state.playbackReturnsToStartFrame = b ? 1 : 0;
        SendMessageA(GetDlgItem(main, 413), BM_SETCHECK, b ? 1 : 0, 0);
        Rd(fd, &b, 1);                                           // 0x45C8D1
        s->state.playbackStartsAtCurrentFrame = b ? 1 : 0;
        SendMessageA(GetDlgItem(main, 414), BM_SETCHECK, b ? 1 : 0, 0);
        std::int32_t playStartFrame = 0, playStopFrame = 0;
        Rd(fd, &playStartFrame, 4);                                          // 0x45C929
        {
            HWND item = GetDlgItem(main, panel::kPlayStartFrameEdit);
            SendMessageA(item, EM_SETSEL, 0, GetWindowTextLengthA(item));
            sprintf_s(text, 0x100, "%d", playStartFrame);                    // 0x45C97A
            SendMessageA(item, EM_REPLACESEL, 0,
                         reinterpret_cast<LPARAM>(text));
        }
        Rd(fd, &playStopFrame, 4);                                          // 0x45C9B1
        {
            HWND item = GetDlgItem(main, panel::kPlayStopFrameEdit);
            SendMessageA(item, EM_SETSEL, 0, GetWindowTextLengthA(item));
            sprintf_s(text, 0x100, "%d", playStopFrame);                    // 0x45CA05
            SendMessageA(item, EM_REPLACESEL, 0,
                         reinterpret_cast<LPARAM>(text));
        }
        ClearTimelineAndCurveDCs(s);                                            // 0x45CA2F
        Rd(fd, &b, 1);                                           // 0x45CA40
        s->state.waveEnabled = b ? 1 : 0;
        pmm_io::ReadFixedText(fd, mbPath, 0x100, 255);                                   // 0x45CA71
        ResolveAnsiUserFile(reinterpret_cast<unsigned char*>(wrap),
                            mbPath,
                            reinterpret_cast<wchar_t*>(s->state.wavPath),
                            0x100, paths);                        // 0x45CA9A
        if (s->state.waveEnabled != 0)
            LoadWaveFile(s);                                     // 0x45CAAA
        std::int32_t w1 = 0, w2 = 0, w3 = 0;
        Rd(fd, &w1, 4);                                          // 0x45CABE
        Rd(fd, &w2, 4);                                          // 0x45CACF
        Rd(fd, &w3, 4);                                          // 0x45CAE3
        pmm_io::ReadFixedText(fd, mbPath, 0x100, 255);                                   // 0x45CAFA
        ResolveAnsiUserFile(reinterpret_cast<unsigned char*>(wrap),
                            mbPath,
                            s->AviBackgroundPath(),
                            0x100, paths);                        // 0x45CB23
        if (mbPath[0] != 0)
            LoadAviFile(s);                                      // 0x45CB34

        // AVI block (0x45CB54..0x45CBFD); read-before-test on 0x91C.
        // (w1/w2/w3 -> 9E414/9E418/9E41C: the exact local-slot mapping in
        // the Ghidra output is scrambled by frame-base drift, so the v2
        // field order of the three dwords is used.)
        if (s->AviBackgroundEnabled() == 1) {
            Rd(fd, &s->AviBackgroundEnabled(), 4);               // 0x45CB54
            s->AviOffsetX() = w1;
            s->AviOffsetY() = w2;
            std::memcpy(&s->AviScale(), &w3, sizeof w3);
            if (s->AviBackgroundEnabled() == 1) {
                CheckMenuItem(GetMenu(main), 0xD8, 8);
                AviBgOverlayRefresh(s);                                    // 0x45CBA6
            } else {
                CheckMenuItem(GetMenu(main), 0xD8, 0);
            }
        } else {
            Rd(fd, &s->AviBackgroundEnabled(), 4);               // 0x45CBD5
            s->AviBackgroundEnabled() = 0;
            CheckMenuItem(GetMenu(main), 0xD8, 0);
            if (s->AviFrameReader() != nullptr) {
                AVIStreamGetFrameClose(s->AviFrameReader());
                s->AviFrameReader() = nullptr;
            }
            if (s->AviStream() != nullptr) {
                AVIStreamRelease(s->AviStream());
                s->AviStream() = nullptr;
            }
            if (s->AviFile() != nullptr) {
                AVIFileRelease(s->AviFile());
                s->AviFile() = nullptr;
            }
        }
        // picture block (0x45CC5F..0x45CD84); same local-slot caveat,
        // v2 field order p1/p2/p3 -> 9E434/9E438/9E43C.
        std::int32_t p1 = 0, p2 = 0, p3 = 0;
        Rd(fd, &p1, 4);                                          // 0x45CC5F
        Rd(fd, &p2, 4);                                          // 0x45CC70
        Rd(fd, &p3, 4);                                          // 0x45CC84
        pmm_io::ReadFixedText(fd, mbPath, 0x100, 255);                                   // 0x45CC9B
        ResolveAnsiUserFile(reinterpret_cast<unsigned char*>(wrap),
                            mbPath,
                            s->PictureBackgroundPath(),
                            0x100, paths);                        // 0x45CCC4
        s->PictureBackgroundEnabled() = 0;
        if (mbPath[0] != 0)
            LoadBackgroundPicture(s);                            // 0x45CCDC
        if (s->PictureBackgroundEnabled() == 0) {
            Rd(fd, &b, 1);                                       // 0x45CCF6
            s->PictureBackgroundEnabled() = 0;
            CheckMenuItem(GetMenu(main), 0xE9, 0);               // 0x45CD7D
        } else {
            Rd(fd, &b, 1);                                       // 0x45CCF6
            s->PictureOffsetX() = p1;
            s->PictureOffsetY() = p2;
            std::memcpy(&s->PictureScale(), &p3, sizeof p3);
            s->PictureBackgroundEnabled() = b ? 1 : 0;
            if (b != 0) {
                CheckMenuItem(GetMenu(main), 0xE9, 8);
                PicBgOverlayRefresh(s);                                    // 0x45CD4E
            } else {
                CheckMenuItem(GetMenu(main), 0xE9, 0);           // 0x45CD7D
            }
        }
    }
    // 0x31E / 0x31D / 0x918 menu checks (0x45CD95..0x45CF2C)
    {
        const HWND owner =
            s->state.floatingWindow
                ? reinterpret_cast<HWND>(s->state.floatingWindow)
                : main;
        unsigned char b = 0;
        Rd(fd, &b, 1);                                           // 0x45CD95
        if (b != 0) {
            s->state.fpsOverlayEnabled = 1;
            CheckMenuItem(GetMenu(main), 0xD3, 8);
            SendMessageA(GetDlgItem(owner, panel::kInfoCheckbox), BM_SETCHECK, 1, 0);
        } else {
            s->state.fpsOverlayEnabled = 0;
            CheckMenuItem(GetMenu(main), 0xD3, 0);
            SendMessageA(GetDlgItem(owner, panel::kInfoCheckbox), BM_SETCHECK, 0, 0);
        }
        Rd(fd, &b, 1);                                           // 0x45CE3F
        if (b != 0) {
            s->state.groundGridEnabled = 1;
            CheckMenuItem(GetMenu(main), 0xD7, 8);
            SendMessageA(GetDlgItem(owner, panel::kCoordAxisCheckbox), BM_SETCHECK, 1, 0);
        } else {
            s->state.groundGridEnabled = 0;
            CheckMenuItem(GetMenu(main), 0xD7, 0);
            SendMessageA(GetDlgItem(owner, panel::kCoordAxisCheckbox), BM_SETCHECK, 0, 0);
        }
        Rd(fd, &b, 1);                                           // 0x45CEE9
        s->state.groundShadowEnabled = b ? 1 : 0;
        CheckMenuItem(GetMenu(main), 0xDD, b ? 8 : 0);           // 0x45CF2C
    }

    LogV1Stage("config-done", _tell(fd));
    // FPS menu (0x45CF3F..0x45D02A); float compares against the 1000.0/30.0
    // double constants at 0x52BA60/0x52BA68.
    Rd(fd, &s->state.fpsLimit, 4);      // 0x45CF3F
    {
        const float v = s->state.fpsLimit;
        if (v == 1000.0f) {
            CheckMenuItem(GetMenu(main), 0xEB, 0);
            CheckMenuItem(GetMenu(main), 0xEC, 0);
            CheckMenuItem(GetMenu(main), 0xEA, 8);
        } else if (v == 30.0f) {
            CheckMenuItem(GetMenu(main), 0xEA, 0);
            CheckMenuItem(GetMenu(main), 0xEC, 0);
            CheckMenuItem(GetMenu(main), 0xEB, 8);
        } else {
            CheckMenuItem(GetMenu(main), 0xEB, 0);
            CheckMenuItem(GetMenu(main), 0xEA, 0);
            CheckMenuItem(GetMenu(main), 0xEC, 8);
        }
    }
    // screen-capture mode (0x45D03D..0x45D1CF)
    Rd(fd, &reinterpret_cast<ScreenCaptureMode&>(s->state.captureMode), 4);         // 0x45D03D
    switch (static_cast<std::int32_t>(s->state.captureMode)) {
        case 0:
            CheckMenuItem(GetMenu(main), 0xF3, 8);
            CheckMenuItem(GetMenu(main), 0xF4, 0);
            CheckMenuItem(GetMenu(main), 0xF5, 0);
            CheckMenuItem(GetMenu(main), 0xF6, 0);
            break;
        case 1:
            CheckMenuItem(GetMenu(main), 0xF3, 0);
            CheckMenuItem(GetMenu(main), 0xF4, 8);
            CheckMenuItem(GetMenu(main), 0xF5, 0);
            CheckMenuItem(GetMenu(main), 0xF6, 0);
            break;
        case 2:
            CheckMenuItem(GetMenu(main), 0xF3, 0);
            CheckMenuItem(GetMenu(main), 0xF4, 0);
            CheckMenuItem(GetMenu(main), 0xF5, 8);
            CheckMenuItem(GetMenu(main), 0xF6, 0);
            break;
        default:
            CheckMenuItem(GetMenu(main), 0xF3, 0);
            CheckMenuItem(GetMenu(main), 0xF4, 0);
            CheckMenuItem(GetMenu(main), 0xF5, 0);
            CheckMenuItem(GetMenu(main), 0xF6, 8);
            break;
    }
    // physics defaults + combos (0x45D1C8..0x45D265)
    s->state.gravityMagnitude = 9.8000002f;              // 0x52A1DC
    s->state.gravityNoiseTimer = 0.0f;
    s->state.gravityX = 0.0f;
    s->state.gravityY = -1.0f;                     // 0x5295E8
    s->state.gravityNoiseEnabled = 0;
    s->state.gravityNoise = 10;
    s->state.gravityZ = 0.0f;
    s->state.rigidBodyDisplayEnabled = 0;
    s->state.modelOutlineColorRed = 0;
    s->state.modelOutlineColorGreen = 0;
    s->state.modelOutlineColorBlue = 0;
    SendMessageA(GetDlgItem(main, panel::kMainComboNormal), CB_SETCURSEL, 0, 0);   // 0x45D248
    SendMessageA(GetDlgItem(main, panel::kBoneRegisterCombo), CB_SETCURSEL, 0, 0);   // 0x45D262

    LogV1Stage("physics-defaults-done", _tell(fd));
}


// ---- 0x45D278..0x45E117: read-gated tail.  The original tests every
// _read result here and unwinds early at end-of-stream; the if(Rd>0)
// tower is kept VERBATIM (flattening would change control flow) - the
// [read-gate N] comments mark each level -------------------------------
static void LoadSceneV1_ReadGatedTail(PmmV1LoadContext& ctx, int fd) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    unsigned char** const slots = ctx.slots;
    mdl::AccessoryRecord** const accs = ctx.accs;
    char* const lbText = ctx.lbText;
    mdl::CameraKey*& cameraKeys = ctx.cameraKeys;
    mdl::SelfShadowKey*& selfShadowKeys = ctx.selfShadowKeys;

    // ---- read-gated tail (0x45D278..0x45E117) ----------------------------
    // Unlike the rest of the body, this chain tests the _read results and
    // unwinds early at end-of-stream.
    // [read-gate 1] end-of-stream unwinds here
    if (pmm_io::OptionalRd(fd, &s->state.accessoryRenderSplitOrder, 4) > 0) {  // 0x45D278
        Rd(fd, &s->ProjectedShadowAmbientIntensity(), 4);       // 0x45D296
        s->SetProjectedShadowAmbient(s->ProjectedShadowAmbientIntensity());
        // x64 load twin sub_7FF7CB498E30: every post-read slot walk runs to
        // 255 (0xFF counters at 0x7FF7CB49D33A..0x7FF7CB4A2A3A in the tail).
        for (int i = 0; i < kModelSlotCount; ++i) {
            if (slots[i] != nullptr) {
                unsigned char b = 0;
                Rd(fd, &b, 1);                                   // 0x45D2EA
                mikudancestudio::mdl::Mdl(slots[i])->postLoadFlag2 =
                    (b == 1) ? 1 : 0;
            }
        }
        if (slots[s->SelectedModelSlot()] != nullptr &&
            mikudancestudio::mdl::Mdl(
                slots[s->SelectedModelSlot()])->postLoadFlag2 != 0 &&
            s->state.optflag[0] == 0)
            SendMessageA(GetDlgItem(main, panel::kAddBlendCheckbox), BM_SETCHECK, 1,
                         0);                                     // 0x45D360
        for (int i = 0; i < 255; ++i) {
            if (accs[i] != nullptr) {
                unsigned char b = 0;
                Rd(fd, &b, 1);                                   // 0x45D38F
                mdl::Accessory(accs[i])->additiveBlend = (b == 1) ? 1 : 0;
            }
        }
        {
            const unsigned char cur = s->SelectedAccessorySlot();
            // Deliberate hardening: the selected object byte originates from the
            // file and is never checked against the 255-entry accessory array
            // (the loader region 0x140082D00..0x140085000 performs no capacity
            // check), so a value of 255 treats lastRegisteredFrame as a
            // record pointer.  The port validates the index and leaves the
            // additive-blend prompt unchanged for an out-of-range value.
            // Defect B3-10.
            if (static_cast<int>(cur) < 255 && accs[cur] != nullptr &&
                mdl::Accessory(accs[cur])->additiveBlend != 0)
                SendMessageA(GetDlgItem(main, panel::kAccessoryAddBlendCheckbox), BM_SETCHECK, 1,
                             0);                                 // 0x45D3FB
        }
        std::int32_t v31C0 = 0;
        // [read-gate 2] end-of-stream unwinds here
        if (pmm_io::OptionalRd(fd, &v31C0, 4) > 0) {                             // 0x45D40F
            float edgeScaleBits;
            std::memcpy(&edgeScaleBits, &v31C0, sizeof edgeScaleBits);
            for (int i = 0; i < kModelSlotCount; ++i)
                if (slots[i] != nullptr)
                    mikudancestudio::mdl::Mdl(slots[i])->edgeScale =
                        edgeScaleBits;
            unsigned char b = 0;
            Rd(fd, &b, 1);                                       // 0x45D44E
            if (b == 1) {
                s->state.projectedShadowBlendEnabled = 1;
                CheckMenuItem(GetMenu(main), 0xFE, 8);           // 0x45D47A
            } else {
                s->state.projectedShadowBlendEnabled = 0;
            }
            const int got48F = pmm_io::OptionalRd(fd, &b, 1);                    // 0x45D48F
            // [read-gate 3] end-of-stream unwinds here
            if (b == 1 && got48F > 0) {
                // per-model 0x31C0 re-read pass (0x45D4B0..0x45D4FB):
                // ONE float dword per occupied slot, unlike the broadcast
                // sweep of the first dword above.
                for (int i = 0; i < kModelSlotCount; ++i) {
                    if (slots[i] != nullptr) {
                        std::int32_t edge = 0;
                        Rd(fd, &edge, 4);                        // 0x45D4D3
                        std::memcpy(&mikudancestudio::mdl::Mdl(slots[i])->edgeScale,
                                    &edge, sizeof edge);
                    }
                }
                unsigned char b509 = 0;
                const int got509 = pmm_io::OptionalRd(fd, &b509, 1);             // 0x45D509
                // [read-gate 4] end-of-stream unwinds here
                if (b509 == 1 && got509 > 0) {
                Rd(fd, reinterpret_cast<unsigned char*>(
                           &s->PlaybackPhysicsMode()),
                   1);                                           // 0x45D538
                // v1: only 0/1/2 update the menus; the default case leaves
                // them untouched (the v2 body re-checks 0x10E).
                switch (s->PlaybackPhysicsMode()) {
                    case 0:
                        CheckMenuItem(GetMenu(main), 0x10D, 0);
                        CheckMenuItem(GetMenu(main), 0x109, 0);
                        CheckMenuItem(GetMenu(main), 0x10E, 0);
                        CheckMenuItem(GetMenu(main), 0x110, 8);
                        break;
                    case 1:
                        CheckMenuItem(GetMenu(main), 0x10D, 0);
                        CheckMenuItem(GetMenu(main), 0x109, 8);
                        CheckMenuItem(GetMenu(main), 0x10E, 0);
                        CheckMenuItem(GetMenu(main), 0x110, 0);
                        break;
                    case 2:
                        CheckMenuItem(GetMenu(main), 0x10D, 8);
                        CheckMenuItem(GetMenu(main), 0x109, 0);
                        CheckMenuItem(GetMenu(main), 0x10E, 0);
                        CheckMenuItem(GetMenu(main), 0x110, 0);
                        break;
                    default:
                        break;
                }

                // physics reads (0x45D697..0x45D6E3)
                Rd(fd, &s->state.gravityMagnitude, 4);
                Rd(fd, &s->state.gravityNoise, 4);
                Rd(fd, &s->state.gravityX, 4);
                Rd(fd, &s->state.gravityY, 4);
                Rd(fd, &s->state.gravityZ, 4);
                unsigned char bc = 0;
                Rd(fd, &bc, 1);                                  // 0x45D6F4
                s->state.gravityNoiseEnabled =
                    (bc == 1) ? 1 : 0;
                {
                    unsigned char bd = 0;
                    const int got716 = pmm_io::OptionalRd(fd, &bd, 1);           // 0x45D716
                    // [read-gate 5] end-of-stream unwinds here
                    if (bd == 1 && got716 > 0) {
                        unsigned char be = 0;
                        Rd(fd, &be, 1);                          // 0x45D743
                        s->state.selfShadowMode = be;
                        s->state.selfShadowEnabled =
                            (be != 0) ? 1 : 0;
                        Rd(fd, &s->state.physicsInterval,
                           4);                                   // 0x45D76E
                        selfShadowKeys[0].mode =
                            static_cast<unsigned char>(
                                s->state.selfShadowMode);
                        selfShadowKeys[0].distance =
                            s->state.physicsInterval;
                        for (int i = 0; i < kModelSlotCount; ++i) {
                            if (slots[i] != nullptr) {
                                unsigned char b = 0;
                                Rd(fd, &b, 1);                   // 0x45D7BB
                                mikudancestudio::mdl::Mdl(slots[i])->toonFlag =
                                    (b == 1) ? 1 : 0;
                            }
                        }
                        unsigned char bs = 0;
                        const int got7FE = pmm_io::OptionalRd(fd, &bs, 1);       // 0x45D7FE
                        // [read-gate 6] end-of-stream unwinds here
                        if (bs == 1 && got7FE > 0) {
                            // selection/self-shadow track on app+0x37C
                            // (24-byte records); shape lives in
                            // pmm_io_common.hpp ReadPmmSelfShadowKey.
                            ReadPmmSelfShadowKey(fd, selfShadowKeys[0]);
                            std::int32_t cnt = 0;
                            pmm_io::ReadCount(fd, cnt);                     // 0x45D8C7
                            for (std::int32_t i = 0; i < cnt; ++i) {
                                std::int32_t timelineFrame = 0;
                                Rd(fd, &timelineFrame, 4);
                                // Deliberate hardening: the original applies
                                // the frame read from the file directly to the
                                // self-shadow key track without checking it
                                // against the track capacity (0x2710 = 10000
                                // is used only as a traversal count, and the
                                // loader performs no capacity
                                // check), so a frame past the end
                                // writes past the track.  The port
                                // consumes the record and discards it, keeping
                                // the stream position unchanged.
                                // Defect B3-04.
                                if (timelineFrame < 0) throw std::runtime_error("Negative PMM key slot");
            if (!mdl::KeyPoolEnsure(selfShadowKeys, std::uint64_t(timelineFrame) + 1)) throw std::bad_alloc();
            s->ShadowKeys() = selfShadowKeys;
                                ReadPmmSelfShadowKey(fd,
                                                     selfShadowKeys[
                                                         timelineFrame]);
                            }
                            unsigned char bq = 0;
                            const int got9EC = pmm_io::OptionalRd(fd, &bq, 1);   // 0x45D9EC
                            // [read-gate 7] end-of-stream unwinds here
                            if (bq == 1 && got9EC > 0) {
                                // model color sweep (0x45DA1B..0x45DA91)
                                Rd(fd, &s->state.modelOutlineColorRed,
                                   4);
                                Rd(fd, &s->state.modelOutlineColorGreen,
                                   4);
                                Rd(fd, &s->state.modelOutlineColorBlue,
                                   4);
                                if (s->state.modelOutlineColorRed != 0 ||
                                    s->state.modelOutlineColorGreen != 0 ||
                                    s->state.modelOutlineColorBlue != 0) {
                                    for (int i = 0; i < kModelSlotCount; ++i)
                                        if (slots[i] != nullptr)
                                            SetModelColor(
                                                reinterpret_cast<MMDApp*>(
                                                    slots[i]),
                                                s->state.modelOutlineColorRed,
                                                s->state.modelOutlineColorGreen,
                                                s->state.modelOutlineColorBlue);
                                }

                                unsigned char br = 0;
                                const int gotAB4 = pmm_io::OptionalRd(fd, &br, 1); // 0x45DAB4
                                // [read-gate 8] end-of-stream unwinds here
                                if (br == 1 && gotAB4 > 0) {
                                    unsigned char b194 = 0;
                                    Rd(fd, &b194, 1);            // 0x45DAE1
                                    s->state.blackBackgroundEnabled = (b194 != 0) ? 1 : 0;
                                    CheckMenuItem(GetMenu(main), 0x11A,
                                                  b194 ? 8 : 0);
                                    unsigned char bcam = 0;
                                    const int gotB35 =
                                        pmm_io::OptionalRd(fd, &bcam, 1);        // 0x45DB35
                                    // [read-gate 9] end-of-stream unwinds here
                                    if (bcam == 1 && gotB35 > 0) {
                                        // camera parent reregister
                                        // (0x45DB67..0x45DC76)
                                        Rd(fd, &cameraKeys[0].parentModel, 4);
                                        Rd(fd, &cameraKeys[0].parentBone, 4);
                                        const std::int32_t nFrame =
                                            cameraKeys[0].parentBone;
                                        const std::int32_t nParent =
                                            cameraKeys[0].parentModel;
                                        if (nFrame >= 0 &&
                                            nFrame < 10000 &&
                                            nParent > -1 &&
                                            nParent < 1000) {

                                            std::int32_t cnt = 0;
                                            pmm_io::ReadCount(fd, cnt);     // 0x45DBD3
                                            for (std::int32_t i = 0;
                                                 i < cnt; ++i) {
                                                std::int32_t timelineFrame = 0;
                                                Rd(fd, &timelineFrame, 4);
                                                // Deliberate hardening: the
                                                // original writes this
                                                // file-supplied frame directly
                                                // into the camera key track
                                                // without checking it against
                                                // the track capacity (0x2710 =
                                                // 10000 is used only as a
                                                // traversal count and the
                                                // loader region performs no
                                                // capacity check), so an
                                                // out-of-range frame writes
                                                // past the track.  The port
                                                // consumes the record and
                                                // discards it, leaving the
                                                // stream position unchanged.
                                                // Defect B3-04.
                                                if (timelineFrame < 0) throw std::runtime_error("Negative PMM key slot");
            if (!mdl::KeyPoolEnsure(cameraKeys, std::uint64_t(timelineFrame) + 1)) throw std::bad_alloc();
            s->CameraKeys() = cameraKeys;
                                                Rd(fd, &cameraKeys[timelineFrame].parentModel,
                                                   4);           // 0x45DC14
                                                Rd(fd, &cameraKeys[timelineFrame].parentBone,
                                                   4);           // 0x45DC32
                                            }
                                            Rd(fd, &s->state.cameraParentModel, 4);              // 0x45DC59
                                            Rd(fd, &s->state.cameraParentBone, 4);              // 0x45DC6C
                                            const std::int32_t sel0 = s->state.cameraParentModel;
                                            const std::int32_t sel1 = s->state.cameraParentBone;
                                            // Deliberate hardening: the
                                            // original rejects only a negative
                                            // index (0x14008186F / 0x140081871)
                                            // and then reads modelSlots[]
                                            // without an upper bound; the
                                            // sibling accessory.parentModel
                                            // site above already checks the
                                            // model slot range, and this site
                                            // now matches it.
                                            // Defect B3-08.
                                            if (sel0 >= 0 &&
                                                sel0 < kModelSlotCount &&
                                                slots[sel0] != nullptr) {
                                                SendMessageA(
                                                    GetDlgItem(main,
                                                               panel::kMainComboNormal),
                                                    CB_SETCURSEL,
                                                    mikudancestudio::mdl::Mdl(
                                                        slots[sel0])->comboSelIndex,
                                                    0);        // 0x45DC9D
                                            }
                                            RefillBoneRegisterCombo(s, sel0); // 0x45DCAE

                                            SelectBoneCombo(GetDlgItem(main, panel::kBoneRegisterCombo), sel1);
                                        }
                                        unsigned char bdd = 0;
                                        const int gotDDFC =
                                            pmm_io::OptionalRd(fd, &bdd, 1);    // 0x45DDFC
                                        // [read-gate 10] end-of-stream unwinds here
                                        if (bdd == 1 && gotDDFC > 0) {
                                        // 16 config dwords
                                        // (0x45DE2B..0x45DF4E)
                                        Rd(fd, &s->state.cameraAttachmentBasis[0],
                                           4);
                                        for (int i = 0; i < 15; ++i)
                                            Rd(fd, &s->state.cameraAttachmentBasis[1 + i], 4);
                                        unsigned char bf7 = 0;
                                        Rd(fd, &bf7, 1);        // 0x45DF5F
                                        if (bf7 == 1) {
                                            s->state.followCameraEnabled = 1;
                                            CheckMenuItem(
                                                GetMenu(main), 0xF7, 8);
                                            SendMessageA(
                                                GetDlgItem(main, panel::kFollowCameraCheckbox),
                                                BM_SETCHECK, 1, 0);
                                        } else {
                                            s->state.followCameraEnabled = 0;
                                            CheckMenuItem(
                                                GetMenu(main), 0xF7, 0);
                                            SendMessageA(
                                                GetDlgItem(main, panel::kFollowCameraCheckbox),
                                                BM_SETCHECK, 0, 0);
                                        }

                                        unsigned char b478 = 0;
                                        Rd(fd, &b478, 1);        // 0x45DFEF
                                        s->state.cameraAttachmentTransformSuppressed =
                                            (b478 == 1) ? 1 : 0;
                                        unsigned char b11d = 0;
                                        const int got00E =
                                            pmm_io::OptionalRd(fd, &b11d, 1);    // 0x45E00E
                                        // [read-gate 11] end-of-stream unwinds here
                                        if (b11d == 1 && got00E > 0) {
                                            unsigned char bd2 = 0;
                                            Rd(fd, &bd2, 1);    // 0x45E03B
                                            if (bd2 != 0) {
                                                CheckMenuItem(
                                                    GetMenu(main),
                                                    0x11D, 8);
                                                s->state.floorVisible = 1;
                                                s->Physics()->groundBody
                                                    ->setCollisionFlags(btCollisionObject::CF_STATIC_OBJECT);
                                            } else {
                                                CheckMenuItem(
                                                    GetMenu(main),
                                                    0x11D, 0);
                                                s->state.floorVisible = 0;
                                                s->Physics()->groundBody
                                                    ->setCollisionFlags(-1);
                                            }
                                            unsigned char bfin = 0;
                                            const int got0BC =
                                                pmm_io::OptionalRd(fd, &bfin, 1); // 0x45E0BC
                                            // [read-gate 12] end-of-stream unwinds here
                                            if (bfin == 1 && got0BC > 0) {
                                                for (int i = 0;
                                                     i < kModelSlotCount;
                                                     ++i)
                                                    if (slots[i] !=
                                                        nullptr)
                                                        Rd(fd,
                                                           &mdl::Mdl(
                                                               slots[i])
                                                                ->comboSelIndex2,
                                                           1);  // 0x45E0FE
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    }
    }

}


// ---- 0x45E149..0x45E7F7: success tail - shadow-mode gate, light
// direction to the physics scene, window title, the two key-chain
// integrity boxes, combo 0x1B2 population, repaint ----------------------
static void LoadSceneV1_SuccessTail(PmmV1LoadContext& ctx) {
    auto* const s = ctx.s;
    HWND const main = ctx.main;
    D3DRenderer* const wrap = ctx.wrap;
    unsigned char** const slots = ctx.slots;
    char* const text = ctx.text;
    char* const lbText = ctx.lbText;
    wchar_t* const wndText = ctx.wndText;

    // ---- success tail (0x45E149..0x45E7F7) --------------------------------
    if (wrap->postProcessEnabled != 0) {
        RefreshSelfShadowPanel(s);                                             // 0x45E149
    } else {
        s->state.selfShadowEnabled = 0;
        s->state.selfShadowMode = 0;
    }
    CheckMenuItem(GetMenu(main), 0x117,
                  s->state.selfShadowEnabled != 0 ? 8 : 0);
    if (slots[s->SelectedModelSlot()] != nullptr &&
        mikudancestudio::mdl::Mdl(
            slots[s->SelectedModelSlot()])->toonFlag != 0 &&
        s->state.optflag[0] == 0)
        SendMessageA(GetDlgItem(main, panel::kShadowCheckbox), BM_SETCHECK, 1, 0); // 0x45E1C1
    {  // light direction into the physics scene (0x45E1EF..0x45E286)
        float dir[3] = {s->state.gravityX,
                        s->state.gravityY,
                        s->state.gravityZ};
        auto& d3dxApi = d3dx::Get();
        if (d3dxApi.Load()) {
            // Preserve d3dx9_32's reciprocal-sqrt rounding (see the v2
            // loader note); the original calls D3DXVec3Normalize directly.
            d3dxApi.vec3Normalize(dir, dir);
        } else {
            const double len =
                std::sqrt(static_cast<double>(dir[0]) * dir[0] +
                          static_cast<double>(dir[1]) * dir[1] +
                          static_cast<double>(dir[2]) * dir[2]);
            if (len > 0.0) {
                dir[0] = static_cast<float>(dir[0] / len);
                dir[1] = static_cast<float>(dir[1] / len);
                dir[2] = static_cast<float>(dir[2] / len);
            }
        }
        const float mag = s->state.gravityMagnitude;
        // The original reaches btDynamicsWorld::setGravity through a vtable
        // slot.  That slot number is an ABI implementation detail and is not
        // valid for the x64 Bullet build.  Keep the same scene state while
        // expressing the intended operation through Bullet's public API.
        PhysicsScene* const physics = s->Physics();
        if (physics != nullptr && physics->world != nullptr) {
            physics->world->setGravity(
                btVector3(dir[0] * mag * 10.0f,
                          dir[1] * mag * 10.0f,
                          dir[2] * mag * 10.0f));
        }
    }
    // Title brand: the port ships as MikuDanceStudio (About-box rename);
    // the same kAppTitleFormat constant serves the v2 load and pmm_save.cpp.
    SetProjectWindowTitle(main, s->EnvFileName());

    // physics key-chain integrity (0x26E8 array, 0x1C stride)
    // (0x45E2C2..0x45E3C0)
    for (int mi = 0; mi < kModelSlotCount; ++mi) {
        unsigned char* m = slots[mi];
        if (m == nullptr) continue;
        mdl::DisplayKey* keys = mdl::DisplayKeys(m);
        keys[0].previous = 0;
        std::int32_t cur = keys[0].next;
        if (cur == 0) continue;
        std::int32_t prev = 0;
        for (;;) {
                // Deliberate hardening: the original walks the chain with
                // only the prev/zero comparisons (0x1400825DA / 0x1400826D7
                // in the v2 twin) and never compares the link against the
                // display-key pool capacity, so an out-of-range link reads
                // outside the pool.  The port stops the walk at the capacity.
                // Defect B3-13.
                if (static_cast<std::uint32_t>(cur) >=
                    static_cast<std::uint32_t>(mdl::KeyPoolCapacity(keys)))
                    break;
                if (static_cast<std::int32_t>(keys[cur].previous) != prev) {
                    // The original passes four varargs to a five-placeholder
                    // format; the fifth reads stack garbage.  Reproduced.
                    sprintf_s(text, 0x100, kJpChainFmtDisp,
                              mdl::Mdl(m)->name,
                              keys[prev].frame, keys[prev].frame,
                              keys[prev].frame);
                LegacyUiMessageBoxA(main, text, kJpChainCapPhys, 0);
                keys[prev].next = 0;
                break;
            }
            prev = cur;
            cur = keys[cur].next;
            if (cur == 0) break;
        }
    }
    // per-bone key-chain integrity (0x26E0 array, 0x3C stride)
    // (0x45E3D9..0x45E510).  Like the v2 body, the first comparison of
    // each bone chain uses the bone index itself as the expected prev
    // value (0x45E52C reloads ECX from the loop counter before the
    // advance), so a root's first sparse key points back to b.
    for (int mi = 0; mi < kModelSlotCount; ++mi) {
        unsigned char* m = slots[mi];
        if (m == nullptr) continue;
        if (static_cast<std::int32_t>(mikudancestudio::mdl::Mdl(m)->boneCount) <= 0)
            continue;
        mdl::BoneKey* keys = mdl::BoneKeys(m);
        // Runtime pool capacity, resolved once for the whole per-bone chain
        // repair below.
        const std::uint32_t boneKeyCapacity =
            static_cast<std::uint32_t>(mdl::BoneKeyCapacity(*mdl::Mdl(m)));
        for (std::int32_t b = 0;
             b < static_cast<std::int32_t>(mikudancestudio::mdl::Mdl(m)->boneCount);
             ++b) {
            keys[b].previous = 0;
            std::int32_t cur = keys[b].next;
            if (cur == 0) continue;
            std::int32_t prev = b;
            for (;;) {
                // Deliberate hardening: the original traverses the chain using
                // only the prev/zero checks (0x1400825DA / 0x1400826D7
                // in the v2 twin) and never bounds the link against the
                // bone-key pool capacity, so an out-of-range link reads
                // past the pool.  The port ends the walk at that capacity.
                // Defect B3-13.
                if (static_cast<std::uint32_t>(cur) >=
                    boneKeyCapacity)
                    break;
                if (static_cast<std::int32_t>(keys[cur].previous) != prev) {
                    unsigned char* frameEntry =
                        mdl::BoneBytes(mdl::Bones(m),
                                       static_cast<std::size_t>(b));
                    sprintf_s(text, 0x100, kJpChainFmtPhys,
                              mdl::Mdl(m)->name,
                              reinterpret_cast<char*>(frameEntry),
                              keys[prev].frame,
                              keys[prev].frame);
                    LegacyUiMessageBoxA(main, text, kJpChainCapDisp, 0);
                    keys[prev].next = 0;
                    break;
                }
                prev = cur;
                cur = keys[cur].next;
                if (cur == 0) break;
            }
        }
    }

    // combo 0x1B2 population (0x45E585..0x45E753)
    {
        const LRESULT sel =
            SendMessageA(GetDlgItem(main, panel::kMainComboModel), CB_GETCURSEL, 0, 0);
        SendMessageA(GetDlgItem(main, panel::kRegisterScopeCombo), CB_RESETCONTENT, 0, 0);
        if (sel == 0) {
            if (s->EnglishUI() == 0) {
                SendMessageW(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(kWCamera));
                SendMessageW(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(kWLight));  // 0x45E672
                SendMessageW(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(kWSelfSh));
                SendMessageW(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(kWGravity));
            } else {
                SendMessageA(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>("camera")); // 0x45E5D8
                SendMessageA(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>("light"));
                SendMessageA(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>("s shadow"));
                SendMessageA(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>("gravity"));
            }
            const LRESULT n =
                SendMessageA(GetDlgItem(main, panel::kAccessoryCombo), CB_GETCOUNT, 0, 0);
            for (LRESULT i = 0; i < n; ++i) {                    // 0x45E705
                wchar_t accessoryLabel[256]{};
                SendMessageW(GetDlgItem(main, panel::kAccessoryCombo), CB_GETLBTEXT,
                             static_cast<WPARAM>(i),
                             reinterpret_cast<LPARAM>(accessoryLabel));
                SendMessageW(GetDlgItem(main, panel::kRegisterScopeCombo), CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(accessoryLabel));
            }
            SendMessageA(GetDlgItem(main, panel::kRegisterScopeCombo), CB_SETCURSEL, 0, 0);
        } else {
            int found = 0;                                       // 0x45E794
            while (found < kModelSlotCount &&
                   (slots[found] == nullptr ||
                    mikudancestudio::mdl::Mdl(slots[found])->comboSelIndex !=
                        static_cast<unsigned char>(sel)))
                ++found;
            if (found < kModelSlotCount) {
                s->SetSelectedModelSlot(static_cast<unsigned char>(found));
                PostLoadInit(slots[found]);
            }
        }
    }
    // child window refresh (0x45E7AA..0x45E7C4)
    if (s->state.floatingWindow != 0) {
        RefreshSeparateWindowViewport(s);                                             // 0x45E7AA
        InvalidateRect(
            reinterpret_cast<HWND>(s->state.floatingWindow),
            nullptr, FALSE);
    }
    if (s->state.optflag[0] != 0) {
        RefreshLightPanel(s);                                             // 0x45E7C7
        PanelPaint(s);                                            // 0x45E7CE
    }
    InvalidateRect(main, nullptr, FALSE);                        // 0x45E7DB
    RelayoutSidebarControls(s);                                                 // 0x45E7E2
    s->PhysicsResetPending() = 1;
    s->state.windowLayoutReady = 1;
    // Restore the tracked camera as the tracking toggle does. A saved model
    // editor pan must not offset it; free views and camera-panel edits remain.
    if (s->state.followCameraEnabled != 0 && s->CameraMode() == 0) {
        ReloadModels(s);
        ApplyCurrentCameraView(s);
    }
    s->ApplyTimelineLightState();
    TraceSceneLightState(s, "pmm-v1-load-tail");
    PostViewRefresh(s);                                           // 0x45E7F7
}


}  // namespace

void LoadSceneV1(MMDApp* app, int fd) {
    auto* s = app;
    PmmV1LoadContext ctx(app);
    pmm_settings::Reset(*app);

    LoadSceneV1_DisposeAndHeader(ctx, fd);            // 0x45916D..0x459630
    const unsigned char modelCount = LoadSceneV1_ModelPrePass(ctx, fd);
    LogV1Stage("pre-pass-done", _tell(fd), modelCount);
    if (modelCount != 0) {
        unsigned char modelIdx = 0;
        for (;;) {
            if (!LoadSceneV1_ModelBlock(ctx, fd))
                return;  // AbortV1Load already ran (0x45A7C5)
            if (++modelIdx >= modelCount) break;                 // 0x45A6B4
        }
    }

    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        if (auto* bytes = s->ModelSlot(slot)) {
            const auto& model = *mdl::Mdl(bytes);
            pmm_io::CheckKeyChains(model.boneKeys, mdl::BoneKeyCapacity(model), model.boneCount);
            pmm_io::CheckKeyChains(model.morphKeys, mdl::MorphKeyCapacity(model.morphKeys), model.morphCount);
            pmm_io::CheckKeyChains(mdl::DisplayKeys(bytes), mdl::KeyPoolCapacity(mdl::DisplayKeys(bytes)));
        }
    }
    LoadSceneV1_RegisterCombo(ctx);                   // 0x45A6D3..0x45AA89
    LoadSceneV1_ReallocateTracks(ctx);                // 0x45AA93..0x45ABCC
    LogV1Stage("regcombo-done", _tell(fd));
    LoadSceneV1_CameraTrack(ctx, fd);                 // 0x45AC10..0x45B090
    LoadSceneV1_LightTrack(ctx, fd);                  // 0x45B090..0x45B761
    if (!LoadSceneV1_AccessoryBlock(ctx, fd)) return; // 0x45B761..0x45C479
    LogV1Stage("accessories-done", _tell(fd));
    pmm_io::CheckKeyChains(s->CameraKeys(), mdl::KeyPoolCapacity(s->CameraKeys()));
    pmm_io::CheckKeyChains(s->LightKeys(), mdl::KeyPoolCapacity(s->LightKeys()));
    for (int slot = 0; slot < kModelSlotCount; ++slot)
        if (s->AccessorySlot(slot))
            pmm_io::CheckKeyChains(s->AccessoryKeys(slot), mdl::KeyPoolCapacity(s->AccessoryKeys(slot)));
    LoadSceneV1_ConfigBlock(ctx, fd);                 // 0x45C48C..0x45D278
    LoadSceneV1_ReadGatedTail(ctx, fd);               // 0x45D278..0x45E117
    pmm_io::CheckKeyChains(s->ShadowKeys(), mdl::KeyPoolCapacity(s->ShadowKeys()));
    LogV1Stage("gated-tail-done", _tell(fd));
    pmm_io::Close(fd);                                       // 0x45E11D
    LoadSceneV1_SuccessTail(ctx);                     // 0x45E149..0x45E7F7
    native_mesh::ProjectOpened(app, app->EnvFileName());
    mmv4mms::ProjectOpened(app, app->EnvFileName());
    recent_projects::Record(app, app->EnvFileName());
}

}  // namespace mikudancestudio
