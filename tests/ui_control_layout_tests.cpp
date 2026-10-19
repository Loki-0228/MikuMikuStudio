#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include "mikudancestudio/ui_control_layout.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

// Native Win32 contract test. No D3D, application state, hooks, foreground
// requests or physical input. The fixture stays offscreen except for the short
// positive erase-delivery case, which uses a bounded no-activate work-area surface.
// Its child paint procedure deliberately uses BeginPaint: a standalone erase
// cannot pass merely because a custom renderer ignores WM_ERASEBKGND.
namespace {
namespace layout = mikudancestudio::ui;
struct DpiApi {
    using Set = HANDLE(WINAPI*)(HANDLE);
    using Thread = HANDLE(WINAPI*)();
    using Window = HANDLE(WINAPI*)(HWND);
    using Equal = BOOL(WINAPI*)(HANDLE, HANDLE);
    Set set = nullptr; Thread thread = nullptr; Window window = nullptr; Equal equal = nullptr;
    DpiApi() {
        const auto user = GetModuleHandleW(L"user32.dll");
        set = reinterpret_cast<Set>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
        thread = reinterpret_cast<Thread>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
        window = reinterpret_cast<Window>(GetProcAddress(user, "GetWindowDpiAwarenessContext"));
        equal = reinterpret_cast<Equal>(GetProcAddress(user, "AreDpiAwarenessContextsEqual"));
    }
    bool Available() const { return set && thread && window && equal; }
};
struct DpiScope {
    const DpiApi& api; HANDLE previous = nullptr;
    DpiScope(const DpiApi& value, HANDLE requested) : api(value) {
        if (api.Available()) previous = api.set(requested);
    }
    ~DpiScope() { if (previous) api.set(previous); }
};
enum class Phase { Idle, Collect, Commit, Repaint, Immediate };
struct Fixture;
struct Window {
    struct Counts { unsigned positions = 0, sizes = 0, paints = 0, erases = 0, fonts = 0; };
    Fixture* fixture = nullptr;
    HWND handle = nullptr;
    bool native = false;
    HFONT font = nullptr;
    unsigned positions = 0, sizes = 0, paints = 0, erases = 0, fontMessages = 0;
    unsigned prematurePosition = 0, prematurePaint = 0, standaloneErase = 0;
    unsigned paintDepth = 0;
    bool insideBeginPaint = false;
    LPARAM lastFontRedraw = 0;
    std::array<Counts, 5> phases{};
    void Reset() {
        positions = sizes = paints = erases = fontMessages = 0;
        prematurePosition = prematurePaint = standaloneErase = 0;
        lastFontRedraw = 0;
        phases = {};
    }
};
struct Expected { Window* window = nullptr; RECT bounds{}; bool needsPaint = true; };
RECT Actual(HWND parent, HWND window) {
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) throw std::runtime_error("GetWindowRect fixture");
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}
bool Equal(const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }
struct Fixture {
    struct Interaction {
        unsigned sequence = 0; Phase phase = Phase::Idle;
        HWND window = nullptr, focus = nullptr, capture = nullptr, foreground = nullptr;
        UINT message = 0; WPARAM wp = 0; LPARAM lp = 0;
        const char* stage = nullptr;
    };
    struct Message {
        unsigned sequence = 0, parent = 0, depth = 0;
        Phase phase = Phase::Idle; const char* stage = nullptr;
        HWND window = nullptr; UINT message = 0; WPARAM wp = 0; LPARAM lp = 0;
        LRESULT result = 0; DWORD error = 0; bool returned = false;
        unsigned paintDepth = 0; bool insideBeginPaint = false;
        RECT beforeClient{}, afterClient{};
        bool positionKnown = false; WINDOWPOS position{};
        DWORD dcType = 0; HWND dcWindow = nullptr; int dcClipKind = ERROR; RECT dcClip{};
    };
    Phase phase = Phase::Idle;
    const char* stage = "fixture-create";
    std::array<Window, 6> windows{};
    std::vector<Expected> expected;
    std::vector<POINT> damageSamples;
    unsigned geometryFailures = 0, pendingFailures = 0, paintsChecked = 0;
    unsigned damageFailures = 0, damageChecked = 0;
    unsigned focusMessages = 0, killFocusMessages = 0, activationMessages = 0, captureChanges = 0;
    unsigned baselineFocusMessages = 0, baselineKillFocusMessages = 0;
    std::array<Interaction, 128> interactions{};
    unsigned interactionCount = 0;
    std::array<Message, 1024> messages{};
    unsigned messageCount = 0, messageOverflow = 0, activeMessage = 0, messageDepth = 0;
    bool firstPaint = true;
    bool surfaceRestoreFailed = false;
    HWND originalFocus = GetFocus(), originalCapture = GetCapture();
    const HWND creationFocus = originalFocus, creationForeground = GetForegroundWindow();
    HFONT alternateFont = nullptr;
    unsigned checks = 0;

