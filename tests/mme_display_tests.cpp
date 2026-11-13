// MAINTENANCE: regression-keep; hidden real-MME display, EMD and material-order checks.
// Exercise the real mapping dialog without displaying or activating a window.
#define NOMINMAX
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/animation_curve_host.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/perf_timing.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/window_dpi_scope.hpp"
#include "mikumikustudio/scene_sampling.hpp"
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/pmm_paths.hpp"
#include "mikumikustudio/image_sequence_export.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/effect_display.hpp"
#include "mikumikustudio/effect_file_watch.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/antialias_motion.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/deformed_mesh_capture.hpp"
#include "../src/render/antialias_shader.hpp"
#include "../src/render/outline_multisample.hpp"
#include <atomic>
#include <MinHook.h>
#include <commctrl.h>
#include <ShObjIdl.h>
#include <commdlg.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <wrl/client.h>
#include <set>
#include <cwctype>

using namespace mikumikustudio;
namespace mikumikustudio { void RegisterCameraState(MMDApp*, int); void RegisterSelfShadowState(MMDApp*, int); void LayoutViewportPanels(MMDApp*); int ViewportToolAtPointForTest(MMDApp*); }
extern "C" char* ExpGetPmdFilename(int);
extern "C" int ExpGetCurrentObject();
namespace {
void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) std::exit(2);
}

unsigned emdEffectCompiles = 0;
unsigned effectPathDiagnostics = 0;
decltype(&runtime_log::WritePath) originalWritePath = nullptr;
void ObserveEffectPath(const char* label, const wchar_t* path) noexcept {
    if (std::strcmp(label, "MME_EFFECT_FILE") == 0 && path && *path) ++effectPathDiagnostics;
    originalWritePath(label, path);
}
struct ShaderMacro { const char* name; const char* definition; };
using CreateEffectFile = HRESULT(WINAPI*)(IDirect3DDevice9*, LPCWSTR,
    const ShaderMacro*, void*, DWORD, void*, void**, void**);
CreateEffectFile originalCompile = nullptr;
unsigned fileEffectCompiles = 0;
std::vector<IUnknown*> sequenceClockEffects;
HRESULT WINAPI ObserveCompile(IDirect3DDevice9* device, LPCWSTR path,
    const ShaderMacro* macros, void* include, DWORD flags, void* pool,
    void** effect, void** errors) {
    const HRESULT result = originalCompile(device, path, macros, include, flags, pool, effect, errors);
    std::string utf8; text_encoding::EncodeExact(path, CP_UTF8, utf8);
    std::printf("COMPILE path=%s result=%08lx\n", utf8.c_str(), static_cast<unsigned long>(result));
    if (std::filesystem::path(path).filename() == L"assigned.fx" && SUCCEEDED(result)) ++emdEffectCompiles;
    if (SUCCEEDED(result)) ++fileEffectCompiles;
    if (SUCCEEDED(result) && std::filesystem::path(path).filename() == L"sequence-clock.fx") {
        auto* object = static_cast<IUnknown*>(*effect); object->AddRef();
        sequenceClockEffects.push_back(object);
    }
    return result;
}
decltype(&ShowWindow) originalShow;
BOOL WINAPI Hide(HWND window, int) { return originalShow(window, SW_HIDE); }
LRESULT CALLBACK HideCreated(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* create = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(create->style & WS_CHILD)) {
            create->style &= ~WS_VISIBLE;
            create->x = create->y = -32000;
        }
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
int WINAPI Unexpected(HWND, LPCWSTR text, LPCWSTR, UINT type) {
    if (std::getenv("MMS_CAMERA_FIXTURE") && (type & MB_ICONMASK) == MB_ICONINFORMATION) {
        std::puts("CAMERA acknowledged project compatibility information"); return IDOK;
    }
    std::fwprintf(stderr, L"FAIL unexpected dialog: %ls\n", text); std::exit(2);
}
int WINAPI UnexpectedAnsi(HWND, LPCSTR text, LPCSTR, UINT) {
    std::string utf8; text_encoding::EncodeExact(text_encoding::Display(text).c_str(), CP_UTF8, utf8);
    std::fprintf(stderr, "FAIL unexpected dialog: %s\n", utf8.c_str()); std::exit(2);
}
int tick = 0, rowsSeen = 0;
bool done = false;
bool bilingualFixture = false;
HWND editor = nullptr;
using DrawIndexed = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
DrawIndexed originalDraw = nullptr;
std::vector<unsigned> drawnMaterials;
using Present = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Present originalPresent = nullptr;
bool inspectOutlines = false;
unsigned outlineDraws = 0, outlineStateErrors = 0;
DWORD expectedOutlineAA = TRUE, observedOutlineAA = 0, observedOutlineSamples = 0;
bool captureRequested = false;
std::vector<std::uint32_t> capturedPixels;
unsigned captureWidth = 0, captureHeight = 0;
std::filesystem::path captureDirectory;
HRESULT WINAPI ObservePresent(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                              HWND window, const RGNDATA* region) {
    if (captureRequested) {
        Microsoft::WRL::ComPtr<IDirect3DSurface9> back, resolved, cpu;
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)), "get actual pre-Present backbuffer");
        D3DSURFACE_DESC desc{}; Check(SUCCEEDED(back->GetDesc(&desc)), "read backbuffer description");
        Check(desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8, "BGRA comparison format");
        auto* surface = back.Get();
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            Check(SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE,
                0, FALSE, &resolved, nullptr)) && SUCCEEDED(device->StretchRect(surface, nullptr, resolved.Get(), nullptr, D3DTEXF_NONE)),
                "resolve actual backbuffer without rebinding");
            surface = resolved.Get();
        }
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
            D3DPOOL_SYSTEMMEM, &cpu, nullptr)) && SUCCEEDED(device->GetRenderTargetData(surface, cpu.Get())), "capture actual frame pixels");
        D3DLOCKED_RECT lock{}; Check(SUCCEEDED(cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY)), "lock comparison pixels");
        captureWidth = desc.Width; captureHeight = desc.Height;
        capturedPixels.resize(std::size_t(desc.Width) * desc.Height);
        for (unsigned y = 0; y < desc.Height; ++y) {
            const auto* row = reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(lock.pBits) + y * lock.Pitch);
            for (unsigned x = 0; x < desc.Width; ++x) capturedPixels[std::size_t(y) * desc.Width + x] = row[x] & 0xFFFFFF;
        }
        cpu->UnlockRect(); captureRequested = false;
    }
    return originalPresent(device, source, destination, window, region);
}

