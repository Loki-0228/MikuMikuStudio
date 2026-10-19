// MAINTENANCE: regression-keep — native bone scale viewport handles.
// Hit testing, the merged mode button's cycle and captions, the panel grid and
// the drag paths the two handle kinds drive.  Everything here is CPU-only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_language.hpp"

using namespace mikumikustudio;
namespace bs = bone_scale;
namespace gizmo = bone_scale::gizmo;

// Declared in localize_ui.cpp (the panel's caption table and mode cycle) and
// used by the command handlers.  Not exported through a header because only the
// UI pass, the command handlers and this test need them.
namespace mikumikustudio {
std::wstring UiControlCaption(int id, unsigned char language);
std::wstring BoneModePanelCaption(ViewportEditMode mode, unsigned char language);
ViewportEditMode NextBoneSelectMode(ViewportEditMode current);
}

namespace {

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void Near(float a, float b, const char* message) {
    Check(std::abs(a - b) < 0.001f, message);
}

void Identity(float* m) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Selected bone plus the two per-bone arrays bone_scale::Enabled and Drag read.
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> record =
        std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;
    std::vector<unsigned char> selected;
    std::vector<unsigned char> dirty;
    explicit Fixture(int count = 3) : bones(count), selected(count), dirty(count) {
        record->boneCount = count;
        record->boneTable = bones.data();
        record->boneSelection = selected.data();
        record->bonePhysicsState = dirty.data();
        record->selectedBone = 0;
        for (int i = 0; i < count; ++i) {
            bones[i].parent = -1;
            bones[i].slotIndex = -1;
            Identity(bones[i].matInit);
            Identity(bones[i].matWorld);
        }
    }
    unsigned char* data() { return reinterpret_cast<unsigned char*>(record.get()); }
    ~Fixture() { bs::Forget(data()); }
};

MMDApp* SelectModel(MMDApp* app, Fixture& fixture) {
    app->state.modelSlots[0] = fixture.data();
    app->state.slotIdx = 0;
    app->PlaybackActive() = 0;
    app->state.optflag[0] = 0;
    return app;
}

// A gizmo as the render pass would publish it: origin at (100, 100), the white
// view-plane square one corner up-right of it at (130, 70), the X handle to the
// right, the Y handle up and the Z handle down-right (screen y grows downward).
gizmo::ScreenGeometry SampleGeometry() {
    gizmo::ScreenGeometry geometry;
    geometry.valid = true;
    geometry.originX = 100.0f;
    geometry.originY = 100.0f;
    geometry.planeX = 130.0f;
    geometry.planeY = 70.0f;
    geometry.tipX[0] = 160.0f;
    geometry.tipY[0] = 100.0f;
    geometry.tipX[1] = 100.0f;
    geometry.tipY[1] = 40.0f;
    geometry.tipX[2] = 120.0f;
    geometry.tipY[2] = 120.0f;
    for (int axis = 0; axis < 3; ++axis) {
        geometry.endX[axis] = geometry.tipX[axis];
        geometry.endY[axis] = geometry.tipY[axis];
        geometry.axisAvailable[axis] = true;
    }
    geometry.half = 8.0f;
    geometry.rod = 3.0f;
    return geometry;
}

// A gizmo seen from a turned camera: the X handle points up-right on screen, the
// Y handle up-left, and the depth direction uses the renderer's screen-space
// fallback so it remains visible and hittable.
gizmo::ScreenGeometry TiltedGeometry() {
    gizmo::ScreenGeometry geometry;
    geometry.valid = true;
    geometry.originX = 200.0f;
    geometry.originY = 200.0f;
    geometry.planeX = 230.0f;
    geometry.planeY = 170.0f;
    geometry.tipX[0] = 230.0f;
    geometry.tipY[0] = 170.0f;
    geometry.tipX[1] = 170.0f;
    geometry.tipY[1] = 170.0f;
    geometry.tipX[2] = 220.0f;
    geometry.tipY[2] = 220.0f;
    geometry.endX[0] = geometry.tipX[0];
    geometry.endY[0] = geometry.tipY[0];
    geometry.endX[1] = geometry.tipX[1];
    geometry.endY[1] = geometry.tipY[1];
    geometry.endX[2] = geometry.tipX[2];
    geometry.endY[2] = geometry.tipY[2];
    geometry.half = 8.0f;
    geometry.rod = 3.0f;
    geometry.axisAvailable[0] = true;
    geometry.axisAvailable[1] = true;
    geometry.axisAvailable[2] = true;
    return geometry;
}

