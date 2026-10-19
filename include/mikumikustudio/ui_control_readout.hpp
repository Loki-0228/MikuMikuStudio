#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <algorithm>
#include <cstring>

namespace mikumikustudio::ui {
// Echoing the same value must not erase/repaint a native control. Keep the
// native paint, capture, keyboard, accessibility and theme implementation.
inline bool SetTrackbarPositionIfChanged(HWND slider, int requested) noexcept {
    if (!slider || !IsWindow(slider)) return false;
    const int minimum = static_cast<int>(SendMessageW(slider, TBM_GETRANGEMIN, 0, 0));
    const int maximum = static_cast<int>(SendMessageW(slider, TBM_GETRANGEMAX, 0, 0));
    const int target = minimum <= maximum ? (std::clamp)(requested, minimum, maximum) : requested;
    if (SendMessageW(slider, TBM_GETPOS, 0, 0) == target) return false;
    SendMessageW(slider, TBM_SETPOS, TRUE, target);
    return true;
}
inline bool ReadoutTextMatchesA(HWND edit, const char* text) noexcept {
    if (!edit || !IsWindow(edit) || !text) return false;
    const auto length = std::strlen(text);
    if (length >= 256 || GetWindowTextLengthA(edit) != static_cast<int>(length)) return false;
    char previous[256]{};
    return GetWindowTextA(edit, previous, sizeof(previous)) == static_cast<int>(length) &&
        std::strcmp(previous, text) == 0;
}
inline bool SetReadoutTextIfChangedA(HWND edit, const char* text) noexcept {
    if (!edit || !IsWindow(edit) || !text || ReadoutTextMatchesA(edit, text)) return false;
    return SetWindowTextA(edit, text) != FALSE;
}
}