void CheckMmeSelfShadowCycle(MMDApp* app, bool expectMme) {
    Check(!app->antialiasing.enabled, "exact effect color fixture starts with AA disabled");
    Check(app->SelectedModel() != nullptr, "load the isolated effect fixture model");
    Check(expectMme ? emdEffectCompiles > 0 : emdEffectCompiles == 0,
          "only a real MME plugin loads the sidecar effect");
    app->EditMode() = ViewportEditMode::None;
    app->GroundGridEnabled() = app->GroundShadowEnabled() = app->FpsOverlayEnabled() = false;
    app->WireframeRenderingEnabled() = 0;
    app->ShadowDistance() = .005f;
    app->state.selfShadowEnabled = 1;
    app->SelfShadowMode() = 1;
    HandleWindowSize(app);
    Check(FrameViewportRenderable(app), "hidden MME fixture viewport is presentable");

    const auto frame = [&] {
        app->DeltaTime() = 1.f / 30;
        app->state.messageSeen = 10;
        FrameDriver(app);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    };
    const auto deadline = GetTickCount64() + 10000;
    do {
        frame();
        if (app->Renderer()->effect && app->Renderer()->hdrTexture) break;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    Check(app->Renderer()->effect && app->Renderer()->hdrTexture && app->SelfShadowMode() == 1,
          "native self-shadow renderer is ready before cycling modes");

    auto* present = (*reinterpret_cast<void***>(app->Renderer()->device))[17];
    Check(MH_CreateHook(present, &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
          MH_EnableHook(present) == MH_OK, "observe actual MME self-shadow cycle presentation");
    const auto capture = [&](int mode, bool enabled) {
        const auto presentedBefore = app->frameRate.presentedFrames;
        capturedPixels.clear();
        captureRequested = true;
        frame();
        Check(!captureRequested && !capturedPixels.empty(), "read the first real Present at the requested mode");
        Check(build_profile::original || app->frameRate.presentedFrames > presentedBefore,
              "the capture comes from a completed scene and successful Present");
        Check(app->SelfShadowMode() == mode && bool(app->state.selfShadowEnabled) == enabled &&
              app->UsesViewportTool(), "the frame retained the requested native self-shadow mode");
        if (enabled && mode != 0)
            Check(app->Renderer()->effect && app->Renderer()->hdrTexture,
                  "native self-shadow resources remain ready in the enabled mode");
        const auto magenta = std::count(capturedPixels.begin(), capturedPixels.end(), 0xFF00FFu);
        std::printf("MME_SELF_SHADOW size=%ux%u magenta=%zu\n", captureWidth, captureHeight, std::size_t(magenta));
        Check(expectMme ? magenta > 50 : magenta == 0,
              "shadow mode does not change whether the EMD-assigned color is drawn");
    };
    CheckMenuItem(GetMenu(app->MainWindow()), 279, MF_BYCOMMAND | MF_CHECKED);
    for (const int enabledMode : {1, 2}) {
        const int modes[] = {0, enabledMode, 0, enabledMode};
        for (int stage = 0; stage < 4; ++stage) {
            const int mode = modes[stage];
            // The radio buttons change the mode while leaving the global self-shadow switch on.
            const auto command = unsigned(562 + mode);
            CommandDispatch(GetDlgItem(app->MainWindow(), command), MAKEWPARAM(command, BN_CLICKED));
            std::printf("MME_SELF_SHADOW cycle=%d stage=%d mode=%d\n", enabledMode, stage, mode);
            capture(mode, true);
        }
        for (const bool enabled : {false, true}) {
            CommandDispatch(nullptr, MAKEWPARAM(279, 0));
            std::printf("MME_SELF_SHADOW global=%d mode=%d\n", int(enabled), enabledMode);
            capture(enabledMode, enabled);
        }
    }
    Check(MH_RemoveHook(present) == MH_OK, "remove MME self-shadow presentation hook");
    Check(!IsWindowVisible(app->MainWindow()), "MME self-shadow host stays hidden");
}

HRESULT WINAPI ObserveDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base, UINT min,
                            UINT vertices, UINT start, UINT primitives) {
    if (inspectOutlines && g_Block && g_Block->ActiveRenderPass() == AccessoryRenderPass::ModelOutline) {
        Microsoft::WRL::ComPtr<IDirect3DSurface9> target;
        D3DSURFACE_DESC desc{}; DWORD aa = 0, mask = 0;
        const bool queried = SUCCEEDED(device->GetRenderTarget(0, &target)) &&
            SUCCEEDED(target->GetDesc(&desc)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &aa)) &&
            SUCCEEDED(device->GetRenderState(D3DRS_MULTISAMPLEMASK, &mask));
        ++outlineDraws; observedOutlineAA = aa; observedOutlineSamples = desc.MultiSampleType;
        if (!queried || aa != expectedOutlineAA || desc.MultiSampleType == D3DMULTISAMPLE_NONE || mask != 0xFFFFFFFFu)
            ++outlineStateErrors;
    }
    if (g_Block && g_Block->ActiveRenderObject() == g_Block->SelectedModel() &&
        (g_Block->ActiveRenderPass() == AccessoryRenderPass::FixedFunction ||
         g_Block->ActiveRenderPass() == AccessoryRenderPass::Effect)) {
        const auto material = mdl::Mdl(g_Block->SelectedModel())->toonShared;
        if (drawnMaterials.empty() || drawnMaterials.back() != material) drawnMaterials.push_back(material);
    }
    return originalDraw(device, type, base, min, vertices, start, primitives);
}
void CheckOutlineAntialias(MMDApp* app) {
    app->EditMode() = ViewportEditMode::None;
    auto** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
    Check(MH_CreateHook(vtable[82], &ObserveDraw, reinterpret_cast<void**>(&originalDraw)) == MH_OK &&
          MH_EnableHook(vtable[82]) == MH_OK, "observe outline draw states");
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
          MH_EnableHook(vtable[17]) == MH_OK, "observe actual outline presentation");
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    app->CameraPositionX() = app->ViewOffsetX() = 0.f;
    app->CameraPositionY() = app->ViewOffsetY() = 18.f;
    app->CameraPositionZ() = 0.f; app->CameraDistance() = -16.f;
    app->CameraYaw() = app->CameraPitch() = app->CameraRoll() = 0.f;
    inspectOutlines = true;
    const auto frame = [&](const char* label, bool enabled) {
        const UINT menu = GetMenuState(GetMenu(app->MainWindow()), 277, MF_BYCOMMAND);
        Check(menu != UINT(-1) && ((menu & MF_CHECKED) != 0) == enabled, "AA menu matches requested state");
        expectedOutlineAA = enabled; outlineDraws = outlineStateErrors = 0;
        captureRequested = true;
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        std::printf("OUTLINE stage=%s aa=%lu samples=%lu draws=%u errors=%u size=%ux%u\n",
            label, observedOutlineAA, observedOutlineSamples, outlineDraws, outlineStateErrors, captureWidth, captureHeight);
        Check(outlineDraws > 0 && outlineStateErrors == 0, "every real outline draw retains AA selection and multisample target");
        Check(!captureRequested && !capturedPixels.empty(), "outline pixels captured at real Present");
        if (!captureDirectory.empty()) {
            std::filesystem::create_directories(captureDirectory);
            std::ofstream file(captureDirectory / (std::string(label) + ".bgra"), std::ios::binary);
            file.write(reinterpret_cast<const char*>(capturedPixels.data()), capturedPixels.size() * 4);
            Check(bool(file), "write outline comparison pixels");
        }
    };
    frame("initial", true);
    CommandDispatch(nullptr, MAKEWPARAM(277, 0)); frame("disabled", false);
    Check(PostDeviceReset(app), "reset with AA disabled"); frame("disabled-reset", false);
    CommandDispatch(nullptr, MAKEWPARAM(277, 0)); frame("enabled", true);
    Check(PostDeviceReset(app), "reset with AA enabled"); frame("enabled-reset", true);
    for (int shadow : {0, 1, 2}) {
        app->SelfShadowMode() = shadow;
        const auto label = std::string("shadow-") + std::to_string(shadow);
        frame(label.c_str(), true);
    }
    for (auto precision : {TransparentSortPrecision::Low, TransparentSortPrecision::Off}) {
        app->TransparentMaterialSortPrecision() = precision;
        frame(precision == TransparentSortPrecision::Off ? "sort-off" : "sort-low", true);
    }
    app->state.englishUI = 1; RefreshMenuLanguage(app); frame("language", true);
    SetWindowPos(app->MainWindow(), nullptr, 0, 0, 1200, 850,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Check(PostDeviceReset(app), "reset after resize"); frame("resized", true);
    inspectOutlines = false;
    MH_DisableHook(vtable[82]); MH_DisableHook(vtable[17]);
    Check(!IsWindowVisible(app->MainWindow()), "outline host stays hidden");
}
void CheckAlphaOrder(MMDApp* app) {
    const bool mmeLoaded = GetModuleHandleW(L"MMEffect.dll") != nullptr;
    auto** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
    auto* target = vtable[82]; // IDirect3DDevice9::DrawIndexedPrimitive
    Check(MH_CreateHook(target, &ObserveDraw, reinterpret_cast<void**>(&originalDraw)) == MH_OK &&
          MH_EnableHook(target) == MH_OK, "observe actual D3D material draws");
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
          MH_EnableHook(vtable[17]) == MH_OK, "observe actual pre-Present pixels");
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    for (int shadow : {0, 2}) {
        app->SelfShadowMode() = shadow;
        std::vector<std::uint32_t> firstOff;
        unsigned run = 0;
        for (auto precision : {TransparentSortPrecision::Off, TransparentSortPrecision::Low, TransparentSortPrecision::Off}) {
            app->TransparentMaterialSortPrecision() = precision;
            drawnMaterials.clear();
            captureRequested = true;
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
            std::printf("DRAW shadow=%d precision=%d:", shadow, int(precision));
            for (auto material : drawnMaterials) std::printf(" %u", material);
            std::puts("");
            const bool recordOrder = MMD_ORIGINAL_PROFILE ||
                (mmeLoaded && precision == TransparentSortPrecision::Off);
            const std::vector<unsigned> expected = recordOrder
                ? std::vector<unsigned>{0, 1} : std::vector<unsigned>{1, 0};
            Check(drawnMaterials == expected, "MME Off preserves authored material order; native and sorted passes retain their order");
            Check(!captureRequested && !capturedPixels.empty(), "capture came from a real Present");
            if (run == 0) firstOff = capturedPixels;
            if (run == 1 && mmeLoaded && !MMD_ORIGINAL_PROFILE)
                Check(firstOff != capturedPixels, "fixture visibly distinguishes authored and alpha-grouped material order");
            if (run == 2) Check(firstOff == capturedPixels, "On to Off preserves every frame RGB pixel");
            std::uint64_t hash = 14695981039346656037ull;
            for (auto pixel : capturedPixels) hash = (hash ^ pixel) * 1099511628211ull;
            std::printf("PIXELS shadow=%d run=%u precision=%d width=%u height=%u hash=%016llx\n",
                shadow, run, int(precision), captureWidth, captureHeight, static_cast<unsigned long long>(hash));
            if (!captureDirectory.empty()) {
                std::filesystem::create_directories(captureDirectory);
                const auto name = L"shadow-" + std::to_wstring(shadow) + L"-run-" + std::to_wstring(run) + L".bgra";
                std::ofstream file(captureDirectory / name, std::ios::binary);
                file.write(reinterpret_cast<const char*>(capturedPixels.data()), capturedPixels.size() * 4);
                Check(bool(file), "write actual comparison pixels");
            }
            ++run;
        }
    }
    MH_DisableHook(target);
    MH_DisableHook(vtable[17]);
    Check(!IsWindowVisible(app->MainWindow()), "material-order host stays hidden");
}
// --- optional MMHack GetToonTexture return probe ---------------------------
// MMS_MME_TOON_TRACE=1 observes the GetToonTexture export of the MMHack.dll
// that the runner actually loaded. The detour calls the original exactly once,
// returns the same borrowed texture without AddRef, and only reads level 0
// through a D3DLOCK_READONLY lock. Nothing is copied, replaced or re-bound, and
// no render or device state is touched. Records are deduplicated per
// (model, subset, drawType, texture) and capped for the whole process. The
// read-only lock perturbs timing, so the records are diagnostic only and are
// not a performance measurement.
constexpr unsigned kToonTraceLimit = 128;
using GetToonTextureExport = IDirect3DTexture9* (WINAPI*)();
using ToonTraceModelExport = std::uint64_t (WINAPI*)();
using ToonTraceSubsetExport = std::uint32_t (WINAPI*)();
using ToonTraceDrawTypeExport = std::uint32_t (WINAPI*)();
GetToonTextureExport originalToonTexture = nullptr;
ToonTraceModelExport toonTraceModel = nullptr;
ToonTraceSubsetExport toonTraceSubset = nullptr;
ToonTraceDrawTypeExport toonTraceDrawType = nullptr;
struct ToonTraceKey {
    std::uint64_t model;
    std::uint32_t subset;
    std::uint32_t drawType;
    std::uintptr_t texture;
};
ToonTraceKey toonTraceKeys[kToonTraceLimit]{};
unsigned toonTraceCount = 0;

bool MmeToonTraceEnabled() {
    const char* value = std::getenv("MMS_MME_TOON_TRACE");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool ToonTraceShouldSkip(std::uint64_t model, std::uint32_t subset,
                         std::uint32_t drawType, IDirect3DTexture9* texture) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(texture);
    for (unsigned i = 0; i < toonTraceCount; ++i) {
        const auto& seen = toonTraceKeys[i];
        if (seen.model == model && seen.subset == subset &&
            seen.drawType == drawType && seen.texture == address) return true;
    }
    if (toonTraceCount >= kToonTraceLimit) return true;
    toonTraceKeys[toonTraceCount++] = {model, subset, drawType, address};
    return false;
}

std::uint64_t ToonTraceLevelHash(const D3DLOCKED_RECT& locked, unsigned width, unsigned height) {
    std::uint64_t hash = 14695981039346656037ull;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    const auto* base = static_cast<const unsigned char*>(locked.pBits);
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = base + static_cast<std::size_t>(y) * static_cast<std::size_t>(locked.Pitch);
        for (std::size_t i = 0; i < rowBytes; ++i) hash = (hash ^ row[i]) * 1099511628211ull;
    }
    return hash;
}

