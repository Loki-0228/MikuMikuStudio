#pragma once
#include <Windows.h>
#include <vector>

namespace mikumikustudio::nvidia_antialias_menu::uninstall_dialog {
constexpr int Warning = 100, Detail = 101;
inline const wchar_t* Text(int language, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    return language == 2 ? zh : language == 0 ? ja : en;
}
inline INT_PTR CALLBACK Proc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const int language = static_cast<int>(lp);
        SetWindowTextW(dialog, Text(language, L"卸载 NVIDIA 补丁", L"Uninstall NVIDIA fix", L"NVIDIA 修正のアンインストール"));
        const auto font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
        const auto control = [&](int id, const wchar_t* type, DWORD style, const wchar_t* text,
                                 int x, int y, int width, int height) {
            RECT rect{x, y, x + width, y + height}; MapDialogRect(dialog, &rect);
            HWND child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
                rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                dialog, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        };
        control(Warning, L"STATIC", SS_LEFT, Text(language,
            L"警告：卸载后，NVIDIA 双面绘制修复将失效。使用 MSAA 时，部分双面材质可能重新出现白边、接缝或绘制异常。",
            L"Warning: uninstalling removes the NVIDIA double-sided rendering fix. With MSAA enabled, some double-sided materials may show white edges, seams or rendering errors again.",
            L"警告：アンインストールすると、NVIDIA の両面描画の修正が無効になります。MSAA 使用時に、一部の両面材質で白い縁や継ぎ目などの描画不具合が再発する可能性があります。"),
            10, 10, 266, 32);
        control(Detail, L"STATIC", SS_LEFT, Text(language,
            L"仅移除此目录中 MMS 的补丁设置，下次启动 MMS 时生效。工程和模型文件不会改变。",
            L"Only the fix for MMS in this folder will be removed. The change takes effect the next time MMS starts. Project and model files are unaffected.",
            L"このフォルダーの MMS に適用した修正のみを解除します。次回の MMS 起動時に反映されます。プロジェクトやモデルのファイルは変更しません。"),
            10, 49, 266, 25);
        control(IDOK, L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON,
            Text(language, L"卸载补丁", L"Uninstall", L"アンインストール"), 110, 83, 86, 18);
        control(IDCANCEL, L"BUTTON", WS_TABSTOP | BS_DEFPUSHBUTTON,
            Text(language, L"取消", L"Cancel", L"キャンセル"), 204, 83, 72, 18);
        SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    if (message == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lp) == GetDlgItem(dialog, Warning)) {
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(192, 0, 0));
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
    }
    if (message == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wp)); return TRUE;
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
inline std::vector<WORD> Template() {
    std::vector<WORD> data((sizeof(DLGTEMPLATE) + 1) / 2, 0);
    auto* dialog = reinterpret_cast<DLGTEMPLATE*>(data.data());
    dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER;
    dialog->cx = 286; dialog->cy = 111;
    data.insert(data.end(), {0, 0, 0, 9}); // no menu, default class, empty title, 9pt
    const wchar_t font[] = L"Segoe UI";
    data.insert(data.end(), font, font + sizeof(font) / sizeof(wchar_t));
    return data;
}
inline bool Confirm(HWND owner, int language) {
    auto data = Template();
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<DLGTEMPLATE*>(data.data()),
        owner, Proc, language) == IDOK;
}
}
