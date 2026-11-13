#pragma once

#include <algorithm>

#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ui_scale.hpp"

namespace mikumikustudio {

inline constexpr int kTimelineHeightGrip = 9;
inline constexpr int kBottomHeightGrip = 10;
inline constexpr int kTimelineRulerTop = 95;
inline constexpr int kTimelineGridTop = 145;
inline constexpr int kBottomPanelHeight = 160;

// Geometry authored against the 160-pixel bottom-panel template. Keep the
// outer padding fixed and distribute the added height between template rows.
struct BottomPanelTemplate {
    int clientBottom;
    int extraHeight;

    int Y(int templateY) const {
        const int fromBottom = clientBottom - templateY;
        return clientBottom - ui_scale::Pixels(fromBottom) -
            MulDiv(fromBottom - 3, extraHeight, kBottomPanelHeight - 6);
    }

    int ContentY(int templateY, bool playback = false) const {
        if (extraHeight == 0) return Y(templateY);
        const int anchor = clientBottom - (playback ? 79 : 157);
        const int distance = templateY - anchor;
        const int extra = std::min(playback ? MulDiv(extraHeight, 76, 154) : extraHeight, ui_scale::Pixels(24));
        const int top = playback ? ContentY(anchor) : Y(anchor);
        return top + ui_scale::Pixels(distance) +
            MulDiv(distance, extra, playback ? 76 : 154);
    }

    bool StackNameSelectors() const { return extraHeight >= ui_scale::Pixels(60); }

    RECT CurveRect() const {
        return {8, ContentY(clientBottom - 135), 136, Y(clientBottom - 7)};
    }
};

struct MainPanelVerticalLayout {
    int timelineOffset;
    int bottomExtra;
    int heightBudget;
    int timelineBottom;
    int bottomTop;
    int timelinePercent;

    int Pixels(int design) const { return MulDiv(design, timelinePercent, 100); }
    int RulerTop() const { return GridTop() - Pixels(kTimelineGridTop - kTimelineRulerTop); }
    int GridTop() const { return Pixels(kTimelineGridTop) + timelineOffset; }
    int FooterY(int offset) const { return bottomTop - Pixels(offset - kBottomPanelHeight); }

    // Ruler, grid and key icons share the 100% bitmap coordinates.
    // Invert rounded edges so neighbouring hit cells share a boundary.
    int DesignPixels(int pixels) const {
        if (timelinePercent == 100) return pixels;
        const int nearest = MulDiv(pixels, 100, timelinePercent);
        return Pixels(nearest) > pixels ? nearest - 1 : nearest;
    }
    int DesignX(int clientX) const { return 6 + DesignPixels(clientX - Pixels(6)); }
    int DesignY(int clientY) const { return kTimelineGridTop + DesignPixels(clientY - GridTop()); }
    int FrameColumns(int sidebarWidth) const { return std::clamp((DesignPixels(sidebarWidth) - 84) / 13, 0, 200); }
};

inline MainPanelVerticalLayout MainPanelLayout(MMDApp* app, int clientBottom) {
    // Leave room for the wrapped footer and four visible rows on a short
    // window. Width-only dragging does not change this vertical budget.
    constexpr int sidebarMinimumHeight = 250 + panel::kTimelineWrapShift + 4 * 14;
    const int bottomHeight = ui_scale::Pixels(kBottomPanelHeight);
    const int percent = std::clamp((clientBottom - bottomHeight) * 100 / sidebarMinimumHeight,
                                  100, ui_scale::Percent());
    MainPanelVerticalLayout layout{};
    layout.timelinePercent = percent;
    const int wrap = panel::TimelinePanelShift(layout.DesignPixels(app->SidebarWidth()));
    const int budget = std::max(0, clientBottom - bottomHeight - layout.Pixels(sidebarMinimumHeight));
    const auto& extra = app->bottomPanelSizing.extra;
    const int timeline = build_profile::original ? 0 : std::clamp(extra[kTimelineHeightGrip], 0, budget);
    const int dragHeight = build_profile::original ? 0 : std::clamp(extra[kBottomHeightGrip], 0, budget - timeline);
    layout.timelineOffset = timeline;
    layout.bottomExtra = dragHeight + bottomHeight - kBottomPanelHeight;
    layout.heightBudget = budget;
    layout.bottomTop = clientBottom - bottomHeight - dragHeight;
    layout.timelineBottom = layout.FooterY(248 + wrap);
    return layout;
}

inline BottomPanelTemplate BottomTemplate(MMDApp* app, int clientBottom) {
    return {clientBottom, MainPanelLayout(app, clientBottom).bottomExtra -
        (ui_scale::Pixels(kBottomPanelHeight) - kBottomPanelHeight)};
}

inline RECT TimelineRulerDamage(MMDApp* app) {
    RECT client{};
    GetClientRect(app->MainWindow(), &client);
    const auto layout = MainPanelLayout(app, client.bottom);
    const int top = layout.RulerTop();
    return {layout.Pixels(6), top, app->SidebarWidth() - layout.Pixels(3), top + layout.Pixels(51)};
}

inline int WheelClientX(HWND owner, LPARAM position, int lastClientX) {
    if (!owner || position == 0) return lastClientX;
    POINT point{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
    return ScreenToClient(owner, &point) ? point.x : lastClientX;
}

}  // namespace mikumikustudio
