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
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/bottom_panel_resize.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "mikumikustudio/ui_scale.hpp"
#include "ui_scale_popup_fixture.hpp"
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
#include "../src/window/bottom_panel_control_paint.hpp"
#include "../src/window/light_slider_paint_buffer.hpp"
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
#include <initializer_list>
#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace mikumikustudio;
namespace mikumikustudio {
int ViewportToolAtPointForTest(MMDApp*);
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
decltype(&InvalidateRect) realInvalidate = nullptr;
HWND watchedSlider = nullptr;
unsigned wholeSliderInvalidations = 0;
BOOL WINAPI ObserveSliderInvalidation(HWND window, const RECT* area, BOOL erase) {
    if (window == watchedSlider && !area) ++wholeSliderInvalidations;
    return realInvalidate(window, area, erase);
}
#if !MMD_ORIGINAL_PROFILE
struct TrackbarFallbackProbe {
    HWND slider;
    WNDPROC native;
    unsigned paints = 0;
    WPARAM paintArgument = 0;
    unsigned erases = 0;
    WPARAM eraseDc = 0;
    unsigned prints = 0;
    WPARAM printDc = 0;
    LPARAM printFlags = 0;
    unsigned keys = 0;
};

TrackbarFallbackProbe* trackbarFallbackProbe = nullptr;

LRESULT CALLBACK ObserveNativeTrackbar(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto& probe = *trackbarFallbackProbe;
    if (window == probe.slider) {
        switch (message) {
        case WM_PAINT: ++probe.paints; probe.paintArgument = wp; return 137;
        case WM_ERASEBKGND: ++probe.erases; probe.eraseDc = wp; return 140;
        case WM_PRINTCLIENT: ++probe.prints; probe.printDc = wp; probe.printFlags = lp; return 138;
        case WM_KEYDOWN: ++probe.keys; return 139;
        }
    }
    return CallWindowProcA(probe.native, window, message, wp, lp);
}

LRESULT CALLBACK ReportEmptyTrackbarGeometry(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == TBM_GETTHUMBRECT) {
        SetRectEmpty(reinterpret_cast<RECT*>(lp));
        return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
}

void CheckBottomTrackbarFallback(MMDApp* app, HWND slider) {
    TrackbarFallbackProbe probe{slider, app->OriginalTrackbarProc()};
    Check(probe.native && SetWindowSubclass(slider, ReportEmptyTrackbarGeometry, 1, 0),
        "simulate a native trackbar before its thumb geometry is ready");
    HDC dc = CreateCompatibleDC(nullptr);
    Check(dc != nullptr, "create fallback test DC");
    trackbarFallbackProbe = &probe;
    app->OriginalTrackbarProc() = ObserveNativeTrackbar;
    const WPARAM suppliedDc = reinterpret_cast<WPARAM>(dc);
    const LRESULT erase = SendMessageW(slider, WM_ERASEBKGND, suppliedDc, 0);
    previewDC = dc;
    const LRESULT paint = SendMessageW(slider, WM_PAINT, 0, 0);
    previewDC = nullptr;
    const LPARAM flags = PRF_CLIENT | PRF_ERASEBKGND;
    const LRESULT print = SendMessageW(slider, WM_PRINTCLIENT, suppliedDc, flags);
    const LRESULT key = SendMessageW(slider, WM_KEYDOWN, VK_RIGHT, 0);
    app->OriginalTrackbarProc() = probe.native;
    trackbarFallbackProbe = nullptr;
    RemoveWindowSubclass(slider, ReportEmptyTrackbarGeometry, 1);
    DeleteDC(dc);
    Check(erase == 140 && probe.erases == 1 && probe.eraseDc == suppliedDc,
        "empty geometry leaves background erasure to the same native paint path");
    Check(paint == 137 && probe.paints == 1 && probe.paintArgument == 0,
        "empty geometry forwards the original WM_PAINT before the template validates it");
    Check(print == 138 && probe.prints == 1 && probe.printDc == suppliedDc && probe.printFlags == flags,
        "empty geometry preserves the original print destination, flags and result");
    Check(key == 139 && probe.keys == 1, "the trackbar template forwards input to native code exactly once");
}
#endif
void SaveBottomPreview(MMDApp* app,const char* filename,bool wholeBand=false) {
    HWND host=app->MainWindow();RECT client{};GetClientRect(host,&client);
    const int width=client.right,top=wholeBand?MainPanelLayout(app,client.bottom).bottomTop:client.bottom-160;
    const int height=client.bottom-top;
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
        if(rect.top<(wholeBand?top:client.bottom-157) || rect.top>=client.bottom)continue;
        bool show=(id>=430&&id<=445)||id==530||id==32901||
                  (id>=402&&id<=414)||id==531||id==534||id==535;
        if(camera)show|=(id>=446&&id<=452)||id==453||id==469||id==566||
                        (id>=471&&id<=488)||id==32900;
        else show|=(id>=490&&id<=501)||id==502||(id>=504&&id<=528)||
                   id==32902||id==32904||id==32905||id==32906;
        if(!(wholeBand?(GetWindowLongW(child,GWL_STYLE)&WS_VISIBLE)!=0:show))continue;
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
#if !MMD_ORIGINAL_PROFILE
void CheckScaledViewportToolbar(MMDApp* app) {
    const HWND window = app->MainWindow();
    const auto panels = FramePanelLayout(app);
    Check(FrameRectWithin(app->ViewportRect(), panels.overlay), "scene stays inside the scaled presentation bounds");
    for (int id : {536, 544, 545, 546, 547, 548, 549, 551, 552, 553, 554, 555, 556, 557}) {
        const auto bounds = ControlBounds(window, id);
        Check(FrameRectWithin(bounds, id >= 551 ? panels.topBand : panels.bottomBand),
              "native viewport toolbar controls fit their painted strip");
    }
    const auto active = app->ViewportInputActive();
    const int mouseX = app->MouseX(), mouseY = app->MouseY();
    const int iconLeft = panels.TopX(58), iconRight = panels.TopX(9);
    const int iconTop = panels.TopY(-24);
    const auto dc = CreateCompatibleDC(nullptr);
    const auto bitmap = CreateBitmap(panels.overlay.right, panels.overlay.top, 1, 32, nullptr);
    Check(dc && bitmap, "allocate the production viewport toolbar paint capture");
    const auto previous = SelectObject(dc, bitmap);
    HandlePaletteChanged2(dc);
    bool iconPainted = false;
    for (int y = iconTop; y < panels.overlay.top && !iconPainted; ++y) {
        const auto background = GetPixel(dc, panels.overlay.right - 3, y);
        for (int x = iconLeft; x < iconRight; ++x)
            if (GetPixel(dc, x, y) != background) {
                iconPainted = true;
                break;
            }
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    Check(iconPainted, "the scaled toolbar icon region contains its production GDI paint");
    app->ViewportInputActive() = 1;
    app->MouseY() = panels.TopY(-12);
    app->MouseX() = panels.TopX(48);
    Check(ViewportToolAtPointForTest(app) == 1, "the scaled zoom icon receives its displayed hit");
    app->MouseX() = panels.TopX(18);
    Check(ViewportToolAtPointForTest(app) == 2, "the scaled pan icon receives its displayed hit");
    app->MouseY() = panels.overlay.top + 1;
    Check(ViewportToolAtPointForTest(app) == 0, "the viewport immediately below the toolbar does not hit the pan icon");
    app->ViewportInputActive() = active;
    app->MouseX() = mouseX;
    app->MouseY() = mouseY;
}

void CheckScaledTimeline(MMDApp* app, const RECT& client) {
    const HWND window = app->MainWindow();
    const auto geometry = MainPanelLayout(app, client.bottom);
    const auto S = [&](int units) { return geometry.Pixels(units); };
    const auto bar = ControlBounds(window, panel::kTimelineVScroll);
    Check(bar.top == geometry.GridTop() + S(17) && bar.bottom == geometry.timelineBottom,
        "the scaled timeline grid and vertical scrollbar share their layout");

    constexpr int start = 1037, column = 7, target = start + column;
    const int page = app->state.timelineScrollNPage;
    Check(page > column + 1, "the scaled horizontal scrollbar has room for the fixture key");
    Check(app->CameraMode() && app->CameraKeys(), "the timeline fixture has the real camera track");
    auto* keys = app->CameraKeys();
    const auto previousRoot = keys[0];
    const auto previousKey = keys[1];
    const int previousStart = app->TimelineStartFrame(), previousFrame = app->CurrentFrame();
    const auto previousModified = app->SceneModified();
    const HWND frameEdit = GetDlgItem(window, panel::kCurrentFrameEdit);
    wchar_t frameText[32]{};
    GetWindowTextW(frameEdit, frameText, static_cast<int>(std::size(frameText)));
    keys[1] = keys[0];
    keys[1].frame = target;
    keys[1].next = 0;
    keys[1].previous = 0;
    keys[1].selected = 0;
    keys[0].next = 1;
    app->TimelineStartFrame() = start - page;
    const HWND scrollbar = GetDlgItem(window, panel::kTimelineHScroll);
    SendMessageW(window, WM_HSCROLL, SB_PAGEDOWN, reinterpret_cast<LPARAM>(scrollbar));
    Check(app->TimelineStartFrame() == start && app->state.rowHitBand0[column] == 1,
        "paging the native scrollbar puts the distant key in its expected column");

    const auto keyX = [&](int frameColumn) { return S(6) + S(100 + 13 * frameColumn); };
    const int markerY = geometry.GridTop() + S(22);
    const auto dc = CreateCompatibleDC(nullptr);
    const auto bitmap = CreateBitmap(client.right, client.bottom, 1, 32, nullptr);
    Check(dc && bitmap, "allocate the production timeline paint capture");
    const auto previous = SelectObject(dc, bitmap);
    const auto ready = app->WindowLayoutReady();
    app->WindowLayoutReady() = 0;
    previewDC = dc;
    HandleWindowPaint(app);
    previewDC = nullptr;
    app->WindowLayoutReady() = ready;
    const int sourceX = 100 + 13 * column;
    const auto keyPixel = GetPixel(app->PanelDC(), sourceX, 22);
    Check(keyPixel != CLR_INVALID && keyPixel != GetPixel(app->PanelDC(), sourceX + 13, 22) &&
          GetPixel(dc, keyX(column), markerY) == keyPixel,
        "the distant camera key is painted at its scaled mouse target");
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);

    const auto click = [&](int x, int y, UINT message = WM_LBUTTONDOWN) {
        SendMessageW(window, message, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    };
    click(keyX(column), markerY);
    Check(keys[1].selected != 0 && keys[1].frame == target,
        "clicking the painted key on the scaled timeline selects the correct distant frame");
    click(keyX(column - 2), markerY);
    Check(!keys[1].selected, "a neighbouring empty cell does not select the distant key");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(keyX(column - 2), markerY));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(keyX(column + 1), markerY + S(12)));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(keyX(column + 1), markerY + S(12)));
    Check(keys[1].selected != 0 && keys[1].frame == target,
        "the scaled selection rectangle selects the distant key without moving it");

