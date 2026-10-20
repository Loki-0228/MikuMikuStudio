#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "autosave_dialog.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <ShObjIdl.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>

namespace mikumikustudio::autosave {
const wchar_t* Text(MMDApp* app, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    const unsigned lang = app ? app->EnglishUI() : 1;
    return lang == 2 ? zh : lang == 0 ? ja : en;
}
namespace {
const wchar_t* Title(MMDApp* app) {
    return Text(app, L"自动备份设置", L"Automatic backup settings", L"自動バックアップ設定");
}
const wchar_t* PreviousTip(MMDApp* app) {
    return Text(app,
        L"保存时，旧文件追加“1”作为备份（.pmm → .pmm1，.pmm1 → .pmm11）。\r\n.emm 仅在 MME 启用随 .pmm 保存时生成。",
        L"Saving backs up the old file by appending \"1\" (.pmm → .pmm1, .pmm1 → .pmm11).\r\nNew .emm files require MME's Save with .pmm option.",
        L"保存時、旧ファイルの末尾に「1」を付けてバックアップします（.pmm → .pmm1、.pmm1 → .pmm11）。\r\n.emm は MME の .pmm 同時保存が有効な場合のみ保存します。");
}
void AddTip(HWND dialog, DialogContext& c, int id) {
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = dialog;
    tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(dialog, id));
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(c.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}
void Error(HWND owner, MMDApp* app, const std::wstring& detail) {
    MessageBoxW(owner, detail.c_str(), Title(app), MB_OK | MB_ICONWARNING);
}
void PickFolder(HWND dialog, DialogContext& c) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker));
    if (SUCCEEDED(hr)) {
        DWORD flags = 0;
        hr = picker->GetOptions(&flags);
        if (SUCCEEDED(hr)) hr = picker->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        picker->SetTitle(Text(c.app, L"选择备份文件夹", L"Choose backup folder", L"バックアップフォルダーを選択"));
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(c.value.directory.c_str(), nullptr, IID_PPV_ARGS(&item)))) picker->SetFolder(item.Get());
        if (SUCCEEDED(hr)) hr = picker->Show(dialog);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        if (SUCCEEDED(hr)) {
            item.Reset(); hr = picker->GetResult(&item);
            PWSTR path = nullptr;
            if (SUCCEEDED(hr)) hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(hr)) {
                c.value.directory = path;
                SetDlgItemTextW(dialog, Folder, DisplayDirectory(c.value.directory).c_str());
            }
            CoTaskMemFree(path);
        }
    }
    if (FAILED(hr)) Error(dialog, c.app, Text(c.app, L"无法打开文件夹选择窗口。", L"Could not open folder picker.", L"フォルダー選択を開けません。"));
}
void ShowSettings(MMDApp* app) {
    DialogContext context{app, Current()};
    UiDialogBoxParamW(static_cast<HINSTANCE>(app->HInstance()), MAKEINTRESOURCEW(kDialog),
        app->MainWindow(), DialogProc, reinterpret_cast<LPARAM>(&context));
}
}
INT_PTR CALLBACK DialogProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    auto* c = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        c = reinterpret_cast<DialogContext*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, Title(c->app));
        const auto label = [&](int id, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
            SetDlgItemTextW(dialog, id, Text(c->app, zh, en, ja));
        };
        label(FolderLabel, L"备份文件夹", L"Backup folder", L"バックアップフォルダー");
        label(Enabled, L"启用定时自动备份", L"Enable automatic backups", L"定期バックアップを有効にする");
        label(Browse, L"选择…", L"Browse…", L"参照…");
        label(IntervalLabel, L"备份间隔（分钟）", L"Interval (minutes)", L"バックアップ間隔（分）");
        label(LimitLabel, L".pmm 备份上限（份）", L".pmm backup limit", L".pmm バックアップ上限（件）");
        label(Overwrite, L"达到上限时覆盖最旧的备份", L"Replace oldest backups when the limit is reached", L"上限に達したら最も古いバックアップを置き換える");
        label(KeepPrevious, L"同时保存 .pmm1", L"Also keep .pmm1", L".pmm1 も保存する");
        label(Explanation, L"备份保留工程名和时间戳。\r\nMME 启用随 .pmm 保存时，同时保存 .emm。",
            L"Backups include the project name and timestamp.\r\nThe .emm file follows MME's Save with .pmm setting.",
            L"元の名前と日時を付けてバックアップします。\r\n.emm は MME の .pmm 同時保存設定に従います。");
        label(IDOK, L"确定", L"OK", L"OK"); label(IDCANCEL, L"取消", L"Cancel", L"キャンセル");
        SetDlgItemTextW(dialog, Folder, DisplayDirectory(c->value.directory).c_str());
        SetDlgItemInt(dialog, Interval, c->value.intervalMinutes, FALSE);
        SetDlgItemInt(dialog, Limit, c->value.maxBackups, FALSE);
        SendDlgItemMessageW(dialog, Interval, EM_SETLIMITTEXT, 4, 0);
        SendDlgItemMessageW(dialog, Limit, EM_SETLIMITTEXT, 6, 0);
        CheckDlgButton(dialog, Overwrite, c->value.overwriteOldest ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, Enabled, c->value.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, KeepPrevious, c->value.keepPrevious ? BST_CHECKED : BST_UNCHECKED);
        c->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, dialog, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (c->tooltip) {
            RECT width{0, 0, 240, 0}; MapDialogRect(dialog, &width);
            SendMessageW(c->tooltip, TTM_SETMAXTIPWIDTH, 0, width.right);
            SendMessageW(c->tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);
            AddTip(dialog, *c, KeepPrevious);
            AddTip(dialog, *c, Folder);
        }
        return TRUE;
    }
    if (!c) return FALSE;
    if (msg == WM_NOTIFY) {
        auto* tip = reinterpret_cast<NMTTDISPINFOW*>(lp);
        if (tip && tip->hdr.hwndFrom == c->tooltip && tip->hdr.code == TTN_GETDISPINFOW) {
            tip->lpszText = tip->hdr.idFrom == reinterpret_cast<UINT_PTR>(GetDlgItem(dialog, KeepPrevious))
                ? const_cast<wchar_t*>(PreviousTip(c->app)) : c->value.directory.data();
            return TRUE;
        }
    }
    if (msg == WM_DESTROY && c->tooltip) { DestroyWindow(c->tooltip); c->tooltip = nullptr; }
    if (msg == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (msg != WM_COMMAND) return FALSE;
    switch (LOWORD(wp)) {
    case Browse: PickFolder(dialog, *c); return TRUE;
    case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
    case IDOK: {
        BOOL a = FALSE, b = FALSE;
        auto value = c->value;
        value.intervalMinutes = GetDlgItemInt(dialog, Interval, &a, FALSE);
        value.maxBackups = GetDlgItemInt(dialog, Limit, &b, FALSE);
        value.overwriteOldest = IsDlgButtonChecked(dialog, Overwrite) == BST_CHECKED;
        value.enabled = IsDlgButtonChecked(dialog, Enabled) == BST_CHECKED;
        value.keepPrevious = IsDlgButtonChecked(dialog, KeepPrevious) == BST_CHECKED;
        std::wstring error;
        if (!a || !b) error = Text(c->app, L"请填写有效的备份间隔和数量上限。", L"Enter a valid interval and backup limit.", L"有効なバックアップ間隔と上限を入力してください。");
        if (!error.empty() || !Configure(c->app, value, error)) { Error(dialog, c->app, error); return TRUE; }
        c->value = value; EndDialog(dialog, IDOK); return TRUE;
    }
    }
    return FALSE;
}
void RefreshMenu(MMDApp* app) {
    if (!app || !IsWindow(app->MainWindow())) return;
    const HMENU bar = GetMenu(app->MainWindow());
    for (int i = 0; i < GetMenuItemCount(bar); ++i) {
        HMENU help = GetSubMenu(bar, i);
        if (!help || GetMenuState(help, 201, MF_BYCOMMAND) == UINT(-1)) continue;
        if (GetMenuState(help, kSettingsCommand, MF_BYCOMMAND) == UINT(-1)) {
            AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(help, MF_STRING, kSettingsCommand, L"Automatic backup settings…");
        }
        ModifyMenuW(help, kSettingsCommand, MF_BYCOMMAND | MF_STRING | (Current().enabled ? MF_CHECKED : MF_UNCHECKED), kSettingsCommand,
            Text(app, L"自动备份设置…", L"Automatic backup settings…", L"自動バックアップ設定…"));
        break;
    }
}
bool HandleCommand(MMDApp* app, unsigned command) {
    if (command == kSettingsCommand) { ShowSettings(app); return true; }
    return false;
}
void PaintMenuWarning(MMDApp* app, HDC dc) {
    if (!FolderFull() || !app || app->FullscreenMode()) return;
    HWND hwnd = app->MainWindow();
    HMENU bar = GetMenu(hwnd);
    MENUBARINFO info{sizeof(info)};
    RECT window{};
    if (!bar || !GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info) || !GetWindowRect(hwnd, &window)) return;
    // Only use the unoccupied portion of the final menu row, including wrapped bars.
    RECT last{};
    const int count = GetMenuItemCount(bar);
    if (count <= 0 || !GetMenuItemRect(hwnd, bar, count - 1, &last)) return;
    RECT rect{last.right + 12 - window.left, last.top - window.top,
        info.rcBar.right - window.left - 6, last.bottom - window.top};
    if (rect.right <= rect.left) return;
    const bool own = !dc;
    if (own) dc = GetWindowDC(hwnd);
    if (!dc) return;
    const int saved = SaveDC(dc);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    HFONT font = CreateFontIndirectW(&metrics.lfMenuFont);
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(210, 0, 0));
    IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    DrawTextW(dc, Text(app, L"备份文件夹已满", L"Backup folder is full", L"バックアップフォルダーがいっぱいです"), -1,
        &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RestoreDC(dc, saved);
    if (font) DeleteObject(font);
    if (own) ReleaseDC(hwnd, dc);
}
}
