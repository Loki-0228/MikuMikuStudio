#pragma once
#include "runtime.hpp"
#include "iat_hook.hpp"
namespace mme_re::mmhack {
IDirect3D9* WINAPI hooked_direct3d_create9(UINT sdk_version);
HRESULT WINAPI hooked_load_mesh_w(LPCWSTR, DWORD, IDirect3DDevice9*, ID3DXBuffer**, ID3DXBuffer**, ID3DXBuffer**, DWORD*, ID3DXMesh**);
HRESULT WINAPI hooked_create_effect_resource_a(IDirect3DDevice9*, HMODULE, LPCSTR, const D3DXMACRO*, ID3DXInclude*, DWORD, ID3DXEffectPool*, ID3DXEffect**, ID3DXBuffer**);
HRESULT WINAPI hooked_texture_file_a(IDirect3DDevice9*, LPCSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO*, PALETTEENTRY*, IDirect3DTexture9**);
HRESULT WINAPI hooked_texture_file_w(IDirect3DDevice9*, LPCWSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO*, PALETTEENTRY*, IDirect3DTexture9**);
HRESULT WINAPI hooked_texture_memory(IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO*, PALETTEENTRY*, IDirect3DTexture9**);
HRESULT WINAPI hooked_create_texture(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**);
HRESULT WINAPI hooked_load_mesh_memory(LPCVOID, DWORD, DWORD, IDirect3DDevice9*, ID3DXBuffer**, ID3DXBuffer**, ID3DXBuffer**, DWORD*, ID3DXMesh**);
void record_texture_output(State&, const std::wstring& filename, IDirect3DTexture9** output);
bool install_host_hooks(std::string& failed_import);
void uninstall_host_hooks() noexcept;
}
