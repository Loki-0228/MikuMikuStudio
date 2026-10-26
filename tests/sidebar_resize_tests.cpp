// Exercise production mouse messages and repainting without opening a user window.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/frame_sampling.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/force_paste.hpp"
#include "mikumikustudio/recent_projects.hpp"
#include "mikumikustudio/plugin_menu.hpp"
#include "mikumikustudio/plugin_menu.h"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/ui_metrics.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/autosave.hpp"
#include "../src/plugins/scene_store.hpp"
#include "../src/plugins/scene_object_history.hpp"
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/native_accessory_hud_history.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include <thread>
#include <btBulletDynamicsCommon.h>
#include "mikumikustudio/model.hpp"
#include <MinHook.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// A probe may permit one precisely matched real message box while retaining
// the normal fail-fast guard for every other dialog. The scoped probe still
// hides/answers the actual dialog through WH_CBT; this never fabricates a result.
decltype(&MessageBoxW) originalMessageBoxW = nullptr;
bool (*expectedMessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT) = nullptr;
int WINAPI UnexpectedW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    if (expectedMessageBoxW && expectedMessageBoxW(owner, text, title, flags))
        return originalMessageBoxW(owner, text, title, flags);
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
    // Read the readiness before the native call: the synchronous part of
    // RedrawWindow may enter a paint that temporarily owns the layout state.
    const unsigned ready = g_Block ? unsigned(g_Block->WindowLayoutReady()) : 0;
    const BOOL result = originalRedraw(window, rect, region, flags);
    // The shared layout batch invalidates the parent chrome and then calls
    // UpdateWindow; it no longer asks RedrawWindow to paint synchronously.
    // Count that one parent invalidation, verify its flags and exercise the
    // real GDI + D3D paint path explicitly for this hidden host.
    const UINT required = RDW_INVALIDATE | RDW_NOCHILDREN;
    if (observe && window == g_Block->MainWindow() && (flags & required) == required &&
        !(flags & (RDW_UPDATENOW|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN))) {
        ++redraws;
        Check(rect && !region,
              "live resize repaints exposed parent chrome without erasing every sibling");
        Check(ready != 0, "paint occurs only after layout is complete");
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
const RECT* forcedPaintRect=nullptr;
decltype(&BeginPaint) realBeginPaint=nullptr;
decltype(&EndPaint) realEndPaint=nullptr;
HDC WINAPI PreviewBeginPaint(HWND h,LPPAINTSTRUCT ps) {
    if(!previewDC)return realBeginPaint(h,ps);
    *ps={};ps->hdc=previewDC;GetClientRect(h,&ps->rcPaint);
    if(forcedPaintRect)ps->rcPaint=*forcedPaintRect;
    return previewDC;
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
struct GeometryProbe {
    struct Entry { HWND window;unsigned changes=0;RECT last{}; };
    std::vector<Entry> entries;
    static LRESULT CALLBACK Observe(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR ref){
        auto& self=*reinterpret_cast<GeometryProbe*>(ref);
        const auto result=DefSubclassProc(hwnd,msg,wp,lp);
        if(msg==WM_WINDOWPOSCHANGED&&IsWindow(hwnd))for(auto& entry:self.entries)if(entry.window==hwnd){
            RECT current{};GetWindowRect(hwnd,&current);
            // ComboBox may recursively correct popup request height to its
            // collapsed height. Count visible geometry after native processing.
            if(!EqualRect(&entry.last,&current)){++entry.changes;entry.last=current;}break;}
        return result;
    }
    explicit GeometryProbe(HWND host){
        for(HWND child=GetWindow(host,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){RECT current{};GetWindowRect(child,&current);entries.push_back({child,0,current});}
        for(const auto& entry:entries)Check(SetWindowSubclass(entry.window,Observe,reinterpret_cast<UINT_PTR>(this),reinterpret_cast<DWORD_PTR>(this)),"observe native geometry notifications");
    }
    ~GeometryProbe(){for(const auto& entry:entries)if(IsWindow(entry.window))RemoveWindowSubclass(entry.window,Observe,reinterpret_cast<UINT_PTR>(this));}
    unsigned Count(HWND window)const{for(const auto& entry:entries)if(entry.window==window)return entry.changes;return 0;}
    void Reset(){for(auto& entry:entries)entry.changes=0;}
    void Once()const{for(const auto& entry:entries)if(entry.changes>1){std::printf("control=%d changes=%u\n",GetDlgCtrlID(entry.window),entry.changes);Check(false,"each control receives only its final geometry once per layout");}}
};
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
    const int sideBeforeBottom=app->SidebarWidth();const float ratioBeforeBottom=app->SidebarRatio();
    app->SidebarWidth()=601;app->SidebarRatio()=float(double(601)/client.right);RelayoutSidebarControls(app);
    const int y=client.bottom-150;
    auto drag=[&](int id,int delta,UINT ending=WM_LBUTTONUP) {
        int edge=0;
        for(auto p:BottomPanelSpans(app))if(p.id==id)edge=BottomPanelX(app,p.right);
        SendMessageW(host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(edge,y));
        Check(app->bottomPanelSizing.dragging==id,"right border captures the selected panel");
        const int control=id==1?436:id==2?449:id==6?471:509;
        const auto beforeControl=ControlBounds(host,control);
        const int beforeExtra=app->bottomPanelSizing.extra[id];
        GeometryProbe geometry(host);
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge+delta,y));
        geometry.Once();
        for(int unrelated:{400,427,434,544,550,554})Check(geometry.Count(GetDlgItem(host,unrelated))==0,"bottom drag leaves unrelated sidebar and viewport windows in place");
        geometry.Reset();
        const auto duringControl=ControlBounds(host,control);
        Check(app->SidebarWidth()==601&&!app->bottomPanelSizing.relayout,
              "bottom-only relayout preserves exact sidebar pixels and clears its batching scope");
        Check(!EqualRect(&beforeControl,&duringControl) && beforeExtra!=app->bottomPanelSizing.extra[id],
              "panel controls follow effective pointer movement before release");
        const auto layouts=ui_metrics::counts[ui_metrics::Layout].load();
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge+delta,y));
        Check(ui_metrics::counts[ui_metrics::Layout].load()==layouts,"repeated bottom drag position skips layout");
        for(const auto& entry:geometry.entries)Check(entry.changes==0,"duplicate bottom position does not notify native geometry");
        SendMessageW(host,ending,ending==WM_KEYDOWN?VK_ESCAPE:0,MAKELPARAM(edge+delta,y));
        Check(app->bottomPanelSizing.dragging<0 && GetCapture()!=host,"panel drag releases capture");
        if(ending!=WM_LBUTTONUP){const auto restored=ControlBounds(host,control);
            Check(EqualRect(&beforeControl,&restored)&&app->bottomPanelSizing.extra[id]==beforeExtra,
                  "cancel lost capture and Escape restore live panel geometry");}
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
    {
        int edge=0;for(const auto& span:BottomPanelSpans(app))if(span.id==1)edge=BottomPanelX(app,span.right);
        SendMessageW(host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(edge,y));
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge+20,y));
        Check(app->bottomPanelSizing.extra[1]==160,"bottom panel changes during movement");
        SendMessageW(host,WM_LBUTTONUP,0,MAKELPARAM(edge+35,y));
        Check(app->bottomPanelSizing.extra[1]==175,"bottom release uses a newer pointer position than the last move");
        drag(1,-35);
    }

    drag(1,50,WM_CANCELMODE);
    Check(app->bottomPanelSizing.extra[1]==140,"cancel restores previous panel width");
    drag(1,50,WM_CAPTURECHANGED);
    Check(app->bottomPanelSizing.extra[1]==140,"lost capture restores previous panel width");
    drag(1,50,WM_KEYDOWN);
    Check(app->bottomPanelSizing.extra[1]==140,"Escape restores previous panel width");
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

    {
        app->bottomPanelSizing.extra[1]=2500;app->bottomPanelSizing.extra[2]=3000;HandleWindowSize(app);
        int edge=0,effective=0;for(const auto& span:BottomPanelSpans(app))if(span.id==1){edge=BottomPanelX(app,span.right);effective=span.extra;}
        Check(effective>40&&effective<2500,"seed a window-constrained requested bottom width");
        SendMessageW(host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(edge,y));
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge-40,y));
        Check(app->bottomPanelSizing.extra[1]==effective-40,"constrained panel starts at its visible width without jumping");
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge,y));
        Check(app->bottomPanelSizing.extra[1]==effective,"shrinking panel can grow back despite sibling requested widths");
        SendMessageW(host,WM_CANCELMODE,0,0);
        Check(app->bottomPanelSizing.extra[1]==2500&&app->bottomPanelSizing.extra[2]==3000,
              "cancel restores constrained requested widths rather than discarding user layout preferences");
    }
    app->bottomPanelSizing={};
    app->SidebarWidth()=sideBeforeBottom;app->SidebarRatio()=ratioBeforeBottom;
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
    const auto savedSelection = app->SelectedAccessorySlot();
    for (auto precision : {TransparentSortPrecision::Off, TransparentSortPrecision::Standard}) {
        app->TransparentMaterialSortPrecision() = precision;
        app->AccessorySlot(17) = first.get();
        app->AccessorySlot(93) = second.get();
        // Reordering preserves the selected slot. Cover a selection that ends
        // up first and one that ends up second, without assuming startup state.
        const auto selected = precision == TransparentSortPrecision::Off ? 17 : 93;
        app->SelectedAccessorySlot() = selected;
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
        Check(second->order == 0 && first->order == 1 && app->SelectedAccessorySlot() == selected,
              "accessory dialog commits order and preserves selected slot with sorting on/off");
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
    app->SelectedAccessorySlot() = savedSelection;
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
            RECT client{}; GetClientRect(host, &client);
            const int shift = panel::TimelinePanelShift(sidebar);
            const RECT hscroll = ControlBounds(host, panel::kTimelineHScroll);
            const RECT vscroll = ControlBounds(host, panel::kTimelineVScroll);
            Check(hscroll.top == client.bottom - 247 - shift && hscroll.bottom == client.bottom - 231 - shift,
                  "timeline scrollbar row follows the wrapped panel bottom");
            Check(vscroll.top == 162 && vscroll.bottom == client.bottom - 248 - shift,
                  "timeline vertical scrollbar ends at the wrapped panel bottom");
            Check(hscroll.top == vscroll.bottom + 1,
                  "wrapped horizontal and vertical scrollbars meet without overlap");
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
    std::string previous;
    const bool existed = system_settings::ReadSection("shadow", previous) == system_settings::Result::Ok;
    const bool adaptive = app->AdaptiveShadowResolution(), filter = app->ShadowFilterEnhanced();
    const float range = app->ShadowDistance();
    const int modified = app->SceneModified(), language = app->EnglishUI();
    HWND host = app->MainWindow();
    HWND dynamic = GetDlgItem(host, panel::kAdaptiveShadowCheckbox);
    HWND button = GetDlgItem(host, panel::kShadowFilterCheckbox);
    Check(dynamic != nullptr && IsWindowEnabled(dynamic), "full build offers dynamic precision");
    Check(button != nullptr && IsWindowEnabled(button), "full build offers the spline filter");
    // The ABI branch stores shadow preferences in the central settings file.
    // A missing dynamic key falls back to the uniform precision.
    Check(system_settings::WriteSection("shadow", "{\"filterEnhanced\":false}") ==
          system_settings::Result::Ok, "seed central shadow preferences for this run");
    app->AdaptiveShadowResolution() = true;
    LoadShadowResolutionPreference(app);
    Check(!app->AdaptiveShadowResolution(), "missing dynamic preference selects the uniform precision");
    Check(system_settings::WriteSection("shadow", "{\"adaptive\":true,\"filterEnhanced\":false}") ==
          system_settings::Result::Ok, "seed central dynamic precision preference");
    LoadShadowResolutionPreference(app);
    Check(app->AdaptiveShadowResolution() && !app->ShadowFilterEnhanced(),
          "central preference restores dynamic precision without enabling the filter");
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
              "filter sits to the right of dynamic precision on the same row");
        wchar_t caption[64]{}, dynamicCaption[64]{};
        GetWindowTextW(button, caption, 64);
        GetWindowTextW(dynamic, dynamicCaption, 64);
        Check(std::wcscmp(caption, lang == 2 ? L"滤波加强" : lang == 1 ? L"Smoothing" : L"平滑化") == 0,
              "filter caption follows the UI language");
        Check(std::wcscmp(dynamicCaption, lang == 2 ? L"动态精度" : lang == 1 ? L"Dynamic" : L"動的精度") == 0,
              "dynamic precision caption follows the UI language");
        HDC dc = GetDC(button);
        HGDIOBJ font = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
        SIZE extent{}; GetTextExtentPoint32W(dc, caption, static_cast<int>(std::wcslen(caption)), &extent);
        SelectObject(dc, font); ReleaseDC(button, dc);
        Check(extent.cx + 18 <= right.right - right.left, "filter caption and checkbox fit their control");
    }
    app->EnglishUI() = language; LocalizeUI(app);
    app->AdaptiveShadowResolution() = adaptive; app->ShadowFilterEnhanced() = filter;
    SendMessageW(button, BM_SETCHECK, filter ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(dynamic, BM_SETCHECK, adaptive ? BST_CHECKED : BST_UNCHECKED, 0);
    if (existed) Check(system_settings::WriteSection("shadow", previous) == system_settings::Result::Ok,
                       "restore prior central shadow preferences");
    else SaveShadowResolutionPreference(app);
}
}
#include "plugins/scheduler_checks.inc"
#include "plugins/scene_host_checks.inc"
#include "plugins/geometry_host_checks.inc"
#include "../src/plugins/project_storage.hpp"
#include "plugins/storage_host_checks.inc"
#include "plugins/slider_paint_trace_host_checks.inc"
#include "plugins/slider_nonoverlap_host_checks.inc"
#if !MMD_ORIGINAL_PROFILE
#include "plugins/evaluation_host_checks.inc"
#include "plugins/morph_sampling_host_checks.inc"
#include "plugins/display_sampling_host_checks.inc"
#include "plugins/bone_definition_host_checks.inc"
#include "plugins/morph_definition_host_checks.inc"
#include "plugins/morph_evaluation_host_checks.inc"
#include "plugins/evaluation_morph_host_checks.inc"
#include "plugins/evaluation_display_host_checks.inc"
#include "plugins/slider_hover_host_checks.inc"
#include "plugins/layout_refresh_host_checks.inc"
#include "plugins/object_history_host_checks.inc"
#include "plugins/import_picker_host_checks.inc"
#include "plugins/edit_history_host_checks.inc"
#include "plugins/import_buffer_failure_host_checks.inc"
#include "plugins/private_import_host_checks.inc"
#include "plugins/native_registration_host_checks.inc"
#include "plugins/global_registration_host_checks.inc"
#include "plugins/camera_interpolation_host_checks.inc"
#endif

