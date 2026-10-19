#pragma once
namespace mikumikustudio {
class MMDApp;
void EnableD3D12Native(bool enabled, bool validation = false);
bool RenderD3D12Native(MMDApp*,const float world[16]);
void ShutdownD3D12Native();
const char* D3D12NativeStatus();
bool WaitD3D12Native();
// Explicit test-only readback, never called by the production frame loop.
bool ValidateD3D12Native(const wchar_t* capturePath);
}
