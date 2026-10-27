#include "mikumikustudio/mesh_definition_snapshot.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/model.hpp"
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace mikumikustudio {
namespace {
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride,
            std::uint64_t limit) {
    if (bytes > limit || (stride && count > (limit - bytes) / stride))
        throw std::length_error("mesh definition snapshot budget");
    bytes += count * stride;
}
template<std::size_t N>
void Copy(std::array<float, N>& out, const float* in) noexcept {
    std::memcpy(out.data(), in, N * sizeof(float));
}
template<std::size_t N>
bool Finite(const std::array<float, N>& values) noexcept {
    for (float value : values) if (!std::isfinite(value)) return false;
    return true;
}
void ValidateVertex(const MeshDefinitionVertex& vertex, unsigned additionalUv) {
    if (!Finite(vertex.position) || !Finite(vertex.normal) || !Finite(vertex.uv) ||
        !Finite(vertex.weights) || !std::isfinite(vertex.edgeScale))
        throw std::invalid_argument("mesh definition snapshot vertex values");
    for (unsigned group = 0; group < additionalUv; ++group)
        if (!Finite(vertex.additionalUv[group]))
            throw std::invalid_argument("mesh definition snapshot additional UV");
    if (vertex.binding == MeshBindingType::Sdef &&
        (!Finite(vertex.sdefCenter) || !Finite(vertex.sdefR0Offset) || !Finite(vertex.sdefR1Offset)))
        throw std::invalid_argument("mesh definition snapshot SDEF values");
}
}

struct MeshDefinitionSnapshot::Impl {
    std::vector<MeshDefinitionVertex> vertices;
    std::vector<MeshDefinitionTriangle> triangles;
    std::vector<MeshDefinitionMaterial> materials;
    std::uint32_t boneCount = 0, indexCount = 0;
    std::uint8_t physicsMode = 0, additionalUvCount = 0;
    std::uint64_t bytes = 0;
};
MeshDefinitionSnapshot::MeshDefinitionSnapshot() = default;
MeshDefinitionSnapshot::~MeshDefinitionSnapshot() = default;

