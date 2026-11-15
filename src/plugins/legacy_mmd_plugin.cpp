#include "mikumikustudio/legacy_mmd_plugin.hpp"
#include "mikumikustudio/local_data_path.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/nvidia_antialias_menu.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "legacy_mmd_menu.hpp"
#include "legacy_mmd_image.hpp"
#include "loader_win32.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include <mms/legacy_camera.h>

#include <algorithm>
#include <memory>
#include <cmath>
#include <wrl/client.h>

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
// MMDPluginDLL1: scalar deleting destructor followed by 119 D3D9 methods.
// MMDPluginDLL2 appends their 119 post callbacks (upstream c55b2f4).
constexpr unsigned resetSlot = 17, postResetSlot = 136;
using ResetCallback = void(__cdecl*)(void*, D3DPRESENT_PARAMETERS*);
using PostResetCallback = void(__cdecl*)(void*, D3DPRESENT_PARAMETERS*, HRESULT&);
// Guard the native antialiasing family, which overlapped the old observer range.
static_assert(antialias::kFirstCommand + unsigned(antialias::Algorithm::Count) <=
              menu_command_ids::kLegacyObserverFirst &&
              antialias::kSmaaCommand < menu_command_ids::kLegacyObserverFirst &&
              antialias::kOffCommand < menu_command_ids::kLegacyObserverFirst &&
              antialias::kOutlineCommand < menu_command_ids::kLegacyObserverFirst &&
              antialias::kOutlineFxaaCommand < menu_command_ids::kLegacyObserverFirst &&
              antialias::kOutlineMsaaCommand < menu_command_ids::kLegacyObserverFirst &&
              nvidia_antialias_menu::kInstallCommand < menu_command_ids::kLegacyObserverFirst &&
              nvidia_antialias_menu::kUninstallCommand < menu_command_ids::kLegacyObserverFirst);
template<class Function>
Function Method(void* object, unsigned slot) {
    return reinterpret_cast<Function>((*static_cast<void***>(object))[slot]);
}

struct Session {
    std::string id;
    plugins::LoadedPackage code;
    void* object = nullptr;
    Destroy destroy = nullptr;
    unsigned abi = 3;
    std::string profile;
    MenuIds menuIds;
    bool started = false;
    bool retiring = false;
    bool deviceLost = false;
    HWND cameraCapture = nullptr;
    MmsLegacyCameraStateV1 cameraRead{};
    int cameraFrame = 0;
    bool rawEffekseerMenu = false;
    bool ownsDrawObserver = false;
    uint64_t beginCallbacks = 0, drawCallbacks = 0;
};

struct Host {
    MMDApp* app = nullptr;
    HWND window = nullptr;
    plugins::NativeLoader loader;
    std::vector<std::unique_ptr<Session>> packages;
    Session* active = nullptr;
    HHOOK hook = nullptr;
    MenuIdAllocator menuIds;
    bool busy = true;
    bool stopRequested = false;
    uint64_t cameraRevision = 0;
};
thread_local Host* host;
thread_local bool materialDraw = false;
struct EffekseerDrawObserver final : deformed_mesh_capture::DrawObserver {
    void Indexed(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT, const void*, unsigned) noexcept override;
    void Unsupported(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT) noexcept override {}
};
EffekseerDrawObserver effekseerDrawObserver;

void Log(const char* message) noexcept {
    runtime_log::Write("LEGACY_MMD %s", message);
    runtime_log::Trace("LEGACY_MMD %s", message);
}

void Log(const std::string& message) noexcept {
    Log(message.c_str());
}

