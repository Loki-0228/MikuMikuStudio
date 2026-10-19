#include "mikudancestudio/transparent_capture.hpp"
#include "mikudancestudio/shadow_filter.hpp"
#include "mikudancestudio/native_mesh.hpp"
#include "mikudancestudio/deformed_mesh_capture.hpp"
#include "mikudancestudio/perf_timing.hpp"
#include "mikudancestudio/antialias.hpp"
// ===========================================================================
// VA 0x00425D20 / 0x00426CD0 / 0x004277E0 - model and shadow render passes
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "mikudancestudio/geometry_workers.hpp"

#include "mikudancestudio/d3dx_effect.hpp"
#include "mikudancestudio/d3d_object_guard.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/model_alpha_pass.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/transparent_triangles.hpp"
#include "mikudancestudio/transparent_geometry.hpp"
#include "mikudancestudio/transparent_peeling.hpp"
#include "mikudancestudio/transparent_material_order.hpp"
#endif
#include <limits>
#include <new>
#include <numeric>
#include "mikudancestudio/ported_funcs.hpp"

#include "mikudancestudio/texture_lookup.hpp"
#include "mikudancestudio/shared_toon.hpp"
#include "mikudancestudio/draw_order.hpp"
#include "mikudancestudio/adaptive_shadow.hpp"
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/native_shadow_projection.hpp"
#endif

namespace mikudancestudio {
namespace {

template <typename ResolveModel>
auto BuildModelDrawOrder(MMDApp* app, ResolveModel resolveModel) {
    return BuildDrawOrder<kModelSlotCount>(
        [&](int slot) { return resolveModel(app->ModelSlot(slot)); },
        [](unsigned char* model) { return mdl::Mdl(model)->comboSelIndex; });
}


using Matrix = d3dx::D3DXMATRIXF;

// Porting-era material-state capture under MIKUDANCESTUDIO_VB_DUMP_DIR
// (CMake option MIKUDANCESTUDIO_DIAG, default OFF); the OFF stubs below
// keep the call sites valid and inline away to nothing.
#ifdef MIKUDANCESTUDIO_DIAG
struct MaterialStateCapture {
    FILE* stream = nullptr;
    bool first = true;
    unsigned sequence = 0;
};

MaterialStateCapture g_materialCapture;

bool IsRegularCaptureFile(const char* path) {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void WriteWideHex(FILE* stream, const wchar_t* value, std::size_t limit) {
    std::fputc('"', stream);
    if (value != nullptr) {
        for (std::size_t i = 0; i < limit && value[i] != L'\0'; ++i)
            std::fprintf(stream, "%04X", static_cast<unsigned>(value[i]));
    }
    std::fputc('"', stream);
}

bool BeginMaterialStateCapture() {
    static LONG captured = 0;
    if (InterlockedCompareExchange(&captured, 0, 0) != 0)
        return false;

    char directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA(
        "MIKUDANCESTUDIO_VB_DUMP_DIR", directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    char stable[2]{};
    if (GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_STABLE_CAPTURE", stable,
                                sizeof(stable)) == 1 && stable[0] == '1') {
        char requireLine[2]{};
        const bool refreshed =
            GetEnvironmentVariableA("MIKUDANCESTUDIO_AB_REQUIRE_LINE", requireLine,
                                    sizeof(requireLine)) == 1 &&
            requireLine[0] == '1';
        char gate[MAX_PATH]{};
        std::snprintf(gate, sizeof(gate), "%s\\%s", directory,
                      refreshed ? "vb.capture.active" : "vb.capture.ready");
        if (!IsRegularCaptureFile(gate))
            return false;
    }
    if (InterlockedCompareExchange(&captured, 1, 0) != 0)
        return false;

    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\material_states.json", directory);
    g_materialCapture.stream = std::fopen(path, "wb");
    if (g_materialCapture.stream == nullptr)
        return false;
    g_materialCapture.first = true;
    g_materialCapture.sequence = 0;
    std::fputs("{\"schema\":1,\"renderer\":\"fixed\",\"materials\":[\n",
               g_materialCapture.stream);
    return true;
}

DWORD TextureStageState(IDirect3DDevice9* device, DWORD stage,
                        D3DTEXTURESTAGESTATETYPE type) {
    DWORD value = 0;
    device->GetTextureStageState(stage, type, &value);
    return value;
}

DWORD SamplerState(IDirect3DDevice9* device, DWORD stage,
                   D3DSAMPLERSTATETYPE type) {
    DWORD value = 0;
    device->GetSamplerState(stage, type, &value);
    return value;
}

DWORD RenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE type) {
    DWORD value = 0;
    device->GetRenderState(type, &value);
    return value;
}

void DumpMaterialState(MMDApp* app, IDirect3DDevice9* device,
                       unsigned char* model, unsigned char* material,
                       UINT materialIndex, UINT firstIndex) {
    FILE* stream = g_materialCapture.stream;
    if (stream == nullptr)
        return;

    int slot = -1;
    // port diagnostic: bound = model-slot capacity (kModelSlotCount)
    for (int i = 0; i < kModelSlotCount; ++i) {
        if (native_mesh::RenderModel(app->ModelSlot(i)) == model) {
            slot = i;
            break;
        }
    }
    const mdl::ModelMaterialRecord& record = mdl::Material(material);
    DWORD alphaBits = 0;
    std::memcpy(&alphaBits, &record.diffuse[3], sizeof(alphaBits));
    std::fprintf(stream,
        "%s{\"sequence\":%u,\"model_slot\":%d,\"model_order\":%u,"
        "\"material\":%u,\"first_index\":%u,\"index_count\":%u,"
        "\"model_format\":%u,\"alpha_bits\":\"%08X\","
        "\"toon_index\":%d,\"edge_flag\":%u,\"pmx_flags\":%u,"
        "\"sphere_mode\":%u,\"main_path_utf16\":",
        g_materialCapture.first ? "" : ",\n", g_materialCapture.sequence++,
        slot, static_cast<unsigned>(mdl::Mdl(model)->comboSelIndex), materialIndex, firstIndex,
        record.faceVertexCount, static_cast<unsigned>(mdl::Mdl(model)->physicsMode),
        alphaBits, static_cast<int>(
            static_cast<std::int8_t>(record.toonReference)),
        static_cast<unsigned>(record.doubleSided),
        static_cast<unsigned>(record.flags),
        static_cast<unsigned>(record.sphereMode));
    WriteWideHex(stream, record.texturePath, 280);
    std::fputs(",\"sphere_path_utf16\":", stream);
    WriteWideHex(stream, record.spherePath, 280);
    std::fputs(",\"stages\":[", stream);

    for (DWORD stage = 0; stage < 3; ++stage) {
        IDirect3DBaseTexture9* texture = nullptr;
        const HRESULT textureResult = device->GetTexture(stage, &texture);
        const bool bound = SUCCEEDED(textureResult) && texture != nullptr;
        if (texture != nullptr)
            texture->Release();
        D3DMATRIX transform{};
        device->GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(
                                 D3DTS_TEXTURE0 + stage), &transform);
        DWORD matrixBits[16]{};
        std::memcpy(matrixBits, &transform, sizeof(matrixBits));
        std::fprintf(stream,
            "%s{\"stage\":%u,\"role\":\"%s\",\"bound\":%s,"
            "\"color_op\":%u,\"color_arg1\":%u,\"color_arg2\":%u,"
            "\"alpha_op\":%u,\"alpha_arg1\":%u,\"alpha_arg2\":%u,"
            "\"texcoord_index\":%u,\"transform_flags\":%u,"
            "\"address_u\":%u,\"address_v\":%u,\"mag_filter\":%u,"
            "\"min_filter\":%u,\"mip_filter\":%u,\"transform_bits\":[",
            stage == 0 ? "" : ",", stage,
            stage == 0 ? "toon" : stage == 1 ? "cascade1" : "cascade2",
            bound ? "true" : "false",
            TextureStageState(device, stage, D3DTSS_COLOROP),
            TextureStageState(device, stage, D3DTSS_COLORARG1),
            TextureStageState(device, stage, D3DTSS_COLORARG2),
            TextureStageState(device, stage, D3DTSS_ALPHAOP),
            TextureStageState(device, stage, D3DTSS_ALPHAARG1),
            TextureStageState(device, stage, D3DTSS_ALPHAARG2),
            TextureStageState(device, stage, D3DTSS_TEXCOORDINDEX),
            TextureStageState(device, stage, D3DTSS_TEXTURETRANSFORMFLAGS),
            SamplerState(device, stage, D3DSAMP_ADDRESSU),
            SamplerState(device, stage, D3DSAMP_ADDRESSV),
            SamplerState(device, stage, D3DSAMP_MAGFILTER),
            SamplerState(device, stage, D3DSAMP_MINFILTER),
            SamplerState(device, stage, D3DSAMP_MIPFILTER));
        for (int word = 0; word < 16; ++word)
            std::fprintf(stream, "%s\"%08X\"", word == 0 ? "" : ",",
                         matrixBits[word]);
        std::fputs("]}", stream);
    }
    std::fprintf(stream,
        "],\"render_states\":{\"cull_mode\":%u,\"z_func\":%u,"
        "\"alpha_blend\":%u,\"alpha_test\":%u,\"src_blend\":%u,"
        "\"dest_blend\":%u,\"lighting\":%u},"
        "\"light_bits\":[",
        RenderState(device, D3DRS_CULLMODE),
        RenderState(device, D3DRS_ZFUNC),
        RenderState(device, D3DRS_ALPHABLENDENABLE),
        RenderState(device, D3DRS_ALPHATESTENABLE),
        RenderState(device, D3DRS_SRCBLEND),
        RenderState(device, D3DRS_DESTBLEND),
        RenderState(device, D3DRS_LIGHTING));
    // Emit the actual typed light record.  Reading this as independent x86
    // dwords bypassed the x64 state-layout translation and made the capture
    // itself report unrelated values.
    DWORD lightBits[sizeof(D3DLIGHT9) / sizeof(DWORD)]{};
    const D3DLIGHT9& sceneLight = app->SceneLight();
    std::memcpy(lightBits, &sceneLight, sizeof(lightBits));
    for (int word = 0; word < 24; ++word) {
        std::fprintf(stream, "%s\"%08X\"", word == 0 ? "" : ",",
                     lightBits[word]);
    }
    std::fputs("],\"toon_ptrs\":[", stream);
    for (int slot = 0; slot < 11; ++slot) {
        std::fprintf(stream, "%s\"%08X\"", slot == 0 ? "" : ",",
                     static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(
                         app->ToonTexture(slot))));
    }
    std::fputs("]}", stream);
    g_materialCapture.first = false;
}

void EndMaterialStateCapture() {
    if (g_materialCapture.stream == nullptr)
        return;
    std::fputs("\n]}\n", g_materialCapture.stream);
    std::fclose(g_materialCapture.stream);
    g_materialCapture.stream = nullptr;
}
#else
inline bool BeginMaterialStateCapture() { return false; }
inline void DumpMaterialState(MMDApp*, IDirect3DDevice9*, unsigned char*,
                              unsigned char*, UINT, UINT) {}
inline void EndMaterialStateCapture() {}
#endif

using d3dx::FxSetTechnique;
using d3dx::FxSetInt;
using d3dx::FxSetFloatArray;
using d3dx::FxSetMatrix;
using d3dx::FxBegin;
using d3dx::FxBeginPass;
using d3dx::FxCommit;
using d3dx::FxEndPass;
using d3dx::FxEnd;

void Identity(Matrix* out) {
    std::memset(out, 0, sizeof(*out));
    out->m[0][0] = out->m[1][1] = out->m[2][2] = out->m[3][3] = 1.0f;
}

void Multiply(Matrix* out, const Matrix* a, const Matrix* b) {
    d3dx::Get().multiply(out, a, b);
}

IDirect3DTexture9* FindCachedTexture(D3DRenderer* sub, const wchar_t* path) {
    if (auto* texture = native_mesh::Texture(path)) return static_cast<IDirect3DTexture9*>(texture);
    const auto* entry = FindTextureResource(sub, path);
    return entry ? reinterpret_cast<IDirect3DTexture9*>(entry->comObject) : nullptr;
}

void CachedTextureColor(D3DRenderer* sub, const wchar_t* path, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 1.0f;
    if (native_mesh::TextureProperties(path, out, nullptr)) return;
    if (const auto* entry = FindTextureResource(sub, path)) {
        const auto* tag = reinterpret_cast<const unsigned char*>(&entry->tag);
        out[0] = tag[0] * (1.0f / 256.0f);
        out[1] = tag[1] * (1.0f / 256.0f);
        out[2] = tag[2] * (1.0f / 256.0f);
    }
}

#if !MMD_ORIGINAL_PROFILE
bool CachedTextureUsesAlpha(D3DRenderer* sub, const wchar_t* path) {
    bool usesAlpha = false;
    if (native_mesh::TextureProperties(path, nullptr, &usesAlpha)) return usesAlpha;
    const auto* entry = FindTextureResource(sub, path);
    if (!entry) return false;
    const auto flags = reinterpret_cast<const unsigned char*>(&entry->tag)[3];
    return (flags & kTextureAlphaKnown) == 0 || (flags & kTextureUsesAlpha) != 0;
}

bool ModelMaterialUsesAlpha(D3DRenderer* sub, const mdl::ModelRecord& state,
                            const mdl::ModelMaterialRecord& material) {
    // The additive two-pass mode carries its own transparency handling and is
    // rejected by the sorted paths anyway.
    if (state.postLoadFlag2 != 0)
        return true;
    const float alpha = EffectiveMaterialAlpha(material.diffuse[3],
                                               state.displayState != 0);
    // Native alpha 0 emits no fragment. Keep the original classification when
    // MME is loaded: a custom FX can replace authored alpha, so moving that draw
    // to the opaque pass could change its visible result.
    if (!MaterialAlphaVisible(alpha) && GetModuleHandleW(L"MMEffect.dll") == nullptr)
        return false;
    return alpha < 1.0f ||
        CachedTextureUsesAlpha(sub, material.texturePath) ||
        ((state.physicsMode != 2 || material.sphereMode != 0) &&
            CachedTextureUsesAlpha(sub, material.spherePath));
}

void SortTransparentModelMaterials(D3DRenderer* sub, unsigned char* model,
                                  IDirect3DVertexBuffer9* vertices,
                                  IDirect3DIndexBuffer9* indices, UINT stride) {
    MMD_PERF_SCOPE(Sort);
    auto& state = *mdl::Mdl(model);
    // CPU source indices stay untouched for saving and stable coplanar order.
    const auto* pmxIndices = native_mesh::Indices32(model);
    const auto* pmdIndices = native_mesh::Indices32(model) == nullptr ? mdl::Indices(model) : nullptr;
    if (pmxIndices == nullptr && pmdIndices == nullptr)
        return;
    D3DINDEXBUFFER_DESC desc{};
    if (FAILED(indices->GetDesc(&desc)) ||
        (desc.Format != D3DFMT_INDEX16 && desc.Format != D3DFMT_INDEX32))
        return;
    const UINT indexSize = desc.Format == D3DFMT_INDEX32 ? 4 : 2;
    if (state.indexCount > desc.Size / indexSize)
        return;
    Matrix world, view, worldView;
    void* positions = nullptr;
    void* target = nullptr;
    bool locked = false;
    struct Range { UINT first, count; std::size_t cache; };
    thread_local std::vector<Range> ranges;
    ranges.clear();
    UINT sortedIndexCount = 0;
    UINT first = 0;
    const auto* materials = mdl::Materials(model);
    for (UINT i = 0; i < state.materialCount; ++i) {
        const auto& material = materials[i];
        const UINT count = static_cast<UINT>(material.faceVertexCount);
        if (count > state.indexCount - first)
            break;
        if (count != 0 && material.diffuse[3] > 0.0f &&
            ModelMaterialUsesAlpha(sub, state, material)) {
            // The translucent pass visits opaque models too. Avoid touching
            // their buffers after the opaque draw unless sorting is needed.
            if (!locked) {
                MMD_PERF_SCOPE(SortLock);
                sub->device->GetTransform(D3DTS_WORLD, reinterpret_cast<D3DMATRIX*>(&world));
                sub->device->GetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(&view));
                Multiply(&worldView, &world, &view);
                // Sorting only reads positions: a writable lock marks the managed
                // VB dirty and can trigger another upload in every MME pass.
                if (FAILED(vertices->Lock(0, 0, &positions, D3DLOCK_READONLY)))
                    return;
                if (FAILED(indices->Lock(0, 0, &target, 0))) {
                    vertices->Unlock();
                    return;
                }
                locked = true;
            }
            auto* cache = TransparentSortFrameCache::active;
            ranges.push_back({first, count, cache ? cache->EntryFor(vertices, indices, first, count)
                                                  : TransparentSortFrameCache::missing});
            sortedIndexCount += count;
        }
        first += count;
    }
    if (locked) {
        MMD_PERF_SCOPE(SortCpu);
        // Workers access disjoint material index ranges and thread-local
        // scratch only. All D3D calls and MME hooks stay on the render thread.
        const int rangeCount = static_cast<int>(ranges.size());
        const Range* workRanges = ranges.data();
        auto* cache = TransparentSortFrameCache::active;
#ifdef _OPENMP
        const int workers = GeometryWorkerCount();
#pragma omp parallel for if(sortedIndexCount >= 49152 && rangeCount > 1) num_threads(workers) schedule(dynamic, 1)
#endif
        for (int range = 0; range < rangeCount; ++range) {
            const UINT first = workRanges[range].first;
            const UINT count = workRanges[range].count;
            const auto sort = [&](auto* output) {
                for (UINT j = first; j < first + count; ++j)
                    output[j] = static_cast<std::remove_reference_t<decltype(output[j])>>(
                        pmxIndices != nullptr ? pmxIndices[j] : pmdIndices[2 * j]);
                SortTransparentTriangles(positions, stride, state.vertexCount,
                    output + first, count, *reinterpret_cast<D3DMATRIX*>(&worldView),
                    cache && workRanges[range].cache != TransparentSortFrameCache::missing
                        ? &cache->entries[workRanges[range].cache].order : nullptr);
            };
            if (indexSize == 4) sort(static_cast<std::uint32_t*>(target));
            else sort(static_cast<std::uint16_t*>(target));
        }
        indices->Unlock();
        vertices->Unlock();
    }
}

