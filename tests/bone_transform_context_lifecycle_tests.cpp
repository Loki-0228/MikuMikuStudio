#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/bone_transform_context.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local int allocationCountdown = -1;
void* Allocate(size_t size) {
    if (allocationCountdown == 0) throw std::bad_alloc();
    if (allocationCountdown > 0) --allocationCountdown;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
}
void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

using namespace mikumikustudio;
namespace bs = bone_scale;
namespace {
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
bool Near(float a, float b) { return std::isfinite(a) && std::abs(a - b) < 1e-5f; }
void Identity(float (&matrix)[16]) {
    std::memset(matrix, 0, sizeof(matrix)); matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
}
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> model = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 4> bones{};
    std::array<mdl::BoneKey, 4> keys{};
    std::array<mdl::BoneOrderEntry, 1> selectors{};
    mdl::IkChain ik{};
    uint16_t link = 1;
    explicit Fixture(bool withIk = false) {
        model->boneCount = withIk ? 4 : 1; model->boneTable = bones.data(); model->boneKeys = keys.data();
        model->boneOrderCount = 1; model->boneOrderTable = selectors.data(); model->loadComplete = 1;
        model->physicsMode = 2;
        selectors[0].boneIndex = -1; selectors[0].linkedModel = -1; selectors[0].linkedBone = 0;
        for (unsigned i = 0; i < bones.size(); ++i) {
            auto& b = bones[i]; b.parent = -1; b.slotIndex = -1; b.tailBone = -1; b.tailIdx = -1;
            b.type = mdl::BoneType::RotateMove; b.flags = mdl::kBoneFlagVisible;
            b.physicsQuat[3] = b.rotQuat[3] = b.rotQuat2[3] = 1; keys[i].rotation[3] = 1;
            Identity(b.matInit); Identity(b.matLocal); Identity(b.matWorld); Identity(b.matExtra);
            std::snprintf(b.name, sizeof(b.name), "context-%u", i);
        }
        if (withIk) {
            bones[1].parent = 0; bones[1].position[1] = 1; bones[1].type = mdl::BoneType::UnderIk;
            bones[2].parent = 1; bones[2].position[1] = 2; bones[2].type = mdl::BoneType::Effector;
            bones[3].position[0] = .7f; bones[3].position[1] = 1.6f; bones[3].type = mdl::BoneType::Ik;
            model->ikChainCount = 1; model->ikChains = &ik;
            ik.boneIndex = 3; ik.targetBone = 2; ik.linkCount = 1; ik.links = &link;
            ik.iterations = 4; ik.enabled = 1; ik.maxAngle = .05f;
        }
    }
    unsigned char* Raw() { return reinterpret_cast<unsigned char*>(model.get()); }
};
#if !MMD_ORIGINAL_PROFILE
void ScaleIsolation(const BoneTransformContext& context) {
    Fixture f;
    f.bones[0].matInit[12] = 4;
    const bs::Matrix scale{{2, 0, 0, 0, 3, 0, 0, 0, 4}};
    Check(bs::SetPose(f.Raw(), 0, scale), "set real scale pose");
    bs::SetKey(f.Raw(), 0, scale); bs::SetKeyCurve(f.Raw(), 0, {5, 20, 80, 120});
    struct Cleanup { unsigned char* raw; ~Cleanup() { bs::Forget(raw); } } cleanup{f.Raw()};
    const auto unscaled = f.bones;
    bs::ApplyTransforms(f.Raw());
    Check(bs::TransformsApplied(f.Raw()) && Near(f.bones[0].matInit[0], 2), "scale runtime cache was actually applied");
    const auto scaled = f.bones; const auto revision = bs::Revision(f.Raw());
    const auto pose = bs::PoseValues(f.Raw()); const auto author = bs::AuthoringState::Capture(f.Raw());
    // No bone belongs to layer 99: any write here would come from hidden
    // global restore/setup, rather than from a legitimate phase operation.
    BoneFrameTransformPrivate(*f.model, 0, 99, nullptr, 0, context);
    Check(bs::TransformsApplied(f.Raw()) && !std::memcmp(scaled.data(), f.bones.data(), sizeof(scaled)),
        "private no-matching-layer call preserves applied scale and all bone bytes");
    Check(bs::Revision(f.Raw()) == revision && bs::PoseValuesEqual(f.Raw(), pose) &&
        author.Equals(bs::AuthoringState::Capture(f.Raw())), "private kernel preserves scale revision pose and authoring");
    BoneFrameTransform(f.Raw(), 0, 99, nullptr, 0);
    Check(!bs::TransformsApplied(f.Raw()) && !std::memcmp(unscaled.data(), f.bones.data(), sizeof(unscaled)),
        "legacy no-matching-layer entry still restores the native matrices");
    Check(bs::Revision(f.Raw()) == revision && bs::PoseValuesEqual(f.Raw(), pose) &&
        author.Equals(bs::AuthoringState::Capture(f.Raw())), "legacy matrix restore leaves scale authoring intact");
}
#endif
void SharedLifetime() {
    auto owner = BoneTransformContext::Create(); Check(bool(owner), "create shared CPU context");
    std::weak_ptr<const BoneTransformContext> weak = owner;
    std::mutex mutex; std::condition_variable changed; unsigned ready = 0; bool releaseOwner = false;
    std::array<bool, 4> valid{{true, true, true, true}};
    std::vector<std::thread> workers;
    try {
        for (unsigned i = 0; i < valid.size(); ++i) workers.emplace_back([&, i, lease = owner] {
            {
                std::unique_lock<std::mutex> lock(mutex); ++ready; changed.notify_all();
                changed.wait(lock, [&] { return releaseOwner; });
            }
            try {
                Fixture f; const float translation = float(i + 1) * 1.25f;
                f.bones[0].physicsOffset[0] = translation;
                for (unsigned step = 0; step < 128; ++step) {
                    BoneFrameTransformPrivate(*f.model, 0, 0, nullptr, 0, *lease);
                    valid[i] = Near(f.bones[0].matInit[12], translation) && Near(f.bones[0].matInit[0], 1) &&
                        Near(f.bones[0].matInit[5], 1) && Near(f.bones[0].matInit[10], 1) && valid[i];
                }
            } catch (...) { valid[i] = false; }
        });
    } catch (...) {
        { std::lock_guard<std::mutex> lock(mutex); releaseOwner = true; } changed.notify_all();
        for (auto& worker : workers) worker.join(); throw;
    }
    bool workersRetained = false;
    {
        std::unique_lock<std::mutex> lock(mutex); changed.wait(lock, [&] { return ready == valid.size(); });
        owner.reset(); workersRetained = !weak.expired();
        releaseOwner = true;
    }
    changed.notify_all(); for (auto& worker : workers) worker.join();
    Check(workersRetained, "worker leases keep the context alive after host owner release");
    for (const auto ok : valid) Check(ok, "independent worker models produce deterministic translations");
    Check(weak.expired(), "context is destroyed after the last worker lease");
}
void AllocationAndRepeatedLifetime(const BoneTransformContext& retained) {
    unsigned failures = 0, successes = 0;
    for (int count = 0; count < 16; ++count) {
        std::shared_ptr<const BoneTransformContext> candidate;
        bool allocationFailed = false;
        struct FailureScope {
            explicit FailureScope(int countdown) { allocationCountdown = countdown; }
            ~FailureScope() { allocationCountdown = -1; }
        };
        {
            FailureScope inject(count);
            try { candidate = BoneTransformContext::Create(); }
            catch (const std::bad_alloc&) { ++failures; allocationFailed = true; }
        }
        Check(candidate || allocationFailed, "allocation failure propagates instead of becoming a backend-unavailable result");
        if (candidate) { ++successes; std::weak_ptr<const BoneTransformContext> weak = candidate;
            candidate.reset(); Check(weak.expired(), "successful context has no hidden ownership cycle"); }
        // A failed constructor must not invalidate any other context's module
        // reference. This never unloads or overwrites a process-global DLL.
        Fixture f; f.bones[0].physicsOffset[0] = 3;
        BoneFrameTransformPrivate(*f.model, 0, 0, nullptr, 0, retained);
        Check(Near(f.bones[0].matInit[12], 3), "existing math lease survives another context's allocation failure");
    }
    Check(failures && successes, "allocation injection covers both failure and successful creation");
    for (unsigned i = 0; i < 32; ++i) {
        auto context = BoneTransformContext::Create(); Check(bool(context), "repeated CPU context creation succeeds");
        Fixture f; f.bones[0].physicsOffset[1] = 2;
        BoneFrameTransformPrivate(*f.model, 0, 0, nullptr, 0, *context);
        Check(Near(f.bones[0].matInit[13], 2), "repeated context dispatch remains usable");
    }
}
#ifdef MIKUMIKUSTUDIO_DIAG
struct ProbeDirectory {
    const char* name = "MIKUMIKUSTUDIO_STATE_DUMP_DIR";
    std::string old; bool hadOld = false;
    std::filesystem::path path;
    ProbeDirectory() {
        const DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
        if (size) { std::vector<char> buffer(size); Check(GetEnvironmentVariableA(name, buffer.data(), size) < size, "read previous probe setting");
            old = buffer.data(); hadOld = true; }
        char temp[MAX_PATH]{}; const auto length = GetTempPathA(MAX_PATH, temp);
        Check(length && length < MAX_PATH, "find short isolated probe output path");
        path = std::filesystem::path(temp) / ("mms-bone-context-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Check(path.string().size() < MAX_PATH && std::filesystem::create_directory(path), "create fresh probe directory without reusing files");
        Check(SetEnvironmentVariableA(name, path.string().c_str()) != 0, "set isolated probe directory");
    }
    ~ProbeDirectory() { SetEnvironmentVariableA(name, hadOld ? old.c_str() : nullptr); }
};
void ProbeIsolation(const BoneTransformContext& context) {
    ProbeDirectory output;
    const auto trace = output.path / "ik_trace.jsonl", precheck = output.path / "ik_precheck.json";
    ArmBoneTransformIkProbe();
    for (unsigned i = 0; i < 3; ++i) {
        Fixture f(true);
        BoneFrameTransformPrivate(*f.model, 0, 0, nullptr, 0, context);
        Check(std::isfinite(f.bones[1].rotQuat2[2]) && std::abs(f.bones[1].rotQuat2[2]) > .001f,
            "private diagnostic fixture really performs nontrivial IK");
    }
    Check(!std::filesystem::exists(trace) && !std::filesystem::exists(precheck), "private IK never writes either diagnostic probe file");
    { std::ofstream sentinel(trace, std::ios::binary); sentinel << "PRIVATE-CONSUMED-COUNTER"; Check(bool(sentinel), "write owned probe counter sentinel"); }
    Fixture legacy(true); BoneFrameTransform(legacy.Raw(), 0, 0, nullptr, 0);
    std::ifstream stream(trace, std::ios::binary); std::string line; std::getline(stream, line);
    Check(line.find("{\"bone_count\":4") == 0 && line.find("\"chain\":0") != std::string::npos,
        "legacy probe remains armed and starts at record one, overwriting the sentinel");
    Check(std::filesystem::exists(precheck), "legacy IK also reaches the precheck diagnostic point");
    std::printf("Diagnostic output: %s\n", output.path.string().c_str());
}
#endif
}
int main() {
    try {
        auto context = BoneTransformContext::Create(); Check(bool(context), "D3DX CPU context is required for lifecycle verification");
#if !MMD_ORIGINAL_PROFILE
        ScaleIsolation(*context);
#endif
        SharedLifetime(); AllocationAndRepeatedLifetime(*context);
#ifdef MIKUMIKUSTUDIO_DIAG
        ProbeIsolation(*context);
#endif
        context.reset();
        std::puts("Bone transform context lifecycle passed"); return 0;
    } catch (const std::exception& e) { allocationCountdown = -1; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
