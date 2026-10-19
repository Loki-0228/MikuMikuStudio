// Exercise production mouse messages and repainting without opening a user window.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/force_paste.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/plugin_menu.h"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/model.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace mikumikustudio {
void BuildAccessoryOrderArray(MMDApp*, int);
void ApplyAccessorySettingsDialog(MMDApp*, int, HWND);
void DisablePlaybackMenus(HWND);
void RestorePlaybackMenus(MMDApp*);
void DispatchLetterHotkeys(MMDApp*, HWND);
}
namespace {
bool expectExpressionPaste = false;
bool cancelRecentOpen = false;
unsigned expressionConfirmations = 0;
unsigned menuBarDraws = 0, recentItemDeletes = 0;
HMENU observedRecentMenu = nullptr;
decltype(&DrawMenuBar) originalDrawMenuBar = nullptr;
decltype(&DeleteMenu) originalDeleteMenu = nullptr;
BOOL WINAPI ObserveMenuBar(HWND window) {
    if(g_Block && window==g_Block->MainWindow())++menuBarDraws;
    return originalDrawMenuBar(window);
}
BOOL WINAPI ObserveMenuDelete(HMENU menu,UINT item,UINT flags) {
    if(menu==observedRecentMenu)++recentItemDeletes;
    return originalDeleteMenu(menu,item,flags);
}
void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) std::exit(1);
}
void CheckMenuRegression(bool ok,const char* label) {
    if(GetEnvironmentVariableW(L"MMS_MENU_REDRAW_BASELINE",nullptr,0)) {
        std::printf("BASELINE %s %s\n",ok?"PASS":"REGRESSION",label);
        return;
    }
    Check(ok,label);
}
int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("Unexpected dialog: %s\n", text); std::exit(2);
}
int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR title, UINT flags) {
    if(cancelRecentOpen)return IDCANCEL;
    if (expectExpressionPaste && std::wcscmp(title, L"Force paste expression") == 0 &&
        (flags & 0xf) == MB_OKCANCEL) {
        ++expressionConfirmations;
        return IDCANCEL;
    }
    std::fwprintf(stderr, L"Unexpected dialog: %ls\n", text); std::exit(2);
}
decltype(&ShowWindow) originalShow = nullptr;
BOOL WINAPI KeepHidden(HWND window, int command) { return originalShow(window, GetParent(window) ? command : SW_HIDE); }
decltype(&RedrawWindow) originalRedraw = nullptr;
unsigned redraws = 0, presents = 0;
bool observe = false;
BOOL WINAPI ObserveRedraw(HWND window, const RECT* rect, HRGN region, UINT flags) {
    const BOOL result = originalRedraw(window, rect, region, flags);
    if (observe && window == g_Block->MainWindow() && (flags & RDW_UPDATENOW)) {
        ++redraws;
        const UINT required = RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN;
        Check(!rect && !region && (flags & required) == required,
              "release repaints the whole parent and all child controls");
        Check(g_Block->WindowLayoutReady() != 0, "paint occurs only after layout is complete");
        // Hidden hosts do not receive normal visible-window paints. Dispatch
        // that paint explicitly to exercise the real GDI + D3D paint path.
        SendMessageW(window, WM_PAINT, 0, 0);
    }
    return result;
}
using PresentFunction = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
PresentFunction originalPresent = nullptr;
HRESULT WINAPI ObservePresent(IDirect3DDevice9* device, const RECT* source,
                              const RECT* destination, HWND window, const RGNDATA* dirty) {
    if (observe && window == g_Block->MainWindow()) {
        ++presents;
        RECT expected = FramePresentationRect(g_Block);
        Check(source && destination && EqualRect(source, &expected) && EqualRect(destination, &expected),
              "scene presentation covers the resized viewport");
    }
    return originalPresent(device, source, destination, window, dirty);
}
template<class Function> void Hook(Function target, Function detour, Function* original = nullptr) {
    Check(MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
        reinterpret_cast<void**>(original)) == MH_OK, "create test hook");
    Check(MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK, "enable test hook");
}
RECT Bounds(HWND window) { RECT rect{}; GetWindowRect(window, &rect); return rect; }
RECT ControlBounds(HWND parent, int id) {
    RECT rect = Bounds(GetDlgItem(parent, id));
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}
void SaveForcePasteRow(MMDApp* app, const char* filename) {
    HWND host = app->MainWindow();
    RECT client{}; GetClientRect(host, &client);
    const int width = app->SidebarWidth(), height = 104, top = client.bottom - 276;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Check(bitmap && pixels, "create native toolbar review surface");
    const auto previous = SelectObject(dc, bitmap);
    SetViewportOrgEx(dc, 0, -top, nullptr);
    HandlePaletteChanged(dc);
    for (int id : {427, 428, 429, 420, 421, 422, 416, 423, panel::kForcePasteButton, 425, 426, 434, 415, 424}) {
        HWND child = GetDlgItem(host, id);
        RECT rect = ControlBounds(host, id);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
        SetViewportOrgEx(dc, rect.left, rect.top - top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
    }
    GdiFlush();
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + width * height * 4;
    std::ofstream stream(filename, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    stream.write(static_cast<const char*>(pixels), width * height * 4);
    Check(bool(stream), "save native toolbar review image");
    SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
}


HDC previewDC=nullptr;
decltype(&BeginPaint) realBeginPaint=nullptr;
decltype(&EndPaint) realEndPaint=nullptr;
HDC WINAPI PreviewBeginPaint(HWND h,LPPAINTSTRUCT ps) {
    if(!previewDC)return realBeginPaint(h,ps);
    *ps={};ps->hdc=previewDC;GetClientRect(h,&ps->rcPaint);return previewDC;
}
BOOL WINAPI PreviewEndPaint(HWND h,const PAINTSTRUCT* ps) {
    return previewDC?TRUE:realEndPaint(h,ps);
}
void SaveBottomPreview(MMDApp* app,const char* filename) {
    HWND host=app->MainWindow();RECT client{};GetClientRect(host,&client);
    const int width=client.right,height=160,top=client.bottom-height;
    BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB};
    void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    auto previous=SelectObject(dc,bitmap);SetViewportOrgEx(dc,0,-top,nullptr);
    HandlePaletteChanged(dc);
    const auto ready=app->WindowLayoutReady();app->WindowLayoutReady()=0;
    previewDC=dc;HandleWindowPaint(app);previewDC=nullptr;app->WindowLayoutReady()=ready;
    const bool camera=app->CameraMode()!=0;
    for(HWND child=GetWindow(host,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        const int id=GetDlgCtrlID(child);RECT rect=ControlBounds(host,id);
        if(rect.top<client.bottom-157 || rect.top>=client.bottom)continue;
        bool show=(id>=430&&id<=445)||id==530||id==32901||
                  (id>=402&&id<=414)||id==531||id==534||id==535;
        if(camera)show|=(id>=446&&id<=452)||id==453||id==469||id==566||
                        (id>=471&&id<=488)||id==32900;
        else show|=(id>=490&&id<=501)||id==502||(id>=504&&id<=528)||
                   id==32902||id==32904||id==32905||id==32906;
        if(!show)continue;
        int saved=SaveDC(dc);IntersectClipRect(dc,rect.left,rect.top,rect.right,rect.bottom);
        SetViewportOrgEx(dc,rect.left,rect.top-top,nullptr);
        SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND|PRF_CHILDREN);
        previewDC=dc; SendMessageW(child,WM_PAINT,reinterpret_cast<WPARAM>(dc),0); previewDC=nullptr;
        RestoreDC(dc,saved);
    }
    GdiFlush();BITMAPFILEHEADER file{};file.bfType=0x4d42;
    file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+width*height*4;
    wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr,executable,MAX_PATH);
    std::ofstream stream(std::filesystem::path(executable).parent_path()/filename,std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file),sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    stream.write(static_cast<const char*>(pixels),width*height*4);
    Check(bool(stream),"save bottom panel native preview");
    SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);
}
void CheckBottomPanelResize(MMDApp* app) {
    if (const wchar_t* path=_wgetenv(L"MMS_PANEL_PREVIEW_MODEL")) {
        Check(std::filesystem::exists(path),"preview model exists");
        LoadModelFile(app,path,false);Check(app->SelectedModel()!=nullptr,"preview model loaded");
    }
    HWND host=app->MainWindow();
    SetWindowPos(host,nullptr,0,0,3600,1100,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    app->CameraMode()=1;
    for(int i=1;i<7;++i)app->UiOptionFlag(i)=1;
    app->UiOptionFlag(2)=0;app->UiOptionFlag(6)=0;
    HandleWindowSize(app);
    RECT client{};GetClientRect(host,&client);
    const int y=client.bottom-150;
    auto drag=[&](int id,int delta,UINT ending=WM_LBUTTONUP) {
        int edge=0;
        for(auto p:BottomPanelSpans(app))if(p.id==id)edge=BottomPanelX(app,p.right);
        SendMessageW(host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(edge,y));
        Check(app->bottomPanelSizing.dragging==id,"right border captures the selected panel");
        const auto beforeModel=ControlBounds(host,436),beforeAccessory=ControlBounds(host,471);
        const int beforeExtra=app->bottomPanelSizing.extra[id];
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge+delta,y));
        const auto duringModel=ControlBounds(host,436),duringAccessory=ControlBounds(host,471);
        Check(EqualRect(&beforeModel,&duringModel) && EqualRect(&beforeAccessory,&duringAccessory) &&
              beforeExtra==app->bottomPanelSizing.extra[id],"panel geometry stays unchanged throughout dragging");
        SendMessageW(host,ending,0,MAKELPARAM(edge+delta,y));
        Check(app->bottomPanelSizing.dragging<0 && GetCapture()!=host,"panel drag releases capture");
    };
    app->EnglishUI()=2;LocalizeUI(app);
    SaveBottomPreview(app,"bottom-before.bmp");
    const auto model=ControlBounds(host,436),camera=ControlBounds(host,449);
    drag(1,140);
    auto wide=ControlBounds(host,436),moved=ControlBounds(host,449);
    Check(wide.right-wide.left>model.right-model.left+100,"model selector expands with its panel");
    Check(moved.left==camera.left+140,"following camera panel moves by the added width");
    HandleWindowSize(app);
    auto repeat=ControlBounds(host,436);
    Check(EqualRect(&wide,&repeat),"repeated layout does not accumulate panel stretching");
    drag(2,100);
    auto follow=ControlBounds(host,449);
    Check(follow.right-follow.left>moved.right-moved.left,"camera follow dropdown expands");
    const auto acc=ControlBounds(host,471);
    drag(6,160);
    auto accWide=ControlBounds(host,471);
    Check(accWide.right-accWide.left>acc.right-acc.left,"accessory name selector expands");
    SaveBottomPreview(app,"bottom-after.bmp");
    drag(1,50,WM_CANCELMODE);
    Check(app->bottomPanelSizing.extra[1]==140,"cancel restores previous panel width");
    drag(1,50,WM_CAPTURECHANGED);
    Check(app->bottomPanelSizing.extra[1]==140,"lost capture restores previous panel width");
    drag(1,10000);
    auto volume=ControlBounds(host,534);
    Check(volume.right<=client.right,"maximum drag keeps volume inside the window");
    drag(1,-10000);
    Check(app->bottomPanelSizing.extra[1]==0,"dragging left respects original minimum width");
    app->UiOptionFlag(1)=0;HandleWindowSize(app);
    app->UiOptionFlag(1)=1;HandleWindowSize(app);
    Check(app->bottomPanelSizing.extra[2]==100,"collapse and expand retain chosen width");
    auto modelRecord=std::make_unique<mdl::ModelRecord>();
    auto oldModel=app->ModelSlot(0);const int oldSlot=app->SelectedModelSlot();
    if(!oldModel)app->ModelSlot(0)=reinterpret_cast<unsigned char*>(modelRecord.get());app->SetSelectedModelSlot(0);
    app->CameraMode()=0;HandleWindowSize(app);
    auto eye=ControlBounds(host,509),mouth=ControlBounds(host,519);
    drag(7,260);
    auto eyeWide=ControlBounds(host,509),mouthWide=ControlBounds(host,519);
    Check(eyeWide.right-eyeWide.left>eye.right-eye.left && mouthWide.right-mouthWide.left>mouth.right-mouth.left,
          "both expression columns expand their name selectors");
    if(oldModel)SaveBottomPreview(app,"bottom-morph-after.bmp");
    app->CameraMode()=1;app->ModelSlot(0)=oldModel;app->SetSelectedModelSlot(oldSlot);
    app->bottomPanelSizing={};
    SetWindowPos(host,nullptr,0,0,1200,820,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);HandleWindowSize(app);
}

