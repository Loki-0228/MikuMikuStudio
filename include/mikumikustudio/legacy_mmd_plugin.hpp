#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d9.h>
#include <string>

namespace mikumikustudio {
class MMDApp;

namespace legacy_mmd_plugin {
void Start(MMDApp* app) noexcept;
void Stop() noexcept;
bool HandleInput(HWND, UINT, WPARAM, LPARAM) noexcept;
// device_reset_v2 always receives the real result; effekseer_v2 recreates its
// resources only after a successful reset. No original EXE layout is accessed.
HRESULT ResetDevice(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) noexcept;
HRESULT BeginScene(IDirect3DDevice9*) noexcept;
HRESULT EndScene(IDirect3DDevice9*) noexcept;
HRESULT DrawIndexedPrimitive(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT base, UINT minimum,
    UINT vertices, UINT start, UINT count) noexcept;
// 0: ordinary model; 1: resolved Effekseer template; -1: explicit .efk error.
int ResolveEffekseerModel(const wchar_t* path, std::wstring& templatePath) noexcept;
const wchar_t* EffekseerFilename(const wchar_t* modelPath);
bool EffekseerContext() noexcept;
// The audited Effekseer profile submits D3D9 draws. Keep its viewport on that
// device while it is active; the native D3D12 path would skip its callbacks.
bool RequiresD3D9Rendering() noexcept;
}
}
