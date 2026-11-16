// MAINTENANCE: regression-keep; --hardware is optional real-device coverage.
// No full application startup, plugins, global input, visible parent window,
// or automatic processor calibration. Native Validate checks GPU indices;
// independent single/two-layer pixels also detect a self-consistent old sorter.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include "mikumikustudio/plugin_runtime.hpp"
#include "../sdk/mms/include/mms/dx12_viewport.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/antialias.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/d3d12_skinning.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/main_panel_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/ui_scale.hpp"
#include "mikumikustudio/view_options.hpp"
#include "mikumikustudio/viewport_bounds.hpp"

#ifndef MMD_ENABLE_DX12
#error The test target must define MMD_ENABLE_DX12 to match mikumikustudio_core.
#endif

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void Hr(HRESULT result, const char* message) { Check(SUCCEEDED(result), message); }
template<class T> void Release(T*& object) { if (object) { object->Release(); object = nullptr; } }

void DefaultDesktop() {
    wchar_t name[128]{}; DWORD required = 0;
    Check(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
        name, sizeof name, &required) && !_wcsicmp(name, L"Default"), "requires inherited Default desktop");
    Check(!GetModuleHandleW(L"MMEffect.dll"), "fixture cannot run with third-party MME loaded");
    Check(_putenv_s("MIKUMIKUSTUDIO_TRACE_FILE", "") == 0, "disable inherited CRT trace output");
    SetLastError(ERROR_SUCCESS);
    const BOOL cleared = SetEnvironmentVariableW(L"MIKUMIKUSTUDIO_TRACE_FILE", nullptr);
    Check(cleared || GetLastError() == ERROR_ENVVAR_NOT_FOUND, "disable inherited Win32 trace output");
}

struct Host {
    static constexpr UINT width = 800, height = 600;
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    MMDApp* previous = g_Block;
    HWND window = nullptr;
    IDirect3D9* factory = nullptr;
    IDirect3DDevice9* device = nullptr;
    D3DMATRIX identity{};
    Host() {
        new (&app->state) MMDAppState{};
        identity._11 = identity._22 = identity._33 = identity._44 = 1;
        window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"MMS native sort policy test",
            WS_POPUP, 0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create self-owned hidden parent");
        app->MainWindow() = window; app->Renderer() = nullptr;
        app->ViewportRect() = {0, 0, width, height};
        app->RenderWidth() = width;
        app->RenderHeight() = height;
        app->viewportRender.automatic = true; app->viewportRender.percent = 100;
        app->state.blackBackgroundEnabled = 1;
        app->LightDirection()[2] = -1;
        app->SceneLight().Ambient = {1, 1, 1, 1};
        app->CameraDistance() = -2; app->CameraFov() = 30;
        g_Block = app.get();
    }
    ~Host() {
        EnableD3D12Native(false); ShutdownD3D12Native();
        app->ModelSlot(0) = nullptr; app->Renderer() = nullptr;
        renderer->device = nullptr; renderer->d3d9 = nullptr;
        Release(device); Release(factory);
        if (window) DestroyWindow(window);
        g_Block = previous;
    }
    const float* World() const { return reinterpret_cast<const float*>(&identity); }
    HWND Child() const { return FindWindowExW(window, nullptr, L"MmdNativeD3D12Viewport", nullptr); }
};

void Hidden(const Host& host) {
    Check(!IsWindowVisible(host.window), "owned parent remains hidden");
    bool visible = false;
    EnumChildWindows(host.window, [](HWND child, LPARAM raw)->BOOL {
        if (IsWindowVisible(child)) *reinterpret_cast<bool*>(raw) = true;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&visible));
    Check(!visible, "native children of the hidden parent remain invisible");
}

struct SettingsSnapshot {
    TransparentSortPrecision precision;
    bool cross;
    std::uint8_t modified;
    std::uint64_t revision, generation;
    explicit SettingsSnapshot(const MMDApp& app)
        : precision(app.TransparentMaterialSortPrecision()), cross(app.TransparentSortCrossObjects()),
          modified(app.SceneModified()), revision(app.SceneRevision()), generation(app.SceneGeneration()) {}
    void Verify(const MMDApp& app) const {
        Check(app.TransparentMaterialSortPrecision() == precision && app.TransparentSortCrossObjects() == cross,
              "renderer preserves requested transparency settings");
        Check(app.SceneModified() == modified && app.SceneRevision() == revision && app.SceneGeneration() == generation,
              "rendering does not edit the project");
    }
};

