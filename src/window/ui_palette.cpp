#include "mikumikustudio/background_save.hpp"
// ===========================================================================
// VA 0x0042CEB0 - HandlePaletteChanged  (original: sub_42CEB0, ~0x4ED bytes)
// ===========================================================================
// Timeline-strip painter of the left panel.  Called with the paint DC from
// HandleWindowPaint 0x47C0A0 (wm_paint.cpp @ 0x47C206, right after the
// panel chrome) and from the WndProc 0x4C3A10 message-0x318 branch
// (wndproc.cpp, wParam as HDC).
//
// Sequence (exact, per the IDA disassembly):
//   GetClientRect(this+0xA06B8 = kPtrHwnd) into a local RECT; `bottom` is
//   Rect.bottom everywhere below.
//   4 gradient rows are driven by ColorLerp (0x40AD00) of the colour pair
//   this+0xA05E0 / this+0xA05E4 (kDwordCol656864 / kDwordCol656868), each
//   with an x87-computed factor truncated to float at the call boundary:
//     t1  = -45.0/((50.0-bottom)-161.0)          (dbl_52C940/190/948)
//     t2  = ((50.0-bottom)-228.0)/((50.0-bottom)-161.0)   (dbl_52C938)
//     v30 = (50.0-bottom)-161.0 (kept double, var_18)
//     t3a = -111.0/v30  (dbl_52C930)   t3b = -94.0/v30 (dbl_52C928)
//     t4  = ((50.0-bottom)-248.0)/((50.0-bottom)-161.0)   (dbl_52C920)
//   each result feeds FillPanelBottom (0x40DF10) as the gradient edge
//   colour; the "from" colours are the colour-table slots 0xA05E0/4,
//   0xA05F0/4, 0xA05E8/C, 0xA05F8/C (colour.txt pair table, see
//   offsets.hpp kDwordCol656xxx) or a previous lerp result.
//   Then three option-flag driven variable-width blocks (flags 0x2F8/0x2F9/
//   0x2FC, 0x2FA/0x2FD, 0x2FE/0x2FB = kByteOptflag0..6) that accumulate an
//   x-offset, followed by three closing rows (colours 0xA0618/C, 0xA0620/4).
//
// The "palette" name is historical (from the message-0x318 stub list); the
// function performs no palette API calls - it is the timeline strip painter.
// Original returns the BOOL of the last FillPanelBottom; the declared
// signature is void.
//
// Reference: ../translated/MikuMikuDance/fcn_0042ceb0.cpp (superseded by
// the IDA disassembly for the exact rect/colour/lerp arguments)
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>

#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/bottom_panel_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/bone_scale.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../plugins/native_pose_axis.hpp"
#endif
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/status_title_checked_gdi.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mikumikustudio {
namespace {

// Shift-JIS (cp932) label bytes from .rdata, used by the Japanese-UI branch
// of HandlePaletteChanged2 (0x42C140).  Bytes match the original exactly
// (byte_52C6DC / byte_52C728 / byte_52C7E0 / byte_52C7EC / byte_52C808 /
// byte_52C8A4 / byte_52C8B0 / byte_52C8C0).
constexpr char kS52C6DC[] = "\x8d\xc4\x90\xb6\x92\x86";  // 再生中 (playing)
constexpr char kS52C728[] = "\x8a\x70\x93\x78";          // 角度 (angle)
constexpr char kS52C7E0[] = "\x83\x7b\x81\x5b\x83\x93\x88\xca\x92\x75";  // ボーン位置 (bone place)
constexpr char kS52C7EC[] = "\x83\x4a\x83\x81\x83\x89\x83\x7b\x81\x5b\x83\x93\x92\xc7\x8f\x5d\x83\x82\x81\x5b\x83\x68\x92\x86";  // カメラボーン追従モード中
constexpr char kS52C808[] = "\x83\x4a\x83\x81\x83\x89\x83\x7b\x81\x5b\x83\x93\x92\xc7\x8f\x5d\x83\x82\x81\x5b\x83\x68\x92\x86\x20\x28\x92\xc7\x8f\x5d\x83\x7b\x83\x5e\x83\x93\x82\xf0\x89\xf0\x8f\x9c\x82\xb5\x82\xc4\x89\xba\x82\xb3\x82\xa2\x29";  // カメラボーン追従モード中 (追従ボタンを解除して下さい)
constexpr char kS52C8A4[] = "\x83\x4a\x83\x81\x83\x89\x92\x86\x90\x53";  // カメラ中心 (camera centre)
constexpr char kS52C8B0[] = "\xb6\xd2\xd7\xa5\x8f\xc6\x96\xbe\xa5\xb1\xb8\xbe\xbb\xd8";  // ｶﾒﾗ･照明･ｱｸｾｻﾘ
constexpr char kS52C8C0[] = "\xb6\xd2\xd7\xa5\x8f\xc6\x96\xbe\xa5\xb1\xb8\xbe\xbb\xd8\x20\x2f\x20\x25\x73";  // ｶﾒﾗ･照明･ｱｸｾｻﾘ / %s
// MMS addition: the scale readout's Japanese label.  The bone-scale channel is
// called サイズ in this codebase (animation_curve_host.cpp), so the readout
// reuses that wording.
constexpr char kJpScale[] = "\x83\x54\x83\x43\x83\x59";  // サイズ (scale)

// MMS addition: the readout text painted on the last refresh.  The per-frame
// tick (BoneScaleReadoutTick) repaints the status band only when the formatted
// values change, so a static scene costs one comparison per frame and an edit
// costs one repaint.
char g_scaleReadoutText[64] = {};

}  // namespace

// ===========================================================================
// MMS addition: selected-bone XYZ scale readout for the status bar at the top
// of the panel (the hideTop-25..hideTop-1 band directly below the main menu,
// which carries the "model : bone" line).  Display only: the readout is
// painted text, it owns no control, adds no interaction and never writes the
// scale state.
//
// The three numbers are the scale factors along the same axes the HUD rotate
// and scale rows edit (bone_scale.hpp: "axis -1 is uniform; 0/1/2 are X/Y/Z in
// the existing coordinate selector"), so their meaning follows the coordinate
// selector - local / global / view - and, in local mode, the selected bone's
// own axis definition (fixed axis / local axis, BoneLocalAxes).  They are
// deliberately not the raw diagonal of the stored matrix: bone_scale::Matrix
// is a row-vector matrix in the bone's unposed model-space basis, and that
// diagonal names different axes from the ones the tools edit.
//
// Rule, mirroring bone_scale.cpp AxisDirection and ApplyScaleIncrement:
//   M = bone_scale::Pose(model, bone)   the stored scale matrix
//   R = Inverse(M) * Linear(matInit)    the bone's rigid basis - exactly the
//       `bases[i]` the edit path conjugates its world increments through,
//       computed after ApplyTransforms has installed the corrected matrices
//   d = the coordinate-selector axis direction (view axis / world unit axis /
//       e_i * BoneLocalAxes * Linear(matInit))
//   u = d * R^-1                        that direction in the basis M lives in
//   factor_i = |u * M| / |u|            the stretch M gives that axis
// Identity M therefore reads 1,1,1, and a HUD row edit of factor f along the
// same axis reads exactly f: the same rule the edit applies, read back.
// ===========================================================================
bool SelectedBoneScaleReadout(MMDApp* app, float out[3]) {
    if (MMD_ORIGINAL_PROFILE) return false;
    out[0] = out[1] = out[2] = 1.0f;
    if (app == nullptr || app->state.optflag[0] != 0)
        return false;                       // camera/light panel: not a bone
    unsigned char* model = app->SelectedModel();
    if (model == nullptr)
        return false;
    const std::int32_t boneIdx = mdl::Mdl(model)->selectedBone;
    if (boneIdx < 0 ||
        boneIdx >= static_cast<std::int32_t>(mdl::Mdl(model)->boneCount))
        return false;
    auto* bones = mdl::Bones(model);
    if (bones == nullptr)
        return false;

    const bone_scale::Matrix scale = bone_scale::Pose(model, boneIdx);
    if (bone_scale::Identity(scale))
        return true;                        // the default 1,1,1

    // The basis must match the edit path's, which reads the corrected bone
    // matrices (bone_scale.cpp ApplyScaleIncrement).
    bone_scale::ApplyTransforms(model);
    const bone_scale::Matrix current =
        bone_scale::Linear(bones[boneIdx].matInit);
    bone_scale::Matrix inverseScale;
    if (!bone_scale::Inverse(scale, inverseScale))
        return true;
    const bone_scale::Matrix basis =
        bone_scale::Multiply(inverseScale, current);
    bone_scale::Matrix inverseBasis;
    if (!bone_scale::Inverse(basis, inverseBasis))
        return true;

    const int mode = EffectiveCoordinateState(app);
    for (int axis = 0; axis < 3; ++axis) {
        float direction[3] = {0.0f, 0.0f, 0.0f};
        // bone_scale.cpp AxisDirection verbatim: view axes in view mode, world
        // unit axes in global mode, the bone's own local axes otherwise.
        if (mode == 2) {
            ViewWorldAxis(app, axis, direction);
        } else if (mode == 1) {
            direction[axis] = 1.0f;
        } else {
            float local[16] = {};
            BoneLocalAxes(app, local);
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k)
                    direction[c] += local[axis * 4 + k] * current.v[k * 3 + c];
        }
        float unscaled[3] = {0.0f, 0.0f, 0.0f};
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                unscaled[c] += direction[k] * inverseBasis.v[k * 3 + c];
        float stretched[3] = {0.0f, 0.0f, 0.0f};
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                stretched[c] += unscaled[k] * scale.v[k * 3 + c];
        const double before = std::sqrt(double(unscaled[0]) * unscaled[0] +
            double(unscaled[1]) * unscaled[1] +
            double(unscaled[2]) * unscaled[2]);
        const double after = std::sqrt(double(stretched[0]) * stretched[0] +
            double(stretched[1]) * stretched[1] +
            double(stretched[2]) * stretched[2]);
        if (std::isfinite(before) && std::isfinite(after) && before > 1e-20)
            out[axis] = static_cast<float>(after / before);
    }
    return true;
}

