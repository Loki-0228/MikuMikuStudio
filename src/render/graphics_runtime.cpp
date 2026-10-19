#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/graphics_runtime.hpp"
#include <string>

namespace mikumikustudio::graphics_runtime {
namespace {
struct Module {
    HMODULE handle = nullptr;
    HRESULT error = E_FAIL;

    explicit Module(const wchar_t* name) {
        wchar_t directory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (!length) { error = HRESULT_FROM_WIN32(GetLastError()); return; }
        if (length >= MAX_PATH) { error = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); return; }
        // An absolute path also works on Windows 7 without the update that
        // introduced LOAD_LIBRARY_SEARCH_SYSTEM32. Never load a local proxy.
        const std::wstring path = std::wstring(directory, length) + L"\\" + name;
        handle = LoadLibraryW(path.c_str());
        error = handle ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        if (!handle && SUCCEEDED(error)) error = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
    // Retain modules until process exit: static COM objects can outlive the
    // loader's own static storage during shutdown.
};
const Module& D3D12() { static const Module module(L"d3d12.dll"); return module; }
const Module& Dxgi() { static const Module module(L"dxgi.dll"); return module; }
const Module& Compiler() { static const Module module(L"d3dcompiler_47.dll"); return module; }

template<class T> struct Procedure {
    T call = nullptr;
    HRESULT error;
    Procedure(const Module& module, const char* name) : error(module.error) {
        if (module.handle) {
            call = reinterpret_cast<T>(GetProcAddress(module.handle, name));
            error = call ? S_OK : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
    }
};
}

HRESULT CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device) {
    if (device) *device = nullptr;
    static const Procedure<decltype(&::D3D12CreateDevice)> proc(D3D12(), "D3D12CreateDevice");
    return proc.call ? proc.call(adapter, level, iid, device) : proc.error;
}
HRESULT GetDebugInterface(REFIID iid, void** debug) {
    if (debug) *debug = nullptr;
    static const Procedure<decltype(&::D3D12GetDebugInterface)> proc(D3D12(), "D3D12GetDebugInterface");
    return proc.call ? proc.call(iid, debug) : proc.error;
}
HRESULT SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
    D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** error) {
    if (blob) *blob = nullptr;
    if (error) *error = nullptr;
    static const Procedure<decltype(&::D3D12SerializeRootSignature)> proc(D3D12(), "D3D12SerializeRootSignature");
    return proc.call ? proc.call(desc, version, blob, error) : proc.error;
}
HRESULT CreateFactory1(REFIID iid, void** factory) {
    if (factory) *factory = nullptr;
    static const Procedure<decltype(&::CreateDXGIFactory1)> proc(Dxgi(), "CreateDXGIFactory1");
    return proc.call ? proc.call(iid, factory) : proc.error;
}
HRESULT CreateFactory2(UINT flags, REFIID iid, void** factory) {
    if (factory) *factory = nullptr;
    static const Procedure<decltype(&::CreateDXGIFactory2)> proc(Dxgi(), "CreateDXGIFactory2");
    return proc.call ? proc.call(flags, iid, factory) : proc.error;
}
HRESULT Compile(const void* source, SIZE_T size, const char* name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, const char* entry,
    const char* target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** error) {
    if (code) *code = nullptr;
    if (error) *error = nullptr;
    static const Procedure<decltype(&::D3DCompile)> proc(Compiler(), "D3DCompile");
    return proc.call ? proc.call(source, size, name, defines, include, entry, target,
                                flags1, flags2, code, error) : proc.error;
}
}
