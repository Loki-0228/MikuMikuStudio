// CPU pose preparation parity. Deliberately exclude layers, IK and simulation.
#define NOMINMAX
#include "mikumikustudio/bone_transform_context.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace { thread_local int failAfter = -1; }
void* operator new(std::size_t bytes) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (auto* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace mikumikustudio;
namespace {
unsigned checks = 0;
const char* currentCase = "setup";
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s line %d: %s\n",currentCase,__LINE__,#x); std::exit(1); } } while (false)
template<class T> bool Bits(const T& a, const T& b) { return !std::memcmp(&a,&b,sizeof(T)); }
using Bones = std::array<mdl::BoneRecord,12>;

// Independent DLL lease for the historical oracle. Never route reference
// quaternion composition through the new context or the host's global table.
struct NativeMath {
    HMODULE module = nullptr;
    d3dx::FnQuatMultiply multiply = nullptr;
    NativeMath() {
        module = LoadLibraryA(sizeof(void*) == 8 ? "d3dx9_43.dll" : "d3dx9_32.dll");
        if (!module) throw std::runtime_error("reference D3DX DLL unavailable");
        multiply = reinterpret_cast<d3dx::FnQuatMultiply>(GetProcAddress(module,"D3DXQuaternionMultiply"));
        if (!multiply) { FreeLibrary(module); module = nullptr; throw std::runtime_error("reference quaternion export unavailable"); }
    }
    ~NativeMath() { if (module) FreeLibrary(module); }
    NativeMath(const NativeMath&) = delete;
    NativeMath& operator=(const NativeMath&) = delete;
};

// Frozen reference: the first two loops of SetPhysicsMode from commit
// 1fc4b9c, src/physics/physics_create.cpp. Only QuatMultiply is replaced by
// its real independently resolved D3DX export. Do not share new helpers here.
void Reference(unsigned char* m, int physicsMode, const NativeMath& math) {
    mdl::ModelRecord& model = *mdl::Mdl(m);
    const int boneCount = model.boneCount;
    mdl::BoneRecord* bones = mdl::Bones(m);
    for (int i = 0; i < boneCount; ++i) {
        mdl::BoneRecord* bone = &bones[i];
        const unsigned char flag = bone->hasRigidBody;
        if (((physicsMode == 1 || (physicsMode >= 2 && bone->physicsDisabled == 0)) && flag) != 0) {
            std::memcpy(bone->physicsOffset,bone->ikBackup,12);
            std::memcpy(bone->physicsQuat,bone->ikBackup + 3,16);
        } else {
            std::memcpy(bone->physicsOffset,bone->trans,12);
            std::memcpy(bone->physicsQuat,bone->rotQuat,16);
        }
    }
    if (model.physicsMode == 2) {
        const int recCount = mdl::BoneMorphOffsetCount(m);
        mdl::BoneMorphOffsetRecord* records = mdl::BoneMorphOffsets(m);
        for (int i = 0; i < recCount; ++i) {
            const mdl::BoneMorphOffsetRecord& record = records[i];
            if (record.boneIndex < 0 || record.boneIndex >= boneCount) continue;
            mdl::BoneRecord* bone = &bones[record.boneIndex];
            bone->physicsOffset[0] += record.translation[0];
            bone->physicsOffset[1] += record.translation[1];
            bone->physicsOffset[2] += record.translation[2];
            float out[4];
            math.multiply(out,reinterpret_cast<const float*>(bone->physicsQuat),record.rotation);
            std::memcpy(bone->physicsQuat,out,16);
        }
    }
}

struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    Bones bones{};
    std::array<mdl::BoneMorphOffsetRecord,7> morphs{};
    Fixture() {
        model->boneCount = uint32_t(bones.size()); model->boneTable = bones.data();
        model->boneMorphCount = int(morphs.size()); model->boneMorphTable = morphs.data();
        model->physicsMode = 2; model->maxBoneLayer = -1; // Real SetPhysicsMode runs preparation only.
        model->searchCursor = 987; model->displayKeyCursor = 654; model->loadComplete = 197;
        const unsigned char rigid[]{0,1,197,2}, disabled[]{0,1,197};
        for (unsigned i = 0; i < bones.size(); ++i) {
            auto& bone = bones[i];
            std::snprintf(bone.name,20,"pose-%u",i); bone.type = mdl::BoneType::RotateMove;
            bone.parent = -1; bone.slotIndex = -1; bone.layer = i % 2 ? INT32_MIN : INT32_MAX;
            bone.hasRigidBody = rigid[i / 3]; bone.physicsDisabled = disabled[i % 3];
            bone.flags = uint16_t(0x1000 | i); bone.hasFlag = uint8_t(21 + i);
            bone.trans[0] = 1 + float(i); bone.trans[1] = -.25f * float(i + 1); bone.trans[2] = .125f;
            bone.rotQuat[0] = .25f; bone.rotQuat[1] = -.5f; bone.rotQuat[2] = .125f; bone.rotQuat[3] = 1.75f;
            bone.ikBackup[0] = -100 - float(i); bone.ikBackup[1] = .75f; bone.ikBackup[2] = -1.5f;
            bone.ikBackup[3] = -.75f; bone.ikBackup[4] = .125f; bone.ikBackup[5] = 1.25f; bone.ikBackup[6] = .5f;
            for (unsigned c = 0; c < 3; ++c) { bone.physicsOffset[c] = 444 + float(c); bone.position[c] = float(i + c); }
            for (unsigned c = 0; c < 4; ++c) { bone.physicsQuat[c] = 555 + float(c); bone.rotQuat2[c] = 666 + float(c); }
            for (unsigned c = 0; c < 16; ++c) {
                bone.matLocal[c] = float(c + 10); bone.matInit[c] = float(c + 30);
                bone.matWorld[c] = float(c + 50); bone.matExtra[c] = float(c + 70);
            }
        }
        for (auto& morph : morphs) morph.rotation[3] = 1;
        morphs[0].boneIndex = 0; morphs[0].translation[0] = 1e20f;
        morphs[0].rotation[1] = .6f; morphs[0].rotation[3] = .8f;
        morphs[1].boneIndex = 3; morphs[1].translation[0] = .25f; morphs[1].translation[1] = -.5f;
        morphs[1].rotation[2] = .5f; morphs[1].rotation[3] = 1.5f;
        morphs[2].boneIndex = 0; morphs[2].translation[0] = -1e20f;
        morphs[2].rotation[0] = .25f; morphs[2].rotation[3] = 1.75f;
        morphs[3].boneIndex = 0; morphs[3].translation[0] = 3; morphs[3].translation[1] = 5; morphs[3].translation[2] = 7;
        morphs[3].rotation[2] = .8f; morphs[3].rotation[3] = .6f;
        const int invalid[]{-1,int(bones.size()),INT32_MAX};
        for (unsigned i = 4; i < morphs.size(); ++i) {
            morphs[i].boneIndex = invalid[i - 4];
            for (float& v : morphs[i].translation) v = std::numeric_limits<float>::quiet_NaN();
            for (float& v : morphs[i].rotation) v = std::numeric_limits<float>::quiet_NaN();
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(model.get()); }
};

bool EqualPose(const Bones& a, const Bones& b) {
    for (unsigned i = 0; i < a.size(); ++i)
        if (std::memcmp(a[i].physicsOffset,b[i].physicsOffset,sizeof(a[i].physicsOffset)) ||
            std::memcmp(a[i].physicsQuat,b[i].physicsQuat,sizeof(a[i].physicsQuat))) return false;
    return true;
}
bool UnchangedApartFromPose(const mdl::BoneRecord& before, const mdl::BoneRecord& after) {
    auto allowed = before;
    std::memcpy(allowed.physicsOffset,after.physicsOffset,sizeof(after.physicsOffset));
    std::memcpy(allowed.physicsQuat,after.physicsQuat,sizeof(after.physicsQuat));
    return Bits(allowed,after);
}
struct ReadOnlyState {
    std::unique_ptr<mdl::ModelRecord> model;
    Bones bones;
    decltype(Fixture::morphs) morphs;
    explicit ReadOnlyState(const Fixture& f) : model(std::make_unique<mdl::ModelRecord>(*f.model)),bones(f.bones),morphs(f.morphs) {}
    bool Unchanged(const Fixture& f) const {
        if (!Bits(*model,*f.model) || !Bits(morphs,f.morphs)) return false;
        for (unsigned i = 0; i < bones.size(); ++i) if (!UnchangedApartFromPose(bones[i],f.bones[i])) return false;
        return true;
    }
};
void PrivateNoAllocation(Fixture& f, int mode, const BoneTransformContext& context) {
    failAfter = 0;
    try { BonePreparePosePrivate(*f.model,mode,context); }
    catch (...) { failAfter = -1; throw; }
    failAfter = -1;
}

void CrossModes(const BoneTransformContext& context, const NativeMath& math) {
    currentCase = "model format, source modes and raw participation bytes";
    for (unsigned format : {0u,1u,2u}) for (int mode : {-1,0,1,2,3}) {
        Fixture legacy, actual, expected;
        legacy.model->physicsMode = actual.model->physicsMode = expected.model->physicsMode = uint8_t(format);
        const ReadOnlyState legacyBefore(legacy), actualBefore(actual), expectedBefore(expected);
        Reference(expected.raw(),mode,math);
        SetPhysicsMode(legacy.raw(),0,nullptr,mode);
        PrivateNoAllocation(actual,mode,context);
        CHECK(EqualPose(actual.bones,legacy.bones) && EqualPose(actual.bones,expected.bones));
        CHECK(legacyBefore.Unchanged(legacy) && actualBefore.Unchanged(actual) && expectedBefore.Unchanged(expected));
        if (format == 2) CHECK(actual.bones[0].physicsOffset[0] == 3); // Sequential float accumulation, not a reduced sum.
        // An unmodified bone exposes source choice without a morph term.
        for (unsigned i : {1u,2u,4u,5u,6u,7u,8u,9u,10u,11u}) {
            const bool backup = mode == 1 ? i >= 3 : mode >= 2 && i >= 3 && i % 3 == 0;
            const auto& input = actualBefore.bones[i];
            CHECK(!std::memcmp(actual.bones[i].physicsOffset,backup ? input.ikBackup : input.trans,12));
            CHECK(!std::memcmp(actual.bones[i].physicsQuat,backup ? input.ikBackup + 3 : input.rotQuat,16));
        }
        const auto first = actual.bones;
        for (auto& bone : actual.bones) { bone.physicsOffset[0] = -999; bone.physicsQuat[2] = 999; }
        PrivateNoAllocation(actual,mode,context);
        CHECK(EqualPose(actual.bones,first) && actualBefore.Unchanged(actual));
        // afterPhysics selects only later transform sweeps; preparation itself
        // is identical before both sweeps and starts from immutable inputs.
        SetPhysicsMode(legacy.raw(),1,nullptr,mode);
        CHECK(EqualPose(actual.bones,legacy.bones) && legacyBefore.Unchanged(legacy));
    }
}

void QuaternionOrderAndRecordOrder(const BoneTransformContext& context, const NativeMath& math) {
    currentCase = "noncommuting quaternion and record composition";
    Fixture actual, expected; actual.model->boneMorphCount = expected.model->boneMorphCount = 1;
    const float q = std::sqrt(.5f);
    for (auto* f : {&actual,&expected}) {
        f->bones[0].rotQuat[0] = q; f->bones[0].rotQuat[1] = f->bones[0].rotQuat[2] = 0; f->bones[0].rotQuat[3] = q;
        f->morphs[0] = {}; f->morphs[0].boneIndex = 0; f->morphs[0].rotation[1] = q; f->morphs[0].rotation[3] = q;
    }
    Reference(expected.raw(),0,math); PrivateNoAllocation(actual,0,context);
    CHECK(EqualPose(actual.bones,expected.bones));
    // D3DXQuaternionMultiply(out, pose, morph) is Hamilton morph * pose.
    // Y90 * X90 = (.5,.5,-.5,.5); the reversed order has positive Z.
    const float product = q*q;
    const std::array<float,4> correct{product,product,-product,product};
    CHECK(!std::memcmp(actual.bones[0].physicsQuat,correct.data(),sizeof(correct)));
    std::array<float,4> wrong{};
    math.multiply(wrong.data(),actual.morphs[0].rotation,actual.bones[0].rotQuat);
    CHECK(wrong[2] > 0 && actual.bones[0].physicsQuat[2] < 0 && !Bits(wrong,correct));
    Fixture forward, reverse, reverseExpected;
    std::swap(reverse.morphs[0],reverse.morphs[3]);
    std::swap(reverseExpected.morphs[0],reverseExpected.morphs[3]);
    PrivateNoAllocation(forward,0,context); PrivateNoAllocation(reverse,0,context); Reference(reverseExpected.raw(),0,math);
    CHECK(EqualPose(reverse.bones,reverseExpected.bones));
    CHECK(forward.bones[0].physicsOffset[0] == 3 && reverse.bones[0].physicsOffset[0] == 0);
    CHECK(std::memcmp(forward.bones[0].physicsQuat,reverse.bones[0].physicsQuat,16) != 0);
    // Native source quaternions are neither normalized nor clamped.
    CHECK(forward.bones[1].physicsQuat[3] == 1.75f);
}

void EmptyTables(const BoneTransformContext& context, const NativeMath& math) {
    currentCase = "empty bones and morph tables";
    for (unsigned emptyBones : {0u,1u}) for (unsigned format : {0u,1u,2u}) {
        Fixture actual, expected, legacy;
        for (auto* f : {&actual,&expected,&legacy}) {
            f->model->physicsMode = uint8_t(format);
            f->model->boneMorphCount = 0; f->model->boneMorphTable = nullptr;
            if (emptyBones) { f->model->boneCount = 0; f->model->boneTable = nullptr; }
        }
        const ReadOnlyState before(actual);
        PrivateNoAllocation(actual,3,context); Reference(expected.raw(),3,math); SetPhysicsMode(legacy.raw(),1,nullptr,3);
        CHECK(EqualPose(actual.bones,expected.bones) && EqualPose(actual.bones,legacy.bones) && before.Unchanged(actual));
        if (emptyBones) CHECK(Bits(actual.bones,before.bones));
    }
}

std::atomic<unsigned> globalCalls{0};
d3dx::FnQuatMultiply forwardedMultiply = nullptr;
float* WINAPI GlobalMultiply(float* out, const float* first, const float* second) {
    ++globalCalls; return forwardedMultiply(out,first,second);
}
struct GlobalApiGuard {
    d3dx::Api& api = d3dx::Get();
    std::array<unsigned char,sizeof(d3dx::Api)> saved{},poisoned{};
    GlobalApiGuard() {
        static_assert(std::is_trivially_copyable<d3dx::Api>::value,"save API without side effects");
        std::memcpy(saved.data(),&api,sizeof(api)); forwardedMultiply = api.quatMultiply; globalCalls = 0;
        api.module = reinterpret_cast<HMODULE>(uintptr_t(1)); api.quatMultiply = GlobalMultiply;
        std::memcpy(poisoned.data(),&api,sizeof(api));
    }
    bool Untouched() const { return globalCalls == 0 && !std::memcmp(poisoned.data(),&api,sizeof(api)); }
    ~GlobalApiGuard() { std::memcpy(&api,saved.data(),sizeof(api)); }
};
void GlobalIsolation(const BoneTransformContext& context, const NativeMath& math) {
    currentCase = "private preparation does not use global math";
    Fixture actual, expected; Reference(expected.raw(),2,math); const ReadOnlyState before(actual);
    bool untouched = false;
    {
        GlobalApiGuard guard;
        PrivateNoAllocation(actual,2,context);
        untouched = guard.Untouched();
    }
    CHECK(untouched && EqualPose(actual.bones,expected.bones) && before.Unchanged(actual));
}

void ConcurrentWorkspaces(const std::shared_ptr<const BoneTransformContext>& context, const NativeMath& math) {
    currentCase = "four independent workspaces share the immutable context";
    std::array<std::unique_ptr<Fixture>,4> fixtures;
    std::array<std::unique_ptr<ReadOnlyState>,4> before;
    std::array<Bones,4> expected;
    for (unsigned i = 0; i < fixtures.size(); ++i) {
        fixtures[i] = std::make_unique<Fixture>();
        fixtures[i]->bones[3].trans[1] += float(i);
        fixtures[i]->morphs[1].translation[2] = float(i) * .25f;
        before[i] = std::make_unique<ReadOnlyState>(*fixtures[i]);
        Fixture oracle; oracle.bones = fixtures[i]->bones; oracle.morphs = fixtures[i]->morphs;
        Reference(oracle.raw(),int(i),math); expected[i] = oracle.bones;
    }
    std::atomic<unsigned> failures{0}; std::array<std::thread,4> workers;
    try {
        for (unsigned i = 0; i < workers.size(); ++i) workers[i] = std::thread([&,i,lease=context] {
            try {
                failAfter = 0;
                for (unsigned repeat = 0; repeat < 500; ++repeat) {
                    // Model preparation must reset outputs every time, even
                    // when a preceding transform pass would have changed them.
                    fixtures[i]->bones[0].physicsOffset[1] = 12345;
                    fixtures[i]->bones[0].physicsQuat[3] = -12345;
                    BonePreparePosePrivate(*fixtures[i]->model,int(i),*lease);
                    if (!EqualPose(fixtures[i]->bones,expected[i]) || !before[i]->Unchanged(*fixtures[i])) ++failures;
                }
                failAfter = -1;
            } catch (...) { failAfter = -1; ++failures; }
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (auto& worker : workers) worker.join(); CHECK(failures == 0);
}
}
int main() {
    try {
        NativeMath math; const auto context = BoneTransformContext::Create(); CHECK(bool(context));
        CHECK(d3dx::Get().Load() && d3dx::Get().quatMultiply);
        CrossModes(*context,math); QuaternionOrderAndRecordOrder(*context,math); EmptyTables(*context,math);
        ConcurrentWorkspaces(context,math); GlobalIsolation(*context,math);
        std::printf("PASS %u bone pose preparation checks\n",checks); return 0;
    } catch (const std::exception& error) {
        failAfter = -1; std::fprintf(stderr,"FAIL %s unexpected exception: %s\n",currentCase,error.what()); return 1;
    }
}
