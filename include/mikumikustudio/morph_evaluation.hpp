#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/morph_definition_snapshot.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace mikumikustudio {
struct MorphMaterialProperties {
    std::array<float, 4> diffuse{}, edgeColor{};
    std::array<float, 3> specular{}, ambient{};
    float specularPower = 0, edgeSize = 0;
};
struct MorphMaterialEvaluation {
    mdl::MaterialMorphChannels base{}, additive{}, multiplicative{};
    // Only the sixteen native material properties are composed here. The
    // renderer consumes tint add/multiply separately against texture samples.
    MorphMaterialProperties properties;
};
class MorphEvaluationFrame {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MorphEvaluationFrame();
public:
    ~MorphEvaluationFrame();
    MorphEvaluationFrame(const MorphEvaluationFrame&) = delete;
    MorphEvaluationFrame& operator=(const MorphEvaluationFrame&) = delete;
    // CPU-only evaluation of frozen definitions and direct weights. The caller
    // supplies an immutable array of exactly MorphCount() finite weights.
    // Negative/overshooting weights are allowed. PMD ignores morph zero.
    // Groups expand one level, preserving native contribution/arithmetic order.
    // Result owns all data; neither input needs to outlive this call.
    // Every vertex/UV definition target needs a matching restore entry, even
    // at zero weight. Results cover only restored vertices, not the full mesh.
    // Missing bases/count mismatch/nonfinite weight: invalid_argument. A
    // nonfinite active computed result: overflow_error. Budget: length_error.
    // Allocation: bad_alloc. Failures publish no frame and modify no input.
    // The limit counts retained object/vector capacity, not temporary peak use.
    static std::unique_ptr<MorphEvaluationFrame> Evaluate(
        const MorphDefinitionSnapshot& definitions, const float* weights,
        std::uint32_t weightCount, std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024);
    std::uint8_t PhysicsMode() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Sparse unique indices are enumerated in ascending order. Duplicate base
    // entries are restored in original order (last wins) before accumulation.
    std::uint32_t PositionCount() const noexcept;
    bool PositionIndexAt(std::uint32_t entry, std::uint32_t& out) const noexcept;
    bool PositionAt(std::uint32_t vertex, std::array<float, 3>& out) const noexcept;
    std::uint32_t UvCount(std::uint32_t family) const noexcept;
    bool UvIndexAt(std::uint32_t family, std::uint32_t entry, std::uint32_t& out) const noexcept;
    // UV0 computes XY only; ZW retain the last base entry's raw bits.
    bool UvAt(std::uint32_t family, std::uint32_t vertex, std::array<float, 4>& out) const noexcept;
    // PMX bone output spans all bones when a bone morph has entries, otherwise
    // zero. It is an offset, before hierarchy, IK, skinning and physics.
    std::uint32_t BoneCount() const noexcept;
    bool BoneAt(std::uint32_t bone, mdl::BoneMorphOffsetRecord& out) const noexcept;
    std::uint32_t MaterialCount() const noexcept;
    bool MaterialAt(std::uint32_t material, MorphMaterialEvaluation& out) const noexcept;
    // All reads are concurrent and allocation-free. Invalid indices leave out
    // untouched. Evaluation itself is independent across concurrent callers.
};
}
#endif
