// ===========================================================================
// Frame-driver interaction modes (0x0046B090 region @ 0x470Cxx..0x4712xx)
// ===========================================================================
// Verified raw offsets (Ghidra ground truth):
//   this+4/+8       previous mouse X/Y      this+0xC/+0x10 current mouse X/Y
//   this+0x24 (36)  precision selector A    this+0xC0 (192) selector B
//   this+0x88 (136) transform selector A    this+0x8C (140) selector B
//   this+0x2F8 (760) model-mode flag        this+0x330 (816) anim gate
//   this+0x308/30C/310 camera translate X/Y/Z
//   this+0x314/318  camera rotation angles
//   this+0x9EDD1 (650705) drag-active flag
//   this+0x9ED98 (650648) select-state gate
//   this+0x910 (2320) frame index  this+0xA0430 (656432) frame cursor
//   this+0xA05D1 (656849) view-dirty  this+0xA442C (672812) view-lock
//
// Camera-translate block ported verbatim from the raw listing at 4638..4686:
//   gate: dragActive & (this+0x8C == 3) & !modelMode
//   scale: selector A(this+0x24==3) -> 0.01, selector B(this+0xC0==3) ->
//          PI/180, else g_MouseScaleC
//   camY += (prevY - curY) * scale;  camX -= (prevX - curX) * scale
//   then 0x40D070/0x40D130 view refresh under the frame-cursor gate.
//
// Runtime scale globals (definitions below).  x64 empirical (2026-09-07,
// reference image SHA-256 882C97DC..., see docs/PORTING_STATUS.md):
//   the x64 image keeps these as FLOAT32 constant-pool entries, not
//   doubles and not writable globals:
//     A 0.5f   @0x14012A4F0 and @0x14012A520 (two copies)
//     B 0.005f @0x140132C2C   C 0.05f @0x140132CB0   D 0.002f @0x140132CF0
//   plus 0.01f @0x140132C28 (camera-translate alternative) and
//   PI/180 float @0x140132BA4 in the same pool.  The port keeps the x86
//   x87-shaped doubles for A/B/C (bit-level deviation vs the float32 x64
//   pool - open recommendation in docs/PORTING_STATUS.md INIT-MOUSE-A..D);
//   D is float32 and matches.  x86 0x52B8F0/0x52E9C0/0x52D738/0x52E8C0
//   image bytes remain unverified (no x86 binary at hand).
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/panel_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
#include "mikumikustudio/viewport_tool_layout.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "frame_state_dump.hpp"

