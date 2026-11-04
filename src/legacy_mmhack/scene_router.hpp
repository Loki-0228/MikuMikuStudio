#pragma once
#include "callbacks.hpp"
#include "host_api.hpp"
namespace mme_re::mmhack {
// Recovered operations shared by a future complete IDirect3DDevice9 wrapper.
class SceneRouter {
public:
  SceneRouter(IDirect3DDevice9* device, State& state, const Callbacks& callbacks) noexcept;
  HRESULT initialize();
  HRESULT begin_scene(const HostApi& host);
  void mark_scene_started(IDirect3DSurface9* captured_render_target) noexcept;
  void finish_scene();
  void finish_if_target(IDirect3DSurface9* target);
  HRESULT draw_primitive(D3DPRIMITIVETYPE, UINT start_vertex, UINT primitive_count);
  HRESULT draw_indexed_primitive(const HostApi&, D3DPRIMITIVETYPE, INT base_vertex, UINT minimum_vertex, UINT vertex_count, UINT start_index, UINT primitive_count);
  HRESULT clear(DWORD count, const D3DRECT* rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
  HRESULT reset(D3DPRESENT_PARAMETERS* presentation);
  void cleanup();
  bool initialized() const noexcept { return initialized_; }
  bool scene_ended() const noexcept { return ended_; }
private:
  IDirect3DDevice9* device_; // borrowed
  State& state_;
  const Callbacks& callbacks_;
  ModelMap models_;
  std::unordered_map<int, ModelId> draw_orders_;
  IDirect3DSurface9* render_target_ = nullptr; // borrowed
  bool initialized_ = false;
  bool ended_ = false;
};
}
