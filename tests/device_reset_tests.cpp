// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/d3dx_effect.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/physics_scene.hpp"
#include "btBulletDynamicsCommon.h"
#include "BulletCollision/BroadphaseCollision/btAxisSweep3.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct EffectProbe { void** vtable; int lost = 0; int reset = 0; };
HRESULT WINAPI Lost(void* object) { ++static_cast<EffectProbe*>(object)->lost; return S_OK; }
HRESULT WINAPI Reset(void* object) { ++static_cast<EffectProbe*>(object)->reset; return S_OK; }
}
int main() {
    using namespace mikumikustudio;
    HWND window = CreateWindowW(L"STATIC", L"Device reset regression", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden device window");
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    auto app = std::make_unique<MMDApp>();
    std::memset(&app->state, 0, sizeof(app->state));
    auto renderer = std::make_unique<D3DRenderer>();
    std::memset(renderer.get(), 0, sizeof(*renderer));
    app->Renderer() = renderer.get();
    auto& pp = renderer->presentParameters;
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.hDeviceWindow = window;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &renderer->device)), "create device");
    auto* device = renderer->device;
    PhysicsScene scene{};
    Check(SceneConstruct(&scene, renderer.get()), "construct actual physics gizmo resources");
    HMODULE dll = LoadLibraryW(L"d3dx9_43.dll");
    Check(dll != nullptr, "load real D3DX runtime");
    using CreateEffect = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT,
        const void*, void*, DWORD, void*, void**, void**);
    auto create = reinterpret_cast<CreateEffect>(GetProcAddress(dll, "D3DXCreateEffect"));
    Check(create != nullptr, "resolve effect compiler");
    const char source[] = "technique T { pass P { ZEnable = false; } }";
    void* effect = nullptr;
    Check(SUCCEEDED(create(device, source, sizeof(source) - 1, nullptr, nullptr,
        0, nullptr, &effect, nullptr)), "create real effect with state blocks");
    renderer->effect = reinterpret_cast<ID3DXEffect*>(effect);
    Check(SUCCEEDED(d3dx::FxSetTechnique(effect, "T")), "select test technique");
    for (UINT size : {64u, 128u, 256u, 64u}) {
        // Exercise state blocks before loss, and fresh capture resources on
        // each cycle, as AVI export does when growing/restoring its target.
        Check(SUCCEEDED(device->BeginScene()), "begin effect before reset");
        UINT passes = 0;
        Check(SUCCEEDED(d3dx::FxBegin(effect, &passes)) && passes == 1, "begin effect");
        Check(SUCCEEDED(d3dx::FxBeginPass(effect, 0)), "begin pass");
        Check(SUCCEEDED(d3dx::FxEndPass(effect)), "end pass");
        Check(SUCCEEDED(d3dx::FxEnd(effect)), "end effect");
        Check(SUCCEEDED(device->EndScene()), "end scene");
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
            &renderer->backbufferSurface)), "retain backbuffer as host does");
        Check(SUCCEEDED(device->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE, 0, TRUE, &renderer->captureSurface, nullptr)), "capture target");
        pp.BackBufferWidth = pp.BackBufferHeight = size;
        Check(PostDeviceReset(app.get()), "reset succeeds with actual physics scene alive");
        Check(renderer->backbufferSurface == nullptr && renderer->captureSurface == nullptr,
              "reset releases old default-pool surfaces");
        Check(SUCCEEDED(device->TestCooperativeLevel()), "device operational after reset");
        IDirect3DSurface9* back = nullptr;
        Check(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)), "new backbuffer");
        D3DSURFACE_DESC desc{}; back->GetDesc(&desc); back->Release();
        Check(desc.Width == size && desc.Height == size, "export target resized successfully");
        Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF336699, 1, 0)),
              "render after reset");
        Check(SUCCEEDED(device->SetStreamSource(0, scene.gizmoCubeVB, 0, 16)) &&
              SUCCEEDED(device->SetIndices(scene.gizmoCubeIB)), "gizmo buffers survive reset");
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        Check(SUCCEEDED(device->BeginScene()), "begin gizmo draw after reset");
        Check(SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_LINELIST, 0, 0, 8, 0, 12)),
              "draw surviving gizmo geometry");
        device->EndScene();
    }
    reinterpret_cast<IUnknown*>(effect)->Release(); renderer->effect = nullptr;
    Check(PostDeviceReset(app.get()), "reset without effect succeeds");
    Check(SUCCEEDED(device->TestCooperativeLevel()), "reset without effect");
    // A separately retained default-pool buffer deliberately forces Reset
    // to fail. Effects must not be notified until the device is usable again.
    void* slots[71]{};
    slots[69] = reinterpret_cast<void*>(&Lost);
    slots[70] = reinterpret_cast<void*>(&Reset);
    EffectProbe probe{slots};
    renderer->effect = reinterpret_cast<ID3DXEffect*>(&probe);
    IDirect3DVertexBuffer9* blocker = nullptr;
    Check(SUCCEEDED(device->CreateVertexBuffer(64, 0, D3DFVF_XYZ,
        D3DPOOL_DEFAULT, &blocker, nullptr)), "create deliberate reset blocker");
    Check(!PostDeviceReset(app.get()), "failed reset is returned to export caller");
    Check(probe.lost == 1 && probe.reset == 0, "failed reset does not recreate effect resources");
    blocker->Release();
    Check(PostDeviceReset(app.get()), "recover after releasing blocker");
    Check(probe.reset == 1, "effect restoration follows successful reset only");
    renderer->effect = nullptr;
    for (auto* vb : {scene.gizmoSphereVB, scene.gizmoCubeVB, scene.gizmoSphere33VB,
                     scene.gizmoArrowVB, scene.gizmoBoxSelVB}) vb->Release();
    for (auto* ib : {scene.gizmoSphereIB, scene.gizmoCubeIB, scene.gizmoSphere33IB,
                     scene.gizmoIdentityIB, scene.gizmoBoxSelIB}) ib->Release();
    scene.world->removeRigidBody(scene.groundBody);
    delete scene.groundBody->getMotionState();
    delete scene.groundBody->getCollisionShape();
    delete scene.groundBody;
    delete scene.world; delete scene.solver; delete scene.broadphase;
    delete scene.dispatcher; delete scene.collisionConfig;
    device->Release(); renderer->device = nullptr;
    d3d->Release(); FreeLibrary(dll); DestroyWindow(window);
    std::puts("Real D3DX effect and repeated export-size device resets passed.");
}