// ---- 1. hit testing -------------------------------------------------------
void HandleHitTesting() {
    const gizmo::ScreenGeometry geometry = SampleGeometry();
    Check(gizmo::HitTest(geometry, 100.0f, 100.0f) == gizmo::Handle::Plane,
          "the white square owns its corner on the axis origin");
    Check(gizmo::HitTest(geometry, 115.0f, 85.0f) == gizmo::Handle::Plane,
          "the inside of the white square is the plane handle");
    Check(gizmo::HitTest(geometry, 160.0f, 100.0f) == gizmo::Handle::AxisX,
          "the X square handle is hit at its centre");
    Check(gizmo::HitTest(geometry, 167.0f, 100.0f) == gizmo::Handle::AxisX,
          "the X square handle is hit at its edge");
    Check(gizmo::HitTest(geometry, 100.0f, 40.0f) == gizmo::Handle::AxisY,
          "the Y square handle is hit at its centre");
    Check(gizmo::HitTest(geometry, 120.0f, 120.0f) == gizmo::Handle::AxisZ,
          "the Z square handle is hit at its centre");
    // The rods are hit beyond the white square's diagonal only: closer in, the
    // pointer belongs to the box.
    Check(gizmo::HitTest(geometry, 145.0f, 100.0f) == gizmo::Handle::AxisX,
          "the X rod is hit outside the white square");
    Check(gizmo::HitTest(geometry, 120.0f, 100.0f) == gizmo::Handle::Plane,
          "the X rod does not steal pixels inside the white square");
    Check(gizmo::HitTest(geometry, 131.0f, 69.0f) == gizmo::Handle::None,
          "just outside the white square's far corner is empty");
    Check(gizmo::HitTest(geometry, 300.0f, 300.0f) == gizmo::Handle::None,
          "empty viewport space hits nothing");
    // The generic hit tester still respects an unavailable axis, though the
    // renderer now publishes a screen-space fallback for view-aligned axes.
    gizmo::ScreenGeometry collapsed = geometry;
    collapsed.tipX[2] = collapsed.originX;
    collapsed.tipY[2] = collapsed.originY;
    collapsed.endX[2] = collapsed.originX;
    collapsed.endY[2] = collapsed.originY;
    collapsed.axisAvailable[2] = false;
    Check(gizmo::HitTest(collapsed, 100.0f, 100.0f) == gizmo::Handle::Plane,
          "an unavailable depth axis leaves the origin to the white square");
    collapsed.axisAvailable[2] = true;
    Check(gizmo::HitTest(collapsed, 100.0f, 100.0f) == gizmo::Handle::AxisZ,
          "a depth square that is still available wins over the white square");
    // An unavailable axis is also skipped on its rod, not just its square.
    gizmo::ScreenGeometry hidden = geometry;
    hidden.axisAvailable[0] = false;
    Check(gizmo::HitTest(hidden, 160.0f, 100.0f) == gizmo::Handle::None &&
              gizmo::HitTest(hidden, 145.0f, 100.0f) == gizmo::Handle::None,
          "an unavailable axis has no square and no rod");
    // Unusable geometry turns every handle off.
    gizmo::ScreenGeometry invalid;
    Check(gizmo::HitTest(invalid, 100.0f, 100.0f) == gizmo::Handle::None,
          "an unpublished geometry has no handles");
    invalid = geometry;
    invalid.valid = false;
    Check(gizmo::HitTest(invalid, 100.0f, 100.0f) == gizmo::Handle::None,
          "an invalidated geometry has no handles");
    invalid = geometry;
    invalid.half = 0.0f;
    Check(gizmo::HitTest(invalid, 160.0f, 100.0f) == gizmo::Handle::None,
          "a zero-sized handle geometry has no handles");
    Check(gizmo::AxisOf(gizmo::Handle::AxisX) == 0 &&
              gizmo::AxisOf(gizmo::Handle::AxisY) == 1 &&
              gizmo::AxisOf(gizmo::Handle::AxisZ) == 2 &&
              gizmo::AxisOf(gizmo::Handle::Plane) == -1 &&
              gizmo::AxisOf(gizmo::Handle::None) == -1,
          "handle to coordinate-selector axis mapping");
    std::puts("PASS scale handle hit testing");
}