    Fixture() {
        for (auto& window : windows) window.fixture = this;
        windows[4].native = windows[5].native = true;
        try {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MMS.Test.UiControlLayout.Contract";
            Require(RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
                    "register isolated layout fixture");
            Create(windows[0], wc.lpszClassName, WS_POPUP | WS_CLIPCHILDREN,
                   -16000, -16000, 600, 420, nullptr, 0, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
            Create(windows[1], wc.lpszClassName, WS_CHILD | WS_VISIBLE,
                   10, 20, 80, 30, Parent(), 101);
            Create(windows[2], wc.lpszClassName, WS_CHILD | WS_VISIBLE,
                   120, 20, 80, 30, Parent(), 102);
            Create(windows[3], wc.lpszClassName, WS_CHILD,
                   10, 200, 80, 30, Parent(), 103);
            Create(windows[4], L"BUTTON", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                   10, 80, 90, 28, Parent(), 104);
            Create(windows[5], L"COMBOBOX", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                   130, 80, 140, 151, Parent(), 105);
            for (unsigned index : {4u, 5u}) {
                Require(SetWindowSubclass(windows[index].handle, Native, index,
                                          reinterpret_cast<DWORD_PTR>(&windows[index])) != FALSE,
                        "observe native control without replacing its behavior");
                SendMessageW(windows[index].handle, WM_SETFONT,
                             reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
            }
            for (const wchar_t* item : {L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight"})
                SendMessageW(windows[5].handle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            SendMessageW(windows[5].handle, CB_SETCURSEL, 0, 0);
            LOGFONTW font{};
            Require(GetObjectW(GetStockObject(DEFAULT_GUI_FONT), int(sizeof(font)), &font) == int(sizeof(font)),
                    "read fixture font description");
            font.lfHeight = font.lfHeight == -19 ? -20 : -19;
            alternateFont = CreateFontIndirectW(&font);
            Require(alternateFont != nullptr, "create independently owned test font");
            // Creating a visible top-level window can activate it even with
            // WS_EX_NOACTIVATE. Initialize hidden, then explicitly show without
            // activation; keep the construction activation assertion strict.
            stage = "fixture-show-noactivate";
            Require(SetWindowPos(Parent(), nullptr, 0, 0, 0, 0,
                                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER) != FALSE,
                    "show initialized offscreen fixture without activation");
            Require(IsWindowVisible(Parent()) != FALSE, "fixture is visible for native paint contracts");
            stage = "fixture-initial-paint";
            // Settle only our own initial update regions; never pump user input.
            RedrawWindow(Parent(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            UpdateWindow(Parent());
            for (auto& window : windows) if (window.handle != Parent()) UpdateWindow(window.handle);
            Reset();
            EstablishBaseline();
            UnchangedInteraction();
        } catch (...) { Destroy(); throw; }
    }
    ~Fixture() { Destroy(); }
    void Destroy() noexcept {
        if (Parent() && IsWindow(Parent())) DestroyWindow(Parent());
        windows[0].handle = nullptr;
        if (alternateFont) DeleteObject(alternateFont);
        alternateFont = nullptr;
    }
    HWND Parent() const { return windows[0].handle; }
    int OwnId(HWND handle) const noexcept {
        for (const auto& window : windows) if (handle && window.handle == handle) return GetDlgCtrlID(handle);
        if (handle && Parent() && IsChild(Parent(), handle)) return GetDlgCtrlID(handle);
        return -1;
    }
    void PrintContext(const char* reason) const noexcept {
        std::fprintf(stderr,
            "LAYOUT_CONTEXT reason=%s stage=%s phase=%u parent=%p creation_focus=%p/%d baseline_focus=%p/%d current_focus=%p/%d "
            "creation_foreground=%p current_foreground=%p baseline_capture=%p current_capture=%p "
            "focus_messages=%u baseline_focus_messages=%u kill_focus_messages=%u baseline_kill_focus_messages=%u activation_messages=%u capture_changes=%u\n",
            reason, stage, unsigned(phase), reinterpret_cast<void*>(Parent()), reinterpret_cast<void*>(creationFocus), OwnId(creationFocus),
            reinterpret_cast<void*>(originalFocus), OwnId(originalFocus), reinterpret_cast<void*>(GetFocus()), OwnId(GetFocus()),
            reinterpret_cast<void*>(creationForeground), reinterpret_cast<void*>(GetForegroundWindow()),
            reinterpret_cast<void*>(originalCapture), reinterpret_cast<void*>(GetCapture()),
            focusMessages, baselineFocusMessages, killFocusMessages, baselineKillFocusMessages, activationMessages, captureChanges);
        for (unsigned index = 0; index < interactionCount && index < interactions.size(); ++index) {
            const auto& event = interactions[index];
            std::fprintf(stderr, "LAYOUT_INTERACTION sequence=%u stage=%s phase=%u hwnd=%p/%d message=0x%x wp=%llu lp=%llu focus=%p/%d capture=%p foreground=%p\n",
                event.sequence, event.stage, unsigned(event.phase), reinterpret_cast<void*>(event.window), OwnId(event.window), event.message,
                static_cast<unsigned long long>(event.wp), static_cast<unsigned long long>(event.lp), reinterpret_cast<void*>(event.focus), OwnId(event.focus),
                reinterpret_cast<void*>(event.capture), reinterpret_cast<void*>(event.foreground));
        }
        if (interactionCount > interactions.size()) std::fprintf(stderr, "LAYOUT_INTERACTION overflow=%u\n", interactionCount - unsigned(interactions.size()));
        for (const auto& window : windows) {
            char name[64]{};
            if (window.handle) GetClassNameA(window.handle, name, int(sizeof(name)));
            std::fprintf(stderr,
                "LAYOUT_WINDOW hwnd=%p id=%d class=%s native=%d positions=%u sizes=%u paints=%u erases=%u fonts=%u early_positions=%u early_paints=%u standalone_erases=%u last_font_redraw=%lld\n",
                reinterpret_cast<void*>(window.handle), OwnId(window.handle), name, int(window.native), window.positions, window.sizes,
                window.paints, window.erases, window.fontMessages, window.prematurePosition, window.prematurePaint,
                window.standaloneErase, static_cast<long long>(window.lastFontRedraw));
            for (unsigned index = 0; index < window.phases.size(); ++index) {
                const auto& count = window.phases[index];
                std::fprintf(stderr, "LAYOUT_PHASE hwnd=%p id=%d phase=%u positions=%u sizes=%u paints=%u erases=%u fonts=%u\n",
                    reinterpret_cast<void*>(window.handle), OwnId(window.handle), index, count.positions, count.sizes, count.paints, count.erases, count.fonts);
            }
        }
        std::fprintf(stderr, "LAYOUT_MESSAGES count=%u overflow=%u; parent is nearest recorded message; phases Idle=0 Collect=1 Commit=2 Repaint=3 Immediate=4\n", messageCount, messageOverflow);
        for (unsigned index = 0; index < messageCount; ++index) {
            const auto& event = messages[index];
            std::fprintf(stderr,
                "LAYOUT_MESSAGE sequence=%u parent=%u depth=%u stage=%s phase=%u hwnd=%p/%d message=0x%x wp=%llu lp=%llu returned=%d result=%lld error=%lu "
                "paint_depth=%u in_begin_paint=%d client_before=%ld,%ld,%ld,%ld client_after=%ld,%ld,%ld,%ld position_known=%d position=%d,%d,%d,%d flags=0x%x "
                "supplied_dc_type=%lu supplied_dc_window=%p clip_kind=%d clip=%ld,%ld,%ld,%ld\n",
                event.sequence, event.parent, event.depth, event.stage, unsigned(event.phase), reinterpret_cast<void*>(event.window), OwnId(event.window),
                event.message, static_cast<unsigned long long>(event.wp), static_cast<unsigned long long>(event.lp), int(event.returned),
                static_cast<long long>(event.result), event.error, event.paintDepth, int(event.insideBeginPaint),
                event.beforeClient.left, event.beforeClient.top, event.beforeClient.right, event.beforeClient.bottom,
                event.afterClient.left, event.afterClient.top, event.afterClient.right, event.afterClient.bottom,
                int(event.positionKnown), event.position.x, event.position.y, event.position.cx, event.position.cy, event.position.flags,
                event.dcType, reinterpret_cast<void*>(event.dcWindow), event.dcClipKind, event.dcClip.left, event.dcClip.top, event.dcClip.right, event.dcClip.bottom);
        }
    }
    void Require(bool success, const char* description) {
        ++checks;
        if (!success) { PrintContext(description); throw std::runtime_error(description); }
    }
    void Create(Window& window, const wchar_t* name, DWORD style, int x, int y, int width, int height,
                HWND parent, int id, DWORD extended = 0) {
        window.handle = CreateWindowExW(extended, name, L"layout contract", style, x, y, width, height,
                                      parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr),
                                      window.native ? nullptr : &window);
        Require(window.handle != nullptr, "create fixture window");
    }
    void Reset() {
        phase = Phase::Idle;
        expected.clear();
        damageSamples.clear();
        geometryFailures = pendingFailures = paintsChecked = 0;
        damageFailures = damageChecked = 0;
        firstPaint = true;
        messageCount = messageOverflow = activeMessage = messageDepth = 0;
        for (auto& window : windows) window.Reset();
    }
    void UnchangedInteraction() {
        Require(GetFocus() == originalFocus, "layout must preserve current-thread focus");
        Require(GetCapture() == originalCapture, "layout must preserve capture");
        const HWND foreground = GetForegroundWindow();
        Require(foreground != Parent(), "fixture must never become foreground");
        // Another application can own the desktop foreground while this
        // offscreen fixture runs. That change is outside the fixture's
        // activation contract, so it is recorded as environment evidence
        // instead of failing the layout check. A fixture that did take the
        // foreground still fails the check above and its WM_ACTIVATE is
        // counted by activationMessages.
        if (foreground != creationForeground)
            std::fprintf(stderr, "LAYOUT_FOREGROUND foreign_change creation=%p current=%p parent=%p activation_messages=%u\n",
                reinterpret_cast<void*>(creationForeground), reinterpret_cast<void*>(foreground),
                reinterpret_cast<void*>(Parent()), activationMessages);
        Require(!surfaceRestoreFailed, "temporary native paint surface must return offscreen");
        Require(focusMessages == baselineFocusMessages && killFocusMessages == baselineKillFocusMessages && !activationMessages && !captureChanges,
                "fixture must not request focus, activation or capture");
        Require(!IsWindowVisible(windows[3].handle) && !(GetWindowLongPtrW(windows[3].handle, GWL_STYLE) & WS_VISIBLE),
                "layout must not reveal the hidden child");
    }
    void EstablishBaseline() {
        Require(!activationMessages && GetForegroundWindow() != Parent(), "fixture construction must not activate or steal foreground");
        Require(GetCapture() == originalCapture && !captureChanges, "fixture construction must not change capture");
        const auto focus = GetFocus();
        Require(!creationFocus || focus == creationFocus, "fixture construction must preserve any pre-existing thread focus");
        Require(focus == creationFocus || focus == Parent() || (focus && IsChild(Parent(), focus)),
                "fixture-created focus must belong to the isolated fixture");
        originalFocus = focus;
        baselineFocusMessages = focusMessages;
        baselineKillFocusMessages = killFocusMessages;
        stage = "fixture-ready";
        PrintContext("fixture construction baseline established; subsequent layout must preserve it");
    }
    void Observe(Window& window, UINT message, WPARAM wp, LPARAM lp) noexcept {
        auto& count = window.phases[unsigned(phase)];
        count.positions += message == WM_WINDOWPOSCHANGED;
        count.sizes += message == WM_SIZE;
        count.paints += message == WM_PAINT;
        count.erases += message == WM_ERASEBKGND;
        count.fonts += message == WM_SETFONT;
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATE || message == WM_ACTIVATEAPP ||
            message == WM_NCACTIVATE || message == WM_CAPTURECHANGED) {
            const unsigned index = interactionCount++;
            if (index < interactions.size()) interactions[index] = {index + 1, phase, window.handle, GetFocus(), GetCapture(),
                GetForegroundWindow(), message, wp, lp, stage};
        }
        if (message == WM_SETFOCUS) ++focusMessages;
        if (message == WM_KILLFOCUS) ++killFocusMessages;
        if (message == WM_CAPTURECHANGED) ++captureChanges;
        if (&window == &windows[0] && message == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) ++activationMessages;
        if (message == WM_WINDOWPOSCHANGED) {
            ++window.positions;
            window.prematurePosition += phase == Phase::Collect;
        }
        if (message == WM_SIZE) ++window.sizes;
        if (message == WM_SETFONT) { ++window.fontMessages; window.lastFontRedraw = lp; }
        if (message == WM_ERASEBKGND) {
            ++window.erases;
            if (phase == Phase::Repaint && (!window.paintDepth || (!window.native && !window.insideBeginPaint)))
                ++window.standaloneErase;
        }
        if (message != WM_PAINT) return;
        ++window.paints;
        window.prematurePaint += phase == Phase::Collect;
        ++window.paintDepth;
        if (phase != Phase::Repaint) return;
        ++paintsChecked;
        if (&window == &windows[0] && !damageSamples.empty()) {
            ++damageChecked;
            const auto region = CreateRectRgn(0, 0, 0, 0);
            if (!region || GetUpdateRgn(window.handle, region, FALSE) == ERROR) ++damageFailures;
            else for (const auto point : damageSamples)
                if (!PtInRegion(region, point.x, point.y)) ++damageFailures;
            if (region) DeleteObject(region);
        }
        // Read actual Win32 geometry, not LayoutBatch's pending rectangles.
        for (const auto& item : expected) {
            RECT bounds{};
            if (!GetWindowRect(item.window->handle, &bounds)) { ++geometryFailures; continue; }
            MapWindowPoints(nullptr, Parent(), reinterpret_cast<POINT*>(&bounds), 2);
            if (!Equal(bounds, item.bounds)) ++geometryFailures;
        }
        // RepaintLayout must have invalidated every affected sibling before
        // the very first paint is allowed, including a parent's paint.
        if (firstPaint) {
            firstPaint = false;
            for (const auto& item : expected)
                if (item.needsPaint && !GetUpdateRect(item.window->handle, nullptr, FALSE)) ++pendingFailures;
        }
    }
    void Returned(Window& window, UINT message) noexcept {
        if (message == WM_PAINT) --window.paintDepth;
    }
    unsigned EnterMessage(Window& window, UINT message, WPARAM wp, LPARAM lp) noexcept {
        if (message != WM_WINDOWPOSCHANGING && message != WM_WINDOWPOSCHANGED && message != WM_SIZE &&
            message != WM_PAINT && message != WM_ERASEBKGND && message != WM_SETFONT && message != WM_GETFONT &&
            message != CB_GETDROPPEDCONTROLRECT && message != WM_SYNCPAINT && message != WM_COMMAND) return 0;
        if (messageCount >= messages.size()) { ++messageOverflow; return 0; }
        auto& event = messages[messageCount++]; event = {};
        event.sequence = messageCount; event.parent = activeMessage; event.depth = messageDepth++;
        event.phase = phase; event.stage = stage; event.window = window.handle; event.message = message; event.wp = wp; event.lp = lp;
        event.paintDepth = window.paintDepth; event.insideBeginPaint = window.insideBeginPaint;
        GetClientRect(window.handle, &event.beforeClient);
        if ((message == WM_WINDOWPOSCHANGING || message == WM_WINDOWPOSCHANGED) && lp) {
            event.positionKnown = true; event.position = *reinterpret_cast<const WINDOWPOS*>(lp);
        }
        if ((message == WM_ERASEBKGND || message == WM_PAINT) && wp) {
            const auto dc = reinterpret_cast<HDC>(wp);
            event.dcType = GetObjectType(dc); event.dcWindow = WindowFromDC(dc); event.dcClipKind = GetClipBox(dc, &event.dcClip);
        }
        activeMessage = event.sequence;
        return event.sequence;
    }
    void LeaveMessage(unsigned sequence, LRESULT result, DWORD error) noexcept {
        if (!sequence) return;
        auto& event = messages[sequence - 1];
        event.returned = true; event.result = result; event.error = error;
        GetClientRect(event.window, &event.afterClient);
        activeMessage = event.parent;
        --messageDepth;
    }
    static LRESULT CALLBACK Native(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& window = *reinterpret_cast<Window*>(data);
        const DWORD entryError = GetLastError();
        const unsigned sequence = window.fixture->EnterMessage(window, message, wp, lp);
        window.fixture->Observe(window, message, wp, lp);
        SetLastError(entryError);
        const auto result = DefSubclassProc(hwnd, message, wp, lp);
        const DWORD resultError = GetLastError();
        window.fixture->Returned(window, message);
        window.fixture->LeaveMessage(sequence, result, resultError);
        SetLastError(resultError);
        return result;
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            window = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
            window->handle = hwnd;
        }
        if (!window) return DefWindowProcW(hwnd, message, wp, lp);
        const DWORD entryError = GetLastError();
        auto& fixture = *window->fixture;
        const unsigned sequence = fixture.EnterMessage(*window, message, wp, lp);
        fixture.Observe(*window, message, wp, lp);
        SetLastError(entryError);
        LRESULT result = 0;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            window->insideBeginPaint = true;
            const auto dc = BeginPaint(hwnd, &paint);
            window->insideBeginPaint = false;
            if (dc) {
                FillRect(dc, &paint.rcPaint, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
                EndPaint(hwnd, &paint);
            }
        } else if (message == WM_ERASEBKGND) {
            RECT client{}; GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wp), &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            result = 1;
        } else if (message == WM_GETFONT) result = reinterpret_cast<LRESULT>(window->font);
        else if (message == WM_SETFONT) {
            window->font = reinterpret_cast<HFONT>(wp);
            if (lp) { InvalidateRect(hwnd, nullptr, TRUE); UpdateWindow(hwnd); }
        } else result = DefWindowProcW(hwnd, message, wp, lp);
        const DWORD resultError = GetLastError();
        fixture.Returned(*window, message);
        fixture.LeaveMessage(sequence, result, resultError);
        SetLastError(resultError);
        return result;
    }
    void NoEarlyMessages() {
        for (const auto& window : windows) {
            Require(!window.prematurePosition && !window.prematurePaint,
                    "collecting a batch must not move or paint any window");
            Require(!window.paints && !window.erases,
                    "geometry commit must not paint or erase before final repaint");
        }
    }
    void RepaintChecks() {
        Require(paintsChecked != 0, "fixture must exercise actual WM_PAINT");
        Require(!geometryFailures, "every paint must see all sibling final positions");
        Require(!pendingFailures, "all affected siblings must be invalid before the first paint");
        Require(!damageFailures && (damageSamples.empty() || damageChecked),
                "parent paint damage must cover the actually exposed prior position");
        for (const auto& window : windows)
            Require(!window.standaloneErase, "erase must occur inside WM_PAINT and BeginPaint, never before it");
        UnchangedInteraction();
    }
};

struct VisiblePaintSurface {
    Fixture& fixture;
    RECT original{};
    bool moved = false;
    explicit VisiblePaintSurface(Fixture& value) : fixture(value) {
        RECT work{};
        fixture.Require(GetWindowRect(fixture.Parent(), &original) != FALSE &&
                        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE,
                        "locate a bounded native paint surface in the work area");
        const LONG width = original.right - original.left, height = original.bottom - original.top;
        fixture.Require(work.right - work.left >= width && work.bottom - work.top >= height,
                        "INCOMPLETE: work area cannot contain the native erase fixture");
        const LONG x = work.right - width - ((work.right - work.left > width + 12) ? 12 : 0);
        const LONG y = work.bottom - height - ((work.bottom - work.top > height + 12) ? 12 : 0);
        fixture.stage = "native-erase-visible-setup";
        fixture.phase = Phase::Idle;
        fixture.Require(SetWindowPos(fixture.Parent(), HWND_TOP, int(x), int(y), 0, 0,
                                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW) != FALSE,
                        "show own temporary native paint surface without activation");
        moved = true;
        // Only this small positive-control case needs a nonempty display clip.
        // Settle positioning damage before observing the framework transaction.
        RedrawWindow(fixture.Parent(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        UpdateWindow(fixture.Parent());
        for (auto& window : fixture.windows) if (window.handle != fixture.Parent()) UpdateWindow(window.handle);
        fixture.UnchangedInteraction();
        fixture.stage = "BatchAndImmediate";
    }
    ~VisiblePaintSurface() {
        if (!moved || !IsWindow(fixture.Parent())) return;
        const auto phase = fixture.phase;
        fixture.phase = Phase::Idle;
        fixture.stage = "native-erase-visible-restore";
        if (!SetWindowPos(fixture.Parent(), nullptr, original.left, original.top, 0, 0,
                          SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER)) {
            fixture.surfaceRestoreFailed = true;
            ShowWindow(fixture.Parent(), SW_HIDE);
        }
        fixture.phase = phase;
    }
};

void BatchAndImmediate(Fixture& fixture) {
    VisiblePaintSurface visible(fixture);
    auto& a = fixture.windows[1]; auto& b = fixture.windows[2];
    const auto beforeA = Actual(fixture.Parent(), a.handle), beforeB = Actual(fixture.Parent(), b.handle);
    const RECT finalA{30, 55, 140, 91}, finalB{160, 55, 250, 91};
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch batch;
        layout::MoveLayoutWindow(a.handle, 20, 50, 90, 36, false);
        layout::MoveLayoutControl(fixture.Parent(), 101, 30, 55, 110, 36, false);
        layout::MoveLayoutWindow(b.handle, 160, 55, 90, 36, false);
        fixture.Require(Equal(Actual(fixture.Parent(), a.handle), beforeA) && Equal(Actual(fixture.Parent(), b.handle), beforeB),
                        "normally immediate helpers must remain deferred inside a batch");
        fixture.Require(Equal(layout::ReadLayoutRect(fixture.Parent(), a.handle), finalA), "pending reads expose the final replacement request");
        fixture.NoEarlyMessages();
        fixture.phase = Phase::Commit;
        fixture.Require(batch.Commit(), "commit valid sibling batch");
        fixture.NoEarlyMessages();
        fixture.Require(Equal(Actual(fixture.Parent(), a.handle), finalA) && Equal(Actual(fixture.Parent(), b.handle), finalB),
                        "commit applies both final sibling positions");
        fixture.expected = {{&a, finalA}, {&b, finalB}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), batch.Changes(), RECT{});
        fixture.RepaintChecks();
        fixture.Require(a.paints && b.paints && (a.erases || b.erases), "exercise native erase delivery through actual BeginPaint");
    }
    fixture.Reset(); fixture.phase = Phase::Immediate;
    layout::MoveLayoutWindow(a.handle, 30, 55, 110, 36, false);
    layout::MoveLayoutWindow(a.handle, 30, 55, 110, 36, true);
    fixture.Require(!a.positions && !a.sizes && !a.paints && !a.erases, "identical geometry is a native no-op even for immediate callers");
    fixture.UnchangedInteraction();
}

void FontOnly(Fixture& fixture) {
    auto& button = fixture.windows[4];
    const auto bounds = Actual(fixture.Parent(), button.handle);
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch batch;
        layout::SetLayoutFont(button.handle, fixture.alternateFont);
        layout::SetLayoutFont(button.handle, fixture.alternateFont);
        fixture.Require(reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0)) == fixture.alternateFont,
                        "font is available for measurement while paint remains deferred");
        fixture.Require(button.fontMessages == 1 && !button.lastFontRedraw, "native font changes once without asking for an early redraw");
        fixture.NoEarlyMessages();
        fixture.phase = Phase::Commit;
        fixture.Require(batch.Commit(), "commit font-only layout batch");
        fixture.NoEarlyMessages();
        fixture.Require(Equal(Actual(fixture.Parent(), button.handle), bounds), "font-only update leaves native geometry unchanged");
        fixture.expected = {{&button, bounds}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), batch.Changes(), RECT{});
        fixture.RepaintChecks();
        fixture.Require(button.paints != 0, "font-only change must be repainted after commit");
        fixture.Require(!fixture.windows[1].paints && !fixture.windows[2].paints, "font-only repaint must not repaint unrelated siblings");
    }
}

