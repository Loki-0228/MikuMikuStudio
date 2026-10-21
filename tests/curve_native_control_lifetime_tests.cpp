// Standalone native-helper lifetime regression. Link user32, gdi32, comctl32,
// gdiplus and uxtheme; do not link animation_curve_editor_tests or its allocator.
// No global input, desktop pixels, provider callbacks or fault injection.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <windowsx.h>
#include <objidl.h>
#include "mikumikustudio/gdiplus_compat.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "../src/window/curve_editor_theme.hpp"
#include "../src/window/curve_editor_segments.hpp"
#include "../src/window/curve_editor_channel_scroll.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace lifetime_storage {
// Only the exact selected State allocation is quarantined. Its destructor
// runs normally; delete then retains the underlying malloc storage until the
// outer native SendMessage has returned. All checks use unsigned char bytes,
// never a State whose C++ lifetime has ended.
constexpr unsigned char poison = 0xa5;
thread_local void* watched = nullptr;
thread_local std::size_t bytes = 0;
thread_local unsigned deletions = 0;
thread_local unsigned allocations = 0;
thread_local bool outerMessage = false, deletedInsideMessage = false;

void Delete(void* memory) noexcept {
    if (memory && memory == watched) {
        ++deletions;
        deletedInsideMessage = deletedInsideMessage || outerMessage;
        if (deletions == 1) std::memset(memory, poison, bytes);
        return;
    }
    std::free(memory);
}
void Begin(void* memory, std::size_t size) noexcept {
    watched = memory;
    bytes = size;
    deletions = 0;
    outerMessage = deletedInsideMessage = false;
}
std::size_t ChangedBytes() noexcept {
    if (!deletions) return 0;
    const auto* storage = static_cast<const unsigned char*>(watched);
    std::size_t changed = 0;
    for (std::size_t index = 0; index < bytes; ++index)
        if (storage[index] != poison) ++changed;
    return changed;
}
void Release() noexcept {
    // If no delete happened, the live window still owns the allocation. Leave
    // normal fixture cleanup responsible for it rather than freeing it here.
    void* released = deletions ? watched : nullptr;
    watched = nullptr;
    bytes = 0;
    if (released) std::free(released);
}
}

