#pragma once
#include <Windows.h>
#include <algorithm>
#include <cmath>

namespace mikumikustudio::recording_preview {
constexpr DWORD kStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU |
    WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;

inline RECT WindowRect(HWND owner, int outputWidth, int outputHeight, bool fit) {
    MONITORINFO monitor{sizeof(monitor)};
    RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
        work = monitor.rcWork;
    RECT frame{};
    AdjustWindowRectEx(&frame, kStyle, FALSE, 0);
    const int borderW = frame.right - frame.left, borderH = frame.bottom - frame.top;
    const int w = (std::max)(1, outputWidth), h = (std::max)(1, outputHeight);
    const double scale = fit ? (std::min)(
        double((std::max)(1L, work.right - work.left - borderW - 32)) / w,
        double((std::max)(1L, work.bottom - work.top - borderH - 48)) / h) : 1.0;
    const int width = (std::max)(1, int(w * scale)) + borderW;
    const int height = (std::max)(1, int(h * scale)) + borderH;
    // Keep the title bar reachable even for exports larger than the monitor.
    const int x = work.left + (std::max)(0L, (work.right - work.left - width) / 2);
    const int y = work.top + (std::max)(0L, (work.bottom - work.top - height) / 2);
    return {x, y, x + width, y + height};
}

inline void SizeToAspect(RECT& rect, WPARAM edge, int outputWidth, int outputHeight) {
    if (outputWidth <= 0 || outputHeight <= 0) return;
    RECT frame{};
    AdjustWindowRectEx(&frame, kStyle, FALSE, 0);
    const int borderW = frame.right - frame.left, borderH = frame.bottom - frame.top;
    const double aspect = double(outputWidth) / outputHeight;
    const bool vertical = edge == WMSZ_TOP || edge == WMSZ_BOTTOM;
    int w = (std::max)(1L, rect.right - rect.left - borderW);
    int h = (std::max)(1L, rect.bottom - rect.top - borderH);
    const double minScale = (std::max)(160.0 / outputWidth, 90.0 / outputHeight);
    if (vertical) { h = (std::max)(h, int(std::ceil(outputHeight * minScale))); w = int(std::lround(h * aspect)); }
    else { w = (std::max)(w, int(std::ceil(outputWidth * minScale))); h = int(std::lround(w / aspect)); }
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
        rect.left = rect.right - w - borderW;
    else rect.right = rect.left + w + borderW;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
        rect.top = rect.bottom - h - borderH;
    else rect.bottom = rect.top + h + borderH;
}
}
