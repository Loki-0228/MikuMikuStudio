// Frozen registered bone channels: native parity, source isolation and lifetime.
#define NOMINMAX
#include "mikumikustudio/bone_motion_snapshot.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace { thread_local long failAfter = -1; }
void* operator new(std::size_t bytes) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio;
namespace bc = bone_curve;
namespace ac = animation_curve;
using Snapshot = BoneMotionSnapshot;
using Status = BoneMotionStatus;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 2> bones{};
    std::array<unsigned char, 2> selected{}, physics{};
    mdl::DisplayKey display{};
    Model() {
        record->boneCount = unsigned(bones.size()); record->boneTable = bones.data();
        record->boneKeys = mdl::BoneKeyPoolCreate(32);
        record->boneSelection = selected.data(); record->bonePhysicsState = physics.data();
        record->displayKeys = &display; display.visible = 1;
        std::memcpy(bones[0].name, "first", 6); std::memcpy(bones[1].name, "second", 7);
        for (unsigned i = 0; i < bones.size(); ++i) {
            bones[i].parent = bones[i].slotIndex = -1;
            bones[i].type = mdl::BoneType::RotateMove; bones[i].rotQuat[3] = 1;
            for (unsigned c = 0; c < 16; ++c) bones[i].matInit[c] = c % 5 == 0 ? 1.f : 0.f;
            auto& key = record->boneKeys[i]; key.rotation[3] = 1; key.allocated = 1;
        }
        auto& last = record->boneKeys[2]; last = record->boneKeys[0];
        record->boneKeys[0].next = 2; last.previous = 0; last.frame = 20;
        last.position[0] = 10; last.position[1] = 12.75f; last.position[2] = -.03125f;
        record->boneKeys[0].position[1] = -3.5f; last.physicsDisabled = 1;
        record->maxFrame = 20;
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
    ~Model() {
        bc::Forget(raw()); bone_scale::Forget(raw()); mdl::BoneKeyPoolDestroy(record->boneKeys);
        for (auto& ring : record->undoRings) for (auto& entry : ring.slots) {
            undo_storage::Free(entry.bonePose); undo_storage::Free(entry.auxiliaryPose);
        }
    }
};
struct NativeState {
    std::unique_ptr<mdl::ModelRecord> model;
    std::array<mdl::BoneRecord, 2> bones;
    std::array<unsigned char, 2> selected, physics;
    std::vector<mdl::BoneKey> keys;
    bone_scale::Snapshot scale;
    bool applied;
    explicit NativeState(const Model& source) : model(std::make_unique<mdl::ModelRecord>(*source.record)),
        bones(source.bones), selected(source.selected), physics(source.physics),
        keys(source.record->boneKeys, source.record->boneKeys + mdl::BoneKeyCapacity(source.record->boneKeys)),
        scale(bone_scale::Capture(source.raw())), applied(bone_scale::TransformsApplied(source.raw())) {}
    void Same(const Model& source) const {
        const auto now = bone_scale::Capture(source.raw());
        Check(!std::memcmp(model.get(), source.record.get(), sizeof(*model)), "snapshot leaves source model record unchanged");
        Check(!std::memcmp(bones.data(), source.bones.data(), sizeof(bones)), "snapshot leaves unregistered pose and matrices unchanged");
        Check(selected == source.selected && physics == source.physics, "snapshot leaves selected and physics flags unchanged");
        Check(keys.size() == mdl::BoneKeyCapacity(source.record->boneKeys) &&
            !std::memcmp(keys.data(), source.record->boneKeys, keys.size() * sizeof(mdl::BoneKey)), "snapshot leaves native pool and links unchanged");
        Check(scale.pose == now.pose && scale.keys == now.keys && scale.curves == now.curves &&
            applied == bone_scale::TransformsApplied(source.raw()), "snapshot leaves scale authoring and cache flag unchanged");
    }
};
void MirrorNative(const Model& source, Model& destination) {
    const auto count = mdl::BoneKeyCapacity(source.record->boneKeys);
    Check(mdl::BoneKeyPoolEnsure(*destination.record, count), "prepare independent native oracle pool");
    std::memcpy(destination.record->boneKeys, source.record->boneKeys, count * sizeof(mdl::BoneKey));
    for (std::size_t i = 0; i < count; ++i) {
        bone_scale::SetKey(destination.raw(), int(i), bone_scale::Key(source.raw(), int(i)));
        bone_scale::SetKeyCurve(destination.raw(), int(i), bone_scale::KeyCurve(source.raw(), int(i)));
    }
}
bool SameSample(const BoneMotionSample& a, const BoneMotionSample& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale && a.physicsDisabled == b.physicsDisabled;
}
BoneMotionSample Sentinel() {
    BoneMotionSample value;
    value.position = {101, 102, 103}; value.rotation = {104, 105, 106, 107};
    value.scale.v = {2, 3, 4, 5, 6, 7, 8, 9, 10}; value.physicsDisabled = true;
    return value;
}
bool Near(float a, float b, float tolerance = 2e-6f) { return std::fabs(a - b) <= tolerance; }
template<class Exception, class Function> bool Throws(Function&& function) {
    try { function(); } catch (const Exception&) { return true; }
    return false;
}
bc::Tracks FreeTracks(double end) {
    bc::Tracks result;
    for (unsigned lane = 0; lane < 9; ++lane) {
        ac::Key first, last; last.frame = 20; first.tangent = last.tangent = ac::Tangent::Free;
        first.value = last.value = lane >= 6 ? 1 : 0;
        if (lane == 0) { last.value = end; first.outSlope = 2; last.inSlope = -.5; first.outWeight = .2; last.inWeight = .4; }
        if (lane == 2) last.value = .1234567890123;
        if (lane == 4) last.value = 450;
        if (lane == 6) last.value = 1.75;
        if (lane == 7) last.value = .5;
        if (lane == 8) last.value = 2;
        result[lane].keys = {first, last};
    }
    std::string error; std::size_t at = 0;
    Check(ac::InsertPreservingShape(result[0], 9, at, error) == ac::InsertResult::Inserted, "prepare sparse free lane");
    return result;
}
void WriteFree(Model& model, double end) {
    std::string error;
    Check(bc::Write(model.raw(), 0, FreeTracks(end), error), "write actual free curve authoring");
}