bool SetSelectedBoneScaleAxis(MMDApp* app, int axis, float value) {
    if (!bone_scale::Enabled(app) || axis < 0 || axis > 2 ||
        !std::isfinite(value) || value <= 0.0f)
        return false;
    unsigned char* model = app->SelectedModel();
    const int bone = mdl::Mdl(model)->selectedBone;
    float currentValues[3] = {1.0f, 1.0f, 1.0f};
    if (!SelectedBoneScaleReadout(app, currentValues) ||
        !std::isfinite(currentValues[axis]) || currentValues[axis] <= 1e-8f)
        return false;
    auto* bones = mdl::Bones(model);
    bone_scale::ApplyTransforms(model);
    const bone_scale::Matrix current = bone_scale::Linear(bones[bone].matInit);
    const bone_scale::Matrix scale = bone_scale::Pose(model, bone);
    bone_scale::Matrix inverseScale;
    if (!bone_scale::Inverse(scale, inverseScale)) return false;
    const bone_scale::Matrix basis = bone_scale::Multiply(inverseScale, current);
    float direction[3] = {};
    const int mode = EffectiveCoordinateState(app);
    if (mode == 2) {
        ViewWorldAxis(app, axis, direction);
    } else if (mode == 1) {
        direction[axis] = 1.0f;
    } else {
        float local[16] = {};
        BoneLocalAxes(app, local);
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                direction[c] += local[axis * 4 + k] * current.v[k * 3 + c];
    }
    return bone_scale::Scale(model, bone, basis, direction,
                             value / currentValues[axis], false);
}

bool BoneScaleReadoutText(MMDApp* app, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0)
        return false;
    out[0] = '\0';
    if (MMD_ORIGINAL_PROFILE) return false;
    if (app == nullptr || app->state.optflag[0] != 0)
        return false;                       // hidden on the camera panel
    // The status bar replaces its content with "Playing" while playing
    // (HandlePaletteChanged2); the scale readout hides with it, which also
    // keeps a running playback from repainting every frame.
    if (app->FrameStepPlayback() == 0 && app->PlaybackActive() != 0)
        return false;
    float values[3] = {1.0f, 1.0f, 1.0f};
    if (SelectedBoneScaleReadout(app, values))
        std::snprintf(out, cap, "X %1.2f Y %1.2f Z %1.2f",
                      static_cast<double>(values[0]),
                      static_cast<double>(values[1]),
                      static_cast<double>(values[2]));
    else
        // Clear empty state, the same marker the value fields show without a
        // selected bone (PostViewRefresh).
        std::snprintf(out, cap, "-----");
    return true;
}

void BoneScaleReadoutTick(MMDApp* app) {
    char text[sizeof(g_scaleReadoutText)] = {};
    BoneScaleReadoutText(app, text, sizeof(text));
    if (std::strcmp(text, g_scaleReadoutText) == 0)
        return;
    std::snprintf(g_scaleReadoutText, sizeof(g_scaleReadoutText), "%s", text);
    HWND hwnd = app != nullptr ? app->FloatingWindow() : nullptr;
    if (hwnd == nullptr && app != nullptr)
        hwnd = reinterpret_cast<HWND>(app->state.hwnd);
    RECT client;
    if (hwnd == nullptr || !GetClientRect(hwnd, &client))
        return;
    // Only the status band the readout lives in; the erase flag is FALSE like
    // the other band invalidations (PostLanguageSweep2).
    RECT band = FramePanelLayout(app).topBand;
    InvalidateRect(hwnd, &band, FALSE);
}

