// Shared private CPU math context versus the native five-phase bone entry.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/bone_transform_context.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
const char* currentCase = "setup";
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %s line %d: %s\n", currentCase, __LINE__, #x); std::exit(1); } } while (false)
template<class T> bool Same(const T& a, const T& b) { return !std::memcmp(&a, &b, sizeof(T)); }
void Identity(float (&matrix)[16]) { std::memset(matrix, 0, sizeof(matrix)); matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1; }
using Bones = std::array<mdl::BoneRecord, 10>;
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    Bones bones{};
    mdl::IkChain chain{};
    std::array<uint16_t, 3> links{2, 1, 1};
    std::array<mdl::BoneOrderEntry, 2> selectors{};
    std::unique_ptr<mdl::ModelRecord> external = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 2> externalBones{};
    std::array<unsigned char*, 2> slots{};
    Fixture() {
        model->physicsMode = 2; model->boneCount = uint32_t(bones.size()); model->boneTable = bones.data();
        model->ikChains = &chain; model->ikChainCount = 1; model->boneOrderTable = selectors.data(); model->boneOrderCount = 2;
        model->maxBoneLayer = 1; model->searchCursor = 771; model->displayKeyCursor = 923;
        external->boneCount = 2; external->boneTable = externalBones.data();
        slots[0] = slots[1] = reinterpret_cast<unsigned char*>(external.get());
        selectors[0].boneIndex = -1; selectors[1].boneIndex = 9;
        for (auto& selector : selectors) { selector.linkedModel = -1; selector.linkedBone = 0; }
        for (unsigned i = 0; i < bones.size(); ++i) {
            auto& bone = bones[i]; bone.parent = i ? 0 : -1; bone.slotIndex = -1;
            bone.tailBone = bone.tailIdx = -1; bone.type = mdl::BoneType::RotateMove;
            bone.physicsQuat[3] = bone.rotQuat[3] = bone.rotQuat2[3] = 1;
            Identity(bone.matInit); Identity(bone.matLocal); Identity(bone.matWorld); Identity(bone.matExtra);
            bone.matExtra[12] = -91; bone.matExtra[13] = -92; bone.matExtra[14] = -93;
            std::snprintf(bone.name, 20, "bone-%u", i);
            bone.trans[0] = float(1000 + i); bone.selState = 300 + i; // Not solver inputs.
        }
        bones[0].physicsOffset[0] = .5f; bones[0].physicsOffset[1] = -.25f; bones[0].physicsOffset[2] = .125f;
        bones[1].position[1] = 1; bones[1].type = mdl::BoneType::UnderIk;
        bones[2].parent = 1; bones[2].position[1] = 2; bones[2].type = mdl::BoneType::UnderIk;
        bones[3].parent = 2; bones[3].position[1] = 3; bones[3].type = mdl::BoneType::Effector;
        bones[4].type = mdl::BoneType::Ik; bones[4].position[0] = 1; bones[4].position[1] = 2; bones[4].position[2] = 1;
        bones[4].physicsOffset[0] = .125f; bones[4].physicsOffset[1] = .25f; bones[4].physicsOffset[2] = -.5f;
        chain.boneIndex = 4; chain.targetBone = 3; chain.linkCount = 2; chain.links = links.data();
        chain.iterations = 8; chain.enabled = 197; chain.maxAngle = .35f;
        for (unsigned i = 5; i < bones.size(); ++i) {
            bones[i].layer = 1; bones[i].position[0] = float(i - 3); bones[i].position[1] = float(i % 3);
            bones[i].physicsOffset[0] = .125f; bones[i].physicsOffset[2] = -.25f;
        }
        bones[5].type = mdl::BoneType::RotateGrant; bones[5].tailIdx = 1; bones[5].hasFlag = 1;
        bones[6].type = mdl::BoneType::CoRotate; bones[6].tailBone = 2; bones[6].tailIdx = 75; bones[6].hasFlag = 1;
        bones[7].hasFlag = 1; bones[7].flags = mdl::kBoneFlagInheritMask | mdl::kBoneFlagAfterPhysics;
        bones[7].tailIdx = 1; bones[7].inheritRatio = -.375f;
        bones[8].flags = mdl::kBoneFlagInheritMask; bones[8].tailIdx = 2; bones[8].inheritRatio = .625f;
        bones[9].parent = -1; bones[9].type = mdl::BoneType::RotateGrant; bones[9].tailIdx = 1; bones[9].hasFlag = 1;
        for (auto& bone : externalBones) { Identity(bone.matExtra); bone.position[0] = 2; bone.position[1] = -1; bone.position[2] = 3; }
        externalBones[0].matExtra[0] = externalBones[0].matExtra[5] = 0;
        externalBones[0].matExtra[1] = 1; externalBones[0].matExtra[4] = -1;
        externalBones[0].matExtra[12] = 10; externalBones[0].matExtra[13] = 20; externalBones[0].matExtra[14] = 30;
        externalBones[1].matExtra[12] = -7; externalBones[1].matExtra[13] = 11; externalBones[1].matExtra[14] = 13;
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(model.get()); }
};
struct Step { unsigned char after; int layer; };
constexpr Step steps[]{{0, 0}, {0, 1}, {1, 0}, {1, 1}};
bool SameMutable(const mdl::BoneRecord& a, const mdl::BoneRecord& b) {
    return !std::memcmp(a.matLocal, b.matLocal, sizeof(a.matLocal)) && !std::memcmp(a.matInit, b.matInit, sizeof(a.matInit)) &&
        !std::memcmp(a.matWorld, b.matWorld, sizeof(a.matWorld)) && !std::memcmp(a.matExtra, b.matExtra, sizeof(a.matExtra)) &&
        !std::memcmp(a.physicsOffset, b.physicsOffset, sizeof(a.physicsOffset)) && !std::memcmp(a.physicsQuat, b.physicsQuat, sizeof(a.physicsQuat)) &&
        !std::memcmp(a.rotQuat2, b.rotQuat2, sizeof(a.rotQuat2));
}
bool SameMutable(const Bones& a, const Bones& b) {
    for (unsigned i = 0; i < a.size(); ++i)
        if (!SameMutable(a[i], b[i])) return false;
    return true;
}
bool OtherFieldsUnchanged(const mdl::BoneRecord& before, const mdl::BoneRecord& after) {
    auto allowed = before;
    std::memcpy(allowed.matLocal, after.matLocal, sizeof(after.matLocal)); std::memcpy(allowed.matInit, after.matInit, sizeof(after.matInit));
    std::memcpy(allowed.matWorld, after.matWorld, sizeof(after.matWorld)); std::memcpy(allowed.matExtra, after.matExtra, sizeof(after.matExtra));
    std::memcpy(allowed.physicsOffset, after.physicsOffset, sizeof(after.physicsOffset)); std::memcpy(allowed.physicsQuat, after.physicsQuat, sizeof(after.physicsQuat));
    std::memcpy(allowed.rotQuat2, after.rotQuat2, sizeof(after.rotQuat2)); return Same(allowed, after);
}
struct ReadOnlyState {
    std::unique_ptr<mdl::ModelRecord> model, external;
    decltype(Fixture::externalBones) externalBones;
    decltype(Fixture::selectors) selectors;
    decltype(Fixture::links) links;
    mdl::IkChain chain;
    Bones bones;
    explicit ReadOnlyState(const Fixture& f) : model(std::make_unique<mdl::ModelRecord>(*f.model)), external(std::make_unique<mdl::ModelRecord>(*f.external)),
        externalBones(f.externalBones), selectors(f.selectors), links(f.links), chain(f.chain), bones(f.bones) {}
    void Check(const Fixture& f) const {
        CHECK(Same(*model, *f.model) && Same(*external, *f.external));
        CHECK(Same(externalBones, f.externalBones) && Same(selectors, f.selectors) && Same(links, f.links) && Same(chain, f.chain));
        for (unsigned i = 0; i < bones.size(); ++i) CHECK(OtherFieldsUnchanged(bones[i], f.bones[i]));
    }
};
void RunLegacy(Fixture& f, int physicsMode = 0) {
    for (const auto step : steps) BoneFrameTransform(f.raw(), step.after, step.layer, f.slots.data(), physicsMode);
}
void RunPrivate(Fixture& f, const BoneTransformContext& context, int physicsMode = 0) {
    for (const auto step : steps) BoneFrameTransformPrivate(*f.model, step.after, step.layer, f.slots.data(), physicsMode, context);
}
template<class Configure> Bones Pair(const BoneTransformContext& context, const char* label, Configure&& configure, int physicsMode = 0, bool expectMotion = true) {
    currentCase = label; Fixture legacy, owned, disabled; configure(legacy); configure(owned); configure(disabled); disabled.chain.enabled = 0;
    const ReadOnlyState legacyBefore(legacy), ownedBefore(owned);
    for (const auto step : steps) {
        BoneFrameTransform(legacy.raw(), step.after, step.layer, legacy.slots.data(), physicsMode);
        BoneFrameTransformPrivate(*owned.model, step.after, step.layer, owned.slots.data(), physicsMode, context);
        for (unsigned i = 0; i < owned.bones.size(); ++i) CHECK(SameMutable(legacy.bones[i], owned.bones[i]));
        legacyBefore.Check(legacy); ownedBefore.Check(owned);
    }
    RunLegacy(disabled, physicsMode);
    const bool moved = !SameMutable(owned.bones[1], disabled.bones[1]) || !SameMutable(owned.bones[2], disabled.bones[2]) ||
        std::memcmp(owned.bones[3].matInit, disabled.bones[3].matInit, sizeof(owned.bones[3].matInit)) != 0;
    CHECK(moved == expectMotion);
    return owned.bones;
}
void ConfigureLimit(Fixture& f, unsigned primary) {
    auto& bone = f.bones[2]; bone.twistEnable = 197;
    bone.ikLimitMin[0] = primary == 0 ? -.04f : -2.f; bone.ikLimitMax[0] = primary == 0 ? .04f : 2.f;
    bone.ikLimitMin[1] = primary <= 1 ? -.03f : -2.f; bone.ikLimitMax[1] = primary <= 1 ? .03f : 2.f;
    bone.ikLimitMin[2] = -.02f; bone.ikLimitMax[2] = .02f;
}
void NativeParity(const BoneTransformContext& context) {
    const auto plain = Pair(context, "PMX five phases", [](Fixture&) {});
    for (unsigned primary = 0; primary < 3; ++primary) {
        const auto limited = Pair(context, "PMX active limit plane", [=](Fixture& f) { ConfigureLimit(f, primary); });
        CHECK(!SameMutable(limited[2], plain[2])); // Limits actually affect the result.
    }
    Pair(context, "PMX repeated links", [](Fixture& f) { f.chain.linkCount = 3; });
    Pair(context, "CCD after physics channel", [](Fixture& f) { f.bones[4].flags |= mdl::kBoneFlagAfterPhysics; });
    Pair(context, "CCD one iteration", [](Fixture& f) { f.chain.iterations = 1; });
    Pair(context, "CCD disabled", [](Fixture& f) { f.chain.enabled = 0; }, 0, false);
    Pair(context, "CCD zero iterations", [](Fixture& f) { f.chain.iterations = 0; }, 0, false);
    const auto generic = Pair(context, "PMD ordinary link", [](Fixture& f) { f.model->physicsMode = 0; });
    const auto left = Pair(context, "PMD left knee", [](Fixture& f) {
        f.model->physicsMode = 0; std::memcpy(f.bones[2].name, "\x8d\xb6\x82\xd0\x82\xb4", 6); f.bones[2].name[6] = 'X';
    });
    const auto right = Pair(context, "PMD right knee", [](Fixture& f) {
        f.model->physicsMode = 0; std::memcpy(f.bones[2].name, "\x89\x45\x82\xd0\x82\xb4", 6); f.bones[2].name[6] = 'Y';
    });
    CHECK(!SameMutable(left[2], generic[2]) && SameMutable(left, right));
    Pair(context, "external global and root selector", [](Fixture& f) {
        f.selectors[0].linkedModel = 0; f.bones[9].slotIndex = 1;
        f.selectors[1].linkedModel = 1; f.selectors[1].linkedBone = 1;
    });
    Pair(context, "selector minus one global fallback", [](Fixture& f) { f.selectors[0].linkedModel = 0; f.bones[9].slotIndex = 1; });
}
void PhysicsGates(const BoneTransformContext& context) {
    struct Case { int mode; uint8_t rigid, disabled; bool motion, copies; };
    const Case cases[]{{0,1,0,true,true}, {1,1,1,false,false}, {2,1,0,false,false}, {2,1,1,true,true}, {3,1,1,true,true}, {3,2,0,true,true}};
    for (const auto item : cases) {
        const auto result = Pair(context, "physics and copy gates", [=](Fixture& f) {
            f.bones[2].hasRigidBody = f.bones[8].hasRigidBody = item.rigid;
            f.bones[2].physicsDisabled = f.bones[8].physicsDisabled = item.disabled;
        }, item.mode, item.motion);
        if (item.copies) CHECK(!std::memcmp(result[8].matExtra, result[8].matInit, sizeof(result[8].matInit)));
        else CHECK(result[8].matExtra[12] == -91 && result[8].matExtra[13] == -92 && result[8].matExtra[14] == -93);
    }
}

