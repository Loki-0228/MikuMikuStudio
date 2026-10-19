#pragma once
#include "mikumikustudio/bottom_panel_layout.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <vector>

namespace mikumikustudio {
struct ResizableBottomPanel { int id, left, right, extra; };
inline std::vector<ResizableBottomPanel> BottomPanelSpans(MMDApp* app) {
    const auto p = ComputeBottomPanelLayout(app);
    const bool camera = app->CameraMode() != 0;
    std::vector<ResizableBottomPanel> spans{{0,3,215,0},{1,218,358,0}};
    auto add = [&](int id,int left,int right) { spans.push_back({id,left,right,0}); };
    add(camera ? 2 : 3,361,app->UiOptionFlag(camera ? 1 : 4) ? (camera ? 514 : 558) : 379);
    if(camera) {
        add(4,499-p.leading,(app->UiOptionFlag(2)?679:517)-p.leading);
        add(5,682-p.afterLightOrFace,(app->UiOptionFlag(6)?859:700)-p.afterLightOrFace);
        add(6,862-p.afterSelfShadow,(app->UiOptionFlag(3)?1058:880)-p.afterSelfShadow);
    } else add(7,596-p.leading,(app->UiOptionFlag(5)?865:614)-p.leading);
    add(8,881-p.final,1042-p.final);
    RECT client{}; GetClientRect(app->MainWindow(),&client);
    int available = (std::max)(0,int(client.right)-3-spans.back().right);
    // Keep every panel reachable after shrinking the application window.
    for(auto& s:spans) if(s.right-s.left>20) {
        s.extra=(std::min)(available,app->bottomPanelSizing.extra[s.id]);
        available-=s.extra;
    }
    return spans;
}
inline int BottomPanelX(MMDApp* app,int x) {
    int shift=0;
    for(const auto& p:BottomPanelSpans(app)) {
        if(x<p.left) return x+shift;
        if(x<=p.right) return x+shift+MulDiv(x-p.left,p.extra,p.right-p.left);
        shift+=p.extra;
    }
    return x+shift;
}
inline void ResizeBottomPanelControls(MMDApp* app) {
    HWND host=app->MainWindow(); RECT client{};GetClientRect(host,&client);
    for(HWND child=GetWindow(host,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,host,reinterpret_cast<POINT*>(&r),2);
        if(r.top<client.bottom-157 || r.top>=client.bottom) continue;
        const int left=BottomPanelX(app,r.left),right=BottomPanelX(app,r.right);
        if(left==r.left && right==r.right) continue;
        int height=r.bottom-r.top;
        wchar_t cls[32]{};GetClassNameW(child,cls,32);
        if(_wcsicmp(cls,L"COMBOBOX")==0) {
            RECT drop{};SendMessageW(child,CB_GETDROPPEDCONTROLRECT,0,reinterpret_cast<LPARAM>(&drop));
            height=(std::max)(height,int(drop.bottom-drop.top));
        }
        SetWindowPos(child,nullptr,left,r.top,right-left,height,SWP_NOZORDER|SWP_NOACTIVATE);
    }
}
inline int HitBottomPanelEdge(MMDApp* app,int x,int y) {
    RECT r{};GetClientRect(app->MainWindow(),&r);
    if(app->FullscreenMode() || y<r.bottom-157 || y>=r.bottom-3) return -1;
    for(const auto& p:BottomPanelSpans(app))
        if(p.right-p.left>20 && abs(x-BottomPanelX(app,p.right))<=2) return p.id;
    return -1;
}
inline bool HandleBottomPanelResize(MMDApp* app,UINT msg,WPARAM key,LPARAM pos) {
    auto& s=app->bottomPanelSizing; HWND host=app->MainWindow();
    const int x=static_cast<short>(LOWORD(pos)),y=static_cast<short>(HIWORD(pos));
    auto redraw=[&] { HandleWindowSize(app);RedrawWindow(host,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN); };
    if(msg==WM_LBUTTONDOWN) {
        const int id=HitBottomPanelEdge(app,x,y);if(id<0)return false;
        s.dragging=id;s.startX=x;
        for(const auto& p:BottomPanelSpans(app)) if(p.id==id)s.startExtra=p.extra;
        SetCapture(host);SetCursor(LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_SIZEWE)));return true;
    }
    if(s.dragging>=0 && msg==WM_MOUSEMOVE) {
        // Keep all panel geometry and painting unchanged until release.
        SetCursor(LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_SIZEWE)));return true;
    }
    if(s.dragging>=0 && msg==WM_LBUTTONUP) {
        RECT r{};GetClientRect(host,&r);auto spans=BottomPanelSpans(app);
        int limit=(std::max)(0,int(r.right)-3-spans.back().right);
        for(const auto& p:spans)if(p.id!=s.dragging)limit-=p.extra;
        s.extra[s.dragging]=(std::clamp)(s.startExtra+x-s.startX,0,(std::max)(0,limit));
        redraw();SetCursor(LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_SIZEWE)));
        if(msg==WM_LBUTTONUP){s.dragging=-1;if(GetCapture()==host)ReleaseCapture();}
        return true;
    }
    if(s.dragging>=0 && (msg==WM_CANCELMODE || msg==WM_CAPTURECHANGED || (msg==WM_KEYDOWN && key==VK_ESCAPE))) {
        s.dragging=-1;
        if(GetCapture()==host)ReleaseCapture();return true;
    }
    if(msg==WM_MOUSEMOVE && HitBottomPanelEdge(app,x,y)>=0) {
        SetCursor(LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_SIZEWE)));return true;
    }
    if(msg==WM_SETCURSOR && LOWORD(pos)==HTCLIENT) {
        POINT p{};GetCursorPos(&p);ScreenToClient(host,&p);
        if(s.dragging>=0 || HitBottomPanelEdge(app,p.x,p.y)>=0){SetCursor(LoadCursorW(nullptr,reinterpret_cast<LPCWSTR>(IDC_SIZEWE)));return true;}
    }
    return false;
}
} // namespace mikumikustudio