// Sorting rewrites GPU indices. Restore their CPU source order once on disable;
// keep the dirty flag if a device-loss lock fails so the next draw retries.
bool RestoreModelIndexOrder(MMDApp* app) {
    bool restored = true;
    MMD_PERF_SCOPE(Sort);
    for (int slot = 0; slot < kModelSlotCount; ++slot) {
        unsigned char* model = native_mesh::RenderModel(app->ModelSlot(slot));
        if (model == nullptr || mdl::Mdl(model)->loadComplete == 0)
            continue;
        auto& state = *mdl::Mdl(model);
        auto* indices = mdl::ResourceAs<IDirect3DIndexBuffer9>(
            mdl::Mdl(model)->indexBuffer);
        if (indices == nullptr || !PlausibleD3dObject(indices))
            continue;
        const auto* pmxIndices = native_mesh::Indices32(model);
        const auto* pmdIndices = native_mesh::Indices32(model) == nullptr ? mdl::Indices(model) : nullptr;
        if (pmxIndices == nullptr && pmdIndices == nullptr)
            continue;
        D3DINDEXBUFFER_DESC desc{};
        if (FAILED(indices->GetDesc(&desc)) ||
            (desc.Format != D3DFMT_INDEX16 && desc.Format != D3DFMT_INDEX32))
            continue;
        const UINT indexSize = desc.Format == D3DFMT_INDEX32 ? 4 : 2;
        if (state.indexCount > desc.Size / indexSize)
            continue;
        void* target = nullptr;
        if (FAILED(indices->Lock(0, 0, &target, 0))) {
            restored = false;
            continue;
        }
        const auto restore = [&](auto* output) {
            for (UINT j = 0; j < state.indexCount; ++j)
                output[j] = static_cast<std::remove_reference_t<decltype(output[j])>>(
                    pmxIndices != nullptr ? pmxIndices[j] : pmdIndices[2 * j]);
        };
        if (indexSize == 4) restore(static_cast<std::uint32_t*>(target));
        else restore(static_cast<std::uint16_t*>(target));
        if (FAILED(indices->Unlock())) restored = false;
    }
    return restored;
}
#endif

// A D3D9 object handed to a vtable method must be a committed user-mode
// address whose first qword - the vtable - points into an image's executable
// code (see include/mikudancestudio/d3d_object_guard.hpp).  A field that was
// never written - or was reached through an offset that only holds it on x86 -
// otherwise reaches the device as a sentinel such as -1 and the fault surfaces
// inside d3d9.dll, far from the caller that passed it.
void ReportInvalidResource(const char* what, unsigned char* model,
                           const void* pointer) noexcept {
    runtime_log::Write("RESOURCE invalid %s=%p model=%p", what, pointer,
                       static_cast<void*>(model));
    runtime_log::Trace("RESOURCE invalid %s=%p model=%p index_count=%u "
                       "vertex_count=%u", what, pointer,
                       static_cast<void*>(model),
                       model != nullptr
                           ? static_cast<unsigned>(mdl::Mdl(model)->indexCount)
                           : 0u,
                       model != nullptr
                           ? static_cast<unsigned>(mdl::Mdl(model)->vertexCount)
                           : 0u);
    if (model != nullptr) {
        runtime_log::WritePath("RESOURCE model path", mdl::Mdl(model)->path);
        char utf8[1024]{};
        if (WideCharToMultiByte(CP_UTF8, 0, mdl::Mdl(model)->path, -1, utf8,
                                sizeof(utf8), nullptr, nullptr))
            runtime_log::Trace("RESOURCE model path=%s", utf8);
    }
}

// Per-slot resource tripwire.  The model walk reports the first frame on which
// a slot's vertex/index buffer stops being a plausible D3D object, so a field
// session names the model and the moment instead of only the d3d9 fault.
std::uint8_t g_modelResourceState[kModelSlotCount];

void ValidateModelResources(unsigned char* model, int slot) noexcept {
    if (slot < 0 || slot >= kModelSlotCount)
        return;
    const void* vertexBuffer = mdl::Mdl(model)->vertexBuffer;
    const void* indexBuffer = mdl::Mdl(model)->indexBuffer;
    const std::uint8_t state =
        PlausibleD3dObject(vertexBuffer) && PlausibleD3dObject(indexBuffer)
            ? 1 : 2;
    if (g_modelResourceState[slot] == state)
        return;
    g_modelResourceState[slot] = state;
    runtime_log::Write("RESOURCE model slot=%d %s vb=%p ib=%p", slot,
                       state == 1 ? "ok" : "invalid", vertexBuffer,
                       indexBuffer);
    char utf8[1024]{};
    if (WideCharToMultiByte(CP_UTF8, 0, mdl::Mdl(model)->path, -1, utf8,
                            sizeof(utf8), nullptr, nullptr))
        runtime_log::Trace("RESOURCE model slot=%d %s vb=%p ib=%p path=%s",
                           slot, state == 1 ? "ok" : "invalid", vertexBuffer,
                           indexBuffer, utf8);
}

void ModelVertexFormat(unsigned char* model, DWORD* fvf, UINT* stride) {
    switch (mdl::Mdl(model)->pmxAdditionalUvCount) {
    case 1: *fvf = 524818;   *stride = 48; break;
    case 2: *fvf = 2622226;  *stride = 64; break;
    case 3: *fvf = 11011090; *stride = 80; break;
    case 4: *fvf = 44565778; *stride = 96; break;
    default: *fvf = 274;     *stride = 32; break;
    }
}

IDirect3DTexture9* ToonTexture(MMDApp* app, D3DRenderer* sub,
                               unsigned char* model,
                               unsigned char* material) {
    // 0x491E8B..0x492163: toonReference == -1 -> shared table slot 0; a
    // model whose indexed toon file name IS "toonNN.bmp" -> shared slot NN; any
    // other name resolves the texture itself (PMX: the material toon path
    // at +0x4C0; PMD: modelDirectory + converted file name).
    const int index = static_cast<std::int8_t>(
        mdl::Material(material).toonReference);
    if (index == -1)
        return app->ToonTexture(0);
    if (index >= 0 && index < 10) {
        const int shared = SharedToonSlot(mdl::PmdToonFileNames(model)[index]);
        if (shared != 0)
            return app->ToonTexture(shared);
    }
    if (mdl::Mdl(model)->physicsMode == 2) {
        // PMX custom toon (0x4920D7): cached texture for material+0x4C0.
        return FindCachedTexture(sub,
                                 mdl::Material(material).toonPath);
    }
    if (index >= 0 && index < 10) {
        // PMD custom toon (0x492111..0x492157): model directory + the
        // converted (SJIS -> wide) toon file name.
        wchar_t converted[256] = {};
        // The directory field alone is 256 wide characters, and the converted
        // toon name adds up to 100 more, so the old 256-character buffer could
        // only hold both by failing the format call (the original keeps
        // the same undersized destination at x64 0x1400B8D93, MSVCR90
        // swprintf_s with a count of 0x100).  Size the buffer for the whole path.
        // Defect RB-10.
        wchar_t path[512] = {};
        ConvertAnsiToWide(sub, mdl::PmdToonFileNames(model)[index],
                          converted, 0x100);
        swprintf_s(path, 0x200, L"%s%s",
                   mdl::Mdl(model)->modelDirectory, converted);
        return FindCachedTexture(sub, path);
    }
    return app->ToonTexture(0);
}

bool HasSuffix(const wchar_t* value, const wchar_t* lower,
               const wchar_t* upper);

void Cross3(float out[3], const float left[3], const float right[3]) {
#if defined(_MSC_VER) && defined(_M_IX86)
    // 0x491BB0..0x491C1A / 0x491C31..0x491C8D keep each
    // multiply-subtract in the x87 register stack until the float store.
    // A double-based equivalent differs by a few ULP after normalization.
    float* destination = out;
    const float* lhs = left;
    const float* rhs = right;
    __asm {
        mov eax, lhs
        mov ecx, rhs
        mov edx, destination

        fld dword ptr [eax + 4]
        fmul dword ptr [ecx + 8]
        fld dword ptr [eax + 8]
        fmul dword ptr [ecx + 4]
        fsubp st(1), st(0)
        fstp dword ptr [edx]

        fld dword ptr [eax + 8]
        fmul dword ptr [ecx]
        fld dword ptr [eax]
        fmul dword ptr [ecx + 8]
        fsubp st(1), st(0)
        fstp dword ptr [edx + 4]

        fld dword ptr [eax]
        fmul dword ptr [ecx + 4]
        fld dword ptr [eax + 4]
        fmul dword ptr [ecx]
        fsubp st(1), st(0)
        fstp dword ptr [edx + 8]
    }
#else
    out[0] = static_cast<float>(
        static_cast<double>(left[1]) * right[2] -
        static_cast<double>(left[2]) * right[1]);
    out[1] = static_cast<float>(
        static_cast<double>(left[2]) * right[0] -
        static_cast<double>(left[0]) * right[2]);
    out[2] = static_cast<float>(
        static_cast<double>(left[0]) * right[1] -
        static_cast<double>(left[1]) * right[0]);
#endif
}

void BuildToonTransform(MMDApp* app, Matrix* result) {
    auto& api = d3dx::Get();
    const D3DVECTOR& lightDirection = app->SceneLight().Direction;
    float direction[3] = {
        -lightDirection.x, -lightDirection.y, -lightDirection.z};
    api.vec3Normalize(direction, direction);

    const float vertical[3] = {
        0.0f, direction[0] == 0.0f && direction[2] == 0.0f ? 0.0f : 1.0f,
        direction[0] == 0.0f && direction[2] == 0.0f ? -1.0f : 0.0f};
    float horizontal[3]{};
    Cross3(horizontal, vertical, direction);
    api.vec3Normalize(horizontal, horizontal);
    float correctedVertical[3]{};
    Cross3(correctedVertical, direction, horizontal);
    api.vec3Normalize(correctedVertical, correctedVertical);

    Matrix basis{};
    for (int row = 0; row < 3; ++row) {
        basis.m[row][0] = horizontal[row];
        basis.m[row][1] = correctedVertical[row];
        basis.m[row][2] = direction[row];
    }
    basis.m[3][3] = 1.0f;

    Matrix rotation;
    Matrix scaling;
    Matrix translation;
    Matrix temporary;
    Matrix projected;
    // flt_530E00 = -(3.141592f * 0.5f) (pi/2 in the original's float
    // precision).  C++17 has no std::bit_cast; MSVC's __builtin_bit_cast
    // is accepted in constant expressions, so the static_assert pins the
    // literal to the original .rdata bit pattern.
    constexpr float kRotationAngle = -(3.141592f * 0.5f);
    static_assert(__builtin_bit_cast(std::uint32_t, kRotationAngle) ==
                      0xBFC90FD8u,
                  "flt_530E00 bit-exact");
    float rotationAngle = kRotationAngle;                 // 0x530E00
    api.rotX(&rotation, rotationAngle);
    api.scaling(&scaling, 0.5f, -0.5f, 1.0f);
    api.translation(&translation, 0.5f, 0.5f, 0.0f);
    api.multiply(&temporary, &basis, &rotation);
    api.multiply(&projected, &temporary, &scaling);
    api.multiply(result, &projected, &translation);
}

// Faithful transcription of sub_4912F0's per-material texture-cascade setup
// (0x491470..0x491B38 + LABEL_46 0x491E41..0x492163).  The original only
// ever touches COLOROP/TEXCOORDINDEX/TEXTURETRANSFORMFLAGS inside the model
// loop; ARG1/ARG2/ALPHAOP keep the InitRenderStates values (0x406E90) and
// bound textures persist across materials - unbinding stage 1 or overriding
// ALPHAOP here would deviate (the shadow/live capture diffs of 2026-08-25
// traced back to exactly that).
void DisableSphereTextureStage(IDirect3DDevice9* device) {
    // Stage 2 is a per-material sphere-map cascade.  An inactive cascade is
    // not merely COLOROP-disabled: the original also leaves it unbound with
    // vertex UV set 2 and texture transformation disabled.  Keeping the
    // camera-space-normal state from a previous sphere map changes later
    // materials on some D3D9 drivers even while COLOROP is disabled.
    device->SetTexture(2, nullptr);
    device->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS,
                                 D3DTTFF_DISABLE);
    device->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, 2);
}

void ConfigureMaterialStages(MMDApp* app, D3DRenderer* sub,
                             IDirect3DDevice9* device,
                             unsigned char* model,
                             unsigned char* material) {
    const mdl::ModelMaterialRecord& record = mdl::Material(material);
    const wchar_t* mainPath = record.texturePath;
    const wchar_t* spherePath = record.spherePath;
    DisableSphereTextureStage(device);

    if (mdl::Mdl(model)->physicsMode == 2) {
        // half-texel / identity stage transforms (v103 / v79 at 0x491353..)
        Matrix halfTexel;
        Identity(&halfTexel);
        halfTexel.m[0][0] = 0.5f;
        halfTexel.m[1][1] = -0.5f;
        halfTexel.m[2][2] = 0.0f;
        halfTexel.m[3][0] = 0.5f;
        halfTexel.m[3][1] = 0.5f;
        Matrix identityTexel;
        Identity(&identityTexel);
        const auto setStageTransform = [&](DWORD stage, const Matrix& m) {
            device->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(
                                     D3DTS_TEXTURE0 + stage),
                                 reinterpret_cast<const D3DMATRIX*>(&m));
        };
        if (mainPath[0] != L'\0') {
            // PMX main texture: 0x4914D2..0x49152B
            device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
            device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTexture(1, FindCachedTexture(sub, mainPath));
            // PMX sphere map on stage 2: 0x491530..0x4916B3
            const unsigned char sphereMode = record.sphereMode;
            if (spherePath[0] != L'\0' && sphereMode != 0) {
                switch (sphereMode) {
                case 1:
                    setStageTransform(2, halfTexel);
                    device->SetTextureStageState(2, D3DTSS_COLOROP,
                                                 D3DTOP_MODULATE);
                    device->SetTextureStageState(2,
                                                 D3DTSS_TEXTURETRANSFORMFLAGS,
                                                 D3DTTFF_COUNT2);
                    device->SetTextureStageState(
                        2, D3DTSS_TEXCOORDINDEX,
                        D3DTSS_TCI_CAMERASPACENORMAL);
                    break;
                case 2:
                    setStageTransform(2, halfTexel);
                    device->SetTextureStageState(2, D3DTSS_COLOROP,
                                                 D3DTOP_ADD);
                    device->SetTextureStageState(2,
                                                 D3DTSS_TEXTURETRANSFORMFLAGS,
                                                 D3DTTFF_COUNT2);
                    device->SetTextureStageState(
                        2, D3DTSS_TEXCOORDINDEX,
                        D3DTSS_TCI_CAMERASPACENORMAL);
                    break;
                case 3:
                    setStageTransform(2, identityTexel);
                    device->SetTextureStageState(2, D3DTSS_COLOROP,
                                                 D3DTOP_MODULATE);
                    device->SetTextureStageState(2,
                                                 D3DTSS_TEXTURETRANSFORMFLAGS,
                                                 D3DTTFF_COUNT2);
                    device->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, 1);
                    break;
                default:
                    break;  // 0x491558 default: bind only, no stage change
                }
                device->SetTexture(2, FindCachedTexture(sub, spherePath));
            }
        } else if (spherePath[0] != L'\0' && record.sphereMode != 0) {
            // PMX sphere map promoted to stage 1: 0x4916DC..0x491835
            const unsigned char sphereMode = record.sphereMode;
            switch (sphereMode) {
            case 1:
                setStageTransform(1, halfTexel);
                device->SetTextureStageState(1, D3DTSS_COLOROP,
                                             D3DTOP_MODULATE);
                device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS,
                                             D3DTTFF_COUNT2);
                device->SetTextureStageState(
                    1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);
                break;
            case 2:
                setStageTransform(1, halfTexel);
                device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
                device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS,
                                             D3DTTFF_COUNT2);
                device->SetTextureStageState(
                    1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);
                break;
            case 3:
                setStageTransform(1, identityTexel);
                device->SetTextureStageState(1, D3DTSS_COLOROP,
                                             D3DTOP_MODULATE);
                device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS,
                                             D3DTTFF_COUNT2);
                device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
                break;
            default:
                break;
            }
            device->SetTexture(1, FindCachedTexture(sub, spherePath));
        } else {
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        }
    } else if (mainPath[0] == L'\0') {
        // PMD empty main path: 0x491A78 disables stage 1 and keeps whatever
        // texture is still bound (stale binding, inert under COLOROP).
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    } else if (HasSuffix(mainPath, L".sph", L".SPH")) {
        // 0x4919BC + common tail 0x4919D3..0x4918C0
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS,
                                     D3DTTFF_COUNT2);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX,
                                     D3DTSS_TCI_CAMERASPACENORMAL);
        device->SetTexture(1, FindCachedTexture(sub, mainPath));
    } else if (HasSuffix(mainPath, L".spa", L".SPA")) {
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS,
                                     D3DTTFF_COUNT2);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX,
                                     D3DTSS_TCI_CAMERASPACENORMAL);
        device->SetTexture(1, FindCachedTexture(sub, mainPath));
    } else {
        // Generic texture (incl. the ".tga"/toon check that lands here too):
        // 0x49189C..0x4918FC
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTexture(1, FindCachedTexture(sub, mainPath));
    }

    // Sphere field (PMD): 0x491901..0x491B38 / LABEL_45 0x491B23.  With no
    // sphere texture the original ONLY disables stage 2 COLOROP - TCI/TF
    // keep their InitRenderStates values.
    if (mdl::Mdl(model)->physicsMode != 2) {
        if (spherePath[0] != L'\0') {
            device->SetTextureStageState(2, D3DTSS_COLOROP,
                HasSuffix(spherePath, L".spa", L".SPA") ? D3DTOP_ADD
                                                        : D3DTOP_MODULATE);
            device->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS,
                                         D3DTTFF_COUNT2);
            device->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX,
                                         D3DTSS_TCI_CAMERASPACENORMAL);
            device->SetTexture(2, FindCachedTexture(sub, spherePath));
        }
    }

    // Toon stage 0 (LABEL_46): 0x491E41..0x492163
    Matrix toonTransform;
    BuildToonTransform(app, &toonTransform);
    device->SetTransform(D3DTS_TEXTURE0,
                          reinterpret_cast<const D3DMATRIX*>(&toonTransform));
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                 D3DTTFF_COUNT2);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
                                 D3DTSS_TCI_CAMERASPACENORMAL);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetTexture(0, ToonTexture(app, sub, model, material));
}