namespace mikumikustudio {

// Exact doubles from the original image.  These stay double until the final
// store, matching the x87 instruction stream at 0x470E4B..0x470EDF.
double g_MouseScaleA = 0.5;                       // VA 0x0052B8F0
double g_MouseScaleB = 0.004999999888241291;      // VA 0x0052E9C0
double g_MouseScaleC = 0.05000000074505806;       // VA 0x0052D738
float g_MouseScaleD = 0.0020000000949949f;        // VA 0x0052E8C0 (true
                                                  // .rdata double; the old
                                                  // PI/360 guess was wrong)

static void ViewRefreshGate(MMDApp* s) {
    // Original dword gates (Ghidra: this+0x9ED98 select-state gate,
    // this+0xA0430 frame cursor).  The old literals 650136/657456 were
    // decimal slips - 650136 landed inside aviOutputPath and 657456 inside
    // exeDir.  0x9ED98 is a dword over followCameraEnabled and the two shadow bytes;
    // 0x2F8 is a dword over the first four radio flags.
    const bool frameGate =
        (s->state.followCameraEnabled | s->state.playbackStartsAtCurrentFrame |
         s->state.projectedShadowBlendEnabled) != 0 &&
        s->state.slotIdx == s->state.cameraParentModel &&
        (s->state.optflag[0] | s->state.optflag[1] |
         s->state.optflag[2] | s->state.optflag[3]) == 0 &&
        s->state.cameraParentModel >= 0 &&
        s->state.playbackActive == 0;
    if (frameGate) {
        // 0xA05D1 = viewDirty view-dirty byte (the old literal 658257 was a
        // decimal slip that landed on the frame-range dialog HWND's byte 1)
        s->state.viewDirty = 1;                                   // 0xA05D1
        s->state.windowLayoutReady = 0;
        PostLanguageSweep2(s);                                    // 0x40D070
        s->state.windowLayoutReady = 1;
    }
    PostViewRefresh(s);                                           // 0x40D130
}

namespace {

// True while the press the tracked left-button state describes has already been
// run through the viewport tools.  Two detectors see the same physical press:
// the main loop runs FrameDriver only while the message queue is empty
// (winmain.cpp:107-162), so PollKey's GetAsyncKeyState fallback below reports the
// button while WM_LBUTTONDOWN is still queued, and that frame path runs the tool
// press; the message then arrives and would run the same press again.  Two runs
// per click move the coordinate selector two states, and once the cycle wraps
// that is the reported "no matter how often I click, the mode does not switch".
// Cleared whenever the tracked state returns to idle, so the next press is new.
bool g_viewportPressHandled = false;

void PollKey(MMDApp* app, std::int32_t& keyState, int virtualKey) {
    const std::int32_t oldState = keyState;
    bool down = (GetKeyState(virtualKey) & 0x8000) != 0;
    if (!down) {
        // A Chinese/Japanese IME can consume the keystroke before the thread
        // key-state table records it - the same reason the letter ladder also
        // reads GetAsyncKeyState.  That global state is only meaningful while
        // this process owns the foreground window, so gate it that way.
        const HWND foreground = GetForegroundWindow();
        const HWND main = app->state.hwnd;
        if (foreground != nullptr &&
            (foreground == main || foreground == app->state.floatingWindow) &&
            (GetAsyncKeyState(virtualKey) & 0x8000) != 0)
            down = true;
    }
    std::int32_t state = 0;
    if (down)
        state = oldState == 0 ? 1 : 3;
    else if (oldState == 1 || oldState == 3)
        state = 2;
    keyState = state;
    if (state == 0 && virtualKey == VK_LBUTTON)
        g_viewportPressHandled = false;      // idle again: the next press is new
    if (state == 1 || state == 2)
        app->state.messageSeen = 1;
}

// True while the left button is physically down; uses the same two sources and
// the same foreground gate as PollKey: the thread key-state table first, then
// the async state for the IME/driver case where the table has not yet recorded the
// press.
bool LeftButtonPhysicallyDown(MMDApp* app) {
    if ((GetKeyState(VK_LBUTTON) & 0x8000) != 0)
        return true;
    const HWND foreground = GetForegroundWindow();
    const HWND main = app->state.hwnd;
    if (foreground == nullptr ||
        (foreground != main && foreground != app->state.floatingWindow))
        return false;
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

float DragScale(MMDApp* app) {
    if (app->ShiftModifierActive())
        return 0.5f;                            // flt_52960C / dbl_52B8F0
    if (app->CtrlModifierActive())
        return 0.005f;                          // flt_529604 / dbl_52E9C0
    return 0.05f;                               // flt_52E9A4 / dbl_52D738
}

void PanCameraPosition(MMDApp* app, int dx, int dy) {
    const float scale = DragScale(app);
    auto& api = d3dx::Get();
    api.Load();
    if (api.rotX != nullptr && api.rotY != nullptr &&
        api.rotZ != nullptr && api.multiply != nullptr) {
        d3dx::D3DXMATRIXF mx{}, my{}, mz{}, matrix{};
        api.rotY(&my, app->CameraYaw());
        api.rotX(&mx, app->CameraPitch());
        api.multiply(&matrix, &mx, &my);
        api.rotZ(&mz, -app->CameraRoll());
        api.multiply(&matrix, &mz, &matrix);

        const float sx = static_cast<float>(-dx) * scale;
        const float sy = static_cast<float>(dy) * scale;
        app->CameraPositionX() += sx * matrix.m[0][0] +
                                  sy * matrix.m[1][0] + matrix.m[3][0];
        app->CameraPositionY() += sx * matrix.m[0][1] +
                                  sy * matrix.m[1][1] + matrix.m[3][1];
        app->CameraPositionZ() -= sx * matrix.m[0][2] +
                                  sy * matrix.m[1][2] + matrix.m[3][2];
        return;
    }

    // The original cannot start without d3dx9_32.dll.  Keep the restored
    // build operable in that documented fallback environment.
    app->CameraPositionX() -= static_cast<float>(dx) * scale;
    app->CameraPositionY() += static_cast<float>(dy) * scale;
}

// PanSelectedCameraKey (parented-camera-key MMB pan) moved to
// pump_navigation.cpp with the rest of the x86 0x470BF5..0x47133D block.

int ViewportToolAtPoint(MMDApp* app) {
    app->ViewportToolHovered() = 0;
    app->ViewportToolOperation() = ViewportToolAction::None;
    if (app->ViewportInputActive() == 0)
        return 0;

    const int x = app->MouseX();
    const int y = app->MouseY();
    const RECT view = app->ViewportRect();
    HWND window = app->FloatingWindow();
    if (window == nullptr)
        window = app->state.hwnd;
    RECT client{};
    GetClientRect(window, &client);

    int operation = 0;
    const RECT frame = FrameOverlayRect(app);
    auto panels = FramePanelLayout(app);
    if (MMD_ORIGINAL_PROFILE) panels.overlay.right = client.right;
    if (y > panels.TopY(-25) && y < frame.top) {
        if (x > panels.TopX(60) && x < panels.TopX(36))
            operation = 1;
        else if (x > panels.TopX(30) && x < panels.TopX(6))
            operation = 2;
    }

    float scale = FrameOverlayScale(app);
    if (!(scale > 0.0f))
        scale = 1.0f;

    // Draw, hit testing and cursor restore share the same corner layout.
    const viewport_tool_layout::Anchor hud = viewport_tool_layout::Make(
        FrameOverlayRect(app), scale, viewport_corner_settings::Current());

    if (operation == 0 && app->PlaybackActive() == 0) {
        const float fx = static_cast<float>(x);
        const float fy = static_cast<float>(y);
        const float x0 = hud.x - 127.0f * scale;
        const float x1 = hud.x - 92.0f * scale;
        const float x2 = hud.x - 86.0f * scale;
        const float x3 = hud.x - 51.0f * scale;
        const float x4 = hud.x - 48.0f * scale;
        const float x5 = hud.x - 13.0f * scale;
        // Both original rows moved up one 40-pixel band to make room for the
        // scale row: rotate now sits at bottom-120..-90, move at
        // bottom-80..-50, and the scale row takes bottom-40..-10.
        const float rotateTop = hud.y - 120.0f * scale;
        const float rotateBottom = hud.y - 90.0f * scale;
        const float moveTop = hud.y - 80.0f * scale;
        const float moveBottom = hud.y - 50.0f * scale;
        const float scaleTop = hud.y - 40.0f * scale;
        const float scaleBottom = hud.y - 10.0f * scale;
        int column = -1;
        if (fx > x0 && fx < x1)
            column = 0;
        else if (fx > x2 && fx < x3)
            column = 1;
        else if (fx > x4 && fx < x5)
            column = 2;
        if (column >= 0) {
            const bool modelPanel =
                app->state.optflag[0] != 0;
            const std::uint32_t axisMode =
                app->state.v32c;
            if (fy > rotateTop && fy < rotateBottom) {
                if (modelPanel)
                    operation = 18 + column;
                else if (axisMode == 0 || axisMode == 1)
                    operation = 9 + column;
            } else if (fy > moveTop && fy < moveBottom) {
                if (modelPanel)
                    operation = 15 + column;
                else if (axisMode == 0 || axisMode == 2)
                    operation = 12 + column;
            }
        }

        // The scale row (uniform/X/Y/Z).  Input shares the drawing's one gate,
        // bone_scale::Enabled: every bone scales, so the row goes inert exactly
        // while the camera/light/accessory panel is active (optflag[0] != 0),
        // while no bone is selected or during playback - the disabled row
        // never drags.  The model panel's Light/Camera edit modes are the bone
        // move/rotate cursors and stay enabled.
        if (operation == 0 && fy > scaleTop && fy < scaleBottom &&
            bone_scale::Enabled(app)) {
            int scaleColumn = -1;
            // Uniform changes sides with the dock; retain the 3-unit inset.
            if (fx > hud.X(hud.ScaleLeftUnits(0) - 3.0f) &&
                fx < hud.X(hud.ScaleLeftUnits(0) - 37.0f))
                scaleColumn = 0;
            else if (column >= 0)
                scaleColumn = 1 + column;
            if (scaleColumn >= 0)
                operation = 22 + scaleColumn;
        }

        if (operation == 0 && app->EditMode() == ViewportEditMode::Camera) {
            const int cx = app->ViewportToolCenterX();
            const int cy = app->ViewportToolCenterY();
            const int px = x - cx;
            const int py = y - cy;
            const bool horizontal =
                ((px > 7 && px < 35) || (px > -35 && px < -7)) &&
                py > -7 && py < 7;
            const bool vertical =
                ((py > 7 && py < 35) || (py > -35 && py < -7)) &&
                px > -7 && px < 7;
            if (horizontal)
                operation = 3;
            else if (vertical)
                operation = 4;
            else {
                const int radius2 = px * px + py * py;
                if (radius2 > 1225 && radius2 < 2025)
                    operation = 5;
            }
        } else if (operation == 0 &&
                   app->EditMode() == ViewportEditMode::Light) {
            const int px = x - app->ViewportToolCenterX();
            const int py = y - app->ViewportToolCenterY();
            if (px > 41 && px < 61 && py > -8 && py < 8)
                operation = 6;
            else if (px > -13 && px < 8 && py > -61 && py < -42)
                operation = 7;
            else if (px > -13 && px < 11 && py > -16 && py < 8)
                operation = 8;
        }

        if (operation == 0) {
            // MMS addition: the scale-mode handles of the viewport gizmo
            // (bone_scale_gizmo.hpp).  The render pass published the geometry it
            // drew, so the hit bands match the painted handles exactly, and
            // bone_scale::gizmo::Active rejects the disabled states - another
            // edit mode, the camera/light/accessory panel, playback, or no
            // valid bone selection.
            switch (bone_scale::gizmo::HitTest(
                app, static_cast<float>(x), static_cast<float>(y))) {
            case bone_scale::gizmo::Handle::AxisX:
                operation = static_cast<int>(ViewportToolAction::ScaleGizmoX);
                break;
            case bone_scale::gizmo::Handle::AxisY:
                operation = static_cast<int>(ViewportToolAction::ScaleGizmoY);
                break;
            case bone_scale::gizmo::Handle::AxisZ:
                operation = static_cast<int>(ViewportToolAction::ScaleGizmoZ);
                break;
            case bone_scale::gizmo::Handle::Plane:
                operation =
                    static_cast<int>(ViewportToolAction::ScaleGizmoPlane);
                break;
            default:
                break;
            }
        }

        if (operation == 0) {
            // The local/global/view strip is a toggle, not a drag target.
            // Do not let a stale drag mode from the previous mouse-up suppress
            // the click; that was the source of intermittent failed toggles.
            //
            // The shared layout places the caption above or below the rows.
            const float coordLeft = hud.x - 130.0f * scale;
            const float coordTop = hud.CoordHitTop();
            if (fx > coordLeft && fx < hud.x - 10.0f * scale &&
                fy > coordTop && fy < hud.CoordHitBottom())
                operation = 21;
        }
    }

    if (operation != 0) {
        app->ViewportToolHovered() = 1;
        app->ViewportToolOperation() =
            static_cast<ViewportToolAction>(operation);
    }
    return operation;
}

HWND ViewportWindow(MMDApp* app) {
    HWND window = app->FloatingWindow();
    if (window == nullptr)
        window = app->state.hwnd;
    return window;
}

float ViewportScale(MMDApp* app) {
    D3DRenderer* r = app->Renderer();
    const float scale = r != nullptr ? r->viewScale : 1.0f;  // +120048
    return scale > 0.0f ? scale : 1.0f;
}

void WarpCursorRaw(MMDApp* app, POINT point) {
    HWND window = ViewportWindow(app);
    if (window == nullptr)
        return;
    ClientToScreen(window, &point);
    SetCursorPos(point.x, point.y);
}

void WarpCursor(MMDApp* app, POINT point) {
    HWND window = ViewportWindow(app);
    if (window == nullptr)
        return;
    WarpCursorRaw(app, point);
    app->state.separateWindowMouseSeen = 1;
}

void BeginCenteredDrag(MMDApp* app, int operation) {
    const RECT view = app->ViewportRect();
    POINT point{(view.left + view.right) / 2,
                (view.top + view.bottom) / 2};
    app->MouseX() = point.x;
    app->MouseY() = point.y;
    app->PreviousMouseX() = point.x;
    app->PreviousMouseY() = point.y;
    app->ViewToolDragOperation() =
        static_cast<ViewportToolAction>(operation);
    app->state.mouseJumped = 0;
    WarpCursor(app, point);
    while (ShowCursor(FALSE) >= 0) {}
}

void RestoreViewportToolCursor(MMDApp* app, int operation) {
    const RECT view = app->ViewportRect();
    const float scale = viewport_tool_layout::NormalizeScale(FrameOverlayScale(app));
    // Same docked anchor as ViewportToolAtPoint: the cursor lands on the
    // pressed button wherever the row block is docked (bottom-right keeps the
    // original numbers).  Operation 1/2 keeps the real viewport.
    const viewport_tool_layout::Anchor hud = viewport_tool_layout::Make(
        FrameOverlayRect(app), scale, viewport_corner_settings::Current());
    POINT point{};
    if (operation == 1 || operation == 2) {
        HWND window = ViewportWindow(app);
        RECT client{};
        GetClientRect(window, &client);
        auto panels = FramePanelLayout(app);
        if (MMD_ORIGINAL_PROFILE) panels.overlay.right = client.right;
        point.x = panels.TopX(operation == 1 ? 48 : 18);
        point.y = panels.TopY(-12);
    } else if (operation >= 22 && operation <= 25) {
        point.x = static_cast<int>(hud.ScaleCenterX(operation - 22));
        point.y = hud.y - static_cast<int>(25.0f * scale);
    } else {
        const int column = operation == 9 || operation == 12 ||
                           operation == 15 || operation == 18 ? 0
                         : operation == 10 || operation == 13 ||
                           operation == 16 || operation == 19 ? 1 : 2;
        const bool upper = operation == 9 || operation == 10 ||
                           operation == 11 || operation == 18 ||
                           operation == 19 || operation == 20;
        static constexpr float kCenterX[3] = {110.0f, 70.0f, 30.0f};
        point.x = hud.x - static_cast<int>(kCenterX[column] * scale);
        // Row centres follow the rows' move up 40: rotate at bottom-120..-90
        // centres on 105, the move row at bottom-80..-50 on 65.
        point.y = hud.y - static_cast<int>((upper ? 105.0f : 65.0f) * scale);
    }
    // Capture loss can be caused by a dialog or an OS resize operation. Do
    // not drag the pointer back out of that window when cancelling our tool.
    if (GetCapture() == ViewportWindow(app) &&
        GetForegroundWindow() == ViewportWindow(app))
        WarpCursor(app, point);
    while (ShowCursor(TRUE) < 0) {}
}

unsigned char* ActiveBoneModel(MMDApp* app) {
    return app->ModelSlot(app->state.slotIdx);
}

bool BoneCanBePicked(MMDApp* app, const mdl::BoneRecord& bone) {
    // A click may reach exactly the bones the overlay paints a marker for
    // (sprite_overlay.cpp): the visible flag and one of the marker bone types
    // (the marker switch covers every type up to Effector plus FixedAxis).
    if ((bone.flags & mdl::kBoneFlagVisible) == 0)
        return false;
    const mdl::BoneType type = bone.type;
    if (type > mdl::BoneType::Effector && type != mdl::BoneType::FixedAxis)
        return false;
    // Only a bone really driven by a rigid body is locked away while physics
    // owns its transform, using the same filter as the marker pass
    // (sprite_overlay.cpp `filtered`): mode 1 ("anytime") locks every
    // rigid-body bone, mode 2 ("on/off") one while physicsDisabled == 0.
    //
    // The old test keyed that lock on the bone TYPE instead of hasRigidBody
    // ("physicsMode == 2 && type != RotateMove", "physicsMode == 1 && type !=
    // RotateMove"), so once a physics menu state was active every non
    // RotateMove bone lost its click - Move/Ik/UnderIk controls and the IK
    // chain bones, which paint markers and box-select fine.  Reported as
    // "clicking a bone in the main viewport selects nothing while box
    // selection works"; covered by tests/bone_registration_tests.cpp
    // ViewportClickSelectionPhysicsLock.
    const int physicsMode = app->PlaybackPhysicsMode();
    if (bone.hasRigidBody != 0) {
        if (physicsMode == 1)
            return false;
        if (physicsMode == 2 && bone.physicsDisabled == 0)
            return false;
    }
    return true;
}

void PickBoneAtCursor(MMDApp* app, bool additive) {
    unsigned char* model = ActiveBoneModel(app);
    if (model == nullptr)
        return;
    auto* bones = mikumikustudio::mdl::Bones(model);
    auto* selected = mikumikustudio::mdl::Mdl(model)->boneSelection;
    const int count = mikumikustudio::mdl::Mdl(model)->boneCount;
    if (bones == nullptr || selected == nullptr || count <= 0)
        return;

    const int x = app->MouseX();
    const int y = app->MouseY();
    const int active = mikumikustudio::mdl::Mdl(model)->selectedBone;
    int firstHit = -1;
    int candidate = -1;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const mdl::BoneRecord& bone = bones[i];
        if (!BoneCanBePicked(app, bone))
            continue;
        const int dx = bone.selState - x;
        const int dy = bone.selState2 - y;
        if (dx * dx + dy * dy >= 64)
            continue;
        if (additive) {
            selected[i] = selected[i] == 0 ? 1 : 0;
            if (selected[i] != 0)
                mikumikustudio::mdl::Mdl(model)->selectedBone = i;
            else if (mikumikustudio::mdl::Mdl(model)->selectedBone == i)
                mikumikustudio::mdl::Mdl(model)->selectedBone = -1;
            changed = true;
        } else {
            // MMD 9.32 (0x140031C81): advance past the active bone in
            // model order, then wrap to the first hit if none follows it.
            if (firstHit < 0)
                firstHit = i;
            if (i > active) {
                candidate = i;
                break;
            }
        }
    }
    if (!additive && candidate < 0)
        candidate = firstHit;
    if (!additive && candidate >= 0) {
        std::memset(selected, 0, static_cast<std::size_t>(count));
        selected[candidate] = 1;
        mikumikustudio::mdl::Mdl(model)->selectedBone = candidate;
        changed = true;
    }
    if (changed) {
        PostLanguageSweep(app);
        PostLanguageSweep2(app);
    }
}

void UpdateBoneBoxSelection(MMDApp* app) {
    if (app->BoneBoxSelectionActive() == 0)
        return;
    unsigned char* model = ActiveBoneModel(app);
    if (model == nullptr)
        return;
    auto* bones = mikumikustudio::mdl::Bones(model);
    auto* selected = mikumikustudio::mdl::Mdl(model)->boneSelection;
    const int count = mikumikustudio::mdl::Mdl(model)->boneCount;
    if (bones == nullptr || selected == nullptr || count <= 0)
        return;

    const int left = std::min(app->BoneBoxStartX(),
                              app->MouseX());
    const int right = std::max(app->BoneBoxStartX(),
                               app->MouseX());
    const int top = std::min(app->BoneBoxStartY(),
                             app->MouseY());
    const int bottom = std::max(app->BoneBoxStartY(),
                                app->MouseY());
    const bool additive = app->ShiftModifierActive();
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const mdl::BoneRecord& bone = bones[i];
        const mdl::BoneType type = bone.type;
        const bool eligible =
            type <= mdl::BoneType::Effector ||
            type == mdl::BoneType::FixedAxis;
        const int x = bone.selState;
        const int y = bone.selState2;
        const bool inside = eligible && x > left && x < right &&
                            y > top && y < bottom;
        const std::uint8_t next = inside ? 1 : (additive ? selected[i] : 0);
        changed |= selected[i] != next;
        selected[i] = next;
    }
    if (changed)
        PostLanguageSweep(app);
}

void BeginBackgroundGesture(MMDApp* app) {
    using Target = MMDApp::BackgroundGesture::Target;
    auto& gesture = app->backgroundGesture;
    gesture = {};
    const int control = app->CtrlModifierState();
    if (control != 1 && control != 3) return;
    // The video is drawn over the picture. Hidden media must not intercept
    // the gesture; Ctrl with no visible media must not move the camera.
    gesture.target = Target::Empty;
    if (app->AviBackgroundEnabled() == 1 && app->AviFrameWidth() > 0 && app->AviFrameHeight() > 0) {
        gesture.target = Target::Video;
        gesture.x = app->AviOffsetX(); gesture.y = app->AviOffsetY();
    } else if (app->PictureBackgroundEnabled() && app->PictureWidth() > 0 && app->PictureHeight() > 0) {
        gesture.target = Target::Picture;
        gesture.x = app->PictureOffsetX(); gesture.y = app->PictureOffsetY();
    }
}

void DragBackground(MMDApp* app, bool zoom, int dx, int dy) {
    using Target = MMDApp::BackgroundGesture::Target;
    auto& gesture = app->backgroundGesture;
    if (gesture.target == Target::Empty || (dx == 0 && dy == 0)) return;
    const bool video = gesture.target == Target::Video;
    if (video ? app->AviBackgroundEnabled() != 1 : !app->PictureBackgroundEnabled()) return;
    auto& x = video ? app->AviOffsetX() : app->PictureOffsetX();
    auto& y = video ? app->AviOffsetY() : app->PictureOffsetY();
    float& scale = video ? app->AviScale() : app->PictureScale();
    if (zoom) {
        const double before = std::isfinite(scale) && scale > 0 ? scale : 1.0;
        scale = static_cast<float>(std::clamp(before * std::exp(std::clamp(-dy * .01, -20.0, 20.0)), .001, 100.0));
        // Keep the media's centre fixed while scaling.
        gesture.x += (before - scale) * (video ? app->AviFrameWidth() : app->PictureWidth()) * .5;
        gesture.y += (before - scale) * (video ? app->AviFrameHeight() : app->PictureHeight()) * .5;
    } else {
        // Invert the overlay's screen-space conversion. Accumulate fractions
        // so slow one-pixel drags work at every viewport scale.
        const double unitsPerPixel = 1.2000000476837158 / ViewportScale(app);
        gesture.x += dx * unitsPerPixel;
        gesture.y += dy * unitsPerPixel;
    }
    const auto bounded = [](double value) {
        return std::clamp(value, double(std::numeric_limits<std::int32_t>::min()),
                          double(std::numeric_limits<std::int32_t>::max()));
    };
    gesture.x = bounded(gesture.x); gesture.y = bounded(gesture.y);
    x = static_cast<std::int32_t>(std::llround(gesture.x));
    y = static_cast<std::int32_t>(std::llround(gesture.y));
    if (video) AviBgPlacementRefresh(app);
    else PicBgOverlayRefresh(app);
    app->state.messageSeen = 1;
}

void BeginOrEndViewportToolDrag(MMDApp* app, int operation) {
    const int leftState = app->LeftMouseButtonState();
    if (leftState == 1) {
        // This press is now owned by the tool: whichever detector reports it first
        // (the frame driver's poll or the window message) runs it, and the other
        // must not run it again. See g_viewportPressHandled.
        g_viewportPressHandled = true;
        if (operation == 1 || operation == 2) {
            // Zoom and pan use the same unlimited relative gesture as the HUD.
            BeginBackgroundGesture(app);
            BeginCenteredDrag(app, operation);
            return;
        }
        static constexpr std::uint8_t kDragMode[30] = {
            0, 0, 0, 1, 2, 3, 7, 8, 9, 4, 5, 6,
            10, 11, 12, 13, 14, 15, 16, 17, 18, 0,
            19, 20, 21, 22,
            // MMS addition: the viewport scale handles (operations 26..29).
            23, 24, 25, 26};
        if ((operation >= 3 && operation <= 20) ||
            (operation >= 22 && operation <= 29)) {
            const int mode = kDragMode[operation];
            if (operation >= 22) {
                // The scale row captures the bone-edit undo before the centred
                // grab starts and flags the same bone-edit refresh byte as the
                // rotate/move rows.  PushBoneEditUndo skips when boneSelection
                // holds no selected entry, so sync the selected bone into the
                // array first (scale branch only): a valid selectedBone with a
                // stale selection array would otherwise lose its undo record.
                // The viewport scale handles (26..29) take the same path, so a
                // handle drag is one undo record like a HUD row drag.
                unsigned char* model = ActiveBoneModel(app);
                if (model != nullptr) {
                    auto* record = mdl::Mdl(model);
                    const int bone = record->selectedBone;
                    const int count = static_cast<int>(record->boneCount);
                    if (record->boneSelection != nullptr && bone >= 0 &&
                        bone < count)
                        record->boneSelection[bone] = 1;
                }
                PushBoneEditUndo(app);
                app->state.a06B5 = 1;
            }
            if (operation >= 9 && operation <= 25) {
                // The HUD row buttons are unlimited vertical gestures and warp
                // the pointer into the viewport.  The scale handles (26..29) are
                // dragged in place, so they are deliberately left out.
                BeginCenteredDrag(app, operation);
            }
            app->InteractionDragMode() = static_cast<ViewportDragMode>(mode);
            if (operation == 5) {
                app->ViewportToolDragOriginX() = app->ViewportToolCenterX();
                app->ViewportToolDragOriginY() = app->ViewportToolCenterY();
            }
            if (operation <= 8) {
                app->EditMode() = ViewportEditMode::ToolDrag;
                PushBoneEditUndo(app);
            } else if (operation <= 14) {
                PushBoneEditUndo(app);
                app->state.a06B5 = 1;
            } else if (operation <= 20) {
                // The camera/accessory drag widgets target the accessory only
                // while that state exists, so a stale byte must not refresh
                // an accessory that no longer exists.
                const int target = EffectiveCoordinateState(app);
                RefreshRequest(target == 2
                    ? app->state.selectedObjectSlot : -1);
                app->state.a06B5 = 1;
            }
        } else if (operation == 21) {
            int& target = app->state.coordinateSystem;
            const int limit = CoordinateStateLimit(app);
            const int before = target;
            if (++target >= limit)
                target = 0;
            runtime_log::Trace("strip advance %d->%d limit=%d", before,
                               static_cast<int>(target), limit);
            PostLanguageSweep(app);
        } else if (operation == 0 &&
                   app->PlaybackActive() == 0) {
            const RECT view = app->ViewportRect();
            const int x = app->MouseX();
            const int y = app->MouseY();
            if (x > view.left && x < view.right &&
                y > view.top && y < view.bottom) {
                const ViewportEditMode panelMode = app->EditMode();
                // MMS: Shift + drag in select mode draws the selection box
                // (additive, see UpdateBoneBoxSelection); a box-select mode
                // loaded from a project still starts it without Shift.
                if (panelMode == ViewportEditMode::BoneBox ||
                    (panelMode == ViewportEditMode::Bone &&
                     app->ShiftModifierActive())) {
                    app->BoneBoxStartX() = x;
                    app->BoneBoxStartY() = y;
                    app->BoneBoxSelectionActive() = 1;
                }
            }
        }
    } else if (leftState == 2) {
        app->PendingTimelineSelectionRow() = TimelineSelectionRow::None;;
        const int viewOperation =
            static_cast<int>(app->ViewToolDragOperation());
        const int mode = static_cast<int>(app->InteractionDragMode());
        if (viewOperation == 1 || viewOperation == 2) {
            RestoreViewportToolCursor(app, viewOperation);
            app->ViewToolDragOperation() = ViewportToolAction::None;
            app->backgroundGesture = {};
        }
        if (mode != 0) {
            if (mode >= 1 && mode <= 3)
                app->EditMode() = ViewportEditMode::Camera;
            else if (mode >= 7 && mode <= 9)
                app->EditMode() = ViewportEditMode::Light;
            if ((viewOperation >= 9 && viewOperation <= 20) ||
                (viewOperation >= 22 && viewOperation <= 25)) {
                RestoreViewportToolCursor(app, viewOperation);
                app->ViewToolDragOperation() = ViewportToolAction::None;
            } else if (viewOperation >= 26 && viewOperation <= 29) {
                // The scale handles are dragged where they are: the release
                // clears the tracked operation but has no button to restore the
                // pointer onto.
                app->ViewToolDragOperation() = ViewportToolAction::None;
            }
            app->InteractionDragMode() = ViewportDragMode::None;
            app->state.a06B5 = 0;
        }
        app->BoneBoxSelectionActive() = 0;
    }
}

}  // namespace