IDirect3DTexture9* WINAPI TraceGetToonTexture() {
    IDirect3DTexture9* texture = originalToonTexture();  // exactly once; returned unchanged below
    const std::uint64_t model = toonTraceModel();
    const std::uint32_t subset = toonTraceSubset();
    const std::uint32_t drawType = toonTraceDrawType();
    if (ToonTraceShouldSkip(model, subset, drawType, texture)) return texture;

    // ExpGetCurrentObject is the host's combined model/accessory order; 0 means
    // the effect engine has no current object. The MMHack model id above is
    // recorded as data only and is never interpreted as a pointer.
    const int currentObject = ExpGetCurrentObject();
    char levelResult[16] = "skip";
    char surfaceResult[16] = "skip";
    char lockResult[16] = "skip";
    char hashText[32] = "null";
    char firstPixel[16] = "skip";
    char lastPixel[16] = "skip";
    unsigned width = 0, height = 0;
    int format = 0, pool = 0, pitch = 0;
    if (texture != nullptr) {
        D3DSURFACE_DESC desc{};
        const HRESULT level = texture->GetLevelDesc(0, &desc);
        std::snprintf(levelResult, sizeof(levelResult), "0x%08lx", static_cast<unsigned long>(level));
        if (SUCCEEDED(level)) {
            width = desc.Width;
            height = desc.Height;
            format = static_cast<int>(desc.Format);
            pool = static_cast<int>(desc.Pool);
        }
        Microsoft::WRL::ComPtr<IDirect3DSurface9> surface;
        const HRESULT surfaceLevel = texture->GetSurfaceLevel(0, surface.GetAddressOf());
        std::snprintf(surfaceResult, sizeof(surfaceResult), "0x%08lx",
            static_cast<unsigned long>(surfaceLevel));
        if (SUCCEEDED(surfaceLevel) && surface) {
            D3DLOCKED_RECT locked{};
            const HRESULT lock = surface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
            std::snprintf(lockResult, sizeof(lockResult), "0x%08lx", static_cast<unsigned long>(lock));
            if (SUCCEEDED(lock)) {
                pitch = static_cast<int>(locked.Pitch);
                const bool bgra = SUCCEEDED(level) &&
                    (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8);
                if (!SUCCEEDED(level)) {
                    std::snprintf(hashText, sizeof(hashText), "descFailed");
                } else if (!bgra) {
                    std::snprintf(hashText, sizeof(hashText), "unsupportedFormat");
                } else if (width == 0 || height == 0 ||
                           locked.Pitch < static_cast<int>(static_cast<std::size_t>(width) * 4)) {
                    std::snprintf(hashText, sizeof(hashText), "invalidLayout");
                } else {
                    std::snprintf(hashText, sizeof(hashText), "0x%016llx",
                        static_cast<unsigned long long>(ToonTraceLevelHash(locked, width, height)));
                    const auto* base = static_cast<const unsigned char*>(locked.pBits);
                    DWORD first = 0, last = 0;
                    std::memcpy(&first, base, sizeof(first));
                    std::memcpy(&last, base + static_cast<std::size_t>(height - 1) * locked.Pitch +
                        static_cast<std::size_t>(width - 1) * 4, sizeof(last));
                    std::snprintf(firstPixel, sizeof(firstPixel), "0x%08lx",
                        static_cast<unsigned long>(first));
                    std::snprintf(lastPixel, sizeof(lastPixel), "0x%08lx",
                        static_cast<unsigned long>(last));
                }
                surface->UnlockRect();
            } else {
                std::snprintf(hashText, sizeof(hashText), "lockFailed");
            }
        } else {
            std::snprintf(hashText, sizeof(hashText), "surfaceFailed");
        }
    }
    std::printf("MME_TOON_TRACE model=%llu object=%d subset=%u drawType=%u texture=%p "
                "levelHR=%s width=%u height=%u format=%d pool=%d surfaceHR=%s lockHR=%s "
                "pitch=%d hash=%s first=%s last=%s\n",
        static_cast<unsigned long long>(model), currentObject,
        static_cast<unsigned>(subset), static_cast<unsigned>(drawType),
        static_cast<void*>(texture), levelResult, width, height, format, pool,
        surfaceResult, lockResult, pitch, hashText, firstPixel, lastPixel);
    return texture;
}

