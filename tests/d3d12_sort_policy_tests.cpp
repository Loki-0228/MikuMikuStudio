// MAINTENANCE: regression-keep; --hardware is optional real-device coverage.
// No full application startup, plugins, global input, visible parent window,
// or automatic processor calibration. Native Validate checks GPU indices;
// independent single/two-layer pixels also detect a self-consistent old sorter.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <MinHook.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include "mikumikustudio/d3d12_native.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/d3d12_skinning.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/processor_settings.hpp"
#include "mikumikustudio/view_options.hpp"

#ifndef MMD_ENABLE_DX12
#error The test target must define MMD_ENABLE_DX12 to match mikumikustudio_core.
#endif

// Match the repository's native_edit_history_tests allocation seam. Only the
// current thread's narrowly armed interval rejects ordinary new/new[]. Other
// threads and all cleanup outside that interval retain normal allocation.
namespace native_policy_allocation {
thread_local bool reject = false;
thread_local unsigned attempts = 0;
struct Reject {
    bool previous = reject;
    Reject() { reject = true; }
    ~Reject() { reject = previous; }
};
}
void* operator new(std::size_t size) {
    if (native_policy_allocation::reject) {
        ++native_policy_allocation::attempts;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

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
            WS_POPUP, 0, 0, 96, 96, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create self-owned hidden parent");
        app->MainWindow() = window; app->Renderer() = nullptr;
        app->ViewportRect() = {0, 0, 96, 96};
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
              "renderer fallback does not edit the project");
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

void AllocationFreeFirstFallback(Host& host, InitializationProbe& probe) {
    // Calibrate the actual allocation seam before relying on zero attempts.
    // Calling an operator function through a volatile pointer avoids new-
    // expression allocation elision. No renderer has been called yet.
    void* (*volatile allocate)(std::size_t) = &::operator new;
    bool intercepted = false;
    native_policy_allocation::attempts = 0;
    {
        native_policy_allocation::Reject reject;
        try { void* memory = allocate(64); ::operator delete(memory); }
        catch (const std::bad_alloc&) { intercepted = true; }
    }
    Check(intercepted && native_policy_allocation::attempts == 1,
          "current-thread ordinary new failure injection is active");
    host.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Low;
    host.app->TransparentSortCrossObjects() = true;
    const SettingsSnapshot before(*host.app);
    EnableD3D12Native(true);
    native_policy_allocation::attempts = 0;
    probe.calls = 0; probe.armed = true;
    bool rendered = true, escaped = false, exactStatus = false;
    {
        native_policy_allocation::Reject reject;
        try {
            rendered = RenderD3D12Native(host.app.get(), host.World());
            exactStatus = std::strcmp(D3D12NativeStatus(),
                "D3D9 fallback: transparent sorting policy") == 0;
        } catch (...) { escaped = true; }
    } // Restore allocation before assertions, printing, or native teardown.
    probe.armed = false;
    std::printf("first-policy-fallback allocation-attempts=%u escaped=%d rendered=%d status=%s\n",
        native_policy_allocation::attempts, escaped, rendered, D3D12NativeStatus());
    Check(!escaped && !rendered && exactStatus && native_policy_allocation::attempts == 0,
          "first non-Off fallback and its exact status need no ordinary new allocation");
    Check(probe.calls == 0 && host.Child() == nullptr, "allocation-free gate still precedes native initialization");
    before.Verify(*host.app);

    // Prove enabled was retained without touching a graphics device. Off must
    // reach the existing recording-mode rejection before Native::Initialize;
    // a silently disabled request would return early with the policy reason.
    host.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    host.app->RecordingWindow() = host.window;
    const bool recordingRendered = RenderD3D12Native(host.app.get(), host.World());
    host.app->RecordingWindow() = nullptr;
    Check(!recordingRendered && std::strstr(D3D12NativeStatus(), "recording/floating/fullscreen") != nullptr,
          "policy fallback retains enabled request for the next eligible Off call");
    EnableD3D12Native(true);
    host.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Low;
    Check(!RenderD3D12Native(host.app.get(), host.World()) &&
          std::strcmp(D3D12NativeStatus(), "D3D9 fallback: transparent sorting policy") == 0,
          "explicit re-enable permits a fresh policy fallback");
    EnableD3D12Native(false);
    Check(std::strcmp(D3D12NativeStatus(), "disabled") == 0,
          "explicit disable clears the policy-fallback status");
}

void RouteChecks(Host& host) {
    processor::ResetForTesting();
    Check(host.app->Renderer() == nullptr && host.Child() == nullptr, "routing fixture has no renderer or native child");
    InitializationProbe probe;
    AllocationFreeFirstFallback(host, probe);
    EnableD3D12Native(true);
    for (auto precision : {TransparentSortPrecision::Low, TransparentSortPrecision::Standard,
                           TransparentSortPrecision::High, TransparentSortPrecision::Maximum,
                           static_cast<TransparentSortPrecision>(200)}) {
        for (bool cross : {false, true}) {
            host.app->TransparentMaterialSortPrecision() = precision;
            host.app->TransparentSortCrossObjects() = cross;
            const SettingsSnapshot before(*host.app);
            probe.calls = 0; probe.armed = true;
            const bool rendered = RenderD3D12Native(host.app.get(), host.World());
            probe.armed = false;
            std::printf("route precision=%u cross=%d rendered=%d init-queries=%u status=%s\n",
                unsigned(precision), cross, rendered, probe.calls, D3D12NativeStatus());
            Check(!rendered && probe.calls == 0, "non-Off returns before native initialization");
            Check(std::strstr(D3D12NativeStatus(), "transparent") != nullptr,
                  "non-Off has an explicit transparent-policy fallback reason");
            Check(host.Child() == nullptr && !WaitD3D12Native() && !ValidateD3D12Native(nullptr),
                  "policy fallback leaves no native child or valid stale frame");
            before.Verify(*host.app); Hidden(host);
        }
    }
    EnableD3D12Native(false);
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
    Check(bool(stream) && file.bfType == 0x4d42 && info.biWidth == 96 && info.biHeight == -96 &&
        info.biBitCount == 32 && info.biCompression == BI_RGB, "native capture has the owned expected BMP shape");
    stream.seekg(std::streamoff(file.bfOffBits) + (48 * 96 + 48) * 4);
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
    parameters.hDeviceWindow = host.window; parameters.BackBufferWidth = parameters.BackBufferHeight = 96;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    Hr(host.factory->CreateDevice(ordinal, D3DDEVTYPE_HAL, host.window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, &host.device),
        "create actual matched D3D9 HAL device");
    processor::RecordDeviceResult(true, false, L"");
    host.renderer->device = host.device; host.renderer->d3d9 = host.factory;
    host.renderer->hwnd = host.window; host.renderer->aspectRatio = 1;
    host.app->Renderer() = host.renderer.get();
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
    host.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::High;
    host.app->TransparentSortCrossObjects() = true;
    // Before the next Render, validation still describes the previously
    // submitted Off frame, not the newly requested UI setting.
    Check(ValidateD3D12Native(nullptr), "validation uses last submitted Off frame after settings change");
    const SettingsSnapshot changed(*host.app);
    Check(!RenderD3D12Native(host.app.get(), host.World()), "new non-Off frame requests D3D9 fallback");
    Check(!IsWindow(oldChild) && host.Child() == nullptr, "policy transition destroys the old native child");
    const auto stale = output / L"must-not-exist.bmp";
    Check(!ValidateD3D12Native(stale.c_str()) && !fs::exists(stale) && !WaitD3D12Native(),
          "fallback cannot validate or save a stale native frame");
    changed.Verify(*host.app); Hidden(host);
    // Do not call Enable again: the user's native-preview request must survive
    // the temporary policy fallback and recover when Off becomes applicable.
    host.app->TransparentMaterialSortPrecision() = TransparentSortPrecision::Off;
    const DWORD resumed = Capture(host, output / L"resumed-off.bmp");
    for (unsigned shift : {0u, 8u, 16u})
        Check(std::abs(int((resumed >> shift) & 255) - int((nearFirst >> shift) & 255)) <= 2,
              "returning to Off restores authored order while retaining cross-object preference");
    Check(host.app->TransparentSortCrossObjects(), "Off does not erase stored cross-object preference");
    std::puts("PASS optional hardware Off order, child teardown, stale-frame rejection and recovery");
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
        std::fprintf(stderr, "FAIL d3d12_sort_policy_tests: %s (%u checks)\n", error.what(), checks);
        return 1;
    }
}
