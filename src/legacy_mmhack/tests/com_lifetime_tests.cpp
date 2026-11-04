#include "com_probe_base.hpp"
#include "device_probe_base.hpp"
#include "../com_wrappers.hpp"
#include <iostream>
#include <stdexcept>

using namespace mme_re::mmhack;
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct References {
  ULONG count = 1;
  ULONG add() { require(count != 0, "AddRef on a retired native object"); return ++count; }
  ULONG release() { require(count != 0, "Native reference underflow"); return --count; }
};

struct Factory final : test::Direct3DProbeBase {
  References refs;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
    *output = static_cast<IDirect3D9*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return refs.add(); }
  ULONG STDMETHODCALLTYPE Release() override { return refs.release(); }
};

struct Device final : test::DeviceProbeBase {
  explicit Device(Factory& value) : factory(value) { factory.AddRef(); }
  Factory& factory;
  References refs;
  IDirect3DVertexBuffer9* vertices = nullptr;
  IDirect3DIndexBuffer9* indices = nullptr;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
    *output = static_cast<IDirect3DDevice9*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return refs.add(); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = refs.release();
    if (!count) factory.Release();
    return count;
  }
  HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
    require(refs.count != 0, "Call through a retired device"); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** output) override {
    *output = &factory; factory.AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamSource(UINT, IDirect3DVertexBuffer9** output,
                                             UINT* offset, UINT* stride) override {
    *output = vertices; vertices->AddRef(); *offset = 0; *stride = 32; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** output) override {
    *output = indices; indices->AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT, DWORD, DWORD, D3DPOOL,
                                               IDirect3DVertexBuffer9** output, HANDLE*) override {
    *output = vertices; vertices->AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT, DWORD, D3DFORMAT, D3DPOOL,
                                              IDirect3DIndexBuffer9** output, HANDLE*) override {
    *output = indices; indices->AddRef(); return S_OK;
  }
};