bool OwnsCenteredViewportDrag(MMDApp* app) {
    const int operation = static_cast<int>(app->ViewToolDragOperation());
    const bool centered = operation == 1 || operation == 2 ||
        (operation >= static_cast<int>(ViewportToolAction::TransformAxisX) &&
         operation <= static_cast<int>(ViewportToolAction::ScaleZ) &&
         operation != static_cast<int>(ViewportToolAction::CycleCoordinateSystem));
    return app->LeftMouseButtonHeld() && centered;
}

void ApplyCenteredDragCursorSample(MMDApp* app, int x, int y) {
    if (!OwnsCenteredViewportDrag(app)) return;
    app->MouseX() = x;
    app->MouseY() = y;
    app->state.separateWindowMouseSeen = 0;
    app->state.mouseJumped = 0;
}

void RecentreCenteredDragCursor(MMDApp* app) {
    const HWND owner = ViewportWindow(app);
    if (owner && (GetCapture() != owner || GetForegroundWindow() != owner)) return;
    // While one of the six operation buttons owns the drag, a pointer that
    // leaves the viewport is warped back to the middle of it, so the gesture
    // has no vertical (or horizontal) limit - that is what lets the original
    // "keep dragging up or down" forever.  The original runs this once per
    // frame inside each drag body, right after the frame's delta has been
    // applied (x64 0x323DA, 0x326B4, 0x32761 for the translate/rotate bodies
    // and 0x36BB6 for the mode group it gates as "mode 4..6 or mode >= 10",
    // which is exactly the port's operations 9..20).  Each site re-centres one
    // axis to the middle of that axis' viewport bounds (0xA1DEC/0xA1DF4 for X,
    // 0xA1DF0/0xA1DF8 for Y, both written by the viewport layout) and stores
    // the result as *both* the current and the previous position, so the warp
    // itself contributes no delta.
    //
    // mouseJumped gates it, exactly as in the original: a stored position that
    // follows a cursor warp is the warp's own value rather than the user's, and
    // re-centring on it would fight that warp.
    if (app->state.mouseJumped != 0)
        return;
    if (!OwnsCenteredViewportDrag(app)) return;
    const RECT view = app->ViewportRect();
    POINT point{app->MouseX(), app->MouseY()};
    bool recentred = false;
    if (point.x <= view.left || point.x >= view.right - 1) {
        app->MouseX() = (view.left + view.right) / 2;
        app->PreviousMouseX() = app->MouseX();
        point.x = app->MouseX();
        recentred = true;
    }
    if (point.y <= view.top || point.y >= view.bottom - 1) {
        app->MouseY() = (view.top + view.bottom) / 2;
        app->PreviousMouseY() = app->MouseY();
        point.y = app->MouseY();
        recentred = true;
    }
    // The recentring stores both positions itself, so it must not mark a warp
    // for the mouse-move handler: that marker makes the next stored move sync
    // the previous position without applying its delta, which would eat the
    // first pixels of the movement that follows each recentring.  The original
    // marks 0xA16C0/0xA16C2 here, not the 0xA0088 byte its move handler reads.
    if (recentred)
        WarpCursorRaw(app, point);
}

