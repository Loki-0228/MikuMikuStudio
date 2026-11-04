#include "device_probe_base.hpp"
#include "../scene_router.hpp"
#include "../d3dx_hooks.hpp"
#include "../com_wrappers.hpp"
#include <d3dx9.h>
#include <iostream>
#include <stdexcept>
using namespace mme_re::mmhack;
namespace {
void require(bool value) { if (!value) throw std::runtime_error("scene router regression"); }
struct Resource final : IUnknown {
  ULONG count = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++count; }
  ULONG STDMETHODCALLTYPE Release() override { return --count; }
};
struct Device final : test::DeviceProbeBase {
  ULONG references = 1;
  int begin_calls = 0, end_calls = 0, draw_calls = 0, reset_calls = 0;
  HRESULT reset_result = D3DERR_DEVICELOST;
  HRESULT STDMETHODCALLTYPE BeginScene() override { ++begin_calls; return S_OK; }
  HRESULT STDMETHODCALLTYPE EndScene() override { ++end_calls; return S_OK; }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX* output) override { D3DXMatrixIdentity(reinterpret_cast<D3DXMATRIX*>(output)); return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD, IDirect3DSurface9** target) override { *target = nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** output) override { *output = nullptr; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE, DWORD* output) override { *output = D3DBLEND_ONE; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD* output) override { *output = D3DTOP_DISABLE; return S_OK; }
  HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) override { ++draw_calls; return S_OK; }
  HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS*) override { ++reset_calls; return reset_result; }
  HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* output) override { *output = {}; output->hFocusWindow = reinterpret_cast<HWND>(0x1111); return S_OK; }
};
struct Counters { int initialize = 0, create = 0, erase = 0, begin = 0, end = 0, draw = 0, indexed = 0, lost = 0, reset = 0, cleanup = 0; } counters;
void* host_memory[8]{};
int host_count = 1;
float WINAPI frame_time() { return 3.5f; }
int WINAPI one() { return host_count; }
int WINAPI zero() { return 0; }
unsigned int WINAPI id(int) { return static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(host_memory)); }
const char* WINAPI filename(int) { return "missing-test.pmd"; }
void* __cdecl backing(int kind, int index, const wchar_t** path, IUnknown** resource) {
  require(kind == 1 && index == 0);
  *path = L"missing-wide-model.pmd";
  *resource = static_cast<IUnknown*>(host_memory[0]);
  return host_memory;
}
int WINAPI order(int) { return 7; }
int WINAPI material_count(int) { return 1; }
int WINAPI current() { return 7; }
int WINAPI technic() { return 2; }
int WINAPI initialize(IDirect3DDevice9*) { ++counters.initialize; return 0; }
int WINAPI initialize_failure(IDirect3DDevice9*) { return 1; }
void WINAPI begin(IDirect3DDevice9*) { ++counters.begin; }
void WINAPI end(IDirect3DDevice9*) { ++counters.end; }
HRESULT WINAPI draw(IDirect3DDevice9*, D3DPRIMITIVETYPE type, UINT first, UINT count) { require(type == D3DPT_TRIANGLELIST && first == 4 && count == 5); ++counters.draw; return E_FAIL; }
HRESULT WINAPI indexed(IDirect3DDevice9*, D3DPRIMITIVETYPE type, INT base, UINT minimum, UINT vertices, UINT first, UINT count) { require(type == D3DPT_TRIANGLELIST && base == -1 && minimum == 2 && vertices == 3 && first == 4 && count == 5); ++counters.indexed; return E_FAIL; }
void WINAPI create(IDirect3DDevice9*, ModelId key, const char* file, std::uint32_t kind, std::uint32_t materials, std::uintptr_t extra1, IUnknown* extra2) { require(key == id(0) && std::string(file) == filename(0) && kind == 1 && materials == 1 && !extra1 && !extra2); ++counters.create; }
void WINAPI erase(IDirect3DDevice9*, ModelId key) { require(key == id(0)); ++counters.erase; }
void WINAPI lost(IDirect3DDevice9*) { ++counters.lost; }
HRESULT WINAPI reset(IDirect3DDevice9*) { ++counters.reset; return S_OK; }
void WINAPI cleanup(IDirect3DDevice9*) { ++counters.cleanup; }
}
int main() {
  try {
    State state;
    Resource resource;
    host_memory[0] = &resource;
    state.index_buffers.insert(&resource);
    state.record_host_output(host_memory, L"missing-test.pmd");
    HostApi host;
    host.frame_time = frame_time;
    host.pmd_count = one; host.accessory_count = zero;
    host.pmd_id = id; host.accessory_id = id;
    host.pmd_filename = filename; host.accessory_filename = filename;
    host.pmd_material_count = material_count; host.accessory_material_count = material_count;
    host.pmd_order = order; host.accessory_order = order;
    host.current_object = current; host.current_material = zero; host.current_technic = technic;
    Callbacks callbacks;
    callbacks.initialize = initialize; callbacks.begin_scene = begin; callbacks.end_scene = end;
    callbacks.create_model = create; callbacks.delete_model = erase;
    callbacks.draw_primitive = draw; callbacks.draw_indexed_primitive = indexed;
    callbacks.lost_device = lost; callbacks.reset_device = reset; callbacks.cleanup = cleanup;
    Device device;
    SceneRouter router(&device, state, callbacks);
    require(router.begin_scene(host) == S_OK);
    require(counters.initialize == 1 && counters.create == 1 && counters.begin == 1 && device.begin_calls == 1 && state.frame_time == 3.5f && resource.count == 2 && state.active_models && state.active_models->size() == 1);
    require(router.draw_indexed_primitive(host, D3DPT_TRIANGLELIST, -1, 2, 3, 4, 5) == S_OK);
    require(counters.indexed == 1 && state.current_model == id(0) && state.subset == 0 && state.draw_type == 2 && state.blend_mode == 1);
    require(router.draw_primitive(D3DPT_TRIANGLELIST, 4, 5) == S_OK && counters.draw == 1 && state.current_model == 0 && state.subset == 0xffffffff);
    router.finish_scene(); router.finish_scene();
    require(counters.end == 1);
    require(router.draw_primitive(D3DPT_TRIANGLELIST, 4, 5) == S_OK && device.draw_calls == 1);
    require(router.reset(nullptr) == D3DERR_DEVICELOST && counters.lost == 1 && counters.reset == 1 && device.reset_calls == 1 && state.reset_window == reinterpret_cast<HWND>(0x1111));
    host_count = 0;
    require(router.begin_scene(host) == S_OK && counters.initialize == 1 && counters.erase == 1 && resource.count == 1 && state.active_models && state.active_models->empty());
    router.cleanup(); require(counters.cleanup == 1 && !router.initialized());
    IDirect3DTexture9* output = reinterpret_cast<IDirect3DTexture9*>(0x4444);
    record_texture_output(state, L"DATA\\toon02.bmp", &output);
    require(state.toon_textures.count(L"toon02.bmp") == 1 && state.toon_textures.count(L"DATA/toon02.bmp") == 0);
    record_texture_output(state, L"custom\\toon02.bmp", &output);
    require(state.toon_textures.count(L"custom/toon02.bmp") == 1);
    Runtime runtime(state);
    runtime.callbacks = callbacks;
    auto* wrapper = runtime.wrap(&device);
    require(wrapper && wrapper != &device && runtime.wrap(&device) == wrapper && runtime.unwrap(wrapper) == &device && device.references == 2);
    require(wrapper->Release() == 0 && runtime.device_wrappers.empty());
    host_count = 1;
    host.object_backing = backing;
    State native_state;
    SceneRouter native_router(&device, native_state, callbacks);
    require(native_router.begin_scene(host) == S_OK && native_state.active_models->size() == 1);
    require(native_state.object_backing(id(0)).base == reinterpret_cast<std::byte*>(host_memory));
    require(native_state.object_backing(id(0)).filename == L"missing-wide-model.pmd");
    native_router.cleanup();
    host_count = 0;
    State failed_state;
    Device failed_device;
    Callbacks failed_callbacks = callbacks;
    failed_callbacks.initialize = initialize_failure;
    SceneRouter failed_router(&failed_device, failed_state, failed_callbacks);
    require(failed_router.begin_scene(host) == S_OK && failed_device.begin_calls == 0 && failed_state.disabled && !failed_router.initialized());
    std::cout << "Model discovery/lifecycle, draw ABI, reset-on-failure, texture aliases and COM lifetime passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
