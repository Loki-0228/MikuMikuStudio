#include "manager.hpp"
#include "native_ui.hpp"
#include "mikudancestudio/plugin_runtime.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/build_profile.hpp"
#include <ShlObj.h>
#include <commctrl.h>

namespace mikudancestudio::plugins {
namespace {
std::unique_ptr<Manager> manager;
std::unique_ptr<NativeUi> ui;
MMDApp* owner = nullptr;
int toolbarHeight = 0;
bool refreshing = false, closing = false;
constexpr UINT kRefresh = WM_APP + 0x6a;
void LogMessage(const std::string& s) { runtime_log::Write("PLUGIN_ABI %s", s.c_str()); }
LRESULT CALLBACK Subclass(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_CLOSE && manager && manager->Ui().InCallback()) { PostMessageW(hwnd, WM_CLOSE, wp, lp); return 0; }
    if (message == kRefresh) { if (!closing && owner) Refresh(owner); return 0; }
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(hwnd, Subclass, id); Shutdown(); }
    return DefSubclassProc(hwnd, message, wp, lp);
}
}
void Initialize(MMDApp* app) {
    if (build_profile::original || !app || manager) return;
    const auto initialized = Boundary([&] {
        owner = app; closing = false;
        wchar_t data[32768]{}; auto dataLength = GetEnvironmentVariableW(L"LOCALAPPDATA", data, 32768);
        if (!dataLength || dataLength >= 32768 || !fs::path(data).is_absolute()) {
            if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, data))) throw std::runtime_error("LOCALAPPDATA unavailable");
        }
        std::wstring module(32768, 0); auto n = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())); if (!n || n >= module.size()) throw std::runtime_error("process path unavailable"); module.resize(n);
        manager = std::make_unique<Manager>(fs::path(data) / L"MMS" / L"plugins", LogMessage, [window = app->MainWindow()] { PostMessageW(window, kRefresh, 0, 0); });
        ui = std::make_unique<NativeUi>(manager->Ui(), app->MainWindow(), LogMessage);
        if (!SetWindowSubclass(app->MainWindow(), Subclass, 0x4d4d5301, 0)) throw std::runtime_error("plugin lifetime hook failed");
        manager->Load(fs::path(module).parent_path() / L"PLUGINS"); Refresh(app); return MMS_OK;
    });
    if (initialized != MMS_OK) {
        runtime_log::Write("PLUGIN_ABI initialization failed: %d", initialized);
        Shutdown();
    }
}
void Shutdown() {
    if (manager && manager->Ui().InCallback()) return;
    if (closing) return; closing = true;
    if (manager) manager->Stop();
    ui.reset(); manager.reset(); toolbarHeight = 0; owner = nullptr;
}
void Layout(MMDApp* app) {
    if (!ui || app != owner) return;
    Boundary([&] {
        RECT rect{}; GetClientRect(app->MainWindow(), &rect);
        int left = app->SidebarWidth() + 9;
        toolbarHeight = ui->Layout(left, std::max(0L, rect.right - left), std::max(0L, (rect.bottom - 217) / 3), !app->FullscreenMode() && !app->FloatingWindow());
        return MMS_OK;
    });
}
int ToolbarHeight(MMDApp* app) { return app == owner && !app->FullscreenMode() && !app->FloatingWindow() ? toolbarHeight : 0; }
void Refresh(MMDApp* app) {
    if (!ui || app != owner || refreshing || closing) return;
    refreshing = true; auto old = toolbarHeight;
    Boundary([&] { ui->Refresh(); Layout(app); if (toolbarHeight != old) HandleWindowSize(app); return MMS_OK; });
    refreshing = false;
}
bool HandleCommand(unsigned id) { bool handled = false; Boundary([&] { if (ui) handled = ui->Command(id); return MMS_OK; }); return handled; }
bool HandleShortcut(unsigned key, bool viewportFocused) {
    unsigned modifiers = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
    bool handled = false; Boundary([&] { if (ui) handled = ui->Shortcut(key, modifiers, viewportFocused); return MMS_OK; }); return handled;
}
bool HandleDialogMessage(MSG* message) { return ui && !closing && ui->DialogMessage(message); }
}
