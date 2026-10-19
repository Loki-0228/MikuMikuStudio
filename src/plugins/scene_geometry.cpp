#include "scene_geometry.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/d3dx_mesh.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include <cmath>
#include <numeric>
#include "scene_digest.hpp"

namespace mikumikustudio::plugins {
namespace {
template<class T, size_t N> void Copy(T (&out)[N], const T (&in)[N]) { std::copy(in, in + N, out); }
struct Geometry {
    std::vector<MmsVertexV1> vertices;
    std::vector<MmsTriangleV1> triangles;
    std::vector<MmsWeightV1> weights;
    std::vector<MmsMaterialV1> materials;
    std::vector<MmsTextureV1> textures;
};
MmsId Texture(SceneIdentity& ids, SceneData& data, Geometry& result,
    const void* token, uint32_t slot, const wchar_t* path) {
    if (!path || !*path) return {};
    MmsTextureV1 value{}; value.h = Header<MmsTextureV1>();
    value.id = ids.Element(token, MMS_CHANNEL_TEXTURES, slot);
    value.path = data.Keep(Utf8(path)); value.revision = 1;
    // No GPU resource is borrowed here. Zero metadata means unknown, and the
    // path describes the texture retained by MMS's loader (including fallback).
    result.textures.push_back(value); return value.id;
}
MmsMaterialV1 Material(SceneIdentity& ids, const void* token, uint32_t index) {
    MmsMaterialV1 value{}; value.h = Header<MmsMaterialV1>();
    value.id = value.sourceId = ids.Element(token, MMS_CHANNEL_MATERIALS, index);
    std::fill(value.textureMul, value.textureMul + 4, 1.f);
    std::fill(value.sphereMul, value.sphereMul + 4, 1.f);
    std::fill(value.toonMul, value.toonMul + 4, 1.f);
    return value;
}
MmsResult Model(SceneIdentity& ids, SceneData& data, Geometry& result, const void* token,
    bool needVertices, bool needWeights, bool needMaterials) {
    const auto& model = *mdl::Mdl(static_cast<const unsigned char*>(token));
    const bool pmx = model.physicsMode == 2;
    if (model.pmxAdditionalUvCount > 4 && pmx) return MMS_E_FAILED;
    if (model.materialCount && !model.materials) return MMS_E_NOT_READY;
    const auto* materials = static_cast<const mdl::ModelMaterialRecord*>(model.materials);
    if (needVertices) {
        if (model.indexCount % 3 || (model.indexCount && !model.indices)) return MMS_E_FAILED;
        if (model.vertexCount && (pmx ? !model.pmxVertices : !model.rawVertices)) return MMS_E_NOT_READY;
        result.vertices.resize(model.vertexCount); result.triangles.resize(model.indexCount / 3);
        if (needWeights) result.weights.resize(model.vertexCount);
        for (uint32_t i = 0; i < model.vertexCount; ++i) {
            auto& vertex = result.vertices[i]; MmsWeightV1 unused{};
            auto& weight = needWeights ? result.weights[i] : unused;
            vertex.sourceVertex = ids.Element(token, MMS_CHANNEL_MESH, i);
            int32_t boneIndices[4]{-1,-1,-1,-1};
            if (pmx) {
                const auto& source = model.pmxVertices[i];
                Copy(vertex.position, source.position); Copy(vertex.normal, source.normal); Copy(vertex.uv, source.uv);
                vertex.edgeScale = source.edgeScale;
                vertex.flags = source.hasPmxEdgeData ? 0 : MMS_VERTEX_EDGE_DISABLED;
                for (uint32_t set = 0; set < model.pmxAdditionalUvCount; ++set)
                    for (unsigned axis = 0; axis < 4; ++axis)
                        vertex.additionalUv[set * 4 + axis] = source.additionalUvByComponent[axis][set];
                weight.type = uint32_t(source.weightType);
                if (weight.type > MMS_WEIGHT_QDEF) return MMS_E_UNSUPPORTED;
                if (weight.type == MMS_WEIGHT_BDEF1) { boneIndices[0] = source.bone[0]; weight.weights[0] = 1; }
                else if (weight.type == MMS_WEIGHT_BDEF2 || weight.type == MMS_WEIGHT_SDEF) {
                    std::copy(source.bone, source.bone + 2, boneIndices);
                    weight.weights[0] = source.weight[0]; weight.weights[1] = 1.f - source.weight[0];
                } else { Copy(boneIndices, source.bone); Copy(weight.weights, source.weight); }
                if (weight.type == MMS_WEIGHT_SDEF) {
                    Copy(weight.sdefCenter, source.sdef.center); Copy(weight.sdefR0, source.sdef.r0Offset); Copy(weight.sdefR1, source.sdef.r1Offset);
                }
            } else {
                const auto& source = model.rawVertices[i];
                Copy(vertex.position, source.position); Copy(vertex.normal, source.normal); Copy(vertex.uv, source.uv);
                vertex.edgeScale = source.edgeDisabled ? 0.f : 1.f;
                vertex.flags = source.edgeDisabled ? MMS_VERTEX_EDGE_DISABLED : 0;
                weight.type = MMS_WEIGHT_BDEF2; boneIndices[0] = source.bone[0]; boneIndices[1] = source.bone[1];
                weight.weights[0] = source.weightPercent * .01f; weight.weights[1] = 1.f - weight.weights[0];
            }
            for (unsigned n = 0; n < 4; ++n) {
                if (boneIndices[n] >= 0 && uint32_t(boneIndices[n]) < model.boneCount)
                    weight.bones[n] = ids.Element(token, MMS_CHANNEL_BONES, uint32_t(boneIndices[n]));
                else if (weight.weights[n] != 0) return MMS_E_FAILED;
            }
        }
        // Morph evaluation mutates source working arrays. Restore only our copy.
        if (model.morph0Count && !model.morph0Table) return MMS_E_NOT_READY;
        for (uint32_t i = 0; i < model.morph0Count; ++i) {
            const auto& base = model.morph0Table[i];
            if (base.vertexIndex >= model.vertexCount) return MMS_E_FAILED;
            Copy(result.vertices[base.vertexIndex].position, base.offset);
        }
        if (pmx) for (unsigned family = 0; family < 5; ++family) {
            const auto count = model.uvMorphCounts.byFamily[family];
            const auto* table = model.uvMorphTables.byFamily[family];
            if (count < 0 || (count && !table)) return MMS_E_FAILED;
            for (int32_t i = 0; i < count; ++i) {
                const auto& base = table[i];
                if (base.vertexIndex < 0 || uint32_t(base.vertexIndex) >= model.vertexCount) return MMS_E_FAILED;
                auto& vertex = result.vertices[base.vertexIndex];
                if (!family) std::copy(base.offset, base.offset + 2, vertex.uv);
                else if (family <= model.pmxAdditionalUvCount) std::copy(base.offset, base.offset + 4, vertex.additionalUv + (family - 1) * 4);
            }
        }
        for (uint32_t i = 0; i < model.indexCount; ++i) {
            const uint32_t index = pmx ? static_cast<const uint32_t*>(model.indices)[i] : static_cast<const uint16_t*>(model.indices)[size_t(i) * 2];
            if (index >= model.vertexCount) return MMS_E_FAILED;
            result.triangles[i / 3].vertex[i % 3] = index;
        }
        size_t first = 0;
        for (uint32_t i = 0; i < model.materialCount; ++i) {
            const auto count = materials[i].faceVertexCount;
            if (count < 0 || count % 3 || size_t(count / 3) > result.triangles.size() - first) return MMS_E_FAILED;
            const auto id = ids.Element(token, MMS_CHANNEL_MATERIALS, i);
            for (int32_t n = 0; n < count / 3; ++n) result.triangles[first++].material = id;
        }
        if (first != result.triangles.size()) return MMS_E_FAILED;
    }
    if (needMaterials) for (uint32_t i = 0; i < model.materialCount; ++i) {
        const auto& source = materials[i]; auto value = Material(ids, token, i);
        value.name = data.Keep(Utf8(text_encoding::MaterialName(model, i, false)));
        Copy(value.diffuse, source.diffuse); Copy(value.ambient, source.ambient); Copy(value.specular, source.specular);
        value.power = source.specularPower; Copy(value.edgeColor, source.edgeColor); value.edgeSize = source.edgeSize;
        value.flags = pmx ? source.flags : (source.doubleSided ? 1u : 0u); value.sphereMode = source.sphereMode;
        if (pmx && model.materialMorphPools.base) {
            const auto& base = model.materialMorphPools.base[i].channels;
            Copy(value.diffuse, base.diffuse); Copy(value.ambient, base.ambient); Copy(value.specular, base.specular);
            value.power = base.specularPower; Copy(value.edgeColor, base.edgeColor); value.edgeSize = base.edgeSize;
            Copy(value.textureMul, base.textureTint); Copy(value.sphereMul, base.sphereTint); Copy(value.toonMul, base.toonTint);
        }
        value.texture = Texture(ids, data, result, token, i * 3, source.texturePath);
        value.sphere = Texture(ids, data, result, token, i * 3 + 1, source.spherePath);
        value.toon = Texture(ids, data, result, token, i * 3 + 2, source.toonPath);
        result.materials.push_back(value);
    }
    return MMS_OK;
}
// Read only the original accessory mesh retained by the loader. External
// deformation uses a separate native_mesh render object. Every lock is scoped.
template<unsigned Unlock> struct MeshLock {
    void* mesh; bool locked = false;
    ~MeshLock() { if (locked) d3dx::MeshMethod<HRESULT(WINAPI*)(void*), Unlock>(mesh)(mesh); }
};
MmsResult Accessory(SceneIdentity& ids, SceneData& data, Geometry& result, const void* token,
    bool needVertices, bool needWeights, bool needMaterials) {
    const auto& source = *static_cast<const mdl::AccessoryRecord*>(token);
    if (needVertices) {
        if (!source.mesh) return MMS_E_NOT_READY;
        if (d3dx::MeshFvf(source.mesh) != (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)) return MMS_E_UNSUPPORTED;
        const auto vertices = d3dx::MeshMethod<DWORD(WINAPI*)(void*), 5>(source.mesh)(source.mesh);
        const auto faces = d3dx::MeshMethod<DWORD(WINAPI*)(void*), 4>(source.mesh)(source.mesh);
        result.vertices.resize(vertices); result.triangles.resize(faces);
        if (needWeights) { result.weights.resize(vertices); for (auto& weight : result.weights) weight.type = MMS_WEIGHT_NONE; }
        if (vertices) {
            MeshLock<16> lock{source.mesh}; const float* raw = nullptr;
            const auto hr = d3dx::MeshMethod<HRESULT(WINAPI*)(void*, DWORD, const float**), 15>(source.mesh)(source.mesh, D3DLOCK_READONLY, &raw);
            if (FAILED(hr)) return MMS_E_NOT_READY;
            lock.locked = true; if (!raw) return MMS_E_FAILED;
            for (uint32_t i = 0; i < vertices; ++i) {
                auto& vertex = result.vertices[i]; std::memcpy(vertex.position, raw + size_t(i) * 8, 32);
                vertex.sourceVertex = ids.Element(token, MMS_CHANNEL_MESH, i); vertex.flags = MMS_VERTEX_EDGE_DISABLED;
            }
        }
        if (faces) {
            MeshLock<18> lock{source.mesh}; const void* raw = nullptr;
            const auto hr = d3dx::MeshMethod<HRESULT(WINAPI*)(void*, DWORD, const void**), 17>(source.mesh)(source.mesh, D3DLOCK_READONLY, &raw);
            if (FAILED(hr)) return MMS_E_NOT_READY;
            lock.locked = true; if (!raw) return MMS_E_FAILED;
            const bool wide = (d3dx::MeshOptions(source.mesh) & 1u) != 0;
            for (size_t i = 0; i < size_t(faces) * 3; ++i) {
                const uint32_t index = wide ? static_cast<const uint32_t*>(raw)[i] : static_cast<const uint16_t*>(raw)[i];
                if (index >= vertices) return MMS_E_FAILED; result.triangles[i / 3].vertex[i % 3] = index;
            }
        }
        if (faces) {
            MeshLock<25> lock{source.mesh}; const DWORD* raw = nullptr;
            const auto hr = d3dx::MeshMethod<HRESULT(WINAPI*)(void*, DWORD, const DWORD**), 24>(source.mesh)(source.mesh, D3DLOCK_READONLY, &raw);
            if (FAILED(hr)) return MMS_E_NOT_READY;
            lock.locked = true; if (!raw) return MMS_E_FAILED;
            for (uint32_t i = 0; i < faces; ++i) {
                if (raw[i] >= source.materialCount) return MMS_E_FAILED;
                result.triangles[i].material = ids.Element(token, MMS_CHANNEL_MATERIALS, raw[i]);
            }
        }
    }
    if (needMaterials) {
        if (source.materialCount && !source.materials) return MMS_E_NOT_READY;
        for (uint32_t i = 0; i < source.materialCount; ++i) {
            const auto& material = static_cast<const D3DMATERIAL9*>(source.materials)[i]; auto value = Material(ids, token, i);
            value.name = data.Keep("subset" + std::to_string(i));
            std::memcpy(value.diffuse, &material.Diffuse, 16); std::memcpy(value.ambient, &material.Ambient, 12);
            std::memcpy(value.specular, &material.Specular, 12); value.power = material.Power;
            if (source.texturePaths) {
                const auto* paths = reinterpret_cast<const wchar_t*>(source.texturePaths[i]);
                const auto type = source.textureTypes ? source.textureTypes[i] : 0;
                if (type == 1 || type == 2) { value.sphere = Texture(ids, data, result, token, i * 3 + 1, paths); value.sphereMode = type; }
                else {
                    value.texture = Texture(ids, data, result, token, i * 3, paths);
                    value.sphere = Texture(ids, data, result, token, i * 3 + 1, paths + 512);
                    value.sphereMode = type == 4 ? 1u : type == 5 ? 2u : 0u;
                }
            }
            result.materials.push_back(value);
        }
    }
    return MMS_OK;
}
}
MmsResult CaptureBindGeometry(SceneIdentity& ids, SceneData& data, SceneData::Channels& record,
    const void* token, uint32_t kind, uint64_t channels, GeometryDigests& digests) {
    const bool vertices = (channels & (MMS_CHANNEL_MESH | MMS_CHANNEL_BINDING)) != 0;
    const bool weights = (channels & MMS_CHANNEL_BINDING) != 0;
    const bool materials = (channels & (MMS_CHANNEL_MATERIALS | MMS_CHANNEL_TEXTURES)) != 0;
    if (!vertices && !materials) return MMS_OK;
    Geometry result;
    const auto status = kind == MMS_OBJECT_MODEL ? Model(ids, data, result, token, vertices, weights, materials) :
        Accessory(ids, data, result, token, vertices, weights, materials);
    if (status != MMS_OK) return status;
    if (vertices) { Digest digest; digest.Add(result.vertices); digest.Add(result.triangles); digests[MMS_CHANNEL_MESH] = digest.Finish(); }
    if (weights) { Digest digest; digest.Add(result.weights); digests[MMS_CHANNEL_BINDING] = digest.Finish(); }
    if (materials) {
        Digest digest;
        const uint64_t count = result.materials.size(); digest.Add(&count, sizeof(count));
        for (const auto& material : result.materials) {
            digest.Add(&material.id, sizeof(material.id)); digest.Add(&material.name.bytes, sizeof(material.name.bytes));
            digest.Add(material.name.data, size_t(material.name.bytes));
            digest.Add(material.diffuse, sizeof(material) - offsetof(MmsMaterialV1, diffuse));
        }
        for (const auto& texture : result.textures) {
            digest.Add(&texture.id, sizeof(texture.id)); digest.Add(&texture.path.bytes, sizeof(texture.path.bytes));
            digest.Add(texture.path.data, size_t(texture.path.bytes));
        }
        digests[MMS_CHANNEL_MATERIALS] = digest.Finish();
    }
    MmsMeshV1 mesh{}; mesh.h = Header<MmsMeshV1>();
    if (!result.vertices.empty()) {
        Copy(mesh.boundsMin, result.vertices.front().position); Copy(mesh.boundsMax, mesh.boundsMin);
        for (const auto& vertex : result.vertices) for (unsigned axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(vertex.position[axis])) return MMS_E_FAILED;
            mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], vertex.position[axis]);
            mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], vertex.position[axis]);
        }
    }
    if (vertices) { mesh.vertices = data.Keep(std::move(result.vertices)); mesh.triangles = data.Keep(std::move(result.triangles)); }
    if (channels & MMS_CHANNEL_MESH) record.values.emplace(MMS_CHANNEL_MESH, data.Keep(std::vector<MmsMeshV1>{mesh}));
    if (weights) {
        MmsBindingV1 binding{}; binding.h = Header<MmsBindingV1>(); binding.bindVertices = binding.morphVertices = mesh.vertices;
        binding.weights = data.Keep(std::move(result.weights));
        std::vector<uint32_t> mapping(size_t(mesh.vertices.count)); std::iota(mapping.begin(), mapping.end(), 0u);
        binding.vertexBindings = data.Keep(std::move(mapping));
        record.values.emplace(MMS_CHANNEL_BINDING, data.Keep(std::vector<MmsBindingV1>{binding}));
    }
    if (channels & MMS_CHANNEL_MATERIALS) record.values.emplace(MMS_CHANNEL_MATERIALS, data.Keep(std::move(result.materials)));
    if (channels & MMS_CHANNEL_TEXTURES) record.values.emplace(MMS_CHANNEL_TEXTURES, data.Keep(std::move(result.textures)));
    return MMS_OK;
}
}
