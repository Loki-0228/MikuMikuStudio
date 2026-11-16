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
  output_.reset();
  if (cropped_) cropped_->Release();
  cropped_ = nullptr;
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
  HWND target = window ? window : parameters.hDeviceWindow;
  if (!target) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    result = device->GetCreationParameters(&creation);
    if (FAILED(result)) return result;
    target = creation.hFocusWindow;
  }
  RECT client{};
  if (!GetClientRect(target, &client)) return D3DERR_INVALIDCALL;
  const RECT input = source ? *source : RECT{0, 0, LONG(description.Width), LONG(description.Height)};
  const RECT output = destination ? *destination : client;
  if (IsRectEmpty(&input) || IsRectEmpty(&output) || input.left < 0 || input.top < 0 ||
      input.right > LONG(description.Width) || input.bottom > LONG(description.Height)) return D3DERR_INVALIDCALL;
  const UINT outputWidth = UINT(output.right - output.left), outputHeight = UINT(output.bottom - output.top);
  if (output_ && (width_ != description.Width || height_ != description.Height ||
      format_ != description.Format || window_ != target || !IsWindow(child_) ||
      samples_ != description.MultiSampleType || outputWidth_ != outputWidth || outputHeight_ != outputHeight)) Reset();
  if (!output_) {
    child_ = CreateViewport(target, output);
    if (!child_) { const auto error = GetLastError(); return error ? HRESULT_FROM_WIN32(error) : E_FAIL; }
    // The final surface is shared with a dedicated DXGI swap chain. Keeping
    // presentation outside D3D9's implicit chain also survives device Reset.
    const auto outputFormat = description.Format == D3DFMT_X8R8G8B8 ? D3DFMT_A8R8G8B8 : description.Format;
    result = device->CreateRenderTarget(outputWidth, outputHeight, outputFormat,
        D3DMULTISAMPLE_NONE, 0, FALSE, &cropped_, nullptr);
    if (SUCCEEDED(result) && description.MultiSampleType != D3DMULTISAMPLE_NONE)
      result = device->CreateRenderTarget(description.Width, description.Height, description.Format,
          D3DMULTISAMPLE_NONE, 0, FALSE, &resolved_, nullptr);
    if (FAILED(result)) { Reset(); return result; }
    try { output_ = factory_(); }
    catch (const std::bad_alloc&) { Reset(); return E_OUTOFMEMORY; }
    if (!output_) { Reset(); return E_OUTOFMEMORY; }
    width_ = description.Width; height_ = description.Height;
    format_ = description.Format; window_ = target; samples_ = description.MultiSampleType;
    outputWidth_ = outputWidth; outputHeight_ = outputHeight;
  }
  // MSAA resolves must cover the full, equally sized surface. Crop/scale only
  // after resolving; the child then presents its entire buffer through DXGI.
  IDirect3DSurface9* pixels = back.Get();
  if (resolved_) {
    result = device->StretchRect(back.Get(), nullptr, resolved_, nullptr, D3DTEXF_NONE);
    if (FAILED(result)) return result;
    pixels = resolved_;
  }
  result = device->StretchRect(pixels, &input, cropped_, nullptr, D3DTEXF_NONE);
  if (FAILED(result)) return result;
  if (!EqualRect(&destination_, &output) || !(GetWindowLongPtrW(child_, GWL_STYLE) & WS_VISIBLE)) {
    if (!SetWindowPos(child_, HWND_BOTTOM, output.left, output.top, outputWidth, outputHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
      const auto error = GetLastError();
      return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    destination_ = output;
  }
  result = output_->Present(device, cropped_, child_);
  if (FAILED(result)) Reset();
  return result;
}
}
