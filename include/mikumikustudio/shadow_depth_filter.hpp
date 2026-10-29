#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>

namespace mikumikustudio::shadow_filter {

// Normalized cubic weights at offsets -1, 0, 1 with the original 4096-map
// scale 0.75 * 1.240967365. Use the same value in receiver reconstruction.
inline constexpr float kDepthFilterSideWeight = 0.14195122271793367f;

// Receiver-independent filtering shared with external effects. The caller
// owns BeginScene/EndScene; Apply restores render targets and device state.
class DepthPrefilter final {
public:
    void Release() noexcept;
    UINT Size() const noexcept { return size; }
    IDirect3DTexture9* Texture() const noexcept { return final.Get(); }
    HRESULT Apply(IDirect3DDevice9* device, IDirect3DTexture9* input, UINT coarse);

private:
    HRESULT Ensure(IDirect3DDevice9* device, UINT requested);
    Microsoft::WRL::ComPtr<IDirect3DTexture9> scratch, final;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> scratchSurface, finalSurface;
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> shader;
    UINT size = 0;
    unsigned frames = 0;
};

} // namespace mikumikustudio::shadow_filter