// Loop-tail stage-1 reset for ".sp*" main paths: 0x4925CE..0x4925E5.
void ResetSphereStageAfterDraw(IDirect3DDevice9* device,
                               unsigned char* material) {
    const wchar_t* mainPath = mdl::Material(material).texturePath;
    if (HasSuffix(mainPath, L".sp", L".SP")) {
        device->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
        device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
    }
}

bool HasSuffix(const wchar_t* value, const wchar_t* lower,
               const wchar_t* upper) {
    return value != nullptr &&
           (wcsstr(value, lower) != nullptr || wcsstr(value, upper) != nullptr);
}

void SetEffectColor(void* effect, const char* name, const float source[4]) {
    float value[4] = {source[0], source[1], source[2], source[3]};
    for (float& channel : value)
        channel = std::min(channel, 1.0f);
    FxSetFloatArray(effect, name, value, 4);
}

void SelectPmxTechnique(D3DRenderer* sub, IDirect3DDevice9* device,
                        void* effect, unsigned char* material) {
    const mdl::ModelMaterialRecord& record = mdl::Material(material);
    const wchar_t* mainPath = record.texturePath;
    const wchar_t* spherePath = record.spherePath;
    IDirect3DTexture9* mainTexture = FindCachedTexture(sub, mainPath);
    const unsigned char sphereMode = record.sphereMode;

    device->SetTexture(1, mainTexture);
    device->SetTexture(2, nullptr);
    if (mainTexture != nullptr) {
        IDirect3DTexture9* sphereTexture =
            sphereMode != 0 && spherePath[0] != L'\0'
                ? FindCachedTexture(sub, spherePath)
                : nullptr;
        if (sphereTexture != nullptr) {
            device->SetTexture(2, sphereTexture);
            if (sphereMode == 1 || sphereMode == 2) {
                FxSetTechnique(effect, "BShadowSphiaTextureTec");
                FxSetInt(effect, "spadd", sphereMode == 2);
            } else if (sphereMode == 3) {
                FxSetTechnique(effect, "BShadowTextureTexCd2Tec");
            } else {
                FxSetTechnique(effect, "BShadowTextureTec");
            }
        } else {
            FxSetTechnique(effect, "BShadowTextureTec");
        }
        return;
    }

    IDirect3DTexture9* sphereTexture =
        sphereMode != 0 && spherePath[0] != L'\0'
            ? FindCachedTexture(sub, spherePath)
            : nullptr;
    if (sphereTexture == nullptr) {
        FxSetTechnique(effect, "BufferShadowTec");
        return;
    }
    device->SetTexture(1, sphereTexture);
    if (sphereMode == 1 || sphereMode == 2) {
        FxSetTechnique(effect, "BShadowSphiaTec");
        FxSetInt(effect, "spadd", sphereMode == 2);
    } else if (sphereMode == 3) {
        FxSetTechnique(effect, "BShadowTexCd2Tec");
    } else {
        FxSetTechnique(effect, "BufferShadowTec");
    }
}

void SelectPmdTechnique(D3DRenderer* sub, IDirect3DDevice9* device,
                        void* effect, unsigned char* material) {
    const mdl::ModelMaterialRecord& record = mdl::Material(material);
    const wchar_t* mainPath = record.texturePath;
    const wchar_t* spherePath = record.spherePath;
    if (mainPath[0] == L'\0') {
        device->SetTexture(1, nullptr);
        device->SetTexture(2, nullptr);
        FxSetTechnique(effect, "BufferShadowTec");
        return;
    }

    if (HasSuffix(mainPath, L".sph", L".SPH") ||
        HasSuffix(mainPath, L".spa", L".SPA")) {
        IDirect3DTexture9* sphereTexture = FindCachedTexture(sub, mainPath);
        device->SetTexture(1, sphereTexture);
        device->SetTexture(2, nullptr);
        if (sphereTexture != nullptr) {
            FxSetTechnique(effect, "BShadowSphiaTec");
            FxSetInt(effect, "spadd",
                     HasSuffix(mainPath, L".spa", L".SPA") ? 1 : 0);
        } else {
            FxSetTechnique(effect, "BufferShadowTec");
        }
        return;
    }

    IDirect3DTexture9* mainTexture = FindCachedTexture(sub, mainPath);
    device->SetTexture(1, mainTexture);
    device->SetTexture(2, nullptr);
    if (mainTexture == nullptr) {
        FxSetTechnique(effect, "BufferShadowTec");
        return;
    }
    IDirect3DTexture9* sphereTexture = spherePath[0] != L'\0'
        ? FindCachedTexture(sub, spherePath) : nullptr;
    if (sphereTexture != nullptr) {
        device->SetTexture(2, sphereTexture);
        FxSetTechnique(effect, "BShadowSphiaTextureTec");
        FxSetInt(effect, "spadd",
                 HasSuffix(spherePath, L".spa", L".SPA") ? 1 : 0);
    } else {
        FxSetTechnique(effect, "BShadowTextureTec");
    }
}

void ConfigureEffectMaterial(MMDApp* app, D3DRenderer* sub,
                             IDirect3DDevice9* device, void* effect,
                             unsigned char* model,
                             unsigned char* material, UINT materialIndex) {
    const mdl::ModelMaterialRecord& record = mdl::Material(material);
    const float alpha = record.diffuse[3];
    const float diffuse[4] = {record.diffuse[0], record.diffuse[1],
                              record.diffuse[2], alpha};
    const float ambient[4] = {record.diffuseMirror[0], record.diffuseMirror[1],
                              record.diffuseMirror[2], alpha};
    const float emissive[4] = {record.ambient[0], record.ambient[1],
                               record.ambient[2], alpha};
    const float specular[4] = {record.specular[0], record.specular[1],
                               record.specular[2], alpha};
    const D3DCOLORVALUE& lightAmbient = app->SceneLight().Ambient;
    const float edge[4] = {
        ambient[0] * lightAmbient.r + emissive[0],
        ambient[1] * lightAmbient.g + emissive[1],
        ambient[2] * lightAmbient.b + emissive[2], alpha};
    SetEffectColor(effect, "EgColor", edge);
    // The skin shaders sample DifColor for the diffuse term.  The previous
    // port only populated the diagnostic MatDifColor variable, leaving the
    // shader's actual material color at its default and producing white
    // models in the scene viewport.
    SetEffectColor(effect, "DifColor", diffuse);
    SetEffectColor(effect, "MatDifColor", diffuse);
    SetEffectColor(effect, "MatAmbColor", ambient);
    SetEffectColor(effect, "MatEmsColor", emissive);
    SetEffectColor(effect, "MatSpcColor", specular);

    float toon[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const int toonIndex = static_cast<std::int8_t>(record.toonReference);
    if (toonIndex != -1) {
        bool standard = false;
        if (toonIndex >= 0 && toonIndex < 10) {
            const int shared = SharedToonSlot(mdl::PmdToonFileNames(model)[toonIndex]);
            standard = shared != 0;
            if (standard) {
                const float* table = app->state.toonEdgeTable;
                toon[0] = table[3 * (shared - 1) + 0];
                toon[1] = table[3 * (shared - 1) + 1];
                toon[2] = table[3 * (shared - 1) + 2];
            }
        }
        if (!standard) {
            if (mdl::Mdl(model)->physicsMode == 2) {
                CachedTextureColor(sub, record.toonPath, toon);
            } else if (toonIndex >= 0 && toonIndex < 10) {
                wchar_t converted[256] = {};
                // Same 256-character destination as the toon lookup above; it
                // cannot hold a 256-character directory plus the converted
                // name, which is what this path builds. Size the buffer for both.
                // Defect RB-10.
                wchar_t path[512] = {};
                ConvertAnsiToWide(sub, mdl::PmdToonFileNames(model)[toonIndex],
                                  converted, 0x100);
                swprintf_s(path, 0x200, L"%s%s",
                    mdl::Mdl(model)->modelDirectory,
                    converted);
                CachedTextureColor(sub, path, toon);
            }
        }
    }
    if (mdl::Mdl(model)->physicsMode == 2) {
        const mdl::MaterialMorphChannels& add =
            mdl::MaterialMorphAdd(model)[materialIndex].channels;
        const mdl::MaterialMorphChannels& mul =
            mdl::MaterialMorphMul(model)[materialIndex].channels;
        for (int i = 0; i < 3; ++i)
            toon[i] = toon[i] * mul.toonTint[i] + add.toonTint[i];
        toon[3] = add.toonTint[3] + mul.toonTint[3];
    }
    FxSetFloatArray(effect, "ToonColor", toon, 4);

    const D3DCOLORVALUE& lightSpecular = app->SceneLight().Specular;
    float litSpecular[4] = {
        specular[0] * lightSpecular.r, specular[1] * lightSpecular.g,
        specular[2] * lightSpecular.b, record.specularPower};
    if (litSpecular[3] == 0.0f)
        litSpecular[3] = 0.1f;
    FxSetFloatArray(effect, "SpcColor", litSpecular, 4);

    const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (mdl::Mdl(model)->physicsMode == 2) {
        const mdl::MaterialMorphChannels& add =
            mdl::MaterialMorphAdd(model)[materialIndex].channels;
        const mdl::MaterialMorphChannels& mul =
            mdl::MaterialMorphMul(model)[materialIndex].channels;
        FxSetFloatArray(effect, "TexCAdd",
                        add.textureTint, 4);
        FxSetFloatArray(effect, "TexCMul",
                        mul.textureTint, 4);
        FxSetFloatArray(effect, "SphCAdd",
                        add.sphereTint, 4);
        FxSetFloatArray(effect, "SphCMul",
                        mul.sphereTint, 4);
        SelectPmxTechnique(sub, device, effect, material);
    } else {
        FxSetFloatArray(effect, "TexCAdd", zero, 4);
        FxSetFloatArray(effect, "TexCMul", one, 4);
        FxSetFloatArray(effect, "SphCAdd", zero, 4);
        FxSetFloatArray(effect, "SphCMul", one, 4);
        SelectPmdTechnique(sub, device, effect, material);
    }
}

struct MaterialSelection {
    UINT material;
    UINT firstIndex;
    UINT indexCount = 0;
    IDirect3DIndexBuffer9* buffer = nullptr;
    IDirect3DVertexBuffer9* vertices = nullptr;
    UINT vertexCount = 0;
    UINT vertexStride = 0;
#if !MMD_ORIGINAL_PROFILE
    TransparentPeeling* peel = nullptr;
    bool probe = false;
    bool* valid = nullptr;
#endif
};

void DrawModelMaterials(MMDApp* app, unsigned char* model, bool effectPass,
                        bool shadowOnly, ModelAlphaPass alphaPass = ModelAlphaPass::Original,
                        const MaterialSelection* selection = nullptr, bool* shadowSucceeded = nullptr) {
    MMD_PERF_SCOPE(Materials);
    if (model == nullptr || mdl::Mdl(model)->loadComplete == 0)
        return;
    mdl::ModelRecord& state = *mdl::Mdl(model);
    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    auto* effect = sub->effect;
    auto* materials = mdl::Materials(model);
    auto* vertices = mdl::ResourceAs<IDirect3DVertexBuffer9>(
        mdl::Mdl(model)->vertexBuffer);
    auto* indices = mdl::ResourceAs<IDirect3DIndexBuffer9>(
        mdl::Mdl(model)->indexBuffer);
    if (selection && selection->buffer) indices = selection->buffer;
    if (selection && selection->vertices) vertices = selection->vertices;
    if (device == nullptr || materials == nullptr || vertices == nullptr ||
        indices == nullptr)
        return;
    // A pointer that survived the null test can still be a sentinel: any code
    // path that leaves the model's resource fields unwritten makes every
    // SetStreamSource/SetIndices/DrawIndexedPrimitive call fault inside
    // d3d9.dll.  Screen both buffers before the material loop binds them.
    if (!PlausibleD3dObject(vertices) || !PlausibleD3dObject(indices)) {
        if (!PlausibleD3dObject(vertices))
            ReportInvalidResource("vertex_buffer", model, vertices);
        if (!PlausibleD3dObject(indices))
            ReportInvalidResource("index_buffer", model, indices);
        return;
    }

    DWORD fvf;
    UINT stride;
    ModelVertexFormat(model, &fvf, &stride);
    if (selection && selection->vertexStride) stride = selection->vertexStride;
#if !MMD_ORIGINAL_PROFILE
    if (selection && selection->peel && !selection->probe)
        fvf = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX6 |
            D3DFVF_TEXCOORDSIZE4(1) | D3DFVF_TEXCOORDSIZE4(2) |
            D3DFVF_TEXCOORDSIZE4(3) | D3DFVF_TEXCOORDSIZE4(4) | D3DFVF_TEXCOORDSIZE1(5);
#endif
    const UINT vertexCount = selection && selection->vertices
        ? selection->indexCount : state.vertexCount;
    const UINT minVertex = selection && selection->vertices ? selection->firstIndex : 0;
#if !MMD_ORIGINAL_PROFILE
    // Prepare native indices before opaque materials submit this model's
    // buffers. Keep the original timing when an MME proxy is present: its
    // callbacks may mutate material state between the two material passes.
    const bool mmeProxy = GetModuleHandleW(L"MMEffect.dll") != nullptr;
    const auto sortPass = mmeProxy ? ModelAlphaPass::Translucent
                                  : ModelAlphaPass::Opaque;
    const bool autoSorting = app->AutoTransparentMaterialSortingEnabled();
    auto& indexOrderDirty = app->TransparentMaterialIndexOrderDirty();
    if (!autoSorting && indexOrderDirty && RestoreModelIndexOrder(app))
        indexOrderDirty = false;
    if (autoSorting && !selection && alphaPass == sortPass && !shadowOnly) {
        indexOrderDirty = true;
        SortTransparentModelMaterials(sub, model, vertices, indices, stride);
    }
#else
    alphaPass = ModelAlphaPass::Original;
#endif
    UINT firstIndex = selection ? selection->firstIndex : 0;
    const UINT firstMaterial = selection ? selection->material : 0;
    const UINT lastMaterial = selection ? firstMaterial + 1 : state.materialCount;
    state.toonShared = firstMaterial - 1;
    for (UINT i = firstMaterial; i < lastMaterial; ++i) {
        unsigned char* material = reinterpret_cast<unsigned char*>(&materials[i]);
        const mdl::ModelMaterialRecord& record = materials[i];
        const UINT indexCount = selection && selection->buffer ? selection->indexCount
            : static_cast<UINT>(record.faceVertexCount);
        ++state.toonShared;
        if (indexCount == 0)
            continue;

        // A native alpha texture that is authored opaque: submit the texels
        // whose alpha is exactly 255 together with the opaque materials, so a
        // solid pixel behind them is no longer rejected by the depth a
        // semi-transparent pixel in front of it left in the buffer. The
        // translucent pass still blends the soft edge and still writes the
        // depth that keeps inverted-hull outlines behind this material.
#if !MMD_ORIGINAL_PROFILE
        const bool usesAlpha = !shadowOnly &&
            alphaPass != ModelAlphaPass::Original &&
            ModelMaterialUsesAlpha(sub, state, record);
        const bool solidFirst = !mmeProxy && !shadowOnly &&
            alphaPass == ModelAlphaPass::Opaque && usesAlpha &&
            record.diffuse[3] >= 1.0f && state.postLoadFlag2 == 0 &&
            state.displayState == 0;
#else
        constexpr bool usesAlpha = false;
        constexpr bool solidFirst = false;
#endif
        bool draw = true;
        if (!shadowOnly && alphaPass != ModelAlphaPass::Original) {
            draw = solidFirst || ModelMaterialMatchesPass(alphaPass, usesAlpha);
        }
        if (shadowOnly) {
            // PMX outline size is independent of shadow casting (flag bit 2).
            if (record.diffuse[3] == 0.9800000190734863f)
                draw = false;
            // 0x492925: PMX shadow-cast disable bit (bit 2 of material+2240)
            // skips the material in the shadow map when clear (PMX mode only).
            if (mdl::Mdl(model)->physicsMode == 2 && (record.flags >> 2 & 1) == 0)
                draw = false;
        }
        if (draw) {
#if !MMD_ORIGINAL_PROFILE
            const bool opaqueCoverage=!mmeProxy&&!effectPass&&!shadowOnly&&
                state.displayState==0&&record.diffuse[3]>=1.f&&!ModelMaterialUsesAlpha(sub,state,record);
            deformed_mesh_capture::MaterialScope exportMaterial(model, i, !shadowOnly && !(selection && selection->probe),opaqueCoverage);
#endif
            D3DMATERIAL9 d3dMaterial{};
            std::memcpy(&d3dMaterial, material, sizeof(d3dMaterial));
            if (state.displayState != 0)
                d3dMaterial.Diffuse.a = 0.5f;
            // Solid-pixel pre-pass: the engine's alpha function already is
            // GREATER_EQUAL, so raising the reference to 255 keeps exactly the
            // texels whose alpha is 255 and rejects every soft edge. Both the
            // fixed path and the built-in effect path take it, each raising the
            // reference at the point its own state setup ends - the effect pass
            // first, because FxBeginPass installs the pass state block.
            DWORD savedAlphaRef = 1;

            if (!effectPass && !shadowOnly) {
                if (solidFirst) {
                    device->GetRenderState(D3DRS_ALPHAREF, &savedAlphaRef);
                    device->SetRenderState(D3DRS_ALPHAREF, 255);
                }
                // Fixed-function model pass, transcribed from sub_4912F0.
                // Blend-state prefix (0x491470..0x49149A) runs for every
                // material; texture cascade and toon setup follow
                // (ConfigureMaterialStages); the draw branches below carry
                // the original CULLMODE semantics:
                //   postLoadFlag2 != 0 -> additive two-pass transparents
                //                          (DESTBLEND=ONE, back faces with
                //                          CULL_CW first, then CULL_CCW)
                //   alpha < 1          -> single pass with CULL_NONE
                //   opaque             -> cull untouched
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                device->SetRenderState(D3DRS_DESTBLEND,
                                       D3DBLEND_INVSRCALPHA);
                ConfigureMaterialStages(app, sub, device, model, material);
                device->SetFVF(fvf);
                bool stateDumped = false;
                const auto issueDraw = [&]() {
                    device->SetStreamSource(0, vertices, 0, stride);
                    device->SetIndices(indices);
                    if (!stateDumped) {
                        DumpMaterialState(app, device, model, material, i,
                                          firstIndex);
                        stateDumped = true;
                    }
                    device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, minVertex,
                                                 vertexCount,
                                                 firstIndex, indexCount / 3);
                };
                // 0x492203 compares the COPY's Diffuse.a (esp+0x138),
                // which carries the 0.5 displayState override - not the
                // raw record value.
                const float alpha = d3dMaterial.Diffuse.a;
                if (state.postLoadFlag2 != 0) {
                    // 0x4921D1..0x49237A
                    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
                    device->SetMaterial(&d3dMaterial);
                    if (alpha < 1.0f) {
                        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
                        issueDraw();
                        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
                    }
                    issueDraw();
                    device->SetRenderState(D3DRS_DESTBLEND,
                                           D3DBLEND_INVSRCALPHA);
                } else if (alpha < 1.0f ||
                           (mdl::Mdl(model)->physicsMode == 2 &&
                            (record.flags & 1) != 0)) {
                    // 0x4923D5..0x4924CE
                    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                    device->SetRenderState(D3DRS_SRCBLEND,
                                           D3DBLEND_SRCALPHA);
                    device->SetRenderState(D3DRS_DESTBLEND,
                                           D3DBLEND_INVSRCALPHA);
                    device->SetMaterial(&d3dMaterial);
                    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                    issueDraw();
                    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
                } else {
                    device->SetMaterial(&d3dMaterial);
                    issueDraw();
                }
                ResetSphereStageAfterDraw(device, material);
                if (solidFirst)
                    device->SetRenderState(D3DRS_ALPHAREF, savedAlphaRef);
            } else {
                if (shadowOnly) {
                    // For PMX, the legacy byte named doubleSided actually
                    // stores the outline-enable flag. Use the real two-sided
                    // flag, just as the visible material pass does.
                    const bool twoSided = state.physicsMode == 2
                        ? (record.flags & 1) != 0 : record.doubleSided != 0;
                    device->SetRenderState(
                        D3DRS_CULLMODE,
                        twoSided ? D3DCULL_NONE : D3DCULL_CCW);
                    // 0x492954: BeginPass(effect, 0) per material; the
                    // enclosing FxBegin was issued by RenderShadowMap
                    // before the model loop (its FxEnd closes it).
                    if (effectPass && effect != nullptr) {
                        const HRESULT result=FxBeginPass(effect, 0);
                        if(shadowSucceeded&&FAILED(result)){*shadowSucceeded=false;return;}
                    }
                } else {
                    device->SetMaterial(&d3dMaterial);
                }
                if (effectPass && effect != nullptr && !shadowOnly) {
                    device->SetRenderState(D3DRS_CULLMODE,
                        record.diffuse[3] >= 1.0f ||
                                mdl::Mdl(model)->physicsMode == 2
                            ? D3DCULL_CCW : D3DCULL_NONE);
                    device->SetRenderState(D3DRS_DESTBLEND,
                        state.postLoadFlag2 != 0 ? D3DBLEND_ONE
                                          : D3DBLEND_INVSRCALPHA);
                    if (mdl::Mdl(model)->physicsMode == 2 && (record.flags & 1) != 0)
                        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                    ConfigureEffectMaterial(app, sub, device, effect,
                                            model, material, i);
                    UINT passes = 0;
                    FxBegin(effect, &passes);
                    device->SetTexture(0,
                        mdl::Mdl(model)->physicsMode == 2 && (record.flags & 8) == 0
                            ? sub->spriteTexture
                            : shadow_filter::VisibleTexture(app));
                    FxBeginPass(effect, 0);
                    // Raise the reference after the pass installed its state
                    // block, so the built-in effect path drops the soft edge
                    // from its opaque-coverage draw exactly like the fixed one.
                    if (solidFirst) {
                        device->GetRenderState(D3DRS_ALPHAREF, &savedAlphaRef);
                        device->SetRenderState(D3DRS_ALPHAREF, 255);
                    }
                }
                const HRESULT fvfResult=device->SetFVF(fvf);
                const HRESULT streamResult=device->SetStreamSource(0, vertices, 0, stride);
                const HRESULT indicesResult=device->SetIndices(indices);
                if(shadowSucceeded&&(FAILED(fvfResult)||FAILED(streamResult)||FAILED(indicesResult)))
                    *shadowSucceeded=false;
                bool issue = true;
#if !MMD_ORIGINAL_PROFILE
                if (selection && selection->peel) {
                    if (selection->probe) device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, minVertex, vertexCount, firstIndex, 0);
                    const bool accepted = selection->probe ? selection->peel->NativePass()
                        : selection->peel->BeginMaterial(effect);
                    if (selection->valid) *selection->valid &= accepted;
                    issue = accepted && !selection->probe;
                }
#endif
                if (issue) {
                    const HRESULT drawn=device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, minVertex,
                                             vertexCount, firstIndex, indexCount / 3);
                    if(shadowSucceeded&&FAILED(drawn))*shadowSucceeded=false;
                }
#if !MMD_ORIGINAL_PROFILE
                if (selection && selection->peel && !selection->probe) selection->peel->EndMaterial();
#endif
                if (effectPass && effect != nullptr) {
                    if (solidFirst)
                        device->SetRenderState(D3DRS_ALPHAREF, savedAlphaRef);
                    const HRESULT ended=FxEndPass(effect);
                    if(shadowSucceeded&&FAILED(ended))*shadowSucceeded=false;
                    if (!shadowOnly) {
                        FxEnd(effect);
                        if (mdl::Mdl(model)->physicsMode == 2 && (record.flags & 1) != 0)
                            device->SetRenderState(D3DRS_CULLMODE,
                                                   D3DCULL_CCW);
                    }
                }
            }
        }
        firstIndex += indexCount;
    }
    state.toonShared = static_cast<std::uint32_t>(-1);
    // 0x4925FD..0x49261D: terminate the fixed-function texture cascade at
    // stage 1 before outlines, accessories, or the next model are submitted.
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
}


