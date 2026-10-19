#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <array>
#include <cstdint>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
struct CameraInterpolationOptions {
    // Bits 0..5: X, Y, Z, rotation, distance, field of view.
    std::uint8_t channels = 0;
    // Per-channel x1, y1, x2, y2. Every enabled value must be in 0..127.
    std::array<std::array<std::uint8_t, 4>, 6> values{};
};

// Prepare an owned edit of selected existing camera keys, including frame 0.
// No key insertion, selection changes, evaluation, UI, or legacy undo writes.
// Every history direction preserves the current unregistered camera pose and
// playhead. A successful no-op leaves out empty. The app outlives its history.
MmsResult PlanCameraInterpolationEdit(MMDApp&, const CameraInterpolationOptions&,
    std::shared_ptr<HistoryChange>& out) noexcept;
}
#endif
