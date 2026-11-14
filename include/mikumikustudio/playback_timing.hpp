#pragma once

#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/physics_bake_settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mikumikustudio::playback_timing {

inline constexpr int kTimelineFps = 30;
inline constexpr int kMinPlaybackFps = 1;
inline constexpr int kMaxPlaybackFps = 240;

struct Settings {
    int playbackFps = kTimelineFps;
    bake::Settings physics;
    bool followPlaybackFps = false;
    bool interpolate = false;
    bool displayFollowsPlaybackFps = false;
};

inline bool Valid(const Settings& settings) {
    const auto& physics = settings.physics;
    return settings.playbackFps >= kMinPlaybackFps && settings.playbackFps <= kMaxPlaybackFps &&
        physics.baseRateHz >= 1 && physics.baseRateHz <= 1000 &&
        physics.substeps >= 1 && physics.substeps <= 16 &&
        physics.rateMultiplier >= 1 && physics.rateMultiplier <= 16 &&
        std::isfinite(physics.erp) && physics.erp >= 0 && physics.erp <= 1 &&
        std::isfinite(physics.cfm) && physics.cfm >= 0 && physics.cfm <= 1000000;
}

inline double Speed(const Settings& settings) noexcept {
    return double(settings.playbackFps) / kTimelineFps;
}

inline double TimelineFrame(double seconds) noexcept {
    const double frame = seconds * kTimelineFps;
    const double whole = std::round(frame);
    // A frame-to-seconds round trip can place an exact key just past its
    // boundary. Preserve fractional samples outside floating-point error.
    const double tolerance = std::numeric_limits<double>::epsilon() * std::abs(frame);
    return std::abs(frame - whole) <= tolerance ? whole : frame;
}

inline int PhysicsRate(const Settings& settings) noexcept {
    const int baseRate = settings.followPlaybackFps ? settings.playbackFps : settings.physics.baseRateHz;
    return baseRate * settings.physics.rateMultiplier * settings.physics.substeps;
}

// Frames at or before a reset do not prove the legacy working pose was
// captured in the current physics run. Future frames have not been sampled.
struct VisualBridgeWindow {
    double after = 0;
    double through = 0;

    bool Contains(std::uint32_t frame) const noexcept {
        return frame > after && frame <= through;
    }
};

struct PhysicsClock {
    double origin = 0;
    std::uint64_t step = 0;
    std::uint32_t windSeed = 1;
    bool initialPosePending = true;

    void Reset(double canonicalSeconds) noexcept {
        *this = {};
        origin = canonicalSeconds;
#if !MMD_ORIGINAL_PROFILE
        InvalidateVisualBridge(canonicalSeconds);
#endif
    }

    double Next(int rate) const noexcept { return origin + double(step + 1) / rate; }

    bool Due(double target, int rate) const noexcept {
        const double tolerance = 2 * std::numeric_limits<double>::epsilon() * (std::max)(1.0, std::abs(target));
        return Next(rate) <= target + tolerance;
    }

#if !MMD_ORIGINAL_PROFILE
    void InvalidateVisualBridge(double resetSeconds) noexcept {
        lastBridgeResetSeconds_ = resetSeconds;
    }

    VisualBridgeWindow VisualBridgeSamples(int rate, bool resetPending) const noexcept {
        if (initialPosePending || resetPending || step == 0) return {};
        return {TimelineFrame(lastBridgeResetSeconds_), TimelineFrame(origin + double(step) / rate)};
    }

#endif

    int NextWindRandom() noexcept {
        windSeed = windSeed * 214013u + 2531011u;
        return (windSeed >> 16) & 32767u;
    }

#if !MMD_ORIGINAL_PROFILE
private:
    double lastBridgeResetSeconds_ = 0;
#endif
};

}  // namespace mikumikustudio::playback_timing