namespace {

// Coordinate strip operation id from ViewportToolAtPoint. It is a
// toggle: every physical click must move it one state.
constexpr int kCoordinateStripOperation = 21;

bool PressViewportTool(MMDApp* app, int requiredOperation) {
    const auto active = app->ViewportInputActive();
    app->ViewportInputActive() = 1;
    const int operation = ViewportToolAtPoint(app);
    app->ViewportInputActive() = active;
    if (operation == 0) return false;
    if (requiredOperation != 0 && operation != requiredOperation) return false;
    // The frame driver's poll runs between messages and already reports a press
    // it detects via GetAsyncKeyState while this message is still queued
    // (the main loop calls FrameDriver only when the queue is empty, see
    // winmain.cpp). That path already ran the tool for this press, so this message
    // must not run it again - two runs per click advance the coordinate selector
    // by two states, and once the cycle wraps, that is the reported "no matter how
    // often I click, the mode does not switch". An idle tracked state means this is
    // a new press, so drop any stale marker and continue.
    if (app->LeftMouseButtonState() == 0)
        g_viewportPressHandled = false;
    else if (g_viewportPressHandled)
        return true;
    app->LeftMouseButtonState() = 1;
    BeginOrEndViewportToolDrag(app, operation);
    // Consume this edge immediately: the click can complete before the next rendered frame.
    app->LeftMouseButtonState() = 3;
    return true;
}

}  // namespace

