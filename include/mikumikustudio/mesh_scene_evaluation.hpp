#pragma once
#include "mikumikustudio/bone_scene_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/mesh_evaluation.hpp"
#include "mikumikustudio/mesh_scene_references.hpp"

namespace mikumikustudio {
class MeshDefinitionSnapshot;
// CPU material values from one captured model/time. nativeDiffuse is the
// unchanged material record RGB; diffuseMirrorAndAlpha is the material-morph
// composed diffuseMirror RGB followed by native diffuse alpha. Texture tints
// are separate additive/multiplicative inputs, not sampled texture colors.
// PMD has no PMX material morph; its captured values are static and tint
// defaults are zero/one. Edge/toon rendering itself is not represented here.
struct MeshSceneMaterial {
    std::uint8_t physicsMode = 0;
    std::array<float,3> nativeDiffuse{}, specular{}, ambient{};
    std::array<float,4> diffuseMirrorAndAlpha{}, edgeColor{};
    float specularPower = 0, edgeSize = 0;
    std::array<float,4> textureAdd{}, textureMultiply{}, sphereAdd{}, sphereMultiply{}, toonAdd{}, toonMultiply{};
};
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
    bool HasMaterials() const noexcept;
    // Preserve the exact discrete side even when Frame() rounds across a key.
    EvaluationTime Time() const noexcept;
    // Borrowed immutable children survive the source and input models.
    const BoneSceneFrame& Bones() const noexcept;
    const MeshEvaluationFrame* ModelFrame(std::uint32_t index) const noexcept;
    // Immutable, allocation-free copies. Invalid indices or nonfinite values
    // return false and leave out unchanged. Nonfinite static inputs do not
    // change the existing geometry capture/evaluation acceptance policy.
    std::uint32_t MaterialCount(std::uint32_t model) const noexcept;
    bool MaterialAt(std::uint32_t model, std::uint32_t material, MeshSceneMaterial& out) const noexcept;
};

class MeshSceneSource final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MeshSceneSource();
    static std::unique_ptr<MeshSceneSource> CaptureImpl(const BoneSceneModelInput*,
        std::uint32_t, std::uint64_t, bool, bool);
public:
    ~MeshSceneSource();
    MeshSceneSource(const MeshSceneSource&) = delete;
    MeshSceneSource& operator=(const MeshSceneSource&) = delete;
    // All source pieces are captured at the same host safe point, with the
    // model identity and dependency validation performed by BoneSceneSource.
    // Retained limits include owned capacity, not transient allocation peaks.
    static std::unique_ptr<MeshSceneSource> Capture(const BoneSceneModelInput*,
        std::uint32_t count, std::uint64_t maxRetainedBytes = 512ull * 1024 * 1024);
    // Opt in to additional material results. Existing Capture retains its
    // morph data but does not read these extra native material fields or
    // allocate per-material output values.
    static std::unique_ptr<MeshSceneSource> CaptureWithMaterials(const BoneSceneModelInput*,
        std::uint32_t count, std::uint64_t maxRetainedBytes = 512ull * 1024 * 1024);
    // Explicitly capture bounded native paths and raw toon/sphere flags at the
    // same safe point. The source owns references, not external files or pixels.
    // Non-terminated paths are rejected; the older capture paths remain unchanged.
    static std::unique_ptr<MeshSceneSource> CaptureWithMaterialReferences(const BoneSceneModelInput*,
        std::uint32_t count, std::uint64_t maxRetainedBytes = 512ull * 1024 * 1024);
    std::uint32_t ModelCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    bool HasMaterials() const noexcept;
    bool HasMaterialReferences() const noexcept;
    // An invalid index or a source without references leaves out unchanged.
    bool MaterialReferencesAt(std::uint32_t model, std::uint32_t material,
        MeshSceneMaterialReferences& out) const noexcept;
    // Returns raw model data without decoding names or resolving shared slots.
    // Failure leaves out unchanged.
    bool ToonTableReferencesAt(std::uint32_t model, MeshSceneToonTableReferences& out) const noexcept;
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