// ---- 2. disabled gates ----------------------------------------------------
void DisabledGates() {
    Fixture fixture;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    SelectModel(app.get(), fixture);
    fixture.selected[0] = 1;
    gizmo::Publish(app.get(), SampleGeometry());

    app->EditMode() = ViewportEditMode::Scale;
    Check(gizmo::Active(app.get()), "scale mode with a selection activates the handles");
    Check(gizmo::HitTest(app.get(), 160.0f, 100.0f) == gizmo::Handle::AxisX,
          "the gated hit test reaches the active handles");

    app->EditMode() = ViewportEditMode::Bone;
    Check(!gizmo::Active(app.get()), "select mode deactivates the handles");
    Check(gizmo::HitTest(app.get(), 160.0f, 100.0f) == gizmo::Handle::None,
          "select mode cannot drag a handle");
    app->EditMode() = ViewportEditMode::BoneBox;
    Check(gizmo::HitTest(app.get(), 100.0f, 100.0f) == gizmo::Handle::None,
          "box-select mode cannot drag a handle");
    app->EditMode() = ViewportEditMode::Camera;
    Check(gizmo::HitTest(app.get(), 160.0f, 100.0f) == gizmo::Handle::None,
          "the bone rotate cursor cannot drag a handle");

    app->EditMode() = ViewportEditMode::Scale;
    app->state.optflag[0] = 1;
    Check(!gizmo::Active(app.get()),
          "the camera/accessory panel disables the handles");
    Check(gizmo::HitTest(app.get(), 160.0f, 100.0f) == gizmo::Handle::None,
          "the camera/accessory panel cannot drag a handle");
    app->state.optflag[0] = 0;

    app->PlaybackActive() = 1;
    Check(!gizmo::Active(app.get()), "playback disables the handles");
    Check(gizmo::HitTest(app.get(), 100.0f, 100.0f) == gizmo::Handle::None,
          "playback cannot drag a handle");
    app->PlaybackActive() = 0;

    fixture.selected[0] = 0;
    fixture.record->selectedBone = -1;
    Check(!gizmo::Active(app.get()), "no selected bone disables the handles");
    Check(gizmo::HitTest(app.get(), 160.0f, 100.0f) == gizmo::Handle::None,
          "an invalid selection cannot drag a handle");
    fixture.record->selectedBone = 0;
    fixture.selected[0] = 1;

    // A stale publish from an earlier frame must not survive an unpublished one.
    gizmo::Publish(app.get(), gizmo::ScreenGeometry{});
    Check(!gizmo::Active(app.get()),
          "an unpublished frame deactivates the handles");
    Check(gizmo::HitTest(app.get(), 160.0f, 100.0f) == gizmo::Handle::None,
          "an unpublished frame cannot drag a handle");

    // The scale mode must not change the renderer routing: it is a bone edit
    // mode, so the model keeps the fixed-function path.
    app->EditMode() = ViewportEditMode::Scale;
    Check(!app->UsesViewportTool(), "scale mode keeps the model renderer");
    app->EditMode() = ViewportEditMode::Camera;
    Check(app->UsesViewportTool(), "the bone rotate cursor keeps its own path");
    app->state.modelSlots[0] = nullptr;
    std::puts("PASS scale handle disabled gates");
}

// ---- 3a. HUD row drag: vertical pointer delta ------------------------------
void HudDragPaths() {
    // The bottom-right HUD row keeps the original rule: the amount is the
    // vertical pointer delta.  bone_scale::Drag is that path, and the viewport
    // handles must not have changed it.
    Fixture fixture;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    SelectModel(app.get(), fixture);
    fixture.selected[0] = 1;
    const float factor = std::exp(0.1f);

    app->state.coordinateSystem = 1;                 // global
    app->PreviousMouseY() = 100;
    app->MouseY() = 90;
    bs::Drag(app.get(), 2);                          // the Z cell
    Near(bs::Pose(fixture.data(), 0).v[8], factor,
         "the HUD Z cell scales the Z axis only");
    Near(bs::Pose(fixture.data(), 0).v[0], 1.0f,
         "the HUD Z cell leaves X alone");
    Near(bs::Pose(fixture.data(), 0).v[4], 1.0f,
         "the HUD Z cell leaves Y alone");
    Check(fixture.dirty[0] != 0, "a HUD drag marks the bone for registration");

    // One increment, not two: a 10-pixel drag applies the same factor once.
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseY() = 100;
    app->MouseY() = 90;
    bs::Drag(app.get(), 0);                          // the X cell
    Near(bs::Pose(fixture.data(), 0).v[0], factor,
         "the HUD X cell applies one increment per drag");
    Check(bs::Pose(fixture.data(), 0).v[4] == 1.0f &&
              bs::Pose(fixture.data(), 0).v[8] == 1.0f,
          "the HUD X cell leaves the other two axes alone");

    // Dragging down shrinks by the same factor.
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseY() = 90;
    app->MouseY() = 100;
    bs::Drag(app.get(), 0);
    Near(bs::Pose(fixture.data(), 0).v[0], 1.0f / factor,
         "the HUD row shrinks when dragged down");

    // A purely horizontal pointer delta is not a HUD row gesture at all.
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseY() = 100;
    app->MouseY() = 100;
    app->PreviousMouseX() = 100;
    app->MouseX() = 130;
    bs::Drag(app.get(), 0);
    Check(bs::Identity(bs::Pose(fixture.data(), 0)),
          "the HUD row ignores horizontal movement");

    app->PreviousMouseY() = 100;
    app->MouseY() = 100;
    bs::Drag(app.get(), -1);
    Check(bs::Identity(bs::Pose(fixture.data(), 0)),
          "a zero-length HUD drag applies nothing");
    app->PlaybackActive() = 1;
    app->PreviousMouseY() = 100;
    app->MouseY() = 90;
    bs::Drag(app.get(), 0);
    Check(bs::Identity(bs::Pose(fixture.data(), 0)),
          "the HUD drag is inert during playback");
    app->PlaybackActive() = 0;
    app->state.modelSlots[0] = nullptr;
    std::puts("PASS HUD row drag stays vertical");
}

