#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <cstdint>
#include <memory>

namespace mikumikustudio {
// Registered external-parent selection and its contiguous matching interval.
// Raw model/bone slots are captured data, not live or persistent identities.
// A future scene evaluator must resolve them against the same captured scene.
struct DisplaySelectorSample {
    std::int32_t modelIndex = -1, boneIndex = 0;
    std::uint32_t windowStart = 0, windowEnd = 0; // end 0 means no later change
};
enum class DisplayMotionStatus { Ok, InvalidIndex, InvalidTime };
class DisplayMotionSnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    DisplayMotionSnapshot();
public:
    ~DisplayMotionSnapshot();
    DisplayMotionSnapshot(const DisplayMotionSnapshot&) = delete;
    DisplayMotionSnapshot& operator=(const DisplayMotionSnapshot&) = delete;
    // UI-safe-point capture owns reachable display keys and nested IK/parent
    // payloads plus IK-bone and selector-bone mappings. Selector 0 is the global
    // model parent (bone -1). Current edited flags/windows are never sampled.
    // Invalid structure throws invalid_argument; retained budget exhaustion
    // throws length_error; allocation failure throws bad_alloc. Source unchanged.
    // Budget covers retained capacities, not temporary capture peak.
    static std::unique_ptr<DisplayMotionSnapshot> Capture(const unsigned char* model,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024);
    std::uint32_t KeyCount() const noexcept;
    bool KeyFrame(std::uint32_t index, std::uint32_t& out) const noexcept;
    std::uint32_t IkCount() const noexcept;
    std::uint32_t SelectorCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    bool IkBone(std::uint32_t index, std::int32_t& out) const noexcept;
    bool SelectorBone(std::uint32_t index, std::int32_t& out) const noexcept;
    // Concurrent allocation-free reads after source destruction. Nonnegative
    // finite frame <= frame_limits::maximum; fractional frames use preceding
    // key (no interpolation). Failure preserves out. Bytes are not normalized
    // to booleans. Does not resolve parent identities, apply IK, or evaluate pose.
    DisplayMotionStatus SampleVisibility(double frame, std::uint8_t& out) const noexcept;
    DisplayMotionStatus SampleIk(int index, double frame, std::uint8_t& out) const noexcept;
    DisplayMotionStatus SampleSelector(int index, double frame, DisplaySelectorSample& out) const noexcept;
};
}
#endif
