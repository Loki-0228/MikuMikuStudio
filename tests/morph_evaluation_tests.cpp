// Independent CPU morph evaluation compared with real native morph passes.
#define NOMINMAX
#include "mikumikustudio/morph_evaluation.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
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
#include <utility>
#include <vector>

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
using Definitions = MorphDefinitionSnapshot;
using Frame = MorphEvaluationFrame;
using Weights = std::array<float, 13>;
namespace {
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
template<class T> bool Same(const T& a, const T& b) { return !std::memcmp(&a, &b, sizeof(T)); }
template<class T> T Poison() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> bool Unchanged(const T& value) { const auto before = Poison<T>(); return Same(value, before); }
template<class E, class F> bool Throws(F&& f) { try { f(); } catch (const E&) { return true; } catch (...) {} return false; }
mdl::MaterialMorphChannels Channels(float value) {
    std::array<float, 30> fields{}; fields.fill(value); mdl::MaterialMorphChannels out;
    std::memcpy(&out, fields.data(), sizeof(out)); out.reserved0 = out.reserved1 = 0; return out;
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::MorphRecord, 13> morphs{};
    std::array<mdl::BoneRecord, 3> skeleton{};
    std::array<mdl::PmxVertex, 8> pmx{};
    std::array<mdl::PmdVertex, 8> pmd{};
    std::array<mdl::ModelMaterialRecord, 2> rendered{};
    std::array<mdl::MaterialMorphPool, 2> baseMaterials{}, additive{}, multiplicative{};
    std::array<mdl::BoneMorphOffsetRecord, 3> boneWork{};
    std::array<mdl::PmdVertexMorphEntry, 4> vertexOffsets{}, baseVertices{};
    std::array<mdl::PmxBoneMorphEntry, 7> boneOffsets{};
    std::array<mdl::PmxBoneMorphEntry, 2> extraBones{};
    std::array<std::array<mdl::PmxUvMorphEntry, 4>, 5> uvOffsets{};
    std::array<std::array<mdl::PmxUvMorphEntry, 3>, 5> baseUv{};
    std::array<mdl::PmxMaterialMorphEntry, 2> mulOffsets{}, addOffsets{};
    std::array<mdl::PmxGroupMorphEntry, 10> earlyRefs{};
    std::array<mdl::PmxGroupMorphEntry, 3> lateRefs{};
    mdl::PmxGroupMorphEntry nestedRef{1, 4};
    Model() {
        record->physicsMode = 2; record->loadComplete = 1; record->pmxAdditionalUvCount = 4;
        record->vertexCount = 8; record->boneCount = 3; record->materialCount = 2; record->morphCount = 13;
        record->morphs = morphs.data(); record->boneTable = skeleton.data(); record->pmxVertices = pmx.data();
        record->rawVertices = pmd.data(); record->materials = rendered.data();
        record->morph0Table = baseVertices.data(); record->morph0Count = uint32_t(baseVertices.size());
        record->boneMorphTable = boneWork.data(); record->boneMorphCount = 3;
        record->materialMorphPools = {baseMaterials.data(), additive.data(), multiplicative.data()};
        for (unsigned i = 0; i < 3; ++i) {
            for (unsigned j = 0; j < 16; ++j) skeleton[i].matInit[j] = j % 5 == 0 ? 1.f : 0.f;
            boneWork[i].boneIndex = int32_t(i); boneWork[i].rotation[3] = 1;
        }
        for (unsigned i = 0; i < 8; ++i) {
            pmx[i].weightType = mdl::PmxWeightType::bdef1; pmx[i].bone[0] = 0; pmx[i].materialIndex = int32_t(i % 2);
            pmx[i].normal[1] = pmd[i].normal[1] = 1; pmd[i].weightPercent = 100;
            for (unsigned c = 0; c < 3; ++c) pmx[i].position[c] = pmd[i].position[c] = -1000.f - float(i + c);
        }
        for (unsigned i = 0; i < 13; ++i) {
            std::snprintf(morphs[i].name, 20, "morph-%u", i);
            morphs[i].panel = mdl::MorphPanel::other; morphs[i].value = 6789;
        }
        baseVertices = {{{7, {10, 11, 12}}, {2, {20, 21, 22}}, {7, {30, 31, 32}}, {5, {50, 51, 52}}}};
        vertexOffsets = {{{7, {16777216.f, 1.234567f, -.125f}}, {7, {-16777216.f, -.7654321f, .5f}},
            {7, {1, .3456789f, -.75f}}, {2, {.125f, -2.5f, 3.75f}}}};
        morphs[1].type = 1; morphs[1].offsetCount = 4; morphs[1].vertexEntries = vertexOffsets.data();
        boneOffsets = {{{0, {.1f, -.25f, .5f}, {2, 0, 0, .5f}},
            {0, {.2f, -.5f, 1}, {0, 3, 0, -.5f}},
            {2, {1, 2, 3}, {1, 0, 0, 2}},
            {2, {-1, -2, -3}, {0, 1, 0, -2}},
            {1, {.125f, 0, 0}, {1e-9f, 0, 0, .5f}},
            {1, {.25f, 0, 0}, {0, 0, 0, 0}},
            {0, {-.3f, .75f, -1.5f}, {0, 0, 4, .75f}}}};
        morphs[2].type = 2; morphs[2].boneCount = 7; morphs[2].boneEntries = boneOffsets.data();
        extraBones = {{{0, {.25f, -.125f, 0}, {0, 1, 1, -.25f}}, {2, {0, 1, 0}, {1, 0, 1, .25f}}}};
        morphs[10].type = 2; morphs[10].boneCount = 2; morphs[10].boneEntries = extraBones.data();
        for (unsigned family = 0; family < 5; ++family) {
            auto& morph = morphs[family + 3]; morph.type = uint8_t(family + 3);
            morph.uvCounts[family] = 4; morph.uvEntries[family] = uvOffsets[family].data();
            record->uvMorphCounts.byFamily[family] = 3; record->uvMorphTables.byFamily[family] = baseUv[family].data();
            for (unsigned e = 0; e < 4; ++e) {
                uvOffsets[family][e].vertexIndex = e == 3 ? 2 : 7;
                for (unsigned c = 0; c < 4; ++c) uvOffsets[family][e].offset[c] =
                    c == 0 && e < 2 ? (e ? -16777216.f : 16777216.f) : float(family + c + e) / 7;
            }
            for (unsigned e = 0; e < 3; ++e) {
                baseUv[family][e].vertexIndex = e == 1 ? 2 : 7;
                for (unsigned c = 0; c < 4; ++c) baseUv[family][e].offset[c] = float(family * 100 + e * 10 + c) + .125f;
            }
        }
        baseUv[0][2].offset[2] = std::numeric_limits<float>::quiet_NaN();
        baseUv[0][2].offset[3] = -std::numeric_limits<float>::infinity();
        uvOffsets[0][0].offset[2] = std::numeric_limits<float>::quiet_NaN();
        uvOffsets[0][0].offset[3] = std::numeric_limits<float>::infinity();
        for (unsigned i = 0; i < 2; ++i) {
            baseMaterials[i].channels = Channels(float(i + 2));
            baseMaterials[i].channels.reserved0 = std::numeric_limits<float>::quiet_NaN();
            baseMaterials[i].channels.reserved1 = std::numeric_limits<float>::infinity();
            mulOffsets[i].materialIndex = i ? 1 : -1; mulOffsets[i].operation = 0; mulOffsets[i].channels = Channels(i ? 1.25f : .5f);
            addOffsets[i].materialIndex = i ? -1 : 0; addOffsets[i].operation = i ? 197 : 1; addOffsets[i].channels = Channels(i ? -.2f : .125f);
        }
        morphs[8].type = 8; morphs[8].materialCount = 2; morphs[8].materialEntries = mulOffsets.data();
        morphs[9].type = 8; morphs[9].materialCount = 2; morphs[9].materialEntries = addOffsets.data();
        earlyRefs = {{{1, .5f}, {1, -.25f}, {2, .75f}, {3, -1.5f}, {7, .33333334f}, {8, .5f}, {8, -.25f}, {9, .375f}, {0, 2}, {12, 1}}};
        morphs[0].type = 0; morphs[0].groupCount = 10; morphs[0].groupEntries = earlyRefs.data();
        lateRefs = {{{8, .5f}, {8, .5f}, {10, -.75f}}};
        morphs[11].type = 0; morphs[11].groupCount = 3; morphs[11].groupEntries = lateRefs.data();
        morphs[12].type = 0; morphs[12].groupCount = 1; morphs[12].groupEntries = &nestedRef;
    }
    void Pmd() {
        record->physicsMode = 0; record->pmxAdditionalUvCount = 255;
        for (unsigned i = 0; i < 13; ++i) {
            morphs[i] = {}; morphs[i].type = 0; morphs[i].offsetCount = i < 3 ? 4 : 0;
            morphs[i].vertexEntries = vertexOffsets.data();
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
};
struct SourceState {
    std::unique_ptr<mdl::ModelRecord> record;
    decltype(Model::morphs) morphs; decltype(Model::skeleton) skeleton; decltype(Model::pmx) pmx; decltype(Model::pmd) pmd;
    decltype(Model::rendered) rendered; decltype(Model::baseMaterials) baseMaterials, additive, multiplicative;
    decltype(Model::boneWork) work; decltype(Model::vertexOffsets) vertexOffsets, baseVertices;
    decltype(Model::boneOffsets) boneOffsets; decltype(Model::extraBones) extraBones;
    decltype(Model::uvOffsets) uvOffsets; decltype(Model::baseUv) baseUv;
    decltype(Model::mulOffsets) mulOffsets, addOffsets;
    decltype(Model::earlyRefs) earlyRefs; decltype(Model::lateRefs) lateRefs; mdl::PmxGroupMorphEntry nestedRef;
    explicit SourceState(const Model& m) : record(std::make_unique<mdl::ModelRecord>(*m.record)), morphs(m.morphs), skeleton(m.skeleton),
        pmx(m.pmx), pmd(m.pmd), rendered(m.rendered), baseMaterials(m.baseMaterials), additive(m.additive), multiplicative(m.multiplicative),
        work(m.boneWork), vertexOffsets(m.vertexOffsets), baseVertices(m.baseVertices), boneOffsets(m.boneOffsets), extraBones(m.extraBones),
        uvOffsets(m.uvOffsets), baseUv(m.baseUv), mulOffsets(m.mulOffsets), addOffsets(m.addOffsets), earlyRefs(m.earlyRefs), lateRefs(m.lateRefs), nestedRef(m.nestedRef) {}
    void Check(const Model& m) const {
        CHECK(Same(*record, *m.record) && Same(morphs, m.morphs) && Same(skeleton, m.skeleton));
        CHECK(Same(pmx, m.pmx) && Same(pmd, m.pmd) && Same(rendered, m.rendered));
        CHECK(Same(baseMaterials, m.baseMaterials) && Same(additive, m.additive) && Same(multiplicative, m.multiplicative) && Same(work, m.boneWork));
        CHECK(Same(vertexOffsets, m.vertexOffsets) && Same(baseVertices, m.baseVertices) && Same(boneOffsets, m.boneOffsets) && Same(extraBones, m.extraBones));
        CHECK(Same(uvOffsets, m.uvOffsets) && Same(baseUv, m.baseUv) && Same(mulOffsets, m.mulOffsets) && Same(addOffsets, m.addOffsets));
        CHECK(Same(earlyRefs, m.earlyRefs) && Same(lateRefs, m.lateRefs) && Same(nestedRef, m.nestedRef));
    }
};
void Native(Model& model, const Weights& weights, GpuModelInput& geometry) {
    for (unsigned i = 0; i < model.record->morphCount; ++i) model.morphs[i].value = weights[i];
    // Use the real public native paths. No reimplementation of morph arithmetic
    // is used as the expected result, and no transform/physics pass is invoked.
    ModelApplyMorphs(model.raw());
    auto app = std::make_unique<MMDApp>(); app->state = MMDAppState{};
    float identity[16]{}; identity[0] = identity[5] = identity[10] = identity[15] = 1;
    CHECK(PrepareGpuModelInput(app.get(), model.raw(), identity, geometry, true));
}
void CompareNative(const Frame& frame, const Model& native, const GpuModelInput& geometry) {
    CHECK(frame.PhysicsMode() == native.record->physicsMode && frame.PositionCount() == 3);
    const uint32_t expectedIndices[]{2, 5, 7};
    for (uint32_t entry = 0; entry < frame.PositionCount(); ++entry) {
        uint32_t index = UINT32_MAX; std::array<float, 3> position;
        CHECK(frame.PositionIndexAt(entry, index) && index == expectedIndices[entry] && frame.PositionAt(index, position));
        CHECK(!std::memcmp(position.data(), geometry.vertices[index].position, sizeof(position)));
    }
    if (native.record->physicsMode != 2) {
        CHECK(frame.BoneCount() == 0 && frame.MaterialCount() == 0);
        for (uint32_t family = 0; family < 5; ++family) CHECK(frame.UvCount(family) == 0);
        return;
    }
    for (uint32_t family = 0; family < 5; ++family) {
        CHECK(frame.UvCount(family) == 2);
        for (uint32_t entry = 0; entry < 2; ++entry) {
            uint32_t index = UINT32_MAX; std::array<float, 4> uv;
            CHECK(frame.UvIndexAt(family, entry, index) && index == (entry ? 7u : 2u) && frame.UvAt(family, index, uv));
            const auto* expected = family ? geometry.vertices[index].additionalUv[family - 1] : geometry.vertices[index].uv;
            CHECK(!std::memcmp(uv.data(), expected, (family ? 4 : 2) * sizeof(float)));
            if (!family) {
                const auto& base = native.baseUv[0][entry ? 2 : 1];
                CHECK(!std::memcmp(uv.data() + 2, base.offset + 2, 2 * sizeof(float)));
            }
        }
    }
    CHECK(frame.BoneCount() == 3 && frame.MaterialCount() == 2);
    for (uint32_t index = 0; index < frame.BoneCount(); ++index) {
        mdl::BoneMorphOffsetRecord bone; CHECK(frame.BoneAt(index, bone) && Same(bone, native.boneWork[index]));
    }
    for (uint32_t index = 0; index < frame.MaterialCount(); ++index) {
        MorphMaterialEvaluation material; CHECK(frame.MaterialAt(index, material));
        CHECK(Same(material.base, native.baseMaterials[index].channels));
        CHECK(Same(material.additive, native.additive[index].channels) && Same(material.multiplicative, native.multiplicative[index].channels));
        const auto& actual = material.properties; const auto& expected = native.rendered[index];
        CHECK(!std::memcmp(actual.diffuse.data(), expected.diffuseMirror, 3 * sizeof(float)) && Same(actual.diffuse[3], expected.diffuse[3]));
        CHECK(!std::memcmp(actual.specular.data(), expected.specular, sizeof(expected.specular)) && !std::memcmp(actual.ambient.data(), expected.ambient, sizeof(expected.ambient)));
        CHECK(!std::memcmp(actual.edgeColor.data(), expected.edgeColor, sizeof(expected.edgeColor)) && Same(actual.specularPower, expected.specularPower) && Same(actual.edgeSize, expected.edgeSize));
    }
}
template<class Configure> void Parity(Configure&& configure, const Weights& weights) {
    Model source; configure(source); SourceState before(source); const auto originalWeights = weights;
    auto definitions = Definitions::Capture(source.raw()); auto frame = Frame::Evaluate(*definitions, weights.data(), source.record->morphCount);
    before.Check(source); CHECK(Same(weights, originalWeights));
    Model native; configure(native); GpuModelInput geometry; Native(native, weights, geometry); CompareNative(*frame, native, geometry);
}
void NativeParity() {
    const auto noChange = [](Model&) {};
    Parity(noChange, Weights{});
    Weights grouped{}; grouped[0] = 1.1f; Parity(noChange, grouped); // Targets' own zero weights do not suppress group contributions.
    Weights mixed{-.75f, 1.1f, -1.25f, .33333334f, -.2f, 1.5f, -.75f, 2.25f, .5f, -1.5f, .75f, 1.25f, -.375f};
    Parity(noChange, mixed);
    for (float weight : {-2.f, -.5f, .5f, 1.f, 2.5f}) { Weights weights{}; weights[2] = weight; weights[10] = 1.25f; Parity(noChange, weights); }
    // Reverse entry order on a separate pair of fixtures. This exercises native
    // non-commuting quaternion composition and cancellation in the new order.
    Parity([](Model& m) { std::swap(m.boneOffsets[0], m.boneOffsets[1]); std::swap(m.vertexOffsets[0], m.vertexOffsets[2]); }, mixed);
    for (const Weights& weights : {Weights{}, grouped, mixed}) Parity([](Model& m) { m.Pmd(); }, weights);
}
void HandCalculatedContracts() {
    Model source;
    // Three independent multiplicative applications with weight .5 and delta
    // .5 give .75^3 = .421875, not one application with summed weight 1.5.
    source.record->morphCount = 2;
    source.morphs[0].groupCount = 2; source.earlyRefs[0] = source.earlyRefs[1] = {1, .5f};
    source.morphs[1] = {}; source.morphs[1].type = 8; source.morphs[1].materialCount = 1; source.morphs[1].materialEntries = source.mulOffsets.data();
    auto definitions = Definitions::Capture(source.raw()); const float weights[]{1, .5f};
    auto frame = Frame::Evaluate(*definitions, weights, 2); MorphMaterialEvaluation material;
    CHECK(frame->BoneCount() == 0 && frame->MaterialAt(0, material));
    CHECK(material.multiplicative.diffuse[0] == .421875f && material.properties.diffuse[0] == .84375f);
    CHECK(material.additive.reserved0 == 0 && material.multiplicative.reserved1 == 0);
    // Group -> group -> vertex does not recurse. Directly activating the inner
    // group contributes the vertex once, including every repeated entry.
    Model nested; nested.record->morphCount = 3;
    nested.morphs[0].groupCount = 1; nested.earlyRefs[0] = {1, 1};
    nested.morphs[1] = {}; nested.morphs[1].type = 0; nested.morphs[1].groupCount = 1;
    nested.nestedRef = {2, 1}; nested.morphs[1].groupEntries = &nested.nestedRef;
    nested.morphs[2] = {}; nested.morphs[2].type = 1; nested.morphs[2].offsetCount = 1;
    nested.vertexOffsets[0] = {7, {4, 0, 0}}; nested.morphs[2].vertexEntries = nested.vertexOffsets.data();
    auto nestedDefinitions = Definitions::Capture(nested.raw()); const float outer[]{1, 0, 0}, inner[]{0, 1, 0};
    auto onlyOuter = Frame::Evaluate(*nestedDefinitions, outer, 3), onlyInner = Frame::Evaluate(*nestedDefinitions, inner, 3);
    std::array<float, 3> a, b; CHECK(onlyOuter->PositionAt(7, a) && onlyInner->PositionAt(7, b) && a[0] == 30 && b[0] == 34);
    Model pmd; pmd.Pmd(); auto pmdDefinitions = Definitions::Capture(pmd.raw()); Weights baseWeight{}; baseWeight[0] = 999;
    auto unchanged = Frame::Evaluate(*pmdDefinitions, baseWeight.data(), 13);
    CHECK(unchanged->PositionAt(7, a) && a[0] == 30 && unchanged->BoneCount() == 0 && unchanged->MaterialCount() == 0);
}
template<class T, class F> void ReadFails(F&& read) { auto out = Poison<T>(); CHECK(!read(out) && Unchanged(out)); }
void ReadBoundsAndSparse() {
    Model source; auto definitions = Definitions::Capture(source.raw()); Weights weights{}; auto frame = Frame::Evaluate(*definitions, weights.data(), 13);
    for (uint32_t index : {3u, UINT32_MAX}) {
        uint32_t out = 99; CHECK(!frame->PositionIndexAt(index, out) && out == 99);
        ReadFails<mdl::BoneMorphOffsetRecord>([&](auto& v) { return frame->BoneAt(index, v); });
    }
    for (uint32_t index : {0u, 1u, 3u, 4u, 6u, 8u, UINT32_MAX}) ReadFails<std::array<float, 3>>([&](auto& v) { return frame->PositionAt(index, v); });
    for (uint32_t family = 0; family < 5; ++family) {
        for (uint32_t index : {2u, UINT32_MAX}) { uint32_t out = 99; CHECK(!frame->UvIndexAt(family, index, out) && out == 99); }
        for (uint32_t index : {0u, 5u, 8u, UINT32_MAX}) ReadFails<std::array<float, 4>>([&](auto& v) { return frame->UvAt(family, index, v); });
    }
    for (uint32_t family : {5u, UINT32_MAX}) {
        CHECK(frame->UvCount(family) == 0); uint32_t out = 99; CHECK(!frame->UvIndexAt(family, 0, out) && out == 99);
        ReadFails<std::array<float, 4>>([&](auto& v) { return frame->UvAt(family, 2, v); });
    }
    for (uint32_t index : {2u, UINT32_MAX}) ReadFails<MorphMaterialEvaluation>([&](auto& v) { return frame->MaterialAt(index, v); });
    // Two-billion vertex identity space still retains only restored entries.
    source.record->vertexCount = INT32_MAX; auto large = Definitions::Capture(source.raw());
    auto sparse = Frame::Evaluate(*large, weights.data(), 13, 32768);
    CHECK(sparse->PositionCount() == 3 && sparse->RetainedBytes() < 32768);
    auto empty = std::make_unique<mdl::ModelRecord>(); auto emptyDefinitions = Definitions::Capture(reinterpret_cast<unsigned char*>(empty.get()));
    auto zero = Frame::Evaluate(*emptyDefinitions, nullptr, 0);
    CHECK(zero->PositionCount() == 0 && zero->BoneCount() == 0 && zero->MaterialCount() == 0);
}
void InvalidInputsAndMissingBases() {
    Model source; auto definitions = Definitions::Capture(source.raw()); Weights weights{};
    CHECK(Throws<std::invalid_argument>([&] { Frame::Evaluate(*definitions, nullptr, 13); }));
    for (uint32_t count : {0u, 12u, 14u, UINT32_MAX}) CHECK(Throws<std::invalid_argument>([&] { Frame::Evaluate(*definitions, weights.data(), count); }));
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        weights[12] = value; const auto before = weights;
        CHECK(Throws<std::invalid_argument>([&] { Frame::Evaluate(*definitions, weights.data(), 13); }) && Same(weights, before));
    }
    weights = {};
    for (unsigned mode = 0; mode < 7; ++mode) {
        Model missing;
        if (mode == 0) missing.record->morph0Count = 0;
        else if (mode <= 5) missing.record->uvMorphCounts.byFamily[mode - 1] = 0;
        else { missing.Pmd(); missing.record->morph0Count = 0; }
        auto frozen = Definitions::Capture(missing.raw()); SourceState before(missing);
        // Weight zero must not conceal an incomplete definition/base mapping.
        CHECK(Throws<std::invalid_argument>([&] { Frame::Evaluate(*frozen, weights.data(), 13); })); before.Check(missing);
    }
    Model missingOne; missingOne.record->morph0Count = 1; // Restores 7, but entry 2 still needs a base.
    auto partial = Definitions::Capture(missingOne.raw());
    CHECK(Throws<std::invalid_argument>([&] { Frame::Evaluate(*partial, weights.data(), 13); }));
}
void OverflowContracts() {
    const float huge = (std::numeric_limits<float>::max)();
    for (unsigned mode = 0; mode < 9; ++mode) {
        Model source; Weights weights{};
        if (mode == 0) { source.vertexOffsets[0].offset[1] = huge; weights[1] = 2; }
        if (mode == 1) { source.uvOffsets[0][0].offset[0] = huge; weights[3] = 2; }
        if (mode == 2) { source.uvOffsets[4][0].offset[3] = huge; weights[7] = 2; }
        if (mode == 3) { source.boneOffsets[0].translation[0] = huge; weights[2] = 2; }
        if (mode == 4) { source.addOffsets[0].channels.toonTint[3] = huge; weights[9] = 2; }
        if (mode == 5) { source.mulOffsets[0].channels.diffuse[0] = huge; weights[8] = 2; }
        if (mode == 6) { source.baseMaterials[0].channels.diffuse[0] = huge; source.mulOffsets[0].channels.diffuse[0] = 2; weights[8] = 1; }
        if (mode == 7) { source.Pmd(); source.vertexOffsets[0].offset[1] = huge; weights[1] = 2; }
        if (mode == 8) { source.morphs[0].groupCount = 1; source.earlyRefs[0] = {2, huge}; weights[0] = 2; }
        auto definitions = Definitions::Capture(source.raw()); SourceState before(source); const auto input = weights;
        CHECK(Throws<std::overflow_error>([&] { Frame::Evaluate(*definitions, weights.data(), 13); }));
        CHECK(Same(input, weights)); before.Check(source);
    }
}
void BudgetsAndFaults() {
    Model source; SourceState before(source); auto definitions = Definitions::Capture(source.raw()); Weights weights{}; weights[0] = .75f;
    auto first = Frame::Evaluate(*definitions, weights.data(), 13); const auto bytes = first->RetainedBytes();
    CHECK(bytes > 0); auto exact = Frame::Evaluate(*definitions, weights.data(), 13, bytes); CHECK(exact->RetainedBytes() == bytes);
    CHECK(Throws<std::length_error>([&] { Frame::Evaluate(*definitions, weights.data(), 13, bytes - 1); }));
    CHECK(Throws<std::length_error>([&] { Frame::Evaluate(*definitions, weights.data(), 13, 0); }));
    const auto originalWeights = weights; bool complete = false; unsigned failures = 0;
    for (int limit = 0; limit < 512; ++limit) {
        std::unique_ptr<Frame> frame; bool oom = false, unexpected = false; failAfter = limit;
        try { frame = Frame::Evaluate(*definitions, weights.data(), 13); } catch (const std::bad_alloc&) { oom = true; } catch (...) { unexpected = true; }
        failAfter = -1; CHECK(!unexpected && Same(weights, originalWeights)); before.Check(source);
        if (frame) { CHECK(frame->RetainedBytes() == bytes); complete = true; break; }
        CHECK(oom); ++failures;
    }
    CHECK(complete && failures > 0);
}
bool ReadFrame(const Frame& frame) {
    std::array<float, 3> position; std::array<float, 4> uv; mdl::BoneMorphOffsetRecord bone; MorphMaterialEvaluation material; uint32_t index = 99;
    if (frame.PhysicsMode() != 2 || frame.PositionCount() != 3 || frame.BoneCount() != 3 || frame.MaterialCount() != 2 || !frame.RetainedBytes() ||
        !frame.PositionIndexAt(0, index) || index != 2 || !frame.PositionAt(5, position) || position[0] != 50 ||
        !frame.BoneAt(0, bone) || bone.boneIndex != 0 || !frame.MaterialAt(0, material)) return false;
    for (uint32_t family = 0; family < 5; ++family)
        if (frame.UvCount(family) != 2 || !frame.UvIndexAt(family, 1, index) || index != 7 || !frame.UvAt(family, 7, uv)) return false;
    return true;
}
void IndependenceLifetimeAndConcurrency() {
    std::unique_ptr<Frame> first, second;
    { Model source; auto definitions = Definitions::Capture(source.raw()); Weights weights{};
      first = Frame::Evaluate(*definitions, weights.data(), 13); weights[1] = 1;
      second = Frame::Evaluate(*definitions, weights.data(), 13); weights.fill(-999); }
    std::array<float, 3> a, b; CHECK(first->PositionAt(2, a) && second->PositionAt(2, b) && a[0] == 20 && b[0] == 20.125f);
    std::atomic<unsigned> failures{0}; std::array<std::thread, 4> readers;
    try {
        for (auto& thread : readers) thread = std::thread([&] {
            failAfter = 0;
            for (unsigned repeat = 0; repeat < 1000; ++repeat) if (!ReadFrame(*first) || !ReadFrame(*second)) ++failures;
            failAfter = -1;
        });
    } catch (...) { for (auto& thread : readers) if (thread.joinable()) thread.join(); throw; }
    for (auto& thread : readers) thread.join(); CHECK(failures == 0);
    Model source; auto definitions = Definitions::Capture(source.raw()); SourceState before(source); std::array<std::thread, 4> evaluators;
    try {
        for (unsigned worker = 0; worker < evaluators.size(); ++worker) evaluators[worker] = std::thread([&, worker] {
            try {
                Weights weights{}; weights[1] = float(worker) * .5f;
                for (unsigned repeat = 0; repeat < 100; ++repeat) {
                    auto frame = Frame::Evaluate(*definitions, weights.data(), 13); std::array<float, 3> position;
                    if (!ReadFrame(*frame) || !frame->PositionAt(2, position) || position[0] != 20 + .0625f * float(worker)) ++failures;
                }
            } catch (...) { ++failures; }
        });
    } catch (...) { for (auto& thread : evaluators) if (thread.joinable()) thread.join(); throw; }
    for (auto& thread : evaluators) thread.join(); CHECK(failures == 0); before.Check(source);
}
}
int main() {
    try {
        NativeParity(); HandCalculatedContracts(); ReadBoundsAndSparse(); InvalidInputsAndMissingBases();
        OverflowContracts(); BudgetsAndFaults(); IndependenceLifetimeAndConcurrency();
        std::printf("PASS %u morph evaluation checks\n", checks); return 0;
    } catch (const std::exception& error) {
        failAfter = -1; std::fprintf(stderr, "FAIL unexpected exception: %s\n", error.what()); return 1;
    }
}
