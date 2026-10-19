#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d9.h>
#include <wrl/client.h>

namespace mikumikustudio {
class MMDApp;
bool TransparentExportActive(MMDApp*);
IDirect3DSurface9* TransparentCaptureSurface(MMDApp*);
void ReleaseTransparentCapture(MMDApp*);
void SetCoverageAlphaBlend(IDirect3DDevice9*);
class TransparentFrameScope {
public:
    explicit TransparentFrameScope(MMDApp*);
    ~TransparentFrameScope();
    bool Ready() const { return ready_; }
private:
    IDirect3DDevice9* device_ = nullptr;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> previous_;
    D3DVIEWPORT9 viewport_{};
    DWORD alpha_[4]{};
    bool ready_ = true;
};
}
