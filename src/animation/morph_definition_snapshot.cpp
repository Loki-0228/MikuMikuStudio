#include "mikumikustudio/morph_definition_snapshot.hpp"
#if !MMD_ORIGINAL_PROFILE

#include "mikumikustudio/model.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
namespace {

constexpr std::uint8_t kNoMorphTable = 0xFFu;

// Per-morph view of the one native entry table Capture copied. PMD records
// report kind 1 because their loader fills only the vertex table; their raw
// type/panel bytes are still retained in the MorphDefinition itself.
struct MorphEntryTable {
    std::uint8_t kind = kNoMorphTable;  // PMX type 0..8, 1 for PMD vertex
    std::uint32_t count = 0;
    std::uint64_t offset = 0;  // first entry in the matching flat table
};

// Header-level description of the active table of one source morph.
struct ActiveTable {
    std::uint8_t kind = kNoMorphTable;
    std::uint32_t count = 0;
    const void* entries = nullptr;
};

// The budget covers the two owning objects and the actual capacity of every
// retained vector. Each addition is checked against the remaining budget
// instead of wrapping.
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride,
            std::uint64_t limit) {
    if (bytes > limit || (stride != 0 && count > (limit - bytes) / stride))
        throw std::length_error("morph definition snapshot budget");
    bytes += count * stride;
}

std::uint64_t AddCount(std::uint64_t total, std::uint64_t count) {
    if (count > (std::numeric_limits<std::uint64_t>::max)() - total)
        throw std::length_error("morph definition snapshot count");
    return total + count;
}

void EnsureSize(std::uint64_t count) {
    if (count > (std::numeric_limits<std::size_t>::max)())
        throw std::length_error("morph definition snapshot size");
}

bool Finite(const float* values, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i])) return false;
    return true;
}

// The two reserved words are inactive padding and may hold any bit pattern.
bool FiniteChannels(const mdl::MaterialMorphChannels& channels) noexcept {
    return Finite(channels.diffuse, 4) &&
           Finite(channels.specular, 3) &&
           std::isfinite(channels.specularPower) &&
           Finite(channels.ambient, 3) &&
           Finite(channels.edgeColor, 4) &&
           std::isfinite(channels.edgeSize) &&
           Finite(channels.textureTint, 4) &&
           Finite(channels.sphereTint, 4) &&
           Finite(channels.toonTint, 4);
}

// Family zero is the native base UV channel; only XY is consumed, while its
// ZW words are restore bytes that may hold any bit pattern. Additional-UV
// families use all four components.
bool FiniteUvOffset(const float* offset, std::size_t family) noexcept {
    return Finite(offset, family == 0 ? 2 : 4);
}

bool ValidVertex(std::uint32_t index, std::uint64_t vertexCount) noexcept {
    return static_cast<std::uint64_t>(index) < vertexCount;
}

bool ValidSigned(std::int32_t index, std::uint64_t count) noexcept {
    return index >= 0 && static_cast<std::uint64_t>(index) < count;
}

// Reads only the header fields of the one active table. Inactive PMX counts
// and pointers are not read, and impulse payloads are never sized by guesswork.
ActiveTable SourceActive(const mdl::MorphRecord& morph, bool pmx) {
    if (morph.impulseEntries != nullptr || morph.impulse2Entries != nullptr)
        throw std::invalid_argument(
            "morph definition snapshot unsupported impulse payload");
    ActiveTable table;
    if (!pmx) {
        table.kind = 1;
        if (morph.offsetCount < 0)
            throw std::invalid_argument(
                "morph definition snapshot vertex count");
        table.count = static_cast<std::uint32_t>(morph.offsetCount);
        table.entries = morph.vertexEntries;
    } else {
        const std::uint8_t type = morph.type;
        if (type > 8)
            throw std::invalid_argument(
                "morph definition snapshot unsupported morph type");
        table.kind = type;
        std::int32_t count = 0;
        switch (type) {
        case 0:
            count = morph.groupCount;
            table.entries = morph.groupEntries;
            break;
        case 1:
            count = morph.offsetCount;
            table.entries = morph.vertexEntries;
            break;
        case 2:
            count = morph.boneCount;
            table.entries = morph.boneEntries;
            break;
        case 8:
            count = morph.materialCount;
            table.entries = morph.materialEntries;
            break;
        default: {
            const std::size_t family =
                static_cast<std::size_t>(type - 3);
            count = morph.uvCounts[family];
            table.entries = morph.uvEntries[family];
            break;
        }
        }
        if (count < 0)
            throw std::invalid_argument(
                "morph definition snapshot active count");
        table.count = static_cast<std::uint32_t>(count);
    }
    if (table.count != 0 && table.entries == nullptr)
        throw std::invalid_argument(
            "morph definition snapshot active table");
    return table;
}

}  // namespace

