// Owned binding geometry: native layouts, hand-written restore expectations,
// unsupported algorithms, failure isolation and allocation-free readers.
#define NOMINMAX
#include "mikumikustudio/mesh_definition_snapshot.hpp"
#include "mikumikustudio/model.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace { thread_local int failAfter = -1; std::atomic<long long> allocations{0}; }
void* operator new(std::size_t bytes) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (void* result = std::malloc(bytes ? bytes : 1)) { ++allocations; return result; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { if (p) { --allocations; std::free(p); } }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

using namespace mikumikustudio;
using Frozen = MeshDefinitionSnapshot;
namespace {
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
float Bits(std::uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
bool SameFloat(float a, float b) { return !std::memcmp(&a, &b, sizeof(float)); }
template<std::size_t N> bool Floats(const std::array<float, N>& a, const std::array<float, N>& b) {
    return !std::memcmp(a.data(), b.data(), sizeof(float) * N);
}
template<class E, class F> bool Throws(F&& function) {
    try { function(); } catch (const E&) { return true; } catch (...) {} return false;
}
template<class F> bool ThrowsMalformed(F&& function) {
    try { function(); }
    catch (const MeshDefinitionUnsupported&) { return false; }
    catch (const std::invalid_argument&) { return true; }
    catch (...) {}
    return false;
}
template<class T> T Poison() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> bool Unchanged(const T& value) {
    const auto before = Poison<T>(); return !std::memcmp(&before, &value, sizeof(T));
}
template<class T> bool SameVector(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(T)));
}

struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::PmxVertex> pmx = std::vector<mdl::PmxVertex>(4);
    std::vector<mdl::PmdVertex> pmd = std::vector<mdl::PmdVertex>(4);
    std::vector<std::int32_t> pmxIndices{0, 1, 2, 3, 1, 0};
    // Each odd word is unused host padding. A compact WORD reader would
    // consume 65535 as a vertex index and fail this otherwise valid fixture.
    std::vector<std::uint16_t> pmdIndices{0, 65535, 1, 61166, 2, 56797, 3, 52428, 1, 48059, 0, 43690};
    std::vector<mdl::ModelMaterialRecord> materials = std::vector<mdl::ModelMaterialRecord>(4);
    std::vector<mdl::PmdVertexMorphEntry> positions = std::vector<mdl::PmdVertexMorphEntry>(3);
    std::array<std::vector<mdl::PmxUvMorphEntry>, 5> uv;
    explicit Model(bool isPmx = true, unsigned additionalUv = 4) {
        record->physicsMode = isPmx ? 2 : 0;
        record->pmxAdditionalUvCount = static_cast<std::uint8_t>(additionalUv);
        record->vertexCount = 4; record->boneCount = 3;
        record->boneTable = reinterpret_cast<mdl::BoneRecord*>(uintptr_t(1)); // Never dereferenced.
        record->indexCount = 6; record->materialCount = 4;
        record->indices = isPmx ? static_cast<void*>(pmxIndices.data()) : static_cast<void*>(pmdIndices.data());
        record->materials = materials.data();
        record->pmxVertices = isPmx ? pmx.data() : reinterpret_cast<mdl::PmxVertex*>(uintptr_t(3));
        record->rawVertices = isPmx ? reinterpret_cast<mdl::PmdVertex*>(uintptr_t(5)) : pmd.data();
        record->vertexBuffer = reinterpret_cast<void*>(uintptr_t(7));
        record->vertexBuffer2 = reinterpret_cast<void*>(uintptr_t(9));
        record->indexBuffer = reinterpret_cast<void*>(uintptr_t(11));
        record->displayKeyCursor = 991; record->selectedBone = 2; record->searchCursor = 123;
        record->morphs = reinterpret_cast<mdl::MorphRecord*>(uintptr_t(13)); record->morphCount = 999;
        record->materialMorphPools.base = reinterpret_cast<mdl::MaterialMorphPool*>(uintptr_t(15));
        // Native rendering can assign a vertex's edge material independently
        // of the material of a particular triangle containing that vertex.
        materials[0].faceVertexCount = 0; materials[1].faceVertexCount = 3;
        materials[2].faceVertexCount = 0; materials[3].faceVertexCount = 3;
        for (auto& material : materials) {
            material.diffuse[0] = Bits(0x7fc01234); material.edgeSize = Bits(0x7fc05678);
            std::memset(material.texturePath, 0xff, sizeof(material.texturePath));
        }
        const std::int16_t pmdBones[4][2]{{-1, 0}, {INT16_MIN, INT16_MAX}, {1, 2}, {-2, 1}};
        const std::int8_t percentages[4]{0, 25, 100, -25};
        const std::uint8_t disabled[4]{0, 197, 255, 1};
        for (unsigned i = 0; i < 4; ++i) {
            // Unused PMX weights, SDEF fields and additional UVs contain NaNs.
            std::memset(&pmx[i], 0xff, sizeof(pmx[i]));
            for (unsigned axis = 0; axis < 3; ++axis) {
                pmx[i].position[axis] = pmd[i].position[axis] = float(100 * (axis + 1) + i);
                pmx[i].normal[axis] = pmd[i].normal[axis] = axis == 1 ? 1.0f : 0.0f;
            }
            pmx[i].uv[0] = pmd[i].uv[0] = float(i) + .25f;
            pmx[i].uv[1] = pmd[i].uv[1] = float(i) + .5f;
            for (unsigned group = 0; group < additionalUv; ++group)
                for (unsigned component = 0; component < 4; ++component)
                    pmx[i].additionalUvByComponent[component][group] = float(100 * group + 10 * i + component) + .125f;
            pmx[i].weightType = static_cast<mdl::PmxWeightType>(i);
            pmx[i].edgeScale = i == 2 ? Bits(0x80000000) : float(i) + .25f;
            pmx[i].hasPmxEdgeData = static_cast<std::uint8_t>(197 + i);
            pmx[i].materialIndex = i % 2 ? 1 : 3;
            pmd[i].bone[0] = pmdBones[i][0]; pmd[i].bone[1] = pmdBones[i][1];
            pmd[i].weightPercent = percentages[i]; pmd[i].edgeDisabled = disabled[i];
        }
        pmx[0].bone[0] = INT32_MIN;
        pmx[1].bone[0] = -1; pmx[1].bone[1] = INT32_MAX; pmx[1].weight[0] = 1.25f;
        const std::int32_t fourBones[4]{0, -2, 999, 1}; const float fourWeights[4]{.125f, -.25f, .5f, 1.5f};
        std::memcpy(pmx[2].bone, fourBones, sizeof(fourBones)); std::memcpy(pmx[2].weight, fourWeights, sizeof(fourWeights));
        pmx[3].bone[0] = 2; pmx[3].bone[1] = -2; pmx[3].weight[0] = .25f;
        const float center[3]{11, -2, .25f}, r0[3]{3, 6, -9}, r1[3]{Bits(0xbf800001), -2, 3};
        std::memcpy(pmx[3].sdef.center, center, sizeof(center));
        std::memcpy(pmx[3].sdef.r0Offset, r0, sizeof(r0)); std::memcpy(pmx[3].sdef.r1Offset, r1, sizeof(r1));
        // Preserve the finite low bits and negative zero. These are not
        // approximate comparisons or values regenerated by Capture.
        pmx[2].normal[0] = pmd[2].normal[0] = Bits(0x80000000);
        pmx[2].normal[1] = pmd[2].normal[1] = Bits(0x3f800001);
        pmx[2].normal[2] = pmd[2].normal[2] = 2;
        positions[0] = {1, {1, 2, 3}}; positions[1] = {3, {-2, .5f, Bits(0x80000000)}};
        positions[2] = {1, {4, 5, 6}};
        record->morph0Count = static_cast<uint32_t>(positions.size()); record->morph0Table = positions.data();
        for (unsigned family = 0; family < 5; ++family) {
            if (!isPmx) {
                // PMD must not inspect any PMX-only UV restore metadata.
                record->uvMorphCounts.byFamily[family] = INT32_MIN;
                record->uvMorphTables.byFamily[family] = reinterpret_cast<mdl::PmxUvMorphEntry*>(uintptr_t(17));
                continue;
            }
            if (family > additionalUv) continue;
            uv[family].resize(3);
            if (!family) {
                uv[0][0] = {1, {.5f, .25f, Bits(0x7fc12345), Bits(0x7f800001)}};
                uv[0][1] = {3, {.125f, .875f, Bits(0x7fc23456), Bits(0x7f800002)}};
                uv[0][2] = {1, {-.25f, .75f, Bits(0x7fc34567), Bits(0x7f800003)}};
            } else {
                const float f = static_cast<float>(family);
                uv[family][0] = {1, {f, f + 1, f + 2, f + 3}};
                uv[family][1] = {3, {.5f, Bits(0x80000000), .25f, .75f}};
                uv[family][2] = {1, {-f, -f - 1, -f - 2, -f - 3}};
            }
            record->uvMorphCounts.byFamily[family] = 3; record->uvMorphTables.byFamily[family] = uv[family].data();
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
};

struct SourceState {
    std::unique_ptr<mdl::ModelRecord> record;
    std::vector<mdl::PmxVertex> pmx;
    std::vector<mdl::PmdVertex> pmd;
    std::vector<std::int32_t> pmxIndices;
    std::vector<std::uint16_t> pmdIndices;
    std::vector<mdl::ModelMaterialRecord> materials;
    std::vector<mdl::PmdVertexMorphEntry> positions;
    std::array<std::vector<mdl::PmxUvMorphEntry>, 5> uv;
    explicit SourceState(const Model& model) : record(std::make_unique<mdl::ModelRecord>(*model.record)), pmx(model.pmx), pmd(model.pmd),
        pmxIndices(model.pmxIndices), pmdIndices(model.pmdIndices), materials(model.materials), positions(model.positions), uv(model.uv) {}
    void Check(const Model& model) const {
        CHECK(!std::memcmp(record.get(), model.record.get(), sizeof(*record)));
        CHECK(SameVector(pmx, model.pmx) && SameVector(pmd, model.pmd));
        CHECK(SameVector(pmxIndices, model.pmxIndices) && SameVector(pmdIndices, model.pmdIndices));
        CHECK(SameVector(materials, model.materials) && SameVector(positions, model.positions));
        for (unsigned family = 0; family < 5; ++family) CHECK(SameVector(uv[family], model.uv[family]));
    }
};

// Hand-written outcomes: restore vertex 1 twice -> (4,5,6), vertex 3 once;
// two faces belong to material 1 and 3 despite vertex 0 naming material 3.
bool VertexMatches(const MeshDefinitionVertex& v, unsigned index, bool pmx, unsigned uvCount) {
    const std::array<float, 3> positions[4]{{100, 200, 300}, {4, 5, 6}, {102, 202, 302}, {-2, .5f, Bits(0x80000000)}};
    const std::array<float, 3> normal = index == 2 ? std::array<float, 3>{Bits(0x80000000), Bits(0x3f800001), 2} : std::array<float, 3>{0, 1, 0};
    const std::array<float, 2> uv = pmx && index == 1 ? std::array<float, 2>{-.25f, .75f} :
        pmx && index == 3 ? std::array<float, 2>{.125f, .875f} : std::array<float, 2>{float(index) + .25f, float(index) + .5f};
    if (!Floats(v.position, positions[index]) || !Floats(v.normal, normal) || !Floats(v.uv, uv)) return false;
    for (unsigned group = 0; group < 4; ++group) {
        std::array<float, 4> expected{};
        if (pmx && group < uvCount) {
            const float family = float(group + 1);
            if (index == 1) expected = {-family, -family - 1, -family - 2, -family - 3};
            else if (index == 3) expected = {.5f, Bits(0x80000000), .25f, .75f};
            else expected = {float(100 * group + 10 * index) + .125f, float(100 * group + 10 * index + 1) + .125f,
                float(100 * group + 10 * index + 2) + .125f, float(100 * group + 10 * index + 3) + .125f};
        }
        if (!Floats(v.additionalUv[group], expected)) return false;
    }
    if (pmx) {
        const std::array<std::int32_t, 4> bones[4]{{INT32_MIN, -1, -1, -1}, {-1, INT32_MAX, -1, -1}, {0, -2, 999, 1}, {2, -2, -1, -1}};
        const std::array<float, 4> weights[4]{{1, 0, 0, 0}, {1.25f, -.25f, 0, 0}, {.125f, -.25f, .5f, 1.5f}, {.25f, .75f, 0, 0}};
        if (v.binding != static_cast<MeshBindingType>(index) || v.bones != bones[index] || !Floats(v.weights, weights[index]) ||
            v.pmdWeightPercent || v.pmdEdgeDisabled || v.pmxEdgeData != 197 + index || v.materialIndex != (index % 2 ? 1 : 3) ||
            !SameFloat(v.edgeScale, index == 2 ? Bits(0x80000000) : float(index) + .25f)) return false;
    } else {
        const std::array<std::int32_t, 4> bones[4]{{-1, 0, -1, -1}, {INT16_MIN, INT16_MAX, -1, -1}, {1, 2, -1, -1}, {-2, 1, -1, -1}};
        const std::array<float, 4> weights[4]{{0, 1, 0, 0}, {.25f, .75f, 0, 0}, {1, 0, 0, 0}, {-.25f, 1.25f, 0, 0}};
        const std::int8_t percentages[4]{0, 25, 100, -25}; const std::uint8_t edges[4]{0, 197, 255, 1};
        if (v.binding != MeshBindingType::Bdef2 || v.bones != bones[index] || !Floats(v.weights, weights[index]) ||
            v.pmdWeightPercent != percentages[index] || v.pmdEdgeDisabled != edges[index] || v.pmxEdgeData || v.materialIndex != -1 ||
            !SameFloat(v.edgeScale, index ? 0.0f : 1.0f)) return false;
    }
    const bool sdef = pmx && index == 3;
    return Floats(v.sdefCenter, sdef ? std::array<float, 3>{11, -2, .25f} : std::array<float, 3>{}) &&
        Floats(v.sdefR0Offset, sdef ? std::array<float, 3>{3, 6, -9} : std::array<float, 3>{}) &&
        Floats(v.sdefR1Offset, sdef ? std::array<float, 3>{Bits(0xbf800001), -2, 3} : std::array<float, 3>{});
}
bool Matches(const Frozen& frozen, bool pmx, unsigned uvCount, std::uint8_t mode = 0) {
    if (frozen.PhysicsMode() != (pmx ? 2 : mode) || frozen.AdditionalUvCount() != (pmx ? uvCount : 0) ||
        frozen.BoneCount() != 3 || frozen.VertexCount() != 4 || frozen.IndexCount() != 6 ||
        frozen.TriangleCount() != 2 || frozen.MaterialCount() != 4 || !frozen.RetainedBytes()) return false;
    for (unsigned index = 0; index < 4; ++index) { MeshDefinitionVertex vertex; if (!frozen.VertexAt(index, vertex) || !VertexMatches(vertex, index, pmx, uvCount)) return false; }
    const std::uint32_t indices[6]{0, 1, 2, 3, 1, 0};
    for (unsigned index = 0; index < 6; ++index) { std::uint32_t out = 99; if (!frozen.IndexAt(index, out) || out != indices[index]) return false; }
    for (unsigned index = 0; index < 2; ++index) {
        MeshDefinitionTriangle triangle; if (!frozen.TriangleAt(index, triangle) || triangle.materialIndex != (index ? 3 : 1)) return false;
        for (unsigned corner = 0; corner < 3; ++corner) if (triangle.vertices[corner] != indices[index * 3 + corner]) return false;
    }
    const std::uint32_t first[4]{0, 0, 3, 3}, counts[4]{0, 3, 0, 3};
    for (unsigned index = 0; index < 4; ++index) { MeshDefinitionMaterial material; if (!frozen.MaterialAt(index, material) || material.firstIndex != first[index] || material.indexCount != counts[index]) return false; }
    return true;
}

void FieldsAndRestore() {
    for (unsigned uvCount = 0; uvCount <= 4; ++uvCount) {
        Model source(true, uvCount); SourceState before(source); auto frozen = Frozen::Capture(source.raw());
        CHECK(Matches(*frozen, true, uvCount)); before.Check(source);
        // These live morph values must be replaced by valid restore entries,
        // rather than validated as if they were the immutable binding data.
        source.pmx[1].position[0] = Bits(0x7fc11223); source.pmx[1].uv[0] = Bits(0x7fc33445);
        if (uvCount) source.pmx[1].additionalUvByComponent[0][0] = Bits(0x7fc55667);
        SourceState dirty(source); auto restored = Frozen::Capture(source.raw());
        CHECK(Matches(*restored, true, uvCount)); dirty.Check(source);
    }
    for (std::uint8_t mode : {std::uint8_t(0), std::uint8_t(1), std::uint8_t(197)}) {
        Model source(false); source.record->physicsMode = mode; source.record->pmxAdditionalUvCount = 255;
        SourceState before(source); auto frozen = Frozen::Capture(source.raw()); CHECK(Matches(*frozen, false, 0, mode)); before.Check(source);
    }
    Model source; auto original = Frozen::Capture(source.raw());
    source.positions[2].offset[0] = 77; source.uv[0][2].offset[0] = -.625f;
    source.pmx[2].weight[0] = 2.125f; source.pmxIndices[0] = 2;
    auto revised = Frozen::Capture(source.raw()); CHECK(Matches(*original, true, 4));
    MeshDefinitionVertex vertex; std::uint32_t index = 99;
    CHECK(revised->VertexAt(1, vertex) && vertex.position[0] == 77 && vertex.uv[0] == -.625f);
    CHECK(revised->VertexAt(2, vertex) && vertex.weights[0] == 2.125f);
    CHECK(revised->IndexAt(0, index) && index == 2);
}

void InvalidReadsAndEmpty() {
    Model model; auto frozen = Frozen::Capture(model.raw());
    for (auto index : {4u, UINT32_MAX}) { auto out = Poison<MeshDefinitionVertex>(); CHECK(!frozen->VertexAt(index, out) && Unchanged(out)); }
    for (auto index : {6u, UINT32_MAX}) { std::uint32_t out = 0xa5a5a5a5; CHECK(!frozen->IndexAt(index, out) && out == 0xa5a5a5a5); }
    for (auto index : {2u, UINT32_MAX}) { auto out = Poison<MeshDefinitionTriangle>(); CHECK(!frozen->TriangleAt(index, out) && Unchanged(out)); }
    for (auto index : {4u, UINT32_MAX}) { auto out = Poison<MeshDefinitionMaterial>(); CHECK(!frozen->MaterialAt(index, out) && Unchanged(out)); }
    for (auto pmx : {false, true}) {
        auto empty = std::make_unique<mdl::ModelRecord>(); empty->physicsMode = pmx ? 2 : 0;
        empty->pmxVertices = reinterpret_cast<mdl::PmxVertex*>(uintptr_t(1));
        empty->rawVertices = reinterpret_cast<mdl::PmdVertex*>(uintptr_t(3));
        empty->indices = reinterpret_cast<void*>(uintptr_t(5)); empty->materials = reinterpret_cast<void*>(uintptr_t(7));
        empty->morph0Table = reinterpret_cast<mdl::PmdVertexMorphEntry*>(uintptr_t(9));
        for (auto& table : empty->uvMorphTables.byFamily) table = reinterpret_cast<mdl::PmxUvMorphEntry*>(uintptr_t(11));
        auto zero = Frozen::Capture(reinterpret_cast<unsigned char*>(empty.get()));
        CHECK(zero->VertexCount() == 0 && zero->IndexCount() == 0 && zero->TriangleCount() == 0 && zero->MaterialCount() == 0 && zero->BoneCount() == 0);
        auto vertex = Poison<MeshDefinitionVertex>(); auto triangle = Poison<MeshDefinitionTriangle>(); auto material = Poison<MeshDefinitionMaterial>(); std::uint32_t index = 719;
        CHECK(!zero->VertexAt(0, vertex) && Unchanged(vertex)); CHECK(!zero->TriangleAt(0, triangle) && Unchanged(triangle));
        CHECK(!zero->MaterialAt(0, material) && Unchanged(material)); CHECK(!zero->IndexAt(0, index) && index == 719);
    }
    CHECK(ThrowsMalformed([] { Frozen::Capture(nullptr); }));
}
template<class F> void Invalid(F&& edit, bool pmx = true) {
    Model source(pmx); edit(source); SourceState before(source);
    CHECK(ThrowsMalformed([&] { Frozen::Capture(source.raw()); })); before.Check(source);
}
void ValidationAndUnsupported() {
    for (unsigned type : {4u, 5u, 255u}) {
        Model source; source.pmx[0].weightType = static_cast<mdl::PmxWeightType>(type); SourceState before(source);
        CHECK(Throws<MeshDefinitionUnsupported>([&] { Frozen::Capture(source.raw()); })); before.Check(source);
    }
    Invalid([](auto& m) { m.record->vertexCount = UINT32_MAX; });
    Invalid([](auto& m) { m.record->indexCount = UINT32_MAX; });
    Invalid([](auto& m) { m.record->materialCount = UINT32_MAX; });
    Invalid([](auto& m) { m.record->boneCount = UINT32_MAX; });
    Invalid([](auto& m) { m.record->morph0Count = UINT32_MAX; });
    Invalid([](auto& m) { m.record->pmxAdditionalUvCount = 5; });
    Invalid([](auto& m) { m.record->indexCount = 5; });
    Invalid([](auto& m) { m.record->pmxVertices = nullptr; });
    Invalid([](auto& m) { m.record->rawVertices = nullptr; }, false);
    Invalid([](auto& m) { m.record->indices = nullptr; });
    Invalid([](auto& m) { m.record->materials = nullptr; });
    Invalid([](auto& m) { m.record->morph0Table = nullptr; });
    Invalid([](auto& m) { m.record->uvMorphCounts.byFamily[0] = -1; });
    Invalid([](auto& m) { m.record->uvMorphTables.byFamily[4] = nullptr; });
    Invalid([](auto& m) { m.pmxIndices[0] = -1; });
    Invalid([](auto& m) { m.pmxIndices[0] = 4; });
    Invalid([](auto& m) { m.pmdIndices[2] = 4; }, false);
    Invalid([](auto& m) { m.materials[1].faceVertexCount = -3; });
    Invalid([](auto& m) { m.materials[1].faceVertexCount = 2; });
    Invalid([](auto& m) { m.materials[3].faceVertexCount = 6; });
    Invalid([](auto& m) { m.materials[3].faceVertexCount = 0; });
    Invalid([](auto& m) { m.pmx[0].materialIndex = -1; });
    Invalid([](auto& m) { m.pmx[0].materialIndex = 4; });
    Invalid([](auto& m) { m.positions[0].vertexIndex = 4; });
    Invalid([](auto& m) { m.uv[0][0].vertexIndex = -1; });
    Invalid([](auto& m) { m.uv[4][0].vertexIndex = 4; });
    const float infinity = (std::numeric_limits<float>::infinity)();
    Invalid([=](auto& m) { m.pmx[0].position[0] = infinity; });
    Invalid([=](auto& m) { m.pmd[0].position[0] = infinity; }, false);
    Invalid([=](auto& m) { m.pmx[0].normal[1] = infinity; });
    Invalid([=](auto& m) { m.pmx[0].uv[0] = infinity; });
    Invalid([=](auto& m) { m.pmx[0].additionalUvByComponent[0][3] = infinity; });
    Invalid([=](auto& m) { m.pmx[1].weight[0] = infinity; });
    Invalid([=](auto& m) { m.pmx[2].weight[3] = infinity; });
    Invalid([=](auto& m) { m.pmx[3].sdef.center[0] = infinity; });
    Invalid([=](auto& m) { m.pmx[3].sdef.r0Offset[1] = infinity; });
    Invalid([=](auto& m) { m.pmx[3].sdef.r1Offset[2] = infinity; });
    Invalid([=](auto& m) { m.pmx[0].edgeScale = infinity; });
    Invalid([=](auto& m) { m.positions[0].offset[0] = infinity; });
    Invalid([=](auto& m) { m.uv[0][0].offset[1] = infinity; });
    Invalid([=](auto& m) { m.uv[4][0].offset[3] = infinity; });
    // The native missing-bone rule includes *all* signed out-of-range values,
    // even at nonzero weights and even when the model has no bone table.
    for (bool pmx : {false, true}) {
        Model source(pmx); source.record->boneCount = 0; source.record->boneTable = nullptr;
        SourceState before(source); auto frozen = Frozen::Capture(source.raw()); CHECK(frozen->BoneCount() == 0);
        for (unsigned i = 0; i < 4; ++i) { MeshDefinitionVertex out; CHECK(frozen->VertexAt(i, out) && VertexMatches(out, i, pmx, pmx ? 4 : 0)); }
        before.Check(source);
    }
}

void BudgetAndAllocationFailures() {
    for (bool pmx : {false, true}) {
        Model source(pmx); SourceState before(source); auto baseline = Frozen::Capture(source.raw());
        const auto bytes = baseline->RetainedBytes();
        CHECK(bytes >= 4 * sizeof(MeshDefinitionVertex) + 2 * sizeof(MeshDefinitionTriangle) + 4 * sizeof(MeshDefinitionMaterial));
        auto exact = Frozen::Capture(source.raw(), bytes); CHECK(exact->RetainedBytes() == bytes && Matches(*exact, pmx, pmx ? 4 : 0));
        CHECK(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), bytes - 1); }));
        CHECK(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), 0); })); before.Check(source);
        bool completed = false; unsigned failures = 0;
        const auto outstanding = allocations.load();
        for (int allocation = 0; allocation < 128; ++allocation) {
            std::unique_ptr<Frozen> candidate; bool rejected = false, unexpected = false;
            failAfter = allocation;
            try { candidate = Frozen::Capture(source.raw()); }
            catch (const std::bad_alloc&) { rejected = true; }
            catch (...) { unexpected = true; }
            failAfter = -1; CHECK(!unexpected); before.Check(source);
            if (candidate) {
                CHECK(Matches(*candidate, pmx, pmx ? 4 : 0) && candidate->RetainedBytes() == bytes);
                candidate.reset(); CHECK(allocations.load() == outstanding); completed = true; break;
            }
            CHECK(rejected && allocations.load() == outstanding); ++failures;
        }
        CHECK(completed && failures >= 5);
    }
    // Structural counts fit their native type but cannot fit the budget.
    // Poison pointers prove rejection precedes walking or copying the payload.
    for (unsigned field = 0; field < 3; ++field) {
        Model source;
        if (!field) { source.record->vertexCount = INT32_MAX; source.record->pmxVertices = reinterpret_cast<mdl::PmxVertex*>(uintptr_t(1)); }
        if (field == 1) { source.record->indexCount = 2147483646u; source.record->indices = reinterpret_cast<void*>(uintptr_t(3)); }
        if (field == 2) { source.record->materialCount = INT32_MAX; source.record->materials = reinterpret_cast<void*>(uintptr_t(5)); }
        SourceState before(source); CHECK(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), 4096); })); before.Check(source);
    }
}