void NestedAndAbandoned(Fixture& fixture) {
    auto& a = fixture.windows[1]; auto& b = fixture.windows[2];
    const auto oldA = Actual(fixture.Parent(), a.handle);
    const RECT finalA{50, 125, 160, 161}, finalB{220, 125, 310, 161};
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch outer;
        layout::MoveLayoutWindow(a.handle, 40, 120, 110, 36, false);
        std::vector<layout::LayoutChange> all;
        {
            layout::LayoutBatch inner;
            fixture.Require(layout::ReadLayoutRect(fixture.Parent(), a.handle).left == 40, "nested reads can see outer pending geometry");
            layout::MoveLayoutWindow(b.handle, 220, 125, 90, 36, false);
            fixture.NoEarlyMessages();
            fixture.phase = Phase::Commit;
            fixture.Require(inner.Commit(), "explicitly commit nested independent batch");
            fixture.Require(Equal(Actual(fixture.Parent(), a.handle), oldA), "inner commit must not consume outer pending geometry");
            all = inner.Changes();
        }
        fixture.phase = Phase::Collect;
        layout::MoveLayoutWindow(a.handle, 50, 125, 110, 36, false);
        fixture.NoEarlyMessages();
        fixture.phase = Phase::Commit;
        fixture.Require(outer.Commit(), "commit surviving outer batch");
        all.insert(all.end(), outer.Changes().begin(), outer.Changes().end());
        fixture.expected = {{&a, finalA}, {&b, finalB}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), all, RECT{});
        fixture.RepaintChecks();
    }
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch abandoned;
        layout::MoveLayoutWindow(a.handle, 70, 145, 100, 40, false);
        layout::MoveLayoutWindow(b.handle, 240, 145, 100, 40, true);
    }
    fixture.Require(Equal(Actual(fixture.Parent(), a.handle), finalA) && Equal(Actual(fixture.Parent(), b.handle), finalB),
                    "destroying an uncommitted batch must discard requested geometry");
    fixture.NoEarlyMessages();
    fixture.UnchangedInteraction();
}

