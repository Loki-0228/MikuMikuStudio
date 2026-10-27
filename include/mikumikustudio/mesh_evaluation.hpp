#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <array>
#include <cstdint>
#include <memory>

namespace mikumikustudio {
class MeshDefinitionSnapshot;
class MorphEvaluationFrame;
class BoneEvaluationFrame;
class BoneTransformContext;

struct MeshEvaluationVertex {
    std::array<float, 3> position{}, normal{};
    std::array<float, 2> uv{};
    std::array<std::array<float, 4>, 4> additionalUv{};
};

// Internal CPU-only geometry result. This is not a plugin ABI or a renderer.
class MeshEvaluationFrame final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MeshEvaluationFrame();
public:
    ~MeshEvaluationFrame();
    MeshEvaluationFrame(const MeshEvaluationFrame&) = delete;
    MeshEvaluationFrame& operator=(const MeshEvaluationFrame&) = delete;

    // Trusted caller MUST arrange for all inputs to describe the same model,
    // capture generation and exact evaluation time. Counts alone do not prove
    // that identity; this low-level entry is never exposed directly to plugins.
    // Immutable inputs and context remain alive for the duration of the call.
    // Physics is not simulated here. Morph positions/UVs are final values,
    // not deltas; unrepresented vertices retain their mesh bind values.
    // UV families beyond the mesh count are checked but not output, as in the
    // native CPU vertex buffer. Out-of-range sparse vertices or inconsistent
    // format/counts: invalid_argument; nonfinite output/math:
    // overflow_error; limits/size overflow: length_error; allocation: bad_alloc.
    // Retained budget counts frame, implementation and vertex vector capacity.
    // Workspace budget counts the bone work vector object and capacity; neither
    // limit is a process peak cap or charges borrowed inputs/context. A failure
    // publishes no partial frame. No input is mutated or retained.
    static std::unique_ptr<MeshEvaluationFrame> Evaluate(
        const MeshDefinitionSnapshot&, const MorphEvaluationFrame&,
        const BoneEvaluationFrame&, const BoneTransformContext&,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024,
        std::uint64_t maxWorkspaceBytes = 128ull * 1024 * 1024);
    std::uint8_t PhysicsMode() const noexcept;
    std::uint8_t AdditionalUvCount() const noexcept;
    std::uint32_t VertexCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Reads do not allocate and may run concurrently. Failure leaves out intact.
    bool VertexAt(std::uint32_t index, MeshEvaluationVertex& out) const noexcept;
};
}
#endif