// ---- 3b. viewport handles: drag along the projected handle direction -------
void ScreenDragPaths() {
    // Wiring invariants the drag path depends on: the three axis handles are
    // consecutive so the frame driver's subtraction yields 0/1/2, the whole
    // gizmo group sits above the HUD row's ScaleZ (the centred-drag gate and the
    // undo branch both key on that), and the handle-to-axis mapping round-trips.
    Check(static_cast<int>(ViewportDragMode::ScaleGizmoY) ==
                  static_cast<int>(ViewportDragMode::ScaleGizmoX) + 1 &&
              static_cast<int>(ViewportDragMode::ScaleGizmoZ) ==
                  static_cast<int>(ViewportDragMode::ScaleGizmoX) + 2,
          "the axis handle drag modes are consecutive, so the axis index is a "
          "subtraction");
    Check(static_cast<int>(ViewportDragMode::ScaleZ) <
              static_cast<int>(ViewportDragMode::ScaleGizmoX) &&
              static_cast<int>(ViewportToolAction::ScaleZ) <
                  static_cast<int>(ViewportToolAction::ScaleGizmoX),
          "the gizmo group sits above the HUD row in both enums");
    for (int axis = 0; axis < 3; ++axis)
        Check(gizmo::AxisOf(gizmo::HandleOfAxis(axis)) == axis,
              "the handle-to-axis mapping round-trips");
    Check(gizmo::HandleOfAxis(-1) == gizmo::Handle::None &&
              gizmo::HandleOfAxis(3) == gizmo::Handle::None,
          "an out-of-range axis has no handle");

    Fixture fixture;
    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    SelectModel(app.get(), fixture);
    fixture.selected[0] = 1;
    app->state.coordinateSystem = 1;                 // global
    const gizmo::ScreenGeometry axisAligned = SampleGeometry();

    // The amount is the pointer delta projected onto the handle's own screen
    // direction, so it follows whichever way the handle points: the X handle
    // takes a horizontal drag, the Y handle an upward one, and neither reacts
    // to a delta perpendicular to itself.
    Check(gizmo::ScreenDragAmount(axisAligned, gizmo::Handle::AxisX, 10.0f, 0.0f) ==
              10.0f,
          "the X handle takes a horizontal drag");
    Check(gizmo::ScreenDragAmount(axisAligned, gizmo::Handle::AxisX, 0.0f, 10.0f) ==
              0.0f,
          "the X handle ignores a vertical drag");
    Check(gizmo::ScreenDragAmount(axisAligned, gizmo::Handle::AxisY, 0.0f, -10.0f) ==
              10.0f,
          "the Y handle takes an upward drag");
    Check(gizmo::ScreenDragAmount(axisAligned, gizmo::Handle::AxisY, -10.0f, 0.0f) ==
              0.0f,
          "the Y handle ignores a horizontal drag");
    Check(gizmo::ScreenDragAmount(axisAligned, gizmo::Handle::AxisZ, 0.0f, 0.0f) ==
              0.0f,
          "a zero pointer delta is zero on every handle");
    const float planeAmount =
        10.0f * 30.0f / std::sqrt(30.0f * 30.0f + 30.0f * 30.0f);
    Near(gizmo::ScreenDragAmount(axisAligned, gizmo::Handle::Plane, 10.0f, 0.0f),
         planeAmount,
         "the white square takes a horizontal drag along its diagonal");
    Near(gizmo::ScreenDragAmount(axisAligned, gizmo::Handle::Plane, 0.0f, -10.0f),
         planeAmount,
         "the white square takes a vertical drag along its diagonal");
    // The amount is the projection on the unit direction, so halving the
    // handle's projected length does not halve the drag amount.
    gizmo::ScreenGeometry shortHandle = TiltedGeometry();
    shortHandle.tipX[0] = shortHandle.originX + 15.0f;
    shortHandle.tipY[0] = shortHandle.originY - 15.0f;
    shortHandle.endX[0] = shortHandle.tipX[0];
    shortHandle.endY[0] = shortHandle.tipY[0];
    const float shortAmount =
        (10.0f * 15.0f + (-10.0f) * (-15.0f)) / std::sqrt(15.0f * 15.0f +
                                                          15.0f * 15.0f);
    Near(gizmo::ScreenDragAmount(shortHandle, gizmo::Handle::AxisX, 10.0f, -10.0f),
         shortAmount,
         "the amount is normalized by the handle's projected length");

    // A horizontal drag on the X handle scales X, through the real entry point.
    const float factor = std::exp(0.1f);
    app->EditMode() = ViewportEditMode::Scale;
    gizmo::Publish(app.get(), axisAligned);
    app->PreviousMouseX() = 100;
    app->MouseX() = 110;
    app->PreviousMouseY() = 100;
    app->MouseY() = 100;
    bs::DragScreen(app.get(), 0);
    Near(bs::Pose(fixture.data(), 0).v[0], factor,
         "a horizontal drag on the X handle grows X");
    Check(bs::Pose(fixture.data(), 0).v[4] == 1.0f &&
              bs::Pose(fixture.data(), 0).v[8] == 1.0f,
          "the horizontal X handle drag leaves Y and Z alone");

    // The coordinate selector still chooses the axis: with the default camera
    // the view X axis is world X, so the same handle drag scales X in view mode.
    bs::SetPose(fixture.data(), 0, {});
    app->state.coordinateSystem = 2;                 // view
    app->PreviousMouseX() = 100;
    app->MouseX() = 110;
    app->PreviousMouseY() = 100;
    app->MouseY() = 100;
    bs::DragScreen(app.get(), 0);
    Near(bs::Pose(fixture.data(), 0).v[0], factor,
         "the view coordinate state scales the selector's own axis");
    app->state.coordinateSystem = 1;                 // back to global

    // An upward drag on the Y handle scales Y.
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseX() = 100;
    app->MouseX() = 100;
    app->PreviousMouseY() = 100;
    app->MouseY() = 90;
    bs::DragScreen(app.get(), 1);
    Near(bs::Pose(fixture.data(), 0).v[4], factor,
         "an upward drag on the Y handle grows Y");
    Check(bs::Pose(fixture.data(), 0).v[0] == 1.0f &&
              bs::Pose(fixture.data(), 0).v[8] == 1.0f,
          "the Y handle drag leaves X and Z alone");

    // Turned camera: the X handle points up-right on screen, so the drag has to
    // follow that direction instead of the vertical axis.
    const gizmo::ScreenGeometry tilted = TiltedGeometry();
    gizmo::Publish(app.get(), tilted);
    const float diagonalLength = std::sqrt(30.0f * 30.0f + 30.0f * 30.0f);
    const float alongAmount =
        (10.0f * 30.0f + (-10.0f) * (-30.0f)) / diagonalLength;
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseX() = 100;
    app->MouseX() = 110;
    app->PreviousMouseY() = 100;
    app->MouseY() = 90;
    bs::DragScreen(app.get(), 0);
    Near(bs::Pose(fixture.data(), 0).v[0], std::exp(alongAmount * 0.01f),
         "a turned-camera handle follows its own screen direction");
    // The perpendicular delta is exactly zero after the projection.
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseX() = 100;
    app->MouseX() = 110;
    app->PreviousMouseY() = 100;
    app->MouseY() = 110;
    bs::DragScreen(app.get(), 0);
    Check(bs::Identity(bs::Pose(fixture.data(), 0)),
          "a drag perpendicular to the turned handle changes nothing");

    // The white square: horizontal and vertical drags both move it, by the same
    // projected amount on its diagonal.
    const float planeFactor = std::exp(planeAmount * 0.01f);
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseX() = 100;
    app->MouseX() = 110;
    app->PreviousMouseY() = 100;
    app->MouseY() = 100;
    bs::DragScreenPlane(app.get());
    Near(bs::Pose(fixture.data(), 0).v[0], planeFactor,
         "a horizontal drag scales the white square's view-plane X");
    Near(bs::Pose(fixture.data(), 0).v[4], planeFactor,
         "a horizontal drag scales the white square's view-plane Y");
    Near(bs::Pose(fixture.data(), 0).v[8], 1.0f,
         "the white square leaves the depth direction alone");
    Check(bs::Pose(fixture.data(), 0).v[1] == 0.0f,
          "the white square adds no shear to the view plane");

    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseX() = 100;
    app->MouseX() = 100;
    app->PreviousMouseY() = 100;
    app->MouseY() = 90;
    bs::DragScreenPlane(app.get());
    Near(bs::Pose(fixture.data(), 0).v[0], planeFactor,
         "a vertical drag scales the white square the same way");
    Near(bs::Pose(fixture.data(), 0).v[4], planeFactor,
         "a vertical drag scales both view-plane directions");

    // The turned-camera fallback axis remains visible, hit-testable, and drives
    // the corresponding scale component instead of disappearing at the origin.
    bs::SetPose(fixture.data(), 0, {});
    app->PreviousMouseX() = 220;
    app->MouseX() = 230;
    app->PreviousMouseY() = 220;
    app->MouseY() = 230;
    bs::DragScreen(app.get(), 2);
    Check(bs::Pose(fixture.data(), 0).v[8] > 1.0f,
          "the view-aligned fallback axis still scales Z");
    Check(gizmo::HitTest(tilted, 220.0f, 220.0f) == gizmo::Handle::AxisZ,
          "the view-aligned fallback square owns its visible endpoint");

    // No published frame, playback, and the other edit modes all leave the
    // handles inert.
    bs::SetPose(fixture.data(), 0, {});
    gizmo::Publish(app.get(), gizmo::ScreenGeometry{});
    bs::DragScreen(app.get(), 0);
    bs::DragScreenPlane(app.get());
    Check(bs::Identity(bs::Pose(fixture.data(), 0)),
          "an unpublished frame applies no viewport handle drag");

    gizmo::Publish(app.get(), axisAligned);
    app->PlaybackActive() = 1;
    app->PreviousMouseX() = 100;
    app->MouseX() = 110;
    bs::DragScreen(app.get(), 0);
    Check(bs::Identity(bs::Pose(fixture.data(), 0)),
          "the viewport handle drag is inert during playback");
    app->PlaybackActive() = 0;

    app->EditMode() = ViewportEditMode::Bone;
    bs::DragScreen(app.get(), 0);
    Check(bs::Identity(bs::Pose(fixture.data(), 0)),
          "the viewport handle drag only runs in scale mode");

    app->state.modelSlots[0] = nullptr;
    std::puts("PASS viewport handle drag follows the projected direction");
}

