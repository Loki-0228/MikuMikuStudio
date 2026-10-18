#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/mmv4mms.hpp"
#include "mikudancestudio/native_media.hpp"
#include "mikudancestudio/native_mesh.hpp"
// ===========================================================================
// VA 0x0044E540 - ResetAppState  (original: sub_44E540, 0x2AA0 bytes)
// ===========================================================================
// "New scene" (menu 0xCD): restores every default cluster, tears down the
// models/accessories/AVI handles, rebuilds the four global keyframe track
// arrays (camera 0x374 / light 0x378 / shadow 0x37C / physics 0x380) and
// the 255 accessory key-track blocks, resets the D3D light (D3DLIGHT9 at
// 0x9E180) and the menu/combo/edit state, then runs the standard refresh
// tail.  Field order verbatim from the decompilation; offsets decimal.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>   // TBM_SETPOS
#include <d3d9.h>
#include <vfw.h>

#include <btBulletDynamicsCommon.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/scene_ownership.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/ui_language.hpp"
#include "mikudancestudio/ui_translation.hpp"

namespace mikudancestudio {
void ReloadModels(MMDApp* app);   // VA 0x0042E640 (timeline_advance.cpp), originally named Sub42E640
// Frees the two 100000-slot scratch arrays of the physics-model editor.
// The function is defined in src/window/physics_model_dialog.cpp.
void ReleasePhysicsEditScratch(MMDApp* app);
}  // namespace mikudancestudio

namespace mikudancestudio {

// The original Help popup, located through a command of its own submenu instead
// of a top-level index.  "About" carries command 201 in both menu templates of
// res/templates/menu/menus.rc, and the port keeps its baking popup directly in
// front of Help, so index 7 names the baking popup and not Help.
static HMENU HelpMenuFromBar(HMENU menu) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW entry{};
        entry.cbSize = sizeof(entry);
        entry.fMask = MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, UINT(i), TRUE, &entry) || entry.hSubMenu == nullptr)
            continue;
        if (GetMenuState(entry.hSubMenu, 201, MF_BYCOMMAND) != UINT(-1))
            return entry.hSubMenu;
    }
    return nullptr;
}

// JP wide literals (byte-verified against the original .rdata):
// 0x52B784 カメラ / 0x52D390 照明 / 0x52D398 セルフ影 / 0x52D3A4 重力
static const wchar_t kJpCamera[] = L"\x30ab\x30e1\x30e9";
static const wchar_t kJpLight[] = L"\x7167\x660e";
static const wchar_t kJpSelfShadow[] = L"\x30bb\x30eb\x30d5\x5f71";
static const wchar_t kJpGravity[] = L"\x91cd\x529b";
// 0x52D370 ｶﾒﾗ／照明＋ｱｸｾｻﾘ
static const wchar_t kJpCamLightAcc[] =
    L"\xff76\xff92\xff97\xff65\x7167\x660e"
    L"\xff65\xff71\xff78\xff7e\xff7b\xff98";
// 0x52D368 地面 / 0x52D360 なし
static const wchar_t kJpGround[] = L"\x5730\x9762";
static const wchar_t kJpNone[] = L"\x306a\x3057";
// combo 433 channel names (0x52D3D4..0x52D3AC)
static const wchar_t kJpMoveX[] = L"\xff38\x79fb\x52d5";
static const wchar_t kJpMoveY[] = L"\xff39\x79fb\x52d5";
static const wchar_t kJpMoveZ[] = L"\xff3a\x79fb\x52d5";
static const wchar_t kJpRotation[] = L"\x56de\x3000\x8ee2";
static const wchar_t kJpDistance[] = L"\x8ddd\x3000\x96e2";
static const wchar_t kJpViewAngle[] = L"\x8996\x91ce\x89d2";
static const wchar_t kJpAll[] = L"\x3059\x3079\x3066";

// Replace the edit text like the original: select all, then REPLACESEL.
static void ReplaceEditText(HWND parent, int id, const char* text) {
    HWND edit = GetDlgItem(parent, id);
    SendMessageA(edit, EM_SETSEL, 0,
                 GetWindowTextLengthA(edit));                    // 0xB1
    SendMessageA(edit, EM_REPLACESEL, 0,
                 reinterpret_cast<LPARAM>(text));                // 0xC2
}

