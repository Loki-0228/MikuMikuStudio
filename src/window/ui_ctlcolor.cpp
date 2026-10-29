#include "mikumikustudio/negative_frames.hpp"
// ===========================================================================
// VA 0x0040E0E0 - HandleCtlColor  (original: sub_40E0E0)
// ===========================================================================
// WM_CTLCOLORSTATIC / WM_CTLCOLORBTN handler (caller: WndProc 0x004C3A10).
//
// Original signature:  LRESULT __fastcall sub_40E0E0(MMDApp* this /*ecx*/,
// HWND ctrlHwnd /*stack arg 0 = WM_CTLCOLORSTATIC lParam*/, HDC hdc
// /*stack arg 1 = wParam - never read*/);  `this` is the g_Block global
// (the ported signature has no app parameter).
//
// The control handle (lParam) is compared against GetDlgItem(mainHwnd, id)
// for the 11 themed control ids below, in this exact order.  On a match the
// corresponding gradient-brush slot is returned; no match returns 0.
// mainHwnd = this+0xA06B8 (657080).  The 11 HBRUSH slots are the array
// created by the 0x47A5B0 init at 656588 (0xA04CC); slot i = 656588 + 4*i:
//
//     id       id(hex)    slot   app offset
//     447      0x1BF        9     0xA04F0 (656624)
//     534      0x216       10     0xA04F4 (656628)
//     455      0x1C7        0     0xA04CC (656588)
//     456      0x1C8        1     0xA04D0 (656592)
//     457      0x1C9        2     0xA04D4 (656596)
//     458      0x1CA        3     0xA04D8 (656600)
//     459      0x1CB        4     0xA04DC (656604)
//     460      0x1CC        5     0xA04E0 (656608)
//     515|510  0x203/0x1FE  6     0xA04E4 (656612)
//     520|505  0x208/0x1F9  7     0xA04E8 (656616)
//     560      0x230        8     0xA04EC (656620)
//
// The two paired checks (515|510 and 520|505) are NOT short-circuited in
// the original: both GetDlgItem calls execute unconditionally (setz/or
// pattern at 0x40E1DF and 0x40E21D), so the port precomputes both results
// before combining them.
//
// Reference: ../translated/MikuMikuDance/fcn_0040e0e0.cpp
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <new>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"

namespace mikumikustudio {
namespace {

// Free if non-null, then null the slot - mirrors the asm block
// (mov eax,[esi+off]; cmp eax,0; jz next; push eax; call free;
//  mov [esi+off],0), with the call order at the call site preserved.
//
// This deliberately deviates from the original, which releases these blocks
// through the CRT free thunk 0x509154 (the `call free` in the asm above),
// while every record array stored in these slots is created with
// ::operator new in ui_editor_click.cpp (FillSelectionRecords and the
// accessory, ModelIk, ModelMorph and ModelBone fill blocks).  The release here
// therefore uses ::operator delete, which matches the allocation; calling free() on
// ::operator new storage is undefined behaviour even though the MSVC CRT heap
// makes it work.
// Defect B1-03.
void FreeTimelineSelectionRecords(MMDApp* app, TimelineSelectionBand band) {
    TimelineSelectionRecord*& p = app->TimelineSelectionRecords(band);
    if (p != nullptr) {
        ::operator delete(p);
        p = nullptr;
    }
}

}  // namespace

// Forward declarations; bodies in the files noted.
void PanelPaint(MMDApp* app);       // VA 0x00414610 (ui_panel_paint.cpp)
void SelectionReeval(MMDApp* app);  // VA 0x00430510 (ui_selection_reeval.cpp)

LRESULT HandleCtlColor(HWND control, HDC dc) {
    // Original themed controls return cached brushes; the scale caption also
    // needs a transparent text background so the gray toolbar remains visible.
    MMDApp* app = g_Block;
    const HWND ctrl = control;
    const HWND main = reinterpret_cast<HWND>(app->state.hwnd);  // 657080
    const auto brushResult = [app](int index) {
        return static_cast<LRESULT>(
            reinterpret_cast<std::intptr_t>(app->UiBrush(index)));
    };

    // Restore the toolbar gradient before each transparent STATIC text draw.
    // NULL_BRUSH alone leaves old glyphs behind after font/language changes.
    if (GetDlgCtrlID(ctrl) == 570) {
        RECT client{}, position{};
        GetClientRect(ctrl, &client);
        GetWindowRect(ctrl, &position);
        MapWindowPoints(nullptr, GetParent(ctrl), reinterpret_cast<POINT*>(&position), 2);
        const auto panels = FramePanelLayout(app);
        const LONG top = panels.bottomBand.top - position.top;
        TRIVERTEX vertices[2]{};
        vertices[0].x = client.left; vertices[0].y = top;
        vertices[0].Red = vertices[0].Green = vertices[0].Blue = 0x7800;
        vertices[1].x = client.right;
        vertices[1].y = top + panels.bottomBand.bottom - panels.bottomBand.top;
        vertices[1].Red = vertices[1].Green = vertices[1].Blue = 0xC800;
        GRADIENT_RECT gradient{0,1};
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
        GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);
        RestoreDC(dc, saved);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }

    // Panel captions use color.txt's shared text color, including custom themes.
    const int controlId = GetDlgCtrlID(ctrl);
    if (controlId == panel::kAdaptiveShadowCheckbox || controlId == panel::kShadowFilterCheckbox) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(app->UiTextRed(), app->UiTextGreen(), app->UiTextBlue()));
        // The checkbox subclass paints the matching panel gradient at its
        // actual position; the slider's solid brush belongs to another row.
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    if (controlId == panel::kCameraRefModelCheckbox ||
        controlId == panel::kCameraRefBoneCheckbox) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(app->UiTextRed(), app->UiTextGreen(), app->UiTextBlue()));
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }

    if (ctrl == GetDlgItem(main, panel::kFovSlider))  // 0x1BF -> slot 9 (0xA04F0)
        return brushResult(9);

    if (ctrl == GetDlgItem(main, panel::kFrameVolumeSlider))  // 0x216 -> slot 10 (0xA04F4)
        return brushResult(10);

    if (ctrl == GetDlgItem(main, panel::kLightColorSliderR))  // 0x1C7 -> slot 0 (0xA04CC)
        return brushResult(0);

    if (ctrl == GetDlgItem(main, panel::kLightColorSliderG))  // 0x1C8 -> slot 1 (0xA04D0)
        return brushResult(1);

    if (ctrl == GetDlgItem(main, panel::kLightColorSliderB))  // 0x1C9 -> slot 2 (0xA04D4)
        return brushResult(2);

    if (ctrl == GetDlgItem(main, panel::kLightDirSliderX))  // 0x1CA -> slot 3 (0xA04D8)
        return brushResult(3);

    if (ctrl == GetDlgItem(main, panel::kLightDirSliderY))  // 0x1CB -> slot 4 (0xA04DC)
        return brushResult(4);

    if (ctrl == GetDlgItem(main, panel::kLightDirSliderZ))  // 0x1CC -> slot 5 (0xA04E0)
        return brushResult(5);

    // Pair 515|510 (0x203/0x1FE) -> slot 6 (0xA04E4): both GetDlgItem calls
    // run unconditionally in the original (setz/or, no short-circuit).
    const bool hit515 = ctrl == GetDlgItem(main, panel::kMorphSlider2);
    const bool hit510 = ctrl == GetDlgItem(main, panel::kMorphSlider1);
    if (hit515 || hit510)
        return brushResult(6);

    // Pair 520|505 (0x208/0x1F9) -> slot 7 (0xA04E8): same non-short-circuit
    // pattern as the pair above.
    const bool hit520 = ctrl == GetDlgItem(main, panel::kMorphSlider3);
    const bool hit505 = ctrl == GetDlgItem(main, panel::kMorphSlider0);
    if (hit520 || hit505)
        return brushResult(7);

    if (ctrl == GetDlgItem(main, panel::kSelfShadowRangeSlider))  // 0x230 -> slot 8 (0xA04EC)
        return brushResult(8);

    return 0;
}

// ===========================================================================
// VA 0x0044A9A0 - HandleLButtonUp  (original: sub_44A9A0)
// ===========================================================================
// WM_LBUTTONUP handler (sole caller: WndProc 0x004C3A10, after
// ReleaseCapture).
//
// Unconditionally:  this+0xC8 (200) = 0 (drag flag) and this+0xA442C
// (672812) = 1 (paint gate).  The drag-active flag this+0xA0189 (655753)
// is read BEFORE those two stores (cmp at 0x44A9A6, stores at 0x44A9AC /
// 0x44A9B2, branches on the entry value).  When it is non-zero, the seven
// original release addresses listed below are freed in this exact order:
// free if non-null, then the slot is set to null (asm 0x44A9BF..0x44AA68, CRT
// free thunk 0x509154):
//
//     0xA0410 (656400)   0xA0418 (656408)   0xA03F0 (656368)
//     0xA03F8 (656376)   0xA0428 (656424)   0xA0400 (656384)
//     0xA0420 (656416)
//
// The port then frees the Gravity band pointer as an eighth call; the comment
// at that call records the reason.
// Defect B1-02.
//
// then this+0xA0189 = 0, this+0xA03EB (656363) = 0, PanelPaint (0x414610)
// and SelectionReeval (0x430510).  Finally this+0x9DA09 (645641) = 0 runs
// on every path (drag active or not).
//
// Reference: ../translated/MikuMikuDance/fcn_0044a9a0.cpp
// =========================================================================//
void HandleLButtonUp(MMDApp* app) {
    mikumikustudio::negative_frames::EndDrag(app);
    HandleViewportToolRelease(app);
    const bool dragActive = app->SelectionBoxDragging() != 0;
    app->SidebarResizeDragging() = 0;
    app->WindowLayoutReady() = 1;

    if (dragActive) {
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::Accessory);
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::ModelIk);
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::Camera);
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::Light);
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::ModelBone);
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::SelfShadow);
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::ModelMorph);
        // Deliberate deviation from the original: its release block at
        // 0x44A9BF-0x44AA68 frees only the seven addresses listed above and
        // omits the Gravity band pointer 0xA0408 (656392), so the Gravity
        // record array leaked on every marquee selection. That array is the
        // one filled at ui_editor_click.cpp:275 (FillSelectionRecords with
        // app->GravityKeys() and TimelineSelectionBand::Gravity); this port
        // also frees it here.
        // Defect B1-02.
        FreeTimelineSelectionRecords(app, TimelineSelectionBand::Gravity);

        app->SelectionBoxDragging() = 0;
        app->TimelineSelectionChanged() = 0;
        app->ClearTimelineRange();
        PanelPaint(app);               // 0x414610
        SelectionReeval(app);          // 0x430510
    }

    app->PendingTimelineSelectionRow() = TimelineSelectionRow::None;
}

}  // namespace mikumikustudio
