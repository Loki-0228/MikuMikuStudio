// MAINTENANCE: regression-keep; actual UpdateAdaptiveShadowMap wiring.
// An owned hidden HWND, a real D3D9 device and real two-vertex VB exercise the
// production bounds collector. This is not a loader, shader, visible rendering,
// performance or GPU-image acceptance test. No settings, plugin service, file,
// global input or private desktop is created. A zero test-only resolution cap
// returns the actual computed projection before shadow-RT allocation/rendering.
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
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <memory>

static_assert(!MMD_ORIGINAL_PROFILE, "Production receiver wiring belongs to the full profile only");
using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
namespace {
using Matrix = std::array<float, 16>;
unsigned checks = 0;
const char* stage = "setup";
void Require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL receiver host [%s] after %u checks: %s\n", stage, checks, message); std::fflush(nullptr); std::exit(1); }
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
void Covers(const Matrix& matrix, const NativeShadowBounds& bounds, bool xyz) {
    Require(FiniteNativeShadowBounds(bounds), "coverage oracle has a complete finite source AABB");
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto p = bounds.Corner(corner); double clip[4]{};
        for (unsigned col = 0; col < 4; ++col) {
            clip[col] = matrix[12 + col];
            for (unsigned row = 0; row < 3; ++row) clip[col] += double(p[row]) * matrix[row * 4 + col];
            Require(std::isfinite(clip[col]), "actual returned projection has finite corner coordinates");
        }
        Require(clip[3] > 0, "actual shadow projection keeps positive homogeneous W");
        const double depth = clip[2] / clip[3];
        Require(depth >= -1e-5 && depth <= 1 + 1e-5, "actual projection covers the complete required scene depth");
        if (xyz) for (unsigned axis = 0; axis < 2; ++axis) {
            const double value = clip[axis] / clip[3];
            Require(value >= -1 - 1e-5 && value <= 1 + 1e-5, "actual projection covers every receiver XY corner");
        }
    }
}
void RequireDefaultDesktop() {
    wchar_t name[256]{}; DWORD bytes = 0;
    Require(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name, sizeof(name), &bytes) != FALSE,
        "read inherited test desktop identity");
    Require(!_wcsicmp(name, L"Default"), "receiver host must run on Default; refusing another desktop rather than claiming a pass");
    std::wprintf(L"DESKTOP %ls\n", name);
}

struct Host {
    HWND window = nullptr;
    std::unique_ptr<MMDApp> app = std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer = std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    ComPtr<IDirect3D9> runtime;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DVertexBuffer9> vertices;
    MMDApp* previous = g_Block;
    Host() {
        window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"MMS receiver wiring isolated host", WS_OVERLAPPED,
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
        Light(true);
    }
    ~Host() {
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
    Matrix Call(const Matrix& frame) {
        const auto revision = app->SceneRevision(); const bool dirty = bool(app->SceneModified());
        const auto recorded = app->RecordedFrameCountByte1(), physics = app->PhysicsResetPending(), playing = app->PlaybackActive();
        const auto time = app->CurrentFrame();
        ComPtr<IDirect3DSurface9> before, after; D3DVIEWPORT9 oldViewport{}, newViewport{};
        Require(SUCCEEDED(device->GetRenderTarget(0, &before)) && SUCCEEDED(device->GetViewport(&oldViewport)), "capture existing device target and viewport without sampling camera transforms");
        const auto actual = UpdateAdaptiveShadowMap(app.get(), frame.data());
        for (float value : actual) Require(std::isfinite(value), "actual production update returns a wholly finite light projection");
        Require(SUCCEEDED(device->GetRenderTarget(0, &after)) && SUCCEEDED(device->GetViewport(&newViewport)) && before.Get() == after.Get() &&
            !std::memcmp(&oldViewport, &newViewport, sizeof(oldViewport)), "projection-only update leaves actual render target and viewport intact");
        Require(!renderer->hdrTexture && !renderer->shadowSurface && !renderer->shadowDepthSurface && !renderer->renderTargetWidth && !renderer->renderTargetHeight,
            "zero test cap prevents production shadow target allocation and size changes");
        Require(app->SceneRevision() == revision && bool(app->SceneModified()) == dirty && app->RecordedFrameCountByte1() == recorded &&
            app->PhysicsResetPending() == physics && app->PlaybackActive() == playing && app->CurrentFrame() == time && !IsWindowVisible(window),
            "direct production projection update does not edit scene/dirty/history inputs, physics, playhead, playback or window visibility");
        return actual;
    }
};

using GetTransform = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);
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