// ---- 3c. the original move / rotate wiring stays untouched ----------------
void OriginalGestureWiring() {
    // The original move and rotate gestures are direction based, not vertical
    // only: the screen-plane bone move (src/app/frame_modes_bone.cpp,
    // ApplyScreenPlaneBoneMove) reads BOTH pointer axes, and the rotate ring
    // keeps its own gesture.  This change must not renumber the ids their
    // handlers are keyed on, and frame_modes.cpp's kDragMode table only grew new
    // entries at the end for operations 26..29.
    Check(static_cast<int>(ViewportToolAction::TransformAxisX) == 9 &&
              static_cast<int>(ViewportToolAction::TransformAxisY) == 10 &&
              static_cast<int>(ViewportToolAction::TransformAxisZ) == 11 &&
              static_cast<int>(ViewportToolAction::PhysicsAxisX) == 12 &&
              static_cast<int>(ViewportToolAction::PhysicsAxisY) == 13 &&
              static_cast<int>(ViewportToolAction::PhysicsAxisZ) == 14,
          "the rotate row (9..11) and the move row (12..14) keep their operation ids");
    Check(static_cast<int>(ViewportToolAction::CameraAdjustX) == 15 &&
              static_cast<int>(ViewportToolAction::AngleAdjustZ) == 20 &&
              static_cast<int>(ViewportToolAction::CycleCoordinateSystem) == 21,
          "the camera/accessory rows and the coordinate strip keep their ids");
    Check(static_cast<int>(ViewportDragMode::LocalAxisTranslateX) == 4 &&
              static_cast<int>(ViewportDragMode::LocalAxisTranslateY) == 5 &&
              static_cast<int>(ViewportDragMode::LocalAxisTranslateZ) == 6,
          "the rotate row's drag modes keep their values");
    Check(static_cast<int>(ViewportDragMode::BoneScale) == 7 &&
              static_cast<int>(ViewportDragMode::BoneMoveVertical) == 8 &&
              static_cast<int>(ViewportDragMode::BoneMoveScreenPlane) == 9,
          "the legacy scale and both bone move modes keep their values");
    Check(static_cast<int>(ViewportDragMode::PhysicsAxisX) == 10 &&
              static_cast<int>(ViewportDragMode::PhysicsAxisY) == 11 &&
              static_cast<int>(ViewportDragMode::PhysicsAxisZ) == 12,
          "the move row's drag modes keep their values");
    Check(static_cast<int>(ViewportDragMode::ScaleUniform) == 19 &&
              static_cast<int>(ViewportDragMode::ScaleZ) == 22 &&
              static_cast<int>(ViewportDragMode::ScaleGizmoX) == 23 &&
              static_cast<int>(ViewportDragMode::ScaleGizmoPlane) == 26,
          "the HUD scale row and the gizmo group keep their values");
    std::puts("PASS original move / rotate wiring unchanged");
}

