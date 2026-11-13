// ===========================================================================
// VA 0x00466D20 phases 21-23 - CreateUiFont  (from sub_466D20)
// ===========================================================================
// Exact port of the font phase inside UI control creation (verified in the
// raw Ghidra listing at lines ~1700-1760 of fcn_00466d20.c):
//
//   SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, 0x154, &metrics, 0)
//   copy lfMessageFont (15 dwords) -> this+656508
//   english = *(byte*)(this+658252)
//   hFont = CreateFontA(
//       english ? 13 : 12,          // height (MS PGothic 12 / Tahoma 13)
//       0, 0, 0, 400,               // FW_NORMAL
//       0, 0, 0,
//       0x80,                       // SHIFTJIS_CHARSET
//       0, 0, 2,                    // PROOF_QUALITY
//       0x31,                       // FIXED_PITCH | FF_MODERN
//       english ? "Tahoma" : "MS PGothic")
//   this+656572 = hFont
//   ... every subsequent control creation issues
//       SendMessageA(ctrl, WM_SETFONT, hFont, TRUE)
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <algorithm>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/ui_scale.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/ui_control_layout.hpp"
#endif
namespace mikumikustudio {

void CreateUiFont(MMDApp* app, HWND hwnd) {
    auto& s = *app;

    NONCLIENTMETRICSA metrics;                                    // 0x467A47
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS /*0x29*/, 0x154,
                          &metrics, 0);
    // lfMessageFont copy (15 dwords = 60 bytes) -> this+656508
    std::memcpy(s.state.logFont,
                &metrics.lfMessageFont, sizeof(LOGFONTA));

    const unsigned char language = s.EnglishUI();
    const int slot = language <= kUiChinese ? language : kUiJapanese;
    // Keep these three shared fonts alive: modeless panels may still borrow a
    // previous language's font when the main window switches language.
    static HFONT fonts[3]{};
    if (!fonts[slot]) fonts[slot] = CreateFontW(
        slot == kUiChinese ? -12 : slot == kUiJapanese ? 12 : 13,
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, PROOF_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        slot == kUiChinese ? L"Microsoft YaHei UI"
        : slot == kUiEnglish ? L"Tahoma" : L"MS PGothic");
    s.state.hFontUI = fonts[slot];

}

namespace {
bool KeepClassFont(int id) {
    // Original sends WM_SETFONT immediately after each creation site.  The
    // frame edit and its six navigation buttons retain their class default.
    switch (id) {
    case 417:
    case 418:
    case 419:
    case 532:
    case 533:
    case 558:
    case 559:
        return true;
    default:
        return false;
    }
}
}

HFONT UiFontForControl(MMDApp* app, HWND control, int id) {
    if (!app || !control || KeepClassFont(id)) return nullptr;
    HFONT font = app->state.hFontUI;
    if (id == panel::kIkChainCombo && app->EnglishUI() == kUiChinese) {
        // Use cell height here. The normal Chinese character height makes
        // the closed combo touch the first IK radio in the fixed panel.
        static HFONT ikComboFont = CreateFontW(14, 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, PROOF_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (ikComboFont) return ikComboFont;
    }
    wchar_t className[32]{};
    GetClassNameW(control, className, 32);
    if (_wcsicmp(className, L"Edit") == 0) {
        // Numeric fields are only 14px high. YaHei's 12px glyphs
        // require a 17px cell, so keep their digits in a 13px cell.
        static HFONT numericFont = CreateFontW(13, 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, PROOF_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
        if (numericFont) font = numericFont;
    }
    return font;
}

void ApplyUiFontToControl(MMDApp* app, HWND control, int id) {
    if (!control || KeepClassFont(id)) return;
    const auto font = UiFontForControl(app, control, id);
#if MMD_ORIGINAL_PROFILE
    SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
#else
    ui::SetLayoutFont(control, font);
#endif
}

void ApplyTemplateUiFont(MMDApp* app, HWND control, int percent, int buttonWidth) {
#if !MMD_ORIGINAL_PROFILE
    if (!control) return;
    // Each shared base font and percentage has a stable handle for controls
    // that may keep borrowing it when another window changes layout.
    struct FontEntry { HFONT base; int percent; HFONT scaled; };
    static std::vector<FontEntry> fonts;
    const int id = GetDlgCtrlID(control);
    const HFONT unscaled = UiFontForControl(app, control, id);
    const HFONT base = KeepClassFont(id)
        ? static_cast<HFONT>(GetStockObject(SYSTEM_FONT)) : unscaled;
    if (!base) return;
    const auto current = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    bool owned = current == unscaled;
    for (const auto& entry : fonts)
        if (entry.scaled == current) owned = true;
    if (!owned) return;
    if (buttonWidth > 0 && (percent < 100 || ui_scale::Percent() > 100)) {
        wchar_t type[16]{}, text[256]{};
        GetClassNameW(control, type, 16);
        const int length = _wcsicmp(type, L"Button") == 0 ? GetWindowTextW(control, text, 256) : 0;
        if (length > 0) {
            const auto dc = GetDC(control);
            if (dc) {
                const auto previous = SelectObject(dc, base);
                SIZE extent{};
                if (GetTextExtentPoint32W(dc, text, length, &extent) && extent.cx > 0) {
                    const auto style = GetWindowLongW(control, GWL_STYLE) & BS_TYPEMASK;
                    const bool check = (style >= BS_CHECKBOX && style <= BS_AUTO3STATE) || style == BS_AUTORADIOBUTTON;
                    const int padding = check ? GetSystemMetrics(SM_CXMENUCHECK) + 5 : 8;
                    percent = std::min(percent, std::max(55, (buttonWidth - padding) * 100 / int(extent.cx)));
                }
                SelectObject(dc, previous);
                ReleaseDC(control, dc);
            }
        }
    }
    if (percent == 100) {
        ui::SetLayoutFont(control, unscaled);
        return;
    }
    for (const auto& entry : fonts) {
        if (entry.base == base && entry.percent == percent) {
            ui::SetLayoutFont(control, entry.scaled);
            return;
        }
    }
    LOGFONTW data{};
    if (GetObjectW(base, sizeof data, &data) != sizeof data) return;
    data.lfHeight = MulDiv(data.lfHeight, percent, 100);
    data.lfWidth = MulDiv(data.lfWidth, percent, 100);
    if (const HFONT scaled = CreateFontIndirectW(&data)) {
        fonts.push_back({base, percent, scaled});
        ui::SetLayoutFont(control, scaled);
    }
#endif
}

}  // namespace mikumikustudio
