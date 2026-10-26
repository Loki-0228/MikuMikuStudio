#include "mikumikustudio/morph_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE

#include "mikumikustudio/morph_definition_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
namespace {

// The legacy axis-angle quirk in ModelApplyMorphs subtracts this literal from
// the angle after a negative quaternion W was passed through acos.
constexpr double kLegacyPi = 3.140000104904175;
// Native comparison uses this double literal, not a float narrowing.
constexpr double kAxisLengthEpsilon = 0.00000011920929;

// The budget follows MorphDefinitionSnapshot: it charges the two owning
// objects plus the real capacity of every retained vector against the limit,
// checking each multiplication instead of letting it wrap.
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride,
            std::uint64_t limit) {
    if (bytes > limit || (stride != 0 && count > (limit - bytes) / stride))
        throw std::length_error("morph evaluation budget");
    bytes += count * stride;
}

void RequireFinite(const float* values, std::size_t count, const char* message) {
    for (std::size_t index = 0; index < count; ++index)
        if (!std::isfinite(values[index])) throw std::overflow_error(message);
}

// D3DXQuaternionMultiply(out, a, b): out = a * b (Hamilton product).
void QuatMul(float out[4], const float a[4], const float b[4]) {
    float result[4];
    result[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    result[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    result[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    result[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = result[0];
    out[1] = result[1];
    out[2] = result[2];
    out[3] = result[3];
}

// One bone-morph entry, arithmetic identical to AccumBoneEntry in
// ModelApplyMorphs: clamp W, double acos, legacy angle shift, float axis
// length, original-order right multiply with no normalization.
void AccumBoneEntry(mdl::BoneMorphOffsetRecord* work, std::uint32_t workCount,
                    const mdl::PmxBoneMorphEntry& entry, float weight) {
    if (!work || entry.boneIndex < 0 ||
        static_cast<std::uint32_t>(entry.boneIndex) >= workCount)
        return;
    mdl::BoneMorphOffsetRecord& record = work[entry.boneIndex];
    float x = entry.rotation[0];
    float y = entry.rotation[1];
    float z = entry.rotation[2];
    float qw = entry.rotation[3];
    if (qw > 1.0f)
        qw = 1.0f;
    else if (qw < -1.0f)
        qw = -1.0f;
    double angle = std::acos(static_cast<double>(qw));
    if (qw < 0.0f) angle -= kLegacyPi;
    const double length =
        std::sqrt(static_cast<double>(x * x + y * y + z * z));
    float q[4];
    if (angle == 0.0 || length < kAxisLengthEpsilon) {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
    } else {
        const double scaled = angle * weight;
        const double scale = std::sin(scaled) / length;
        q[0] = static_cast<float>(scale * x);
        q[1] = static_cast<float>(scale * y);
        q[2] = static_cast<float>(scale * z);
        q[3] = static_cast<float>(std::cos(scaled));
    }
    QuatMul(record.rotation, record.rotation, q);
    record.translation[0] += entry.translation[0] * weight;
    record.translation[1] += entry.translation[1] * weight;
    record.translation[2] += entry.translation[2] * weight;
}

void ApplyChannel(float& add, float& multiply, float delta, float weight,
                  bool additive) {
    if (additive)
        add += delta * weight;
    else
        multiply = (1.0f - (1.0f - delta) * weight) * multiply;
}

template <std::size_t Count>
void ApplyChannels(float (&add)[Count], float (&multiply)[Count],
                   const float (&delta)[Count], float weight, bool additive) {
    for (std::size_t index = 0; index < Count; ++index)
        ApplyChannel(add[index], multiply[index], delta[index], weight,
                     additive);
}

// One 128-byte material-morph entry applied to one additive/multiplicative
// channel pair. Reserved gap words stay zero because they are never touched.
void ApplyMaterialSlot(MorphMaterialEvaluation& material,
                       const mdl::PmxMaterialMorphEntry& entry, float weight) {
    const bool additive = entry.operation != 0;
    const mdl::MaterialMorphChannels& delta = entry.channels;
    mdl::MaterialMorphChannels& add = material.additive;
    mdl::MaterialMorphChannels& multiply = material.multiplicative;
    ApplyChannels(add.diffuse, multiply.diffuse, delta.diffuse, weight,
                  additive);
    ApplyChannels(add.specular, multiply.specular, delta.specular, weight,
                  additive);
    ApplyChannels(add.ambient, multiply.ambient, delta.ambient, weight,
                  additive);
    ApplyChannels(add.edgeColor, multiply.edgeColor, delta.edgeColor, weight,
                  additive);
    ApplyChannels(add.textureTint, multiply.textureTint, delta.textureTint,
                  weight, additive);
    ApplyChannels(add.sphereTint, multiply.sphereTint, delta.sphereTint, weight,
                  additive);
    ApplyChannels(add.toonTint, multiply.toonTint, delta.toonTint, weight,
                  additive);
    ApplyChannel(add.specularPower, multiply.specularPower,
                 delta.specularPower, weight, additive);
    ApplyChannel(add.edgeSize, multiply.edgeSize, delta.edgeSize, weight,
                 additive);
}

// Sparse result storage. The original entry order is retained as an ordinal so
// duplicate restore entries collapse to their last occurrence.
struct PositionEntry {
    std::uint32_t index = 0;
    std::uint32_t ordinal = 0;
    std::array<float, 3> value{};
};

struct UvEntry {
    std::uint32_t index = 0;
    std::uint32_t ordinal = 0;
    std::array<float, 4> value{};
};

// Sort by (index, ordinal) and keep the last base entry of every index. The
// vector is resized down to the unique count; its capacity deliberately stays
// at the restored entry count and is what the budget charges.
template <typename Entry>
void CompactByIndex(std::vector<Entry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const Entry& left, const Entry& right) {
                  if (left.index != right.index)
                      return left.index < right.index;
                  return left.ordinal < right.ordinal;
              });
    std::size_t write = 0;
    for (std::size_t read = 0; read < entries.size();) {
        std::size_t end = read + 1;
        while (end < entries.size() && entries[end].index == entries[read].index)
            ++end;
        if (write != end - 1) entries[write] = entries[end - 1];
        ++write;
        read = end;
    }
    entries.resize(write);
}

template <typename Entry>
Entry* FindEntry(std::vector<Entry>& entries, std::uint32_t index) {
    auto found = std::lower_bound(
        entries.begin(), entries.end(), index,
        [](const Entry& entry, std::uint32_t value) {
            return entry.index < value;
        });
    if (found == entries.end() || found->index != index) return nullptr;
    return &*found;
}

template <typename Entry>
const Entry* FindEntry(const std::vector<Entry>& entries,
                       std::uint32_t index) {
    auto found = std::lower_bound(
        entries.begin(), entries.end(), index,
        [](const Entry& entry, std::uint32_t value) {
            return entry.index < value;
        });
    if (found == entries.end() || found->index != index) return nullptr;
    return &*found;
}

void InitializeBones(std::vector<mdl::BoneMorphOffsetRecord>& bones) {
    for (std::size_t index = 0; index < bones.size(); ++index) {
        mdl::BoneMorphOffsetRecord& record = bones[index];
        std::memset(&record, 0, sizeof(record));
        record.boneIndex = static_cast<std::int32_t>(index);
        record.rotation[3] = 1.0f;
    }
}

void SetUnit(float* values, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) values[index] = 1.0f;
}

// Additive starts at zero, multiplicative at one (two reserved words stay
// zero), and every base byte comes from the frozen restore table.
void InitializeMaterials(
    std::vector<MorphMaterialEvaluation>& materials,
    const MorphDefinitionSnapshot& definitions) {
    for (std::size_t index = 0; index < materials.size(); ++index) {
        MorphMaterialEvaluation& material = materials[index];
        std::memset(&material, 0, sizeof(material));
        mdl::MaterialMorphChannels base{};
        if (!definitions.BaseMaterialAt(static_cast<std::uint32_t>(index),
                                        base))
            throw std::invalid_argument("morph evaluation material base");
        std::memcpy(&material.base, &base, sizeof(base));
        mdl::MaterialMorphChannels& multiply = material.multiplicative;
        SetUnit(multiply.diffuse, 4);
        SetUnit(multiply.specular, 3);
        SetUnit(multiply.ambient, 3);
        SetUnit(multiply.edgeColor, 4);
        SetUnit(multiply.textureTint, 4);
        SetUnit(multiply.sphereTint, 4);
        SetUnit(multiply.toonTint, 4);
        multiply.specularPower = 1.0f;
        multiply.edgeSize = 1.0f;
    }
}

void ComposeMaterialProperties(MorphMaterialEvaluation& material) {
    const mdl::MaterialMorphChannels& base = material.base;
    const mdl::MaterialMorphChannels& add = material.additive;
    const mdl::MaterialMorphChannels& multiply = material.multiplicative;
    const auto compose = [](float multiplier, float original, float added) {
        return multiplier * original + added;
    };
    for (std::size_t channel = 0; channel < 3; ++channel) {
        material.properties.diffuse[channel] =
            compose(multiply.diffuse[channel], base.diffuse[channel],
                    add.diffuse[channel]);
        material.properties.specular[channel] =
            compose(multiply.specular[channel], base.specular[channel],
                    add.specular[channel]);
        material.properties.ambient[channel] =
            compose(multiply.ambient[channel], base.ambient[channel],
                    add.ambient[channel]);
        material.properties.edgeColor[channel] =
            compose(multiply.edgeColor[channel], base.edgeColor[channel],
                    add.edgeColor[channel]);
    }
    material.properties.diffuse[3] =
        compose(multiply.diffuse[3], base.diffuse[3], add.diffuse[3]);
    material.properties.specularPower =
        compose(multiply.specularPower, base.specularPower, add.specularPower);
    material.properties.edgeColor[3] =
        compose(multiply.edgeColor[3], base.edgeColor[3], add.edgeColor[3]);
    material.properties.edgeSize =
        compose(multiply.edgeSize, base.edgeSize, add.edgeSize);
}

void CheckMaterialChannelsFinite(const mdl::MaterialMorphChannels& channels,
                                 const char* message) {
    RequireFinite(channels.diffuse, 4, message);
    RequireFinite(channels.specular, 3, message);
    RequireFinite(&channels.specularPower, 1, message);
    RequireFinite(channels.ambient, 3, message);
    RequireFinite(channels.edgeColor, 4, message);
    RequireFinite(&channels.edgeSize, 1, message);
    RequireFinite(channels.textureTint, 4, message);
    RequireFinite(channels.sphereTint, 4, message);
    RequireFinite(channels.toonTint, 4, message);
}

void CheckMaterialPropertiesFinite(const MorphMaterialProperties& properties,
                                   const char* message) {
    RequireFinite(properties.diffuse.data(), 4, message);
    RequireFinite(properties.specular.data(), 3, message);
    RequireFinite(&properties.specularPower, 1, message);
    RequireFinite(properties.ambient.data(), 3, message);
    RequireFinite(properties.edgeColor.data(), 4, message);
    RequireFinite(&properties.edgeSize, 1, message);
}

// PMD morphs only carry vertex offsets: the loader already remapped later
// entries through the first table, so the native expression is offset*weight.
void AccumPmdVertexMorph(std::vector<PositionEntry>& positions,
                         const MorphDefinitionSnapshot& definitions,
                         std::uint32_t morph,
                         const MorphDefinition& definition, float weight) {
    for (std::uint32_t entry = 0; entry < definition.vertexCount; ++entry) {
        mdl::PmdVertexMorphEntry value{};
        if (!definitions.VertexAt(morph, entry, value))
            throw std::invalid_argument("morph evaluation vertex entry");
        PositionEntry* position = FindEntry(positions, value.vertexIndex);
        if (!position) continue;
        position->value[0] += value.offset[0] * weight;
        position->value[1] += value.offset[1] * weight;
        position->value[2] += value.offset[2] * weight;
    }
}

// PMX type 1 offsets. The native ladder multiplies offset * morph weight *
// group weight one factor at a time; a direct contribution passes 1.0f.
void AccumPmxVertexMorph(std::vector<PositionEntry>& positions,
                         const MorphDefinitionSnapshot& definitions,
                         std::uint32_t morph,
                         const MorphDefinition& definition, float weight,
                         float groupWeight) {
    for (std::uint32_t entry = 0; entry < definition.vertexCount; ++entry) {
        mdl::PmdVertexMorphEntry value{};
        if (!definitions.VertexAt(morph, entry, value))
            throw std::invalid_argument("morph evaluation vertex entry");
        PositionEntry* position = FindEntry(positions, value.vertexIndex);
        if (!position) continue;
        position->value[0] += value.offset[0] * weight * groupWeight;
        position->value[1] += value.offset[1] * weight * groupWeight;
        position->value[2] += value.offset[2] * weight * groupWeight;
    }
}

// PMX type 3..7 offsets. Family zero only touches XY; the ZW words keep the
// raw restore bits.
void AccumPmxUvMorph(std::array<std::vector<UvEntry>, 5>& uvs,
                     const MorphDefinitionSnapshot& definitions,
                     std::uint32_t morph, const MorphDefinition& definition,
                     std::uint32_t family, float weight, float groupWeight) {
    for (std::uint32_t entry = 0; entry < definition.uvCounts[family];
         ++entry) {
        mdl::PmxUvMorphEntry value{};
        if (!definitions.UvAt(morph, family, entry, value))
            throw std::invalid_argument("morph evaluation UV entry");
        UvEntry* uv = FindEntry(uvs[family],
                                static_cast<std::uint32_t>(value.vertexIndex));
        if (!uv) continue;
        if (family == 0) {
            uv->value[0] += value.offset[0] * weight * groupWeight;
            uv->value[1] += value.offset[1] * weight * groupWeight;
        } else {
            for (std::size_t component = 0; component < 4; ++component)
                uv->value[component] +=
                    value.offset[component] * weight * groupWeight;
        }
    }
}

void AccumBoneMorph(std::vector<mdl::BoneMorphOffsetRecord>& bones,
                    const MorphDefinitionSnapshot& definitions,
                    std::uint32_t morph, const MorphDefinition& definition,
                    float weight) {
    const std::uint32_t workCount = static_cast<std::uint32_t>(bones.size());
    for (std::uint32_t entry = 0; entry < definition.boneCount; ++entry) {
        mdl::PmxBoneMorphEntry value{};
        if (!definitions.BoneAt(morph, entry, value))
            throw std::invalid_argument("morph evaluation bone entry");
        AccumBoneEntry(bones.data(), workCount, value, weight);
    }
}

// A -1 material index applies the entry to every material in ascending order,
// exactly like AccumMatMorph.
void AccumMaterialMorph(std::vector<MorphMaterialEvaluation>& materials,
                        const MorphDefinitionSnapshot& definitions,
                        std::uint32_t morph,
                        const MorphDefinition& definition, float weight) {
    for (std::uint32_t entry = 0; entry < definition.materialCount; ++entry) {
        mdl::PmxMaterialMorphEntry value{};
        if (!definitions.MaterialAt(morph, entry, value))
            throw std::invalid_argument("morph evaluation material entry");
        if (value.materialIndex == -1) {
            for (MorphMaterialEvaluation& material : materials)
                ApplyMaterialSlot(material, value, weight);
        } else if (value.materialIndex >= 0 &&
                   static_cast<std::size_t>(value.materialIndex) <
                       materials.size()) {
            ApplyMaterialSlot(
                materials[static_cast<std::size_t>(value.materialIndex)],
                value, weight);
        }
    }
}

}  // namespace