void HiddenAndCombo(Fixture& fixture) {
    auto& hidden = fixture.windows[3]; auto& combo = fixture.windows[5];
    const auto before = Actual(fixture.Parent(), combo.handle);
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch batch;
        layout::MoveLayoutWindow(hidden.handle, 35, 245, 95, 40, false);
        layout::MoveLayoutWindow(combo.handle, 145, 200, 160, 173, false);
        const int requested = layout::ReadLayoutRequestedHeight(combo.handle);
        fixture.Require(requested == 173, "pending combo height preserves the original dropdown request");
        const auto pending = layout::ReadLayoutRect(fixture.Parent(), combo.handle);
        fixture.Require(pending.bottom - pending.top == before.bottom - before.top, "pending combo rectangle stays collapsed");
        layout::MoveLayoutWindow(combo.handle, 150, 200, 190, requested, false);
        fixture.Require(Equal(Actual(fixture.Parent(), combo.handle), before), "combo remains physically unchanged until commit");
        fixture.NoEarlyMessages();
        fixture.phase = Phase::Commit;
        fixture.Require(batch.Commit(), "commit hidden child and combo popup-height request");
        fixture.NoEarlyMessages();
        const auto collapsed = Actual(fixture.Parent(), combo.handle);
        RECT dropped{};
        fixture.Require(SendMessageW(combo.handle, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropped)) != 0,
                        "native combo reports its actual dropdown geometry");
        fixture.Require(collapsed.left == 150 && collapsed.top == 200 && collapsed.right - collapsed.left == 190 &&
                        collapsed.bottom - collapsed.top < dropped.bottom - dropped.top,
                        "combo popup height must not become the collapsed control height");
        fixture.expected = {{&hidden, RECT{35, 245, 130, 285}, false}, {&combo, collapsed}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), batch.Changes(), RECT{});
        fixture.RepaintChecks();
    }
    fixture.Reset(); fixture.phase = Phase::Immediate;
    layout::MoveLayoutWindow(combo.handle, 150, 200, 190, 173, false);
    fixture.Require(!combo.positions && !combo.sizes && !combo.paints && !combo.erases,
                    "repeated native combo request is a no-op despite dropdown row rounding");
    fixture.UnchangedInteraction();
}