bool CameraReady(const Host& current, const Session& package) noexcept {
    auto& app = *current.app;
    const auto capture = GetCapture();
    return package.started && !package.retiring && !current.stopRequested &&
        !app.sceneRecoveryPending && IsWindowEnabled(app.MainWindow()) && app.CameraMode() &&
        !app.state.followCameraEnabled && !app.PlaybackActive() &&
        !app.FrameStepPlayback() && !app.state.recordPlaybackStartPending &&
        !app.SceneMutationInProgress() && !bake::UiLocked() && !SceneSaveBlocksEdits() &&
        (!capture || capture == package.cameraCapture) && !app.FrameRangeDialog() &&
        !app.state.frameCopyDialog && !app.RecordingWindow() && !app.GravitySettingDialog() &&
        !app.GroundShadowColorDialog() && !app.AccessoryOrderArray() && !app.state.accessoryEditDialogOpen;
}
MmsLegacyCameraStateV1 CameraSnapshot(MMDApp& app) noexcept {
    MmsLegacyCameraStateV1 result{};
    result.byteSize = sizeof(result); result.version = MMS_LEGACY_CAMERA_API_VERSION;
    result.position[0] = app.CameraPositionX(); result.position[1] = app.CameraPositionY(); result.position[2] = app.CameraPositionZ();
    result.rotation[0] = app.CameraPitch(); result.rotation[1] = app.CameraYaw(); result.rotation[2] = app.CameraRoll();
    result.distance = app.CameraDistance();
    return result;
}
bool SamePose(const MmsLegacyCameraStateV1& a, const MmsLegacyCameraStateV1& b) noexcept {
    return !std::memcmp(a.position, b.position, sizeof(a.position)) &&
        !std::memcmp(a.rotation, b.rotation, sizeof(a.rotation)) && a.distance == b.distance;
}
MmsResult MMS_CALL CameraRead(MmsLegacyCameraStateV1* out) {
    if (!out || out->byteSize < sizeof(*out) || out->version != MMS_LEGACY_CAMERA_API_VERSION) return MMS_E_ARGUMENT;
    if (!host || !host->active || host->active->profile != "first_person_camera_v4") return MMS_E_NOT_READY;
    auto& package = *host->active;
    if (!CameraReady(*host, package)) return MMS_E_BUSY;
    auto state = CameraSnapshot(*host->app);
    state.flags = MMS_LEGACY_CAMERA_EDITABLE;
    state.revision = ++host->cameraRevision;
    package.cameraRead = state; package.cameraFrame = host->app->CurrentFrame();
    *out = state;
    return MMS_OK;
}
MmsResult MMS_CALL CameraEdit(const MmsLegacyCameraStateV1* expected, const MmsLegacyCameraStateV1* desired) {
    if (!expected || !desired || expected->byteSize < sizeof(*expected) || desired->byteSize < sizeof(*desired) ||
        expected->version != MMS_LEGACY_CAMERA_API_VERSION || desired->version != MMS_LEGACY_CAMERA_API_VERSION) return MMS_E_ARGUMENT;
    if (!host || !host->active || host->active->profile != "first_person_camera_v4") return MMS_E_NOT_READY;
    auto& package = *host->active;
    auto& app = *host->app;
    if (!CameraReady(*host, package)) return MMS_E_BUSY;
    if (!expected->revision || expected->revision != package.cameraRead.revision ||
        package.cameraFrame != app.CurrentFrame() || !SamePose(*expected, package.cameraRead) ||
        !SamePose(*expected, CameraSnapshot(app))) return MMS_E_STALE;
    for (const auto value : {desired->position[0], desired->position[1], desired->position[2],
            desired->rotation[0], desired->rotation[1], desired->rotation[2], desired->distance})
        if (!std::isfinite(value) || std::abs(value) > 1.e7f) return MMS_E_ARGUMENT;
    // No callbacks or message pumping between validation and the seven stores.
    app.CameraPositionX() = desired->position[0]; app.CameraPositionY() = desired->position[1]; app.CameraPositionZ() = desired->position[2];
    app.CameraPitch() = desired->rotation[0]; app.CameraYaw() = desired->rotation[1]; app.CameraRoll() = desired->rotation[2];
    app.CameraDistance() = desired->distance;
    package.cameraRead.revision = 0;
    app.SceneModified() = 1;
    PostViewRefresh(&app);
    updates::RequestViewport(&app, false);
    return MMS_OK;
}