void NativeParity() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Model source, oracle;
        auto& last = source.record->boneKeys[2];
        if (mode) {
            const float sign = mode == 1 ? 1.f : -1.f;
            last.rotation[1] = sign * .70710677f; last.rotation[3] = sign * .70710677f;
            for (unsigned lane = 0; lane < 4; ++lane) {
                last.interpolation[lane] = static_cast<unsigned char>(15 + lane * 3);
                last.interpolation[lane + 4] = static_cast<unsigned char>(90 - lane * 8);
                last.interpolation[lane + 8] = static_cast<unsigned char>(70 + lane * 5);
                last.interpolation[lane + 12] = static_cast<unsigned char>(115 - lane * 3);
            }
        }
        bone_scale::Matrix scale; scale.v = {2, .2f, 0, 0, 3, 0, 0, 0, .5f};
        bone_scale::SetKey(source.raw(), 2, scale);
        bone_scale::SetKeyCurve(source.raw(), 2, {10, 90, 90, 115});
        source.bones[0].trans[0] = 777; source.bones[1].trans[2] = -55;
        MirrorNative(source, oracle);
        const NativeState before(source); const auto frozen = Snapshot::Capture(source.raw()); before.Same(source);
        Check(frozen && frozen->BoneCount() == 2 && frozen->RetainedBytes() != 0, "capture two registered native bones");
        for (int frame : {0, 1, 4, 9, 10, 19, 20, 21, 200, frame_limits::maximum}) {
            BoneMotionSample sample;
            Check(frozen->SampleChannels(0, frame, sample) == Status::Ok, "sample native integer frame");
            SeekModelFrame(oracle.raw(), frame, 0);
            Check(!std::memcmp(sample.position.data(), oracle.bones[0].trans, sizeof(float) * 3), "native integer position preserves actual direct-seek bits");
            Check(!std::memcmp(sample.rotation.data(), oracle.bones[0].rotQuat, sizeof(float) * 4), "native integer quaternion preserves actual direct-seek bits");
            Check(sample.scale == bone_scale::Pose(oracle.raw(), 0), "native integer scale matches direct seek");
            Check(sample.physicsDisabled == (frame >= 20), "registered physics-disabled value is a discrete key channel");
        }
        for (double frame : {.125, 1.5, 9.25, 19.75}) {
            BoneMotionSample sample;
            Check(frozen->SampleChannels(0, frame, sample) == Status::Ok, "sample native fractional frame");
            Check(sample.scale == bone_scale::Sample(source.raw(), 0, frame), "fractional scale retains native curve and full matrix basis");
            if (!mode) Check(Near(sample.position[0], float(frame * .5)), "linear native channel has continuous fractional values");
            for (float component : sample.rotation) Check(std::isfinite(component), "fractional native rotation remains finite");
        }
        before.Same(source);
    }
}

