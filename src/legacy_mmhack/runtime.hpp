#pragma once
#include "state.hpp"
#include "callbacks.hpp"
#include "host_api.hpp"
#include <d3dx9.h>
namespace mme_re::mmhack {
class Direct3DWrapper;
class DeviceWrapper;
class MeshWrapper;
struct Runtime {
  explicit Runtime(State& value) : state(value) {}
  State& state;
  HostApi host;
  Callbacks callbacks;
  std::unordered_map<IDirect3D9*, Direct3DWrapper*> direct3d_wrappers;
  std::unordered_map<IDirect3DDevice9*, DeviceWrapper*> device_wrappers;
  std::unordered_map<ID3DXMesh*, MeshWrapper*> mesh_wrappers;
  IDirect3D9* wrap(IDirect3D9*);
  IDirect3DDevice9* wrap(IDirect3DDevice9*);
  ID3DXMesh* wrap(ID3DXMesh*);
  IDirect3DDevice9* unwrap(IDirect3DDevice9*) const noexcept;
};
Runtime& runtime() noexcept;
}
