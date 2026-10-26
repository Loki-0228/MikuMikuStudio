// Static bone definitions: ownership, validation and allocation-free readers.
#define NOMINMAX
#include "mikumikustudio/bone_definition_snapshot.hpp"
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

namespace { thread_local int failAfter = -1; }
void* operator new(std::size_t bytes) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio;
using Frozen = BoneDefinitionSnapshot;
namespace {
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
template<class E, class F> bool Throws(F&& function) {
    try { function(); } catch (const E&) { return true; } catch (...) {} return false;
}
template<class T> T Poison() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> bool Unchanged(const T& value) { const auto before = Poison<T>(); return !std::memcmp(&value, &before, sizeof(value)); }
template<class T> bool BytesEqual(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(T)));
}
template<std::size_t N> bool SameFloats(const std::array<float, N>& a, const float (&b)[N]) {
    return !std::memcmp(a.data(), b, sizeof(b));
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones = std::vector<mdl::BoneRecord>(4);
    std::vector<mdl::IkChain> ik = std::vector<mdl::IkChain>(2);
    std::vector<mdl::BoneOrderEntry> selectors = std::vector<mdl::BoneOrderEntry>(3);
    std::vector<mdl::PmxIkLinkRecord> pmx = std::vector<mdl::PmxIkLinkRecord>(3);
    std::vector<uint16_t> links{2, 2, 0};
    Model() {
        record->boneTable = bones.data(); record->boneCount = uint32_t(bones.size());
        record->ikChains = ik.data(); record->ikChainCount = uint32_t(ik.size());
        record->boneOrderTable = selectors.data(); record->boneOrderCount = uint32_t(selectors.size());
        record->physicsMode = 2; record->maxBoneLayer = INT32_MIN;
        record->selectedBone = 3; record->searchCursor = 917; record->displayKeyCursor = 81;
        selectors[0].boneIndex = -1; selectors[1].boneIndex = 0; selectors[2].boneIndex = 2;
        for (unsigned i = 0; i < selectors.size(); ++i) {
            selectors[i].linkedModel = 41; selectors[i].linkedBone = 90 + i;
            selectors[i].windowStart = 123; selectors[i].windowEnd = 456;
        }
        for (unsigned i = 0; i < bones.size(); ++i) {
            auto& bone = bones[i]; bone.parent = -1; bone.slotIndex = -1;
            bone.tailBone = -1; bone.tailIdx = -1; bone.ikTarget = -1;
            bone.type = mdl::BoneType::RotateMove; bone.layer = i % 2 ? INT32_MAX : INT32_MIN;
            bone.hasFlag = uint8_t(197 + i); bone.hasRigidBody = uint8_t(250 + i);
            bone.extParent = i % 2 ? INT32_MIN : INT32_MAX;
            // Fixed 20-byte names, including embedded zeroes and non-ASCII bytes.
            // Loader text pointers are deliberately unusable and must not be read.
            for (unsigned j = 0; j < 20; ++j) { bone.name[j] = char(128 + i + j); bone.nameEn[j] = char(60 + i + j); }
            bone.name[6] = 0; bone.nameEn[9] = 0;
            bone.jpText = reinterpret_cast<wchar_t*>(uintptr_t(1)); bone.enText = reinterpret_cast<wchar_t*>(uintptr_t(3));
            for (unsigned j = 0; j < 3; ++j) {
                bone.position[j] = float(i * 10 + j) + .25f; bone.tailOffset[j] = float(j) - 1.5f;
                bone.axis[j] = float(j) + .125f; bone.ikLimitMin[j] = -float(j + 1); bone.ikLimitMax[j] = float(j + 2);
                bone.trans[j] = float(1000 + i); bone.physicsOffset[j] = float(-2000 - int(i));
            }
            for (unsigned j = 0; j < 6; ++j) bone.localAxes[j] = float(j) / 8;
            for (unsigned j = 0; j < 16; ++j) bone.matWorld[j] = float(200 + j);
            bone.rotQuat[3] = .75f; bone.physicsDisabled = 231; bone.selState = 123; bone.selState2 = -345;
        }
        bones[0].parent = 2; bones[1].parent = 0; bones[3].parent = 1; // Forward parent is valid.
        bones[0].slotIndex = 1; bones[2].slotIndex = 2;
        bones[0].flags = mdl::kBoneFlagTailIsBone | mdl::kBoneFlagIk | mdl::kBoneFlagInheritMask |
            mdl::kBoneFlagFixedAxis | mdl::kBoneFlagLocalAxes | mdl::kBoneFlagExternalParent | mdl::kBoneFlagVisible;
        bones[0].tailBone = 2; bones[0].tailIdx = 1; bones[0].inheritRatio = -.375f;
        bones[0].twistEnable = 197; bones[0].ikTarget = 3; bones[0].ikLoop = 1234; bones[0].ikAngle = .125f;
        bones[0].ikLinkCount = int32_t(pmx.size()); bones[0].ikLinks = pmx.data();
        bones[1].type = mdl::BoneType::RotateGrant; bones[1].tailIdx = 0; bones[1].inheritRatio = 1.25f;
        bones[2].type = mdl::BoneType::CoRotate; bones[2].tailBone = 1; bones[2].tailIdx = -250;
        bones[3].type = mdl::BoneType::FixedAxis;
        for (unsigned i = 0; i < pmx.size(); ++i) {
            pmx[i].boneIndex = i ? 1 : 0; pmx[i].hasLimits = i == 0 ? 1 : uint8_t(i == 1 ? 0 : 197);
            for (unsigned j = 0; j < 3; ++j) { pmx[i].minimum[j] = -float(i + j + 1); pmx[i].maximum[j] = float(i + j + 2); }
        }
        ik[0].boneIndex = 2; ik[0].targetBone = 0; ik[0].links = links.data(); ik[0].linkCount = uint8_t(links.size());
        ik[0].iterations = 65535; ik[0].enabled = 197; ik[0].maxAngle = -.25f;
        ik[1].boneIndex = 0; ik[1].targetBone = 3; ik[1].enabled = 255; // Zero links and iterations are valid.
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
};
struct SourceState {
    std::unique_ptr<mdl::ModelRecord> record;
    std::vector<mdl::BoneRecord> bones;
    std::vector<mdl::IkChain> ik;
    std::vector<mdl::BoneOrderEntry> selectors;
    std::vector<mdl::PmxIkLinkRecord> pmx;
    std::vector<uint16_t> links;
    explicit SourceState(const Model& source) : record(std::make_unique<mdl::ModelRecord>(*source.record)),
        bones(source.bones), ik(source.ik), selectors(source.selectors), pmx(source.pmx), links(source.links) {}
    void Check(const Model& source) const {
        CHECK(!std::memcmp(record.get(), source.record.get(), sizeof(*record)));
        CHECK(BytesEqual(bones, source.bones) && BytesEqual(ik, source.ik) && BytesEqual(selectors, source.selectors));
        CHECK(BytesEqual(pmx, source.pmx) && links == source.links);
    }
};
bool Matches(const BoneDefinition& a, const mdl::BoneRecord& b, uint32_t pmxCount) {
    return !std::memcmp(a.name.data(), b.name, 20) && !std::memcmp(a.nameEn.data(), b.nameEn, 20) &&
        a.parent == b.parent && SameFloats(a.position, b.position) && a.type == b.type && a.flags == b.flags &&
        a.layer == b.layer && a.hasFlag == b.hasFlag && a.hasRigidBody == b.hasRigidBody && a.tailBone == b.tailBone &&
        a.tailIdx == b.tailIdx && SameFloats(a.tailOffset, b.tailOffset) &&
        !std::memcmp(&a.inheritRatio, &b.inheritRatio, sizeof(float)) && SameFloats(a.axis, b.axis) && SameFloats(a.localAxes, b.localAxes) &&
        a.extParent == b.extParent && a.slotIndex == b.slotIndex && a.twistEnable == b.twistEnable &&
        SameFloats(a.ikLimitMin, b.ikLimitMin) && SameFloats(a.ikLimitMax, b.ikLimitMax) &&
        a.ikTarget == b.ikTarget && a.ikLoop == b.ikLoop && !std::memcmp(&a.ikAngle, &b.ikAngle, sizeof(float)) && a.pmxLinkCount == pmxCount;
}
void FieldsAndIndependence() {
    Model model; SourceState before(model); auto frozen = Frozen::Capture(model.raw()); before.Check(model);
    CHECK(frozen->BoneCount() == 4 && frozen->IkCount() == 2 && frozen->SelectorCount() == 3);
    CHECK(frozen->PhysicsMode() == 2 && frozen->MaxBoneLayer() == INT32_MIN && frozen->RetainedBytes() > 0);
    for (uint32_t i = 0; i < 4; ++i) { BoneDefinition bone; CHECK(frozen->BoneAt(i, bone) && Matches(bone, model.bones[i], i == 0 ? 3 : 0)); }
    for (uint32_t i = 0; i < 3; ++i) {
        BoneDefinitionIkLink link; CHECK(frozen->PmxLinkAt(0, i, link));
        CHECK(link.bone == model.pmx[i].boneIndex && link.hasLimits == model.pmx[i].hasLimits &&
            SameFloats(link.minimum, model.pmx[i].minimum) && SameFloats(link.maximum, model.pmx[i].maximum));
        int32_t selector = 99; CHECK(frozen->SelectorBone(i, selector) && selector == model.selectors[i].boneIndex);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        BoneDefinitionIk chain; CHECK(frozen->IkAt(i, chain));
        CHECK(chain.bone == model.ik[i].boneIndex && chain.target == model.ik[i].targetBone && chain.linkCount == model.ik[i].linkCount &&
            chain.iterations == model.ik[i].iterations && chain.maxAngle == model.ik[i].maxAngle);
    }
    for (uint32_t i = 0; i < 3; ++i) { uint32_t link = 99; CHECK(frozen->IkLinkAt(0, i, link) && link == model.links[i]); }
    // Pose, chain enabled byte and live parent selection are not static definitions.
    model.bones[0].trans[0] = 9000; model.bones[0].rotQuat[3] = -.5f; model.bones[0].physicsDisabled = 0;
    model.bones[0].selState = -12; model.bones[0].matWorld[0] = -123; model.ik[0].enabled = 0;
    model.selectors[1].linkedModel = -2; model.selectors[1].windowStart = 12;
    auto poseOnly = Frozen::Capture(model.raw()); BoneDefinition a, b; BoneDefinitionIk ia, ib;
    CHECK(frozen->BoneAt(0, a) && poseOnly->BoneAt(0, b) && Matches(b, before.bones[0], 3));
    CHECK(frozen->IkAt(0, ia) && poseOnly->IkAt(0, ib) && ia.bone == ib.bone && ia.target == ib.target && ia.maxAngle == ib.maxAngle);
    model.bones[0].position[0] = -100; model.bones[0].name[0] = 'z'; model.pmx[0].boneIndex = 3; model.links[0] = 1;
    auto changed = Frozen::Capture(model.raw()); BoneDefinitionIkLink oldLink, newLink; uint32_t oldRuntime = 99, newRuntime = 99;
    CHECK(frozen->BoneAt(0, a) && changed->BoneAt(0, b) && a.position[0] == .25f && b.position[0] == -100 && a.name[0] != b.name[0]);
    CHECK(frozen->PmxLinkAt(0, 0, oldLink) && changed->PmxLinkAt(0, 0, newLink) && oldLink.bone == 0 && newLink.bone == 3);
    CHECK(frozen->IkLinkAt(0, 0, oldRuntime) && changed->IkLinkAt(0, 0, newRuntime) && oldRuntime == 2 && newRuntime == 1);
}
void InvalidIndicesAndEmpty() {
    Model model; auto frozen = Frozen::Capture(model.raw());
    for (uint32_t index : {4u, UINT32_MAX}) { auto out = Poison<BoneDefinition>(); CHECK(!frozen->BoneAt(index, out) && Unchanged(out)); }
    for (const auto pair : std::array<std::array<uint32_t, 2>, 4>{{{0, 3}, {1, 0}, {4, 0}, {UINT32_MAX, UINT32_MAX}}}) {
        auto out = Poison<BoneDefinitionIkLink>(); CHECK(!frozen->PmxLinkAt(pair[0], pair[1], out) && Unchanged(out));
    }
    for (uint32_t index : {2u, UINT32_MAX}) { auto out = Poison<BoneDefinitionIk>(); CHECK(!frozen->IkAt(index, out) && Unchanged(out)); }
    for (const auto pair : std::array<std::array<uint32_t, 2>, 4>{{{0, 3}, {1, 0}, {2, 0}, {UINT32_MAX, UINT32_MAX}}}) {
        uint32_t out = 0xa5a5a5a5; CHECK(!frozen->IkLinkAt(pair[0], pair[1], out) && out == 0xa5a5a5a5);
    }
    for (uint32_t index : {3u, UINT32_MAX}) { int32_t out = -987; CHECK(!frozen->SelectorBone(index, out) && out == -987); }
    auto empty = std::make_unique<mdl::ModelRecord>();
    auto zero = Frozen::Capture(reinterpret_cast<unsigned char*>(empty.get()));
    CHECK(zero->BoneCount() == 0 && zero->IkCount() == 0 && zero->SelectorCount() == 0);
    auto bone = Poison<BoneDefinition>(); CHECK(!zero->BoneAt(0, bone) && Unchanged(bone));
    CHECK(Throws<std::invalid_argument>([] { Frozen::Capture(nullptr); }));
}
template<class F> void Invalid(F&& edit) {
    Model model; edit(model); SourceState before(model);
    CHECK(Throws<std::invalid_argument>([&] { Frozen::Capture(model.raw()); })); before.Check(model);
}
void Validation() {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    Invalid([](Model& m) { m.record->boneTable = nullptr; });
    Invalid([](Model& m) { m.record->ikChains = nullptr; });
    Invalid([](Model& m) { m.record->boneOrderTable = nullptr; });
    Invalid([](Model& m) { m.record->boneCount = UINT32_MAX; });
    Invalid([](Model& m) { m.record->ikChainCount = UINT32_MAX; });
    Invalid([](Model& m) { m.record->boneOrderCount = UINT32_MAX; });
    Invalid([](Model& m) { m.bones[0].parent = -2; });
    Invalid([](Model& m) { m.bones[0].parent = 4; });
    Invalid([](Model& m) { m.bones[0].parent = 0; });
    Invalid([](Model& m) { m.bones[2].parent = 3; });
    Invalid([](Model& m) { m.bones[0].type = static_cast<mdl::BoneType>(10); });
    Invalid([&](Model& m) { m.bones[0].position[1] = nan; });
    Invalid([&](Model& m) { m.bones[0].inheritRatio = inf; });
    Invalid([](Model& m) { m.bones[0].tailBone = 4; });
    Invalid([](Model& m) { m.bones[0].tailBone = -2; });
    Invalid([](Model& m) { m.bones[1].tailIdx = -1; });
    Invalid([](Model& m) { m.bones[0].tailIdx = 4; });
    Invalid([](Model& m) { m.bones[2].tailBone = -1; });
    Invalid([&](Model& m) { m.bones[1].tailOffset[0] = nan; });
    Invalid([&](Model& m) { m.bones[0].axis[1] = nan; });
    Invalid([&](Model& m) { m.bones[3].axis[0] = inf; });
    Invalid([&](Model& m) { m.bones[0].localAxes[5] = nan; });
    Invalid([&](Model& m) { m.bones[0].ikLimitMin[2] = nan; });
    Invalid([&](Model& m) { m.bones[0].ikLimitMax[0] = inf; });
    Invalid([](Model& m) { m.bones[0].ikLimitMin[0] = m.bones[0].ikLimitMax[0] + 1; });
    Invalid([](Model& m) { m.bones[0].slotIndex = -2; });
    Invalid([](Model& m) { m.bones[0].slotIndex = 3; });
    Invalid([](Model& m) { m.bones[0].slotIndex = 2; });
    Invalid([](Model& m) { m.selectors[0].boneIndex = 0; });
    Invalid([](Model& m) { m.selectors[1].boneIndex = -1; });
    Invalid([](Model& m) { m.selectors[2].boneIndex = 4; });
    Invalid([](Model& m) { m.record->boneOrderCount = 0; m.bones[0].slotIndex = m.bones[2].slotIndex = -1; });
    Invalid([](Model& m) { m.bones[0].ikLinkCount = -1; });
    Invalid([](Model& m) { m.bones[0].ikLinks = nullptr; });
    Invalid([](Model& m) { m.bones[0].ikTarget = -1; });
    Invalid([](Model& m) { m.bones[0].ikTarget = 4; });
    Invalid([](Model& m) { m.bones[0].ikLoop = -1; });
    Invalid([&](Model& m) { m.bones[0].ikAngle = nan; });
    Invalid([](Model& m) { m.pmx[0].boneIndex = -1; });
    Invalid([](Model& m) { m.pmx[0].boneIndex = 4; });
    Invalid([&](Model& m) { m.pmx[0].minimum[1] = nan; });
    Invalid([&](Model& m) { m.pmx[0].maximum[0] = inf; });
    Invalid([](Model& m) { m.pmx[0].minimum[1] = 100; });
    Invalid([](Model& m) { m.ik[0].boneIndex = -1; });
    Invalid([](Model& m) { m.ik[0].boneIndex = 4; });
    Invalid([](Model& m) { m.ik[0].targetBone = -1; });
    Invalid([](Model& m) { m.ik[0].targetBone = 4; });
    Invalid([&](Model& m) { m.ik[0].maxAngle = inf; });
    Invalid([](Model& m) { m.ik[0].links = nullptr; });
    Invalid([](Model& m) { m.links[1] = UINT16_MAX; });
    for (unsigned type = 0; type <= 9; ++type) {
        Model model; model.bones[3].type = static_cast<mdl::BoneType>(type); model.bones[3].tailIdx = 0; model.bones[3].tailBone = 0;
        auto frozen = Frozen::Capture(model.raw()); BoneDefinition out;
        CHECK(frozen->BoneAt(3, out) && unsigned(out.type) == type);
    }
    Model allowed; allowed.bones[0].tailBone = -1; allowed.bones[0].ikLoop = 0;
    allowed.bones[0].ikLinkCount = 0; allowed.bones[0].ikLinks = nullptr;
    auto frozen = Frozen::Capture(allowed.raw()); BoneDefinition out;
    CHECK(frozen->BoneAt(0, out) && out.tailBone == -1 && out.ikLoop == 0 && out.pmxLinkCount == 0);
    for (int32_t percentage : {INT32_MIN, INT32_MAX}) {
        allowed.bones[2].tailIdx = percentage; auto copy = Frozen::Capture(allowed.raw());
        CHECK(copy->BoneAt(2, out) && out.tailIdx == percentage && out.tailBone == 1);
    }
}
void DisabledFieldsAndPhysicsModes() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    Model model;
    model.bones[0].tailOffset[0] = nan; // Tail index is active instead of offset.
    model.bones[1].axis[0] = nan; model.bones[1].localAxes[1] = nan; model.bones[1].ikLimitMin[1] = nan;
    model.pmx[1].minimum[0] = nan; model.pmx[1].maximum[1] = -999;
    model.pmx[2].minimum[2] = nan; // hasLimits=197 is raw data, not enabled==1.
    SourceState before(model); auto frozen = Frozen::Capture(model.raw()); before.Check(model);
    for (uint32_t i : {0u, 1u}) { BoneDefinition out; CHECK(frozen->BoneAt(i, out) && Matches(out, model.bones[i], i == 0 ? 3 : 0)); }
    for (uint32_t i : {1u, 2u}) {
        BoneDefinitionIkLink out; CHECK(frozen->PmxLinkAt(0, i, out) && SameFloats(out.minimum, model.pmx[i].minimum) && out.hasLimits == model.pmx[i].hasLimits);
    }
    // PMD/other modes never follow the PMX-only nested pointer, even if the
    // otherwise unused PMX count/target fields contain non-PMX sentinels.
    for (uint8_t mode : {uint8_t(0), uint8_t(1), uint8_t(255)}) {
        Model source; source.record->physicsMode = mode; source.bones[0].ikLinkCount = -9;
        source.bones[0].ikLinks = reinterpret_cast<mdl::PmxIkLinkRecord*>(uintptr_t(1));
        source.bones[0].ikTarget = -99; source.bones[0].ikLoop = -123; source.bones[0].ikAngle = nan;
        // PMD has no active offset-tail field: this RotateGrant bone has no
        // tail-index flag, but its unused raw offset bytes must still survive.
        // Validation() separately rejects the same NaN in active PMX mode 2.
        if (mode == 0 || mode == 1) source.bones[1].tailOffset[0] = nan;
        SourceState modeBefore(source); std::unique_ptr<Frozen> copy; bool rejected = false;
        try { copy = Frozen::Capture(source.raw()); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(!rejected && copy); BoneDefinition out;
        modeBefore.Check(source);
        CHECK(copy->PhysicsMode() == mode && copy->BoneAt(0, out) && Matches(out, source.bones[0], 0));
        CHECK(copy->BoneAt(1, out) && SameFloats(out.tailOffset, source.bones[1].tailOffset));
        CHECK(copy->IkCount() == 2);
    }
    Model noFlag; noFlag.bones[0].flags &= ~mdl::kBoneFlagIk;
    noFlag.bones[0].ikLinkCount = -1; noFlag.bones[0].ikLinks = reinterpret_cast<mdl::PmxIkLinkRecord*>(uintptr_t(1));
    auto ignored = Frozen::Capture(noFlag.raw()); BoneDefinition out;
    CHECK(ignored->BoneAt(0, out) && out.pmxLinkCount == 0);
}
void BudgetsAndAllocationFailures() {
    Model source; SourceState before(source); auto initial = Frozen::Capture(source.raw()); const auto bytes = initial->RetainedBytes();
    CHECK(bytes >= 4 * sizeof(BoneDefinition) + 2 * sizeof(BoneDefinitionIk) + 3 * sizeof(BoneDefinitionIkLink) + 6 * sizeof(uint32_t));
    CHECK(bytes > 1); auto exact = Frozen::Capture(source.raw(), bytes); CHECK(exact->RetainedBytes() == bytes);
    CHECK(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), bytes - 1); })); before.Check(source);
    CHECK(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), 0); }));
    // These counts cannot fit the requested budget. The valid short backing
    // arrays intentionally make walking the advertised extent before rejection invalid.
    for (unsigned mode = 0; mode < 4; ++mode) {
        Model large;
        if (mode == 0) large.record->boneCount = 1000000;
        if (mode == 1) large.record->ikChainCount = 1000000;
        if (mode == 2) large.record->boneOrderCount = 1000000;
        if (mode == 3) large.bones[0].ikLinkCount = INT32_MAX;
        CHECK(Throws<std::length_error>([&] { Frozen::Capture(large.raw(), 16384); }));
    }
    // Link storage must actually be charged, not just the outer bone vector.
    Model shortLinks; shortLinks.bones[0].ikLinkCount = 0; shortLinks.bones[0].ikLinks = nullptr;
    shortLinks.ik[0].linkCount = 0; shortLinks.ik[0].links = nullptr;
    auto small = Frozen::Capture(shortLinks.raw()); CHECK(bytes > small->RetainedBytes());
    bool completed = false; unsigned failures = 0;
    for (int limit = 0; limit < 512; ++limit) {
        std::unique_ptr<Frozen> copy; bool allocationFailed = false, unexpected = false;
        failAfter = limit;
        try { copy = Frozen::Capture(source.raw()); } catch (const std::bad_alloc&) { allocationFailed = true; } catch (...) { unexpected = true; }
        failAfter = -1; CHECK(!unexpected); before.Check(source);
        if (copy) { CHECK(copy->RetainedBytes() == bytes && copy->BoneCount() == 4); completed = true; break; }
        CHECK(allocationFailed); ++failures;
    }
    CHECK(completed && failures > 0);
}
void LongForwardChain() {
    auto record = std::make_unique<mdl::ModelRecord>();
    constexpr uint32_t count = 50000;
    std::vector<mdl::BoneRecord> bones(count); mdl::BoneOrderEntry global{}; global.boneIndex = -1;
    record->boneTable = bones.data(); record->boneCount = count; record->boneOrderTable = &global; record->boneOrderCount = 1;
    record->maxBoneLayer = INT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        bones[i].parent = i + 1 < count ? int32_t(i + 1) : -1;
        bones[i].slotIndex = -1; bones[i].tailBone = -1; bones[i].tailIdx = -1;
        bones[i].layer = i % 2 ? INT32_MAX : INT32_MIN;
    }
    const auto raw = reinterpret_cast<unsigned char*>(record.get()); auto frozen = Frozen::Capture(raw);
    BoneDefinition first, last;
    CHECK(frozen->BoneCount() == count && frozen->MaxBoneLayer() == INT32_MAX && frozen->BoneAt(0, first) && frozen->BoneAt(count - 1, last));
    CHECK(first.parent == 1 && last.parent == -1 && first.layer == INT32_MIN && last.layer == INT32_MAX);
    bones.back().parent = 0;
    CHECK(Throws<std::invalid_argument>([&] { Frozen::Capture(raw); }));
}
void LifetimeAndConcurrentNoAllocation() {
    std::unique_ptr<Frozen> frozen;
    { Model source; frozen = Frozen::Capture(source.raw()); }
    std::atomic<unsigned> failures{0}; std::array<std::thread, 4> workers;
    try {
        for (auto& worker : workers) worker = std::thread([&] {
            failAfter = 0;
            for (unsigned iteration = 0; iteration < 1000; ++iteration) {
                BoneDefinition bone; BoneDefinitionIkLink link; BoneDefinitionIk chain; uint32_t index = 99; int32_t selector = 99;
                if (!frozen->BoneAt(0, bone) || bone.position[0] != .25f || bone.name[0] != char(128) || bone.name[6] != 0 || bone.name[19] != char(147) ||
                    !frozen->PmxLinkAt(0, 2, link) || link.bone != 1 || link.hasLimits != 197 ||
                    !frozen->IkAt(0, chain) || chain.bone != 2 || chain.target != 0 || chain.iterations != 65535 ||
                    !frozen->IkLinkAt(0, 0, index) || index != 2 || !frozen->SelectorBone(2, selector) || selector != 2 ||
                    frozen->BoneCount() != 4 || frozen->IkCount() != 2 || frozen->SelectorCount() != 3 || frozen->PhysicsMode() != 2 ||
                    frozen->MaxBoneLayer() != INT32_MIN || !frozen->RetainedBytes()) ++failures;
            }
            failAfter = -1;
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (auto& worker : workers) worker.join(); CHECK(failures == 0);
}
}
int main() {
    FieldsAndIndependence(); InvalidIndicesAndEmpty(); Validation(); DisabledFieldsAndPhysicsModes();
    BudgetsAndAllocationFailures(); LongForwardChain(); LifetimeAndConcurrentNoAllocation();
    std::printf("PASS %u bone definition snapshot checks\n", checks); return 0;
}