    const int rulerY = geometry.GridTop() + S(7);
    click(keyX(column + 1), rulerY, WM_LBUTTONDBLCLK);
    Check(app->CurrentFrame() == target + 1, "double-clicking the scaled ruler jumps to the correct frame");
    POINT wheel{keyX(column), markerY};
    Check(ClientToScreen(window, &wheel) != FALSE, "convert the timeline wheel point to screen coordinates");
    const float distance = app->CameraDistance();
    app->MouseX() = app->SidebarWidth() + 20;
    SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(wheel.x, wheel.y));
    Check(app->CameraDistance() == distance, "a wheel event over the timeline stays in the sidebar despite a stale pointer");

    keys[0] = previousRoot;
    keys[1] = previousKey;
    app->TimelineStartFrame() = previousStart;
    app->CurrentFrame() = previousFrame;
    SetWindowTextW(frameEdit, frameText);
    app->SceneModified() = previousModified;
    SendMessageW(window, WM_HSCROLL, SB_ENDSCROLL, reinterpret_cast<LPARAM>(scrollbar));
}

void CheckBottomTrackbarPixels(MMDApp* app, bool firstPaint = false,
    std::initializer_list<int> ids = {panel::kFovSlider, panel::kLightColorSliderR,
        panel::kLightColorSliderG, panel::kLightColorSliderB, panel::kLightDirSliderX,
        panel::kLightDirSliderY, panel::kLightDirSliderZ, panel::kSelfShadowRangeSlider,
        panel::kFrameVolumeSlider});

RECT CheckSquareBottomCurve(MMDApp* app) {
    const auto window = app->MainWindow();
    RECT client{};
    GetClientRect(window, &client);
    const RECT curve = BottomCurveRect(app, client.bottom);
    const RECT selector = ControlBounds(window, panel::kInterpCurveCombo);
    Check(curve.right > curve.left && curve.right - curve.left == curve.bottom - curve.top &&
        curve.right <= selector.left && curve.bottom < client.bottom,
        "the displayed interpolation preview is square and clear of its controls");
    const POINT origin = BottomCurveDesignPoint(curve, {curve.left, curve.top});
    const POINT end = BottomCurveDesignPoint(curve, {curve.right - 1, curve.bottom - 1});
    Check(origin.x == 0 && origin.y == 0 && end.x == 127 && end.y == 127,
        "interpolation input reaches both source edges with equal axis scaling");
    return curve;
}

