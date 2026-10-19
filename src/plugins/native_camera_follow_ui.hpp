#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <memory>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Preserve every pending native edit, including an edit which has already
// lost focus. Capture/allocation must complete before the caller publishes.
// Ordinary readouts use per-edit modify checks without constructing a scope.
// Capture/restore and the readout helpers gate native input without disabling
// a window or changing focus. Keeping a saved draft does not authorize edits.
class ScopedFollowReadoutDrafts final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit ScopedFollowReadoutDrafts(MMDApp&);
    ~ScopedFollowReadoutDrafts();
    ScopedFollowReadoutDrafts(const ScopedFollowReadoutDrafts&) = delete;
    ScopedFollowReadoutDrafts& operator=(const ScopedFollowReadoutDrafts&) = delete;
};
// Derived controls only. Does not read keys, evaluate authoring, seek, or
// invoke RefreshBoundGlobalReadout. Safe as a standalone readout refresh.
void RefreshBoundCameraFollowReadout(MMDApp&) noexcept;
// The checkbox has already moved when its notification arrives. Refusal must
// restore these three flags even when no C++ allocation is available.
void RefreshBoundCameraFollowFlags(MMDApp&) noexcept;
}
#endif
