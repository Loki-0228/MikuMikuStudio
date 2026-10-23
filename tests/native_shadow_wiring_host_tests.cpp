// MAINTENANCE: regression-keep; actual UpdateAdaptiveShadowMap wiring.
// An owned hidden HWND, a real D3D9 device, a real two-vertex VB and the real
// production effect exercise the coverage/detail shadow planner. This is not a
// loader, visible rendering, performance or GPU-image acceptance test. It does
// not create settings, plugin services, files or a private desktop. The
// production shader path is supplied by the runner; a zero ShadowResolutionLimit
// keeps the allocation at the 512 coverage minimum.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <MinHook.h>
#include "mikumikustudio/adaptive_shadow.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/shadow_filter.hpp"
#include "mikumikustudio/viewport_render_scale.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

static_assert(!MMD_ORIGINAL_PROFILE, "Production shadow wiring belongs to the full profile only");
using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
namespace {
using Matrix = std::array<float, 16>;
unsigned checks = 0;
const char* stage = "setup";
void Require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL shadow wiring host [%s] after %u checks: %s\n", stage, checks, message); std::fflush(nullptr); std::exit(1); }
}
Matrix Identity() { Matrix m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (unsigned row = 0; row < 4; ++row) for (unsigned column = 0; column < 4; ++column) {
        double value = 0; for (unsigned k = 0; k < 4; ++k) value += double(a[row * 4 + k]) * b[k * 4 + column];
        out[row * 4 + column] = float(value);
    }
    return out;
}
Matrix Projection(bool orthographic, bool jitter) {
    Matrix out{}; out[0] = .5f; out[5] = .75f;
    if (orthographic) { out[10] = .001f; out[15] = 1; }
    else { out[10] = 1.001f; out[11] = 1; out[14] = -1.001f; }
    if (jitter) for (unsigned row = 0; row < 4; ++row) {
        out[row * 4] += (1.f / 1024) * out[row * 4 + 3];
        out[row * 4 + 1] -= (1.f / 512) * out[row * 4 + 3];
    }
    return out;
}
NativeShadowBounds SceneBounds() {
    NativeShadowBounds bounds; const float low[3]{-1000, -1000, -1000}, high[3]{1000, 1000, 1000};
    bounds.Include(low); bounds.Include(high); return bounds;
}
bool NearMatrix(const Matrix& a, const Matrix& b) {
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(double(a[i]) - b[i]) > 1e-7 + std::abs(double(b[i])) * 4e-6) return false;
    return true;
}
void SameMatrix(const Matrix& actual, const Matrix& expected, const char* message) {
    if (!NearMatrix(actual, expected)) for (unsigned row = 0; row < 4; ++row) {
        std::printf("MATRIX row=%u actual=%g,%g,%g,%g expected=%g,%g,%g,%g\n", row,
            actual[row*4], actual[row*4+1], actual[row*4+2], actual[row*4+3],
            expected[row*4], expected[row*4+1], expected[row*4+2], expected[row*4+3]);
    }
    Require(NearMatrix(actual, expected), message);
}
void Covers(const Matrix& matrix, const NativeShadowBounds& bounds) {
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner); double clip[4]{};
        for (unsigned col = 0; col < 4; ++col) {
            clip[col] = matrix[12 + col];
            for (unsigned row = 0; row < 3; ++row) clip[col] += double(p[row]) * matrix[row * 4 + col];
            Require(std::isfinite(clip[col]), "returned coverage projection has finite corner coordinates");
        }
        Require(clip[3] > 0, "coverage projection keeps positive homogeneous W");
        const double depth = clip[2] / clip[3];
        Require(depth >= -1e-5 && depth <= 1 + 1e-5, "coverage projection covers the complete scene depth");
        for (unsigned axis = 0; axis < 2; ++axis) {
            const double value = clip[axis] / clip[3];
            Require(value >= -1 - 1e-5 && value <= 1 + 1e-5, "coverage projection covers every scene XY corner");
        }
    }
}
void RequireDefaultDesktop() {
    wchar_t name[256]{}; DWORD bytes = 0;
    Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof(name), &bytes) != FALSE,
        "read inherited test desktop identity");
    Require(!_wcsicmp(name, L"Default"), "shadow wiring host must run on Default; refusing another desktop rather than claiming a pass");
    std::wprintf(L"DESKTOP %ls\n", name);
}
struct Effect {
    void* value = nullptr;
    Effect() = default;
    Effect(Effect&& other) noexcept : value(other.value) { other.value = nullptr; }
    Effect& operator=(Effect&& other) noexcept { if (this != &other) { if (value) static_cast<IUnknown*>(value)->Release(); value = other.value; other.value = nullptr; } return *this; }
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
    ~Effect() { if (value) static_cast<IUnknown*>(value)->Release(); }
};
Effect LoadEffect(IDirect3DDevice9* device, const wchar_t* file) {
    std::ifstream stream(file, std::ios::binary);
    Require(bool(stream), "read the production shader fixture supplied by the runner");
    const std::string source{std::istreambuf_iterator<char>(stream), {}};
    static HMODULE module = LoadLibraryW(L"d3dx9_43.dll");
    using Create = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT, const void*, const void*, DWORD, void*, void**, void**);
    const auto create = reinterpret_cast<Create>(module ? GetProcAddress(module, "D3DXCreateEffect") : nullptr);
    Require(create != nullptr, "load the production D3DX effect entry point");
    Effect effect; void* errors = nullptr;
    const auto hr = create(device, source.data(), UINT(source.size()), nullptr, nullptr, 0, nullptr, &effect.value, &errors);
    if (errors) {
        using Data = void*(WINAPI*)(void*);
        std::printf("%s\n", static_cast<const char*>(d3dx::EffectMethod<Data, 3>(errors)(errors)));
        static_cast<IUnknown*>(errors)->Release();
    }
    Require(SUCCEEDED(hr) && effect.value, "compile the production shadow shader");
    return effect;
}
using GetValue = HRESULT(WINAPI*)(void*, const char*, void*, UINT);
struct Host {
    HWND window = nullptr;
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    ComPtr<IDirect3D9> runtime;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DVertexBuffer9> vertices;
    Effect effect;
    MMDApp* previous = g_Block;
    explicit Host(const wchar_t* shader) {
        window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"MMS shadow wiring isolated host", WS_OVERLAPPED,
            0, 0, 96, 96, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DWORD process = 0; const auto thread = GetWindowThreadProcessId(window, &process);
        Require(window && thread == GetCurrentThreadId() && process == GetCurrentProcessId() && !IsWindowVisible(window),
            "create only a hidden inactive window owned by this test thread/process");
        runtime.Attach(Direct3DCreate9(D3D_SDK_VERSION)); Require(bool(runtime), "create real D3D9 runtime");
        D3DPRESENT_PARAMETERS parameters{}; parameters.Windowed = TRUE; parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = window; parameters.BackBufferWidth = parameters.BackBufferHeight = 64;
        parameters.BackBufferFormat = D3DFMT_UNKNOWN; parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        constexpr DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
        auto result = runtime->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &parameters, &device);
        if (FAILED(result)) result = runtime->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, window, flags, &parameters, &device);
        Require(SUCCEEDED(result) && device, "real D3D9 device is required; unavailable prerequisites are failures, not skipped passes");
        Require(d3dx::Get().Load(), "load production D3DX matrix operations without initializing the application or shaders");
        std::memset(&app->state, 0, sizeof(app->state)); app->Hwnd() = window; app->HInstance() = GetModuleHandleW(nullptr);
        app->Renderer() = renderer.get(); renderer->hwnd = window; renderer->device = device.Get(); renderer->d3d9 = runtime.Get(); renderer->aspectRatio = 1;
        app->CameraMode() = 0; app->CameraReferenceMode() = CameraAttachmentReference::None; app->CameraParentModel() = -1;
        app->ViewOffsetX() = app->ViewOffsetY() = 0; app->CameraDistance() = -10; app->CameraFov() = 30;
        app->ShadowDistance() = .1f; app->ShadowMode() = 1; app->AdaptiveShadowResolution() = false;
        app->ShadowResolutionLimit() = 0; app->viewportRender.automatic = true;
        // The two real buffer positions establish the exact scene AABB. The
        // synthetic metadata exercises only bounds collection, never drawing
        // or claiming that a file loader accepted the absent index contents.
        struct Vertex { float position[3], normal[3], uv[2]; };
        static_assert(sizeof(Vertex) == 32, "production unextended vertex stride");
        const Vertex data[2]{{{-1000,-1000,-1000},{0,0,1},{0,0}}, {{1000,1000,1000},{0,0,1},{0,0}}};
        Require(SUCCEEDED(device->CreateVertexBuffer(sizeof(data), 0, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1,
            D3DPOOL_MANAGED, &vertices, nullptr)) && vertices, "create real readable thirty-two-byte-stride vertex buffer");
        void* storage = nullptr; Require(SUCCEEDED(vertices->Lock(0, sizeof(data), &storage, 0)) && storage, "lock owned VB for the two known positions");
        std::memcpy(storage, data, sizeof(data)); Require(SUCCEEDED(vertices->Unlock()), "publish the owned real VB contents");
        model->loadComplete = 1; model->indexCount = 3; model->vertexCount = 2; model->vertexBuffer = vertices.Get(); model->pmxAdditionalUvCount = 0;
        app->ModelSlot(0) = reinterpret_cast<unsigned char*>(model.get()); g_Block = app.get();
        effect = LoadEffect(device.Get(), shader);
        renderer->effect = static_cast<ID3DXEffect*>(effect.value);
        renderer->shaderModelCaps = 3;
        Light(true);
    }
    ~Host() {
        renderer->effect = nullptr;
        if (renderer->hdrTexture) { renderer->hdrTexture->Release(); renderer->hdrTexture = nullptr; }
        if (renderer->shadowSurface) { renderer->shadowSurface->Release(); renderer->shadowSurface = nullptr; }
        if (renderer->shadowDepthSurface) { renderer->shadowDepthSurface->Release(); renderer->shadowDepthSurface = nullptr; }
        app->ModelSlot(0) = nullptr; model->vertexBuffer = nullptr; app->Renderer() = nullptr; g_Block = previous;
        renderer->device = nullptr; renderer->d3d9 = nullptr;
        vertices.Reset(); device.Reset(); runtime.Reset();
        Require(DestroyWindow(window) != FALSE, "destroy only the owned hidden test window after COM owners are released");
    }
    void Light(bool safeReference) {
        const float light[3]{safeReference ? 0.f : -.5f, safeReference ? 0.f : -1.f, safeReference ? 1.f : .5f};
        std::copy_n(light, 3, app->LightDirection()); app->ApplyTimelineLightState();
    }
    void Install(const Matrix& view, const Matrix& projection) {
        Require(SUCCEEDED(device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view.data()))) &&
            SUCCEEDED(device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(projection.data()))),
            "install actual D3D9 VIEW and PROJECTION transforms");
    }
    std::array<float, 3> Focus() {
        float frameFocus[3]{}; float focus[3]{};
        const auto source = ResolveFrameFocus(app.get(), static_cast<int>(app->CameraReferenceMode()), frameFocus);
        for (int axis = 0; axis < 3; ++axis) focus[axis] = frameFocus[axis];
        if (source == FrameFocusSource::FreeView) { focus[0] = app->ViewOffsetX(); focus[1] = app->ViewOffsetY(); }
        return {focus[0], focus[1], focus[2]};
    }
    Matrix Expected() {
        const auto scene = SceneBounds();
        const auto focus = Focus();
        return NativeShadowProjection(focus.data(), app->LightDirection(), .1f, 1, &scene);
    }
    Matrix Call(const Matrix& frame) {
        const auto revision = app->SceneRevision(); const bool dirty = bool(app->SceneModified());
        const auto recorded = app->RecordedFrameCountByte1(), physics = app->PhysicsResetPending(), playing = app->PlaybackActive();
        const auto time = app->CurrentFrame();
        ComPtr<IDirect3DSurface9> before, after; D3DVIEWPORT9 oldViewport{}, newViewport{};
        Require(SUCCEEDED(device->GetRenderTarget(0, &before)) && SUCCEEDED(device->GetViewport(&oldViewport)), "capture existing device target and viewport without sampling camera transforms");
        const auto actual = UpdateAdaptiveShadowMap(app.get(), frame.data());
        for (float value : actual) Require(std::isfinite(value), "actual production update returns a wholly finite light projection");
        Require(SUCCEEDED(device->GetRenderTarget(0, &after)) && SUCCEEDED(device->GetViewport(&newViewport)) && before.Get() == after.Get() &&
            !std::memcmp(&oldViewport, &newViewport, sizeof(oldViewport)), "projection and planning leave the actual render target and viewport intact");
        Require(app->SceneRevision() == revision && bool(app->SceneModified()) == dirty && app->RecordedFrameCountByte1() == recorded &&
            app->PhysicsResetPending() == physics && app->PlaybackActive() == playing && app->CurrentFrame() == time && !IsWindowVisible(window),
            "direct production update does not edit scene/dirty/history inputs, physics, playhead, playback or window visibility");
        int stable = 0;
        Require(SUCCEEDED((d3dx::EffectMethod<GetValue, 21>(effect.value)(effect.value, "MdsStableShadowGrid", &stable, sizeof(stable)))) && stable == 1,
            "production update enables the shared stable shadow grid on the real effect");
        return actual;
    }
};
Matrix CameraView() { auto view = Identity(); view[12] = 3; view[13] = -6; view[14] = 8; return view; }
const Matrix kFrame{0,-3,0,0, 2,1,0,0, 1,0,4,0, 2,-1,3,1};
void CoverageCases(Host& host) {
    stage = "coverage planning";
    const auto scene = SceneBounds();
    for (bool safeReference : {true, false}) for (const Matrix& frame : std::array<Matrix, 2>{Identity(), kFrame}) {
        host.Light(safeReference);
        const auto expected = host.Expected();
        Matrix baseline{};
        bool haveBaseline = false;
        unsigned variants = 0;
        for (int mode : {1, 2}) for (float range : {.1f, .02f})
            for (bool orthographic : {false, true}) for (bool jitter : {false, true}) {
            host.app->ShadowMode() = mode;
            host.app->ShadowDistance() = range;
            const auto view = CameraView(), projection = Projection(orthographic, jitter);
            host.Install(view, projection);
            const auto actual = host.Call(frame);
            SameMatrix(actual, expected, "actual update returns the fixed coverage projection computed from the real scene and light");
            Covers(actual, scene);
            if (!haveBaseline) { baseline = actual; haveBaseline = true; }
            else SameMatrix(actual, baseline, "camera pose, AA jitter and precision never change the coverage mapping");
            ++variants;
        }
        Require(variants == 16, "every coverage variant was actually executed");
    }
    // The effect ramp matrix must be the production inverse(coverage)*ramp,
    // where the mode-2 camera-relative Y gain is zeroed for stability.
    auto& api = d3dx::Get();
    Matrix inverse{}, ramp{};
    const auto expected = host.Expected();
    auto* inverseMatrix = reinterpret_cast<d3dx::D3DXMATRIXF*>(inverse.data());
    Require(api.inverse(inverseMatrix, nullptr, reinterpret_cast<const d3dx::D3DXMATRIXF*>(expected.data())) != nullptr, "invert the real coverage mapping");
    const auto focus = host.Focus();
    auto rampProjection = NativeShadowProjection(focus.data(), host.app->LightDirection(), .1f, 1);
    for (int row = 0; row < 4; ++row) rampProjection[row * 4 + 1] = 0;
    api.multiply(reinterpret_cast<d3dx::D3DXMATRIXF*>(ramp.data()), inverseMatrix,
        reinterpret_cast<const d3dx::D3DXMATRIXF*>(rampProjection.data()));
    Matrix actualRamp{};
    Require(SUCCEEDED((d3dx::EffectMethod<GetValue, 21>(host.effect.value)(host.effect.value, "MdsShadowToNative", &actualRamp, sizeof(actualRamp)))),
        "read the production ramp conversion from the real effect");
    SameMatrix(actualRamp, ramp, "real effect receives inverse(coverage)*stable-ramp");
    Require(host.renderer->hdrTexture && host.renderer->shadowSurface && host.renderer->shadowDepthSurface &&
        host.renderer->renderTargetWidth == 512 && host.renderer->renderTargetHeight == 512,
        "zero precision limit still allocates the 512 minimum coverage map");
    auto* texture = host.renderer->hdrTexture;
    (void)host.Call(kFrame);
    Require(host.renderer->hdrTexture == texture, "unchanged precision reuses the existing coverage target");
}
using GetTransform = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
GetTransform originalGet = nullptr;
IDirect3DDevice9* observedDevice = nullptr;
DWORD observedThread = 0;
D3DTRANSFORMSTATETYPE failureKind = D3DTS_WORLD;
bool armed = false;
unsigned failures = 0;
Matrix injected{};
HRESULT WINAPI ReadTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE kind, D3DMATRIX* out) {
    const auto result = originalGet(device, kind, out);
    if (device == observedDevice && GetCurrentThreadId() == observedThread && armed && kind == failureKind) {
        Require(SUCCEEDED(result) && out, "controlled failure begins with a real successful transform read on exactly this test device");
        // A failed API does not promise usable output. Deliberately retaining
        // legal matrix bytes makes ignoring HRESULT observable; no driver
        // failure or device-loss behavior is claimed by this bounded fault.
        std::memcpy(out, injected.data(), sizeof(D3DMATRIX)); armed = false; ++failures; return E_FAIL;
    }
    return result;
}
struct TransformHook {
    void* target = nullptr;
    explicit TransformHook(IDirect3DDevice9* device) {
        Require(MH_Initialize() == MH_OK, "initialize hooks owned exclusively by this isolated test");
        observedDevice = device; observedThread = GetCurrentThreadId();
        target = (*reinterpret_cast<void***>(device))[45]; // IDirect3DDevice9::GetTransform.
        Require(MH_CreateHook(target, reinterpret_cast<void*>(&ReadTransform), reinterpret_cast<void**>(&originalGet)) == MH_OK &&
            MH_EnableHook(target) == MH_OK, "install the actual GetTransform hook; unsupported entry points fail the prerequisite explicitly");
    }
    ~TransformHook() {
        armed = false; Require(MH_DisableHook(target) == MH_OK && MH_RemoveHook(target) == MH_OK && MH_Uninitialize() == MH_OK,
            "remove this test's transform hook before releasing its real device"); observedDevice = nullptr; originalGet = nullptr;
    }
    void Fail(D3DTRANSFORMSTATETYPE kind, const Matrix& matrix) { Require(!armed, "one bounded matrix fault at a time"); failureKind = kind; injected = matrix; armed = true; }
};
void FallbackCases(Host& host, TransformHook& hook) {
    stage = "device read failures";
    const auto baseline = host.Expected();
    const auto view = CameraView(), projection = Projection(false, true);
    host.Install(view, projection);
    const auto valid = host.Call(kFrame);
    SameMatrix(valid, baseline, "ordinary device reads produce the coverage mapping");
    for (auto kind : {D3DTS_PROJECTION, D3DTS_VIEW}) {
        const auto count = failures; hook.Fail(kind, kind == D3DTS_VIEW ? view : projection);
        const auto failed = host.Call(kFrame);
        Require(!armed && failures == count + 1, "production read actually consumes exactly one current-device HRESULT failure");
        SameMatrix(failed, baseline, "VIEW/PROJECTION E_FAIL output is rejected even when the output matrix bytes look legal");
        Covers(failed, SceneBounds());
    }
    auto singular = view; singular[0] = singular[1] = singular[2] = 0;
    host.Install(singular, projection);
    const auto singularResult = host.Call(kFrame);
    SameMatrix(singularResult, baseline, "unsupported singular scene-to-view transform keeps the coverage mapping");
    host.Install(view, projection);
    const auto recovered = host.Call(kFrame);
    SameMatrix(recovered, valid, "ordinary device reads recover immediately after the one-shot controlled failures");
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Require(argc == 2, "usage: native_shadow_wiring_host_tests <production shader>");
    try {
        std::fprintf(stderr, "STAGE boot\n"); std::fflush(stderr);
        RequireDefaultDesktop();
        std::fprintf(stderr, "STAGE desktop\n"); std::fflush(stderr);
        const std::wstring shader(argv[1], argv[1] + std::strlen(argv[1]));
        Host host(shader.c_str());
        std::fprintf(stderr, "STAGE host\n"); std::fflush(stderr);
        TransformHook hook(host.device.Get());
        std::fprintf(stderr, "STAGE hook\n"); std::fflush(stderr);
        CoverageCases(host);
        std::fprintf(stderr, "STAGE coverage\n"); std::fflush(stderr);
        FallbackCases(host, hook);
        std::fprintf(stderr, "STAGE fallback\n"); std::fflush(stderr);
        std::printf("PASS actual shadow coverage/detail planning wiring: %u checks; no scene draw, visible rendering, file or service initialization\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL shadow wiring host [%s]: %s\n", stage, error.what()); return 1;
    }
}