void CheckScalePopupInterruption(MMDApp* app) {
    const HWND owner = app->MainWindow();
    const auto openingBounds = ControlBounds(owner, 451);
    const auto opening = ui_scale::Percent();
    std::string saved;
    const auto savedResult = system_settings::ReadSection("uiScaling", saved);
    for (bool escape : {true, false}) {
        Check(ui_scale_popup_fixture::Choose(owner, 137, false, [&](HWND dialog) {
            const HWND slider = GetDlgItem(dialog, ui_scale::kSlider);
            const auto drain = [&] {
                MSG pending{};
                while (PeekMessageW(&pending, dialog, WM_APP, WM_APP + 0x71, PM_REMOVE))
                    DispatchMessageW(&pending);
            };
            RECT thumb{}, channel{};
            SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            const int x = (thumb.left + thumb.right) / 2, y = (thumb.top + thumb.bottom) / 2;
            Check(channel.right > x + 10, "the thumb has room to move right");
            SendMessageW(slider, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            Check(GetCapture() == slider, "the actual trackbar captures the thumb drag");
            SendMessageW(slider, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(channel.right - 5, y));
            drain();
            Check(ui_scale::Percent() > 137, "the captured drag has changed the live preview");
            if (escape) SetCapture(dialog);
            else SendMessageW(dialog, WM_CANCELMODE, 0, 0);
            drain();
            wchar_t readout[12]{};
            GetDlgItemTextW(dialog, ui_scale::kReadout, readout, 12);
            Check(GetCapture() != slider && ui_scale::Percent() == 137 &&
                  SendMessageW(slider, TBM_GETPOS, 0, 0) == 137 && !std::wcscmp(readout, L"137%"),
                  "interrupted thumb drag restores the last preview and readout");
            if (GetCapture() == dialog) ReleaseCapture();
            SendMessageW(slider, WM_LBUTTONUP, 0, MAKELPARAM(channel.right - 5, y));
            drain();
            Check(ui_scale::Percent() == 137, "a stale release cannot reapply the interrupted drag");
            if (escape) {
                MSG key{};
                key.hwnd = slider; key.message = WM_KEYDOWN; key.wParam = VK_ESCAPE;
                Check(IsDialogMessageW(dialog, &key), "native dialog navigation consumes Escape");
            } else {
                SendMessageW(dialog, WM_CLOSE, 0, 0);
            }
        }, false), "the scale popup previews, then cancels back to its opening scale");
        std::string after;
        const auto bounds = ControlBounds(owner, 451);
        Check(ui_scale::Percent() == opening && EqualRect(&bounds, &openingBounds) &&
              system_settings::ReadSection("uiScaling", after) == savedResult && after == saved,
              "Escape or close restores the layout and leaves persisted scale unchanged");
    }
    int released = 137;
    Check(ui_scale_popup_fixture::Choose(owner, 137, false, [&](HWND dialog) {
        const HWND slider = GetDlgItem(dialog, ui_scale::kSlider);
        RECT thumb{}, channel{};
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        const int y = (thumb.top + thumb.bottom) / 2;
        SendMessageW(slider, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((thumb.left + thumb.right) / 2, y));
        Check(GetCapture() == slider, "the normal drag captures the thumb");
        SendMessageW(slider, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(channel.right - 5, y));
        SendMessageW(slider, WM_LBUTTONUP, 0, MAKELPARAM(channel.right - 5, y));
        released = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        Check(released > 137 && GetCapture() != slider, "normal release keeps the new choice");
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    }, false) && ui_scale::Percent() == released, "confirming after normal release applies the selected scale");
    ui_scale::ApplyAtStartup();
    Check(ui_scale::Percent() == released && ui_scale_popup_fixture::Choose(owner, opening),
          "normal release persists the choice and the popup can return to its opening scale");
    std::puts("PASS interface scale popup drag interruption and cancellation");
}

void CheckDpiScalingMenu(MMDApp* app) {
    const HWND window = app->MainWindow();
    const auto findHelp = [&] {
        const HMENU bar = GetMenu(window);
        for (int index = 0; index < GetMenuItemCount(bar); ++index) {
            const auto help = GetSubMenu(bar, index);
            if (!help || GetMenuState(help, 201, MF_BYCOMMAND) == UINT(-1)) continue;
            if (GetMenuState(help, ui_scale::kMenuCommand, MF_BYCOMMAND) != UINT(-1)) return help;
        }
        return HMENU{};
    };
    const auto savedLanguage = app->EnglishUI();
    for (const int lang : {0, 1, 2}) {
        app->EnglishUI() = lang;
        LocalizeUI(app);
        ui_scale::RefreshMenu(app);
        const auto help = findHelp();
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_SUBMENU;
        wchar_t caption[128]{};
        Check(help && GetMenuItemInfoW(help, ui_scale::kMenuCommand, FALSE, &item) && !item.hSubMenu &&
              GetMenuStringW(help, ui_scale::kMenuCommand, caption, 128, MF_BYCOMMAND) > 0,
            "Help has a direct interface scale command without a submenu in each language");
    }

    const HWND button = GetDlgItem(window, 451);
    const HWND slider = GetDlgItem(window, panel::kFovSlider);
    const HWND frameEdit = GetDlgItem(window, panel::kCurrentFrameEdit);
    const auto originalFrameEdit = ControlBounds(window, panel::kCurrentFrameEdit);
    const auto frameEditFont = SendMessageW(frameEdit, WM_GETFONT, 0, 0);
    const auto originalButton = ControlBounds(window, 451);
    const auto originalSlider = ControlBounds(window, panel::kFovSlider);
    const auto originalViewport = FramePanelLayout(app);
    const auto toolbarButton = GetDlgItem(window, 557);
    const auto toolbarFont = SendMessageW(toolbarButton, WM_GETFONT, 0, 0);
    const auto fontBefore = SendMessageW(button, WM_GETFONT, 0, 0);
    const auto fittedBefore = BottomPanelFontPercent(app);
    const auto sliderPosition = SendMessageW(slider, TBM_GETPOS, 0, 0);
    RECT client{};
    GetClientRect(window, &client);
    const auto originalLayout = MainPanelLayout(app, client.bottom);
    std::string originalSettings;
    const auto originalSettingsResult = system_settings::ReadSection("uiScaling", originalSettings);
    const auto frameBeforePreview = app->CurrentFrame();
    const auto dirtyBeforePreview = app->SceneModified();
    const auto presentsBefore = app->frameRate.presentedFrames;
    Check(ui_scale_popup_fixture::Choose(window, 137, false, [&](HWND dialog) {
        wchar_t readout[12]{};
        GetDlgItemTextW(dialog, ui_scale::kReadout, readout, 12);
        std::string stored;
        Check(!std::wcscmp(readout, L"137%") && ui_scale::Percent() == 137 &&
              MainPanelLayout(app, client.bottom).bottomTop < originalLayout.bottomTop,
              "a non-preset value updates the readout and actual layout during thumb tracking");
        Check(app->frameRate.presentedFrames > presentsBefore && app->CurrentFrame() == frameBeforePreview &&
              app->SceneModified() == dirtyBeforePreview, "preview presents the current scene without moving the playhead or editing it");
        Check(system_settings::ReadSection("uiScaling", stored) == originalSettingsResult && stored == originalSettings,
              "tracking never persists the preview value");
    }), "cancel the popup and restore the opening percentage");
    const auto cancelledButton = ControlBounds(window, 451);
    Check(EqualRect(&originalButton, &cancelledButton), "cancel restores native button geometry");
    Check(ui_scale_popup_fixture::Choose(window, 137), "confirm a non-preset scale in the real popup");
    ui_scale::ApplyAtStartup();
    Check(ui_scale::Percent() == 137, "a confirmed non-preset percentage survives settings reload");
    Check(ui_scale_popup_fixture::Choose(window, 125), "confirm 125 percent through the real popup");
    const auto enlarged = ControlBounds(window, 451);
    const auto enlargedLayout = MainPanelLayout(app, client.bottom);
    Check(ui_scale::Percent() == 125 &&
          enlarged.top < originalButton.top && enlarged.bottom - enlarged.top > originalButton.bottom - originalButton.top &&
          (SendMessageW(button, WM_GETFONT, 0, 0) != fontBefore) == (BottomPanelFontPercent(app) != fittedBefore) &&
          enlargedLayout.bottomTop == originalLayout.bottomTop - 40,
        "the Help command scales the bottom template and fits its font to the available width");
    Check(enlargedLayout.timelinePercent == 125 && SendMessageW(frameEdit, WM_GETFONT, 0, 0) != frameEditFont,
        "the timeline and its frame field use the selected percentage when there is room");
    const auto enlargedViewport = FramePanelLayout(app);
    Check(enlargedViewport.topScale > originalViewport.topScale &&
          enlargedViewport.bottomScale > originalViewport.bottomScale &&
          enlargedViewport.topRowHeight == 31 &&
          SendMessageW(toolbarButton, WM_GETFONT, 0, 0) != toolbarFont,
        "the viewport toolbar and its native font resize on the same menu command");
    CheckScaledViewportToolbar(app);
    ui_scale::ApplyAtStartup();
    Check(ui_scale::Percent() == 125, "the persisted app percentage is read at startup");
    const int rulerX = app->SidebarWidth() / 2;
    const int gripY = enlargedLayout.RulerTop() - 1;
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(rulerX, gripY));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(rulerX, gripY + 16));
    Check(app->bottomPanelSizing.dragging == kTimelineHeightGrip &&
          MainPanelLayout(app, client.bottom).GridTop() == enlargedLayout.GridTop() + 16,
        "the left divider moves the scaled ruler and grid together");
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    Check(MainPanelLayout(app, client.bottom).GridTop() == enlargedLayout.GridTop(),
        "cancel restores the left divider at the scaled layout");
    CheckScaledTimeline(app, client);
    const int x = (app->SidebarWidth() + client.right) / 2;
    const int y = enlargedLayout.bottomTop + 1;
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y - 20));
    Check(app->bottomPanelSizing.dragging == kBottomHeightGrip &&
          MainPanelLayout(app, client.bottom).bottomTop == enlargedLayout.bottomTop - 20,
        "the divider hit-test and drag agree at 125 percent");
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    Check(MainPanelLayout(app, client.bottom).bottomTop == enlargedLayout.bottomTop,
        "Escape restores the scaled template height");
    Check(ui_scale_popup_fixture::Choose(window, 200), "confirm 200 percent through the real popup");
    CheckSquareBottomCurve(app);
    const auto compact = MainPanelLayout(app, client.bottom);
    Check(ui_scale::Percent() == 200 && compact.timelinePercent >= 125 && compact.timelinePercent < 200 &&
          compact.timelineBottom - compact.GridTop() >= compact.Pixels(17 + 4 * 14),
        "200 percent leaves at least four timeline rows in the short editor window");
    CheckScaledTimeline(app, client);
    CheckScaledViewportToolbar(app);
    Check(ui_scale_popup_fixture::Choose(window, 100), "return to 100 percent through the real popup");
    CheckSquareBottomCurve(app);
    CheckBottomTrackbarPixels(app, true);
    const auto restoredButton = ControlBounds(window, 451);
    const auto restoredSlider = ControlBounds(window, panel::kFovSlider);
    const auto restoredFrameEdit = ControlBounds(window, panel::kCurrentFrameEdit);
    Check(ui_scale::Percent() == 100 && EqualRect(&originalButton, &restoredButton) &&
          EqualRect(&originalSlider, &restoredSlider) && SendMessageW(button, WM_GETFONT, 0, 0) == fontBefore &&
          SendMessageW(slider, TBM_GETPOS, 0, 0) == sliderPosition &&
          EqualRect(&originalFrameEdit, &restoredFrameEdit) &&
          SendMessageW(frameEdit, WM_GETFONT, 0, 0) == frameEditFont &&
          SendMessageW(toolbarButton, WM_GETFONT, 0, 0) == toolbarFont,
        "returning to 100 percent restores geometry, fonts and slider value");
    app->EnglishUI() = savedLanguage;
    LocalizeUI(app);
}

