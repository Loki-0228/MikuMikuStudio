#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cache_manager_dialog.hpp"
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <future>
#include <string>

namespace mikumikustudio::cache_manager_ui {
namespace {
namespace cache=cache_manager;
constexpr double GiB=1024.0*1024*1024;
const wchar_t* Text(unsigned language,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
    return language==2?zh:language==0?ja:en;
}
std::wstring Size(std::uint64_t bytes) {
    wchar_t text[64]{};
    if(bytes>=1024ull*1024*1024)swprintf_s(text,L"%.2f GB",bytes/GiB);
    else if(bytes>=1024*1024)swprintf_s(text,L"%.1f MB",double(bytes)/(1024*1024));
    else if(bytes>=1024)swprintf_s(text,L"%.1f KB",double(bytes)/1024);
    else swprintf_s(text,L"%llu B",bytes);
    return text;
}
std::wstring Date(std::uint64_t stamp) {
    FILETIME utc{DWORD(stamp),DWORD(stamp>>32)},local{};SYSTEMTIME time{};
    if(!FileTimeToLocalFileTime(&utc,&local)||!FileTimeToSystemTime(&local,&time))return L"";
    wchar_t text[64]{};swprintf_s(text,L"%04u-%02u-%02u %02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute);return text;
}
struct Work {cache::Snapshot snapshot;cache::CleanupResult cleaned;std::wstring message,error;};
struct Context {
    MMDApp* app=nullptr;
    unsigned language=1;
    cache::Snapshot snapshot;
    std::future<Work> work;
    std::wstring selected;
    bool closing=false,updating=false;
};
const wchar_t* Tr(const Context& c,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {return Text(c.language,zh,en,ja);}
const wchar_t* Error(const Context& c) {return Tr(c,L"缓存操作未完成。请检查缓存和 data 文件夹是否可写，或稍后重试。",
    L"The cache operation did not finish. Check write access to Cache and data, or try again later.",
    L"キャッシュ操作を完了できません。Cache と data の書き込み権限を確認して再試行してください。");}
bool Busy(const Context& c) {return c.work.valid();}
int Selected(HWND dialog) {return ListView_GetNextItem(GetDlgItem(dialog,Projects),-1,LVNI_SELECTED);}
void EnableActions(HWND dialog,Context& c) {
    const bool available=!Busy(c),enabled=IsDlgButtonChecked(dialog,Enabled)==BST_CHECKED;
    for(int id:{Enabled,Limit,Recent,Apply,ClearReclaimable,ClearAll})EnableWindow(GetDlgItem(dialog,id),available);
    EnableWindow(GetDlgItem(dialog,Automatic),available&&enabled);
    const int row=Selected(dialog);
    const bool selected=available && row>=0 && std::size_t(row)<c.snapshot.projects.size();
    EnableWindow(GetDlgItem(dialog,Pin),selected);
    EnableWindow(GetDlgItem(dialog,ClearProject),selected && c.snapshot.projects[row].reclaimableBytes>0);
    SetDlgItemTextW(dialog,Pin,selected && c.snapshot.projects[row].pinned?
        Tr(c,L"取消固定",L"Unpin",L"固定を解除"):Tr(c,L"固定保留",L"Pin",L"固定して保持"));
}
void Selection(HWND dialog,Context& c) {
    const int row=Selected(dialog);
    c.selected=row>=0 && std::size_t(row)<c.snapshot.projects.size()?c.snapshot.projects[row].path:L"";
    SetDlgItemTextW(dialog,Path,c.selected.c_str());EnableActions(dialog,c);
}
void LayoutUsageRow(HWND dialog) {
    HWND summary=GetDlgItem(dialog,Summary),kinds=GetDlgItem(dialog,Kinds);
    RECT bounds{},preferred{154,0,350,6};GetWindowRect(summary,&bounds);
    MapWindowPoints(nullptr,dialog,reinterpret_cast<POINT*>(&bounds),2);MapDialogRect(dialog,&preferred);
    wchar_t text[512]{};GetWindowTextW(summary,text,512);HDC dc=GetDC(summary);
    auto previous=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(summary,WM_GETFONT,0,0)));
    SIZE size{};GetTextExtentPoint32W(dc,text,int(std::wcslen(text)),&size);
    SelectObject(dc,previous);ReleaseDC(summary,dc);
    const int split=std::max(preferred.left,bounds.left+size.cx+preferred.bottom);
    SetWindowPos(summary,nullptr,0,0,split-bounds.left-preferred.bottom/2,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    SetWindowPos(kinds,nullptr,split,bounds.top,std::max<LONG>(0,preferred.right-split),bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE);
}
void Fill(HWND dialog,Context& c,bool settings) {
    const auto& s=c.snapshot;
    if(settings) {
        CheckDlgButton(dialog,Enabled,s.settings.enabled?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(dialog,Automatic,s.settings.automaticCleanup?BST_CHECKED:BST_UNCHECKED);
        wchar_t limit[32]{};swprintf_s(limit,L"%.6g",s.settings.limitBytes/GiB);SetDlgItemTextW(dialog,Limit,limit);
        SetDlgItemInt(dialog,Recent,s.settings.recentProjects,FALSE);
    }
    const auto used=Size(s.totalBytes),maximum=Size(s.settings.limitBytes),free=Size(s.reclaimableBytes);
    wchar_t text[512]{};
    swprintf_s(text,Tr(c,L"已用 %ls / %ls    可回收 %ls",L"Used %ls / %ls    Reclaimable %ls",L"使用量 %ls / %ls    回収可能 %ls"),used.c_str(),maximum.c_str(),free.c_str());
    SetDlgItemTextW(dialog,Summary,text);
    const auto fx=Size(s.effectBytes),textures=Size(s.textureBytes);
    swprintf_s(text,Tr(c,L"MME 效果 %ls    纹理 %ls",L"MME effects %ls    Textures %ls",L"MME エフェクト %ls    テクスチャ %ls"),fx.c_str(),textures.c_str());SetDlgItemTextW(dialog,Kinds,text);
    LayoutUsageRow(dialog);
    HWND list=GetDlgItem(dialog,Projects);c.updating=true;SendMessageW(list,WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(list);
    for(std::size_t i=0;i<s.projects.size();++i) {
        const auto& p=s.projects[i];const std::filesystem::path path(p.path);
        auto name=path.filename().wstring();
        if(std::count_if(s.projects.begin(),s.projects.end(),[&](const auto& other){return std::filesystem::path(other.path).filename()==path.filename();})>1)
            name+=L" ("+path.parent_path().filename().wstring()+L")";
        std::wstring values[]={std::move(name),Date(p.lastOpened),
            Size(p.totalBytes-p.sharedBytes),Size(p.sharedBytes),Size(p.reclaimableBytes),
            p.current?Tr(c,L"使用中",L"In use",L"使用中"):p.pinned?Tr(c,L"已固定",L"Pinned",L"固定済み"):L""};
        LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=int(i);item.pszText=values[0].data();SendMessageW(list,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
        for(int col=1;col<6;++col){LVITEMW sub{};sub.iSubItem=col;sub.pszText=values[col].data();SendMessageW(list,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&sub));}
        if(p.path==c.selected)ListView_SetItemState(list,int(i),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    }
    c.updating=false;SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,nullptr,TRUE);
    Selection(dialog,c);
}
void Start(HWND dialog,Context& c,std::function<Work()> operation) {
    try{c.work=std::async(std::launch::async,std::move(operation));}
    catch(...){SetDlgItemTextW(dialog,Status,Error(c));return;}
    SetDlgItemTextW(dialog,Status,Tr(c,L"正在处理…",L"Working…",L"処理中…"));
    SetTimer(dialog,1,75,nullptr);EnableActions(dialog,c);
}
bool ReadSettings(HWND dialog,Context& c,cache::Settings& settings) {
    wchar_t number[64]{};GetDlgItemTextW(dialog,Limit,number,64);wchar_t* end=nullptr;
    const double limit=std::wcstod(number,&end);BOOL valid=FALSE;const auto recent=GetDlgItemInt(dialog,Recent,&valid,FALSE);
    while(end && *end==L' ')++end;
    if(!end || *end || !std::isfinite(limit) || limit<.0625 || limit>100 || !valid || recent<1 || recent>100) {
        SetDlgItemTextW(dialog,Status,Tr(c,L"空间上限请填写 0.0625–100 GB，最近工程数为 1–100。",
            L"Enter a limit from 0.0625 to 100 GB and 1 to 100 recent projects.",L"上限は 0.0625～100 GB、最近のプロジェクト数は 1～100 で入力してください。"));return false;
    }
    settings.enabled=IsDlgButtonChecked(dialog,Enabled)==BST_CHECKED;
    settings.automaticCleanup=IsDlgButtonChecked(dialog,Automatic)==BST_CHECKED;
    settings.limitBytes=std::uint64_t(std::llround(limit*1024))*1024*1024;settings.recentProjects=recent;return true;
}
void Show(MMDApp* app) {
    Context c;c.app=app;c.language=app->EnglishUI();
    UiDialogBoxParamW(static_cast<HINSTANCE>(app->HInstance()),MAKEINTRESOURCEW(kDialog),app->MainWindow(),DialogProc,reinterpret_cast<LPARAM>(&c));
}
void AddTooltips(HWND dialog,Context& c) {
    HWND tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,dialog,nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog,GWLP_HINSTANCE)),nullptr);
    if(!tips)return;
    SendMessageW(tips,TTM_SETMAXTIPWIDTH,0,380);
    SendMessageW(tips,TTM_SETDELAYTIME,TTDT_INITIAL,450);
    SendMessageW(tips,TTM_SETDELAYTIME,TTDT_AUTOPOP,15000);
    const auto add=[&](int id,const wchar_t* zh,const wchar_t* en,const wchar_t* ja) {
        TOOLINFOW tool{};tool.cbSize=sizeof(tool);tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
        tool.hwnd=dialog;tool.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(dialog,id));
        tool.lpszText=const_cast<wchar_t*>(Tr(c,zh,en,ja));SendMessageW(tips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
    };
    add(Automatic,L"超限后自动清理至约 80%，优先保留最近工程。固定和使用中的缓存不清理。",
        L"Over the limit, clean to about 80% and prefer recent projects. Keep pinned and in-use cache.",
        L"上限を超えると約 80% まで整理し、最近の項目を優先。固定済み・使用中の分は残します。");
    add(ClearReclaimable,L"手动清理保留最近、固定和使用中的缓存；自动清理在超限时回收到约 80%。",
        L"Manual cleanup keeps recent, pinned and in-use cache. Automatic cleanup targets 80% after the limit is exceeded.",
        L"手動整理では最近・固定済み・使用中の分を保持。自動整理は上限超過時に約 80% まで削減します。");
    add(ClearProject,L"仅清理所选工程的闲置独占缓存，保留共享和使用中的文件。",
        L"Clean only this project's idle exclusive cache. Keep shared and in-use files.",
        L"選択した項目の未使用専用キャッシュだけを整理。共有・使用中の分は残します。");
    add(ClearAll,L"清空闲置磁盘缓存，固定工程也包括在内。使用中的文件会跳过。",
        L"Clear idle disk cache, including pinned projects. Skip in-use files.",
        L"固定済みも含む未使用のディスクキャッシュを削除。使用中の分は残します。");
}
}
INT_PTR CALLBACK DialogProc(HWND dialog,UINT msg,WPARAM wp,LPARAM lp) {
    auto* c=reinterpret_cast<Context*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(msg==WM_INITDIALOG) {
        c=reinterpret_cast<Context*>(lp);SetWindowLongPtrW(dialog,DWLP_USER,lp);
        SetWindowTextW(dialog,Tr(*c,L"缓存管理",L"Cache management",L"キャッシュ管理"));
        const auto label=[&](int id,const wchar_t* zh,const wchar_t* en,const wchar_t* ja){SetDlgItemTextW(dialog,id,Tr(*c,zh,en,ja));};
        label(Enabled,L"启用缓存",L"Enable caching",L"キャッシュを有効にする");
        label(Automatic,L"自动清理（清理可回收缓存）",L"Automatically clean reclaimable cache",L"回収可能なキャッシュを自動整理");
        label(LimitLabel,L"空间上限（GB）",L"Size limit (GB)",L"容量上限（GB）");
        label(RecentLabel,L"优先保留最近工程数",L"Prefer recent projects",L"優先する最近の件数");
        label(Pin,L"固定保留",L"Pin",L"固定して保持");label(ClearProject,L"清理所选工程",L"Clean selected project",L"選択した項目を整理");
        label(ClearReclaimable,L"清理可回收缓存",L"Clean reclaimable",L"回収可能分を整理");
        label(ClearAll,L"清空磁盘缓存…",L"Clear disk cache…",L"ディスクキャッシュを削除…");
        label(Apply,L"应用",L"Apply",L"適用");label(IDCANCEL,L"关闭",L"Close",L"閉じる");
        const wchar_t* names[]={Tr(*c,L"工程",L"Project",L"プロジェクト"),Tr(*c,L"上次打开",L"Last opened",L"最終オープン"),
            Tr(*c,L"独占",L"Exclusive",L"専用"),Tr(*c,L"共享",L"Shared",L"共有"),Tr(*c,L"可释放",L"Releasable",L"解放可能"),Tr(*c,L"状态",L"Status",L"状態")};
        HWND list=GetDlgItem(dialog,Projects);ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
        const int widths[]={90,76,41,41,41,39};
        for(int col=0;col<6;++col){RECT width{0,0,widths[col],0};MapDialogRect(dialog,&width);LVCOLUMNW column{};
            column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<wchar_t*>(names[col]);column.cx=width.right;SendMessageW(list,LVM_INSERTCOLUMNW,col,reinterpret_cast<LPARAM>(&column));}
        SendDlgItemMessageW(dialog,Limit,EM_SETLIMITTEXT,12,0);SendDlgItemMessageW(dialog,Recent,EM_SETLIMITTEXT,3,0);
        AddTooltips(dialog,*c);
        c->snapshot.settings=cache::Current();Fill(dialog,*c,true);
        Start(dialog,*c,[]{Work out;out.snapshot=cache::Inspect();return out;});return TRUE;
    }
    if(!c)return FALSE;
    if(msg==WM_TIMER && wp==1 && Busy(*c)) {
        if(c->work.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return TRUE;
        KillTimer(dialog,1);
        try{auto result=c->work.get();c->snapshot=std::move(result.snapshot);Fill(dialog,*c,true);
            if(!result.error.empty() || !c->snapshot.error.empty())SetDlgItemTextW(dialog,Status,Error(*c));
            else if(!result.message.empty())SetDlgItemTextW(dialog,Status,result.message.c_str());
            else SetDlgItemTextW(dialog,Status,c->snapshot.projects.empty()?Tr(*c,L"暂无工程记录。成功打开工程后会自动登记缓存。",L"No project history yet. Opening a project records its cache.",L"履歴はありません。プロジェクトを開くとキャッシュを記録します。"):L"");
        }catch(...){SetDlgItemTextW(dialog,Status,Error(*c));EnableActions(dialog,*c);}
        if(c->closing)EndDialog(dialog,IDCANCEL);return TRUE;
    }
    if(msg==WM_NOTIFY) {
        const auto* notification=reinterpret_cast<NMHDR*>(lp);
        if(!c->updating && notification && notification->idFrom==Projects && notification->code==LVN_ITEMCHANGED)Selection(dialog,*c);
        return FALSE;
    }
    if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL)) {
        if(Busy(*c)){c->closing=true;SetDlgItemTextW(dialog,Status,Tr(*c,L"操作完成后关闭…",L"Closing when the operation finishes…",L"処理の完了後に閉じます…"));}
        else EndDialog(dialog,IDCANCEL);return TRUE;
    }
    if(msg!=WM_COMMAND)return FALSE;
    const auto id=LOWORD(wp);
    if(id==Enabled){EnableActions(dialog,*c);return TRUE;}
    if(Busy(*c))return TRUE;
    if(id==Pin && !c->selected.empty()) {
        const auto project=c->selected;const int row=Selected(dialog);const bool pin=!c->snapshot.projects[row].pinned;
        Start(dialog,*c,[project,pin]{Work out;if(!cache::Pin(project,pin))out.error=L"pin-failed";out.snapshot=cache::Inspect();return out;});return TRUE;
    }
    if(id==Apply || id==ClearProject || id==ClearReclaimable || id==ClearAll) {
        cache::Settings settings;if(!ReadSettings(dialog,*c,settings))return TRUE;
        if(id==ClearProject && c->selected.empty())return TRUE;
        if(id==ClearAll && MessageBoxW(dialog,Tr(*c,L"清空可删除的磁盘缓存？下次打开工程时会重新生成。正在使用的缓存会跳过。",
            L"Clear deletable disk cache? It will be rebuilt when needed. In-use files will be skipped.",L"削除可能なキャッシュを消去しますか？必要時に再生成し、使用中のファイルは残します。"),
            Tr(*c,L"缓存管理",L"Cache management",L"キャッシュ管理"),MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return TRUE;
        const auto project=c->selected;const auto language=c->language;
        Start(dialog,*c,[settings,id,project,language] {
            Work out;
            if(!cache::Configure(settings,out.error)){out.snapshot=cache::Inspect();return out;}
            if(id!=Apply) {
                out.cleaned=cache::Clean(id==ClearAll?cache::Cleanup::All:id==ClearProject?cache::Cleanup::Project:cache::Cleanup::Reclaimable,project);
                out.error=out.cleaned.error;const auto bytes=Size(out.cleaned.bytes);wchar_t text[256]{};
                swprintf_s(text,Text(language,L"已清理 %u 个文件，释放 %ls；跳过 %u 个使用中或不可删除的文件。",
                    L"Removed %u files (%ls); skipped %u in-use or unavailable files.",L"%u ファイル（%ls）を削除、使用中など %u ファイルをスキップ。"),out.cleaned.files,bytes.c_str(),out.cleaned.skipped);out.message=text;
            } else out.message=Text(language,L"缓存设置已保存。",L"Cache settings saved.",L"設定を保存しました。");
            out.snapshot=cache::Inspect();return out;
        });return TRUE;
    }
    return FALSE;
}
void RefreshMenu(MMDApp* app) {
    if(build_profile::original || !app || !app->MainWindow())return;
    HMENU bar=GetMenu(app->MainWindow());
    for(int i=0;i<GetMenuItemCount(bar);++i) {
        HMENU menu=GetSubMenu(bar,i);if(!menu || GetMenuState(menu,201,MF_BYCOMMAND)==UINT(-1))continue;
        const wchar_t* caption=Text(app->EnglishUI(),L"缓存管理…",L"Cache management…",L"キャッシュ管理…");
        if(GetMenuState(menu,kCommand,MF_BYCOMMAND)==UINT(-1))AppendMenuW(menu,MF_STRING,kCommand,caption);
        else {wchar_t old[128]{};GetMenuStringW(menu,kCommand,old,128,MF_BYCOMMAND);
            if(std::wcscmp(old,caption))ModifyMenuW(menu,kCommand,MF_BYCOMMAND|MF_STRING,kCommand,caption);}
        return;
    }
}
bool HandleCommand(MMDApp* app,unsigned command) {
    if(build_profile::original || command!=kCommand)return false;
    Show(app);return true;
}
}