void FreeCurvesAndConstCapture() {
    Model source; WriteFree(source, 10);
    const auto originalAuthor = bc::AuthoringState::Capture(source.raw()); bc::CaptureUndo(source.raw(), 4);
    // Deliberately leave this native edit unreconciled in the live author map.
    source.record->boneKeys[0].position[0] = 27.25f;
    source.bones[0].trans[0] = -987; source.bones[0].rotQuat[2] = .25f;
    const auto pendingAuthor = bc::AuthoringState::Capture(source.raw());
    Check(pendingAuthor->Equals(*originalAuthor), "fixture captures pending native edit without synchronizing authoring");
    bc::Tracks expected; Check(bc::ReadDetached(source.raw(), 0, expected), "independent detached curve oracle");
    const NativeState before(source);
    const auto frozen = Snapshot::Capture(source.raw());
    before.Same(source); Check(bc::AuthoringState::Capture(source.raw())->Equals(*pendingAuthor), "capture reconciles only its own frozen authoring");
    for (double frame : {0., .125, 2.5, 9., 12.75, 19.999, 20., 100.}) {
        BoneMotionSample sample; Check(frozen->SampleChannels(0, frame, sample) == Status::Ok, "sample private free motion");
        for (unsigned lane = 0; lane < 3; ++lane)
            Check(sample.position[lane] == float(ac::Evaluate(expected[lane], frame).value), "free position preserves sparse and high-precision authoring");
        const double half = ac::Evaluate(expected[4], frame).value * 3.14159265358979323846 / 360;
        Check(Near(sample.rotation[0], 0) && Near(sample.rotation[1], float(std::sin(half))) &&
            Near(sample.rotation[2], 0) && Near(sample.rotation[3], float(std::cos(half))), "free rotation preserves Euler winding rather than taking native shortest arc");
        for (unsigned lane = 0; lane < 3; ++lane)
            Check(Near(sample.scale.v[lane * 4], float(ac::Evaluate(expected[6 + lane], frame).value)), "free scale channels remain independently sampled");
    }
    before.Same(source); Check(bc::AuthoringState::Capture(source.raw())->Equals(*pendingAuthor), "sampling does not publish reconciled authoring");
    bc::Undo(source.raw(), 4); Check(bc::AuthoringState::Capture(source.raw())->Equals(*originalAuthor), "capture preserves source undo ring");
    bc::Redo(source.raw(), 4); Check(bc::AuthoringState::Capture(source.raw())->Equals(*pendingAuthor), "capture preserves source redo behavior");
}