void CheckExpandedControlBackground(MMDApp* app) {
    const HWND host = app->MainWindow();
    struct Target { int id; UINT colorMessage; POINT margin; };
    const Target controls[] = {
        {446, WM_CTLCOLORBTN, {0, 0}}, // Camera Perspective
        {451, WM_CTLCOLORBTN, {0, 0}}, // Camera Reset
    };
    for (const auto& target : controls) {
        const HWND child = GetDlgItem(host, target.id);
        RECT bounds{};
        POINT origin{};
        Check(child && GetClientRect(child, &bounds) && !IsRectEmpty(&bounds) &&
              ClientToScreen(child, &origin) && ScreenToClient(host, &origin),
            "locate a native control in the expanded bottom panel");
        const HDC dc = CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = CreateBitmap(bounds.right, bounds.bottom, 1, 32, nullptr);
        Check(dc && bitmap, "allocate a control background sample surface");
        const auto previous = SelectObject(dc, bitmap);
        SetViewportOrgEx(dc, -origin.x, -origin.y, nullptr);
        HandlePaletteChanged(dc);
        SetViewportOrgEx(dc, 0, 0, nullptr);
        const auto expected = GetPixel(dc, target.margin.x, target.margin.y);
        const COLORREF sentinel = RGB(1, 254, 1);
        const auto brush = CreateSolidBrush(sentinel);
        Check(brush && FillRect(dc, &bounds, brush), "seed the independent control surface");
        const auto returned = reinterpret_cast<HBRUSH>(SendMessageW(host, target.colorMessage,
            reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(child)));
        if (returned) FillRect(dc, &bounds, returned);
        const auto childColor = GetPixel(dc, target.margin.x, target.margin.y);
        Check(FillRect(dc, &bounds, brush), "seed the native WM_PRINT surface");
        DeleteObject(brush);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc),
            PRF_CLIENT | PRF_ERASEBKGND | PRF_NONCLIENT);
        const auto nativePaint = GetPixel(dc, target.margin.x, target.margin.y);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (childColor != expected)
            std::printf("DIAG bottom control=%d gradient=%06lx native=%06lx\n", target.id, expected, childColor);
        Check(expected != CLR_INVALID && childColor == expected,
            "native button and slider backgrounds use the exact parent gradient after resizing");
        if (nativePaint != expected)
            std::printf("DIAG bottom control=%d gradient=%06lx painted=%06lx\n", target.id, expected, nativePaint);
        Check(nativePaint == expected, "the native control paints its margin over the current panel gradient");
    }
}