template<class Interface, class Description> struct Buffer final : Interface {
  explicit Buffer(Device& value) : device(value) {}
  Device& device;
  References refs;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
    *output = static_cast<Interface*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return refs.add(); }
  ULONG STDMETHODCALLTYPE Release() override { return refs.release(); }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** output) override {
    *output = &device; device.AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
  DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
  DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
  void STDMETHODCALLTYPE PreLoad() override {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }
  HRESULT STDMETHODCALLTYPE Lock(UINT, UINT, void**, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Unlock() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(Description*) override { return E_NOTIMPL; }
};

struct Mesh final : test::MeshProbeBase {
  Mesh(Device& value, IDirect3DVertexBuffer9* vb = nullptr,
       IDirect3DIndexBuffer9* ib = nullptr, bool active = true)
      : device(value), vertices(vb), indices(ib) {
    refs.count = 0;
    if (active) acquire();
  }
  Device& device;
  References refs;
  IDirect3DVertexBuffer9* vertices;
  IDirect3DIndexBuffer9* indices;
  Mesh* clone = nullptr;
  void acquire() {
    if (!refs.count) { refs.count = 1; device.AddRef(); }
    else refs.add();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
    *output = static_cast<ID3DXMesh*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return refs.add(); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = refs.release();
    if (!count) {
      if (vertices) vertices->Release();
      if (indices) indices->Release();
      device.Release();
    }
    return count;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** output) override {
    *output = &device; device.AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetVertexBuffer(IDirect3DVertexBuffer9** output) override {
    *output = vertices; vertices->AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetIndexBuffer(IDirect3DIndexBuffer9** output) override {
    *output = indices; indices->AddRef(); return S_OK;
  }
  HRESULT returnClone(ID3DXMesh** output) {
    require(clone != nullptr, "No synthetic clone was selected");
    clone->acquire(); *output = clone; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CloneMeshFVF(DWORD, DWORD, IDirect3DDevice9* target,
                                         ID3DXMesh** output) override {
    require(target == expectedCloneDevice, "CloneMeshFVF changed the caller's device");
    return returnClone(output);
  }
  HRESULT STDMETHODCALLTYPE CloneMesh(DWORD, const D3DVERTEXELEMENT9*,
                                      IDirect3DDevice9* target, ID3DXMesh** output) override {
    require(target == expectedCloneDevice, "CloneMesh changed the caller's device");
    return returnClone(output);
  }
  HRESULT STDMETHODCALLTYPE Optimize(DWORD, const DWORD*, DWORD*, DWORD*, ID3DXBuffer**,
                                     ID3DXMesh** output) override { return returnClone(output); }
  IDirect3DDevice9* expectedCloneDevice = nullptr;
};

unsigned cleanups = 0;
ULONG referencesAtCleanup = 0;
IDirect3DDevice9* lastCleanupDevice = nullptr;
void WINAPI cleanup(IDirect3DDevice9* device) {
  ++cleanups;
  lastCleanupDevice = device;
  referencesAtCleanup = static_cast<Device*>(device)->refs.count;
  require(referencesAtCleanup != 0, "Cleanup ran after native device destruction");
}

void returnInterfacesAndFinalRelease() {
  State state;
  Runtime runtime(state);
  runtime.callbacks.cleanup = cleanup;
  Factory nativeFactory;
  Device nativeDevice(nativeFactory);
  Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC> nativeVertices(nativeDevice);
  Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC> nativeIndices(nativeDevice);
  nativeDevice.vertices = &nativeVertices;
  nativeDevice.indices = &nativeIndices;
  auto* factory = runtime.wrap(&nativeFactory);
  auto* device = runtime.wrap(&nativeDevice);
  require(nativeDevice.refs.count == 2, "Device wrapper must retain exactly one native reference");
  Mesh nativeMesh(nativeDevice, &nativeVertices, &nativeIndices);
  auto* mesh = runtime.wrap(&nativeMesh);
  require(nativeMesh.refs.count == 1 && state.wrapped_meshes.count(mesh) == 1,
          "Wrapping a newly returned mesh must adopt its existing reference");

  // Original mesh slot 10 is 0x180001ec0, a direct forwarder. Wrapping this
  // already-AddRef'd result used to consume the host's local device reference
  // on Release, deleting a still-used host wrapper.
  IDirect3DDevice9* fromMesh = nullptr;
  require(mesh->GetDevice(&fromMesh) == S_OK && fromMesh == &nativeDevice && fromMesh != device,
          "Mesh::GetDevice must return the native device, not its cached wrapper");
  require(nativeDevice.refs.count == 4, "GetDevice must add exactly one native reference");
  require(fromMesh->Release() == 3 && cleanups == 0 && runtime.device_wrappers.size() == 1,
          "Releasing a mesh-returned device retired the host wrapper");
  require(device->TestCooperativeLevel() == S_OK, "Host wrapper was not usable after GetDevice/Release");

  IDirect3D9* fromDevice = nullptr;
  require(device->GetDirect3D(&fromDevice) == S_OK && fromDevice == factory,
          "GetDirect3D must reuse the factory wrapper");
  require(nativeFactory.refs.count == 3 && fromDevice->Release() == 2,
          "GetDirect3D wrapping added or lost a reference");
  const GUID probeIid{};
  void* queried = nullptr;
  require(factory->QueryInterface(probeIid, &queried) == S_OK && queried == &nativeFactory,
          "Factory QueryInterface must forward its native result");
  require(static_cast<IDirect3D9*>(queried)->Release() == 2, "Factory QueryInterface reference imbalance");
  require(mesh->QueryInterface(probeIid, &queried) == S_OK && queried == &nativeMesh,
          "Mesh QueryInterface must forward its native result");
  require(static_cast<ID3DXMesh*>(queried)->Release() == 1, "Mesh QueryInterface reference imbalance");

  IDirect3DVertexBuffer9* vb = nullptr;
  IDirect3DIndexBuffer9* ib = nullptr;
  require(mesh->GetVertexBuffer(&vb) == S_OK && vb == &nativeVertices && vb->Release() == 1,
          "Mesh vertex-buffer identity/reference imbalance");
  require(mesh->GetIndexBuffer(&ib) == S_OK && ib == &nativeIndices && ib->Release() == 1,
          "Mesh index-buffer identity/reference imbalance");
  UINT offset = 0, stride = 0;
  require(device->GetStreamSource(0, &vb, &offset, &stride) == S_OK && vb == &nativeVertices && vb->Release() == 1,
          "Device stream-source identity/reference imbalance");
  require(device->GetIndices(&ib) == S_OK && ib == &nativeIndices && ib->Release() == 1,
          "Device index-buffer identity/reference imbalance");
  require(device->CreateVertexBuffer(16, 0, 0, D3DPOOL_MANAGED, &vb, nullptr) == S_OK &&
          vb == &nativeVertices && vb->Release() == 1, "Created vertex buffer was wrapped or retained");
  require(device->CreateIndexBuffer(6, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr) == S_OK &&
          ib == &nativeIndices && ib->Release() == 1 && state.index_buffers.count(ib) == 1,
          "Created index buffer must remain native while being recorded");

  Mesh cloneA(nativeDevice, nullptr, nullptr, false);
  Mesh cloneB(nativeDevice, nullptr, nullptr, false);
  Mesh cloneC(nativeDevice, nullptr, nullptr, false);
  nativeMesh.clone = &cloneA;
  nativeMesh.expectedCloneDevice = device;
  ID3DXMesh* first = nullptr;
  ID3DXMesh* second = nullptr;
  require(mesh->CloneMeshFVF(0, D3DFVF_XYZ, device, &first) == S_OK && first != &cloneA,
          "CloneMeshFVF result must be wrapped");
  require(mesh->CloneMeshFVF(0, D3DFVF_XYZ, device, &second) == S_OK && first == second && cloneA.refs.count == 2,
          "Repeated clone return must reuse wrapper without changing native references");
  require(first->Release() == 1 && runtime.mesh_wrappers.size() == 2,
          "Non-final mesh Release removed a live wrapper");
  require(second->Release() == 0 && runtime.mesh_wrappers.size() == 1 && state.wrapped_meshes.size() == 1,
          "Final clone Release did not remove both mesh registrations");
  nativeMesh.clone = &cloneB;
  const D3DVERTEXELEMENT9 declaration[] = {D3DDECL_END()};
  require(mesh->CloneMesh(0, declaration, device, &first) == S_OK && first != &cloneB && first->Release() == 0,
          "CloneMesh result ownership imbalance");
  nativeMesh.clone = &cloneC;
  require(mesh->Optimize(0, nullptr, nullptr, nullptr, nullptr, &first) == S_OK && first != &cloneC && first->Release() == 0,
          "Optimize result ownership imbalance");
  require(runtime.mesh_wrappers.size() == 1 && state.wrapped_meshes.size() == 1,
          "Clone/Optimize left stale registrations");
  require(mesh->Release() == 0 && runtime.mesh_wrappers.empty() && state.wrapped_meshes.empty(),
          "Final source mesh Release left stale registrations");
  require(nativeVertices.refs.count == 0 && nativeIndices.refs.count == 0 && nativeDevice.refs.count == 2,
          "Source mesh release retained native resources");

  // Reuse the exact native address, as a native allocator can after destruction.
  // A stale cache entry would bypass construction/registration for this lifetime.
  nativeMesh.vertices = nullptr;
  nativeMesh.indices = nullptr;
  nativeMesh.acquire();
  mesh = runtime.wrap(&nativeMesh);
  require(state.wrapped_meshes.size() == 1 && mesh->Release() == 0 && runtime.mesh_wrappers.empty(),
          "A subsequent native mesh lifetime was not independently registered and retired");

  require(device->AddRef() == 3 && device->Release() == 2 && cleanups == 0,
          "A balanced wrapper AddRef/Release retired the host device");
  require(device->Release() == 0 && runtime.device_wrappers.empty() && nativeDevice.refs.count == 0,
          "Final device release leaked its retained native reference or identity entry");
  require(cleanups == 1 && lastCleanupDevice == &nativeDevice && referencesAtCleanup == 1,
          "Device Cleanup did not run exactly once before releasing the retained native reference");
  require(factory->Release() == 0 && runtime.direct3d_wrappers.empty(),
          "Final factory release left a cached wrapper");
}

void rawDeviceReferenceSurvivesWrapper() {
  State state;
  Runtime runtime(state);
  runtime.callbacks.cleanup = cleanup;
  Factory nativeFactory;
  Device nativeDevice(nativeFactory);
  auto* factory = runtime.wrap(&nativeFactory);
  auto* wrapper = runtime.wrap(&nativeDevice);
  const GUID probeIid{};
  void* queried = nullptr;
  require(wrapper->QueryInterface(probeIid, &queried) == S_OK && queried == &nativeDevice,
          "Device QueryInterface must return the native interface");
  require(nativeDevice.refs.count == 3, "Device QueryInterface lost its native reference");
  require(wrapper->Release() == 1 && runtime.device_wrappers.empty() && nativeDevice.refs.count == 1,
          "Last local device reference did not retire the wrapper while preserving the raw reference");
  require(cleanups == 2 && referencesAtCleanup == 2,
          "Raw-reference device cleanup occurred in the wrong lifetime phase");
  auto* raw = static_cast<IDirect3DDevice9*>(queried);
  require(raw->TestCooperativeLevel() == S_OK && raw->Release() == 0,
          "Raw QueryInterface result did not survive wrapper retirement");
  require(factory->Release() == 0 && runtime.direct3d_wrappers.empty(),
          "Raw-device case retained the factory");
}
} // namespace

int main() {
  try {
    returnInterfacesAndFinalRelease();
    rawDeviceReferenceSurvivesWrapper();
    std::cout << "COM lifetime passed: native GetDevice/QI/VB/IB, cached GetDirect3D/Clone/Optimize, cleanup and final cache removal\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
