// MAINTENANCE: regression-keep; purpose and cleanup conditions in tests/README.md.
// User-acceptance regressions for the PMM scene lifetime and the accessory /
// model resources it rebuilds.
//
// Field report (defect 1): open a large PMM, File -> New, then reopen the same
// PMM and the process dies.
//
// Field report (defect 2): after switching the UI language to Chinese, loading
// a PMM can kill the process.
//
// Pin resource initialization, x64 offsets and the actual D3DX mesh ABI here.
// A crash inside d3d9.dll can also be delayed damage from the PMX bone-morph
// work buffer; morph_runtime_tests and scene_lifecycle_tests cover that cause.
#include "mikumikustudio/d3d_object_guard.hpp"
#include "mikumikustudio/d3dx_mesh.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"

#include <Windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace mikumikustudio {
// src/render/accessory.cpp - the accessory loader's FVF normalisation.
bool NormalizeAccessoryMeshFvf(void*& mesh, IDirect3DDevice9* device);
}  // namespace mikumikustudio

namespace {

void Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

class TrackedCom final : public IUnknown {
    ULONG references = 1;
    bool& destroyed;
public:
    explicit TrackedCom(bool& flag) : destroyed(flag) {}
    ~TrackedCom() { destroyed = true; }
    ULONG RefCount() const { return references; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references;
        if (!remaining) delete this;
        return remaining;
    }
};

void CheckValidationLifetime() {
    using namespace mikumikustudio;
    bool destroyed = false;
    auto* object = new TrackedCom(destroyed);
    Check(PlausibleD3dObject(object) && object->RefCount()==1,
          "validation outside rendering does not retain objects");
    {
        D3dObjectValidationScope outer;
        Check(!PlausibleD3dObject(nullptr), "scoped validation rejects null");
        Check(PlausibleD3dObject(object) && object->RefCount()==2,
              "first validation pins object");
        for (int i=0; i<100; ++i) Check(PlausibleD3dObject(object), "repeat validation");
        Check(object->RefCount()==2, "repeat validation retains only one reference");
        object->Release();
        Check(!destroyed, "owner release cannot invalidate a cached object");
        {
            D3dObjectValidationScope inner;
            Check(PlausibleD3dObject(object) && object->RefCount()==2,
                  "nested rendering has independent ownership");
        }
        Check(object->RefCount()==1 && PlausibleD3dObject(object),
              "nested scope restores outer validation");
    }
    Check(destroyed, "render scope releases its last reference");

    bool released[300]{};
    TrackedCom* objects[300]{};
    {
        D3dObjectValidationScope scope;
        for (int i=0; i<300; ++i) {
            objects[i]=new TrackedCom(released[i]);
            Check(PlausibleD3dObject(objects[i]), "overflow still validates objects");
        }
        for (int i=0; i<300; ++i) {
            Check(objects[i]->RefCount()==(i<256 ? 2u : 1u),
                  "bounded table falls back without retaining overflow");
            objects[i]->Release();
        }
    }
    for (bool flag:released) Check(flag, "all references released after cache overflow");
    Check(activeD3dValidation==nullptr, "validation scope never escapes rendering");
    std::puts("PASS scoped resource validation lifetime and overflow");
}

// ID3DXBaseMesh slots, verified against d3dx9mesh.h and real buffer descriptors.
constexpr std::size_t kMeshDrawSubset = 3;
constexpr std::size_t kMeshGetNumFaces = 4;
constexpr std::size_t kMeshGetNumVertices = 5;
constexpr std::size_t kMeshGetFvf = 6;
constexpr std::size_t kMeshGetOptions = 9;

DWORD MeshDword(void* mesh, std::size_t slot) {
    using Getter = DWORD(__stdcall*)(void*);
    return reinterpret_cast<Getter>(
        (*reinterpret_cast<void***>(mesh))[slot])(mesh);
}

HRESULT MeshDraw(void* mesh, DWORD subset) {
    using Draw = HRESULT(__stdcall*)(void*, DWORD);
    return reinterpret_cast<Draw>(
        (*reinterpret_cast<void***>(mesh))[kMeshDrawSubset])(mesh, subset);
}

void FillMesh(void* mesh, DWORD fvf) {
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    Check(SUCCEEDED(mikumikustudio::d3dx::MeshVertexBuffer(mesh, &vb)) && vb,
          "GetVertexBuffer returns a real COM resource");
    Check(SUCCEEDED(mikumikustudio::d3dx::MeshIndexBuffer(mesh, &ib)) && ib,
          "GetIndexBuffer returns a real COM resource");
    D3DVERTEXBUFFER_DESC vd{}; D3DINDEXBUFFER_DESC id{};
    Check(SUCCEEDED(vb->GetDesc(&vd)) && vd.FVF == fvf, "VB descriptor matches mesh layout");
    Check(SUCCEEDED(ib->GetDesc(&id)) && id.Format == D3DFMT_INDEX16, "IB descriptor has 16-bit indices");
    void* data = nullptr;
    Check(SUCCEEDED(vb->Lock(0, 0, &data, 0)), "lock vertices");
    std::memset(data, 0, vd.Size);
    const unsigned stride = vd.Size / MeshDword(mesh, kMeshGetNumVertices);
    for (unsigned i = 0; i < 6; ++i) {
        auto* vertex = reinterpret_cast<float*>(static_cast<unsigned char*>(data) + i * stride);
        vertex[0] = (i % 3 == 1) ? .5f : -.5f;
        vertex[1] = (i % 3 == 2) ? .5f : -.5f;
        vertex[2] = .5f;
    }
    vb->Unlock();
    Check(SUCCEEDED(ib->Lock(0, 0, &data, 0)), "lock indices");
    for (unsigned short i = 0; i < 6; ++i) static_cast<unsigned short*>(data)[i] = i;
    ib->Unlock();
    using LockAttributes = HRESULT(WINAPI*)(void*, DWORD, DWORD**);
    DWORD* attributes = nullptr;
    Check(SUCCEEDED((mikumikustudio::d3dx::MeshMethod<LockAttributes, 24>(mesh)(mesh, 0, &attributes))),
          "lock attributes");
    attributes[0] = attributes[1] = 0;
    mikumikustudio::d3dx::MeshMethod<HRESULT(WINAPI*)(void*), 25>(mesh)(mesh);
    vb->Release(); ib->Release();
}

}  // namespace