void CheckBottomTrackbarPixels(MMDApp* app, bool firstPaint, std::initializer_list<int> ids) {
    const HWND host = app->MainWindow();
    for (const int id : ids) {
        const HWND slider = GetDlgItem(host, id);
        RECT client{}, thumb{}, channel{};
        POINT origin{};
        Check(slider && GetClientRect(slider, &client) && !IsRectEmpty(&client) &&
              ClientToScreen(slider, &origin) && ScreenToClient(host, &origin),
            "get the actual trackbar rectangle in the resized bottom template");
        const HDC dc = CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = CreateBitmap(client.right, client.bottom, 1, 32, nullptr);
        Check(dc && bitmap, "allocate a native trackbar pixel surface");
        const auto saved = SelectObject(dc, bitmap);
        SetViewportOrgEx(dc, -origin.x, -origin.y, nullptr);
        HandlePaletteChanged(dc);
        SetViewportOrgEx(dc, 0, 0, nullptr);
        const auto capture = [&] {
            std::vector<COLORREF> pixels;
            for (int y = 0; y < client.bottom; ++y)
                for (int x = 0; x < client.right; ++x)
                    pixels.push_back(GetPixel(dc, x, y));
            return pixels;
        };
        const auto background = capture();
        if (firstPaint) {
            previewDC = dc;
            SendMessageW(slider, WM_PAINT, 0, 0);
            previewDC = nullptr;
        } else {
            SendMessageW(slider, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        }
        const auto printed = capture();
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        if (GetWindowLongPtrW(slider, GWL_STYLE) & TBS_VERT)
            channel = {channel.top, channel.left, channel.bottom, channel.right};
        const auto changed = [&](RECT region, bool excludeThumb) {
            IntersectRect(&region, &region, &client);
            unsigned count = 0;
            for (int y = region.top; y < region.bottom; ++y)
                for (int x = region.left; x < region.right; ++x) {
                    if (excludeThumb && PtInRect(&thumb, {x, y})) continue;
                    const auto index = y * client.right + x;
                    count += background[index] != printed[index];
                }
            return count;
        };
        const auto thumbPixels = changed(thumb, false);
        const auto trackPixels = changed(channel, true);
        const auto margin = 2 * client.right + 2;
        SelectObject(dc, saved);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (thumbPixels < 8 || trackPixels < 8)
            std::printf("DIAG native trackbar id=%d thumb=%u track=%u\n", id, thumbPixels, trackPixels);
        Check(thumbPixels >= 8 && trackPixels >= 8,
            "native trackbar draws a visible thumb and channel over the resized panel gradient");
        Check(printed[margin] == background[margin],
            "the native trackbar paints its margin with the template gradient");
    }
}

#if !MMD_ORIGINAL_PROFILE
constexpr UINT kTrackbarChainProbe = WM_APP + 0x155;

LRESULT CALLBACK ExistingTrackbarSubclass(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    if (message == kTrackbarChainProbe) {
        ++*reinterpret_cast<unsigned*>(data);
        return 177;
    }
    return DefSubclassProc(window, message, wp, lp);
}

unsigned layoutBufferAttempts = 0;

HDC WINAPI RefuseTrackbarBuffer(HDC) {
    ++layoutBufferAttempts;
    return nullptr;
}

int WINAPI TrackbarFixtureUpdateRegion(HWND window, HRGN region, BOOL) {
    RECT client{};
    if (!GetClientRect(window, &client) || IsRectEmpty(&client) ||
        !SetRectRgn(region, client.left, client.top, client.right, client.bottom))
        return ERROR;
    return SIMPLEREGION;
}

void CheckNewTrackbarLayoutPaint(MMDApp* app, HWND slider) {
    namespace buffer = ui::light_slider_paint_buffer;
    const HWND main = app->MainWindow();
    const auto priorDragging = app->SidebarResizeDragging();
    buffer::GdiFunctions gdi{};
    // The host stays hidden, so Windows may leave the real update region empty.
    gdi.getUpdateRgn = TrackbarFixtureUpdateRegion;
    gdi.createCompatibleDC = RefuseTrackbarBuffer;
    buffer::ScopedFunctionsForTesting functions(gdi);

    // A failed allocation exercises the native fallback after admission,
    // without depending on the contents of the hidden window's display DC.
    SendMessageW(slider, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    SetCapture(main);
    Check(GetCapture() == main &&
        (SendMessageW(slider, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS),
        "prepare a divider paint with mouse focus cues hidden");
    layoutBufferAttempts = 0;
    app->SidebarResizeDragging() = 1;
    Check(InvalidateRect(slider, nullptr, TRUE), "invalidate the newly attached trackbar");
    SendMessageW(slider, WM_PAINT, 0, 0);
    if (ui_scale::Percent() > 100)
        CheckBottomTrackbarPixels(app, true, {GetDlgCtrlID(slider)});
    app->SidebarResizeDragging() = priorDragging;
    ReleaseCapture();
    if (ui_scale::Percent() == 100) {
        Check(layoutBufferAttempts == 1,
            "a new trackbar receives the native divider buffer without a hard-coded ID");
    } else {
        Check(layoutBufferAttempts == 0,
            "the scaled template paints the new trackbar directly during divider drag");
    }
    CheckBottomTrackbarPixels(app, false, {GetDlgCtrlID(slider)});
}

void CheckNewBottomTrackbar(MMDApp* app, HWND existing) {
    const HWND main = app->MainWindow();
    const RECT bounds = ControlBounds(main, GetDlgCtrlID(existing));
    Check(!ui::AttachBottomTrackbar(*app, GetDlgItem(main, panel::kCurrentFrameEdit)),
        "the trackbar template declines a different native control class");
    constexpr int id = 64001;
    Check(!GetDlgItem(main, id), "pick a slider ID absent from the control catalog");
    enum class Variant { Stock, Unicode, AlreadySubclassed };
    for (const auto variant : {Variant::Stock, Variant::Unicode, Variant::AlreadySubclassed}) {
        const auto menu = reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
        const auto instance = GetModuleHandleW(nullptr);
        const HWND slider = variant == Variant::Unicode
            ? CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_NOTICKS,
                bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                main, menu, instance, nullptr)
            : CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | TBS_NOTICKS,
                bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                main, menu, instance, nullptr);
        Check(slider != nullptr, "create a native trackbar with the new ID");
        unsigned forwarded = 0;
        if (variant == Variant::AlreadySubclassed)
            Check(SetWindowSubclass(slider, ExistingTrackbarSubclass, 1, reinterpret_cast<DWORD_PTR>(&forwarded)),
                "attach another subclass before the template");
        const WNDPROC original = app->OriginalTrackbarProc();
        const auto before = GetWindowLongPtrW(slider, GWLP_WNDPROC);
        InstallControlSubclass(app, slider, id);
        const auto installed = GetWindowLongPtrW(slider, GWLP_WNDPROC);
        if (variant != Variant::AlreadySubclassed)
            Check(installed && installed != before, "the template attaches without an ID list entry");
        Check(ui::AttachBottomTrackbar(*app, slider),
            "the direct template entry point reports an already attached trackbar as ready");
        Check(installed == GetWindowLongPtrW(slider, GWLP_WNDPROC) && app->OriginalTrackbarProc() == original,
            "reinstalling preserves the existing forwarding chain");
        if (variant == Variant::AlreadySubclassed)
            Check(SendMessageW(slider, kTrackbarChainProbe, 0, 0) == 177 && forwarded == 1,
                "the template forwards input to the existing subclass exactly once");
        SendMessageW(slider, TBM_SETPOS, FALSE, 57);
        Check(SendMessageW(slider, TBM_GETPOS, 0, 0) == 57, "native trackbar state remains available");
        CheckBottomTrackbarPixels(app, true, {id});
        CheckNewTrackbarLayoutPaint(app, slider);
        if (variant == Variant::AlreadySubclassed) {
            Check(RemoveWindowSubclass(slider, ExistingTrackbarSubclass, 1),
                "the existing subclass can detach independently of the template");
            CheckBottomTrackbarPixels(app, false, {id});
        }
        DestroyWindow(slider);
    }
    const HWND panel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 200, 40,
        main, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(panel != nullptr, "create a separate native child panel");
    const HWND other = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD, 0, 0, 150, 24,
        panel, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(other != nullptr, "create a trackbar owned by another panel");
    const auto native = GetWindowLongPtrW(other, GWLP_WNDPROC);
    Check(!ui::AttachBottomTrackbar(*app, other) && GetWindowLongPtrW(other, GWLP_WNDPROC) == native,
        "a trackbar outside the main panel keeps its native window procedure");
    DestroyWindow(panel);
}
#endif

void CheckFrameOperationTitle(MMDApp* app) {
    const int width = app->SidebarWidth();
    const RECT title{0, 51, width, 66};
    const HDC dc = CreateCompatibleDC(nullptr);
    const HBITMAP bitmap = CreateBitmap(width, title.bottom - title.top, 1, 32, nullptr);
    Check(dc && bitmap, "allocate offscreen frame operation title surface");
    const HGDIOBJ previous = SelectObject(dc, bitmap);
    SetViewportOrgEx(dc, 0, -title.top, nullptr);
    const auto ready = app->WindowLayoutReady();
    app->WindowLayoutReady() = 0;
    forcedPaintRect = &title;
    previewDC = dc;
    HandleWindowPaint(app);
    previewDC = nullptr;
    forcedPaintRect = nullptr;
    app->WindowLayoutReady() = ready;

    int left = width, right = 0;
    for (int y = title.top; y < title.bottom; ++y) {
        const COLORREF background = GetPixel(dc, 8, y);
        for (int x = 12; x < width - 12; ++x) {
            if (GetPixel(dc, x, y) == background) continue;
            left = std::min(left, x);
            right = std::max(right, x);
        }
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    Check(left < right && std::abs(left + right - width) <= 5,
          "painted frame operation title is centered in the left panel");
}

void CheckVerticalPanelResize(MMDApp* app) {
    const HWND host = app->MainWindow();
    RECT client{};
    GetClientRect(host, &client);
    const auto layout = [&] { return MainPanelLayout(app, client.bottom); };
    const auto original = layout();
    const auto scrollBefore = ControlBounds(host, panel::kTimelineVScroll);
    const auto horizontalBefore = ControlBounds(host, panel::kTimelineHScroll);
    const auto stepBefore = ControlBounds(host, panel::kCurrentFrameEdit);
    const auto firstLight = ControlBounds(host, panel::kLightColorSliderR);
    const auto lastLight = ControlBounds(host, panel::kLightDirSliderZ);
    const auto volumeBefore = ControlBounds(host, panel::kFrameVolumeSlider);
    const RECT curveBefore = CheckSquareBottomCurve(app);
    const auto viewBefore = app->ViewportRect();
    const int sidebar = app->SidebarWidth();
    const float sidebarRatio = app->SidebarRatio();
    Check(original.heightBudget >= 84, "window has room for both vertical divider gestures");

    const auto language = app->EnglishUI();
    for (const int lang : {0, 1, 2}) {
        app->EnglishUI() = lang;
        LocalizeUI(app);
        CheckFrameOperationTitle(app);
        for (const int id : {panel::kUndoButton, panel::kRedoButton}) {
            const HWND button = GetDlgItem(host, id);
            const RECT bounds = ControlBounds(host, id);
            wchar_t caption[64]{};
            const int length = GetWindowTextW(button, caption, 64);
            const HDC dc = GetDC(button);
            Check(dc && length > 0, "measure a localized history button");
            const auto font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
            const HGDIOBJ previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
            SIZE text{};
            const BOOL measured = GetTextExtentPoint32W(dc, caption, length, &text);
            SelectObject(dc, previous);
            ReleaseDC(button, dc);
            Check(measured && text.cx + 8 <= bounds.right - bounds.left &&
                  text.cy + 4 <= bounds.bottom - bounds.top &&
                  bounds.left >= 3 && bounds.right <= sidebar - 3 && bounds.bottom <= 47,
                  "undo and redo captions fit inside their buttons and the history band");
        }
    }
    app->EnglishUI() = language;
    LocalizeUI(app);

    const auto begin = [&](int grip, int x, int y) {
        Check(HitBottomPanelEdge(app, x, y) == grip, "divider hits at its displayed edge");
        SendMessageW(host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        Check(app->bottomPanelSizing.dragging == grip && GetCapture() == host,
              "divider press takes capture");
    };
    const auto move = [&](int x, int y) {
        SendMessageW(host, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
    };
    const auto release = [&](int x, int y) {
        SendMessageW(host, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        Check(app->bottomPanelSizing.dragging < 0 && GetCapture() != host,
              "divider release retires the gesture");
    };

    const int rulerX = sidebar / 2;
    begin(kTimelineHeightGrip, rulerX, original.RulerTop() - 1);
    move(rulerX + 10, original.RulerTop() + 23);
    Check(layout().RulerTop() == original.RulerTop() + 24 &&
          ControlBounds(host, panel::kTimelineVScroll).top == scrollBefore.top + 24 &&
          ControlBounds(host, panel::kCurrentFrameEdit).top == stepBefore.top + 12 &&
          EqualRect(&viewBefore, &app->ViewportRect()),
          "left divider moves the ruler, step row and vertical scrollbar");
    release(rulerX, original.RulerTop() + 35);
    Check(layout().RulerTop() == original.RulerTop() + 36,
          "left divider uses the release position");
    Check(TimelineRulerDamage(app).top == layout().RulerTop(),
          "timeline redraw follows the moved ruler");

    const int bottomX = (sidebar + client.right) / 2;
    const int bottomY = original.bottomTop + 1;
    begin(kBottomHeightGrip, bottomX, bottomY);
    move(bottomX, bottomY - 24);
    Check(layout().bottomTop == original.bottomTop - 24 &&
          ControlBounds(host, panel::kTimelineHScroll).top == horizontalBefore.top - 24 &&
          app->ViewportRect().bottom < viewBefore.bottom,
          "bottom divider reserves height in the viewport and moves the sidebar footer");
    CheckExpandedControlBackground(app);
    CheckBottomTrackbarPixels(app);
    release(bottomX, bottomY - 48);
    CheckBottomTrackbarPixels(app);
    const int committedBottom = layout().bottomTop;
    Check(committedBottom == original.bottomTop - 48, "bottom divider uses the release position");
    const auto firstExpanded = ControlBounds(host, panel::kLightColorSliderR);
    const auto lastExpanded = ControlBounds(host, panel::kLightDirSliderZ);
    const auto volumeExpanded = ControlBounds(host, panel::kFrameVolumeSlider);
    const auto expandedCurve = CheckSquareBottomCurve(app);
    Check(firstExpanded.top < firstLight.top && lastExpanded.top < lastLight.top &&
          lastExpanded.top - firstExpanded.top > lastLight.top - firstLight.top &&
          volumeExpanded.bottom - volumeExpanded.top > volumeBefore.bottom - volumeBefore.top &&
          expandedCurve.top < curveBefore.top && expandedCurve.bottom - expandedCurve.top >= curveBefore.bottom - curveBefore.top,
        "the expanded template moves the square preview and grows the vertical slider");
    Check(FramePresentationRect(app).bottom <= committedBottom,
          "viewport presentation ends above the expanded bottom panels");
    const auto panel = BottomPanelSpans(app).front();
    Check(HitBottomPanelEdge(app, BottomPanelX(app, panel.right), committedBottom + 10) == panel.id,
          "existing horizontal panel resizing still hits in the expanded band");

    begin(kBottomHeightGrip, bottomX, committedBottom + 1);
    move(bottomX, committedBottom - 10000);
    const auto clamped = layout();
    Check(clamped.timelineBottom - (clamped.GridTop() + 17) >= 4 * 14,
          "maximum panel height leaves four timeline rows");
    SendMessageW(host, WM_KEYDOWN, VK_ESCAPE, 0);
    Check(layout().bottomTop == committedBottom && GetCapture() != host &&
          app->bottomPanelSizing.dragging < 0,
          "Escape restores the committed bottom height and releases capture");
    CheckExpandedControlBackground(app);
    CheckBottomTrackbarPixels(app);

    begin(kBottomHeightGrip, bottomX, committedBottom + 1);
    move(bottomX, bottomY);
    Check(layout().bottomTop == original.bottomTop, "dragging the bottom divider back restores the original height");
    watchedSlider = GetDlgItem(host, panel::kLightColorSliderR);
    wholeSliderInvalidations = 0;
    release(bottomX, bottomY);
    watchedSlider = nullptr;
    Check(wholeSliderInvalidations > 0, "releasing a restored divider invalidates the whole trackbar at rest");
    CheckBottomTrackbarPixels(app, true);
    const RECT restoredCurve = CheckSquareBottomCurve(app);
    Check(EqualRect(&curveBefore, &restoredCurve), "the square interpolation preview returns to its exact original bounds");

    const int committedRuler = layout().RulerTop();
    begin(kTimelineHeightGrip, rulerX, committedRuler - 1);
    move(rulerX, committedRuler - 20);
    Check(layout().RulerTop() < committedRuler, "left divider moves before capture loss");
    ReleaseCapture();
    Check(layout().RulerTop() == committedRuler && GetCapture() != host &&
          app->bottomPanelSizing.dragging < 0 &&
          app->SidebarWidth() == sidebar && app->SidebarRatio() == sidebarRatio,
          "capture loss restores the ruler and preserves the sidebar width");

    begin(kTimelineHeightGrip, rulerX, committedRuler - 1);
    move(rulerX, committedRuler + 8);
    SendMessageW(host, WM_CANCELMODE, 0, 0);
    Check(layout().RulerTop() == committedRuler && GetCapture() != host &&
          app->bottomPanelSizing.dragging < 0,
          "WM_CANCELMODE restores the ruler and releases capture");

    app->bottomPanelSizing.extra[kTimelineHeightGrip] = original.timelineOffset;
    HandleWindowSize(app);
    const auto firstRestored = ControlBounds(host, panel::kLightColorSliderR);
    const auto volumeRestored = ControlBounds(host, panel::kFrameVolumeSlider);
    Check(EqualRect(&firstLight, &firstRestored) && EqualRect(&volumeBefore, &volumeRestored),
        "restoring the default height restores exact template geometry");
}

void CheckResponsiveBottomPanel(MMDApp* app) {
    const HWND host = app->MainWindow();
    const auto checkLightActions = [&] {
        const RECT reset = ControlBounds(host, 467), apply = ControlBounds(host, 468);
        Check(reset.right > reset.left && reset.right < apply.left &&
              reset.right - reset.left == apply.right - apply.left &&
              reset.top == apply.top && reset.bottom == apply.bottom,
              "lighting Reset and Register are exactly the same size on one row");
    };
    app->CameraMode() = 1;
    app->EnglishUI() = kUiChinese;
    LocalizeUI(app);
    for (int id : {454, 489, 469, 566}) SendMessageW(host, WM_COMMAND, id, 0);
    const RECT narrow = ControlBounds(host, 449);
    RECT client{};
    GetClientRect(host, &client);
    const int dragX = client.right / 2;
    const int gripY = MainPanelLayout(app, client.bottom).bottomTop + 1;
    SendMessageW(host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(dragX, gripY));
    Check(app->bottomPanelSizing.dragging == kBottomHeightGrip, "the responsive height test captures the actual divider");
    SendMessageW(host, WM_LBUTTONUP, 0, MAKELPARAM(dragX, gripY - 160));
    const RECT first = ControlBounds(host, 449), second = ControlBounds(host, 450);
    const RECT parent = ControlBounds(host, 474), bone = ControlBounds(host, 475);
    Check(first.right - first.left >= narrow.right - narrow.left + 40 && first.right == second.right &&
          first.bottom <= second.top && ControlBounds(host, 452).top >= second.bottom,
        "dragging taller gives camera follow names full-width rows ahead of Register");
    Check(parent.left == bone.left && parent.right == bone.right && parent.bottom <= bone.top,
        "taller accessory panel gives the two parent names separate full-width rows");
    const HWND combo = GetDlgItem(host, 450);
    const wchar_t* longName = L"Very long follow target name with an imported model and its parent bone";
    const auto row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(longName));
    Check(row >= 0, "add an isolated long follow-name fixture");
    SendMessageW(host, WM_COMMAND, MAKEWPARAM(450, CBN_DROPDOWN), reinterpret_cast<LPARAM>(combo));
    Check(SendMessageW(combo, CB_GETDROPPEDWIDTH, 0, 0) > second.right - second.left + 70,
        "the opened list requests enough width for a name longer than the closed field");
    SendMessageW(combo, CB_DELETESTRING, row, 0);
    SaveBottomPreview(app, "bottom-tall.bmp", true);

    app->bottomPanelSizing.extra[2] = 140;
    HandleWindowSize(app);
    const RECT left = ControlBounds(host, 449), right = ControlBounds(host, 450), apply = ControlBounds(host, 452);
    Check(left.top == right.top && right.top == apply.top && left.right < right.left && right.right < apply.left &&
          left.right - left.left > narrow.right - narrow.left,
        "a camera panel below twice its original width places the two names and Register on one row");

    app->bottomPanelSizing.extra[2] = 0;
    app->bottomPanelSizing.extra[4] = 130;
    for (int id : {488, 470}) SendMessageW(host, WM_COMMAND, id, 0);
    for (int rowIndex = 0; rowIndex < 3; ++rowIndex) {
        const RECT rgb = ControlBounds(host, 455 + rowIndex), xyz = ControlBounds(host, 458 + rowIndex);
        Check(rgb.top == xyz.top && rgb.right < xyz.left, "a wider light panel aligns its RGB and XYZ rows side by side");
        const int id = 458 + rowIndex;
        const HWND slider = GetDlgItem(host, id);
        const auto originalPosition = SendMessageW(slider, TBM_GETPOS, 0, 0);
        SetCapture(slider);
        Check(GetCapture() == slider, "hold a second-column lighting slider during repaint");
        for (const int position : {-65, 45}) {
            SendMessageW(slider, TBM_SETPOS, TRUE, position);
            SendMessageW(host, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, position), reinterpret_cast<LPARAM>(slider));
            Check(SendMessageW(slider, TBM_GETPOS, 0, 0) == position &&
                std::abs(app->LightDirection()[rowIndex] - position / 100.f) < 0.0001f,
                "tracking the second-column slider updates its native value and the light direction");
            CheckBottomTrackbarPixels(app, true, {id});
        }
        ReleaseCapture();
        CheckBottomTrackbarPixels(app, true, {id});
        SendMessageW(slider, TBM_SETPOS, TRUE, originalPosition);
        SendMessageW(host, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), reinterpret_cast<LPARAM>(slider));
    }
    checkLightActions();
    SaveBottomPreview(app, "bottom-wide.bmp", true);

    app->bottomPanelSizing.extra[kBottomHeightGrip] = 0;
    HandleWindowSize(app);
    const int lightGrip = 4;
    const auto spans = BottomPanelSpans(app);
    const int lightEdge = BottomPanelX(app, BottomPanelSpan(spans, lightGrip).right);
    const int lightY = MainPanelLayout(app, client.bottom).bottomTop + 10;
    const RECT xyzWide = ControlBounds(host, panel::kLightDirSliderX);
    Check(HitBottomPanelEdge(app, lightEdge, lightY) == lightGrip, "the actual lighting separator can resize its two columns");
    SendMessageW(host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(lightEdge, lightY));
    SendMessageW(host, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(lightEdge - 65, lightY));
    const RECT xyzNarrow = ControlBounds(host, panel::kLightDirSliderX);
    Check(app->bottomPanelSizing.dragging == lightGrip &&
        xyzNarrow.top > ControlBounds(host, panel::kLightColorSliderR).top,
        "dragging the lighting separator narrower restores one column");
    SendMessageW(host, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(lightEdge, lightY));
    const RECT xyzRestored = ControlBounds(host, panel::kLightDirSliderX);
    Check(EqualRect(&xyzWide, &xyzRestored), "dragging the separator back restores the second column");
    CheckBottomTrackbarPixels(app, true, {458, 459, 460});
    watchedSlider = GetDlgItem(host, panel::kLightDirSliderX);
    wholeSliderInvalidations = 0;
    SendMessageW(host, WM_LBUTTONUP, 0, MAKELPARAM(lightEdge, lightY));
    watchedSlider = nullptr;
    Check(app->bottomPanelSizing.dragging < 0 && wholeSliderInvalidations > 0,
        "the restored second column receives a complete repaint when the divider is released");
    CheckBottomTrackbarPixels(app, true, {458, 459, 460});

    app->bottomPanelSizing.extra[4] = 0;
    for (int id : {489, 567}) SendMessageW(host, WM_COMMAND, id, 0);
    Check(ui_scale_popup_fixture::Choose(host, 200), "open the responsive bottom layout at 200 percent");
    Check(ui_scale::Percent() == 200 && BottomPanelFontPercent(app) < 200,
        "200 percent on a narrow host constrains font growth to available panel width");
    checkLightActions();
    for (int id : {562, 563, 564, panel::kAdaptiveShadowCheckbox, panel::kShadowFilterCheckbox, 565}) {
        const HWND child = GetDlgItem(host, id);
        RECT rect{};
        GetClientRect(child, &rect);
        const HDC dc = GetDC(child);
        const auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0)));
        wchar_t text[64]{};
        const int len = GetWindowTextW(child, text, 64);
        SIZE size{};
        TEXTMETRICW metrics{};
        const bool measured = GetTextExtentPoint32W(dc, text, len, &size) && GetTextMetricsW(dc, &metrics);
        SelectObject(dc, old);
        ReleaseDC(child, dc);
        const int reserved = id == panel::kAdaptiveShadowCheckbox || id == panel::kShadowFilterCheckbox
            ? GetSystemMetrics(SM_CXMENUCHECK) + 4 : 6;
        Check(len > 0 && measured && size.cx + reserved <= rect.right && metrics.tmHeight <= rect.bottom,
            "Chinese shadow captions fit their actual button or checkbox client at 200 percent");
    }
    SaveBottomPreview(app, "bottom-200.bmp", true);
    Check(ui_scale_popup_fixture::Choose(host, 100), "restore the responsive bottom layout at 100 percent");
    checkLightActions();
}

