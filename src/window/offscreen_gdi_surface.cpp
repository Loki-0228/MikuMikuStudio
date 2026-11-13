#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/offscreen_gdi_surface.hpp"

namespace mikumikustudio {
namespace {
std::uintptr_t CreateDc(void*, std::uintptr_t ref) noexcept {
    return reinterpret_cast<std::uintptr_t>(CreateCompatibleDC(reinterpret_cast<HDC>(ref)));
}
std::uintptr_t CreateBitmap(void*, std::uintptr_t ref, int width, int height, void** pixels) noexcept {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    return reinterpret_cast<std::uintptr_t>(CreateDIBSection(reinterpret_cast<HDC>(ref), &info, DIB_RGB_COLORS, pixels, nullptr, 0));
}
bool SelectBitmap(void*, std::uintptr_t dc, std::uintptr_t bitmap) noexcept {
    auto previous = SelectObject(reinterpret_cast<HDC>(dc), reinterpret_cast<HBITMAP>(bitmap));
    return previous && previous != HGDI_ERROR;
}
bool DeleteBitmap(void*, std::uintptr_t bitmap) noexcept { return DeleteObject(reinterpret_cast<HBITMAP>(bitmap)) != 0; }
bool SelectFont(void*, std::uintptr_t dc, std::uintptr_t font, std::uintptr_t* previous) noexcept {
    const auto old = SelectObject(reinterpret_cast<HDC>(dc), reinterpret_cast<HFONT>(font));
    if (!old || old == HGDI_ERROR) return false;
    *previous = reinterpret_cast<std::uintptr_t>(old); return true;
}
bool RestoreFont(void*, std::uintptr_t dc, std::uintptr_t previous) noexcept {
    const auto old = SelectObject(reinterpret_cast<HDC>(dc), reinterpret_cast<HGDIOBJ>(previous));
    return old && old != HGDI_ERROR;
}
bool DeleteDc(void*, std::uintptr_t dc) noexcept { return DeleteDC(reinterpret_cast<HDC>(dc)) != 0; }
bool DeleteFont(void*, std::uintptr_t font) noexcept { return DeleteObject(reinterpret_cast<HFONT>(font)) != 0; }
}
OffscreenSurfaceOps Win32OffscreenSurfaceOps() noexcept {
    return {nullptr, CreateDc, CreateBitmap, SelectBitmap, DeleteBitmap,
        {nullptr, SelectFont, RestoreFont, DeleteDc, DeleteFont}};
}
}