// MMS addition: keep the panel's 缩放 X / Y / Z edits live.  PostViewRefresh
// only writes them on a view refresh, so a HUD or viewport scale drag, an undo
// or a frame step left stale numbers until the view changed.  Called once per
// frame: each edit is rewritten only when its formatted value differs, and
// never while it has the keyboard focus, so a value being typed is not
// overwritten under the caret.
void SyncBoneScaleEdits(MMDApp* app) {
    if (MMD_ORIGINAL_PROFILE) return;
    BoneScaleReadoutTick(app);
    if (app == nullptr || app->state.optflag[0] != 0)
        return;                             // camera panel: edits hidden
    HWND hwnd = app->FloatingWindow();
    if (hwnd == nullptr)
        hwnd = reinterpret_cast<HWND>(app->state.hwnd);
    if (hwnd == nullptr)
        return;
    unsigned char* model = app->SelectedModel();
    const bool hasBone = model != nullptr &&
        mdl::Mdl(model)->selectedBone >= 0 &&
        mdl::Mdl(model)->selectedBone <
            static_cast<std::int32_t>(mdl::Mdl(model)->boneCount);
    float values[3] = {1.0f, 1.0f, 1.0f};
    if (hasBone)
        SelectedBoneScaleReadout(app, values);
    const int ids[3] = {panel::kReadoutScaleXEdit, panel::kReadoutScaleYEdit,
                        panel::kReadoutScaleZEdit};
    const HWND focus = GetFocus();
    for (int i = 0; i < 3; ++i) {
        HWND edit = GetDlgItem(hwnd, ids[i]);
        if (edit == nullptr || edit == focus)
            continue;
        char text[32];
        if (hasBone)
            std::snprintf(text, sizeof(text), "%1.2f",
                          static_cast<double>(values[i]));
        else
            std::snprintf(text, sizeof(text), "-----");
        char shown[32] = {};
        GetWindowTextA(edit, shown, sizeof(shown));
        if (std::strcmp(shown, text) != 0)
            SetWindowTextA(edit, text);
    }
}

// MMS addition: the X / Y / Z buttons beside the 缩放 edits reset that one
// axis of the selected bone's scale back to 1.00, as one undoable bone edit.
void ResetSelectedBoneScaleAxis(MMDApp* app, int axis) {
#if !MMD_ORIGINAL_PROFILE
    if (plugins::TryResetBoundPoseAxis(app, plugins::NativePoseAxisKind::Scale, axis)) return;
#endif
    if (!bone_scale::Enabled(app) || axis < 0 || axis > 2)
        return;
    float values[3] = {1.0f, 1.0f, 1.0f};
    if (!SelectedBoneScaleReadout(app, values) ||
        std::fabs(values[axis] - 1.0f) < 1.0e-6f)
        return;
    PushBoneEditUndo(app);
    if (!SetSelectedBoneScaleAxis(app, axis, 1.0f))
        return;
    unsigned char* model = app->SelectedModel();
    auto* record = mdl::Mdl(model);
    if (record->bonePhysicsState)
        record->bonePhysicsState[record->selectedBone] = 1;
    app->SceneModified() = 1;
    app->RecordedFrameCountByte1() = 1;
    RefreshRequest(-1);
    SyncBoneScaleEdits(app);
}

namespace {

// Paints one "scale X .. Y .. Z .." group into the status band at (x, y),
// using the band's own label font and colours.  Hidden when the readout has no
// content, and not drawn when the free band to the left of the window-top
// button row cannot hold it - the controls never move for it.
void DrawBoneScaleReadout(MMDApp* app, HDC hdc, int x, int y, LONG right) {
    char values[sizeof(g_scaleReadoutText)] = {};
    if (!BoneScaleReadoutText(app, values, sizeof(values)))
        return;
    if (x + 200 > right - 450)
        return;
    if (app->EnglishUI() != 0)
        x += DrawUiGlyph(app, "scale", hdc, 14, x, y, 0, 0, 0, 1);
    else
        x += DrawGlyph(app, kJpScale, hdc, 14, x, y + 1, 0, 0, 0, 1);
    x += 6;
    DrawUiGlyph(app, values, hdc, 14, x, y, 0, 0, 0, 1);
}

}  // namespace

void HandlePaletteChanged(HDC hdc) {
    MMDApp* app = g_Block;  // original `this` (ecx)

    RECT rc;
    GetClientRect(reinterpret_cast<HWND>(app->state.hwnd),
                  &rc);                                     // 0x42CEC5 (this+0xA06B8)
    const std::int32_t bottom = rc.bottom;
    const auto layout = MainPanelLayout(app, bottom);
    const int sidebarBottom = layout.bottomTop - 1;
    const int side = app->SidebarWidth();
    const auto S = [&](int units) { return layout.Pixels(units); };
    const COLORREF colA = app->ThemeColor(UiThemeColor::ControlLight);
    const COLORREF colB = app->ThemeColor(UiThemeColor::ControlDark);
    const auto sidebarColor = [&](int y) {
        return ColorLerp(colA, colB, float(double(y - S(50)) / std::max(1, sidebarBottom - S(50))));
    };
    const int ruler = layout.RulerTop(), grid = layout.GridTop();
    const int gutterBottom = std::max(ruler, layout.FooterY(228));
    FillPanelBottom(hdc, S(3), S(50), side - S(3), ruler, colA, sidebarColor(ruler), 1);
    FillPanelBottom(hdc, S(3), ruler, S(6), gutterBottom, sidebarColor(ruler), sidebarColor(gutterBottom), 1);
    const int gutterEnd = std::max(grid, std::min(grid + S(16), layout.timelineBottom));
    FillPanelBottom(hdc, side - S(19), grid - 1, side - S(3), gutterEnd,
                    sidebarColor(grid - 1), sidebarColor(gutterEnd), 1);
    FillPanelBottom(hdc, S(3), std::max(S(50), layout.timelineBottom), side - S(3), sidebarBottom,
                    sidebarColor(std::max(S(50), layout.timelineBottom)), colB, 1);
    FillPanelBottom(hdc, S(3), S(3), side - S(3), S(47),
                    app->ThemeColorAt(4), app->ThemeColorAt(5), 1);
    PaintBottomPanelBackground(app, hdc, rc);
}