int main() {
    using namespace mikumikustudio;
    // Unbuffered: a crash inside Direct3D must not swallow the progress lines.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    CheckValidationLifetime();

    // --- the resource guard itself ----------------------------------------
    // Reject obvious sentinels before device calls. This check alone cannot
    // establish that an arbitrary non-null pointer is a live COM object.
    Check(!PlausibleD3dObject(nullptr), "null is not a D3D object");
    Check(!PlausibleD3dObject(reinterpret_cast<const void*>(
              static_cast<std::uintptr_t>(0xFFFFFFFFFFFFFFFFull))),
          "-1 is not a D3D object (the crash sentinel)");
    Check(!PlausibleD3dObject(reinterpret_cast<const void*>(
              static_cast<std::uintptr_t>(44))),
          "a low sentinel is not a D3D object");
    Check(!PlausibleD3dObject(reinterpret_cast<const void*>(
              static_cast<std::uintptr_t>(0x7FFFFFFFFFFFFFFFull))),
          "a non-user-mode address is not a D3D object");
    std::puts("PASS resource guard rejects sentinels");

    // --- ID3DXMesh layout the accessory loader depends on ------------------
    HWND window = CreateWindowW(L"STATIC", L"pmm reload regression", WS_POPUP,
                                0, 0, 64, 64, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create hidden device window");
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(d3d != nullptr, "create D3D9");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "create device");

    HMODULE d3dx = LoadLibraryW(L"d3dx9_43.dll");
    Check(d3dx != nullptr, "load the real D3DX runtime");
    std::puts("step: d3dx loaded");
    using CreateMeshFvf = HRESULT(WINAPI*)(DWORD, DWORD, DWORD, DWORD,
                                           IDirect3DDevice9*, void**);
    auto createMesh = reinterpret_cast<CreateMeshFvf>(
        GetProcAddress(d3dx, "D3DXCreateMeshFVF"));
    Check(createMesh != nullptr, "resolve D3DXCreateMeshFVF");

    // A mesh whose FVF is *not* the accessory pass's 274 (XYZ|NORMAL|TEX1):
    // the loader has to clone it, and the clone must go through the options /
    // CloneMeshFVF pair rather than the buffer locks.
    const DWORD kSourceFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    // D3DXMESH_MANAGED - spelled out because the DirectX SDK headers are not
    // part of the build (the loader passes the same value at accessory load).
    const DWORD kOptions = 0x220;
    void* mesh = nullptr;
    Check(SUCCEEDED(createMesh(2, 6, kOptions, kSourceFvf, device, &mesh)),
          "create a non-274 mesh");
    std::puts("step: mesh created");
    Check(MeshDword(mesh, kMeshGetNumFaces) == 2 &&
              MeshDword(mesh, kMeshGetNumVertices) == 6,
          "the getters confirm the slot order (faces 4, vertices 5)");
    Check(MeshDword(mesh, kMeshGetFvf) == kSourceFvf,
          "slot 6 is GetFVF");
    Check(MeshDword(mesh, kMeshGetOptions) == kOptions,
          "slot 9 is GetOptions - the loader's clone pair");
    Check(PlausibleD3dObject(mesh), "a live mesh passes the guard");
    std::puts("PASS ID3DXMesh vtable slots 6/9 on a real mesh");

    FillMesh(mesh, kSourceFvf);
    void* before = mesh;
    Check(NormalizeAccessoryMeshFvf(mesh, device),
          "a non-274 mesh is normalised");
    Check(mesh != nullptr && PlausibleD3dObject(mesh),
          "the normalised mesh is still a live object");
    const DWORD normalisedFvf = MeshDword(mesh, kMeshGetFvf);
    Check(normalisedFvf == 274,
          "the normalised mesh reports one consistent FVF");
    Check(mesh != before,
          "changing the vertex layout produces a new mesh");
    // Require successful drawing of initialized geometry after releasing the
    // original mesh. Merely surviving a failed DrawSubset would miss the bug.
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    Check(SUCCEEDED(device->BeginScene()), "begin real draw");
    const HRESULT draw = MeshDraw(mesh, 0);
    device->EndScene();
    Check(SUCCEEDED(draw),
          "the cloned mesh is walkable by DrawSubset");
    Check(MeshDword(mesh, kMeshGetNumFaces) == 2 &&
              MeshDword(mesh, kMeshGetNumVertices) == 6,
          "the clone keeps the geometry counts");
    Check(PlausibleD3dObject(mesh), "the clone is a live object");
    std::puts("PASS accessory mesh FVF normalisation");

    // A mesh already at 274 is left alone (same object, no clone churn).
    void* already = nullptr;
    Check(SUCCEEDED(createMesh(1, 3, kOptions, 274, device, &already)),
          "create a 274 mesh");
    void* untouched = already;
    Check(NormalizeAccessoryMeshFvf(already, device),
          "a 274 mesh reports success");
    Check(already == untouched, "a 274 mesh is not cloned");

    // Repeatedly load/convert/draw/dispose the field scene's FVF-338 layout.
    // Old tests accidentally called GetDeclaration into an 8-byte pointer and
    // accepted failed draws; require real initialized geometry and success.
    for (int cycle = 0; cycle < 20; ++cycle) {
        void* source = nullptr;
        Check(SUCCEEDED(createMesh(2, 6, kOptions, 338, device, &source)), "create 338 mesh");
        FillMesh(source, 338);
        Check(NormalizeAccessoryMeshFvf(source, device), "convert 338 mesh");
        Check(MeshDword(source, kMeshGetFvf) == 274, "conversion actually changed FVF");
        IDirect3DDevice9* owner = nullptr;
        Check(SUCCEEDED(d3dx::MeshDevice(source, &owner)) && owner == device, "mesh retains current device");
        owner->Release();
        for (int frame = 0; frame < 10; ++frame) {
            Check(SUCCEEDED(device->BeginScene()), "begin scene");
            Check(SUCCEEDED(MeshDraw(source, 0)), "real DrawSubset succeeds after original mesh release");
            device->EndScene();
        }
        reinterpret_cast<IUnknown*>(source)->Release();
    }

    // --- the x86 displayFrames offset the frame seek used to read ----------
    // The morph-track walk read *(model + 9948) as a pointer.  9948 is the x86
    // placement of displayFrames; on x64 that byte sits inside path[], so the
    // walk dereferenced the model path's characters.
    Check(offsetof(mdl::ModelRecord, path) == 0x2548, "x64 path offset");
    Check(offsetof(mdl::ModelRecord, displayFrames) == 0x2788,
          "x64 displayFrames offset");
    Check(9948 >= offsetof(mdl::ModelRecord, path) &&
              9948 + sizeof(void*) <=
                  offsetof(mdl::ModelRecord, path) + sizeof(wchar_t) * 256,
          "the x86 displayFrames offset lands inside path[] on x64");
    {
        auto* buffer = reinterpret_cast<unsigned char*>(
            ::operator new(sizeof(mdl::ModelRecord)));
        std::memset(buffer, 0, sizeof(mdl::ModelRecord));
        wcscpy_s(mdl::Mdl(buffer)->path, L"D:\\stage\\White.pmx");
        auto* table = reinterpret_cast<mdl::FrameGroup*>(
            ::operator new(46 * 4));
        std::memset(table, 0, 46 * 4);
        mdl::Mdl(buffer)->displayFrames = table;
        Check(mdl::DisplayFrames(buffer) == table,
              "the named accessor returns displayFrames");
        const void* viaRawOffset =
            *reinterpret_cast<const void* const*>(buffer + 9948);
        Check(!PlausibleD3dObject(viaRawOffset) ||
                  viaRawOffset == static_cast<const void*>(table),
              "the raw x86 offset never yields a usable unrelated pointer");
        Check(viaRawOffset != static_cast<const void*>(table),
              "the raw x86 offset does not reach displayFrames on x64");
        ::operator delete(table);
        ::operator delete(buffer);
    }
    std::puts("PASS displayFrames x64 placement");

    reinterpret_cast<IUnknown*>(mesh)->Release();
    reinterpret_cast<IUnknown*>(already)->Release();
    device->Release();
    d3d->Release();
    FreeLibrary(d3dx);
    DestroyWindow(window);
    std::puts("PMM reload regressions passed");
    return 0;
}
