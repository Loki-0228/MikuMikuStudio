#pragma once
#include "state.hpp"
#include <string>
namespace mme_re::mmhack {
struct Callbacks {
  using Scene = void (WINAPI*)(IDirect3DDevice9*);
  Scene begin_scene = nullptr;
  Scene end_scene = nullptr;
  HRESULT (WINAPI* draw_primitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
  HRESULT (WINAPI* draw_indexed_primitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) = nullptr;
  int (WINAPI* initialize)(IDirect3DDevice9*) = nullptr;
  Scene cleanup = nullptr;
  // Both call sites at 0x35b1 and 0x376c set the last two inputs to zero.
  void (WINAPI* create_model)(IDirect3DDevice9*, ModelId, const char*, std::uint32_t, std::uint32_t, std::uintptr_t, IUnknown*) = nullptr;
  void (WINAPI* delete_model)(IDirect3DDevice9*, ModelId) = nullptr;
  Scene lost_device = nullptr;
  HRESULT (WINAPI* reset_device)(IDirect3DDevice9*) = nullptr;
  HRESULT (WINAPI* clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD, BOOL) = nullptr;
  bool bind(HMODULE effect_module, std::string& missing);
};
// Explicitly loads MMEffect from the specified directory. Never loads MMHack
// or a reference DLL automatically. Caller owns FreeLibrary on success.
HMODULE load_effect_callbacks(const std::wstring& directory, Callbacks&, bool& debug, std::string& error);
}