int main(int argc, char** argv) {
    const bool schedulerOnly=argc==2 && std::string(argv[1])=="--scheduler-only";
    if(schedulerOnly) SetEnvironmentVariableW(L"MMS_UI_METRICS",L"scheduler-test.metrics.jsonl");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool sliderHoverUnaware=false;
#if !MMD_ORIGINAL_PROFILE
    if(argc==4&&std::string(argv[1])=="--slider-hover-only"){
        wchar_t requested[2]{};
        sliderHoverUnaware=GetEnvironmentVariableW(L"MMS_TEST_SLIDER_UNAWARE",requested,2)==1&&requested[0]==L'1';
    }
#endif
    if(!sliderHoverUnaware)SetProcessDPIAware();
    Check(MH_Initialize() == MH_OK, "initialize hidden-window guards");
    Hook(&ShowWindow, &KeepHidden, &originalShow);
    Hook(&MessageBoxA, &UnexpectedA); Hook(&MessageBoxW, &UnexpectedW, &originalMessageBoxW);
    auto* app = new MMDApp;
    // Match the other hidden-window fixtures: avoid a 661 KB value-initialized
    // temporary occupying the 1 MB test-thread stack during nested PMM loads.
    g_Block = app; std::memset(&app->state, 0, sizeof(app->state)); app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize production host");
    HWND window = app->MainWindow();
    SetWindowPos(window, nullptr, 0, 0, 1200,820, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HandleWindowSize(app);
    Check(!IsWindowVisible(window), "production host stays hidden");
    if (argc == 4 && std::string(argv[1]) == "--slider-paint-only") {
        const bool bound = std::string(argv[3]) == "bound";
        try {
#if !MMD_ORIGINAL_PROFILE
            if (bound) {
                plugins::Shutdown();
                Check(plugins::InitializeHistoryTestRuntime(app), "bind actual common history for slider paint diagnosis");
                Check(plugins::NativeHistory(app)->SetMode(plugins::SharedHistory::RecordingMode::AllOperations) == MMS_OK,
                    "exercise real all-operation slider transactions while tracing");
            }
#endif
            app->CameraMode() = 1;
            PostLanguageSweep2(app); PostViewRefresh(app); HandleWindowSize(app);
            // This diagnostic alone intentionally exposes its disposable host
            // so GetDC snapshots can distinguish real child pixels from DIBs.
            originalShow(window, SW_SHOWNOACTIVATE);
            for (int id : {447,455,456,457,458,459,460}) originalShow(GetDlgItem(window,id), SW_SHOWNOACTIVATE);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            slider_nonoverlap_host_checks::Run(app, std::filesystem::absolute(argv[2]) / "nonoverlap");
            slider_paint_trace_host_checks::Run(app, std::filesystem::absolute(argv[2]), bound);
            originalShow(window, SW_HIDE);
            ShutdownCleanup(app); g_Block = nullptr; delete app; return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL slider paint: %s\n", error.what());
            originalShow(window, SW_HIDE);
            ShutdownCleanup(app); g_Block = nullptr; delete app; return 1;
        }
    }
#if !MMD_ORIGINAL_PROFILE
    if(argc==4&&std::string(argv[1])=="--slider-hover-only") {
        const std::string mode=argv[3];
        try {
            slider_paint_trace::Require(mode=="ordinary"||mode=="bound","hover mode must be ordinary or bound");
            const bool bound=mode=="bound";
            if(bound){plugins::Shutdown();
                Check(plugins::InitializeHistoryTestRuntime(app),"bind actual common history for separate cursor/hover diagnosis");
                Check(plugins::NativeHistory(app)->SetMode(plugins::SharedHistory::RecordingMode::AllOperations)==MMS_OK,"exercise actual slider history transactions");}
            app->CameraMode()=1;PostLanguageSweep2(app);PostViewRefresh(app);HandleWindowSize(app);
            const auto previousForeground=GetForegroundWindow();
            originalShow(window,SW_SHOW);
            const BOOL foregroundRequested=SetForegroundWindow(window);
            const auto foregroundAfterRequest=GetForegroundWindow();
            for(int id:{447,455,456,457,458,459,460})originalShow(GetDlgItem(window,id),SW_SHOWNOACTIVATE);
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
            slider_hover_host_checks::Run(app,std::filesystem::absolute(argv[2]),bound,previousForeground,foregroundRequested,foregroundAfterRequest);
            originalShow(window,SW_HIDE);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
        }catch(const std::exception& error){std::fprintf(stderr,"FAIL slider cursor/hover: %s\n",error.what());
            originalShow(window,SW_HIDE);ShutdownCleanup(app);g_Block=nullptr;delete app;return 1;}
    }
    if(argc==3 && std::string(argv[1])=="--layout-integration-only") {
        layout_refresh_host_checks::Run(app,std::string(argv[2])=="bound");ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--camera-interpolation-only") {
        camera_interpolation_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--global-registration-only") {
        global_registration_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--native-registration-only") {
        native_registration_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--private-import-only") {
        private_import_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--edit-history-only") {
        edit_history_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--curve-motion-only") {
        edit_history_host_checks::Run(app,true);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--object-history-only") {
        object_history_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
#endif
    if(argc==2 && std::string(argv[1])=="--storage-only") {
        storage_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--geometry-only") {
        scene_geometry_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--scene-only") {
        scene_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
#if !MMD_ORIGINAL_PROFILE
    if(argc==2 && std::string(argv[1])=="--evaluation-only") {
        evaluation_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--morph-sampling-only") {
        morph_sampling_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--bone-definition-only") {
        bone_definition_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--morph-definition-only") {
        morph_definition_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--morph-evaluation-only") {
        morph_evaluation_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--display-sampling-only") {
        display_sampling_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--evaluation-morph-only") {
        evaluation_morph_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--evaluation-display-ik-only") {
        evaluation_display_host_checks::Run(app, true);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--evaluation-display-only") {
        evaluation_display_host_checks::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
#endif
    if(schedulerOnly) {
        Hook(&BeginPaint,&PreviewBeginPaint,&realBeginPaint);
        Hook(&EndPaint,&PreviewEndPaint,&realEndPaint);
        scheduler_checks::Run(app);
        ShutdownCleanup(app);g_Block=nullptr;delete app;
        std::puts("Production scheduler checks passed");return 0;
    }
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
    {
        RECT band{};GetClientRect(window,&band);band.top=std::max(0L,band.bottom-160);
        const auto paint=[&]{RedrawWindow(window,&band,nullptr,RDW_INVALIDATE|RDW_NOCHILDREN);SendMessageW(window,WM_PAINT,0,0);};
        for(int i=0;i<4;++i)paint();
        const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        Check(before>0,"read process GDI resources for chrome buffer lifetime checks");
        for(int i=0;i<32;++i)paint();
        const DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        Check(after<=before+2,"repeated buffered bottom paints release their GDI resources");
    }
    Hook(&IsWindowVisible,&scheduler_checks::Visible,&scheduler_checks::realVisible);
    scheduler_checks::available=true;app->FpsLimit()=1000;
    Check(updates::Initialize(app),"initialize coordinator for resize regression");
    scheduler_checks::Tick(app);
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
        redraws = presents = 0;
        begin();
        for (int x : {450,850,270}) {
            const RECT previous=app->ViewportRect();
            std::vector<RECT> controls;for(int id:ids)controls.push_back(Bounds(GetDlgItem(window,id)));
            const auto beforeRedraw=redraws,beforePresent=presents;
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x,100));
            Check(app->SidebarWidth()==x&&!EqualRect(&previous,&app->ViewportRect()),
                  "sidebar and viewport geometry follow each live pointer position");
            bool moved=false;for(size_t i=0;i<controls.size();++i){const auto current=Bounds(GetDlgItem(window,ids[i]));moved|=!EqualRect(&current,&controls[i]);}
            Check(moved&&redraws==beforeRedraw+1&&presents==beforePresent,
                  "live controls move in one repaint without recursively presenting the scene");
            Check(app->WindowLayoutReady()!=0,"live painting resumes after the complete layout batch");
            SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,100));
            Check(redraws==beforeRedraw+1,"duplicate sidebar position avoids another repaint");
            scheduler_checks::Tick(app);
            Check(presents==beforePresent+1,"safe coordinator boundary presents each resized viewport");
        }
        const auto beforePresent=presents;
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(target,100));
        Check(presents==beforePresent,"release queues presentation without entering the frame driver");
        scheduler_checks::Tick(app);
        Check(app->SidebarWidth()==target&&redraws==4&&presents==4,"release uses the final pointer position and one final repaint");
        Check(app->SidebarResizeDragging()==0&&GetCapture()!=window&&!app->sidebarSizing.active,
              "release ends resize and mouse capture without reverting the committed width");
    }
    for (UINT cancel : {WM_CANCELMODE, WM_CAPTURECHANGED, WM_KEYDOWN}) {
        const int width=app->SidebarWidth();const float ratio=app->SidebarRatio();const auto viewport=app->ViewportRect();
        begin();SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(650,100));
        Check(app->SidebarWidth()==650,"cancel case first publishes live geometry");
        SendMessageW(window,cancel,cancel==WM_KEYDOWN?VK_ESCAPE:0,0);
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(650,100));
        Check(app->SidebarWidth()==width&&app->SidebarRatio()==ratio&&EqualRect(&viewport,&app->ViewportRect())&&
              !app->SidebarResizeDragging()&&!app->sidebarSizing.active&&GetCapture()!=window,
              "cancel lost capture and Escape restore exact starting width ratio and viewport");
    }

    const int beforeClampWidth=app->SidebarWidth();const float beforeClampRatio=app->SidebarRatio();
    for(int pointer:{-100,30000}){
        RECT client{};GetClientRect(window,&client);begin();
        SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(pointer,100));
        const int expected=pointer<0?250:int(client.right)-5;
        Check(app->SidebarWidth()==expected,"live sidebar clamps movement beyond either window edge");
        const auto painted=redraws;SendMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(pointer,100));
        Check(redraws==painted,"repeated clamped sidebar position does not repaint");
        SendMessageW(window,WM_LBUTTONUP,0,MAKELPARAM(pointer,100));
        Check(app->SidebarWidth()==expected&&!app->SidebarResizeDragging(),"same-position boundary release retains live width");
    }
    app->SidebarWidth()=beforeClampWidth;app->SidebarRatio()=beforeClampRatio;HandleWindowSize(app);
    Check(app->CurrentFrame()==frame && app->SceneModified()==modified, "resizing preserves frame and animation data");
    Check(!scheduler_checks::realVisible(window), "test never shows the production window");
    observe=false;
    cursor_checks::Run(app);
    ShutdownCleanup(app); g_Block=nullptr; delete app;
    std::puts("Sidebar live resize regressions passed");
}