void* operator new(std::size_t size) {
    ++lifetime_storage::allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { lifetime_storage::Delete(memory); }
void operator delete[](void* memory) noexcept { lifetime_storage::Delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { lifetime_storage::Delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { lifetime_storage::Delete(memory); }

namespace {
unsigned checks = 0, failures = 0;

bool Check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", description);
        std::fflush(stderr);
    }
    return condition;
}

struct GdiPlus {
    ULONG_PTR token = 0;
    GdiPlus() {
        Gdiplus::GdiplusStartupInput input;
        Check(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok && token,
            "initialize GDI+ for the real production drawing helpers");
    }
    ~GdiPlus() { curve_editor_theme::Release(); if (token) Gdiplus::GdiplusShutdown(token); }
};

struct Windows {
    HWND root = nullptr, host = nullptr, previousFocus = nullptr;
    HWND requestedFocus = nullptr, mouseTarget = nullptr;
    bool armed = false, destroyReturned = false, destroyedBeforeKillReturn = false;
    bool killDestroys = true;
    bool outerMouseReturned = false, killInsideOuter = false;
    unsigned kills = 0, captureChanges = 0, destroys = 0, nonclientDestroys = 0, notifications = 0;
    HWND requestedCapture = nullptr, captureRedirect = nullptr;
    bool captureRedirected = false;

    static LRESULT CALLBACK Host(HWND window, UINT message, WPARAM w, LPARAM l,
                                  UINT_PTR id, DWORD_PTR data) noexcept {
        auto& self = *reinterpret_cast<Windows*>(data);
        if (message == WM_COMMAND && HIWORD(w) == CBN_SELCHANGE) ++self.notifications;
        if (message == WM_DESTROY) ++self.destroys;
        if (message == WM_NCDESTROY) {
            ++self.nonclientDestroys;
            RemoveWindowSubclass(window, Host, id);
        }
        return DefSubclassProc(window, message, w, l);
    }
    static LRESULT CALLBACK PreviousFocus(HWND window, UINT message, WPARAM w, LPARAM l,
                                           UINT_PTR id, DWORD_PTR data) noexcept {
        auto& self = *reinterpret_cast<Windows*>(data);
        if (message == WM_KILLFOCUS) {
            ++self.kills;
            if (self.armed && self.killDestroys) {
                self.armed = false;
                self.requestedFocus = reinterpret_cast<HWND>(w);
                self.killInsideOuter = lifetime_storage::outerMessage && !self.outerMouseReturned;
                self.destroyReturned = DestroyWindow(self.host) != FALSE;
                self.destroyedBeforeKillReturn = !IsWindow(self.host);
            }
        }
        if (message == WM_CAPTURECHANGED && self.armed && !self.killDestroys) {
            ++self.captureChanges; self.armed = false; self.requestedCapture = reinterpret_cast<HWND>(l);
            self.killInsideOuter = lifetime_storage::outerMessage && !self.outerMouseReturned;
            if(self.captureRedirect){
                SetCapture(self.captureRedirect);
                self.captureRedirected=GetCapture()==self.captureRedirect;
            }else{
                self.destroyReturned = DestroyWindow(self.host) != FALSE;
                self.destroyedBeforeKillReturn = !IsWindow(self.host);
            }
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, PreviousFocus, id);
        return DefSubclassProc(window, message, w, l);
    }
    Windows() {
        const auto instance = GetModuleHandleW(nullptr);
        root = CreateWindowExW(0, L"STATIC", L"Curve helper lifetime fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, nullptr, nullptr, instance, nullptr);
        if (!Check(root != nullptr, "create a fixture-owned hidden standard root window")) return;
        host = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            0, 60, 400, 240, root, reinterpret_cast<HMENU>(INT_PTR(410)), instance, nullptr);
        previousFocus = CreateWindowExW(0, L"EDIT", L"previous focus", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 180, 24, root, reinterpret_cast<HMENU>(INT_PTR(411)), instance, nullptr);
        if (!Check(host && previousFocus, "create a separate focus EDIT and the target's host")) return;
        Check(SetWindowSubclass(host, Host, 0x484f5354, reinterpret_cast<DWORD_PTR>(this)) != FALSE,
            "observe actual destruction of only the fixture's target host");
        Check(SetWindowSubclass(previousFocus, PreviousFocus, 0x464f4353, reinterpret_cast<DWORD_PTR>(this)) != FALSE,
            "install the old EDIT's synchronous kill-focus destruction callback");
    }
    ~Windows() {
        armed = false;
        if (GetCapture() && (GetCapture() == mouseTarget || GetCapture() == root || IsChild(root, GetCapture()))) ReleaseCapture();
        if (root && IsWindow(root)) DestroyWindow(root);
    }
    bool Ready() const noexcept { return root && host && previousFocus; }
    bool FocusPrevious() {
        SetFocus(previousFocus);
        return Check(GetFocus() == previousFocus, "establish real native focus on the independent EDIT");
    }
    void SendMouse(HWND target, LPARAM point, bool destroy) {
        armed = destroy;
        SendNative(target,WM_LBUTTONDOWN,MK_LBUTTON,point);
    }
    void SendNative(HWND target,UINT message,WPARAM w=0,LPARAM l=0) {
        mouseTarget = target;
        outerMouseReturned = false;
        lifetime_storage::outerMessage = true;
        SendMessageW(target,message,w,l);
        lifetime_storage::outerMessage = false;
        outerMouseReturned = true;
    }
    void CheckDestroyed(HWND target, HWND expectedFocus, const char* helper) {
        Check(killInsideOuter && requestedFocus == expectedFocus && kills == 1,
            "the helper's real SetFocus synchronously enters the old EDIT kill-focus callback");
        Check(destroyReturned && destroyedBeforeKillReturn && destroys == 1 && nonclientDestroys == 1 &&
            !IsWindow(host) && !IsWindow(target) && outerMouseReturned,
            "the target host and native target are destroyed before the original mouse callback returns");
        CheckStorage(target,helper);
    }
    void CheckStorage(HWND target,const char* helper) {
        Check(lifetime_storage::deletions == 1 && lifetime_storage::deletedInsideMessage,
            "exactly the watched helper State is deleted while the outer native dispatch remains active");
        const auto changed = lifetime_storage::ChangedBytes();
        std::printf("OBSERVE helper=%s deletes=%u deleted_inside=%u changed_after_delete=%zu capture=%p target=%p\n",
            helper, lifetime_storage::deletions, lifetime_storage::deletedInsideMessage ? 1u : 0u,
            changed, static_cast<void*>(GetCapture()), static_cast<void*>(target));
        Check(lifetime_storage::deletions == 1 && changed == 0,
            "after State deletion the outer mouse callback must not write its quarantined storage");
        Check(GetCapture() == nullptr && GetCapture() != target,
            "the retired mouse callback must not leave capture on the destroyed control or another window");
    }
};

// Installed below the production helper, so destruction happens inside its
// real DefSubclassProc/SendMessage continuation, not after that helper exits.
struct NativeBoundary {
    enum class Action { Host, Tree, ReplaceSegments, ReplaceScroll } action = Action::Host;
    Windows* fixture = nullptr; HWND window = nullptr; UINT trigger = 0;
    bool armed = false, replaced = false, actionDone = false, exception = false;
    unsigned fired = 0, obsoleteMessages = 0; DWORD_PTR replacement = 0;
    static LRESULT CALLBACK Observe(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
        auto& self=*reinterpret_cast<NativeBoundary*>(data);
        if(self.replaced&&lifetime_storage::outerMessage&&(message==CB_GETCOUNT||message==CB_SETCURSEL))++self.obsoleteMessages;
        if(message==WM_NCDESTROY){RemoveWindowSubclass(window,Observe,id);self.window=nullptr;}
        const auto result=DefSubclassProc(window,message,w,l);
        if(self.armed&&message==self.trigger){
            self.armed=false;++self.fired;
            try {
                if(self.action==Action::ReplaceSegments){
                    self.actionDone=RemoveWindowSubclass(window,curve_editor_segments::Proc,curve_editor_segments::kSubclass)!=FALSE;
                    curve_editor_segments::Apply(self.fixture->host);
                    self.actionDone=self.actionDone&&GetWindowSubclass(window,curve_editor_segments::Proc,
                        curve_editor_segments::kSubclass,&self.replacement)&&self.replacement;
                    self.replaced=true;
                }else if(self.action==Action::ReplaceScroll){
                    self.actionDone=RemoveWindowSubclass(window,curve_editor_channel_scroll::TreeProc,
                        curve_editor_channel_scroll::kSubclass)!=FALSE;
                    curve_editor_channel_scroll::Attach(window);
                    self.actionDone=self.actionDone&&GetWindowSubclass(window,curve_editor_channel_scroll::TreeProc,
                        curve_editor_channel_scroll::kSubclass,&self.replacement)&&self.replacement;
                    if(self.actionDone){auto& state=*reinterpret_cast<curve_editor_channel_scroll::State*>(self.replacement);
                        SetWindowPos(state.bar,nullptr,300,100,13,70,SWP_NOZORDER|SWP_NOACTIVATE);}
                }else if(self.action==Action::Tree){
                    self.actionDone=DestroyWindow(window)!=FALSE;
                }else {
                    self.fixture->destroyReturned=DestroyWindow(self.fixture->host)!=FALSE;
                    self.fixture->destroyedBeforeKillReturn=!IsWindow(self.fixture->host);
                    self.actionDone=self.fixture->destroyReturned;
                }
            }catch(...){self.exception=true;}
        }
        return result;
    }
    void Install(HWND target,Windows& value){
        window=target;fixture=&value;
        Check(SetWindowSubclass(target,Observe,0x4c54424e,reinterpret_cast<DWORD_PTR>(this))!=FALSE,
            "install the actual lower native continuation before the production helper");
    }
    ~NativeBoundary(){
        if(window&&curve_editor_control_lifetime::Exact(window,Observe,0x4c54424e,this))RemoveWindowSubclass(window,Observe,0x4c54424e);
    }
};

struct Segments : Windows {
    HWND combo = nullptr;
    DWORD_PTR state = 0;
    explicit Segments(NativeBoundary* boundary=nullptr) {
        if (!Ready()) return;
        combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            0, 0, 280, 160, host, reinterpret_cast<HMENU>(INT_PTR(1007)), GetModuleHandleW(nullptr), nullptr);
        if (!Check(combo != nullptr, "create the real segmented production combo")) return;
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Value"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Speed"));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        if(boundary)boundary->Install(combo,*this);
        curve_editor_segments::Apply(host);
        Check(GetWindowSubclass(combo, curve_editor_segments::Proc, 0x5345474d, &state) && state,
            "obtain the actual production segmented State allocation");
    }
    LPARAM Point(int index) const {
        const auto rect = curve_editor_segments::Cell(combo, index);
        return MAKELPARAM((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
    }
};

struct Scroll : Windows {
    HWND tree = nullptr, bar = nullptr;
    DWORD_PTR state = 0;
    curve_editor_channel_scroll::Metrics metrics{};
    explicit Scroll(NativeBoundary* boundary=nullptr) {
        if (!Ready()) return;
        tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | TVS_HASLINES,
            0, 0, 230, 140, host, reinterpret_cast<HMENU>(INT_PTR(1015)), GetModuleHandleW(nullptr), nullptr);
        if (!Check(tree != nullptr, "create the real native channel tree")) return;
        for (unsigned i = 0; i < 100; ++i) {
            wchar_t text[32]{}; swprintf_s(text, L"Channel %03u", i);
            TVINSERTSTRUCTW row{}; row.hParent = TVI_ROOT; row.hInsertAfter = TVI_LAST;
            row.item.mask = TVIF_TEXT; row.item.pszText = text;
            if (!Check(SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&row)) != 0,
                "insert one real overflow tree row")) return;
        }
        if(boundary)boundary->Install(tree,*this);
        curve_editor_channel_scroll::Attach(tree);
        curve_editor_channel_scroll::Layout(tree);
        bar = GetDlgItem(host, curve_editor_channel_scroll::kId);
        if (!Check(bar && GetWindowSubclass(bar, curve_editor_channel_scroll::BarProc,
            curve_editor_channel_scroll::kSubclass, &state) && state,
            "obtain the actual production scroll-bar State allocation")) return;
        metrics = curve_editor_channel_scroll::Measure(bar, *reinterpret_cast<curve_editor_channel_scroll::State*>(state));
        Check(metrics.maximum > metrics.info.nMin && metrics.travel > 0 &&
            metrics.bounds.right > 0 && metrics.thumb.bottom > metrics.thumb.top,
            "the native tree genuinely overflows and its real bar has a draggable thumb");
    }
    LPARAM ThumbPoint() const noexcept {
        return MAKELPARAM((metrics.bounds.left + metrics.bounds.right) / 2,
            (metrics.thumb.top + metrics.thumb.bottom) / 2);
    }
    bool CanDrag() const noexcept {
        return tree && bar && state && metrics.maximum > metrics.info.nMin && metrics.travel > 0;
    }
};

