#include "com_wrappers.hpp"
namespace mme_re::mmhack {
namespace {
bool wrapping_disabled() {
  char value[8] = {};
  static const bool disabled =
      GetEnvironmentVariableA("MMHACK_NO_WRAP", value, sizeof(value)) > 0;
  return disabled;
}
}
Runtime& runtime() noexcept { static Runtime instance(state()); return instance; }
IDirect3D9* Runtime::wrap(IDirect3D9* value) {
  if (!value || wrapping_disabled()) return value;
  auto found = direct3d_wrappers.find(value);
  return found == direct3d_wrappers.end() ? new Direct3DWrapper(value, *this) : found->second;
}
IDirect3DDevice9* Runtime::wrap(IDirect3DDevice9* value) {
  if (!value || wrapping_disabled()) return value;
  auto found = device_wrappers.find(value);
  return found == device_wrappers.end() ? new DeviceWrapper(value, *this) : found->second;
}
ID3DXMesh* Runtime::wrap(ID3DXMesh* value) {
  if (!value || wrapping_disabled()) return value;
  auto found = mesh_wrappers.find(value);
  return found == mesh_wrappers.end() ? new MeshWrapper(value, *this) : found->second;
}
IDirect3DDevice9* Runtime::unwrap(IDirect3DDevice9* value) const noexcept {
  // Identity lookup avoids dereferencing an arbitrary untrusted object's +8.
  for (const auto& entry : device_wrappers) if (entry.second == value) return entry.first;
  return value;
}
Direct3DWrapper::Direct3DWrapper(IDirect3D9* value, Runtime& owner) : original_(value), runtime_(owner) { runtime_.direct3d_wrappers.emplace(value, this); }
Direct3DWrapper::~Direct3DWrapper() { runtime_.direct3d_wrappers.erase(original_); }
ULONG STDMETHODCALLTYPE Direct3DWrapper::Release() {
  // 0x180003e40: the final native reference removes the identity-cache entry.
  const ULONG remaining = original_->Release();
  if (remaining == 0) delete this;
  return remaining;
}
HRESULT STDMETHODCALLTYPE Direct3DWrapper::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS* presentation, IDirect3DDevice9** output) {
  if (runtime_.state.debug) { type = D3DDEVTYPE_REF; behavior = (behavior & ~D3DCREATE_HARDWARE_VERTEXPROCESSING) | D3DCREATE_SOFTWARE_VERTEXPROCESSING; }
  if (!runtime_.state.main_window) {
    HWND current = focus;
    while (current) { runtime_.state.main_window = current; current = reinterpret_cast<HWND>(GetWindowLongPtrW(current, GWLP_HWNDPARENT)); }
  }
  const auto result = original_->CreateDevice(adapter, type, focus, behavior & ~D3DCREATE_PUREDEVICE, presentation, output);
  runtime_.state.reset_window = presentation && presentation->hDeviceWindow ? presentation->hDeviceWindow : focus;
  if (result == S_OK && output && *output) *output = runtime_.wrap(*output);
  return result;
}
DeviceWrapper::DeviceWrapper(IDirect3DDevice9* value, Runtime& owner) : original_(value), runtime_(owner), router_(value, owner.state, owner.callbacks) {
  runtime_.device_wrappers.emplace(value, this);
  original_->AddRef(); // Extra retained reference observed in 0x1bd0.
}
DeviceWrapper::~DeviceWrapper() { runtime_.device_wrappers.erase(original_); }
ULONG STDMETHODCALLTYPE DeviceWrapper::AddRef() { ++local_references_; return original_->AddRef(); }
ULONG STDMETHODCALLTYPE DeviceWrapper::Release() {
  // 0x180001d50: the wrapper owns one extra native reference (0x180001bd0).
  // Raw interfaces returned by QueryInterface or a mesh holding the native
  // device bypass this wrapper. A mesh holding this wrapper balances its own
  // AddRef/Release here; Mesh::GetDevice forwards whichever device it holds.
  ULONG remaining = original_->Release();
  --local_references_;
  if (remaining == 1 || local_references_ == 0) {
    router_.cleanup();                 // 0x180001db0, before the retained Release
    remaining = original_->Release();
    delete this;                       // 0x180001c90, including identity-cache erase
  }
  return remaining;
}
HRESULT STDMETHODCALLTYPE DeviceWrapper::GetDirect3D(IDirect3D9** output) {
  const auto result = original_->GetDirect3D(output);
  if (result == S_OK && output && *output) *output = runtime_.wrap(*output);
  return result;
}
HRESULT STDMETHODCALLTYPE DeviceWrapper::Reset(D3DPRESENT_PARAMETERS* presentation) { return router_.reset(presentation); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::Present(const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty) {
  runtime_.state.drawn_window = window ? window : runtime_.state.reset_window;
  return original_->Present(source, destination, window, dirty);
}
HRESULT STDMETHODCALLTYPE DeviceWrapper::BeginScene() { return router_.begin_scene(runtime_.host); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::EndScene() { router_.finish_scene(); return original_->EndScene(); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::Clear(DWORD count, const D3DRECT* rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil) { return router_.clear(count, rectangles, flags, color, z, stencil); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) { return router_.draw_primitive(type, start, count); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT start, UINT count) { return router_.draw_indexed_primitive(runtime_.host, type, base, minimum, vertices, start, count); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** output, HANDLE* shared) {
  const auto result = original_->CreateIndexBuffer(length, usage, format, pool, output, shared);
  if (result == S_OK && output && *output) {
    runtime_.state.record_host_output(output, runtime_.state.last_read_file);
    runtime_.state.index_buffers.insert(*output);
  }
  return result;
}
HRESULT STDMETHODCALLTYPE DeviceWrapper::GetRenderTargetData(IDirect3DSurface9* source, IDirect3DSurface9* destination) { router_.finish_if_target(source); return original_->GetRenderTargetData(source, destination); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::StretchRect(IDirect3DSurface9* source, const RECT* source_rect, IDirect3DSurface9* destination, const RECT* destination_rect, D3DTEXTUREFILTERTYPE filter) { router_.finish_if_target(source); return original_->StretchRect(source, source_rect, destination, destination_rect, filter); }
HRESULT STDMETHODCALLTYPE DeviceWrapper::UpdateSurface(IDirect3DSurface9* source, const RECT* source_rect, IDirect3DSurface9* destination, const POINT* point) { router_.finish_if_target(source); return original_->UpdateSurface(source, source_rect, destination, point); }
MeshWrapper::MeshWrapper(ID3DXMesh* value, Runtime& owner) : original_(value), runtime_(owner) { runtime_.mesh_wrappers.emplace(value, this); runtime_.state.wrapped_meshes.insert(this); }
MeshWrapper::~MeshWrapper() { runtime_.state.wrapped_meshes.erase(this); runtime_.mesh_wrappers.erase(original_); }
ULONG STDMETHODCALLTYPE MeshWrapper::Release() {
  // 0x180001990 / 0x180006860: final native Release removes the mesh wrapper.
  const ULONG remaining = original_->Release();
  if (remaining == 0) delete this;
  return remaining;
}
HRESULT STDMETHODCALLTYPE MeshWrapper::CloneMeshFVF(DWORD options, DWORD fvf, IDirect3DDevice9* device, ID3DXMesh** output) {
  // 0x180001a30/0x180001a60 preserve the caller's R9 device argument.
  const auto result = original_->CloneMeshFVF(options, fvf, device, output);
  if (result == S_OK && output && *output) *output = runtime_.wrap(*output);
  return result;
}
HRESULT STDMETHODCALLTYPE MeshWrapper::CloneMesh(DWORD options, const D3DVERTEXELEMENT9* declaration, IDirect3DDevice9* device, ID3DXMesh** output) {
  const auto result = original_->CloneMesh(options, declaration, device, output);
  if (result == S_OK && output && *output) *output = runtime_.wrap(*output);
  return result;
}
HRESULT STDMETHODCALLTYPE MeshWrapper::Optimize(DWORD flags, const DWORD* adjacency_in, DWORD* adjacency_out, DWORD* face_remap, ID3DXBuffer** vertex_remap, ID3DXMesh** output) {
  const auto result = original_->Optimize(flags, adjacency_in, adjacency_out, face_remap, vertex_remap, output);
  if (result == S_OK && output && *output) *output = runtime_.wrap(*output);
  return result;
}
}
