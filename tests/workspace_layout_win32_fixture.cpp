// MAINTENANCE: Keep this opt-in HWND fixture until a real workspace pane covers the same UI-08 paths.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include "../src/plugins/workspace_layout_preview.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

namespace {
using namespace mikumikustudio::plugins::workspace_layout;

constexpr wchar_t RootClass[] = L"MMS.UI08.Fixture.Root";
constexpr wchar_t GripClass[] = L"MMS.UI08.Fixture.Grip";
constexpr Region Workspace = 1;
constexpr Ref PaneId{1, 1}, HomeId{2, 1}, SpareId{3, 1};
constexpr RECT Home{24, 112, 248, 188}, Spare{296, 112, 520, 188};
constexpr RECT GripBox{24, 48, 192, 88};
constexpr LONG ClientWidth = 544, ClientHeight = 246;
constexpr UINT DpiChangedMessage = 0x02E0; // WM_DPICHANGED, also with the Win7 SDK target.
constexpr UINT HitFlags = CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT;

POINT Center(const RECT& rect) noexcept {
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

enum class End {
    Released, Escape, CaptureLost, CancelMode, Deactivated, Environment,
    SourceLost, OwnerUnloaded, Closing, Error
};

class Fixture {
public:
    bool Create(HINSTANCE instance);
    int Run(bool selfTest);
    ~Fixture() { if (root_) DestroyWindow(root_); }

private:
    static LRESULT CALLBACK RootProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK GripProc(HWND, UINT, WPARAM, LPARAM);
    bool MountGrip();
    void Begin(POINT);
    void Update(POINT, WPARAM);
    void Stop(End);
    void Unload();
    void SourceDestroyed(HWND);
    void TakeCapture();
    bool Key(WPARAM);
    bool Publish();
    bool CheckPosition(Ref slot) const;
    bool Fits() const;
    void Status(const wchar_t*);
    void Failure(const char*);
    bool SelfTest();

    HINSTANCE instance_ = nullptr;
    HWND root_ = nullptr, grip_ = nullptr, pane_ = nullptr;
    Source source_{};
    HostLayout live_{{1, 1}, {{PaneId, HomeId, true}},
                     {{HomeId, Workspace}, {SpareId, Workspace, true}}, {}, {{PaneId, SpareId}}};
    Preview preview_;
    const std::vector<Ref> claims_{PaneId};
    const std::vector<Move> yield_{{PaneId, SpareId}}, empty_{};
    SessionState session_{};
    Ref projected_{};
    unsigned placements_ = 0;
    End lastEnd_ = End::Closing;
    const wchar_t* status_ = L"Ready. Drag the grip into the reserved slot.";
    bool failed_ = false;
};

bool Fixture::Create(HINSTANCE instance) {
    instance_ = instance;
    for (const auto& entry : {std::pair{RootClass, RootProc}, std::pair{GripClass, GripProc}}) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = entry.second;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = entry.first;
        if (!RegisterClassW(&wc)) return false;
    }
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT bounds{0, 0, ClientWidth, ClientHeight};
    if (!AdjustWindowRect(&bounds, style, FALSE)) return false;
    root_ = CreateWindowExW(0, RootClass, L"MMS UI-08 | temporary workspace yield fixture", style,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance_, this);
    if (!root_) return false;
    // The actual pane is one native Win32 control, never reparented or rebuilt
    // when a preview is published or cancelled.
    pane_ = CreateWindowExW(0, L"BUTTON", L"Native pane (same HWND)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Home.left, Home.top, Home.right - Home.left, Home.bottom - Home.top, root_, nullptr, instance_, nullptr);
    if (!pane_ || !MountGrip()) return false;
    projected_ = HomeId;
    ShowWindow(root_, SW_SHOWNORMAL);
    UpdateWindow(root_);
    return true;
}

bool Fixture::MountGrip() {
    if (grip_) return true;
    grip_ = CreateWindowExW(0, GripClass, L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
        GripBox.left, GripBox.top, GripBox.right - GripBox.left, GripBox.bottom - GripBox.top,
        root_, nullptr, instance_, this);
    if (!grip_) return false;
    ++live_.basis.environment;
    source_ = {live_.basis.environment, {4, live_.basis.environment}};
    live_.offerSource = source_;
    return true;
}

bool Fixture::Fits() const {
    RECT client{};
    return GetClientRect(root_, &client) && client.right >= Spare.right &&
        client.bottom >= Spare.bottom && !IsIconic(root_);
}

void Fixture::Status(const wchar_t* text) {
    status_ = text;
    if (root_) InvalidateRect(root_, nullptr, TRUE);
}

void Fixture::Failure(const char* text) {
    if (!failed_) std::fprintf(stderr, "%s (last Win32 error: %lu)\n", text, GetLastError());
    failed_ = true;
    Status(L"Fixture failure. See the console.");
}

void Fixture::Begin(POINT point) {
    RECT client{};
    if (failed_ || session_.token || GetCapture() || GetForegroundWindow() != root_ ||
        !IsWindowVisible(grip_) || !Fits() || !GetClientRect(grip_, &client) || !PtInRect(&client, point))
        return;
    // Raw key messages (Esc, etc.) go to the grip even during capture.
    SetFocus(grip_);
    if (GetFocus() != grip_ || GetForegroundWindow() != root_ || GetCapture()) return;
    if (preview_.Begin(source_, Workspace, live_.basis, live_, claims_, session_) != Result::Ok) {
        Failure("Preview::Begin rejected the fixture's authorized pane");
        return;
    }
    SetCapture(grip_);
    if (GetCapture() != grip_) {
        Stop(End::Error);
        Failure("Could not capture the mouse for the grip");
        return;
    }
    Status(L"Drag active. Only the outlined spare slot accepts the pane.");
}

void Fixture::Update(POINT point, WPARAM keys) {
    if (!session_.token) return;
    if (GetCapture() != grip_) { Stop(End::CaptureLost); return; }
    if (!(keys & MK_LBUTTON)) { Stop(End::Released); return; }
    if (GetForegroundWindow() != root_) { Stop(End::Deactivated); return; }
    if (!Fits() || !ClientToScreen(grip_, &point) || !ScreenToClient(root_, &point)) {
        Stop(End::Environment);
        return;
    }

    const bool accepts = PtInRect(&Spare, point) != FALSE;
    if ((projected_ == SpareId) == accepts) return;
    if (preview_.Replace(source_, session_.token, session_.revision, live_.basis, live_,
                        accepts ? yield_ : empty_) != Result::Ok ||
        preview_.Query(source_, session_.token, live_.basis, session_) != Result::Ok || !Publish()) {
        Stop(End::Error);
        Failure("Could not publish a complete preview");
        return;
    }
    Status(accepts ? L"Preview: same pane in the reserved slot." : L"Preview cleared: the pane is home.");
}

bool Fixture::Publish() {
    const Ref slot = preview_.EffectiveSlot(live_, PaneId);
    if (!IsWindow(pane_) || GetParent(pane_) != root_ || (slot != HomeId && slot != SpareId)) return false;
    if (slot == projected_) return true;
    const RECT& rect = slot == SpareId ? Spare : Home;
    if (!SetWindowPos(pane_, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                      SWP_NOACTIVATE | SWP_NOZORDER)) {
        projected_ = {}; // A failed move must not suppress the attempt to restore home.
        return false;
    }
    projected_ = slot;
    ++placements_;
    return true;
}

void Fixture::Stop(End reason) {
    if (!session_.token) return;
    const auto ended = std::exchange(session_, SessionState{});
    lastEnd_ = reason;
    if (reason == End::OwnerUnloaded) preview_.OwnerUnloaded(source_.owner);
    else if (reason == End::SourceLost) preview_.SourceLost(source_);
    else if (preview_.Cancel(source_, ended.token) != Result::Ok) Failure("Could not cancel the live preview");

    // ReleaseCapture can synchronously send WM_CAPTURECHANGED to the grip.
    // The cleared token makes that path a no-op; never release another HWND's capture.
    if (grip_ && GetCapture() == grip_ && !ReleaseCapture()) Failure("Could not release grip capture");
    if (!Publish()) Failure("Could not restore the pane from the current host layout");
    if (failed_) return;
    switch (reason) {
    case End::Released: Status(L"Button released: pane restored."); break;
    case End::Escape: Status(L"Escape: pane restored."); break;
    case End::CaptureLost: Status(L"Capture changed: pane restored."); break;
    case End::CancelMode: Status(L"Cancel mode: pane restored."); break;
    case End::Deactivated: Status(L"Window deactivated: pane restored."); break;
    case End::Environment: Status(L"Workspace changed: pane restored."); break;
    case End::SourceLost: Status(L"Source destroyed: pane restored."); break;
    case End::OwnerUnloaded: Status(L"Fixture owner unloaded: pane restored."); break;
    case End::Closing:
    case End::Error: break;
    }
}

void Fixture::SourceDestroyed(HWND window) {
    if (window != grip_) return;
    Stop(End::SourceLost);
    ++live_.basis.environment;
    live_.offerSource = {};
    source_ = {};
    grip_ = nullptr;
}

void Fixture::Unload() {
    if (!grip_) return;
    // Simulates manager notification; there is no real plugin DLL in this fixture.
    const bool hadFocus = GetFocus() == grip_;
    Stop(End::OwnerUnloaded);
    if (!DestroyWindow(grip_)) { Failure("Could not destroy the fixture source"); return; }
    if (hadFocus && GetForegroundWindow() == root_ && !GetFocus()) SetFocus(root_);
    if (!failed_) Status(L"Fixture owner unloaded. Pane restored; R remounts the grip.");
}

void Fixture::TakeCapture() {
    if (!session_.token) return;
    // A genuine transfer to a different HWND, which produces WM_CAPTURECHANGED.
    SetCapture(root_);
    if (session_.token || GetCapture() != root_) Failure("Capture transfer did not terminate the drag");
    if (GetCapture() == root_ && !ReleaseCapture()) Failure("Could not release transferred capture");
}

bool Fixture::Key(WPARAM key) {
    switch (key) {
    case VK_ESCAPE: Stop(End::Escape); return true;
    case 'C': TakeCapture(); return true;
    case 'U': Unload(); return true;
    case 'R':
        if (!failed_ && !grip_) {
            if (!MountGrip()) Failure("Could not remount the fixture source");
            else Status(L"New fixture owner mounted. Drag the grip into the spare slot.");
        }
        return true;
    default: return false;
    }
}

LRESULT CALLBACK Fixture::RootProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (fixture) switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) fixture->Stop(End::Deactivated);
        break;
    case WM_ACTIVATEAPP:
        if (!wp) fixture->Stop(End::Deactivated);
        break;
    case WM_CANCELMODE: fixture->Stop(End::CancelMode); return 0;
    case WM_SIZE:
    case DpiChangedMessage: fixture->Stop(End::Environment); break;
    case WM_KEYDOWN: if (fixture->Key(wp)) return 0; break;
    case WM_DESTROY: fixture->Stop(End::Closing); PostQuitMessage(0); return 0;
    case WM_NCDESTROY:
        if (fixture->root_ == window) fixture->root_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        const auto text = [&](int x, int y, const wchar_t* value) {
            TextOutW(dc, x, y, value, lstrlenW(value));
        };
        text(24, 14, L"Hold the grip, then drag into the spare slot. Nothing is saved.");
        text(Home.left, 92, L"Home");
        text(Spare.left, 92, L"Reserved spare slot");
        FrameRect(dc, &Home, GetSysColorBrush(COLOR_WINDOWTEXT));
        FrameRect(dc, &Spare, GetSysColorBrush(COLOR_WINDOWTEXT));
        text(24, 198, fixture->status_);
        text(24, 220, L"Esc: cancel  |  C: transfer capture  |  U: unload source  |  R: remount");
        EndPaint(window, &paint);
        return 0;
    }
    }
    return DefWindowProcW(window, msg, wp, lp);
}