void LifetimeAndParallelRead() {
    auto source = std::make_unique<Model>(); WriteFree(*source, 10);
    auto frozen = Snapshot::Capture(source->raw());
    constexpr std::array<double, 6> times{0., .125, 5.5, 9., 19.75, 20.};
    std::array<BoneMotionSample, times.size()> expected{};
    for (std::size_t i = 0; i < times.size(); ++i) Check(frozen->SampleChannels(0, times[i], expected[i]) == Status::Ok, "capture lifetime baseline");
    WriteFree(*source, 25); const auto changed = Snapshot::Capture(source->raw());
    BoneMotionSample oldEnd, newEnd;
    Check(frozen->SampleChannels(0, 20, oldEnd) == Status::Ok && changed->SampleChannels(0, 20, newEnd) == Status::Ok &&
        oldEnd.position[0] == 10 && newEnd.position[0] == 25, "old snapshot freezes prior motion while new snapshot captures edits");
    source.reset();
    std::atomic<unsigned> failures{0}; std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        failAfter = 0;
        for (unsigned repeat = 0; repeat < 300; ++repeat) for (std::size_t i = 0; i < times.size(); ++i) {
            BoneMotionSample sample;
            if (frozen->SampleChannels(0, times[i], sample) != Status::Ok || !SameSample(sample, expected[i])) ++failures;
        }
        failAfter = -1;
    });
    for (auto& worker : workers) worker.join();
    Check(failures == 0, "four threads sample after source deletion without any allocation or live lookup");
    Check(changed->SampleChannels(0, 20, newEnd) == Status::Ok && newEnd.position[0] == 25, "new revision also survives original model destruction");
}

void InvalidRequestsAndBones() {
    Model source; auto frozen = Snapshot::Capture(source.raw());
    const auto sentinel = Sentinel();
    for (int bone : {-1, 2, (std::numeric_limits<int>::max)()}) {
        auto output = sentinel;
        Check(frozen->SampleChannels(bone, 0, output) == Status::InvalidBone && SameSample(output, sentinel), "invalid bone leaves output unchanged");
    }
    for (double time : {-1., -.125, std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), double(frame_limits::maximum) + .5}) {
        auto output = sentinel;
        Check(frozen->SampleChannels(0, time, output) == Status::InvalidTime && SameSample(output, sentinel), "invalid time leaves output unchanged");
    }
    source.bones[0].slotIndex = 0; source.bones[1].type = mdl::BoneType::CoRotate;
    frozen = Snapshot::Capture(source.raw());
    auto output = sentinel;
    Check(frozen->SampleChannels(0, 5, output) == Status::ExternalParent && SameSample(output, sentinel), "external-parent dependent bone is explicitly refused");
    Check(frozen->SampleChannels(1, 5, output) == Status::UnsupportedBone && SameSample(output, sentinel), "derived bone is explicitly refused");
    source.bones[0].slotIndex = -1; source.bones[0].type = mdl::BoneType::InertTip; source.bones[1].type = mdl::BoneType::FixedAxis;
    frozen = Snapshot::Capture(source.raw());
    Check(frozen->SampleChannels(0, 5, output) == Status::UnsupportedBone && SameSample(output, sentinel), "inert tip is not reported as independently animated");
    Check(frozen->SampleChannels(1, 5, output) == Status::Ok, "fixed-axis registered channels remain supported before hierarchy evaluation");
    // Finite input keys can still overflow the native float subtraction during interpolation.
    Model extremes;
    extremes.record->boneKeys[0].position[0] = -(std::numeric_limits<float>::max)();
    extremes.record->boneKeys[2].position[0] = (std::numeric_limits<float>::max)();
    const auto overflowing = Snapshot::Capture(extremes.raw());
    output = sentinel;
    Check(overflowing->SampleChannels(0, 10, output) == Status::InvalidSample && SameSample(output, sentinel),
        "nonfinite interpolated result is refused without partially replacing output");
}

