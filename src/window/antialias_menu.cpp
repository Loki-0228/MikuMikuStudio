#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/build_profile.hpp"
#include <algorithm>

namespace mikumikustudio::antialias {
namespace {
MMDApp* owner=nullptr;
HMENU parent=nullptr, popup=nullptr, smaaPopup=nullptr, outlinePopup=nullptr;
HHOOK hook=nullptr;
int position=-1;
bool selected=false;
bool OutlineEditable(const MMDApp* app) {
    const auto& settings=app->antialiasing;
    return settings.enabled && settings.algorithm>=Algorithm::Fxaa && settings.algorithm<Algorithm::Count;
}
bool Locate(HMENU menu,HMENU& result,int& index) {
    for(int i=0;i<GetMenuItemCount(menu);++i) {
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_ID|MIIM_SUBMENU;
        if(!GetMenuItemInfoW(menu,i,TRUE,&item))continue;
        if(item.wID==277){result=menu;index=i;return true;}
        if(item.hSubMenu && Locate(item.hSubMenu,result,index))return true;
    }
    return false;
}
void Populate(MMDApp* app) {
    while(GetMenuItemCount(popup)>0)DeleteMenu(popup,0,MF_BYPOSITION);
    smaaPopup=outlinePopup=nullptr;
    const wchar_t* off=app->EnglishUI()==2?L"关闭":app->EnglishUI()==1?L"Off":L"オフ";
    AppendMenuW(popup,MF_STRING,kOffCommand,off);
    MENUITEMINFOW offItem{sizeof(offItem)};offItem.fMask=MIIM_FTYPE|MIIM_STATE;offItem.fType=MFT_RADIOCHECK;
    offItem.fState=app->antialiasing.enabled?MFS_UNCHECKED:MFS_CHECKED;
    SetMenuItemInfoW(popup,kOffCommand,FALSE,&offItem);
    const wchar_t* original=app->EnglishUI()==2?L"MMD 原始（MSAA）":app->EnglishUI()==1?L"MMD original (MSAA)":L"MMD オリジナル（MSAA）";
    const wchar_t* temporal=app->EnglishUI()==2?L"TAA（运动矢量）":app->EnglishUI()==1?L"TAA (motion vectors)":L"TAA（モーションベクトル）";
    // Keep serialized algorithm IDs stable; explicit MSAA counts remain readable
    // from older projects, but the menu exposes only the original MSAA choice.
    const struct { Algorithm algorithm; const wchar_t* label; } entries[]={
        {Algorithm::MmdOriginal,original},{Algorithm::Fxaa,L"FXAA"},{Algorithm::Taa,temporal}};
    // Keep the selected algorithm while disabled; only Off is marked until
    // the parent is enabled again. Explicit MSAA counts share the original row.
    const auto checked=app->antialiasing.algorithm<=Algorithm::Msaa8?
        Algorithm::MmdOriginal:app->antialiasing.algorithm;
    for(const auto& entry:entries) {
        const auto command=kFirstCommand+unsigned(entry.algorithm);
        const bool supported=Supported(app,entry.algorithm);
        AppendMenuW(popup,MF_STRING|(supported?MF_ENABLED:MF_GRAYED),command,entry.label);
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_FTYPE|MIIM_STATE;item.fType=MFT_RADIOCHECK;
        item.fState=(supported?MFS_ENABLED:MFS_GRAYED)|
            (app->antialiasing.enabled && checked==entry.algorithm?MFS_CHECKED:MFS_UNCHECKED);
        SetMenuItemInfoW(popup,command,FALSE,&item);
    }
    smaaPopup=CreatePopupMenu();
    const Algorithm modes[]={Algorithm::SmaaStandard,Algorithm::Smaa,Algorithm::SmaaT2xStandard,Algorithm::SmaaT2xEnhanced};
    const wchar_t* chinese[]={L"SMAA 1x（标准）",L"SMAA 1x（斜线与拐角增强）",L"SMAA T2x（标准）",L"SMAA T2x（斜线与拐角增强）"};
    const wchar_t* english[]={L"SMAA 1x (standard)",L"SMAA 1x (diagonals and corners)",L"SMAA T2x (standard)",L"SMAA T2x (diagonals and corners)"};
    const wchar_t* japanese[]={L"SMAA 1x（標準）",L"SMAA 1x（斜線・コーナー強化）",L"SMAA T2x（標準）",L"SMAA T2x（斜線・コーナー強化）"};
    const auto* smaaLabels=app->EnglishUI()==2?chinese:app->EnglishUI()==1?english:japanese;
    for(unsigned i=0;i<4;++i) {
        const auto command=kFirstCommand+unsigned(modes[i]);
        AppendMenuW(smaaPopup,MF_STRING|(Supported(app,modes[i])?MF_ENABLED:MF_GRAYED),command,smaaLabels[i]);
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_FTYPE|MIIM_STATE;item.fType=MFT_RADIOCHECK;
        item.fState=(Supported(app,modes[i])?MFS_ENABLED:MFS_GRAYED)|
            (app->antialiasing.enabled && app->antialiasing.algorithm==modes[i]?MFS_CHECKED:MFS_UNCHECKED);
        SetMenuItemInfoW(smaaPopup,command,FALSE,&item);
    }
    MENUITEMINFOW smaa{sizeof(smaa)};smaa.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_STRING|MIIM_FTYPE|MIIM_STATE;
    smaa.wID=kSmaaCommand;smaa.hSubMenu=smaaPopup;smaa.dwTypeData=const_cast<wchar_t*>(L"SMAA");
    smaa.fType=MFT_RADIOCHECK;
    smaa.fState=((Supported(app,Algorithm::Smaa) || Supported(app,Algorithm::SmaaStandard))?MFS_ENABLED:MFS_GRAYED)|
        (app->antialiasing.enabled && app->antialiasing.algorithm>=Algorithm::Smaa &&
            app->antialiasing.algorithm<Algorithm::Count?MFS_CHECKED:MFS_UNCHECKED);
    InsertMenuItemW(popup,GetMenuItemCount(popup),TRUE,&smaa);
    AppendMenuW(popup,MF_SEPARATOR,0,nullptr);
    outlinePopup=CreatePopupMenu();
    const wchar_t* outlineLabel=app->EnglishUI()==2?L"描边处理":app->EnglishUI()==1?L"Outline processing":L"輪郭線処理";
    const wchar_t* fxaaOutline=app->EnglishUI()==2?L"FXAA 自适应柔化":app->EnglishUI()==1?L"FXAA adaptive softening":L"FXAA 適応平滑化";
    const wchar_t* msaaOutline=app->EnglishUI()==2?L"MSAA 覆盖＋自适应柔化":app->EnglishUI()==1?L"MSAA coverage + adaptive softening":L"MSAA カバレッジ＋適応平滑化";
    const bool editable=OutlineEditable(app);
    const struct { OutlineMode mode; unsigned command; const wchar_t* label; } outlineEntries[]={
        {OutlineMode::FxaaAdaptive,kOutlineFxaaCommand,fxaaOutline},
        {OutlineMode::Multisample,kOutlineMsaaCommand,msaaOutline}};
    for(const auto& entry:outlineEntries) {
        AppendMenuW(outlinePopup,MF_STRING|(editable?MF_ENABLED:MF_GRAYED),entry.command,entry.label);
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_FTYPE|MIIM_STATE;item.fType=MFT_RADIOCHECK;
        item.fState=(editable?MFS_ENABLED:MFS_GRAYED)|
            (app->antialiasing.outlineMode==entry.mode?MFS_CHECKED:MFS_UNCHECKED);
        SetMenuItemInfoW(outlinePopup,entry.command,FALSE,&item);
    }
    MENUITEMINFOW outline{sizeof(outline)};outline.fMask=MIIM_ID|MIIM_SUBMENU|MIIM_STRING|MIIM_STATE;
    outline.wID=kOutlineCommand;outline.hSubMenu=outlinePopup;outline.dwTypeData=const_cast<wchar_t*>(outlineLabel);
    outline.fState=editable?MFS_ENABLED:MFS_GRAYED;
    InsertMenuItemW(popup,GetMenuItemCount(popup),TRUE,&outline);
}
void Detach() {
    if(hook){UnhookWindowsHookEx(hook);hook=nullptr;}
    if(IsMenu(parent) && position>=0) {
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_SUBMENU;item.hSubMenu=nullptr;
        SetMenuItemInfoW(parent,position,TRUE,&item);
    }
    if(IsMenu(popup))DestroyMenu(popup);
    popup=parent=smaaPopup=outlinePopup=nullptr;position=-1;selected=false;
}
}
void RefreshMenu(MMDApp* app) {
    if(build_profile::original || !app || !app->MainWindow())return;
    const HMENU menu=app->FullscreenMode()?app->state.savedMenu:GetMenu(app->MainWindow());
    HMENU found=nullptr;int index=-1;
    if(Locate(menu,found,index)) {
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_STATE;
        item.fState=app->antialiasing.enabled?MFS_CHECKED:MFS_UNCHECKED;
        SetMenuItemInfoW(found,index,TRUE,&item);
    }
    if(IsMenu(popup))Populate(app);
}
LRESULT CALLBACK MenuFilter(int code,WPARAM wp,LPARAM lp) {
    if(code==MSGF_MENU && owner && position>=0) {
        const auto& message=*reinterpret_cast<MSG*>(lp);
        bool activate=message.message==WM_KEYDOWN && message.wParam==VK_RETURN && selected;
        if(message.message==WM_LBUTTONUP) {
            RECT rect{};
            activate=selected && GetMenuItemRect(owner->MainWindow(),parent,position,&rect) && PtInRect(&rect,message.pt);
            // Clicking the arrow opens the submenu; the label remains a toggle.
            if(activate && message.pt.x>=rect.right-GetSystemMetrics(SM_CXMENUCHECK)-8)activate=false;
        }
        if(activate){EndMenu();PostMessageW(owner->MainWindow(),WM_COMMAND,277,0);return 1;}
    }
    return CallNextHookEx(hook,code,wp,lp);
}
bool HandleMessage(MMDApp* app,UINT message,WPARAM wp,LPARAM lp) {
    if(build_profile::original)return false;
    if(message==WM_ENTERMENULOOP) {
        Detach();owner=app;Initialize(app);RefreshMenu(app);
        if(Locate(GetMenu(app->MainWindow()),parent,position)) {
            hook=SetWindowsHookExW(WH_MSGFILTER,MenuFilter,nullptr,GetCurrentThreadId());
            if(hook) {
                popup=CreatePopupMenu();Populate(app);
                MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_SUBMENU;item.hSubMenu=popup;
                SetMenuItemInfoW(parent,position,TRUE,&item);
            }
        }
    } else if(message==WM_MENUSELECT) {
        selected=reinterpret_cast<HMENU>(lp)==parent &&
            ((HIWORD(wp)&MF_POPUP)?LOWORD(wp)==position:LOWORD(wp)==277);
    } else if(message==WM_EXITMENULOOP || message==WM_DESTROY) {
        Detach();if(message==WM_DESTROY)owner=nullptr;
    } else if(message==WM_COMMAND && HIWORD(wp)==0 &&
              ((LOWORD(wp)>=kFirstCommand && LOWORD(wp)<kFirstCommand+unsigned(Algorithm::Count)) ||
               LOWORD(wp)==kSmaaCommand || LOWORD(wp)==kOffCommand ||
               LOWORD(wp)==kOutlineCommand || LOWORD(wp)==kOutlineFxaaCommand || LOWORD(wp)==kOutlineMsaaCommand)) {
        // The SMAA parent only opens its submenu. Ignore the former shortcut
        // command so stale queued input cannot apply a remembered variant.
        if(LOWORD(wp)==kSmaaCommand || LOWORD(wp)==kOutlineCommand)return true;
        std::wstring error;
        Initialize(app);
        if(LOWORD(wp)==kOutlineFxaaCommand || LOWORD(wp)==kOutlineMsaaCommand) {
            if(!OutlineEditable(app))return true;
            const auto mode=LOWORD(wp)==kOutlineFxaaCommand?OutlineMode::FxaaAdaptive:OutlineMode::Multisample;
            if(!SelectOutlineMode(app,mode,error))
                MessageBoxW(app->MainWindow(),error.c_str(),DialogTitle(app),MB_OK|MB_ICONERROR);
            return true;
        }
        if(LOWORD(wp)==kOffCommand) {
            if(app->antialiasing.enabled && !Toggle(app,error))
                MessageBoxW(app->MainWindow(),error.c_str(),DialogTitle(app),MB_OK|MB_ICONERROR);
            return true;
        }
        const auto algorithm=Algorithm(LOWORD(wp)-kFirstCommand);
        // Choosing an algorithm is also an explicit request to turn AA on.
        // If activation fails, restore the former enabled state as well.
        const bool wasEnabled=app->antialiasing.enabled;
        app->antialiasing.enabled=true;
        if(!Select(app,algorithm,error)) {
            app->antialiasing.enabled=wasEnabled;ApplyRenderState(app);RefreshMenu(app);
            MessageBoxW(app->MainWindow(),error.c_str(),DialogTitle(app),MB_OK|MB_ICONERROR);
        }
        return true;
    }
    return false;
}
}