int ViewportToolAtPointForTest(MMDApp* app) {
    const auto active = app->ViewportInputActive();
    app->ViewportInputActive() = 1;
    const int result = ViewportToolAtPoint(app);
    app->ViewportInputActive() = active;
    return result;
}

bool HandleViewportToolPress(MMDApp* app) {
    return PressViewportTool(app, 0);
}

bool HandleCoordinateStripPress(MMDApp* app) {
    // The window class sets CS_DBLCLKS, so the second click of a double click is
    // sent as WM_LBUTTONDBLCLK and never as WM_LBUTTONDOWN; that message
    // reached the bone pick and the viewport double-click handler but not
    // HandleViewportToolPress, so two quick clicks on the strip moved the
    // selector once - the reported "clicking is not responsive". The floating
    // window's own proc already treats the two messages alike.
    return PressViewportTool(app, kCoordinateStripOperation);
}

void ReleaseLeftButtonState(MMDApp* app, bool physicallyHeld) {
    // The gesture has ended: the next press, regardless of the tracked state, is a
    // new gesture and may run the tools again.
    g_viewportPressHandled = false;
    // 2 = the released edge that the drag logic keys off, then the tracked state
    // for everything else.
    app->LeftMouseButtonState() = 2;
    BeginOrEndViewportToolDrag(app, 0);
    // WM_CAPTURECHANGED and WM_CANCELMODE end a gesture through the same
    // mouse-up path even while the button may still be held. Writing 0 there
    // made the frame driver's PollKey read the same physical press as a *new*
    // press on the next frame (its edge test is "idle -> down"), so the strip
    // click ran a second time - the coordinate selector then advanced two states
    // per click, which is the reported "the caption flashes another state and
    // the mode comes back unchanged". While the button is genuinely held down the
    // state stays "held", so the poll cannot fabricate that edge; the real
    // mouse-up clears it.
    app->LeftMouseButtonState() = physicallyHeld ? 3 : 0;
}

