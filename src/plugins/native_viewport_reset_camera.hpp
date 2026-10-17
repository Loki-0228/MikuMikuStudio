#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_viewport_history.hpp"
#include <cstdint>
#include <memory>

namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// An immutable CPU sample of the camera state restored by model-view NUMPAD0.
// Capturing never installs a pose, reconciles live authoring, or evaluates a
// model. Initial publication must check IsCurrent; history replays its owned
// numeric before/after states without re-sampling this source.
class ViewportResetCameraSource final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit ViewportResetCameraSource(std::unique_ptr<Impl>);
    friend MmsResult CaptureViewportResetCamera(MMDApp&,
        std::shared_ptr<ViewportResetCameraSource>&) noexcept;
public:
    ~ViewportResetCameraSource();
    ViewportResetCameraSource(const ViewportResetCameraSource&) = delete;
    ViewportResetCameraSource& operator=(const ViewportResetCameraSource&) = delete;
    bool IsCurrent() const noexcept;
    // Changes only position, rotation, distance, FOV, perspective and binding
    // in the supplied detached state. Offsets, basis and suppression stay put.
    bool Sample(ViewportPresetState& inOut) const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
};
MmsResult CaptureViewportResetCamera(MMDApp&,
    std::shared_ptr<ViewportResetCameraSource>& out) noexcept;
}
#endif