// ---- 4. mode cycle --------------------------------------------------------
void ModeCycle() {
    Check(NextBoneSelectMode(ViewportEditMode::None) == ViewportEditMode::Bone,
          "none steps into select");
    Check(NextBoneSelectMode(ViewportEditMode::Bone) == ViewportEditMode::None,
          "select toggles back to none (box select is Shift + drag)");
    Check(NextBoneSelectMode(ViewportEditMode::BoneBox) == ViewportEditMode::None,
          "a loaded box-select mode counts as select and toggles off");
    Check(NextBoneSelectMode(ViewportEditMode::None) ==
              NextBoneSelectMode(ViewportEditMode::Camera),
          "any other mode steps into plain select");
    Check(NextBoneSelectMode(ViewportEditMode::Scale) == ViewportEditMode::Bone,
          "the scale mode steps into plain select");
    Check(NextBoneSelectMode(ViewportEditMode::Light) == ViewportEditMode::Bone,
          "the move mode steps into plain select");
    // The three states are distinct and none of them is the box-only mode the
    // panel no longer has.
    Check(static_cast<std::int32_t>(ViewportEditMode::Bone) == 0 &&
              static_cast<std::int32_t>(ViewportEditMode::BoneBox) == 1 &&
              static_cast<std::int32_t>(ViewportEditMode::None) == 2 &&
              static_cast<std::int32_t>(ViewportEditMode::Camera) == 3 &&
              static_cast<std::int32_t>(ViewportEditMode::Light) == 4,
          "the PMM edit-mode byte keeps its original values");
    Check(static_cast<std::int32_t>(ViewportEditMode::Scale) == 6,
          "the scale mode uses the free edit-mode value");
    std::puts("PASS select button toggle");
}

