#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#define MMS_SHARED_DIALOG_TRANSLATION 1

namespace mikumikustudio {
class MMDApp;
void LocalizeChineseDialog(MMDApp* app, HWND dialog);
HWND CreateUiDialogParamA(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
HWND CreateUiDialogParamW(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM);
INT_PTR UiDialogBoxParamA(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
INT_PTR UiDialogBoxParamW(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM);
int LegacyUiMessageBoxA(HWND, LPCSTR, LPCSTR, UINT);
const wchar_t* LocalizedUiTextW(const wchar_t* text);
BOOL GetUiOpenFileNameW(LPOPENFILENAMEW);
BOOL GetUiSaveFileNameW(LPOPENFILENAMEW);

// Only application-owned labels go through this dictionary. Model, bone,
// morph and accessory names keep their original text.
const wchar_t* ChineseUiText(const char* english);

// Font face of the coordinate-system caption (local / global / view / accessory).
// The original HUD sheet paints that one label with a serif face, while every
// other label uses the label font.  The English and Japanese UI keep that face
// (both spell the caption with the same Latin words, so both take Times New
// Roman); the Chinese UI shows the translated Han wording and keeps its label
// face, Microsoft YaHei UI.
const wchar_t* UiCaptionFaceName(unsigned char englishUI);

// Which face family paints a glyph run.  Label is the original
// mapping (Arial / MS PGothic, and Microsoft YaHei UI for the Chinese UI);
// Caption is UiCaptionFaceName.  The caption is the only serif run, and only
// in the English and Japanese UI.
enum class UiGlyphFace { Label, Caption };

int DrawUiGlyph(MMDApp* app, const char* text, HDC hdc, int size, int x, int y,
                unsigned char r, unsigned char g, unsigned char b, int bold,
                UiGlyphFace face = UiGlyphFace::Label);
int DrawWideUiGlyph(const wchar_t* text, HDC hdc, int size, int x, int y,
                    unsigned char r, unsigned char g, unsigned char b, int bold,
                    const wchar_t* face = L"Microsoft YaHei UI");
void SetUiControlText(MMDApp* app, HWND control, const char* english);
LRESULT AddUiComboText(MMDApp* app, HWND combo, const char* english);
}