void CheckNarrowBottomPanel(MMDApp* app) {
    const HWND host = app->MainWindow();
    app->CameraMode() = 1;
    for (int option : {1, 2, 3, 6}) app->UiOptionFlag(option) = 1;
    HandleWindowSize(app);
    RECT client{}, outer{};
    Check(GetClientRect(host, &client) && GetWindowRect(host, &outer), "measure the host and native frame");
    constexpr int narrowWidth = 920;
    const int frameWidth = outer.right - outer.left - client.right;
    Check(SetWindowPos(host, nullptr, 0, 0, narrowWidth + frameWidth, outer.bottom - outer.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
          "narrow the actual editor window");
    HandleWindowSize(app);
    Check(GetClientRect(host, &client) && client.right == narrowWidth,
          "test the narrow editor at its actual client width");

    for (int requested : {100, 125, 100}) {
        Check(ui_scale_popup_fixture::Choose(host, requested), "select the application scale in its actual popup");
        const int fitted = BottomPanelFontPercent(app);
        const RECT volume = ControlBounds(host, panel::kFrameVolumeSlider);
        const auto span = BottomPanelSpans(app).back();
        std::printf("NARROW-BOTTOM requested=%d fitted=%d client=%ld volumeRight=%ld bandRight=%d\n",
                    ui_scale::Percent(), fitted, client.right, volume.right, BottomPanelX(app, span.right));
        Check(ui_scale::Percent() == requested && fitted >= kMinimumBottomPanelWidthPercent && fitted < 100 &&
              volume.left < volume.right && volume.right <= client.right && BottomPanelX(app, span.right) <= client.right,
              "the complete bottom band including volume fits the editor at the requested scale");
        CheckSquareBottomCurve(app);
        CheckBottomTrackbarPixels(app, true);

        const HWND combo = GetDlgItem(host, 450);
        const RECT comboBounds = ControlBounds(host, 450);
        const wchar_t* longName = L"Very long follow target name with an imported model and its parent bone";
        const auto row = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(longName));
        Check(row >= 0 && comboBounds.left >= 0 && comboBounds.right <= client.right,
              "the camera follow selector remains inside the narrow client");
        SendMessageW(host, WM_COMMAND, MAKEWPARAM(450, CBN_DROPDOWN), reinterpret_cast<LPARAM>(combo));
        Check(SendMessageW(combo, CB_GETDROPPEDWIDTH, 0, 0) > comboBounds.right - comboBounds.left + 70,
              "the long camera follow name widens its dropdown using the fitted font");
        SendMessageW(combo, CB_DELETESTRING, row, 0);

        const HWND slider = GetDlgItem(host, panel::kLightDirSliderX);
        const auto before = SendMessageW(slider, TBM_GETPOS, 0, 0);
        const int target = before < 0 ? 37 : -37;
        SendMessageW(slider, TBM_SETPOS, TRUE, target);
        SendMessageW(host, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, target), reinterpret_cast<LPARAM>(slider));
        Check(SendMessageW(slider, TBM_GETPOS, 0, 0) == target &&
              std::abs(app->LightDirection()[0] - target / 100.f) < 0.0001f,
              "the fitted lighting slider updates its live scene value");
        CheckBottomTrackbarPixels(app, false, {panel::kLightDirSliderX});
    }
}
#endif

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
        Check(app->SidebarWidth()==601, "panel gesture starts with the exact sidebar width");
        SendMessageW(host,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(edge,y));
        Check(app->bottomPanelSizing.dragging==id,"right border captures the selected panel");
        Check(app->SidebarWidth()==601, "capturing the panel divider preserves sidebar width");
        const int control=id==1?436:id==2?449:id==6?471:509;
        const auto beforeControl=ControlBounds(host,control);
        const int beforeExtra=app->bottomPanelSizing.extra[id];
        GeometryProbe geometry(host);
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge+delta,y));
        geometry.Once();
        for(int unrelated:{400,427,434,544,550,554})Check(geometry.Count(GetDlgItem(host,unrelated))==0,"bottom drag leaves unrelated sidebar and viewport windows in place");
        geometry.Reset();
        const auto duringControl=ControlBounds(host,control);
        Check(app->SidebarWidth()==601, "bottom-only relayout preserves exact sidebar width");
        Check(!app->bottomPanelSizing.relayout, "bottom-only relayout clears its batching scope");
        Check(!EqualRect(&beforeControl,&duringControl) && beforeExtra!=app->bottomPanelSizing.extra[id],
              "panel controls follow effective pointer movement before release");
        const auto layouts=ui_metrics::counts[ui_metrics::Layout].load();
        SendMessageW(host,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(edge+delta,y));
        Check(ui_metrics::counts[ui_metrics::Layout].load()==layouts,"repeated bottom drag position skips layout");
        for(const auto& entry:geometry.entries)Check(entry.changes==0,"duplicate bottom position does not notify native geometry");
        if(ending==WM_CAPTURECHANGED){Check(GetCapture()==host,"capture loss begins from captured host");ReleaseCapture();}
        else SendMessageW(host,ending,ending==WM_KEYDOWN?VK_ESCAPE:0,MAKELPARAM(edge+delta,y));
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
#include "plugins/bone_kinematic_host_checks.inc"
#include "plugins/morph_definition_host_checks.inc"
#include "plugins/morph_evaluation_host_checks.inc"
#include "plugins/bone_evaluation_host_checks.inc"
#include "plugins/mesh_definition_host_checks.inc"
#include "plugins/mesh_material_scene_host_checks.inc"
#include "plugins/bone_scene_evaluation_host_checks.inc"
#include "plugins/evaluation_morph_host_checks.inc"
#include "plugins/evaluation_display_host_checks.inc"
#include "plugins/evaluation_parent_state_host_checks.inc"
#include "plugins/scene_evaluation_abi_host_checks.inc"
#include "plugins/scene_unsupported_abi_host_checks.inc"
#include "plugins/slider_hover_host_checks.inc"
#include "plugins/layout_refresh_host_checks.inc"
#include "plugins/layout_takeover_host_checks.inc"
#include "background_picture_reset_checks.hpp"
#include "native_picture_history_checks.inc"
#include "native_picture_gesture_checks.inc"
#include "plugins/object_history_host_checks.inc"
#include "plugins/import_picker_host_checks.inc"
#include "plugins/edit_history_host_checks.inc"
#include "plugins/import_buffer_failure_host_checks.inc"
#include "plugins/private_import_host_checks.inc"
#include "plugins/native_registration_host_checks.inc"
#include "plugins/global_registration_host_checks.inc"
#include "plugins/camera_interpolation_host_checks.inc"
#endif