std::unique_ptr<MeshDefinitionSnapshot> MeshDefinitionSnapshot::Capture(
    const unsigned char* source, std::uint64_t limit) {
    if (!source) throw std::invalid_argument("mesh definition snapshot source");
    const auto& model = *mdl::Mdl(source);
    const bool pmx = model.physicsMode == 2;
    const std::uint64_t maxCount = (std::numeric_limits<std::int32_t>::max)();
    const auto vertices = model.vertexCount, indices = model.indexCount, materials = model.materialCount;
    if (vertices > maxCount || indices > maxCount || materials > maxCount ||
        model.boneCount > maxCount || model.morph0Count > maxCount || indices % 3)
        throw std::invalid_argument("mesh definition snapshot counts");
    if ((vertices && (pmx ? !model.pmxVertices : !model.rawVertices)) ||
        (indices && !model.indices) || (materials && !model.materials) ||
        (model.morph0Count && !model.morph0Table))
        throw std::invalid_argument("mesh definition snapshot tables");
    const unsigned additionalUv = pmx ? model.pmxAdditionalUvCount : 0;
    if (additionalUv > 4)
        throw std::invalid_argument("mesh definition snapshot UV count");
    std::array<std::uint32_t, 5> uvCounts{};
    if (pmx) for (unsigned family = 0; family < 5; ++family) {
        const auto count = model.uvMorphCounts.byFamily[family];
        if (count < 0 || (count && !model.uvMorphTables.byFamily[family]))
            throw std::invalid_argument("mesh definition snapshot UV restore table");
        uvCounts[family] = static_cast<std::uint32_t>(count);
    }

    // No vertex, face, material or restore payload is walked until the whole
    // minimum retained result fits. PMD's source indices have a four-byte
    // host stride; the result stores only their numeric values.
    const std::uint64_t base = sizeof(MeshDefinitionSnapshot) + sizeof(Impl);
    std::uint64_t bytes = 0;
    Charge(bytes, 1, base, limit);
    Charge(bytes, vertices, sizeof(MeshDefinitionVertex), limit);
    Charge(bytes, indices / 3, sizeof(MeshDefinitionTriangle), limit);
    Charge(bytes, materials, sizeof(MeshDefinitionMaterial), limit);

    auto result = std::unique_ptr<MeshDefinitionSnapshot>(new MeshDefinitionSnapshot());
    result->impl_ = std::make_unique<Impl>();
    auto& value = *result->impl_;
    value.physicsMode = model.physicsMode;
    value.additionalUvCount = static_cast<std::uint8_t>(additionalUv);
    value.boneCount = model.boneCount;
    value.indexCount = indices;
    value.vertices.resize(vertices);
    Charge(bytes, value.vertices.capacity() - vertices, sizeof(MeshDefinitionVertex), limit);
    value.triangles.resize(indices / 3);
    Charge(bytes, value.triangles.capacity() - indices / 3, sizeof(MeshDefinitionTriangle), limit);
    value.materials.resize(materials);
    Charge(bytes, value.materials.capacity() - materials, sizeof(MeshDefinitionMaterial), limit);

    for (std::uint32_t index = 0; index < vertices; ++index) {
        auto& out = value.vertices[index];
        if (pmx) {
            const auto& in = model.pmxVertices[index];
            switch (in.weightType) {
            case mdl::PmxWeightType::bdef1:
                out.binding = MeshBindingType::Bdef1;
                out.bones[0] = in.bone[0]; out.weights[0] = 1.0f;
                break;
            case mdl::PmxWeightType::bdef2:
            case mdl::PmxWeightType::sdef:
                out.binding = in.weightType == mdl::PmxWeightType::sdef ? MeshBindingType::Sdef : MeshBindingType::Bdef2;
                std::memcpy(out.bones.data(), in.bone, 2 * sizeof(std::int32_t));
                std::memcpy(out.weights.data(), in.weight, sizeof(float));
                out.weights[1] = 1.0f - out.weights[0];
                if (out.binding == MeshBindingType::Sdef) {
                    Copy(out.sdefCenter, in.sdef.center);
                    Copy(out.sdefR0Offset, in.sdef.r0Offset);
                    Copy(out.sdefR1Offset, in.sdef.r1Offset);
                }
                break;
            case mdl::PmxWeightType::bdef4:
                out.binding = MeshBindingType::Bdef4;
                std::memcpy(out.bones.data(), in.bone, sizeof(in.bone));
                Copy(out.weights, in.weight);
                break;
            default: throw MeshDefinitionUnsupported();
            }
            Copy(out.position, in.position); Copy(out.normal, in.normal); Copy(out.uv, in.uv);
            for (unsigned group = 0; group < additionalUv; ++group)
                for (unsigned component = 0; component < 4; ++component)
                    std::memcpy(&out.additionalUv[group][component],
                        &in.additionalUvByComponent[component][group], sizeof(float));
            out.edgeScale = in.edgeScale;
            out.pmxEdgeData = in.hasPmxEdgeData;
            if (in.materialIndex < 0 || static_cast<std::uint32_t>(in.materialIndex) >= materials)
                throw std::invalid_argument("mesh definition snapshot vertex material");
            out.materialIndex = in.materialIndex;
        } else {
            const auto& in = model.rawVertices[index];
            Copy(out.position, in.position); Copy(out.normal, in.normal); Copy(out.uv, in.uv);
            out.binding = MeshBindingType::Bdef2;
            out.bones[0] = in.bone[0]; out.bones[1] = in.bone[1];
            out.pmdWeightPercent = in.weightPercent;
            out.weights[0] = static_cast<float>(in.weightPercent) * 0.009999999776482582f;
            out.weights[1] = 1.0f - out.weights[0];
            out.pmdEdgeDisabled = in.edgeDisabled;
            out.edgeScale = in.edgeDisabled ? 0.0f : 1.0f;
        }
    }

    // Live working positions and UVs can contain the current morph result.
    // Apply original restore entries in order to the owned copy only. UV0's
    // Z/W are not geometry and are deliberately neither read nor validated.
    for (std::uint32_t entry = 0; entry < model.morph0Count; ++entry) {
        const auto& baseVertex = model.morph0Table[entry];
        if (baseVertex.vertexIndex >= vertices)
            throw std::invalid_argument("mesh definition snapshot position restore index");
        std::array<float, 3> position; Copy(position, baseVertex.offset);
        if (!Finite(position)) throw std::invalid_argument("mesh definition snapshot position restore values");
        value.vertices[baseVertex.vertexIndex].position = position;
    }
    if (pmx) for (unsigned family = 0; family < 5; ++family) {
        for (std::uint32_t entry = 0; entry < uvCounts[family]; ++entry) {
            const auto& baseUv = model.uvMorphTables.byFamily[family][entry];
            if (baseUv.vertexIndex < 0 || static_cast<std::uint32_t>(baseUv.vertexIndex) >= vertices)
                throw std::invalid_argument("mesh definition snapshot UV restore index");
            auto& vertex = value.vertices[static_cast<std::uint32_t>(baseUv.vertexIndex)];
            if (!family) {
                std::array<float, 2> uv; Copy(uv, baseUv.offset);
                if (!Finite(uv)) throw std::invalid_argument("mesh definition snapshot UV restore values");
                vertex.uv = uv;
            } else if (family <= additionalUv) {
                std::array<float, 4> uv; Copy(uv, baseUv.offset);
                if (!Finite(uv)) throw std::invalid_argument("mesh definition snapshot additional UV restore values");
                vertex.additionalUv[family - 1] = uv;
            }
        }
    }
    for (const auto& vertex : value.vertices) ValidateVertex(vertex, additionalUv);

    for (std::uint32_t index = 0; index < indices; ++index) {
        std::uint32_t vertex = 0;
        if (pmx) {
            const auto raw = static_cast<const std::int32_t*>(model.indices)[index];
            if (raw < 0) throw std::invalid_argument("mesh definition snapshot negative face index");
            vertex = static_cast<std::uint32_t>(raw);
        } else {
            vertex = static_cast<const std::uint16_t*>(model.indices)[std::size_t(index) * 2];
        }
        if (vertex >= vertices) throw std::invalid_argument("mesh definition snapshot face index");
        value.triangles[index / 3].vertices[index % 3] = vertex;
    }
    const auto* sourceMaterials = static_cast<const mdl::ModelMaterialRecord*>(model.materials);
    std::uint32_t first = 0;
    for (std::uint32_t material = 0; material < materials; ++material) {
        const auto count = sourceMaterials[material].faceVertexCount;
        if (count < 0 || count % 3 || static_cast<std::uint32_t>(count) > indices - first)
            throw std::invalid_argument("mesh definition snapshot material range");
        const auto size = static_cast<std::uint32_t>(count);
        value.materials[material] = {first, size};
        for (std::uint32_t triangle = first / 3; triangle < (first + size) / 3; ++triangle)
            value.triangles[triangle].materialIndex = material;
        first += size;
    }
    if (first != indices) throw std::invalid_argument("mesh definition snapshot incomplete material ranges");
    value.bytes = bytes;
    return result;
}

