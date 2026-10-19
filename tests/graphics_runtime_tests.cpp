// MAINTENANCE: hardware-manual. Hidden D3D9 device; process-local DLL faults.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <MinHook.h>
#include "mikumikustudio/graphics_runtime.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <vector>

using namespace mikumikustudio;
using Microsoft::WRL::ComPtr;
namespace {
const wchar_t* missingDll = nullptr;
const char* missingExport = nullptr;
unsigned blocked = 0;
decltype(&LoadLibraryW) realLoad = nullptr;
decltype(&GetProcAddress) realGet = nullptr;
void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
HMODULE WINAPI Load(LPCWSTR path) {
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (missingDll && _wcsicmp(base, missingDll) == 0) {
        ++blocked; SetLastError(ERROR_MOD_NOT_FOUND); return nullptr;
    }
    return realLoad(path);
}
FARPROC WINAPI Resolve(HMODULE module, LPCSTR name) {
    if (reinterpret_cast<ULONG_PTR>(name) > 0xffff && missingExport &&
        std::strcmp(name, missingExport) == 0) {
        ++blocked; SetLastError(ERROR_PROC_NOT_FOUND); return nullptr;
    }
    return realGet(module, name);
}
std::vector<unsigned char> Read(IDirect3DVertexBuffer9* buffer, UINT size) {
    void* data = nullptr;
    Check(SUCCEEDED(buffer->Lock(0, size, &data, D3DLOCK_READONLY)), "read vertices");
    std::vector<unsigned char> bytes(static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
    buffer->Unlock(); return bytes;
}
void VerifyD3D9(bool fallback) {
    HWND window = CreateWindowExW(0, L"STATIC", L"MMS compatibility test", WS_POPUP,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr && !IsWindowVisible(window), "hidden window");
    {
        ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        Check(bool(d3d), "D3D9 runtime remains available");
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferWidth = pp.BackBufferHeight = 64;
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        ComPtr<IDirect3DDevice9> device;
        Check(SUCCEEDED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)), "D3D9 hardware device");
        auto app = std::make_unique<MMDApp>();
        app->state = MMDAppState{};
        app->CameraDistance() = -20; app->CameraFov() = 30;
        auto model = std::make_unique<mdl::ModelRecord>();
        constexpr UINT count = 3, stride = 32;
        mdl::BoneRecord bones[2]{}; mdl::PmdVertex vertices[count]{};
        mdl::ModelMaterialRecord material{};
        float world[16]{}; world[0] = world[5] = world[10] = world[15] = 1;
        for (unsigned b = 0; b < 2; ++b) {
            std::memcpy(bones[b].matInit, world, sizeof world);
            bones[b].matInit[12] = b * .4f;
        }
        for (unsigned v = 0; v < count; ++v) {
            vertices[v].position[0] = v * .25f;
            vertices[v].normal[1] = 1;
            vertices[v].bone[1] = 1; vertices[v].weightPercent = 60;
        }
        model->vertexCount = count; model->boneCount = 2; model->materialCount = 1;
        model->loadComplete = 1; model->edgeScale = 1;
        model->boneTable = bones; model->materials = &material; model->rawVertices = vertices;
        ComPtr<IDirect3DVertexBuffer9> body, edge;
        Check(SUCCEEDED(device->CreateVertexBuffer(count * stride, 0, 0, D3DPOOL_MANAGED, &body, nullptr)), "body buffer");
        Check(SUCCEEDED(device->CreateVertexBuffer(count * sizeof(mdl::EdgeVertex), 0, 0, D3DPOOL_MANAGED, &edge, nullptr)), "edge buffer");
        void* initial = nullptr;
        Check(SUCCEEDED(edge->Lock(0, 0, &initial, 0)), "initialize edge");
        std::memset(initial, 0, count * sizeof(mdl::EdgeVertex)); edge->Unlock();
        model->vertexBuffer = body.Get(); model->vertexBuffer2 = edge.Get();
        auto* bytes = reinterpret_cast<unsigned char*>(model.get());
        skinning::SetPreference({skinning::Mode::Cpu});
        UpdateModelVertexBuffers(app.get(), bytes, world);
        const auto cpu = Read(body.Get(), count * stride);
        const auto cpuEdge = Read(edge.Get(), count * sizeof(mdl::EdgeVertex));
        float firstX = 0; std::memcpy(&firstX, cpu.data(), sizeof firstX);
        Check(std::isfinite(firstX) && std::abs(firstX - .16f) < .0001f, "CPU applies bone translation");
        if (fallback) {
            // A saved explicit GPU choice must also recover when its runtime disappears.
            for (auto preference : {skinning::Preference{}, skinning::Preference{skinning::Mode::Adapter, UINT64_MAX}}) {
                skinning::SetPreference(preference);
                UpdateModelVertexBuffers(app.get(), bytes, world);
                Check(skinning::GetStatus().gpuUpdates == 0 && skinning::GetStatus().cpuUpdates > 0, "CPU handles unavailable GPU");
                Check(cpu == Read(body.Get(), count * stride), "fallback body equals CPU");
                Check(cpuEdge == Read(edge.Get(), count * sizeof(mdl::EdgeVertex)), "fallback edge equals CPU");
            }
        }
        Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(31, 63, 95), 1, 0)), "D3D9 clear");
        Check(SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr)), "D3D9 present");
        ComPtr<IDirect3DSurface9> target, readback;
        Check(SUCCEEDED(device->GetRenderTarget(0, &target)), "render target");
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &readback, nullptr)), "readback surface");
        Check(SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(31, 63, 95), 1, 0)), "clear after discard");
        Check(SUCCEEDED(device->GetRenderTargetData(target.Get(), readback.Get())), "D3D9 GPU readback");
        D3DLOCKED_RECT pixels{};
        Check(SUCCEEDED(readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY)), "read pixels");
        Check((*static_cast<DWORD*>(pixels.pBits) & 0xffffff) == 0x1f3f5f, "D3D9 rendered expected pixels");
        readback->UnlockRect();
        skinning::Shutdown();
    }
    DestroyWindow(window);
}
}