void InstallMmeToonTrace() {
    HMODULE hack = GetModuleHandleW(L"MMHack.dll");
    Check(hack != nullptr, "toon trace requires the real MMHack.dll");
    auto* target = reinterpret_cast<void*>(GetProcAddress(hack, "GetToonTexture"));
    toonTraceModel = reinterpret_cast<ToonTraceModelExport>(GetProcAddress(hack, "GetCurrentModelID"));
    toonTraceSubset = reinterpret_cast<ToonTraceSubsetExport>(GetProcAddress(hack, "GetCurrentSubsetIndex"));
    toonTraceDrawType = reinterpret_cast<ToonTraceDrawTypeExport>(GetProcAddress(hack, "GetCurrentDrawType"));
    Check(target != nullptr && toonTraceModel != nullptr && toonTraceSubset != nullptr &&
          toonTraceDrawType != nullptr, "resolve MMHack toon-trace exports");
    Check(MH_CreateHook(target, &TraceGetToonTexture, reinterpret_cast<void**>(&originalToonTexture)) == MH_OK &&
          MH_EnableHook(target) == MH_OK, "hook MMHack GetToonTexture for toon trace");
}
DrawIndexed surfaceDraw = nullptr;
std::set<std::pair<void*, int>> surfaceShaders;
std::set<unsigned> hiddenSurfaceMaterials;
unsigned blockedSurfaceDx12=0;
decltype(&LoadLibraryW) surfaceLoadLibrary=nullptr;
HMODULE WINAPI SurfaceLoadLibrary(LPCWSTR path){
    const wchar_t* name=std::wcsrchr(path,L'\\');name=name?name+1:path;
    if(_wcsicmp(name,L"d3d12.dll")==0){++blockedSurfaceDx12;SetLastError(ERROR_MOD_NOT_FOUND);return nullptr;}
    return surfaceLoadLibrary(path);
}
HRESULT WINAPI InspectSurfaceDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT base,
    UINT min, UINT vertices, UINT start, UINT primitives) {
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> shader;
    device->GetPixelShader(&shader);
    const int pass = int(g_Block->ActiveRenderPass());
    const auto* activeModel=mdl::Mdl(g_Block->SelectedModel());
    if(activeModel && hiddenSurfaceMaterials.count(activeModel->toonShared))return S_OK;
    static bool dumped=false;
    if(pass==2 && !dumped) {
        dumped=true;
        Microsoft::WRL::ComPtr<IDirect3DVertexBuffer9> vb;
        Microsoft::WRL::ComPtr<IDirect3DIndexBuffer9> ib;
        UINT offset=0,stride=0;device->GetStreamSource(0,&vb,&offset,&stride);device->GetIndices(&ib);
        D3DVERTEXBUFFER_DESC vd{};D3DINDEXBUFFER_DESC id{};vb->GetDesc(&vd);ib->GetDesc(&id);
        void* data=nullptr;
        if(SUCCEEDED(vb->Lock(0,0,&data,D3DLOCK_READONLY))) {
            std::ofstream f(captureDirectory/"vertices.bin",std::ios::binary);f.write(static_cast<char*>(data),vd.Size);vb->Unlock();
        }
        if(SUCCEEDED(ib->Lock(0,0,&data,D3DLOCK_READONLY))) {
            std::ofstream f(captureDirectory/"indices.bin",std::ios::binary);f.write(static_cast<char*>(data),id.Size);ib->Unlock();
        }
        std::ofstream meta(captureDirectory/"geometry.txt");meta<<stride<<" "<<offset<<" "<<vertices<<" "<<int(id.Format)<<"\n";
        float constants[256][4]{};device->GetVertexShaderConstantF(0,constants[0],256);
        std::ofstream f(captureDirectory/"vs-constants.bin",std::ios::binary);f.write(reinterpret_cast<char*>(constants),sizeof(constants));
        D3DMATRIX m[3]{};device->GetTransform(D3DTS_WORLD,&m[0]);device->GetTransform(D3DTS_VIEW,&m[1]);device->GetTransform(D3DTS_PROJECTION,&m[2]);
        std::ofstream matrices(captureDirectory/"transforms.bin",std::ios::binary);matrices.write(reinterpret_cast<char*>(m),sizeof(m));
    }
    if(captureRequested) {
        const auto* model=mdl::Mdl(g_Block->SelectedModel());
        std::ofstream log(captureDirectory/"draws.csv",std::ios::app);
        log<<g_Block->SelfShadowMode()<<","<<g_Block->CameraYaw()<<","<<pass<<","<<model->toonShared<<","<<start<<","<<primitives<<"\n";
    }
    if (surfaceShaders.emplace(shader.Get(), pass).second) {
        DWORD z=0,write=0,cull=0,aa=0,blend=0,fill=0;
        device->GetRenderState(D3DRS_ZENABLE,&z);device->GetRenderState(D3DRS_ZWRITEENABLE,&write);
        device->GetRenderState(D3DRS_CULLMODE,&cull);device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS,&aa);
        device->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend);device->GetRenderState(D3DRS_FILLMODE,&fill);
        std::printf("SURFACE DRAW shader=%zu pass=%d shadow=%d start=%u faces=%u z=%lu write=%lu cull=%lu aa=%lu blend=%lu fill=%lu\n",
            surfaceShaders.size(),pass,g_Block->SelfShadowMode(),start,primitives,z,write,cull,aa,blend,fill);
        if (shader) {
            UINT size=0;shader->GetFunction(nullptr,&size);std::vector<DWORD> bytes((size+3)/4);shader->GetFunction(bytes.data(),&size);
            using Disassemble=HRESULT(WINAPI*)(const DWORD*,BOOL,LPCSTR,void**);
            auto disassemble=reinterpret_cast<Disassemble>(GetProcAddress(d3dx::Get().module,"D3DXDisassembleShader"));
            void* assembly=nullptr;
            if (disassemble && SUCCEEDED(disassemble(bytes.data(),FALSE,nullptr,&assembly))) {
                using Data=const char*(WINAPI*)(void*);using Size=DWORD(WINAPI*)(void*);
                std::ofstream file(captureDirectory/("shader-"+std::to_string(surfaceShaders.size())+".txt"));
                file.write(d3dx::EffectMethod<Data,3>(assembly)(assembly),d3dx::EffectMethod<Size,4>(assembly)(assembly));
                static_cast<IUnknown*>(assembly)->Release();
            }
        }
    }
    DWORD previousCull=0;
    const bool singleSided=pass==2 && std::getenv("MMS_SURFACE_SINGLE_SIDED");
    if(singleSided){device->GetRenderState(D3DRS_CULLMODE,&previousCull);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW);}
    const HRESULT result=surfaceDraw(device,type,base,min,vertices,start,primitives);
    if(singleSided)device->SetRenderState(D3DRS_CULLMODE,previousCull);
    return result;
}
void CheckSurfaceGrid(MMDApp* app) {
    unsigned width=3840,height=2160;
    if(const char* size=std::getenv("MMS_SURFACE_SIZE"))Check(std::sscanf(size,"%u,%u",&width,&height)==2,"read test resolution");
    const auto* testModel=mdl::Mdl(app->SelectedModel());
    for(unsigned i=0;i<testModel->materialCount;++i){
        auto name=text_encoding::MaterialName(*testModel,i,false);
        auto english=text_encoding::MaterialName(*testModel,i,true);
        std::string utf8;text_encoding::EncodeExact((name+L" / "+english).c_str(),CP_UTF8,utf8);
        std::printf("MATERIAL %u %s\n",i,utf8.c_str());
        std::transform(english.begin(),english.end(),english.begin(),[](wchar_t ch){return std::towlower(ch);});
        if(std::getenv("MMS_SURFACE_HIDE_HAIR") && (name.find(L"髪")!=std::wstring::npos || english.find(L"hair")!=std::wstring::npos))hiddenSurfaceMaterials.insert(i);
    }
    auto* renderer=app->Renderer();
    app->RenderWidth()=width;app->RenderHeight()=height;
    renderer->presentParameters.BackBufferWidth=renderer->screenWidth=width;
    renderer->presentParameters.BackBufferHeight=renderer->screenHeight=height;
    if(const char* quality=std::getenv("MMS_SURFACE_AA_QUALITY"))renderer->presentParameters.MultiSampleQuality=std::atoi(quality);
    Check(PostDeviceReset(app),"create 3840x2160 production backbuffer");
    std::printf("SURFACE AA type=%u quality=%lu depth=%u\n",unsigned(renderer->presentParameters.MultiSampleType),renderer->presentParameters.MultiSampleQuality,unsigned(renderer->presentParameters.AutoDepthStencilFormat));
    auto** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
    Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
          MH_EnableHook(vtable[17]) == MH_OK, "observe surface presentation");
    app->EditMode() = ViewportEditMode::Camera;
    app->GroundGridEnabled() = false; app->GroundShadowEnabled() = false;
    app->CameraPositionX() = app->ViewOffsetX() = 0.f;
    app->CameraPositionY() = app->ViewOffsetY() = 10.f;
    app->CameraPositionZ() = 0.f; app->CameraDistance() = -28.f;
    app->CameraPitch() = app->CameraRoll() = 0.f;
    if (const char* camera = std::getenv("MMS_SURFACE_CAMERA")) {
        float x=0,y=10,z=0,d=-28;
        Check(std::sscanf(camera,"%f,%f,%f,%f",&x,&y,&z,&d)==4,"read test camera");
        app->CameraPositionX()=app->ViewOffsetX()=x;
        app->CameraPositionY()=app->ViewOffsetY()=y;
        app->CameraPositionZ()=z;app->CameraDistance()=d;
    }
    if (std::getenv("MMS_SURFACE_NO_EDGE")) mdl::Mdl(app->SelectedModel())->edgeScale=0;
    if (std::getenv("MMS_SURFACE_NO_AA")) CommandDispatch(nullptr,MAKEWPARAM(277,0));
    app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    std::filesystem::create_directories(captureDirectory);
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    auto library=LoadLibraryW((std::filesystem::path(system)/L"d3d9.dll").c_str());
    auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(library,"Direct3DCreate9"));
    Microsoft::WRL::ComPtr<IDirect3D9> raw;raw.Attach(create(D3D_SDK_VERSION));
    Microsoft::WRL::ComPtr<IDirect3DDevice9> dummy;
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=app->MainWindow();
    pp.BackBufferWidth=pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    Check(SUCCEEDED(raw->CreateDevice(0,D3DDEVTYPE_HAL,app->MainWindow(),D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dummy)),"obtain native surface draw entry");
    auto** native=*reinterpret_cast<void***>(dummy.Get());
    Check(MH_CreateHook(native[82],&InspectSurfaceDraw,reinterpret_cast<void**>(&surfaceDraw))==MH_OK && MH_EnableHook(native[82])==MH_OK,"inspect actual surface shaders");
    for (int shadow : {0, 2}) for (int yaw : {0, 1, 2}) {
        app->SelfShadowMode() = shadow;
        const char* baseYaw=std::getenv("MMS_SURFACE_YAW");
        app->CameraYaw() = (baseYaw?float(std::atof(baseYaw)):0.f) + yaw * .035f;
        for (int i = 0; i < 3; ++i) {
            const bool legacy=std::getenv("MMS_SURFACE_LEGACY_VIEWPORT")!=nullptr;
            D3DVIEWPORT9 viewport=legacy?D3DVIEWPORT9{312,25,618,464,0,1}:D3DVIEWPORT9{0,0,width,height,0,1};
            app->ViewportRect()={LONG(viewport.X),LONG(viewport.Y),LONG(viewport.X+viewport.Width),LONG(viewport.Y+viewport.Height)};
            renderer->aspectRatio=float(viewport.Width)/viewport.Height;
            Check(SUCCEEDED(renderer->device->SetViewport(&viewport)),"install 4K viewport");
            app->EditMode()=ViewportEditMode::Camera;
            app->state.selfShadowEnabled=shadow!=0;
            captureRequested = i == 2;
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        }
        Check(!captureRequested && !capturedPixels.empty(), "capture surface pixels");
        const auto name = "surface-" + std::to_string(shadow) + "-" + std::to_string(yaw);
        std::ofstream file(captureDirectory / (name + ".bgra"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(capturedPixels.data()), capturedPixels.size() * 4);
        std::ofstream size(captureDirectory / (name + ".txt"));
        size << captureWidth << " " << captureHeight;
        std::printf("SURFACE shadow=%d yaw=%d width=%u height=%u\n", shadow, yaw, captureWidth, captureHeight);
    }
    MH_DisableHook(vtable[17]);
    MH_DisableHook(native[82]);
    const auto status=skinning::GetStatus();
    std::printf("SURFACE SKINNING cpu=%llu gpu=%llu blockedDx12=%u\n",static_cast<unsigned long long>(status.cpuUpdates),static_cast<unsigned long long>(status.gpuUpdates),blockedSurfaceDx12);
    if(std::getenv("MMS_SURFACE_MISSING_DX12"))Check(blockedSurfaceDx12>0 && status.gpuUpdates==0 && status.cpuUpdates>0,"real missing-DX12 CPU fallback rendered model");
    dummy.Reset();raw.Reset();FreeLibrary(library);
}
std::uint64_t PaintHash(HWND list) {
    RECT rect{}; GetClientRect(list, &rect);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr); void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Check(bitmap && pixels, "allocate control readback");
    HGDIOBJ old = SelectObject(dc, bitmap);
    SendMessageW(list, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    std::uint64_t hash = 14695981039346656037ull;
    const auto* bytes = static_cast<unsigned char*>(pixels);
    for (int i = 0; i < rect.right * rect.bottom * 4; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
    return hash;
}
void CheckDisplay(HWND list) {
    wchar_t original[1024]{};
    LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = 2;
    item.pszText = original; item.cchTextMax = 1024;
    Check(SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)), "read original material item");
    const auto identity = item.lParam;
    const std::wstring stored = original;
    auto* model = mdl::Mdl(g_Block->SelectedModel());
    const auto native = text_encoding::MaterialName(*model, 0, false);
    const auto english = text_encoding::MaterialName(*model, 0, true);
    SetModelInfoLanguagePreference(kModelInfoLanguageEnglish);
    SetModelNameAutoTranslation(false);
    Check(EffectListDisplayLabel(g_Block, list, 1) == L"Display Model", "MME model label uses metadata");
    Check(EffectListDisplayLabel(g_Block, list, 2) == L"  `-- 0: CustomArm", "English material metadata is displayed");
    const auto rawHash = PaintHash(list);
    SendMessageW(g_Block->MainWindow(), WM_COMMAND, 311, 0);
    Check(gModelNameAutoTranslation, "enable translation through actual menu");
    Check(EffectListDisplayLabel(g_Block, list, 2) == L"  `-- 0: arm_L", "MME displays dictionary translation");
    const auto translatedHash = PaintHash(list);
    Check(rawHash != translatedHash, "actual MME control pixels change with translated labels");
    SendMessageW(g_Block->MainWindow(), WM_COMMAND, 311, 0);
    Check(!gModelNameAutoTranslation, "translation remains switchable with PMX and MME loaded");
    Check(PaintHash(list) == rawHash, "disabling translation restores original display pixels");
    SetModelInfoLanguagePreference(kModelInfoLanguageAuto);
    g_Block->EnglishUI() = kUiChinese;
    Check(EffectListDisplayLabel(g_Block, list, 2) == L"  `-- 0: 左腕", "Chinese auto preference uses Japanese material name");
    wchar_t finalText[1024]{}; item.pszText = finalText;
    SendMessageW(list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
    Check(stored == finalText && item.lParam == identity, "MME stored text and assignment identity remain unchanged");
    Check(text_encoding::MaterialName(*model, 0, false) == native &&
          text_encoding::MaterialName(*model, 0, true) == english,
          "raw model material names remain unchanged");
    Check((GetMenuState(GetMenu(g_Block->MainWindow()), 311, MF_BYCOMMAND) & (MF_GRAYED | MF_DISABLED)) == 0,
          "translation preference is enabled after loading PMX with MME");
    std::printf("DISPLAY hashes original=%llx translated=%llx\n", rawHash, translatedHash);
}
void CALLBACK Probe(HWND, UINT, UINT_PTR timer, DWORD) {
    ++tick;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        HMENU menu = GetMenu(window);
        if (menu && GetMenuState(menu, 40028, MF_BYCOMMAND) != UINT(-1) &&
            GetMenuState(menu, 40007, MF_BYCOMMAND) != UINT(-1)) editor = window;
        return TRUE;
    }, 0);
    if (editor) {
        EnumChildWindows(editor, [](HWND child, LPARAM) -> BOOL {
            wchar_t cls[64]{}; GetClassNameW(child, cls, 64);
            if (_wcsicmp(cls, WC_LISTVIEWW)) return TRUE;
            const int count = ListView_GetItemCount(child);
            std::printf("LIST tick=%d id=%d rows=%d\n", tick, GetDlgCtrlID(child), count);
            for (int row = 0; row < count; ++row) {
                wchar_t text[1024]{};
                LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_INDENT;
                item.iItem = row; item.pszText = text; item.cchTextMax = 1024;
                SendMessageW(child, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
                std::string utf8; text_encoding::EncodeExact(text, CP_UTF8, utf8);
                std::printf("ROW %d indent=%d data=%llx text=%s\n", row, item.iIndent,
                    static_cast<unsigned long long>(item.lParam), utf8.c_str());
                ++rowsSeen;
            }
            if (tick == 1 && count > 1) ListView_SetItemState(child, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            if (tick == 2 && bilingualFixture) { Check(count == 3, "fixture has one expanded material"); CheckDisplay(child); }
            return TRUE;
        }, 0);
        if (tick == 1) PostMessageW(editor, WM_COMMAND, 40006, 0);
        else { PostMessageW(editor, WM_COMMAND, 40011, 0); done = true; KillTimer(nullptr, timer); }
    }
    if (tick > 10) { std::puts("FAIL mapping dialog not found"); std::exit(2); }
}
#include "viewport_render_scale_scene_checks.hpp"
#include "viewport_scale_performance_checks.hpp"
#include "viewport_scale_dialog_checks.hpp"
#include "viewport_panel_checks.hpp"
#include "antialias_scene_checks.hpp"
#include "antialias_startup_checks.hpp"
#include "outline_coverage_scene_checks.hpp"
#include "mme_ground_shadow_checks.hpp"
#include "effect_file_watch_checks.hpp"
#include "pmm_version_effect_checks.hpp"
#include "image_visibility_checks.hpp"
#include "camera_motion_scene_checks.hpp"
// ---------------------------------------------------------------------------
// --native-aa-idle: paused edit-viewport temporal accumulation driven only by
// the ordinary message loop. This path never calls FrameDriver,
// RenderFrameScene, ResetHistory or RequestViewport; updates::SafePoint alone
// decides when a frame exists. Scheduler availability is enabled by reporting
// only the hidden main window as visible to IsWindowVisible. The window itself
// stays genuinely hidden and inactive for the whole run.
constexpr DWORD kNativeAaIdleStageMs = 10000;    // hard per-stage deadline
constexpr DWORD kNativeAaIdleSettleMs = 500;     // let the existing gate drain
constexpr DWORD kNativeAaIdleObserveMs = 500;    // prove samples/presents steady
constexpr DWORD kNativeAaIdleQuietMs = 200;      // no Present for this long
constexpr int kNativeAaIdleShadowStep = 2500;    // stays inside the 0..9999 trackbar

struct NativeAaIdleWindow {
    unsigned presents = 0;
    unsigned frameDrivers = 0;
    unsigned restarts = 0;
    unsigned samplesFirst = 0;
    unsigned samplesLast = 0;
    ULONGLONG elapsedMs = 0;
    ULONGLONG lastPresentMs = 0;
    bool timedOut = false;
    bool quit = false;
    bool deviceError = false;
    bool leftHidden = false;
    bool becameActive = false;
};

using NativeAaIdleFrameDriver = void (*)(MMDApp*);
NativeAaIdleFrameDriver originalNativeAaIdleFrameDriver = nullptr;
MMDApp* nativeAaIdleObservedApp = nullptr;
unsigned nativeAaIdleFrameDrivers = 0;
unsigned nativeAaIdleRestarts = 0;
unsigned nativeAaIdleSamplesBefore = 0;
unsigned nativeAaIdleSamplesAfter = 0;
unsigned nativeAaIdleRestartBefore = 0;
unsigned nativeAaIdleRestartAfter = 0;
bool traceNativeShadowIdle = false;

// Pure observation: one forwarded call, read-only sample counters, no input or
// device state change.
void ObserveNativeAaIdleFrameDriver(MMDApp* app) {
    if (app != nativeAaIdleObservedApp) { originalNativeAaIdleFrameDriver(app); return; }
    ++nativeAaIdleFrameDrivers;
    const unsigned before = antialias::AccumulatedSamples(app);
    const auto presented = app->frameRate.presentedFrames;
    const unsigned messageSeen = app->MessageSeen();
    const bool exposure = traceNativeShadowIdle && updates::RepaintOnly();
    originalNativeAaIdleFrameDriver(app);
    if (traceNativeShadowIdle && app->frameRate.presentedFrames != presented)
        std::printf("SHADOW_IDLE_DRIVER at=%llu messageSeen=%u->%u exposure=%d presented=%llu->%llu\n",
                    GetTickCount64(), messageSeen, unsigned(app->MessageSeen()), int(exposure),
                    static_cast<unsigned long long>(presented),
                    static_cast<unsigned long long>(app->frameRate.presentedFrames));
    const unsigned after = antialias::AccumulatedSamples(app);
    nativeAaIdleSamplesBefore = before;
    nativeAaIdleSamplesAfter = after;
    if (after < before) {
        ++nativeAaIdleRestarts;
        nativeAaIdleRestartBefore = before;
        nativeAaIdleRestartAfter = after;
    }
}

decltype(&IsWindowVisible) originalNativeAaIdleIsWindowVisible = nullptr;
HWND nativeAaIdleMainWindow = nullptr;

// Only the current app main window is reported as visible so the production
// scheduler's Displayable() condition is met. Every other window keeps the
// real answer. The window never receives WS_VISIBLE or activation.
BOOL WINAPI ControlNativeAaIdleVisibility(HWND window) {
    if (window && window == nativeAaIdleMainWindow) return TRUE;
    return originalNativeAaIdleIsWindowVisible(window);
}

bool NativeAaIdleGenuinelyHidden() {
    const HWND main = nativeAaIdleMainWindow;
    if (!main || !IsWindow(main) || originalNativeAaIdleIsWindowVisible == nullptr) return false;
    if (originalNativeAaIdleIsWindowVisible(main) != FALSE) return false;
    if ((GetWindowLongPtrW(main, GWL_STYLE) & WS_VISIBLE) != 0) return false;
    return GetActiveWindow() != main;
}

void LogNativeAaIdleWindow(const char* stage, MMDApp* app, const NativeAaIdleWindow& window) {
    const HWND main = nativeAaIdleMainWindow;
    const BOOL realVisible = main && originalNativeAaIdleIsWindowVisible != nullptr
        ? originalNativeAaIdleIsWindowVisible(main) : TRUE;
    const LONG_PTR style = main ? GetWindowLongPtrW(main, GWL_STYLE) : 0;
    std::printf("AA_IDLE stage=%s algorithm=%u enabled=%u samples=%u required=%u initializing=%u "
                "messageSeen=%u frame=%ld cursor=%.6f presentedFrames=%llu presentDelta=%u "
                "frameDrivers=%u restarts=%u lastSamples=%u->%u lastRestart=%u->%u "
                "realVisible=%d styleVisible=%d active=%d hidden=%d timedOut=%d quit=%d "
                "deviceError=%d elapsedMs=%llu\n",
        stage, unsigned(app->antialiasing.algorithm), unsigned(app->antialiasing.enabled ? 1 : 0),
        antialias::AccumulatedSamples(app), antialias::RequiredSamples(app),
        unsigned(antialias::Initializing(app) ? 1 : 0),
        static_cast<unsigned>(app->MessageSeen()), static_cast<long>(app->CurrentFrame()),
        app->PlaybackCursorSeconds(),
        static_cast<unsigned long long>(app->frameRate.presentedFrames), window.presents,
        window.frameDrivers, window.restarts, nativeAaIdleSamplesBefore, nativeAaIdleSamplesAfter,
        nativeAaIdleRestartBefore, nativeAaIdleRestartAfter,
        int(realVisible != FALSE), int((style & WS_VISIBLE) != 0),
        int(GetActiveWindow() == main), int(NativeAaIdleGenuinelyHidden()),
        int(window.timedOut), int(window.quit), int(window.deviceError),
        static_cast<unsigned long long>(window.elapsedMs));
}

// One ordinary production loop iteration: a real updates::SafePoint, at most
// one translated and dispatched message, then the production wait. No
// test-only frame call is made.
template <typename Done>
bool PumpNativeAaIdle(MMDApp* app, const char* stage, DWORD timeoutMs, Done done,
                      NativeAaIdleWindow& window) {
    window = NativeAaIdleWindow{};
    const ULONGLONG start = GetTickCount64();
    const std::uint64_t presentedStart = app->frameRate.presentedFrames;
    const unsigned driversStart = nativeAaIdleFrameDrivers;
    const unsigned restartsStart = nativeAaIdleRestarts;
    window.samplesFirst = antialias::AccumulatedSamples(app);
    window.lastPresentMs = start;
    std::uint64_t presentedLast = presentedStart;
    MSG message{};
    bool ok = true;
    while (!done()) {
        if (GetTickCount64() - start > timeoutMs) { window.timedOut = true; ok = false; break; }
        updates::SafePoint(app);
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { window.quit = true; ok = false; break; }
            const unsigned seen = app->MessageSeen();
            RECT damage{};
            const bool mainPaint = traceNativeShadowIdle && message.hwnd == app->MainWindow() &&
                message.message == WM_PAINT && GetUpdateRect(message.hwnd, &damage, FALSE);
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (traceNativeShadowIdle && (mainPaint || seen != app->MessageSeen()))
                std::printf("SHADOW_IDLE_MESSAGE at=%llu main=%d msg=0x%04x wp=%llu seen=%u->%u "
                            "damage=%ld,%ld,%ld,%ld\n",
                            GetTickCount64(), int(message.hwnd == app->MainWindow()),
                            message.message, static_cast<unsigned long long>(message.wParam),
                            seen, unsigned(app->MessageSeen()),
                            damage.left, damage.top, damage.right, damage.bottom);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, updates::WaitMilliseconds(app),
                                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        IDirect3DDevice9* device = app->Renderer() ? app->Renderer()->device : nullptr;
        if (device == nullptr || FAILED(device->TestCooperativeLevel())) {
            window.deviceError = true; ok = false; break;
        }
        const std::uint64_t presented = app->frameRate.presentedFrames;
        if (presented != presentedLast) {
            presentedLast = presented;
            window.lastPresentMs = GetTickCount64();
        }
        if (!NativeAaIdleGenuinelyHidden()) window.leftHidden = true;
        if (GetActiveWindow() == nativeAaIdleMainWindow) window.becameActive = true;
    }
    window.presents = unsigned(app->frameRate.presentedFrames - presentedStart);
    window.frameDrivers = nativeAaIdleFrameDrivers - driversStart;
    window.restarts = nativeAaIdleRestarts - restartsStart;
    window.samplesLast = antialias::AccumulatedSamples(app);
    window.elapsedMs = GetTickCount64() - start;
    LogNativeAaIdleWindow(stage, app, window);
    return ok;
}

void AwaitNativeAaIdleSamples(MMDApp* app, const char* stage, unsigned expected,
                              bool waitRestart, unsigned restartBaseline,
                              int frame, double cursor, NativeAaIdleWindow& window) {
    const auto done = [&]() {
        if (!antialias::Initializing(app) && antialias::RequiredSamples(app) != expected) return true;
        if (waitRestart && nativeAaIdleRestarts <= restartBaseline) return false;
        if (antialias::Initializing(app)) return false;
        return antialias::AccumulatedSamples(app) >= expected;
    };
    const bool ok = PumpNativeAaIdle(app, stage, kNativeAaIdleStageMs, done, window);
    Check(ok, "temporal accumulation finished inside its stage deadline");
    Check(!window.quit && !window.deviceError, "scene and device stayed alive during temporal accumulation");
    Check(!antialias::Initializing(app), "requested temporal algorithm finished initializing");
    Check(antialias::RequiredSamples(app) == expected,
          "motion path keeps the authored required sample count");
    Check(antialias::AccumulatedSamples(app) >= expected,
          "ordinary loop accumulated every required sample");
    Check(window.presents > 0, "temporal accumulation presented real frames");
    Check(window.frameDrivers > 0, "production frame driver ran during temporal accumulation");
    if (waitRestart)
        Check(window.restarts > 0 && nativeAaIdleRestarts > restartBaseline,
              "temporal history restarted from its own production gate");
    Check(app->CurrentFrame() == frame && app->PlaybackCursorSeconds() == cursor,
          "paused frame and playback cursor stayed fixed during accumulation");
    Check(!window.leftHidden && !window.becameActive && NativeAaIdleGenuinelyHidden(),
          "main window stayed genuinely hidden and inactive during accumulation");
}

void ObserveNativeAaIdleStability(MMDApp* app, const char* stage) {
    const int frame = app->CurrentFrame();
    const double cursor = app->PlaybackCursorSeconds();
    const auto drainStart = GetTickCount64();
    NativeAaIdleWindow drain{};
    const bool drained = PumpNativeAaIdle(app, stage, kNativeAaIdleStageMs,
        [&]() { return GetTickCount64() - drainStart >= kNativeAaIdleSettleMs; }, drain);
    Check(drained, "existing render gate finished inside its stage deadline");
    const unsigned stableSamples = antialias::AccumulatedSamples(app);
    const std::uint64_t stablePresented = app->frameRate.presentedFrames;
    const auto quietStart = GetTickCount64();
    NativeAaIdleWindow quiet{};
    const bool observed = PumpNativeAaIdle(app, stage, kNativeAaIdleStageMs,
        [&]() { return GetTickCount64() - quietStart >= kNativeAaIdleObserveMs; }, quiet);
    Check(observed, "quiet observation finished inside its stage deadline");
    Check(antialias::AccumulatedSamples(app) == stableSamples,
          "accumulated samples stay stable once every required sample exists");
    Check(app->frameRate.presentedFrames == stablePresented && quiet.presents == 0,
          "paused settled viewport issues no further Present");
    Check(quiet.frameDrivers > 0, "ordinary loop kept polling the frame driver while paused");
    Check(!drain.leftHidden && !quiet.leftHidden && !drain.becameActive && !quiet.becameActive &&
          NativeAaIdleGenuinelyHidden(),
          "main window stayed genuinely hidden and inactive through settle and observation");
    Check(app->CurrentFrame() == frame && app->PlaybackCursorSeconds() == cursor,
          "settling did not advance the frame or playback cursor");
}

// Change the real self-shadow range control through the production HW_HSCROLL
// route. The temporal history detects the changed shadow configuration inside
// its next real frame and restarts accumulation by itself.
void RunNativeAaIdleShadowChange(MMDApp* app, HWND slider, int position, unsigned expected,
                                 int frame, double cursor, const char* stage) {
    const float before = app->ShadowDistance();
    const unsigned restartBaseline = nativeAaIdleRestarts;
    SendMessageW(slider, TBM_SETPOS, TRUE, position);
    Check(static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0)) == position,
          "production self-shadow slider accepted the requested position");
    SendMessageW(app->MainWindow(), WM_HSCROLL, TB_THUMBPOSITION,
                 reinterpret_cast<LPARAM>(slider));
    Check(app->ShadowDistance() != before, "production WM_HSCROLL changed the self-shadow range");
    NativeAaIdleWindow window{};
    AwaitNativeAaIdleSamples(app, stage, expected, true, restartBaseline, frame, cursor, window);
    Check(nativeAaIdleRestartAfter < nativeAaIdleRestartBefore,
          "self-shadow range change restarted accumulation inside a real frame");
    ObserveNativeAaIdleStability(app, stage);
}