struct MorphEvaluationFrame::Impl {
    std::uint8_t physicsMode = 0;
    std::vector<PositionEntry> positions;
    std::array<std::vector<UvEntry>, 5> uvs;
    std::vector<mdl::BoneMorphOffsetRecord> bones;
    std::vector<MorphMaterialEvaluation> materials;
    std::uint64_t bytes = 0;
};

MorphEvaluationFrame::MorphEvaluationFrame() = default;
MorphEvaluationFrame::~MorphEvaluationFrame() = default;

std::unique_ptr<MorphEvaluationFrame> MorphEvaluationFrame::Evaluate(
    const MorphDefinitionSnapshot& definitions, const float* weights,
    std::uint32_t weightCount, std::uint64_t maxRetainedBytes) {
    const std::uint32_t morphCount = definitions.MorphCount();
    if (weightCount != morphCount)
        throw std::invalid_argument("morph evaluation weight count");
    if (weightCount != 0 && weights == nullptr)
        throw std::invalid_argument("morph evaluation weights");
    for (std::uint32_t index = 0; index < weightCount; ++index) {
        if (!std::isfinite(weights[index]))
            throw std::invalid_argument("morph evaluation weight");
    }

    const std::uint8_t physicsMode = definitions.PhysicsMode();
    const bool pmx = physicsMode == 2;
    const std::uint32_t baseVertexCount = definitions.BaseVertexCount();
    std::array<std::uint32_t, 5> baseUvCounts{};
    for (std::size_t family = 0; family < baseUvCounts.size(); ++family)
        baseUvCounts[family] =
            definitions.BaseUvCount(static_cast<std::uint32_t>(family));
    const std::uint32_t materialCount =
        pmx ? definitions.MaterialCount() : 0;

    // The bone work pool exists whenever any bone morph owns entries; it spans
    // every bone and is not sized by entry count.
    bool boneMorphEntries = false;
    if (pmx) {
        for (std::uint32_t morph = 0; morph < morphCount; ++morph) {
            MorphDefinition definition{};
            if (!definitions.MorphAt(morph, definition))
                throw std::invalid_argument("morph evaluation definition");
            if (definition.type == 2 && definition.boneCount != 0) {
                boneMorphEntries = true;
                break;
            }
        }
    }
    const std::uint32_t boneCount =
        boneMorphEntries ? definitions.BoneCount() : 0;

    // Reject an impossible retained budget before touching any payload. All
    // counts are charged through overflow-checked multiplications.
    std::uint64_t minimum = 0;
    Charge(minimum, 1,
           static_cast<std::uint64_t>(sizeof(MorphEvaluationFrame)) +
               static_cast<std::uint64_t>(sizeof(Impl)),
           maxRetainedBytes);
    Charge(minimum, baseVertexCount, sizeof(PositionEntry), maxRetainedBytes);
    for (const std::uint32_t count : baseUvCounts)
        Charge(minimum, count, sizeof(UvEntry), maxRetainedBytes);
    Charge(minimum, boneCount, sizeof(mdl::BoneMorphOffsetRecord),
           maxRetainedBytes);
    Charge(minimum, materialCount, sizeof(MorphMaterialEvaluation),
           maxRetainedBytes);

    // Nothing is published until every stage has succeeded; a throw destroys
    // the frame and every retained buffer.
    std::unique_ptr<MorphEvaluationFrame> frame(new MorphEvaluationFrame);
    frame->impl_ = std::make_unique<Impl>();
    Impl& impl = *frame->impl_;
    impl.physicsMode = physicsMode;
    impl.positions.resize(baseVertexCount);
    for (std::size_t family = 0; family < impl.uvs.size(); ++family)
        impl.uvs[family].resize(baseUvCounts[family]);
    impl.bones.resize(boneCount);
    impl.materials.resize(materialCount);

    // ---- restore every base entry in original order -----------------------
    for (std::uint32_t entry = 0; entry < baseVertexCount; ++entry) {
        mdl::PmdVertexMorphEntry base{};
        if (!definitions.BaseVertexAt(entry, base))
            throw std::invalid_argument("morph evaluation vertex base");
        PositionEntry& position = impl.positions[entry];
        position.index = base.vertexIndex;
        position.ordinal = entry;
        std::memcpy(position.value.data(), base.offset, sizeof(base.offset));
    }
    CompactByIndex(impl.positions);
    for (std::size_t family = 0; family < impl.uvs.size(); ++family) {
        std::vector<UvEntry>& entries = impl.uvs[family];
        for (std::uint32_t entry = 0; entry < baseUvCounts[family]; ++entry) {
            mdl::PmxUvMorphEntry base{};
            if (!definitions.BaseUvAt(static_cast<std::uint32_t>(family), entry,
                                      base))
                throw std::invalid_argument("morph evaluation UV base");
            UvEntry& uv = entries[entry];
            uv.index = static_cast<std::uint32_t>(base.vertexIndex);
            uv.ordinal = entry;
            std::memcpy(uv.value.data(), base.offset, sizeof(base.offset));
        }
        CompactByIndex(entries);
    }

    // ---- validate every active definition, independent of weights ---------
    // Each vertex and UV target needs a matching restore base. Group targets
    // need no extra rule; duplicates, self references and cycles stay as-is.
    for (std::uint32_t morph = 0; morph < morphCount; ++morph) {
        MorphDefinition definition{};
        if (!definitions.MorphAt(morph, definition))
            throw std::invalid_argument("morph evaluation definition");
        if (!pmx) {
            for (std::uint32_t entry = 0; entry < definition.vertexCount;
                 ++entry) {
                mdl::PmdVertexMorphEntry value{};
                if (!definitions.VertexAt(morph, entry, value))
                    throw std::invalid_argument(
                        "morph evaluation vertex entry");
                if (!FindEntry(impl.positions, value.vertexIndex))
                    throw std::invalid_argument(
                        "morph evaluation vertex restore");
            }
            continue;
        }
        switch (definition.type) {
        case 0:
            for (std::uint32_t entry = 0; entry < definition.groupCount;
                 ++entry) {
                mdl::PmxGroupMorphEntry value{};
                if (!definitions.GroupAt(morph, entry, value))
                    throw std::invalid_argument(
                        "morph evaluation group entry");
            }
            break;
        case 1:
            for (std::uint32_t entry = 0; entry < definition.vertexCount;
                 ++entry) {
                mdl::PmdVertexMorphEntry value{};
                if (!definitions.VertexAt(morph, entry, value))
                    throw std::invalid_argument(
                        "morph evaluation vertex entry");
                if (!FindEntry(impl.positions, value.vertexIndex))
                    throw std::invalid_argument(
                        "morph evaluation vertex restore");
            }
            break;
        case 2:
            for (std::uint32_t entry = 0; entry < definition.boneCount;
                 ++entry) {
                mdl::PmxBoneMorphEntry value{};
                if (!definitions.BoneAt(morph, entry, value))
                    throw std::invalid_argument(
                        "morph evaluation bone entry");
            }
            break;
        case 8:
            for (std::uint32_t entry = 0; entry < definition.materialCount;
                 ++entry) {
                mdl::PmxMaterialMorphEntry value{};
                if (!definitions.MaterialAt(morph, entry, value))
                    throw std::invalid_argument(
                        "morph evaluation material entry");
            }
            break;
        default: {
            if (definition.type < 3 || definition.type > 7) break;
            const std::uint32_t family =
                static_cast<std::uint32_t>(definition.type - 3);
            for (std::uint32_t entry = 0; entry < definition.uvCounts[family];
                 ++entry) {
                mdl::PmxUvMorphEntry value{};
                if (!definitions.UvAt(morph, family, entry, value))
                    throw std::invalid_argument("morph evaluation UV entry");
                if (!FindEntry(impl.uvs[family],
                               static_cast<std::uint32_t>(value.vertexIndex)))
                    throw std::invalid_argument(
                        "morph evaluation UV restore");
            }
            break;
        }
        }
    }

    InitializeBones(impl.bones);
    InitializeMaterials(impl.materials, definitions);

    // ---- accumulate in native morph order ---------------------------------
    if (!pmx) {
        for (std::uint32_t morph = 1; morph < morphCount; ++morph) {
            const float weight = weights[morph];
            if (weight == 0.0f) continue;
            MorphDefinition definition{};
            if (!definitions.MorphAt(morph, definition))
                throw std::invalid_argument("morph evaluation definition");
            AccumPmdVertexMorph(impl.positions, definitions, morph, definition,
                                weight);
        }
    } else {
        for (std::uint32_t morph = 0; morph < morphCount; ++morph) {
            const float weight = weights[morph];
            if (weight == 0.0f) continue;
            MorphDefinition definition{};
            if (!definitions.MorphAt(morph, definition))
                throw std::invalid_argument("morph evaluation definition");
            switch (definition.type) {
            case 0:
                for (std::uint32_t entry = 0; entry < definition.groupCount;
                     ++entry) {
                    mdl::PmxGroupMorphEntry reference{};
                    if (!definitions.GroupAt(morph, entry, reference))
                        throw std::invalid_argument(
                            "morph evaluation group entry");
                    if (reference.morphIndex < 0 ||
                        static_cast<std::uint32_t>(reference.morphIndex) >=
                            morphCount)
                        continue;
                    const std::uint32_t targetIndex =
                        static_cast<std::uint32_t>(reference.morphIndex);
                    MorphDefinition target{};
                    if (!definitions.MorphAt(targetIndex, target))
                        throw std::invalid_argument(
                            "morph evaluation definition");
                    // Group targets of type zero are ignored: expansion is one
                    // level only and the target still contributes on its own.
                    if (target.type == 1) {
                        AccumPmxVertexMorph(impl.positions, definitions,
                                            targetIndex, target, weight,
                                            reference.weight);
                    } else if (target.type == 2) {
                        const float groupWeight = weight * reference.weight;
                        AccumBoneMorph(impl.bones, definitions, targetIndex,
                                       target, groupWeight);
                    } else if (target.type >= 3 && target.type <= 7) {
                        AccumPmxUvMorph(
                            impl.uvs, definitions, targetIndex, target,
                            static_cast<std::uint32_t>(target.type - 3), weight,
                            reference.weight);
                    } else if (target.type == 8) {
                        const float groupWeight = weight * reference.weight;
                        AccumMaterialMorph(impl.materials, definitions,
                                           targetIndex, target, groupWeight);
                    }
                }
                break;
            case 1:
                AccumPmxVertexMorph(impl.positions, definitions, morph,
                                    definition, weight, 1.0f);
                break;
            case 2:
                AccumBoneMorph(impl.bones, definitions, morph, definition,
                               weight);
                break;
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
                AccumPmxUvMorph(impl.uvs, definitions, morph, definition,
                                static_cast<std::uint32_t>(definition.type - 3),
                                weight, 1.0f);
                break;
            case 8:
                AccumMaterialMorph(impl.materials, definitions, morph,
                                   definition, weight);
                break;
            default:
                break;
            }
        }
    }

    for (MorphMaterialEvaluation& material : impl.materials)
        ComposeMaterialProperties(material);

    // ---- only completed results are checked for nonfinite arithmetic ------
    for (const PositionEntry& position : impl.positions)
        RequireFinite(position.value.data(), 3,
                      "morph evaluation position overflow");
    for (std::size_t family = 0; family < impl.uvs.size(); ++family) {
        for (const UvEntry& uv : impl.uvs[family])
            RequireFinite(uv.value.data(), family == 0 ? 2 : 4,
                          "morph evaluation UV overflow");
    }
    for (const mdl::BoneMorphOffsetRecord& bone : impl.bones) {
        RequireFinite(bone.translation, 3,
                      "morph evaluation bone translation overflow");
        RequireFinite(bone.rotation, 4,
                      "morph evaluation bone rotation overflow");
    }
    for (const MorphMaterialEvaluation& material : impl.materials) {
        CheckMaterialChannelsFinite(material.additive,
                                    "morph evaluation material add overflow");
        CheckMaterialChannelsFinite(
            material.multiplicative,
            "morph evaluation material multiply overflow");
        CheckMaterialPropertiesFinite(material.properties,
                                      "morph evaluation material overflow");
    }

    // Final accounting uses the real capacities, which the standard does not
    // require to equal the requested sizes.
    std::uint64_t retained = 0;
    Charge(retained, 1,
           static_cast<std::uint64_t>(sizeof(MorphEvaluationFrame)) +
               static_cast<std::uint64_t>(sizeof(Impl)),
           maxRetainedBytes);
    Charge(retained,
           static_cast<std::uint64_t>(impl.positions.capacity()),
           sizeof(PositionEntry), maxRetainedBytes);
    for (const auto& family : impl.uvs)
        Charge(retained, static_cast<std::uint64_t>(family.capacity()),
               sizeof(UvEntry), maxRetainedBytes);
    Charge(retained, static_cast<std::uint64_t>(impl.bones.capacity()),
           sizeof(mdl::BoneMorphOffsetRecord), maxRetainedBytes);
    Charge(retained, static_cast<std::uint64_t>(impl.materials.capacity()),
           sizeof(MorphMaterialEvaluation), maxRetainedBytes);
    impl.bytes = retained;
    return frame;
}