std::uint8_t MeshDefinitionSnapshot::PhysicsMode() const noexcept { return impl_->physicsMode; }
std::uint8_t MeshDefinitionSnapshot::AdditionalUvCount() const noexcept { return impl_->additionalUvCount; }
std::uint32_t MeshDefinitionSnapshot::BoneCount() const noexcept { return impl_->boneCount; }
std::uint32_t MeshDefinitionSnapshot::VertexCount() const noexcept { return static_cast<std::uint32_t>(impl_->vertices.size()); }
std::uint32_t MeshDefinitionSnapshot::IndexCount() const noexcept { return impl_->indexCount; }
std::uint32_t MeshDefinitionSnapshot::TriangleCount() const noexcept { return static_cast<std::uint32_t>(impl_->triangles.size()); }
std::uint32_t MeshDefinitionSnapshot::MaterialCount() const noexcept { return static_cast<std::uint32_t>(impl_->materials.size()); }
std::uint64_t MeshDefinitionSnapshot::RetainedBytes() const noexcept { return impl_->bytes; }
bool MeshDefinitionSnapshot::VertexAt(std::uint32_t index, MeshDefinitionVertex& out) const noexcept {
    if (index >= impl_->vertices.size()) return false;
    out = impl_->vertices[index]; return true;
}
bool MeshDefinitionSnapshot::IndexAt(std::uint32_t index, std::uint32_t& out) const noexcept {
    if (index >= impl_->indexCount) return false;
    out = impl_->triangles[index / 3].vertices[index % 3]; return true;
}
bool MeshDefinitionSnapshot::TriangleAt(std::uint32_t index, MeshDefinitionTriangle& out) const noexcept {
    if (index >= impl_->triangles.size()) return false;
    out = impl_->triangles[index]; return true;
}
bool MeshDefinitionSnapshot::MaterialAt(std::uint32_t index, MeshDefinitionMaterial& out) const noexcept {
    if (index >= impl_->materials.size()) return false;
    out = impl_->materials[index]; return true;
}
}
#endif
