// ===========================================================================
// VA 0x0041A550 - DrawControlText  (original: sub_41A550, 0x100 bytes)
// ===========================================================================
// Bold single-line control label drawing used by HandleNotify (0x4398B0):
//   GetWindowTextA(hwnd, buf[0x100], 0x100) -> SetBkMode(hdc, TRANSPARENT)
//   -> GlobalAlloc(GPTR, 0x3C) zeroed LOGFONTA block; the GPTR zero-init is
//   load-bearing (lfOrientation/lfStrikeOut/lfOutPrecision/lfClipPrecision/
//   lfQuality/lfPitchAndFamily stay 0) -> face and height picked by the
//   EnglishUI flag (this+658252 = 0xA0B4C, MMDApp::EnglishUI()):
//       EN: strcpy_s "Tahoma"              lfHeight = 13 (0x0D)
//       JP: strcpy_s "MS P Gothic" (SJIS)  lfHeight = 12 (0x0C)
//   -> lfWeight = 700 (FW_BOLD), lfEscapement/lfWidth/lfItalic/lfUnderline
//   = 0, lfCharSet = 0x80 (SHIFTJIS_CHARSET) -> CreateFontIndirectA ->
//   SelectObject (old saved) -> DrawTextA(hdc, text, -1, &rc,
//   DT_CENTER|DT_VCENTER|DT_SINGLELINE = 0x25) -> SelectObject(old) ->
//   DeleteObject(font) -> GlobalFree(lf).
// Reference: ../translated/MikuMikuDance/fcn_0041a550.cpp
// =========================================================================//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>

#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include <cwchar>

namespace mikumikustudio {

void DrawControlText(MMDApp* app, HWND hwnd, HDC hdc, RECT rc) {
    wchar_t text[256]{};
    GetWindowTextW(hwnd, text, 256);
    SetBkMode(hdc, TRANSPARENT);
    LOGFONTW fontSpec{};
    HFONT controlFont = reinterpret_cast<HFONT>(SendMessageW(hwnd,WM_GETFONT,0,0));
    if (!controlFont) controlFont=app->state.hFontUI;
    if (!GetObjectW(controlFont,sizeof(fontSpec),&fontSpec)) {
        fontSpec.lfHeight = app->EnglishUI() == kUiChinese ? -12 : app->EnglishUI() == kUiJapanese ? 12 : 13;
        wcscpy_s(fontSpec.lfFaceName, app->EnglishUI() == kUiChinese ? L"Microsoft YaHei UI"
            : app->EnglishUI() == kUiEnglish ? L"Tahoma" : L"MS PGothic");
    }
    fontSpec.lfWeight = FW_BOLD;
    fontSpec.lfCharSet = DEFAULT_CHARSET;
    HFONT font = CreateFontIndirectW(&fontSpec);
    HGDIOBJ previous = SelectObject(hdc, font);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, previous);
    DeleteObject(font);
}

// ===========================================================================
// VA 0x00429770 - HandleTimer100  (original: sub_429770, 23 bytes)
// ===========================================================================
// WM_TIMER id 100 handler.  The caller (MainWndProc 0x4C3A10) has already
// KillTimer'd id 100; this closes the streaming data-file sub-object stored
// at this+204 (0xCC, the 0x25C obj -> sub_4C2680 CloseDataFile) and then
// sets this+656054 (0xA02B6) = 1.
// Reference: ../translated/MikuMikuDance/fcn_00429770.cpp
// =========================================================================//

// VA 0x004C2680 - defined in ui_refresh.cpp; original is __thiscall on the
// 0x25C streaming-file sub-object.
void CloseDataFile(void* file);

void HandleTimer100(MMDApp* app) {
    CloseDataFile(app->Audio());                           // this+204 (0xCC)
    app->AudioSeekReady() = 1;                             // this+656054
}

}  // namespace mikumikustudio
