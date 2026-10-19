#pragma once

namespace mikumikustudio::ui {
// A sidebar resize repaints the complete host only after both the panel
// controls and viewport controls have reached their final positions.
inline void MoveLayoutControl(HWND parent, int id, int x, int y, int width, int height,
                              bool deferPaint) {
    HWND control = GetDlgItem(parent, id);
    if (!control) return;
    if (deferPaint) {
        SetWindowPos(control, nullptr, x, y, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    } else {
        MoveWindow(control, x, y, width, height, TRUE);
    }
}
}
