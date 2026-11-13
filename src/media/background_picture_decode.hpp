#pragma once
// Private common decode recipe and import placement for the project loader
// and atomic import preparation. No scene field is written here.
#include "mikumikustudio/background_picture_asset.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include <cmath>
#include <cstdint>

namespace mikumikustudio::media::picture_detail {
struct ImageInfo {
    UINT Width, Height, Depth, MipLevels;
    D3DFORMAT Format;
    D3DRESOURCETYPE ResourceType;
    UINT ImageFileFormat;
};
static_assert(sizeof(ImageInfo) == 28, "D3DXIMAGE_INFO ABI");

// false means a reentrant operation invalidated the caller. A failed HRESULT
// permits the import caller's one fallback; restoration chooses one recipe.
template<class Current>
bool DecodeAttempt(IDirect3DDevice9* device, const BackgroundPictureSourcePtr& source,
    PictureDecodeMode mode, ImageInfo& info, IDirect3DTexture9*& candidate,
    HRESULT& result, const Current& current, bool* attempted = nullptr) {
    result = E_FAIL;
    if (attempted) *attempted = false;
    auto& api = d3dx::Get();
    const bool loaded = api.Load();
    if (!current()) return false;
    const auto decode = loaded ? api.fromMemEx : nullptr;
    if (!decode) return true;
    if (attempted) *attempted = true;
    const bool fallback = mode == PictureDecodeMode::Fallback512;
    const UINT side = fallback ? 512 : 1024;
    result = decode(device, source->Data(), source->Size(), side, side,
        fallback ? 1 : 0, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, UINT(-1), UINT(-1),
        0, &info, nullptr, &candidate);
    return current();
}

inline bool ImportPlacement(const MMDApp& app, const D3DRenderer& renderer,
    std::int32_t width, std::int32_t height, float& scale, std::int32_t& y) noexcept {
    if (width <= 0 || height <= 0) return false;
    const auto viewScale = renderer.viewScale;
    const auto spanW = static_cast<std::int64_t>(app.state.hideLeft) - app.state.hideRight;
    const auto spanH = static_cast<std::int64_t>(app.state.hideBottom) - app.state.hideTop;
    if (!std::isfinite(viewScale) || viewScale <= 0 || spanW <= 0 || spanH <= 0 ||
        spanW > INT32_MAX || spanH > INT32_MAX) return false;
    // Preserve the original single-precision MediaAspect evaluation order.
    const float nextScale = (static_cast<float>(spanW) / viewScale * 1.2f) / static_cast<float>(width);
    const float nextY = static_cast<float>(spanH / 2) -
        (static_cast<float>(height) * nextScale * viewScale / 1.2f) * .5f;
    if (!std::isfinite(nextScale) || nextScale <= 0 || !std::isfinite(nextY) ||
        static_cast<double>(nextY) < static_cast<double>(INT32_MIN) ||
        static_cast<double>(nextY) > static_cast<double>(INT32_MAX)) return false;
    scale = nextScale; y = static_cast<std::int32_t>(nextY);
    return true;
}
}
