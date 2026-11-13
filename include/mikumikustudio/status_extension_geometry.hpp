#pragma once

#include "mikumikustudio/viewport_panel_layout.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace mikumikustudio {

// Input rectangles and output use the same client-coordinate units as
// ViewportPanelLayout. The caller must measure all title ink, including trace
// and saving text, and provide every native control/icon touching the title
// row. A false readiness flag keeps the extension hidden until that is done.
struct StatusExtensionGeometryInput {
    RECT titleInk{};
    const RECT* nativeObstacles = nullptr;
    std::size_t obstacleCount = 0;
    int gap = 0;
    int minimumWidth = 0;
    int minimumHeight = 0;
    bool titleMeasured = false;
    bool obstaclesComplete = false;
};

// Chooses the widest free interval to the right of the measured title ink,
// limited to the first row and the original title clipping region. It never
// changes the viewport, title clip or native geometry. With equal widths the
// rightmost interval wins. Empty geometry is returned on incomplete input,
// insufficient space or invalid rectangles.
inline RECT FindStatusExtensionSlot(const ViewportPanelLayout& layout,
                                    const StatusExtensionGeometryInput& input) noexcept {
    RECT empty{};
    if (!input.titleMeasured || !input.obstaclesComplete || input.gap < 0 ||
        input.minimumWidth <= 0 || input.minimumHeight <= 0 ||
        (input.obstacleCount && !input.nativeObstacles) ||
        input.titleInk.right < input.titleInk.left || input.titleInk.bottom < input.titleInk.top)
        return empty;
    using Wide = std::int64_t;
    const Wide rowLeft = layout.topBand.left;
    const Wide rowTop = layout.topBand.top;
    const Wide rowRight = (std::min)(static_cast<Wide>(layout.topBand.right),
                                     static_cast<Wide>(layout.TitleRight()));
    const Wide rowBottom = (std::min)(static_cast<Wide>(layout.topBand.bottom), static_cast<Wide>(layout.TitleBottom()));
    if (rowRight <= rowLeft || rowBottom <= rowTop || rowBottom - rowTop < input.minimumHeight)
        return empty;

    const Wide titleRight = (std::max)(rowLeft, (std::min)(rowRight, static_cast<Wide>(input.titleInk.right)));
    Wide cursor = (std::min)(rowRight, titleRight + static_cast<Wide>(input.gap));
    Wide bestLeft = cursor, bestRight = cursor;
    while (cursor < rowRight) {
        Wide nextLeft = rowRight, nextRight = rowRight;
        for (std::size_t i = 0; i < input.obstacleCount; ++i) {
            const RECT& obstacle = input.nativeObstacles[i];
            if (obstacle.right < obstacle.left || obstacle.bottom < obstacle.top) return empty;
            // Empty obstacles carry no occupied area.
            if (obstacle.right == obstacle.left || obstacle.bottom == obstacle.top) continue;
            const Wide gap = input.gap;
            if (static_cast<Wide>(obstacle.bottom) + gap <= rowTop ||
                static_cast<Wide>(obstacle.top) - gap >= rowBottom) continue;
            const Wide left = (std::max)(rowLeft, static_cast<Wide>(obstacle.left) - gap);
            const Wide right = (std::min)(rowRight, static_cast<Wide>(obstacle.right) + gap);
            if (right <= cursor || left >= rowRight) continue;
            if (left < nextLeft || (left == nextLeft && right > nextRight)) {
                nextLeft = left;
                nextRight = right;
            }
        }
        const Wide end = (std::max)(cursor, nextLeft);
        if (end - cursor >= bestRight - bestLeft) { bestLeft = cursor; bestRight = end; }
        if (nextLeft == rowRight) break;
        // nextRight is strictly greater than cursor for every selected obstacle.
        cursor = nextRight;
    }
    if (bestRight - bestLeft < input.minimumWidth) return empty;
    return RECT{static_cast<LONG>(bestLeft), static_cast<LONG>(rowTop),
                static_cast<LONG>(bestRight), static_cast<LONG>(rowBottom)};
}
} // namespace mikumikustudio
