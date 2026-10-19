#include "mikumikustudio/frame_limits.hpp"
#include <algorithm>
// ===========================================================================
// VA 0x00433A40 - playback-start state initialization
// VA 0x004341E0 - playback-stop UI/state restoration
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include <Windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/force_paste.hpp"

namespace mikumikustudio {

void SyncModelEditControls(unsigned char* model);  // VA 0x004A02C0, was Sub4A02C0
void ApplyAccessoryTrack(MMDApp* app, int index);  // VA 0x00413120
void ApplyGravityTrack(MMDApp* app);  // VA 0x00412330

namespace {

constexpr double kFrameRate = 30.0;
constexpr double kPiOver180 = 0.01745329238474369;

float ReadF32(const unsigned char* p) {
    float value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

void EnableRange(HWND parent, int first, int last, BOOL enabled) {
    for (int id = first; id <= last; ++id)
        EnableWindow(GetDlgItem(parent, id), enabled);
}

void SetMenuRange(HMENU menu, int first, int last, UINT state) {
    for (int id = first; id <= last; ++id)
        EnableMenuItem(menu, id, state);
}

// The original Help popup, located through a command of its own submenu instead
// of a top-level index.  "About" carries command 201 in both menu templates of
// res/templates/menu/menus.rc, and the port keeps its baking popup directly in
// front of Help, so index 7 names the baking popup and not Help.
HMENU HelpMenuFromBar(HMENU menu) {
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

void SetPhysicsMenuState(HWND hwnd, UINT state) {
    HMENU submenu = HelpMenuFromBar(GetMenu(hwnd));
    if (submenu == nullptr)
        return;
    MENUITEMINFOA item{};
    item.cbSize = sizeof item;
    item.fMask = MIIM_STATE;
    item.fState = state;
    // Runtime Help items shift the original model-extension popup. Its child
    // command is stable; position 2 now belongs to model-name translation.
    for (int i = 0; i < GetMenuItemCount(submenu); ++i) {
        HMENU child = GetSubMenu(submenu, i);
        if (child && GetMenuState(child, 259, MF_BYCOMMAND) != UINT(-1)) {
            SetMenuItemInfoA(submenu, UINT(i), TRUE, &item);
            break;
        }
    }
}

}  // namespace

// Exported at mikumikustudio scope for the fullscreen record restore path
// (0x4629D0 in src/app/avi_record_start.cpp); the bodies were
// originally file-local here.
// VA 0x00429790.  UINT 1 is MF_GRAYED, matching the immediate operands in
// the original rather than treating it as a Boolean.
void DisablePlaybackMenus(HWND hwnd) {
    EnableWindow(GetDlgItem(hwnd, panel::kForcePasteButton), FALSE);
    HMENU menu = GetMenu(hwnd);
    EnableMenuItem(menu, panel::kForcePasteButton, MF_GRAYED);
    SetMenuRange(menu, 202, 210, MF_GRAYED);
    SetMenuRange(menu, 212, 213, MF_GRAYED);
    SetMenuRange(menu, 217, 220, MF_GRAYED);
    SetMenuRange(menu, 222, 232, MF_GRAYED);
    SetMenuRange(menu, 237, 242, MF_GRAYED);
    EnableMenuItem(menu, 251, MF_GRAYED);
    EnableMenuItem(menu, 252, MF_GRAYED);
    EnableMenuItem(menu, 276, MF_GRAYED);
    SetPhysicsMenuState(hwnd, MFS_DISABLED | MFS_GRAYED);
    DrawMenuBar(hwnd);
}

// VA 0x004298E0.  This restores the broad menu set and then reapplies the
// camera/model-mode restrictions that exist outside playback.
void RestorePlaybackMenus(MMDApp* app) {
    HWND hwnd = static_cast<HWND>(app->Hwnd());
    HMENU menu = GetMenu(hwnd);
    SetMenuRange(menu, 202, 210, MF_ENABLED);
    SetMenuRange(menu, 212, 213, MF_ENABLED);
    SetMenuRange(menu, 217, 220, MF_ENABLED);
    SetMenuRange(menu, 222, 232, MF_ENABLED);
    SetMenuRange(menu, 237, 242, MF_ENABLED);
    EnableMenuItem(menu, 276, MF_ENABLED);

    if (app->state.optflag[0] != 0) {
        const int disabled[] = {217, 220, 218, 202, 203, 219, 222, 251, 252};
        for (int id : disabled)
            EnableMenuItem(menu, id, MF_GRAYED);
        SetMenuRange(menu, 224, 231, MF_GRAYED);
        SetMenuRange(menu, 273, 275, MF_GRAYED);
        EnableWindow(GetDlgItem(hwnd, panel::kExpandShrinkButton), FALSE);
        EnableRange(hwnd, 437, 445, FALSE);
    } else {
        SetMenuRange(menu, 237, 242, MF_GRAYED);
        const int enabled[] = {202, 203, 219, 222, 251, 252};
        for (int id : enabled)
            EnableMenuItem(menu, id, MF_ENABLED);
        SetMenuRange(menu, 224, 231, MF_ENABLED);
        SetMenuRange(menu, 273, 275, MF_ENABLED);
        EnableWindow(GetDlgItem(hwnd, panel::kExpandShrinkButton), TRUE);

        unsigned char* model = app->SelectedModel();
        const UINT state = model != nullptr && mikumikustudio::mdl::Mdl(model)->physicsMode == 2
                               ? MFS_DISABLED | MFS_GRAYED
                               : MFS_ENABLED;
        SetPhysicsMenuState(hwnd, state);
    }
    RefreshForcePasteControls(app);
    DrawMenuBar(hwnd);
}

namespace {

void ApplyProjection(MMDApp* app) {
    D3DRenderer* wrapper = app->Renderer();
    auto& api = d3dx::Get();
    d3dx::D3DXMATRIXF matrix{};
    const float fov = static_cast<float>(
        static_cast<double>(app->CameraFov()) * kPiOver180);
    if (api.Load())
        api.perspectiveFovLH(&matrix, fov, wrapper->aspectRatio,
                             1.0f, 100000.0f);
    IDirect3DDevice9* device = wrapper->device;  // +120032
    device->SetTransform(D3DTS_PROJECTION,
                         reinterpret_cast<const D3DMATRIX*>(&matrix));
}

void CopyCameraTerminal(MMDApp* app, const mdl::CameraKey& key) {
    for (int axis = 0; axis < 3; ++axis)
        app->CameraPositionAxis(axis) = key.eye[axis];
    std::memcpy(app->CameraRotation(), key.target, sizeof(key.target));
    app->CameraDistance() = key.distance;
    app->CameraPerspective() = key.perspective;
    app->CameraFov() = static_cast<float>(key.fov);
    app->CameraParentModel() = key.parentModel;
    app->CameraParentBone() = key.parentBone;
    ApplyProjection(app);
}

void CopyLightTerminal(MMDApp* app, const mdl::LightKey& key) {
    std::memcpy(app->LightDirection(), key.direction, sizeof(key.direction));
    std::memcpy(app->LightColor(), key.color, sizeof(key.color));
    app->ApplyTimelineLightState();
    D3DRenderer* wrapper = app->Renderer();
    IDirect3DDevice9* device = wrapper->device;  // +120032
    device->SetLight(0, &app->SceneLight());
}

template <typename Key>
static const Key* SeekTypedTrackEnd(Key* keys, std::uint32_t& cursor,
                                    double frame,
                                    bool& ended) {
    cursor = 0;
    ended = false;
    if (static_cast<double>(keys[0].frame) >= static_cast<double>(frame)) {
        return &keys[0];
    }
    for (;;) {
        const Key& current = keys[cursor];
        if (current.next == 0) {
            ended = true;
            return &current;
        }
        cursor = current.next;
        if (static_cast<double>(keys[cursor].frame) >=
            static_cast<double>(frame)) {
            return &keys[cursor];
        }
    }
}

void InitLightAndShadowTracks(MMDApp* app, double frame) {
    bool ended = false;
    app->LightTrackActive() = 1;
    const mdl::LightKey* lightKey = SeekTypedTrackEnd(
        app->LightKeys(), app->LightTrackCursor(), frame, ended);
    if (ended) {
        app->LightTrackActive() = 0;
        CopyLightTerminal(app, *lightKey);
    }

    app->ShadowTrackActive() = 1;
    const mdl::SelfShadowKey* shadowKey = SeekTypedTrackEnd(
        app->ShadowKeys(), app->ShadowTrackCursor(), frame, ended);
    if (ended) {
        app->ShadowTrackActive() = 0;
        app->ShadowMode() =
            static_cast<std::int8_t>(shadowKey->mode);
        app->ShadowDistance() = shadowKey->distance;
    }
}

void InitGlobalTracks(MMDApp* app, double frame) {
    app->ViewOffsetX() = 0.0f;
    app->ViewOffsetY() = 0.0f;

    bool ended = false;
    app->CameraTrackActive() = 1;
    const mdl::CameraKey* cameraKey = SeekTypedTrackEnd(
        app->CameraKeys(), app->CameraTrackCursor(), frame, ended);
    if (ended) {
        app->CameraTrackActive() = 0;
        CopyCameraTerminal(app, *cameraKey);
    }

    InitLightAndShadowTracks(app, frame);

    app->GravityTrackActive() = 1;
    const mdl::GravityKey* gravityKey = SeekTypedTrackEnd(
        app->GravityKeys(), app->GravityTrackCursor(), frame, ended);
    if (ended) {
        app->GravityTrackActive() = 0;
        app->GravityNoiseEnabled() = gravityKey->noiseEnabled;
        app->GravityNoise() = gravityKey->noise;
        app->GravityMagnitude() = gravityKey->acceleration;
        std::memcpy(app->GravityDirection(), gravityKey->direction,
                    sizeof(gravityKey->direction));
    }
}

void InitAccessoryTracks(MMDApp* app, double frame) {
    for (int i = 0; i < 255; ++i) {
        app->AccessoryTrackActive(i) = 0;
        mdl::AccessoryRecord* accessory = app->AccessorySlot(i);
        if (accessory == nullptr)
            continue;

        app->AccessoryTrackActive(i) = 1;
        mdl::AccessoryKey* keys = app->AccessoryKeys(i);
        auto& cursor = app->AccessoryTrackCursor(i);
        cursor = 0;
        if (static_cast<double>(keys[0].frame) >= static_cast<double>(frame))
            continue;

        for (;;) {
            const int cur = static_cast<int>(cursor);
            const mdl::AccessoryKey& rec = keys[cur];
            const int next = static_cast<int>(rec.next);
            if (next != 0) {
                cursor = static_cast<std::uint32_t>(next);
                if (static_cast<double>(keys[next].frame) >=
                    static_cast<double>(frame))
                    break;
                continue;
            }

            app->AccessoryTrackActive(i) = 0;
            accessory->visible = rec.visible;
            accessory->shadowEnabled = rec.shadowEnabled;
            accessory->parentModel = rec.parentModel;
            accessory->parentBone = rec.parentBone;
            std::memcpy(accessory->rotation, rec.rotation,
                        sizeof accessory->rotation);
            std::memcpy(accessory->position, rec.position,
                        sizeof accessory->position);
            accessory->scale = rec.scale;
            accessory->opacity = rec.opacity;
            break;
        }
    }
}

void SavePlaybackUndoSnapshot(MMDApp* app, unsigned char* model) {
    mdl::ModelRecord& state = *mdl::Mdl(model);
    const int boneCount = static_cast<int>(state.boneCount);
    unsigned char* selected = state.bonePhysicsState;
    bool anySelected = false;
    for (int i = 0; i < boneCount; ++i)
        anySelected = anySelected || selected[i] != 0;
    if (!anySelected)
        return;

    HWND hwnd = static_cast<HWND>(app->Hwnd());
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
    state.undoDirty = 1;
    state.redoDirty = 0;
    std::uint32_t& undoIndex = state.undoState[0];
    undoIndex = (undoIndex + 1) % 30;
    state.undoState[1] = undoIndex;
    bone_scale::CaptureUndo(model);
    auto& undo = state.undoRings[0].slots[undoIndex];
    undo.operation = 3;
    undo.dirty = boneCount;
    undo.frame = app->state.currentFrame;
    // The other paths fill the undo ring's bonePose slot with the VC9
    // scalar operator new (ui_editor_click.cpp:342, query_audio_gaps.cpp:433
    // at 0x4A57F1, command_file_menu.cpp:1865), so ::operator new is the
    // allocation idiom for this field. This site paired std::malloc with the
    // CRT free instead - a third idiom for one slot - and free() on storage
    // obtained from ::operator new is undefined behaviour, even though the
    // MSVC CRT heap makes it work. The allocation and release here are now
    // ::operator new and ::operator delete, matching the rest of the
    // ring; the previous block is released before the new one is stored, so
    // the slot cannot be released twice.
    // Defect AB-06.
    auto*& buffer = undo.bonePose;
    ::operator delete(buffer);
    buffer = static_cast<mikumikustudio::mdl::BonePoseSnapshot*>(
        ::operator new(sizeof(*buffer) * boneCount));
    std::memset(buffer, 0, static_cast<std::size_t>(36) * boneCount);
    auto* records = buffer;
    mikumikustudio::mdl::BoneRecord* bones = mikumikustudio::mdl::Bones(model);
    for (int i = 0; i < boneCount; ++i) {
        auto& dst = records[i];
        mikumikustudio::mdl::BoneRecord* bone = &bones[i];
        dst.boneIndex = i;
        std::memcpy(dst.position, bone->trans, sizeof(dst.position));
        std::memcpy(dst.rotation, bone->rotQuat, sizeof(dst.rotation));
        dst.physicsDisabled = selected[i];
        selected[i] = 0;
    }
}

}  // namespace

void InitExportPlaybackTracks(MMDApp* app, double frame) {
    InitGlobalTracks(app, frame);
    InitAccessoryTracks(app, frame);
}

void UpdateBoneFrames(MMDApp* app) {
    app->SyncPlaybackTimeCompatibility();
    const double cursor = app->PlaybackCursorSeconds();
    app->SavedPlaybackPhysicsMode() = app->PlaybackPhysicsMode();
    if (app->state.playbackAlwaysOnOffMode != 0)
        app->PlaybackPhysicsMode() = 2;

    for (int i = 0; i < kModelSlotCount; ++i) {
        unsigned char* model = app->ModelSlot(i);
        if (model != nullptr)
            InitModelTrackCursors(model, cursor,
                      app->PlaybackPhysicsMode());
    }

    if (app->PlaybackFrameChanged() != 0)
        app->PlaybackFrameChanged() = 0;
    else
        app->PhysicsResetPending() = 1;

    const double frame = cursor * kFrameRate;
    if (app->FrameStepPlayback() != 0 || app->state.optflag[0] != 0 ||
        app->state.followCameraEnabled != 0)
        InitGlobalTracks(app, frame);
    else
        InitLightAndShadowTracks(app, frame);
    InitAccessoryTracks(app, frame);

    HWND hwnd = static_cast<HWND>(app->Hwnd());
    EnableRange(hwnd, 400, 401, FALSE);
    EnableRange(hwnd, 409, 410, FALSE);
    EnableRange(hwnd, 415, 468, FALSE);
    EnableRange(hwnd, 471, 487, FALSE);
    EnableRange(hwnd, 490, 501, FALSE);
    EnableRange(hwnd, 504, 527, FALSE);
    HWND overlay = app->FloatingWindow();
    EnableRange(overlay != nullptr ? overlay : hwnd, 536, 550, FALSE);
    DisablePlaybackMenus(hwnd);
    PostLanguageSweep2(app);
}

void StopPlayback(MMDApp* app) {  // was Sub4341E0
#ifdef MIKUMIKUSTUDIO_DIAG
    if (getenv("MIKUMIKUSTUDIO_TRACE_REC")) {
        FILE* tf = fopen(getenv("MIKUMIKUSTUDIO_TRACE_REC"), "a");
        if (tf) { fputs("StopPlayback stop-playback enter\n", tf); fclose(tf); }
    }
#endif
    HWND hwnd = static_cast<HWND>(app->Hwnd());
    EnableRange(hwnd, 400, 401, TRUE);
    EnableRange(hwnd, 409, 410, TRUE);
    EnableRange(hwnd, 415, 468, TRUE);
    EnableRange(hwnd, 471, 487, TRUE);
    EnableRange(hwnd, 490, 501, TRUE);
    EnableRange(hwnd, 504, 527, TRUE);
    HWND overlay = app->FloatingWindow();
    EnableRange(overlay != nullptr ? overlay : hwnd, 536, 550, TRUE);

    // snapshot bytes saved at playback start (command_control_400.cpp):
    // [0]=497 [1]=498 [2]=421 [3]=422 [4]=431 [5]=400 [6]=401
    EnableWindow(GetDlgItem(hwnd, panel::kBonePasteButton),
                 app->state.playbackEnabledSnapshot[0]);
    EnableWindow(GetDlgItem(hwnd, panel::kBoneReversePasteButton),
                 app->state.playbackEnabledSnapshot[1]);
    EnableWindow(GetDlgItem(hwnd, panel::kCurvePasteButton),
                 app->state.playbackEnabledSnapshot[4]);
    EnableWindow(GetDlgItem(hwnd, panel::kPasteButton),
                 app->state.playbackEnabledSnapshot[2]);
    EnableWindow(GetDlgItem(hwnd, panel::kReversePasteButton),
                 app->state.playbackEnabledSnapshot[3]);
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton),
                 app->state.playbackEnabledSnapshot[5]);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton),
                 app->state.playbackEnabledSnapshot[6]);

    const bool enable250 =
        (app->ClipboardCounts().bones == 0 ||
         app->state.optflag[0] != 0) &&
        !(app->ClipboardCounts().accessories != 0 &&
          app->state.optflag[0] != 0);
    EnableMenuItem(GetMenu(hwnd), 250,
                   enable250 ? MF_ENABLED : MF_GRAYED);
    RestorePlaybackMenus(app);

    app->PlaybackPhysicsMode() = app->SavedPlaybackPhysicsMode();

    unsigned char* activeModel = app->SelectedModel();
    if (app->state.playbackReturnsToStartFrame != 0) {
        if (activeModel != nullptr)
            SavePlaybackUndoSnapshot(app, activeModel);
        const int frame = static_cast<int>((std::clamp)(app->PlaybackCursorSeconds() * kFrameRate, 0.0, double(frame_limits::maximum)));
        app->state.currentFrame = frame;
        app->state.timelineStartFrame =
            frame > 6 ? frame - 6 : 0;
        char text[50];
        sprintf_s(text, "%d", frame);
        HWND edit = GetDlgItem(hwnd, panel::kCurrentFrameEdit);
        SendMessageA(edit, EM_SETSEL, 0, GetWindowTextLengthA(edit));
        SendMessageA(edit, EM_REPLACESEL, FALSE,
                     reinterpret_cast<LPARAM>(text));
        PanelPaint(app);
        if (app->state.waveEnabled != 0) {
            TimelineDrawTicks(app->state.timelineStartFrame,
                              app->SidebarWidth());
            RECT rect{6, 95,
                      app->SidebarWidth() - 3, 146};
            InvalidateRect(hwnd, &rect, FALSE);
        }
    }

    const int frame = app->state.currentFrame;
    for (int i = 0; i < kModelSlotCount; ++i) {
        unsigned char* model = app->ModelSlot(i);
        if (model != nullptr)
            SeekModelFrame(model, frame, app->PlaybackPhysicsMode());
    }
    if (app->state.optflag[0] == 0 &&
        activeModel != nullptr)
        SyncModelEditControls(activeModel);

    app->CameraParentModel() = -1;
    SendMessageA(GetDlgItem(hwnd, panel::kBoneRegisterCombo), CB_SETCURSEL, 0, 0);

    if (app->state.optflag[0] != 0) {
        // 原版会在此处清除视角偏移，跟踪分支中亦然；移植版保留视角：
        // 视角即当前帧的观察点，因此
        // 停止播放时，画面会停在最后播放的帧所处的位置，
        // 而不会回弹到初始状态。
        ReloadModels(app);
        RefreshLightPanel(app);
        RefreshSelfShadowPanel(app);
        ApplyGravityTrack(app);
    } else if (app->state.followCameraEnabled != 0) {
        if (app->CameraParentModel() >= 0) {
            ModelApplyMorphs(activeModel);
            SetPhysicsMode(activeModel, 0, app->ModelSlots(),
                           app->NativePhysicsMode());
        }
        // As in the camera-panel branch above: keep the eye unchanged so that stopping
        // does not move the viewpoint.
        ReloadModels(app);
        RefreshLightPanel(app);
        RefreshSelfShadowPanel(app);
        ApplyGravityTrack(app);
        app->CameraAttachmentTransformSuppressed() = 0;
        // Camera mode stops playback after evaluating the final camera key and
        // does not re-anchor the eye. Match that behavior while following from
        // model mode: the frame renderer already uses the evaluated parent and
        // bone as its pivot, while this one-time post-reload adjustment would
        // add the same camera/reference delta again at every playback stop.
    } else {
        RefreshLightPanel(app);
        RefreshSelfShadowPanel(app);
    }

    for (int i = 0; i < 255; ++i)
        if (app->AccessorySlot(i) != nullptr)
            ApplyAccessoryTrack(app, i);
    SyncAccessoryEditPanel(app);
    if (app->AviBackgroundEnabled() != 0)
        AviBgOverlayRefresh(app);
    app->PhysicsResetPending() = 1;
    PostViewRefresh(app);
    PostLanguageSweep2(app);
}

}  // namespace mikumikustudio