LRESULT CALLBACK Fixture::GripProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* fixture = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (fixture) switch (msg) {
    case WM_LBUTTONDOWN:
        if (wp & MK_LBUTTON) fixture->Begin({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_MOUSEMOVE: fixture->Update({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, wp); return 0;
    case WM_LBUTTONUP: fixture->Stop(End::Released); return 0;
    case WM_KEYDOWN: if (fixture->Key(wp)) return 0; break;
    case WM_CAPTURECHANGED:
        if (GetCapture() != window) fixture->Stop(End::CaptureLost);
        return 0;
    case WM_CANCELMODE: fixture->Stop(End::CancelMode); return 0;
    case WM_DESTROY: fixture->SourceDestroyed(window); return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        DrawTextW(dc, L"Drag grip", -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        EndPaint(window, &paint);
        return 0;
    }
    }
    return DefWindowProcW(window, msg, wp, lp);
}

bool Fixture::CheckPosition(Ref slot) const {
    RECT bounds{};
    if (!IsWindowVisible(pane_) || GetParent(pane_) != root_ || !GetWindowRect(pane_, &bounds)) return false;
    POINT topLeft{bounds.left, bounds.top}, bottomRight{bounds.right, bounds.bottom};
    if (!ScreenToClient(root_, &topLeft) || !ScreenToClient(root_, &bottomRight)) return false;
    const RECT actual{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    const RECT& here = slot == SpareId ? Spare : Home;
    const RECT& other = slot == SpareId ? Home : Spare;
    return EqualRect(&actual, &here) && ChildWindowFromPointEx(root_, Center(here), HitFlags) == pane_ &&
           ChildWindowFromPointEx(root_, Center(other), HitFlags) == root_;
}

bool Fixture::SelfTest() {
    // Mouse progression is deterministic message injection. The underlying
    // HWNDs, positions, capture transfer and Deactivate notification are real.
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return false; } } while (false)
    if (GetForegroundWindow() != root_) SetForegroundWindow(root_);
    if (GetForegroundWindow() != root_) {
        std::fputs("Self-test needs the foreground on an isolated interactive desktop.\n", stderr);
        return false;
    }
    const HWND samePane = pane_;
    const Basis baseline = live_.basis;
    const auto move = [&](POINT point, WPARAM keys = MK_LBUTTON) {
        if (!ClientToScreen(root_, &point) || !ScreenToClient(grip_, &point)) return false;
        SendMessageW(grip_, WM_MOUSEMOVE, keys, MAKELPARAM(point.x, point.y));
        return true;
    };
    Source testedSource{};
    Token testedToken = 0;
    const auto start = [&] {
        SendMessageW(grip_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 12));
        testedSource = source_;
        testedToken = session_.token;
        return testedToken && GetCapture() == grip_ && GetFocus() == grip_ &&
               move(Center(Spare)) && CheckPosition(SpareId);
    };
    const auto restored = [&](End why, unsigned placements) {
        SessionState unused;
        return !failed_ && !session_.token && lastEnd_ == why &&
               preview_.Query(testedSource, testedToken, live_.basis, unused) == Result::NotFound && GetCapture() == nullptr &&
               placements_ == placements && samePane == pane_ && live_.panes.front().slot == HomeId &&
               live_.basis.preferences == baseline.preferences && CheckPosition(HomeId);
    };

    CHECK(CheckPosition(HomeId));
    SendMessageW(grip_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(-1, 12));
    CHECK(!session_.token && !GetCapture() && placements_ == 0);
    CHECK(start());
    CHECK(move(Center(Spare)) && placements_ == 1); // same slot: no redundant move
    CHECK(move({-8, -8}) && CheckPosition(HomeId) && GetCapture() == grip_ && placements_ == 2);
    CHECK(move(Center(Spare)) && CheckPosition(SpareId) && placements_ == 3);
    SendMessageW(grip_, WM_LBUTTONUP, 0, 0);
    CHECK(restored(End::Released, 4));
    CHECK(move(Center(Spare)) && restored(End::Released, 4)); // stale move after UP

    CHECK(start());
    SendMessageW(grip_, WM_CAPTURECHANGED, 0, 0); // old notification during a fresh drag
    CHECK(session_.token == testedToken && GetCapture() == grip_ && CheckPosition(SpareId));
    SendMessageW(grip_, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(restored(End::Escape, 6));

    CHECK(start());
    SetCapture(root_);
    CHECK(GetCapture() == root_ && !session_.token && lastEnd_ == End::CaptureLost);
    CHECK(ReleaseCapture() && restored(End::CaptureLost, 8));
    SendMessageW(grip_, WM_CAPTURECHANGED, 0, 0); // duplicate notification is inert
    CHECK(restored(End::CaptureLost, 8));

    CHECK(start());
    SendMessageW(root_, WM_CANCELMODE, 0, 0);
    CHECK(restored(End::CancelMode, 10));

    CHECK(start());
    SendMessageW(grip_, WM_CANCELMODE, 0, 0);
    CHECK(restored(End::CancelMode, 12));

    // A second top-level window on the same UI thread activates without
    // injecting WM_ACTIVATE. Give it no overlap with the pane for hit checks.
    HWND other = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Activation fixture",
        WS_POPUP | WS_CAPTION, 0, 0, 130, 48, nullptr, nullptr, instance_, nullptr);
    CHECK(other != nullptr);
    ShowWindow(other, SW_SHOWNOACTIVATE);
    const bool started = start();
    if (started) SetActiveWindow(other);
    const bool switched = GetActiveWindow() == other;
    const bool deactivated = restored(End::Deactivated, 14) || restored(End::CaptureLost, 14);
    DestroyWindow(other);
    SetActiveWindow(root_);
    CHECK(started && switched && deactivated && GetForegroundWindow() == root_);

    CHECK(start());
    const auto prior = source_.surface;
    const auto priorToken = session_.token;
    Unload();
    CHECK(!grip_ && restored(End::OwnerUnloaded, 16));
    SendMessageW(root_, WM_KEYDOWN, 'R', 0);
    CHECK(grip_ && source_.surface != prior && start() && session_.token != priorToken);
    CHECK(DestroyWindow(grip_));
    CHECK(!grip_ && (restored(End::SourceLost, 18) || restored(End::CaptureLost, 18)));
    CHECK(MountGrip() && start());
    CHECK(move(Center(Spare), 0) && restored(End::Released, 20));
    CHECK(start());
    SendMessageW(root_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(ClientWidth, ClientHeight));
    CHECK(restored(End::Environment, 22));
    CHECK(live_.basis.preferences == baseline.preferences);
    std::puts("PASS: same HWND, geometry/hit, release, Esc, capture transfer, cancel mode, activation, owner unload, source teardown and workspace-change cancellation.");
    return true;
#undef CHECK
}

int Fixture::Run(bool selfTest) {
    if (selfTest) return SelfTest() ? 0 : 1;
    MSG msg{};
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return result < 0 || failed_ ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    const bool selfTest = argc == 2 && std::strcmp(argv[1], "--self-test") == 0;
    if (argc != 1 && !selfTest) {
        std::fprintf(stderr, "Usage: workspace_layout_win32_fixture [--self-test]\n");
        return 2;
    }
    Fixture fixture;
    if (!fixture.Create(GetModuleHandleW(nullptr))) {
        std::fprintf(stderr, "Could not create workspace fixture windows: %lu\n", GetLastError());
        return 1;
    }
    return fixture.Run(selfTest);
}
