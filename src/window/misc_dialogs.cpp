#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/bone_curve.hpp"
// ===========================================================================
// Misc dialog bodies: frame-control apply, reorder fill, accessory-edit init, physics ON/OFF apply
// ===========================================================================
// Split out of src/window/command_view_menu.cpp (the menu-251..302 command
// family) so the dialog's helper bodies can be ported independently.
// Every function keeps its original x86 VA; behaviour notes live in the
// per-function comments.  The x64 confirmation addresses (MikuMikuDance
// v932x64) are recorded per function:
//   ApplyBoneFrameScaleAdd -> 9.26 x64 RVA 0x9C9F0 (selected bone-key transform)
//   ApplyMorphScaleAdd  ->  sub_7FF7CB4BD120   (called by dialog proc sub_7FF7CB478B40)
//   InitModelOrderDialog  ->  sub_7FF7CB4A98C0   (array builder; the listbox fill + the
//                                       allocation live in dialog proc
//                                       sub_7FF7CB477D70's WM_INITDIALOG -
//                                       the x86 0x41E810 rolls all three
//                                       together because its caller
//                                       ModelCalculateOrderDlgProc only passes (count, hDlg))
//   InitGravityDialog  ->  sub_7FF7CB4B5F00   (gravity cluster echo into 709..713)
//   ApplyPhysicsOnOff  ->  sub_7FF7CB4BED40   (called by dialog proc sub_7FF7CB4BEC00)
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_scale.hpp"
#include <Windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cwchar>
#include <cerrno>
#include <cctype>
#include <new>
#include <unordered_set>

#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/global_key_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {

// ---- interpolation-curve panel helpers ------------------------------------
// The two GDI helpers are twins of the ones ported in
// src/window/ui_selection_reeval.cpp (0x415E90 / 0x416090); they are
// duplicated here because that TU keeps them file-local.