void ResetAppState(MMDApp* app) {
    app->PlaybackStartSeconds() = app->PlaybackCursorSeconds() = app->PlaybackEndSeconds() = 0;
    app->SyncPlaybackTimeCompatibility();
    ClearWaveAudio(app);
    media::ReleaseVideo(app);
    mmv4mms::ClearScene();
    native_mesh::ClearScene();
    ScopedSceneMutation mutation(*app);
    const bool rebuildCameraPanel = app->state.optflag[0] == 0;
    // A failed import may already have released its models and global tracks.
    // RebuildCameraModePanel evaluates those tracks, so let the refresh tail
    // evaluate the replacement scene. A model-visibility click is unsafe
    // here and unnecessary for models that are about to be discarded.
    app->state.optflag[0] = 1;
    app->SetSelectedModelSlot(0);
    HWND hwnd = app->MainWindow();
    app->SceneModified() = 0;
    pmm_settings::Reset(*app);

    // ---- menu checks + physics defaults -----------------------------------
    CheckMenuItem(GetMenu(hwnd), 0x10D, 8u);                     // 0x44E583
    CheckMenuItem(GetMenu(hwnd), 0x109, 0u);
    CheckMenuItem(GetMenu(hwnd), 0x10E, 0u);
    CheckMenuItem(GetMenu(hwnd), 0x110, 0u);
    app->GravityMagnitude() = 9.8000002f;
    PhysicsScene* scene = app->Physics();       // +650672 (kPtrSub048)
    app->PlaybackPhysicsMode() = 2;
    app->state.gravityNoiseTimer = 0.0f;
    app->GravityNoiseEnabled() = 0;
    app->GravityDirection()[0] = 0.0f;
    app->GravityNoise() = 10;
    app->state.rigidBodyDisplayEnabled = 0;
    app->GravityDirection()[1] = -1.0f;
    app->GravityDirection()[2] = 0.0f;
    {
        const float gravity[4] = {0.0f, -98.0f, 0.0f, 0.0f};     // 0x44E65F
        auto* world = scene->world;          // slot 64 / 0x40
        world->setGravity(*reinterpret_cast<const btVector3*>(gravity));
    }
    app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);
    app->CameraParentModel() = -1;
    app->CameraParentBone() = 0;
    for (float& v : app->state.cameraAttachmentBasis)
        v = 0.0f;
    app->state.cameraAttachmentBasis[15] = 1.0f;
    app->state.cameraAttachmentBasis[10] = 1.0f;
    app->state.cameraAttachmentBasis[5] = 1.0f;
    app->state.cameraAttachmentBasis[0] = 1.0f;
    std::memset(app->state.buf656632, 0, 0xC8);

    // ---- sub-window teardown ----------------------------------------------
    if (app->GravitySettingDialog() != nullptr)
        DestroyWindow(app->GravitySettingDialog());               // 0x44E719
    HWND w292 = app->state.frameCopyDialog;
    app->GravitySettingDialog() = nullptr;
    if (w292 != nullptr)
        DestroyWindow(w292);
    app->state.frameCopyDialog = nullptr;
    // The physics-model editor's two 100000-slot scratch arrays outlive its
    // window: DestroyWindow skips the OK/Cancel handlers that free them, so
    // every "new scene" or PMM load used to leak a pair (~30 MB).
    // The helper nulls both slots so a later teardown cannot free the same
    // block twice.
    // Defect B2-06.
    ReleasePhysicsEditScratch(app);
    HWND w256 = app->FrameRangeDialog();
    if (w256 != nullptr)
        DestroyWindow(w256);
    app->FrameRangeDialog() = nullptr;
    HWND w244 = app->state.edgeThicknessDialog;
    if (w244 != nullptr)
        DestroyWindow(w244);
    app->state.edgeThicknessDialog = nullptr;
    app->EnhancedModelDirty() = 0;
    app->state.mainModelComboSelection = 0;
    HWND parent = app->FloatingWindow();
    if (parent == nullptr)
        parent = hwnd;
    SetWindowTextA(GetDlgItem(parent, panel::kGotoFrameEdit), "0");                // 0x44E78A

    // ---- AVI background teardown ------------------------------------------
    PGETFRAME getFrame = static_cast<PGETFRAME>(app->AviFrameReader());
    app->ShadowDistance() = 0.01125f;
    app->ShadowMode() = 1;
    app->state.modelOutlineColorRed = 0;
    app->state.modelOutlineColorGreen = 0;
    app->state.modelOutlineColorBlue = 0;
    if (getFrame != nullptr) {
        AVIStreamGetFrameClose(getFrame);                        // 0x44E7C3
        app->AviFrameReader() = nullptr;
    }
    if (app->AviStream() != nullptr) {
        AVIStreamRelease(static_cast<PAVISTREAM>(app->AviStream()));
        app->AviStream() = nullptr;
    }
    if (app->AviFile() != nullptr) {
        AVIFileRelease(static_cast<PAVIFILE>(app->AviFile()));
        app->AviFile() = nullptr;
    }
    app->state.aviBackgroundEnabled = 0;
    swprintf_s(app->state.aviBackgroundPath, 0x100, L"");
    if (app->AviBackgroundTexture() != nullptr) {
        app->AviBackgroundTexture()->Release();
        app->AviBackgroundTexture() = nullptr;
    }

    // ---- camera/accessory edit state --------------------------------------
    // A new scene starts at the reference's default viewpoint: the camera key is
    // (0, 10, 0) at distance -45, with both view offsets cleared, exactly as the
    // reference's initializer writes them (a single 8-byte zero store at
    // app+0x340). The 10.0f in that viewpoint belongs to cameraPosY; raising a
    // view offset as well lifts the eye above the camera panel's pivot. This is
    // the startup "viewpoint is much too high" defect documented in
    // InitDefaults. ReloadModels, called at the end of this function, clears the
    // pair again.
    app->ViewOffsetX() = 0.0f;
    app->ViewOffsetY() = 0.0f;
    app->state.accessoryRenderSplitOrder = 1;
    app->CurrentFrame() = 0;
    app->CameraDistance() = -45.0f;
    app->ViewToolDragOperation() = ViewportToolAction::None;
    app->ViewportToolOperation() = ViewportToolAction::None;
    app->InteractionDragMode() = ViewportDragMode::None;
    app->CameraRotation()[0] = 0.0f;
    app->BoneBoxSelectionActive() = 0;
    app->CameraRotation()[1] = 0.0f;
    app->state.copiedBoneCount = 0;
    app->CameraRotation()[2] = 0.0f;
    app->state.timelineStartFrame = 0;
    app->LastRegisteredFrame() = 0;
    app->state.clipboardCounts = mdl::ClipboardSelectionCounts{};
    app->CaptureMode() = ScreenCaptureMode::Disabled;
    app->state.projectedShadowBlendEnabled = 1;

    // ---- models + global/accessory track arrays ---------------------------
    ReleaseSceneModels(*app);                                    // 0x44E95A
    ReleaseGlobalTimelineTracks(*app);                            // 0x44E99C
    ReleaseAccessoriesAndTracks(*app);                            // 0x44EA10

    // ---- combo refills ------------------------------------------------------
    // Application-owned vocabulary (not model names): the English string is the
    // key for the Chinese dictionary, so the CHS pass shares the EN branch
    // through AddUiComboText and the Japanese UI keeps its own literals - the
    // same three-way split LocalizeUI uses for these combos.  Testing the
    // language byte with `!= 0` alone made the Chinese UI fall into the English
    // branch and refill every combo with English text after a "new scene".
    SendMessageA(GetDlgItem(hwnd, panel::kIkChainCombo), CB_RESETCONTENT, 0, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kRegisterScopeCombo), CB_RESETCONTENT, 0, 0);
    HWND scopeCombo = GetDlgItem(hwnd, panel::kRegisterScopeCombo);
    if (app->state.englishUI != kUiJapanese) {   // English UI or the Chinese pass
        static const char* kScopes[] = {"camera", "light", "s shadow", "gravity"};
        for (const char* name : kScopes)
            AddUiComboText(app, scopeCombo, name);
    } else {
        SendMessageW(scopeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kJpCamera));
        SendMessageW(scopeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kJpLight));
        SendMessageW(scopeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kJpSelfShadow));
        SendMessageW(scopeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kJpGravity));
    }
    SendMessageA(GetDlgItem(hwnd, panel::kRegisterScopeCombo), CB_SETCURSEL, 0, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kMainComboModel), CB_RESETCONTENT, 0, 0);
    if (app->state.englishUI != kUiJapanese)
        AddUiComboText(app, GetDlgItem(hwnd, panel::kMainComboModel),
                       "camera/light/accessory");
    else
        SendMessageW(GetDlgItem(hwnd, panel::kMainComboModel), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kJpCamLightAcc));
    SendMessageA(GetDlgItem(hwnd, panel::kMainComboModel), CB_SETCURSEL, 0, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kMainComboGround), CB_RESETCONTENT, 0, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kMainComboNormal), CB_RESETCONTENT, 0, 0);
    if (app->state.englishUI != kUiJapanese) {
        AddUiComboText(app, GetDlgItem(hwnd, panel::kMainComboGround), "ground");
        AddUiComboText(app, GetDlgItem(hwnd, panel::kMainComboNormal), "non");
    } else {
        SendMessageW(GetDlgItem(hwnd, panel::kMainComboGround), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kJpGround));
        SendMessageW(GetDlgItem(hwnd, panel::kMainComboNormal), CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(kJpNone));
    }
    static const int kResetCombos[] = {475, 450, 471, 504, 509, 514, 519,
                                       433};
    for (int id : kResetCombos)
        SendMessageA(GetDlgItem(hwnd, id), CB_RESETCONTENT, 0, 0);
    if (app->state.englishUI != kUiJapanese) {
        static const char* kChannels[] = {"x axis move", "y axis move",
                                          "z axis move", "rotation",
                                          "distance", "view angle", "all"};
        for (const char* name : kChannels)
            AddUiComboText(app, GetDlgItem(hwnd, panel::kInterpCurveCombo), name);
    } else {
        static const wchar_t* kChannels[] = {kJpMoveX, kJpMoveY, kJpMoveZ,
                                             kJpRotation, kJpDistance,
                                             kJpViewAngle, kJpAll};
        for (const wchar_t* name : kChannels)
            SendMessageW(GetDlgItem(hwnd, panel::kInterpCurveCombo), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(name));
    }
    SendMessageA(GetDlgItem(hwnd, panel::kInterpCurveCombo), CB_SETCURSEL, 3, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kShadowCheckbox), BM_CLICK, 0, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kAddBlendCheckbox), BM_CLICK, 0, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kAccessoryAddBlendCheckbox), BM_CLICK, 0, 0);
    ReplaceEditText(hwnd, 417, "0");                              // 0x44F01D
    static const int kDisable[] = {430, 400, 401, 497, 498, 421, 422, panel::kForcePasteButton};
    for (int id : kDisable)
        EnableWindow(GetDlgItem(hwnd, id), FALSE);
    {
        static const int kFrameEdits[] = {425, 426, 409, 410};
        for (int id : kFrameEdits)
            ReplaceEditText(hwnd, id, "0");
        static const int kZeroEdits[] = {478, 479, 480, 481, 482, 483};
        for (int id : kZeroEdits)
            ReplaceEditText(hwnd, id, "0.0000");
        ReplaceEditText(hwnd, 484, "1.0000");
        ReplaceEditText(hwnd, 485, "1.0000");
    }
    ClearTimelineAndCurveDCs(app);                                               // 0x44F29F

    // ---- menu state ---------------------------------------------------------
    static const std::uint32_t kChecks[] = {
        0xD7u | (8u << 12), 0xDDu | (8u << 12), 0xECu | (8u << 12),
        0xEBu, 0xEAu, 0xF3u | (8u << 12), 0xF4u, 0xF5u, 0xF6u,
        0xFEu | (8u << 12)};
    for (std::uint32_t entry : kChecks)
        CheckMenuItem(GetMenu(hwnd), entry & 0xFFFu, entry >> 12);
    app->EditMode() = ViewportEditMode::None;
    static const int kClicks[] = {490, 491, 492, 493};
    for (int id : kClicks)
        SendMessageA(GetDlgItem(hwnd, id), BM_CLICK, 0, 0);
    static const std::uint32_t kGray[] = {0xF9, 0xFA, 0x120, 0x121};
    for (std::uint32_t id : kGray)
        EnableMenuItem(GetMenu(hwnd), id, 1u);                    // MF_GRAYED

    // ---- second AVI sweep + light/shadow menu defaults --------------------
    getFrame = static_cast<PGETFRAME>(app->AviFrameReader());
    app->state.waveEnabled = 0;
    if (getFrame != nullptr) {
        AVIStreamGetFrameClose(getFrame);                         // 0x44F4A2
        app->AviFrameReader() = nullptr;
    }
    if (app->AviStream() != nullptr) {
        AVIStreamRelease(static_cast<PAVISTREAM>(app->AviStream()));
        app->AviStream() = nullptr;
    }
    if (app->AviFile() != nullptr) {
        AVIFileRelease(static_cast<PAVIFILE>(app->AviFile()));
        app->AviFile() = nullptr;
    }
    app->state.fpsOverlayEnabled = 0;
    CheckMenuItem(GetMenu(hwnd), 0xD3, 0u);
    SendMessageA(GetDlgItem(app->FloatingWindow() != nullptr
                                ? app->FloatingWindow() : hwnd, panel::kInfoCheckbox),
                 BM_CLICK, 0, 0);
    app->state.groundGridEnabled = 1;
    CheckMenuItem(GetMenu(hwnd), 0xD7, 8u);
    // Reset to the enabled default without dispatching the user's toggle
    // command, which would immediately disable the just-restored grid.
    SendMessageA(GetDlgItem(app->FloatingWindow() != nullptr
                                ? app->FloatingWindow() : hwnd, panel::kCoordAxisCheckbox),
                 BM_SETCHECK, BST_CHECKED, 0);
    app->state.groundShadowEnabled = 1;
    CheckMenuItem(GetMenu(hwnd), 0xDD, 8u);

    // ---- rebuild the global track arrays ----------------------------------
    auto* camArr = mdl::KeyPoolCreate<mdl::CameraKey>();
    app->CameraKeys() = camArr;
    std::memset(camArr, 0,
                sizeof(mdl::CameraKey) * mdl::kTimelineKeyCapacity);
    auto* lightArr = mdl::KeyPoolCreate<mdl::LightKey>();
    app->LightKeys() = lightArr;
    std::memset(lightArr, 0,
                sizeof(mdl::LightKey) * mdl::kTimelineKeyCapacity);
    auto* shadowArr = mdl::KeyPoolCreate<mdl::SelfShadowKey>();
    app->ShadowKeys() = shadowArr;
    std::memset(shadowArr, 0,
                sizeof(mdl::SelfShadowKey) * mdl::kTimelineKeyCapacity);
    auto* physArr = mdl::KeyPoolCreate<mdl::GravityKey>();
    app->GravityKeys() = physArr;
    std::memset(physArr, 0,
                sizeof(mdl::GravityKey) * mdl::kTimelineKeyCapacity);
    for (int i = 0; i < 255; ++i) {                               // 0x44F627
        auto* track = mdl::KeyPoolCreate<mdl::AccessoryKey>();
        app->AccessoryKeys(i) = track;
        std::memset(track, 0,
                    sizeof(mdl::AccessoryKey) * mdl::kTimelineKeyCapacity);
        track[0].visible = 1;
        track[0].parentModel = -1;
        track[0].opacity = 1.0f;
        track[0].scale = 1.0f;
        app->AccessorySlot(i) = nullptr;
    }
    camArr[0].distance = -45.0f;
    camArr[0].eye[1] = 10.0f;
    camArr[0].perspective = 0;
    camArr[0].fov = 30;
    camArr[0].parentModel = -1;
    camArr[0].parentBone = 0;
    for (std::size_t rec = 0; rec < mdl::kTimelineKeyCapacity; ++rec) { // 0x44F6D0
        camArr[rec].parentModel = -1;
        for (int c = 0; c < 6; ++c) {
            camArr[rec].interpolation[0][c] = 20;
            camArr[rec].interpolation[1][c] = 20;
            camArr[rec].interpolation[2][c] = 107;
            camArr[rec].interpolation[3][c] = 107;
        }
    }

    // ---- D3DLIGHT9 default (cluster at 0x9E180) ---------------------------
    app->CameraFov() = 30.0f;
    app->LightDirection()[0] = -0.5f;
    app->LightDirection()[1] = -1.0f;
    app->LightDirection()[2] = 0.5f;
    D3DLIGHT9& light = app->SceneLight();
    light = {};
    light.Direction.x = app->LightDirection()[0];
    light.Direction.y = app->LightDirection()[1];
    light.Direction.z = app->LightDirection()[2];
    app->LightColor()[0] = 0.602f;
    app->LightColor()[1] = 0.602f;
    app->LightColor()[2] = 0.602f;
    light.Specular.r = 0.602f;
    light.Specular.g = 0.602f;
    // The v932 state block starts as a directional key light (D3DLIGHT9
    // type 3).  A point light at the origin happens to leave simple scenes
    // visible, but makes staged PMM scenes dramatically underlit.
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Specular.b = 0.602f;
    {
        D3DRenderer* r = app->Renderer();
        IDirect3DDevice9* device = r->device;  // +120032
        device->SetLight(0, &light);                               // 0x44F804
        device->LightEnable(0, TRUE);                              // 0x44F81F
    }
    lightArr[0].color[0] = 0.602f;
    lightArr[0].color[1] = 0.602f;
    lightArr[0].color[2] = 0.602f;
    lightArr[0].direction[0] = -0.5f;
    lightArr[0].direction[1] = -1.0f;
    lightArr[0].direction[2] = 0.5f;
    shadowArr[0].mode = static_cast<std::uint8_t>(
        app->Renderer()->postProcessEnabled != 0);  // wrapper+120132
    shadowArr[0].distance = 0.01125f;
    physArr[0].noiseEnabled = 0;
    physArr[0].noise = 10;
    physArr[0].acceleration = 9.8000002f;
    physArr[0].direction[1] = -1.0f;
    DrawMenuBar(hwnd);

    // ---- frame/title state --------------------------------------------------
    app->state.aviBackgroundEnabled = 0;
    app->state.pictureBackgroundEnabled = 0;
    app->state.characterTransparentMode = 0;
    CheckMenuItem(GetMenu(hwnd), 0xD6, 0u);
    CheckMenuItem(GetMenu(hwnd), 0xD8, 0u);
    CheckMenuItem(GetMenu(hwnd), 0xE9, 0u);
    {
        char title[0x100];
        sprintf_s(title, 0x100, "MikuMikuDance");
        SetWindowTextA(hwnd, title);
    }
    app->state.envFileName[0] = 0;
    app->state.optflag[1] = 1;
    app->state.optflag[2] = 1;
    app->state.optflag[3] = 1;
    app->state.optflag[4] = 1;
    app->state.optflag[5] = 1;
    app->state.optflag[6] = 1;
    app->CameraPerspective() = 0;
    SendMessageA(GetDlgItem(hwnd, panel::kPerspectiveCheckbox), BM_CLICK, 1, 0);
    app->CameraPositionAxis(0) = 0.0f;
    app->CameraPositionAxis(1) = 10.0f;
    app->CameraPositionAxis(2) = 0.0f;
    app->state.v32c = 0;
    app->CameraReferenceMode() = CameraAttachmentReference::None;
    SendMessageA(GetDlgItem(hwnd, panel::kCameraRefModelCheckbox), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kCameraRefBoneCheckbox), BM_SETCHECK, BST_UNCHECKED, 0);
    app->PlaybackLoopEnabled() = 0;
    SendMessageA(GetDlgItem(hwnd, 411), BM_CLICK, 0, 0);
    app->state.playbackReturnsToStartFrame = 0;
    SendMessageA(GetDlgItem(hwnd, 413), BM_CLICK, 0, 0);
    app->state.viewportToolHovered = 0;
    app->CameraFov() = 45.0f;
    SendMessageA(GetDlgItem(hwnd, panel::kAccessoryShadowCheckbox), BM_CLICK, 0, 0);
    SendMessageA(GetDlgItem(hwnd, panel::kAccessoryVisibleCheckbox), BM_CLICK, 0, 0);
    app->FpsLimit() = 60.0f;

    // ---- light-accessory edit echoes ---------------------------------------
    char text[0x100];
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(app->LightColor()[0] * 256.0));
    SendMessageA(GetDlgItem(hwnd, panel::kLightColorEditR), WM_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(text));
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(app->LightColor()[1] * 256.0));
    SendMessageA(GetDlgItem(hwnd, panel::kLightColorEditG), WM_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(text));
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(app->LightColor()[2] * 256.0));
    SendMessageA(GetDlgItem(hwnd, panel::kLightColorEditB), WM_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(text));
    sprintf_s(text, 0x100, "%+3.1f", app->LightDirection()[0]);
    SendMessageA(GetDlgItem(hwnd, panel::kLightDirEditX), WM_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(text));
    sprintf_s(text, 0x100, "%+3.1f", app->LightDirection()[1]);
    SendMessageA(GetDlgItem(hwnd, panel::kLightDirEditY), WM_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(text));
    sprintf_s(text, 0x100, "%+3.1f", app->LightDirection()[2]);
    SendMessageA(GetDlgItem(hwnd, panel::kLightDirEditZ), WM_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(text));
    sprintf_s(text, 0x100, "%3d",
              static_cast<int>(app->CameraFov()));
    SendMessageA(GetDlgItem(hwnd, panel::kFovEdit), WM_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(text));
    SendMessageA(GetDlgItem(hwnd, panel::kFovSlider), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<int>(app->CameraFov())));
    SendMessageA(GetDlgItem(hwnd, panel::kLightColorSliderR), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<int>(app->LightColor()[0] * 256.0)));
    SendMessageA(GetDlgItem(hwnd, panel::kLightColorSliderG), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<int>(app->LightColor()[1] * 256.0)));
    SendMessageA(GetDlgItem(hwnd, panel::kLightColorSliderB), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<int>(app->LightColor()[2] * 256.0)));
    SendMessageA(GetDlgItem(hwnd, panel::kLightDirSliderX), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<int>(app->LightDirection()[0] * 100.0)));
    SendMessageA(GetDlgItem(hwnd, panel::kLightDirSliderY), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<int>(app->LightDirection()[1] * 100.0)));
    SendMessageA(GetDlgItem(hwnd, panel::kLightDirSliderZ), TBM_SETPOS, 1,
                 static_cast<LPARAM>(
                     static_cast<int>(app->LightDirection()[2] * 100.0)));

    // ---- menu enable state --------------------------------------------------
    static const std::uint32_t kEnable[] = {0xED, 0xEE, 0xEF, 0xF0, 0xF1,
                                            0xF2};
    for (std::uint32_t id : kEnable)
        EnableMenuItem(GetMenu(hwnd), id, 0u);
    static const std::uint32_t kGray2[] = {0xFD, 0xD9, 0xDC, 0xDA, 0xCA,
                                           0xCB, 0xDB, 0xDE, 0xFB, 0xFC};
    for (std::uint32_t id : kGray2)
        EnableMenuItem(GetMenu(hwnd), id, 1u);
    for (std::uint32_t id = 224; id <= 231; ++id)
        EnableMenuItem(GetMenu(hwnd), id, 1u);
    for (std::uint32_t id = 273; id <= 275; ++id)
        EnableMenuItem(GetMenu(hwnd), id, 1u);
    SetModelExtensionMenuEnabled(app, false);
    DrawMenuBar(hwnd);

    // ---- refresh tail -------------------------------------------------------
    if (rebuildCameraPanel) HandleWindowSize(app);
    PostLanguageSweep(app);                                      // 0x42F1E0
    PanelPaint(app);                                             // 0x414610
    SelectionReeval(app);                                        // 0x430510
    ReloadModels(app);                                           // 0x42E640
    RefreshLightPanel(app);                                              // 0x411070
    RefreshSelfShadowPanel(app);                                              // 0x411B90
    ApplyGravityTrack(app);                                              // 0x412330
    PostModelReload2(app);                                       // 0x40D940
    PostViewRefresh(app);                                        // 0x40D130
    InvalidateRect(hwnd, nullptr, FALSE);
}

}  // namespace mikudancestudio