void NestedSameControlDamage(Fixture& fixture) {
    auto& a = fixture.windows[1];
    const RECT outerFinal{10, 260, 50, 284}, innerFinal{300, 260, 340, 284};
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch outer;
        layout::MoveLayoutWindow(a.handle, 10, 260, 40, 24, false);
        {
            layout::LayoutBatch inner;
            layout::MoveLayoutWindow(a.handle, 300, 260, 40, 24, false);
            fixture.phase = Phase::Commit;
            fixture.Require(inner.Commit(), "commit inner move of the same window");
            fixture.expected = {{&a, innerFinal}};
            fixture.phase = Phase::Repaint;
            layout::RepaintLayout(fixture.Parent(), inner.Changes(), RECT{});
            fixture.RepaintChecks();
        }
        fixture.Require(Equal(Actual(fixture.Parent(), a.handle), innerFinal), "inner result is actual before outer commit");
        fixture.Reset(); fixture.phase = Phase::Commit;
        fixture.Require(outer.Commit(), "outer move replaces already painted inner geometry");
        fixture.Require(Equal(Actual(fixture.Parent(), a.handle), outerFinal), "outer final geometry is applied");
        fixture.expected = {{&a, outerFinal}};
        // The stale pre-queue position at x=50 cannot cover x=320. This checks
        // real invalidation consumed by the parent, not Changes() bookkeeping.
        fixture.damageSamples = {{320, 270}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), outer.Changes(), RECT{});
        fixture.RepaintChecks();
    }
}

