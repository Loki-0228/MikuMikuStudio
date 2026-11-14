// MAINTENANCE: regression-keep; invalid inheritance must preserve the own pose.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_transform_context.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

using namespace mikumikustudio;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
bool Near(const float* a, const float* b, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (!std::isfinite(a[i]) || std::fabs(a[i] - b[i]) > .0001f) return false;
    return true;
}
void Rotation(float* q, float angle) {
    q[0] = q[1] = 0; q[2] = std::sin(angle / 2); q[3] = std::cos(angle / 2);
}
enum class Phase { A, B, E };
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    // Two poison records on either side make -2, -1, count and count+1
    // deterministic regressions without relying on unmapped heap memory.
    std::array<mdl::BoneRecord, 7> storage{};
    mdl::BoneOrderEntry selector{};
    mdl::BoneRecord& target = storage[3];
    Phase phase;
    explicit Fixture(Phase p) : phase(p) {
        model->boneCount = 3; model->boneTable = storage.data() + 2;
        selector.linkedModel = -1;
        model->boneOrderTable = &selector; model->boneOrderCount = 1;
        model->physicsMode = 2;
        for (auto& bone : storage) {
            bone.parent = bone.slotIndex = bone.tailIdx = bone.tailBone = -1;
            bone.layer = 9; bone.physicsQuat[3] = bone.rotQuat[3] = bone.rotQuat2[3] = 1;
            bone.matInit[0] = bone.matInit[5] = bone.matInit[10] = bone.matInit[15] = 1;
            std::memcpy(bone.matWorld, bone.matInit, sizeof(bone.matWorld));
            std::memcpy(bone.matExtra, bone.matInit, sizeof(bone.matExtra));
            Rotation(bone.physicsQuat, .8f);
            std::memcpy(bone.rotQuat2, bone.physicsQuat, sizeof(bone.rotQuat2));
            d3dx::Get().matrixRotationQuaternion(
                reinterpret_cast<d3dx::D3DXMATRIXF*>(bone.matLocal), bone.physicsQuat);
            bone.physicsOffset[0] = 8; bone.physicsOffset[1] = -4; bone.physicsOffset[2] = 6;
        }
        target.layer = 4; target.hasFlag = p == Phase::B ? 0 : 1;
        target.type = p == Phase::A ? mdl::BoneType::UnderIk : mdl::BoneType::RotateMove;
        target.position[0] = 7; target.position[1] = 11; target.position[2] = 13;
        target.physicsOffset[0] = 2; target.physicsOffset[1] = -3; target.physicsOffset[2] = 4;
        Rotation(target.physicsQuat, .4f);
    }
    ~Fixture() { bone_scale::Forget(reinterpret_cast<unsigned char*>(model.get())); }
    void Run(const BoneTransformContext* context, unsigned char after = 0) {
        const auto before = storage;
        if (context) BoneFrameTransformPrivate(*model, after, 4, nullptr, 0, *context);
        else BoneFrameTransform(reinterpret_cast<unsigned char*>(model.get()), after, 4, nullptr, 0);
        for (unsigned i = 0; i < storage.size(); ++i)
            if (i != 3) Check(!std::memcmp(&storage[i], &before[i], sizeof(storage[i])), "sources and poison records remain unchanged");
    }
    void Expect(float angle, const float* offset, bool accumulated) {
        // Independent affine pivot identity, using D3DX reference rotation.
        float q[4]; Rotation(q, angle);
        d3dx::D3DXMATRIXF rotation{};
        d3dx::Get().matrixRotationQuaternion(&rotation, q);
        float expected[16]; std::memcpy(expected, &rotation, sizeof(expected));
        for (unsigned c = 0; c < 3; ++c) expected[12 + c] = target.position[c] + offset[c] -
            (target.position[0] * expected[c] + target.position[1] * expected[4 + c] + target.position[2] * expected[8 + c]);
        Check(Near(target.matLocal, expected, 16), "local pose matches independent rotation and pivot reference");
        Check(Near(target.matInit, expected, 16), "root world preserves the expected pose");
        Check(Near(target.matExtra, expected, 16), "published pose matches expected pose");
        const float own[]{2, -3, 4};
        Check(Near(target.physicsOffset, accumulated ? offset : own, 3), "offset accumulates inheritance only in phases B/E");
    }
};
void PmxSources(const BoneTransformContext* context, bool extreme) {
    const unsigned flags[]{mdl::kBoneFlagRotInherit, mdl::kBoneFlagTransInherit,
        mdl::kBoneFlagRotInherit | mdl::kBoneFlagTransInherit};
    for (auto phase : {Phase::A, Phase::B, Phase::E}) for (auto flag : flags)
    for (unsigned char after : {0, 1}) {
        const int indices[]{-2, -1, 3, 4, (std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()};
        for (int index : indices) {
            if (!extreme && (index == indices[4] || index == indices[5])) continue;
            Fixture f(phase); f.target.flags = flag | (after ? mdl::kBoneFlagAfterPhysics : 0);
            f.target.tailIdx = index; f.target.inheritRatio = -.5f;
            f.Run(context, after); const float own[]{2, -3, 4}; f.Expect(.4f, own, false);
            float q[4]; Rotation(q, .4f);
            Check(Near(f.target.physicsQuat, q, 4), "invalid inheritance retains the own quaternion");
        }
        for (int index : {0, 2}) for (float rate : {0.f, .5f, 1.f, -.5f}) {
            Fixture f(phase); f.target.flags = flag | (after ? mdl::kBoneFlagAfterPhysics : 0);
            f.target.tailIdx = index; f.target.inheritRatio = rate;
            f.Run(context, after);
            const float r = (flag & mdl::kBoneFlagTransInherit) ? rate : 0;
            const float offset[]{2 + 8 * r, -3 - 4 * r, 4 + 6 * r};
            f.Expect(.4f + ((flag & mdl::kBoneFlagRotInherit) ? .8f * rate : 0), offset, phase != Phase::A);
        }
    }
}
void PmdSources(const BoneTransformContext* context) {
    for (auto phase : {Phase::B, Phase::E})
    for (auto type : {mdl::BoneType::RotateGrant, mdl::BoneType::CoRotate}) {
        for (int index : {-2, -1, 3, 4}) {
            Fixture f(phase); f.target.type = type;
            f.target.tailIdx = type == mdl::BoneType::CoRotate ? -50 : index;
            f.target.tailBone = index; f.Run(context);
            const float own[]{2, -3, 4}; f.Expect(.4f, own, false);
        }
        for (int index : {0, 2}) {
            Fixture f(phase); f.target.type = type;
            f.target.tailIdx = type == mdl::BoneType::CoRotate ? -50 : index;
            f.target.tailBone = index; f.Run(context);
            const float own[]{2, -3, 4};
            f.Expect(type == mdl::BoneType::CoRotate ? -.4f : 1.2f, own, false);
        }
    }
}
}
int main(int argc, char** argv) {
    auto& api = d3dx::Get();
    if (!api.Load() || !api.matrixRotationQuaternion) return 2;
    auto context = BoneTransformContext::Create();
    if (!context) return 2;
    const bool extreme = argc <= 1 || std::strcmp(argv[1], "--bounded") != 0;
    for (const auto* math : {static_cast<const BoneTransformContext*>(nullptr), context.get()}) {
        PmxSources(math, extreme); PmdSources(math);
    }
    std::printf("%s %u inheritance index checks, %u failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
