#pragma once
#include "on12_output.hpp"

namespace mme_re::mmhack {
class On12Presenter {
public:
  ~On12Presenter() { Reset(); }
  using OutputFactory = std::unique_ptr<On12Output>(*)();
  explicit On12Presenter(OutputFactory factory = CreateOn12Output) : factory_(factory) {}
  On12Presenter(const On12Presenter&) = delete;
  On12Presenter& operator=(const On12Presenter&) = delete;
  void Reset() noexcept;
  HRESULT Present(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                  HWND window, const RGNDATA* dirty);
private:
  bool checked_ = false;
  bool on12_ = false;
  OutputFactory factory_;
  std::unique_ptr<On12Output> output_;
  IDirect3DSurface9* cropped_ = nullptr;
  IDirect3DSurface9* resolved_ = nullptr;
  HWND child_ = nullptr;
  UINT width_ = 0, height_ = 0;
  UINT outputWidth_ = 0, outputHeight_ = 0;
  D3DMULTISAMPLE_TYPE samples_ = D3DMULTISAMPLE_NONE;
  D3DFORMAT format_ = D3DFMT_UNKNOWN;
  HWND window_ = nullptr;
  RECT destination_{};
};
}