void NormalSegments() {
    Segments f;
    if (!f.combo || !f.state || !f.FocusPrevious()) return;
    const auto allocations=lifetime_storage::allocations;
    f.SendMouse(f.combo, f.Point(1), false);
    DWORD_PTR current = 0;
    Check(IsWindow(f.host) && GetFocus() == f.combo &&
        SendMessageW(f.combo, CB_GETCURSEL, 0, 0) == 1 && f.notifications == 1,
        "normal segmented click changes one real native selection and notification");
    Check(GetWindowSubclass(f.combo, curve_editor_segments::Proc, 0x5345474d, &current) && current == f.state,
        "normal segmented click preserves its exact subclass State identity");
    f.SendMouse(f.combo, f.Point(0), false);
    Check(SendMessageW(f.combo, CB_GETCURSEL, 0, 0) == 0 && f.notifications == 2,
        "the undestroyed segmented control remains usable for another click");
    Check(lifetime_storage::allocations==allocations,"ordinary segmented callback lifetime pins allocate no C++ storage");
    curve_editor_segments::Apply(f.host);
    Check(GetWindowSubclass(f.combo,curve_editor_segments::Proc,curve_editor_segments::kSubclass,&current)&&current==f.state,
        "reapplying segmented presentation preserves its original State instead of overwriting ownership");
    SendMessageW(f.combo,WM_SIZE,0,MAKELPARAM(280,24));
    if(Check(GetWindowSubclass(f.combo,curve_editor_segments::Proc,curve_editor_segments::kSubclass,&current)&&current==f.state,
        "native resize preserves the segmented State while updating its tooltip")){
        auto& state=*reinterpret_cast<curve_editor_segments::State*>(current);DWORD_PTR tip=0;
        Check(state.tips&&GetWindowSubclass(state.tips,curve_editor_segments::TipProc,curve_editor_segments::kTipsSubclass,&tip)&&tip==current&&state.labels.size()==2,
            "real tooltip resources retain the same State and both labels");
        SendMessageW(f.combo,WM_MOUSEMOVE,0,f.Point(1));
        Check(state.hover==1,"normal segmented hover still follows the actual cell");
        SendMessageW(f.combo,WM_MOUSELEAVE,0,0);Check(state.hover==-1,"normal segmented mouse leave clears hover");
        SendMessageW(f.combo,WM_KEYDOWN,VK_END,0);SendMessageW(f.combo,WM_CHAR,L'\r',0);
        Check(SendMessageW(f.combo,CB_GETCURSEL,0,0)==1&&f.notifications==3,"keyboard activation changes one selection without a duplicate character action");
    }
}