void RunNativeAaIdleAlgorithm(MMDApp* app, antialias::Algorithm algorithm, unsigned expected,
                              const char* label) {
    const HWND main = app->MainWindow();
    const int frame = app->CurrentFrame();
    const double cursor = app->PlaybackCursorSeconds();
    // Every temporal run starts from the production Off state. Record the total
    // present counter, not a pump-local delta: the command itself may present
    // synchronously before the pump is entered.
    const std::uint64_t presentedBeforeOff = app->frameRate.presentedFrames;
    const bool enabledBeforeOff = app->antialiasing.enabled;
    SendMessageW(main, WM_COMMAND, antialias::kOffCommand, 0);
    Check(!app->antialiasing.enabled, "production Off command disables AA before the temporal run");
    {
        NativeAaIdleWindow off{};
        const bool quiet = PumpNativeAaIdle(app, "off-settle", kNativeAaIdleStageMs,
            [&]() { return GetTickCount64() - off.lastPresentMs >= kNativeAaIdleQuietMs; }, off);
        Check(quiet, "disabled AA scene reached a quiet paused state");
        Check(off.frameDrivers > 0,
              "production scheduler drove frames from the controlled hidden availability");
    }
    const std::uint64_t presentedAfterOff = app->frameRate.presentedFrames;
    std::printf("AA_IDLE phase=off-refresh %s enabledBefore=%u presentedBefore=%llu "
                "presentedAfter=%llu presentedDelta=%llu\n",
        label, unsigned(enabledBeforeOff ? 1 : 0),
        static_cast<unsigned long long>(presentedBeforeOff),
        static_cast<unsigned long long>(presentedAfterOff),
        static_cast<unsigned long long>(presentedAfterOff - presentedBeforeOff));
    // Leaving an enabled temporal run must present the disabled scene at least
    // once. A run that already starts Off has nothing to refresh.
    if (enabledBeforeOff)
        Check(presentedAfterOff > presentedBeforeOff,
              "disabling AA presented a fresh frame for the paused viewport");
    Check(!app->antialiasing.enabled && NativeAaIdleGenuinelyHidden(),
          "disabled paused scene stayed genuinely hidden before the temporal run");
    std::printf("AA_IDLE phase=select %s algorithm=%u expectedSamples=%u messageSeen=%u\n",
        label, unsigned(algorithm), expected, static_cast<unsigned>(app->MessageSeen()));
    Check(antialias::Supported(app, algorithm), "device supports the requested native temporal algorithm");
    SendMessageW(main, WM_COMMAND, antialias::kFirstCommand + unsigned(algorithm), 0);
    Check(app->antialiasing.enabled && app->antialiasing.algorithm == algorithm,
          "production WM_COMMAND selected and enabled the requested algorithm");
    NativeAaIdleWindow accumulated{};
    AwaitNativeAaIdleSamples(app, "accumulate", expected, false, 0, frame, cursor, accumulated);
    ObserveNativeAaIdleStability(app, "settled");
    const HWND slider = GetDlgItem(main, panel::kSelfShadowRangeSlider);
    Check(slider != nullptr, "production self-shadow range slider exists");
    const int original = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    const int alternate = original > 9999 - kNativeAaIdleShadowStep
        ? original - kNativeAaIdleShadowStep : original + kNativeAaIdleShadowStep;
    Check(alternate >= 0 && alternate <= 9999 && alternate != original,
          "alternate self-shadow range stays inside the production slider range");
    std::printf("AA_IDLE phase=range %s algorithm=%u from=%d to=%d\n",
        label, unsigned(algorithm), original, alternate);
    RunNativeAaIdleShadowChange(app, slider, alternate, expected, frame, cursor, "range-change");
    RunNativeAaIdleShadowChange(app, slider, original, expected, frame, cursor, "range-restore");
    Check(static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0)) == original,
          "production self-shadow range slider position is restored");
    std::printf("AA_IDLE phase=done %s algorithm=%u\n", label, unsigned(algorithm));
}

