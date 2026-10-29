#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_curve.hpp"
// ===========================================================================
// VA 0x00430510 with helpers 0x00415E90 / 0x00416090
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/interpolation_link.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {

void ClearGlobalFrameSelection(MMDApp* app) {
    if (!app) return;
    const auto clear = [](auto* keys) {
        if (!keys) return;
        for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(keys)); i < poolCapacity; ++i)
            keys[i].selected = 0;
    };
    clear(app->CameraKeys());
    clear(app->LightKeys());
    clear(app->ShadowKeys());
    clear(app->GravityKeys());
    for (int slot = 0; slot < 255; ++slot)
        clear(app->AccessoryKeys(slot));
}

// x64 0x7FF7CB482BB0 - the selection re-evaluation body, ported in
// misc_dialogs.cpp as CurvePanelRepaint (same original as the x86
// 0x00430510 entry this file used to carry inline).
void CurvePanelRepaint(MMDApp* app);

namespace {

struct Curve {
    int x1;
    int y1;
    int x2;
    int y2;
};

void DrawCurve(MMDApp* app, const Curve& curve) {  // 0x415E90
    HDC dc = app->CurveDC();
    HPEN pen = CreatePen(PS_SOLID, 1, 0xFF0000u);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    int oldX = 0;
    int oldY = 126;
    for (int step = 1; step < 100; ++step) {
        const float t = static_cast<float>(step) / 100.0f;
        const float ax = static_cast<float>(curve.x1) * t;
        const float ay = static_cast<float>(curve.y1) * t;
        const float bx = static_cast<float>(curve.x1) +
                         static_cast<float>(curve.x2 - curve.x1) * t;
        const float by = static_cast<float>(curve.y1) +
                         static_cast<float>(curve.y2 - curve.y1) * t;
        const float cx = static_cast<float>(curve.x2) +
                         static_cast<float>(128 - curve.x2) * t;
        const float cy = static_cast<float>(curve.y2) +
                         static_cast<float>(128 - curve.y2) * t;
        const float dx = ax + (bx - ax) * t;
        const float dy = ay + (by - ay) * t;
        const float ex = bx + (cx - bx) * t;
        const float ey = by + (cy - by) * t;
        const int x = static_cast<int>(dx + (ex - dx) * t);
        const int y = 126 - static_cast<int>(dy + (ey - dy) * t);
        MoveToEx(dc, oldX, oldY, nullptr);
        LineTo(dc, x, y);
        oldX = x;
        oldY = y;
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawControlPoints(MMDApp* app, const Curve& curve) {  // 0x416090
    HDC dc = app->CurveDC();
    HPEN pen = CreatePen(PS_SOLID, 2, 0x000000FFu);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    app->state.interpCurveControlCache[0] = static_cast<std::uint8_t>(curve.x1);
    app->state.interpCurveControlCache[1] =
        static_cast<std::uint8_t>(127 - curve.y1);
    app->state.interpCurveControlCache[2] = static_cast<std::uint8_t>(curve.x2);
    app->state.interpCurveControlCache[3] =
        static_cast<std::uint8_t>(127 - curve.y2);

    MoveToEx(dc, curve.x1 - 3, 124 - curve.y1, nullptr);
    LineTo(dc, curve.x1 + 3, 130 - curve.y1);
    MoveToEx(dc, curve.x1 + 3, 124 - curve.y1, nullptr);
    LineTo(dc, curve.x1 - 3, 130 - curve.y1);
    MoveToEx(dc, curve.x2 - 3, 124 - curve.y2, nullptr);
    LineTo(dc, curve.x2 + 3, 130 - curve.y2);
    MoveToEx(dc, curve.x2 + 3, 124 - curve.y2, nullptr);
    LineTo(dc, curve.x2 - 3, 130 - curve.y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    HPEN guide = CreatePen(PS_SOLID, 0, 0);
    SelectObject(dc, guide);
    MoveToEx(dc, 0, 127, nullptr);
    LineTo(dc, curve.x1, 127 - curve.y1);
    MoveToEx(dc, 127, 0, nullptr);
    LineTo(dc, curve.x2, 127 - curve.y2);
    SelectObject(dc, oldPen);
    DeleteObject(guide);
}

}  // namespace

// ---------------------------------------------------------------------------
// VA 0x00416280 - DragInterpolationControlPoint(app) (was Sub416280):
//   interpolation-curve panel drag handler
// (mouse move while dragging a control point).  Repaints the 128x128 curve
// cache white, clamps the pointer to the panel (x = mouse X-8, y =
// client.bottom - mouse Y - 8, both clamped 0..127), then rewrites the
// dragged control point on every selected record and redraws the curve
// from the last record touched:
//   byte 0x2F8 != 0  -> camera/global records app+0x374 (84B, 10000,
//                       gate +72): mode byte 0x9D939 == 1 edits the x1/y1
//                       groups (+40/+46, all 6 channels when the channel
//                       combo 433 selection >= 6, else only the selected
//                       channel), == 2 edits x2/y2 (+52/+58); curve drawn
//                       with the opposite group's bytes.
//   else             -> selected model's BoneKey records (kBoneKeyCapacity entries):
//                       mode 1 edits interpolation x1/y1, mode 2 edits
//                       x2/y2 (four channels).
// The original passes uninitialized stack values to the draw calls when no
// record matched; the port passes a zeroed curve (deterministic stand-in).
// Ends with DrawCurve + DrawControlPoints (0x415E90/0x416090) and the
// 8/bottom-135/137/bottom-6 invalidate rect.
// ---------------------------------------------------------------------------
void DragInterpolationControlPoint(MMDApp* app) {  // was Sub416280, VA 0x00416280
#if !MMD_ORIGINAL_PROFILE
    if(app->state.optflag[0]==0 && app->SelectedModel() && bone_curve::Has(app->SelectedModel(),mdl::Mdl(app->SelectedModel())->selectedBone))return;
#endif
    const HWND window = static_cast<HWND>(app->Hwnd());
    HDC dc = app->CurveDC();
    HPEN pen = CreatePen(PS_SOLID, 1, 0x00FFFFFFu);              // 0x41629D
    HBRUSH brush = CreateSolidBrush(0x00FFFFFFu);                // 0x4162AB
    // The pen and brush remain selected in the curve DC while
    // DeleteObject runs, so the calls fail and both handles stay allocated; the
    // handles SelectObject returned for the DC's previous pen and brush are
    // dropped without being restored.  This handler runs on every mouse move
    // during a control-point drag, so the GDI handles leak for the whole
    // duration of the drag.  Both returned handles are kept and restored before
    // the temporaries are deleted, in the order CurvePanelRepaint in
    // misc_dialogs.cpp uses.  The x64 original has the same defect (CreatePen
    // 0x140063A0B, CreateSolidBrush 0x140063A19, SelectObject 0x140063A2C and
    // 0x140063A3C with the results discarded, Rectangle 0x140063A5C,
    // DeleteObject 0x140063A65 and 0x140063A6E with no restore in between), so
    // the port deviates deliberately; the painted pixels are unchanged.
    // Defect B1-04.
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    Rectangle(dc, 0, 0, 128, 128);                               // 0x4162D6
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT client{};
    GetClientRect(window, &client);                              // 0x4162F4
    int x = app->MouseX() - 8;                                   // 0x4162FD
    if (x < 0) x = 0; else if (x > 127) x = 127;
    int y = client.bottom - app->MouseY() - 8;                   // 0x416317
    if (y < 0) y = 0; else if (y > 127) y = 127;

    Curve drawn{};  // (x1, y1, x2, y2) fed to both draw helpers
    const int selected = static_cast<int>(SendMessageA(
        GetDlgItem(window, panel::kInterpCurveCombo), CB_GETCURSEL, 0, 0));
    const bool link = IsDlgButtonChecked(window,panel::kPhysicsFrameCheckbox)==BST_CHECKED;
    const auto adjacentIndex = [&](const auto* records,std::size_t capacity,std::size_t index,int mode) -> int {
        const auto next=mode==1?records[index].previous:records[index].next;
        if(next>=capacity||next==index)return -1;
        if(mode==1)return records[next].frame>0&&records[next].frame<records[index].frame?int(next):-1;
        return records[next].frame>records[index].frame?int(next):-1;
    };

    if (app->state.optflag[0] != 0) {   // 0x416337
        auto* records = app->CameraKeys();
        if (records != nullptr && selected >= 0) {
            for (std::size_t index = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(records)); index < poolCapacity; ++index) {
                auto& record = records[index];
                if (record.selected == 0)
                    continue;
                const auto mode = app->state.pendingTimelineSelectionRow;
                if (mode != 1 && mode != 2)
                    continue;
                const int channel = selected >= 6 ? 0 : selected;
                const int endChannel = selected >= 6 ? 6 : channel + 1;
                const int group = mode == 1 ? 0 : 2;
                for (int c = channel; c < endChannel; ++c) {
                    record.interpolation[group][c] = static_cast<std::uint8_t>(x);
                    record.interpolation[group + 1][c] = static_cast<std::uint8_t>(y);
                    const int adjacent=link?adjacentIndex(records,mdl::KeyPoolCapacity(records),index,mode):-1;
                    if(adjacent>=0){
                        auto& neighbor=records[adjacent];std::array<std::uint8_t,4> curve{},edited{};
                        for(int k=0;k<4;++k){curve[k]=neighbor.interpolation[k][c];edited[k]=record.interpolation[k][c];}
                        if(interpolation_link::Mirror(curve,edited,mode==1))for(int k=0;k<4;++k)neighbor.interpolation[k][c]=curve[k];
                    }
                }
                drawn = {record.interpolation[0][channel],
                         record.interpolation[1][channel],
                         record.interpolation[2][channel],
                         record.interpolation[3][channel]};
            }
        }
    } else {                                                     // 0x41656B
        unsigned char* model = app->SelectedModel();
        mdl::BoneKey* records = model != nullptr
            ? mdl::BoneKeys(model) : nullptr;
        if (records != nullptr && selected >= 0) {
            const int boneKeyCapacity =
                static_cast<int>(mdl::BoneKeyCapacity(*mdl::Mdl(model)));
            // MMS addition: channel layout is 0..3 the four legacy channels, 4
            // the bone scale channel and 5 = all (the legacy channels AND the
            // scale curve).  The scale curve lives in bone_scale's side table,
            // keyed by the key slot, so it is written through that API.
            const bool allChannels = selected >= bone_scale::kInterpBoneAll;
            const bool scaleOnly = !MMD_ORIGINAL_PROFILE && selected == bone_scale::kInterpScaleChannel;
            for (int index = 0; index < boneKeyCapacity; ++index) {
                mdl::BoneKey& record = records[index];
                if (record.allocated == 0)
                    continue;
                const std::uint8_t mode =
                    app->state.pendingTimelineSelectionRow;
                if (mode != 1 && mode != 2)
                    continue;
                // mode 1 edits the first control point (x1/y1 = interpolation
                // groups 0/1), mode 2 the second (x2/y2 = groups 2/3).
                const int group = mode == 1 ? 0 : 2;
                if (!scaleOnly) {
                    const int first = allChannels ? 0 : selected;
                    const int end = allChannels ? 4 : selected + 1;
                    for (int c = first; c < end; ++c) {
                        record.interpolation[group * 4 + c] =
                            static_cast<std::uint8_t>(x);
                        record.interpolation[(group + 1) * 4 + c] =
                            static_cast<std::uint8_t>(y);
                        const int adjacent=link?adjacentIndex(records,boneKeyCapacity,index,mode):-1;
                        if(adjacent>=0){
                            auto& neighbor=records[adjacent];std::array<std::uint8_t,4> curve{},edited{};
                            for(int k=0;k<4;++k){curve[k]=neighbor.interpolation[k*4+c];edited[k]=record.interpolation[k*4+c];}
                            if(interpolation_link::Mirror(curve,edited,mode==1))for(int k=0;k<4;++k)neighbor.interpolation[k*4+c]=curve[k];
                        }
                    }
                }
                const int channel = allChannels ? 0 : selected;
                if (!MMD_ORIGINAL_PROFILE && (scaleOnly || allChannels)) {
                    bone_scale::Curve curve = bone_scale::KeyCurve(model, index);
                    curve[group] = static_cast<std::uint8_t>(x);
                    curve[group + 1] = static_cast<std::uint8_t>(y);
                    bone_scale::SetKeyCurve(model, index, curve);
                    const int adjacent=link?adjacentIndex(records,boneKeyCapacity,index,mode):-1;
                    if(adjacent>=0){
                        auto neighbor=bone_scale::KeyCurve(model,adjacent);
                        if(interpolation_link::Mirror(neighbor,curve,mode==1))bone_scale::SetKeyCurve(model,adjacent,neighbor);
                    }
                }
                if (scaleOnly) {                             // 0x4165C7 / 0x416725
                    const bone_scale::Curve curve =
                        bone_scale::KeyCurve(model, index);
                    drawn = {static_cast<std::int8_t>(curve[0]),
                             static_cast<std::int8_t>(curve[1]),
                             static_cast<std::int8_t>(curve[2]),
                             static_cast<std::int8_t>(curve[3])};
                } else if (mode == 1) {
                    drawn = {x, y,
                             static_cast<std::int8_t>(
                                 record.interpolation[8 + channel]),
                             static_cast<std::int8_t>(
                                 record.interpolation[12 + channel])};
                } else {
                    drawn = {
                        static_cast<std::int8_t>(
                            record.interpolation[channel]),
                        static_cast<std::int8_t>(
                            record.interpolation[4 + channel]), x, y};
                }
            }
        }
    }

    DrawCurve(app, drawn);                                       // 0x41684F
    DrawControlPoints(app, drawn);                               // 0x41686C
    GetClientRect(window, &client);                              // 0x41687D
    RECT dirty{8, client.bottom - 135, 137, client.bottom - 6};
    InvalidateRect(window, &dirty, FALSE);                       // 0x4168BC
}

// The x86 original (0x430510) compared every channel curve against a
// single baseline, so button 430 went grey whenever the X/Y interpolation
// curves differed from each other.  The x64 E build (0x7FF7CB482BB0,
// CurvePanelRepaint) seeds per-channel baselines and keeps 430 enabled
// while each channel stays self-consistent - that is the behavioral
// baseline, so every caller funnels through the x64 body.
void SelectionReeval(MMDApp* app) {  // 0x430510 / x64 0x7FF7CB482BB0
    CurvePanelRepaint(app);
    negative_frames::RefreshControls(app);
}

}  // namespace mikumikustudio
