// Frozen loaded morph definitions: typed payloads, restore tables and lifetime.
#define NOMINMAX
#include "mikumikustudio/morph_definition_snapshot.hpp"
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

namespace { thread_local int failAfter = -1; }
void* operator new(std::size_t bytes) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio;
using Frozen = MorphDefinitionSnapshot;
namespace {
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
template<class E, class F> bool Throws(F&& f) {
    try { f(); } catch (const E&) { return true; } catch (...) {} return false;
}
template<class T> T Poison() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> bool Same(const T& a, const T& b) { return !std::memcmp(&a, &b, sizeof(T)); }
template<class T> bool Unchanged(const T& value) { const auto before = Poison<T>(); return Same(value, before); }
template<class T> T* Unreadable() { return reinterpret_cast<T*>(uintptr_t(1)); }
mdl::MaterialMorphChannels Channels(float seed) {
    std::array<float, 30> raw{};
    for (unsigned i = 0; i < raw.size(); ++i) raw[i] = seed + float(i) / 8;
    raw[7] = std::numeric_limits<float>::quiet_NaN(); raw[12] = -std::numeric_limits<float>::infinity();
    mdl::MaterialMorphChannels out; static_assert(sizeof(out) == sizeof(raw), "native material fields");
    std::memcpy(&out, raw.data(), sizeof(out)); return out;
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::MorphRecord, 9> morphs{};
    std::array<mdl::PmdVertexMorphEntry, 2> vertices{};
    std::array<mdl::PmxBoneMorphEntry, 2> bones{};
    std::array<mdl::PmxGroupMorphEntry, 4> groups{{{0, -2}, {1, 1.5f}, {1, .25f}, {8, .75f}}};
    std::array<std::array<mdl::PmxUvMorphEntry, 2>, 5> uv{}, baseUv{};
    std::array<mdl::PmxMaterialMorphEntry, 2> materials{};
    std::array<mdl::PmdVertexMorphEntry, 3> baseVertices{};
    std::array<mdl::MaterialMorphPool, 2> baseMaterials{};
    Model() {
        record->morphCount = uint32_t(morphs.size()); record->morphs = morphs.data();
        record->vertexCount = 8; record->boneCount = 3; record->materialCount = 2;
        record->physicsMode = 2; record->pmxAdditionalUvCount = 4;
        record->morph0Count = uint32_t(baseVertices.size()); record->morph0Table = baseVertices.data();
        record->materialMorphPools.base = baseMaterials.data();
        // Capturing definitions must never read weights, keys, evaluation
        // scratch pools, actual vertex/bone geometry, or device resources.
        record->rawVertices = Unreadable<mdl::PmdVertex>(); record->pmxVertices = Unreadable<mdl::PmxVertex>();
        record->boneTable = Unreadable<mdl::BoneRecord>(); record->materials = Unreadable<mdl::ModelMaterialRecord>();
        record->morphKeys = Unreadable<mdl::MorphKey>(); record->morphKeyCursors = Unreadable<uint32_t>();
        record->morphTrackActive = Unreadable<unsigned char>(); record->boneMorphTable = Unreadable<mdl::BoneMorphOffsetRecord>();
        record->materialMorphPools.additive = Unreadable<mdl::MaterialMorphPool>();
        record->materialMorphPools.multiplicative = Unreadable<mdl::MaterialMorphPool>();
        record->searchCursor = 991; record->displayKeyCursor = 77;
        for (unsigned i = 0; i < morphs.size(); ++i) {
            auto& morph = morphs[i]; morph.type = uint8_t(i); morph.panel = static_cast<mdl::MorphPanel>(i == 8 ? 255 : i % 5);
            for (unsigned j = 0; j < 20; ++j) { morph.name[j] = char(128 + i + j); morph.nameEn[j] = char(40 + i + j); }
            morph.name[6] = 0; morph.nameEn[11] = 0;
            morph.jpText = Unreadable<wchar_t>(); morph.enText = Unreadable<wchar_t>();
            morph.value = std::numeric_limits<float>::quiet_NaN();
            morph.offsetCount = morph.boneCount = morph.groupCount = morph.materialCount = -123;
            morph.vertexEntries = Unreadable<mdl::PmdVertexMorphEntry>(); morph.boneEntries = Unreadable<mdl::PmxBoneMorphEntry>();
            morph.groupEntries = Unreadable<mdl::PmxGroupMorphEntry>(); morph.materialEntries = Unreadable<mdl::PmxMaterialMorphEntry>();
            for (unsigned family = 0; family < 5; ++family) { morph.uvCounts[family] = -456; morph.uvEntries[family] = Unreadable<mdl::PmxUvMorphEntry>(); }
        }
        morphs[0].groupCount = uint32_t(groups.size()); morphs[0].groupEntries = groups.data();
        morphs[1].offsetCount = uint32_t(vertices.size()); morphs[1].vertexEntries = vertices.data();
        morphs[2].boneCount = uint32_t(bones.size()); morphs[2].boneEntries = bones.data();
        morphs[8].materialCount = uint32_t(materials.size()); morphs[8].materialEntries = materials.data();
        vertices[0] = {7, {1.25f, -2.5f, 4}}; vertices[1] = {2, {-9, .125f, 3}};
        bones[0] = {2, {3, -4, .25f}, {2, -3, .5f, 4}}; // Deliberately non-unit quaternion.
        bones[1] = {0, {-2, 1, 8}, {0, 0, 0, 0}}; // Capture validates finiteness, not normalization.
        baseVertices[0] = {7, {101, 102, 103}}; baseVertices[1] = {2, {201, 202, 203}}; baseVertices[2] = {6, {301, 302, 303}};
        for (unsigned family = 0; family < 5; ++family) {
            morphs[family + 3].uvCounts[family] = 2; morphs[family + 3].uvEntries[family] = uv[family].data();
            record->uvMorphCounts.byFamily[family] = 2; record->uvMorphTables.byFamily[family] = baseUv[family].data();
            for (unsigned i = 0; i < 2; ++i) {
                uv[family][i].vertexIndex = i ? 2 : 7; baseUv[family][i].vertexIndex = i ? 6 : 2;
                for (unsigned c = 0; c < 4; ++c) {
                    uv[family][i].offset[c] = float(family * 100 + i * 10 + c) - .375f;
                    baseUv[family][i].offset[c] = float(1000 + family * 100 + i * 10 + c) + .125f;
                }
            }
        }
        for (unsigned i = 0; i < 2; ++i) {
            materials[i].materialIndex = i ? 1 : -1; materials[i].operation = i ? 197 : 0;
            materials[i].operationPadding[0] = 255; materials[i].operationPadding[1] = 12; materials[i].operationPadding[2] = 197;
            materials[i].channels = Channels(float(i) - 3);
            baseMaterials[i].channels = Channels(float(i + 100));
            std::memset(baseMaterials[i].prefix, 0xa5, sizeof(baseMaterials[i].prefix));
        }
    }
    void Pmd(uint8_t mode) {
        record->physicsMode = mode; record->pmxAdditionalUvCount = 255;
        for (unsigned i = 0; i < morphs.size(); ++i) {
            morphs[i].type = i == 0 ? 0 : uint8_t(247 + i); // type zero still carries vertices.
            morphs[i].panel = static_cast<mdl::MorphPanel>(255 - i);
            morphs[i].offsetCount = 2; morphs[i].vertexEntries = vertices.data();
            morphs[i].boneCount = morphs[i].groupCount = morphs[i].materialCount = -321;
            morphs[i].boneEntries = Unreadable<mdl::PmxBoneMorphEntry>(); morphs[i].groupEntries = Unreadable<mdl::PmxGroupMorphEntry>();
            morphs[i].materialEntries = Unreadable<mdl::PmxMaterialMorphEntry>();
            for (unsigned family = 0; family < 5; ++family) {
                morphs[i].uvCounts[family] = -100; morphs[i].uvEntries[family] = Unreadable<mdl::PmxUvMorphEntry>();
            }
        }
        record->materialMorphPools.base = Unreadable<mdl::MaterialMorphPool>();
        for (unsigned family = 0; family < 5; ++family) {
            record->uvMorphCounts.byFamily[family] = -789; record->uvMorphTables.byFamily[family] = Unreadable<mdl::PmxUvMorphEntry>();
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
};
struct SourceState {
    std::unique_ptr<mdl::ModelRecord> record;
    decltype(Model::morphs) morphs; decltype(Model::vertices) vertices; decltype(Model::bones) bones;
    decltype(Model::groups) groups; decltype(Model::uv) uv, baseUv;
    decltype(Model::materials) materials; decltype(Model::baseVertices) baseVertices;
    decltype(Model::baseMaterials) baseMaterials;
    explicit SourceState(const Model& source) : record(std::make_unique<mdl::ModelRecord>(*source.record)),
        morphs(source.morphs), vertices(source.vertices), bones(source.bones), groups(source.groups), uv(source.uv), baseUv(source.baseUv),
        materials(source.materials), baseVertices(source.baseVertices), baseMaterials(source.baseMaterials) {}
    void Check(const Model& source) const {
        CHECK(Same(*record, *source.record) && Same(morphs, source.morphs));
        CHECK(Same(vertices, source.vertices) && Same(bones, source.bones) && Same(groups, source.groups));
        CHECK(Same(uv, source.uv) && Same(baseUv, source.baseUv) && Same(materials, source.materials));
        CHECK(Same(baseVertices, source.baseVertices) && Same(baseMaterials, source.baseMaterials));
    }
};
bool NamesMatch(const MorphDefinition& a, const mdl::MorphRecord& b) {
    return !std::memcmp(a.name.data(), b.name, 20) && !std::memcmp(a.nameEn.data(), b.nameEn, 20) &&
        a.type == b.type && a.panel == uint8_t(b.panel);
}
void FullPmxDefinitions() {
    Model source; SourceState before(source); auto frozen = Frozen::Capture(source.raw()); before.Check(source);
    CHECK(frozen->MorphCount() == 9 && frozen->VertexCount() == 8 && frozen->BoneCount() == 3 && frozen->MaterialCount() == 2);
    CHECK(frozen->PhysicsMode() == 2 && frozen->AdditionalUvCount() == 4 && frozen->BaseVertexCount() == 3 && frozen->BaseMaterialCount() == 2);
    for (uint32_t i = 0; i < 9; ++i) {
        MorphDefinition out; CHECK(frozen->MorphAt(i, out) && NamesMatch(out, source.morphs[i]));
        CHECK(out.vertexCount == (i == 1 ? 2u : 0u) && out.boneCount == (i == 2 ? 2u : 0u) &&
            out.groupCount == (i == 0 ? 4u : 0u) && out.materialCount == (i == 8 ? 2u : 0u));
        for (unsigned family = 0; family < 5; ++family) CHECK(out.uvCounts[family] == (i == family + 3 ? 2u : 0u));
    }
    for (uint32_t i = 0; i < 2; ++i) {
        mdl::PmdVertexMorphEntry vertex; mdl::PmxBoneMorphEntry bone; mdl::PmxMaterialMorphEntry material; mdl::MaterialMorphChannels base;
        CHECK(frozen->VertexAt(1, i, vertex) && Same(vertex, source.vertices[i]));
        CHECK(frozen->BoneAt(2, i, bone) && Same(bone, source.bones[i]));
        CHECK(frozen->MaterialAt(8, i, material) && Same(material, source.materials[i]));
        CHECK(frozen->BaseMaterialAt(i, base) && Same(base, source.baseMaterials[i].channels));
    }
    for (uint32_t i = 0; i < 4; ++i) { mdl::PmxGroupMorphEntry out; CHECK(frozen->GroupAt(0, i, out) && Same(out, source.groups[i])); }
    for (uint32_t i = 0; i < 3; ++i) { mdl::PmdVertexMorphEntry out; CHECK(frozen->BaseVertexAt(i, out) && Same(out, source.baseVertices[i])); }
    for (uint32_t family = 0; family < 5; ++family) {
        CHECK(frozen->BaseUvCount(family) == 2);
        for (uint32_t i = 0; i < 2; ++i) {
            mdl::PmxUvMorphEntry out; CHECK(frozen->UvAt(family + 3, family, i, out) && Same(out, source.uv[family][i]));
            CHECK(frozen->BaseUvAt(family, i, out) && Same(out, source.baseUv[family][i]));
        }
    }
}
void PmdAndIgnoredFields() {
    for (uint8_t mode : {uint8_t(0), uint8_t(1), uint8_t(255)}) {
        Model source; source.Pmd(mode); SourceState before(source); auto frozen = Frozen::Capture(source.raw()); before.Check(source);
        CHECK(frozen->PhysicsMode() == mode && frozen->AdditionalUvCount() == 255 && frozen->BaseMaterialCount() == 0);
        for (uint32_t i = 0; i < 9; ++i) {
            MorphDefinition out; CHECK(frozen->MorphAt(i, out) && NamesMatch(out, source.morphs[i]));
            CHECK(out.vertexCount == 2 && !out.boneCount && !out.groupCount && !out.materialCount);
            for (uint32_t family = 0; family < 5; ++family) CHECK(!out.uvCounts[family] && !frozen->BaseUvCount(family));
            mdl::PmdVertexMorphEntry vertex; CHECK(frozen->VertexAt(i, 0, vertex) && Same(vertex, source.vertices[0]) && vertex.vertexIndex == 7);
            // 7 exceeds the three-entry base table: it is already a model index
            // and must neither be remapped again nor validated as a base slot.
            auto group = Poison<mdl::PmxGroupMorphEntry>(); CHECK(!frozen->GroupAt(i, 0, group) && Unchanged(group));
        }
        for (uint32_t i = 0; i < 3; ++i) { mdl::PmdVertexMorphEntry out; CHECK(frozen->BaseVertexAt(i, out) && Same(out, source.baseVertices[i])); }
    }
    // Native family-zero UV uses XY only; the two unused components are raw
    // bytes. Other UV families use every component and are checked below.
    Model source; const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    source.uv[0][0].offset[2] = nan; source.uv[0][0].offset[3] = inf;
    source.baseUv[0][1].offset[2] = -inf; source.baseUv[0][1].offset[3] = nan;
    SourceState before(source); auto frozen = Frozen::Capture(source.raw()); before.Check(source);
    mdl::PmxUvMorphEntry out;
    CHECK(frozen->UvAt(3, 0, 0, out) && Same(out, source.uv[0][0]));
    CHECK(frozen->BaseUvAt(0, 1, out) && Same(out, source.baseUv[0][1]));
}
template<class T, class F> void RejectedRead(F&& f) { auto out = Poison<T>(); CHECK(!f(out) && Unchanged(out)); }
void ReadBoundsAndEmpty() {
    Model source; auto frozen = Frozen::Capture(source.raw());
    for (uint32_t index : {9u, UINT32_MAX}) {
        RejectedRead<MorphDefinition>([&](auto& out) { return frozen->MorphAt(index, out); });
        RejectedRead<mdl::PmdVertexMorphEntry>([&](auto& out) { return frozen->VertexAt(index, 0, out); });
        RejectedRead<mdl::PmxBoneMorphEntry>([&](auto& out) { return frozen->BoneAt(index, 0, out); });
        RejectedRead<mdl::PmxGroupMorphEntry>([&](auto& out) { return frozen->GroupAt(index, 0, out); });
        RejectedRead<mdl::PmxUvMorphEntry>([&](auto& out) { return frozen->UvAt(index, 0, 0, out); });
        RejectedRead<mdl::PmxMaterialMorphEntry>([&](auto& out) { return frozen->MaterialAt(index, 0, out); });
    }
    RejectedRead<mdl::PmdVertexMorphEntry>([&](auto& out) { return frozen->VertexAt(0, 0, out); });
    RejectedRead<mdl::PmxBoneMorphEntry>([&](auto& out) { return frozen->BoneAt(0, 0, out); });
    RejectedRead<mdl::PmxGroupMorphEntry>([&](auto& out) { return frozen->GroupAt(1, 0, out); });
    RejectedRead<mdl::PmxMaterialMorphEntry>([&](auto& out) { return frozen->MaterialAt(0, 0, out); });
    RejectedRead<mdl::PmxUvMorphEntry>([&](auto& out) { return frozen->UvAt(3, 1, 0, out); });
    for (uint32_t index : {2u, UINT32_MAX}) {
        RejectedRead<mdl::PmdVertexMorphEntry>([&](auto& out) { return frozen->VertexAt(1, index, out); });
        RejectedRead<mdl::PmxBoneMorphEntry>([&](auto& out) { return frozen->BoneAt(2, index, out); });
        RejectedRead<mdl::PmxMaterialMorphEntry>([&](auto& out) { return frozen->MaterialAt(8, index, out); });
        RejectedRead<mdl::MaterialMorphChannels>([&](auto& out) { return frozen->BaseMaterialAt(index, out); });
        for (uint32_t family = 0; family < 5; ++family) {
            RejectedRead<mdl::PmxUvMorphEntry>([&](auto& out) { return frozen->UvAt(family + 3, family, index, out); });
            RejectedRead<mdl::PmxUvMorphEntry>([&](auto& out) { return frozen->BaseUvAt(family, index, out); });
        }
    }
    for (uint32_t family : {5u, UINT32_MAX}) {
        CHECK(frozen->BaseUvCount(family) == 0);
        RejectedRead<mdl::PmxUvMorphEntry>([&](auto& out) { return frozen->UvAt(3, family, 0, out); });
        RejectedRead<mdl::PmxUvMorphEntry>([&](auto& out) { return frozen->BaseUvAt(family, 0, out); });
    }
    for (uint32_t index : {4u, UINT32_MAX}) RejectedRead<mdl::PmxGroupMorphEntry>([&](auto& out) { return frozen->GroupAt(0, index, out); });
    for (uint32_t index : {3u, UINT32_MAX}) RejectedRead<mdl::PmdVertexMorphEntry>([&](auto& out) { return frozen->BaseVertexAt(index, out); });
    auto empty = std::make_unique<mdl::ModelRecord>(); empty->physicsMode = 2;
    auto zero = Frozen::Capture(reinterpret_cast<unsigned char*>(empty.get()));
    CHECK(!zero->MorphCount() && !zero->VertexCount() && !zero->BoneCount() && !zero->MaterialCount() && !zero->BaseMaterialCount() && !zero->BaseVertexCount());
    RejectedRead<MorphDefinition>([&](auto& out) { return zero->MorphAt(0, out); });
    CHECK(Throws<std::invalid_argument>([] { Frozen::Capture(nullptr); }));
    Model noEntries;
    noEntries.morphs[0].groupCount = noEntries.morphs[1].offsetCount = noEntries.morphs[2].boneCount = noEntries.morphs[8].materialCount = 0;
    noEntries.morphs[0].groupEntries = nullptr; noEntries.morphs[1].vertexEntries = nullptr;
    noEntries.morphs[2].boneEntries = nullptr; noEntries.morphs[8].materialEntries = nullptr;
    for (unsigned family = 0; family < 5; ++family) { noEntries.morphs[family + 3].uvCounts[family] = 0; noEntries.morphs[family + 3].uvEntries[family] = nullptr; }
    auto noOffsets = Frozen::Capture(noEntries.raw()); CHECK(noOffsets->MorphCount() == 9 && noOffsets->BaseVertexCount() == 3);
}
template<class F> void Invalid(F&& edit) {
    Model source; edit(source); SourceState before(source);
    CHECK(Throws<std::invalid_argument>([&] { Frozen::Capture(source.raw()); })); before.Check(source);
}
void InvalidDefinitions() {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    Invalid([](Model& m) { m.record->morphs = nullptr; });
    Invalid([](Model& m) { m.record->morphCount = UINT32_MAX; });
    Invalid([](Model& m) { m.record->vertexCount = UINT32_MAX; });
    Invalid([](Model& m) { m.record->boneCount = UINT32_MAX; });
    Invalid([](Model& m) { m.record->materialCount = UINT32_MAX; });
    Invalid([](Model& m) { m.record->pmxAdditionalUvCount = 5; });
    for (uint8_t type : {uint8_t(9), uint8_t(10), uint8_t(255)}) Invalid([&](Model& m) { m.morphs[0].type = type; });
    for (uint8_t mode : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
        Invalid([&](Model& m) { if (mode != 2) m.Pmd(mode); m.morphs[3].impulseEntries = Unreadable<unsigned char>(); });
        Invalid([&](Model& m) { if (mode != 2) m.Pmd(mode); m.morphs[3].impulse2Entries = Unreadable<unsigned char>(); });
    }
    Invalid([](Model& m) { m.morphs[0].groupCount = -1; });
    Invalid([](Model& m) { m.morphs[1].offsetCount = -1; });
    Invalid([](Model& m) { m.morphs[2].boneCount = -1; });
    Invalid([](Model& m) { m.morphs[8].materialCount = -1; });
    Invalid([](Model& m) { m.morphs[0].groupEntries = nullptr; });
    Invalid([](Model& m) { m.morphs[1].vertexEntries = nullptr; });
    Invalid([](Model& m) { m.morphs[2].boneEntries = nullptr; });
    Invalid([](Model& m) { m.morphs[8].materialEntries = nullptr; });
    Invalid([](Model& m) { m.record->morph0Table = nullptr; });
    Invalid([](Model& m) { m.record->materialMorphPools.base = nullptr; });
    Invalid([](Model& m) { m.groups[0].morphIndex = -1; });
    Invalid([](Model& m) { m.groups[0].morphIndex = 9; });
    Invalid([&](Model& m) { m.groups[0].weight = nan; });
    Invalid([](Model& m) { m.vertices[0].vertexIndex = 8; });
    Invalid([](Model& m) { m.vertices[0].vertexIndex = UINT32_MAX; });
    Invalid([](Model& m) { m.bones[0].boneIndex = -1; });
    Invalid([](Model& m) { m.bones[0].boneIndex = 3; });
    Invalid([](Model& m) { m.materials[0].materialIndex = -2; });
    Invalid([](Model& m) { m.materials[0].materialIndex = 2; });
    Invalid([](Model& m) { m.baseVertices[0].vertexIndex = 8; });
    Invalid([](Model& m) { m.Pmd(0); m.morphs[0].offsetCount = -1; });
    Invalid([](Model& m) { m.Pmd(1); m.morphs[0].vertexEntries = nullptr; });
    Invalid([](Model& m) { m.Pmd(0); m.vertices[0].vertexIndex = 8; });
    for (unsigned component = 0; component < 3; ++component) {
        Invalid([&](Model& m) { m.vertices[0].offset[component] = nan; });
        Invalid([&](Model& m) { m.baseVertices[0].offset[component] = inf; });
        Invalid([&](Model& m) { m.bones[0].translation[component] = nan; });
    }
    for (unsigned component = 0; component < 4; ++component) Invalid([&](Model& m) { m.bones[0].rotation[component] = inf; });
    for (unsigned family = 0; family < 5; ++family) {
        Invalid([&](Model& m) { m.morphs[family + 3].uvCounts[family] = -1; });
        Invalid([&](Model& m) { m.morphs[family + 3].uvEntries[family] = nullptr; });
        Invalid([&](Model& m) { m.record->uvMorphCounts.byFamily[family] = -1; });
        Invalid([&](Model& m) { m.record->uvMorphTables.byFamily[family] = nullptr; });
        Invalid([&](Model& m) { m.uv[family][0].vertexIndex = -1; });
        Invalid([&](Model& m) { m.uv[family][0].vertexIndex = 8; });
        Invalid([&](Model& m) { m.baseUv[family][0].vertexIndex = 8; });
        for (unsigned component = 0; component < (family == 0 ? 2u : 4u); ++component) {
            Invalid([&](Model& m) { m.uv[family][0].offset[component] = nan; });
            Invalid([&](Model& m) { m.baseUv[family][0].offset[component] = inf; });
        }
    }
    // Test all 28 meaningful native material floats, excluding the two gaps.
    for (unsigned component = 0; component < 30; ++component) if (component != 7 && component != 12) {
        Invalid([&](Model& m) { std::memcpy(reinterpret_cast<unsigned char*>(&m.materials[0].channels) + component * sizeof(float), &nan, sizeof(float)); });
        Invalid([&](Model& m) { std::memcpy(reinterpret_cast<unsigned char*>(&m.baseMaterials[0].channels) + component * sizeof(float), &inf, sizeof(float)); });
    }
}
void SourceChangesAndSharedPayload() {
    Model source; auto old = Frozen::Capture(source.raw());
    source.morphs[1].value = -10000; source.record->searchCursor = 22;
    auto weightOnly = Frozen::Capture(source.raw()); MorphDefinition before, after;
    CHECK(old->MorphAt(1, before) && weightOnly->MorphAt(1, after) && NamesMatch(after, source.morphs[1]));
    CHECK(before.vertexCount == after.vertexCount && before.boneCount == after.boneCount && before.groupCount == after.groupCount &&
        before.materialCount == after.materialCount && before.uvCounts == after.uvCounts);
    source.morphs[1].name[0] = 'Z'; source.vertices[0].offset[0] = -11; source.bones[0].rotation[0] = -12;
    source.groups[0].weight = 99; source.uv[4][0].offset[3] = -13; source.materials[0].operation = 255;
    source.baseVertices[0].offset[0] = -14; source.baseUv[3][0].offset[1] = -15; source.baseMaterials[0].channels.edgeSize = -16;
    auto fresh = Frozen::Capture(source.raw());
    mdl::PmdVertexMorphEntry v0, v1; mdl::PmxBoneMorphEntry b0, b1; mdl::PmxGroupMorphEntry g0, g1;
    mdl::PmxUvMorphEntry u0, u1; mdl::PmxMaterialMorphEntry m0, m1; mdl::MaterialMorphChannels c0, c1;
    CHECK(old->MorphAt(1, before) && fresh->MorphAt(1, after) && before.name[0] != after.name[0]);
    CHECK(old->VertexAt(1, 0, v0) && fresh->VertexAt(1, 0, v1) && v0.offset[0] == 1.25f && v1.offset[0] == -11);
    CHECK(old->BoneAt(2, 0, b0) && fresh->BoneAt(2, 0, b1) && b0.rotation[0] == 2 && b1.rotation[0] == -12);
    CHECK(old->GroupAt(0, 0, g0) && fresh->GroupAt(0, 0, g1) && g0.weight == -2 && g1.weight == 99);
    CHECK(old->UvAt(7, 4, 0, u0) && fresh->UvAt(7, 4, 0, u1) && u0.offset[3] == 402.625f && u1.offset[3] == -13);
    CHECK(old->MaterialAt(8, 0, m0) && fresh->MaterialAt(8, 0, m1) && m0.operation == 0 && m1.operation == 255);
    CHECK(old->BaseVertexAt(0, v0) && fresh->BaseVertexAt(0, v1) && v0.offset[0] == 101 && v1.offset[0] == -14);
    CHECK(old->BaseUvAt(3, 0, u0) && fresh->BaseUvAt(3, 0, u1) && u0.offset[1] == 1301.125f && u1.offset[1] == -15);
    CHECK(old->BaseMaterialAt(0, c0) && fresh->BaseMaterialAt(0, c1) && c0.edgeSize != -16 && c1.edgeSize == -16);
    // Source aliases are legal. A frozen definition must own the shared bytes,
    // keep both logical entry sequences and preserve a two-node group cycle.
    source.morphs[1].type = 0; source.morphs[1].groupCount = 4; source.morphs[1].groupEntries = source.groups.data();
    auto aliases = Frozen::Capture(source.raw()); source.groups[1].weight = -200;
    CHECK(aliases->GroupAt(0, 1, g0) && aliases->GroupAt(1, 1, g1) && Same(g0, g1) && g0.weight == 1.5f);
    // Distinct payloads of the same kind must remain attached to their owner.
    // This also checks UV indexing when two morphs use the same family.
    Model owners; owners.morphs[0].type = 1; owners.morphs[0].offsetCount = 2;
    owners.morphs[0].vertexEntries = owners.baseVertices.data();
    owners.morphs[3].type = 7; owners.morphs[3].uvCounts[4] = 2; owners.morphs[3].uvEntries[4] = owners.baseUv[4].data();
    auto distinct = Frozen::Capture(owners.raw());
    CHECK(distinct->VertexAt(0, 0, v0) && distinct->VertexAt(1, 0, v1) && v0.offset[0] == 101 && v1.offset[0] == 1.25f);
    CHECK(distinct->UvAt(3, 4, 0, u0) && distinct->UvAt(7, 4, 0, u1) && u0.offset[0] == 1400.125f && u1.offset[0] == 399.625f);
}
void BudgetsAndFaults() {
    Model source; SourceState before(source); auto baseline = Frozen::Capture(source.raw()); const auto bytes = baseline->RetainedBytes();
    CHECK(bytes >= 9 * sizeof(MorphDefinition) + 5 * 4 * sizeof(mdl::PmxUvMorphEntry) + 2 * sizeof(mdl::PmxMaterialMorphEntry));
    auto exact = Frozen::Capture(source.raw(), bytes); CHECK(exact->RetainedBytes() == bytes);
    CHECK(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), bytes - 1); }));
    CHECK(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), 0); })); before.Check(source);
    // Minimal storage must be budgeted before walking any huge advertised
    // payload; the backing arrays here deliberately remain small.
    for (unsigned mode = 0; mode < 9; ++mode) {
        Model large;
        if (mode == 0) large.record->morphCount = 1000000;
        if (mode == 1) large.morphs[0].groupCount = 1000000;
        if (mode == 2) large.morphs[1].offsetCount = 1000000;
        if (mode == 3) large.morphs[2].boneCount = 1000000;
        if (mode == 4) large.morphs[7].uvCounts[4] = 1000000;
        if (mode == 5) large.morphs[8].materialCount = 1000000;
        if (mode == 6) large.record->morph0Count = 1000000;
        if (mode == 7) large.record->uvMorphCounts.byFamily[4] = 1000000;
        if (mode == 8) large.record->materialCount = 1000000;
        CHECK(Throws<std::length_error>([&] { Frozen::Capture(large.raw(), 32768); }));
    }
    bool complete = false; unsigned failures = 0;
    for (int limit = 0; limit < 512; ++limit) {
        std::unique_ptr<Frozen> copy; bool oom = false, unexpected = false; failAfter = limit;
        try { copy = Frozen::Capture(source.raw()); } catch (const std::bad_alloc&) { oom = true; } catch (...) { unexpected = true; }
        failAfter = -1; CHECK(!unexpected); before.Check(source);
        if (copy) { CHECK(copy->RetainedBytes() == bytes && copy->MorphCount() == 9); complete = true; break; }
        CHECK(oom); ++failures;
    }
    CHECK(complete && failures > 0);
}
void LifetimeAndConcurrentNoAllocation() {
    std::unique_ptr<Frozen> frozen;
    { Model source; frozen = Frozen::Capture(source.raw()); }
    std::atomic<unsigned> failures{0}; std::array<std::thread, 4> workers;
    try {
        for (auto& worker : workers) worker = std::thread([&] {
            failAfter = 0;
            for (unsigned repeat = 0; repeat < 1000; ++repeat) {
                MorphDefinition definition; mdl::PmdVertexMorphEntry vertex; mdl::PmxBoneMorphEntry bone; mdl::PmxGroupMorphEntry group;
                mdl::PmxUvMorphEntry uv; mdl::PmxMaterialMorphEntry material; mdl::MaterialMorphChannels base;
                bool ok = frozen->MorphAt(1, definition) && definition.name[0] == char(129) && definition.name[6] == 0 && definition.name[19] == char(148) &&
                    frozen->VertexAt(1, 0, vertex) && vertex.vertexIndex == 7 && vertex.offset[0] == 1.25f &&
                    frozen->BoneAt(2, 0, bone) && bone.rotation[0] == 2 && bone.rotation[3] == 4 &&
                    frozen->GroupAt(0, 2, group) && group.morphIndex == 1 && group.weight == .25f &&
                    frozen->MaterialAt(8, 1, material) && material.operation == 197 && material.operationPadding[0] == 255 &&
                    frozen->BaseVertexAt(2, vertex) && vertex.vertexIndex == 6 && vertex.offset[0] == 301 &&
                    frozen->BaseMaterialAt(1, base) && base.diffuse[0] == 101;
                for (uint32_t family = 0; family < 5; ++family) {
                    ok = ok && frozen->UvAt(family + 3, family, 0, uv) && uv.offset[0] == float(family * 100) - .375f;
                    ok = ok && frozen->BaseUvAt(family, 0, uv) && uv.offset[0] == float(1000 + family * 100) + .125f && frozen->BaseUvCount(family) == 2;
                }
                if (!ok || frozen->MorphCount() != 9 || frozen->VertexCount() != 8 || frozen->BoneCount() != 3 || frozen->MaterialCount() != 2 ||
                    frozen->PhysicsMode() != 2 || frozen->AdditionalUvCount() != 4 || frozen->BaseVertexCount() != 3 || frozen->BaseMaterialCount() != 2 || !frozen->RetainedBytes()) ++failures;
            }
            failAfter = -1;
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (auto& worker : workers) worker.join(); CHECK(failures == 0);
}
}
int main() {
    try {
        FullPmxDefinitions(); PmdAndIgnoredFields(); ReadBoundsAndEmpty(); InvalidDefinitions();
        SourceChangesAndSharedPayload(); BudgetsAndFaults(); LifetimeAndConcurrentNoAllocation();
        std::printf("PASS %u morph definition snapshot checks\n", checks); return 0;
    } catch (const std::exception& error) {
        failAfter = -1; std::fprintf(stderr, "FAIL unexpected exception: %s\n", error.what()); return 1;
    }
}