void FinishedInnerRouting(Fixture& fixture) {
    auto& a = fixture.windows[1]; auto& button = fixture.windows[4];
    const auto old = Actual(fixture.Parent(), a.handle), buttonBounds = Actual(fixture.Parent(), button.handle);
    const RECT final{70, 300, 130, 322};
    const auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch outer;
        layout::MoveLayoutWindow(a.handle, 40, 300, 60, 22, false);
        {
            layout::LayoutBatch inner;
            fixture.phase = Phase::Commit;
            fixture.Require(inner.Commit(), "finish inner scope without destroying it");
            fixture.phase = Phase::Collect;
            fixture.Require(layout::LayoutBatch::IsCollecting(), "finished inner scope must still expose collecting outer scope");
            layout::MoveLayoutWindow(a.handle, 70, 300, 60, 22, false);
            layout::SetLayoutFont(button.handle, font);
            fixture.Require(Equal(Actual(fixture.Parent(), a.handle), old), "finished inner must not let writes bypass outer batching");
            fixture.Require(Equal(layout::ReadLayoutRect(fixture.Parent(), a.handle), final), "finished-inner reads and writes select the same outer queue");
            fixture.Require(button.fontMessages == 1 && !button.lastFontRedraw, "finished-inner font is deferred in the outer scope");
            fixture.NoEarlyMessages();
        }
        fixture.phase = Phase::Commit;
        fixture.Require(outer.Commit(), "commit requests routed through a finished inner scope");
        fixture.NoEarlyMessages();
        fixture.expected = {{&a, final}, {&button, buttonBounds}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), outer.Changes(), RECT{});
        fixture.RepaintChecks();
        fixture.Require(button.paints != 0, "outer owns the routed font-only repaint obligation");
    }
}

