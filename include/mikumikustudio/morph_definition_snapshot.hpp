#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/subrecord_layout.hpp"
#include <array>
#include <cstdint>
#include <memory>

namespace mikumikustudio {
// Loaded definitions, not current weights or deformed output. PMD type bytes
// are retained but its entries are vertices, already remapped by the loader.
struct MorphDefinition {
    std::array<char, 20> name{}, nameEn{};
    std::uint8_t panel = 0, type = 0;
    std::uint32_t vertexCount = 0, boneCount = 0, groupCount = 0, materialCount = 0;
    std::array<std::uint32_t, 5> uvCounts{};
};
class MorphDefinitionSnapshot {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MorphDefinitionSnapshot();
public:
    ~MorphDefinitionSnapshot();
    MorphDefinitionSnapshot(const MorphDefinitionSnapshot&) = delete;
    MorphDefinitionSnapshot& operator=(const MorphDefinitionSnapshot&) = delete;
    // UI-safe-point capture; caller guarantees actual table extents. Owns all
    // active PMD vertex / PMX 0..8 offsets and model restore tables. Names use
    // the fixed byte fields; wide text, current value, accumulation pools,
    // vertices and device resources are not accessed. No live-model writes.
    // PMX 9/10 and unknown opaque payloads are rejected, never silently lost.
    // Invalid data: invalid_argument; budget: length_error; allocation: bad_alloc.
    // Retained budget includes owned capacities, not peak temporary storage.
    static std::unique_ptr<MorphDefinitionSnapshot> Capture(const unsigned char* model,
        std::uint64_t maxRetainedBytes = 64ull * 1024 * 1024);
    std::uint32_t MorphCount() const noexcept;
    std::uint32_t VertexCount() const noexcept;
    std::uint32_t BoneCount() const noexcept;
    std::uint32_t MaterialCount() const noexcept;
    std::uint8_t PhysicsMode() const noexcept;
    std::uint8_t AdditionalUvCount() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
    std::uint32_t BaseVertexCount() const noexcept;
    std::uint32_t BaseUvCount(std::uint32_t family) const noexcept;
    std::uint32_t BaseMaterialCount() const noexcept;
    // Allocation-free concurrent reads survive source destruction. Invalid
    // indices leave out unchanged. Group order, duplicates and cycles remain.
    bool MorphAt(std::uint32_t morph, MorphDefinition& out) const noexcept;
    bool VertexAt(std::uint32_t morph, std::uint32_t entry, mdl::PmdVertexMorphEntry& out) const noexcept;
    bool BoneAt(std::uint32_t morph, std::uint32_t entry, mdl::PmxBoneMorphEntry& out) const noexcept;
    bool GroupAt(std::uint32_t morph, std::uint32_t entry, mdl::PmxGroupMorphEntry& out) const noexcept;
    bool UvAt(std::uint32_t morph, std::uint32_t family, std::uint32_t entry, mdl::PmxUvMorphEntry& out) const noexcept;
    bool MaterialAt(std::uint32_t morph, std::uint32_t entry, mdl::PmxMaterialMorphEntry& out) const noexcept;
    bool BaseVertexAt(std::uint32_t entry, mdl::PmdVertexMorphEntry& out) const noexcept;
    bool BaseUvAt(std::uint32_t family, std::uint32_t entry, mdl::PmxUvMorphEntry& out) const noexcept;
    bool BaseMaterialAt(std::uint32_t entry, mdl::MaterialMorphChannels& out) const noexcept;
};
}
#endif