// ---- 5. captions and the panel grid ---------------------------------------
// The same LOGFONTA DrawControlText builds (ui_control_text.cpp): Tahoma 13 for
// the English and Chinese passes, MS P Gothic 12 (its Shift-JIS bytes) for the
// Japanese one, both bold with charset 0x80.
int CaptionWidth(const std::wstring& text, bool japanese) {
    LOGFONTA lf{};
    if (japanese) {
        strcpy_s(lf.lfFaceName,
                 "\x82\x6c\x82\x72 \x82\x6f\x83\x53\x83\x56\x83\x62\x83\x4e");
        lf.lfHeight = 12;
    } else {
        strcpy_s(lf.lfFaceName, "Tahoma");
        lf.lfHeight = 13;
    }
    lf.lfWeight = 700;
    lf.lfCharSet = 0x80;
    HFONT font = CreateFontIndirectA(&lf);
    Check(font != nullptr, "caption font");
    HDC dc = CreateCompatibleDC(nullptr);
    Check(dc != nullptr, "caption device context");
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    const BOOL measured = GetTextExtentPoint32W(
        dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previous);
    DeleteDC(dc);
    DeleteObject(font);
    Check(measured != FALSE, "caption measurement");
    return static_cast<int>(size.cx);
}

// Caption a cell shows; the select button keeps one caption in every mode.
std::wstring WidestCellCaption(int id, unsigned char language) {
    if (id != panel::kBoneSelectRadio)
        return UiControlCaption(id, language);
    return BoneModePanelCaption(ViewportEditMode::Bone, language);
}

// One row of the grid, in display order.
struct GridRow {
    int ids[3];
    int yOffset;
};