void HandleViewportToolRelease(MMDApp* app) {
    // Include the release position even when the last move was coalesced.
    UpdateBoneBoxSelection(app);
    ReleaseLeftButtonState(app, LeftButtonPhysicallyDown(app));
}

void HandleViewportRightDoubleClick(MMDApp* app) {
    const RECT view = app->ViewportRect();
    if (app->PlaybackActive() || app->MouseX() <= view.left ||
        app->MouseX() >= view.right || app->MouseY() <= view.top ||
        app->MouseY() >= view.bottom) return;
    const HWND window = app->MainWindow();
    if (app->CameraMode() != 0) {
        const HWND combo = GetDlgItem(window, panel::kMainComboModel);
        const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
        if (count <= 1) return;
        auto& selection = app->state.mainModelComboSelection;
        if (selection == 0 ||
            selection >= static_cast<std::uint32_t>(count))
            selection = static_cast<std::uint32_t>(count - 1);
        SendMessageW(window, WM_COMMAND, panel::kModelEditToggle, 0);
    } else {
        // MMS: control 490 is the merged select / box-select cycle button, so
        // the double click toggles the select modes directly instead of poking
        // the button's check state and re-dispatching WM_COMMAND.  Either
        // select state turns off; every other mode turns plain select on.
        const ViewportEditMode mode = app->EditMode();
        app->EditMode() = (mode == ViewportEditMode::Bone ||
                           mode == ViewportEditMode::BoneBox)
                              ? ViewportEditMode::None
                              : ViewportEditMode::Bone;
        RefreshBoneModePanel(app);
    }
}

void HandleViewportBonePress(MMDApp* app, bool additive) {
    // Picking is a press event. Polling only once per rendered frame loses
    // clicks whose down/up messages arrive together, and can act on a later
    // mouse position (e.g. over Register) instead of the clicked bone.
    if (app->PlaybackActive() != 0 || app->EditMode() != ViewportEditMode::Bone)
        return;
    const RECT view = app->ViewportRect();
    if (app->MouseX() <= view.left || app->MouseX() >= view.right ||
        app->MouseY() <= view.top || app->MouseY() >= view.bottom)
        return;
    const auto previousActive = app->ViewportInputActive();
    app->ViewportInputActive() = 1;
    const int operation = ViewportToolAtPoint(app);
    app->ViewportInputActive() = previousActive;
    if (operation == 0) {
        PickBoneAtCursor(app, additive);
        if (additive && app->BoneBoxSelectionActive() == 0) {
            // Start from the message's press position rather than waiting for
            // a rendered frame, which may arrive after the pointer has moved.
            app->ShiftModifierState() = 3;
            app->LeftMouseButtonState() = 1;
            BeginOrEndViewportToolDrag(app, 0);
            app->LeftMouseButtonState() = 3;
        }
    }
}

// ---- test seam (hidden-window viewport box-select input) --------------------
// The frame driver's poll starts, updates and ends the viewport bone box from
// the thread key state and the foreground window.  A hidden test window can
// control neither deterministically: faking them (SetKeyboardState, a foreign
// foreground HWND) touches the user's desktop input and lets the poll pick up
// the user's real Shift/Ctrl state, which flips the box to additive and hides
// the "shrinking the box clears the bones outside it" behaviour.  This entry
// drives the very same state chain the poll drives - BeginOrEndViewportToolDrag
// with operation 0 on the press and release edges (the empty-viewport point of
// the poll's ViewportToolAtPoint) and UpdateBoneBoxSelection while held - with
// the input supplied by the caller.  It writes only the interaction state the
// poll writes and is called from tests alone; the production poll is unchanged.
//   step 0: press edge   (left state 1 -> 3, box starts at the mouse point)
//   step 1: held move    (mouse point updates, box selection recomputed)
//   step 2: release edge (the same release path a mouse-up takes)
void DriveViewportBoxSelect(MMDApp* app, int step, int x, int y) {
    app->MouseX() = app->PreviousMouseX() = x;
    app->MouseY() = app->PreviousMouseY() = y;
    if (step == 0) {
        app->LeftMouseButtonState() = 1;
        BeginOrEndViewportToolDrag(app, 0);
        // Consume the edge immediately, as PressViewportTool does: the click can
        // complete before the next rendered frame.
        app->LeftMouseButtonState() = 3;
    } else if (step == 1) {
        if (app->LeftMouseButtonHeld())
            UpdateBoneBoxSelection(app);
    } else {
        ReleaseLeftButtonState(app, false);
    }
}

void MouseInteractionBegin(MMDApp* app) {
    // 0x46FF02 -> 0x42D3A0 -> 0x40E3D0.  State values are exactly
    // 0 idle, 1 pressed, 2 released, 3 held.
    struct KeySlot { int key; std::int32_t MMDAppState::* state; };
    static constexpr KeySlot kKeySlots[] = {
        {VK_UP, &MMDAppState::upKeyState},
        {VK_DOWN, &MMDAppState::downKeyState},
        {VK_LEFT, &MMDAppState::leftKeyState},
        {VK_RIGHT, &MMDAppState::rightKeyState},
        {VK_SPACE, &MMDAppState::spaceKeyState},
        {VK_DELETE, &MMDAppState::deleteKeyState},
        {VK_ESCAPE, &MMDAppState::escKeyState},
        {VK_TAB, &MMDAppState::tabKeyState},
        {VK_RETURN, &MMDAppState::enterKeyState},
        {VK_MENU, &MMDAppState::menuKeyState},
        {221, &MMDAppState::keyState221},
        {226, &MMDAppState::keyState226},
    };
    for (const KeySlot& slot : kKeySlots)
        PollKey(app, app->state.*slot.state, slot.key);

    PollLetterHotkeys(app);

    for (int i = 0; i < 10; ++i)
        PollKey(app, app->state.numpadKeyState[i], VK_NUMPAD0 + i);

    PollKey(app, app->ShiftModifierState(), VK_SHIFT);
    PollKey(app, app->CtrlModifierState(), VK_CONTROL);
    PollKey(app, app->LeftMouseButtonState(), VK_LBUTTON);
    PollKey(app, app->RightMouseButtonState(), VK_RBUTTON);
    PollKey(app, app->MiddleMouseButtonState(), VK_MBUTTON);

    // 0x46FF07..0x46FF32 immediately follows the 0x42D3A0 call: input is
    // active when the foreground HWND is A0D38, or the main HWND when A0D38
    // is null.
    HWND active = app->FloatingWindow();
    if (active == nullptr)
        active = app->state.hwnd;
    app->ViewportInputActive() = static_cast<std::uint8_t>(
        GetForegroundWindow() == active && IsWindowEnabled(active) &&
        (!GetCapture() || GetCapture() == active));
    const int operation = ViewportToolAtPoint(app);
    // Only a client-area mouse-down gives the viewport capture. A global
    // button press on the title bar, window border or child must not start a
    // tool at the last client coordinates and warp the cursor to its centre.
    if (app->LeftMouseButtonState() != 1 || GetCapture() == active)
        BeginOrEndViewportToolDrag(app, operation);
    if (app->ViewportInputActive() == 0)
        return;

    if (app->LeftMouseButtonHeld())
        UpdateBoneBoxSelection(app);

    // SetCursorPos can leave old move messages in the queue, including moves
    // from before a boundary recenter. Consume live client coordinates only
    // here, after the press/release transition and before applying any delta.
    if (OwnsCenteredViewportDrag(app)) {
        HWND window = ViewportWindow(app);
        POINT point{};
        if (window && GetCursorPos(&point) && ScreenToClient(window, &point))
            ApplyCenteredDragCursorSample(app, point.x, point.y);
    }

    const int dx = app->MouseX() - app->PreviousMouseX();
    const int dy = app->MouseY() - app->PreviousMouseY();

    if (app->LeftMouseButtonHeld()) {
        const ViewportToolAction viewMode = app->ViewToolDragOperation();
        const bool header = viewMode == ViewportToolAction::CameraOrbit || viewMode == ViewportToolAction::CameraPan;
        if (header && app->backgroundGesture.target != MMDApp::BackgroundGesture::Target::Camera) {
            DragBackground(app, viewMode == ViewportToolAction::CameraOrbit, dx, dy);
        } else if (viewMode == ViewportToolAction::CameraOrbit) {
            float& distance = app->CameraMode() != 0 && app->CameraParentModel() >= 0
                ? app->CameraPositionZ() : app->CameraDistance();
            distance -= static_cast<float>(dy) * 0.1f;
            ViewRefreshGate(app);
        } else if (viewMode == ViewportToolAction::CameraPan) {
            if (app->state.optflag[0] != 0)
                PanCameraPosition(app, dx, dy);
            else {
                app->ViewOffsetX() -= static_cast<float>(dx) * DragScale(app);
                app->ViewOffsetY() += static_cast<float>(dy) * DragScale(app);
            }
            ViewRefreshGate(app);
        }
        if (viewMode == ViewportToolAction::CameraOrbit || viewMode == ViewportToolAction::CameraPan)
            RecentreCenteredDragCursor(app);
    }

    // Right/middle-button camera drags (x86 0x470BF5..0x47133D) live in
    // pump_navigation.cpp ConsumeRightButtonDrag / ConsumeMiddleButtonPan,
    // wired from FrameDriver before the letter ladder; the approximation
    // that used to live here was removed with their arrival.
}

