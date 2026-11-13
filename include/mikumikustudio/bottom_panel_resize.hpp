#pragma once
#include "mikumikustudio/bottom_panel_layout.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <vector>
#include "mikumikustudio/ui_control_layout.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/main_panel_layout.hpp"
#include "mikumikustudio/update_coordinator.hpp"

namespace mikumikustudio {
void ApplyTemplateUiFont(MMDApp*, HWND, int percent, int buttonWidth = 0);

struct ResizableBottomPanel { int id, left, right, extra; };
inline constexpr int kCameraFollowRowWidth = 282;
inline constexpr int kLightTwoColumnWidth = 306;
inline constexpr int kMinimumBottomPanelWidthPercent = 70;
inline std::vector<ResizableBottomPanel> BottomPanelSpans(MMDApp* app, int* horizontalPercent = nullptr) {
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
    const int requested = ui_scale::Percent();
    const int minimum = build_profile::original ? 100 : kMinimumBottomPanelWidthPercent;
    const int fit = std::max(minimum,
        (static_cast<int>(client.right) - 3) * 100 / spans.back().right);
    const int percent = std::min(requested, fit);
    if (horizontalPercent) *horizontalPercent = percent;
    int available = std::max(0, (static_cast<int>(client.right) - 3) * 100 / percent - spans.back().right);
    // Keep every panel reachable after shrinking the application window.
    for(auto& s:spans) if(s.right-s.left>20) {
        s.extra=(std::min)(available,app->bottomPanelSizing.extra[s.id]);
        available-=s.extra;
    }
    return spans;
}
inline int BottomPanelX(const std::vector<ResizableBottomPanel>& spans, int x, int percent = 100) {
    int shift=0;
    for(const auto& p:spans) {
        if(x<p.left) return MulDiv(x + shift, percent, 100);
        if(x<=p.right) return MulDiv(x + shift + MulDiv(x-p.left,p.extra,p.right-p.left), percent, 100);
        shift+=p.extra;
    }
    return MulDiv(x + shift, percent, 100);
}
inline int BottomPanelX(MMDApp* app, int x) {
    int percent = 100;
    const auto spans = BottomPanelSpans(app, &percent);
    return BottomPanelX(spans, x, percent);
}
inline int BottomPanelFontPercent(MMDApp* app) {
    int percent = 100;
    BottomPanelSpans(app, &percent);
    return percent;
}
inline const ResizableBottomPanel& BottomPanelSpan(const std::vector<ResizableBottomPanel>& spans, int id) {
    return *std::find_if(spans.begin(), spans.end(), [=](const auto& span) { return span.id == id; });
}
inline bool BottomPanelFitsRow(const ResizableBottomPanel& span, int width) {
    return span.right - span.left + span.extra >= width;
}
inline RECT BottomCurveRect(MMDApp* app, int clientBottom) {
    auto curve = BottomTemplate(app, clientBottom).CurveRect();
    int percent = 100;
    const auto spans = BottomPanelSpans(app, &percent);
    curve.left = BottomPanelX(spans, curve.left, percent);
    curve.right = BottomPanelX(spans, curve.right, percent);
    if (!build_profile::original) {
        const LONG side = std::max(0L, std::min(curve.right - curve.left, curve.bottom - curve.top));
        curve.right = curve.left + side;
        curve.bottom = curve.top + side;
    }
    return curve;
}
inline POINT BottomCurveDesignPoint(const RECT& curve, POINT pointer) {
    const auto axis = [](LONG value, LONG first, LONG end) {
        const LONG span = end - first - 1;
        return span > 0 ? MulDiv(std::clamp(value - first, 0L, span), 127, span) : 0;
    };
    return {axis(pointer.x, curve.left, curve.right), axis(pointer.y, curve.top, curve.bottom)};
}
inline void ResizeBottomPanelControls(MMDApp* app) {
    HWND host=app->MainWindow(); RECT client{};GetClientRect(host,&client);
    const auto layout = BottomTemplate(app, client.bottom);
    int percent = 100;
    const auto spans = BottomPanelSpans(app, &percent);
    const bool wideNames = layout.StackNameSelectors() && app->CameraMode() != 0;
    const auto X = [&](int x) { return BottomPanelX(spans, x, percent); };
    const auto W = [&](int width) { return MulDiv(width, percent, 100); };
    const bool camera = !build_profile::original && app->CameraMode() != 0;
    const bool cameraRow = camera && app->UiOptionFlag(1) &&
        BottomPanelFitsRow(BottomPanelSpan(spans, 2), kCameraFollowRowWidth);
    const bool lightColumns = camera && app->UiOptionFlag(2) &&
        BottomPanelFitsRow(BottomPanelSpan(spans, 4), kLightTwoColumnWidth);
    for(HWND child=GetWindow(host,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT r=ui::ReadLayoutRect(host,child);
        if(r.top<client.bottom-157 || r.top>=client.bottom) continue;
        const int id = GetDlgCtrlID(child);
        const bool volume = id == panel::kFrameVolumeSlider;
        const bool playback = id >= 408 && id <= 414 && id != 412;
        int left = X(r.left), right = X(r.right);
        int top = r.top <= client.bottom - 154 ? layout.Y(r.top) : layout.ContentY(r.top, playback || volume);
        int height = ui_scale::Pixels(r.bottom - r.top);
        if (cameraRow && (id == 449 || id == 450 || id == 452)) {
            const auto& span = BottomPanelSpan(spans, 2);
            const int start = X(span.left) + W(7);
            const int end = X(span.right) - W(6);
            const int registerLeft = end - W(45);
            const int half = (registerLeft - start - W(8)) / 2;
            left = id == 449 ? start : id == 450 ? start + half + W(4) : registerLeft;
            right = id == 449 ? start + half : id == 450 ? registerLeft - W(4) : end;
            top = layout.ContentY(client.bottom - 53);
            if (id == 452) height = ui_scale::Pixels(22);
        } else if (wideNames && app->UiOptionFlag(1)) {
            if (id == 449 || id == 450) right = X(BottomPanelSpan(spans, 2).right) - W(6);
            if (id == 452) top = layout.ContentY(client.bottom - 28) + ui_scale::Pixels(26);
        }
        if (lightColumns && id >= 455 && id <= 466) {
            const auto& span = BottomPanelSpan(spans, 4);
            const bool edit = id >= 461;
            const int channel = id - (edit ? 461 : 455);
            const int start = X(span.left), end = X(span.right);
            const int middle = (start + end) / 2;
            const int columnLeft = channel < 3 ? start : middle;
            const int columnRight = channel < 3 ? middle : end;
            const int fieldRight = columnRight - W(6);
            const int fieldLeft = fieldRight - W(27);
            left = edit ? fieldLeft : columnLeft + W(17);
            right = edit ? fieldRight : fieldLeft - W(3);
            top = layout.ContentY(client.bottom - 140 + (channel % 3) * 22 + (edit ? 2 : 0));
        }
        if (camera && app->UiOptionFlag(2) && (id == 467 || id == 468)) {
            const auto& span = BottomPanelSpan(spans, 4);
            const int start = X(span.left), end = X(span.right);
            const int middle = (start + end) / 2;
            const int width = W(64);
            const bool reset = id == 467;
            if (lightColumns) {
                const int center = reset ? (start + middle) / 2 : (middle + end) / 2;
                left = center - width / 2;
                top = layout.ContentY(client.bottom - 69);
            } else {
                const int gap = W(12);
                left = reset ? middle - gap / 2 - width : middle + (gap + 1) / 2;
            }
            right = left + width;
        }
        if (wideNames && app->UiOptionFlag(3)) {
            if (id == 474 || id == 475) {
                if (id == 475) left = X(r.left - 94);
                right = X(BottomPanelSpan(spans, 6).right) - W(6);
            }
            if (id == 475 || (id >= 478 && id <= 485) || id == 487) top += ui_scale::Pixels(26);
        }
        ApplyTemplateUiFont(app, child, percent, right - left);
        if(left==r.left && right==r.right && top==r.top && height==r.bottom-r.top && !volume && ui_scale::Percent() == 100) continue;
        if (volume) height = layout.Y(r.bottom) - top;
        wchar_t cls[32]{};GetClassNameW(child,cls,32);
        if(_wcsicmp(cls,L"COMBOBOX")==0) {
            height=(std::max)(height,ui_scale::Pixels(ui::ReadLayoutRequestedHeight(child)));
        }
        ui::MoveLayoutWindow(child,left,top,right-left,height,app->bottomPanelSizing.relayout);
    }
}
inline int HitBottomPanelEdge(MMDApp* app,int x,int y) {
    RECT r{};GetClientRect(app->MainWindow(),&r);
    if(app->FullscreenMode() || app->sidebarSizing.active || app->sidebarSizing.preparing) return -1;
    const auto layout = MainPanelLayout(app, r.bottom);
    if (!build_profile::original && !app->RecordingWindow() && layout.heightBudget > 0) {
        if (x >= layout.Pixels(3) && x < app->SidebarWidth() - layout.Pixels(3) &&
            y >= layout.RulerTop() - 3 && y <= layout.RulerTop())
            return kTimelineHeightGrip;
        if (x >= 0 && x < r.right && y >= layout.bottomTop - 1 && y < layout.bottomTop + 3)
            return kBottomHeightGrip;
    }
    if(y<layout.bottomTop+3 || y>=r.bottom-3) return -1;
    for(const auto& p:BottomPanelSpans(app))
        if(p.right-p.left>20 && abs(x-BottomPanelX(app,p.right))<=2) return p.id;
    return -1;
}
inline bool IsPanelHeightGrip(int grip) {
    return grip == kTimelineHeightGrip || grip == kBottomHeightGrip;
}

inline void SetPanelResizeCursor(int grip) {
    SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IsPanelHeightGrip(grip) ? IDC_SIZENS : IDC_SIZEWE)));
}

