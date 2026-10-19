#pragma once
#include <d3d9.h>
#include <memory>
#include "mikumikustudio/view_options.hpp"
namespace mikumikustudio {
class TransparentPeeling {
public:
    explicit TransparentPeeling(IDirect3DDevice9* device);
    ~TransparentPeeling();
    bool Ready() const;
    bool NativePass();
    bool PrepareTargets();
    bool BeginLayer(unsigned layer);
    bool BeginMaterial(void* originalEffect);
    void EndMaterial();
    // 1 = layer folded into the private float accumulation, 0 = no fragments,
    // -1 = device failure.  The original render target and depth buffer are not
    // written here; the layers stay private until Commit().  The accumulation
    // is a private float ping-pong whose blend is computed by the pixel shader
    // with blending disabled, so no float render-target blending is required.
    // A multisampled original depth buffer cannot be bound beside those float
    // targets, so its occlusion is first captured per sample into an FP16
    // coverage mask; only that coverage is quantised.
    int EndLayer();
    // Blends the accumulated premultiplied colour into the original render
    // target with ONE / INVSRCALPHA and restores the nearest valid fragment
    // depth.  Returns true only when that draw was submitted: with no
    // accumulated layer, after a previous commit, or on device failure it
    // returns false and the original render target and depth buffer keep the
    // contents they had before the first BeginLayer.
    bool Commit();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
void ClearTransparentPeelingCache();
} // namespace mikumikustudio