unsigned fileBrowsers=0;
BOOL WINAPI RecentMenuBounds(HWND,HMENU,UINT,LPRECT r){*r={100,100,300,125};return TRUE;}
BOOL CancelFileBrowser(LPOPENFILENAMEW) { ++fileBrowsers;return FALSE; }
void CheckAccessoryMenu(MMDApp* app) {
    HWND host = app->MainWindow();
    HMENU menu = GetMenu(host);
    auto first = std::make_unique<mdl::AccessoryRecord>();
    auto second = std::make_unique<mdl::AccessoryRecord>();
    const auto savedPrecision = app->TransparentMaterialSortPrecision();
    for (auto precision : {TransparentSortPrecision::Off, TransparentSortPrecision::Standard}) {
        app->TransparentMaterialSortPrecision() = precision;
        app->AccessorySlot(17) = first.get();
        app->AccessorySlot(93) = second.get();
        EnableMenuItem(menu, 249, MF_GRAYED);
        SendMessageW(host, WM_INITMENUPOPUP, 0, 0);
        Check((GetMenuState(menu,249,MF_BYCOMMAND) & (MF_DISABLED|MF_GRAYED)) == 0,
              "accessory edit recovers stale disabled state with sorting on/off");
        first->order = 0; second->order = 1;
        mdl::AccessoryRecord* order[2]{};
        app->AccessoryOrderArray() = order;
        BuildAccessoryOrderArray(app, 2);
        Check(order[0] == first.get() && order[1] == second.get(),
              "accessory dialog reads authored order with sorting on/off");
        std::swap(order[0], order[1]);
        HWND dialog = CreateWindowExW(0,L"STATIC",L"",0,0,0,100,100,nullptr,nullptr,nullptr,nullptr);
        HWND list = CreateWindowExW(0,L"LISTBOX",L"",WS_CHILD,0,0,100,100,dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kOrderListBox)),nullptr,nullptr);
        SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"second"));
        SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"first"));
        app->AccessoryRenderSplitOrder() = 1;
        ApplyAccessorySettingsDialog(app,2,dialog);
        Check(second->order == 0 && first->order == 1 && app->SelectedAccessorySlot() == 93,
              "accessory dialog commits new order and selects correct slot with sorting on/off");
        Check(app->AccessoryRenderSplitOrder() == 1,
              "manual model-before/after boundary survives transparent sorting");
        app->AccessoryOrderArray() = nullptr;
        DestroyWindow(dialog);
        app->AccessorySlot(17) = nullptr;
        SendMessageW(host, WM_INITMENUPOPUP, 0, 0);
        Check((GetMenuState(menu,249,MF_BYCOMMAND) & (MF_DISABLED|MF_GRAYED)) == 0,
              "accessory edit remains available with one accessory remaining");
        app->AccessorySlot(93) = nullptr;
        SendMessageW(host, WM_INITMENUPOPUP, 0, 0);
        Check((GetMenuState(menu,249,MF_BYCOMMAND) & MF_GRAYED) != 0,
              "accessory edit disabled only when accessory list is empty");
        Check(app->TransparentMaterialSortPrecision() == precision,
              "accessory availability preserves sorting selection");
    }
    app->TransparentMaterialSortPrecision() = savedPrecision;
}