std::uint8_t MorphEvaluationFrame::PhysicsMode() const noexcept {
    return impl_ ? impl_->physicsMode : 0;
}

std::uint64_t MorphEvaluationFrame::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : 0;
}

std::uint32_t MorphEvaluationFrame::PositionCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->positions.size()) : 0;
}

bool MorphEvaluationFrame::PositionIndexAt(std::uint32_t entry,
                                           std::uint32_t& out) const noexcept {
    if (!impl_ || entry >= impl_->positions.size()) return false;
    out = impl_->positions[entry].index;
    return true;
}

bool MorphEvaluationFrame::PositionAt(
    std::uint32_t vertex, std::array<float, 3>& out) const noexcept {
    if (!impl_) return false;
    const PositionEntry* position = FindEntry(impl_->positions, vertex);
    if (!position) return false;
    std::memcpy(out.data(), position->value.data(), sizeof(position->value));
    return true;
}

std::uint32_t MorphEvaluationFrame::UvCount(std::uint32_t family) const
    noexcept {
    if (!impl_ || family >= impl_->uvs.size()) return 0;
    return static_cast<std::uint32_t>(impl_->uvs[family].size());
}

bool MorphEvaluationFrame::UvIndexAt(std::uint32_t family,
                                     std::uint32_t entry,
                                     std::uint32_t& out) const noexcept {
    if (!impl_ || family >= impl_->uvs.size()) return false;
    const std::vector<UvEntry>& entries = impl_->uvs[family];
    if (entry >= entries.size()) return false;
    out = entries[entry].index;
    return true;
}

