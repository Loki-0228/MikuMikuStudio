// Native phase-E root inheritance must not use boneTable[-1] as a parent.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace mikumikustudio;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
using Matrix = std::array<float, 16>;
Matrix Identity() { Matrix value{}; value[0] = value[5] = value[10] = value[15] = 1; return value; }
Matrix Translation(float x, float y, float z) { auto value = Identity(); value[12] = x; value[13] = y; value[14] = z; return value; }
void Put(float (&out)[16], const Matrix& in) { std::memcpy(out, in.data(), sizeof(out)); }
bool Near(const float* actual, const float* expected, unsigned count, float tolerance = .0001f) {
    for (unsigned i = 0; i < count; ++i) if (!std::isfinite(actual[i]) || std::fabs(actual[i] - expected[i]) > tolerance) return false;
    return true;
}
template<class T> bool Same(const T& a, const T& b) { return !std::memcmp(&a, &b, sizeof(T)); }
// Independent affine pivot identity: p' = (p-rest)*R + rest + offset.
// The reference rotation comes from D3DX, not the solver's local/world passes.
Matrix PivotRotation(const float quaternion[4], const float rest[3], const float offset[3]) {
    d3dx::D3DXMATRIXF rotation{};
    d3dx::Get().matrixRotationQuaternion(&rotation, quaternion);
    Matrix value; std::memcpy(value.data(), &rotation, sizeof(rotation));
    for (unsigned c = 0; c < 3; ++c) value[12 + c] = rest[c] + offset[c] -
        (rest[0] * value[c] + rest[1] * value[4 + c] + rest[2] * value[8 + c]);
    return value;
}
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    // Leading sentinel remains part of the real allocation. The old negative
    // subscript therefore yields deterministic wrong numbers, not a crash.
    std::array<mdl::BoneRecord, 3> storage{};
    mdl::BoneRecord& sentinel = storage[0];
    mdl::BoneRecord& root = storage[1];
    mdl::BoneRecord& source = storage[2];
    std::array<mdl::BoneOrderEntry, 2> selectors{};
    std::unique_ptr<mdl::ModelRecord> external = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 2> externalBones{};
    std::array<unsigned char*, 2> slots{};
    Fixture() {
        model->boneCount = 2; model->boneTable = &storage[1]; model->boneOrderTable = selectors.data(); model->boneOrderCount = 2;
        external->boneCount = 2; external->boneTable = externalBones.data();
        slots[0] = slots[1] = reinterpret_cast<unsigned char*>(external.get());
        for (auto& bone : storage) {
            bone.parent = bone.slotIndex = -1; bone.tailBone = -1; bone.tailIdx = -1;
            bone.physicsQuat[3] = bone.rotQuat[3] = bone.rotQuat2[3] = 1;
            Put(bone.matInit, Identity()); Put(bone.matLocal, Identity()); Put(bone.matWorld, Identity()); Put(bone.matExtra, Identity());
        }
        auto poison = Translation(101, 203, 307); poison[0] = 2; poison[5] = 3; poison[10] = 4;
        Put(sentinel.matInit, poison); Put(sentinel.matLocal, Translation(-31, -37, -41)); Put(sentinel.matExtra, Translation(501, 601, 701));
        std::memcpy(sentinel.name, "sentinel", 9);
        root.hasFlag = 1; root.layer = 4; root.type = mdl::BoneType::RotateGrant; root.tailIdx = 1;
        root.position[0] = 7; root.position[1] = 11; root.position[2] = 13;
        root.physicsOffset[0] = 2; root.physicsOffset[1] = -3; root.physicsOffset[2] = 4;
        Put(root.matExtra, Translation(-500, -600, -700));
        source.type = mdl::BoneType::UnderIk; source.layer = 9;
        source.position[0] = -2; source.position[1] = 3; source.position[2] = 5;
        Put(source.matInit, Translation(9, 8, 7));
        selectors[0].boneIndex = -1; selectors[1].boneIndex = 0;
        for (auto& selector : selectors) { selector.linkedModel = -1; selector.linkedBone = 0; }
        externalBones[0].position[0] = 5; externalBones[0].position[1] = 6; externalBones[0].position[2] = 7;
        Put(externalBones[0].matExtra, Translation(40, 50, 60));
        externalBones[1].position[0] = -2; externalBones[1].position[1] = 3; externalBones[1].position[2] = 4;
        Put(externalBones[1].matExtra, Translation(-10, 20, 30));
    }
    ~Fixture() { bone_scale::Forget(raw()); }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(model.get()); }
    void Run(unsigned char after = 0, int layer = 4, int physics = 0, bool externalSlots = false) {
        const auto sentinelBefore = sentinel;
        const auto sourceBefore = source;
        const auto externalBefore = externalBones;
        const auto selectorsBefore = selectors;
        const auto externalRecord = std::make_unique<mdl::ModelRecord>(*external);
        BoneFrameTransform(raw(), after, layer, externalSlots ? slots.data() : nullptr, physics);
        Check(Same(sentinel, sentinelBefore), "leading sentinel is not modified");
        Check(Same(source, sourceBefore), "different-layer inheritance source is not modified");
        Check(Same(externalBones, externalBefore) && Same(*external, *externalRecord), "external parent model remains read-only");
        Check(Same(selectors, selectorsBefore), "external selector records remain unchanged");
    }
};
void CheckRootWorld(Fixture& f, const Matrix& local, const Matrix& world, const Matrix& parentRotation, bool copy = true) {
    Check(Near(f.root.matLocal, local.data(), 16), "phase E builds expected local matrix");
    Check(Near(f.root.matInit, world.data(), 16), "phase E root world uses root/external routing, never the leading sentinel");
    Check(Near(f.root.matWorld, parentRotation.data(), 16), "root parent rotation mirror follows root/external routing");
    if (copy) Check(Near(f.root.matExtra, world.data(), 16), "copy gate publishes corrected world matrix");
}
void PlainRoots() {
    for (bool selectorFallback : {false, true}) {
        Fixture f; if (selectorFallback) f.root.slotIndex = 1;
        f.Run(); const auto expected = Translation(2, -3, 4);
        CheckRootWorld(f, expected, expected, Identity());
        // Rest pivot (7,11,13) plus translation (2,-3,4) is (9,8,17).
        const float point[]{7 * f.root.matInit[0] + 11 * f.root.matInit[4] + 13 * f.root.matInit[8] + f.root.matInit[12],
            7 * f.root.matInit[1] + 11 * f.root.matInit[5] + 13 * f.root.matInit[9] + f.root.matInit[13],
            7 * f.root.matInit[2] + 11 * f.root.matInit[6] + 13 * f.root.matInit[10] + f.root.matInit[14]};
        const float expectedPoint[]{9, 8, 17}; Check(Near(point, expectedPoint, 3), "root rest position moves by its own offset only");
    }
}
void PmxInheritedRoots() {
    for (bool rotated : {false, true}) {
        Fixture f; f.model->physicsMode = 2; f.root.type = mdl::BoneType::RotateMove;
        f.root.flags = mdl::kBoneFlagTransInherit | (rotated ? mdl::kBoneFlagRotInherit : 0);
        f.root.inheritRatio = rotated ? 1.f : .5f;
        f.source.physicsOffset[0] = 4; f.source.physicsOffset[1] = -6; f.source.physicsOffset[2] = 8;
        const float quaternion[]{0, 0, rotated ? .6f : 0.f, rotated ? .8f : 1.f};
        d3dx::D3DXMATRIXF rotation{}; d3dx::Get().matrixRotationQuaternion(&rotation, quaternion);
        std::memcpy(f.source.matLocal, &rotation, sizeof(rotation));
        const float offset[]{rotated ? 6.f : 4.f, rotated ? -9.f : -6.f, rotated ? 12.f : 8.f};
        const auto expected = PivotRotation(quaternion, f.root.position, offset);
        f.Run(); CheckRootWorld(f, expected, expected, Identity());
        Check(Near(f.root.physicsOffset, offset, 3), "translation inheritance updates root offset once");
    }
}
void PmdRotationRoot() {
    Fixture f; const float quaternion[]{0, 0, .6f, .8f};
    std::memcpy(f.source.rotQuat2, quaternion, sizeof(quaternion));
    // physicsQuat remains identity: RotateGrant must use the UnderIk result.
    const auto expected = PivotRotation(quaternion, f.root.position, f.root.physicsOffset);
    f.Run(); CheckRootWorld(f, expected, expected, Identity());
}
void ExternalRoots() {
    for (unsigned route = 0; route < 4; ++route) {
        Fixture f; f.selectors[0].linkedModel = 0; f.selectors[0].linkedBone = 0;
        if (route == 1) f.root.slotIndex = 1; // Explicit -1 falls back to global.
        if (route >= 2) { f.root.slotIndex = 1; f.selectors[1].linkedModel = 1; f.selectors[1].linkedBone = 1; }
        if (route == 3) f.selectors[0].linkedModel = -1;
        const auto expected = route < 2 ? Translation(47, 53, 71) : Translation(-17, 9, 25);
        f.Run(0, 4, 0, true); CheckRootWorld(f, Translation(2, -3, 4), expected, Identity());
    }
    Fixture rotated; rotated.selectors[0].linkedModel = 0;
    auto quarterTurn = Identity(); quarterTurn[0] = quarterTurn[5] = 0; quarterTurn[1] = 1; quarterTurn[4] = -1;
    auto externalMatrix = quarterTurn; externalMatrix[12] = 40; externalMatrix[13] = 50; externalMatrix[14] = 60;
    Put(rotated.externalBones[0].matExtra, externalMatrix);
    // (own offset + external rest)=(7,3,11); a quarter turn gives
    // (-3,7,11), then external translation gives (37,57,71).
    auto expected = quarterTurn; expected[12] = 37; expected[13] = 57; expected[14] = 71;
    rotated.Run(0, 4, 0, true); CheckRootWorld(rotated, Translation(2, -3, 4), expected, quarterTurn);
}
void GatesAndExistingChild() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture f;
        if (mode != 0) f.root.flags |= mdl::kBoneFlagAfterPhysics;
        const auto before = f.root;
        if (mode == 0) f.Run(0, 3); // Layer mismatch.
        else if (mode == 1) f.Run(0, 4); // Before-physics request excludes this bone.
        else { f.Run(1, 4); CheckRootWorld(f, Translation(2, -3, 4), Translation(2, -3, 4), Identity()); }
        if (mode != 2) Check(Same(before, f.root), "inactive layer/channel leaves the entire root untouched");
    }
    for (int mode = 0; mode <= 3; ++mode) for (uint8_t rigid : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(197)}) for (uint8_t disabled : {uint8_t(0), uint8_t(1)}) {
        Fixture f; f.root.hasRigidBody = rigid; f.root.physicsDisabled = disabled;
        Matrix extra; std::memcpy(extra.data(), f.root.matExtra, sizeof(f.root.matExtra));
        f.Run(0, 4, mode);
        const bool shouldCopy = mode == 0 || (rigid & 1) == 0 || (mode >= 2 && disabled != 0);
        CheckRootWorld(f, Translation(2, -3, 4), Translation(2, -3, 4), Identity(), shouldCopy);
        if (!shouldCopy) Check(!std::memcmp(extra.data(), f.root.matExtra, sizeof(f.root.matExtra)), "physics copy gate retains the previous matExtra");
    }
    Fixture child; child.root.parent = 1;
    child.Run();
    CheckRootWorld(child, Translation(2, -3, 4), Translation(11, 5, 11), Identity());
    // Phase E's existing nonroot path intentionally takes the ordinary parent
    // even when phase C used a selector. This root fix must not broaden scope.
    Fixture selectedChild; selectedChild.root.parent = 1; selectedChild.root.slotIndex = 1;
    selectedChild.selectors[1].linkedModel = 1; selectedChild.selectors[1].linkedBone = 1;
    selectedChild.Run(0, 4, 0, true);
    CheckRootWorld(selectedChild, Translation(2, -3, 4), Translation(11, 5, 11), Identity());
}
}
int main() {
    auto& api = d3dx::Get();
    if (!api.Load() || !api.matrixRotationQuaternion) { std::fprintf(stderr, "FAIL: D3DX reference math unavailable\n"); return 1; }
    PlainRoots(); PmxInheritedRoots(); PmdRotationRoot(); ExternalRoots(); GatesAndExistingChild();
    std::printf("%s %u root inheritance checks, %u failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
