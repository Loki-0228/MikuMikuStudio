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
namespace {
const wchar_t* const tokens[]{L"[/NAME]",L"[/N]",L"[/YY]",L"[/YYYY]",L"[/MM]",L"[/DD]",L"[/hh]",L"[/mm]",L"[/ss]"};
void UpdateNaming(HWND dialog,NamingContext& context) {
    wchar_t buffer[512]{};GetDlgItemTextW(dialog,Pattern,buffer,512);
    SYSTEMTIME now{};GetLocalTime(&now);std::wstring result,error;
    const bool valid=save_naming::Expand(buffer,context.name,now,L"001",result,error);
    SetDlgItemTextW(dialog,Preview,(valid?result+L".pmm":error).c_str());
    EnableWindow(GetDlgItem(dialog,IDOK),valid);
}
}
INT_PTR CALLBACK NamingProc(HWND dialog,UINT msg,WPARAM wp,LPARAM lp) {
    auto* c=reinterpret_cast<NamingContext*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(msg==WM_INITDIALOG) {
        c=reinterpret_cast<NamingContext*>(lp);SetWindowLongPtrW(dialog,DWLP_USER,lp);
        SetWindowTextW(dialog,c->backup?Text(c->app,L"备份命名",L"Backup naming",L"バックアップ名"):
            Text(c->app,L"增量命名",L"Increment naming",L"増分名"));
        SetDlgItemTextW(dialog,PatternLabel,Text(c->app,L"文件名规则",L"Filename pattern",L"ファイル名の規則"));
        SetDlgItemTextW(dialog,PreviewLabel,Text(c->app,L"预览",L"Preview",L"プレビュー"));
        const wchar_t* zh[]{L"工程名",L"序号",L"年（YY）",L"年（YYYY）",L"月",L"日",L"时",L"分",L"秒"};
        const wchar_t* en[]{L"Project",L"Sequence",L"Year (YY)",L"Year (YYYY)",L"Month",L"Day",L"Hour",L"Minute",L"Second"};
        const wchar_t* ja[]{L"工程名",L"連番",L"年（YY）",L"年（YYYY）",L"月",L"日",L"時",L"分",L"秒"};
        for(int i=0;i<int(std::size(tokens));++i)SetDlgItemTextW(dialog,TokenFirst+i,Text(c->app,zh[i],en[i],ja[i]));
        LOGFONTW font{};
        const auto inputFont=reinterpret_cast<HFONT>(SendDlgItemMessageW(dialog,Pattern,WM_GETFONT,0,0));
        if(inputFont && GetObjectW(inputFont,sizeof(font),&font)) {
            font.lfHeight=MulDiv(font.lfHeight,6,5);
            c->previewFont=CreateFontIndirectW(&font);
            if(c->previewFont)SendDlgItemMessageW(dialog,Preview,WM_SETFONT,reinterpret_cast<WPARAM>(c->previewFont),TRUE);
        }
        SetDlgItemTextW(dialog,IDOK,Text(c->app,L"确定",L"OK",L"OK"));
        SetDlgItemTextW(dialog,IDCANCEL,Text(c->app,L"取消",L"Cancel",L"キャンセル"));
        SendDlgItemMessageW(dialog,Pattern,EM_SETLIMITTEXT,240,0);
        SetDlgItemTextW(dialog,Pattern,c->pattern.c_str());UpdateNaming(dialog,*c);
        SetFocus(GetDlgItem(dialog,Pattern));SendDlgItemMessageW(dialog,Pattern,EM_SETSEL,-1,-1);
        SetTimer(dialog,1,1000,nullptr);return FALSE;
    }
    if(!c)return FALSE;
    if(msg==WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lp)==GetDlgItem(dialog,Preview)) {
        SetTextColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(reinterpret_cast<HDC>(wp),GetSysColor(COLOR_3DFACE));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    }
    if(msg==WM_TIMER){UpdateNaming(dialog,*c);return TRUE;}
    if(msg==WM_DESTROY){KillTimer(dialog,1);if(c->previewFont){DeleteObject(c->previewFont);c->previewFont=nullptr;}return TRUE;}
    if(msg==WM_CLOSE){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(msg!=WM_COMMAND)return FALSE;
    const auto id=LOWORD(wp);
    if(id==Pattern && HIWORD(wp)==EN_CHANGE){UpdateNaming(dialog,*c);return TRUE;}
    if(id>=TokenFirst && id<TokenFirst+std::size(tokens)) {
        SendDlgItemMessageW(dialog,Pattern,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(tokens[id-TokenFirst]));
        SetFocus(GetDlgItem(dialog,Pattern));return TRUE;
    }
    if(id==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}
    if(id==IDOK) {
        wchar_t buffer[512]{};GetDlgItemTextW(dialog,Pattern,buffer,512);std::wstring error;
        if(!save_naming::Validate(buffer,error)){UpdateNaming(dialog,*c);return TRUE;}
        c->pattern=buffer;EndDialog(dialog,IDOK);return TRUE;
    }
    return FALSE;
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
        label(Overwrite, L"达到上限时覆盖最旧的备份", L"Replace oldest at limit", L"上限時に古いバックアップを上書き");
        label(KeepPrevious, L"同时保存 .pmm1", L"Also keep .pmm1", L".pmm1 も保存する");
        const UINT effectSave=GetMenuState(GetMenu(c->app->MainWindow()),40022,MF_BYCOMMAND);
        const bool effectEnabled=effectSave!=UINT(-1) && (effectSave&MF_CHECKED);
        label(Explanation,effectEnabled?L"当前 .emm 保存已开启。":L"当前 .emm 保存未开启。",
            effectEnabled?L".emm saving is on.":L".emm saving is off.",
            effectEnabled?L".emm の同時保存：有効":L".emm の同時保存：無効");
        label(NumberedManual,L"增量保存",L"Incremental saving",L"増分保存");
        label(BackupNaming,L"备份命名",L"Backup name",L"バックアップ名");
        label(IncrementNaming,L"增量命名",L"Increment name",L"増分名");
        label(IDOK, L"确定", L"OK", L"OK"); label(IDCANCEL, L"取消", L"Cancel", L"キャンセル");
        SetDlgItemTextW(dialog, Folder, DisplayDirectory(c->value.directory).c_str());
        SetDlgItemInt(dialog, Interval, c->value.intervalMinutes, FALSE);
        SetDlgItemInt(dialog, Limit, c->value.maxBackups, FALSE);
        SendDlgItemMessageW(dialog, Interval, EM_SETLIMITTEXT, 4, 0);
        SendDlgItemMessageW(dialog, Limit, EM_SETLIMITTEXT, 6, 0);
        CheckDlgButton(dialog, Overwrite, c->value.overwriteOldest ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, Enabled, c->value.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, KeepPrevious, c->value.keepPrevious ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, NumberedManual, c->value.numberedManualVersions ? BST_CHECKED : BST_UNCHECKED);
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
    case BackupNaming:
    case IncrementNaming: {
        const bool backup=LOWORD(wp)==BackupNaming;
        auto& pattern=backup?c->value.backupPattern:c->value.incrementPattern;
        auto name=save_naming::ProjectStem(c->app->EnvFileName(),c->app->PmmProjectSettings().document);
        if(name.empty())name=Text(c->app,L"未命名",L"Untitled",L"無題");
        NamingContext naming{c->app,pattern,name,backup};
        if(UiDialogBoxParamW(static_cast<HINSTANCE>(c->app->HInstance()),MAKEINTRESOURCEW(kNamingDialog),
            dialog,NamingProc,reinterpret_cast<LPARAM>(&naming))==IDOK)pattern=naming.pattern;
        return TRUE;
    }
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
        value.numberedManualVersions = IsDlgButtonChecked(dialog, NumberedManual) == BST_CHECKED;
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