int main(int argc, char** argv) {
    Check(argc == 2, "mode argument required");
    const char* mode = argv[1];
    if (!std::strcmp(mode, "missing-d3d12")) missingDll = L"d3d12.dll";
    else if (!std::strcmp(mode, "missing-dxgi")) missingDll = L"dxgi.dll";
    else if (!std::strcmp(mode, "missing-compiler")) missingDll = L"d3dcompiler_47.dll";
    else if (!std::strcmp(mode, "missing-device-export")) missingExport = "D3D12CreateDevice";
    else if (!std::strcmp(mode, "missing-factory2-export")) missingExport = "CreateDXGIFactory2";
    else Check(!std::strcmp(mode, "system"), "known mode");
    Check(MH_Initialize() == MH_OK, "initialize process-local hooks");
    Check(MH_CreateHook(reinterpret_cast<void*>(&LoadLibraryW), reinterpret_cast<void*>(&Load),
        reinterpret_cast<void**>(&realLoad)) == MH_OK, "hook DLL loading");
    Check(MH_CreateHook(reinterpret_cast<void*>(&GetProcAddress), reinterpret_cast<void*>(&Resolve),
        reinterpret_cast<void**>(&realGet)) == MH_OK, "hook export resolution");
    Check(MH_EnableHook(MH_ALL_HOOKS) == MH_OK, "enable hooks");
    const bool fallback = missingDll || (missingExport && !std::strcmp(missingExport, "D3D12CreateDevice"));
    {
        gpu::Skinning compute;
        const bool ready = compute.Initialize(false);
        Check(ready != fallback, fallback ? "DX12 initialization fails safely" : "DX12 hardware path still works");
        if (fallback) Check(compute.Error()[0] != '\0', "failed initialization has a reason");
        if (missingDll && !_wcsicmp(missingDll, L"d3d12.dll")) {
            void* debug = reinterpret_cast<void*>(1);
            Check(FAILED(graphics_runtime::GetDebugInterface(__uuidof(ID3D12Device), &debug)) && !debug, "absent debug DLL");
            D3D12_ROOT_SIGNATURE_DESC desc{};
            ID3DBlob* blob = reinterpret_cast<ID3DBlob*>(1);
            ID3DBlob* error = reinterpret_cast<ID3DBlob*>(1);
            Check(FAILED(graphics_runtime::SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))
                && !blob && !error, "absent serializer DLL");
        }
        VerifyD3D9(fallback);
    }
    if (missingDll || missingExport) Check(blocked > 0, "fault was actually injected");
    Check(MH_DisableHook(MH_ALL_HOOKS) == MH_OK, "disable hooks");
    Check(MH_Uninitialize() == MH_OK, "release hooks");
    std::printf("PASS %s: DX9 device, CPU vertices, edge vertices, present and readback; injected=%u\n", mode, blocked);
    return 0;
}
