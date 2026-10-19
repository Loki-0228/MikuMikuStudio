#pragma once
#include <d3d12.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>

// Optional graphics APIs resolved from System32 on first use. Missing DLLs or
// exports return a failing HRESULT instead of preventing the EXE from starting.
namespace mikumikustudio::graphics_runtime {
HRESULT CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device);
HRESULT GetDebugInterface(REFIID iid, void** debug);
HRESULT SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
    D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** error);
HRESULT CreateFactory1(REFIID iid, void** factory);
HRESULT CreateFactory2(UINT flags, REFIID iid, void** factory);
HRESULT Compile(const void* source, SIZE_T size, const char* name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, const char* entry,
    const char* target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** error);
}