namespace {

constexpr int kCurvePanelSize = 128;   // 128x128 interpolation mini panel

// One interpolation curve: control-point pair as signed panel bytes.
struct Curve {
    int x1;
    int y1;
    int x2;
    int y2;
};

bool operator!=(const Curve& left, const Curve& right) {
    return left.x1 != right.x1 || left.y1 != right.y1 ||
           left.x2 != right.x2 || left.y2 != right.y2;
}

// x64 sub_7FF7CB483530 (x86 twin 0x415E90): draw one interpolation curve
// into hdcInterpCurve - red 1px pen, cubic Bezier between (0,0) and
// (128,128) with the control pair (x1,y1)/(x2,y2), sampled at
// t = 1/100 .. 99/100 (y plotted as 126 - y).
void DrawCurveSegment(MMDApp* app, const Curve& curve) {
    HDC dc = app->CurveDC();
    HPEN pen = CreatePen(PS_SOLID, 1, 0xFF0000u);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    POINT points[100]{};
    points[0] = {0, 126};
    for (int step = 1; step < 100; ++step) {
        const float t = static_cast<float>(step) / 100.0f;
        const float ax = static_cast<float>(curve.x1) * t;
        const float ay = static_cast<float>(curve.y1) * t;
        const float bx = static_cast<float>(curve.x1) +
                         static_cast<float>(curve.x2 - curve.x1) * t;
        const float by = static_cast<float>(curve.y1) +
                         static_cast<float>(curve.y2 - curve.y1) * t;
        const float cx = static_cast<float>(curve.x2) +
                         static_cast<float>(kCurvePanelSize - curve.x2) * t;
        const float cy = static_cast<float>(curve.y2) +
                         static_cast<float>(kCurvePanelSize - curve.y2) * t;
        const float dx = ax + (bx - ax) * t;
        const float dy = ay + (by - ay) * t;
        const float ex = bx + (cx - bx) * t;
        const float ey = by + (cy - by) * t;
        const int x = static_cast<int>(dx + (ex - dx) * t);
        const int y = 126 - static_cast<int>(dy + (ey - dy) * t);
        points[step] = {x, y};
    }
    Polyline(dc, points, 100);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// x64 sub_7FF7CB483790 (x86 twin 0x416090): highlight the current curve -
// 2px blue X marks on both control points (y as 127-complement, cached
// into interpCurveControlCache for the drag editor) plus black guide
// lines from the panel corners.
void DrawCurveHighlight(MMDApp* app, const Curve& curve) {
    HDC dc = app->CurveDC();
    HPEN pen = CreatePen(PS_SOLID, 2, 0x000000FFu);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    app->state.interpCurveControlCache[0] =
        static_cast<std::uint8_t>(curve.x1);
    app->state.interpCurveControlCache[1] =
        static_cast<std::uint8_t>(127 - curve.y1);
    app->state.interpCurveControlCache[2] =
        static_cast<std::uint8_t>(curve.x2);
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

// Curve-gate byte: camera records mark timeline-selected keys at +72
// (CameraKey::selected), bone keys mark allocated records at +56.
bool CurveKeyActive(const mdl::CameraKey& key) { return key.selected != 0; }
bool CurveKeyActive(const mdl::BoneKey& key) { return key.allocated != 0; }

// Channel curve of one record.  Camera: interpolation[4][6], channel index
// inside each group; bone: interpolation[16], groups x1/y1/x2/y2 at 0/4/8/12.
Curve ReadCurve(const mdl::CameraKey& key, int channel) {
    const auto value = [&key, channel](int group) {
        return static_cast<int>(static_cast<std::int8_t>(
            key.interpolation[group][channel]));
    };
    return {value(0), value(1), value(2), value(3)};
}
Curve ReadCurve(const mdl::BoneKey& key, int channel) {
    const auto value = [&key, channel](int base) {
        return static_cast<int>(static_cast<std::int8_t>(
            key.interpolation[base + channel]));
    };
    return {value(0), value(4), value(8), value(12)};
}

// MMS addition: the bone scale channel lives in bone_scale's side table, keyed
// by the key slot, so it is read from there instead of the key's own
// interpolation bytes.  Same signed byte view as ReadCurve.
Curve ReadScaleCurve(MMDApp* app, std::size_t slot) {
    const bone_scale::Curve curve = bone_scale::KeyCurve(
        app->SelectedModel(), static_cast<int>(slot));
    return {static_cast<std::int8_t>(curve[0]), static_cast<std::int8_t>(curve[1]),
            static_cast<std::int8_t>(curve[2]), static_cast<std::int8_t>(curve[3])};
}

// One track scan of the x64 repaint loop (camera and bone records share
// the shape).  First active record draws its curve and seeds the baseline
// plus (in all-channel mode) the per-channel snapshots; every later record
// redraws only differing curves, clearing interpCurveUniformFound on any
// cross-record difference and perChannelUniform on any per-channel one.
// In single-channel mode the original forces perChannelUniform false right
// after the first record (kept as-is: button 430 then keys off
// interpCurveUniformFound alone).
template <typename Key, typename CurveAt>
void ScanCurveTrack(MMDApp* app, const Key* keys, std::size_t capacity,
                    int channelCount, int channel, Curve& baseline,
                    Curve (&firstChannels)[6], bool& found,
                    bool& perChannelUniform, CurveAt curveAt) {
    if (keys == nullptr)
        return;
    // Imported/baked motion can select hundreds of thousands of keys sharing
    // just a few curves. Drawing the same opaque line again has no visual
    // effect, but the old per-key GDI loop blocked input for large selections.
    std::unordered_set<std::uint32_t> drawn;
    const auto drawUnique = [&](const Curve& curve) {
        const std::uint32_t id =
            std::uint32_t(static_cast<std::uint8_t>(curve.x1)) |
            (std::uint32_t(static_cast<std::uint8_t>(curve.y1)) << 8) |
            (std::uint32_t(static_cast<std::uint8_t>(curve.x2)) << 16) |
            (std::uint32_t(static_cast<std::uint8_t>(curve.y2)) << 24);
        if (drawn.insert(id).second) DrawCurveSegment(app, curve);
    };
    for (std::size_t i = 0; i < capacity; ++i) {
        const Key& key = keys[i];
        if (!CurveKeyActive(key))
            continue;
        if (!found) {
            if (channel >= channelCount) {
                drawUnique(curveAt(key, 0, i));
                baseline = curveAt(key, 0, i);
                firstChannels[0] = baseline;
                app->state.interpCurveUniformFound = 1;
                for (int c = 1; c < channelCount; ++c) {
                    firstChannels[c] = curveAt(key, c, i);
                    if (curveAt(key, c, i) != baseline) {
                        drawUnique(curveAt(key, c, i));
                        app->state.interpCurveUniformFound = 0;
                    }
                }
            } else {
                drawUnique(curveAt(key, channel, i));
                baseline = curveAt(key, channel, i);
                perChannelUniform = false;
                app->state.interpCurveUniformFound = 1;
            }
            found = true;
        } else if (channel >= channelCount) {
            for (int c = 0; c < channelCount; ++c) {
                if (curveAt(key, c, i) != baseline) {
                    drawUnique(curveAt(key, c, i));
                    app->state.interpCurveUniformFound = 0;
                }
            }
            for (int c = 0; c < channelCount; ++c) {
                if (curveAt(key, c, i) != firstChannels[c])
                    perChannelUniform = false;
            }
        } else {
            if (curveAt(key, channel, i) != baseline) {
                drawUnique(curveAt(key, channel, i));
                app->state.interpCurveUniformFound = 0;
            }
        }
    }
}

}  // namespace

// ===========================================================================
// x64 0x7FF7CB482BB0 - CurvePanelRepaint(app)
// ===========================================================================
// Repaints the 128x128 interpolation-curve mini panel (white pen/brush
// wash into hdcInterpCurve).  Channel = CB_GETCURSEL of combo 433; the
// camera mode (optflag[0]) scans the global camera track (84-byte keys,
// gate = selected +72, six interpolation channels in interpolation[4][6]),
// model mode the selected model's bone-key table (60-byte records, gate =
// allocated +56, four channels in interpolation[16]).  When the combo
// selects the "all" entry (camera >= 6 / bone >= 4) the first active
// record's channel-0 curve seeds the baseline and each further differing
// curve (any channel, any record) is redrawn over it, else only the
// selected channel is compared.  All curves identical -> the current
// curve is highlighted (sub_7FF7CB483790) and 0x9DA04 stores that fact;
// button 430 follows that flag or, in all-channel mode only, the weaker
// per-channel consistency; 432 follows "any key found"; 431 additionally
// requires the signed preset-A lead byte (0x9DA0A) to be >= 0.  Tail:
// InvalidateRect{8, bottom-135, 137, bottom-6} on the main window.
// Port detail: the x64 walks 0x927C0 60-byte bone slots (the E build's
// larger table); the port scans the pool's runtime capacity like the sibling
// ports
// (0x416280 / 0x430510 in ui_selection_reeval.cpp).
// =========================================================================//
void CurvePanelRepaint(MMDApp* app) {  // x64 0x7FF7CB482BB0
    constexpr int kInterpCurveCombo = 433;   // channel selector combo
    constexpr int kCurveUniformButton = 430; // enabled when curves agree
    constexpr int kCurvePresetButton = 431;  // also gated on preset-A byte
    constexpr int kCurveFoundButton = 432;   // enabled when a key exists
    constexpr int kCurveDirtyLeft = 8;
    constexpr int kCurveDirtyRight = 137;
    constexpr int kCurveDirtyTopMargin = 135;
    constexpr int kCurveDirtyBottomMargin = 6;

    app->state.interpCurveUniformFound = 0;
    HDC dc = app->CurveDC();
    HPEN whitePen = CreatePen(PS_SOLID, 1, 0x00FFFFFFu);
    HBRUSH whiteBrush = CreateSolidBrush(0x00FFFFFFu);
    HGDIOBJ oldPen = SelectObject(dc, whitePen);
    HGDIOBJ oldBrush = SelectObject(dc, whiteBrush);
    Rectangle(dc, 0, 0, kCurvePanelSize, kCurvePanelSize);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(whitePen);
    DeleteObject(whiteBrush);

    const HWND window = static_cast<HWND>(app->Hwnd());
    // MMS addition: the selector layout is owned by this module (control 433 now
    // carries the bone scale channel in the model panel).  Another code path can
    // truncate the list, so the layout is repaired before the cursor is read.
    EnsureInterpChannelCombo(app);
    int channel = static_cast<int>(SendMessageA(
        GetDlgItem(window, kInterpCurveCombo), CB_GETCURSEL, 0, 0));

    bool found = false;             // x64 v83: any active record seen
    bool perChannelUniform = true;  // x64 v84: per-channel consistency
#if !MMD_ORIGINAL_PROFILE
    if(app->state.optflag[0]==0 && bone_curve::DrawPreview(*app)) {
        for(int id : {kCurveUniformButton,kCurvePresetButton,kCurveFoundButton})EnableWindow(GetDlgItem(window,id),FALSE);
        RECT client{};GetClientRect(window,&client);
        RECT dirty{8,client.bottom-135,137,client.bottom-6};InvalidateRect(window,&dirty,FALSE);
        return;
    }
#endif
    Curve baseline{};
    Curve firstChannels[6]{};

    if (app->state.optflag[0] != 0) {
        if (channel < 0 || channel > bone_scale::kInterpCameraAll)
            channel = 3;
        ScanCurveTrack(app, app->CameraKeys(), mdl::KeyPoolCapacity(app->CameraKeys()),
                       6, channel, baseline, firstChannels, found,
                       perChannelUniform,
                       [](const mdl::CameraKey& key, int c, std::size_t) {
                           return ReadCurve(key, c);
                       });
    } else {
        unsigned char* model = app->SelectedModel();
        // Runtime capacity of the selected model's pool; the null-pool overload
        // reports 0, which ScanCurveTrack already treats as "no keys".  The
        // model panel's channels are the four legacy ones plus the bone scale
        // channel, and the trailing "all" entry is index 5.
        if (channel < 0 || channel > bone_scale::kInterpBoneAll)
            channel = 3;
        ScanCurveTrack(app, model != nullptr ? mdl::BoneKeys(model) : nullptr,
                       mdl::BoneKeyCapacity(model != nullptr ? mdl::BoneKeys(model)
                                                             : nullptr),
                       bone_scale::kInterpBoneAll, channel, baseline,
                       firstChannels, found, perChannelUniform,
                       [app](const mdl::BoneKey& key, int c, std::size_t slot) {
                           return c == bone_scale::kInterpScaleChannel
                                      ? ReadScaleCurve(app, slot)
                                      : ReadCurve(key, c);
                       });
    }

    if (app->state.interpCurveUniformFound != 0) {
        DrawCurveHighlight(app, baseline);
        EnableWindow(GetDlgItem(window, kCurveUniformButton), TRUE);
    } else {
        EnableWindow(GetDlgItem(window, kCurveUniformButton),
                     perChannelUniform ? TRUE : FALSE);
    }
    EnableWindow(GetDlgItem(window, kCurveFoundButton),
                 found ? TRUE : FALSE);
    EnableWindow(GetDlgItem(window, kCurvePresetButton),
                 found &&
                         reinterpret_cast<const std::int8_t&>(
                             app->state.lightA[0]) >= 0
                     ? TRUE
                     : FALSE);

    RECT client{};
    GetClientRect(window, &client);
    RECT dirty{kCurveDirtyLeft,
               client.bottom - kCurveDirtyTopMargin,
               kCurveDirtyRight,
               client.bottom - kCurveDirtyBottomMargin};
    InvalidateRect(window, &dirty, FALSE);
}

// ===========================================================================
// x64 0x7FF7CB4ED750 - MorphPanelRefresh(model)
// ===========================================================================
// Facial/IK panel echo of one model record: checkbox 439 mirrors the
// visibility byte (loadComplete, x64 +12569), the radio pair 444/445
// mirrors the IK-chain state of combo 443's current selection
// (ikChains[sel].enabled -> 444 else 445), and each of the four morph
// selectors (selectedMorphs[0..3], x64 +12592..) with an index >= 0 parks
// morphs[idx].value * 100 on its trackbar (505/510/515/520) and echoes
// "%5.4f" into the paired edit (506/511/516/521).  Negative selectors
// leave their group untouched (unlike PostLoadInit's echo).
// =========================================================================//
void MorphPanelRefresh(unsigned char* model) {  // x64 0x7FF7CB4ED750
    constexpr int kModelVisibleCheck = 439;
    constexpr int kIkChainCombo = 443;
    constexpr int kIkOnRadio = 444;
    constexpr int kIkOffRadio = 445;
    static const int kMorphSlider[4] = {505, 510, 515, 520};
    static const int kMorphEdit[4] = {506, 511, 516, 521};

    mdl::ModelRecord* m = mdl::Mdl(model);
    HWND window = static_cast<HWND>(m->hwnd);

    SendMessageA(GetDlgItem(window, kModelVisibleCheck), BM_SETCHECK,
                 m->loadComplete != 0 ? 1 : 0, 0);
    if (m->ikChains != nullptr) {
        const int selection = static_cast<int>(SendMessageA(
            GetDlgItem(window, kIkChainCombo), CB_GETCURSEL, 0, 0));
        CheckRadioButton(window, kIkOnRadio, kIkOffRadio,
                         m->ikChains[selection].enabled != 0 ? kIkOnRadio
                                                             : kIkOffRadio);
    }
    if (m->morphs != nullptr) {
        char text[256];
        for (int group = 0; group < 4; ++group) {
            const std::int32_t selection = m->selectedMorphs[group];
            if (selection < 0)
                continue;
            const float value = m->morphs[selection].value;
            SendMessageA(GetDlgItem(window, kMorphSlider[group]),
                         TBM_SETPOS, 1,
                         static_cast<LPARAM>(
                             static_cast<int>(value * 100.0f)));
            sprintf_s(text, 0x100, "%5.4f", static_cast<double>(value));
            SetWindowTextA(GetDlgItem(window, kMorphEdit[group]), text);
        }
    }
}

// Original bone-frame multiply helper: 9.26 x64 RVA 0x9C9F0..0x9D145.
// Only selected bone keys are transformed. MMS adds three scale-channel pairs
// below the original position/angle pairs, using the unposed model-space axes
// of the stored key. Row directions (including shear) and curves are preserved.
void SnapshotSelectedKeysForUndo(unsigned char*, int);
bool ApplyBoneFrameScaleAdd(MMDApp* app, HWND dialog) {
    auto* bytes = app->SelectedModel();
    auto* model = mdl::Mdl(bytes);
    if (!model || !model->boneKeys) return true;
    float scale[9], add[9];
    std::fill_n(scale, 9, 1.f); std::fill_n(add, 9, 0.f);
    bool changed = false, rotate = false, resize = false;
    const auto read = [&](int id, float& value) {
        HWND edit = GetDlgItem(dialog, id);
        char text[128]{};
        if (GetWindowTextLengthA(edit) >= sizeof(text)) return false;
        GetWindowTextA(edit, text, sizeof(text));
        char* end = nullptr; errno = 0;
        const double parsed = std::strtod(text, &end);
        if (end == text || errno == ERANGE) return false;
        while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        value = static_cast<float>(parsed);
        return !*end && std::isfinite(value);
    };
    const int pairs = MMD_ORIGINAL_PROFILE ? 6 : 9;
    for (int pair = 0; pair < pairs; ++pair) {
        for (int column = 0; column < 2; ++column) {
            const int id = panel::kBoneMulPosXScaleEdit + 2 * pair + column;
            if (!read(id, column ? add[pair] : scale[pair])) {
                SetFocus(GetDlgItem(dialog, id));
                SendDlgItemMessageW(dialog, id, EM_SETSEL, 0, -1);
                return false;
            }
        }
        if (scale[pair] != 1.f || add[pair] != 0.f) {
            changed = true;
            if (pair >= 3 && pair < 6) rotate = true;
            if (pair >= 6) resize = true;
        }
    }
    if (!changed) return true;
    const std::size_t boneKeyCapacity = mdl::BoneKeyCapacity(*model);
    auto& api = d3dx::Get();
    if (rotate && !api.Load()) return false;
    constexpr float pi = 3.141592f;
    add[3] *= pi / 180.f;
    add[4] *= -pi / 180.f;
    add[5] *= -pi / 180.f;
    struct Pending { int index; mdl::BoneKey key; bone_scale::Matrix scale; };
    std::vector<Pending> pending;
    for (std::size_t i = 0; i < boneKeyCapacity; ++i) {
        auto key = model->boneKeys[i];
        if (!key.allocated) continue;
        for (int axis = 0; axis < 3; ++axis) {
            key.position[axis] = key.position[axis] * scale[axis] + add[axis];
            if (!std::isfinite(key.position[axis])) return false;
        }
        auto stretch = bone_scale::Key(bytes, static_cast<int>(i));
        if (resize) {
            for (int axis = 0; axis < 3; ++axis) {
                if (scale[axis + 6] == 1.f && add[axis + 6] == 0.f) continue;
                double length = 0;
                for (int c = 0; c < 3; ++c)
                    length += double(stretch.v[axis * 3 + c]) * stretch.v[axis * 3 + c];
                length = std::sqrt(length);
                const double result = length * scale[axis + 6] + add[axis + 6];
                if (!(length > 0) || !(result > 0) || !std::isfinite(result)) return false;
                for (int c = 0; c < 3; ++c)
                    stretch.v[axis * 3 + c] = static_cast<float>(stretch.v[axis * 3 + c] * result / length);
            }
            if (!bone_scale::Valid(stretch)) return false;
        }
        if (rotate) {
            d3dx::D3DXMATRIXF matrix{}, rotation{}, axisMatrix{};
            api.matrixRotationQuaternion(&matrix, key.rotation);
            float x = std::asin(std::fmax(-1.f, std::fmin(1.f, -matrix.m[2][1])));
            float y = std::atan2(matrix.m[2][0], matrix.m[2][2]);
            float z = std::atan2(matrix.m[0][1], matrix.m[1][1]);
            if (std::fabs(std::cos(x)) < 1e-6f) {
                z += matrix.m[0][1] > 0.f ? pi : -pi;
                y += matrix.m[2][0] > 0.f ? pi : -pi;
            }
            api.rotZ(&rotation, z * scale[5] + add[5]);
            api.rotX(&axisMatrix, x * scale[3] + add[3]);
            api.multiply(&rotation, &rotation, &axisMatrix);
            api.rotY(&axisMatrix, y * scale[4] + add[4]);
            api.multiply(&rotation, &rotation, &axisMatrix);
            api.quatFromMatrix(key.rotation, &rotation);
            for (float value : key.rotation) if (!std::isfinite(value)) return false;
        }
        pending.push_back({static_cast<int>(i), key, stretch});
    }
    if (pending.empty()) return true;
    // Validate the whole selection before opening one undo entry or changing
    // any key. SetKey keeps each slot's existing scale interpolation curve.
    SnapshotSelectedKeysForUndo(bytes, app->CurrentFrame());
    for (const auto& item : pending) {
        model->boneKeys[item.index] = item.key;
        if (resize) bone_scale::SetKey(bytes, item.index, item.scale);
    }
    PanelPaint(app);
    CurvePanelRepaint(app);
    SeekModelFrame(bytes, app->CurrentFrame(), app->PlaybackPhysicsMode());
    PostViewRefresh(app);
    app->SceneModified() = 1;
    return true;
}

// ===========================================================================
// MAINTENANCE: legacy camera-only duplicate with no callers in this port.
// Retained for reference; the bone dialog uses the function above. Defect B2-14.
// ===========================================================================
// Reads the 16 edits 686..701 as eight (scale, add) pairs and applies each
// pair to every selected camera key (camera track app+0x374, 84-byte
// CameraKey records, 10000 of them, gated on the +0x48 selection byte):
//   pair  field                add conversion
//   0     eye.x    (+0x10)     -
//   1     eye.y    (+0x14)     -
//   2     eye.z    (+0x18)     -
//   3     target.x (+0x1C)     * -3.141592 / 180
//   4     target.y (+0x20)     *  3.141592 / 180
//   5     target.z (+0x24)     *  3.141592 / 180
//   6     distance (+0x0C)     -
//   7     fov      (+0x44 int) -
// A pair is skipped when scale == 1.0 and add == 0.0 (the dialog's default
// prefill).  Tail: camera seek (x64 sub_7FF7CB479C30 == the ported 0x42E640
// ReloadModels), PostViewRefresh, dirty 0xA0B0D = 1.
// =========================================================================//
void ApplyCameraFrameScaleAdd(MMDApp* app, HWND hDlg) {  // was Sub43E000, VA 0x0043E000
    float scale[8];
    float add[8];
    char buf[20];
    for (int pair = 0; pair < 8; ++pair) {
        GetWindowTextA(GetDlgItem(hDlg, panel::kCamMulPosXScaleEdit + 2 * pair), buf, 20);
        scale[pair] = static_cast<float>(atof(buf));
        GetWindowTextA(GetDlgItem(hDlg, panel::kCamMulPosXOffsetEdit + 2 * pair), buf, 20);
        add[pair] = static_cast<float>(atof(buf));
    }
    // degree -> radian on the three target-angle adds (target.x negated)
    add[3] = static_cast<float>(add[3] * -3.141592) / 180.0f;
    add[4] = static_cast<float>(add[4] * 3.141592) / 180.0f;
    add[5] = static_cast<float>(add[5] * 3.141592) / 180.0f;

    mdl::CameraKey* keys = app->CameraKeys();
    for (std::size_t i = 0, poolCapacity = static_cast<std::size_t>(mdl::KeyPoolCapacity(keys)); i < poolCapacity; ++i) {
        mdl::CameraKey& key = keys[i];
        if (key.selected == 0)
            continue;
        if (scale[0] != 1.0f || add[0] != 0.0f)
            key.eye[0] = static_cast<float>(scale[0] * key.eye[0]) + add[0];
        if (scale[1] != 1.0f || add[1] != 0.0f)
            key.eye[1] = static_cast<float>(scale[1] * key.eye[1]) + add[1];
        if (scale[2] != 1.0f || add[2] != 0.0f)
            key.eye[2] = static_cast<float>(scale[2] * key.eye[2]) + add[2];
        if (scale[3] != 1.0f || add[3] != 0.0f)
            key.target[0] =
                static_cast<float>(scale[3] * key.target[0]) + add[3];
        if (scale[4] != 1.0f || add[4] != 0.0f)
            key.target[1] =
                static_cast<float>(scale[4] * key.target[1]) + add[4];
        if (scale[5] != 1.0f || add[5] != 0.0f)
            key.target[2] =
                static_cast<float>(scale[5] * key.target[2]) + add[5];
        if (scale[6] != 1.0f || add[6] != 0.0f)
            key.distance =
                static_cast<float>(scale[6] * key.distance) + add[6];
        if (scale[7] != 1.0f || add[7] != 0.0f)
            key.fov = static_cast<int>(
                static_cast<float>(static_cast<float>(
                    static_cast<float>(key.fov) * scale[7]) + add[7]));
    }
    ReloadModels(app);      // 0x42E640 (x64 sub_7FF7CB479C30)
    PostViewRefresh(app);   // 0x40D130 (x64 sub_7FF7CB440DD0)
    app->SceneModified() = 1;
}

// ===========================================================================
// 0x0043E680 - case-252 "frame control" apply (x64 sub_7FF7CB4BD120)
// ===========================================================================
// Reads the (scale, add) pair from edits 686/687 and, when it is not the
// identity (1.0 / 0.0), scales+shifts every allocated morph key of the
// current model (morphKeys table, 20000 20-byte records, value +0x0C,
// allocated byte +0x10).  Tail: PanelPaint, curve-panel repaint
// (x64 sub_7FF7CB482BB0), model frame seek SeekModelFrame(model, currentFrame,
// playbackPhysicsMode) (x64 sub_7FF7CB4EBD90), facial/IK panel refresh
// (x64 sub_7FF7CB4ED750), dirty 0xA0B0D = 1.
// =========================================================================//
void ApplyMorphScaleAdd(MMDApp* app, HWND hDlg) {  // was Sub43E680, VA 0x0043E680
    unsigned char* modelBytes = app->ModelSlot(app->state.slotIdx);
    if (modelBytes == nullptr)
        return;

    char buf[20];
    GetWindowTextA(GetDlgItem(hDlg, panel::kMorphMulScaleEdit), buf, 20);
    const float scale = static_cast<float>(atof(buf));
    GetWindowTextA(GetDlgItem(hDlg, panel::kMorphMulOffsetEdit), buf, 20);
    const float add = static_cast<float>(atof(buf));

    if (scale != 1.0f || add != 0.0f) {
        mdl::MorphKey* keys = mdl::MorphKeys(modelBytes);
        const auto morphCapacity1 = mdl::MorphKeyCapacity(keys);
        for (std::size_t i = 0; i < morphCapacity1; ++i) {
            if (keys[i].allocated != 0)
                keys[i].value =
                    static_cast<float>(scale * keys[i].value) + add;
        }
    }
    PanelPaint(app);         // 0x414610 (x64 sub_7FF7CB480EA0)
    CurvePanelRepaint(app);  // x64 sub_7FF7CB482BB0
    SeekModelFrame(modelBytes, app->state.currentFrame,
              app->state.playbackPhysicsMode);  // 0x4B4260 (x64 0x4EBD90)
    MorphPanelRefresh(modelBytes);               // x64 sub_7FF7CB4ED750
    app->SceneModified() = 1;
}

// ===========================================================================
// 0x0041E810 - case-289 reorder-dialog init fill (x64 sub_7FF7CB4A98C0)
// ===========================================================================
// `count` is CB_GETCOUNT(main combo 0x1B4) - 1 (cached in
// g_calculateOrderDialogCount by
// the caller ModelCalculateOrderDlgProc).  The helper owns three steps the x64 dialog proc
// (sub_7FF7CB477D70) performs inline:
//   1. allocate the int scratch array app+0xA0B1C (count+1 dwords - the
//      original allocates CB_GETCOUNT entries),
//   2. copy combo entries 1..count into listbox 628 (entry 0 is the
//      "camera/light/accessory" header row),
//   3. build the order array: for order = 1..count scan the model slots for
//      the model whose combo order byte (+0x2D7C) equals `order` and store
//      slotIndex+1 (the +1 quirk ApplyModelCalculateOrderDialog relies on).
// =========================================================================//
void InitModelOrderDialog(int count, HWND hDlg) {  // was Sub41E810, VA 0x0041E810
    MMDApp* app = g_Block;
    constexpr std::size_t kModelOrder2D7C = 0x2D7C;  // combo order byte
    // value-init: element 0 stays untouched by the fill below, and the OK
    // apply (ApplyModelCalculateOrderDialog) walks the array from index 0
    app->AccessoryOrderArray() =
        std::calloc(static_cast<std::size_t>(count) + 1, sizeof(std::int32_t));

    for (int i = 1; i <= count; ++i) {
        const auto length = SendMessageW(GetDlgItem(app->state.hwnd, panel::kMainComboModel), CB_GETLBTEXTLEN, i, 0);
        std::wstring buf(length >= 0 ? static_cast<size_t>(length) + 1 : 1, L'\0');
        SendMessageW(GetDlgItem(app->state.hwnd, panel::kMainComboModel),
                     CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(buf.data()));
        SendMessageW(GetDlgItem(hDlg, panel::kOrderListBox), LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(buf.data()));
    }

    std::int32_t* order =
        static_cast<std::int32_t*>(app->AccessoryOrderArray());
    // x64 sub_7FF7CB4A98C0+0x4A98F0: order runs 1..count (the combo count
    // cached at dword_7FF7CB564604), the slot scan to 0xFF (cmp ecx,0FFh
    // @0x4A991A) - kModelSlotCount wide.
    for (int ord = 1; ord <= count; ++ord) {
        for (int slot = 0; slot < kModelSlotCount; ++slot) {
            unsigned char* model = app->ModelSlot(slot);
            if (model != nullptr &&
                mdl::Mdl(model)->comboSelIndex ==
                    static_cast<unsigned char>(ord)) {
                order[ord] = slot + 1;
                break;
            }
        }
    }
}

// ===========================================================================
// 0x00423160 - case-266 accessory-frame dialog init (x64 sub_7FF7CB4B5F00)
// ===========================================================================
// Echoes the gravity cluster into the dialog controls: "%3.2f" of the
// magnitude (0x9EDC8) into edit 709 and of the direction X/Y/Z (0x9EDBC /
// 0x9EDC0 / 0x9EDC4) into edits 710..712; trackbars 637..639 get range
// -100..100, tick frequency 0x3E8 and pos = value * 100; "%d" of the noise
// dword (0x9EDCC) into edit 713; checkbox 731 mirrors the noise-mode byte
// 0xA0CD4 and gates edit 713 through EnableWindow.
// =========================================================================//
void InitGravityDialog(HWND hDlg) {  // was Sub423160, VA 0x00423160
    MMDApp* app = g_Block;
    char buf[0x100];
    sprintf_s(buf, 0x100, "%3.2f",
              static_cast<double>(app->state.gravityMagnitude));
    SetWindowTextA(GetDlgItem(hDlg, panel::kGravityAccelEdit), buf);
    const float direction[3] = {app->state.gravityX, app->state.gravityY,
                                app->state.gravityZ};
    static const int kDirectionEdits[3] = {710, 711, 712};
    static const int kDirectionTracks[3] = {637, 638, 639};
    for (int axis = 0; axis < 3; ++axis) {
        sprintf_s(buf, 0x100, "%3.2f",
                  static_cast<double>(direction[axis]));
        SetWindowTextA(GetDlgItem(hDlg, kDirectionEdits[axis]), buf);
        SendMessageA(GetDlgItem(hDlg, kDirectionTracks[axis]),
                     TBM_SETRANGEMIN, 0, -100);
        SendMessageA(GetDlgItem(hDlg, kDirectionTracks[axis]),
                     TBM_SETRANGEMAX, 0, 100);
        SendMessageA(GetDlgItem(hDlg, kDirectionTracks[axis]),
                     TBM_SETTICFREQ, 0x3E8, 0);
        SendMessageA(GetDlgItem(hDlg, kDirectionTracks[axis]),
                     TBM_SETPOS, 1,
                     static_cast<int>(
                         static_cast<double>(direction[axis]) * 100.0));
    }
    sprintf_s(buf, 0x100, "%d", app->state.gravityNoise);
    SetWindowTextA(GetDlgItem(hDlg, panel::kGravityNoiseEdit), buf);
    if (app->state.gravityNoiseEnabled != 0) {
        SendMessageA(GetDlgItem(hDlg, panel::kGravityNoiseCheckbox), BM_SETCHECK, 1, 0);
        EnableWindow(GetDlgItem(hDlg, panel::kGravityNoiseEdit), TRUE);
    } else {
        SendMessageA(GetDlgItem(hDlg, panel::kGravityNoiseCheckbox), BM_SETCHECK, 0, 0);
        EnableWindow(GetDlgItem(hDlg, panel::kGravityNoiseEdit), FALSE);
    }
}

// 0x00412330 ApplyGravityTrack (gravity-track apply + physics dialog refresh) is
// ported in src/model/track_apply.cpp.

// ===========================================================================
// 0x004403C0 - case-275 physics ON/OFF apply (x64 sub_7FF7CB4BED40)
// ===========================================================================
// `on` is (CB_GETCURSEL(combo 669) != 0): 0 = "ON (X mark)", 1 = "OFF" - it
// lands in BoneKey::physicsDisabled of every registered key.  Body:
//   * walk the current model's bone keys (bones gated on the physics X-mark
//     list flag BoneRecord::hasRigidBody) and count the allocated keys along each
//     bone's key chain - the count sizes the undo auxiliary buffer,
//   * enable undo (button 400) / disable redo (401), advance the undo ring
//     (undoDirty=1, redoDirty=0, cursor wrap at 30, redo cursor mirrors),
//     register undo slot operation 2 at the current frame, snapshot every
//     bone (36-byte BonePoseSnapshot: index, trans, rotQuat,
//     physicsDisabled from bonePhysicsState[i]) and allocate the 64-byte
//     auxiliary records,
//   * clear keyVisitMap and re-register a physics key for every allocated
//     key of the hasRigidBody bones, writing `on` into physicsDisabled,
//   * tail: PanelPaint + model frame seek SeekModelFrame(model, currentFrame,
//     playbackPhysicsMode).
// =========================================================================//
void ApplyPhysicsOnOff(int on) {  // was Sub4403C0, VA 0x004403C0
    MMDApp* app = g_Block;
    unsigned char* modelBytes = app->ModelSlot(app->state.slotIdx);
    if (modelBytes == nullptr)
        return;
    mdl::ModelRecord* model = mdl::Mdl(modelBytes);
    const std::int32_t boneCount = model->boneCount;
    if (boneCount <= 0)
        return;
    mdl::BoneRecord* bones = model->boneTable;
    mdl::BoneKey* keys = model->boneKeys;
    if (bones == nullptr || keys == nullptr)
        return;
    const unsigned char physicsOff = static_cast<unsigned char>(on != 0);

    // ---- count the physics-marked keys (chain walk; the root is visited
    // twice by the original - once directly, once by the tail/walk re-check
    // - kept as-is since it only sizes the undo buffer) -------------------
    int marked = 0;
    for (int i = 0; i < boneCount; ++i) {
        if (bones[i].hasRigidBody == 0)
            continue;
        if (keys[i].allocated != 0)
            ++marked;
        int cur = i;
        while (keys[cur].next > 0) {
            if (keys[cur].allocated != 0)
                ++marked;
            cur = static_cast<int>(keys[cur].next);
        }
        if (keys[cur].allocated != 0)
            ++marked;
    }
    if (marked == 0)
        return;

    // ---- undo ring slot --------------------------------------------------
    HWND hwnd = app->state.hwnd;
    EnableWindow(GetDlgItem(hwnd, panel::kUndoButton), TRUE);
    EnableWindow(GetDlgItem(hwnd, panel::kRedoButton), FALSE);
    model->undoDirty = 1;
    model->redoDirty = 0;
    std::uint32_t& cursor = model->undoState[0];
    if (++cursor >= 30)
        cursor = 0;
    model->undoState[1] = cursor;
    bone_scale::CaptureUndo(modelBytes);
    mdl::UndoRecord& undo = model->undoRings[0].slots[cursor];
    undo.operation = 2;
    undo.frame = static_cast<std::uint32_t>(app->state.currentFrame);

    if (undo.bonePose != nullptr) {
        std::free(undo.bonePose);
        undo.bonePose = nullptr;
    }
    auto* snapshot = static_cast<mdl::BonePoseSnapshot*>(::operator new(
        sizeof(mdl::BonePoseSnapshot) * boneCount));
    undo.bonePose = snapshot;
    std::memset(snapshot, 0,
                sizeof(mdl::BonePoseSnapshot) *
                    static_cast<std::size_t>(boneCount));
    for (int i = 0; i < boneCount; ++i) {
        snapshot[i].boneIndex = i;
        snapshot[i].position[0] = bones[i].trans[0];
        snapshot[i].position[1] = bones[i].trans[1];
        snapshot[i].position[2] = bones[i].trans[2];
        snapshot[i].rotation[0] = bones[i].rotQuat[0];
        snapshot[i].rotation[1] = bones[i].rotQuat[1];
        snapshot[i].rotation[2] = bones[i].rotQuat[2];
        snapshot[i].rotation[3] = bones[i].rotQuat[3];
        snapshot[i].physicsDisabled =
            model->bonePhysicsState != nullptr
                ? model->bonePhysicsState[i]
                : 0;
    }
    undo.dirty = 0;
    if (undo.auxiliaryPose != nullptr) {
        std::free(undo.auxiliaryPose);
        undo.auxiliaryPose = nullptr;
    }
    undo.auxiliaryPose = ::operator new(
        static_cast<std::size_t>(marked) * sizeof(mdl::BoneKeySnapshot));
    std::memset(undo.auxiliaryPose, 0,
                static_cast<std::size_t>(marked) * sizeof(mdl::BoneKeySnapshot));
    mdl::BoneKeyVisitClear(*model);

    // ---- register the physics keys ---------------------------------------
    // 64-byte auxiliary record = {int boneIndex, 60-byte BoneKey copy};
    // the append dedups through the visit map (x64 sub_7FF7CB4E8FD0).  The map
    // is resolved once here so the append below costs no capacity lookup, and
    // it is the same map the clear above emptied even after the pool grew.
    unsigned char* const visited = mdl::BoneKeyVisitMapOf(*model).bytes;
    const auto appendAux = [model, &undo, visited](int boneIndex) {
        if (visited[boneIndex] != 0)
            return;
        visited[boneIndex] = 1;
        auto* records = static_cast<mdl::BoneKeySnapshot*>(undo.auxiliaryPose);
        records[undo.dirty] = {boneIndex, model->boneKeys[boneIndex]};
        ++undo.dirty;
    };
    for (int i = 0; i < boneCount; ++i) {
        if (bones[i].hasRigidBody == 0)
            continue;
        if (keys[i].allocated != 0) {
            appendAux(i);
            keys[i].physicsDisabled = physicsOff;
        }
        int cur = i;
        while (keys[cur].next > 0) {
            if (keys[cur].allocated != 0) {
                appendAux(cur);
                keys[cur].physicsDisabled = physicsOff;
            }
            cur = static_cast<int>(keys[cur].next);
        }
        if (keys[cur].allocated != 0) {
            appendAux(cur);
            keys[cur].physicsDisabled = physicsOff;
        }
    }
    PanelPaint(app);  // 0x414610 (x64 sub_7FF7CB480EA0)
    SeekModelFrame(modelBytes, app->state.currentFrame,
              app->state.playbackPhysicsMode);  // 0x4B4260 (x64 0x4EBD90)
}

}  // namespace mikumikustudio
