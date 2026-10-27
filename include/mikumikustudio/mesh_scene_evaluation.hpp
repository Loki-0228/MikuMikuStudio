#pragma once
#include "mikumikustudio/bone_scene_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/mesh_evaluation.hpp"

namespace mikumikustudio {
class MeshDefinitionSnapshot;
// Internal coherent scene capture. The caller serializes Capture with model
// edits and lifetime changes. No model pointer is retained afterwards.
class MeshSceneFrame final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MeshSceneFrame();
    friend class MeshSceneSource;
public:
    ~MeshSceneFrame();
    MeshSceneFrame(const MeshSceneFrame&) = delete;
    MeshSceneFrame& operator=(const MeshSceneFrame&) = delete;
    std::uint32_t ModelCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Preserve the exact discrete side even when Frame() rounds across a key.
    EvaluationTime Time() const noexcept;
    // Borrowed immutable children survive the source and input models.
    const BoneSceneFrame& Bones() const noexcept;
    const MeshEvaluationFrame* ModelFrame(std::uint32_t index) const noexcept;
};

class MeshSceneSource final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MeshSceneSource();
public:
    ~MeshSceneSource();
    MeshSceneSource(const MeshSceneSource&) = delete;
    MeshSceneSource& operator=(const MeshSceneSource&) = delete;
    // All source pieces are captured at the same host safe point, with the
    // model identity and dependency validation performed by BoneSceneSource.
    // Retained limits include owned capacity, not transient allocation peaks.
    static std::unique_ptr<MeshSceneSource> Capture(const BoneSceneModelInput*,
        std::uint32_t count, std::uint64_t maxRetainedBytes = 512ull * 1024 * 1024);
    std::uint32_t ModelCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Frozen topology and bind data captured alongside this scene; index order
    // matches BoneSceneFrame::ModelAt. Borrowed until the source is destroyed.
    const MeshDefinitionSnapshot* MeshAt(std::uint32_t index) const noexcept;
    bool ModelAt(std::uint32_t index, BoneSceneModelInfo& out) const noexcept;
    // Uses the same exact time and frozen inputs for bones and geometry.
    // Borrowed inputs/context remain alive through the call; no partial frame
    // escapes. Workspace limit is conservative accounting, not a heap cap.
    std::unique_ptr<MeshSceneFrame> EvaluateAt(const EvaluationTime&,
        const BoneTransformContext&, std::uint64_t maxRetainedBytes = 512ull * 1024 * 1024,
        std::uint64_t maxWorkspaceBytes = 512ull * 1024 * 1024) const;
};
}
#endif