#if !MMD_ORIGINAL_PROFILE
#include "bottom_panel_live_drag_checks.inc"
#endif

int main(int argc, char** argv) {
    const bool schedulerOnly=argc==2 && std::string(argv[1])=="--scheduler-only";
    if(schedulerOnly) SetEnvironmentVariableW(L"MMS_UI_METRICS",L"scheduler-test.metrics.jsonl");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool sliderHoverUnaware=false;
#if !MMD_ORIGINAL_PROFILE
    const bool firstTrackbarPaint = argc == 2 && std::string(argv[1]) == "--bottom-trackbar-first-paint";
    const bool liveDividerPaint = argc == 2 && std::string(argv[1]) == "--lighting-divider-live-paint";
    sliderHoverUnaware = firstTrackbarPaint;
    if(argc==4&&std::string(argv[1])=="--slider-hover-only"){
        wchar_t requested[2]{};
        sliderHoverUnaware=GetEnvironmentVariableW(L"MMS_TEST_SLIDER_UNAWARE",requested,2)==1&&requested[0]==L'1';
    }
#endif
    if(!sliderHoverUnaware)SetProcessDPIAware();
    Check(MH_Initialize() == MH_OK, "initialize hidden-window guards");
    Hook(&ShowWindow, &KeepHidden, &originalShow);
    Hook(&MessageBoxA, &UnexpectedA); Hook(&MessageBoxW, &UnexpectedW, &originalMessageBoxW);
#if !MMD_ORIGINAL_PROFILE
    if(argc==3 && std::string(argv[1])=="--layout-takeover-only") layout_takeover_host_checks::InstallInputGuards();
    if (firstTrackbarPaint || liveDividerPaint) ui_scale::ApplyAtStartup();
#endif
    auto* app = new MMDApp;
    // Match the other hidden-window fixtures: avoid a 661 KB value-initialized
    // temporary occupying the 1 MB test-thread stack during nested PMM loads.
    g_Block = app; std::memset(&app->state, 0, sizeof(app->state)); app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize production host");
    HWND window = app->MainWindow();
    SetWindowPos(window, nullptr, 0, 0, 1200,820, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HandleWindowSize(app);
    Check(!IsWindowVisible(window), "production host stays hidden");
#if !MMD_ORIGINAL_PROFILE
    if (argc == 2 && std::string(argv[1]) == "--scale-popup-interruption") {
        CheckScalePopupInterruption(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--narrow-bottom-panel") {
        Hook(&BeginPaint, &PreviewBeginPaint, &realBeginPaint);
        Hook(&EndPaint, &PreviewEndPaint, &realEndPaint);
        CheckNarrowBottomPanel(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        std::puts("PASS narrow bottom panel");
        return 0;
    }
    if (liveDividerPaint) {
        bottom_live_drag::Run(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        return 0;
    }
    if (firstTrackbarPaint) {
        Hook(&BeginPaint, &PreviewBeginPaint, &realBeginPaint);
        Hook(&EndPaint, &PreviewEndPaint, &realEndPaint);
        Check(!GetCapture(), "start the editor before any drag");
        const HWND slider = GetDlgItem(window, panel::kFovSlider);
        Check(slider != nullptr, "get the native camera slider");
        std::printf("DIAG startup interface scale=%d\n", ui_scale::Percent());
        CheckBottomTrackbarPixels(app, true);
        CheckBottomTrackbarPixels(app);
        CheckNewBottomTrackbar(app, slider);
        CheckBottomTrackbarFallback(app, slider);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        std::puts("Bottom trackbar first paint passed");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--vertical-panels-only") {
        Hook(&BeginPaint, &PreviewBeginPaint, &realBeginPaint);
        Hook(&EndPaint, &PreviewEndPaint, &realEndPaint);
        Hook(&InvalidateRect, &ObserveSliderInvalidation, &realInvalidate);
        CheckDpiScalingMenu(app);
        CheckVerticalPanelResize(app);
        CheckResponsiveBottomPanel(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        std::puts("Vertical panel resize regressions passed");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--picture-gesture-only") {
        native_picture_gesture_checks::Run(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--picture-history-only") {
        native_picture_history_checks::Run(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--picture-reset-only") {
        bool closed = false;
        try {
            picture_reset_checks::Run(app);
            ShutdownCleanup(app); closed = true;
            picture_reset_checks::VerifyClosed();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL picture reset: %s\n", error.what());
            if (!closed) ShutdownCleanup(app);
            g_Block = nullptr; delete app; return 1;
        }
        g_Block = nullptr; delete app;
        std::puts("PASS picture asset real project reset, redisplay and shutdown");
        return 0;
    }
#endif
    if (argc == 2 && (std::string(argv[1]) == "--viewport-cursor-only" ||
                      std::string(argv[1]) == "--scaled-viewport-cursor-only")) {
        if (!build_profile::original && std::string(argv[1]) == "--scaled-viewport-cursor-only") {
            Check(ui_scale_popup_fixture::Choose(window, 125), "select 125 percent through the real popup");
            Check(FrameOverlayScale(app) > 1, "the live Help command enlarges the HUD on this host");
        }
        cursor_checks::Run(app);
        ShutdownCleanup(app); g_Block = nullptr; delete app;
        std::puts("Viewport cursor regressions passed");
        return 0;
    }
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
    if(argc==3 && std::string(argv[1])=="--layout-takeover-only") {
        Check(std::string(argv[2])=="ordinary" || std::string(argv[2])=="bound","valid layout takeover history mode");
        layout_takeover_host_checks::Run(app,std::string(argv[2])=="bound");
        ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
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
    if(argc==2 && std::string(argv[1])=="--bone-kinematic-only") {
        bone_kinematic_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--bone-evaluation-only") {
        bone_evaluation_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--mesh-definition-only") {
        mesh_definition_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--mesh-material-scene-only") {
        mesh_material_scene_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--bone-scene-evaluation-only") {
        bone_scene_evaluation_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--scene-evaluation-abi-only") {
        scene_evaluation_abi_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
    }
    if(argc==2 && std::string(argv[1])=="--scene-unsupported-abi-only") {
        scene_unsupported_abi_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
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
    if(argc==2 && std::string(argv[1])=="--evaluation-parent-state-only") {
        evaluation_parent_state_probe::Run(app);ShutdownCleanup(app);g_Block=nullptr;delete app;return 0;
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
        if(cancel==WM_CAPTURECHANGED){Check(GetCapture()==window,"sidebar capture loss begins from captured host");ReleaseCapture();}
        else SendMessageW(window,cancel,cancel==WM_KEYDOWN?VK_ESCAPE:0,0);
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
