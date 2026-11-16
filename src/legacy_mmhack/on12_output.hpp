#pragma once
#include <d3d9.h>
#include <memory>

namespace mme_re::mmhack {
class On12Output {
public:
  virtual ~On12Output() = default;
  virtual HRESULT Present(IDirect3DDevice9* device, IDirect3DSurface9* surface, HWND window) = 0;
};
std::unique_ptr<On12Output> CreateOn12Output();
}
