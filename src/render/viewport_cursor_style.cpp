// Flat viewport cursors in the right half of the existing HUD atlas.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include "viewport_cursor_style.hpp"
#include <cstddef>
#include <cmath>

namespace mikumikustudio {
namespace {
constexpr int kSheetSize = 512;
constexpr int kCellSize = 128;
constexpr int kSamples = 8;
constexpr unsigned kRingAlpha = 179;  // 70% opacity
constexpr float kStroke = 0.9f;
constexpr float kRim = 1.7f;
constexpr std::uint32_t kClear = 0x00FFFFFFu;
constexpr std::uint32_t kBlack = 0xFF000000u;
constexpr std::uint32_t kLight = 0xFFF0F0F0u;
enum Ink { Clear = -1, Green, Blue, Red, Yellow, White };
constexpr std::uint32_t kMove[] = {
    0xFF00C832u, 0xFF1478FFu, 0xFFE1192Du, 0xFFFFE600u, 0xFFFFFFFFu};
constexpr std::uint32_t kRotate[] = {
    0xFF00E020u, 0xFF0060FFu, 0xFFFF2020u, 0xFFFFE600u, 0xFFFFFFFFu};

bool Rect(float x, float y, float left, float top, float right, float bottom) {
    return x >= left && x <= right && y >= top && y <= bottom;
}

// Coordinates retain the original 128-pixel cell layout and operation origin.
// Each region has exactly one fill: no source-image gradients or hand pixels
// enter the result. The existing sprite producer still selects all eight cells.
Ink ShapeAt(float x, float y, int row, int column) {
    if (row < 2) {
        const bool upHover = row == 0 && column == 1;
        const bool rightHover = row == 1 && column == 0;
        const bool centerHover = row == 1 && column == 1;
        // Up triangle: apex (22, 9), base (7, 36)..(37, 36).
        if (y >= 9.0f && y <= 36.0f &&
            x >= 22.0f - (y - 9.0f) * (15.0f / 27.0f) &&
            x <= 22.0f + (y - 9.0f) * (15.0f / 27.0f))
            return upHover ? Yellow : Green;
        // Right triangle: apex (126, 114), base (100, 101)..(100, 127).
        if (x >= 100.0f && x <= 126.0f &&
            y >= 114.0f - (126.0f - x) * (13.0f / 26.0f) &&
            y <= 114.0f + (126.0f - x) * (13.0f / 26.0f))
            return rightHover ? Yellow : Red;
        if (Rect(x, y, 17, 109, 27, 119))
            return centerHover ? Red : White;
        if (Rect(x, y, 19.5f, 34, 24.5f, 114) ||
            Rect(x, y, 22, 111.5f, 102, 116.5f))
            return White;
        return Clear;
    }
    const float dx = x - 64.0f;
    const float dy = y - 64.0f;
    const float radiusSquared = dx * dx + dy * dy;
    // The centre stays open for the selected-bone marker.
    if (radiusSquared < 6.0f * 6.0f)
        return Clear;
    if (Rect(x, y, 56, 13, 72, 115))
        return row == 2 && column == 1 ? Yellow : Red;
    if (Rect(x, y, 13, 56, 115, 72))
        return row == 3 && column == 0 ? Yellow : Blue;
    if (radiusSquared >= 49.0f * 49.0f &&
        radiusSquared <= 62.25f * 62.25f)
        return row == 3 && column == 1 ? Yellow : Green;
    return Clear;
}

std::uint32_t Sample(float x, float y, int row, int column) {
    const Ink ink = ShapeAt(x, y, row, column);
    const float dx = x - 64.0f, dy = y - 64.0f;
    const float radius = row >= 2 ? std::sqrt(dx * dx + dy * dy) : 0.0f;
    const bool ringInk = row >= 2 &&
        (ink == Green || (ink == Yellow && row == 3 && column == 1));
    const auto withRingAlpha = [](std::uint32_t color) {
        return (color & 0xFFFFFFu) | (kRingAlpha << 24);
    };
    if (ink != Clear) {
        // Radial distance keeps circular strokes equally thick at every angle.
        const bool radialEdge = ringInk &&
            (radius < 49.0f + kStroke || radius > 62.25f - kStroke);
        const bool edge = radialEdge ||
            ShapeAt(x - kStroke, y, row, column) != ink ||
            ShapeAt(x + kStroke, y, row, column) != ink ||
            ShapeAt(x, y - kStroke, row, column) != ink ||
            ShapeAt(x, y + kStroke, row, column) != ink;
        const auto color = edge ? kBlack : (row < 2 ? kMove[ink] : kRotate[ink]);
        return ringInk ? withRingAlpha(color) : color;
    }
    if (row >= 2 && radius >= 49.0f - kRim && radius <= 62.25f + kRim)
        return withRingAlpha(kLight);
    if (ShapeAt(x - kRim, y, row, column) != Clear ||
        ShapeAt(x + kRim, y, row, column) != Clear ||
        ShapeAt(x, y - kRim, row, column) != Clear ||
        ShapeAt(x, y + kRim, row, column) != Clear)
        return kLight;
    return kClear;
}
}  // namespace

void RestyleViewportCursorAtlasPixels(std::uint32_t* pixels, int width,
                                      int height, int stridePixels) {
    if (!pixels || width != kSheetSize || height != kSheetSize ||
        stridePixels < kSheetSize)
        return;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 2; ++column) {
            for (int y = 0; y < kCellSize; ++y) {
                for (int x = 0; x < kCellSize; ++x) {
                    unsigned alphaSum = 0, red = 0, green = 0, blue = 0;
                    // Integrate the 90/75-screen-pixel sampling footprint, not
                    // just a source texel; thin edges survive atlas minification.
                    const float footprint = kCellSize / (row < 2 ? 75.0f : 90.0f);
                    for (int sy = 0; sy < kSamples; ++sy) {
                        for (int sx = 0; sx < kSamples; ++sx) {
                            const auto color = Sample(
                                x + 0.5f + ((sx + 0.5f) / kSamples - 0.5f) * footprint,
                                y + 0.5f + ((sy + 0.5f) / kSamples - 0.5f) * footprint, row, column);
                            const unsigned alpha = color >> 24;
                            alphaSum += alpha;
                            red += ((color >> 16) & 255) * alpha;
                            green += ((color >> 8) & 255) * alpha;
                            blue += (color & 255) * alpha;
                        }
                    }
                    const auto color = alphaSum == 0 ? kClear :
                        ((alphaSum / (kSamples * kSamples)) << 24) |
                        ((red / alphaSum) << 16) | ((green / alphaSum) << 8) |
                        (blue / alphaSum);
                    pixels[static_cast<std::size_t>(row * kCellSize + y) *
                               stridePixels + 256 + column * kCellSize + x] = color;
                }
            }
        }
    }
}

bool RestyleViewportCursorAtlas(IDirect3DTexture9* texture) {
    if (!texture) return false;
    D3DSURFACE_DESC description{};
    if (FAILED(texture->GetLevelDesc(0, &description)) ||
        description.Format != D3DFMT_A8R8G8B8 ||
        description.Width != kSheetSize || description.Height != kSheetSize)
        return false;
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) return false;
    RestyleViewportCursorAtlasPixels(static_cast<std::uint32_t*>(locked.pBits),
        kSheetSize, kSheetSize, locked.Pitch / sizeof(std::uint32_t));
    return SUCCEEDED(texture->UnlockRect(0));
}
}  // namespace mikumikustudio