#if !MMD_ORIGINAL_PROFILE
// Keep large or special materials as ordering barriers. Reordering is confined
// to contiguous, bounded runs of alpha materials; interleaving can split draws.
struct NativeAlphaDraw {
    unsigned char* model;
    MaterialSelection selection;
    bool effect;
    bool candidate;
};
constexpr UINT kAlphaOrderMaterialTriangles = 4096;
constexpr UINT kAlphaOrderGroupTriangles = 8192;
constexpr UINT kAlphaOrderFrameTriangles = 16384;
// Upper bound on the split draws all accepted fallback groups may emit per frame.
constexpr std::size_t kAlphaOrderFrameSplitDraws = 512;

bool ProjectAlphaGroup(MMDApp* app, const std::vector<NativeAlphaDraw>& draws,
                       std::size_t begin, std::size_t end,
                       std::vector<AlphaMaterialGeometry>& geometry,
                       std::vector<std::vector<UINT>>& sourceOffsets) {
    auto* sub = app->Renderer();
    Matrix world, view, projection, worldView, fixedTransform;
    if (FAILED(sub->device->GetTransform(D3DTS_WORLD, reinterpret_cast<D3DMATRIX*>(&world))) ||
        FAILED(sub->device->GetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(&view))) ||
        FAILED(sub->device->GetTransform(D3DTS_PROJECTION, reinterpret_cast<D3DMATRIX*>(&projection))))
        return false;
    Multiply(&worldView, &world, &view);
    Multiply(&fixedTransform, &worldView, &projection);
    struct VertexRead {
        IDirect3DVertexBuffer9* buffer = nullptr;
        const unsigned char* data = nullptr;
        void Release() { if (buffer) buffer->Unlock(); buffer = nullptr; data = nullptr; }
        ~VertexRead() { Release(); }
    } read;
    unsigned char* currentModel = nullptr;
    UINT stride = 0;
    geometry.resize(end - begin);
    sourceOffsets.resize(end - begin);
    for (std::size_t d = begin; d < end; ++d) {
        const auto& draw = draws[d];
        const Matrix& transform = draw.effect
            ? *reinterpret_cast<const Matrix*>(&app->WorldViewProjection()) : fixedTransform;
        const auto& state = *mdl::Mdl(draw.model);
        if (currentModel != draw.model) {
            read.Release();
            auto* buffer = static_cast<IDirect3DVertexBuffer9*>(state.vertexBuffer);
            if (!PlausibleD3dObject(buffer)) return false;
            DWORD fvf;
            ModelVertexFormat(draw.model, &fvf, &stride);
            D3DVERTEXBUFFER_DESC desc{};
            if (stride < 3 * sizeof(float) || FAILED(buffer->GetDesc(&desc)) ||
                std::size_t(state.vertexCount) * stride > desc.Size)
                return false;
            void* data = nullptr;
            if (FAILED(buffer->Lock(0, 0, &data, D3DLOCK_READONLY))) return false;
            read.buffer = buffer;
            read.data = static_cast<const unsigned char*>(data);
            currentModel = draw.model;
        }
        const auto* pmx = native_mesh::Indices32(draw.model);
        const auto* pmd = native_mesh::Indices32(draw.model) == nullptr ? mdl::Indices(draw.model) : nullptr;
        if (!pmx && !pmd) return false;
        const auto& material = mdl::Materials(draw.model)[draw.selection.material];
        const bool pmxMaterial = state.physicsMode == 2;
        const bool doubleSided = (pmxMaterial && (material.flags & 1) != 0) ||
            (material.diffuse[3] < 1.0f && (!draw.effect || !pmxMaterial)) ||
            (!draw.effect && state.displayState != 0);
        const UINT count = static_cast<UINT>(material.faceVertexCount);
        if (count % 3 || draw.selection.firstIndex > state.indexCount ||
            count > state.indexCount - draw.selection.firstIndex) return false;
        auto& triangles = geometry[d - begin].triangles;
        triangles.clear();
        auto& sources = sourceOffsets[d - begin];
        sources.clear();
        triangles.reserve(count / 3);
        sources.reserve(count / 3);
        for (UINT offset = 0; offset < count; offset += 3) {
            AlphaScreenTriangle triangle{};
            for (UINT corner = 0; corner < 3; ++corner) {
                const UINT at = draw.selection.firstIndex + offset + corner;
                const UINT index = static_cast<UINT>(pmx ? pmx[at] : pmd[2 * at]);
                if (index >= state.vertexCount) return false;
                float position[3];
                std::memcpy(position, read.data + std::size_t(index) * stride, sizeof(position));
                const float x = position[0], y = position[1], z = position[2];
                const float w = x * transform.m[0][3] + y * transform.m[1][3] + z * transform.m[2][3] + transform.m[3][3];
                if (!std::isfinite(w) || w <= 0.0f) return false;
                auto& out = triangle.p[corner];
                out.x = (x * transform.m[0][0] + y * transform.m[1][0] + z * transform.m[2][0] + transform.m[3][0]) / w;
                out.y = (x * transform.m[0][1] + y * transform.m[1][1] + z * transform.m[2][1] + transform.m[3][1]) / w;
                out.z = (x * transform.m[0][2] + y * transform.m[1][2] + z * transform.m[2][2] + transform.m[3][2]) / w;
                // Near/far-plane clipping is left to the existing draw path.
                // A run crossing either plane cannot use this unclipped proof.
                if (!std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z) ||
                    out.z < 0.0f || out.z > 1.0f) return false;
            }
            const double area = (double(triangle.p[1].x) - triangle.p[0].x) *
                                    (double(triangle.p[2].y) - triangle.p[0].y) -
                                (double(triangle.p[1].y) - triangle.p[0].y) *
                                    (double(triangle.p[2].x) - triangle.p[0].x);
            if (area == 0.0 || (!doubleSided && area > 0.0)) continue;
            triangles.push_back(triangle);
            sources.push_back(draw.selection.firstIndex + offset);
        }
    }
    return true;
}

// Opt-in stage timing for the native ordered-alpha path.  A plain counter
// guard keeps QueryPerformanceCounter (and the frequency query) out of ordinary
// runs: the block below is the only place this file calls either API, and it is
// reached only while MIKUDANCESTUDIO_TRACE_FILE names a non-empty path.  Every
// stage is timed on its own - the caller reads Now() immediately before the
// call and adds Elapsed(start) immediately after it - so the totals never
// subtract one stage's timestamp from another's.  Durations accumulate over all
// groups of a frame, and a stage that is not entered this frame keeps its 0.
struct AlphaStageTimer {
    bool enabled = false;
    LARGE_INTEGER frequency{};
    double projectMs = 0.0;
    double orderMs = 0.0;
    double batchMs = 0.0;
    double uploadMs = 0.0;
    double drawMs = 0.0;

    AlphaStageTimer() {
        const char* path = std::getenv("MIKUDANCESTUDIO_TRACE_FILE");
        enabled = path != nullptr && path[0] != '\0';
        if (enabled) QueryPerformanceFrequency(&frequency);
    }

    LARGE_INTEGER Now() const {
        LARGE_INTEGER value{};
        if (enabled) QueryPerformanceCounter(&value);
        return value;
    }

    double Elapsed(const LARGE_INTEGER& start) const {
        if (!enabled || frequency.QuadPart == 0) return 0.0;
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        if (end.QuadPart < start.QuadPart) return 0.0;
        return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 /
            static_cast<double>(frequency.QuadPart);
    }

    void Report() const {
        if (!enabled) return;
        runtime_log::Trace("alpha-timing project_ms=%.4f order_ms=%.4f "
                           "batch_ms=%.4f upload_ms=%.4f draw_ms=%.4f",
                           projectMs, orderMs, batchMs, uploadMs, drawMs);
    }
};