struct MorphDefinitionSnapshot::Impl {
    std::vector<MorphDefinition> morphs;
    std::vector<MorphEntryTable> tables;
    std::vector<mdl::PmdVertexMorphEntry> vertexEntries;
    std::vector<mdl::PmxBoneMorphEntry> boneEntries;
    std::vector<mdl::PmxGroupMorphEntry> groupEntries;
    std::vector<mdl::PmxUvMorphEntry> uvEntries;
    std::vector<mdl::PmxMaterialMorphEntry> materialEntries;
    std::vector<mdl::PmdVertexMorphEntry> baseVertexEntries;
    std::array<std::vector<mdl::PmxUvMorphEntry>, 5> baseUvEntries;
    std::vector<mdl::MaterialMorphChannels> baseMaterialEntries;
    std::uint32_t vertexCount = 0;
    std::uint32_t boneCount = 0;
    std::uint32_t materialCount = 0;
    std::uint8_t physicsMode = 0;
    std::uint8_t additionalUvCount = 0;
    std::uint64_t bytes = 0;
};

MorphDefinitionSnapshot::MorphDefinitionSnapshot() = default;
MorphDefinitionSnapshot::~MorphDefinitionSnapshot() = default;

std::unique_ptr<MorphDefinitionSnapshot> MorphDefinitionSnapshot::Capture(
    const unsigned char* source, std::uint64_t limit) {
    if (!source)
        throw std::invalid_argument("morph definition snapshot source");
    const auto& model = *mdl::Mdl(source);

    const std::uint64_t int32Max = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int32_t>::max)());
    const std::uint64_t morphCount = model.morphCount;
    const std::uint64_t vertexCount = model.vertexCount;
    const std::uint64_t boneCount = model.boneCount;
    const std::uint64_t materialCount = model.materialCount;
    const std::uint64_t baseMorphCount = model.morph0Count;
    if (morphCount > int32Max || vertexCount > int32Max ||
        boneCount > int32Max || materialCount > int32Max ||
        baseMorphCount > int32Max)
        throw std::invalid_argument("morph definition snapshot counts");
    if (morphCount != 0 && model.morphs == nullptr)
        throw std::invalid_argument(
            "morph definition snapshot morph table");

    const bool pmx = model.physicsMode == 2;
    const std::uint8_t additionalUvCount = model.pmxAdditionalUvCount;
    if (pmx && additionalUvCount > 4)
        throw std::invalid_argument(
            "morph definition snapshot additional UV count");

    if (baseMorphCount != 0 && model.morph0Table == nullptr)
        throw std::invalid_argument(
            "morph definition snapshot base vertex table");

    // Only PMX owns the flattened UV and material-morph restore tables. PMD
    // keeps no such base data and its bytes are not read here.
    std::array<std::uint64_t, 5> baseUvCounts{};
    if (pmx) {
        for (std::size_t family = 0; family < baseUvCounts.size(); ++family) {
            const std::int32_t count =
                model.uvMorphCounts.byFamily[family];
            if (count < 0)
                throw std::invalid_argument(
                    "morph definition snapshot UV count");
            baseUvCounts[family] = static_cast<std::uint64_t>(count);
            if (count != 0 &&
                model.uvMorphTables.byFamily[family] == nullptr)
                throw std::invalid_argument(
                    "morph definition snapshot UV base table");
        }
        if (materialCount != 0 &&
            model.materialMorphPools.base == nullptr)
            throw std::invalid_argument(
                "morph definition snapshot material base table");
    }

    // Reject an impossible retained budget before touching the morph table or
    // any large payload. The owning objects and every top-level table are
    // already paid for here.
    std::uint64_t minimum = 0;
    Charge(minimum, 1,
           static_cast<std::uint64_t>(sizeof(MorphDefinitionSnapshot)) +
               static_cast<std::uint64_t>(sizeof(Impl)),
           limit);
    Charge(minimum, morphCount, sizeof(MorphDefinition), limit);
    Charge(minimum, morphCount, sizeof(MorphEntryTable), limit);
    Charge(minimum, baseMorphCount, sizeof(mdl::PmdVertexMorphEntry), limit);
    if (pmx) {
        for (const std::uint64_t count : baseUvCounts)
            Charge(minimum, count, sizeof(mdl::PmxUvMorphEntry), limit);
        Charge(minimum, materialCount, sizeof(mdl::MaterialMorphChannels),
               limit);
    }

    // Header scan: validate the active count of every morph, then reserve the
    // minimum space for the tables they point at before allocating anything.
    const mdl::MorphRecord* const morphs = model.morphs;
    std::uint64_t vertexTotal = 0;
    std::uint64_t boneTotal = 0;
    std::uint64_t groupTotal = 0;
    std::uint64_t uvTotal = 0;
    std::uint64_t materialTotal = 0;
    for (std::uint64_t index = 0; index < morphCount; ++index) {
        const ActiveTable active =
            SourceActive(morphs[static_cast<std::size_t>(index)], pmx);
        switch (active.kind) {
        case 0:
            groupTotal = AddCount(groupTotal, active.count);
            break;
        case 1:
            vertexTotal = AddCount(vertexTotal, active.count);
            break;
        case 2:
            boneTotal = AddCount(boneTotal, active.count);
            break;
        case 8:
            materialTotal = AddCount(materialTotal, active.count);
            break;
        default:
            uvTotal = AddCount(uvTotal, active.count);
            break;
        }
    }

    Charge(minimum, vertexTotal, sizeof(mdl::PmdVertexMorphEntry), limit);
    Charge(minimum, boneTotal, sizeof(mdl::PmxBoneMorphEntry), limit);
    Charge(minimum, groupTotal, sizeof(mdl::PmxGroupMorphEntry), limit);
    Charge(minimum, uvTotal, sizeof(mdl::PmxUvMorphEntry), limit);
    Charge(minimum, materialTotal, sizeof(mdl::PmxMaterialMorphEntry), limit);

    EnsureSize(vertexTotal);
    EnsureSize(boneTotal);
    EnsureSize(groupTotal);
    EnsureSize(uvTotal);
    EnsureSize(materialTotal);

    auto result = std::unique_ptr<MorphDefinitionSnapshot>(
        new MorphDefinitionSnapshot);
    result->impl_ = std::make_unique<Impl>();
    auto& value = *result->impl_;
    value.vertexCount = static_cast<std::uint32_t>(vertexCount);
    value.boneCount = static_cast<std::uint32_t>(boneCount);
    value.materialCount = static_cast<std::uint32_t>(materialCount);
    value.physicsMode = model.physicsMode;
    value.additionalUvCount = additionalUvCount;

    value.morphs.resize(static_cast<std::size_t>(morphCount));
    if (!value.morphs.empty())
        std::memset(value.morphs.data(), 0,
                    value.morphs.size() * sizeof(MorphDefinition));
    value.tables.resize(static_cast<std::size_t>(morphCount));
    value.vertexEntries.resize(static_cast<std::size_t>(vertexTotal));
    value.boneEntries.resize(static_cast<std::size_t>(boneTotal));
    value.groupEntries.resize(static_cast<std::size_t>(groupTotal));
    value.uvEntries.resize(static_cast<std::size_t>(uvTotal));
    value.materialEntries.resize(static_cast<std::size_t>(materialTotal));
    value.baseVertexEntries.resize(
        static_cast<std::size_t>(baseMorphCount));
    if (pmx) {
        for (std::size_t family = 0; family < baseUvCounts.size(); ++family)
            value.baseUvEntries[family].resize(
                static_cast<std::size_t>(baseUvCounts[family]));
        value.baseMaterialEntries.resize(
            static_cast<std::size_t>(materialCount));
    }

    // ---- model-wide restore tables ---------------------------------------
    for (std::uint64_t entry = 0; entry < baseMorphCount; ++entry) {
        const std::size_t slot = static_cast<std::size_t>(entry);
        const mdl::PmdVertexMorphEntry& sourceEntry =
            model.morph0Table[slot];
        if (!ValidVertex(sourceEntry.vertexIndex, vertexCount) ||
            !Finite(sourceEntry.offset, 3))
            throw std::invalid_argument(
                "morph definition snapshot base vertex entry");
        std::memcpy(&value.baseVertexEntries[slot], &sourceEntry,
                    sizeof(sourceEntry));
    }
    if (pmx) {
        for (std::size_t family = 0; family < baseUvCounts.size(); ++family) {
            const mdl::PmxUvMorphEntry* const sourceEntries =
                model.uvMorphTables.byFamily[family];
            std::vector<mdl::PmxUvMorphEntry>& target =
                value.baseUvEntries[family];
            for (std::uint64_t entry = 0; entry < baseUvCounts[family];
                 ++entry) {
                const std::size_t slot = static_cast<std::size_t>(entry);
                const mdl::PmxUvMorphEntry& sourceEntry =
                    sourceEntries[slot];
                if (!ValidSigned(sourceEntry.vertexIndex, vertexCount) ||
                    !FiniteUvOffset(sourceEntry.offset, family))
                    throw std::invalid_argument(
                        "morph definition snapshot base UV entry");
                std::memcpy(&target[slot], &sourceEntry,
                            sizeof(sourceEntry));
            }
        }
        const mdl::MaterialMorphPool* const basePool =
            model.materialMorphPools.base;
        for (std::uint64_t entry = 0; entry < materialCount; ++entry) {
            const std::size_t slot = static_cast<std::size_t>(entry);
            const mdl::MaterialMorphChannels& channels =
                basePool[slot].channels;
            if (!FiniteChannels(channels))
                throw std::invalid_argument(
                    "morph definition snapshot base material entry");
            std::memcpy(&value.baseMaterialEntries[slot], &channels,
                        sizeof(channels));
        }
    }

    // ---- per-morph definitions and the single active entry table ---------
    std::uint64_t vertexOffset = 0;
    std::uint64_t boneOffset = 0;
    std::uint64_t groupOffset = 0;
    std::uint64_t uvOffset = 0;
    std::uint64_t materialOffset = 0;
    for (std::uint64_t index = 0; index < morphCount; ++index) {
        const std::size_t slot = static_cast<std::size_t>(index);
        const mdl::MorphRecord& sourceMorph = morphs[slot];
        MorphDefinition& definition = value.morphs[slot];
        std::memcpy(definition.name.data(), sourceMorph.name,
                    sizeof(definition.name));
        std::memcpy(definition.nameEn.data(), sourceMorph.nameEn,
                    sizeof(definition.nameEn));
        std::memcpy(&definition.panel, &sourceMorph.panel,
                    sizeof(definition.panel));
        definition.type = sourceMorph.type;
        definition.vertexCount = 0;
        definition.boneCount = 0;
        definition.groupCount = 0;
        definition.materialCount = 0;
        definition.uvCounts.fill(0);

        const ActiveTable active = SourceActive(sourceMorph, pmx);
        MorphEntryTable& table = value.tables[slot];
        table.kind = active.kind;
        table.count = active.count;
        table.offset = 0;
        switch (active.kind) {
        case 0: {
            definition.groupCount = active.count;
            table.offset = groupOffset;
            const auto* const entries =
                static_cast<const mdl::PmxGroupMorphEntry*>(active.entries);
            for (std::uint32_t entry = 0; entry < active.count; ++entry) {
                const mdl::PmxGroupMorphEntry& sourceEntry = entries[entry];
                if (!ValidSigned(sourceEntry.morphIndex, morphCount) ||
                    !std::isfinite(sourceEntry.weight))
                    throw std::invalid_argument(
                        "morph definition snapshot group entry");
                std::memcpy(
                    &value.groupEntries[static_cast<std::size_t>(
                        groupOffset + entry)],
                    &sourceEntry, sizeof(sourceEntry));
            }
            groupOffset += active.count;
            break;
        }
        case 1: {
            definition.vertexCount = active.count;
            table.offset = vertexOffset;
            const auto* const entries =
                static_cast<const mdl::PmdVertexMorphEntry*>(active.entries);
            for (std::uint32_t entry = 0; entry < active.count; ++entry) {
                const mdl::PmdVertexMorphEntry& sourceEntry = entries[entry];
                if (!ValidVertex(sourceEntry.vertexIndex, vertexCount) ||
                    !Finite(sourceEntry.offset, 3))
                    throw std::invalid_argument(
                        "morph definition snapshot vertex entry");
                std::memcpy(
                    &value.vertexEntries[static_cast<std::size_t>(
                        vertexOffset + entry)],
                    &sourceEntry, sizeof(sourceEntry));
            }
            vertexOffset += active.count;
            break;
        }
        case 2: {
            definition.boneCount = active.count;
            table.offset = boneOffset;
            const auto* const entries =
                static_cast<const mdl::PmxBoneMorphEntry*>(active.entries);
            for (std::uint32_t entry = 0; entry < active.count; ++entry) {
                const mdl::PmxBoneMorphEntry& sourceEntry = entries[entry];
                if (!ValidSigned(sourceEntry.boneIndex, boneCount) ||
                    !Finite(sourceEntry.translation, 3) ||
                    !Finite(sourceEntry.rotation, 4))
                    throw std::invalid_argument(
                        "morph definition snapshot bone entry");
                std::memcpy(
                    &value.boneEntries[static_cast<std::size_t>(
                        boneOffset + entry)],
                    &sourceEntry, sizeof(sourceEntry));
            }
            boneOffset += active.count;
            break;
        }
        case 8: {
            definition.materialCount = active.count;
            table.offset = materialOffset;
            const auto* const entries =
                static_cast<const mdl::PmxMaterialMorphEntry*>(active.entries);
            for (std::uint32_t entry = 0; entry < active.count; ++entry) {
                const mdl::PmxMaterialMorphEntry& sourceEntry =
                    entries[entry];
                if (!(sourceEntry.materialIndex == -1 ||
                      ValidSigned(sourceEntry.materialIndex, materialCount)) ||
                    !FiniteChannels(sourceEntry.channels))
                    throw std::invalid_argument(
                        "morph definition snapshot material entry");
                std::memcpy(
                    &value.materialEntries[static_cast<std::size_t>(
                        materialOffset + entry)],
                    &sourceEntry, sizeof(sourceEntry));
            }
            materialOffset += active.count;
            break;
        }
        default: {
            const std::size_t family =
                static_cast<std::size_t>(active.kind - 3);
            definition.uvCounts[family] = active.count;
            table.offset = uvOffset;
            const auto* const entries =
                static_cast<const mdl::PmxUvMorphEntry*>(active.entries);
            for (std::uint32_t entry = 0; entry < active.count; ++entry) {
                const mdl::PmxUvMorphEntry& sourceEntry = entries[entry];
                if (!ValidSigned(sourceEntry.vertexIndex, vertexCount) ||
                    !FiniteUvOffset(sourceEntry.offset, family))
                    throw std::invalid_argument(
                        "morph definition snapshot UV entry");
                std::memcpy(
                    &value.uvEntries[static_cast<std::size_t>(
                        uvOffset + entry)],
                    &sourceEntry, sizeof(sourceEntry));
            }
            uvOffset += active.count;
            break;
        }
        }
    }

    // Final accounting uses the real capacities, which the standard does not
    // require to equal the requested sizes.
    std::uint64_t retained = 0;
    Charge(retained, 1,
           static_cast<std::uint64_t>(sizeof(MorphDefinitionSnapshot)) +
               static_cast<std::uint64_t>(sizeof(Impl)),
           limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.morphs.capacity()),
           sizeof(MorphDefinition), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.tables.capacity()),
           sizeof(MorphEntryTable), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.vertexEntries.capacity()),
           sizeof(mdl::PmdVertexMorphEntry), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.boneEntries.capacity()),
           sizeof(mdl::PmxBoneMorphEntry), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.groupEntries.capacity()),
           sizeof(mdl::PmxGroupMorphEntry), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.uvEntries.capacity()),
           sizeof(mdl::PmxUvMorphEntry), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.materialEntries.capacity()),
           sizeof(mdl::PmxMaterialMorphEntry), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.baseVertexEntries.capacity()),
           sizeof(mdl::PmdVertexMorphEntry), limit);
    for (const auto& family : value.baseUvEntries)
        Charge(retained,
               static_cast<std::uint64_t>(family.capacity()),
               sizeof(mdl::PmxUvMorphEntry), limit);
    Charge(retained,
           static_cast<std::uint64_t>(value.baseMaterialEntries.capacity()),
           sizeof(mdl::MaterialMorphChannels), limit);
    value.bytes = retained;
    return result;
}