void NormalScroll() {
    Scroll f;
    if (!f.CanDrag() || !f.FocusPrevious()) return;
    const auto before = TreeView_GetFirstVisible(f.tree);
    const auto allocations=lifetime_storage::allocations;
    f.SendMouse(f.bar, f.ThumbPoint(), false);
    DWORD_PTR current = 0;
    Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc,
        curve_editor_channel_scroll::kSubclass, &current) && current == f.state &&
        GetFocus() == f.tree && GetCapture() == f.bar,
        "normal bar click retains its exact State, native tree focus and owned capture");
    const int y = (f.metrics.thumb.top + f.metrics.thumb.bottom) / 2 + std::max(1, f.metrics.travel / 2);
    SendMessageW(f.bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(f.metrics.bounds.right / 2, y));
    Check(TreeView_GetFirstVisible(f.tree) != before && GetCapture() == f.bar,
        "normal production thumb drag scrolls the actual native tree");
    SendMessageW(f.bar, WM_LBUTTONUP, 0, 0);
    Check(GetCapture() == nullptr && IsWindow(f.tree) && IsWindow(f.bar),
        "normal thumb release clears capture and leaves both controls usable");
    Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc,
        curve_editor_channel_scroll::kSubclass, &current) && current == f.state,
        "the normal scrollbar preserves its original helper identity after dragging");
    Check(lifetime_storage::allocations==allocations,"ordinary paired scrollbar callback lifetime pins allocate no C++ storage");
}