bool RenderNativeOrderedAlpha(MMDApp* app, bool effectFrame) {
    if (GetModuleHandleW(L"MMEffect.dll") != nullptr || app->WireframeRenderingEnabled() != 0 ||
        (effectFrame && app->ModelNonDisplayMode() != 0)) return false;
    auto* sub = app->Renderer();
    thread_local std::vector<NativeAlphaDraw> draws;
    draws.clear();
    bool adjacentCandidates = false;
    const auto orderedModels = BuildModelDrawOrder(app,
        [](unsigned char* object) { return native_mesh::RenderModel(object); });
    for (const auto& entry : orderedModels) {
        auto* model = entry.model;
        if (!model) continue;
        const auto& state = *mdl::Mdl(model);
        const auto* materials = mdl::Materials(model);
        if (!state.loadComplete || !materials) continue;
        // Translucent display keeps its original record-order pass
        // (x64 0x1400B6D70); the ordered paths must not collect these
        // materials, or the model would be drawn a second time.
        if (state.displayState != 0) continue;
        UINT first = 0;
        for (UINT i = 0; i < state.materialCount; ++i) {
            const auto& material = materials[i];
            const UINT count = static_cast<UINT>(material.faceVertexCount);
            if (first > state.indexCount || count > state.indexCount - first) return false;
            if (count && ModelMaterialUsesAlpha(sub, state, material)) {
                const bool candidate = count % 3 == 0 && count / 3 <= kAlphaOrderMaterialTriangles &&
                    std::isfinite(material.diffuse[3]) && material.diffuse[3] > 0.0f &&
                    state.postLoadFlag2 == 0;
                if (candidate && !draws.empty() && draws.back().candidate) adjacentCandidates = true;
                draws.push_back({model, {i, first}, effectFrame && state.toonFlag != 0, candidate});
            }
            first += count;
        }
    }
    if (!adjacentCandidates) return false; // No geometry reads on the fast path.
    // Constructed only past the early outs, so a frame without candidate runs
    // never reaches the counter calls even while the trace file is set.
    AlphaStageTimer timing;
    thread_local std::vector<AlphaMaterialGeometry> geometry;
    thread_local std::vector<std::vector<UINT>> sourceOffsets;
    struct CachedPlan {
        std::vector<AlphaMaterialGeometry> geometry;
        std::vector<std::size_t> order;
        std::vector<AlphaTriangleReference> triangles;
        AlphaBatchOverlapCache overlap;
        AlphaOrderDiagnostic diagnostic;
        AlphaOrderResult result = AlphaOrderResult::Unsupported;
        bool valid = false, batched = false, staticRefined = false;
    };
    thread_local CachedPlan cache;
    thread_local std::vector<NativeAlphaDraw> commands;
    thread_local std::vector<std::uint32_t> packedIndices;
    commands.clear();
    packedIndices.clear();
    UINT frameTriangles = 0;
    // Split draws already emitted by accepted fallback groups this frame.
    std::size_t frameSplitDraws = 0;
    bool changed = false;
    for (std::size_t begin = 0; begin < draws.size();) {
        if (!draws[begin].candidate) { commands.push_back(draws[begin++]); continue; }
        std::size_t end = begin;
        UINT triangles = 0;
        while (end < draws.size() && draws[end].candidate) {
            triangles += static_cast<UINT>(mdl::Materials(draws[end].model)
                [draws[end].selection.material].faceVertexCount) / 3;
            ++end;
        }
        bool planned = false;
        if (end - begin >= 2 && end - begin <= 64 && triangles <= kAlphaOrderGroupTriangles &&
            triangles <= kAlphaOrderFrameTriangles - frameTriangles) {
            frameTriangles += triangles;
            const auto projectStart = timing.Now();
            const bool projected = ProjectAlphaGroup(app, draws, begin, end, geometry, sourceOffsets);
            timing.projectMs += timing.Elapsed(projectStart);
            if (projected) {
                bool same = cache.valid && cache.geometry.size() == geometry.size();
                for (std::size_t i = 0; same && i < geometry.size(); ++i) {
                    const auto& current = geometry[i].triangles;
                    const auto& previous = cache.geometry[i].triangles;
                    same = current.size() == previous.size() &&
                        (current.empty() || std::memcmp(current.data(), previous.data(),
                            current.size() * sizeof(AlphaScreenTriangle)) == 0);
                }
                if (!same) {
                    cache.geometry = geometry;
                    cache.order.resize(end - begin);
                    std::iota(cache.order.begin(), cache.order.end(), std::size_t(0));
                    const auto orderStart = timing.Now();
                    cache.result = OrderAlphaMaterials(geometry, cache.order, 262144, &cache.diagnostic);
                    timing.orderMs += timing.Elapsed(orderStart);
                    cache.triangles.clear();
                    cache.batched = cache.result == AlphaOrderResult::Unsupported &&
                        std::strcmp(cache.diagnostic.reason, "invalid") != 0 &&
                        std::strcmp(cache.diagnostic.reason, "budget") != 0;
                    if (cache.batched) {
                        const auto batchStart = timing.Now();
                        cache.batched = BatchAlphaTriangles(geometry, cache.triangles, 262144, &cache.overlap);
                        timing.batchMs += timing.Elapsed(batchStart);
                    }
                    cache.valid = true;
                    cache.staticRefined = false;
                }
                // The overlap cache pads triangles for moving geometry. Once the
                // geometry held still, rebuild once without that cache: the
                // padding is dropped, the fixed anchors stay untouched, and the
                // retry is not repeated on later frames.
                if (same && cache.batched && !cache.staticRefined) {
                    std::vector<AlphaTriangleReference> refined;
                    const auto batchStart = timing.Now();
                    const bool refinedOk = BatchAlphaTriangles(geometry, refined, 262144, nullptr);
                    timing.batchMs += timing.Elapsed(batchStart);
                    if (refinedOk) cache.triangles.swap(refined);
                    cache.staticRefined = true;
                }
                const auto result = cache.result;
                const auto& diagnostic = cache.diagnostic;
                runtime_log::Trace("alpha-order first=%u materials=%zu triangles=%u result=%d reason=%s pair=%zu,%zu cached=%u",
                    draws[begin].selection.material, end - begin, triangles, static_cast<int>(result),
                    diagnostic.reason, diagnostic.first, diagnostic.second, same ? 1u : 0u);
                if (result != AlphaOrderResult::Unsupported) {
                    for (auto index : cache.order) commands.push_back(draws[begin + index]);
                    changed |= result == AlphaOrderResult::Reordered;
                    planned = true;
                } else if (std::strcmp(diagnostic.reason, "invalid") != 0 &&
                           std::strcmp(diagnostic.reason, "budget") != 0) {
                    // A material cannot describe interleaved facial layers.
                    // Sort only this bounded run's triangles; consecutive uses
                    // of a material share one draw. Intersecting triangles remain
                    // a centroid approximation, as in the material-local sorter.
                    const auto& work = cache.triangles;
                    const bool batched = cache.batched;
                    const auto commandStart = commands.size();
                    const auto indexStart = packedIndices.size();
                    std::size_t last = geometry.size();
                    for (const auto& t : work) {
                        const auto& draw = draws[begin + t.material];
                        if (last != t.material) {
                            auto command = draw;
                            command.selection.firstIndex = static_cast<UINT>(packedIndices.size());
                            command.selection.indexCount = 0;
                            commands.push_back(command);
                            last = t.material;
                        }
                        auto* model = draw.model;
                        const UINT source = sourceOffsets[t.material][t.triangle];
                        const bool pmx = mdl::Mdl(model)->physicsMode == 2;
                        for (UINT v = 0; v < 3; ++v)
                            packedIndices.push_back(pmx ? mdl::PmxIndices(model)[source + v]
                                                        : mdl::Indices(model)[2 * (source + v)]);
                        commands.back().selection.indexCount += 3;
                    }
                    const auto runs = commands.size() - commandStart;
                    // Bound state changes as well as geometry work. If the run
                    // is too fragmented, keep the complete original group.
                    if (!batched || runs > 256 ||
                        runs > kAlphaOrderFrameSplitDraws - frameSplitDraws) {
                        commands.resize(commandStart);
                        packedIndices.resize(indexStart);
                    } else if (!work.empty()) {
                        frameSplitDraws += runs;
                        planned = changed = true;
                    }
                    runtime_log::Trace("alpha-triangle first=%u triangles=%zu runs=%zu accepted=%u",
                        draws[begin].selection.material, work.size(), runs, planned ? 1u : 0u);
                }
            }
        }
        if (!planned) commands.insert(commands.end(), draws.begin() + begin, draws.begin() + end);
        begin = end;
    }
    if (!changed) return false;
    // Frame-local scratch avoids retaining default-pool resources across device
    // resets or model replacement. Only fallback groups allocate/upload indices.
    struct IndexScratch {
        IDirect3DIndexBuffer9* buffer = nullptr;
        ~IndexScratch() { if (buffer) buffer->Release(); }
    } scratch;
    const auto uploadStart = timing.Now();
    if (!packedIndices.empty()) {
        const UINT bytes = static_cast<UINT>(packedIndices.size() * sizeof(std::uint32_t));
        if (FAILED(sub->device->CreateIndexBuffer(bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
            D3DFMT_INDEX32, D3DPOOL_DEFAULT, &scratch.buffer, nullptr))) return false;
        void* data = nullptr;
        if (FAILED(scratch.buffer->Lock(0, bytes, &data, D3DLOCK_DISCARD))) return false;
        std::memcpy(data, packedIndices.data(), bytes);
        if (FAILED(scratch.buffer->Unlock())) return false;
    }
    timing.uploadMs += timing.Elapsed(uploadStart);
    const auto drawStart = timing.Now();
    for (auto draw : commands) {
        if (draw.selection.indexCount) draw.selection.buffer = scratch.buffer;
        app->ActiveRenderObject() = draw.model;
        app->ActiveRenderPass() = draw.effect ? AccessoryRenderPass::Effect : AccessoryRenderPass::FixedFunction;
        if (draw.effect) sub->device->SetTexture(0, shadow_filter::VisibleTexture(app));
        DrawModelMaterials(app, draw.model, draw.effect, false, ModelAlphaPass::Translucent, &draw.selection);
    }
    timing.drawMs += timing.Elapsed(drawStart);
    timing.Report();
    if (scratch.buffer) sub->device->SetIndices(nullptr);
    app->ActiveRenderObject() = nullptr;
    return true;
}

// Geometry splitting is independent of the material shader. It preserves the
// original material identity (including MME callbacks) for every emitted run.
struct AlphaClipVertex {
    double clip[4]{};
    float data[24]{};
};

double AlphaClipDistance(const AlphaClipVertex& v, int plane) {
    switch (plane) {
    case 0: return v.clip[3] + v.clip[0];
    case 1: return v.clip[3] - v.clip[0];
    case 2: return v.clip[3] + v.clip[1];
    case 3: return v.clip[3] - v.clip[1];
    case 4: return v.clip[2];
    default: return v.clip[3] - v.clip[2];
    }
}

void ClipAlphaTriangle(const AlphaClipVertex input[3], UINT draw, UINT ordinal,
                       std::vector<transparent_geometry::Triangle>& output) {
    std::vector<AlphaClipVertex> polygon(input, input + 3), next;
    for (int plane = 0; plane < 6 && !polygon.empty(); ++plane) {
        next.clear();
        auto previous = polygon.back();
        double previousDistance = AlphaClipDistance(previous, plane);
        for (const auto& current : polygon) {
            const double distance = AlphaClipDistance(current, plane);
            if ((distance >= 0) != (previousDistance >= 0)) {
                const double t = previousDistance / (previousDistance - distance);
                AlphaClipVertex crossing{};
                for (int i = 0; i < 4; ++i)
                    crossing.clip[i] = previous.clip[i] + t * (current.clip[i] - previous.clip[i]);
                for (int i = 0; i < 24; ++i)
                    crossing.data[i] = float(previous.data[i] + t * (double(current.data[i]) - previous.data[i]));
                next.push_back(crossing);
            }
            if (distance >= 0) next.push_back(current);
            previous = current;
            previousDistance = distance;
        }
        polygon.swap(next);
    }
    for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
        transparent_geometry::Triangle triangle{};
        triangle.draw = draw;
        triangle.ordinal = ordinal;
        const AlphaClipVertex* corners[] = {&polygon[0], &polygon[i], &polygon[i + 1]};
        bool valid = true;
        for (int c = 0; c < 3; ++c) {
            const auto& v = *corners[c];
            if (!(v.clip[3] > 1e-20)) { valid = false; break; }
            auto& out = triangle.v[c];
            out.inverseW = 1.0 / v.clip[3];
            for (int j = 0; j < 3; ++j) out.p[j] = v.clip[j] * out.inverseW;
            std::memcpy(out.data, v.data, sizeof(out.data));
        }
        if (valid) output.push_back(triangle);
    }
}

bool CollectAlphaGeometry(MMDApp* app, const std::vector<NativeAlphaDraw>& draws,
                          std::vector<transparent_geometry::Triangle>& triangles) {
    auto* device = app->Renderer()->device;
    Matrix world{}, view{}, projection{}, worldView{}, fixedTransform{};
    if (FAILED(device->GetTransform(D3DTS_WORLD, reinterpret_cast<D3DMATRIX*>(&world))) ||
        FAILED(device->GetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(&view))) ||
        FAILED(device->GetTransform(D3DTS_PROJECTION, reinterpret_cast<D3DMATRIX*>(&projection)))) return false;
    Multiply(&worldView, &world, &view);
    Multiply(&fixedTransform, &worldView, &projection);
    UINT ordinal = 0;
    for (UINT d = 0; d < draws.size(); ++d) {
        const auto& draw = draws[d];
        const auto& state = *mdl::Mdl(draw.model);
        const Matrix& transform = draw.effect
            ? *reinterpret_cast<const Matrix*>(&app->WorldViewProjection()) : fixedTransform;
        auto* vb = static_cast<IDirect3DVertexBuffer9*>(state.vertexBuffer);
        if (!PlausibleD3dObject(vb)) return false;
        DWORD fvf; UINT stride;
        ModelVertexFormat(draw.model, &fvf, &stride);
        D3DVERTEXBUFFER_DESC desc{};
        if (FAILED(vb->GetDesc(&desc)) || std::size_t(state.vertexCount) * stride > desc.Size) return false;
        void* data = nullptr;
        if (FAILED(vb->Lock(0, 0, &data, D3DLOCK_READONLY))) return false;
        struct Unlock { IDirect3DVertexBuffer9* buffer; ~Unlock() { buffer->Unlock(); } } unlock{vb};
        const auto* pmx = native_mesh::Indices32(draw.model);
        const auto* pmd = native_mesh::Indices32(draw.model) == nullptr ? mdl::Indices(draw.model) : nullptr;
        bool valid = (pmx || pmd) && stride <= sizeof(AlphaClipVertex::data);
        const UINT count = mdl::Materials(draw.model)[draw.selection.material].faceVertexCount;
        for (UINT offset = 0; valid && offset < count; offset += 3, ++ordinal) {
            AlphaClipVertex vertices[3]{};
            for (UINT c = 0; c < 3; ++c) {
                const UINT at = draw.selection.firstIndex + offset + c;
                const UINT index = pmx ? UINT(pmx[at]) : UINT(pmd[2 * at]);
                if (index >= state.vertexCount) { valid = false; break; }
                auto& v = vertices[c];
                std::memcpy(v.data, static_cast<const char*>(data) + std::size_t(index) * stride, stride);
                for (int j = 0; j < 4; ++j) {
                    v.clip[j] = double(v.data[0]) * transform.m[0][j] +
                                double(v.data[1]) * transform.m[1][j] +
                                double(v.data[2]) * transform.m[2][j] + transform.m[3][j];
                    if (!std::isfinite(v.clip[j])) valid = false;
                }
            }
            if (valid) ClipAlphaTriangle(vertices, d, ordinal, triangles);
        }
        if (!valid) return false;
    }
    return true;
}

// Optional, explicit diagnostic output: projected geometry, never a desktop capture.
void DumpAlphaGeometry(const std::vector<transparent_geometry::Triangle>& source,
                       const std::vector<transparent_geometry::Triangle>& sorted) {
    const char* path = std::getenv("MIKUDANCESTUDIO_ALPHA_GEOMETRY_DUMP");
    if (!path || !*path || source.empty()) return;
    thread_local bool written = false;
    if (written) return;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") || !file) return;
    const char magic[8] = {'M','D','S','B','S','P','0','1'};
    const std::uint64_t counts[] = {source.size(), sorted.size()};
    bool ok = std::fwrite(magic, 1, 8, file) == 8 && std::fwrite(counts, sizeof(counts), 1, file) == 1;
    for (const auto* list : {&source, &sorted}) {
        for (const auto& triangle : *list) {
            for (const auto& vertex : triangle.v) ok &= std::fwrite(vertex.p, sizeof(vertex.p), 1, file) == 1;
            const std::uint32_t identity[] = {triangle.draw, triangle.ordinal};
            ok &= std::fwrite(identity, sizeof(identity), 1, file) == 1;
        }
    }
    ok &= std::fclose(file) == 0;
    written = ok;
}


bool RenderPeelingAlpha(MMDApp* app,const std::vector<NativeAlphaDraw>& draws,
                       const std::vector<transparent_geometry::Triangle>& input) {
    const auto precision=app->TransparentMaterialSortPrecision();
    // Only Maximum peels. The three lower strengths only reorder whole faces.
    if(precision!=TransparentSortPrecision::Maximum)return false;
    if(input.empty())return false;
    for(const auto& draw:draws)if(!draw.effect)return false;
    for(const auto& triangle:input)if(triangle.ordinal>=16777216u)return false;
    auto* device=app->Renderer()->device;
    TransparentPeeling peel(device);if(!peel.Ready())return false;
    const auto oldPass=app->ActiveRenderPass();auto* oldObject=app->ActiveRenderObject();
    struct Restore {MMDApp* app;AccessoryRenderPass pass;void* object;
        ~Restore(){app->ActiveRenderPass()=pass;app->ActiveRenderObject()=object;}
    } restore{app,oldPass,oldObject};
    bool valid=true;
    for(auto command:draws){
        command.selection.peel=&peel;command.selection.probe=true;command.selection.valid=&valid;
        app->ActiveRenderObject()=command.model;app->ActiveRenderPass()=AccessoryRenderPass::Effect;
        DrawModelMaterials(app,command.model,true,false,ModelAlphaPass::Translucent,&command.selection);
        if(!valid){runtime_log::Trace("alpha-peel unsupported shader");return false;}
    }
    if(input.size()>std::numeric_limits<UINT>::max()/300 || !peel.PrepareTargets())return false;
    struct Buffers {IDirect3DVertexBuffer9* vb=nullptr;IDirect3DIndexBuffer9* ib=nullptr;
        ~Buffers(){if(vb)vb->Release();if(ib)ib->Release();}
    } buffers;
    const UINT count=UINT(input.size()*3);
    if(FAILED(device->CreateVertexBuffer(count*100,D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&buffers.vb,nullptr)) ||
       FAILED(device->CreateIndexBuffer(count*4,D3DUSAGE_WRITEONLY,D3DFMT_INDEX32,D3DPOOL_DEFAULT,&buffers.ib,nullptr)))return false;
    void* memory=nullptr;
    if(FAILED(buffers.vb->Lock(0,0,&memory,0)))return false;
    auto* data=static_cast<float*>(memory);
    // GREATEREQUAL makes the last equal-depth fragment win. Reverse both the
    // vertex upload and the draw groups below so that winner has the smallest
    // ordinal, matching the shader's increasing-ID tie progression.
    for(auto i=input.rbegin();i!=input.rend();++i)for(const auto& vertex:i->v){std::memcpy(data,vertex.data,96);data[24]=float(i->ordinal);data+=25;}
    if(FAILED(buffers.vb->Unlock()) || FAILED(buffers.ib->Lock(0,0,&memory,0)))return false;
    for(UINT i=0;i<count;++i)static_cast<UINT*>(memory)[i]=i;
    if(FAILED(buffers.ib->Unlock()))return false;
    // Maximum remains bounded by the geometry
    // itself: a peeled triangle occupies at most one layer, so input.size()
    // layers consume the whole set and the iteration after them is the empty
    // query that reports completion.  EndLayer already ends the peel on that
    // empty query, so no counting pre-pass is needed.  The input size check
    // above (<= UINT_MAX/300) keeps this cast and the +1 from overflowing.
    const unsigned limit=unsigned(input.size())+1;
    unsigned layers=0;bool complete=false;
    for(;layers<limit;++layers){
        app->ActiveRenderObject()=nullptr;app->ActiveRenderPass()=AccessoryRenderPass::None;
        if(!peel.BeginLayer(layers))break;
        for(std::size_t first=0;first<input.size();){
            const auto d=input[input.size()-1-first].draw;std::size_t end=first+1;
            while(end<input.size() && input[input.size()-1-end].draw==d)++end;
            auto command=draws[d];
            command.selection.firstIndex=UINT(first*3);command.selection.indexCount=UINT((end-first)*3);
            command.selection.buffer=buffers.ib;command.selection.vertices=buffers.vb;
            command.selection.vertexCount=count;command.selection.vertexStride=100;command.selection.peel=&peel;command.selection.valid=&valid;
            app->ActiveRenderObject()=command.model;app->ActiveRenderPass()=AccessoryRenderPass::Effect;
            DrawModelMaterials(app,command.model,true,false,ModelAlphaPass::Translucent,&command.selection);
            first=end;
        }
        if(!valid)break;
        app->ActiveRenderObject()=nullptr;app->ActiveRenderPass()=AccessoryRenderPass::None;
        const int result=peel.EndLayer();
        if(result==0){complete=true;break;}if(result<0)break;
    }
    // The private accumulation is disposable until every layer is complete.
    // A budget/device/effect failure must replay the whole set via the fallback,
    // rather than leave only the already processed far layers in the scene.
    app->ActiveRenderObject()=nullptr;app->ActiveRenderPass()=AccessoryRenderPass::None;
    const bool committed = complete && valid && peel.Commit();
    runtime_log::Trace("alpha-peel input=%zu layers=%u complete=%u committed=%u precision=%u limit=%u",
                       input.size(),layers,complete?1:0,committed?1:0,unsigned(precision),limit);
    return committed;
}

