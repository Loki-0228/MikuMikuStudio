#pragma once
#include <algorithm>
#include <vector>

namespace mikumikustudio::curve_editor {
// Query in nondecreasing frame order. Native callers already provide sorted
// frames; normalize callbacks from other hosts without modifying their input.
class FrameSelectionCursor {
public:
    explicit FrameSelectionCursor(const std::vector<int>& frames) : frames_(&frames) {
        if (!std::is_sorted(frames.begin(), frames.end())) {
            sorted_ = frames;
            std::sort(sorted_.begin(), sorted_.end());
            frames_ = &sorted_;
        }
    }
    FrameSelectionCursor(const FrameSelectionCursor&) = delete;
    FrameSelectionCursor& operator=(const FrameSelectionCursor&) = delete;
    bool Contains(int frame) {
        while (next_ < frames_->size() && (*frames_)[next_] < frame) ++next_;
        return next_ < frames_->size() && (*frames_)[next_] == frame;
    }
private:
    std::vector<int> sorted_;
    const std::vector<int>* frames_;
    std::size_t next_ = 0;
};
}
