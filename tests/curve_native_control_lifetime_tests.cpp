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
namespace repeat_timer_calls {
thread_local HWND watched = nullptr;
thread_local unsigned calls = 0, kills = 0;
thread_local UINT interval = 0;
thread_local UINT_PTR requestedId = 0, killedId = 0, result = 0;
thread_local std::array<UINT_PTR, 64> owned{};
thread_local std::size_t ownedCount = 0;
// Observe all this translation unit's IDs on the exact owned bar. The Win32
// call retains its original delay and return value; no global hook is used.
UINT_PTR WINAPI Forward(HWND window, UINT_PTR id, UINT delay, TIMERPROC callback) {
    const auto timer = ::SetTimer(window, id, delay, callback);
    if (window == watched) {
        ++calls; interval = delay; requestedId = id; result = timer;
        const auto end = owned.begin() + ownedCount;
        if (std::find(owned.begin(), end, id) == end && ownedCount < owned.size()) owned[ownedCount++] = id;
    }
    return timer;
}
BOOL WINAPI ForwardKill(HWND window, UINT_PTR id) {
    const auto removed = ::KillTimer(window, id);
    if (window == watched) { ++kills; killedId = id; }
    return removed;
}
}
#define SetTimer repeat_timer_calls::Forward
#define KillTimer repeat_timer_calls::ForwardKill
#include "../src/window/curve_editor_channel_scroll.hpp"
#undef KillTimer
#undef SetTimer

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
LPARAM RepeatPoint(const curve_editor_channel_scroll::Metrics&, bool);
struct NativeBoundary {
    enum class Action { Host, Tree, ReplaceSegments, ReplaceScroll, RedirectCapture, RestartRepeat } action = Action::Host;
    Windows* fixture = nullptr; HWND window = nullptr; UINT trigger = 0;
    HWND captureTarget = nullptr, captureBefore = nullptr, captureAfter = nullptr;
    bool armed = false, replaced = false, actionDone = false, exception = false;
    unsigned fired = 0, obsoleteMessages = 0, scrolls = 0, timerCallsAtCancellation = 0; DWORD_PTR replacement = 0;
    HWND restartBar = nullptr, captureAfterRestart = nullptr; LPARAM restartPoint = 0; bool restartPage = false;
    DWORD_PTR restartState = 0; unsigned timerCallsAfterRestart = 0, timerKillsAfterRestart = 0;
    UINT intervalAfterRestart = 0; UINT_PTR timerAfterRestart = 0, requestAfterRestart = 0;
    static LRESULT CALLBACK Observe(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
        auto& self=*reinterpret_cast<NativeBoundary*>(data);
        if(message==WM_VSCROLL)++self.scrolls;
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
                }else if(self.action==Action::RedirectCapture||self.action==Action::RestartRepeat){
                    self.captureBefore=GetCapture();SetCapture(self.captureTarget);
                    self.captureAfter=GetCapture();self.actionDone=self.captureTarget&&self.captureAfter==self.captureTarget;
                    self.timerCallsAtCancellation=repeat_timer_calls::calls;
                    if(self.action==Action::RestartRepeat&&self.actionDone){
                        DWORD_PTR current=0;
                        if(GetWindowSubclass(self.restartBar,curve_editor_channel_scroll::BarProc,curve_editor_channel_scroll::kSubclass,&current)&&current){
                            const auto metrics=curve_editor_channel_scroll::Measure(self.restartBar,*reinterpret_cast<curve_editor_channel_scroll::State*>(current));
                            self.restartPoint=RepeatPoint(metrics,self.restartPage);
                            SendMessageW(self.restartBar,WM_LBUTTONDOWN,MK_LBUTTON,self.restartPoint);
                        }
                        self.captureAfterRestart=GetCapture();
                        GetWindowSubclass(self.restartBar,curve_editor_channel_scroll::BarProc,curve_editor_channel_scroll::kSubclass,&self.restartState);
                        self.timerCallsAfterRestart=repeat_timer_calls::calls;self.timerKillsAfterRestart=repeat_timer_calls::kills;
                        self.intervalAfterRestart=repeat_timer_calls::interval;self.timerAfterRestart=repeat_timer_calls::result;
                        self.requestAfterRestart=repeat_timer_calls::requestedId;
                    }
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

struct RepeatTimerWatch {
    HWND bar;
    explicit RepeatTimerWatch(HWND window) : bar(window) {
        repeat_timer_calls::watched = bar; repeat_timer_calls::ownedCount = 0; Reset();
    }
    void Reset() const noexcept {
        repeat_timer_calls::calls = repeat_timer_calls::kills = 0; repeat_timer_calls::interval = 0; repeat_timer_calls::result = 0;
        repeat_timer_calls::requestedId = repeat_timer_calls::killedId = 0;
    }
    ~RepeatTimerWatch() {
        repeat_timer_calls::watched = nullptr;
        if (IsWindow(bar)) for (std::size_t i = 0; i < repeat_timer_calls::ownedCount; ++i) KillTimer(bar, repeat_timer_calls::owned[i]);
    }
};
struct CaptureEvents {
    const HWND root, host, bar, third;
    const DWORD start = GetTickCount();
    const char* phase = "setup";
    unsigned barCaptureChanges = 0;
    explicit CaptureEvents(const Scroll& f, HWND destination) : root(f.root), host(f.host), bar(f.bar), third(destination) {
        for (const auto window : {root, host, bar, third})
            Check(SetWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)) != FALSE,
                "observe capture and cancellation only on this fixture's own root, host, bar and third window");
    }
    const char* Role(HWND window) const noexcept {
        return window == root ? "root" : window == host ? "host" : window == bar ? "bar" : "third";
    }
    static const char* Name(UINT message) noexcept {
        switch (message) {
        case WM_CAPTURECHANGED: return "WM_CAPTURECHANGED";
        case WM_CANCELMODE: return "WM_CANCELMODE";
        case WM_KILLFOCUS: return "WM_KILLFOCUS";
        case WM_SETFOCUS: return "WM_SETFOCUS";
        case WM_ACTIVATE: return "WM_ACTIVATE";
        case WM_ACTIVATEAPP: return "WM_ACTIVATEAPP";
        case WM_DESTROY: return "WM_DESTROY";
        case WM_NCDESTROY: return "WM_NCDESTROY";
        default: return nullptr;
        }
    }
    void Log(HWND window, UINT message, WPARAM w, LPARAM l, const char* edge) const {
        if (const auto name = Name(message))
            std::printf("CAPTURE_EVENT phase=%s elapsed_ms=%lu role=%s window=%p edge=%s message=%s w=0x%llx l=0x%llx capture=%p focus=%p\n",
                phase, static_cast<unsigned long>(GetTickCount() - start), Role(window), static_cast<void*>(window), edge, name,
                static_cast<unsigned long long>(w), static_cast<unsigned long long>(l), static_cast<void*>(GetCapture()), static_cast<void*>(GetFocus()));
    }
    static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto& self = *reinterpret_cast<CaptureEvents*>(data);
        if (window == self.bar && message == WM_CAPTURECHANGED) ++self.barCaptureChanges;
        self.Log(window, message, w, l, "enter");
        const auto result = DefSubclassProc(window, message, w, l);
        self.Log(window, message, w, l, "leave");
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Observe, id);
        return result;
    }
    void Mark(const char* next) {
        phase = next;
        std::printf("CAPTURE_PHASE phase=%s elapsed_ms=%lu capture=%p focus=%p root=%p bar=%p third=%p\n", phase,
            static_cast<unsigned long>(GetTickCount() - start), static_cast<void*>(GetCapture()), static_cast<void*>(GetFocus()),
            static_cast<void*>(root), static_cast<void*>(bar), static_cast<void*>(third));
    }
    ~CaptureEvents() {
        for (const auto window : {root, host, bar, third})
            if (curve_editor_control_lifetime::Exact(window, Observe, reinterpret_cast<UINT_PTR>(this), this))
                RemoveWindowSubclass(window, Observe, reinterpret_cast<UINT_PTR>(this));
    }
};
unsigned PumpOwnBarTimers(HWND bar) {
    unsigned delivered = 0; const auto start = GetTickCount();
    while (GetTickCount() - start < 250) {
        MSG message{};
        if (PeekMessageW(&message, bar, WM_TIMER, WM_TIMER, PM_REMOVE)) {
            ++delivered; DispatchMessageW(&message);
        } else Sleep(1);
    }
    return delivered;
}
LPARAM RepeatPoint(const curve_editor_channel_scroll::Metrics& metrics, bool page) {
    const int bottom = metrics.bounds.bottom - metrics.arrow;
    return MAKELPARAM(metrics.bounds.right / 2,
        page ? (metrics.thumb.bottom + bottom) / 2 : metrics.bounds.bottom - std::max(1, metrics.arrow / 2));
}
void RepeatCancelledInsideNativeStep() {
    for (bool page : {false, true}) {
        NativeBoundary boundary; Scroll f(&boundary);
        if (!f.CanDrag() || !f.FocusPrevious()) continue;
        const HWND third = CreateWindowExW(0, L"EDIT", L"repeat capture owner", WS_CHILD | WS_VISIBLE,
            200, 0, 180, 24, f.root, reinterpret_cast<HMENU>(INT_PTR(413)), GetModuleHandleW(nullptr), nullptr);
        if (!Check(third && IsChild(f.root, third), "create the repeat step's independent fixture-owned capture target")) continue;
        if (!Check(!page || f.metrics.thumb.bottom < f.metrics.bounds.bottom - f.metrics.arrow,
            "the real overflowing bar has a page-down area outside its thumb and arrow")) continue;
        boundary.action = NativeBoundary::Action::RedirectCapture; boundary.captureTarget = third;
        boundary.trigger = WM_VSCROLL; boundary.armed = true;
        CaptureEvents captureEvents(f, third);
        const auto before = TreeView_GetFirstVisible(f.tree); RepeatTimerWatch timers(f.bar);
        captureEvents.Mark("native-arrow-page-step");
        f.SendMouse(f.bar, RepeatPoint(f.metrics, page), false);
        const auto captureAtReturn = GetCapture();
        captureEvents.Mark("outer-send-return");
        DWORD_PTR current = 0;
        if (!Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc, curve_editor_channel_scroll::kSubclass, &current) &&
            current == f.state && curve_editor_channel_scroll::ForTree(f.tree) == reinterpret_cast<curve_editor_channel_scroll::State*>(current) &&
            IsWindow(f.host) && IsWindow(f.tree) && IsWindow(f.bar) && IsWindow(third) && !f.destroys && !f.nonclientDestroys,
            "native repeat capture cancellation preserves the tree, all owned windows and the original paired State")) continue;
        auto& state = *reinterpret_cast<curve_editor_channel_scroll::State*>(current);
        Check(boundary.fired == 1 && boundary.actionDone && !boundary.exception && boundary.captureBefore == f.bar && boundary.captureAfter == third &&
            boundary.scrolls == 1 && captureAtReturn == third && !state.dragging && state.repeat == -1 && TreeView_GetFirstVisible(f.tree) != before,
            "one real arrow/page tree scroll transfers capture and synchronously cancels the repeat without destroying anything");
        const auto after = TreeView_GetFirstVisible(f.tree); const auto scrolls = boundary.scrolls;
        captureEvents.Mark("observe-own-repeat-timers");
        const auto delivered = PumpOwnBarTimers(f.bar);
        const auto captureAfterPump = GetCapture(); captureEvents.Mark("own-timer-observation-ended");
        std::printf("OBSERVE helper=scroll-repeat-cancel page=%u timer_calls=%u last_interval=%u timer_result=%llu delivered=%u native_scrolls=%u repeat=%d callback_before=%p callback_after=%p capture_at_send_return=%p capture=%p third=%p\n",
            page ? 1u : 0u, repeat_timer_calls::calls, repeat_timer_calls::interval, static_cast<unsigned long long>(repeat_timer_calls::result),
            delivered, boundary.scrolls, state.repeat, static_cast<void*>(boundary.captureBefore), static_cast<void*>(boundary.captureAfter),
            static_cast<void*>(captureAtReturn), static_cast<void*>(captureAfterPump), static_cast<void*>(third));
        // Transfer is asserted synchronously above. During the wait a later
        // native cancellation may release third; the helper must never regain
        // capture, restart its timer or scroll in response to that release.
        Check(repeat_timer_calls::calls == 0 && delivered == 0 && boundary.scrolls == scrolls && TreeView_GetFirstVisible(f.tree) == after,
            "returning from a cancelled native repeat step never starts even the 400-ms timer or scrolls again");
        Check(captureAfterPump != f.bar, "the cancelled repeat bar never reacquires capture during native timer observation");
        Check(!state.dragging && state.repeat == -1 && curve_editor_channel_scroll::Current(state),
            "the same surviving helper remains fully cancelled after native timer observation");

        // Keep the original State and use a genuinely new captured gesture.
        captureEvents.Mark("fresh-owned-gesture");
        if (GetCapture() == third) ReleaseCapture();
        if (!f.FocusPrevious()) continue;
        f.metrics = curve_editor_channel_scroll::Measure(f.bar, state);
        const auto freshBefore = TreeView_GetFirstVisible(f.tree);
        f.SendMouse(f.bar, f.ThumbPoint(), false);
        Check(GetCapture() == f.bar && state.dragging && state.repeat == -1,
            "the surviving cancelled-repeat State admits a fresh real thumb gesture");
        const int delta = std::max(1, f.metrics.travel / 2) * (f.metrics.info.nPos > f.metrics.info.nMin ? -1 : 1);
        SendMessageW(f.bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(f.metrics.bounds.right / 2,
            (f.metrics.thumb.top + f.metrics.thumb.bottom) / 2 + delta));
        Check(GetCapture() == f.bar && TreeView_GetFirstVisible(f.tree) != freshBefore,
            "a new thumb movement scrolls the actual tree after repeat capture cancellation");
        SendMessageW(f.bar, WM_LBUTTONUP, 0, 0);
        Check(!GetCapture() && !state.dragging && state.repeat == -1 &&
            GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc, curve_editor_channel_scroll::kSubclass, &current) && current == f.state,
            "the fresh gesture releases capture normally and retains the same usable helper identity");
    }
}
void QueuedRepeatAfterCancellation() {
    for (bool page : {false, true}) {
        NativeBoundary boundary; Scroll f(&boundary);
        if (!f.CanDrag() || !f.FocusPrevious()) continue;
        if (!Check(!page || f.metrics.thumb.bottom < f.metrics.bounds.bottom - f.metrics.arrow,
            "the queued-repeat fixture has a real page-down hit region")) continue;
        RepeatTimerWatch timers(f.bar); f.SendMouse(f.bar, RepeatPoint(f.metrics, page), false);
        DWORD_PTR current = 0;
        if (!Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc, curve_editor_channel_scroll::kSubclass, &current) && current == f.state,
            "the normal repeat uses the actual original bar State before cancellation")) continue;
        auto& state = *reinterpret_cast<curve_editor_channel_scroll::State*>(current);
        Check(GetCapture() == f.bar && !state.dragging && state.repeat == (page ? SB_PAGEDOWN : SB_LINEDOWN) && boundary.scrolls == 1 &&
            repeat_timer_calls::calls == 1 && repeat_timer_calls::interval == 400 && repeat_timer_calls::result != 0,
            "a normal real arrow/page step arms its unmodified 400-ms native repeat timer");
        f.SendNative(f.bar, WM_CANCELMODE);
        Check(!GetCapture() && !state.dragging && state.repeat == -1,
            "normal cancellation clears capture and repeat before the already-queued timer arrives");
        const auto oldTimerId = repeat_timer_calls::requestedId;
        const auto before = TreeView_GetFirstVisible(f.tree); const auto scrolls = boundary.scrolls; timers.Reset();
        Check(oldTimerId && PostMessageW(f.bar, WM_TIMER, oldTimerId, 0) != FALSE, "post one old timer message only to the fixture's own cancelled bar");
        MSG queued{};
        if (!Check(PeekMessageW(&queued, f.bar, WM_TIMER, WM_TIMER, PM_REMOVE) && queued.hwnd == f.bar && queued.message == WM_TIMER &&
            queued.wParam == oldTimerId && queued.lParam == 0, "remove and identify the exact local queued timer before observing any subsequent native timer")) continue;
        DispatchMessageW(&queued);
        const auto delivered = PumpOwnBarTimers(f.bar);
        std::printf("OBSERVE helper=scroll-stale-repeat page=%u timer_calls=%u last_interval=%u timer_result=%llu delivered_after_post=%u native_scrolls=%u repeat=%d capture=%p\n",
            page ? 1u : 0u, repeat_timer_calls::calls, repeat_timer_calls::interval, static_cast<unsigned long long>(repeat_timer_calls::result),
            delivered, boundary.scrolls, state.repeat, static_cast<void*>(GetCapture()));
        Check(repeat_timer_calls::calls == 0 && delivered == 0 && boundary.scrolls == scrolls && TreeView_GetFirstVisible(f.tree) == before &&
            !GetCapture() && !state.dragging && state.repeat == -1 && curve_editor_channel_scroll::Current(state),
            "one stale queued timer cannot resurrect the 60-ms timer, repeat interaction or native tree scrolling after cancellation");
    }
}
void RepeatCancelledInsideTimerStep() {
    NativeBoundary boundary; Scroll f(&boundary);
    if (!f.CanDrag() || !f.FocusPrevious()) return;
    const HWND third = CreateWindowExW(0, L"EDIT", L"timer-step capture owner", WS_CHILD | WS_VISIBLE,
        200, 0, 180, 24, f.root, reinterpret_cast<HMENU>(INT_PTR(414)), GetModuleHandleW(nullptr), nullptr);
    if (!Check(third && IsChild(f.root, third), "create an owned capture target for cancellation inside the repeat timer's native scroll")) return;
    CaptureEvents captureEvents(f, third);
    RepeatTimerWatch timers(f.bar); f.SendMouse(f.bar, RepeatPoint(f.metrics, false), false);
    DWORD_PTR current = 0;
    if (!Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc, curve_editor_channel_scroll::kSubclass, &current) &&
        current == f.state && GetCapture() == f.bar && boundary.scrolls == 1 && repeat_timer_calls::result != 0,
        "begin one real captured arrow repeat before testing its later timer callback")) return;
    auto& state = *reinterpret_cast<curve_editor_channel_scroll::State*>(current);
    // Only move/resize this hidden child. Do not move the desktop cursor or
    // inject input. A broad arrow area keeps the actual cursor lookup away
    // from a boundary while the production WM_TIMER hit-test runs unchanged.
    POINT cursor{};
    if (!Check(!IsWindowVisible(f.root) && GetCursorPos(&cursor) && ScreenToClient(f.host, &cursor) &&
        SetWindowPos(f.bar, nullptr, cursor.x - 2048, cursor.y - 3072, 4096, 4096, SWP_NOZORDER | SWP_NOACTIVATE),
        "place only the fixture's hidden bar around the current cursor without global input or a visible window")) return;
    POINT hit{}; const auto metrics = curve_editor_channel_scroll::Measure(f.bar, state);
    if (!Check(GetCursorPos(&hit) && ScreenToClient(f.bar, &hit) && PtInRect(&metrics.bounds, hit) &&
        hit.y >= metrics.bounds.bottom - metrics.arrow && state.repeat == SB_LINEDOWN,
        "the unmodified timer hit-test genuinely addresses the active lower arrow")) return;
    const auto activeTimerId = repeat_timer_calls::requestedId;
    const auto before = TreeView_GetFirstVisible(f.tree); const auto scrolls = boundary.scrolls; timers.Reset();
    boundary.action = NativeBoundary::Action::RedirectCapture; boundary.captureTarget = third; boundary.trigger = WM_VSCROLL; boundary.armed = true;
    Check(activeTimerId && PostMessageW(f.bar, WM_TIMER, activeTimerId, 0) != FALSE, "post an active repeat tick only to this fixture's captured bar");
    MSG tick{};
    if (!Check(PeekMessageW(&tick, f.bar, WM_TIMER, WM_TIMER, PM_REMOVE) && tick.hwnd == f.bar && tick.wParam == activeTimerId,
        "dispatch the exact local active-repeat tick through the production native procedure")) return;
    captureEvents.Mark("native-timer-step");
    DispatchMessageW(&tick);
    const auto captureAtReturn = GetCapture(); captureEvents.Mark("outer-timer-dispatch-return");
    Check(boundary.fired == 1 && boundary.actionDone && !boundary.exception && boundary.captureBefore == f.bar && boundary.captureAfter == third &&
        captureAtReturn == third && state.repeat == -1 && !state.dragging && boundary.scrolls == scrolls + 1 && TreeView_GetFirstVisible(f.tree) != before,
        "the timer's actual native Step scrolls once and then synchronously transfers capture to cancel repeat");
    const auto after = TreeView_GetFirstVisible(f.tree); captureEvents.Mark("observe-own-repeat-timers");
    const auto delivered = PumpOwnBarTimers(f.bar);
    const auto captureAfterPump = GetCapture(); captureEvents.Mark("own-timer-observation-ended");
    std::printf("OBSERVE helper=scroll-cancel-inside-timer calls_at_cancel=%u timer_calls=%u delivered=%u native_scrolls=%u repeat=%d callback_before=%p callback_after=%p capture_at_dispatch_return=%p capture=%p third=%p\n",
        boundary.timerCallsAtCancellation, repeat_timer_calls::calls, delivered, boundary.scrolls, state.repeat,
        static_cast<void*>(boundary.captureBefore), static_cast<void*>(boundary.captureAfter), static_cast<void*>(captureAtReturn),
        static_cast<void*>(captureAfterPump), static_cast<void*>(third));
    Check(repeat_timer_calls::calls == boundary.timerCallsAtCancellation && delivered == 0 && boundary.scrolls == scrolls + 1 &&
        TreeView_GetFirstVisible(f.tree) == after,
        "returning from native cancellation inside a timer Step never rearms a timer or repeats the scroll");
    Check(captureAfterPump != f.bar, "the bar cancelled inside its native timer Step never takes capture back during observation");
    Check(state.repeat == -1 && !state.dragging && curve_editor_channel_scroll::Current(state),
        "native cancellation inside a timer Step leaves the same live State fully cancelled");
}
bool PlaceCursorOverHiddenRepeatArrow(Scroll& f, curve_editor_channel_scroll::State& state) {
    POINT cursor{};
    if (!Check(!IsWindowVisible(f.root) && GetCursorPos(&cursor) && ScreenToClient(f.host, &cursor) &&
        SetWindowPos(f.bar, nullptr, cursor.x - 256, cursor.y - 3840, 512, 4096, SWP_NOZORDER | SWP_NOACTIVATE),
        "position only the owned hidden bar so both a broad arrow and a separate page region remain real native hit targets")) return false;
    f.metrics = curve_editor_channel_scroll::Measure(f.bar, state); POINT hit{};
    return Check(GetCursorPos(&hit) && ScreenToClient(f.bar, &hit) && PtInRect(&f.metrics.bounds, hit) &&
        hit.y >= f.metrics.bounds.bottom - f.metrics.arrow && f.metrics.thumb.bottom < f.metrics.bounds.bottom - f.metrics.arrow,
        "the unchanged real timer cursor hit-test selects the lower arrow while nested page-down remains available");
}
bool DispatchOwnRepeatTick(HWND bar, UINT_PTR timerId) {
    if (!Check(timerId && PostMessageW(bar, WM_TIMER, timerId, 0) != FALSE, "post a repeat tick to only this fixture's own bar using its recorded real request ID")) return false;
    MSG tick{};
    if (!Check(PeekMessageW(&tick, bar, WM_TIMER, WM_TIMER, PM_REMOVE) && tick.hwnd == bar && tick.message == WM_TIMER && tick.wParam == timerId && !tick.lParam,
        "identify the exact locally posted repeat tick before dispatching its real native continuation")) return false;
    DispatchMessageW(&tick); return true;
}
void InteractionTicketBoundaries() {
    const auto processTicket = curve_editor_channel_scroll::lastInteraction;
    UINT_PTR local = 0;
    Check(curve_editor_channel_scroll::AllocateInteraction(local) == 1 && local == 1,
        "the first isolated interaction ticket is nonzero");
    Check(curve_editor_channel_scroll::AllocateInteraction(local) == 2 && local == 2,
        "isolated interaction tickets increase instead of reusing the prior ID");
    const auto maximum = ~UINT_PTR(0); local = maximum - 1;
    Check(curve_editor_channel_scroll::AllocateInteraction(local) == maximum && local == maximum,
        "the final representable interaction ticket remains a valid native timer request ID");
    for (unsigned retry = 0; retry < 3; ++retry)
        Check(!curve_editor_channel_scroll::AllocateInteraction(local) && local == maximum,
            "exhaustion permanently refuses an isolated ticket without wrapping to an earlier ID");
    Check(curve_editor_channel_scroll::lastInteraction == processTicket,
        "ticket exhaustion coverage does not consume or mutate the actual UI thread's gesture sequence");
}
void SameStateRepeatReplacement() {
    for (bool inTimer : {false, true}) for (bool page : {false, true}) {
        NativeBoundary boundary; Scroll f(&boundary);
        if (!f.CanDrag() || !f.FocusPrevious()) continue;
        const HWND third = CreateWindowExW(0, L"EDIT", L"repeat replacement owner", WS_CHILD | WS_VISIBLE,
            200, 0, 180, 24, f.root, reinterpret_cast<HMENU>(INT_PTR(415)), GetModuleHandleW(nullptr), nullptr);
        if (!Check(third && IsChild(f.root, third), "create a separate owned capture destination for a same-State replacement gesture")) continue;
        auto& state = *reinterpret_cast<curve_editor_channel_scroll::State*>(f.state);
        if (!PlaceCursorOverHiddenRepeatArrow(f, state)) continue;
        RepeatTimerWatch timers(f.bar);
        if (inTimer) {
            f.SendMouse(f.bar, RepeatPoint(f.metrics, false), false);
            if (!Check(GetCapture() == f.bar && state.repeat == SB_LINEDOWN && boundary.scrolls == 1 &&
                repeat_timer_calls::calls == 1 && repeat_timer_calls::interval == 400 && repeat_timer_calls::result != 0,
                "prepare the real captured initial repeat before replacing it inside its next timer Step")) continue;
            if (!PlaceCursorOverHiddenRepeatArrow(f, state)) continue;
        }
        const auto oldTimerId = repeat_timer_calls::requestedId;
        const auto scrolls = boundary.scrolls; const auto before = TreeView_GetFirstVisible(f.tree); timers.Reset();
        boundary.action = NativeBoundary::Action::RestartRepeat; boundary.captureTarget = third; boundary.restartBar = f.bar;
        boundary.restartPage = page; boundary.trigger = WM_VSCROLL; boundary.armed = true;
        if (inTimer) { if (!DispatchOwnRepeatTick(f.bar, oldTimerId)) continue; }
        else f.SendMouse(f.bar, RepeatPoint(f.metrics, false), false);
        const auto captureAtReturn = GetCapture(); DWORD_PTR current = 0;
        if (!Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc, curve_editor_channel_scroll::kSubclass, &current) && current == f.state &&
            curve_editor_channel_scroll::ForTree(f.tree) == reinterpret_cast<curve_editor_channel_scroll::State*>(current),
            "the old and nested gestures retain exactly the same live native State allocation")) continue;
        std::printf("OBSERVE helper=scroll-repeat-replacement outer=%s nested=%s callback=%u scrolls=%u calls_at_cancel=%u calls_after_new=%u calls_after_old=%u kills_after_new=%u kills_after_old=%u interval_after_new=%u interval_after_old=%u timer_after_new=%llu old_request_id=%llu new_request_id=%llu last_kill_id=%llu repeat=%d before_capture=%p cancelled_capture=%p new_capture=%p returned_capture=%p state=%p nested_state=%p\n",
            inTimer ? "timer" : "down", page ? "page" : "arrow", boundary.fired, boundary.scrolls,
            boundary.timerCallsAtCancellation, boundary.timerCallsAfterRestart, repeat_timer_calls::calls,
            boundary.timerKillsAfterRestart, repeat_timer_calls::kills, boundary.intervalAfterRestart, repeat_timer_calls::interval,
            static_cast<unsigned long long>(boundary.timerAfterRestart), static_cast<unsigned long long>(oldTimerId),
            static_cast<unsigned long long>(boundary.requestAfterRestart), static_cast<unsigned long long>(repeat_timer_calls::killedId),
            state.repeat, static_cast<void*>(boundary.captureBefore),
            static_cast<void*>(boundary.captureAfter), static_cast<void*>(boundary.captureAfterRestart), static_cast<void*>(captureAtReturn),
            reinterpret_cast<void*>(f.state), reinterpret_cast<void*>(boundary.restartState));
        Check(boundary.fired == 1 && boundary.actionDone && !boundary.exception && boundary.captureBefore == f.bar && boundary.captureAfter == third &&
            boundary.captureAfterRestart == f.bar && boundary.restartState == f.state && captureAtReturn == f.bar &&
            !state.dragging && state.repeat == (page ? SB_PAGEDOWN : SB_LINEDOWN) && boundary.scrolls == scrolls + 2 && TreeView_GetFirstVisible(f.tree) != before,
            "the real native callback cancels the old gesture and synchronously starts one new arrow/page gesture on the same State");
        Check(boundary.timerCallsAfterRestart == boundary.timerCallsAtCancellation + 1 && boundary.intervalAfterRestart == 400 && boundary.timerAfterRestart != 0 &&
            boundary.requestAfterRestart != 0 && (!inTimer || boundary.requestAfterRestart != oldTimerId),
            "the nested new gesture starts exactly its own successful 400-ms native timer");
        Check(repeat_timer_calls::calls == boundary.timerCallsAfterRestart && repeat_timer_calls::interval == 400 && repeat_timer_calls::result == boundary.timerAfterRestart &&
            repeat_timer_calls::requestedId == boundary.requestAfterRestart,
            "the superseded outer down/timer continuation cannot reset or accelerate the new gesture's 400-ms timer");
        Check(repeat_timer_calls::kills == boundary.timerKillsAfterRestart,
            "the superseded outer continuation cannot kill the new gesture's native timer");
        f.SendNative(f.bar, WM_CANCELMODE);
        Check(GetCapture() != f.bar && state.repeat == -1 && !state.dragging && !state.interaction && !state.timerId &&
            curve_editor_channel_scroll::Current(state),
            "explicitly cancelling only the surviving new gesture leaves its original State usable and releases capture");
    }
}
void NormalRepeatTimerCadence() {
    NativeBoundary boundary; Scroll f(&boundary);
    if (!f.CanDrag() || !f.FocusPrevious()) return;
    auto& state = *reinterpret_cast<curve_editor_channel_scroll::State*>(f.state);
    if (!PlaceCursorOverHiddenRepeatArrow(f, state)) return;
    RepeatTimerWatch timers(f.bar); f.SendMouse(f.bar, RepeatPoint(f.metrics, false), false);
    if (!Check(GetCapture() == f.bar && state.repeat == SB_LINEDOWN && boundary.scrolls == 1 &&
        repeat_timer_calls::calls == 1 && repeat_timer_calls::interval == 400 && repeat_timer_calls::result != 0,
        "normal repeat first starts the unchanged successful 400-ms native timer")) return;
    if (!PlaceCursorOverHiddenRepeatArrow(f, state)) return;
    const auto activeTimerId = repeat_timer_calls::requestedId, ticket = state.interaction;
    Check(ticket && ticket == activeTimerId && state.timerId == activeTimerId,
        "the active gesture owns precisely its recorded native timer request ID");
    const auto before = TreeView_GetFirstVisible(f.tree); timers.Reset();
    if (!DispatchOwnRepeatTick(f.bar, activeTimerId)) return;
    std::printf("OBSERVE helper=scroll-normal-repeat timer_calls=%u kills=%u interval=%u result=%llu request_id=%llu scrolls=%u repeat=%d capture=%p bar=%p\n",
        repeat_timer_calls::calls, repeat_timer_calls::kills, repeat_timer_calls::interval, static_cast<unsigned long long>(repeat_timer_calls::result),
        static_cast<unsigned long long>(repeat_timer_calls::requestedId), boundary.scrolls, state.repeat,
        static_cast<void*>(GetCapture()), static_cast<void*>(f.bar));
    Check(repeat_timer_calls::calls == 1 && repeat_timer_calls::interval == 60 && repeat_timer_calls::result != 0 && !repeat_timer_calls::kills &&
        repeat_timer_calls::requestedId == activeTimerId &&
        boundary.scrolls == 2 && TreeView_GetFirstVisible(f.tree) != before && GetCapture() == f.bar && state.repeat == SB_LINEDOWN &&
        !state.dragging && state.interaction == ticket && state.timerId == activeTimerId && curve_editor_channel_scroll::Current(state),
        "a still-current captured repeat really scrolls once and arms the unchanged 60-ms native timer");

    // Position only this hidden child so the unchanged global cursor lookup
    // falls outside the active lower arrow. A live repeat still checks at
    // 60 ms, then resumes immediately when that same region is re-entered.
    POINT cursor{};
    if (!Check(!IsWindowVisible(f.root) && GetCursorPos(&cursor) && ScreenToClient(f.host, &cursor) &&
        SetWindowPos(f.bar, nullptr, cursor.x - 256, cursor.y - 256, 512, 4096, SWP_NOZORDER | SWP_NOACTIVATE),
        "move only the owned hidden bar to place the actual cursor outside the active repeat arrow")) return;
    const auto outsideMetrics = curve_editor_channel_scroll::Measure(f.bar, state); POINT outside{};
    if (!Check(GetCursorPos(&outside) && ScreenToClient(f.bar, &outside) && PtInRect(&outsideMetrics.bounds, outside) &&
        outside.y < outsideMetrics.bounds.bottom - outsideMetrics.arrow && state.repeat == SB_LINEDOWN,
        "the real native timer hit-test is outside the current lower arrow without ending capture")) return;
    const auto pausedAt = TreeView_GetFirstVisible(f.tree); const auto pausedScrolls = boundary.scrolls; timers.Reset();
    if (!DispatchOwnRepeatTick(f.bar, activeTimerId)) return;
    std::printf("OBSERVE helper=scroll-repeat-outside request_id=%llu calls=%u interval=%u kills=%u scrolls_before=%u scrolls_after=%u ticket=%llu capture=%p\n",
        static_cast<unsigned long long>(repeat_timer_calls::requestedId), repeat_timer_calls::calls, repeat_timer_calls::interval,
        repeat_timer_calls::kills, pausedScrolls, boundary.scrolls, static_cast<unsigned long long>(state.interaction), static_cast<void*>(GetCapture()));
    Check(repeat_timer_calls::calls == 1 && repeat_timer_calls::interval == 60 && repeat_timer_calls::result != 0 &&
        repeat_timer_calls::requestedId == activeTimerId && !repeat_timer_calls::kills && boundary.scrolls == pausedScrolls &&
        TreeView_GetFirstVisible(f.tree) == pausedAt && GetCapture() == f.bar && state.repeat == SB_LINEDOWN &&
        state.interaction == ticket && state.timerId == activeTimerId,
        "outside the active arrow the same captured gesture keeps its 60-ms checks without scrolling or restarting 400 ms");
    if (!PlaceCursorOverHiddenRepeatArrow(f, state)) return;
    timers.Reset();
    if (!DispatchOwnRepeatTick(f.bar, activeTimerId)) return;
    Check(repeat_timer_calls::calls == 1 && repeat_timer_calls::interval == 60 && repeat_timer_calls::result != 0 &&
        repeat_timer_calls::requestedId == activeTimerId && !repeat_timer_calls::kills && boundary.scrolls == pausedScrolls + 1 &&
        TreeView_GetFirstVisible(f.tree) != pausedAt && GetCapture() == f.bar && state.repeat == SB_LINEDOWN &&
        state.interaction == ticket && state.timerId == activeTimerId,
        "re-entering the active arrow resumes one scroll on the same 60-ms timer without a fresh 400-ms wait");
    f.SendNative(f.bar, WM_CANCELMODE);
    Check(GetCapture() != f.bar && state.repeat == -1 && !state.dragging && !state.interaction && !state.timerId &&
        repeat_timer_calls::kills == 1 && repeat_timer_calls::killedId == activeTimerId,
        "normal 60-ms repeat still ends through the real owned cancellation path");
}
void QueuedOldTickAfterNewGesture() {
    for (bool page : {false, true}) {
        NativeBoundary boundary; Scroll f(&boundary);
        if (!f.CanDrag() || !f.FocusPrevious()) continue;
        auto& state = *reinterpret_cast<curve_editor_channel_scroll::State*>(f.state);
        if (!PlaceCursorOverHiddenRepeatArrow(f, state)) continue;
        RepeatTimerWatch timers(f.bar); f.SendMouse(f.bar, RepeatPoint(f.metrics, false), false);
        if (!Check(GetCapture() == f.bar && state.repeat == SB_LINEDOWN && repeat_timer_calls::calls == 1 &&
            repeat_timer_calls::interval == 400 && repeat_timer_calls::result != 0,
            "establish the original captured gesture and its actual native timer before saving a queued tick")) continue;
        const auto oldTimerId = repeat_timer_calls::requestedId;
        Check(oldTimerId && PostMessageW(f.bar, WM_TIMER, oldTimerId, 0) != FALSE, "queue one old gesture timer on only this owned bar");
        MSG old{};
        if (!Check(PeekMessageW(&old, f.bar, WM_TIMER, WM_TIMER, PM_REMOVE) && old.hwnd == f.bar && old.message == WM_TIMER && old.wParam == oldTimerId && !old.lParam,
            "save the old native-request-ID timer message before cancelling, without dispatching it yet")) continue;
        f.SendNative(f.bar, WM_CANCELMODE);
        if (!Check(GetCapture() != f.bar && state.repeat == -1 && !state.dragging,
            "actually cancel the old gesture before any replacement is started")) continue;
        f.metrics = curve_editor_channel_scroll::Measure(f.bar, state);
        f.SendMouse(f.bar, RepeatPoint(f.metrics, page), false);
        DWORD_PTR current = 0;
        if (!Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc, curve_editor_channel_scroll::kSubclass, &current) && current == f.state &&
            GetCapture() == f.bar && state.repeat == (page ? SB_PAGEDOWN : SB_LINEDOWN) && !state.dragging &&
            repeat_timer_calls::interval == 400 && repeat_timer_calls::result != 0 && repeat_timer_calls::requestedId && repeat_timer_calls::requestedId != oldTimerId,
            "a genuinely new arrow/page gesture owns capture and a successful 400-ms timer on the same State")) continue;
        if (!PlaceCursorOverHiddenRepeatArrow(f, state)) continue;
        const auto calls = repeat_timer_calls::calls, kills = repeat_timer_calls::kills, scrolls = boundary.scrolls;
        const auto timer = repeat_timer_calls::result, newTimerId = repeat_timer_calls::requestedId; const auto before = TreeView_GetFirstVisible(f.tree);
        DispatchMessageW(&old);
        std::printf("OBSERVE helper=scroll-old-tick-new-gesture nested=%s old_message_id=%llu new_request_id=%llu last_kill_id=%llu calls_before=%u calls_after=%u kills_before=%u kills_after=%u interval_after=%u scrolls_before=%u scrolls_after=%u repeat=%d capture=%p bar=%p\n",
            page ? "page" : "arrow", static_cast<unsigned long long>(old.wParam), static_cast<unsigned long long>(newTimerId),
            static_cast<unsigned long long>(repeat_timer_calls::killedId), calls, repeat_timer_calls::calls, kills,
            repeat_timer_calls::kills, repeat_timer_calls::interval, scrolls, boundary.scrolls, state.repeat, static_cast<void*>(GetCapture()), static_cast<void*>(f.bar));
        Check(repeat_timer_calls::calls == calls && repeat_timer_calls::kills == kills && repeat_timer_calls::interval == 400 && repeat_timer_calls::result == timer &&
            repeat_timer_calls::requestedId == newTimerId &&
            boundary.scrolls == scrolls && TreeView_GetFirstVisible(f.tree) == before && GetCapture() == f.bar &&
            state.repeat == (page ? SB_PAGEDOWN : SB_LINEDOWN) && !state.dragging && curve_editor_channel_scroll::Current(state),
            "a saved old timer cannot act as a fresh tick, kill or accelerate the replacement gesture's 400-ms timer");
        f.SendNative(f.bar, WM_CANCELMODE);
        Check(GetCapture() != f.bar && state.repeat == -1 && !state.dragging && !state.interaction && !state.timerId &&
            repeat_timer_calls::killedId == newTimerId,
            "the replacement gesture remains normally cancellable after the obsolete timer arrives");
    }
}
void NewDownWhileAlreadyCaptured() {
    for (bool page : {false, true}) {
        NativeBoundary boundary; Scroll f(&boundary);
        if (!f.CanDrag() || !f.FocusPrevious()) continue;
        auto& state = *reinterpret_cast<curve_editor_channel_scroll::State*>(f.state);
        if (!PlaceCursorOverHiddenRepeatArrow(f, state)) continue;
        RepeatTimerWatch timers(f.bar); f.SendMouse(f.bar, RepeatPoint(f.metrics, false), false);
        const auto oldTimerId = repeat_timer_calls::requestedId;
        if (!Check(GetCapture() == f.bar && state.repeat == SB_LINEDOWN && boundary.scrolls == 1 && oldTimerId &&
            state.interaction == oldTimerId && state.timerId == oldTimerId && repeat_timer_calls::interval == 400 && repeat_timer_calls::result,
            "the original gesture already owns the bar's capture and one real 400-ms timer")) continue;
        CaptureEvents captureEvents(f, f.previousFocus);
        if (!Check(PostMessageW(f.bar, WM_TIMER, oldTimerId, 0) != FALSE,
            "queue the existing captured gesture's recorded timer message before starting another down")) continue;
        MSG old{};
        if (!Check(PeekMessageW(&old, f.bar, WM_TIMER, WM_TIMER, PM_REMOVE) && old.hwnd == f.bar && old.message == WM_TIMER &&
            old.wParam == oldTimerId && !old.lParam, "save the exact old timer while the bar still owns capture")) continue;
        f.metrics = curve_editor_channel_scroll::Measure(f.bar, state); timers.Reset();
        captureEvents.Mark("new-down-while-already-captured");
        f.SendMouse(f.bar, RepeatPoint(f.metrics, page), false);
        DWORD_PTR current = 0; const auto newTimerId = repeat_timer_calls::requestedId;
        std::printf("OBSERVE helper=scroll-new-down-already-captured nested=%s old_request_id=%llu new_request_id=%llu killed_id=%llu calls=%u kills=%u capture_changes=%u interval=%u scrolls=%u capture=%p bar=%p\n",
            page ? "page" : "arrow", static_cast<unsigned long long>(oldTimerId), static_cast<unsigned long long>(newTimerId),
            static_cast<unsigned long long>(repeat_timer_calls::killedId), repeat_timer_calls::calls, repeat_timer_calls::kills,
            captureEvents.barCaptureChanges, repeat_timer_calls::interval, boundary.scrolls, static_cast<void*>(GetCapture()), static_cast<void*>(f.bar));
        if (!Check(GetWindowSubclass(f.bar, curve_editor_channel_scroll::BarProc, curve_editor_channel_scroll::kSubclass, &current) && current == f.state &&
            GetCapture() == f.bar && !captureEvents.barCaptureChanges && boundary.scrolls == 2 && !state.dragging &&
            state.repeat == (page ? SB_PAGEDOWN : SB_LINEDOWN) && newTimerId && newTimerId != oldTimerId &&
            state.interaction == newTimerId && state.timerId == newTimerId && repeat_timer_calls::calls == 1 && repeat_timer_calls::interval == 400 &&
            repeat_timer_calls::result && repeat_timer_calls::kills == 1 && repeat_timer_calls::killedId == oldTimerId,
            "a new down on the already-captured bar changes only the gesture ticket and exact timer without releasing capture")) continue;
        const auto before = TreeView_GetFirstVisible(f.tree); const auto calls = repeat_timer_calls::calls, kills = repeat_timer_calls::kills;
        DispatchMessageW(&old);
        Check(repeat_timer_calls::calls == calls && repeat_timer_calls::kills == kills && repeat_timer_calls::requestedId == newTimerId &&
            repeat_timer_calls::interval == 400 && boundary.scrolls == 2 && TreeView_GetFirstVisible(f.tree) == before &&
            GetCapture() == f.bar && !captureEvents.barCaptureChanges && state.interaction == newTimerId && state.timerId == newTimerId,
            "the old tick cannot scroll, accelerate, cancel or release the replacement that inherited the bar's capture");
        f.SendNative(f.bar, WM_CANCELMODE);
        Check(GetCapture() != f.bar && !state.interaction && !state.timerId && !state.dragging && state.repeat == -1 &&
            repeat_timer_calls::kills == kills + 1 && repeat_timer_calls::killedId == newTimerId,
            "normal cancellation kills precisely the replacement timer and clears its interaction after same-capture reuse");
    }
}

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

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // Standard EDIT/COMBOBOX classes already exist. ICC_STANDARD_CLASSES
    // requires a v6 activation context; this standalone fixture has none.
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES};
    if (!Check(InitCommonControlsEx(&common) != FALSE, "initialize actual native common controls")) return 1;
    GdiPlus gdi;
    if (!gdi.token) return 1;
    const bool repeatOnly = argc == 2 && !std::strcmp(argv[1], "--repeat-cancel-only");
    const bool interleaveOnly = argc == 2 && !std::strcmp(argv[1], "--repeat-interleave-only");
    const bool queuedNewOnly = argc == 2 && !std::strcmp(argv[1], "--repeat-old-tick-new-gesture-only");
    const bool cadenceOnly = argc == 2 && !std::strcmp(argv[1], "--repeat-cadence-only");
    if (argc != 1 && !repeatOnly && !interleaveOnly && !queuedNewOnly && !cadenceOnly) {
        std::fputs("usage: curve_native_control_lifetime_tests [--repeat-cancel-only|--repeat-interleave-only|--repeat-old-tick-new-gesture-only|--repeat-cadence-only]\n", stderr); return 2;
    }
    try {
        std::puts("CASE interaction ticket monotonicity and permanent exhaustion"); InteractionTicketBoundaries();
        if (argc == 1) {
            std::puts("CASE normal segments"); NormalSegments();
            std::puts("CASE normal scroll"); NormalScroll();
            std::puts("CASE retired segments after real kill-focus"); RetiredSegments();
            std::puts("CASE retired scroll after real kill-focus"); RetiredScroll();
            std::puts("CASE retired scroll after real capture transfer"); RetiredAfterCapture();
            std::puts("CASE capture redirected while all native windows and State remain alive"); RedirectedCaptureKeepsStateAlive();
            std::puts("CASE segmented default/read boundaries and exact State replacement"); SegmentedNativeBoundaries();
            std::puts("CASE tree default boundaries and exact paired State replacement"); ScrollNativeBoundaries();
        }
        if (argc == 1 || repeatOnly) {
            std::puts("CASE arrow/page native step cancels capture without restarting repeat"); RepeatCancelledInsideNativeStep();
            std::puts("CASE queued repeat timer arrives after normal cancellation"); QueuedRepeatAfterCancellation();
            std::puts("CASE repeat timer native Step transfers capture and cancels before rearming"); RepeatCancelledInsideTimerStep();
        }
        if (argc == 1 || repeatOnly || interleaveOnly) {
            std::puts("CASE old down/timer resumes after the same State starts a new arrow/page gesture"); SameStateRepeatReplacement();
            std::puts("CASE new arrow/page down reuses existing bar capture while retiring only the old ticket"); NewDownWhileAlreadyCaptured();
        }
        if (argc == 1 || repeatOnly || queuedNewOnly) {
            std::puts("CASE saved old timer arrives after a new gesture starts on the same State"); QueuedOldTickAfterNewGesture();
        }
        if (argc == 1 || repeatOnly || cadenceOnly) {
            std::puts("CASE normal current repeat retains its real 400-to-60-ms timer cadence"); NormalRepeatTimerCadence();
        }
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