void FontRollback(Fixture& fixture) {
    auto& button = fixture.windows[4];
    const auto old = reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0));
    fixture.Require(old != fixture.alternateFont, "rollback fixture has distinct old and temporary fonts");
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch abandoned;
        layout::SetLayoutFont(button.handle, fixture.alternateFont);
    }
    fixture.Require(reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0)) == old,
                    "uncommitted font change restores the previous native font");
    fixture.NoEarlyMessages();
    struct Cancel {};
    try {
        layout::LayoutBatch failed;
        layout::SetLayoutFont(button.handle, fixture.alternateFont);
        throw Cancel{};
    } catch (const Cancel&) {}
    fixture.Require(reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0)) == old,
                    "exception unwinding restores an uncommitted font");
    fixture.NoEarlyMessages();
    const auto committed = reinterpret_cast<HFONT>(GetStockObject(SYSTEM_FONT));
    fixture.Require(committed != old && committed != fixture.alternateFont, "nested font result is independently distinguishable");
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch outer;
        layout::SetLayoutFont(button.handle, fixture.alternateFont);
        {
            layout::LayoutBatch inner;
            layout::SetLayoutFont(button.handle, committed);
            fixture.phase = Phase::Commit;
            fixture.Require(inner.Commit(), "commit a nested font result");
            fixture.expected = {{&button, Actual(fixture.Parent(), button.handle)}};
            fixture.phase = Phase::Repaint;
            layout::RepaintLayout(fixture.Parent(), inner.Changes(), RECT{});
            fixture.RepaintChecks();
        }
        // The outer scope is intentionally abandoned after the inner commit.
    }
    fixture.Require(reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0)) == committed,
                    "abandoned outer font must not overwrite an independently committed inner font");
    fixture.UnchangedInteraction();
}

void FailedCommitWithExternalChange(Fixture& fixture, bool nestedFont) {
    fixture.stage = nestedFont ? "FailedCommitExternalNestedFont" : "FailedCommitExternal";
    auto& moved = fixture.windows[1]; auto& button = fixture.windows[4];
    Window extra; extra.fixture = &fixture;
    fixture.Create(extra, L"MMS.Test.UiControlLayout.Contract", WS_CHILD, 5, 5, 20, 20, fixture.Parent(), 106);
    struct ExtraWindow {
        Window& window;
        ~ExtraWindow() { if (window.handle && IsWindow(window.handle)) DestroyWindow(window.handle); }
    } cleanup{extra};
    const auto before = Actual(fixture.Parent(), moved.handle);
    const int x = before.left < 300 ? 420 : 330;
    const int width = before.right - before.left, height = before.bottom - before.top;
    const auto oldFont = reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0));
    fixture.Require(oldFont != fixture.alternateFont, "failed-commit fixture needs a distinct temporary font");
    HFONT expectedFont = oldFont;
    fixture.Reset(); fixture.phase = Phase::Collect;
    {
        layout::LayoutBatch batch;
        layout::ReserveLayoutChanges(1);
        // Simulate a native UI host's independent geometry publication. Only
        // its repaint obligation joins this outer transaction.
        fixture.phase = Phase::Commit;
        fixture.Require(SetWindowPos(moved.handle, nullptr, x, 260, width, height,
                                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS) != FALSE,
                        "publish actual external geometry before outer commit");
        const auto after = Actual(fixture.Parent(), moved.handle);
        fixture.Require(!Equal(before, after), "external transaction actually moved its window");
        fixture.Require(layout::RecordLayoutChange({moved.handle, fixture.Parent(), before, after}),
                        "record already-published external repaint obligation");
        fixture.phase = Phase::Collect;
        layout::SetLayoutFont(button.handle, fixture.alternateFont);
        if (nestedFont) {
            const auto defaultFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            expectedFont = reinterpret_cast<HFONT>(GetStockObject(oldFont == defaultFont ? SYSTEM_FONT : DEFAULT_GUI_FONT));
            fixture.Require(expectedFont != oldFont && expectedFont != fixture.alternateFont, "nested failed-commit font is independently distinguishable");
            {
                layout::LayoutBatch inner;
                layout::SetLayoutFont(button.handle, expectedFont);
                fixture.phase = Phase::Commit;
                fixture.Require(inner.Commit(), "commit independent font before outer preparation failure");
                fixture.expected = {{&button, Actual(fixture.Parent(), button.handle)}};
                fixture.phase = Phase::Repaint;
                layout::RepaintLayout(fixture.Parent(), inner.Changes(), RECT{});
                fixture.RepaintChecks();
            }
            // That independent paint was intentional. Observe the following
            // failing preparation separately, while the outer batch stays live.
            fixture.Reset(); fixture.phase = Phase::Collect;
        }
        layout::MoveLayoutWindow(extra.handle, 15, 15, 37, 19, false);
        const HWND destroyed = extra.handle;
        fixture.Require(DestroyWindow(destroyed) != FALSE && !IsWindow(destroyed),
                        "destroy only the extra queued HWND to force native guard attachment failure");
        extra.handle = nullptr;
        fixture.NoEarlyMessages();
        fixture.phase = Phase::Commit;
        fixture.Require(!batch.Commit(), "native guard attachment failure rejects commit");
        fixture.Require(!layout::LayoutBatch::IsCollecting(), "failed preparation ends the batch collection scope");
        fixture.Require(reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0)) == expectedFont,
                        "failed commit restores its temporary font immediately without overwriting nested committed font");
        const auto& changes = batch.Changes();
        unsigned externalCount = 0;
        for (const auto& change : changes) if (change.window == moved.handle) {
            ++externalCount;
            fixture.Require(change.parent == fixture.Parent() && Equal(change.before, before) && Equal(change.after, after),
                            "failed commit preserves actual external before/after damage");
        }
        fixture.Require(externalCount == 1, "failed commit retains the external publication exactly once");
        fixture.Require(Equal(Actual(fixture.Parent(), moved.handle), after), "failed outer preparation must not undo independently published geometry");
        fixture.NoEarlyMessages();
        const auto retained = changes;
        const unsigned messagesBeforeRetry = fixture.messageCount;
        fixture.Require(!batch.Commit(), "failed batch is finalized and cannot be committed again");
        fixture.Require(fixture.messageCount == messagesBeforeRetry && batch.Changes().size() == retained.size(),
                        "retry performs no native work and preserves retained repaint obligations");
        for (std::size_t index = 0; index < retained.size(); ++index)
            fixture.Require(batch.Changes()[index].window == retained[index].window && batch.Changes()[index].parent == retained[index].parent &&
                            Equal(batch.Changes()[index].before, retained[index].before) && Equal(batch.Changes()[index].after, retained[index].after),
                            "failed batch retry must not duplicate or rewrite damage");
        fixture.expected = {{&moved, after}};
        fixture.damageSamples = {{(before.left + before.right) / 2, (before.top + before.bottom) / 2}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), batch.Changes(), RECT{});
        fixture.RepaintChecks();
    }
    fixture.Require(reinterpret_cast<HFONT>(SendMessageW(button.handle, WM_GETFONT, 0, 0)) == expectedFont,
                    "finalized failed batch destructor must not alter the resolved font");
    fixture.UnchangedInteraction();
}