std::uint32_t MorphDefinitionSnapshot::MorphCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->morphs.size()) : 0;
}

std::uint32_t MorphDefinitionSnapshot::VertexCount() const noexcept {
    return impl_ ? impl_->vertexCount : 0;
}

std::uint32_t MorphDefinitionSnapshot::BoneCount() const noexcept {
    return impl_ ? impl_->boneCount : 0;
}

std::uint32_t MorphDefinitionSnapshot::MaterialCount() const noexcept {
    return impl_ ? impl_->materialCount : 0;
}

std::uint8_t MorphDefinitionSnapshot::PhysicsMode() const noexcept {
    return impl_ ? impl_->physicsMode : 0;
}

std::uint8_t MorphDefinitionSnapshot::AdditionalUvCount() const noexcept {
    return impl_ ? impl_->additionalUvCount : 0;
}

std::uint64_t MorphDefinitionSnapshot::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}

std::uint32_t MorphDefinitionSnapshot::BaseVertexCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(
                       impl_->baseVertexEntries.size())
                 : 0;
}

std::uint32_t MorphDefinitionSnapshot::BaseUvCount(
    std::uint32_t family) const noexcept {
    if (!impl_ || family >= impl_->baseUvEntries.size()) return 0;
    return static_cast<std::uint32_t>(
        impl_->baseUvEntries[family].size());
}

