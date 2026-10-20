#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <array>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
struct CameraMultiplyOptions {
    // Native dialog pairs: eye XYZ, rotation XYZ, distance, integer FOV.
    // Rotation offsets are degrees; the native X offset has a negative sign.
    std::array<float, 8> scale{1, 1, 1, 1, 1, 1, 1, 1};
    std::array<float, 8> offset{};
};

// Begin before opening menu 242's modal. Read only prepares private values;
// Take is consumed after closing it with one ExplicitCommit. Destruction is
// cancellation, and an unchanged result leaves the output empty. No file IO,
// renderer, seek, legacy undo ring, dirty/UI writes or scene-wide evaluation.
// Clean camera pose follows the changed track at its existing playhead;
// unregistered pose, bindings, track cursors and selection remain untouched.
class NativeCameraMultiplyPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    MmsResult Begin(MMDApp&) noexcept;
    MmsResult Read(const CameraMultiplyOptions&) noexcept;
    MmsResult ReadDialog(HWND) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
}
#endif