const Matrix kFrame{0,-3,0,0, 2,1,0,0, 1,0,4,0, 2,-1,3,1};
Matrix CameraView() { auto view = Identity(); view[12] = 3; view[13] = -6; view[14] = 8; return view; }
float ViewRadius(const Matrix& projection, float distance) {
    const float depth = projection[15] == 1 ? 1.f : std::abs(distance);
    return std::hypot(depth / projection[0], depth / projection[5]);
}
void CameraCases(Host& host) {
    stage = "actual camera wiring"; const auto scene = SceneBounds(); const float focus[3]{}; unsigned discriminating = 0;
    for (bool safeReference : {true, false}) for (int mode : {1, 2})
        for (bool affine : {false, true}) for (bool orthographic : {false, true}) for (bool jitter : {false, true}) {
            host.Light(safeReference); host.app->ShadowMode() = mode;
            const auto frame = affine ? kFrame : Identity(), view = CameraView(), projection = Projection(orthographic, jitter);
            host.Install(view, projection); const auto actual = host.Call(frame);
            const float radius = (std::max)(1.f / NativeShadowLateralScale(host.app->ShadowDistance(), mode), ViewRadius(projection, host.app->CameraDistance()));
            const auto receivers = VisibleShadowReceiverBounds(scene, focus, radius, Multiply(frame, view), projection);
            Require(FiniteNativeShadowBounds(receivers), "real-device fixture has a nonempty receiver footprint for its installed matrices");
            Covers(actual, receivers, true); Covers(actual, scene, false);
            if (safeReference) {
                // A light parallel to the native look-at up vector takes the
                // established safe-reference branch. Reuse its public pure
                // helper; do not duplicate private ReferenceShadowProjection.
                const auto reference = NativeShadowProjection(focus, host.app->LightDirection(), host.app->ShadowDistance(), mode, &scene);
                const auto expected = FitFocusedShadowProjection(reference, scene, focus, host.app->LightDirection(), host.app->ShadowDistance(), mode,
                    ViewRadius(projection, host.app->CameraDistance()), &receivers);
                SameMatrix(actual, expected, "actual update consumes frame-times-VIEW and the installed projection, including AA offsets");
                if (affine && !orthographic) {
                    const auto wrong = VisibleShadowReceiverBounds(scene, focus, radius, Multiply(view, frame), projection);
                    const auto wrongFit = FitFocusedShadowProjection(reference, scene, focus, host.app->LightDirection(), host.app->ShadowDistance(), mode,
                        ViewRadius(projection, host.app->CameraDistance()), &wrong);
                    Require(!NearMatrix(expected, wrongFit), "noncommuting fixture distinguishes VIEW-times-frame from the required frame-times-VIEW"); ++discriminating;
                }
            }
        }
    Require(discriminating == 4, "both modes and AA states exercise a genuinely order-sensitive real production call");
}
void FallbackCases(Host& host, TransformHook& hook) {
    stage = "actual fallback wiring"; const auto scene = SceneBounds(); const float focus[3]{};
    for (int mode : {1, 2}) {
        host.Light(true); host.app->ShadowMode() = mode;
        const auto view = CameraView(), projection = Projection(false, true);
        auto unsupported = projection; unsupported[1] = .125f;
        host.Install(view, unsupported); const auto fallback = host.Call(kFrame); Covers(fallback, scene, true);
        const auto safe = NativeShadowProjection(focus, host.app->LightDirection(), host.app->ShadowDistance(), mode, &scene);
        SameMatrix(fallback, FitReferenceShadowProjection(safe, scene), "unsupported camera projection selects complete-scene fitting");
        host.Install(view, projection); const auto valid = host.Call(kFrame);
        Require(!NearMatrix(valid, fallback), "failed-API fixture would visibly distinguish accidentally trusted legal output from the required full-scene fallback");
        for (auto kind : {D3DTS_PROJECTION, D3DTS_VIEW}) {
            const auto count = failures; hook.Fail(kind, kind == D3DTS_VIEW ? view : projection);
            const auto failed = host.Call(kFrame);
            Require(!armed && failures == count + 1, "production read actually consumes exactly one current-device HRESULT failure");
            SameMatrix(failed, fallback, "VIEW/PROJECTION E_FAIL output is rejected even when the output matrix bytes look legal"); Covers(failed, scene, true);
        }
        auto singular = view; singular[0] = singular[1] = singular[2] = 0;
        host.Install(singular, projection); const auto singularResult = host.Call(kFrame);
        SameMatrix(singularResult, fallback, "unsupported singular scene-to-view transform uses the same complete-scene fallback"); Covers(singularResult, scene, true);
        host.Install(view, projection); const auto recovered = host.Call(kFrame); SameMatrix(recovered, valid, "ordinary device reads recover immediately after the one-shot controlled failures");
    }
}
}

int main(int argc, char**) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Require(argc == 1, "usage: native_shadow_receiver_host_tests (no input files or service arguments)");
    try {
        RequireDefaultDesktop(); Host host; TransformHook hook(host.device.Get());
        CameraCases(host); FallbackCases(host, hook);
        std::printf("PASS actual shadow receiver wiring: %u checks; no scene draw, shadow RT, file or service initialization\n", checks); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL receiver host [%s]: %s\n", stage, error.what()); return 1;
    }
}