void InvalidSourcesAndBudget() {
    Check(Throws<std::invalid_argument>([] { Snapshot::Capture(nullptr); }), "null capture source is invalid");
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        Model source;
        if (scenario == 0) source.record->boneKeys[0].next = 1000;
        if (scenario == 1) source.record->boneKeys[2].next = 2;
        if (scenario == 2) source.record->boneKeys[2].frame = 0;
        if (scenario == 3) source.record->boneKeys[2].position[0] = std::numeric_limits<float>::quiet_NaN();
        if (scenario == 4) source.record->boneKeys[2].rotation[1] = std::numeric_limits<float>::infinity();
        if (scenario == 5) source.record->boneKeys[2].previous = 1;
        if (scenario == 6) source.record->boneKeys[2].frame = std::uint32_t(frame_limits::maximum) + 1;
        const NativeState before(source); const auto author = bc::AuthoringState::Capture(source.raw());
        Check(Throws<std::invalid_argument>([&] { Snapshot::Capture(source.raw()); }), "invalid reachable key or chain rejects capture");
        before.Same(source); Check(bc::AuthoringState::Capture(source.raw())->Equals(*author), "invalid capture preserves source authoring");
    }
    Model source; WriteFree(source, 10); const auto initial = Snapshot::Capture(source.raw());
    const auto bytes = initial->RetainedBytes(); Check(bytes > 0 && bytes <= 64ull * 1024 * 1024, "retained memory is charged within default budget");
    const NativeState before(source); const auto author = bc::AuthoringState::Capture(source.raw());
    Check(Throws<std::length_error>([&] { Snapshot::Capture(source.raw(), 0); }), "zero retained budget rejects nonempty snapshot");
    Check(Throws<std::length_error>([&] { Snapshot::Capture(source.raw(), bytes - 1); }), "one byte below retained charge is rejected");
    const auto exact = Snapshot::Capture(source.raw(), bytes);
    Check(exact->RetainedBytes() == bytes, "exact retained budget succeeds");
    before.Same(source); Check(bc::AuthoringState::Capture(source.raw())->Equals(*author), "budget failure never edits source");
    // Unreachable storage may contain recycled garbage and is not motion data.
    source.record->boneKeys[31].position[0] = std::numeric_limits<float>::quiet_NaN();
    source.record->boneKeys[31].next = 1000000;
    const auto unreachable = Snapshot::Capture(source.raw(), bytes);
    BoneMotionSample a, b;
    Check(unreachable->RetainedBytes() == bytes && initial->SampleChannels(0, 7.25, a) == Status::Ok &&
        unreachable->SampleChannels(0, 7.25, b) == Status::Ok && SameSample(a, b), "capture owns reachable keys only, not unrelated pool capacity or garbage");
}

void AllocationAtomicity() {
    Model source; WriteFree(source, 10);
    source.record->boneKeys[0].position[0] = 99;
    source.bones[0].trans[1] = -222;
    const NativeState native(source); const auto author = bc::AuthoringState::Capture(source.raw());
    bool completed = false; unsigned failures = 0;
    for (long limit = 0; limit < 1024; ++limit) {
        std::unique_ptr<Snapshot> frozen; bool memory = false, unexpected = false;
        failAfter = limit;
        try { frozen = Snapshot::Capture(source.raw()); }
        catch (const std::bad_alloc&) { memory = true; }
        catch (...) { unexpected = true; }
        failAfter = -1;
        Check(!unexpected, "allocation sweep reports bad_alloc without alternate failure");
        native.Same(source); Check(bc::AuthoringState::Capture(source.raw())->Equals(*author), "every capture allocation failure keeps pending authoring intact");
        if (frozen) {
            BoneMotionSample sample; Check(frozen->SampleChannels(0, 0, sample) == Status::Ok && sample.position[0] == 99, "fully prepared snapshot includes privately reconciled pending key");
            completed = true; break;
        }
        Check(memory, "failed capture allocation propagates rather than publishing partial object"); ++failures;
    }
    Check(completed && failures != 0, "allocation sweep reaches successful complete snapshot");
    std::printf("bone motion capture allocation failures=%u\n", failures);
}
}

int main() {
    NativeParity(); FreeCurvesAndConstCapture(); LifetimeAndParallelRead(); InvalidRequestsAndBones();
    InvalidSourcesAndBudget(); AllocationAtomicity();
    std::printf("PASS %u bone motion snapshot checks\n", checks);
    return 0;
}