bool RenderSplitOrderedAlphaImpl(MMDApp* app, bool effectFrame) {
    if (app->WireframeRenderingEnabled() || (effectFrame && app->ModelNonDisplayMode())) return false;
    std::vector<NativeAlphaDraw> draws;
    auto* sub = app->Renderer();
    // Additive materials retain their authored placement. The regular alpha
    // set must be complete: no model/material/triangle-count ordering barriers.
    const auto orderedModels = BuildModelDrawOrder(app,
        [](unsigned char* object) { return native_mesh::RenderModel(object); });
    for (const auto& entry : orderedModels) {
        auto* model = entry.model;
        if (!model) continue;
        const auto& state = *mdl::Mdl(model);
        const auto* materials = mdl::Materials(model);
        if (!state.loadComplete || !materials) continue;
        // Translucent display keeps its original record-order pass
        // (x64 0x1400B6D70); the ordered paths must not collect these
        // materials, or the model would be drawn a second time.
        if (state.displayState != 0) continue;
        UINT first = 0;
        for (UINT i = 0; i < state.materialCount; ++i) {
            const auto& material = materials[i];
            const UINT count = UINT(material.faceVertexCount);
            if (count % 3 || first > state.indexCount || count > state.indexCount - first) return false;
            if (count && ModelMaterialUsesAlpha(sub, state, material)) {
                if (state.postLoadFlag2 || !std::isfinite(material.diffuse[3])) return false;
                draws.push_back({model, {i, first}, effectFrame && state.toonFlag != 0, true});
            }
            first += count;
        }
    }
    if (draws.empty()) return false;
    thread_local std::vector<transparent_geometry::Triangle> input;
    input.clear();
    if (!CollectAlphaGeometry(app, draws, input)) return false;
    if (effectFrame && RenderPeelingAlpha(app, draws, input)) return true;
    D3DVIEWPORT9 batchViewport{};
    // Unknown MME shaders can move vertices or have effects outside a source
    // triangle. Native passes alone have the screen-coverage contract needed
    // to move disjoint triangles without changing their output.
    const bool batchNative = GetModuleHandleW(L"MMEffect.dll") == nullptr &&
        SUCCEEDED(sub->device->GetViewport(&batchViewport));
    struct Plan {
        std::vector<transparent_geometry::Triangle> source, sorted;
        TransparentSortPrecision precision = TransparentSortPrecision::Low;
        transparent_geometry::Statistics stats;
        bool batchNative = false;
        DWORD width = 0, height = 0;
    };
    thread_local Plan cache;
    const auto precision = app->TransparentMaterialSortPrecision();
    const bool same = precision == cache.precision && batchNative == cache.batchNative &&
        batchViewport.Width == cache.width && batchViewport.Height == cache.height &&
        input.size() == cache.source.size() &&
        (input.empty() || std::memcmp(input.data(), cache.source.data(), input.size() * sizeof(input[0])) == 0);
    if (!same) {
        cache.sorted = transparent_geometry::Order(input, precision, cache.stats);
        if (batchNative)
            transparent_geometry::BatchDraws(cache.sorted, batchViewport.Width, batchViewport.Height);
        cache.source = input;
        cache.precision = precision;
        cache.batchNative = batchNative;
        cache.width = batchViewport.Width;
        cache.height = batchViewport.Height;
        runtime_log::Trace("alpha-bsp precision=%u input=%zu output=%zu splits=%zu limited=%u buckets=%zu",
            unsigned(precision), cache.stats.inputTriangles, cache.stats.outputTriangles,
            cache.stats.splitCount, cache.stats.limited ? 1u : 0u, cache.stats.bucketCount);
    }
    DumpAlphaGeometry(input, cache.sorted);
    if (cache.sorted.empty()) return true;
    if (cache.sorted.size() > std::numeric_limits<UINT>::max() / (3 * 96)) return false;
    const UINT vertexCount = UINT(cache.sorted.size() * 3);
    struct Buffers {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
        ~Buffers() { if (vb) vb->Release(); if (ib) ib->Release(); }
    } scratch;
    auto* device = sub->device;
    if (FAILED(device->CreateVertexBuffer(vertexCount * 96, D3DUSAGE_WRITEONLY, 0,
                                          D3DPOOL_DEFAULT, &scratch.vb, nullptr)) ||
        FAILED(device->CreateIndexBuffer(vertexCount * 4, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
                                         D3DPOOL_DEFAULT, &scratch.ib, nullptr))) return false;
    void* destination = nullptr;
    if (FAILED(scratch.vb->Lock(0, 0, &destination, 0))) return false;
    auto* floats = static_cast<float*>(destination);
    for (const auto& triangle : cache.sorted) {
        for (const auto& vertex : triangle.v) {
            std::memcpy(floats, vertex.data, sizeof(vertex.data));
            floats += 24;
        }
    }
    if (FAILED(scratch.vb->Unlock())) return false;
    if (FAILED(scratch.ib->Lock(0, 0, &destination, 0))) return false;
    for (UINT i = 0; i < vertexCount; ++i) static_cast<UINT*>(destination)[i] = i;
    if (FAILED(scratch.ib->Unlock())) return false;
    IDirect3DVertexBuffer9* oldVertices = nullptr;
    IDirect3DIndexBuffer9* oldIndices = nullptr;
    IDirect3DVertexDeclaration9* oldDeclaration = nullptr;
    UINT oldOffset = 0, oldStride = 0; DWORD oldFvf = 0;
    device->GetStreamSource(0, &oldVertices, &oldOffset, &oldStride);
    device->GetIndices(&oldIndices);
    device->GetFVF(&oldFvf);
    device->GetVertexDeclaration(&oldDeclaration);
    const auto oldPass = app->ActiveRenderPass();
    auto* oldObject = app->ActiveRenderObject();
    for (std::size_t first = 0; first < cache.sorted.size();) {
        std::size_t end = first + 1;
        const UINT d = cache.sorted[first].draw;
        while (end < cache.sorted.size() && cache.sorted[end].draw == d) ++end;
        auto command = draws[d];
        command.selection.firstIndex = UINT(first * 3);
        command.selection.indexCount = UINT((end - first) * 3);
        command.selection.buffer = scratch.ib;
        command.selection.vertices = scratch.vb;
        command.selection.vertexCount = vertexCount;
        command.selection.vertexStride = 96;
        app->ActiveRenderObject() = command.model;
        app->ActiveRenderPass() = command.effect ? AccessoryRenderPass::Effect : AccessoryRenderPass::FixedFunction;
        if (command.effect) device->SetTexture(0, shadow_filter::VisibleTexture(app));
        DrawModelMaterials(app, command.model, command.effect, false, ModelAlphaPass::Translucent, &command.selection);
        first = end;
    }
    device->SetStreamSource(0, oldVertices, oldOffset, oldStride);
    device->SetIndices(oldIndices);
    if (oldDeclaration) device->SetVertexDeclaration(oldDeclaration);
    else if (oldFvf) device->SetFVF(oldFvf);
    if (oldDeclaration) oldDeclaration->Release();
    if (oldVertices) oldVertices->Release();
    if (oldIndices) oldIndices->Release();
    app->ActiveRenderPass() = oldPass;
    app->ActiveRenderObject() = oldObject;
    return true;
}

bool RenderSplitOrderedAlpha(MMDApp* app, bool effectFrame) {
    // Planning and all uploads finish before the first draw; allocation failure
    // can therefore fall back without compositing the same surface twice.
    try { return RenderSplitOrderedAlphaImpl(app, effectFrame); }
    catch (const std::bad_alloc&) {
        runtime_log::Write("alpha-bsp allocation failed; retaining previous draw path");
        return false;
    }
}
#endif

void DrawModelEdgeGeometry(MMDApp* app, unsigned char* model,
                           bool projectedShadow, bool effectEdge) {
    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    void* effect = sub->effect;
    if (model == nullptr || mdl::Mdl(model)->loadComplete == 0 ||
        mdl::Mdl(model)->displayState != 0)
        return;
    mdl::ModelRecord& state = *mdl::Mdl(model);
    auto* vb = mdl::ResourceAs<IDirect3DVertexBuffer9>(
        mdl::Mdl(model)->vertexBuffer2);
    auto* ib = mdl::ResourceAs<IDirect3DIndexBuffer9>(
        mdl::Mdl(model)->indexBuffer);
    auto* materials = mdl::Materials(model);
    if (vb == nullptr || ib == nullptr || materials == nullptr)
        return;

    UINT firstIndex = 0;
    state.toonShared = static_cast<std::uint32_t>(-1);
    for (UINT i = 0; i < state.materialCount; ++i) {
        auto* material = reinterpret_cast<unsigned char*>(&materials[i]);
        const mdl::ModelMaterialRecord& record = materials[i];
        ++state.toonShared;
        const UINT count = static_cast<UINT>(record.faceVertexCount);
        bool draw = count != 0;
        if (!projectedShadow && record.doubleSided == 0)
            draw = false;
        const bool pmx = mdl::Mdl(model)->physicsMode == 2;
        if (pmx && projectedShadow && (record.flags & 2) == 0)
            draw = false;
        if (pmx && projectedShadow && sub->postProcessEnabled != 0 &&
            record.edgeSize <= 0.0f)
            draw = false;
        if (draw) {
            if (effectEdge && pmx && sub->postProcessEnabled != 0) {
                FxEndPass(effect);
                FxSetFloatArray(effect, "EgColor", record.edgeColor, 4);
                FxBeginPass(effect, 0);
            }
            device->SetFVF(66);
            device->SetStreamSource(0, vb, 0, 16);
            device->SetIndices(ib);
            const float defaultColor[]={app->ModelOutlineColorRed()*(1.f/256.f),
                app->ModelOutlineColorGreen()*(1.f/256.f),app->ModelOutlineColorBlue()*(1.f/256.f),1};
            const float* color=effectEdge?(pmx && sub->postProcessEnabled?record.edgeColor:defaultColor):nullptr;
            if(projectedShadow || !antialias::DrawOutline(app,model,firstIndex,count,color))
                device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                    state.vertexCount, firstIndex, count / 3);
        }
        firstIndex += count;
    }
    state.toonShared = static_cast<std::uint32_t>(-1);
}

void DrawModelsProjectedShadow(MMDApp* app) {
    // x64 inlined twin sub_7FF7CB4BFB20+0x4C0880..0x4C0916: both walks run
    // to 0xFF (cmp edx,0FFh / cmp edi,0FFh) - kModelSlotCount wide.
    const auto orderedModels = BuildModelDrawOrder(app,
        [](unsigned char* object) { return native_mesh::RenderModel(object); });
    for (const auto& entry : orderedModels) {
        if (!entry.model) continue;
        app->ActiveRenderObject() = entry.model;
        DrawModelEdgeGeometry(app, entry.model, true, false);
        app->ActiveRenderObject() = nullptr;
    }
}

bool BeginProjectedGroundShadow(MMDApp* app, IDirect3DDevice9* device,
                                Matrix* base) {
    const bool cameraGate = app->PlaybackActive() != 0 ||
        app->UsesViewportTool();
    const float* light = app->LightDirection();
    if (app->GroundShadowEnabled() == 0 || !cameraGate ||
        !std::isfinite(light[0]) || !std::isfinite(light[1]) ||
        !std::isfinite(light[2]) || light[1] >= 0.0f)
        return false;
    Matrix projection{};
    Matrix projected;
    device->GetTransform(D3DTS_WORLD, reinterpret_cast<D3DMATRIX*>(base));
    Matrix view{};
    Matrix inverseView{};
    device->GetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(&view));
    const bool haveInverseView = d3dx::Get().Load() &&
        d3dx::Get().inverse(&inverseView, nullptr, &view) != nullptr;
    const float cameraY = haveInverseView ? inverseView.m[3][1] : 1.0f;
    // Keep the projected surface just in front of the ground plane from the
    // current camera side. A fixed +Y bias puts the shadow behind the plane
    // whenever the user orbits below it, where the strict LESS test drops it.
    projection.m[3][1] = cameraY < 0.0f ? -0.1f : 0.1f;
    projection.m[0][0] = 1.0f;
    constexpr float kMaxShadowSlope = 32.0f;
    const float safeLightY = light[1] < -0.001f ? light[1] : -0.001f;
    projection.m[1][0] = std::clamp(-light[0] / safeLightY,
                                     -kMaxShadowSlope, kMaxShadowSlope);
    projection.m[1][2] = std::clamp(-light[2] / safeLightY,
                                     -kMaxShadowSlope, kMaxShadowSlope);
    projection.m[2][2] = 1.0f;
    projection.m[3][3] = 1.0f;
    Multiply(&projected, &projection, base);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&projected));
    // CULL/ZFUNC/texture-clear already issued by the caller
    // (0x426384..0x4263BC); the original does not repeat them here.
    app->ActiveRenderPass() = AccessoryRenderPass::ProjectedGroundShadow;
    return true;
}

void RenderProjectedGroundShadowPass(MMDApp* app,
                                    IDirect3DDevice9* device) {
    MMD_PERF_SCOPE(GroundShadow);
    DWORD oldBlend = FALSE;
    DWORD oldSourceBlend = D3DBLEND_ONE;
    DWORD oldDestinationBlend = D3DBLEND_ZERO;
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldBlend);
    device->GetRenderState(D3DRS_SRCBLEND, &oldSourceBlend);
    device->GetRenderState(D3DRS_DESTBLEND, &oldDestinationBlend);
    Matrix base;
    if (!BeginProjectedGroundShadow(app, device, &base))
        return;
    RenderAccessoriesProjectedGroundShadowGeometry(app);
    DrawModelsProjectedShadow(app);
    // The transparent-ground-shadow switch is scoped to the projected shadow
    // itself. Leaving alpha blending enabled here causes the following model
    // material passes to blend with the framebuffer as well.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, oldBlend);
    device->SetRenderState(D3DRS_SRCBLEND, oldSourceBlend);
    device->SetRenderState(D3DRS_DESTBLEND, oldDestinationBlend);
    device->SetTransform(D3DTS_WORLD,
                         reinterpret_cast<const D3DMATRIX*>(&base));
}

void SetProjectedStencil(D3DRenderer* sub, IDirect3DDevice9* device,
                         bool projectedPass) {
    if (sub->d3dInitialized == 0)
        return;
    device->SetRenderState(D3DRS_STENCILFUNC,
        projectedPass ? D3DCMP_GREATER : D3DCMP_ALWAYS);
    device->SetRenderState(D3DRS_STENCILREF, projectedPass ? 2 : 1);
    device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
}

void BeginProjectedShadowPass(D3DRenderer* sub, IDirect3DDevice9* device) {
    // Original frame trace: NORMALIZENORMALS is cleared by the preceding
    // accessory pass, then the projected geometry uses stencil value 2,
    // no culling, and strict depth comparison.  x64 pass 起始段
    // （0x7FF7CB4C0319..0x7FF7CB4C0402）不写 ALPHABLENDENABLE：混合态由配件
    // 投射循环逐配件重写，循环收尾再按透明地面影开关统一重写。
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    SetProjectedStencil(sub, device, true);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    device->SetTexture(0, nullptr);
}

void RestoreModelMaterialPass(MMDApp* app, D3DRenderer* sub,
                              IDirect3DDevice9* device) {
    // Fixed-frame restore, x64 sub_7FF7CB4BFB20 @0x7FF7CB4C07B0..0x7FF7CB4C0875
    // (FILLMODE -> stencil trio -> CULLMODE -> ZFUNC; port collapses the
    // FILLMODE/stencil order).  The effect frame no longer routes through
    // here: sub_7FF7CB4C1E60 defers CULLMODE/ZFUNC past the FxSet block and
    // adds ALPHABLENDENABLE, so RenderModelsEffect spells its restore inline.
    SetProjectedStencil(sub, device, false);
    device->SetRenderState(D3DRS_FILLMODE,
        app->WireframeRenderingEnabled() == 0 ? D3DFILL_SOLID
                                               : D3DFILL_WIREFRAME);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
}