// Count the real initialization-only policy query, without substituting its
// result or stubbing RenderD3D12Native. The gate must run before this query.
struct InitializationProbe {
    using Function = decltype(&processor::ActualRenderer);
    inline static InitializationProbe* active = nullptr;
    Function original = nullptr;
    DWORD thread = GetCurrentThreadId();
    unsigned calls = 0;
    bool armed = false;
    InitializationProbe() {
        Check(MH_Initialize() == MH_OK, "initialize test hook library");
        Check(MH_CreateHook(reinterpret_cast<void*>(&processor::ActualRenderer), reinterpret_cast<void*>(&Query),
            reinterpret_cast<void**>(&original)) == MH_OK, "observe real native initialization query");
        active = this;
        Check(MH_EnableHook(reinterpret_cast<void*>(&processor::ActualRenderer)) == MH_OK, "enable initialization observer");
    }
    ~InitializationProbe() {
        MH_DisableHook(reinterpret_cast<void*>(&processor::ActualRenderer));
        MH_RemoveHook(reinterpret_cast<void*>(&processor::ActualRenderer));
        active = nullptr; MH_Uninitialize();
    }
    static const processor::ResolvedInfo& Query() {
        auto& self = *active;
        if (self.armed && GetCurrentThreadId() == self.thread) ++self.calls;
        return self.original();
    }
};

void RouteChecks(Host& host) {
    processor::ResetForTesting();
    Check(host.app->Renderer() == nullptr && host.Child() == nullptr, "routing fixture has no renderer or native child");
    InitializationProbe probe;
    host.app->RecordingWindow() = host.window;
    EnableD3D12Native(true);
    for (auto precision : {TransparentSortPrecision::Off, TransparentSortPrecision::Low,
                           TransparentSortPrecision::Standard, TransparentSortPrecision::High,
                           TransparentSortPrecision::Maximum}) {
        host.app->TransparentMaterialSortPrecision() = precision;
        const SettingsSnapshot before(*host.app);
        probe.calls = 0;
        probe.armed = true;
        const bool rendered = RenderD3D12Native(host.app.get(), host.World());
        probe.armed = false;
        Check(!rendered && probe.calls == 0 && host.Child() == nullptr,
              "unsupported recording mode precedes native initialization");
        Check(std::strstr(D3D12NativeStatus(), "recording/floating/fullscreen") != nullptr,
              "recording fallback preserves the actual admission reason for every sorting setting");
        before.Verify(*host.app);
    }
    host.app->RecordingWindow() = nullptr;
    EnableD3D12Native(false);
    Hidden(host);
    std::puts("PASS route-before-initialization (no graphics device created)");
}

struct Geometry {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::PmdVertex, 12> vertices{};
    std::array<mdl::BoneRecord, 1> bones{};
    std::array<mdl::ModelMaterialRecord, 1> materials{};
    std::array<WORD, 12> indices{};
    Host& host;
    explicit Geometry(Host& owner) : host(owner) {
        std::memcpy(bones[0].matInit, &host.identity, 64);
        const unsigned nearIds[] = {9, 2, 7}, farIds[] = {10, 3, 8};
        for (unsigned face = 0; face < 2; ++face) for (unsigned c = 0; c < 3; ++c) {
            auto& vertex = vertices[(face ? farIds : nearIds)[c]];
            vertex.position[0] = c == 0 ? -.8f : c == 1 ? 0.f : .8f;
            vertex.position[1] = c == 1 ? .8f : -.8f;
            vertex.position[2] = face ? .8f : .2f;
            vertex.normal[2] = -1; vertex.weightPercent = 100; vertex.edgeDisabled = 1;
        }
        for (auto& vertex : vertices) { vertex.weightPercent = 100; vertex.edgeDisabled = 1; }
        auto& material = materials[0];
        std::fill_n(material.diffuse, 4, 1.f); material.diffuse[3] = .5f;
        std::fill_n(material.ambient, 3, 1.f); material.toonReference = 0xff;
        record->rawVertices = vertices.data(); record->boneTable = bones.data();
        record->materials = materials.data(); record->indices = indices.data();
        record->vertexCount = unsigned(vertices.size()); record->boneCount = 1;
        record->materialCount = 1; record->loadComplete = 1; record->comboSelIndex = 0;
        host.app->ModelSlot(0) = reinterpret_cast<unsigned char*>(record.get());
        Set(false, false);
    }
    ~Geometry() {
        ShutdownD3D12Native(); host.app->ModelSlot(0) = nullptr;
        model_vertex_cache::Forget(reinterpret_cast<unsigned char*>(record.get()));
    }
    void Set(bool two, bool farFirst) {
        const unsigned nearIds[] = {9, 2, 7}, farIds[] = {10, 3, 8};
        for (unsigned face = 0; face < 2; ++face) for (unsigned c = 0; c < 3; ++c) {
            indices[(face * 3 + c) * 2] = WORD((bool(face) != farFirst ? farIds : nearIds)[c]);
            indices[(face * 3 + c) * 2 + 1] = 0x5a5a;
        }
        record->indexCount = two ? 6 : 3; materials[0].faceVertexCount = record->indexCount;
    }
};

