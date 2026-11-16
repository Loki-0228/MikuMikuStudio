#include "on12_output.hpp"
#include <d3d9on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>

namespace mme_re::mmhack {
namespace {
using Microsoft::WRL::ComPtr;
class Output final : public On12Output {
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> swap_;
  ComPtr<ID3D12Fence> fence_;
  struct Frame {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    UINT64 finished = 0;
  };
  std::array<Frame, 2> frames_;
  UINT64 serial_ = 0;
  HANDLE event_ = nullptr;

  HRESULT Wait(UINT64 value) {
    if (!value || fence_->GetCompletedValue() >= value) return S_OK;
    const auto hr = fence_->SetEventOnCompletion(value, event_);
    if (FAILED(hr)) return hr;
    return WaitForSingleObject(event_, INFINITE) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
  }
  HRESULT InitializeSwap(ID3D12Resource* source, HWND window) {
    // Resolve DXGI only for On12; ordinary DX9 keeps its older OS requirements.
    const auto dxgi = GetModuleHandleW(L"dxgi.dll");
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = dxgi ? reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!create) return E_NOINTERFACE;
    ComPtr<IDXGIFactory2> factory;
    auto hr = create(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    const auto resource = source->GetDesc();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(resource.Width); desc.Height = resource.Height;
    desc.Format = resource.Format; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = UINT(frames_.size());
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> first;
    hr = factory->CreateSwapChainForHwnd(queue_.Get(), window, &desc, nullptr, nullptr, &first);
    if (FAILED(hr)) return hr;
    hr = first.As(&swap_);
    if (FAILED(hr)) return hr;
    hr = factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(hr)) return hr;
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) return hr;
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return HRESULT_FROM_WIN32(GetLastError());
    for (UINT i = 0; i < frames_.size(); ++i) {
      auto& frame = frames_[i];
      hr = swap_->GetBuffer(i, IID_PPV_ARGS(&frame.buffer));
      if (FAILED(hr)) return hr;
      hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
      if (FAILED(hr)) return hr;
      hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr, IID_PPV_ARGS(&frame.commands));
      if (FAILED(hr)) return hr;
      hr = frame.commands->Close();
      if (FAILED(hr)) return hr;
    }
    return S_OK;
  }
public:
  ~Output() override {
    if (queue_ && fence_ && event_) {
      const auto value = ++serial_;
      if (SUCCEEDED(queue_->Signal(fence_.Get(), value))) Wait(value);
    }
    if (event_) CloseHandle(event_);
  }
  HRESULT Present(IDirect3DDevice9* device, IDirect3DSurface9* surface, HWND window) override {
    const auto hr = Publish(device, surface, window);
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ? D3DERR_DEVICELOST : hr;
  }
private:
  HRESULT Publish(IDirect3DDevice9* device, IDirect3DSurface9* surface, HWND window) {
    ComPtr<IDirect3DDevice9On12> interop;
    auto hr = device->QueryInterface(IID_PPV_ARGS(&interop));
    if (FAILED(hr)) return hr;
    if (!queue_) {
      hr = interop->GetD3D12Device(IID_PPV_ARGS(&device_));
      if (FAILED(hr)) return hr;
      D3D12_COMMAND_QUEUE_DESC desc{};
      desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      hr = device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_));
      if (FAILED(hr)) return hr;
      queue_->SetName(L"MMHack viewport copy queue");
    }
    ComPtr<ID3D12Resource> source;
    hr = interop->UnwrapUnderlyingResource(surface, queue_.Get(), IID_PPV_ARGS(&source));
    if (FAILED(hr)) return hr;
    // Every successful unwrap must be returned, including initialization failures.
    UINT64 submitted = 0;
    auto copy = [&]() -> HRESULT {
      if (!swap_) {
        const auto initialized = InitializeSwap(source.Get(), window);
        if (FAILED(initialized)) return initialized;
      }
      auto& frame = frames_[swap_->GetCurrentBackBufferIndex()];
      auto result = Wait(frame.finished);
      if (FAILED(result)) return result;
      result = frame.allocator->Reset();
      if (FAILED(result)) return result;
      result = frame.commands->Reset(frame.allocator.Get(), nullptr);
      if (FAILED(result)) return result;
      D3D12_RESOURCE_BARRIER barriers[2]{};
      for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      }
      barriers[0].Transition = {source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
      barriers[1].Transition = {frame.buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST};
      frame.commands->ResourceBarrier(2, barriers);
      frame.commands->CopyResource(frame.buffer.Get(), source.Get());
      for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      frame.commands->ResourceBarrier(2, barriers);
      result = frame.commands->Close();
      if (FAILED(result)) return result;
      ID3D12CommandList* commands[] = {frame.commands.Get()};
      queue_->ExecuteCommandLists(1, commands);
      const auto value = ++serial_;
      result = queue_->Signal(fence_.Get(), value);
      if (FAILED(result)) return result;
      submitted = value; frame.finished = value;
      return S_OK;
    };
    hr = copy();
    // Some system On12 versions schedule their next transition before applying
    // ReturnUnderlyingResource's deferred wait. Finish our read before returning
    // the surface so the next D3D9 write cannot race the native copy queue.
    if (SUCCEEDED(hr) && submitted) hr = Wait(submitted);
    ID3D12Fence* fences[] = {fence_.Get()};
    const auto returned = interop->ReturnUnderlyingResource(surface, submitted ? 1 : 0,
        submitted ? &submitted : nullptr, submitted ? fences : nullptr);
    if (FAILED(hr)) return hr;
    if (FAILED(returned)) return returned;
    return swap_->Present(0, 0);
  }
};
}
std::unique_ptr<On12Output> CreateOn12Output() { return std::make_unique<Output>(); }
}