void ForeignDpiCaller(Fixture& fixture, const DpiApi& api, bool createdUnaware) {
    if (!createdUnaware || !api.Available()) {
        std::printf("SKIP ui_control_layout foreign-DPI boundary: dynamic APIs or unaware fixture unavailable\n");
        return;
    }
    const auto parentContext = api.window(fixture.Parent());
    const auto entryContext = api.thread();
    auto& a = fixture.windows[1]; auto& b = fixture.windows[2];
    const RECT finalA{100, 330, 180, 354}, finalB{260, 330, 350, 354};
    fixture.Reset(); fixture.phase = Phase::Collect;
    layout::LayoutBatch batch;
    {
        mikudancestudio::WindowDpiScope gathering(fixture.Parent());
        layout::MoveLayoutWindow(a.handle, 100, 330, 80, 24, false);
        layout::MoveLayoutWindow(b.handle, 260, 330, 90, 24, false);
        fixture.NoEarlyMessages();
    }
    // This is an independent test-side context guard. It does not use the
    // product's geometry reader to judge the actual committed coordinates.
    HANDLE previous = api.set(reinterpret_cast<HANDLE>(INT_PTR(-4)));
    if (!previous) previous = api.set(reinterpret_cast<HANDLE>(INT_PTR(-3)));
    if (!previous) {
        std::printf("SKIP ui_control_layout foreign-DPI boundary: per-monitor caller unsupported\n");
        return; // The uncommitted batch is discarded normally.
    }
    {
        struct Restore { const DpiApi& api; HANDLE previous; ~Restore() { api.set(previous); } } restore{api, previous};
        const auto foreign = api.thread();
        fixture.Require(!api.equal(foreign, parentContext), "foreign caller has a genuinely different DPI awareness context");
        fixture.Require(Equal(layout::ReadLayoutRect(fixture.Parent(), a.handle), finalA), "pending reads keep parent logical coordinates for a foreign caller");
        fixture.Require(api.equal(api.thread(), foreign), "pending geometry query restores the foreign caller context");
        fixture.phase = Phase::Commit;
        fixture.Require(batch.Commit(), "commit parent-DPI candidates from a foreign caller context");
        fixture.Require(api.equal(api.thread(), foreign), "commit restores the foreign caller context");
        {
            DpiScope oracle(api, parentContext);
            fixture.Require(oracle.previous != nullptr, "independent parent-context geometry oracle is available");
            fixture.Require(Equal(Actual(fixture.Parent(), a.handle), finalA) && Equal(Actual(fixture.Parent(), b.handle), finalB),
                            "actual Win32 bounds equal parent-logical candidates despite foreign commit context");
        }
        fixture.Require(api.equal(api.thread(), foreign), "independent geometry oracle restores the foreign caller");
        fixture.expected = {{&a, finalA}, {&b, finalB}};
        fixture.phase = Phase::Repaint;
        layout::RepaintLayout(fixture.Parent(), batch.Changes(), RECT{});
        fixture.Require(api.equal(api.thread(), foreign), "repaint restores the foreign caller context");
        fixture.RepaintChecks();
    }
    fixture.Require(api.equal(api.thread(), entryContext), "foreign caller test restores its entry DPI context");
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        DpiApi dpi;
        DpiScope product(dpi, reinterpret_cast<HANDLE>(INT_PTR(-1)));
        Fixture fixture;
        fixture.stage = "BatchAndImmediate";
        BatchAndImmediate(fixture);
        fixture.UnchangedInteraction();
        fixture.stage = "FontOnly";
        FontOnly(fixture);
        fixture.stage = "NestedAndAbandoned";
        NestedAndAbandoned(fixture);
        fixture.stage = "HiddenAndCombo";
        HiddenAndCombo(fixture);
        fixture.stage = "NestedSameControlDamage";
        NestedSameControlDamage(fixture);
        fixture.stage = "FinishedInnerRouting";
        FinishedInnerRouting(fixture);
        fixture.stage = "FontRollback";
        FontRollback(fixture);
        FailedCommitWithExternalChange(fixture, false);
        FailedCommitWithExternalChange(fixture, true);
        fixture.stage = "ForeignDpiCaller";
        const bool unaware = product.previous && dpi.equal(dpi.window(fixture.Parent()), reinterpret_cast<HANDLE>(INT_PTR(-1)));
        ForeignDpiCaller(fixture, dpi, unaware);
        std::printf("PASS ui_control_layout native contracts checks=%u; no desktop flicker claim\n", fixture.checks);
        return 0;
    } catch (const std::exception& reason) {
        std::fprintf(stderr, "FAIL ui_control_layout native contract: %s\n", reason.what());
        return 1;
    }
}
