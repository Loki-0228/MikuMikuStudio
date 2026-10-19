// Regression: preserve legacy UI layout across DPI-scaled fullscreen transitions.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <utility>
#include <MinHook.h>

namespace mikumikustudio { void LayoutViewportPanels(MMDApp*); }
using namespace mikumikustudio;
static void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) std::exit(1);
}
static int WINAPI UnexpectedA(HWND, LPCSTR text, LPCSTR, UINT) {
    std::printf("Unexpected dialog: %s\n", text); std::exit(2);
}
static int WINAPI UnexpectedW(HWND, LPCWSTR text, LPCWSTR, UINT) {
    std::fwprintf(stderr, L"Unexpected dialog: %ls\n", text); std::exit(2);
}
static bool VisibleStyle(HWND window) {
    return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
}
int main(int argc, char** argv) {
    if (argc > 3) SetProcessDPIAware();
    const int width = argc > 1 ? std::atoi(argv[1]) : 1000;
    const int height = argc > 2 ? std::atoi(argv[2]) : 740;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    MH_Initialize();
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA), reinterpret_cast<void*>(&UnexpectedA), nullptr);
    MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW), reinterpret_cast<void*>(&UnexpectedW), nullptr);
    MH_EnableHook(MH_ALL_HOOKS);
    auto* app = new MMDApp;
    g_Block = app;
    app->state = MMDAppState{};
    app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize hidden production host");
    HWND window = app->MainWindow();
    Check(!IsWindowVisible(window), "host stays hidden");
    SetWindowPos(window, nullptr, 80, 80, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    HWND extra = CreateWindowW(L"STATIC", L"visible extension", WS_CHILD | WS_VISIBLE,
        3, 3, 20, 20, window, reinterpret_cast<HMENU>(9001), nullptr, nullptr);
    HWND hidden = CreateWindowW(L"STATIC", L"hidden extension", WS_CHILD,
        3, 3, 20, 20, window, reinterpret_cast<HMENU>(9002), nullptr, nullptr);
    std::vector<std::pair<HWND, bool>> children;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        children.emplace_back(child, VisibleStyle(child));
    std::vector<RECT> controlBounds;
    for (const auto& child : children) { RECT rc{}; GetWindowRect(child.first, &rc); controlBounds.push_back(rc); }
    RECT before{}; GetWindowRect(window, &before);
    RECT beforeView = app->ViewportRect();
    const auto beforeParams = app->Renderer()->presentParameters;
    const auto beforeStyle = GetWindowLongPtrW(window, GWL_STYLE);
    const auto menu = GetMenu(window);
    const int sidebar = app->SidebarWidth();
    std::printf("NORMAL window=%ldx%ld viewport=%ldx%ld sidebar=%d dpi=%u children=%zu\n",
        before.right-before.left, before.bottom-before.top,
        beforeView.right-beforeView.left, beforeView.bottom-beforeView.top,
        sidebar, GetDpiForWindow(window), children.size());
    for (int cycle = 0; cycle < 2; ++cycle) {
        SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, LPARAM(1) << 29);
        Check(app->FullscreenMode() != 0, "Alt+Enter reaches fullscreen handler");
        Check(app->Renderer()->presentParameters.Windowed != FALSE, "interactive fullscreen preserves desktop display mode");
        Check(app->Renderer()->device->TestCooperativeLevel() == D3D_OK, "fullscreen device reset succeeds");
        Check(GetMenu(window) == nullptr, "fullscreen menu is removed");
        Check(!IsWindowVisible(window), "background check does not show its host");
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT full{}; GetWindowRect(window, &full);
        std::printf("FULLSCREEN bounds=%ld,%ld %ldx%ld backbuffer=%ux%u\n", full.left, full.top, full.right-full.left, full.bottom-full.top, app->Renderer()->presentParameters.BackBufferWidth, app->Renderer()->presentParameters.BackBufferHeight);
        Check(EqualRect(&full, &monitor.rcMonitor), "fullscreen covers the monitor in the current DPI coordinate space");
        bool allHidden = true;
        for (const auto& child : children) allHidden &= !VisibleStyle(child.first);
        Check(allHidden, "fullscreen hides every child control");
        ApplyFullscreenWindowState(app); // repeated entry must not overwrite the snapshot
        HandleWindowSize(app);
        LayoutViewportPanels(app);
        RelayoutSidebarControls(app);
        Check(app->SidebarWidth() == sidebar, "fullscreen resize preserves ordinary sidebar width");
        Check(!VisibleStyle(extra) && !VisibleStyle(hidden), "fullscreen relayout keeps extensions hidden");
        const RECT view = app->ViewportRect();
        const long long cross = static_cast<long long>(view.right-view.left)*app->RenderHeight() -
            static_cast<long long>(view.bottom-view.top)*app->RenderWidth();
        Check(std::llabs(cross) <= 2LL*(app->RenderWidth()+app->RenderHeight()), "fullscreen preserves scene aspect ratio");
        auto* device = app->Renderer()->device;
        IDirect3DSurface9* buffer = nullptr;
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &buffer)), "read fullscreen backbuffer");
        device->ColorFill(buffer, nullptr, D3DCOLOR_XRGB(255,255,255));
        FillFramePresentationMargins(app, device);
        D3DSURFACE_DESC desc{}; buffer->GetDesc(&desc);
        IDirect3DSurface9* resolved = nullptr;
        Check(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
            D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
            SUCCEEDED(device->StretchRect(buffer, nullptr, resolved, nullptr, D3DTEXF_NONE)), "resolve fullscreen backbuffer");
        IDirect3DSurface9* readback = nullptr;
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
            D3DPOOL_SYSTEMMEM, &readback, nullptr)) && SUCCEEDED(device->GetRenderTargetData(resolved, readback)), "read back margin pixels");
        D3DLOCKED_RECT pixels{}; readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY);
        if (view.left > 0 || view.top > 0)
            Check((*static_cast<const DWORD*>(pixels.pBits) & 0xffffff) == 0, "fullscreen bars contain black pixels without stale controls");
        readback->UnlockRect(); readback->Release(); resolved->Release(); buffer->Release();
        SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, (LPARAM(1)<<29) | (LPARAM(1)<<30));
        Check(app->FullscreenMode() != 0, "holding Alt+Enter does not toggle repeatedly");
        if (cycle == 0) SendMessageW(window, WM_SYSKEYDOWN, VK_RETURN, LPARAM(1)<<29);
        else SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(app->FullscreenMode() == 0, "Alt+Enter and Escape restore ordinary window");
        Check(!IsWindowVisible(window), "restored background host stays hidden");
        RECT restored{}; GetWindowRect(window, &restored);
        Check(EqualRect(&before, &restored) && EqualRect(&beforeView, &app->ViewportRect()), "ordinary window and viewport restore exact dimensions");
        Check(GetWindowLongPtrW(window, GWL_STYLE) == beforeStyle && GetMenu(window) == menu, "ordinary style and menu are restored");
        bool visibilityRestored = true;
        for (const auto& child : children) visibilityRestored &= VisibleStyle(child.first) == child.second;
        Check(visibilityRestored, "original control visibility is restored");
        bool positionsRestored = true;
        for (size_t i = 0; i < children.size(); ++i) {
            RECT rc{}; GetWindowRect(children[i].first, &rc);
            positionsRestored &= EqualRect(&rc, &controlBounds[i]) != FALSE;
        }
        Check(positionsRestored, "all controls restore their original positions and sizes");
        Check(app->Renderer()->presentParameters.BackBufferWidth == beforeParams.BackBufferWidth &&
            app->Renderer()->presentParameters.BackBufferHeight == beforeParams.BackBufferHeight,
            "ordinary backbuffer size is restored");
    }
    ShutdownCleanup(app);
    g_Block = nullptr;
    delete app;
    std::puts("Fullscreen window regressions passed");
}
