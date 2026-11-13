#pragma once

#include <Windows.h>
#include <optional>

namespace mikumikustudio {
class MMDApp;

namespace ui {

// Resolve the child and its template coordinates together. Drawing uses the
// child's client DC and leaves its origin and clipping unchanged on return.
class BottomPanelControlSurface {
public:
    static std::optional<BottomPanelControlSurface> Find(MMDApp&, HWND child);
    bool PaintBackground(HDC childDC) const;
    const RECT& Client() const noexcept { return client_; }
    bool FitsHeight() const noexcept { return client_.bottom <= host_.bottom - origin_.y; }
    bool Expanded() const noexcept { return expanded_; }

private:
    explicit BottomPanelControlSurface(MMDApp& app) : app_(app) {}
    MMDApp& app_;
    RECT host_{}, client_{};
    POINT origin_{};
    bool expanded_ = false;
};

// Attach a native main-window trackbar on its UI thread. The standard panel
// creation path calls this automatically. Repeating the call is safe.
// True means the template is attached. Otherwise the child keeps native paint.
bool AttachBottomTrackbar(MMDApp&, HWND child);

}  // namespace ui
}  // namespace mikumikustudio
