#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/ui_metrics.hpp"
// ===========================================================================
// VA 0x00443300 - HandleWindowSize  (original: sub_443300, ~0x19B1 bytes)
// ===========================================================================
// WM_SIZE layout engine.  Called from MainWndProc (0x4C3A10 WM_SIZE), the
// command dispatch (0x47E8A0) and the scene loaders (0x458F80 / 0x460430 /
// 0x460B30).  After a resize the 168 child controls of the main window are
// fully repositioned:
//
//   1. 0x44331B  clamp: GetWindowRect; if width  > 2560 ->
//      SetWindowPos(cx=2560, SWP_NOZORDER); if height > 1920 ->
//      SetWindowPos(cy=1920, SWP_NOZORDER).  Position is preserved.
//   2. 0x4433A4  sidebar width (this+0xA06C8): GetClientRect; when the
//      accessory-column gate (this+0xA0D38) is set the sidebar is client
//      width - 3 and the label sweep PostLanguageSweep (0x42F1E0) runs;
//      otherwise sidebar = max(250, (int)(ratio @0xA4428 * client width))
//      and PanelPaint (0x414610) runs when the sidebar grew.
//   3. 0x4433F5  control layout, three coordinate families:
//      - left tool row (ids 400..559): X = sidebar/2 + k (or sidebar - k,
//        fixed 15 for 558), constant Y/W/H.
//      - main grid (ids 427..453): Y = clientBottom - k (stretched to the
//        window bottom), X = sidebar-derived or fixed; several controls get
//        W/H = clientBottom - k (e.g. 427 H, 428 W, 434/436/443 big panels).
//      - option bands (ids 455..535): X = const - bandXBase / const -
//        bandXBase2 where
//        the two bases accumulate conditional deltas from the UI option
//        flag bytes this+0x2F8..0x2FE (kByteOptflag0..6), see the body.
//   4. 0x444B3B  visibility re-toggle: five id ranges (455..469, 471..488,
//      504..528, 402..414, 531..535) get ShowWindow(SW_HIDE) followed by
//      ShowWindow(SW_SHOW) when IsWindowVisible.
//   5. 0x444C9D  refresh tail: RefreshMainWindowViewport (0x42C810) then Sub40CAC0
//      (0x40CAC0).
//
// All MoveWindow calls pass bRepaint = TRUE; every GetDlgItem handle is
// consumed by the immediately following MoveWindow, so the per-control
// sequence is rendered through one file-static helper (control order and
// parameters are unchanged).
//
// Reference: ../translated/MikuMikuDance/fcn_00443300.cpp is a partial
// auto-generated dump (coordinates/IDs mislabelled past 0x44365E); this
// port follows the live IDA disassembly/pseudocode of VA 0x00443300.
// =========================================================================//
#include "mikumikustudio/animation_curve_host.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <algorithm>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "ui_control_layout.hpp"