// Harmless forwarding sentinels make accidental global calls observable
// without relying on a crash from an invalid function address.
std::atomic<unsigned> globalCalls{0};
d3dx::Api forwarded;
d3dx::D3DXMATRIXF* WINAPI GlobalMultiply(d3dx::D3DXMATRIXF* out, const d3dx::D3DXMATRIXF* a, const d3dx::D3DXMATRIXF* b) {
    ++globalCalls; return forwarded.multiply(out, a, b);
}
d3dx::D3DXMATRIXF* WINAPI GlobalRotX(d3dx::D3DXMATRIXF* out, float angle) { ++globalCalls; return forwarded.rotX(out, angle); }
d3dx::D3DXMATRIXF* WINAPI GlobalRotY(d3dx::D3DXMATRIXF* out, float angle) { ++globalCalls; return forwarded.rotY(out, angle); }
d3dx::D3DXMATRIXF* WINAPI GlobalRotZ(d3dx::D3DXMATRIXF* out, float angle) { ++globalCalls; return forwarded.rotZ(out, angle); }
float* WINAPI GlobalQuat(float* out, const d3dx::D3DXMATRIXF* matrix) { ++globalCalls; return forwarded.quatFromMatrix(out, matrix); }
float* WINAPI GlobalNormalize(float* out, const float* input) { ++globalCalls; return forwarded.vec3Normalize(out, input); }
float* WINAPI GlobalQuatMultiply(float* out, const float* a, const float* b) { ++globalCalls; return forwarded.quatMultiply(out, a, b); }
d3dx::D3DXMATRIXF* WINAPI GlobalRotation(d3dx::D3DXMATRIXF* out, const float* q) { ++globalCalls; return forwarded.matrixRotationQuaternion(out, q); }
struct GlobalApiGuard {
    d3dx::Api& api = d3dx::Get();
    std::array<unsigned char, sizeof(d3dx::Api)> saved{}, poisoned{};
    GlobalApiGuard() {
        static_assert(std::is_trivially_copyable<d3dx::Api>::value, "API test saves raw fields");
        std::memcpy(saved.data(), &api, sizeof(api)); std::memcpy(&forwarded, &api, sizeof(api)); globalCalls = 0;
        api.module = reinterpret_cast<HMODULE>(uintptr_t(1));
        api.rotX = GlobalRotX; api.rotY = GlobalRotY; api.rotZ = GlobalRotZ; api.multiply = GlobalMultiply;
        api.quatFromMatrix = GlobalQuat; api.vec3Normalize = GlobalNormalize; api.quatMultiply = GlobalQuatMultiply; api.matrixRotationQuaternion = GlobalRotation;
        std::memcpy(poisoned.data(), &api, sizeof(api));
    }
    bool Untouched() const { return globalCalls == 0 && !std::memcmp(poisoned.data(), &api, sizeof(api)); }
    ~GlobalApiGuard() { std::memcpy(&api, saved.data(), sizeof(api)); }
};
void GlobalApiIsolation(const std::shared_ptr<const BoneTransformContext>& context) {
    currentCase = "global API isolation";
    Fixture expected, actual, createdAfterPoison;
    ConfigureLimit(expected, 1); ConfigureLimit(actual, 1); ConfigureLimit(createdAfterPoison, 1);
    RunLegacy(expected); const ReadOnlyState before(actual), otherBefore(createdAfterPoison);
    bool untouched = false, sameExisting = false, sameNew = false, created = false;
    {
        GlobalApiGuard guard;
        RunPrivate(actual, *context); sameExisting = SameMutable(actual.bones, expected.bones);
        const auto independent = BoneTransformContext::Create(); created = bool(independent);
        if (independent) { RunPrivate(createdAfterPoison, *independent); sameNew = SameMutable(createdAfterPoison.bones, expected.bones); }
        untouched = guard.Untouched();
    }
    CHECK(untouched && created && sameExisting && sameNew);
    before.Check(actual); otherBefore.Check(createdAfterPoison);
}
void ConcurrentWorkspaces(const std::shared_ptr<const BoneTransformContext>& context) {
    currentCase = "shared context concurrent workspaces";
    Fixture sharedExternal;
    const auto externalBefore = sharedExternal.externalBones;
    const auto externalModelBefore = std::make_unique<mdl::ModelRecord>(*sharedExternal.external);
    std::array<Bones, 4> expected;
    for (unsigned worker = 0; worker < expected.size(); ++worker) {
        Fixture fixture; ConfigureLimit(fixture, worker % 3);
        fixture.bones[4].physicsOffset[0] += float(worker) * .125f;
        fixture.selectors[0].linkedModel = 0; fixture.slots[0] = reinterpret_cast<unsigned char*>(sharedExternal.external.get());
        RunLegacy(fixture); expected[worker] = fixture.bones;
    }
    std::atomic<unsigned> errors{0}; std::array<std::thread, 4> workers;
    try {
        for (unsigned worker = 0; worker < workers.size(); ++worker) workers[worker] = std::thread([&, worker, lease = context] {
            try {
                for (unsigned repeat = 0; repeat < 50; ++repeat) {
                    Fixture fixture; ConfigureLimit(fixture, worker % 3);
                    fixture.bones[4].physicsOffset[0] += float(worker) * .125f;
                    fixture.selectors[0].linkedModel = 0; fixture.slots[0] = reinterpret_cast<unsigned char*>(sharedExternal.external.get());
                    RunPrivate(fixture, *lease);
                    if (!SameMutable(fixture.bones, expected[worker])) ++errors;
                }
            } catch (...) { ++errors; }
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (auto& worker : workers) worker.join();
    CHECK(errors == 0 && Same(sharedExternal.externalBones, externalBefore) && Same(*sharedExternal.external, *externalModelBefore));
}
}
int main() {
    try {
        const auto context = BoneTransformContext::Create(); CHECK(bool(context));
        NativeParity(*context); PhysicsGates(*context); ConcurrentWorkspaces(context);
        // No legacy calibration or worker may run while the global table is poisoned.
        GlobalApiIsolation(context);
        std::printf("PASS %u bone transform context checks\n", checks); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s unexpected exception: %s\n", currentCase, error.what()); return 1;
    }
}
