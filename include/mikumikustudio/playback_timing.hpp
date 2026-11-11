#pragma once
#include "mikumikustudio/physics_bake_settings.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace mikumikustudio::playback_timing {
struct Settings {
    int playbackFps = 30;
    bake::Settings physics;
    bool followPlaybackFps = false;
    bool interpolate = false;
    bool displayFollowsPlaybackFps = false;
};

inline bool Valid(const Settings& settings) {
    const auto& physics = settings.physics;
    return settings.playbackFps >= 1 && settings.playbackFps <= 240 &&
        physics.baseRateHz >= 1 && physics.baseRateHz <= 1000 &&
        physics.substeps >= 1 && physics.substeps <= 16 &&
        physics.rateMultiplier >= 1 && physics.rateMultiplier <= 16 &&
        std::isfinite(physics.erp) && physics.erp >= 0 && physics.erp <= 1 &&
        std::isfinite(physics.cfm) && physics.cfm >= 0 && physics.cfm <= 1000000;
}
inline double Speed(const Settings& settings) { return settings.playbackFps / 30.0; }
inline double TimelineFrame(double seconds) {
    const double frame = seconds * 30.0;
    const double whole = std::round(frame);
    // A frame-to-seconds round trip can place an exact key just past its
    // boundary. Preserve fractional samples outside floating-point error.
    const double tolerance = std::numeric_limits<double>::epsilon() * std::abs(frame);
    return std::abs(frame - whole) <= tolerance ? whole : frame;
}
inline double PhysicsRate(const Settings& settings) {
    return double(settings.followPlaybackFps ? settings.playbackFps : settings.physics.baseRateHz) *
        settings.physics.rateMultiplier * settings.physics.substeps;
}

// Simulation timestamps are anchored to the playback start, independent of
// display samples. Integer ticks avoid drift when the output rate changes.
struct Clock {
    double origin = 0;
    std::uint64_t ticks = 0;
    bool active = false;
    bool stepping = false;
    std::uint32_t noise = 1;
    void Reset(double seconds) { origin = seconds; ticks = 0; active = true; stepping = false; noise = 1; }
    double Next(double rate) const { return origin + double(ticks + 1) / rate; }
    bool Due(double target, double rate) const {
        const double tolerance = 2 * std::numeric_limits<double>::epsilon() *
            (std::max)(1.0, std::abs(target));
        return Next(rate) <= target + tolerance;
    }
    int Random() { noise = noise * 214013u + 2531011u; return int((noise >> 16) & 32767u); }
};
}
