#pragma once
#include <Windows.h>
#include <algorithm>
#include <cmath>

namespace mikumikustudio {
// All coordinates use the window's DPI coordinate system, like its controls.
// Extra rows consume existing letterbox margins; the scene never moves.
struct ViewportPanelLayout {
    RECT overlay{}, topBand{}, bottomBand{};
    bool topWrapped = false, bottomWrapped = false;
    float topScale = 1, bottomScale = 1;
    int bottomRowHeight = 32;
    // Numeric fields may give up 15 of their usual 53 units before any font
    // shrinks. Labels and gaps keep their normal width during this step.
    float fieldReduction = 0;
    int fieldStarts[9]{}, fieldCount = 0;
    static int Pixels(float value) { return static_cast<int>(std::lround(value)); }
    int TopX(int fromRight) const { return overlay.right - Pixels(fromRight * topScale); }
    int TopY(int offset) const { return overlay.top - 12 + Pixels((offset + 12) * topScale); }
    int TitleRight() const { return topWrapped ? overlay.right - 5 : TopX(450); }
    float BottomColumn(int offset) const {
        float result = float(offset);
        for (int i = 0; i < fieldCount; ++i)
            result -= std::clamp(float(offset - fieldStarts[i]) / 53.0f, 0.0f, 1.0f) * fieldReduction;
        return result;
    }
    int BottomX(int offset) const {
        return overlay.left + Pixels((BottomColumn(offset) -
            (bottomWrapped && offset >= 365 ? BottomColumn(360) : 0)) * bottomScale);
    }
    int BottomY(int column, int offset) const {
        return overlay.bottom + (bottomWrapped && column >= 365 ? bottomRowHeight : 0) +
            bottomRowHeight / 2 + Pixels((offset - 16) * bottomScale);
    }
};

inline ViewportPanelLayout MakeViewportPanelLayout(RECT available, RECT scene,
        bool adaptive, bool model, int scaleCaptionWidth = 36) {
    ViewportPanelLayout p;
    p.overlay = available;
    p.topBand = {available.left, available.top - 25, available.right, available.top};
    p.bottomBand = {available.left, available.bottom, available.right, available.bottom + 32};
    if (!adaptive) return p;
    const int width = std::max(1L, available.right - available.left);
    // 441 units of controls/icons plus room for the current mode/name.
    p.topWrapped = width < 720 && scene.top - available.top >= 25;
    if (p.topWrapped) p.overlay.top += 25;
    p.topBand.bottom = p.overlay.top;
    p.topScale = std::min(1.0f, float(width) / (p.topWrapped ? 441.0f : 720.0f));
    const int bottomWidth = model ? 640 + scaleCaptionWidth + 216 + 5 : 714;
    const int starts[] = {157, 230, 303, 417, 490, 563,
        model ? 640 + scaleCaptionWidth + 17 : 656,
        640 + scaleCaptionWidth + 90, 640 + scaleCaptionWidth + 163};
    std::copy(std::begin(starts), std::end(starts), p.fieldStarts);
    p.fieldCount = model ? 9 : 7;
    const auto fit = [&](bool wrapped) {
        const float reduction = wrapped
            ? std::max((361.0f - width) / 3, float(bottomWidth - 360 - width) / (p.fieldCount - 3))
            : float(bottomWidth - width) / p.fieldCount;
        p.fieldReduction = std::clamp(reduction, 0.0f, 15.0f);
        const float rowWidth = wrapped
            ? std::max(p.BottomColumn(361), p.BottomColumn(bottomWidth) - p.BottomColumn(360))
            : p.BottomColumn(bottomWidth);
        // Layout ultimately uses integer pixels. Fractional field reductions
        // must not trigger a font shrink at an otherwise exact-fit width.
        p.bottomScale = std::min(1.0f, float(width) / std::max(1, ViewportPanelLayout::Pixels(rowWidth)));
        p.bottomRowHeight = std::max(24, ViewportPanelLayout::Pixels(32 * p.bottomScale));
    };
    fit(true);
    p.bottomWrapped = width < bottomWidth - 15 * p.fieldCount &&
        available.bottom - scene.bottom >= 2 * p.bottomRowHeight - 32;
    fit(p.bottomWrapped);
    // Native control borders do not scale with their font. Keep a usable row
    // height, but remove the unused space around compressed two-row panels.
    p.overlay.bottom = p.bottomBand.bottom - (p.bottomWrapped ? 2 : 1) * p.bottomRowHeight;
    p.bottomBand.top = p.overlay.bottom;
    return p;
}
}