void RetiredSegments() {
    Segments f;
    if (!f.combo || !f.state || !f.FocusPrevious()) return;
    DWORD_PTR current = 0;
    if (!Check(GetWindowSubclass(f.combo, curve_editor_segments::Proc, 0x5345474d, &current) && current == f.state,
        "quarantine the exact still-attached segmented State before native focus reentry")) return;
    const auto point = f.Point(1);
    lifetime_storage::Begin(reinterpret_cast<void*>(f.state), sizeof(curve_editor_segments::State));
    f.SendMouse(f.combo, point, true);
    f.CheckDestroyed(f.combo, f.combo, "segments");
    lifetime_storage::Release();
}

void RetiredScroll() {
    Scroll f;
    if (!f.CanDrag() || !f.FocusPrevious()) return;
    DWORD_PTR current = 0;
    if (!Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc,
        curve_editor_channel_scroll::kSubclass, &current) && current == f.state,
        "quarantine the exact still-attached scrollbar State before native focus reentry")) return;
    const auto point = f.ThumbPoint();
    lifetime_storage::Begin(reinterpret_cast<void*>(f.state), sizeof(curve_editor_channel_scroll::State));
    f.SendMouse(f.bar, point, true);
    f.CheckDestroyed(f.bar, f.tree, "channel-scroll");
    lifetime_storage::Release();
}