DWORD ReadCenter(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    stream.read(reinterpret_cast<char*>(&file), sizeof file);
    stream.read(reinterpret_cast<char*>(&info), sizeof info);
    Check(bool(stream) && file.bfType == 0x4d42 && info.biWidth > 0 && info.biWidth <= LONG(Host::width) &&
        info.biHeight < 0 && info.biHeight >= -LONG(Host::height) &&
        info.biBitCount == 32 && info.biCompression == BI_RGB, "native capture has the owned expected BMP shape");
    const auto center = std::streamoff(-info.biHeight/2) * info.biWidth + info.biWidth/2;
    stream.seekg(std::streamoff(file.bfOffBits) + center * 4);
    DWORD pixel = 0; stream.read(reinterpret_cast<char*>(&pixel), sizeof pixel);
    Check(bool(stream), "read the actual native center pixel");
    return pixel;
}

DWORD Capture(Host& host, const fs::path& destination) {
    Check(!fs::exists(destination), "capture never overwrites an existing file");
    const SettingsSnapshot before(*host.app);
    const bool rendered = RenderD3D12Native(host.app.get(), host.World());
    std::printf("native frame=%ls rendered=%d status=%s\n", destination.filename().c_str(), rendered, D3D12NativeStatus());
    Check(rendered && std::strcmp(D3D12NativeStatus(), "D3D12 native active") == 0,
          "matched hardware renders a real native frame without fallback");
    Check(WaitD3D12Native(), "real native queue completes");
    Check(ValidateD3D12Native(destination.c_str()), "real GPU indices validate and native frame is captured");
    before.Verify(*host.app); Hidden(host);
    Check(host.Child() != nullptr, "successful native frame owns a child viewport");
    return ReadCenter(destination);
}

#include "d3d12_antialias_checks.inc"