void DrawModelOutlines(MMDApp* app, bool effectEdge) {
    MMD_PERF_SCOPE(Edges);
    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    void* effect = sub->effect;
    UINT passes = 0;
    if (effectEdge) {
        if (effect == nullptr)
            return;
        FxSetTechnique(effect, "ColorRenderTec");
        FxBegin(effect, &passes);
    }
    // x64 inlines this walk into both callers with 0xFF bounds: fixed pass
    // sub_7FF7CB4BFB20+0x4C09E0 (cmp edi,0FFh @0x4C0A74 / cmp r8d,0FFh
    // @0x4C0A0D), effect pass sub_7FF7CB4C1E60+0x4C37A0 (@0x4C38FE /
    // @0x4C37CE).  The outline-suppression byte (app+0xA1105) is checked
    // only by the effect frame's copy (0x7FF7CB4C3815, after the
    // edgeScale/postLoad/camera gates); the fixed frame's copy has no
    // reference to it anywhere in sub_7FF7CB4BFB20.
    const bool suppressed = effectEdge &&
        app->ModelNonDisplayMode() != 0;
    const auto orderedModels = BuildModelDrawOrder(app,
        [](unsigned char* object) { return native_mesh::RenderModel(object); });
    for (const auto& entry : orderedModels) {
        auto* model = entry.model;
        if (model == nullptr)
            continue;
        const mdl::ModelRecord& state = *mdl::Mdl(model);
        if (state.edgeScale <= 0.0f || state.postLoadFlag2 != 0)
            continue;
        const bool cameraGate = app->PlaybackActive() != 0 ||
            app->UsesViewportTool();
        if (!cameraGate)
            continue;
        if (suppressed)
            continue;
        app->ActiveRenderObject() = model;
        if (effectEdge) {
            const float edge[4] = {
                app->ModelOutlineColorRed() * (1.0f / 256.0f),
                app->ModelOutlineColorGreen() * (1.0f / 256.0f),
                app->ModelOutlineColorBlue() * (1.0f / 256.0f), 1.0f};
            FxSetFloatArray(effect, "EgColor", edge, 4);
            FxBeginPass(effect, 0);
        }
        DrawModelEdgeGeometry(app, model, false, effectEdge);
        if (effectEdge)
            FxEndPass(effect);
        app->ActiveRenderObject() = nullptr;
    }
    if (effectEdge) {
        FxEnd(effect);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
    }
    antialias::FinishOutlines(app);
}

void DrawGroundGeometry(MMDApp* app, IDirect3DDevice9* device) {
    if (app->GroundGridEnabled() == 0)
        return;
    auto* vb = app->GroundGridVertices();
    auto* ib = app->GroundGridIndices();
    if (vb != nullptr && ib != nullptr) {
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetTexture(0, nullptr);
        device->SetFVF(66);
        device->SetStreamSource(0, vb, 0, 16);
        device->SetIndices(ib);
        device->DrawIndexedPrimitive(D3DPT_LINELIST, 0, 0, 90, 0, 45);
    }

    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    D3DRenderer* sub = app->Renderer();
    const bool stencil = sub != nullptr && sub->d3dInitialized != 0;
    if (stencil)
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    auto* plane = app->GroundPlaneVertices();
    if (plane != nullptr) {
        device->SetStreamSource(0, plane, 0, 16);
        device->SetFVF(66);
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    if (stencil)
        device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
}

void DrawPreModelQuad(IDirect3DDevice9* device,
                      IDirect3DTexture9* texture,
                      IDirect3DVertexBuffer9* vertices) {
    if (texture == nullptr || vertices == nullptr)
        return;
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetTexture(0, texture);
    device->SetStreamSource(0, vertices, 0, 28);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    // Both background buffers contain six vertices (two independent triangles).
    device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
}

void DrawPreModelQuads(MMDApp* app, IDirect3DDevice9* device) {
    if (TransparentExportActive(app)) return;
    if (app->PictureBackgroundEnabled() != 0) {
        DrawPreModelQuad(device,
            app->PictureBackgroundTexture(),
            app->PictureOverlayVertices());
    }
    if (app->AviBackgroundEnabled() == 1) {
        DrawPreModelQuad(device,
            app->AviBackgroundTexture(),
            app->AviOverlayVertices());
    }
}

bool EffectRenderEnabled(const MMDApp* app) {
    const bool cameraGate = app->PlaybackActive() != 0 ||
        app->UsesViewportTool();
    return cameraGate && app->state.selfShadowMode > 0 &&    // 0xA0D30
           app->state.selfShadowEnabled != 0;            // 0xA0188
}


void RestoreTextureStages(IDirect3DDevice9* device) {
    for (DWORD stage = 0; stage < 3; ++stage)
        device->SetTexture(stage, nullptr);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                 D3DTTFF_DISABLE);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
}

}  // namespace

bool UseEffectModelRenderer(MMDApp* app) {
    return EffectRenderEnabled(app);
}

void RenderModelsFixed(MMDApp* app) {                         // 0x425D20
    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    // x64 帧头 0x7FF7CB4BFC08 仅一条 SetTextureStageState(2, TEXCOORDINDEX,
    // D3DTSS_TCI_CAMERASPACENORMAL)：不解绑纹理 2、不写 COLOROP/TTF。完整的
    // 球面贴图级联关闭保留在 ConfigureMaterialStages 的逐材质循环里
    // （0x491470.. 的逐材质行为）。
    device->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX,
                                 D3DTSS_TCI_CAMERASPACENORMAL);
    if (sub->d3dInitialized != 0) {
        device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
        device->SetRenderState(D3DRS_STENCILREF, 1);
        device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
    }
    DrawPreModelQuads(app, device);
    DrawGroundGeometry(app, device);
    // FILLMODE 排在背景图/AVI 四边形与地面之后：x64 sub_7FF7CB4BFB20 先画
    // 背景四边形（0x7FF7CB4BFC74 图片 / 0x7FF7CB4BFD4A AVI）、地面
    // （0x7FF7CB4BFE20），然后才在 0x7FF7CB4BFE88..0x7FF7CB4BFEA8 按线框开关
    // 写 RS(8) 并紧跟 RS(137)=1——线框模式下背景与地面仍实体填充。
    device->SetRenderState(D3DRS_FILLMODE,
        app->WireframeRenderingEnabled() == 0 ? D3DFILL_SOLID : D3DFILL_WIREFRAME);
    device->SetRenderState(D3DRS_LIGHTING, TRUE);

    const int accessorySplit = std::max(0, std::min(
        app->AccessoryRenderSplitOrder(), 255));
    RenderAccessoriesFixedRange(app, 0, accessorySplit);
    BeginProjectedShadowPass(sub, device);
    RenderProjectedGroundShadowPass(app, device);
    RestoreModelMaterialPass(app, sub, device);

    const bool materialCapture = BeginMaterialStateCapture();
    const auto orderedModels = BuildModelDrawOrder(
        app, [](unsigned char* object) { return native_mesh::RenderModel(object); });
    // x64 twin sub_7FF7CB4BFB20+0x4C0710..0x4C077A: order and slot both run
    // to 0xFF (cmp edi,0FFh @0x4C0774 / cmp edx,0FFh @0x4C073C).
    for (auto alphaPass : {ModelAlphaPass::Opaque, ModelAlphaPass::Translucent}) {
#if !MMD_ORIGINAL_PROFILE
        if (alphaPass == ModelAlphaPass::Translucent &&
            app->AutoTransparentMaterialSortingEnabled() &&
            (RenderSplitOrderedAlpha(app, false) || RenderNativeOrderedAlpha(app, false))) continue;
#endif
        for (int order = 0; order < kModelSlotCount; ++order) {
            const auto& entry = orderedModels[order];
            auto* model = entry.model;
            if (model != nullptr) {
                app->ActiveRenderObject() = model;
                app->ActiveRenderPass() = AccessoryRenderPass::FixedFunction;
                // With MME and sorting Off, submit one authored-order walk.
                // Splitting opaque/translucent materials would still reorder
                // MME draws even though the user explicitly disabled sorting.
                const bool originalMaterialOrder =
                    mdl::Mdl(model)->displayState != 0 ||
                    MMD_ORIGINAL_PROFILE ||
                    (!app->AutoTransparentMaterialSortingEnabled() &&
                     GetModuleHandleW(L"MMEffect.dll") != nullptr);
                if (!originalMaterialOrder || alphaPass == ModelAlphaPass::Opaque)
                    DrawModelMaterials(app, model, false, false,
                                       originalMaterialOrder
                                           ? ModelAlphaPass::Original
                                           : alphaPass);
                app->ActiveRenderObject() = nullptr;
            }
        }
    }
    if (materialCapture)
        EndMaterialStateCapture();

    // Edge preamble, x64 sub_7FF7CB4BFB20 @0x7FF7CB4C0930..0x7FF7CB4C09C8
    // (inside the wireframe gate): pass=4, SetTexture(0,null),
    // LIGHTING(137)=FALSE, ALPHABLENDENABLE(27)=TRUE, CULLMODE(22)=CW(2),
    // ZFUNC(23)=LESS(2); then the model walk.
    if (app->WireframeRenderingEnabled() == 0) {
        app->ActiveRenderPass() = AccessoryRenderPass::ModelOutline;
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        DrawModelOutlines(app, false);
    }
    // Second accessory range preamble, x64 0x7FF7CB4C0A87..0x7FF7CB4C0B89 -
    // runs unconditionally (wireframe-on jumps straight here from the gate):
    // pass=1, CULLMODE=CCW(3), ZFUNC=LESSEQUAL(4), LIGHTING=TRUE,
    // stage-0 passthrough reset, wrap samplers, then ALPHABLENDENABLE=TRUE.
    app->ActiveRenderPass() = AccessoryRenderPass::FixedFunction;
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    RenderAccessoriesFixedRange(app, accessorySplit, 255);
    // Frame tail, x64 sub_7FF7CB4BFB20 @0x7FF7CB4C0FC3/0x7FF7CB4C0FE3:
    // DESTBLEND(20)=INVSRCALPHA then FILLMODE(8)=SOLID.  No ZENABLE write
    // here in the original.
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    app->ActiveRenderPass() = AccessoryRenderPass::None;
}

void RenderModelsEffect(MMDApp* app, const float frameMatrix[16]) { // 0x4277E0
    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    void* effect = sub->effect;
    if (effect == nullptr) {
        RenderModelsFixed(app);
        return;
    }

    Matrix view;
    Matrix projection;
    Matrix worldView;
    Matrix wvp;
    const Matrix frame = *reinterpret_cast<const Matrix*>(frameMatrix);
    device->GetTransform(D3DTS_VIEW,
                         reinterpret_cast<D3DMATRIX*>(&view));
    device->GetTransform(D3DTS_PROJECTION,
                         reinterpret_cast<D3DMATRIX*>(&projection));
    // 0x4275F9..0x42764B prepares app+A0228 as
    // (frame world * D3DTS_VIEW) * D3DTS_PROJECTION.  The previous port
    // skipped VIEW, so the effect vertex shader emitted clip-space geometry
    // as giant black/cyan triangles while fixed-function rendering remained
    // correct.
    Multiply(&worldView, &frame, &view);
    Multiply(&wvp, &worldView, &projection);
    std::memcpy(&app->WorldViewProjection(), &wvp, sizeof(wvp));
    // x64 twin sub_7FF7CB4C1E60 submits the background before any frame
    // render state: picture quad 0x7FF7CB4C1F36..0x7FF7CB4C1F61 (gate
    // app+0x9F310, ZENABLE(7) off/on around the quad), AVI quad
    // 0x7FF7CB4C200C..0x7FF7CB4C20DC (gate app+0x13EC == 1), then the ground
    // 0x7FF7CB4C20E2..0x7FF7CB4C2133 (gate app+0x355).  Only afterwards come
    // the LightDir/Place/matrix FxSets (0x7FF7CB4C2193..0x7FF7CB4C22FD) and
    // the ALPHABLENDENABLE/FILLMODE/LIGHTING triple
    // (0x7FF7CB4C231D..0x7FF7CB4C236C) - the same quads -> ground -> RS(8)
    // shape the fixed frame gained in the FILLMODE round, so wireframe mode
    // never rasterises the background or the ground here either.  The head
    // also binds nothing to stage 0: the first stage-0 write after the
    // matrices is the projected-pass preamble's null (0x7FF7CB4C301B); every
    // accessory path rebinds stage 0 itself.
    DrawPreModelQuads(app, device);
    DrawGroundGeometry(app, device);

    float light[4] = {app->LightDirection()[0], app->LightDirection()[1],
                      app->LightDirection()[2], 1.0f};
    d3dx::Get().vec3Normalize(light, light);
    light[3] = 1.0f;
    Matrix inverse;
    float target[3] = {app->ViewOffsetX(), app->ViewOffsetY(),
                       app->CameraDistance()};
    float place[4] = {};
    d3dx::Get().inverse(&inverse, nullptr, &frame);
    d3dx::Get().vec3Transform(place, target, &inverse);
    FxSetFloatArray(effect, "LightDir", light, 4);
    FxSetFloatArray(effect, "Place", place, 4);
#if !MMD_ORIGINAL_PROFILE
    // Spline reconstruction needs the actual allocation, including adaptive
    // resizing. The default branch retains the native linear-depth ramp.
    shadow_filter::ConfigureEffect(app, effect);
#endif
    FxSetMatrix(effect, "matWorldViewProj",
                reinterpret_cast<const Matrix*>(&app->WorldViewProjection()));
    FxSetMatrix(effect, "matLightViewProj",
                reinterpret_cast<const Matrix*>(&app->LightViewProjection()));
    FxSetMatrix(effect, "matRotate",
                reinterpret_cast<const Matrix*>(&app->ViewRotationTransform()));
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_FILLMODE,
        app->WireframeRenderingEnabled() == 0
            ? D3DFILL_SOLID : D3DFILL_WIREFRAME);
    device->SetRenderState(D3DRS_LIGHTING, TRUE);

    const int accessorySplit = std::max(0, std::min(
        app->AccessoryRenderSplitOrder(), 255));
    runtime_log::SetPhase("fx: accessories pre-model");
    RenderAccessoriesEffectRange(app, 0, accessorySplit);
    BeginProjectedShadowPass(sub, device);
    RenderProjectedGroundShadowPass(app, device);
    // Post-projected restore + model preamble, x64 0x7FF7CB4C33B1..0x7FF7CB4C3566
    // (unconditional - the ground-shadow gate re-enters at 0x7FF7CB4C33B1):
    // stencil trio -> FILLMODE -> TSS(1, TEXCOORDINDEX, 1) -> SetTexture(0,
    // hdr) -> matWorldViewProj/matLightViewProj re-set -> transp -> ZFUNC ->
    // CULLMODE -> ALPHABLENDENABLE, immediately before the walk.  The matrix
    // re-set is load-bearing: the accessory effect range leaves the last
    // accessory's matWorldViewProj/matLightViewProj in the effect, and the
    // model materials would sample those without it.  matRotate stays
    // clobbered - the original does not restore it here either.  transp is
    // first set at 0x7FF7CB4C34E9 (not at the frame head), so the leading
    // accessory range runs with the previous frame's trailing zero.
    SetProjectedStencil(sub, device, false);
    device->SetRenderState(D3DRS_FILLMODE,
        app->WireframeRenderingEnabled() == 0 ? D3DFILL_SOLID
                                              : D3DFILL_WIREFRAME);
    device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    device->SetTexture(0, shadow_filter::VisibleTexture(app));
    FxSetMatrix(effect, "matWorldViewProj",
                reinterpret_cast<const Matrix*>(&app->WorldViewProjection()));
    FxSetMatrix(effect, "matLightViewProj",
                reinterpret_cast<const Matrix*>(&app->LightViewProjection()));
    FxSetInt(effect, "transp", app->state.characterTransparentMode != 0);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

    // x64 twin sub_7FF7CB4C1E60+0x4C3570..0x4C36D0 (toonFlag dispatch at
    // model+0x3B68): order and slot both run to 0xFF (cmp edi,0FFh
    // @0x4C36CA / cmp edx,0FFh @0x4C359C).
    runtime_log::SetPhase("fx: model draw walk");
    const auto orderedModels = BuildModelDrawOrder(
        app, [](unsigned char* object) { return native_mesh::RenderModel(object); });
    for (auto alphaPass : {ModelAlphaPass::Opaque, ModelAlphaPass::Translucent}) {
#if !MMD_ORIGINAL_PROFILE
        if (alphaPass == ModelAlphaPass::Translucent &&
            app->AutoTransparentMaterialSortingEnabled() &&
            (RenderSplitOrderedAlpha(app, true) || RenderNativeOrderedAlpha(app, true))) continue;
#endif
        for (int order = 0; order < kModelSlotCount; ++order) {
            const auto& entry = orderedModels[order];
            auto* model = entry.model;
            const int slot = entry.slot;
            if (model != nullptr) {
                if (app->ModelNonDisplayMode() != 0)
                    continue;
                app->ActiveRenderObject() = model;
                ValidateModelResources(model, slot);
                // Names the last object reached before a fault inside Direct3D.
                runtime_log::Write("FX model slot=%d order=%d toon=%u", slot,
                                   order,
                                   static_cast<unsigned>(mdl::Mdl(model)->toonFlag));
                // Match the fixed-function entry: MME with sorting Off keeps
                // authored material order and draws each material once.
                const bool originalMaterialOrder =
                    mdl::Mdl(model)->displayState != 0 ||
                    MMD_ORIGINAL_PROFILE ||
                    (!app->AutoTransparentMaterialSortingEnabled() &&
                     GetModuleHandleW(L"MMEffect.dll") != nullptr);
                const auto drawPass = originalMaterialOrder
                    ? ModelAlphaPass::Original
                    : alphaPass;
                const bool drawThisPass = !originalMaterialOrder ||
                    alphaPass == ModelAlphaPass::Opaque;
                if (drawThisPass && mdl::Mdl(model)->toonFlag != 0) {
                    app->ActiveRenderPass() = AccessoryRenderPass::Effect;
                    device->SetTexture(0, shadow_filter::VisibleTexture(app));
                    DrawModelMaterials(app, model, true, false, drawPass);
                } else if (drawThisPass) {
                    app->ActiveRenderPass() = AccessoryRenderPass::FixedFunction;
                    DrawModelMaterials(app, model, false, false, drawPass);
                }
                app->ActiveRenderObject() = nullptr;
            }
        }
    }
    FxSetInt(effect, "transp", 0);
    // x64 sub_7FF7CB4C1E60 @0x7FF7CB4C3711: ALPHABLENDENABLE=TRUE is set
    // before the wireframe gate, so blending stays on when the edge pass
    // is skipped.  The gate jumps straight to the restore sequence
    // (@0x7FF7CB4C395D); this preamble has no ZFUNC write - the edge
    // geometry keeps the LESSEQUAL comparison restored earlier, and the
    // edge draw itself (sub_7FF7CB4D82A0) sets no comparison state.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    if (app->WireframeRenderingEnabled() == 0) {
        app->ActiveRenderPass() = AccessoryRenderPass::ModelOutline;
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        DrawModelOutlines(app, true);
    }
    // Restore sequence, x64 0x7FF7CB4C395D..0x7FF7CB4C3A3C (unconditional;
    // the wireframe gate enters here directly): CULLMODE=CCW(3),
    // ZFUNC=LESSEQUAL(4), LIGHTING=TRUE, then the same stage-0 passthrough
    // reset and wrap samplers as the fixed frame.  No trailing
    // ALPHABLENDENABLE here - it was already set before the gate.
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_LIGHTING, TRUE);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    runtime_log::SetPhase("fx: accessories post-model");
    // Per-call records for the effect tail: a fault inside d3d9 after the
    // accessory range names the exact device call that tripped it.
    runtime_log::Write("fx tail: before RenderAccessoriesEffectRange");
    RenderAccessoriesEffectRange(app, accessorySplit, 255);
    runtime_log::Write("fx tail: before SetVertexShader");
    device->SetVertexShader(nullptr);
    runtime_log::Write("fx tail: before SetPixelShader");
    device->SetPixelShader(nullptr);
    runtime_log::Write("fx tail: before SetTexture0");
    device->SetTexture(0, nullptr);
    runtime_log::Write("fx tail: before RestoreTextureStages");
    RestoreTextureStages(device);
    runtime_log::Write("fx tail: after RestoreTextureStages");
    // Frame tail, x64 sub_7FF7CB4C1E60 @0x7FF7CB4C461F/0x7FF7CB4C463D (after
    // the accessory-light restore SetLight at 0x7FF7CB4C4601):
    // DESTBLEND(20)=INVSRCALPHA then FILLMODE(8)=SOLID - the same closing
    // pair as the fixed frame (0x7FF7CB4C0FC3/0x7FF7CB4C0FE3).
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    app->ActiveRenderPass() = AccessoryRenderPass::None;
}

