#include "mikumikustudio/perf_timing.hpp"
#include "mikumikustudio/geometry_workers.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
// ===========================================================================
// VA 0x004B0C50 / 0x004A9400 - per-frame model VB deformation
// ===========================================================================
// 0x4B0C50 restores and accumulates vertex morphs, locks model+8/model+12,
// and dispatches one of the 0x4A9400-family OpenMP skinning workers. This
// file closes the PMD path serially and the x64 PMX twin family in full:
// the five stride workers selected by the additional-UV count (x64
// 0x14011F3A0 / 0x140120D20 / 0x140122720 / 0x140124150 / 0x140125BF0),
// the per-frame PMX vertex/UV morph restore+apply pass that feeds them
// (x64 0x1400E1415..0x1400E2670), and the SDEF quaternion blend helper
// (x64 0x1400E2D20). The PMD arithmetic keeps its x87 __asm bit-parity
// branch; the PMX arithmetic follows the x64 SSE orderings, which is the
// behavior baseline for this port.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <algorithm>

#include "mikumikustudio/affine_skinning.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"

namespace mikumikustudio {
namespace {

// Porting-era VB dump under MIKUMIKUSTUDIO_VB_DUMP_DIR (CMake option
// MIKUMIKUSTUDIO_DIAG, default OFF); the OFF stub below keeps the call
// site valid and inlines away to nothing.
#ifdef MIKUMIKUSTUDIO_DIAG
bool ModelVbCaptureReady(char directory[MAX_PATH]) {
    const DWORD length = GetEnvironmentVariableA(
        "MIKUMIKUSTUDIO_VB_DUMP_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    char stable[2]{};
    if (GetEnvironmentVariableA("MIKUMIKUSTUDIO_AB_STABLE_CAPTURE", stable,
                                sizeof(stable)) != 1 || stable[0] != '1')
        return true;

    char requireLine[2]{};
    const bool refreshed =
        GetEnvironmentVariableA("MIKUMIKUSTUDIO_AB_REQUIRE_LINE", requireLine,
                                sizeof(requireLine)) == 1 &&
        requireLine[0] == '1';
    char gate[MAX_PATH]{};
    std::snprintf(gate, sizeof(gate), "%s\\%s", directory,
                  refreshed ? "vb.capture.active" : "vb.capture.ready");
    const DWORD attributes = GetFileAttributesA(gate);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void WriteBytes(const char* path, const void* bytes, DWORD size) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, bytes, size, &written, nullptr);
    CloseHandle(file);
}

void DumpModelVertexBuffers(MMDApp* app, unsigned char* model,
                            const mdl::SkinnedVertexBase* mainVertices,
                            const mdl::EdgeVertex* edgeVertices,
                            std::uint32_t count,
                            std::uint32_t mainStride) {
    char directory[MAX_PATH]{};
    if (!ModelVbCaptureReady(directory))
        return;

    int slot = -1;
    // port diagnostic: bound = model-slot capacity (kModelSlotCount)
    for (int i = 0; i < kModelSlotCount; ++i) {
        if (app->ModelSlot(i) == model) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return;

    // sized to the slot capacity: the old [100] let a slot >= 100 index past
    // the array (out-of-bounds InterlockedCompareExchange write)
    static LONG dumped[kModelSlotCount]{};
    if (InterlockedCompareExchange(&dumped[slot], 1, 0) != 0)
        return;

    CreateDirectoryA(directory, nullptr);
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\model.%03d.main.bin",
                  directory, slot);
    WriteBytes(path, mainVertices, mainStride * count);
    std::snprintf(path, sizeof(path), "%s\\model.%03d.edge.bin",
                  directory, slot);
    WriteBytes(path, edgeVertices, sizeof(*edgeVertices) * count);
    std::snprintf(path, sizeof(path), "%s\\model.%03d.meta.json",
                  directory, slot);
    char metadata[160]{};
    const mdl::ModelRecord& state = *mdl::Mdl(model);
    const int chars = std::snprintf(
        metadata, sizeof(metadata),
        "{\"schema\":1,\"slot\":%d,\"format\":%u,"
        "\"vertex_count\":%u,\"main_stride\":%u,\"edge_stride\":16}\n",
        slot, static_cast<unsigned>(state.physicsMode),
        count, mainStride);
    WriteBytes(path, metadata, static_cast<DWORD>(chars));
}
#else
inline void DumpModelVertexBuffers(MMDApp*, unsigned char*,
                                   const mdl::SkinnedVertexBase*,
                                   const mdl::EdgeVertex*, std::uint32_t,
                                   std::uint32_t) {}
#endif

// The skinning transforms, the weight blends and the SDEF quaternion blend
// live in mikumikustudio/affine_skinning.hpp together with the per-bone
// affine precomputation; this file keeps the model-record plumbing.
void TransformPosition(float out[3], const float in[3], const float* m) {
    affine::TransformPosition(out, in, m);
}

float EdgeComponent(float position, float normal, float amount) {
#if defined(_M_IX86)
    float result = 0.0f;
    __asm {
        fld dword ptr [normal]
        fmul dword ptr [amount]
        fadd dword ptr [position]
        fstp dword ptr [result]
    }
    return result;
#else
    return position + normal * amount;
#endif
}

// x64 fetches one matInit per referenced bone and only tests the sign of
// the index; a negative reference contributes an all-zero matrix, not an
// identity. The upper bound is a port-side guard against corrupt files -
// every loader-produced index is inside the table.
constexpr float kPmxZeroMatrix[16] = {
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
};

// Affine precomputation for a missing bone reference: rigid, identity stretch,
// the zero matrix's own inverse transpose, and the shader's missing-bone
// quaternion, so CPU and GPU agree even on corrupt files.
constexpr affine::BoneAux kMissingBoneAux = {
    {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {0.5f, 0.0f, 0.0f, 0.0f},
    0u,
};

affine::BoneRef PmxSkinBone(const mdl::BoneRecord* bones,
                            const affine::BoneAux* auxTable, int boneCount,
                            std::int32_t boneIndex) {
    if (boneIndex >= 0 && boneIndex < boneCount)
        return {bones[boneIndex].matInit, &auxTable[boneIndex]};
    return {kPmxZeroMatrix, &kMissingBoneAux};
}

// The affine_skinning SDEF kernels take their two D3DX entry points as
// parameters so the header stays D3DX-free; production injects the real ones
// (bit parity with the x64 original) and the tests inject the header's
// reference implementations.
void QuatFromMatrixD3dx(float out[4], const float matrix[16]) {
    d3dx::Get().quatFromMatrix(out,
                               reinterpret_cast<const d3dx::D3DXMATRIXF*>(matrix));
}

void MatrixFromQuaternionD3dx(float out[16], const float quaternion[4]) {
    d3dx::Get().matrixRotationQuaternion(
        reinterpret_cast<d3dx::D3DXMATRIXF*>(out), quaternion);
}

const affine::RotationOps kD3dxRotationOps = {QuatFromMatrixD3dx,
                                              MatrixFromQuaternionD3dx};

// Per-bone affine precomputation: polar factors, inverse-transpose normal
// matrix and the rigid test, computed once per bone per frame. The table is
// read-only inside the OpenMP loops and is a call-local allocation - no shared
// static cache.
void BuildPmxBoneAux(const mdl::BoneRecord* bones, int boneCount,
                     d3dx::Api& d3dxApi, std::vector<affine::BoneAux>& aux) {
    aux.resize(boneCount);
    for (int i = 0; i < boneCount; ++i) {
        float rotation[4];
        d3dxApi.quatFromMatrix(
            rotation, reinterpret_cast<const d3dx::D3DXMATRIXF*>(bones[i].matInit));
        affine::PrecomputeBone(bones[i].matInit, rotation, aux[i]);
    }
}

void BuildPmdBoneAux(const mdl::BoneRecord* bones, int boneCount,
                     std::vector<affine::BoneAux>& aux) {
    // PMD has no SDEF vertices, so no rotation quaternion is needed here.
    aux.resize(boneCount);
    for (int i = 0; i < boneCount; ++i)
        affine::PrecomputeBone(bones[i].matInit, nullptr, aux[i]);
}

void StoreSkinned(mdl::SkinnedVertexBase& destination,
                  const affine::Skinned& skinned) {
    std::memcpy(destination.position, skinned.position, sizeof(skinned.position));
    std::memcpy(destination.normal, skinned.normal, sizeof(skinned.normal));
}

// x64 PMX worker 0x14011F991: BDEF1 transforms position with the full
// matrix and normal without translation into the 32-byte base record. With
// bone scaling the normal runs through the per-bone inverse transpose and is
// normalized again (affine_skinning.hpp).
void SkinPmxBdef1(const mdl::PmxVertex& source,
                  const mdl::BoneRecord* bones,
                  const affine::BoneAux* auxTable, int boneCount,
                  mdl::SkinnedVertexBase& destination) {
    affine::Skinned skinned;
    affine::SkinBdef1(PmxSkinBone(bones, auxTable, boneCount, source.bone[0]),
                      source.position, source.normal, skinned);
    StoreSkinned(destination, skinned);
}

// x64 PMX worker 0x14011F513: BDEF2 uses bone 1 first with (1-weight),
// then adds bone 0 scaled by weight. affine::SkinBdef2 preserves that
// source order on the x64 path (and bit-exactly on the x87 path).
void SkinPmxBdef2(const mdl::PmxVertex& source,
                  const mdl::BoneRecord* bones,
                  const affine::BoneAux* auxTable, int boneCount,
                  mdl::SkinnedVertexBase& destination) {
    affine::Skinned skinned;
    affine::SkinBdef2(PmxSkinBone(bones, auxTable, boneCount, source.bone[0]),
                      PmxSkinBone(bones, auxTable, boneCount, source.bone[1]),
                      source.weight[0], source.position, source.normal, skinned);
    StoreSkinned(destination, skinned);
}

// x64 PMX worker 0x14011FB7F: BDEF4 retrieves all four matrices with the
// same negative-index zero fallback. The scalar chains are ordered
// differently for positions and normals in the original: the position
// accumulates bone 1 first and then bones 0, 2 and 3, while the normal
// starts from bone 0. Float addition does not associate, so both orders
// are spelled out in affine::SkinBdef4 instead of being hidden in one
// generic loop.
void SkinPmxBdef4(const mdl::PmxVertex& source,
                  const mdl::BoneRecord* bones,
                  const affine::BoneAux* auxTable, int boneCount,
                  mdl::SkinnedVertexBase& destination) {
    const affine::BoneRef refs[4] = {
        PmxSkinBone(bones, auxTable, boneCount, source.bone[0]),
        PmxSkinBone(bones, auxTable, boneCount, source.bone[1]),
        PmxSkinBone(bones, auxTable, boneCount, source.bone[2]),
        PmxSkinBone(bones, auxTable, boneCount, source.bone[3]),
    };
    affine::Skinned skinned;
    affine::SkinBdef4(refs, source.weight, source.position, source.normal,
                      skinned);
    StoreSkinned(destination, skinned);
}

// x64 PMX worker 0x140120285. The loader has already rebased R0/R1 onto the
// weighted SDEF midpoint, which makes the blend stable at the bind pose.
// For every component the original computes
//   Pc = M0(C)*w0 + M1(C)*(1-w0)            (full affine transforms)
//   U  = M0linear(R0)*w0 + M1linear(R1)*(1-w0)  (no translation)
//   center = ((U + Pc) + Pc) * 0.5          (exact x64 folding)
// and places the vertex at R * (position - C) + center, where R comes from
// D3DXQuaternionRotationMatrix on both bone matrices, the nlerp in
// affine::BlendSdefQuaternions, and D3DXMatrixRotationQuaternion - the very
// entry points the x64 imports, so even degenerate zero-matrix references
// produce identical bits. That exact path is kept verbatim for rigid bones;
// affine::SkinSdef switches to the polar rotation and the blended stretch as
// soon as one bone carries scaling or shear.
void SkinPmxSdef(const mdl::PmxVertex& source,
                 const mdl::BoneRecord* bones,
                 const affine::BoneAux* auxTable, int boneCount,
                 mdl::SkinnedVertexBase& destination) {
    affine::Skinned skinned;
    affine::SkinSdef(PmxSkinBone(bones, auxTable, boneCount, source.bone[0]),
                     PmxSkinBone(bones, auxTable, boneCount, source.bone[1]),
                     source.weight[0], source.position, source.normal,
                     source.sdef.center, source.sdef.r0Offset,
                     source.sdef.r1Offset, kD3dxRotationOps, skinned);
    StoreSkinned(destination, skinned);
}

// x64 PMX common tail 0x140120A01/0x140120A9C: both branches truncate the
// material's edge channels to 8 bits. The self-shadow render pass forces an
// opaque alpha word; the regular pass keeps the material's own edge alpha.
std::uint32_t PackPmxEdgeColor(const mdl::ModelMaterialRecord& material,
                               bool opaqueAlpha) {
    const auto channel = [](float value) -> std::uint32_t {
        return static_cast<std::uint8_t>(static_cast<std::int32_t>(
            value * 255.0f));
    };
    const std::uint32_t red = channel(material.edgeColor[0]);
    const std::uint32_t green = channel(material.edgeColor[1]);
    const std::uint32_t blue = channel(material.edgeColor[2]);
    const std::uint32_t alpha =
        opaqueAlpha ? 0xFFu : channel(material.edgeColor[3]);
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

// The x64 worker ladder uses a 32-byte base record plus one contiguous
// float4 for every PMX additional UV set, transposed out of the loader's
// component-first source storage.
template <std::size_t AdditionalUvCount>
void CopyPmxAdditionalUvs(const mdl::PmxVertex& source,
                          mdl::SkinnedVertex<AdditionalUvCount>& destination) {
    destination.base.uv[0] = source.uv[0];
    destination.base.uv[1] = source.uv[1];
    if constexpr (AdditionalUvCount > 0) {
        for (std::size_t uv = 0; uv < AdditionalUvCount; ++uv)
            for (std::size_t component = 0; component < 4; ++component)
                destination.additionalUv[uv][component] =
                    source.additionalUvByComponent[component][uv];
    }
}

// One PMX morph's vertex/UV contribution (the x64 direct ladder at
// 0x1400E2030..0x1400E2659; group references 0x1400E1870 reuse it with the
// group weight folded in after the morph value, one multiply at a time).
// Bone (2) and material (8) morphs live in ModelApplyMorphs instead.
void AccumPmxVertexMorph(mdl::PmxVertex* vertices,
                         std::uint32_t firstVertex, std::uint32_t lastVertex,
                         const mdl::MorphRecord& morph, float value,
                         float groupWeight) {
    if (morph.type == 1) {
        const int count = morph.offsetCount;
        for (int k = 0; k < count; ++k) {
            const mdl::PmdVertexMorphEntry& entry = morph.vertexEntries[k];
            // Deliberate hardening: the original code multiplies the file-supplied
            // vertex index directly by the vertex stride and writes through the result
            // (verified at 0x1400C204A and 0x1400C205D) without checking it against
            // the model's vertex count, so a corrupt entry overwrites adjacent
            // model memory. This port drops such an entry.
            // Defect MB-05.
            if (entry.vertexIndex < firstVertex || entry.vertexIndex >= lastVertex)
                continue;
            mdl::PmxVertex& vertex = vertices[entry.vertexIndex];
            vertex.position[0] += entry.offset[0] * value * groupWeight;
            vertex.position[1] += entry.offset[1] * value * groupWeight;
            vertex.position[2] += entry.offset[2] * value * groupWeight;
        }
        return;
    }
    if (morph.type < 3 || morph.type > 7)
        return;
    const int family = morph.type - 3;
    const int count = morph.uvCounts[family];
    const mdl::PmxUvMorphEntry* entries = morph.uvEntries[family];
    for (int k = 0; k < count; ++k) {
        const mdl::PmxUvMorphEntry& entry = entries[k];
        // Deliberate hardening: the original indexes the vertex array with this
        // file-supplied index without bounds checking (verified at 0x1400C204A and
        // 0x1400C205D for the vertex ladder); this record is signed, so negative
        // values are unchecked as well. The port drops the entry.
        // Defect MB-05.
        if (entry.vertexIndex < 0 ||
            static_cast<std::uint32_t>(entry.vertexIndex) < firstVertex ||
            static_cast<std::uint32_t>(entry.vertexIndex) >= lastVertex)
            continue;
        mdl::PmxVertex& vertex = vertices[entry.vertexIndex];
        if (family == 0) {
            vertex.uv[0] += entry.offset[0] * value * groupWeight;
            vertex.uv[1] += entry.offset[1] * value * groupWeight;
        } else {
            for (int component = 0; component < 4; ++component)
                vertex.additionalUvByComponent[component][family - 1] +=
                    entry.offset[component] * value * groupWeight;
        }
    }
}

// 0x1400E1415..0x1400E17E8 plus 0x1400E17FE..0x1400E2670: the PMX twin of
// the PMD morph pass. Morph zero's aggregated position table and the five
// flattened UV base tables restore every morph-targeted component first,
// then each morph (including morph zero - unlike PMD) accumulates its
// weighted offsets; group morphs scale the vertex/UV morphs they reference.
void ApplyPmxVertexMorphRange(unsigned char* model, std::uint32_t firstVertex,
                            std::uint32_t lastVertex) {
    mdl::ModelRecord& record = *mdl::Mdl(model);
    mdl::MorphRecord* morphs = mdl::Morphs(model);
    mdl::PmxVertex* vertices = record.pmxVertices;
    if (morphs == nullptr || vertices == nullptr)
        return;

    const mdl::PmdVertexMorphEntry* base = mdl::BaseVertexMorphTable(model);
    const std::uint32_t baseCount = mdl::BaseVertexMorphCount(model);
    if (base != nullptr) {
        for (std::uint32_t i = 0; i < baseCount; ++i) {
            // Deliberate hardening: the original code multiplies this base-table index by
            // the vertex stride with no bounds check (verified at 0x1400C204A and
            // 0x1400C205D), then writes the restored position through the resulting
            // offset. This port drops the entry.
            // Defect MB-05.
            if (base[i].vertexIndex < firstVertex || base[i].vertexIndex >= lastVertex)
                continue;
            mdl::PmxVertex& vertex = vertices[base[i].vertexIndex];
            vertex.position[0] = base[i].offset[0];
            vertex.position[1] = base[i].offset[1];
            vertex.position[2] = base[i].offset[2];
        }
    }

    for (int family = 0; family < 5; ++family) {
        const mdl::PmxUvMorphEntry* entries =
            mdl::UvMorphTables(model).byFamily[family];
        const std::int32_t count = mdl::UvMorphCounts(model).byFamily[family];
        if (entries == nullptr || count <= 0)
            continue;
        for (int i = 0; i < count; ++i) {
            // Deliberate hardening: this reuses the same unbounded
            // vertex index supplied by the file, as in the base table above (verified at
            // 0x1400C204A and 0x1400C205D); this record is signed, so negative
            // values are likewise unchecked. The port drops the entry.
            // Defect MB-05.
            if (entries[i].vertexIndex < 0 ||
                static_cast<std::uint32_t>(entries[i].vertexIndex) < firstVertex ||
                static_cast<std::uint32_t>(entries[i].vertexIndex) >=
                    lastVertex)
                continue;
            mdl::PmxVertex& vertex = vertices[entries[i].vertexIndex];
            if (family == 0) {
                vertex.uv[0] = entries[i].offset[0];
                vertex.uv[1] = entries[i].offset[1];
            } else {
                for (int component = 0; component < 4; ++component)
                    vertex.additionalUvByComponent[component][family - 1] =
                        entries[i].offset[component];
            }
        }
    }

    const int morphCount = record.morphCount;
    for (int i = 0; i < morphCount; ++i) {
        const mdl::MorphRecord& morph = morphs[i];
        const float weight = morph.value;
        if (weight == 0.0f)
            continue;
        if (morph.type != 0) {
            AccumPmxVertexMorph(vertices, firstVertex, lastVertex, morph, weight, 1.0f);
            continue;
        }
        const int groupCount = morph.groupCount;
        for (int k = 0; k < groupCount; ++k) {
            const mdl::PmxGroupMorphEntry& ref = morph.groupEntries[k];
            // Deliberately hardened: the original shifts this group reference's
            // morph index into the morph stride and dereferences it (verified at
            // 0x1400C187C, 0x1400C1884 and 0x1400C1888) with no check at all
            // against the morph count, not even a negative one. The port drops
            // the reference, consistent with the gate ModelApplyMorphs already
            // applies to the same field.
            // Defect MB-06.
            if (ref.morphIndex < 0 || ref.morphIndex >= morphCount)
                continue;
            AccumPmxVertexMorph(vertices, firstVertex, lastVertex, morphs[ref.morphIndex],
                                weight, ref.weight);
        }
    }
}

void ApplyPmxVertexMorphs(unsigned char* model) {
    MMD_PERF_SCOPE(Morph);
    const auto& record = *mdl::Mdl(model);
    if (!record.pmxVertices || !mdl::Morphs(model)) return;
    const std::uint32_t vertexCount = record.vertexCount;
    std::uint64_t restoreCount = mdl::BaseVertexMorphCount(model);
    for (const auto count : mdl::UvMorphCounts(model).byFamily)
        if (count > 0) restoreCount += count;
    // Each vertex belongs to one worker. Duplicate entries and overlapping
    // group morphs retain their original accumulation order within that worker.
    // Tables are read-only; all writes stay inside the assigned vertex range.
#pragma omp parallel if(vertexCount >= 32768 && restoreCount >= 16384) num_threads(GeometryWorkerCount())
    {
#ifdef _OPENMP
        const unsigned workers = omp_get_num_threads(), worker = omp_get_thread_num();
        ApplyPmxVertexMorphRange(model, std::uint64_t(vertexCount) * worker / workers,
                                std::uint64_t(vertexCount) * (worker + 1) / workers);
#else
        ApplyPmxVertexMorphRange(model, 0, vertexCount);
#endif
    }
}

// 0x4B0C74..0x4B0CE9 and 0x4B1C9C..0x4B1D8B.  PMD morph zero is
// the base table; all following morphs contain offsets indexed through it.
void ApplyPmdVertexMorphs(unsigned char* model) {
    MMD_PERF_SCOPE(Morph);
    mdl::ModelRecord& record = *mdl::Mdl(model);
    mdl::PmdVertex* vertices = record.rawVertices;
    const std::uint32_t vertexCount = record.vertexCount;
    const mdl::PmdVertexMorphEntry* base = mdl::BaseVertexMorphTable(model);
    const std::uint32_t baseCount = mdl::BaseVertexMorphCount(model);
    if (vertices == nullptr || base == nullptr)
        return;

    for (std::uint32_t i = 0; i < baseCount; ++i) {
        const mdl::PmdVertexMorphEntry& entry = base[i];
        // Deliberate hardening: the original writes the restored position
        // through an unbounded index taken from the file (verified at
        // 0x1400C204A and 0x1400C205D). This port drops the entry.
        // Defect MB-05.
        if (entry.vertexIndex >= vertexCount)
            continue;
        mdl::PmdVertex& vertex = vertices[entry.vertexIndex];
        vertex.position[0] = entry.offset[0];
        vertex.position[1] = entry.offset[1];
        vertex.position[2] = entry.offset[2];
    }

    mikumikustudio::mdl::MorphRecord* morphs = mikumikustudio::mdl::Morphs(model);
    const int morphCount = record.morphCount;
    if (morphs == nullptr || morphCount <= 1)
        return;
    for (int i = 1; i < morphCount; ++i) {
        mikumikustudio::mdl::MorphRecord* morph = &morphs[i];
        const float weight = morph->value;
        if (weight == 0.0f)
            continue;
        auto* entries = morph->vertexEntries;
        const std::uint32_t count = morph->offsetCount;
        if (entries == nullptr)
            continue;
        for (std::uint32_t k = 0; k < count; ++k) {
            const mdl::PmdVertexMorphEntry& entry = entries[k];
            // Deliberate hardening: the same unbounded file-supplied index as the
            // base table above (verified at 0x1400C204A and 0x1400C205D), reached
            // here through the PMD base-remap table. The port drops the entry.
            // Defect MB-05.
            if (entry.vertexIndex >= vertexCount)
                continue;
            mdl::PmdVertex& vertex = vertices[entry.vertexIndex];
            vertex.position[0] += entry.offset[0] * weight;
            vertex.position[1] += entry.offset[1] * weight;
            vertex.position[2] += entry.offset[2] * weight;
        }
    }
}

float PmdEdgeDistance(MMDApp* app, unsigned char* model,
                      const float frameWorld[16]) {
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    auto* bones = mdl::Bones(model);
    const int boneCount = record.boneCount;
    float modelPoint[3]{};
    if (bones != nullptr && boneCount > 0) {
        mikumikustudio::mdl::BoneRecord* bone = &bones[(boneCount > 1 ? 1 : 0)];
        const float bind[3] = {bone->position[0], bone->position[1],
                               bone->position[2]};
        TransformPosition(modelPoint, bind,
                          bone->matInit);
    }
    float worldPoint[3];
    TransformPosition(worldPoint, modelPoint, frameWorld);
    // frameWorld already translates by the orbit target and rotates the scene.
    // Use the eye from SetupFrameWorldTransform's fixed look-at view, in that
    // same space. CameraPosition() is the target before this transformation;
    // subtracting it again makes close-up outlines much too thick.
    // MMD 9.26: 0x14002A213..0x14002A247 passes view X/Y and camera distance
    // to the common PMD/PMX edge worker at 0x1400C1390.
    const float camera[3] = {
        app->ViewOffsetX(), app->ViewOffsetY(), app->CameraDistance()};
    const double dx = static_cast<double>(worldPoint[0] - camera[0]);
    const double dy = static_cast<double>(worldPoint[1] - camera[1]);
    const double dz = static_cast<double>(worldPoint[2] - camera[2]);
    const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return static_cast<float>(distance * 0.00004500000068219379 *
        (static_cast<double>(app->CameraFov()) *
             0.6000000238418579 +
         1.0) *
        static_cast<double>(record.edgeScale));
}

void SkinPmd(unsigned char* model, float edgeDistance,
             const std::vector<affine::BoneAux>& auxTable,
             mdl::EdgeVertex* edgeVertices,
             mdl::SkinnedVertexBase* mainVertices) {
    MMD_PERF_SCOPE(Skin);
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const std::uint32_t vertexCount = record.vertexCount;
    auto* vertices = record.rawVertices;
    auto* bones = mdl::Bones(model);
    const int boneCount = record.boneCount;
    const affine::BoneAux* aux = auxTable.data();
    if (vertices == nullptr || bones == nullptr)
        return;

    // MMD 926's zero-UV worker also parallelizes PMD (0x14011F536).
    // Each worker writes only its vertex; keep small meshes serial to avoid
    // paying a thread-team cost for a few thousand cheap two-bone blends.
#pragma omp parallel for schedule(static) if(vertexCount >= 16384) num_threads(GeometryWorkerCount())
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(vertexCount); ++i) {
        const mdl::PmdVertex& src = vertices[i];
        mdl::SkinnedVertexBase& main = mainVertices[i];
        mdl::EdgeVertex& edge = edgeVertices[i];
        const int bone0 = src.bone[0];
        const int bone1 = src.bone[1];
        const float weight0 = static_cast<float>(
            src.weightPercent) * 0.009999999776482582f;
        affine::Skinned skinned;
        affine::SkinBdef2(PmxSkinBone(bones, aux, boneCount, bone0),
                          PmxSkinBone(bones, aux, boneCount, bone1), weight0,
                          src.position, src.normal, skinned);
        StoreSkinned(main, skinned);
        main.uv[0] = src.uv[0];
        main.uv[1] = src.uv[1];

        const float amount = src.edgeDisabled != 0
            ? -0.004999999888241291f
            : edgeDistance;
        edge.position[0] = EdgeComponent(main.position[0], main.normal[0], amount);
        edge.position[1] = EdgeComponent(main.position[1], main.normal[1], amount);
        edge.position[2] = EdgeComponent(main.position[2], main.normal[2], amount);
        // edge.diffuse is the load-time 0xFF000000 and is intentionally retained.
    }
}

// The x64 stride workers share one per-vertex shape: dispatch on the weight
// type (QDEF and any unknown type leave position/normal untouched, only the
// common tail runs), then copy UVs, pack the edge color and expand the
// outline. The vertex marker at +172 picks between the material-driven
// expansion normal * frame scale * vertex scale * material size and the
// fixed 0.005 shrink.
template <std::size_t AdditionalUvCount>
void SkinPmx(unsigned char* model, float edgeDistance, bool opaqueEdge,
             const std::vector<affine::BoneAux>& auxTable,
             mdl::EdgeVertex* edgeVertices,
             mdl::SkinnedVertex<AdditionalUvCount>* mainVertices) {
    MMD_PERF_SCOPE(Skin);
    const mdl::ModelRecord& record = *mdl::Mdl(model);
    const std::uint32_t vertexCount = record.vertexCount;
    const mdl::PmxVertex* vertices = record.pmxVertices;
    const mdl::BoneRecord* bones = mdl::Bones(model);
    const mdl::ModelMaterialRecord* materials = mdl::Materials(model);
    const int boneCount = static_cast<int>(record.boneCount);
    const affine::BoneAux* aux = auxTable.data();
    if (vertices == nullptr || bones == nullptr || materials == nullptr)
        return;

    // Material morphs have already run for this frame. Pack each material's
    // outline colour once, instead of repeating four float conversions for
    // every vertex. Keep the size separate so the expansion's multiply order
    // and the shadow pass's forced alpha stay exactly the same.
    struct EdgeMaterial { std::uint32_t color; float size; };
    std::vector<EdgeMaterial> edgeMaterials(record.materialCount);
    for (std::size_t i = 0; i < edgeMaterials.size(); ++i)
        edgeMaterials[i] = {PackPmxEdgeColor(materials[i], opaqueEdge),
                            materials[i].edgeSize};
    const EdgeMaterial* edges = edgeMaterials.data();

    // x64 forks the stride workers through VCOMP90 (_vcomp_for_static_simple_
    // init/end): the per-vertex loop is the original's default-schedule
    // parallel region. MSVC /openmp (OpenMP 2.0) only accepts signed loop
    // indexes, so this is one signed index where the port kept unsigned.
#pragma omp parallel for schedule(static) if(vertexCount >= 16384) num_threads(GeometryWorkerCount())
    for (int i = 0; i < static_cast<int>(vertexCount); ++i) {
        const mdl::PmxVertex& source = vertices[i];
        mdl::SkinnedVertex<AdditionalUvCount>& main = mainVertices[i];
        mdl::EdgeVertex& edge = edgeVertices[i];
        switch (source.weightType) {
        case mdl::PmxWeightType::bdef2:
            SkinPmxBdef2(source, bones, aux, boneCount, main.base);
            break;
        case mdl::PmxWeightType::bdef1:
            SkinPmxBdef1(source, bones, aux, boneCount, main.base);
            break;
        case mdl::PmxWeightType::bdef4:
            SkinPmxBdef4(source, bones, aux, boneCount, main.base);
            break;
        case mdl::PmxWeightType::sdef:
            SkinPmxSdef(source, bones, aux, boneCount, main.base);
            break;
        default:
            break;
        }
        CopyPmxAdditionalUvs(source, main);

        const EdgeMaterial& material = edges[source.materialIndex];
        edge.diffuse = material.color;
        if (source.hasPmxEdgeData == 0) {
            for (int component = 0; component < 3; ++component)
                edge.position[component] =
                    main.base.position[component] -
                    main.base.normal[component] * 0.004999999888241291f;
        } else {
            for (int component = 0; component < 3; ++component) {
                float amount = edgeDistance * main.base.normal[component];
                amount *= source.edgeScale;
                amount *= material.size;
                edge.position[component] =
                    main.base.position[component] + amount;
            }
        }
    }
}

// x64 0x1400E2A5F..0x1400E2CEC: the PMX branch locks the main buffer with
// 32 + 16 * additionalUvCount bytes per vertex and the edge buffer with 16,
// then dispatches the stride worker selected by the additional-UV count.
// A count above four matches no worker and the original leaves both
// buffers untouched that frame. The frame edge scale is the same
// camera-distance value the PMD path uses (x64 computes it once for both).
bool UpdatePmxModelVertexBuffers(MMDApp* app, unsigned char* model,
                                 const float frameWorld[16]) {
    mdl::ModelRecord& state = *mdl::Mdl(model);
    ApplyPmxVertexMorphs(model);

    const unsigned int additionalUv = state.pmxAdditionalUvCount;
    if (additionalUv > 4)
        return false;
    auto* mainVb = mdl::ResourceAs<IDirect3DVertexBuffer9>(
        state.vertexBuffer);
    auto* edgeVb = mdl::ResourceAs<IDirect3DVertexBuffer9>(
        state.vertexBuffer2);
    if (mainVb == nullptr || edgeVb == nullptr)
        return false;
    d3dx::Api& d3dxApi = d3dx::Get();
    if (!d3dxApi.Load() || d3dxApi.quatFromMatrix == nullptr ||
        d3dxApi.matrixRotationQuaternion == nullptr)
        return false;

    const UINT count = state.vertexCount;
    const UINT stride = 32 + 16 * additionalUv;
    mdl::SkinnedVertexBase* mainVertices = nullptr;
    mdl::EdgeVertex* edgeVertices = nullptr;
    if (FAILED(mainVb->Lock(0, stride * count,
                            reinterpret_cast<void**>(&mainVertices), 0)))
        return false;
    if (FAILED(edgeVb->Lock(0, 16 * count,
                            reinterpret_cast<void**>(&edgeVertices), 0))) {
        mainVb->Unlock();
        return false;
    }

    // One precomputation per bone per frame (polar factors, inverse
    // transpose, rigid test); the per-vertex workers only read it.
    std::vector<affine::BoneAux> auxTable;
    BuildPmxBoneAux(mdl::Bones(model), static_cast<int>(state.boneCount),
                    d3dxApi, auxTable);
    const float edgeDistance = PmdEdgeDistance(app, model, frameWorld);
    const bool opaqueEdge = app->state.selfShadowEnabled != 0;
    switch (additionalUv) {
    case 0:
        SkinPmx<0>(model, edgeDistance, opaqueEdge, auxTable, edgeVertices,
                   reinterpret_cast<mdl::SkinnedVertex<0>*>(mainVertices));
        break;
    case 1:
        SkinPmx<1>(model, edgeDistance, opaqueEdge, auxTable, edgeVertices,
                   reinterpret_cast<mdl::SkinnedVertex<1>*>(mainVertices));
        break;
    case 2:
        SkinPmx<2>(model, edgeDistance, opaqueEdge, auxTable, edgeVertices,
                   reinterpret_cast<mdl::SkinnedVertex<2>*>(mainVertices));
        break;
    case 3:
        SkinPmx<3>(model, edgeDistance, opaqueEdge, auxTable, edgeVertices,
                   reinterpret_cast<mdl::SkinnedVertex<3>*>(mainVertices));
        break;
    default:
        SkinPmx<4>(model, edgeDistance, opaqueEdge, auxTable, edgeVertices,
                   reinterpret_cast<mdl::SkinnedVertex<4>*>(mainVertices));
        break;
    }
    DumpModelVertexBuffers(app, model, mainVertices, edgeVertices, count,
                           stride);
    model_vertex_cache::RememberCpuBody(model, mainVertices, count, stride);
    mainVb->Unlock();
    edgeVb->Unlock();
    return true;
}

bool UpdateCachedOutline(MMDApp* app, unsigned char* model, float distance,
                         const model_vertex_cache::OutlineVertex* body, std::size_t count) {
    const auto& state = *mdl::Mdl(model);
    if (count != state.vertexCount || !state.vertexBuffer2) return false;
    const bool pmx = state.physicsMode == 2;
    struct EdgeMaterial { std::uint32_t color; float size; };
    std::vector<EdgeMaterial> materials;
    if (pmx) {
        if (!state.pmxVertices || !state.materials) return false;
        materials.resize(state.materialCount);
        for (unsigned i = 0; i < state.materialCount; ++i)
            materials[i] = {PackPmxEdgeColor(mdl::Materials(model)[i], app->state.selfShadowEnabled != 0),
                            mdl::Materials(model)[i].edgeSize};
    } else if (!state.rawVertices) return false;
    auto* buffer = static_cast<IDirect3DVertexBuffer9*>(state.vertexBuffer2);
    mdl::EdgeVertex* edges = nullptr;
    if (FAILED(buffer->Lock(0, UINT(count * sizeof(mdl::EdgeVertex)), reinterpret_cast<void**>(&edges), 0)))
        return false;
    // Match the complete CPU worker's operation order. The retained positions
    // and normals were copied before that worker unlocked its output; this
    // path never maps or reads the main D3D9 vertex buffer.
#pragma omp parallel for schedule(static) if(count >= 16384) num_threads(GeometryWorkerCount())
    for (std::int64_t i = 0; i < std::int64_t(count); ++i) {
        const auto& main = body[i]; auto& edge = edges[i];
        if (pmx) {
            const auto& material = materials[main.material & 0x7fffffffu];
            edge.diffuse = material.color;
            for (int c = 0; c < 3; ++c) {
                if (main.material & 0x80000000u)
                    edge.position[c] = main.position[c] - main.normal[c] * 0.004999999888241291f;
                else {
                    float amount = distance * main.normal[c];
                    amount *= main.edgeScale; amount *= material.size;
                    edge.position[c] = main.position[c] + amount;
                }
            }
        } else {
            const float amount = main.material & 0x80000000u ? -0.004999999888241291f : distance;
            for (int c = 0; c < 3; ++c)
                edge.position[c] = EdgeComponent(main.position[c], main.normal[c], amount);
        }
    }
    return SUCCEEDED(buffer->Unlock());
}

}  // namespace

// MMV_HOST_MME_BRIDGE: share the source renderer's outline width calculation.
float ModelOutlineDistance(MMDApp* app, unsigned char* model, const float world[16]) {
    return PmdEdgeDistance(app, model, world);
}

bool UpdateModelVertexBuffersCpu(MMDApp* app, unsigned char* model,
                              const float frameWorld[16]) {
    model_vertex_cache::Invalidate(model);
    if (app == nullptr || model == nullptr || frameWorld == nullptr)
        return false;
    mdl::ModelRecord& state = *mdl::Mdl(model);
    if (state.loadComplete == 0 || state.vertexCount == 0)
        return false;

    // The PMX twin (x64 0x1400E13C0) runs its own restore+morph pass and
    // dispatches the additional-UV stride workers; PMD uses the two-bone
    // worker below with the same per-vertex arithmetic.
    if (state.physicsMode == 2) {
        return UpdatePmxModelVertexBuffers(app, model, frameWorld);
    }

    ApplyPmdVertexMorphs(model);
    auto* mainVb = mdl::ResourceAs<IDirect3DVertexBuffer9>(
        state.vertexBuffer);
    auto* edgeVb = mdl::ResourceAs<IDirect3DVertexBuffer9>(
        state.vertexBuffer2);
    if (mainVb == nullptr || edgeVb == nullptr)
        return false;

    const UINT count = state.vertexCount;
    mdl::SkinnedVertexBase* mainVertices = nullptr;
    mdl::EdgeVertex* edgeVertices = nullptr;
    if (FAILED(mainVb->Lock(0, 32 * count,
                            reinterpret_cast<void**>(&mainVertices), 0)))
        return false;
    if (FAILED(edgeVb->Lock(0, 16 * count,
                            reinterpret_cast<void**>(&edgeVertices), 0))) {
        mainVb->Unlock();
        return false;
    }

    std::vector<affine::BoneAux> auxTable;
    BuildPmdBoneAux(mdl::Bones(model), state.boneCount, auxTable);
    SkinPmd(model, PmdEdgeDistance(app, model, frameWorld), auxTable,
            edgeVertices, mainVertices);
    DumpModelVertexBuffers(app, model, mainVertices, edgeVertices, count, 32);
    model_vertex_cache::RememberCpuBody(model, mainVertices, count, 32);
    mainVb->Unlock();
    edgeVb->Unlock();
    return true;
}

void UpdateModelVertexBuffers(MMDApp* app, unsigned char* model,
                              const float frameWorld[16]) {
    MMD_PERF_SCOPE(VertexUpdate);
    if (!app || !model || !frameWorld) return;
    const auto& state = *mdl::Mdl(model);
    if (!state.loadComplete || !state.vertexCount) return;
    const float distance = PmdEdgeDistance(app, model, frameWorld);
    if (model_vertex_cache::Reuse(app, model, distance, UpdateCachedOutline)) return;
    bool updated = skinning::UpdateModel(app, model, frameWorld, UpdateModelVertexBuffersCpu);
    if (!updated) updated = UpdateModelVertexBuffersCpu(app, model, frameWorld);
    if (updated) model_vertex_cache::Remember(app, model, distance);
}
bool PrepareGpuModelInput(MMDApp* app,unsigned char* model,const float world[16],GpuModelInput& out,bool geometry) {
    if(!app || !model || !world) return false;
    const auto& m=*mdl::Mdl(model);
    if(!m.loadComplete || !m.vertexCount || !m.boneCount || !m.materialCount) return false;
    const bool pmx=m.physicsMode==2;
    if(pmx && m.pmxAdditionalUvCount>4) return false;
    if(geometry) {
        model_vertex_cache::Invalidate(model);
        if(pmx) ApplyPmxVertexMorphs(model); else ApplyPmdVertexMorphs(model);
    }
    out.parameters={PmdEdgeDistance(app,model,world),app->state.selfShadowEnabled!=0,!pmx};
    out.vertices.resize(m.vertexCount); out.bones.resize(m.boneCount); out.materials.resize(m.materialCount);
    auto& api=d3dx::Get(); if(!api.Load()) return false;
    for(unsigned i=0;i<m.boneCount;++i) {
        auto& b=out.bones[i]; std::memcpy(b.matrix,mdl::Bones(model)[i].matInit,64);
        float rotation[4];
        api.quatFromMatrix(rotation,reinterpret_cast<const d3dx::D3DXMATRIXF*>(b.matrix));
        // Every field of the extended bone record is initialised here: the
        // polar rotation quaternion replaces D3DX's for affine bones, and
        // stretch/normalMatrix/flags carry the affine extension. A rigid bone
        // keeps its D3DX quaternion, an identity stretch, the linear part as
        // its own inverse transpose and flags 0, so it follows the shader's
        // legacy rigid path bit for bit.
        affine::BoneAux aux;
        affine::PrecomputeBone(b.matrix, rotation, aux);
        std::memcpy(b.quaternion, aux.rotation, sizeof(b.quaternion));
        for (unsigned row = 0; row < 3; ++row) {
            std::memcpy(b.stretch[row], &aux.stretch[row * 3], 12);
            std::memcpy(b.normalMatrix[row], &aux.normalMatrix[row * 3], 12);
            b.stretch[row][3] = 0.0f;
            b.normalMatrix[row][3] = 0.0f;
        }
        b.flags = aux.flags;
        b.reserved[0] = b.reserved[1] = b.reserved[2] = 0;
    }
    for(unsigned i=0;i<m.materialCount;++i) {
        auto& material=out.materials[i]; material={};
        std::memcpy(material.edgeColor,mdl::Materials(model)[i].edgeColor,16);
        material.edgeSize=mdl::Materials(model)[i].edgeSize;
        material.edgeDistance=out.parameters.edgeDistance;
        material.pmd=!pmx;material.opaqueEdge=out.parameters.opaqueEdge;
    }
    if(!geometry)return true;
    for(unsigned i=0;i<m.vertexCount;++i) {
        auto& v=out.vertices[i]; v={};
        if(pmx) {
            const auto& source=m.pmxVertices[i];
            v.type=static_cast<unsigned>(source.weightType);
            if(v.type>3 || source.materialIndex<0 || unsigned(source.materialIndex)>=m.materialCount) return false;
            std::memcpy(v.position,source.position,12); std::memcpy(v.normal,source.normal,12);
            std::memcpy(v.uv,source.uv,8);
            // The legacy loader leaves fields unused by this weight type
            // uninitialized. Do not read or upload those inactive lanes.
            const unsigned boneCount=v.type==0?1:v.type==2?4:2;
            std::memcpy(v.bones,source.bone,boneCount*sizeof(v.bones[0]));
            if(v.type==0)v.weights[0]=1.f;
            else std::memcpy(v.weights,source.weight,(v.type==2?4:1)*sizeof(float));
            if(v.type==3){
                std::memcpy(v.center,source.sdef.center,12);std::memcpy(v.r0,source.sdef.r0Offset,12);
                std::memcpy(v.r1,source.sdef.r1Offset,12);
            }
            v.edgeScale=source.edgeScale; v.edgeEnabled=source.hasPmxEdgeData; v.material=source.materialIndex;
            for(unsigned uv=0;uv<m.pmxAdditionalUvCount;++uv)
                for(unsigned c=0;c<4;++c) v.additionalUv[uv][c]=source.additionalUvByComponent[c][uv];
        } else {
            const auto& source=m.rawVertices[i];
            v.type=1; v.bones[0]=source.bone[0]; v.bones[1]=source.bone[1];
            v.weights[0]=float(source.weightPercent)*0.009999999776482582f;
            std::memcpy(v.position,source.position,12); std::memcpy(v.normal,source.normal,12);
            std::memcpy(v.uv,source.uv,8); v.edgeEnabled=source.edgeDisabled==0;
        }
    }
    return true;
}

}  // namespace mikumikustudio