void RunNativeAaIdleSequence(MMDApp* app) {
    const HWND main = app->MainWindow();
    RunNativeAaIdleAlgorithm(app, antialias::Algorithm::Taa, 8, "taa");
    RunNativeAaIdleAlgorithm(app, antialias::Algorithm::SmaaT2xEnhanced, 2, "smaa-t2x-enhanced");
    // A real menu change presents the freshly disabled scene. Let that natural
    // settle finish first, then require a separate quiet observation window.
    // Totals span the synchronous send; a pump-local delta would miss a Present
    // produced inside the command itself.
    const std::uint64_t finalOffPresentedBefore = app->frameRate.presentedFrames;
    SendMessageW(main, WM_COMMAND, antialias::kOffCommand, 0);
    Check(!app->antialiasing.enabled, "final production Off command disables AA");
    {
        NativeAaIdleWindow off{};
        const bool quiet = PumpNativeAaIdle(app, "final-off-settle", kNativeAaIdleStageMs,
            [&]() { return GetTickCount64() - off.lastPresentMs >= kNativeAaIdleQuietMs; }, off);
        Check(quiet, "final Off scene reached a quiet paused state");
        Check(!off.leftHidden && !off.becameActive && NativeAaIdleGenuinelyHidden(),
              "main window stayed genuinely hidden and inactive through the final Off settle");
    }
    const std::uint64_t finalOffPresentedAfter = app->frameRate.presentedFrames;
    std::printf("AA_IDLE phase=final-off-refresh presentedBefore=%llu presentedAfter=%llu "
                "presentedDelta=%llu\n",
        static_cast<unsigned long long>(finalOffPresentedBefore),
        static_cast<unsigned long long>(finalOffPresentedAfter),
        static_cast<unsigned long long>(finalOffPresentedAfter - finalOffPresentedBefore));
    Check(finalOffPresentedAfter > finalOffPresentedBefore,
          "final Off command presented a fresh frame for the paused viewport");
    // Separate observation window: the paused disabled scene stays stable and
    // issues no additional Present once the Off refresh has finished.
    const unsigned finalSamples = antialias::AccumulatedSamples(app);
    const std::uint64_t finalPresented = app->frameRate.presentedFrames;
    const int finalFrame = app->CurrentFrame();
    const double finalCursor = app->PlaybackCursorSeconds();
    const auto finalObserveStart = GetTickCount64();
    NativeAaIdleWindow finalWindow{};
    const bool finalObserved = PumpNativeAaIdle(app, "final-off", kNativeAaIdleStageMs,
        [&]() { return GetTickCount64() - finalObserveStart >= kNativeAaIdleObserveMs; }, finalWindow);
    Check(finalObserved, "final quiet observation finished inside its stage deadline");
    Check(antialias::AccumulatedSamples(app) == finalSamples &&
          app->frameRate.presentedFrames == finalPresented && finalWindow.presents == 0,
          "disabled paused scene issues no extra Present after the Off refresh");
    Check(finalWindow.frameDrivers > 0, "ordinary loop kept running after the Off refresh");
    Check(!finalWindow.leftHidden && !finalWindow.becameActive && NativeAaIdleGenuinelyHidden(),
          "main window stayed genuinely hidden and inactive during the final observation");
    Check(app->CurrentFrame() == finalFrame && app->PlaybackCursorSeconds() == finalCursor,
          "final observation did not advance the frame or playback cursor");
}