void RetiredAfterCapture() {
    Scroll f;if(!f.CanDrag()||!f.FocusPrevious())return;
    f.killDestroys=false;SetCapture(f.previousFocus);
    if(!Check(GetCapture()==f.previousFocus,"establish real capture that the helper must synchronously replace"))return;
    const auto point=f.ThumbPoint();
    lifetime_storage::Begin(reinterpret_cast<void*>(f.state),sizeof(curve_editor_channel_scroll::State));
    f.SendMouse(f.bar,point,true);
    Check(f.captureChanges==1&&f.requestedCapture==f.bar&&f.killInsideOuter&&f.destroyReturned&&
        f.destroyedBeforeKillReturn&&!IsWindow(f.host)&&!IsWindow(f.bar)&&f.destroys==1&&f.nonclientDestroys==1,
        "the helper's actual SetCapture synchronously destroys its own host through the previous capturer");
    f.CheckStorage(f.bar,"scroll-capture-return");lifetime_storage::Release();
}

void RedirectedCaptureKeepsStateAlive() {
    Scroll f;if(!f.CanDrag()||!f.FocusPrevious())return;
    const HWND third=CreateWindowExW(0,L"EDIT",L"third capture owner",WS_CHILD|WS_VISIBLE,
        200,0,180,24,f.root,reinterpret_cast<HMENU>(INT_PTR(412)),GetModuleHandleW(nullptr),nullptr);
    if(!Check(third&&IsWindow(third)&&IsChild(f.root,third),"create an independent fixture-owned third capture destination"))return;
    f.killDestroys=false;f.captureRedirect=third;
    SetCapture(f.previousFocus);
    if(!Check(GetCapture()==f.previousFocus,"establish actual capture on the former capturer before the thumb gesture"))return;
    const auto before=TreeView_GetFirstVisible(f.tree);
    const auto point=f.ThumbPoint();
    f.SendMouse(f.bar,point,true);
    DWORD_PTR current=0;
    if(!Check(GetWindowSubclass(f.bar,curve_editor_channel_scroll::BarProc,curve_editor_channel_scroll::kSubclass,&current)&&
        current==f.state&&IsWindow(f.host)&&IsWindow(f.tree)&&IsWindow(f.bar)&&!f.destroys&&!f.nonclientDestroys,
        "capture redirection preserves all native windows and the exact original helper State"))return;
    auto& state=*reinterpret_cast<curve_editor_channel_scroll::State*>(current);
    Check(f.captureChanges==1&&f.requestedCapture==f.bar&&f.killInsideOuter&&f.captureRedirected&&GetCapture()==third,
        "the former capturer's real WM_CAPTURECHANGED synchronously redirects capture from the bar to the third window");
    std::printf("OBSERVE helper=scroll-redirected-capture capture=%p third=%p dragging=%u repeat=%d\n",
        static_cast<void*>(GetCapture()),static_cast<void*>(third),state.dragging?1u:0u,state.repeat);
    Check(!state.dragging&&state.repeat==-1,
        "the outer click must preserve nested capture cancellation instead of starting a captureless ghost drag");
    const int y=(f.metrics.thumb.top+f.metrics.thumb.bottom)/2+std::max(1,f.metrics.travel/2);
    SendMessageW(f.bar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(f.metrics.bounds.right/2,y));
    Check(TreeView_GetFirstVisible(f.tree)==before&&GetCapture()==third&&!state.dragging&&state.repeat==-1,
        "local mouse movement after redirected capture cannot scroll the live tree or revive cancelled dragging");

    // A genuinely new gesture is still admitted after explicit release. Do
    // not cancel the old bar manually or replace its State to make it usable.
    f.captureRedirect=nullptr;f.armed=false;
    if(GetCapture()==third)ReleaseCapture();
    Check(GetCapture()==nullptr,"explicitly release only the fixture's third-window capture before a fresh gesture");
    if(!f.FocusPrevious())return;
    f.metrics=curve_editor_channel_scroll::Measure(f.bar,state);
    const auto freshBefore=TreeView_GetFirstVisible(f.tree);
    f.SendMouse(f.bar,f.ThumbPoint(),false);
    Check(GetCapture()==f.bar&&state.dragging&&state.repeat==-1&&GetFocus()==f.tree,
        "a fresh real thumb gesture can acquire capture and begin dragging on the same surviving State");
    const int delta=std::max(1,f.metrics.travel/2)*(f.metrics.info.nPos>f.metrics.info.nMin?-1:1);
    const int freshY=(f.metrics.thumb.top+f.metrics.thumb.bottom)/2+delta;
    SendMessageW(f.bar,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(f.metrics.bounds.right/2,freshY));
    Check(TreeView_GetFirstVisible(f.tree)!=freshBefore&&GetCapture()==f.bar,
        "fresh captured thumb movement scrolls the actual tree after the earlier redirection");
    SendMessageW(f.bar,WM_LBUTTONUP,0,0);
    Check(!GetCapture()&&!state.dragging&&state.repeat==-1&&
        GetWindowSubclass(f.bar,curve_editor_channel_scroll::BarProc,curve_editor_channel_scroll::kSubclass,&current)&&current==f.state,
        "fresh thumb release cancels interaction normally and preserves the original helper identity");
}

