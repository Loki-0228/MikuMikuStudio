#pragma once

// Off-screen capture helper for status-title painter comparisons. The caller
// creates the app fixture, supplies the real title-band rectangle, sets each
// desired state and passes the production painter as `paint`. This helper does
// not create a window, change the app state, or write a baseline to disk.
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace status_title_capture {
struct Image {
    RECT band{};
    std::vector<std::uint32_t> rgb;
};

// The surface covers the entire client area so paint receives normal client
// coordinates. RGB is retained; the undefined GDI alpha byte is ignored.
// Failure leaves `out` unchanged. Callers must use a dedicated, hidden app
// fixture and run on the required Windows desktop.
template<class Painter>
bool Capture(HDC reference, int width, int height, RECT band, Painter&& paint, Image& out) {
    if (!reference || width <= 0 || height <= 0 || band.left < 0 || band.top < 0 ||
        band.right <= band.left || band.bottom <= band.top || band.right > width || band.bottom > height)
        return false;
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t) || pixels > 64000000)
        return false;
    HDC dc = CreateCompatibleDC(reference);
    if (!dc) return false;
    struct DcOwner { HDC dc; ~DcOwner() { DeleteDC(dc); } } dcOwner{dc};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* raw = nullptr;
    HBITMAP bitmap = CreateDIBSection(reference, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!bitmap || !raw) { if (bitmap) DeleteObject(bitmap); return false; }
    struct BitmapOwner { HBITMAP bitmap; ~BitmapOwner() { DeleteObject(bitmap); } } bitmapOwner{bitmap};
    HGDIOBJ previous = SelectObject(dc, bitmap);
    if (!previous || previous == HGDI_ERROR) return false;
    struct SelectionOwner { HDC dc; HGDIOBJ previous; ~SelectionOwner() { SelectObject(dc, previous); } } selection{dc, previous};
    auto* data = static_cast<std::uint32_t*>(raw);
    std::fill_n(data, static_cast<std::size_t>(pixels), 0u);
    std::forward<Painter>(paint)(dc);
    if (!GdiFlush()) return false;
    Image candidate{};
    candidate.band = band;
    const auto bandWidth = static_cast<std::size_t>(band.right - band.left);
    const auto bandHeight = static_cast<std::size_t>(band.bottom - band.top);
    candidate.rgb.reserve(bandWidth * bandHeight);
    for (LONG y = band.top; y < band.bottom; ++y)
        for (LONG x = band.left; x < band.right; ++x)
            candidate.rgb.push_back(data[static_cast<std::size_t>(y) * width + x] & 0x00ffffffu);
    out = std::move(candidate);
    return true;
}

inline bool Equal(const Image& a, const Image& b) noexcept {
    return a.band.left == b.band.left && a.band.top == b.band.top &&
        a.band.right == b.band.right && a.band.bottom == b.band.bottom && a.rgb == b.rgb;
}
}