bool HardwareChecks(Host& host, const fs::path& output) {
    Check(output.is_absolute() && !fs::exists(output) && fs::is_directory(output.parent_path()),
          "hardware output must be a new absolute directory with an existing parent");
    processor::ResetForTesting();
    host.factory = Direct3DCreate9(D3D_SDK_VERSION);
    if (!host.factory) { std::puts("SKIP hardware: D3D9 runtime unavailable"); return false; }
    std::wstring reason;
    const unsigned ordinal = processor::ResolveRenderer(host.factory, processor::Selection{}, reason);
    const auto& actual = processor::ActualRenderer();
    if (!actual.adapterId) { std::puts("SKIP hardware: main D3D9 adapter has no verified LUID"); return false; }
    const auto adapters = gpu::EnumerateAdapters();
    if (std::none_of(adapters.begin(), adapters.end(), [&](const auto& a) { return a.id == actual.adapterId; })) {
        std::puts("SKIP hardware: no matching D3D12 hardware adapter/runtime"); return false;
    }
    ID3D12Debug* debug = nullptr;
    const HRESULT debugResult = graphics_runtime::GetDebugInterface(IID_PPV_ARGS(&debug));
    Release(debug);
    if (FAILED(debugResult)) { std::puts("SKIP hardware: native capture validation requires the unavailable D3D12 debug component"); return false; }
    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE; parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = host.window;
    parameters.BackBufferWidth = Host::width;
    parameters.BackBufferHeight = Host::height;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    Hr(host.factory->CreateDevice(ordinal, D3DDEVTYPE_HAL, host.window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, &host.device),
        "create actual matched D3D9 HAL device");
    processor::RecordDeviceResult(true, false, L"");
    host.renderer->device = host.device; host.renderer->d3d9 = host.factory;
    host.renderer->hwnd = host.window; host.renderer->aspectRatio = 1;
    host.app->Renderer() = host.renderer.get();
    RECT client{};
    Check(GetClientRect(host.window, &client) != FALSE, "read owned viewport client");
    const auto panels = MainPanelLayout(host.app.get(), client.bottom);
    const auto rows = FitViewportToolbarRows(panels.bottomTop, ui_scale::Percent());
    FrameViewportGeometry geometryBounds;
    Check(CalculateFrameViewport(client, host.app->SidebarWidth(), 0, FrameViewportMode::Docked,
        host.app->RenderWidth(), host.app->RenderHeight(), geometryBounds,
        0, 0, 0, panels.bottomExtra, rows), "calculate an admissible editor viewport");
    host.app->ViewportRect() = geometryBounds.scene;
    Check(FrameViewportRenderable(host.app.get()), "main viewport size is admitted by the actual route");
    Hr(host.device->SetTransform(D3DTS_WORLD, &host.identity), "set owned world transform");
    Hr(host.device->SetTransform(D3DTS_VIEW, &host.identity), "set owned view transform");
    Hr(host.device->SetTransform(D3DTS_PROJECTION, &host.identity), "set owned projection transform");
    Check(d3dx::Get().Load(), "load real D3DX math for model packing");
    Check(fs::create_directory(output), "create only the fresh hardware output directory");
    Geometry geometry(host);
    host.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    host.app->TransparentSortCrossObjects() = false;
    EnableD3D12Native(true, true);
    const DWORD one = Capture(host, output / L"one-face.bmp");
    geometry.Set(true, false);
    const auto sourceIndices = geometry.indices;
    const DWORD nearFirst = Capture(host, output / L"near-first.bmp");
    Check(geometry.indices == sourceIndices, "Off rendering preserves the near-first author index array");
    geometry.Set(true, true);
    const auto reversedIndices = geometry.indices;
    const DWORD farFirst = Capture(host, output / L"far-first.bmp");
    Check(geometry.indices == reversedIndices, "Off rendering preserves the reversed author index array");
    for (unsigned shift : {0u, 8u, 16u}) {
        const int a = int((one >> shift) & 255), b = int((nearFirst >> shift) & 255), c = int((farFirst >> shift) & 255);
        Check(a > 90 && a < 160 && std::abs(a - b) <= 2 && c > b + 35,
              "Off keeps near-first authored depth rejection; reversing source faces adds a second layer");
    }
    geometry.Set(true, false);
    Check(geometry.indices == sourceIndices, "fixture restores exact nonsequential PMD source indices and companion words");
    Capture(host, output / L"before-switch.bmp");
    const HWND oldChild = host.Child();
    host.app->TransparentSortCrossObjects() = true;
    for (auto precision : {TransparentSortPrecision::Low, TransparentSortPrecision::Standard,
                           TransparentSortPrecision::High, TransparentSortPrecision::Maximum}) {
        host.app->TransparentMaterialSortPrecision() = precision;
        Check(ValidateD3D12Native(nullptr), "validation uses the last submitted ordering after a UI setting changes");
        const SettingsSnapshot changed(*host.app);
        const DWORD sorted = Capture(host, output / (L"sorted-" + std::to_wstring(unsigned(precision)) + L".bmp"));
        Check(IsWindow(oldChild) && host.Child() == oldChild, "sorting keeps the original DX12 viewport alive");
        for (unsigned shift : {0u, 8u, 16u})
            Check(std::abs(int((sorted >> shift) & 255) - int((farFirst >> shift) & 255)) <= 2,
                  "DX12 sorts the overlapping transparent faces far first");
        Check(geometry.indices == sourceIndices, "GPU sorting keeps author indices intact");
        changed.Verify(*host.app);
    }
    host.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    const DWORD resumed = Capture(host, output / L"resumed-off.bmp");
    for (unsigned shift : {0u, 8u, 16u})
        Check(std::abs(int((resumed >> shift) & 255) - int((nearFirst >> shift) & 255)) <= 2,
              "returning to Off restores authored order while retaining cross-object preference");
    Check(host.app->TransparentSortCrossObjects(), "Off does not erase stored cross-object preference");
    host.app->antialiasing.initialized=true;
    host.app->antialiasing.enabled=false;
    geometry.Set(false,false);
    geometry.materials[0].diffuse[3]=1;
    const auto pixels=[&](const fs::path& file){
        std::ifstream stream(file,std::ios::binary);BITMAPFILEHEADER header{};BITMAPINFOHEADER info{};
        stream.read(reinterpret_cast<char*>(&header),sizeof header);stream.read(reinterpret_cast<char*>(&info),sizeof info);
        std::vector<DWORD> data(size_t(info.biWidth)*unsigned(-info.biHeight));stream.seekg(header.bfOffBits);
        stream.read(reinterpret_cast<char*>(data.data()),data.size()*4);Check(bool(stream),"read AA capture pixels");return data;
    };
    Capture(host,output/L"aa-off.bmp");const auto off=pixels(output/L"aa-off.bmp");
    {
    DepthProbe depth;
    for(unsigned algorithm=0;algorithm<unsigned(antialias::Algorithm::Count);++algorithm) {
        host.app->antialiasing.enabled=true;host.app->antialiasing.algorithm=antialias::Algorithm(algorithm);
        const auto legacySamples=host.renderer->presentParameters.MultiSampleType;
        antialias::PrepareFrame(host.app.get());
        Check(host.renderer->presentParameters.MultiSampleType==legacySamples,"native AA preparation does not reset the D3D9 auxiliary device");
        const auto file=output/(L"aa-"+std::to_wstring(algorithm)+L".bmp");
        for(unsigned sample=0;sample<8;++sample)if(!RenderD3D12Native(host.app.get(),host.World()))throw std::runtime_error(D3D12NativeStatus());
        Capture(host,file);const auto filtered=pixels(file);
        Check(filtered!=off,"each enabled AA algorithm changes real GPU edge coverage");
        Check(host.Child()==oldChild,"AA selection preserves the DX12 viewport");
        depth.Verify();
    }
    host.app->antialiasing.enabled=false;
    Capture(host,output/L"aa-disabled-again.bmp");
    Check(pixels(output/L"aa-disabled-again.bmp")==off,"disabling AA restores the original unfiltered frame");
    }
    for(unsigned algorithm:{5u,8u,9u}) {
        host.app->antialiasing.enabled=true;host.app->antialiasing.algorithm=antialias::Algorithm(algorithm);
        host.app->CurrentFrame()=0;
        const auto prefix=L"temporal-"+std::to_wstring(algorithm);
        Capture(host,output/(prefix+L"-fresh.bmp"));const auto fresh=pixels(output/(prefix+L"-fresh.bmp"));
        for(unsigned i=0;i<8;++i)Check(RenderD3D12Native(host.app.get(),host.World()),"warm moving-surface history");
        geometry.bones[0].matInit[12]=1.4f;host.app->CurrentFrame()=1;
        const DWORD exposed=Capture(host,output/(prefix+L"-moved.bmp"));
        Check((exposed&0xffffff)==0,"moving geometry leaves no history at the newly exposed center");
        geometry.bones[0].matInit[12]=0;host.app->CurrentFrame()=100;
        Capture(host,output/(prefix+L"-seek.bmp"));
        Check(pixels(output/(prefix+L"-seek.bmp"))==fresh,"timeline seek discards temporal history and restarts jitter");
        host.app->viewportRender.automatic=false;host.app->viewportRender.percent=50;
        Capture(host,output/(prefix+L"-resize.bmp"));
        host.app->viewportRender.automatic=true;host.app->viewportRender.percent=100;
        Capture(host,output/(prefix+L"-restore.bmp"));
        Check(pixels(output/(prefix+L"-restore.bmp"))==fresh,"viewport resize resets history without a DX9 fallback");
    }
    MipChecks(host,geometry,output);
    std::puts("PASS optional hardware authored order, stable sorting and DX12 viewport continuity");
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        Check(argc == 1 || (argc == 3 && !wcscmp(argv[1], L"--hardware")),
              "usage: d3d12_sort_policy_tests [--hardware NEW_ABSOLUTE_OUTPUT_DIRECTORY]");
        DefaultDesktop();
        Host host;
#if defined(MMD_ENABLE_DX12) && MMD_ENABLE_DX12 && !MMD_ORIGINAL_PROFILE
        RouteChecks(host);
        const bool hardware = argc == 3 && HardwareChecks(host, fs::path(argv[2]));
        if (argc == 1) std::puts("SKIP optional hardware: not requested");
        std::printf("PASS d3d12_sort_policy_tests %u checks; routing=passed hardware=%s\n", checks, hardware ? "passed" : "skipped");
#else
        EnableD3D12Native(true);
        Check(!RenderD3D12Native(host.app.get(), host.World()) && !ValidateD3D12Native(nullptr) && !WaitD3D12Native(),
              "DX12-disabled build keeps unavailable entry points inert");
        Check(host.Child() == nullptr, "disabled build creates no native child");
        std::printf("PASS disabled-build contract %u checks; SKIP native routing and hardware (not compiled)\n", checks);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL d3d12_sort_policy_tests: %s (%u checks), status=%s\n", error.what(), checks, D3D12NativeStatus());
        return 1;
    }
}