void SegmentedNativeBoundaries() {
    for(UINT message:{UINT(WM_SIZE),UINT(CB_GETCOUNT)}){
        NativeBoundary boundary;Segments f(&boundary);
        if(!f.combo||!f.state||!f.FocusPrevious())continue;
        const auto point=f.Point(1);
        boundary.trigger=message;boundary.armed=true;
        lifetime_storage::Begin(reinterpret_cast<void*>(f.state),sizeof(curve_editor_segments::State));
        if(message==WM_SIZE)f.SendNative(f.combo,WM_SIZE,0,MAKELPARAM(280,24));
        else f.SendMouse(f.combo,point,false);
        Check(boundary.fired==1&&boundary.actionDone&&!boundary.exception&&f.destroyReturned&&
            !IsWindow(f.host)&&!IsWindow(f.combo)&&f.destroys==1&&f.nonclientDestroys==1,
            "the actual lower native procedure destroys the host before the segmented continuation resumes");
        f.CheckStorage(f.combo,message==WM_SIZE?"segments-default-return":"segments-send-return");
        lifetime_storage::Release();
    }
    NativeBoundary boundary;Segments f(&boundary);
    if(!f.combo||!f.state)return;
    boundary.action=NativeBoundary::Action::ReplaceSegments;boundary.trigger=CB_GETCURSEL;boundary.armed=true;
    lifetime_storage::Begin(reinterpret_cast<void*>(f.state),sizeof(curve_editor_segments::State));
    f.SendNative(f.combo,WM_KEYDOWN,VK_RIGHT,0);
    Check(boundary.fired==1&&boundary.actionDone&&!boundary.exception&&boundary.replacement!=f.state&&
        !boundary.obsoleteMessages&&IsWindow(f.host)&&IsWindow(f.combo)&&!f.notifications,
        "a same-HWND replacement State receives no obsolete count, selection or notification after the native read");
    f.CheckStorage(f.combo,"segments-replaced-state");lifetime_storage::Release();
    DWORD_PTR current=0;
    if(Check(GetWindowSubclass(f.combo,curve_editor_segments::Proc,curve_editor_segments::kSubclass,&current)&&
        current==boundary.replacement,"the replacement segmented identity remains attached")){
        Check(reinterpret_cast<curve_editor_segments::State*>(current)->focus==-1&&
            SendMessageW(f.combo,CB_GETCURSEL,0,0)==0,"old keyboard continuation cannot mutate the replacement's focus or selection");
        SendMessageW(f.combo,WM_KEYDOWN,VK_RIGHT,0);
        Check(SendMessageW(f.combo,CB_GETCURSEL,0,0)==1&&f.notifications==1,
            "the surviving replacement handles a fresh keyboard action normally");
    }
}