void LifetimeAndConcurrentReads() {
    std::array<std::unique_ptr<Frozen>, 2> frozen;
    { Model pmd(false), pmx(true); frozen[0] = Frozen::Capture(pmd.raw()); frozen[1] = Frozen::Capture(pmx.raw()); }
    std::atomic<unsigned> failures{0}; std::array<std::thread, 4> workers;
    try {
        for (auto& worker : workers) worker = std::thread([&] {
            failAfter = 0;
            for (unsigned repeat = 0; repeat < 500; ++repeat) {
                if (!Matches(*frozen[0], false, 0) || !Matches(*frozen[1], true, 4)) ++failures;
                auto vertex = Poison<MeshDefinitionVertex>(); auto face = Poison<MeshDefinitionTriangle>();
                auto material = Poison<MeshDefinitionMaterial>(); std::uint32_t index = 173;
                if (frozen[1]->VertexAt(UINT32_MAX, vertex) || !Unchanged(vertex) ||
                    frozen[1]->TriangleAt(UINT32_MAX, face) || !Unchanged(face) ||
                    frozen[1]->MaterialAt(UINT32_MAX, material) || !Unchanged(material) ||
                    frozen[1]->IndexAt(UINT32_MAX, index) || index != 173) ++failures;
            }
            failAfter = -1;
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (auto& worker : workers) worker.join(); CHECK(failures == 0);
}
}
int main() {
    const auto initialAllocations = allocations.load();
    FieldsAndRestore(); InvalidReadsAndEmpty(); ValidationAndUnsupported();
    BudgetAndAllocationFailures(); LifetimeAndConcurrentReads();
    CHECK(allocations.load() == initialAllocations);
    std::printf("PASS %u mesh definition snapshot checks\n", checks); return 0;
}
