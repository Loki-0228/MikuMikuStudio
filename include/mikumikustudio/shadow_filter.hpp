#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <array>
#include <vector>

namespace mikumikustudio {
class MMDApp;
class D3DRenderer;

namespace shadow_filter {
// A private, same-projection shadow target. It never replaces hdrTexture or
// changes the renderer ABI. All APIs below are called on the render thread.
class FineDepthTarget final {
public:
    HRESULT Ensure(IDirect3DDevice9* device, UINT coarseSize, UINT requestedFineSize);
    void Release() noexcept;
    UINT Size() const noexcept { return size_; }
    IDirect3DTexture9* Texture() const noexcept { return texture_.Get(); }
    IDirect3DSurface9* Surface() const noexcept { return surface_.Get(); }
    IDirect3DSurface9* DepthSurface() const noexcept { return depth_.Get(); }
private:
    Microsoft::WRL::ComPtr<IDirect3DDevice9> device_;
    Microsoft::WRL::ComPtr<IDirect3DTexture9> texture_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> surface_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> depth_;
    UINT size_ = 0;
};

// Saves all supported render targets, depth, viewport and D3DSBT_ALL state.
// The caller owns BeginScene/EndScene, application fields and effect parameters.
class TargetScope final {
public:
    TargetScope() = default;
    TargetScope(const TargetScope&) = delete;
    TargetScope& operator=(const TargetScope&) = delete;
    ~TargetScope() { Restore(); }
    HRESULT Begin(IDirect3DDevice9* device, const FineDepthTarget& target);
    HRESULT Restore() noexcept;
private:
    void ClearSaved() noexcept;
    Microsoft::WRL::ComPtr<IDirect3DDevice9> device_;
    Microsoft::WRL::ComPtr<IDirect3DStateBlock9> state_;
    std::array<Microsoft::WRL::ComPtr<IDirect3DSurface9>, 4> targets_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> depth_;
    D3DVIEWPORT9 viewport_{};
    UINT targetCount_ = 0;
    bool captured_ = false;
};

// Call at frame start, even when the native shadow pass will be skipped.
// Disabling the option also releases resources and its failed-attempt cache.
void Invalidate(MMDApp* app);

// Call only after the current frame's native shadow pass. A non-null return
// requests a second caster pass with exactly the same projection and casters.
// A null return can also mean the coarse map is already large enough: the
// module then enables directional filtering on that current coarse map.
FineDepthTarget* Prepare(MMDApp* app, bool nativeSucceeded);

// Call only for a non-null Prepare result, after TargetScope::Restore. Include
// restoration failures in succeeded; no failed/previous-frame fine map is used.
void MarkRendered(MMDApp* app, bool succeeded);
void FinalizeDepth(MMDApp* app, bool succeeded);
IDirect3DTexture9* VisibleTexture(MMDApp* app);
struct SamplingState {
    IDirect3DTexture9* texture = nullptr; // Borrowed; caller may retain for its synchronous draw.
    bool enhanced = false; // Receiver-side filtering still required.
    bool prefiltered = false; // Shared texture has already been filtered.
    float coarseSize[2]{0.0f, 0.0f}; // Required when MMV rebuilds casters.
    float mapSize[4]{1.0f, 1.0f, 0.0f, 0.0f};
};
SamplingState ReadSamplingState(MMDApp* app);
// Native enhanced receivers use raw coverage on their private sampler 6;
// sampler 0 keeps the shared prefiltered texture inherited by MME.
SamplingState ReadNativeSamplingState(MMDApp* app);
void ConfigureEffect(MMDApp* app, void* effect);

// Camera-depth layers are dynamic precision only, independent of filtering.
// Fixed mode may use the separate per-model maps below, never these layers.
void PlanDetail(MMDApp* app, const std::array<float,16>& projection,
                const std::array<float,16>& coverage, const float view[16],
                float farDepth, UINT size, unsigned layer = 0, float densityFloor = 0,
                float nearDepth = 0, float blendFraction = .2f,
                float screenDemand = 0, bool orthographic = false);
FineDepthTarget* PrepareDetail(MMDApp* app, bool nativeSucceeded,
                              std::array<float,16>& projection, unsigned layer = 0);
void MarkDetailRendered(MMDApp* app, bool succeeded, unsigned layer = 0);
void ConfigureDetailReceiver(MMDApp* app, void* effect, bool receivesShadow,
                             unsigned char* model = nullptr);
struct DetailInfo {
    bool valid = false;
    UINT size = 0;
    std::array<float,16> projection{};
    float densityFloor = 0;
    float nearDepth = 0, farDepth = 0, blendStart = 0;
};
DetailInfo ReadDetailInfo(MMDApp* app, unsigned layer = 0);

// Fixed per-model maps use native slider density, never camera depth tiers.
// Each pass renders all scene casters; only its owning model receives the map.
void PlanModel(MMDApp*, unsigned char* model, const std::array<float,16>& projection,
               const std::array<float,16>& coverage, UINT size, float density);
std::vector<unsigned char*> PlannedModels(MMDApp*);
FineDepthTarget* PrepareModel(MMDApp*, unsigned char* model, bool nativeSucceeded,
                             std::array<float,16>& projection);
void MarkModelRendered(MMDApp*, unsigned char* model, bool succeeded);
DetailInfo ReadModelInfo(MMDApp*, unsigned char* model);
void SetModelShadowsEnabledForTests(bool enabled);

// Before device Reset, release without forgetting; at renderer shutdown forget.
// Both forms clear failed-attempt state so a recovered device can try again.
void ReleaseRenderer(D3DRenderer* renderer, bool forget = false);
} // namespace shadow_filter
} // namespace mikumikustudio
