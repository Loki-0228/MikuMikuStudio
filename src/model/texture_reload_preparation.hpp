#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/import_texture_cache.hpp"
#include <array>
#include <cstdint>
#include <memory>
namespace mikumikustudio {
class MMDApp;
namespace texture_reload {
struct TextureRelease { void operator()(IDirect3DTexture9* p) const noexcept { if (p) p->Release(); } };
using Texture = std::unique_ptr<IDirect3DTexture9, TextureRelease>;
struct PreparedSharedToon {
    std::array<Texture, 11> textures;
    std::array<float, 30> edge{};
};
struct PreparedCacheTexture { Texture texture; std::int32_t tag = 0; };
using SharedToonLoader = MmsResult (*)(MMDApp&, PreparedSharedToon&);
// Outputs retain their old values on failure. These loaders never publish to
// app.ToonTexture, toonEdgeTable or renderer.resourcePool, or change CWD.
// The default D3DX calls continue through the existing shared/IAT-aware loader.
// Host pointer publication is atomic; third-party hook-private registries are
// outside this resource owner and must not be inferred atomic from this API.
MmsResult PrepareSharedToon(MMDApp&, PreparedSharedToon&) noexcept;
// out.tag supplies the previous RGB fallback if a successful texture cannot
// be locked for colour sampling, matching the original cache reload. Fatal
// failure preserves the complete out object, including its owned reference.
MmsResult PrepareCacheTexture(IDirect3DDevice9*, const wchar_t*,
    import_texture_cache::TextureLoader, PreparedCacheTexture&) noexcept;
}
}
#endif