namespace mikumikustudio {

namespace {

void LayoutSidebarRows(MMDApp* app, HWND hwnd, int sidebar, int clientBottom, bool deferPaint) {
    const auto layout = MainPanelLayout(app, clientBottom);
    const int side = layout.DesignPixels(sidebar);
    const auto move = [&](int id, int x, int y, int width, int height) {
        const HWND control = GetDlgItem(hwnd, id);
        ApplyTemplateUiFont(app, control, layout.timelinePercent);
        const int origin = layout.Pixels(6);
        const int left = origin + layout.Pixels(x - 6);
        const int right = origin + layout.Pixels(x + width - 6);
        ui::MoveLayoutWindow(control, left, y, std::max(1, right - left), layout.Pixels(height), deferPaint);
    };
    const auto top = [&](int y) { return layout.Pixels(y); };
    move(400, side / 2 - 80, top(21), 70, 24);
    move(401, side / 2 + 10, top(21), 70, 24);

    const int stepOffset = layout.timelineOffset / 2;
    move(418, side / 2 - 49, top(67) + stepOffset, 20, 24);
    move(419, side / 2 + 29, top(67) + stepOffset, 20, 24);
    move(533, side / 2 - 79, top(69) + stepOffset, 20, 22);
    move(532, side / 2 + 59, top(69) + stepOffset, 20, 22);
    move(417, side / 2 - 27, top(66) + stepOffset, 54, 24);
    move(559, side - 30, top(69) + stepOffset, 20, 22);
    move(558, 15, top(69) + stepOffset, 20, 22);

    const int scrollTop = layout.GridTop() + layout.Pixels(17);
    ui::MoveLayoutControl(hwnd, 427, sidebar - layout.Pixels(19), scrollTop,
        layout.Pixels(16), std::max(0, layout.timelineBottom - scrollTop), deferPaint);
    move(428, 97, layout.timelineBottom + top(1), side - 116, 16);
    move(429, 8, layout.timelineBottom + top(1), 80, 18);

    if (GetDlgItem(hwnd, panel::kForcePasteButton)) {
        const int shift = panel::TimelinePanelShift(side);
        const bool wrap = shift != 0;
        const int y = layout.FooterY(224 + shift);
        const int start = wrap ? side / 2 - 119 : std::min(side / 2 - 119, side - 322);
        const int ids[] = {420, 421, 422, 416, 423};
        const int offsets[] = {0, 43, 86, 134, 189};
        for (int i = 0; i < 5; ++i)
            move(ids[i], start + offsets[i], y, i < 3 ? 40 : 50, 18);
        move(panel::kForcePasteButton, wrap ? side / 2 + 48 : start + 244, layout.FooterY(224), 72, 18);
    } else {
        move(416, side / 2 + 15, layout.FooterY(224), 50, 18);
        move(423, side / 2 + 70, layout.FooterY(224), 50, 18);
        move(420, side / 2 - 119, layout.FooterY(224), 40, 18);
        move(421, side / 2 - 76, layout.FooterY(224), 40, 18);
        move(422, side / 2 - 33, layout.FooterY(224), 40, 18);
    }
    move(425, side / 2 - 3, layout.FooterY(201), 50, 14);
    move(426, side / 2 + 69, layout.FooterY(201), 50, 14);
    move(434, side / 2 - 118, layout.FooterY(193), 100, 400);
    move(415, side / 2 - 4, layout.FooterY(181), 60, 18);
    move(424, side / 2 + 60, layout.FooterY(181), 60, 18);
}

// Visibility re-toggle loop body (0x444B4A..0x444B6F and the four siblings).
void ToggleVisible(HWND parent, int firstId, int lastId) {
    for (int id = firstId; id <= lastId; ++id) {
        HWND ctrl = GetDlgItem(parent, id);
        if (IsWindowVisible(ctrl)) {
            ShowWindow(ctrl, SW_HIDE);
            ShowWindow(ctrl, SW_SHOW);
        }
    }
}

}  // namespace

void PanelPaint(MMDApp* app);  // VA 0x00414610 (defined in ui_panel_paint.cpp)
void LayoutViewportPanels(MMDApp* app);   // VA 0x0040CAC0, was Sub40CAC0
                                          // (ui_viewport_layout.cpp)

void HandleWindowSize(MMDApp* app) {
    if (!app->bottomPanelSizing.relayout) updates::RequestViewport(app, true);
    ui_metrics::Add(ui_metrics::Layout);
    auto& s = *app;
    const bool deferPaint = s.bottomPanelSizing.relayout || (!MMD_ORIGINAL_PROFILE && ui::LayoutBatch::IsCollecting());
    const auto MoveCtrl = [deferPaint](HWND parent, int id, int x, int y, int w, int h) {
        ui::MoveLayoutControl(parent, id, x, y, w, h, deferPaint);
    };
    const HWND hwnd = static_cast<HWND>(app->Hwnd());  // 657080 (0xA06B8)
    const WindowDpiScope dpi(hwnd);

    if (s.FullscreenMode() != 0) {
        plugins::Layout(app);
        RefreshSeparateWindowViewport(app);
        return;
    }

    // Ordinary-window layout remains in its original 96-DPI coordinates.
    // D3D limits apply to the client surface, not the title bar and borders.
    // Before device initialization, allow modern desktop sizes as well.
    LONG maxClientWidth = 16384, maxClientHeight = 16384;
    if (const D3DRenderer* renderer = app->Renderer()) {
        if (renderer->maxTextureWidth > 0) maxClientWidth = renderer->maxTextureWidth;
        if (renderer->maxTextureHeight > 0) maxClientHeight = renderer->maxTextureHeight;
        // Ordinary rendering uses a backbuffer sized to the largest monitor.
        // During capture, its saved dimensions still describe the editor window.
        const UINT backingWidth = app->RecordingWindow()
            ? static_cast<UINT>(std::max(0, app->state.recRTW))
            : renderer->presentParameters.BackBufferWidth;
        const UINT backingHeight = app->RecordingWindow()
            ? static_cast<UINT>(std::max(0, app->state.recRTH))
            : renderer->presentParameters.BackBufferHeight;
        if (backingWidth) maxClientWidth = std::min<UINT>(maxClientWidth, backingWidth);
        if (backingHeight) maxClientHeight = std::min<UINT>(maxClientHeight, backingHeight);
    }
    RECT wr{}, client{};
    if (GetWindowRect(hwnd, &wr) && GetClientRect(hwnd, &client)) {
        const LONG width = wr.right - wr.left;
        const LONG height = wr.bottom - wr.top;
        const LONG excessWidth = std::max(0L, client.right - client.left - maxClientWidth);
        const LONG excessHeight = std::max(0L, client.bottom - client.top - maxClientHeight);
        if (excessWidth || excessHeight)
            SetWindowPos(hwnd, nullptr, 0, 0, width - excessWidth, height - excessHeight,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // --- sidebar width (0x4433A4..0x4433F2) --------------------------------
    RECT cr1;
    GetClientRect(hwnd, &cr1);  // var_10 - only .right feeds the sidebar
    std::int32_t& sidebar = s.SidebarWidth();
    if (s.bottomPanelSizing.relayout) {
        // A bottom splitter changes neither the client rectangle nor sidebar
        // geometry. Avoid a lossy ratio-to-pixel round trip during this batch.
    } else if (s.FloatingWindow() != nullptr) {
        sidebar = cr1.right - 3;                               // 0x4433B8
        PostLanguageSweep(app);                                // 0x4433BE
    } else {
        std::int32_t newSidebar = static_cast<std::int32_t>(
            static_cast<double>(s.SidebarRatio()) *
            cr1.right);                                        // 0x4433CF
        // An unchanged client can encode the exact sidebar width in a float
        // just below its pixel boundary. Keep the drag result in that case.
        if (!build_profile::original && cr1.right > 0 &&
            s.SidebarRatio() == static_cast<float>(static_cast<double>(sidebar) / cr1.right))
            newSidebar = sidebar;
        const int minimum = MainPanelLayout(app, cr1.bottom).Pixels(250);
        if (newSidebar < minimum) newSidebar = minimum;                                // 0x4433D9
        const bool grew = sidebar < newSidebar;                        // 0x4433E0
        sidebar = newSidebar;                                          // 0x4433E6
        if (grew && ui_scale::Percent() == 100) PanelPaint(app); // 0x4433EC
    }
    const std::int32_t side = sidebar;
    // Plugin mounts depend on client/sidebar geometry, not bottom extra widths.
    if (!s.bottomPanelSizing.relayout) plugins::Layout(app);
    RECT cb;                                                   // var_30
    GetClientRect(hwnd, &cb);                                  // 0x443563
    const std::int32_t bottom = cb.bottom;

    LayoutSidebarRows(app, hwnd, side, bottom, deferPaint);
    MoveCtrl(hwnd, 432, 144, bottom - 26, 63, 18);
    MoveCtrl(hwnd, 431, 144, bottom - 52, 63, 18);
    MoveCtrl(hwnd, 430, 144, bottom - 78, 63, 18);
    MoveCtrl(hwnd, 433, 144, bottom - 135, 63, 120);
    MoveCtrl(hwnd, 530, 144, bottom - 105, 13, 13);
    MoveCtrl(hwnd, 436, 225, bottom - 138, 125, 200);
    MoveCtrl(hwnd, 435, 224, bottom - 112, 60, 21);
    MoveCtrl(hwnd, 437, 291, bottom - 112, 60, 21);
    MoveCtrl(hwnd, 438, 291, bottom - 35, 60, 28);
    MoveCtrl(hwnd, 439, 225, bottom - 84, 13, 13);
    MoveCtrl(hwnd, 440, 267, bottom - 86, 40, 18);
    MoveCtrl(hwnd, 441, 310, bottom - 86, 41, 18);
    // Leave room for the two-character Chinese outside-parent caption.
    MoveCtrl(hwnd, 442, 316, bottom - 62, 34, 22);
    MoveCtrl(hwnd, 443, 225, bottom - 61, 88, 100);
    MoveCtrl(hwnd, 444, 227, bottom - 36, 13, 13);
    MoveCtrl(hwnd, 445, 227, bottom - 19, 13, 13);
    MoveCtrl(hwnd, 451, 368, bottom - 139, 70, 23);
    MoveCtrl(hwnd, 446, 443, bottom - 139, 65, 23);
    MoveCtrl(hwnd, 448, 477, bottom - 111, 30, 14);
    MoveCtrl(hwnd, 447, 363, bottom - 95, 151, 20);
    MoveCtrl(hwnd, 452, 463, bottom - 40, 45, 33);
    MoveCtrl(hwnd, 449, 368, bottom - 53, 90, 100);
    MoveCtrl(hwnd, 450, 368, bottom - 28, 90, 300);
    MoveCtrl(hwnd, 454, 363, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 453, 363, bottom - 155, 14, 14);

    // --- option band 1: base bandXBase (original v52; 0x443B02..0x443D9E) --
    // bandXBase = -18, or 117 while bone mode is off (optflag[0] set) and the
    // expand toggle optflag[1] is clear.
    const std::uint8_t opt0 = s.CameraMode();     // 760
    const std::uint8_t opt1 = s.UiOptionFlag(1);  // 761
    const std::uint8_t opt2 = s.UiOptionFlag(2);  // 762
    const std::uint8_t opt3 = s.UiOptionFlag(3);  // 763
    const std::uint8_t opt4 = s.UiOptionFlag(4);  // 764
    const std::uint8_t opt5 = s.UiOptionFlag(5);  // 765
    const std::uint8_t opt6 = s.UiOptionFlag(6);  // 766

    std::int32_t bandXBase = -18;
    if (opt0 != 0 && opt1 == 0) bandXBase = 117;                   // 0x443B1A
    MoveCtrl(hwnd, 455, 514 - bandXBase, bottom - 140, 136, 18);
    MoveCtrl(hwnd, 461, 650 - bandXBase, bottom - 140, 23, 14);
    MoveCtrl(hwnd, 456, 514 - bandXBase, bottom - 122, 136, 18);
    MoveCtrl(hwnd, 462, 650 - bandXBase, bottom - 122, 23, 14);
    MoveCtrl(hwnd, 457, 514 - bandXBase, bottom - 104, 136, 18);
    MoveCtrl(hwnd, 463, 650 - bandXBase, bottom - 104, 23, 14);
    // Keep the native 20px thumb/tick area, with disjoint sibling windows.
    // The old 18px pitch let each direction slider repaint two rows of its
    // neighbour even while that neighbour's thumb and paint count were idle.
    MoveCtrl(hwnd, 458, 514 - bandXBase, bottom - 86, 132, 20);
    MoveCtrl(hwnd, 464, 646 - bandXBase, bottom - 84, 27, 14);
    MoveCtrl(hwnd, 459, 514 - bandXBase, bottom - 66, 132, 20);
    MoveCtrl(hwnd, 465, 646 - bandXBase, bottom - 66, 27, 14);
    MoveCtrl(hwnd, 460, 514 - bandXBase, bottom - 46, 132, 20);
    MoveCtrl(hwnd, 466, 646 - bandXBase, bottom - 48, 27, 14);
    MoveCtrl(hwnd, 468, 590 - bandXBase, bottom - 26, 75, 20);
    MoveCtrl(hwnd, 467, 524 - bandXBase, bottom - 26, 50, 20);
    MoveCtrl(hwnd, 470, 501 - bandXBase, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 469, 501 - bandXBase, bottom - 155, 14, 14);

    // --- option band 2: bandXBase += 162 with expand toggle optflag[2] (0x443DB3)
    if (opt0 != 0 && opt2 == 0) bandXBase += 162;
    MoveCtrl(hwnd, 567, 684 - bandXBase, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 566, 684 - bandXBase, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 562, 688 - bandXBase, bottom - 135, 54, 22);
    MoveCtrl(hwnd, 563, 745 - bandXBase, bottom - 135, 54, 22);
    MoveCtrl(hwnd, 564, 802 - bandXBase, bottom - 135, 54, 22);
#if !MMD_ORIGINAL_PROFILE
    MoveCtrl(hwnd, panel::kAdaptiveShadowCheckbox, 688 - bandXBase, bottom - 112, 84, 18);
    MoveCtrl(hwnd, panel::kShadowFilterCheckbox, 775 - bandXBase, bottom - 112, 83, 18);
#endif
    MoveCtrl(hwnd, 560, 683 - bandXBase, bottom - 75, 176, 20);
    MoveCtrl(hwnd, 561, 815 - bandXBase, bottom - 92, 35, 14);
    MoveCtrl(hwnd, 565, 745 - bandXBase, bottom - 36, 54, 30);

    // --- option band 3: bandXBase += 160 with expand toggle optflag[6] (0x443F19)
    if (opt0 != 0 && opt6 == 0) bandXBase += 160;
    MoveCtrl(hwnd, 489, 864 - bandXBase, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 488, 864 - bandXBase, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 471, 868 - bandXBase, bottom - 140, 125, 150);
    MoveCtrl(hwnd, 472, 867 - bandXBase, bottom - 116, 60, 18);
    MoveCtrl(hwnd, 473, 934 - bandXBase, bottom - 116, 60, 18);
    MoveCtrl(hwnd, 476, 1000 - bandXBase, bottom - 142, 13, 13);
    MoveCtrl(hwnd, 486, 1000 - bandXBase, bottom - 127, 13, 13);
    MoveCtrl(hwnd, 477, 999 - bandXBase, bottom - 113, 53, 17);
    MoveCtrl(hwnd, 474, 868 - bandXBase, bottom - 95, 90, 100);
    MoveCtrl(hwnd, 475, 962 - bandXBase, bottom - 95, 90, 300);
    MoveCtrl(hwnd, 478, 877 - bandXBase, bottom - 70, 48, 14);
    MoveCtrl(hwnd, 479, 940 - bandXBase, bottom - 70, 48, 14);
    MoveCtrl(hwnd, 480, 1003 - bandXBase, bottom - 70, 48, 14);
    MoveCtrl(hwnd, 481, 877 - bandXBase, bottom - 50, 48, 14);
    MoveCtrl(hwnd, 482, 940 - bandXBase, bottom - 50, 48, 14);
    MoveCtrl(hwnd, 483, 1003 - bandXBase, bottom - 50, 48, 14);
    MoveCtrl(hwnd, 484, 877 - bandXBase, bottom - 30, 48, 14);
    MoveCtrl(hwnd, 485, 940 - bandXBase, bottom - 30, 48, 14);
    MoveCtrl(hwnd, 487, 995 - bandXBase, bottom - 32, 58, 26);
    curve_host::LayoutControls(app);

    // --- option band 4: bandXBase += 178 with expand toggle optflag[3] (0x44422B);
    // band 4 controls use fixed X -------------------------------------------
    if (opt0 != 0 && opt3 == 0) bandXBase += 178;
    MoveCtrl(hwnd, 503, 363, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 502, 363, bottom - 155, 14, 14);
    // MMS bone-operation mode grid: 3 columns by 4 rows, driven by the table in
    // panel_controls.hpp so the layout and its regression test share the
    // numbers.  Row 1 cell 1 is one merged select / box-select cycle button
    // (490; the mode state and its caption come from RefreshBoneModePanel),
    // cell 2 is select-all (494) and cell 3 is select-unregistered (501).
    // Row 2 is move (492), rotate (493) and the MMS bone scale mode (491).
    // Both mode rows are 28 tall - the original's mode-row height - which gives
    // the four-glyph Japanese captions of 494/501 their margins back; rows 3
    // and 4 keep the original geometry.
    for (const panel::BoneModeCell& cell : panel::kBoneModeGrid)
        MoveCtrl(hwnd, cell.id, cell.x, bottom - cell.yOffset, cell.width,
                 cell.height);

    // --- option band 5: second base bandXBase2 (original v110;
    // 0x44444C..0x4448BE) ----------------------------------------------------
    // optflag[4]/optflag[0] both clear -> bandXBase += 179; then bandXBase2 = bandXBase - 180
    // (bone mode on) or bandXBase + 53; optflag[5]/optflag[0] both clear -> bandXBase2 +=
    // 251; optflag[0] clear -> bandXBase2 += 13.
    if (opt4 == 0 && opt0 == 0) bandXBase += 179;                   // 0x444453
    std::int32_t bandXBase2 = opt0 != 0 ? bandXBase - 180 : bandXBase + 53;     // 0x44445D
    MoveCtrl(hwnd, 529, 598 - bandXBase2, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 528, 598 - bandXBase2, bottom - 155, 14, 14);
    MoveCtrl(hwnd, 509, 618 - bandXBase2, bottom - 120, 92, 500);
    MoveCtrl(hwnd, 512, 602 - bandXBase2, bottom - 121, 16, 22);
    MoveCtrl(hwnd, 513, 710 - bandXBase2, bottom - 121, 16, 22);
    MoveCtrl(hwnd, 510, 597 - bandXBase2, bottom - 100, 135, 25);
    MoveCtrl(hwnd, 511, 642 - bandXBase2, bottom - 138, 28, 14);
    MoveCtrl(hwnd, 504, 618 - bandXBase2, bottom - 50, 92, 500);
    MoveCtrl(hwnd, 507, 602 - bandXBase2, bottom - 51, 16, 22);
    MoveCtrl(hwnd, 508, 710 - bandXBase2, bottom - 51, 16, 22);
    MoveCtrl(hwnd, 505, 597 - bandXBase2, bottom - 30, 135, 25);
    MoveCtrl(hwnd, 506, 642 - bandXBase2, bottom - 68, 28, 14);
    MoveCtrl(hwnd, 514, 749 - bandXBase2, bottom - 120, 92, 500);
    MoveCtrl(hwnd, 517, 733 - bandXBase2, bottom - 121, 16, 22);
    MoveCtrl(hwnd, 518, 841 - bandXBase2, bottom - 121, 16, 22);
    MoveCtrl(hwnd, 515, 729 - bandXBase2, bottom - 100, 135, 25);
    MoveCtrl(hwnd, 516, 773 - bandXBase2, bottom - 138, 28, 14);
    MoveCtrl(hwnd, 519, 749 - bandXBase2, bottom - 50, 92, 500);
    MoveCtrl(hwnd, 522, 733 - bandXBase2, bottom - 51, 16, 22);
    MoveCtrl(hwnd, 523, 841 - bandXBase2, bottom - 51, 16, 22);
    MoveCtrl(hwnd, 520, 729 - bandXBase2, bottom - 30, 135, 25);
    MoveCtrl(hwnd, 521, 773 - bandXBase2, bottom - 68, 28, 14);
    MoveCtrl(hwnd, 524, 675 - bandXBase2, bottom - 141, 50, 19);
    MoveCtrl(hwnd, 525, 675 - bandXBase2, bottom - 71, 50, 19);
    MoveCtrl(hwnd, 526, 807 - bandXBase2, bottom - 71, 50, 19);
    MoveCtrl(hwnd, 527, 807 - bandXBase2, bottom - 141, 50, 19);

    // The four expression launchers take the left 22 px of the four panel
    // selectors and leave each selector 70 px, so its right edge stays against
    // the forward arrow that this band owns.  This runs after every MoveCtrl of
    // the band, because the selectors are re-placed by the option-band passes
    // above and narrowing them twice would be cumulative.
    curve_host::LayoutMorphControls(app);

    // The original updates bandXBase2 only after the complete facial group above;
    // the adjusted base belongs exclusively to the following view/play band.
    if (opt5 == 0 && opt0 == 0) bandXBase2 += 251;                  // 0x4448A8
    if (opt0 == 0) bandXBase2 += 13;                                // 0x444895
    MoveCtrl(hwnd, 402, 886 - bandXBase2, bottom - 141, 38, 17);
    MoveCtrl(hwnd, 403, 925 - bandXBase2, bottom - 141, 37, 17);
    MoveCtrl(hwnd, 404, 963 - bandXBase2, bottom - 141, 37, 17);
    MoveCtrl(hwnd, 405, 886 - bandXBase2, bottom - 121, 38, 17);
    MoveCtrl(hwnd, 406, 925 - bandXBase2, bottom - 121, 37, 17);
    MoveCtrl(hwnd, 407, 963 - bandXBase2, bottom - 121, 37, 17);
    MoveCtrl(hwnd, 408, 887 - bandXBase2, bottom - 63, 50, 24);
    MoveCtrl(hwnd, 409, 888 - bandXBase2, bottom - 36, 50, 14);
    MoveCtrl(hwnd, 410, 952 - bandXBase2, bottom - 36, 50, 14);
    MoveCtrl(hwnd, 411, 952 - bandXBase2, bottom - 58, 13, 13);
    // MMS: the model / bone references are checkboxes carrying their own
    // captions (they were 13x13 boxes with labels painted beside them), so the
    // caption is clickable too.
    MoveCtrl(hwnd, 412, 890 - bandXBase2, bottom - 99, 56, 14);
    MoveCtrl(hwnd, 531, 950 - bandXBase2, bottom - 99, 50, 14);
    MoveCtrl(hwnd, 413, 952 - bandXBase2, bottom - 19, 13, 13);
    MoveCtrl(hwnd, 414, 888 - bandXBase2, bottom - 19, 13, 13);
#if !MMD_ORIGINAL_PROFILE
    // Keep enough title space in every language, including a live switch to Chinese.
    MoveCtrl(hwnd, 534, 1017 - bandXBase2, bottom - 61, 22, 55);
#else
    MoveCtrl(hwnd, 534, 1017 - bandXBase2, bottom - 63, 22, 57);
#endif
    MoveCtrl(hwnd, 535, 1002 - bandXBase2, bottom - 141, 35, 37);

    ResizeBottomPanelControls(app);

    // --- visibility re-toggle, five id ranges (0x444B3B..0x444C99) ---------
    if (!deferPaint) {
        ToggleVisible(hwnd, 455, 469);
        ToggleVisible(hwnd, 471, 488);
        ToggleVisible(hwnd, 504, 528);
        ToggleVisible(hwnd, 402, 414);
        ToggleVisible(hwnd, 531, 535);
    }

    // --- refresh tail (0x444C9D..0x444CB0) ---------------------------------
    RefreshMainWindowViewport(app);
    LayoutViewportPanels(app);  // original tail call: `return sub_40CAC0(this);`
    if (ui_scale::Percent() != 100 && !s.bottomPanelSizing.relayout && s.PanelDC() &&
        !s.SceneMutationInProgress() && !s.sceneRecoveryPending) {
        PanelPaint(app);
    }
}

// was Sub442EB0, VA 0x00442EB0 - live relayout of the sidebar-anchored
// controls (top tool row 400..417/532/533/558/559 plus the bottom frame
// grid 415..434) at the current sidebar width and client height; the
// visibility re-toggle refreshes those bands without the full WM_SIZE
// option-band pass.  Called by the sidebar-resize drag (HandleMouseMove
// 0x444CC0), the scene loaders and the mic window procs.
void RelayoutSidebarControls(MMDApp* app) {
    if (app->FullscreenMode() != 0) return;
    const HWND hwnd = static_cast<HWND>(app->Hwnd());
    const int side = app->SidebarWidth();
    const bool deferPaint = app->SidebarResizeDragging() != 0 || (!MMD_ORIGINAL_PROFILE && ui::LayoutBatch::IsCollecting());
    RECT client{};
    GetClientRect(hwnd, &client);

    LayoutSidebarRows(app, hwnd, side, client.bottom, deferPaint);

    if (!deferPaint) {
        ToggleVisible(hwnd, 400, 401);
        ToggleVisible(hwnd, 415, 426);
        ToggleVisible(hwnd, 558, 559);
        ToggleVisible(hwnd, 434, 434);
    }
    RefreshMainWindowViewport(app);
    LayoutViewportPanels(app);
}

namespace {
void ApplySidebarGeometry(MMDApp& app, int width, float ratio) {
    if (width == app.SidebarWidth() && ratio == app.SidebarRatio()) return;
    const auto epoch = app.sidebarSizingSerial;
    const auto bottomEpoch = app.bottomPanelSizingSerial;
    const auto token = app.sidebarSizing.layoutToken;
    const auto active = app.sidebarSizing.active;
    auto current = [&] {
        if (build_profile::original) return true;
        const bool side = (app.sidebarSizingSerial == epoch && app.sidebarSizing.layoutToken == token &&
            app.sidebarSizing.active == active) || (!app.sidebarSizing.active && !app.sidebarSizing.preparing);
        const bool bottom = app.bottomPanelSizingSerial == bottomEpoch ||
            (app.bottomPanelSizing.dragging < 0 && !app.bottomPanelSizing.preparing);
        // A failed nested attempt owns no new gesture and must not strand a
        // partially applied batch. Continue only if its requested geometry is
        // still current; an actual newer gesture or changed size wins.
        return side && bottom && app.SidebarWidth() == width && app.SidebarRatio() == ratio;
    };
    struct ReadyScope {
        MMDApp& app;
        std::uint64_t side, bottom, token;
        bool active, completed = false;
        unsigned char previous;
        ~ReadyScope() {
            if (build_profile::original || completed) return;
            const bool newerSide = (app.sidebarSizingSerial != side ||
                app.sidebarSizing.layoutToken != token || app.sidebarSizing.active != active) &&
                (app.sidebarSizing.active || app.sidebarSizing.preparing);
            const bool newerBottom = app.bottomPanelSizingSerial != bottom &&
                (app.bottomPanelSizing.dragging >= 0 || app.bottomPanelSizing.preparing);
            // A failed nested start changes the serial without acquiring a
            // gesture. Do not strand this scope's temporary not-ready flag.
            if (!newerSide && !newerBottom) app.WindowLayoutReady() = previous;
        }
    } ready{app,epoch,bottomEpoch,token,active,false,app.WindowLayoutReady()};
    app.WindowLayoutReady() = 0;
    app.SidebarWidth() = width;
    app.SidebarRatio() = ratio;
    // The compatibility profile keeps its original immediate moves and paints.
    if (build_profile::original) {
        plugins::Layout(&app);
        RelayoutSidebarControls(&app);
        PanelPaint(&app);
        app.WindowLayoutReady() = 1;
        return;
    }
    // Plugin mounts use the same current sidebar/client geometry as the scene.
    // Their own layout applies before the viewport consumes its reserved height.
    ui::LayoutBatch batch;
    plugins::Layout(&app);
    if (!current()) return;
    RelayoutSidebarControls(&app);
    if (!current()) return;
    batch.Commit();
    if (!current()) return;
    PanelPaint(&app);
    if (!current()) return;
    app.WindowLayoutReady() = 1;
    ready.completed = true;
    RECT damage{};GetClientRect(app.MainWindow(),&damage);
    damage.bottom = std::max(0, MainPanelLayout(&app, damage.bottom).bottomTop + 3);
    ui::RepaintLayout(app.MainWindow(),batch.Changes(),damage);
}
}
void UpdateSidebarResize(MMDApp* app) {
    if (!app || !app->sidebarSizing.active || !app->SidebarResizeDragging() ||
        app->FullscreenMode() || app->FloatingWindow()) return;
    RECT client{};
    if (!GetClientRect(app->MainWindow(), &client) || client.right <= 5) return;
    const int width = std::min(std::max(MainPanelLayout(app, client.bottom).Pixels(250), app->MouseX()), int(client.right) - 5);
    ApplySidebarGeometry(*app, width, float(double(width) / client.right));
}
void CommitSidebarResize(MMDApp* app) {
    if (!app || !app->sidebarSizing.active) return;
    const auto start = app->sidebarSizing;
    const auto epoch = app->sidebarSizingSerial;
    const auto bottomEpoch = app->bottomPanelSizingSerial;
    UpdateSidebarResize(app);
#if !MMD_ORIGINAL_PROFILE
    if (app->sidebarSizingSerial != epoch || app->sidebarSizing.epoch != start.epoch ||
        app->sidebarSizing.layoutToken != start.layoutToken || !app->sidebarSizing.active) return;
    if (app->bottomPanelSizingSerial != bottomEpoch &&
        (app->bottomPanelSizing.dragging >= 0 || app->bottomPanelSizing.preparing)) return;
#endif
    // ReleaseCapture synchronously sends WM_CAPTURECHANGED. Retire the gesture
    // first so that its cleanup cannot restore the committed starting geometry.
    app->sidebarSizing = {};
    app->SidebarResizeDragging() = 0;
    app->WindowLayoutReady() = 1;
#if !MMD_ORIGINAL_PROFILE
    plugins::EndLayoutInteraction(app,start.layoutToken);
#endif
}
void CancelSidebarResize(MMDApp* app) {
#if !MMD_ORIGINAL_PROFILE
    if (app && app->sidebarSizing.preparing) {
        app->sidebarSizing.preparing = false;
        ++app->sidebarSizingSerial; // Invalidate the Begin continuation; it owns the local token.
        return;
    }
#endif
    if (!app || !app->sidebarSizing.active) return;
    const auto start = app->sidebarSizing;
    app->sidebarSizing = {};
    // Retain the batching flag until the restored controls have been repainted.
    app->SidebarResizeDragging() = 1;
    ApplySidebarGeometry(*app, start.startWidth, start.startRatio);
#if !MMD_ORIGINAL_PROFILE
    if (!app->sidebarSizing.active && !app->sidebarSizing.preparing && !app->sidebarSizing.layoutToken) {
#endif
    app->SidebarResizeDragging() = 0;
#if !MMD_ORIGINAL_PROFILE
    // The retired sidebar owns this flag, but a new bottom gesture owns the
    // global readiness state. A failed nested attempt owns neither.
    if (app->bottomPanelSizing.dragging < 0 && !app->bottomPanelSizing.preparing)
#endif
    app->WindowLayoutReady() = 1;
#if !MMD_ORIGINAL_PROFILE
    }
    plugins::EndLayoutInteraction(app,start.layoutToken);
#endif
}

}  // namespace mikumikustudio