struct MenuPluginFixture {
    unsigned populated=0, first=0;
    bool checked=false;
    static const wchar_t* __cdecl Title(void*,uint32_t language) {
        return language==2?L"菜单测试":language==1?L"Menu test":L"メニューテスト";
    }
    static void __cdecl Populate(void* data,HMENU menu,uint32_t first,uint32_t) {
        auto& fixture=*static_cast<MenuPluginFixture*>(data);
        ++fixture.populated;fixture.first=first;
        while(GetMenuItemCount(menu)>0)DeleteMenu(menu,0,MF_BYPOSITION);
        AppendMenuW(menu,MF_STRING|(fixture.checked?MF_CHECKED:0),first,L"Dynamic item");
    }
    static void __cdecl Command(void*,uint32_t) {}
};
void CheckPluginMenuRefresh(MMDApp* app) {
    const auto get=reinterpret_cast<MmsGetMenuHostApiFn>(GetProcAddress(GetModuleHandleW(nullptr),"MMSGetMenuHostApi"));
    MmsMenuHostApi api{sizeof(api)};
    Check(get&&get(MMS_MENU_ABI_VERSION,&api),"menu host API remains available");
    MenuPluginFixture fixture;
    MmsPluginMenu description{sizeof(description),MMS_MENU_ABI_VERSION,app->MainWindow(),
        "menu-redraw-regression",&fixture,MenuPluginFixture::Title,MenuPluginFixture::Populate,MenuPluginFixture::Command,nullptr};
    menuBarDraws=0;
    const auto token=api.registerMenu(&description);
    Check(token&&menuBarDraws>0,"new plugin root entry redraws the menu bar");
    HMENU root=GetMenu(app->MainWindow());
    HMENU popup=GetSubMenu(root,GetMenuItemCount(root)-1);
    const auto populated=fixture.populated;
    fixture.checked=true;menuBarDraws=0;
    api.refreshMenu(token);
    std::printf("plugin_menu_refresh unchanged_bar_draws=%u populate_delta=%u\n",menuBarDraws,fixture.populated-populated);
    CheckMenuRegression(menuBarDraws==0,"plugin item state update does not redraw unchanged root title");
    Check(fixture.populated==populated+1&&(GetMenuState(popup,fixture.first,MF_BYCOMMAND)&MF_CHECKED),
        "plugin callbacks still refresh dynamic item check state");
    for(int language:{0,1,2}) {
        app->EnglishUI()=language;menuBarDraws=0;plugin_menu::Refresh(app);
        Check(menuBarDraws>0,"changed plugin root title redraws for each language");
        wchar_t text[80]{};GetMenuStringW(root,GetMenuItemCount(root)-1,text,80,MF_BYPOSITION);
        Check(!std::wcscmp(text,MenuPluginFixture::Title(nullptr,language)),"plugin root caption follows all three languages");
    }
    menuBarDraws=0;api.unregisterMenu(token);
    Check(menuBarDraws>0&&!IsMenu(popup),"plugin removal redraws and destroys its popup");
}
void CheckRecentProjects(MMDApp* app) {
    namespace fs=std::filesystem;
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    auto folder=fs::path(executable).parent_path()/L"recent-pmm-test";fs::create_directories(folder);
    auto store=folder/L"history.txt";SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",store.c_str());
    std::ofstream(store,std::ios::binary|std::ios::trunc).close();
    for(int i=0;i<12;++i) {
        auto path=folder/(L"工程 & 动作 "+std::to_wstring(i)+L".pmm");std::ofstream(path).close();
        recent_projects::Record(app,path.c_str());
    }
    auto newest=folder/L"工程 & 动作 3.pmm";recent_projects::Record(app,newest.c_str());
    recent_projects::Record(app,(folder/L"ignored.vmd").c_str());
    HWND host=app->MainWindow();app->EnglishUI()=2;LocalizeUI(app);
    Hook(&DrawMenuBar,&ObserveMenuBar,&originalDrawMenuBar);
    Hook(&DeleteMenu,&ObserveMenuDelete,&originalDeleteMenu);
    SendMessageW(host,WM_ENTERMENULOOP,FALSE,0);
    HMENU file=GetSubMenu(GetMenu(host),0);MENUITEMINFOW open{sizeof(open)};open.fMask=MIIM_ID|MIIM_SUBMENU;
    Check(GetMenuItemInfoW(file,205,FALSE,&open)&&open.wID==205&&open.hSubMenu,"Open keeps original command and gains native hover submenu");
    Check(GetMenuItemCount(open.hSubMenu)==14,"recent list caps at ten with browse fallback");
    observedRecentMenu=open.hSubMenu;menuBarDraws=recentItemDeletes=0;
    SendMessageW(host,WM_INITMENU,reinterpret_cast<WPARAM>(GetMenu(host)),0);
    const unsigned initialDraws=menuBarDraws;
    CheckMenuRegression(initialDraws==0,"opening an unchanged menu bar without plugins does not redraw the bar");
    menuBarDraws=0;
    for(int opening=0;opening<3;++opening)
        SendMessageW(host,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(open.hSubMenu),0);
    std::printf("recent_menu_refresh initial_bar_draws=%u popup_bar_draws=%u recent_deletes=%u\n",
        initialDraws,menuBarDraws,recentItemDeletes);
    CheckMenuRegression(menuBarDraws==0,"opening recent submenu does not redraw the unchanged menu bar");
    CheckMenuRegression(recentItemDeletes==0,"repeated recent submenu opening preserves its existing rows");
    std::filesystem::remove(newest);
    SendMessageW(host,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(open.hSubMenu),0);
    Check((GetMenuState(open.hSubMenu,34800,MF_BYCOMMAND)&MF_GRAYED)!=0,"file removed during menu loop becomes disabled");
    std::ofstream(newest).close();
    SendMessageW(host,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(open.hSubMenu),0);
    Check((GetMenuState(open.hSubMenu,34800,MF_BYCOMMAND)&(MF_GRAYED|MF_DISABLED))==0,"restored file becomes available without reopening the menu loop");
    wchar_t caption[1024]{};GetMenuStringW(open.hSubMenu,34800,caption,1024,MF_BYCOMMAND);
    Check(wcsstr(caption,L"工程 && 动作 3.pmm")!=nullptr,"Unicode history persists, deduplicates, promotes latest and escapes menu ampersands");
    Hook(&GetUiOpenFileNameW,&CancelFileBrowser);
    const auto dirty=app->SceneModified();app->SceneModified()=0;
    int position=0;for(;position<GetMenuItemCount(file);++position)if(GetSubMenu(file,position)==open.hSubMenu)break;
    SendMessageW(host,WM_MENUSELECT,MAKEWPARAM(position,MF_POPUP),reinterpret_cast<LPARAM>(file));
    MSG navigate{};navigate.message=WM_KEYDOWN;navigate.wParam=VK_RIGHT;
    Check(recent_projects::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&navigate))==0,"right arrow remains native submenu navigation");
    navigate.message=WM_MOUSEMOVE;
    Check(recent_projects::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&navigate))==0,"hover remains native submenu navigation");
    MSG enter{};enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
    Check(recent_projects::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&enter))==1,"Enter on original Open requests file browser immediately");
    SendMessageW(host,WM_EXITMENULOOP,FALSE,0);
    MSG command{};Check(PeekMessageW(&command,host,WM_COMMAND,WM_COMMAND,PM_REMOVE)&&command.wParam==205,"original Open posts unchanged command 205");
    DispatchMessageW(&command);Check(fileBrowsers==1,"original Open reaches file browser without selecting a recent file");
    SendMessageW(host,WM_ENTERMENULOOP,FALSE,0);
    Hook(&GetMenuItemRect,&RecentMenuBounds);
    MSG click{};click.message=WM_LBUTTONUP;click.pt={150,110};
    // A hidden File popup can retain the same screen bounds as a View row.
    // Clicking the scale command must not be intercepted as Open (205).
    HMENU view = nullptr;
    for (int i=0;i<GetMenuItemCount(GetMenu(host));++i) {
        const auto child=GetSubMenu(GetMenu(host),i);
        if(GetMenuState(child,viewport_render_scale::kCommand,MF_BYCOMMAND)!=UINT(-1))view=child;
    }
    Check(view!=nullptr,"find the viewport scale command in the real View menu");
    SendMessageW(host,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(file),0);
    SendMessageW(host,WM_UNINITMENUPOPUP,reinterpret_cast<WPARAM>(file),0);
    SendMessageW(host,WM_MENUSELECT,MAKEWPARAM(viewport_render_scale::kCommand,0),reinterpret_cast<LPARAM>(view));
    Check(recent_projects::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&click))==0,
        "clicking viewport scale ignores stale Open menu bounds");
    Check(!PeekMessageW(&command,host,WM_COMMAND,WM_COMMAND,PM_REMOVE),
        "viewport scale click never posts an Open command");
    SendMessageW(host,WM_INITMENUPOPUP,reinterpret_cast<WPARAM>(file),0);
    SendMessageW(host,WM_MENUSELECT,MAKEWPARAM(position,MF_POPUP),reinterpret_cast<LPARAM>(file));
    Check(recent_projects::MenuFilter(MSGF_MENU,0,reinterpret_cast<LPARAM>(&click))==1,"clicking original Open bypasses submenu navigation");
    MH_DisableHook(reinterpret_cast<void*>(&GetMenuItemRect));MH_RemoveHook(reinterpret_cast<void*>(&GetMenuItemRect));
    SendMessageW(host,WM_EXITMENULOOP,FALSE,0);
    Check(PeekMessageW(&command,host,WM_COMMAND,WM_COMMAND,PM_REMOVE)&&command.wParam==205,"mouse click dispatches original Open");
    DispatchMessageW(&command);Check(fileBrowsers==2,"mouse click reaches the file browser once");
    app->SceneModified()=1;cancelRecentOpen=true;
    const std::wstring previous=app->EnvFileName();SendMessageW(host,WM_COMMAND,34800,0);
    cancelRecentOpen=false;Check(previous==app->EnvFileName()&&app->SceneModified(),"canceling unsaved-scene confirmation leaves current project untouched");
    app->SceneModified()=0;SendMessageW(host,WM_COMMAND,205,0);Check(fileBrowsers==3,"ordinary Open still opens file browser after menu closes");
    for(int language:{0,1,2}) {
        app->EnglishUI()=language;LocalizeUI(app);
        SendMessageW(host,WM_ENTERMENULOOP,FALSE,0);
        file=GetSubMenu(GetMenu(host),0);open.hSubMenu=nullptr;
        Check(GetMenuItemInfoW(file,205,FALSE,&open)&&open.hSubMenu,"recent submenu reattaches after language change");
        GetMenuStringW(open.hSubMenu,34810,caption,1024,MF_BYCOMMAND);
        const wchar_t* expected=language==0?L"参照…":language==1?L"Browse…":L"浏览文件…";
        Check(std::wcscmp(caption,expected)==0,"recent browse caption follows Japanese English and Chinese");
        HMENU attached=open.hSubMenu;
        SendMessageW(host,WM_EXITMENULOOP,FALSE,0);
        Check(!IsMenu(attached),"exiting menu loop destroys owned recent submenu");
        open.hSubMenu=attached;
        Check(GetMenuItemInfoW(file,205,FALSE,&open)&&!open.hSubMenu,"Open command returns to a plain item after menu exit");
    }
    observedRecentMenu=nullptr;
    CheckPluginMenuRefresh(app);
    MH_DisableHook(reinterpret_cast<void*>(&DrawMenuBar));MH_RemoveHook(reinterpret_cast<void*>(&DrawMenuBar));
    MH_DisableHook(reinterpret_cast<void*>(&DeleteMenu));MH_RemoveHook(reinterpret_cast<void*>(&DeleteMenu));
    MH_DisableHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));MH_RemoveHook(reinterpret_cast<void*>(&GetUiOpenFileNameW));
    app->SceneModified()=dirty;SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE",nullptr);
}
void CheckForcePasteControls(MMDApp* app) {
    const int language = app->EnglishUI();
    const int width = app->SidebarWidth();
    const float ratio = app->SidebarRatio();
    HWND host = app->MainWindow(), button = GetDlgItem(host, panel::kForcePasteButton);
    Check(button != nullptr, "production host creates the force paste button");
    const auto savedCounts = app->ClipboardCounts();
    app->ClipboardCounts() = {};
    RefreshForcePasteControls(app);
    Check(!IsWindowEnabled(button), "empty clipboard disables force paste");
    app->ClipboardCounts().accessories = 1;
    RefreshForcePasteControls(app);
    Check(IsWindowEnabled(button), "accessory clipboard enables the shared force paste command");
    DisablePlaybackMenus(host);
    Check(!IsWindowEnabled(button), "starting playback disables force paste");
    RestorePlaybackMenus(app);
    Check(IsWindowEnabled(button), "ending playback restores force paste availability");
    // A model with only expression data must enable the new entry as well.
    auto model = std::make_unique<mdl::ModelRecord>();
    const auto previousModel = app->ModelSlot(0);
    const int previousSlot = app->SelectedModelSlot();
    const auto camera = app->CameraMode();
    app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get()); app->SetSelectedModelSlot(0);
    app->CameraMode() = 0; app->ClipboardCounts() = {}; app->ClipboardCounts().morphs = 1;
    RefreshForcePasteControls(app);
    Check(IsWindowEnabled(button) && !(GetMenuState(GetMenu(host), 250, MF_BYCOMMAND) & MF_GRAYED),
          "expression-only clipboard enables both toolbar and menu");
    mdl::MorphRecord morph{}; morph.panel = mdl::MorphPanel::eye;
    mdl::FrameGroup row{}; row.selected = 1;
    model->morphCount = model->facialFrameCount = 1;
    model->morphs = &morph; model->displayFrames = &row;
    mdl::MorphClipboardRecord record{}; record.value = .75f;
    const auto previousClipboard = app->MorphClipboard();
    app->MorphClipboard() = &record; app->EnglishUI() = 1;
    expectExpressionPaste = true;
    SendMessageW(button, BM_CLICK, 0, 0);
    Check(expressionConfirmations == 1, "real button click dispatches the expression force paste operation");
    const auto previousCtrl = app->state.ctrlModifierState;
    app->state.ctrlModifierState = 3;
    unsigned char keyboard[256]{};
    app->LetterHotkeys().Update(keyboard, true);
    keyboard['F'] = 0x80; app->LetterHotkeys().Update(keyboard, true);
    DispatchLetterHotkeys(app, button);
    Check(expressionConfirmations == 2, "Ctrl+F dispatches the same expression force paste operation");
    keyboard['F'] = 0; app->LetterHotkeys().Update(keyboard, false);
    app->state.ctrlModifierState = previousCtrl; app->MorphClipboard() = previousClipboard;
    expectExpressionPaste = false;
    app->ModelSlot(0) = previousModel; app->SetSelectedModelSlot(previousSlot); app->CameraMode() = camera;
    app->ClipboardCounts() = savedCounts;
    for (int lang : {0, 1, 2}) {
        app->EnglishUI() = lang; LocalizeUI(app);
        wchar_t caption[64]{}; GetWindowTextW(button, caption, 64);
        Check(std::wcscmp(caption, lang == 2 ? L"强制粘贴" : lang == 1 ? L"Force paste" : L"強制貼付") == 0,
              "force paste caption follows the selected UI language");
        for (int sidebar : {250, 310, 328, 400, 600}) {
            app->SidebarWidth() = sidebar; RelayoutSidebarControls(app);
            const RECT rect = ControlBounds(host, panel::kForcePasteButton);
            HDC dc = GetDC(button); auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
            SIZE text{}; GetTextExtentPoint32W(dc, caption, static_cast<int>(std::wcslen(caption)), &text);
            SelectObject(dc, oldFont); ReleaseDC(button, dc);
            Check(rect.left >= 0 && rect.right <= sidebar && text.cx + 6 <= rect.right - rect.left && text.cy <= rect.bottom - rect.top,
                  "force paste remains inside the sidebar with an unclipped localized caption");
            const RECT remove = ControlBounds(host, 423);
            Check(sidebar >= 328 ? rect.left > remove.right && rect.top == remove.top : rect.top > remove.bottom,
                  "force paste follows Delete and wraps only when the sidebar is too narrow");
            bool separate = true;
            for (int id : {428, 429, 420, 421, 422, 416, 423, 425, 426, 434, 415, 424}) {
                RECT intersection{}, other = ControlBounds(host, id);
                // A closed combo box has a tall window rect for its dropdown.
                if (id == 434) other.bottom = other.top + 24;
                separate &= !IntersectRect(&intersection, &rect, &other);
            }
            Check(separate, "force paste does not cover any existing timeline control");
            if (lang == 2 && (sidebar == 250 || sidebar == 400)) {
                const auto filename = "force-paste-" + std::to_string(sidebar) + ".bmp";
                SaveForcePasteRow(app, filename.c_str());
            }
        }
    }
    app->EnglishUI() = language; LocalizeUI(app);
    app->SidebarWidth() = width; app->SidebarRatio() = ratio; RelayoutSidebarControls(app);
    RefreshForcePasteControls(app);
}
}

