// ===========================================================================
// MMS addition (no original VA): screen-space handles of the bone-scale gizmo.
// ===========================================================================
// The scale-mode gizmo is drawn in the viewport by debug_geometry.cpp
// (DrawBoneScaleHandles) during the 3D pass.  The hit test runs in the input
// path (frame_modes.cpp, ViewportToolAtPoint), which has no camera matrices of
// its own.  The render pass therefore publishes the projected handle geometry
// here and the input path reads it back, so the hit regions always match the
// handles the frame actually drew.
//
// The render pass publishes an invalid geometry whenever the handles are not
// drawn: another edit mode, the camera/light/accessory panel, playback, or no
// valid bone selection (bone_scale::Enabled).  The input path additionally
// re-checks those gates, so a stale publish can never be hit.
//
// Shape: the white square spans the coordinate selector's X and Y axes
// (local / global / view), so it projects to a parallelogram with one corner
// on the projected bone origin; in view mode that is an axis-aligned square.
// The three single-axis handles are billboarded squares at the ends of the
// coordinate selector's axes; their rods are hit only outside the white square.
// ===========================================================================
#pragma once

namespace mikumikustudio {
class MMDApp;

namespace bone_scale {
namespace gizmo {

// Which handle the pointer is over.  Plane scales both view-plane axes in one
// increment; AxisX/Y/Z scale the axis with that index in the current coordinate
// state (local / global / view), exactly like the X/Y/Z cells of the HUD scale
// row.  AxisX/Y/Z double as the 0/1/2 indices used by AxisOf().
enum class Handle : int {
    None = 0,
    Plane = 1,
    AxisX = 2,
    AxisY = 3,
    AxisZ = 4,
};

// Axis index (0/1/2) of an axis handle; -1 for Plane and None.
int AxisOf(Handle handle);

// Axis handle of a coordinate-selector axis (0/1/2); Handle::None when the
// index is out of range.
Handle HandleOfAxis(int axis);

// Screen geometry of one frame's gizmo, in client coordinates.  `valid` is
// false when the handles were not drawn, which turns every hit test off.
//
// originX/originY   projected gizmo origin (the selected bone's origin)
// planeX/planeY     the corner of the white square opposite the origin
// tipX/tipY         projected centres of the three square handles
// endX/endY         projected rod ends (equal to the square-handle centres)
// half              projected half extent of a square handle, in pixels
// rod               half width of a rod's hit band, in pixels
// axisAvailable     per axis: whether the axis has a visible endpoint.  The
//                   renderer gives view-aligned axes a short screen-space
//                   fallback so they remain drawn and hittable instead of
//                   collapsing onto the white square.
// planeSkewed       the square spans the coordinate selector's X and Y axes,
//                   so on screen it is a parallelogram with the edge corners
//                   planeRightX/Y (along X) and planeUpX/Y (along Y).  When
//                   false the square is the axis-aligned origin..plane box.
struct ScreenGeometry {
    bool valid = false;
    float originX = 0.0f;
    float originY = 0.0f;
    float planeX = 0.0f;
    float planeY = 0.0f;
    bool planeSkewed = false;
    float planeRightX = 0.0f;
    float planeRightY = 0.0f;
    float planeUpX = 0.0f;
    float planeUpY = 0.0f;
    float tipX[3]{};
    float tipY[3]{};
    float endX[3]{};
    float endY[3]{};
    float half = 0.0f;
    float rod = 0.0f;
    bool axisAvailable[3] = {false, false, false};
};

// Handles to paint highlighted this frame: the hovered or dragged viewport
// handle, or the HUD scale-row cell under the pointer (uniform lights every
// handle, X/Y/Z light that axis).
struct Highlight {
    bool plane = false;
    bool axis[3] = {false, false, false};
};
Highlight HighlightOf(MMDApp* app);

// Pointer hit test.  The square handles win over the rods and the white
// square, because in view mode the depth rod collapses onto the origin where
// the box would otherwise cover it.  The rods are hit only outside the box, so
// the box owns the pixels it covers.  Handles whose axis is not available are
// skipped entirely, which is what keeps the box's pixels free.
Handle HitTest(const ScreenGeometry& geometry, float x, float y);

// Signed drag amount of one handle for this frame's pointer delta, in screen
// pixels along the handle's own projected direction:
//
//   axis handles  dot((dx, dy), normalize(tip - origin))
//   plane handle  dot((dx, dy), normalize(planeCorner - origin))
//
// The axis form measures the drag along the direction the axis points on
// screen, so a horizontal axis is driven by a horizontal drag.  The plane form
// measures it along the square's diagonal, which projects at 45 degrees, so a
// horizontal and a vertical drag both move the plane handle.
//
// Returns 0 when the geometry is unusable, the axis handle is unavailable, or
// the direction is degenerate; a caller treats 0 as "apply nothing".
float ScreenDragAmount(const ScreenGeometry& geometry, Handle handle, float dx,
                       float dy);

// True when the handles are live: the bone scale edit mode is active, scaling
// is enabled (model panel, not playing, valid selected bone) and the render
// pass published this frame's geometry.  The input path and the regression
// tests use this one gate, so a disabled state can never be dragged.
bool Active(MMDApp* app);

// Gated hit test for the input path.  It returns Handle::None whenever
// Active() is false, so callers do not repeat the gates.
Handle HitTest(MMDApp* app, float x, float y);

// Render pass -> input path hand-off, keyed by the application instance.
void Publish(MMDApp* app, const ScreenGeometry& geometry);
const ScreenGeometry& Published(const MMDApp* app);

}  // namespace gizmo
}  // namespace bone_scale
}  // namespace mikumikustudio