static bool RenderShadowMapImpl(MMDApp* app, const float frameMatrix[16], bool checked) {   // 0x426CD0
    shadow_filter::Invalidate(app);
    MMD_PERF_SCOPE(ShadowMap);
    D3DRenderer* sub = app->Renderer();
    auto* device = sub->device;
    void* effect = sub->effect;
    auto& api = d3dx::Get();
    if (device == nullptr || effect == nullptr || !api.Load())
        return false;

#if !MMD_ORIGINAL_PROFILE
    const auto lightProjection = UpdateAdaptiveShadowMap(app, frameMatrix);
#endif
    auto*& texture = sub->hdrTexture;
    auto*& surface = sub->shadowSurface;
    auto*& depth = sub->shadowDepthSurface;
    const UINT width = static_cast<UINT>(sub->renderTargetWidth);
    const UINT height = static_cast<UINT>(sub->renderTargetHeight);
    if (texture == nullptr &&
        SUCCEEDED(api.createTexture(device, width, height, 1,
            D3DUSAGE_RENDERTARGET, static_cast<D3DFORMAT>(114),
            D3DPOOL_DEFAULT, &texture)))
        texture->GetSurfaceLevel(0, &surface);
    if (depth == nullptr)
        device->CreateDepthStencilSurface(width, height,
            static_cast<D3DFORMAT>(77), D3DMULTISAMPLE_NONE, 0, FALSE,
            &depth, nullptr);
    if (sub->backbufferSurface == nullptr) {
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
            &sub->backbufferSurface);
        device->GetDepthStencilSurface(&sub->depthStencilSurface);
    }
    if (surface == nullptr || depth == nullptr)
        return false;

    D3DVIEWPORT9 oldViewport;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> sceneTarget, sceneDepth;
    if (FAILED(device->GetRenderTarget(0, &sceneTarget))) return false;
    device->GetDepthStencilSurface(&sceneDepth);
    Matrix view;
    Matrix projection;
    Matrix worldView;
    device->GetTransform(D3DTS_VIEW,
                         reinterpret_cast<D3DMATRIX*>(&view));
    device->GetViewport(&oldViewport);
    device->GetTransform(D3DTS_PROJECTION,
                         reinterpret_cast<D3DMATRIX*>(&projection));
    const HRESULT technique=FxSetTechnique(effect, "ZValuePlotTec");
    if(checked&&FAILED(technique))return false;
    device->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    device->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, 2);
    const HRESULT targetSet=device->SetRenderTarget(0, surface);
    const HRESULT depthSet=device->SetDepthStencilSurface(depth);
    if(checked&&(FAILED(targetSet)||FAILED(depthSet)))return false;
    const HRESULT cleared=device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                  0xFFFFFFFFu, 1.0f, 0);
    if(checked&&FAILED(cleared))return false;
    device->SetTexture(0, nullptr);
    D3DVIEWPORT9 shadowViewport{0, 0, width, height, 0.0f, 1.0f};
    const HRESULT viewportSet=device->SetViewport(&shadowViewport);
    if(checked&&FAILED(viewportSet))return false;

#if MMD_ORIGINAL_PROFILE
    // MMD v9.32 projection, retained from the pre-refactor renderer.
    // Its mode-dependent warp is paired with the original effect resources.
    float target[3] = {app->ViewOffsetX(), app->ViewOffsetY(),
                       app->CameraDistance()};
    const D3DVECTOR& lightDirection = app->SceneLight().Direction;
    float eye[3] = {
        target[0] - lightDirection.x * 50.0f,
        target[1] - lightDirection.y * 50.0f,
        target[2] - lightDirection.z * 50.0f};
    const float up[3] = {0.0f, 0.0f, 1.0f};
    Matrix lightView;
    api.lookAtLH(&lightView, eye, target, up);

    float direction[3] = {lightDirection.x, lightDirection.y,
                          lightDirection.z};
    api.vec3Normalize(direction, direction);
    const float slope = -direction[2];
    const float edge = 1.0f - std::fabs(slope);
    const float base = app->ShadowDistance();
    const bool modeOne = app->ShadowMode() == 1;
    float scale;
    int part;
    if (modeOne) {
        part = 0;
        if (slope < -0.9f || slope > 0.9f)
            scale = base;
        else if (slope < -0.8f)
            scale = ((slope - 0.1f) * 10.0f + 10.0f) * base + base;
        else if (slope > 0.8f)
            scale = (10.0f - (slope + 0.1f) * 10.0f) * base + base;
        else
            scale = base + base;
    } else {
        part = 1;
        if (slope < -0.9f || slope > 0.9f)
            scale = base;
        else if (slope < -0.8f)
            scale = (slope * 20.0f + 19.0f) * base;
        else if (slope > 0.8f)
            scale = (19.0f - slope * 20.0f) * base;
        else
            scale = base * 3.0f;
    }
    FxSetInt(effect, "parthf", part);

    Matrix shadowProjection{};
    shadowProjection.m[0][0] = scale;
    shadowProjection.m[1][1] = scale;
    shadowProjection.m[2][2] = base * 0.1500000059604645f;
    shadowProjection.m[3][3] = 1.0f;
    Matrix shadowMatrix;
    Matrix temp;
    Multiply(&temp, reinterpret_cast<const Matrix*>(frameMatrix), &lightView);
    Multiply(&shadowMatrix, &temp, &shadowProjection);
    Matrix transform;
    api.scaling(&transform, 1.0f, 1.0f, 2.0f);
    Multiply(&shadowMatrix, &shadowMatrix, &transform);
    api.translation(&transform, 0.0f, 0.0f, -1.0f);
    Multiply(&shadowMatrix, &shadowMatrix, &transform);

    Matrix orientation;
    Identity(&orientation);
    float positive;
    float negative;
    if (slope < -0.9f || slope > 0.9f) {
        positive = edge;
        negative = -edge;
    } else if (slope < -0.8f) {
        positive = modeOne ? 4.0f * slope + 3.7f
                           : 9.0f * slope + 8.2f;
        negative = -9.0f * slope - 8.2f;
    } else if (slope > 0.8f) {
        positive = modeOne ? 3.7f - 4.0f * slope
                           : 8.2f - 9.0f * slope;
        negative = 9.0f * slope - 8.2f;
    } else {
        positive = modeOne ? 0.5f : 1.0f;
        negative = -1.0f;
    }
    orientation.m[1][3] = positive;
    orientation.m[3][1] = negative;
    Multiply(&shadowMatrix, &shadowMatrix, &orientation);
    api.translation(&transform, 0.0f, 0.0f, 1.0f);
    Multiply(&shadowMatrix, &shadowMatrix, &transform);
    api.scaling(&transform, 1.0f, 1.0f, 0.5f);
    Multiply(&shadowMatrix, &shadowMatrix, &transform);
    std::memcpy(&app->LightViewProjection(), &shadowMatrix,
                sizeof(shadowMatrix));

#else
    Matrix shadowMatrix;
    std::memcpy(&shadowMatrix, lightProjection.data(), sizeof(shadowMatrix));
    FxSetInt(effect, "parthf", app->ShadowMode() == 1 ? 0 : 1);
    std::memcpy(&app->LightViewProjection(), &shadowMatrix,
                sizeof(shadowMatrix));

#endif

    UINT passes = 0;
    app->ActiveRenderPass() = AccessoryRenderPass::ModelEffect;
    bool succeeded=SUCCEEDED(FxBegin(effect, &passes));
    if (succeeded) {
        // x64 sub_7FF7CB4C1030 order inside the Begin/End block: the
        // accessory shadow casters come first (0x7FF7CB4C1992..0x7FF7CB4C1BD4,
        // sub_7FF7CB4FDCB0 per slot), then matLightViewProj is set
        // (0x7FF7CB4C1BF9), then matWorldViewProj is computed from
        // frame*view*proj and set (0x7FF7CB4C1C0E..0x7FF7CB4C1C86) between
        // the two loops, and only then does the model slot walk run
        // (0x7FF7CB4C1CB5..0x7FF7CB4C1CF4, sub_7FF7CB4D8520; 255-count
        // do/while over app+0xBE8, mov r13d,0FFh @0x4C198C / dec r13/jnz,
        // toonFlag read at model+0x3B68).
        if(checked) {
            if(!RenderAccessoriesShadowChecked(app))succeeded=false;
        } else RenderAccessoriesShadow(app);
        const HRESULT projectionSet=FxSetMatrix(effect, "matLightViewProj", &shadowMatrix);
        if(FAILED(projectionSet))succeeded=false;
        Matrix wvp;
        Multiply(&worldView, reinterpret_cast<const Matrix*>(frameMatrix), &view);
        Multiply(&wvp, &worldView, &projection);
        std::memcpy(&app->WorldViewProjection(), &wvp, sizeof(wvp));
        FxSetMatrix(effect, "matWorldViewProj", &wvp);
        for (int slot = 0; slot < kModelSlotCount; ++slot) {
            auto* model = native_mesh::RenderModel(app->ModelSlot(slot));
            if (model == nullptr || mdl::Mdl(model)->toonFlag == 0)
                continue;
            app->ActiveRenderObject() = model;
            DrawModelMaterials(app, model, true, true,ModelAlphaPass::Original,nullptr,checked?&succeeded:nullptr);
            app->ActiveRenderObject() = nullptr;
        }
        if(FAILED(FxEnd(effect)))succeeded=false;
    }
#if !MMD_ORIGINAL_PROFILE
    // Preserve the raw native map for fallback. The finer map uses the same
    // projection and casters; its filtered result is shared by all receivers.
    if (auto* fine = shadow_filter::Prepare(app, succeeded)) {
        shadow_filter::TargetScope scope;
        bool auxiliary = SUCCEEDED(scope.Begin(device, *fine));
        const auto objectBefore = app->ActiveRenderObject();
        const auto passBefore = app->ActiveRenderPass();
        UINT finePasses = 0;
        if (auxiliary) {
            auxiliary = SUCCEEDED(FxSetTechnique(effect, "ZValuePlotTec"));
            auxiliary = auxiliary && SUCCEEDED(FxBegin(effect, &finePasses));
            if (auxiliary) {
                app->ActiveRenderPass() = AccessoryRenderPass::ModelEffect;
                if (!RenderAccessoriesShadowChecked(app)) auxiliary = false;
                if (FAILED(FxSetMatrix(effect, "matLightViewProj", &shadowMatrix))) auxiliary = false;
                Matrix fineWvp;
                Multiply(&worldView, reinterpret_cast<const Matrix*>(frameMatrix), &view);
                Multiply(&fineWvp, &worldView, &projection);
                if (FAILED(FxSetMatrix(effect, "matWorldViewProj", &fineWvp))) auxiliary = false;
                for (int slot = 0; slot < kModelSlotCount; ++slot) {
                    auto* model = native_mesh::RenderModel(app->ModelSlot(slot));
                    if (!model || mdl::Mdl(model)->toonFlag == 0) continue;
                    app->ActiveRenderObject() = model;
                    DrawModelMaterials(app, model, true, true, ModelAlphaPass::Original, nullptr, &auxiliary);
                }
                if (FAILED(FxEnd(effect))) auxiliary = false;
            }
        }
        app->ActiveRenderObject() = objectBefore;
        app->ActiveRenderPass() = passBefore;
        if (FAILED(scope.Restore())) auxiliary = false;
        if (FAILED(FxSetMatrix(effect, "matLightViewProj", &shadowMatrix))) auxiliary = false;
        if (FAILED(FxSetMatrix(effect, "matWorldViewProj",
            reinterpret_cast<const Matrix*>(&app->WorldViewProjection())))) auxiliary = false;
        shadow_filter::MarkRendered(app, auxiliary && succeeded);
    }
    shadow_filter::FinalizeDepth(app, succeeded);
#endif
    app->ActiveRenderPass() = AccessoryRenderPass::None;
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    // The frame may belong to an alpha export, AA, or scaled preview target.
    // Returning to the cached window backbuffer loses that frame: PNG capture
    // then reads the untouched transparent target instead of the model image.
    device->SetDepthStencilSurface(nullptr);
    device->SetRenderTarget(0, sceneTarget.Get());
    device->SetDepthStencilSurface(sceneDepth.Get());
    device->SetViewport(&oldViewport);
    device->SetTransform(D3DTS_PROJECTION,
                         reinterpret_cast<const D3DMATRIX*>(&projection));
    // x64 sub_7FF7CB4C1030 tail @0x7FF7CB4C1E1D/0x7FF7CB4C1E43:
    // ALPHABLENDENABLE(27)=TRUE, then CULLMODE(22)=CCW(3).
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    return succeeded;
}

void RenderShadowMap(MMDApp* app,const float frameMatrix[16]) {
    (void)RenderShadowMapImpl(app,frameMatrix,false);
}
bool RenderShadowMapChecked(MMDApp* app,const float frameMatrix[16]) {
    // The checked entry is also safe on early resource/FX failure. The normal
    // native entry retains its historical state transitions; MMV additionally
    // needs the pre-shadow camera/device state for its own scene snapshot.
    if(!app||!app->Renderer()||!app->Renderer()->device||!app->Renderer()->effect)return false;
    using Microsoft::WRL::ComPtr;
    auto* device=app->Renderer()->device;void* effect=app->Renderer()->effect;
    ComPtr<IDirect3DStateBlock9> state;ComPtr<IDirect3DSurface9> targets[4],depth;
    D3DVIEWPORT9 viewport{};
    if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&state))||FAILED(device->GetRenderTarget(0,&targets[0]))||
       FAILED(device->GetViewport(&viewport)))return false;
    for(unsigned i=1;i<4;++i)device->GetRenderTarget(i,&targets[i]);
    device->GetDepthStencilSurface(&depth);
    using GetValue=HRESULT(WINAPI*)(void*,const char*,void*,UINT);
    using SetValue=HRESULT(WINAPI*)(void*,const char*,const void*,UINT);
    using GetTechnique=const char*(WINAPI*)(void*);
    const auto get=d3dx::EffectMethod<GetValue,21>(effect);
    const auto set=d3dx::EffectMethod<SetValue,20>(effect);
    const char* technique=d3dx::EffectMethod<GetTechnique,59>(effect)(effect);
    Matrix light{},wvp{};int part=0;
    if(!technique||FAILED(get(effect,"matLightViewProj",&light,sizeof(light)))||
       FAILED(get(effect,"matWorldViewProj",&wvp,sizeof(wvp)))||FAILED(get(effect,"parthf",&part,sizeof(part))))return false;
    const auto object=app->ActiveRenderObject();const auto pass=app->ActiveRenderPass();
    struct RestoreShadow {
        MMDApp* app;IDirect3DDevice9* device;IDirect3DStateBlock9* state;ComPtr<IDirect3DSurface9>* targets;
        IDirect3DSurface9* depth;D3DVIEWPORT9 viewport;void* effect;SetValue set;const char* technique;
        Matrix light,wvp;int part;void* object;AccessoryRenderPass pass;
        bool restored=false;
        bool Apply(){
            if(restored)return true;
            restored=true;bool ok=true;const auto check=[&](HRESULT result){if(FAILED(result))ok=false;};
            check(set(effect,"matLightViewProj",&light,sizeof(light)));check(set(effect,"matWorldViewProj",&wvp,sizeof(wvp)));
            check(set(effect,"parthf",&part,sizeof(part)));check(FxSetTechnique(effect,technique));
            device->SetDepthStencilSurface(nullptr);for(unsigned i=1;i<4;++i)device->SetRenderTarget(i,nullptr);
            check(device->SetRenderTarget(0,targets[0].Get()));
            for(unsigned i=1;i<4;++i)if(targets[i])check(device->SetRenderTarget(i,targets[i].Get()));
            check(device->SetDepthStencilSurface(depth));check(state->Apply());check(device->SetViewport(&viewport));
            app->ActiveRenderObject()=object;app->ActiveRenderPass()=pass;return ok;
        }
        ~RestoreShadow(){Apply();}
    } restore{app,device,state.Get(),targets,depth.Get(),viewport,effect,set,technique,light,wvp,part,object,pass};
    for(unsigned i=1;i<4;++i)device->SetRenderTarget(i,nullptr);
    const bool rendered=RenderShadowMapImpl(app,frameMatrix,true);
    return restore.Apply()&&rendered;
}

}  // namespace mikudancestudio