void PaintBottomPanelBackground(MMDApp* app, HDC hdc, const RECT& client) {
    const int bottom = client.bottom;
    const auto layout = BottomTemplate(app, bottom);
    const int bottomHeader = layout.Y(bottom - 157);
    const int bottomEdge = layout.Y(bottom - 3);
    RECT clip{};
    const int clipType = GetClipBox(hdc, &clip);
    if (clipType == ERROR || clipType == NULLREGION) return;
    int percent = 100;
    const auto spans = BottomPanelSpans(app, &percent);
    const auto FillPanelBottom = [&spans, &clip, percent](HDC dc, int l, int t, int r, int b, COLORREF c1, COLORREF c2, int edge) {
        if (b < clip.top || t >= clip.bottom) return;
        const int left = BottomPanelX(spans, l, percent), right = BottomPanelX(spans, r, percent);
        if (right < clip.left || left >= clip.right) return;
        mikumikustudio::FillPanelBottom(dc, left, t, right, b, c1, c2, edge);
    };

    // Fixed rows from the colour-table pairs (colour.txt).
    FillPanelBottom(hdc, 3, bottomHeader, 215, bottomEdge,
                    app->ThemeColorAt(6), app->ThemeColorAt(7),
                    1);                                          // 0x42D089
    FillPanelBottom(hdc, 218, bottomHeader, 358, bottomEdge,
                    app->ThemeColorAt(8), app->ThemeColorAt(9),
                    1);                                          // 0x42D0E1

    // Block A (0x42D0E6..0x42D179): IK/FK-style width switch; left 0x169,
    // top bottom-157, bottom bottom-3, colours 0xA0600/0xA0604.
    const COLORREF colC = app->ThemeColorAt(10);
    const COLORREF colD = app->ThemeColorAt(11);
    const BottomPanelLayout panelLayout = ComputeBottomPanelLayout(app);
    std::int32_t xOff = panelLayout.leading;
    if (app->state.optflag[0] != 0) {   // 0x2F8
        if (app->state.optflag[1] != 0) {  // 0x2F9
            FillPanelBottom(hdc, 0x169, bottomHeader, 0x202, bottomEdge, colC, colD, 1);  // 0x42D123
        } else {
            FillPanelBottom(hdc, 0x169, bottomHeader, 0x17B, bottomEdge, colC, colD, 1);  // 0x42D13B
        }
    } else if (app->state.optflag[4] != 0) {  // 0x2FC
        FillPanelBottom(hdc, 0x169, bottomHeader, 0x22E, bottomEdge, colC, colD, 1);  // 0x42D15C
    } else {
        FillPanelBottom(hdc, 0x169, bottomHeader, 0x17B, bottomEdge, colC, colD, 1);  // 0x42D174
    }

    // Block B (0x42D17E..0x42D218): colours 0xA0608/0xA060C; the offset is
    // subtracted from both edges and optionally grown.
    const COLORREF colE = app->ThemeColorAt(12);
    const COLORREF colF = app->ThemeColorAt(13);
    if (app->state.optflag[0] != 0) {   // 0x2F8
        if (app->state.optflag[2] != 0) {  // 0x2FA
            FillPanelBottom(hdc, 0x1F3 - xOff, bottomHeader, 0x2A7 - xOff,
                            bottomEdge, colE, colF, 1);        // 0x42D1C1
        } else {
            FillPanelBottom(hdc, 0x1F3 - xOff, bottomHeader, 0x205 - xOff,
                            bottomEdge, colE, colF, 1);        // 0x42D1D5
        }
    } else if (app->state.optflag[5] != 0) {  // 0x2FD
        FillPanelBottom(hdc, 0x254 - xOff, bottomHeader, 0x361 - xOff,
                        bottomEdge, colE, colF, 1);            // 0x42D1FF
    } else {
        FillPanelBottom(hdc, 0x254 - xOff, bottomHeader, 0x266 - xOff,
                        bottomEdge, colE, colF, 1);            // 0x42D213
    }
    xOff = panelLayout.afterLightOrFace;

    // Block C (0x42D21E..0x42D27E): only when 0x2F8 set; note the swapped
    // colour roles - colour 0xA0628, edge 0xA0604 (push ho=0xA0604 first).
    if (app->state.optflag[0] != 0) {   // 0x2F8
        const COLORREF colG = app->ThemeColorAt(20);
        if (app->state.optflag[6] != 0) {  // 0x2FE
            FillPanelBottom(hdc, 0x2AA - xOff, bottomHeader, 0x35B - xOff,
                            bottomEdge, colG, colD, 1);        // 0x42D265
        } else {
            FillPanelBottom(hdc, 0x2AA - xOff, bottomHeader, 0x2BC - xOff,
                            bottomEdge, colG, colD, 1);        // 0x42D279
        }
    }
    xOff = panelLayout.afterSelfShadow;

    // Block D (0x42D284..0x42D2E0): only when 0x2F8 set; colours 0xA0610/4.
    if (app->state.optflag[0] != 0) {   // 0x2F8
        const COLORREF colH = app->ThemeColorAt(14);
        const COLORREF colI = app->ThemeColorAt(15);
        if (app->state.optflag[3] != 0) {  // 0x2FB
            FillPanelBottom(hdc, 0x35E - xOff, bottomHeader, 0x422 - xOff,
                            bottomEdge, colH, colI, 1);        // 0x42D2C7
        } else {
            FillPanelBottom(hdc, 0x35E - xOff, bottomHeader, 0x370 - xOff,
                            bottomEdge, colH, colI, 1);        // 0x42D2DB
        }
    }

    // Final offset (0x42D2E6..0x42D2FA): 0x2F8 set -> -180, else +13.
    const std::int32_t xFinal = panelLayout.final;

    // Three closing rows (0x42D2FA..0x42D38E).
    FillPanelBottom(hdc, 0x371 - xFinal, bottomHeader, 0x412 - xFinal,
                    layout.ContentY(bottom - 0x52),
                    app->ThemeColorAt(16), app->ThemeColorAt(17),
                    1);                                          // 0x42D330
    FillPanelBottom(hdc, 0x371 - xFinal, layout.ContentY(bottom - 0x4F), 0x3F3 - xFinal,
                    bottomEdge,
                    app->ThemeColorAt(18), app->ThemeColorAt(19),
                    1);                                          // 0x42D361
    FillPanelBottom(hdc, 0x3F6 - xFinal, layout.ContentY(bottom - 0x4F), 0x412 - xFinal,
                    bottomEdge,
                    app->ThemeColorAt(18), app->ThemeColorAt(19),
                    1);                                          // 0x42D38E
}