std::uint32_t MorphDefinitionSnapshot::BaseMaterialCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(
                       impl_->baseMaterialEntries.size())
                 : 0;
}

bool MorphDefinitionSnapshot::MorphAt(std::uint32_t morph,
                                      MorphDefinition& out) const noexcept {
    if (!impl_ || morph >= impl_->morphs.size()) return false;
    std::memcpy(&out, &impl_->morphs[morph], sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::VertexAt(
    std::uint32_t morph, std::uint32_t entry,
    mdl::PmdVertexMorphEntry& out) const noexcept {
    if (!impl_ || morph >= impl_->tables.size()) return false;
    const MorphEntryTable& table = impl_->tables[morph];
    if (table.kind != 1 || entry >= table.count) return false;
    const std::uint64_t index = table.offset + entry;
    if (index >= impl_->vertexEntries.size()) return false;
    std::memcpy(&out,
                &impl_->vertexEntries[static_cast<std::size_t>(index)],
                sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::BoneAt(
    std::uint32_t morph, std::uint32_t entry,
    mdl::PmxBoneMorphEntry& out) const noexcept {
    if (!impl_ || morph >= impl_->tables.size()) return false;
    const MorphEntryTable& table = impl_->tables[morph];
    if (table.kind != 2 || entry >= table.count) return false;
    const std::uint64_t index = table.offset + entry;
    if (index >= impl_->boneEntries.size()) return false;
    std::memcpy(&out,
                &impl_->boneEntries[static_cast<std::size_t>(index)],
                sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::GroupAt(
    std::uint32_t morph, std::uint32_t entry,
    mdl::PmxGroupMorphEntry& out) const noexcept {
    if (!impl_ || morph >= impl_->tables.size()) return false;
    const MorphEntryTable& table = impl_->tables[morph];
    if (table.kind != 0 || entry >= table.count) return false;
    const std::uint64_t index = table.offset + entry;
    if (index >= impl_->groupEntries.size()) return false;
    std::memcpy(&out,
                &impl_->groupEntries[static_cast<std::size_t>(index)],
                sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::UvAt(
    std::uint32_t morph, std::uint32_t family,
    std::uint32_t entry, mdl::PmxUvMorphEntry& out) const noexcept {
    if (!impl_ || morph >= impl_->tables.size() || family >= 5)
        return false;
    const MorphEntryTable& table = impl_->tables[morph];
    if (table.kind != static_cast<std::uint8_t>(3 + family) ||
        entry >= table.count)
        return false;
    const std::uint64_t index = table.offset + entry;
    if (index >= impl_->uvEntries.size()) return false;
    std::memcpy(&out,
                &impl_->uvEntries[static_cast<std::size_t>(index)],
                sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::MaterialAt(
    std::uint32_t morph, std::uint32_t entry,
    mdl::PmxMaterialMorphEntry& out) const noexcept {
    if (!impl_ || morph >= impl_->tables.size()) return false;
    const MorphEntryTable& table = impl_->tables[morph];
    if (table.kind != 8 || entry >= table.count) return false;
    const std::uint64_t index = table.offset + entry;
    if (index >= impl_->materialEntries.size()) return false;
    std::memcpy(&out,
                &impl_->materialEntries[static_cast<std::size_t>(index)],
                sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::BaseVertexAt(
    std::uint32_t entry, mdl::PmdVertexMorphEntry& out) const noexcept {
    if (!impl_ || entry >= impl_->baseVertexEntries.size()) return false;
    std::memcpy(&out, &impl_->baseVertexEntries[entry], sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::BaseUvAt(
    std::uint32_t family, std::uint32_t entry,
    mdl::PmxUvMorphEntry& out) const noexcept {
    if (!impl_ || family >= impl_->baseUvEntries.size()) return false;
    const std::vector<mdl::PmxUvMorphEntry>& entries =
        impl_->baseUvEntries[family];
    if (entry >= entries.size()) return false;
    std::memcpy(&out, &entries[entry], sizeof(out));
    return true;
}

bool MorphDefinitionSnapshot::BaseMaterialAt(
    std::uint32_t entry,
    mdl::MaterialMorphChannels& out) const noexcept {
    if (!impl_ || entry >= impl_->baseMaterialEntries.size()) return false;
    std::memcpy(&out, &impl_->baseMaterialEntries[entry], sizeof(out));
    return true;
}

}  // namespace mikumikustudio
#endif
