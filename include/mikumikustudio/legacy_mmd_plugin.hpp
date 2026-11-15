#pragma once
#include <d3d9.h>

namespace mikumikustudio {
class MMDApp;

namespace legacy_mmd_plugin {
void Start(MMDApp* app) noexcept;
void Stop() noexcept;
// Only device_reset_v2 packages receive this pair; the actual device call is
// bracketed even when Reset fails. No original executable layout is accessed.
HRESULT ResetDevice(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) noexcept;
}
}