bool Observed(const Host& current, const Session& package, const CWPSTRUCT& message) {
    if (message.message == WM_COMMAND) {
        return package.menuIds.ObservesCommand(LOWORD(message.wParam), HIWORD(message.wParam),
            message.lParam != 0, message.hwnd == current.window);
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
        if (package->started && package->abi >= 3) Method<Lifecycle>(package->object, stopSlot)(package->object);
        if (package->object) package->destroy(package->object);
        if (package->started) {
            if (package->profile == "effekseer_v2")
                Log("Effekseer callbacks begin=" + std::to_string(package->beginCallbacks) + " draw=" + std::to_string(package->drawCallbacks));
            runtime_log::Write("LEGACY_MMD stopped and destroyed (%s)", package->id.c_str());
            runtime_log::Trace("LEGACY_MMD stopped and destroyed (%s)", package->id.c_str());
        }
    } catch (...) {
        package->code.Pin();
        Log("teardown threw; retaining loaded code until exit");
    }
    if (package->cameraCapture && GetCapture() == package->cameraCapture) ReleaseCapture();
    if (package->rawEffekseerMenu) DeleteMenu(GetMenu(current.window), WM_APP + 1, MF_BYCOMMAND);
    if (package->profile == "effekseer_v2") {
        if (package->ownsDrawObserver) deformed_mesh_capture::StopObserving(&effekseerDrawObserver);
        using Clear = void(__cdecl*)();
        if (auto provider = GetModuleHandleW(L"MMDPlugin.dll"))
            if (auto clear = reinterpret_cast<Clear>(GetProcAddress(provider, "MMSClearLegacyHookBinders"))) clear();
    }
    if (HMENU menu = GetMenu(current.window); menu && package->menuIds.Any()) {
        package->menuIds.RemoveAll([menu](unsigned id) { DeleteMenu(menu, id, MF_BYCOMMAND); });
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
            if (!package || !package->started || package->abi != 3 || !Observed(*current, *package, original)) continue;
#if !MMD_ORIGINAL_PROFILE
            // A synchronous close from the observer must wait until it returns.
            const ScopedSceneCloseDeferral deferClose(*current->app);
#endif
            auto message = original;
            current->active = package.get();
            bool failed = false;
            try {
                Method<Observer>(package->object, observerSlot)(package->object, &message);
            } catch (...) {
                Log("window observer threw; stopping package");
                failed = true;
            }
            // Destroy the foreign exception before unloading its module.
            if (failed) Retire(*current, package);
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
        for (const auto& diagnostic : discovered.diagnostics) Log(diagnostic);
        std::vector<const plugins::Manifest*> packages;
        for (const auto index : plugins::Resolve(discovered, {})) {
            const auto& candidate = discovered.packages[index];
            if (candidate.kind == plugins::Manifest::Kind::LegacyMmdPlugin) packages.push_back(&candidate);
        }
        if (packages.empty()) return;

        const auto provider = plugins::LegacyMmdProviderFile(install);
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
            package->abi = manifest->major;
            package->profile = manifest->legacyProfile;
            host->active = package.get();
            bool failed = false;
            try {
                const auto image = plugins::PrepareLegacyMmdImage(*manifest, install);
                package->code = host->loader.Load(image.manifest, [](const std::string& message) { Log(message); }, provider, image.facade);
                const auto module = package->code.entry;
                for (const auto& other : host->packages)
                    if (other && other.get() != package.get() && (other->code.entry == module ||
                        ((package->profile == "effekseer_v2" || package->profile == "first_person_camera_v4") && other->profile == package->profile)))
                        throw std::runtime_error("legacy MMDPlugin DLL is already active in another package");
                const auto version = reinterpret_cast<Version>(GetProcAddress(module, "version"));
                const auto create = reinterpret_cast<Create>(GetProcAddress(module, package->abi == 2 ? "create2" : package->abi == 3 ? "create3" : "create4"));
                package->destroy = reinterpret_cast<Destroy>(GetProcAddress(module, package->abi == 2 ? "destroy2" : package->abi == 3 ? "destroy3" : "destroy4"));
                if (!version || !create || !package->destroy || version() != int(package->abi))
                    throw std::runtime_error("legacy MMDPlugin version does not match the selected profile factory and destroy exports");
                if (package->abi == 4) {
                    const auto adapter = reinterpret_cast<Version>(GetProcAddress(module, "MMSLegacyCameraAdapterVersion"));
                    if (!adapter || adapter() != MMS_LEGACY_CAMERA_API_VERSION)
                        throw std::runtime_error("unsupported MMS camera adapter API version");
                }
                if (host->stopRequested) throw std::runtime_error("host closed during plugin load");
                if (package->profile == "effekseer_v2" && GetMenuState(GetMenu(host->window), WM_APP + 1, MF_BYCOMMAND) != UINT(-1))
                    throw std::runtime_error("Effekseer status menu ID is already in use");
                package->rawEffekseerMenu = package->profile == "effekseer_v2";
                Microsoft::WRL::ComPtr<IDirect3DDevice9> renderDevice;
                auto* factoryDevice = app->Renderer()->device;
                if (package->profile == "effekseer_v2") {
                    // Effekseer submits its own shaders and draws. Give it the
                    // resource-owning device, so its draws do not re-enter the
                    // MME model-material proxy and acquire the model's shader.
                    if (FAILED(factoryDevice->QueryInterface(__uuidof(IDirect3DDevice9),
                            reinterpret_cast<void**>(renderDevice.GetAddressOf()))))
                        throw std::runtime_error("cannot acquire Effekseer render device");
                    factoryDevice = renderDevice.Get();
                }
                package->object = create(factoryDevice);
                if (!package->object) throw std::runtime_error("legacy MMDPlugin factory refused the host");
                if (package->profile == "effekseer_v2") {
                    std::string error;
                    if (!deformed_mesh_capture::Observe(*app, &effekseerDrawObserver, error))
                        throw std::runtime_error("cannot observe Effekseer material draws: " + error);
                    package->ownsDrawObserver = true;
                    const auto menu = GetMenu(host->window);
                    const auto id = host->menuIds.Allocate(package->menuIds, [menu](unsigned value) {
                        return GetMenuState(menu, value, MF_BYCOMMAND) != UINT(-1);
                    });
                    MENUITEMINFOW status{sizeof(status)};
                    status.fMask = MIIM_ID | MIIM_STATE; status.wID = unsigned(id); status.fState = MFS_DISABLED;
                    if (id < 0 || !SetMenuItemInfoW(menu, WM_APP + 1, FALSE, &status))
                        throw std::runtime_error("cannot adopt Effekseer status menu");
                    package->rawEffekseerMenu = false;
                    DrawMenuBar(host->window);
                }
                if (host->stopRequested) throw std::runtime_error("host closed during plugin creation");
                package->started = true;
                if (package->abi >= 3) Method<Lifecycle>(package->object, startSlot)(package->object);
                if (!host->stopRequested) Log(package->id + " started (" + package->profile + ")");
            } catch (const std::exception& error) {
                runtime_log::Write("LEGACY_MMD startup failed: %s (%s)", error.what(), manifest->id.c_str());
                runtime_log::Trace("LEGACY_MMD startup failed: %s (%s)", error.what(), manifest->id.c_str());
                failed = true;
            } catch (...) {
                Log("startup threw");
                failed = true;
            }
            if (failed) Retire(*host, package);
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

HRESULT ResetDevice(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) noexcept {
    if (!device || !parameters) return D3DERR_INVALIDCALL;
    auto* current = host;
    if (!current || !current->app->Renderer() || current->app->Renderer()->device != device)
        return device->Reset(parameters);
    if (std::none_of(current->packages.begin(), current->packages.end(),
        [](const auto& package) { return package && package->started && package->abi == 2; }))
        return device->Reset(parameters);
    // Do not recursively reset while foreign code is running or close is pending.
    if (current->busy || current->stopRequested) return D3DERR_INVALIDCALL;
#if !MMD_ORIGINAL_PROFILE
    const ScopedSceneCloseDeferral deferClose(*current->app);
#endif
    current->busy = true;
    for (auto& package : current->packages) {
        if (!package || !package->started || package->abi != 2) continue;
        if (package->profile == "effekseer_v2") package->deviceLost = true;
        current->active = package.get();
        bool failed = false;
        try { Method<ResetCallback>(package->object, resetSlot)(package->object, parameters); }
        catch (...) { Log("Reset callback threw; retiring package"); failed = true; }
        if (failed) Retire(*current, package);
        current->active = nullptr;
    }
    const HRESULT actual = device->Reset(parameters);
    for (auto& package : current->packages) {
        if (!package || !package->started || package->abi != 2) continue;
        // Effekseer's PostReset recreates resources without checking HRESULT.
        if (package->profile == "effekseer_v2" && FAILED(actual)) continue;
        current->active = package.get();
        // The observer profile exposes the real result to each package. Changes
        // to its local reference cannot disguise device loss to MMS or peers.
        HRESULT observed = actual;
        bool failed = false;
        try { Method<PostResetCallback>(package->object, postResetSlot)(package->object, parameters, observed); }
        catch (...) { Log("PostReset callback threw; retiring package"); failed = true; }
        if (failed) Retire(*current, package);
        else package->deviceLost = false;
        current->active = nullptr;
    }
    current->busy = false;
    if (current->stopRequested) Stop();
    return actual;
}

namespace {
template<class Function, class... Args>
void EffekseerCallback(IDirect3DDevice9* device, unsigned slot, Args&&... args) noexcept {
    auto* current = host;
    if (!current || current->busy || current->stopRequested || current->app->sceneRecoveryPending ||
        !current->app->Renderer() || current->app->Renderer()->device != device) return;
    current->busy = true;
    for (auto& package : current->packages) {
        if (!package || !package->started || package->deviceLost || package->profile != "effekseer_v2") continue;
        if (slot == 42) ++package->beginCallbacks;
        if (slot == 83) ++package->drawCallbacks;
        current->active = package.get();
        bool failed = false;
        try { Method<Function>(package->object, slot)(package->object, std::forward<Args>(args)...); }
        catch (...) { Log("Effekseer render callback threw; retiring package"); failed = true; }
        if (failed) Retire(*current, package);
        current->active = nullptr;
        if (current->stopRequested) break;
    }
    current->busy = false;
    if (current->stopRequested) Stop();
}
template<unsigned Slot, class Call>
HRESULT SceneCall(IDirect3DDevice9* device, Call call) noexcept {
    if (!device) return D3DERR_INVALIDCALL;
    if (!host) return call();
    const ScopedSceneCloseDeferral deferClose(*host->app);
    EffekseerCallback<void(__cdecl*)(void*)>(device, Slot);
    const HRESULT actual = call();
    HRESULT observed = actual;
    EffekseerCallback<void(__cdecl*)(void*, HRESULT&)>(device, Slot + 119, observed);
    return actual;
}
void EffekseerDrawObserver::Indexed(IDirect3DDevice9*, D3DPRIMITIVETYPE type, INT base,
        UINT minimum, UINT vertices, UINT start, UINT count, const void* object, unsigned material) noexcept {
    // Observe the native draw after MME has chosen its render target and shader.
    // The shared observer excludes recursive draws submitted by this callback.
    if (!host || !materialDraw || !object || material != 0 || !count) return;
    EffekseerCallback<void(__cdecl*)(void*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)>(
        host->app->Renderer()->device, 83, type, base, minimum, vertices, start, count);
}
}
HRESULT BeginScene(IDirect3DDevice9* device) noexcept {
    return SceneCall<42>(device, [device] { return device->BeginScene(); });
}
HRESULT EndScene(IDirect3DDevice9* device) noexcept {
    return SceneCall<43>(device, [device] { return device->EndScene(); });
}
HRESULT DrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base,
        UINT minimum, UINT vertices, UINT start, UINT count) noexcept {
    if (!device) return D3DERR_INVALIDCALL;
    if (!host) return device->DrawIndexedPrimitive(type, base, minimum, vertices, start, count);
    const ScopedSceneCloseDeferral deferClose(*host->app);
    struct DrawScope { bool previous = materialDraw; DrawScope() { materialDraw = true; } ~DrawScope() { materialDraw = previous; } } scope;
    return device->DrawIndexedPrimitive(type, base, minimum, vertices, start, count);
}

int ResolveEffekseerModel(const wchar_t* path, std::wstring& templatePath) noexcept {
    templatePath.clear();
    try {
        const std::filesystem::path source(path ? path : L"");
        if (plugins::Lower(source.extension().native()) != L".efk") return 0;
        if (!std::filesystem::is_regular_file(source)) { Log("Effekseer source file is missing"); return -1; }
        if (host && !host->stopRequested) for (const auto& package : host->packages) {
            if (!package || !package->started || package->profile != "effekseer_v2") continue;
            auto custom = source; custom.replace_extension(L".pmd");
            wchar_t modulePath[32768]{};
            if (!GetModuleFileNameW(package->code.entry, modulePath, 32768)) return -1;
            templatePath = (std::filesystem::is_regular_file(custom) ? custom :
                plugins::PackageFile(std::filesystem::path(modulePath).parent_path(), "efk.pmd")).native();
            return 1;
        }
        Log("Effekseer .efk import requires an enabled effekseer_v2 package");
        return -1;
    } catch (...) { Log("Effekseer template resolution failed"); return -1; }
}
const wchar_t* EffekseerFilename(const wchar_t* modelPath) {
    if (host && host->active && host->active->profile == "effekseer_v2" &&
        modelPath && plugins::Lower(std::filesystem::path(modelPath).extension().native()) == L".efk") {
        thread_local std::wstring result;
        result = modelPath;
        result += L".pmd.efk";
        return result.c_str();
    }
    return modelPath;
}
bool EffekseerContext() noexcept {
    return host && host->active && host->active->profile == "effekseer_v2";
}

bool HandleInput(HWND window, UINT message, WPARAM key, LPARAM data) noexcept {
    auto* current = host;
    if (!current || current->busy || current->stopRequested ||
        (window != current->window && window != current->app->FloatingWindow())) return false;
    const ScopedSceneCloseDeferral deferClose(*current->app);
    bool consumed = false;
    current->busy = true;
    for (auto& package : current->packages) {
        if (!package || !package->started || package->profile != "first_person_camera_v4") continue;
        const bool cancel = message == WM_CANCELMODE || message == WM_KILLFOCUS || message == WM_CAPTURECHANGED ||
            message == WM_INITMENU || message == WM_COMMAND || (message == WM_ACTIVATE && LOWORD(key) == WA_INACTIVE);
        bool release = cancel || !CameraReady(*current, *package) || (message == WM_KEYDOWN && key == VK_ESCAPE);
        const bool keyboard = message == WM_KEYDOWN || message == WM_KEYUP;
        bool dispatch = false;
        UINT mouseMessage = message;
        POINT point{short(LOWORD(data)), short(HIWORD(data))};
        if (release) { dispatch = package->cameraCapture != nullptr; mouseMessage = WM_CANCELMODE; }
        else if (keyboard) {
            const auto focus = GetFocus();
            const bool shortcut = ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) |
                GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
            dispatch = !shortcut && (!focus || focus == window) && (key == 'W' || key == 'A' || key == 'S' || key == 'D');
        } else if (message == WM_LBUTTONDOWN && !package->cameraCapture) {
            RECT bounds = current->app->ViewportRect();
            if (PtInRect(&bounds, point) && !(key & (MK_CONTROL | MK_SHIFT))) {
                package->cameraCapture = window; SetFocus(window); SetCapture(window);
                dispatch = GetCapture() == window;
                if (!dispatch) package->cameraCapture = nullptr;
            }
        } else if (package->cameraCapture == window && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP)) {
            dispatch = true;
            release = message == WM_LBUTTONUP || !(key & MK_LBUTTON);
            if (release) mouseMessage = WM_LBUTTONUP;
        }
        if (dispatch) {
            current->active = package.get();
            bool failed = false;
            try {
                if (keyboard && !release)
                    Method<void(__cdecl*)(void*, WPARAM, LPARAM)>(package->object, 247)(package->object, key,
                        message == WM_KEYUP ? data | (LPARAM(1) << 31) : data);
                else {
                    ClientToScreen(window, &point);
                    MOUSEHOOKSTRUCT mouse{point, window, HTCLIENT, 0};
                    Method<void(__cdecl*)(void*, WPARAM, MOUSEHOOKSTRUCT*)>(package->object, 245)(package->object, mouseMessage, &mouse);
                }
            } catch (...) { Log("camera input callback threw; retiring package"); failed = true; }
            if (failed) Retire(*current, package);
            current->active = nullptr;
            consumed = !cancel;
        }
        if (package && release) {
            const auto captured = package->cameraCapture;
            package->cameraCapture = nullptr;
            if (captured && GetCapture() == captured) ReleaseCapture();
        }
        if (current->stopRequested) break;
    }
    current->busy = false;
    if (current->stopRequested) Stop();
    return consumed;
}