void ScrollNativeBoundaries() {
    for(bool replace:{false,true}){
        NativeBoundary boundary;Scroll f(&boundary);if(!f.CanDrag())continue;
        const auto oldBar=f.bar;
        boundary.action=replace?NativeBoundary::Action::ReplaceScroll:NativeBoundary::Action::Tree;
        boundary.trigger=TVM_SETITEMHEIGHT;boundary.armed=true;
        lifetime_storage::Begin(reinterpret_cast<void*>(f.state),sizeof(curve_editor_channel_scroll::State));
        f.SendNative(f.tree,TVM_SETITEMHEIGHT,19,0);
        Check(boundary.fired==1&&boundary.actionDone&&!boundary.exception&&IsWindow(f.host)&&
            !f.destroys&&!f.nonclientDestroys&&!IsWindow(oldBar),
            "tree default processing retires the exact old paired bar without destroying its host");
        f.CheckStorage(oldBar,replace?"tree-replaced-state":"tree-default-return");lifetime_storage::Release();
        if(replace){
            DWORD_PTR current=0;
            if(Check(GetWindowSubclass(f.tree,curve_editor_channel_scroll::TreeProc,curve_editor_channel_scroll::kSubclass,&current)&&
                current==boundary.replacement&&current!=f.state,"the replacement tree/bar State keeps its exact identity")){
                const auto& state=*reinterpret_cast<curve_editor_channel_scroll::State*>(current);
                RECT rect{};GetWindowRect(state.bar,&rect);MapWindowPoints(nullptr,f.host,reinterpret_cast<POINT*>(&rect),2);
                Check(IsWindow(f.tree)&&IsWindow(state.bar)&&rect.left==300&&rect.top==100&&
                    rect.right-rect.left==13&&rect.bottom-rect.top==70,
                    "old TreeProc cannot run Layout against the replacement bar after its native continuation returns");
            }
        }else Check(!IsWindow(f.tree),"destroying only the native tree also closes its paired helper bar");
    }
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // Standard EDIT/COMBOBOX classes already exist. ICC_STANDARD_CLASSES
    // requires a v6 activation context; this standalone fixture has none.
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES};
    if (!Check(InitCommonControlsEx(&common) != FALSE, "initialize actual native common controls")) return 1;
    GdiPlus gdi;
    if (!gdi.token) return 1;
    try {
        std::puts("CASE normal segments"); NormalSegments();
        std::puts("CASE normal scroll"); NormalScroll();
        std::puts("CASE retired segments after real kill-focus"); RetiredSegments();
        std::puts("CASE retired scroll after real kill-focus"); RetiredScroll();
        std::puts("CASE retired scroll after real capture transfer"); RetiredAfterCapture();
        std::puts("CASE capture redirected while all native windows and State remain alive"); RedirectedCaptureKeepsStateAlive();
        std::puts("CASE segmented default/read boundaries and exact State replacement"); SegmentedNativeBoundaries();
        std::puts("CASE tree default boundaries and exact paired State replacement"); ScrollNativeBoundaries();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "ERROR %s\n", error.what());
        lifetime_storage::Release();
    } catch (...) {
        ++failures;
        std::fputs("ERROR non-standard exception\n", stderr);
        lifetime_storage::Release();
    }
    std::printf("%s curve native control lifetime: checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