void CaptionsAndGrid() {
    // Exact captions of the select button, in the three languages; a loaded
    // box-select mode no longer changes it.
    Check(BoneModePanelCaption(ViewportEditMode::Bone, kUiJapanese) ==
              std::wstring(L"\x9078\x3000\x629e") &&
              BoneModePanelCaption(ViewportEditMode::BoneBox, kUiJapanese) ==
                  std::wstring(L"\x9078\x3000\x629e"),
          "select button shows 選　択");
    Check(BoneModePanelCaption(ViewportEditMode::Bone, kUiEnglish) ==
              std::wstring(L"Select") &&
              BoneModePanelCaption(ViewportEditMode::BoneBox, kUiEnglish) ==
                  std::wstring(L"Select"),
          "select button English caption");
    Check(BoneModePanelCaption(ViewportEditMode::Bone, kUiChinese) ==
              std::wstring(L"\x9009\x62e9"),
          "select button Chinese caption");
    // The scale button keeps a caption in every language.
    Check(UiControlCaption(panel::kBoneScaleRadio, kUiJapanese) ==
              std::wstring(L"\x62e1\x5927\x7e2e\x5c0f") &&
              UiControlCaption(panel::kBoneScaleRadio, kUiEnglish) ==
                  std::wstring(L"Scale") &&
              UiControlCaption(panel::kBoneScaleRadio, kUiChinese) ==
                  std::wstring(L"\x7f29\x653e"),
          "the scale button is captioned 拡大縮小 / scale / 缩放");

    // The grid: three 60-pixel columns 64 pixels apart, four rows, and every
    // cell holds the button the model panel documents.
    const GridRow rows[4] = {
        {{panel::kBoneSelectRadio, panel::kBoneRotateRadio,
          panel::kBoneMoveRadio}, 140},
        {{panel::kBoneScaleRadio, panel::kBoneSelectAllButton,
          panel::kSelectUnregisteredButton}, 105},
        {{panel::kBoneCopyButton, panel::kBonePasteButton,
          panel::kBoneReversePasteButton}, 70},
        {{panel::kRegisterFrameButton, panel::kBoneResetButton,
          panel::kPhysicsCheckbox}, 36}};
    const int expectedX[3] = {366, 430, 494};
    Check(panel::kBoneModeGridCount == 12, "the bone-operation grid has 12 cells");
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 3; ++column) {
            const panel::BoneModeCell& cell =
                panel::kBoneModeGrid[row * 3 + column];
            Check(cell.id == rows[row].ids[column],
                  "bone-operation grid cell holds the expected control");
            Check(cell.x == (row==3&&column==2?500:expectedX[column]),
                  "bone-operation grid column x position");
            if (row < 3)
                Check(cell.yOffset == rows[row].yOffset,
                      "bone-operation grid row offset");
            else
                // Row 4 keeps the original geometry: reset sits four pixels
                // higher than the register button beside it.
                Check(cell.yOffset == (column == 1 ? 32 : 36),
                      "the register/reset/physics row keeps its original offsets");
        }
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            const panel::BoneModeCell& cell =
                panel::kBoneModeGrid[row * 3 + column];
            Check(cell.width == 60, "bone-operation grid cell width");
            Check(cell.height == (row == 2 ? 24 : 28),
                  "bone-operation grid cell height");
        }
    Check(panel::kBoneModeGrid[11].width == 48 &&
              panel::kBoneModeGrid[11].height == 28 &&
              panel::kBoneModeGrid[11].x == 500,
          "the physics toggle keeps its original 48x28 cell");
    for (int row = 0; row < 3; ++row) {
        const int pitch = rows[row].yOffset - rows[row + 1].yOffset;
        Check(pitch == (row==2?34:35), "bone-operation grid keeps original final-row spacing");
    }
    // No two cells overlap.
    for (int a = 0; a < panel::kBoneModeGridCount; ++a)
        for (int b = a + 1; b < panel::kBoneModeGridCount; ++b) {
            const panel::BoneModeCell& first = panel::kBoneModeGrid[a];
            const panel::BoneModeCell& second = panel::kBoneModeGrid[b];
            const bool horizontal =
                first.x + first.width <= second.x ||
                second.x + second.width <= first.x;
            const bool vertical =
                first.yOffset >= second.yOffset + second.height ||
                second.yOffset >= first.yOffset + first.height;
            Check(horizontal || vertical,
                  "bone-operation grid cells never overlap");
        }

    // Caption fit, measured with the font each language actually draws with.
    // The bound is the widest caption of the 60-pixel cells this change does not
    // rewrite: the new wordings must not need more room than a caption that
    // already ships in the same cell size, and must keep a margin on both sides.
    const int rewrittenIds[] = {panel::kBoneSelectRadio,
                                panel::kBoneScaleRadio,
                                panel::kBoneReversePasteButton};
    const auto rewritten = [&](int id) {
        for (int candidate : rewrittenIds)
            if (candidate == id)
                return true;
        return false;
    };
    const int measuredIds[] = {
        panel::kBoneSelectAllButton, panel::kSelectUnregisteredButton,
        panel::kBoneMoveRadio, panel::kBoneRotateRadio, panel::kBoneCopyButton,
        panel::kBonePasteButton};
    for (unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
        const bool japanese = language == kUiJapanese;
        const char* name = language == kUiJapanese ? "Japanese"
                         : language == kUiChinese ? "Chinese" : "English";
        int reference = 0;
        for (int id : measuredIds)
            reference = std::max(
                reference,
                CaptionWidth(UiControlCaption(id, language), japanese));
        for (const panel::BoneModeCell& cell : panel::kBoneModeGrid) {
            const std::wstring caption = WidestCellCaption(cell.id, language);
            Check(!caption.empty(), "every bone-operation cell has a caption");
            const int width = CaptionWidth(caption, japanese);
            std::printf("  %-8s cell %3d: %2d px (cell %d px, bound %d px)\n",
                        name, cell.id, width, cell.width, reference);
            if (!rewritten(cell.id))
                continue;
            Check(width <= reference,
                  "a new or rewritten caption fits the shipped caption bound");
            Check(width <= cell.width - 2,
                  "a new or rewritten caption keeps a margin in its cell");
        }
    }
    std::puts("PASS bone-operation panel grid, captions and caption fit");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    HandleHitTesting();
    DisabledGates();
    HudDragPaths();
    ScreenDragPaths();
    OriginalGestureWiring();
    ModeCycle();
    CaptionsAndGrid();
    std::puts("PASS: native bone scale viewport handles (hit test, gates, HUD "
              "and projected drag, original gesture wiring, mode cycle, panel "
              "grid)");
}
