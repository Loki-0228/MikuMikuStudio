#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "autosave_dialog.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ui_translation.hpp"
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <algorithm>

namespace mikudancestudio::autosave {
const wchar_t* Text(MMDApp* app, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    const unsigned lang = app ? app->EnglishUI() : 1;
    return lang == 2 ? zh : lang == 0 ? ja : en;
}
namespace {
const wchar_t* Title(MMDApp* app) {
    return Text(app, L"自动保存设置", L"Autosave Settings", L"自動保存の設定");
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
        picker->SetTitle(Text(c.app, L"选择自动保存文件夹", L"Choose autosave folder", L"自動保存フォルダーを選択"));
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(c.value.directory.c_str(), nullptr, IID_PPV_ARGS(&item)))) picker->SetFolder(item.Get());
        if (SUCCEEDED(hr)) hr = picker->Show(dialog);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
        if (SUCCEEDED(hr)) {
            item.Reset(); hr = picker->GetResult(&item);
            PWSTR path = nullptr;
            if (SUCCEEDED(hr)) hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(hr)) { c.value.directory = path; SetDlgItemTextW(dialog, Folder, path); }
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
        label(FolderLabel, L"自动保存文件夹", L"Backup folder", L"自動保存フォルダー");
        label(Enabled, L"启用定时自动保存", L"Enable periodic autosave", L"定期的な自動保存を有効にする");
        label(Browse, L"选择…", L"Browse…", L"参照…");
        label(IntervalLabel, L"保存间隔（分钟）", L"Interval (minutes)", L"保存間隔（分）");
        label(LimitLabel, L"备份数量上限（份）", L"Maximum backups", L"バックアップの上限（件）");
        label(Overwrite, L"达到上限时覆盖最旧的备份", L"Replace oldest backups when the limit is reached", L"上限に達したら最も古いバックアップを置き換える");
        label(Explanation, L"崩溃备份和错误日志保存在程序旁的 backups 文件夹。\r\n备份保留原工程名和时间戳，不影响手动保存。\r\nPMM 数量上限按文件夹分别计算，其他文件保留。",
            L"Crash copies and error logs go to backups beside the EXE.\r\nCopies keep the project name and timestamp; manual saves stay separate.\r\nThe PMM limit applies per folder. Other files are kept.",
            L"クラッシュ時のコピーとログは EXE 横の backups に保存します。\r\n元の名前に日時を付け、手動保存したファイルは上書きしません。\r\nPMM の上限は各フォルダーに適用します。他のファイルは残します。");
        label(IDOK, L"确定", L"OK", L"OK"); label(IDCANCEL, L"取消", L"Cancel", L"キャンセル");
        SetDlgItemTextW(dialog, Folder, c->value.directory.c_str());
        SetDlgItemInt(dialog, Interval, c->value.intervalMinutes, FALSE);
        SetDlgItemInt(dialog, Limit, c->value.maxBackups, FALSE);
        SendDlgItemMessageW(dialog, Interval, EM_SETLIMITTEXT, 4, 0);
        SendDlgItemMessageW(dialog, Limit, EM_SETLIMITTEXT, 6, 0);
        CheckDlgButton(dialog, Overwrite, c->value.overwriteOldest ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, Enabled, c->value.enabled ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    if (!c) return FALSE;
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
        std::wstring error;
        if (!a || !b) error = Text(c->app, L"请填写有效的保存间隔和备份数量。", L"Enter a valid interval and backup limit.", L"有効な保存間隔と上限を入力してください。");
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
            AppendMenuW(help, MF_STRING, kSettingsCommand, L"Autosave settings…");
        }
        ModifyMenuW(help, kSettingsCommand, MF_BYCOMMAND | MF_STRING | (Current().enabled ? MF_CHECKED : MF_UNCHECKED), kSettingsCommand,
            Text(app, L"自动保存设置…", L"Autosave settings…", L"自動保存の設定…"));
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
