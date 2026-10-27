#pragma once
#include "mikumikustudio/bone_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE

namespace mikumikustudio {
// Copy of the host object identity (MmsId lo/hi and generation), not a new
// identity registry. The trusted host supplies identities at its UI safe point.
struct BoneSceneIdentity {
    std::uint64_t lo = 0, hi = 0, generation = 0;
};
struct BoneSceneModelInput {
    std::uint32_t slot = 0;
    BoneSceneIdentity identity{};
    const unsigned char* model = nullptr;
};
struct BoneSceneModelInfo {
    std::uint32_t slot = 0, calculationOrder = 0, boneCount = 0;
    BoneSceneIdentity identity{};
};
class BoneSceneSource;
class BoneSceneFrame final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BoneSceneFrame();
    friend class BoneSceneSource;
public:
    ~BoneSceneFrame();
    BoneSceneFrame(const BoneSceneFrame&) = delete;
    BoneSceneFrame& operator=(const BoneSceneFrame&) = delete;
    std::uint32_t ModelCount() const noexcept;
    double Frame() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Enumeration preserves capture input order, not calculation order.
    // Invalid index leaves out unchanged. All reads are allocation-free.
    bool ModelAt(std::uint32_t index, BoneSceneModelInfo& out) const noexcept;
    // Borrowed immutable child, valid for this frame's lifetime. Null if invalid.
    const BoneEvaluationFrame* ModelFrame(std::uint32_t index) const noexcept;
};
class BoneSceneSource final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    BoneSceneSource();
public:
    ~BoneSceneSource();
    BoneSceneSource(const BoneSceneSource&) = delete;
    BoneSceneSource& operator=(const BoneSceneSource&) = delete;
    // Capture the complete requested model set at one caller-serialized UI safe
    // point. Slots, model pointers and object IDs must be unique; identities
    // need a nonzero ID and generation. Every reachable parent key (including
    // future keys) must name a captured slot and an existing target bone.
    // Native -1 (inherited parent) and -2 (ground) stay distinct and need no
    // target model. Other negative target slots are rejected.
    // Captures all target motion as well as identities; retains no live pointer.
    // Empty input is valid. Inputs may be destroyed after successful capture.
    static std::unique_ptr<BoneSceneSource> Capture(const BoneSceneModelInput* models,
        std::uint32_t count, std::uint64_t maxRetainedBytes = 256ull * 1024 * 1024);
    std::uint32_t ModelCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    bool ModelAt(std::uint32_t index, BoneSceneModelInfo& out) const noexcept;
    // Fresh loader basis, all phase 0 then all phase 1, each ordered by captured
    // (comboSelIndex2, original slot). Actual private workspaces are wired into
    // the slot table, including self references. Cycles read the target's then-
    // current matrix exactly as native scans do; no topological sort/iteration.
    // This deterministic fresh evaluation does not reproduce prior-frame state
    // of continuous playback. Physics is disabled. Scale is finalized only
    // after both complete scene sweeps. No partial frame escapes on failure.
    // Retained budget aggregates all children; workspace budget is shared by
    // simultaneously live models. Nested transient allocations are not a peak
    // heap cap. Sources can be shared across concurrent calls. Result owns all
    // data and outlives this source and the mathematical context.
    // Exceptions match BoneEvaluationSource::Evaluate. Not a public C ABI yet.
    std::unique_ptr<BoneSceneFrame> Evaluate(double frame, const BoneTransformContext&,
        std::uint64_t maxRetainedBytes = 256ull * 1024 * 1024,
        std::uint64_t maxWorkspaceBytes = 512ull * 1024 * 1024) const;
    // Keeps exact discrete key selection separate from continuous interpolation.
    std::unique_ptr<BoneSceneFrame> EvaluateAt(const EvaluationTime& time, const BoneTransformContext&,
        std::uint64_t maxRetainedBytes = 256ull * 1024 * 1024,
        std::uint64_t maxWorkspaceBytes = 512ull * 1024 * 1024) const;
};
}
#endif
