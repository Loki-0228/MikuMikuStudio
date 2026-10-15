#include <native_ui/layout.hpp>
#include <native_ui/update.hpp>
#include <native_ui/win32_host.hpp>
#include <algorithm>
#include <cwchar>
#include <map>
#include <string>

namespace {
struct Host {
    std::map<std::string, HWND> controls;
    native_ui::PanelBuffer buffer;
    native_ui::UpdateCoordinator updates;
    unsigned layouts = 0, simulatedRenders = 0, simulatedPresents = 0;
    bool failed = false;
    void Layout(HWND hwnd) {
        RECT bounds{}; GetClientRect(hwnd, &bounds);
        native_ui::Node root; root.id = "form"; root.hints.flow = native_ui::Flow::Grid; root.hints.columns = 2;
        for (const auto& [id, control] : controls) {
            native_ui::Node child; child.id = id; child.measuredMinimum = {120, 28}; child.measuredPreferred = {220, 28}; child.hints.shrink.width = 1;
            root.children.push_back(child);
        }
        auto result = native_ui::Arrange(root, {float(bounds.right), 0});
        std::vector<native_ui::WindowMove> moves;
        for (const auto& item : result.items) if (auto it = controls.find(item.id); it != controls.end()) {
            auto r = item.rect; moves.push_back({it->second, {LONG(r.x), LONG(r.y), LONG(r.x + r.width), LONG(r.y + r.height)}});
        }
        failed |= native_ui::ApplyWindows(moves) != native_ui::ApplyResult::Applied; ++layouts;
    }
    void Drain() {
        updates.Drain(GetTickCount64(), 8, [&](const native_ui::UpdateRequest& r) {
            if (r.flags & native_ui::RenderDirty) ++simulatedRenders;
            if (r.flags & native_ui::PresentDirty) ++simulatedPresents;
            return native_ui::UpdateOutcome{native_ui::UpdateResult::Done, r.flags};
        });
    }
};
LRESULT CALLBACK Window(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) { host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host)); }
    if (!host) return DefWindowProcW(hwnd, message, wp, lp);
    switch (message) {
    case WM_CREATE:
        host->controls["a.label"] = CreateWindowW(L"STATIC", L"名称（支持中文输入）", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hwnd, nullptr, nullptr, nullptr);
        host->controls["b.input"] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"独立原生表单", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 1, 1, hwnd, nullptr, nullptr, nullptr);
        host->controls["c.check"] = CreateWindowW(L"BUTTON", L"保持分组", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 1, 1, hwnd, nullptr, nullptr, nullptr);
        host->controls["d.button"] = CreateWindowW(L"BUTTON", L"刷新本面板", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
        for (const auto& [id, window] : host->controls) { if (!window) host->failed = true; SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE); }
        return 0;
    case WM_SIZE: host->Layout(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 100) { host->updates.Request({1, 1, 1, GetTickCount64(), native_ui::UiDirty}); host->Drain(); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint); RECT rect{}; GetClientRect(hwnd, &rect);
        HDC memory = host->buffer.Begin(dc, rect.right, rect.bottom, 96, RGB(242, 242, 242));
        HDC target = memory ? memory : dc; HBRUSH background = CreateSolidBrush(RGB(242, 242, 242)); FillRect(target, &paint.rcPaint, background); DeleteObject(background);
#ifdef NATIVE_UI_SIMULATED_VIEWPORT
        RECT viewport{8, 100, std::max(8L, rect.right - 8), std::max(100L, rect.bottom - 8)};
        HBRUSH ink = CreateSolidBrush(RGB(35, 40, 50)); FillRect(target, &viewport, ink); DeleteObject(ink);
        SetTextColor(target, RGB(240, 240, 240)); SetBkMode(target, TRANSPARENT);
        DrawTextW(target, L"模拟视口 · 无图形设备依赖", -1, &viewport, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
#endif
        if (memory) host->failed |= !host->buffer.Copy(dc, paint.rcPaint);
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, message, wp, lp);
    }
}
}
int WINAPI wWinMain(HINSTANCE module, HINSTANCE, PWSTR arguments, int show) {
    Host host;
    WNDCLASSW c{}; c.hInstance = module; c.lpszClassName = L"NativeUi.Standalone.Form"; c.lpfnWndProc = Window; c.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&c)) return 1;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, c.lpszClassName, L"native_ui 独立宿主", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 540, 350, nullptr, nullptr, module, &host);
    if (!window) return 2;
    if (std::wcsstr(arguments, L"--self-test")) {
        for (int width : {540, 250, 540}) { SetWindowPos(window, nullptr, 0, 0, width, 350, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); SendMessageW(window, WM_PAINT, 0, 0); }
        for (int i = 0; i < 8; ++i) SendMessageW(window, WM_COMMAND, 100, 0);
        host.failed |= host.simulatedRenders != 0;
        host.updates.Available(2, false); host.updates.Request({2, 1, 1, 0, native_ui::RenderDirty | native_ui::PresentDirty}); host.Drain();
        host.failed |= host.simulatedRenders != 0;
        host.updates.Available(2, true); host.Drain(); host.failed |= host.simulatedRenders != 1 || host.simulatedPresents != 1;
        DestroyWindow(window); return host.failed ? 3 : 0;
    }
    ShowWindow(window, show); MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    return host.failed ? 4 : 0;
}
