#include "mikumikustudio/legacy_mmd_plugin.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "loader_win32.hpp"

#include <algorithm>
#include <bitset>
#include <memory>

namespace mikumikustudio::legacy_mmd_plugin {
namespace {
using Version = int(__cdecl*)();
using Create = void*(__cdecl*)(IDirect3DDevice9*);
using Destroy = void(__cdecl*)(void*);
using Lifecycle = void(__cdecl*)(void*);
using Observer = void(__cdecl*)(void*, CWPSTRUCT*);

// MSVC x64 MMDPluginDLL3 slots for the audited observer profile.
// Checked from compiler output for the read-only reference header and sample.
constexpr unsigned startSlot = 240, stopSlot = 241, observerSlot = 243;
// Keep these menu command IDs outside the native ABI's 0xa000..0xbfff range
// and MME's 40000-series commands. They are never reused within a host session.
constexpr unsigned firstMenuId = 0x8800, menuIdCount = 0x800;
template<class Function>
Function Method(void* object, unsigned slot) {
    return reinterpret_cast<Function>((*static_cast<void***>(object))[slot]);
}

struct Session {
    std::string id;
    plugins::LoadedPackage code;
    void* object = nullptr;
    Destroy destroy = nullptr;
    std::bitset<menuIdCount> menuIds;
    bool started = false;
    bool retiring = false;
};

struct Host {
    MMDApp* app = nullptr;
    HWND window = nullptr;
    plugins::NativeLoader loader;
    std::vector<std::unique_ptr<Session>> packages;
    Session* active = nullptr;
    HHOOK hook = nullptr;
    unsigned nextMenuId = firstMenuId;
    bool busy = true;
    bool stopRequested = false;
};
thread_local Host* host;

void Log(const char* message) noexcept {
    runtime_log::Write("LEGACY_MMD %s", message);
    runtime_log::Trace("LEGACY_MMD %s", message);
}

void Log(const std::string& message) noexcept {
    Log(message.c_str());
}

bool Observed(const Host& current, const Session& package, const CWPSTRUCT& message) {
    if (message.message == WM_COMMAND) {
        const unsigned id = LOWORD(message.wParam);
        return message.hwnd == current.window && !message.lParam && !HIWORD(message.wParam) &&
            id >= firstMenuId && id < firstMenuId + menuIdCount && package.menuIds[id - firstMenuId];
    }
    if (message.message != WM_SHOWWINDOW && message.message != WM_WINDOWPOSCHANGED &&
        message.message != WM_CTLCOLORSTATIC && message.message != WM_CTLCOLOREDIT) return false;
    const HWND floating = current.app->FloatingWindow();
    return message.hwnd == current.window || IsChild(current.window, message.hwnd) ||
           (floating && (message.hwnd == floating || IsChild(floating, message.hwnd)));
}

void Retire(Host& current, std::unique_ptr<Session>& package) noexcept {
    if (!package) return;
    current.active = package.get();
    package->retiring = true;
    try {
        if (package->started) Method<Lifecycle>(package->object, stopSlot)(package->object);
        if (package->object) package->destroy(package->object);
        if (package->started) {
            runtime_log::Write("LEGACY_MMD stopped and destroyed (%s)", package->id.c_str());
            runtime_log::Trace("LEGACY_MMD stopped and destroyed (%s)", package->id.c_str());
        }
    } catch (...) {
        package->code.Pin();
        Log("teardown threw; retaining loaded code until exit");
    }
    if (HMENU menu = GetMenu(current.window); menu && package->menuIds.any()) {
        for (unsigned i = 0; i < menuIdCount; ++i)
            if (package->menuIds[i]) DeleteMenu(menu, firstMenuId + i, MF_BYCOMMAND);
        DrawMenuBar(current.window);
    }
    current.active = nullptr;
    package.reset();
}

LRESULT CALLBACK ObserveMessage(int code, WPARAM sent, LPARAM data) {
    auto* current = host;
    if (code == HC_ACTION && data && current && !current->busy &&
        !current->stopRequested && !current->app->sceneRecoveryPending) {
        const auto& original = *reinterpret_cast<const CWPSTRUCT*>(data);
        current->busy = true;
        for (auto& package : current->packages) {
            if (!package || !package->started || !Observed(*current, *package, original)) continue;
#if !MMD_ORIGINAL_PROFILE
            // A synchronous close from the observer must wait until it returns.
            const ScopedSceneCloseDeferral deferClose(*current->app);
#endif
            auto message = original;
            current->active = package.get();
            try {
                Method<Observer>(package->object, observerSlot)(package->object, &message);
            } catch (...) {
                Log("window observer threw; stopping package");
                Retire(*current, package);
            }
            current->active = nullptr;
            if (current->stopRequested) break;
        }
        current->busy = false;
        if (current->stopRequested) Stop();
    }
    return CallNextHookEx(nullptr, code, sent, data);
}
}

void Start(MMDApp* app) noexcept {
    if (host || !app || !app->MainWindow() || !app->Renderer() || !app->Renderer()->device) return;
    if (GetWindowThreadProcessId(app->MainWindow(), nullptr) != GetCurrentThreadId()) return;
#if !MMD_ORIGINAL_PROFILE
    const ScopedSceneCloseDeferral deferClose(*app);
#endif
    try {
        const auto install = local_data::ExecutableDirectory();
        if (install.empty()) return;
        const auto discovered = plugins::Discover(install / L"PLUGINS");
        std::vector<const plugins::Manifest*> packages;
        for (const auto index : plugins::Resolve(discovered, {})) {
            const auto& candidate = discovered.packages[index];
            if (candidate.kind == plugins::Manifest::Kind::LegacyMmdPlugin) packages.push_back(&candidate);
        }
        if (packages.empty()) return;

        const auto provider = plugins::PackageFile(install, "legacy-mmdplugin/MMDPlugin.dll");
        auto current = std::make_unique<Host>();
        current->app = app;
        current->window = app->MainWindow();
        current->packages.reserve(packages.size());
        host = current.release();
        host->hook = SetWindowsHookExW(WH_CALLWNDPROC, ObserveMessage, nullptr, GetCurrentThreadId());
        if (!host->hook) throw std::runtime_error("legacy MMDPlugin window observer hook failed");
        for (const auto* manifest : packages) {
            auto& package = host->packages.emplace_back(std::make_unique<Session>());
            package->id = manifest->id;
            host->active = package.get();
            try {
                package->code = host->loader.Load(*manifest, [](const std::string& message) { Log(message); }, provider);
                const auto module = package->code.entry;
                for (const auto& other : host->packages)
                    if (other && other.get() != package.get() && other->code.entry == module)
                        throw std::runtime_error("legacy MMDPlugin DLL is already active in another package");
                const auto version = reinterpret_cast<Version>(GetProcAddress(module, "version"));
                const auto create = reinterpret_cast<Create>(GetProcAddress(module, "create3"));
                package->destroy = reinterpret_cast<Destroy>(GetProcAddress(module, "destroy3"));
                if (!version || !create || !package->destroy || version() != 3)
                    throw std::runtime_error("legacy MMDPlugin requires matching version 3 factory and destroy exports");
                if (host->stopRequested) throw std::runtime_error("host closed during plugin load");
                package->object = create(app->Renderer()->device);
                if (!package->object) throw std::runtime_error("legacy MMDPlugin factory refused the host");
                if (host->stopRequested) throw std::runtime_error("host closed during plugin creation");
                package->started = true;
                Method<Lifecycle>(package->object, startSlot)(package->object);
                if (!host->stopRequested) Log(package->id + " started (window_observer_v3)");
            } catch (const std::exception& error) {
                runtime_log::Write("LEGACY_MMD startup failed: %s (%s)", error.what(), manifest->id.c_str());
                runtime_log::Trace("LEGACY_MMD startup failed: %s (%s)", error.what(), manifest->id.c_str());
                Retire(*host, package);
            } catch (...) {
                Log("startup threw");
                Retire(*host, package);
            }
            host->active = nullptr;
            if (host->stopRequested) break;
        }
        host->busy = false;
        if (host->stopRequested || std::none_of(host->packages.begin(), host->packages.end(),
                                               [](const auto& package) { return bool(package); })) Stop();
    } catch (const std::exception& error) {
        runtime_log::Write("LEGACY_MMD startup failed: %s", error.what());
        runtime_log::Trace("LEGACY_MMD startup failed: %s", error.what());
        if (host) host->busy = false;
        Stop();
    } catch (...) {
        Log("startup threw");
        if (host) host->busy = false;
        Stop();
    }
}

void Stop() noexcept {
    auto* current = host;
    if (!current) return;
    current->stopRequested = true;
    if (current->busy) return;
    current->busy = true;
    if (current->hook) UnhookWindowsHookEx(current->hook);
    for (auto it = current->packages.rbegin(); it != current->packages.rend(); ++it) Retire(*current, *it);
    host = nullptr;
    delete current;
}

HWND CurrentWindow() noexcept { return host && IsWindow(host->window) ? host->window : nullptr; }

int AllocateMenuId() noexcept {
    if (!host || !host->active || host->active->retiring || host->stopRequested ||
        host->nextMenuId >= firstMenuId + menuIdCount) return -1;
    const auto id = host->nextMenuId++;
    host->active->menuIds[id - firstMenuId] = true;
    return static_cast<int>(id);
}
}

extern "C" __declspec(dllexport) HWND __cdecl MMSLegacyMmdGetWindow() {
    return mikumikustudio::legacy_mmd_plugin::CurrentWindow();
}

extern "C" __declspec(dllexport) int __cdecl MMSLegacyMmdAllocateMenuId() {
    return mikumikustudio::legacy_mmd_plugin::AllocateMenuId();
}