void MouseInteractionEnd(MMDApp* app) {
    app->PreviousMouseX() = app->MouseX();
    app->PreviousMouseY() = app->MouseY();
}

// runtime-initialized in the original (.rdata image differs); the
// registry-echo conversion pair 0x52B760/0x52B768 turns the stored radian
// value into the "%3.4f" display figure (best evidence: PI/180 divisor)
double g_Scale52E9F0 = 0.20000000298023224; // VA 0x0052E9F0
double g_Scale52E8C8 = 0.019999999552965164;// VA 0x0052E8C8
double g_ConvA52B760 = 180.0;               // VA 0x0052B760
double g_ConvB52B768 = 3.141592025756836;   // VA 0x0052B768

// ---- light/camera/registry chains (raw listing 10776..11260) --------------
// Common shape per axis mode (selector A=this+0x24==3, B=this+0xC0==3):
//   direct  (this+0x9ED9C == 1): app[axis] += dy * scale
//   registry(this+0x9ED9C == 2 && slot): slot[axis'] -=/+ dy * scale,
//     echo "%3.4f" into the axis edit box via SetWindowTextA.
// dy = (this+8) - (this+0x10);  slots: this+0x9DD70[this+0x9E170].
namespace {

int DyOf(MMDApp* s) {
    return s->MouseY() - s->PreviousMouseY();
}

bool SelA3(MMDApp* s) { return s->ShiftModifierActive(); }
bool SelB3(MMDApp* s) { return s->CtrlModifierActive(); }

mdl::AccessoryRecord* RegSlotOf(MMDApp* s) {
    const unsigned idx = s->state.selectedObjectSlot;
    return s->AccessorySlot(idx);
}

void EchoEdit(MMDApp* s, int dlgItem, const char* text) {
    HWND window = static_cast<HWND>(s->Hwnd());
    if (window == nullptr)
        window = static_cast<HWND>(s->Hwnd());
    SetWindowTextA(GetDlgItem(window, dlgItem), text);
}

}  // namespace

int CoordinateStateLimit(MMDApp* app) {
    // The model/bone panel's third state is the screen-axis ("View") drag, which
    // only needs the viewport and is therefore always offered.
    if (app->state.optflag[0] == 0)
        return 3;
    // The camera/light/accessory panel's third state edits the selected
    // accessory, so it is only offered while the scene carries one at all.
    // Scanning the whole object list instead of just the selected slot matters:
    // the loaded accessories live in that list, and the shared selection byte
    // points at an empty slot until the user picks a row, so testing only the
    // selected slot hid the state in a panel that was full of accessories
    // (reported as "no matter how often I click, the mode does not switch").
    for (int slot = 0; slot < kModelSlotCount; ++slot)
        if (app->AccessorySlot(slot) != nullptr)
            return 3;
    return 2;
}

int EffectiveCoordinateState(MMDApp* app) {
    const int limit = CoordinateStateLimit(app);
    const int target = app->state.coordinateSystem;
    return (target >= 0 && target < limit) ? target : 0;
}

void NormalizeCoordinateState(MMDApp* app) {
    // The third state is accessory editing on the camera/light/accessory panel,
    // so the stored byte can outlive its target: deleting the last accessory or
    // selecting a slot that holds none leaves a 2 behind. Cycling can no longer
    // reach that state, but the consumers read the byte directly: the accessory
    // drag handlers, the operation-axis gizmo, the mouse-wheel refresh gate, and
    // the caption. A stale 2 kept steering them into an accessory that no
    // longer exists ("the accessory coordinate system is not completely disabled").
    // Clamp it once per frame; both writers (the strip click and the 'L' key)
    // already stay inside the limit.
    app->state.coordinateSystem = EffectiveCoordinateState(app);
}

void ModeCameraAdjust(MMDApp* app, int axis) {
    // 0x4786FB..0x4790DF, modes 13..15.  The same operation widgets edit
    // either the camera target or the selected accessory position.
    static const int kEdit[3] = {0x1DE, 0x1DF, 0x1E0};
    if (axis < 0 || axis > 2) { ViewRefreshGate(app); return; }
    const int target = EffectiveCoordinateState(app);
    const int dy = DyOf(app);
    double scale;
    if (SelA3(app)) scale = g_MouseScaleA;            // 0x52B8F0
    else if (SelB3(app)) scale = g_MouseScaleB;       // 0x52E9C0
    else scale = g_MouseScaleC;                       // 0x52D738
    if (target == 2 &&
        app->state.optflag[0] != 0) {
        auto* slot = RegSlotOf(app);
        if (slot != nullptr) {
            float& value = slot->position[axis];
            value = static_cast<float>(static_cast<double>(value) -
                                       static_cast<double>(dy) * scale);
            char buf[0x100];
            sprintf_s(buf, 0x100, "%3.4f",
                      slot->position[axis]);
            EchoEdit(app, kEdit[axis], buf);
        }
    } else if (app->state.cameraParentModel >= 0 || target == 1) {
        app->CameraPositionAxis(axis) = static_cast<float>(
            static_cast<double>(app->CameraPositionAxis(axis)) +
            static_cast<double>(dy) * scale);
    } else {
        auto& api = d3dx::Get();
        api.Load();
        if (api.rotX && api.rotY && api.rotZ && api.multiply &&
            api.vec3Transform) {
            d3dx::D3DXMATRIXF rx{}, ry{}, rz{}, rotation{};
            api.rotZ(&rz, -app->CameraRoll());
            api.rotX(&rx, -app->CameraPitch());
            api.multiply(&rotation, &rz, &rx);
            api.rotY(&ry, -app->CameraYaw());
            api.multiply(&rotation, &rotation, &ry);
            float local[3]{};
            local[axis] = static_cast<float>(static_cast<double>(dy) * scale);
            float world[4]{};
            api.vec3Transform(world, local, &rotation);
            app->CameraPositionX() += world[0];
            app->CameraPositionY() += world[1];
            app->CameraPositionZ() += world[2];
        } else {
            app->CameraPositionAxis(axis) = static_cast<float>(
                static_cast<double>(app->CameraPositionAxis(axis)) +
                static_cast<double>(dy) * scale);
        }
    }
    ViewRefreshGate(app);
}

