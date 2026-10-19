#include "mikumikustudio/build_profile.hpp"
// ===========================================================================
// VA 0x0040CAC0 - LayoutViewportPanels  (original: sub_40CAC0, 0x5A9 bytes,
// was Sub40CAC0)
// ===========================================================================
// Repositions the camera/light/accessory overlay strip at the top of the 3D
// viewport (ids 536..550) and the window-top button row (ids 551..557).
// Called from HandleWindowSize (0x443300 tail), the command dispatch
// (0x47E8A0 case 212), sub_442EB0 and MicWndProc (0x466A10).
//
// Base rect: the separate-viewport window when this+0xA0D38 is a live HWND,
// otherwise the main window with a left inset of sidebar(this+0xA06C8)+9.
// The camera strip anchors left (x = inset + K, shown while inset+K <=
// right), the button row anchors right (x = right - K, shown while
// right-K >= inset); controls that do not fit are parked off-screen at
// (-9000,-9000) with size 1x1, exactly like the original.
// Y bases come from the viewport hide-rect fields: strip rows sit at
// this+0xA0D4C + 3..11, buttons at this+0xA0D44 - 22 (or -19 for id 554).
// Tail: ids 536..557 get the IsWindowVisible -> SW_HIDE -> SW_SHOW
// re-toggle so repainted overlays pick up their new positions.
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "ui_control_layout.hpp"