const MmsLegacyCameraApiV1* CameraApi(uint32_t version) noexcept {
    static const MmsLegacyCameraApiV1 api{sizeof(api), MMS_LEGACY_CAMERA_API_VERSION, CameraRead, CameraEdit};
    return version == MMS_LEGACY_CAMERA_API_VERSION ? &api : nullptr;
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
    if (!host || !host->active || host->active->retiring || host->stopRequested) return -1;
    const HMENU menu = GetMenu(host->window);
    return host->menuIds.Allocate(host->active->menuIds, [menu](unsigned id) {
        return menu && GetMenuState(menu, id, MF_BYCOMMAND) != UINT(-1);
    });
}

void* RewriteFunction(const char* module, const char* function, void* replacement, int ordinal) noexcept {
    if (!host || !host->active || host->active->retiring || host->stopRequested ||
        host->active->profile != "effekseer_v2") return nullptr;
    // The audited plugin uses these hooks only to disguise .efk as a PMD.
    // MMS resolves .efk explicitly, preserving the real path for project saves.
    // No CRT/file/drop hook is installed; null is the upstream failure result.
    runtime_log::Write("LEGACY_MMD explicit .efk import replaces legacy file hook %s!%s",
        module ? module : "?", function ? function : "?");
    return nullptr;
}
}

extern "C" __declspec(dllexport) HWND __cdecl MMSLegacyMmdGetWindow() {
    return mikumikustudio::legacy_mmd_plugin::CurrentWindow();
}

extern "C" __declspec(dllexport) const MmsLegacyCameraApiV1* MMS_CALL MMSAcquireLegacyCameraApi(uint32_t version) {
    return mikumikustudio::legacy_mmd_plugin::CameraApi(version);
}

extern "C" __declspec(dllexport) int __cdecl MMSLegacyMmdAllocateMenuId() {
    return mikumikustudio::legacy_mmd_plugin::AllocateMenuId();
}

extern "C" __declspec(dllexport) void* __cdecl MMSLegacyMmdRewriteFunction(
        const char* module, const char* function, void* replacement, int ordinal) {
    return mikumikustudio::legacy_mmd_plugin::RewriteFunction(module, function, replacement, ordinal);
}