void RunNativeShadowFilterIdle(MMDApp* app) {
    const HWND checkbox = GetDlgItem(app->MainWindow(), panel::kShadowFilterCheckbox);
    Check(checkbox && IsWindowEnabled(checkbox), "production enhanced shadow filter checkbox is enabled");
    Check(app->ShadowMode() > 0 && !app->ShadowFilterEnhanced() &&
          SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
          "native self shadow starts with enhanced filtering off");
    // A cold first draw can exceed the normal quiet observation interval.
    // Start the click test only after the production startup gate drains.
    const auto beforeStartup = app->frameRate.presentedFrames;
    NativeAaIdleWindow startup{};
    Check(PumpNativeAaIdle(app, "shadow-filter-startup", kNativeAaIdleStageMs,
        [&]() {
            return app->frameRate.presentedFrames > beforeStartup && app->MessageSeen() == 0 &&
                   GetTickCount64() - startup.lastPresentMs >= kNativeAaIdleQuietMs;
        }, startup) && !startup.leftHidden && !startup.becameActive,
        "native viewport finishes its real startup presents before the first filter click");
    ObserveNativeAaIdleStability(app, "shadow-filter-initial-off");
    const auto initial = shadow_filter::ReadNativeSamplingState(app);
    Check(initial.texture && !initial.enhanced, "initial native receiver uses ordinary shadow sampling");

    for (const bool enabled : {true, false}) {
        const auto before = app->frameRate.presentedFrames;
        const char* stage = enabled ? "shadow-filter-enable" : "shadow-filter-disable";
        SendMessageW(checkbox, BM_CLICK, 0, 0);
        Check(app->ShadowFilterEnhanced() == enabled &&
              SendMessageW(checkbox, BM_GETCHECK, 0, 0) == (enabled ? BST_CHECKED : BST_UNCHECKED),
              "production checkbox changes both the control and the filter preference");
        NativeAaIdleWindow window{};
        const bool settled = PumpNativeAaIdle(app, stage, kNativeAaIdleStageMs,
            [&]() { return GetTickCount64() - window.lastPresentMs >= kNativeAaIdleQuietMs; }, window);
        const auto sampling = shadow_filter::ReadNativeSamplingState(app);
        const auto after = app->frameRate.presentedFrames;
        std::printf("SHADOW_FILTER_IDLE phase=%s requested=%d enhanced=%d size=%.0fx%.0f "
                    "presentedBefore=%llu presentedAfter=%llu\n",
                    stage, int(enabled), int(sampling.enhanced), sampling.mapSize[0], sampling.mapSize[1],
                    static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
        Check(settled && window.frameDrivers > 0 && !window.leftHidden && !window.becameActive,
              "ordinary paused loop processed the filter click while genuinely hidden");
        Check(after > before, "changing the enhanced filter refreshes the paused viewport");
        Check(sampling.texture && sampling.enhanced == enabled,
              "the refreshed native shadow receiver uses the requested filter");
        ObserveNativeAaIdleStability(app, stage);
    }
}

void CheckNativeAaIdle(MMDApp* app, const wchar_t* projectPath, bool shadowFilterOnly) {
    Check(!build_profile::original, "native AA idle path requires the enhanced ABI build profile");
    Check(GetModuleHandleW(L"MMEffect.dll") == nullptr &&
          GetModuleHandleW(L"MMHack.dll") == nullptr &&
          GetModuleHandleW(L"MMV4MMS.dll") == nullptr,
          "native AA idle path rejects an already loaded MME module set");
    Check(projectPath != nullptr && pmm_paths::IsProject(projectPath),
          "native AA idle path requires a real PMM project path");
    const HWND main = app->MainWindow();
    Check(main != nullptr && IsWindow(main), "hidden production main window exists");
    Check(!IsWindowVisible(main), "production main window starts genuinely hidden");
    Check(GetActiveWindow() != main, "production main window starts inactive");
    Check(IsWindowEnabled(main), "hidden production main window is enabled");
    Check(LoadSceneFile(projectPath), "load the requested PMM into the hidden production host");
    int modelSlot = -1;
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        auto* model = app->ModelSlot(slot);
        if (model != nullptr && mdl::Mdl(model)->loadComplete != 0 &&
            mdl::Mdl(model)->vertexCount > 0) { modelSlot = slot; break; }
    }
    Check(modelSlot >= 0, "loaded PMM contains at least one real model");
    app->SetSelectedModelSlot(static_cast<std::uint8_t>(modelSlot));
    Check(app->SelectedModel() != nullptr, "selected model is a loaded PMM model");
    std::printf("AA_IDLE project modelSlot=%d vertexCount=%u\n", modelSlot,
        static_cast<unsigned>(mdl::Mdl(app->SelectedModel())->vertexCount));
    // Explicit paused fixture: no playback, no recording or range dialog, and
    // the normal frame-volume gate enabled so it can settle to zero by itself.
    app->PlaybackActive() = 0;
    app->FrameStepPlayback() = 0;
    app->RecordingWindow() = nullptr;
    app->FrameRangeDialog() = nullptr;
    app->FrameVolumeControlEnabled() = 1;
    Check(app->PlaybackActive() == 0 && app->FrameStepPlayback() == 0 &&
          app->RecordingWindow() == nullptr && app->FrameRangeDialog() == nullptr &&
          app->FrameVolumeControlEnabled() == 1,
          "fixture is the paused ordinary edit viewport");
    // Off first: a persisted MSAA preference can reset the device, so the
    // production layout entry below runs after the fixture is in its Off state.
    SendMessageW(main, WM_COMMAND, antialias::kOffCommand, 0);
    Check(!app->antialiasing.enabled, "production Off command starts the fixture without AA");
    // The SW_HIDE startup never receives the visible application's first size
    // layout. Build the ordinary editor viewport through its production entry.
    HandleWindowSize(app);
    Check(app->WindowLayoutReady() != 0, "production layout is ready for the hidden editor viewport");
    Check(!app->SceneMutationInProgress(), "loaded scene mutation is complete");
    Check(FrameViewportRenderable(app), "hidden editor viewport is valid and presentable");
    const int startFrame = app->CurrentFrame();
    const double startCursor = app->PlaybackCursorSeconds();
    std::printf("AA_IDLE setup fpsLimit=%.3f frame=%d cursor=%.6f frameVolume=%u playback=%u "
                "recording=%p rangeDialog=%p layoutReady=%u renderable=%d\n",
        app->FpsLimit(), startFrame, startCursor,
        static_cast<unsigned>(app->FrameVolumeControlEnabled()),
        static_cast<unsigned>(app->PlaybackActive()),
        static_cast<void*>(app->RecordingWindow()), static_cast<void*>(app->FrameRangeDialog()),
        static_cast<unsigned>(app->WindowLayoutReady()), int(FrameViewportRenderable(app)));
    // Only this CLI path reports the hidden main window as available. Every
    // other window keeps the real IsWindowVisible result.
    nativeAaIdleMainWindow = main;
    nativeAaIdleObservedApp = app;
    Check(MH_CreateHook(reinterpret_cast<void*>(&FrameDriver),
              reinterpret_cast<void*>(&ObserveNativeAaIdleFrameDriver),
              reinterpret_cast<void**>(&originalNativeAaIdleFrameDriver)) == MH_OK &&
          MH_EnableHook(reinterpret_cast<void*>(&FrameDriver)) == MH_OK,
          "observe the production frame driver");
    Check(MH_CreateHook(reinterpret_cast<void*>(&IsWindowVisible),
              reinterpret_cast<void*>(&ControlNativeAaIdleVisibility),
              reinterpret_cast<void**>(&originalNativeAaIdleIsWindowVisible)) == MH_OK &&
          MH_EnableHook(reinterpret_cast<void*>(&IsWindowVisible)) == MH_OK,
          "control scheduler availability for the hidden main window");
    std::printf("AA_IDLE availability=controlled-hidden\n");
    Check(IsWindowVisible(main) != FALSE && originalNativeAaIdleIsWindowVisible(main) == FALSE,
          "availability hook reports only the hidden main window as visible");
    Check(NativeAaIdleGenuinelyHidden(),
          "main window stays genuinely hidden under the availability hook");
    Check(updates::Initialize(app), "initialize the production update coordinator");
    traceNativeShadowIdle = shadowFilterOnly;
    if (shadowFilterOnly) RunNativeShadowFilterIdle(app);
    else RunNativeAaIdleSequence(app);
    Check(NativeAaIdleGenuinelyHidden(), "main window stayed genuinely hidden for the whole run");
    updates::Shutdown(app);
    Check(MH_DisableHook(reinterpret_cast<void*>(&FrameDriver)) == MH_OK &&
          MH_RemoveHook(reinterpret_cast<void*>(&FrameDriver)) == MH_OK,
          "remove the frame driver observation hook");
    originalNativeAaIdleFrameDriver = nullptr;
    Check(MH_DisableHook(reinterpret_cast<void*>(&IsWindowVisible)) == MH_OK &&
          MH_RemoveHook(reinterpret_cast<void*>(&IsWindowVisible)) == MH_OK,
          "remove the availability control hook");
    originalNativeAaIdleIsWindowVisible = nullptr;
    nativeAaIdleMainWindow = nullptr;
    nativeAaIdleObservedApp = nullptr;
    Check(!IsWindowVisible(app->MainWindow()),
          "main window is genuinely hidden after removing the control hook");
    Check(app->CurrentFrame() == startFrame && app->PlaybackCursorSeconds() == startCursor,
          "the whole run never advanced the paused frame or playback cursor");
}
ULONGLONG startupClock = 0;
void StartupLog(const char* format, ...) noexcept {
    std::printf("STARTUP %llu ", GetTickCount64() - startupClock);
    va_list args; va_start(args, format); std::vprintf(format, args); va_end(args);
    std::putchar('\n');
}
}
#include "mme_sequence_clock_checks.hpp"
#include "mme_scene_capture.hpp"
#include "model_edit_controls_checks.hpp"
int wmain(int argc, wchar_t** argv) {
    Check(argc >= 2 && argc <= 4, "model path supplied");
    bilingualFixture = argc == 3 && std::wcscmp(argv[2], L"--bilingual") == 0;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Check(MH_Initialize() == MH_OK, "initialize hidden-window hook");
    Check(MH_CreateHook(&runtime_log::WritePath, &ObserveEffectPath, reinterpret_cast<void**>(&originalWritePath)) == MH_OK,
        "observe paths before native effect compilation");
    const bool shaderStartup = argc == 3 && std::wcscmp(argv[2], L"--shader-startup") == 0;
    if(shaderStartup)InstallAntialiasCompilerGate();
    const bool startupProbe = argc == 3 && std::wcscmp(argv[2], L"--startup-probe") == 0;
    startupClock = GetTickCount64();
    if (startupProbe) Check(MH_CreateHook(&runtime_log::Write, &StartupLog, nullptr) == MH_OK,
        "observe startup stages");
    Check(MH_CreateHook(&ShowWindow, &Hide, reinterpret_cast<void**>(&originalShow)) == MH_OK, "hook ShowWindow");
    Check(MH_CreateHook(&MessageBoxW, &Unexpected, nullptr) == MH_OK, "guard unexpected dialogs");
    Check(MH_CreateHook(&MessageBoxA, &UnexpectedAnsi, nullptr) == MH_OK, "guard legacy dialogs");
    if(std::getenv("MMS_SURFACE_MISSING_DX12"))Check(MH_CreateHook(&LoadLibraryW,&SurfaceLoadLibrary,reinterpret_cast<void**>(&surfaceLoadLibrary))==MH_OK,"simulate missing DX12 in test process");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable hidden-window guards");
    if(std::getenv("MMS_VIEWPORT_PERF"))runtime_log::Initialize();
    HHOOK guard = SetWindowsHookExW(WH_CBT, HideCreated, nullptr, GetCurrentThreadId());
    Check(guard != nullptr, "prevent window activation");
    auto* app = new MMDApp; g_Block = app;
    app->state = MMDAppState{}; app->InitDefaults();
    Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "start production host");
    const bool shadowFilterIdle = argc == 3 && std::wcscmp(argv[2], L"--native-shadow-filter-idle") == 0;
    if (shadowFilterIdle || (argc == 3 && std::wcscmp(argv[2], L"--native-aa-idle") == 0)) {
        CheckNativeAaIdle(app, argv[1], shadowFilterIdle);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--mme-scene-capture") == 0) {
        auto* compile = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
        Check(compile && MH_CreateHook(compile, &ObserveCompile, reinterpret_cast<void**>(&originalCompile)) == MH_OK &&
            MH_EnableHook(compile) == MH_OK, "observe actual MME capture effect compilation");
        int result = 0;
        try { mme_scene_capture::Run(app, argv[1], argv[3]); }
        catch (const std::exception& error) { std::fprintf(stderr, "FAIL MME_CAPTURE %s\n", error.what()); result = 2; }
        mme_scene_capture::Release(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard);
        std::printf("MME_CAPTURE cleanup complete result=%d\n", result);
        return result;
    }
    if(shaderStartup) {
        CheckAntialiasStartup(app);
        ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(guard);return 0;
    }
    if (startupProbe) {
        StartupLog("UI ready");
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        StartupLog("First empty frame");
        LoadModelFile(app, argv[1], false);
        StartupLog("Model loaded");
        for (int i = 0; i < 20; ++i) {
            const auto begin = GetTickCount64();
            app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
            StartupLog("Frame %d took %llu ms", i, GetTickCount64() - begin);
            MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(16);
        }
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 3 && std::wcscmp(argv[2], L"--antialias-preference") == 0) {
        Check(app->antialiasing.algorithm == antialias::Algorithm::Fxaa && app->antialiasing.enabled,
            "restart restores selected algorithm and enable state");
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 3 && std::wcscmp(argv[2], L"--antialias-menu") == 0) {
        CheckAntialiasMenus(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 3 && std::wcscmp(argv[2], L"--antialias-temporal") == 0) {
        CheckTemporalResolveShader();
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    const bool emdAutoload = argc == 3 && std::wcscmp(argv[2], L"--emd-autoload") == 0;
    const bool emdSelfShadowCycle = argc == 3 && std::wcscmp(argv[2], L"--emd-self-shadow-cycle") == 0;
    const bool fileWatch = argc == 4 && std::wcscmp(argv[2], L"--file-watch") == 0;
    const bool emmLoad = argc == 4 && std::wcscmp(argv[2], L"--emm-load") == 0;
    const bool aaScene = argc == 4 && std::wcscmp(argv[2], L"--antialias-scene") == 0;
    const bool imageVisibility = argc == 4 && std::wcscmp(argv[2], L"--image-visibility") == 0;
    const bool groundShadow = argc == 4 && std::wcscmp(argv[2], L"--ground-shadow") == 0;
    const bool sequenceClock = argc == 4 && std::wcscmp(argv[2], L"--sequence-clock") == 0;
    if (sequenceClock || emdAutoload || emdSelfShadowCycle || fileWatch || emmLoad || aaScene || groundShadow ||
        (argc == 4 && std::wcscmp(argv[2], L"--surface-grid") == 0)) {
        auto* compile = reinterpret_cast<void*>(GetProcAddress(d3dx::Get().module, "D3DXCreateEffectFromFileW"));
        Check(compile && MH_CreateHook(compile, &ObserveCompile, reinterpret_cast<void**>(&originalCompile)) == MH_OK &&
              MH_EnableHook(compile) == MH_OK, "observe actual MME effect compilation");
    }
    wchar_t native[4]{};
    const bool nativeOrder = GetEnvironmentVariableW(L"MMS_MME_ORDER_NATIVE", native, 4) == 1 && native[0] == L'1';
    Check(nativeOrder ? !GetModuleHandleW(L"MMEffect.dll") && !GetModuleHandleW(L"MMHack.dll") :
        GetModuleHandleW(L"MMEffect.dll") && GetModuleHandleW(L"MMHack.dll"),
        nativeOrder ? "native baseline excludes MME DLLs" : "real MME DLLs loaded");
    if (MmeToonTraceEnabled()) InstallMmeToonTrace();
    if(std::getenv("MMS_SURFACE_FORCE_GPU")){
        const auto& adapters=skinning::AvailableAdapters();Check(!adapters.empty(),"GPU skinning adapter available");
        skinning::SetPreference({skinning::Mode::Adapter,adapters.front().id,adapters.front().name});
    }
    if ((aaScene || (argc == 4 && std::wcscmp(argv[2], L"--viewport-scale") == 0)) &&
        pmm_paths::IsProject(argv[1])) {
        Check(LoadSceneFile(argv[1]), "load the requested project before scene verification");
        app->SetSelectedModelSlot(0);
        std::set<const void*> loadedModels;
        for (int slot=0;slot<kModelSlotCount;++slot)if(auto* model=app->ModelSlot(slot))
            Check(loadedModels.insert(model).second,"loaded project model slots retain distinct ownership");
        if (const wchar_t* effects = _wgetenv(L"MMS_AA_EMM")) NotifyEffectFileOpen(effects);
    } else LoadModelFile(app, argv[1], false);
    if (argc == 4 && std::wcscmp(argv[2], L"--model-edit-controls") == 0) {
        CheckModelEditControls(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard);
        return 0;
    }
    // Test-only second model and accessory slots: insert them into the scene
    // instead of replacing the base model.
    if (imageVisibility) PrepareImageVisibilityScene(app);
    if (const wchar_t* extra = _wgetenv(L"MMS_EXTRA_MODEL")) {
        LoadModelFile(app, extra, false);
        for (int i = 0; i < 3; ++i) {
            app->state.messageSeen = 1;
            FrameDriver(app);
        }
    }
    if (const wchar_t* accessory = _wgetenv(L"MMS_EXTRA_ACCESSORY")) {
        LoadAccessoryFile(accessory);
        for (int i = 0; i < 3; ++i) {
            app->state.messageSeen = 1;
            FrameDriver(app);
        }
    }
    Check(app->SelectedModel() != nullptr, "load textured PMX");
    if (imageVisibility) {
        PrepareImageVisibilityScene(app);
        if (const auto* project = _wgetenv(L"MMS_IMAGE_EFFECT_PROJECT")) {
            wcscpy_s(app->EnvFileName(), kPathFieldCapacity, project);
            Check(SaveSceneFile(app), "save image visibility effect project");
            auto fixture = std::filesystem::path(project); fixture.replace_extension(L".fixture.emm");
            const auto emm = pmm_paths::Effect(project);
            std::filesystem::copy_file(fixture, emm, std::filesystem::copy_options::overwrite_existing);
            ApplyImageVisibilitySelfShadow(app);
            NotifyEffectFileOpen(project);
            for (int i = 0; i < 3; ++i) { app->state.messageSeen = 1; FrameDriver(app); }
        }
        CheckImageVisibility(app, argv[3]);
        ResetAppState(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--camera-motion") == 0) {
        CheckCameraMotionScene(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    for (int i = 0; i < 3; ++i) { app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app); }
    if (emdSelfShadowCycle) {
        CheckMmeSelfShadowCycle(app, !nativeOrder);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (sequenceClock) {
        CheckMmeSequenceClock(app, argv[3]);
        for (auto* effect : sequenceClockEffects) effect->Release();
        sequenceClockEffects.clear();
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (fileWatch) {
        CheckEffectFileWatch(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (aaScene) {
        CheckAntialiasScene(app, argv[3]);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--viewport-scale") == 0) {
        if(std::getenv("MMS_VIEWPORT_PANELS")) CheckViewportPanels(app,argv[3]);
        else if(std::getenv("MMS_VIEWPORT_PERF")) CheckViewportScalePerformance(app,argv[3]);
        else if(std::getenv("MMS_VIEWPORT_DIALOG")) CheckViewportScaleDialog(app,argv[3]);
        else CheckViewportRenderScale(app, argv[3]);
        ResetAppState(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (emmLoad) {
        Check(std::filesystem::is_regular_file(argv[3]), "EMM project notification target exists");
        NotifyEffectFileOpen(argv[3]);
        if (_wgetenv(L"MMS_EMM_MODEL_NOTIFICATION")) NotifyEffectFileOpen(argv[1]);
        if (const auto* working = _wgetenv(L"MMS_EMM_WORKING_DIRECTORY"))
            Check(SetCurrentDirectoryW(working) != FALSE, "simulate a file picker changing the working directory");
        for (int i = 0; i < 4; ++i) {
            app->DeltaTime() = 1.f / 30;
            app->state.messageSeen = 10;
            FrameDriver(app);
        }
        Check(emdEffectCompiles > 0, "EMM loads and compiles its assigned effect");
        void** vtable = *reinterpret_cast<void***>(app->Renderer()->device);
        Check(MH_CreateHook(vtable[17], &ObservePresent, reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
              MH_EnableHook(vtable[17]) == MH_OK, "observe actual EMM presentation");
        captureRequested = true;
        app->DeltaTime() = 1.f / 30; app->state.messageSeen = 10; FrameDriver(app);
        Check(!captureRequested && std::count(capturedPixels.begin(), capturedPixels.end(), 0xFF00FFu) > 50,
              "assigned EMM effect renders magenta pixels on the real model");
        Check(effectPathDiagnostics > 0, "effect file is recorded before native D3DX compilation");
        MH_DisableHook(vtable[17]);
        if (std::getenv("MMS_PMM_VERSION_TEST")) CheckVersionEffectSave(app, argv[3]);
        Check(!IsWindowVisible(app->MainWindow()), "host stays hidden");
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (emdAutoload) {
        const auto* filename = reinterpret_cast<const unsigned char*>(ExpGetPmdFilename(0));
        std::printf("EXPORTED_PATH acp=%u bytes=", GetACP());
        for (; filename && *filename; ++filename) std::printf("%02x", *filename);
        std::puts("");
        Check(emdEffectCompiles > 0, "same-name EMD automatically loads and compiles its assigned effect");
        Check(!IsWindowVisible(app->MainWindow()), "host stays hidden");
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (groundShadow) {
        if (!nativeOrder) Check(fileEffectCompiles > 0, "ground-shadow MME effects compiled");
        captureDirectory = argv[3];
        CheckGroundShadow(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--surface-grid") == 0) {
        captureDirectory = argv[3];
        CheckSurfaceGrid(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--outline-coverage") == 0) {
        CheckOutlineCoverage(app,argv[3]);
        ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(guard);return 0;
    }
    if (argc == 4 && std::wcscmp(argv[2], L"--outline-stability") == 0) {
        CheckOutlineStability(app,argv[3]);
        ShutdownCleanup(app);delete app;g_Block=nullptr;UnhookWindowsHookEx(guard);return 0;
    }
    if (argc >= 3 && std::wcscmp(argv[2], L"--outline-antialias") == 0) {
        if (argc == 4) captureDirectory = argv[3];
        CheckOutlineAntialias(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    if (argc >= 3 && std::wcscmp(argv[2], L"--alpha-order") == 0) {
        if (argc == 4) captureDirectory = argv[3];
        CheckAlphaOrder(app);
        ShutdownCleanup(app); delete app; g_Block = nullptr; UnhookWindowsHookEx(guard); return 0;
    }
    Check(SetTimer(nullptr, 0, 100, Probe) != 0, "inspect dialog asynchronously");
    SendMessageW(app->MainWindow(), WM_COMMAND, 40005, 0);
    while (!done) { MSG message{}; if (GetMessageW(&message, nullptr, 0, 0) <= 0) break; TranslateMessage(&message); DispatchMessageW(&message); }
    Check(done && rowsSeen > 1, "inspect actual expanded material rows");
    Check(!IsWindowVisible(app->MainWindow()), "host stays hidden");
    ShutdownCleanup(app); delete app; g_Block = nullptr;
    UnhookWindowsHookEx(guard);
    return 0;
}
