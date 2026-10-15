#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <vector>

namespace native_ui {
struct WindowMove { HWND window; RECT bounds; };
enum class ApplyResult { Applied, Restored, Failed };
// Must run on the owner thread. Takes complete candidate geometry; failure
// attempts to restore every old rectangle and verifies the actual result.
ApplyResult ApplyWindows(const std::vector<WindowMove>&);
class PanelBuffer {
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    int width_ = 0, height_ = 0;
    uint64_t key_ = 0;
public:
    PanelBuffer() = default;
    ~PanelBuffer();
    PanelBuffer(const PanelBuffer&) = delete;
    PanelBuffer& operator=(const PanelBuffer&) = delete;
    void Reset() noexcept;
    // A new/changed buffer is fully initialized with the background color.
    // The caller paints dirty content; Copy only transfers that region.
    HDC Begin(HDC target, int width, int height, uint64_t dpiThemeFontKey, COLORREF background);
    bool Copy(HDC target, const RECT& damage) const;
    HBITMAP Bitmap() const { return bitmap_; }
};
}
