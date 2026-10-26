#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/display_motion_snapshot.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace mikumikustudio {
class BoneTransformContext;

// Explicit, immutable attachment input for one active captured selector.
// modelIndex/boneIndex must match that selector at the requested time. These
// are captured slots, not persistent identities. The future scene coordinator
// resolves stable identities and supplies each phase's target matExtra.
struct BoneEvaluationDependency {
    std::uint32_t selector = 0;
    std::int32_t modelIndex = -1, boneIndex = -1;
    std::array<float, 3> restPosition{};
    std::array<bone_scale::Transform, 2> attachment{};
};
struct BoneEvaluationPose {
    bone_scale::Transform rigid{}, scaled{};
    // Native matExtra after each complete layer sweep, before display scale.
    std::array<bone_scale::Transform, 2> attachment{};
    std::array<float, 3> jointPosition{}; // rest position transformed by scaled
};

class BoneEvaluationSource;
class BoneEvaluationFrame final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BoneEvaluationFrame();
    friend class BoneEvaluationSource;
public:
    ~BoneEvaluationFrame();
    BoneEvaluationFrame(const BoneEvaluationFrame&) = delete;
    BoneEvaluationFrame& operator=(const BoneEvaluationFrame&) = delete;
    std::uint32_t BoneCount() const noexcept;
    double Frame() const noexcept;
    std::uint8_t Visibility() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Concurrent allocation-free reads; invalid index leaves out unchanged.
    // No source, model, context or dependency needs to outlive the result.
    bool BoneAt(std::uint32_t bone, BoneEvaluationPose& out) const noexcept;
};

class BoneEvaluationSource final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BoneEvaluationSource();
public:
    ~BoneEvaluationSource();
    BoneEvaluationSource(const BoneEvaluationSource&) = delete;
    BoneEvaluationSource& operator=(const BoneEvaluationSource&) = delete;
    // Capture all five definitions/motion components at one caller-serialized
    // UI safe point. Owns all data, never copies current edited pose matrices.
    // Budget aggregates the components' retained capacities, not peak heap use.
    static std::unique_ptr<BoneEvaluationSource> Capture(const unsigned char* model,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024);
    std::uint32_t BoneCount() const noexcept;
    std::uint32_t SelectorCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    DisplayMotionStatus SampleSelector(int selector, double frame,
                                       DisplaySelectorSample& out) const noexcept;
    // Fresh deterministic loader basis (PMX/PMD), registered local channels,
    // bone morphs, native two sweeps/IK and complete scale. Physics simulation
    // is disabled (mode 0); this is not a public scene stage or mesh frame.
    // Each active selector needs exactly one matching dependency, including
    // global selector 0. Extra, duplicate, missing or nonfinite inputs fail.
    // Selectors sharing a raw target must agree on its rest/phase transforms.
    // Input ownership/stability covers this call; sources may be shared across
    // callers with independent outputs. No global scale/math/scene state used.
    // Working budget accounts owned arrays, map storage and morph results;
    // nested solver/scale/morph transient allocations are not a peak-heap cap.
    // Invalid inputs: invalid_argument; nonfinite results: overflow_error;
    // budgets: length_error; allocations: bad_alloc. Failure publishes no frame.
    std::unique_ptr<BoneEvaluationFrame> Evaluate(double frame,
        const BoneTransformContext& context,
        const BoneEvaluationDependency* dependencies = nullptr,
        std::uint32_t dependencyCount = 0,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024,
        std::uint64_t maxWorkspaceBytes = 128ull * 1024 * 1024) const;
};
}
#endif