// ===========================================================================
// VA 0x0042C140 - HandlePaletteChanged2  (original: sub_42C140, 0x6C5 bytes)
// ===========================================================================
// Status-overlay painter of the panel top: two gradient background bars, a
// 49x24 "playing" icon blit, and the model/camera status text lines.
// Called with the paint DC from HandleWindowPaint 0x47C0A0 (wm_paint.cpp
// @ 0x47E70A), MicWndProc 0x466A10 (@ 0x466B60 / 0x466CBC) and the WndProc
// 0x4C3A10 message-0x318 branch (wndproc.cpp, wParam as HDC).
//
// Sequence (exact, per the IDA disassembly):
//   Target window: this+0xA0D38 (kDwordA0D38, child panel) when non-zero,
//   else the main window this+0xA06B8 (kPtrHwnd); x base is 0 vs
//   this+0xA06C8 (kDwordSidebar) + 9; `right` = client.right both paths.
//   Two FillPanelBottom gradient bars:
//     (0x969696, 0x646464) at hideTop-25 .. hideTop-1     (0xA0D44 = kDwordHideTop)
//     (0x787878, 0xC8C8C8) at hideBottom .. hideBottom+32 (0xA0D4C = kDwordHideBottom)
//   CreateCompatibleDC(0) + SelectObject of the handle at this+0x2F4 (756,
//   the 49x24 playing-icon bitmap DC) + BitBlt SRCCOPY to
//   (right-58, hideTop-24, 49, 24) + DeleteDC - no restore of the selected
//   object, exactly like the original.
//   Text section (DrawGlyph 0x40E290) branches on this+0x2F8 (kByteOptflag0
//   camera/light vs bone mode) x EnglishUI (0xA0B4C):
//     - camera/light: slot array this+0x9DD70 [byte this+0x9E170] accessory
//       records; name at record+0x238, or the fixed "camera light accessary"
//       / "ｶﾒﾗ･照明･ｱｸｾｻﾘ" literal; formatted via "camera light accessary
//       / %s" / "ｶﾒﾗ･照明･ｱｸｾｻﾘ / %s" into the 256-byte Buffer.
//     - bone: model slots this+0x780 [byte this+0x910]; name at model+0x227A
//       (EN) / model+0x2248 (JP); bone index model+0x2D90; bone record =
//       (model+0x26BC) + sizeof(mikumikustudio::mdl::BoneRecord) *index, name at +0x14 (EN, "%s : %s").  The JP
//       branch keeps the original quirk: sprintf_s with a "%s" format and
//       TWO extra arguments, the second (bone record, no +0x14) is ignored.
//     - trace-mode line pair when
//       byte 0x9ED98 & (slotidx == this+0xA0430) & (this+0xA0430 >= 0);
//       second line (r=0xFF, g=b=0x32) at x+1/y-1 repeats the text.
//   "Playing" is drawn as "Playing"/"再生中" when (this+0x9ED90 == 0) &
//   (this+0x330 != 0) - the play flag test - else the name/accessory text.
//   Each mode ends with two 14px label lines; EN paths return early, JP
//   paths fall through to the shared "角度" (kS52C728) tail call.
//
// The "palette" name is historical (from the message-0x318 stub list); the
// function performs no palette API calls - it is the status overlay painter.
// Original returns the LONG of the last DrawGlyph; the declared signature
// is void.
//
// Reference: ../translated/MikuMikuDance/fcn_0042c140.cpp (superseded by
// the IDA disassembly for strings/coordinates)
// =========================================================================//
// The original text sequence includes bottom labels and early returns.
// Keep it together until title-only measurement can be verified.
static void PaintStatusText(MMDApp* app, HDC hdc, const ViewportPanelLayout& panels,
                            int xBase, int hideTop, int hideBottom,
                            StatusTitleGdiSession* checked = nullptr, bool* checkedOk = nullptr) {
    const wchar_t* saveStatus = background_save::StatusText(app);
    const bool savingText = saveStatus && *saveStatus;
    if (checked && savingText) { *checkedOk = false; return; }
    if (savingText) {
        const int saved = SaveDC(hdc);
        RECT band{panels.topBand.left, panels.topBand.top, panels.TitleRight(), panels.TitleBottom()};
        IntersectClipRect(hdc, band.left, band.top, band.right, band.bottom);
        const HBRUSH surface = CreateSolidBrush(RGB(255, 241, 241));
        FillRect(hdc, &band, surface); DeleteObject(surface);
        HFONT font = CreateFontW(-std::max(1, static_cast<int>(18 * panels.topScale + .5f)), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Microsoft YaHei UI");
        const auto old = SelectObject(hdc, font);
        SetTextColor(hdc, RGB(180, 20, 20)); SetBkMode(hdc, TRANSPARENT);
        band.left += std::max(1, static_cast<int>(10 * panels.topScale + .5f));
        band.right -= std::max(1, static_cast<int>(6 * panels.topScale + .5f));
        DrawTextW(hdc, saveStatus, -1, &band, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, old); DeleteObject(font); RestoreDC(hdc, saved);
    }
    const auto paint = [&](int size, int x, int y, auto draw) {
        if (checked && !*checkedOk) return 0;
        if (savingText && y < hideBottom) return 0;
        if (MMD_ORIGINAL_PROFILE) return draw(size, x, y);
        const bool bottom = y >= hideBottom;
        const float scale = bottom ? panels.bottomScale : panels.topScale;
        const int column = x - xBase;
        const int mappedX = bottom ? panels.BottomX(column)
            : xBase + ViewportPanelLayout::Pixels(column * scale);
        const int mappedY = bottom ? panels.BottomY(column, y - hideBottom)
            : panels.TitleBottom() + ViewportPanelLayout::Pixels((y - hideTop) * scale);
        const int saved = SaveDC(hdc);
        const RECT clip = bottom ? panels.bottomBand : RECT{
            panels.topBand.left, panels.topBand.top, panels.TitleRight(), panels.TitleBottom()};
        if (checked && !saved) { *checkedOk = false; return 0; }
        const int clipped = IntersectClipRect(hdc, clip.left, clip.top, clip.right, clip.bottom);
        if (checked && clipped == ERROR) { RestoreDC(hdc, saved); *checkedOk = false; return 0; }
        const int width = draw(std::max(1, ViewportPanelLayout::Pixels(size * scale)), mappedX, mappedY);
        const BOOL restored = RestoreDC(hdc, saved);
        if (checked && !restored) *checkedOk = false;
        return ViewportPanelLayout::Pixels(width / scale);
    };
    const auto drawUi = [&](MMDApp* owner, const char* text, HDC dc, int size, int x, int y,
            unsigned char r, unsigned char g, unsigned char b, int bold) {
        return paint(size, x, y, [&](int h, int px, int py) {
            if (checked) { int width=0; *checkedOk=DrawStatusUiGlyphChecked(*checked,owner,text,h,px,py,r,g,b,bold,width); return width; }
            return DrawUiGlyph(owner, text, dc, h, px, py, r, g, b, bold);
        });
    };
    const auto drawLegacy = [&](MMDApp* owner, const char* text, HDC dc, int size, int x, int y,
            unsigned char r, unsigned char g, unsigned char b, int bold) {
        return paint(size, x, y, [&](int h, int px, int py) {
            if (checked) { int width=0; *checkedOk=DrawStatusGlyphChecked(*checked,owner,text,h,px,py,r,g,b,bold,width); return width; }
            return DrawGlyph(owner, text, dc, h, px, py, r, g, b, bold);
        });
    };
    const auto drawWide = [&](const wchar_t* text, HDC dc, int size, int x, int y,
            unsigned char r, unsigned char g, unsigned char b, int bold,
            const wchar_t* face = L"Microsoft YaHei UI") {
        return paint(size, x, y, [&](int h, int px, int py) {
            if (checked) { int width=0; *checkedOk=DrawStatusWideGlyphChecked(*checked,text,h,px,py,r,g,b,bold,width,face); return width; }
            return DrawWideUiGlyph(text, dc, h, px, py, r, g, b, bold, face);
        });
    };

    // Play flag test, recomputed by the original in each branch
    // (this+0x330 loaded once at 0x42C233, this+0x9ED90 per branch; no side
    // effects intervene, so a single hoisted evaluation is taken).
    const bool playing =
        (app->FrameStepPlayback() == 0) & (app->PlaybackActive() != 0);


    if (app->state.optflag[0] != 0) {   // 0x2F8: camera/light mode
        if (app->EnglishUI() != 0) {                             // 0xA0B4C
            if (playing) {
                    drawUi(app, "Playing", hdc, 20, xBase + 10, hideTop - 25,
                          0xFF, 0xFF, 0xFF, 1);                  // 0x42C287
            } else {
                // Accessory slot array this+0x9DD70 [byte this+0x9E170].
                mdl::AccessoryRecord* acc =
                    app->AccessorySlot(app->SelectedAccessorySlot());
                const bool chinese = app->EnglishUI() == kUiChinese;
                std::wstring label = chinese ? L"相机 / 照明 / 配件"
                                              : L"Camera light accessory";
                if (acc != nullptr)
                    label += L" / " + text_encoding::Display(acc->name);
                // Keep the title's font stable when an accessory is selected.
                drawWide(label.c_str(), hdc, 20, xBase + 10,
                                hideTop - 25 - (chinese && acc == nullptr ? 3 : 0),
                                0xFF, 0xFF, 0xFF, chinese ? 1 : 0,
                                chinese ? L"Microsoft YaHei UI" : L"Arial");
            }
            drawUi(app, "camera", hdc, 14, xBase + 90, hideBottom + 10,
                      0, 0, 0, 1);                               // 0x42C33B
            drawUi(app, "angle", hdc, 14, xBase + 365, hideBottom + 10,
                      0, 0, 0, 1);                               // 0x42C361
            return;
        }
        if (playing) {
            drawLegacy(app, kS52C6DC, hdc, 16, xBase + 10, hideTop - 20,
                      0xFF, 0xFF, 0xFF, 1);                      // 0x42C39A
        } else {
            mdl::AccessoryRecord* acc =
                app->AccessorySlot(app->SelectedAccessorySlot());
            if (acc == nullptr) {
                drawLegacy(app, kS52C8B0, hdc, 16, xBase + 10, hideTop - 20,
                          0xFF, 0xFF, 0xFF, 1);                  // 0x42C429
            } else {
                const auto label = L"カメラ・照明・アクセサリ / " + text_encoding::Display(acc->name);
                drawWide(label.c_str(), hdc, 16, xBase + 10, hideTop - 20,
                                0xFF, 0xFF, 0xFF, 1);
            }
        }
        drawLegacy(app, kS52C8A4, hdc, 14, xBase + 72, hideBottom + 11,
                  0, 0, 0, 1);                                   // 0x42C44C
    } else {                                                     // bone mode
        if (app->EnglishUI() != 0) {
            int tail = xBase + 10;              // MMS: right edge of the name line
            if (playing) {
                tail += drawUi(app, "Playing", hdc, 20, xBase + 10, hideTop - 25,
                          0xFF, 0xFF, 0xFF, 1);                  // 0x42C492
            } else {
                unsigned char* model = app->SelectedModel();
                if (model != nullptr) {                          // 0x42C4AD
                    const mikumikustudio::mdl::ModelRecord& record =
                        *mikumikustudio::mdl::Mdl(model);
                    const std::int32_t boneIdx = record.selectedBone;
                    auto label = text_encoding::ModelLabel(record, UiUsesEnglishNames(app->EnglishUI()));
                    if (boneIdx >= 0) {
                        const auto& bone = record.boneTable[boneIdx];
                        label += L" : " + text_encoding::BoneLabel(bone, UiUsesEnglishNames(app->EnglishUI()));
                    }
                    const int width = drawWide(label.c_str(), hdc, 20,
                                                       xBase + 10, hideTop - 25,
                                                       0xFF, 0xFF, 0xFF, 1);
                    tail = xBase + 10 + width;  // MMS
                    // Trace-mode line pair: byte 0x9ED98 & (slot==0xA0430)
                    // & (0xA0430 >= 0).
                    const std::int32_t traceTarget =
                        app->CameraParentModel();
                    const bool traceActive =
                        (app->state.followCameraEnabled != 0) &
                        (app->SelectedModelSlot() == traceTarget) &
                        (traceTarget >= 0);                      // 0x42C553..0x42C572
                    if (traceActive) {
                        if (app->state.viewDirty != 0) {  // 0xA05D1
                            const int traceWidth = drawUi(app,
                                "camera bone trace mode (release trace button)",
                                hdc, 16, xBase + width + 30, hideTop - 25,
                                0xFF, 0xFF, 0xFF, 1);            // 0x42C5A7
                            drawUi(app, "camera bone trace mode (release trace button)",
                                      hdc, 16, xBase + width + 29, hideTop - 26,
                                      0xFF, 0x32, 0x32, 1);      // 0x42C5CE
                            tail = xBase + width + 30 + traceWidth;  // MMS
                        } else {
                            const int traceWidth = drawUi(app,
                                "camera bone trace mode",
                                hdc, 16, xBase + width + 30, hideTop - 25,
                                0xFF, 0xFF, 0xFF, 1);            // 0x42C5E9
                            tail = xBase + width + 30 + traceWidth;  // MMS
                        }
                    }
                }
            }
            drawUi(app, "bone place", hdc, 14, xBase + 66, hideBottom + 10,
                      0, 0, 0, 1);                               // 0x42C60C
            drawUi(app, "angle", hdc, 14, xBase + 365, hideBottom + 10,
                      0, 0, 0, 1);                               // 0x42C361
            return;
        }
        int tail = xBase + 10;                  // MMS: right edge of the name line
        if (playing) {
            tail += drawLegacy(app, kS52C6DC, hdc, 20, xBase + 10, hideTop - 25,
                      0xFF, 0xFF, 0xFF, 1);                      // 0x42C645
        } else {
            unsigned char* model = app->SelectedModel();
            if (model != nullptr) {                              // 0x42C660
                const mikumikustudio::mdl::ModelRecord& record =
                    *mikumikustudio::mdl::Mdl(model);
                const auto label = text_encoding::ModelLabel(record, UiUsesEnglishNames(app->EnglishUI()));
                const int width = drawWide(label.c_str(), hdc, 16,
                                                   xBase + 10, hideTop - 20,
                                                   0xFF, 0xFF, 0xFF, 1);
                tail = xBase + 10 + width;      // MMS
                const std::int32_t traceTarget = app->CameraParentModel();
                const bool traceActive =
                    (app->state.followCameraEnabled != 0) &
                    (app->SelectedModelSlot() == traceTarget) &
                    (traceTarget >= 0);                          // 0x42C6F9..0x42C721
                if (traceActive) {
                    if (app->state.viewDirty != 0) {  // 0xA05D1
                        const int traceWidth = drawLegacy(app, kS52C808, hdc, 16,
                                  xBase + width + 30,
                                  hideTop - 20, 0xFF, 0xFF, 0xFF, 1);  // 0x42C756
                        drawLegacy(app, kS52C808, hdc, 16, xBase + width + 29,
                                  hideTop - 21, 0xFF, 0x32, 0x32, 1);  // 0x42C77D
                        tail = xBase + width + 30 + traceWidth;  // MMS
                    } else {
                        const int traceWidth = drawLegacy(app, kS52C7EC, hdc, 16,
                                  xBase + width + 30,
                                  hideTop - 20, 0xFF, 0xFF, 0xFF, 1);  // 0x42C798
                        tail = xBase + width + 30 + traceWidth;  // MMS
                    }
                }
            }
        }
        drawLegacy(app, kS52C7E0, hdc, 14, xBase + 68, hideBottom + 11,
                  0, 0, 0, 1);                                   // 0x42C7BD
    }

    // Shared tail of the JP paths: "角度" at xBase+0x171 (369).
    drawLegacy(app, kS52C728, hdc, 14, xBase + 369, hideBottom + 11,
              0, 0, 0, 1);                                       // 0x42C7E3
}

bool PaintStatusTextChecked(StatusTitleGdiOwner& owner, OffscreenSurfaceHandle handle, MMDApp* app) noexcept {
    if (!app || app != g_Block) return false;
    return owner.WithDraw(handle, [&](StatusTitleGdiSession& session) {
        OffscreenSurfaceView view{};
        if (!session.Read(view) || !view.ready || !view.dc || !view.pixels) return false;
        HWND window = app->FloatingWindow();
        int xBase = 0;
        if (!window) { window = app->MainWindow(); xBase = app->SidebarWidth() + 9; }
        RECT bounds{};
        if (!window || !GetClientRect(window, &bounds) || bounds.left != 0 || bounds.top != 0 ||
            bounds.right <= 0 || bounds.bottom <= 0 || view.width < bounds.right || view.height < bounds.bottom)
            return false;
        const auto panels = FramePanelLayout(app);
        if (!MMD_ORIGINAL_PROFILE) xBase = panels.overlay.left;
        bool ok = true;
        PaintStatusText(app, reinterpret_cast<HDC>(view.dc), panels, xBase,
            panels.topBand.top + 25, panels.bottomBand.bottom - 32, &session, &ok);
        return ok && GdiFlush() != 0;
    });
}

// This experimental path is not used by the visible presenter. The same DIB
// contains both the native gradient and checked text; only RGB differences in
// the existing title clip are reported. It must not publish a plugin slot.
bool CaptureStatusTitleDifference(OffscreenGdiRuntimeCore<16>& runtime,
    OffscreenRuntimeHandle handle, MMDApp* app, RECT& changed) noexcept {
#if MMD_ORIGINAL_PROFILE
    (void)runtime; (void)handle; (void)app; (void)changed; return false;
#else
    if (!app || app != g_Block) return false;
    const wchar_t* status = background_save::StatusText(app);
    if (status && *status) return false;
    RECT candidate{};
    const bool ok = runtime.WithDraw(handle, [&](StatusTitleGdiSession& session) {
        OffscreenSurfaceView view{};
        if (!session.Read(view) || !view.ready || !view.dc || !view.pixels) return false;
        HWND window = app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow();
        RECT client{};
        if (!window || !GetClientRect(window, &client) || client.left || client.top ||
            client.right <= 0 || client.bottom <= 0 || view.width < client.right || view.height < client.bottom) return false;
        const auto panels = FramePanelLayout(app);
        if (!std::isfinite(panels.topScale) || panels.topScale <= 0.f) return false;
        const int right = panels.overlay.right, left = panels.overlay.left;
        const int bandTop = panels.topBand.top, bandBottom = panels.topBand.bottom;
        const int titleRight = panels.TitleRight();
        if (left < 0 || right > client.right || right <= left || bandTop < 0 ||
            bandBottom <= bandTop || bandBottom > client.bottom || titleRight <= left || titleRight > right ||
            panels.TitleBottom() > bandBottom) return false;
        const RECT title{left, bandTop, titleRight, panels.TitleBottom()};
        const int iconLeft = panels.TopX(58), iconTop = panels.TopY(-24);
        const int iconWidth = ViewportPanelLayout::Pixels(49 * panels.topScale);
        const int iconHeight = ViewportPanelLayout::Pixels(24 * panels.topScale);
        if (iconWidth <= 0 || iconHeight <= 0 || iconLeft < 0 || iconTop < 0 ||
            iconLeft > client.right - iconWidth || iconTop > client.bottom - iconHeight) return false;
        const RECT icon{iconLeft, iconTop, iconLeft + iconWidth, iconTop + iconHeight};
        RECT intersection{};
        if (IntersectRect(&intersection, &title, &icon)) return false;
        HDC dc = reinterpret_cast<HDC>(view.dc);
        // Coordinates below index DIB pixels directly. Reject altered mapping,
        // mirroring, or drawing modes instead of silently measuring another area.
        POINT windowOrigin{}, viewportOrigin{};
        if (GetMapMode(dc) != MM_TEXT || GetGraphicsMode(dc) != GM_COMPATIBLE ||
            GetLayout(dc) != 0 || GetROP2(dc) != R2_COPYPEN ||
            GetTextAlign(dc) != (TA_LEFT | TA_TOP | TA_NOUPDATECP) ||
            !GetWindowOrgEx(dc, &windowOrigin) || !GetViewportOrgEx(dc, &viewportOrigin) ||
            windowOrigin.x || windowOrigin.y || viewportOrigin.x || viewportOrigin.y) return false;
        const int saved = SaveDC(dc);
        if (!saved) return false;
        struct Restore { HDC dc; int state; ~Restore() { if (state) RestoreDC(dc,state); } } restore{dc,saved};
        // A bounding box alone is insufficient: a complex region can contain
        // holes. Require the effective clip to be exactly the complete title.
        // Query the effective region after setting it. The setter's successful
        // return value can differ from GetClipBox on a memory DIB DC.
        if (IntersectClipRect(dc, title.left, title.top, title.right, title.bottom) == ERROR) return false;
        RECT effectiveClip{};
        if (GetClipBox(dc, &effectiveClip) != SIMPLEREGION ||
            effectiveClip.left != title.left || effectiveClip.top != title.top ||
            effectiveClip.right != title.right || effectiveClip.bottom != title.bottom) return false;
        TRIVERTEX vertices[2]{};
        vertices[0].x=left; vertices[0].y=bandTop;
        vertices[0].Red=vertices[0].Green=vertices[0].Blue=0x9600;
        vertices[1].x=right; vertices[1].y=bandBottom-1;
        vertices[1].Red=vertices[1].Green=vertices[1].Blue=0x6400;
        GRADIENT_RECT mesh{0,1};
        if (!GradientFill(dc,vertices,2,&mesh,1,GRADIENT_FILL_RECT_V)) return false;
        const auto stock=GetStockObject(BLACK_PEN);
        if (!stock) return false;
        const auto previous=SelectObject(dc,stock);
        if (!previous || previous == HGDI_ERROR) return false;
        const bool border = MoveToEx(dc,left,bandTop,nullptr) && LineTo(dc,right,bandTop) &&
            LineTo(dc,right,bandBottom-1) && LineTo(dc,left,bandBottom-1) && LineTo(dc,left,bandTop);
        const auto restoredPen=SelectObject(dc,previous);
        if (!border || !restoredPen || restoredPen == HGDI_ERROR || !GdiFlush()) return false;
        const auto width = static_cast<std::size_t>(title.right-title.left);
        const auto height = static_cast<std::size_t>(title.bottom-title.top);
        std::vector<std::uint32_t> before(width*height);
        const auto* pixels=static_cast<const std::uint32_t*>(view.pixels);
        for (std::size_t y=0;y<height;++y)
            for (std::size_t x=0;x<width;++x)
                before[y*width+x]=pixels[(static_cast<std::size_t>(title.top)+y)*view.width+title.left+x]&0x00ffffffu;
        bool painted=true;
        PaintStatusText(app,dc,panels,left,bandTop+25,panels.bottomBand.bottom-32,&session,&painted);
        if (!painted || !GdiFlush()) return false;
        bool found=false;
        for (std::size_t y=0;y<height;++y) for (std::size_t x=0;x<width;++x) {
            const auto value=pixels[(static_cast<std::size_t>(title.top)+y)*view.width+title.left+x]&0x00ffffffu;
            if (value==before[y*width+x]) continue;
            const LONG px=static_cast<LONG>(title.left+x), py=static_cast<LONG>(title.top+y);
            if (!found) { candidate={px,py,px+1,py+1}; found=true; }
            else { candidate.left=(std::min)(candidate.left,px);candidate.top=(std::min)(candidate.top,py);
                   candidate.right=(std::max)(candidate.right,px+1);candidate.bottom=(std::max)(candidate.bottom,py+1); }
        }
        if (!RestoreDC(dc,saved)) return false;
        restore.state=0;
        return true;
    });
    if (ok) changed=candidate;
    return ok;
#endif
}

void HandlePaletteChanged2(HDC hdc) {
    MMDApp* app = g_Block;  // original `this` (ecx)

    // Target window and x base (0x42C160..0x42C1A6).
    RECT rc;
    std::int32_t xBase;
    HWND hwnd = app->FloatingWindow();
    if (hwnd != nullptr) {
        GetClientRect(hwnd, &rc);
        xBase = 0;
    } else {
        hwnd = reinterpret_cast<HWND>(app->state.hwnd);  // 0xA06B8
        GetClientRect(hwnd, &rc);
        xBase = app->SidebarWidth() + 9;
    }
    const auto panels = FramePanelLayout(app);
    if (!MMD_ORIGINAL_PROFILE) xBase = panels.overlay.left;
    const LONG right = MMD_ORIGINAL_PROFILE ? rc.right : panels.overlay.right;   // ebp
    const LONG blitX = rc.right;   // v26 (BitBlt x base)

    // Existing text positions remain logical single-row coordinates. The
    // shared layout maps each group into its final row below.
    const std::int32_t hideTop = panels.topBand.top + 25;
    const std::int32_t hideBottom = panels.bottomBand.bottom - 32;

    FillPanelBottom(hdc, xBase, panels.topBand.top, right, panels.topBand.bottom - 1,
                    0x969696u, 0x646464u, 1);                    // 0x42C1C7
    FillPanelBottom(hdc, xBase, panels.bottomBand.top, right, panels.bottomBand.bottom,
                    0x787878u, 0xC8C8C8u, 1);                    // 0x42C1E8

    // 49x24 playing-icon blit from the offscreen DC handle at this+0x2F4.
    HDC compat = CreateCompatibleDC(nullptr);                    // 0x42C1EF
    SelectObject(compat,
                 reinterpret_cast<HGDIOBJ>(app->state.bmpRes119));  // this+0x2F4 (756)
    StretchBlt(hdc, MMD_ORIGINAL_PROFILE ? blitX - 58 : panels.TopX(58),
           panels.TopY(-24), ViewportPanelLayout::Pixels(49 * panels.topScale),
           ViewportPanelLayout::Pixels(24 * panels.topScale), compat, 0, 0, 49, 24,
           0xCC0020u);                                           // 0x42C226 (SRCCOPY)
    DeleteDC(compat);                                            // 0x42C22D

    PaintStatusText(app, hdc, panels, xBase, hideTop, hideBottom);
}

}  // namespace mikumikustudio
