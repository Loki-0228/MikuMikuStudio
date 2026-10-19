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

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
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

void ApplyUiFontToControl(MMDApp* app, HWND control, int id) {
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
            return;
        default:
            break;
        }
        if (control != nullptr) {
            HFONT font = app->state.hFontUI;
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
            SendMessageA(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(font),
                         TRUE);
        }
}

}  // namespace mikumikustudio