bool MorphEvaluationFrame::UvAt(std::uint32_t family, std::uint32_t vertex,
                                std::array<float, 4>& out) const noexcept {
    if (!impl_ || family >= impl_->uvs.size()) return false;
    const UvEntry* uv = FindEntry(impl_->uvs[family], vertex);
    if (!uv) return false;
    std::memcpy(out.data(), uv->value.data(), sizeof(uv->value));
    return true;
}

std::uint32_t MorphEvaluationFrame::BoneCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->bones.size()) : 0;
}

bool MorphEvaluationFrame::BoneAt(std::uint32_t bone,
                                  mdl::BoneMorphOffsetRecord& out) const
    noexcept {
    if (!impl_ || bone >= impl_->bones.size()) return false;
    std::memcpy(&out, &impl_->bones[bone], sizeof(out));
    return true;
}

std::uint32_t MorphEvaluationFrame::MaterialCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->materials.size()) : 0;
}

bool MorphEvaluationFrame::MaterialAt(std::uint32_t material,
                                      MorphMaterialEvaluation& out) const
    noexcept {
    if (!impl_ || material >= impl_->materials.size()) return false;
    std::memcpy(&out, &impl_->materials[material], sizeof(out));
    return true;
}

}  // namespace mikumikustudio
#endif
