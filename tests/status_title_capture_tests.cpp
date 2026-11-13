#include "status_title_capture.hpp"
#include <algorithm>
#include <cstdio>

// This checks the capture helper with a synthetic GDI painter only. It does
// not assert any production title, language, playback, save or trace branch.
int main() {
    HDC screen = GetDC(nullptr);
    if (!screen) return 1;
    struct ScreenOwner { HDC dc; ~ScreenOwner() { ReleaseDC(nullptr, dc); } } screenOwner{screen};
    const RECT band{2, 3, 15, 10};
    const auto paint = [](HDC dc) {
        const RECT r{4, 5, 10, 8};
        HBRUSH brush = CreateSolidBrush(RGB(10, 50, 90));
        if (brush) { FillRect(dc, &r, brush); DeleteObject(brush); }
    };
    status_title_capture::Image first, second, changed;
    if (!status_title_capture::Capture(screen, 20, 15, band, paint, first) ||
        !status_title_capture::Capture(screen, 20, 15, band, paint, second) ||
        !status_title_capture::Equal(first, second) || first.rgb.size() != 91) return 2;
    const auto different = [](HDC dc) {
        const RECT r{4, 5, 10, 8};
        HBRUSH brush = CreateSolidBrush(RGB(11, 50, 90));
        if (brush) { FillRect(dc, &r, brush); DeleteObject(brush); }
    };
    if (!status_title_capture::Capture(screen, 20, 15, band, different, changed) ||
        status_title_capture::Equal(first, changed)) return 3;
    const auto before = first;
    if (status_title_capture::Capture(screen, 20, 15, RECT{-1, 0, 2, 2}, paint, first) ||
        !status_title_capture::Equal(first, before)) return 4;
    std::puts("status title capture helper PASS");
    return 0;
}
