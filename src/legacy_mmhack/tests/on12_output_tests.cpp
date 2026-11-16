#include "../on12_output.hpp"
#include <d3d9on12.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace mme_re::mmhack;
namespace {
void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    std::cerr << operation << " hr=0x" << std::hex << unsigned(hr) << '\n';
    throw std::runtime_error(operation);
  }
}
struct Window {
  HWND value = CreateWindowExW(0, L"STATIC", L"On12 output test", WS_POPUP,
      0, 0, 160, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  ~Window() { if (value) DestroyWindow(value); }
};
}
int main() {
  try {
    // Enable validation when Graphics Tools is installed, before any D3D device.
    const auto d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using GetDebug = HRESULT(WINAPI*)(REFIID, void**);
    const auto debug = d3d12 ? reinterpret_cast<GetDebug>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
    ComPtr<ID3D12Debug> validation;
    if (debug && SUCCEEDED(debug(IID_PPV_ARGS(&validation)))) validation->EnableDebugLayer();
    const auto d3d9 = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create = d3d9 ? reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(d3d9, "Direct3DCreate9On12")) : nullptr;
    if (!create) { std::cout << "SKIP: D3D9On12 unavailable\n"; return 77; }
    D3D9ON12_ARGS args{}; args.Enable9On12 = TRUE;
    ComPtr<IDirect3D9> factory; factory.Attach(create(D3D_SDK_VERSION, &args, 1));
    if (!factory) { std::cout << "SKIP: no On12 adapter\n"; return 77; }
    Window parent, child;
    if (!parent.value || !child.value) throw std::runtime_error("Window creation");
    SetWindowLongPtrW(child.value, GWL_STYLE, WS_CHILD | WS_DISABLED);
    SetParent(child.value, parent.value);
    D3DPRESENT_PARAMETERS params{};
    params.hDeviceWindow = parent.value; params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.BackBufferWidth = params.BackBufferHeight = 160;
    params.BackBufferFormat = D3DFMT_A8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    check(factory->CreateDevice(0, D3DDEVTYPE_HAL, parent.value,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device), "Create On12 device");
    ComPtr<IDirect3DDevice9On12> interop;
    check(device.As(&interop), "Confirm actual On12 backend");
    ComPtr<ID3D12Device> native;
    check(interop->GetD3D12Device(IID_PPV_ARGS(&native)), "Get D3D12 device");
    ComPtr<ID3D12InfoQueue> messages;
    if (validation) check(native.As(&messages), "Get D3D12 validation messages");
    if (messages) messages->ClearStoredMessages();
    for (unsigned generation = 0; generation != 3; ++generation) {
      const UINT size = 64 + generation * 16;
      ComPtr<IDirect3DSurface9> surface, readback;
      check(device->CreateRenderTarget(size, size, D3DFMT_A8R8G8B8,
          D3DMULTISAMPLE_NONE, 0, FALSE, &surface, nullptr), "Create shared render target");
      check(device->CreateOffscreenPlainSurface(size, size, D3DFMT_A8R8G8B8,
          D3DPOOL_SYSTEMMEM, &readback, nullptr), "Create readback");
      auto output = CreateOn12Output();
      for (unsigned frame = 0; frame != 12; ++frame) {
        const DWORD color = frame % 2 ? 0xff126ac9 : 0xffd94621;
        check(device->ColorFill(surface.Get(), nullptr, color), "D3D9 update after native use");
        check(output->Present(device.Get(), surface.Get(), child.value), "DXGI presentation");
        check(device->GetRenderTargetData(surface.Get(), readback.Get()), "D3D9 read after native return");
        D3DLOCKED_RECT pixels{};
        check(readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY), "Read pixels");
        const auto actual = *static_cast<const DWORD*>(pixels.pBits);
        check(readback->UnlockRect(), "Unlock pixels");
        if (actual != color) throw std::runtime_error("Stale/corrupted shared surface after native copy");
      }
      output.reset(); surface.Reset(); readback.Reset();
      check(device->Reset(&params), "Reset after releasing DXGI output");
    }
    if (messages) {
      for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; messages->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        check(messages->GetMessage(i, message, &size), "Read validation message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << message->pDescription << '\n';
          throw std::runtime_error("D3D12 validation error");
        }
      }
    }
    std::cout << "PASS: real On12/DXGI interop, 36 changing frames, three output lifecycles and Reset; validation="
        << (validation ? "enabled" : "unavailable") << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
