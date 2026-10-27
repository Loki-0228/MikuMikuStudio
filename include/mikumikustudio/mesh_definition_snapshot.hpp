#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace mikumikustudio {
enum class MeshBindingType : std::uint8_t { Bdef1 = 0, Bdef2 = 1, Bdef4 = 2, Sdef = 3 };

// Unsupported weight algorithms are distinct from malformed mesh storage.
class MeshDefinitionUnsupported final : public std::invalid_argument {
public:
    MeshDefinitionUnsupported() : std::invalid_argument("mesh definition unsupported weight type") {}
};

struct MeshDefinitionVertex {
    std::array<float, 3> position{}, normal{};
    std::array<float, 2> uv{};
    // Group first, unlike the native PMX component-first working array.
    std::array<std::array<float, 4>, 4> additionalUv{};
    MeshBindingType binding = MeshBindingType::Bdef1;
    std::array<std::int32_t, 4> bones{{-1, -1, -1, -1}};
    // BDEF1: 1; BDEF2/SDEF: w and 1-w; BDEF4: all four raw weights.
    // PMD uses its signed weightPercent * the native float constant.
    std::array<float, 4> weights{};
    std::array<float, 3> sdefCenter{}, sdefR0Offset{}, sdefR1Offset{};
    float edgeScale = 0;
    // Raw loader bytes, not normalized flags. Only the matching format's
    // field is populated. PMD has no per-vertex material association.
    std::int8_t pmdWeightPercent = 0;
    std::uint8_t pmdEdgeDisabled = 0, pmxEdgeData = 0;
    std::int32_t materialIndex = -1;
};
struct MeshDefinitionTriangle {
    std::array<std::uint32_t, 3> vertices{};
    std::uint32_t materialIndex = 0;
};
struct MeshDefinitionMaterial {
    // Original material order and contiguous triangle-index ranges.
    std::uint32_t firstIndex = 0, indexCount = 0;
};

class MeshDefinitionSnapshot final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MeshDefinitionSnapshot();
public:
    ~MeshDefinitionSnapshot();
    MeshDefinitionSnapshot(const MeshDefinitionSnapshot&) = delete;
    MeshDefinitionSnapshot& operator=(const MeshDefinitionSnapshot&) = delete;
    // Capture loaded PMX (physicsMode == 2) or PMD mesh fields at a serialized
    // safe point. The caller guarantees actual table extents. Position/UV
    // restore tables overwrite only the private copy, in original order.
    // Duplicate restore entries retain last-write-wins semantics.
    // Active finite float bits and native weights are preserved; no weight
    // normalization or second SDEF rebasing is performed. Inactive weight,
    // SDEF and additional-UV lanes are not read and have deterministic defaults.
    // Both native CPU formats use the same missing-bone rule: negative or
    // out-of-range active indices are retained, not dereferenced or rejected.
    // A future skinning caller must use the native zero-matrix/BoneAux fallback.
    // QDEF/unknown: MeshDefinitionUnsupported. Invalid structures, geometry
    // references or active nonfinite data: invalid_argument. Budget/overflow:
    // length_error. Allocation: bad_alloc. Empty meshes are valid.
    // Budget counts retained objects/vector capacities, not transient peak use.
    // Does not capture names, texture paths, material colors, morph definitions,
    // bone definitions, current pose, device buffers or public object identities.
    static std::unique_ptr<MeshDefinitionSnapshot> Capture(const unsigned char* model,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024);
    std::uint8_t PhysicsMode() const noexcept;
    std::uint8_t AdditionalUvCount() const noexcept;
    std::uint32_t BoneCount() const noexcept;
    std::uint32_t VertexCount() const noexcept;
    std::uint32_t IndexCount() const noexcept;
    std::uint32_t TriangleCount() const noexcept;
    std::uint32_t MaterialCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    // Allocation-free concurrent copies, independent of the source lifetime.
    // Invalid indices leave out unchanged.
    bool VertexAt(std::uint32_t index, MeshDefinitionVertex& out) const noexcept;
    bool IndexAt(std::uint32_t index, std::uint32_t& out) const noexcept;
    bool TriangleAt(std::uint32_t index, MeshDefinitionTriangle& out) const noexcept;
    bool MaterialAt(std::uint32_t index, MeshDefinitionMaterial& out) const noexcept;
};
}
#endif
