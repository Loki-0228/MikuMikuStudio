#include "on12_presenter.hpp"
#include <d3d9on12.h>
#include <wrl/client.h>

namespace mme_re::mmhack {
using Microsoft::WRL::ComPtr;
namespace {
LRESULT CALLBACK ViewportProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_ERASEBKGND) return 1;
  return DefWindowProcW(window, message, w, l);
}
HWND CreateViewport(HWND parent, const RECT& rectangle) {
  // A separate HWND keeps D3D9On12's flip presentation independent of the
  // parent's GDI controls. Mouse input continues to reach the editor parent.
  static const wchar_t name[] = L"MMS.MME.On12.Viewport";
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&ViewportProcedure), &module)) return nullptr;
  WNDCLASSW type{};
  type.lpfnWndProc = ViewportProcedure; type.hInstance = module;
  type.lpszClassName = name;
  if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
  return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY, name, L"",
      WS_CHILD | WS_DISABLED, rectangle.left, rectangle.top,
      rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
      parent, nullptr, module, nullptr);
}
}
void On12Presenter::Reset() noexcept {
  if (chain_) chain_->Release();
  chain_ = nullptr;
  if (resolved_) resolved_->Release();
  resolved_ = nullptr;
  if (child_) DestroyWindow(child_);
  child_ = nullptr;
  destination_ = {};
}
HRESULT On12Presenter::Present(IDirect3DDevice9* device, const RECT* source,
                              const RECT* destination, HWND window, const RGNDATA* dirty) {
  // Borderless fullscreen uses the same windowed device with null rectangles.
  if (!checked_) {
    ComPtr<IDirect3DDevice9On12> interop;
    on12_ = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&interop))) && interop;
    checked_ = true;
  }
  if (!on12_) return device->Present(source, destination, window, dirty);

  ComPtr<IDirect3DSwapChain9> implicit;
  HRESULT result = device->GetSwapChain(0, &implicit);
  if (FAILED(result)) return result;
  D3DPRESENT_PARAMETERS parameters{};
  result = implicit->GetPresentParameters(&parameters);
  if (FAILED(result)) return result;
  if (!parameters.Windowed) {
    Reset();
    return device->Present(source, destination, window, dirty);
  }

  ComPtr<IDirect3DSurface9> back;
  result = implicit->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
  if (FAILED(result)) return result;
  D3DSURFACE_DESC description{};
  result = back->GetDesc(&description);
  if (FAILED(result)) return result;
  const HWND target = window ? window : parameters.hDeviceWindow;
  RECT client{};
  if (!GetClientRect(target, &client)) return D3DERR_INVALIDCALL;
  const RECT input = source ? *source : RECT{0, 0, LONG(description.Width), LONG(description.Height)};
  const RECT output = destination ? *destination : client;
  if (IsRectEmpty(&input) || IsRectEmpty(&output) || input.left < 0 || input.top < 0 ||
      input.right > LONG(description.Width) || input.bottom > LONG(description.Height)) return D3DERR_INVALIDCALL;
  const UINT outputWidth = UINT(output.right - output.left), outputHeight = UINT(output.bottom - output.top);
  if (chain_ && (width_ != description.Width || height_ != description.Height ||
      format_ != description.Format || window_ != target || !IsWindow(child_) ||
      samples_ != description.MultiSampleType || outputWidth_ != outputWidth || outputHeight_ != outputHeight)) Reset();
  if (!chain_) {
    child_ = CreateViewport(target, output);
    if (!child_) { const auto error = GetLastError(); return error ? HRESULT_FROM_WIN32(error) : E_FAIL; }
    parameters.BackBufferWidth = outputWidth;
    parameters.BackBufferHeight = outputHeight;
    parameters.BackBufferFormat = description.Format;
    parameters.BackBufferCount = 1;
    parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    parameters.MultiSampleQuality = 0;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = child_;
    parameters.EnableAutoDepthStencil = FALSE;
    parameters.Flags = 0;
    parameters.FullScreen_RefreshRateInHz = 0;
    result = device->CreateAdditionalSwapChain(&parameters, &chain_);
    if (SUCCEEDED(result) && description.MultiSampleType != D3DMULTISAMPLE_NONE)
      result = device->CreateRenderTarget(description.Width, description.Height, description.Format,
          D3DMULTISAMPLE_NONE, 0, FALSE, &resolved_, nullptr);
    if (FAILED(result)) { Reset(); return result; }
    width_ = description.Width; height_ = description.Height;
    format_ = description.Format; window_ = target; samples_ = description.MultiSampleType;
    outputWidth_ = outputWidth; outputHeight_ = outputHeight;
  }
  // MSAA resolves must cover the full, equally sized surface. Crop/scale only
  // after resolving; the child then presents its entire buffer with null rects.
  IDirect3DSurface9* pixels = back.Get();
  if (resolved_) {
    result = device->StretchRect(back.Get(), nullptr, resolved_, nullptr, D3DTEXF_NONE);
    if (FAILED(result)) return result;
    pixels = resolved_;
  }
  ComPtr<IDirect3DSurface9> outputBuffer;
  result = chain_->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &outputBuffer);
  if (SUCCEEDED(result)) result = device->StretchRect(pixels, &input, outputBuffer.Get(), nullptr, D3DTEXF_NONE);
  if (FAILED(result)) return result;
  if (!EqualRect(&destination_, &output)) {
    if (!SetWindowPos(child_, HWND_BOTTOM, output.left, output.top, outputWidth, outputHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW)) return HRESULT_FROM_WIN32(GetLastError());
    destination_ = output;
  }
  // Always publish the complete viewport, also for a dirty-region request.
  // Partial COPY presents on the GDI parent can succeed but display stale pixels.
  return chain_->Present(nullptr, nullptr, nullptr, nullptr, 0);
}
}