namespace mikumikustudio {

namespace {
int scaleCaptionWidths[3]{};

HFONT ScaleLabelBoldFont(MMDApp* app) {
    // Match the 14px toolbar captions drawn through DrawUiGlyph. Only the
    // weight changes; using the system's smaller 12px control font made this
    // otherwise identical caption look undersized.
    static HFONT cached[3]{};
    const int language = app->EnglishUI();
    const int slot = language >= kUiJapanese && language <= kUiChinese
        ? language : kUiJapanese;
    if (cached[slot] != nullptr)
        return cached[slot];

    LOGFONTW font{};
    font.lfHeight = -14;
    font.lfWeight = FW_BOLD;
    if (slot == kUiChinese) {
        font.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    } else if (slot == kUiEnglish) {
        font.lfCharSet = SHIFTJIS_CHARSET;
        wcscpy_s(font.lfFaceName, L"Arial");
    } else {
        font.lfCharSet = SHIFTJIS_CHARSET;
        wcscpy_s(font.lfFaceName, L"MS PGothic");
    }
    cached[slot] = CreateFontIndirectW(&font);
    return cached[slot];
}

int ScaleLabelTextWidth(HWND label, MMDApp* app) {
    if (label == nullptr)
        return 36;
    wchar_t text[64]{};
    if (GetWindowTextW(label, text, 64) <= 0)
        wcscpy_s(text, L"Scale");
    HDC dc = GetDC(label);
    if (dc == nullptr)
        return 36;
    HFONT font = ScaleLabelBoldFont(app);
    HGDIOBJ previous = font != nullptr ? SelectObject(dc, font) : nullptr;
    SIZE extent{};
    GetTextExtentPoint32W(dc, text, lstrlenW(text), &extent);
    if (previous != nullptr)
        SelectObject(dc, previous);
    ReleaseDC(label, dc);
    // Static text has no edit margins; keep only a 3px visual gap before X.
    return std::max(20, static_cast<int>(extent.cx) + 3);
}

}  // namespace

int ViewportScaleCaptionWidth(MMDApp* app) {
    const HWND window = app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow();
    const HWND label = GetDlgItem(window, 570);
    if (!label) return 36;
    // The label has a fixed font and only changes with the UI language.
    const int language = std::clamp<int>(app->EnglishUI(), 0, 2);
    if (!scaleCaptionWidths[language]) scaleCaptionWidths[language] = ScaleLabelTextWidth(label, app);
    return scaleCaptionWidths[language];
}

void LayoutViewportPanels(MMDApp* app) {  // was Sub40CAC0, VA 0x0040CAC0
    if (app->FullscreenMode() != 0) return;
    auto& s = *app;

    HWND separate = s.FloatingWindow();                           // 658744
    const WindowDpiScope dpi(separate ? separate : s.MainWindow());
    HWND base;
    int inset;
    RECT rc;
    if (separate != nullptr) {                                    // 0x40CADB
        base = separate;
        GetClientRect(base, &rc);
        inset = 0;
    } else {
        base = s.MainWindow();                                    // 657080
        GetClientRect(base, &rc);
        inset = s.SidebarWidth() + 9;                             // 657096
    }
    const int right = rc.right;
    if (const HWND label = GetDlgItem(base, 570))
        scaleCaptionWidths[std::clamp<int>(app->EnglishUI(), 0, 2)] = ScaleLabelTextWidth(label, app);
    const auto panels = FramePanelLayout(app);
    const RECT viewport = panels.overlay;
    const int stripY = viewport.bottom;                           // 658764
    const int rowY = viewport.top;                                // 658756
    const bool deferPaint = app->SidebarResizeDragging() != 0;
    auto move = [&](int id, int x, int y, int w, int h) {
        ui::MoveLayoutControl(base, id, x, y, w, h, deferPaint);
    };
    const auto fontScale = [&](int id, float scale) {
        if (MMD_ORIGINAL_PROFILE) return;
        const HWND control = GetDlgItem(base, id);
        if (!control) return;
        ApplyUiFontToControl(app, control, id);
        HFONT font = id == 570 ? ScaleLabelBoldFont(app) :
            reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        if (!font) return;
        if (scale < 1.0f) {
            LOGFONTW definition{};
            if (GetObjectW(font, sizeof(definition), &definition)) {
                const int height = (definition.lfHeight < 0 ? -1 : 1) *
                    std::max(1, ViewportPanelLayout::Pixels(std::abs(definition.lfHeight) * scale));
                static std::map<std::pair<HFONT, int>, HFONT> fonts;
                auto& scaled = fonts[{font, height}];
                if (!scaled) { definition.lfHeight = height; scaled = CreateFontIndirectW(&definition); }
                if (scaled) font = scaled;
            }
        }
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    };
    const auto controlHeight = [&](int id, int requested) {
        const HWND control = GetDlgItem(base, id);
        if (!control) return requested;
        const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        static std::map<HFONT, int> fontHeights;
        auto& height = fontHeights[font];
        if (!height) {
            const HDC dc = GetDC(control);
            if (!dc) return requested;
            const auto previous = font ? SelectObject(dc, font) : nullptr;
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            height = metrics.tmHeight;
            if (previous) SelectObject(dc, previous);
            ReleaseDC(control, dc);
        }
        RECT edge{0, 0, 1, 1};
        AdjustWindowRectEx(&edge, GetWindowLongW(control, GWL_STYLE), FALSE,
            GetWindowLongW(control, GWL_EXSTYLE));
        wchar_t className[16]{};
        GetClassNameW(control, className, 16);
        const bool edit = _wcsicmp(className, L"Edit") == 0;
        const int padding = edit ? 2 : _wcsicmp(className, L"Button") == 0 ? 4 : 0;
        return std::max(requested, height + int(edge.bottom - edge.top - 1) + padding);
    };

    // Camera strip: x = inset + K, shown while inset + K <= right.
    auto moveLeft = [&](int id, int k, int dy, int w, int h) {
        if (MMD_ORIGINAL_PROFILE) {
            if (inset + k <= right) move(id, inset + k, stripY + dy, w, h);
            else move(id, -9000, -9000, 1, 1);
            return;
        }
        fontScale(id, panels.bottomScale);
        if (k >= 0) {
            const int height = controlHeight(id, ViewportPanelLayout::Pixels(h * panels.bottomScale));
            move(id, panels.BottomX(k), panels.BottomY(k, dy + h / 2) - height / 2,
                std::max(1, panels.BottomX(k + w) - panels.BottomX(k)),
                height);
        } else
            move(id, -9000, -9000, 1, 1);
    };
    // Top button row: x = right - K, shown while right - K >= inset.
    auto moveRight = [&](int id, int k, int dy, int w, int h) {
        if (MMD_ORIGINAL_PROFILE) {
            if (right - k >= inset) move(id, right - k, rowY + dy, w, h);
            else move(id, -9000, -9000, 1, 1);
            return;
        }
        fontScale(id, panels.topScale);
        const int height = controlHeight(id, ViewportPanelLayout::Pixels(h * panels.topScale));
        move(id, panels.TopX(k), panels.TopY(dy + h / 2) - height / 2,
            std::max(1, panels.TopX(k - w) - panels.TopX(k)), height);
    };

    moveLeft(536,   5,  3, 55, 27);   // 0x40CB25 model-select strip label
    moveLeft(537, 140,  9, 16, 17);   // X
    moveLeft(544, 157, 11, 53, 14);   // X value
    moveLeft(538, 213,  9, 16, 17);   // Y
    moveLeft(545, 230, 11, 53, 14);   // Y value
    moveLeft(539, 286,  9, 16, 17);   // Z
    moveLeft(546, 303, 11, 53, 14);   // Z value
    const bool modelScaleVisible = app->state.optflag[0] == 0;
    if (modelScaleVisible) {
        // Bone rotation values use the same 2-decimal text width as position
        // and scale. Reflow the Y/Z columns to keep 3px between each group.
        moveLeft(540, 400,  9, 16, 17);   // X
        moveLeft(547, 417, 11, 53, 14);
        moveLeft(541, 473,  9, 16, 17);   // Y
        moveLeft(548, 490, 11, 53, 14);
        moveLeft(542, 546,  9, 16, 17);   // Z
        moveLeft(549, 563, 11, 53, 14);
    } else {
        // Camera angle values use the same width as camera position. Move Y/Z
        // along the position-column pitch rather than squeezing the fields.
        moveLeft(540, 400,  9, 16, 17);   // X
        moveLeft(547, 417, 11, 53, 14);
        moveLeft(541, 473,  9, 16, 17);   // Y
        moveLeft(548, 490, 11, 53, 14);
        moveLeft(542, 546,  9, 16, 17);   // Z
        moveLeft(549, 563, 11, 53, 14);
    }
    if (modelScaleVisible) {
        moveLeft(543, -9000, 0, 1, 1);
        moveLeft(550, -9000, 0, 1, 1);
    } else {
        moveLeft(543, 619,  8, 36, 21);   // distance follows the widened angles
        moveLeft(550, 656, 11, 53, 14);
    }
#if !MMD_ORIGINAL_PROFILE
    // Position, rotation and scale share the same adaptable field width.
    // The scale caption alone is sized from its localized text.
    // Install the final font before MoveWindow repaints the caption. Drawing
    // once with the control font and again with the bold font caused ghosts.
    for (int id : {570, 571, 572, 574, 575, 576, 577}) {
        HWND control = GetDlgItem(base, id);
        if (!deferPaint && control != nullptr && app->state.hFontUI != nullptr) {
            ApplyUiFontToControl(app, control, id);
            if (id == 570)
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(ScaleLabelBoldFont(app)), FALSE);
        }
    }
    const int scaleTitleWidth = ViewportScaleCaptionWidth(app);
    const int scaleTitleX = 640;
    const int scaleEditWidth = 53;
    const int scaleXAxis = scaleTitleX + scaleTitleWidth;
    const int scaleXField = scaleXAxis + 17;
    const int scaleYAxis = scaleXField + scaleEditWidth + 3;
    const int scaleYField = scaleYAxis + 17;
    const int scaleZAxis = scaleYField + scaleEditWidth + 3;
    const int scaleZField = scaleZAxis + 17;
    const bool scaleFits = modelScaleVisible;
    auto moveScale = [&](int id, int x, int dy, int w, int h) {
        if (scaleFits)
            moveLeft(id, x, dy, w, h);
        else
            move(id, -9000, -9000, 1, 1);
    };
    moveScale(570, scaleTitleX, 8, scaleTitleWidth, 18);  // 14px toolbar font
    moveScale(571, scaleXAxis, 9, 16, 17);  // X
    moveScale(572, scaleXField, 11, scaleEditWidth, 14);
    moveScale(574, scaleYAxis,  9, 16, 17);  // 3px gap after the prior field
    moveScale(575, scaleYField, 11, scaleEditWidth, 14);
    moveScale(576, scaleZAxis,  9, 16, 17);
    moveScale(577, scaleZField, 11, scaleEditWidth, 14);
#endif

    moveRight(557, 140, -22, 50, 20); // 0x40D018 coordinate-axis toggle
    moveRight(552, 195, -22, 50, 20); // power-save
    moveRight(551, 250, -22, 50, 20); // info display
    moveRight(556, 305, -22, 50, 20);
    moveRight(553, 360, -22, 40, 20);
    moveRight(554, 398, -19, 35, 14); // frame edit box
    moveRight(555, 441, -22, 40, 20);

    // Hide/show would force intermediate paints while the splitter is moving.
    if (deferPaint) return;
    for (int i = 536; i <= 557; ++i) {                            // 0x40D053
        HWND overlay = GetDlgItem(base, i);
        if (IsWindowVisible(overlay)) {
            ShowWindow(overlay, SW_HIDE);
            ShowWindow(overlay, SW_SHOW);
        }
    }
#if !MMD_ORIGINAL_PROFILE
    for (int id : {570, 571, 572, 574, 575, 576, 577}) {
        HWND control = GetDlgItem(base, id);
        if (IsWindowVisible(control)) {
            ShowWindow(control, SW_HIDE);
            ShowWindow(control, SW_SHOW);
        }
    }
#endif
}

}  // namespace mikumikustudio