inline bool HandleBottomPanelResize(MMDApp* app,UINT msg,WPARAM key,LPARAM pos) {
    auto& s=app->bottomPanelSizing; HWND host=app->MainWindow();
    const int x=static_cast<short>(LOWORD(pos)),y=static_cast<short>(HIWORD(pos));
#if !MMD_ORIGINAL_PROFILE
    if(msg==WM_CAPTURECHANGED && GetCapture()==host)return false;
    if(s.preparing){
        if(msg==WM_LBUTTONUP || msg==WM_CANCELMODE || msg==WM_CAPTURECHANGED || (msg==WM_KEYDOWN && key==VK_ESCAPE)){
            s.preparing=false;++app->bottomPanelSizingSerial;return true;
        }
        if(msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK || msg==WM_MOUSEMOVE)return true;
    }
#endif
    const auto entryEpoch=app->bottomPanelSizingSerial;
    const auto entryToken=s.layoutToken;
    const auto entryDragging=s.dragging;
    const auto sidebarEpoch=app->sidebarSizingSerial;
    auto current=[&] {
        return build_profile::original || (app->bottomPanelSizingSerial==entryEpoch &&
            s.layoutToken==entryToken && s.dragging==entryDragging && app->sidebarSizingSerial==sidebarEpoch);
    };
    auto redraw=[&](int grip, bool viewportMoved = false) {
        const auto redrawEpoch=app->bottomPanelSizingSerial;
        const auto redrawToken=s.layoutToken;
        const auto redrawDragging=s.dragging;
        const auto redrawSidebar=app->sidebarSizingSerial;
        const auto redrawExtra = s.extra;
        const int redrawWidth=app->SidebarWidth();const float redrawRatio=app->SidebarRatio();
        auto unchanged=[&] {
            if(build_profile::original)return true;
            const bool bottom=(app->bottomPanelSizingSerial==redrawEpoch &&
                s.layoutToken==redrawToken && s.dragging==redrawDragging) || (s.dragging<0 && !s.preparing);
            const bool side=app->sidebarSizingSerial==redrawSidebar ||
                (!app->sidebarSizing.active && !app->sidebarSizing.preparing);
            // A nested press can fail before starting a gesture. Its serial
            // alone must not discard the geometry queued by this restoration.
            return bottom && side && s.extra == redrawExtra &&
                app->SidebarWidth()==redrawWidth && app->SidebarRatio()==redrawRatio;
        };
        ui::LayoutBatch batch;
        {
            struct LayoutScope {
                MMDApp& app;bool previous;unsigned char ready;std::uint64_t epoch,token,side;int dragging;
                explicit LayoutScope(MMDApp& value):app(value),previous(value.bottomPanelSizing.relayout),ready(value.WindowLayoutReady()),
                    epoch(value.bottomPanelSizingSerial),token(value.bottomPanelSizing.layoutToken),side(value.sidebarSizingSerial),dragging(value.bottomPanelSizing.dragging){
                    app.bottomPanelSizing.relayout=true;app.WindowLayoutReady()=0;
                }
                ~LayoutScope(){if(build_profile::original || (app.bottomPanelSizingSerial==epoch &&
                    app.bottomPanelSizing.layoutToken==token && app.bottomPanelSizing.dragging==dragging && app.sidebarSizingSerial==side) ||
                    (app.bottomPanelSizing.dragging<0 && !app.bottomPanelSizing.preparing &&
                     !app.sidebarSizing.active && !app.sidebarSizing.preparing)){
                    app.bottomPanelSizing.relayout=previous;app.WindowLayoutReady()=ready;}}
            } scope(*app);
            if (viewportMoved) {
                plugins::Layout(app);
                if (!unchanged()) return;
            }
            HandleWindowSize(app);
            if(!unchanged())return;
            batch.Commit();
        }
        if(!unchanged())return;
        RECT band{};
        GetClientRect(host, &band);
        const auto layout = MainPanelLayout(app, band.bottom);
        if (viewportMoved) {
            updates::RequestViewport(app, true);
            RECT controls = band;
            controls.top = std::max(0, layout.bottomTop + 3);
            RedrawWindow(host, &controls, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        } else if (grip == kTimelineHeightGrip) {
            band.right = app->SidebarWidth();
        } else {
            band.top = std::max(0, layout.bottomTop);
        }
        ui::RepaintLayout(host, batch.Changes(), band);
    };
    auto update=[&] {
        RECT r{};
        GetClientRect(host, &r);
        if (IsPanelHeightGrip(s.dragging)) {
            const auto layout = MainPanelLayout(app, r.bottom);
            const bool timeline = s.dragging == kTimelineHeightGrip;
            const int effective = timeline ? layout.timelineOffset : BottomTemplate(app, r.bottom).extraHeight;
            // Use the sibling's effective size at press time. On a
            // compact window it can grow while this grip gives up space.
            const int sibling = timeline
                ? std::clamp(s.extra[kBottomHeightGrip], 0, std::max(0, layout.heightBudget - s.startExtra))
                : layout.timelineOffset;
            const int delta = timeline ? y - s.startY : s.startY - y;
            const int next = std::clamp(s.startExtra + delta, 0, layout.heightBudget - sibling);
            if (next != effective) {
                s.extra[s.dragging] = next;
                redraw(s.dragging, MainPanelLayout(app, r.bottom).bottomExtra != layout.bottomExtra);
            }
            return;
        }
        int percent = 100;
        const auto spans=BottomPanelSpans(app, &percent);
        const int total=std::max(0,(int(r.right)-3)*100/percent-spans.back().right);
        int available=total,reserved=0,current=0;
        // Reserve siblings at the starting width, rather than at each live
        // intermediate width: shrinking must not prevent dragging back right.
        for(const auto& p:spans){
            if(p.id==s.dragging)current=p.extra;
            if(p.right-p.left<=20)continue;
            const int extra=(std::min)(available,p.id==s.dragging?s.startExtra:s.extra[p.id]);
            available-=extra;if(p.id!=s.dragging)reserved+=extra;
        }
        const int limit=total-reserved;
        const int next=(std::clamp)(s.startExtra+MulDiv(x-s.startX,100,percent),0,(std::max)(0,limit));
        // A repeated notification (including release) needs no new layout.
        if(next!=current){s.extra[s.dragging]=next;redraw(s.dragging);}
    };
    if(msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK) {
        if (s.dragging >= 0) return true;
        const int id=HitBottomPanelEdge(app,x,y);if(id<0)return false;
        const bool heightGrip = IsPanelHeightGrip(id);
        if (msg == WM_LBUTTONDBLCLK && !heightGrip) return false;
        if (heightGrip && GetCapture() != nullptr) return true;
#if !MMD_ORIGINAL_PROFILE
        if(s.dragging>=0 || app->sidebarSizing.active || app->sidebarSizing.preparing)return true;
        const auto epoch=++app->bottomPanelSizingSerial;
        std::uint64_t token=0;
        POINT point{x,y};if(!ClientToScreen(host,&point))return true;
        s.preparing=true;s.epoch=epoch;
        if(!plugins::BeginLayoutInteraction(app,heightGrip ? 31 : 2,token)){
            if(app->bottomPanelSizingSerial==epoch)s.preparing=false;
            return true;
        }
        if(app->bottomPanelSizingSerial!=epoch || !s.preparing || s.dragging>=0 || app->sidebarSizing.active ||
            !IsWindow(host) || !ScreenToClient(host,&point) || HitBottomPanelEdge(app,point.x,point.y)!=id){
            if(app->bottomPanelSizingSerial==epoch)s.preparing=false;
            plugins::EndLayoutInteraction(app,token);return true;
        }
        s.layoutToken=token;s.epoch=epoch;s.relayout=false;s.preparing=false;
        s.dragging=id;s.startExtra=s.extra[id];s.startX=point.x;s.startY=point.y;
#else
        s.dragging=id;s.startExtra=s.extra[id];s.startX=x;
#endif
        // Keep the requested extent for cancellation and anchor movement
        // at the effective extent when the client has constrained it.
        if (heightGrip) {
            RECT client{};
            GetClientRect(host, &client);
            const auto layout = MainPanelLayout(app, client.bottom);
            if (id == kTimelineHeightGrip)
                s.startY += s.startExtra - layout.timelineOffset;
            else
                s.startY -= s.startExtra - BottomTemplate(app, client.bottom).extraHeight;
        } else {
            int percent = 100;
            const auto spans = BottomPanelSpans(app, &percent);
            for(const auto& p:spans)
                if(p.id==id)s.startX+=MulDiv(s.startExtra-p.extra,percent,100);
        }
        SetCapture(host);
#if !MMD_ORIGINAL_PROFILE
        // SetCapture can synchronously cancel this gesture or start a new one.
        if(app->bottomPanelSizingSerial!=epoch || s.epoch!=epoch || s.layoutToken!=token || s.dragging!=id)return true;
        if(GetCapture()!=host){s.dragging=-1;s.layoutToken=0;plugins::EndLayoutInteraction(app,token);return true;}
        if (heightGrip) {
            SetFocus(host);
            if (app->bottomPanelSizingSerial != epoch || s.layoutToken != token ||
                s.dragging != id || GetCapture() != host) return true;
        }
#else
        if(GetCapture()!=host)s.dragging=-1;
#endif
        SetPanelResizeCursor(id);return true;
    }
    if(s.dragging>=0 && msg==WM_MOUSEMOVE) {
        update();if(current())SetPanelResizeCursor(entryDragging);return true;
    }
    if(s.dragging>=0 && msg==WM_LBUTTONUP) {
        update();if(!current() || s.dragging<0)return true;
        SetPanelResizeCursor(entryDragging);
        s.dragging=-1;s.layoutToken=0;
        if(!build_profile::original){s.relayout=false;app->WindowLayoutReady()=1;}
        if(GetCapture()==host)ReleaseCapture();
#if !MMD_ORIGINAL_PROFILE
        ui::InvalidateBottomTrackbars(*app);
        plugins::EndLayoutInteraction(app,entryToken);
#endif
        return true;
    }
    if(s.dragging>=0 && (msg==WM_CANCELMODE || msg==WM_CAPTURECHANGED || (msg==WM_KEYDOWN && key==VK_ESCAPE))) {
        const int id=s.dragging;s.dragging=-1;s.layoutToken=0;
        if(!build_profile::original){s.relayout=false;app->WindowLayoutReady()=1;}
        if (s.extra[id] != s.startExtra) {
            RECT client{};
            GetClientRect(host, &client);
            const int previousBottom = MainPanelLayout(app, client.bottom).bottomExtra;
            s.extra[id] = s.startExtra;
            redraw(id, MainPanelLayout(app, client.bottom).bottomExtra != previousBottom);
        }
        if((build_profile::original || (s.dragging<0 && !s.preparing &&
            !app->sidebarSizing.active && !app->sidebarSizing.preparing)) && GetCapture()==host)ReleaseCapture();
#if !MMD_ORIGINAL_PROFILE
        ui::InvalidateBottomTrackbars(*app);
        plugins::EndLayoutInteraction(app,entryToken);
#endif
        return true;
    }
    int grip = -1;
    if (msg == WM_MOUSEMOVE && !GetCapture()) grip = HitBottomPanelEdge(app, x, y);
    if (msg == WM_SETCURSOR && LOWORD(pos) == HTCLIENT &&
        (s.dragging >= 0 || (!GetCapture() && reinterpret_cast<HWND>(key) == host))) {
        POINT p{};
        if (GetCursorPos(&p) && ScreenToClient(host, &p))
            grip = s.dragging >= 0 ? s.dragging : HitBottomPanelEdge(app, p.x, p.y);
    }
    if (grip >= 0) {
        SetPanelResizeCursor(grip);
        return true;
    }
    return false;
}
} // namespace mikumikustudio
