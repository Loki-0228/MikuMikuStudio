#pragma once
#include <cstdint>
#include "curve_editor_control_lifetime.hpp"

// The native tree owns scrolling; both callbacks share one lifetime. Neither
// a recycled HWND nor a replacement subclass inherits the old interaction.
namespace curve_editor_channel_scroll {
constexpr int kId = 1110;
constexpr UINT_PTR kSubclass = 0x43534352;
// Timer messages may outlive cancellation and even a native HWND. Never
// reuse an ID on this UI thread; exhaustion refuses a new interaction.
inline thread_local UINT_PTR lastInteraction = 0;
inline UINT_PTR AllocateInteraction(UINT_PTR& last) noexcept {
    if(last==~UINT_PTR(0))return 0;
    return ++last;
}
struct State : curve_editor_control_lifetime::State {
    HWND tree = nullptr, bar = nullptr, parent = nullptr;
    bool initializing = true, dragging = false, hover = false;
    int grab = 0, repeat = -1;
    UINT_PTR interaction = 0, timerId = 0;
    std::uint64_t content = 0;
};
struct Metrics {
    RECT bounds{}, thumb{};
    SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL};
    int arrow = 0, travel = 0, maximum = 0;
};
inline LRESULT CALLBACK TreeProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
inline LRESULT CALLBACK BarProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
inline bool Current(const State& state) noexcept {
    return !state.retired &&
        curve_editor_control_lifetime::Exact(state.tree,TreeProc,kSubclass,&state) &&
        curve_editor_control_lifetime::Exact(state.bar,BarProc,kSubclass,&state) &&
        GetParent(state.tree)==state.parent && GetParent(state.bar)==state.parent;
}
inline bool Interaction(const State& state,UINT_PTR ticket) noexcept {
    return ticket&&Current(state)&&state.interaction==ticket;
}
inline void CancelInteraction(State& state) noexcept {
    const auto timer=state.timerId;
    state.interaction=state.timerId=0;
    state.dragging=false;state.repeat=-1;
    if(timer&&curve_editor_control_lifetime::Exact(state.bar,BarProc,kSubclass,&state))KillTimer(state.bar,timer);
}
inline void Retire(State& state,HWND closing=nullptr) noexcept {
    if(state.retired)return;
    state.retired=true;
    const bool ownTree=curve_editor_control_lifetime::Exact(state.tree,TreeProc,kSubclass,&state);
    const bool ownBar=curve_editor_control_lifetime::Exact(state.bar,BarProc,kSubclass,&state);
    CancelInteraction(state);
    if(ownTree)RemoveWindowSubclass(state.tree,TreeProc,kSubclass);
    if(ownBar){
        RemoveWindowSubclass(state.bar,BarProc,kSubclass);
        if(closing!=state.bar)DestroyWindow(state.bar);
    }
}
struct Scope {
    State& state;curve_editor_control_lifetime::Pin<State> pin;
    explicit Scope(State& value)noexcept:state(value),pin(value){}
    ~Scope(){if(!state.initializing&&!state.retired&&!Current(state))Retire(state);}
};
inline UINT_PTR BeginInteraction(State& state) noexcept {
    CancelInteraction(state);
    if(!Current(state)||state.interaction)return 0;
    const auto ticket=AllocateInteraction(lastInteraction);
    if(!ticket){
        if(GetCapture()==state.bar)ReleaseCapture();
        return 0;
    }
    state.interaction=ticket;
    return ticket;
}
inline bool ArmRepeat(State& state,UINT_PTR ticket,UINT delay) {
    if(!Interaction(state,ticket)||GetCapture()!=state.bar||state.repeat<0)return false;
    state.timerId=ticket;
    // With an HWND, wParam uses our request ID, not SetTimer's return value.
    if(!SetTimer(state.bar,ticket,delay,nullptr)){
        if(Interaction(state,ticket)&&state.timerId==ticket)CancelInteraction(state);
        return false;
    }
    return Interaction(state,ticket)&&state.timerId==ticket;
}
inline State* ForTree(HWND tree)noexcept {
    DWORD_PTR data=0;
    return GetWindowSubclass(tree,TreeProc,kSubclass,&data)?reinterpret_cast<State*>(data):nullptr;
}
inline void Layout(HWND tree);
inline Metrics Measure(HWND bar,State& state) {
    Scope scope(state);Metrics m;
    if(!Current(state)||bar!=state.bar)return m;
    GetClientRect(bar,&m.bounds);if(!Current(state))return {};
    GetScrollInfo(state.tree,SB_VERT,&m.info);if(!Current(state))return {};
    m.arrow=std::min(int(m.bounds.right),int(m.bounds.bottom/2));
    const int length=std::max(0,int(m.bounds.bottom)-2*m.arrow);
    const int range=std::max(1,m.info.nMax-m.info.nMin+1);
    m.maximum=std::max(m.info.nMin,m.info.nMax-int(m.info.nPage)+1);
    const int size=std::min(length,std::max(m.arrow*2,int(double(length)*m.info.nPage/range)));
    m.travel=std::max(0,length-size);
    const int top=m.arrow+(m.maximum>m.info.nMin
        ?MulDiv(m.travel,m.info.nPos-m.info.nMin,m.maximum-m.info.nMin):0);
    m.thumb={0,top,m.bounds.right,top+size};
    return m;
}
inline void Paint(HWND bar,State& state,HDC dc) {
    using namespace curve_editor_theme;
    const auto m=Measure(bar,state);if(!Current(state))return;
    Fill(dc,m.bounds,Mix(paper,shell,.3));
    const bool enabled=m.maximum>m.info.nMin&&IsWindowEnabled(state.tree);
    const int center=m.bounds.right/2,arrowSize=std::max(2,center/2);
    for(int direction:{-1,1}){
        const int y=direction<0?m.arrow/2:m.bounds.bottom-m.arrow/2;
        POINT points[]={{center-arrowSize,y-direction*2},{center+arrowSize,y-direction*2},{center,y+direction*2}};
        HBRUSH brush=CreateSolidBrush(enabled?muted:Mix(paper,muted,.35));
        auto oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
        Polygon(dc,points,3);SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
    }
    if(enabled&&m.thumb.bottom>m.thumb.top){
        const int width=std::max(4,int(m.bounds.right)*(state.hover||state.dragging?3:2)/5);
        HBRUSH brush=CreateSolidBrush(state.dragging?accent:(state.hover?text:muted));
        auto oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
        RoundRect(dc,center-width/2,m.thumb.top,center+(width+1)/2,m.thumb.bottom,width,width);
        SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
    }
}
inline void ScrollTo(State& state,int position,UINT_PTR ticket) {
    const auto content=state.content;
    const auto current=[&]{return Interaction(state,ticket)&&state.content==content;};
    SCROLLINFO info{sizeof(SCROLLINFO),SIF_ALL};GetScrollInfo(state.tree,SB_VERT,&info);if(!current())return;
    position=std::clamp(position,info.nMin,std::max(info.nMin,info.nMax-int(info.nPage)+1));
    HTREEITEM item=TreeView_GetFirstVisible(state.tree);if(!current())return;
    int delta=position-info.nPos;
    while(item&&delta!=0){
        auto next=delta>0?TreeView_GetNextVisible(state.tree,item):TreeView_GetPrevVisible(state.tree,item);
        if(!current())return;if(!next)break;
        item=next;delta+=delta>0?-1:1;
    }
    if(item&&current())TreeView_SelectSetFirstVisible(state.tree,item);
}
inline void Step(HWND bar,State& state,UINT_PTR ticket) {
    if(!Interaction(state,ticket)||bar!=state.bar||GetCapture()!=bar||state.repeat<0)return;
    SendMessageW(state.tree,WM_VSCROLL,state.repeat,0);
    if(Interaction(state,ticket))InvalidateRect(bar,nullptr,FALSE);
}
inline LRESULT CALLBACK TreeProc(HWND tree,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto& state=*reinterpret_cast<State*>(data);Scope scope(state);
    if(message==WM_NCDESTROY){Retire(state,tree);return DefSubclassProc(tree,message,wp,lp);}
    if(!Current(state))return state.initializing&&!state.retired&&
        curve_editor_control_lifetime::Exact(tree,TreeProc,kSubclass,&state)?DefSubclassProc(tree,message,wp,lp):0;
    if(message==TVM_INSERTITEMW||message==TVM_INSERTITEMA||message==TVM_DELETEITEM)++state.content;
    const LRESULT result=DefSubclassProc(tree,message,wp,lp);
    if(!Current(state))return result;
    if(message==WM_VSCROLL||message==WM_MOUSEWHEEL||message==WM_KEYDOWN||
       message==WM_SIZE||message==WM_ENABLE||message==WM_PAINT||
       (message>=TV_FIRST&&message<TV_FIRST+100))
        InvalidateRect(state.bar,nullptr,FALSE);
    if(message==WM_SIZE||message==TVM_EXPAND||message==TVM_INSERTITEMW||
       message==TVM_DELETEITEM||message==TVM_SETITEMHEIGHT||message==TVM_SELECTITEM)
        Layout(tree);
    return result;
}
inline LRESULT CALLBACK BarProc(HWND bar,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
    auto& state=*reinterpret_cast<State*>(data);Scope scope(state);
    if(message==WM_NCDESTROY){Retire(state,bar);return DefSubclassProc(bar,message,wp,lp);}
    if(!Current(state))return 0;
    switch(message){
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        curve_editor_control_lifetime::Paint paint(bar);
        if(Current(state))Paint(bar,state,paint.dc);return 0;
    }
    case WM_PRINTCLIENT:Paint(bar,state,reinterpret_cast<HDC>(wp));return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:{
        const auto ticket=BeginInteraction(state);if(!ticket)return 0;
        const auto m=Measure(bar,state);
        if(!Interaction(state,ticket))return 0;
        if(m.maximum<=m.info.nMin||!IsWindowEnabled(state.tree)){CancelInteraction(state);return 0;}
        SetFocus(state.tree);if(!Interaction(state,ticket))return 0;
        if(GetCapture()!=bar)SetCapture(bar);
        if(!Interaction(state,ticket))return 0;
        if(GetCapture()!=bar){CancelInteraction(state);return 0;}
        const int y=GET_Y_LPARAM(lp);
        state.dragging=y>=m.thumb.top&&y<m.thumb.bottom;
        if(state.dragging)state.grab=y-m.thumb.top;
        else{
            state.repeat=y<m.arrow?SB_LINEUP:y>=m.bounds.bottom-m.arrow?SB_LINEDOWN:
                y<m.thumb.top?SB_PAGEUP:SB_PAGEDOWN;
            Step(bar,state,ticket);if(!Interaction(state,ticket))return 0;
            if(!ArmRepeat(state,ticket,400))return 0;
        }
        InvalidateRect(bar,nullptr,FALSE);return 0;
    }
    case WM_MOUSEMOVE:{
        if(!state.hover){state.hover=true;TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,bar,0};TrackMouseEvent(&t);if(!Current(state))return 0;}
        if(state.dragging){
            const auto ticket=state.interaction;
            const auto m=Measure(bar,state);if(!Interaction(state,ticket)||GetCapture()!=bar)return 0;
            if(m.travel>0)ScrollTo(state,m.info.nMin+MulDiv(std::clamp(GET_Y_LPARAM(lp)-state.grab-m.arrow,0,m.travel),m.maximum-m.info.nMin,m.travel),ticket);
            if(!Interaction(state,ticket))return 0;
        }
        InvalidateRect(bar,nullptr,FALSE);return 0;
    }
    case WM_MOUSELEAVE:state.hover=false;InvalidateRect(bar,nullptr,FALSE);return 0;
    case WM_TIMER:{
        if(wp&&wp==state.timerId){
            const auto ticket=state.interaction;
            if(!Interaction(state,ticket))return 0;
            if(GetCapture()!=bar||state.repeat<0){CancelInteraction(state);return 0;}
            POINT point{};GetCursorPos(&point);ScreenToClient(bar,&point);
            const auto m=Measure(bar,state);if(!Interaction(state,ticket))return 0;
            if(GetCapture()!=bar||state.repeat<0){CancelInteraction(state);return 0;}
            if(PtInRect(&m.bounds,point)&&
               ((state.repeat==SB_LINEUP&&point.y<m.arrow)||
                (state.repeat==SB_LINEDOWN&&point.y>=m.bounds.bottom-m.arrow)||
                (state.repeat==SB_PAGEUP&&point.y>=m.arrow&&point.y<m.thumb.top)||
                (state.repeat==SB_PAGEDOWN&&point.y>=m.thumb.bottom&&point.y<m.bounds.bottom-m.arrow)))
                Step(bar,state,ticket);
            if(!Interaction(state,ticket))return 0;
            ArmRepeat(state,ticket,60);
            return 0;
        }return 0;
    }
    case WM_LBUTTONUP:if(GetCapture()==bar)ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        CancelInteraction(state);if(!Current(state)||state.interaction)return 0;
        if(message==WM_CANCELMODE&&GetCapture()==bar)ReleaseCapture();
        if(Current(state))InvalidateRect(bar,nullptr,FALSE);return 0;
    case WM_MOUSEWHEEL:return SendMessageW(state.tree,message,wp,lp);
    }
    return DefSubclassProc(bar,message,wp,lp);
}
inline void Attach(HWND tree) {
    if(!tree||ForTree(tree))return;
    auto* state=new State;state->tree=tree;state->parent=GetParent(tree);
    curve_editor_control_lifetime::Pin<State> construction(*state);
    if(!SetWindowSubclass(tree,TreeProc,kSubclass,reinterpret_cast<DWORD_PTR>(state))){Retire(*state);return;}
    const HWND bar=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_NOTIFY,
        0,0,10,10,state->parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kId)),GetModuleHandleW(nullptr),nullptr);
    if(state->retired||!curve_editor_control_lifetime::Exact(tree,TreeProc,kSubclass,state)||GetParent(tree)!=state->parent){
        if(bar&&IsWindow(bar))DestroyWindow(bar);Retire(*state);return;
    }
    if(!bar){Retire(*state);return;}
    state->bar=bar;
    if(!SetWindowSubclass(bar,BarProc,kSubclass,reinterpret_cast<DWORD_PTR>(state))){
        const bool installed=curve_editor_control_lifetime::Exact(bar,BarProc,kSubclass,state);
        Retire(*state);if(!installed)DestroyWindow(bar);return;
    }
    state->initializing=false;
    if(!Current(*state))Retire(*state);
}
inline void Layout(HWND tree) {
    auto* state=ForTree(tree);if(!state)return;Scope scope(*state);
    if(!Current(*state))return;
    SCROLLBARINFO info{sizeof(info)};
    const bool visible=(GetWindowLongPtrW(tree,GWL_STYLE)&WS_VSCROLL)&&GetScrollBarInfo(tree,OBJID_VSCROLL,&info);
    if(!Current(*state))return;
    if(!visible){ShowWindow(state->bar,SW_HIDE);return;}
    RECT r=info.rcScrollBar;MapWindowPoints(nullptr,state->parent,reinterpret_cast<POINT*>(&r),2);
    if(!Current(*state))return;
    SetWindowPos(state->bar,HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    if(Current(*state))InvalidateRect(state->bar,nullptr,FALSE);
}
}
