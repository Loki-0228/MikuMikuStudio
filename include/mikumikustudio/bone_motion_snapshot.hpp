#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/bone_scale.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace mikumikustudio {
// Registered local motion before hierarchy, IK, morphs, layers and physics.
// This is not an evaluated model pose or an MMS_STAGE_ANIMATED scene frame.
struct BoneMotionSample {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    bone_scale::Matrix scale;
    bool physicsDisabled = false;
};
enum class BoneMotionStatus { Ok, InvalidBone, InvalidTime, UnsupportedBone, ExternalParent, InvalidSample };
class BoneMotionSnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BoneMotionSnapshot();
public:
    ~BoneMotionSnapshot();
    BoneMotionSnapshot(const BoneMotionSnapshot&) = delete;
    BoneMotionSnapshot& operator=(const BoneMotionSnapshot&) = delete;
    // The caller serializes Capture with model edits at a UI safe point.
    // Copies only reachable keys, scales and reconciled free curves; never
    // current unregistered poses or undo rings. No source address is retained.
    // Invalid data throws invalid_argument, budget exhaustion length_error,
    // allocation failure bad_alloc. No live state is changed on any path.
    // The budget limits retained storage, not peak temporary allocation.
    static std::unique_ptr<BoneMotionSnapshot> Capture(const unsigned char* model,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024);
    std::uint32_t BoneCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Concurrent const reads remain valid after the original model is edited
    // or destroyed. No allocation, live lookup, evaluation cache or writeback.
    // Negative/nonfinite/out-of-range time and unsupported selector-dependent
    // or derived bone types are refused, leaving out untouched. Integer frames
    // retain native direct-seek arithmetic; fractional frames are continuous.
    BoneMotionStatus SampleChannels(int bone, double frame, BoneMotionSample& out) const noexcept;
};
}
#endif