#include "window_cursor_checks.hpp"

namespace {
void CheckShadowFilter(MMDApp* app) {
    const auto preference = std::filesystem::path(app->ExeDir()) / L"Data/adaptive_shadow.ini";
    const bool existed = std::filesystem::exists(preference);
    std::ifstream input(preference, std::ios::binary);
    const std::string previous((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const bool adaptive = app->AdaptiveShadowResolution(), filter = app->ShadowFilterEnhanced();
    const float range = app->ShadowDistance();
    const int modified = app->SceneModified(), language = app->EnglishUI();
    HWND host = app->MainWindow(), button = GetDlgItem(host, panel::kShadowFilterCheckbox);
    Check(button != nullptr && IsWindowEnabled(button), "full build offers the spline filter");
    { std::ofstream out(preference); out << "1 9999\n"; }
    LoadShadowResolutionPreference(app);
    Check(app->AdaptiveShadowResolution() && !app->ShadowFilterEnhanced(),
          "old numeric precision never opts into enhanced filtering");
    for (bool enabled : {true, false, true}) {
        SendMessageW(button, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(host, WM_COMMAND, MAKEWPARAM(panel::kShadowFilterCheckbox, BN_CLICKED), reinterpret_cast<LPARAM>(button));
        Check(app->ShadowFilterEnhanced() == enabled, "filter checkbox dispatches its real setting");
        app->ShadowFilterEnhanced() = !enabled;
        LoadShadowResolutionPreference(app);
        Check(app->ShadowFilterEnhanced() == enabled && app->AdaptiveShadowResolution(),
              "filter preference round trips independently of dynamic precision");
        Check(app->ShadowDistance() == range && app->SceneModified() == modified,
              "filter toggle preserves project shadow range and modified state");
    }
    for (int lang : {0, 1, 2}) {
        app->EnglishUI() = lang; LocalizeUI(app); HandleWindowSize(app);
        const RECT left = ControlBounds(host, panel::kAdaptiveShadowCheckbox);
        const RECT right = ControlBounds(host, panel::kShadowFilterCheckbox);
        Check(left.right <= right.left && left.top == right.top,
              "filter is on the right of dynamic precision without overlap");
        wchar_t caption[64]{}; GetWindowTextW(button, caption, 64);
        Check(std::wcscmp(caption, lang == 2 ? L"滤波加强" : lang == 1 ? L"Smoothing" : L"平滑化") == 0,
              "filter caption follows the UI language");
        HDC dc = GetDC(button);
        HGDIOBJ font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
        SIZE extent{}; GetTextExtentPoint32W(dc, caption, static_cast<int>(std::wcslen(caption)), &extent);
        SelectObject(dc, font); ReleaseDC(button, dc);
        Check(extent.cx + 18 <= right.right - right.left, "filter caption and checkbox fit their control");
    }
    app->EnglishUI() = language; LocalizeUI(app);
    app->AdaptiveShadowResolution() = adaptive; app->ShadowFilterEnhanced() = filter;
    SendMessageW(button, BM_SETCHECK, filter ? BST_CHECKED : BST_UNCHECKED, 0);
    if (existed) { std::ofstream out(preference, std::ios::binary); out << previous; }
    else std::filesystem::remove(preference);
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDPIAware();
    Check(MH_Initialize() == MH_OK, "initialize hidden-window guards");
    Hook(&ShowWindow, &KeepHidden, &originalShow);
    Hook(&MessageBoxA, &UnexpectedA); Hook(&MessageBoxW, &UnexpectedW);
    auto* app = new MMDApp;
    g_Block = app; app->state = MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize production host");
    HWND window = app->MainWindow();
    SetWindowPos(window, nullptr, 0, 0, 1200,820, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HandleWindowSize(app);
    Check(!IsWindowVisible(window), "production host stays hidden");
    if (argc == 2 && (std::string(argv[1]) == "--recent-menu-only" || std::string(argv[1]) == "--accessory-only")) {
        if (std::string(argv[1]) == "--recent-menu-only") CheckRecentProjects(app);
        else CheckAccessoryMenu(app);
        ShutdownCleanup(app); g_Block=nullptr; delete app;
        std::puts("Requested menu regressions passed");
        return 0;
    }
    CheckShadowFilter(app);
    CheckForcePasteControls(app);
    CheckRecentProjects(app);
    CheckAccessoryMenu(app);
    Hook(&BeginPaint,&PreviewBeginPaint,&realBeginPaint);
    Hook(&EndPaint,&PreviewEndPaint,&realEndPaint);
    CheckBottomPanelResize(app);
    Hook(&RedrawWindow, &ObserveRedraw, &originalRedraw);
    auto** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
    Hook(reinterpret_cast<PresentFunction>(vtable[17]), &ObservePresent, &originalPresent);
    const int frame = app->CurrentFrame();
    const int modified = app->SceneModified();
    const int ids[] = {400, 417, 427, 428, 434, 544, 550, 554};
    auto begin = [&] {
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(app->SidebarWidth()+3,100));
        Check(app->SidebarResizeDragging() != 0, "sidebar edge starts resize gesture");
    };
    observe = true;
    for (int target : {600, 310, 800, 250}) {
        const int width = app->SidebarWidth();
        const float ratio = app->SidebarRatio();
        const RECT viewport = app->ViewportRect();
        std::vector<RECT> controls;
        for (int id : ids) controls.push_back(Bounds(GetDlgItem(window,id)));
        redraws = presents = 0;
        begin();
        for (int x : {450,850,270}) {
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x,100));
            Check(app->SidebarWidth() == width && app->SidebarRatio() == ratio &&
                  EqualRect(&viewport,&app->ViewportRect()), "drag leaves sidebar and viewport unchanged");
            bool unchanged = true;
            for (size_t i=0;i<controls.size();++i) {
                const auto rect=Bounds(GetDlgItem(window,ids[i]));
                unchanged &= EqualRect(&rect,&controls[i]) != FALSE;
            }
            Check(unchanged && redraws == 0 && presents == 0, "drag leaves native controls in place without resize repaints");
            Check(app->WindowLayoutReady() != 0, "ordinary painting remains available during drag");
        }
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(target,100));
        Check(app->SidebarWidth() == target, "release uses the final pointer position");
        Check(redraws == 1 && presents == 1, "release performs one complete repaint and scene presentation");
        Check(app->SidebarResizeDragging() == 0 && GetCapture() != window, "release ends resize and mouse capture");
    }
    for (UINT cancel : {WM_CANCELMODE, WM_CAPTURECHANGED}) {
        const int width=app->SidebarWidth();
        begin(); SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(650,100));
        SendMessageW(window,cancel,0,0);
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(650,100));
        Check(app->SidebarWidth()==width && app->SidebarResizeDragging()==0, "cancel or lost capture discards pending width");
    }
    Check(app->CurrentFrame()==frame && app->SceneModified()==modified, "resizing preserves frame and animation data");
    Check(!IsWindowVisible(window), "test never shows the production window");
    observe=false;
    cursor_checks::Run(app);
    ShutdownCleanup(app); g_Block=nullptr; delete app;
    std::puts("Sidebar release-only resize regressions passed");
}