void ModeAngleAdjust(MMDApp* app, int axis) {
    // 0x4790E0..0x4798xx, modes 16..18.  Stored angles are radians;
    // accessory edit controls display degrees.
    static const int kEdit[3] = {0x1E1, 0x1E2, 0x1E3};
    if (axis < 0 || axis > 2) { ViewRefreshGate(app); return; }
    const int target = EffectiveCoordinateState(app);
    const int dy = DyOf(app);
    double scale;
    if (SelA3(app)) scale = g_Scale52E9F0;            // 0.2
    else if (SelB3(app)) scale = 0.0020000000949949026;
    else scale = g_Scale52E8C8;                       // 0x52E8C8
    if (target == 2 &&
        app->state.optflag[0] != 0) {
        auto* slot = RegSlotOf(app);
        if (slot != nullptr) {
            float& value = slot->rotation[axis];
            value = static_cast<float>(static_cast<double>(value) +
                                       static_cast<double>(dy) * scale);
            char buf[0x100];
            sprintf_s(buf, 0x100, "%3.4f",
                      slot->rotation[axis] /
                          g_ConvB52B768 * g_ConvA52B760);
            EchoEdit(app, kEdit[axis], buf);
        }
    } else {
        app->CameraRotation()[axis] = static_cast<float>(
            static_cast<double>(app->CameraRotation()[axis]) +
            static_cast<double>(dy) * scale);
    }
    ViewRefreshGate(app);
}

void ModeRotate(MMDApp* app, int /*axis*/) {
    // app+0x34C modes 1..3: view-axis bone rotation.
    BoneEditModes(app);
    ViewRefreshGate(app);
}

void ModeTranslate(MMDApp* app, int /*axis*/) {
    // app+0x34C modes 4..6: selected-bone local-axis rotation.
    BoneEditModes(app);
    ViewRefreshGate(app);
}

void ModeScale(MMDApp* app) {
    // app+0x34C mode 7 is horizontal selected-root translation, not scaling.
    BoneEditModes(app);
    ViewRefreshGate(app);
}

void ModeBoneRotate(MMDApp* app, int) {
    // app+0x34C modes 8/9 add vertical or combined screen-plane movement.
    BoneEditModes(app);
    ViewRefreshGate(app);
}

void ModePhysicsBody(MMDApp* app, int) {
    // app+0x34C modes 10..12: local-axis translation or active record edits.
    BoneEditModes(app);
    ViewRefreshGate(app);
}

// ---------------------------------------------------------------------------
// VA 0x0041ACD0 - ApplyCameraReferenceModeChange(app, oldMode) (was
// Sub41ACD0): camera-reference switch re-anchor
// (command dispatch 0x47FA60/0x47FA88, control 0x213 family).  When the mode
// byte at +0x340 (kByte340) changes, the accessory ground position stored at
// app+0x308/+0x30C/+0xA08DC is re-anchored: both the OLD mode (the `mode`
// argument) and the NEW mode (current byte +0x340) resolve to a bone-record
// origin of the selected model, each origin matrix is rotated through the
// accessory rotation R = RotY(+0x314) * RotX(+0x310) * RotZ(+0x318) (and the
// parent transform at +0xA0438 when the parent index +0xA0430 >= 0), and the
// difference of the rotated translations is subtracted from the position.
//
// ResolveFrameFocus in src/render/debug_geometry.cpp selects the rendered
// pivot from camera/tracking state. The point is the selected model root or
// selected bone transformed to world space; ResolveFrameFocus pivots the
// untracked model view on it.
// ---------------------------------------------------------------------------
void ModelViewReferencePoint(MMDApp* app, int mode, float point[3]) {
    point[0] = point[1] = point[2] = 0.0f;
    const auto* model = mdl::Mdl(app->SelectedModel());
    if (!model || !model->boneTable || model->boneCount == 0 || mode < 1 || mode > 2)
        return;
    const int index = mode == 1 ? 0 : model->selectedBone;
    if (index < 0 || index >= static_cast<int>(model->boneCount)) return;
    const auto& bone = model->boneTable[index];
    for (int axis = 0; axis < 3; ++axis)
        point[axis] = bone.matInit[axis] * bone.position[0] +
                      bone.matInit[4 + axis] * bone.position[1] +
                      bone.matInit[8 + axis] * bone.position[2] + bone.matInit[12 + axis];
}

// Moves the eye so the rendered image stays in place when the orbit pivot changes.  Shared by
// the reference-mode switch and the panel switch: both change the point the frame rotates
// the world about, and both must leave the image where it was.
void CarryEyeForFocusChange(MMDApp* app, const float focusOld[3],
                            const float focusNew[3]) {
    auto& api = d3dx::Get();
    if (!(api.Load() && api.rotX && api.rotY && api.rotZ && api.multiply &&
          api.translation))
        return;  // original hard-links d3dx9_32.dll; unreachable there
    // R = RotY * RotX * RotZ  (0x41ACEB..0x41AD5D)
    d3dx::D3DXMATRIXF ry{}, rot{}, rz{};
    api.rotY(&ry, app->CameraYaw());
    api.rotX(&rot, app->CameraPitch());
    api.multiply(&rot, &ry, &rot);
    api.rotZ(&rz, app->CameraRoll());
    api.multiply(&rot, &rot, &rz);
    if (app->state.followCameraEnabled != 0 && app->CameraParentModel() >= 0)
        api.multiply(&rot,
                     reinterpret_cast<d3dx::D3DXMATRIXF*>(
                         &app->CameraAttachmentBasis()),
                     &rot);

    // Shift the view offsets so the rendered view stays fixed while the orbit
    // pivot moves.  The frame composes the pivot translation with R and then applies
    // the fixed +Z look-at view, so the rendered translation is -R(pivot) - eye:
    // keeping the image fixed means R(pivot) + eye stays constant, so the eye
    // must move by R(focus_old - focus_new).  tOld/tNew hold -R(pivot) for
    // the old and the new pivot, so subtracting their difference from the offsets
    // gives that correction, which is what the original does (0x41B005/0x41B01A
    // build the two values, and the header comment records the subtraction).
    d3dx::D3DXMATRIXF tOld{}, tNew{};
    api.translation(&tOld, -focusOld[0], -focusOld[1], -focusOld[2]);
    api.translation(&tNew, -focusNew[0], -focusNew[1], -focusNew[2]);
    api.multiply(&tOld, &tOld, &rot);                 // 0x41B005
    api.multiply(&tNew, &tNew, &rot);                 // 0x41B01A
    app->ViewOffsetX() -= tOld.m[3][0] - tNew.m[3][0];
    app->ViewOffsetY() -= tOld.m[3][1] - tNew.m[3][1];
    app->CameraDistance() -= tOld.m[3][2] - tNew.m[3][2];
}

void ApplyCameraReferenceModeChange(MMDApp* app, int oldMode) {
    // The untracked model view pivots on the model / bone reference, so a
    // switch moves the pivot; tracking overrides the reference and keeps its
    // camera target. Resolve both sides through the render path and carry
    // the eye so the image does not jump.
    float focusOld[3] = {0.0f, 0.0f, 0.0f};
    float focusNew[3] = {0.0f, 0.0f, 0.0f};
    ResolveFrameFocus(app, oldMode, focusOld);
    ResolveFrameFocus(app, static_cast<int>(app->CameraReferenceMode()), focusNew);
    CarryEyeForFocusChange(app, focusOld, focusNew);
}

}  // namespace mikumikustudio
