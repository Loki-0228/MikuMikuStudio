// Frozen free-curve sampling owns all its inputs and never reconciles a live
// model from the sampling path. No window, renderer or physical device needed.
#define NOMINMAX
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include <array>
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
void* operator new(std::size_t size) {
    if (failAfter == 0) throw std::bad_alloc();
    if (failAfter > 0) --failAfter;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mikumikustudio;
namespace bc = bone_curve;
namespace ac = animation_curve;
using Author = bc::AuthoringState;
using Frozen = bc::SamplingState;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 2> bones{};
    std::array<unsigned char, 2> selection{}, physics{};
    Model() {
        record->boneCount = 2; record->boneTable = bones.data();
        record->boneKeys = mdl::BoneKeyPoolCreate(32);
        Check(record->boneKeys != nullptr, "allocate sampling fixture pool");
        record->boneSelection = selection.data(); record->bonePhysicsState = physics.data();
        std::memcpy(bones[0].name, "sample", 7); std::memcpy(bones[1].name, "other", 6);
        for (unsigned i = 0; i < bones.size(); ++i) {
            bones[i].parent = -1; bones[i].rotQuat[3] = 1;
            for (unsigned k = 0; k < 16; ++k) bones[i].matInit[k] = k % 5 == 0 ? 1.f : 0.f;
            record->boneKeys[i].rotation[3] = 1; record->boneKeys[i].allocated = 1;
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
    unsigned Slot(unsigned frame) const {
        for (unsigned slot = 0, hops = 0; hops < mdl::BoneKeyCapacity(record->boneKeys); ++hops) {
            Check(slot < mdl::BoneKeyCapacity(record->boneKeys), "fixture chain stays in pool");
            if (record->boneKeys[slot].frame == frame) return slot;
            if (!record->boneKeys[slot].next) break;
            slot = record->boneKeys[slot].next;
        }
        Check(false, "fixture frame exists"); return 0;
    }
    ~Model() {
        bc::Forget(raw()); bone_scale::Forget(raw());
        mdl::BoneKeyPoolDestroy(record->boneKeys);
        for (auto& ring : record->undoRings) for (auto& item : ring.slots) {
            undo_storage::Free(item.bonePose); undo_storage::Free(item.auxiliaryPose);
        }
    }
};
bc::Tracks Tracks() {
    bc::Tracks tracks;
    for (int lane = 0; lane < 9; ++lane) {
        ac::Key first, last; last.frame = 20;
        first.tangent = last.tangent = ac::Tangent::Free;
        first.value = last.value = lane >= 6 ? 1 : 0;
        if (lane == 0) { last.value = 10; first.outSlope = 2; last.inSlope = -.5; first.outWeight = .2; last.inWeight = .4; }
        if (lane == 2) last.value = .1234567890123;
        if (lane == 3) last.value = 30;
        if (lane == 4) last.value = 450;
        if (lane == 5) last.value = -45;
        if (lane >= 6) last.value = 1 + (lane - 5) * .25;
        tracks[lane].keys = {first, last};
    }
    std::size_t at = 0; std::string error;
    Check(ac::InsertPreservingShape(tracks[0], 9, at, error) == ac::InsertResult::Inserted,
        "add sparse high precision authored lane");
    return tracks;
}
void Write(Model& model, const bc::Tracks& tracks) {
    std::string error;
    Check(bc::Write(model.raw(), 0, tracks, error), "write free-curve fixture");
}
struct SourceState {
    std::unique_ptr<Author> author;
    std::unique_ptr<mdl::ModelRecord> record;
    std::array<mdl::BoneRecord, 2> bones;
    std::vector<mdl::BoneKey> keys;
    bone_scale::Snapshot scale;
    explicit SourceState(const Model& model) : author(Author::Capture(model.raw())),
        record(std::make_unique<mdl::ModelRecord>(*model.record)), bones(model.bones),
        keys(model.record->boneKeys, model.record->boneKeys + mdl::BoneKeyCapacity(model.record->boneKeys)),
        scale(bone_scale::Capture(model.raw())) {}
    void Same(const Model& model) const {
        Check(author->Equals(*Author::Capture(model.raw())), "capture preserves source authoring and native baseline");
        Check(!std::memcmp(record.get(), model.record.get(), sizeof(*record)), "capture preserves source model and undo metadata");
        Check(!std::memcmp(bones.data(), model.bones.data(), sizeof(bones)), "capture preserves source edit pose");
        Check(keys.size() == mdl::BoneKeyCapacity(model.record->boneKeys) &&
            !std::memcmp(keys.data(), model.record->boneKeys, keys.size() * sizeof(mdl::BoneKey)), "capture preserves source key pool");
        const auto current = bone_scale::Capture(model.raw());
        Check(scale.keys == current.keys && scale.curves == current.curves && scale.pose == current.pose,
            "capture preserves source scale state");
    }
};
struct Sample {
    std::array<float, 3> position{91, 92, 93};
    std::array<float, 4> rotation{81, 82, 83, 84};
    bone_scale::Matrix scale{{2, .5f, 0, 0, 3, .25f, .125f, 0, 4}};
};
bool Near(const Sample& a, const Sample& b) {
    for (unsigned i = 0; i < a.position.size(); ++i) if (!std::isfinite(a.position[i]) || std::abs(a.position[i] - b.position[i]) > 2e-6f) return false;
    for (unsigned i = 0; i < a.rotation.size(); ++i) if (!std::isfinite(a.rotation[i]) || std::abs(a.rotation[i] - b.rotation[i]) > 2e-6f) return false;
    for (unsigned i = 0; i < a.scale.v.size(); ++i) if (!std::isfinite(a.scale.v[i]) || std::abs(a.scale.v[i] - b.scale.v[i]) > 2e-6f) return false;
    return true;
}
Sample Reference(Model& reference, const bc::Tracks& tracks, double frame) {
    Sample result;
    Check(bone_scale::SetPose(reference.raw(), 0, result.scale), "set reference scale basis");
    bc::Evaluate(reference.raw(), frame);
    std::memcpy(result.position.data(), reference.bones[0].trans, sizeof(reference.bones[0].trans));
    std::memcpy(result.rotation.data(), reference.bones[0].rotQuat, sizeof(reference.bones[0].rotQuat));
    result.scale = bone_scale::Pose(reference.raw(), 0);
    for (unsigned axis = 0; axis < 3; ++axis)
        Check(result.position[axis] == float(ac::Evaluate(tracks[axis], frame).value), "reference retains exact free translation until final float conversion");
    return result;
}
bool Read(const Frozen& frozen, double frame, Sample& out, int bone = 0) noexcept {
    return frozen.Sample(bone, frame, out.position.data(), out.rotation.data(), out.scale);
}
template<class Exception, class F> bool Throws(F&& call) {
    try { call(); } catch (const Exception&) { return true; } catch (...) {}
    return false;
}
void InvalidSamples(const Frozen& frozen) {
    for (double frame : {-1., -.25, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), double(frame_limits::maximum) + 1}) {
        Sample out; const auto before = out;
        Check(!Read(frozen, frame, out), "invalid sample time is refused");
        Check(out.position == before.position && out.rotation == before.rotation && out.scale == before.scale,
            "invalid time leaves every output untouched");
    }
    for (int bone : {-1, 1, (std::numeric_limits<int>::max)()}) {
        Sample out; const auto before = out;
        Check(!Read(frozen, 0, out, bone), "missing bone is refused");
        Check(out.position == before.position && out.rotation == before.rotation && out.scale == before.scale,
            "missing bone leaves every output untouched");
    }
    Sample out; const auto before = out;
    Check(!frozen.Sample(0, 0, nullptr, out.rotation.data(), out.scale), "null position is refused");
    Check(out.position == before.position && out.rotation == before.rotation && out.scale == before.scale,
        "null position leaves valid outputs untouched");
    Check(!frozen.Sample(0, 0, out.position.data(), nullptr, out.scale), "null quaternion is refused");
    Check(out.position == before.position && out.rotation == before.rotation && out.scale == before.scale,
        "null quaternion leaves valid outputs untouched");
}
void LifetimeAndConcurrency() {
    const auto tracks = Tracks();
    constexpr std::array<double, 9> times{0, .125, 1.25, 9, 12.75, 19.9, 20, 30, double(frame_limits::maximum)};
    std::array<Sample, times.size()> expected{};
    Model reference; Write(reference, tracks);
    for (std::size_t i = 0; i < times.size(); ++i) expected[i] = Reference(reference, tracks, times[i]);
    std::unique_ptr<Frozen> frozen;
    {
        Model source; Write(source, tracks); const SourceState before(source);
        frozen = Frozen::Capture(source.raw()); before.Same(source);
        Check(frozen->Contains(0) && !frozen->Contains(1) && !frozen->Contains(-1), "Contains distinguishes authored and missing bones");
        Check(frozen->RetainedBytes() > sizeof(Frozen), "snapshot reports retained track storage");
        auto changed = tracks; changed[0].keys.back().value = 900;
        Write(source, changed);
        Sample old; Check(Read(*frozen, 20, old) && Near(old, expected[6]), "source edits do not mutate frozen samples");
        const auto newer = Frozen::Capture(source.raw());
        Sample current; Check(Read(*newer, 20, current) && current.position[0] == 900, "new capture observes later source revision");
    }
    // All source native data and side tables are now destroyed. The frozen
    // evaluator must not consult any pointer or source identity during reads.
    for (std::size_t i = 0; i < times.size(); ++i) {
        Sample out;
        Check(Read(*frozen, times[i], out) && Near(out, expected[i]), "sampling survives destruction and matches production Euler/scale evaluation");
    }
    InvalidSamples(*frozen);
    std::array<bool, 4> workerPassed{};
    std::array<std::thread, 4> workers;
    for (unsigned worker = 0; worker < workers.size(); ++worker) workers[worker] = std::thread([&, worker] {
        bool passed = true;
        failAfter = 0;
        for (unsigned repeat = 0; repeat < 1000; ++repeat) {
            const auto index = (repeat * 7 + worker) % times.size();
            Sample out;
            passed = Read(*frozen, times[index], out) && Near(out, expected[index]) && frozen->Contains(0) && passed;
        }
        failAfter = -1;
        workerPassed[worker] = passed;
    });
    for (auto& worker : workers) worker.join();
    for (bool passed : workerPassed) Check(passed, "concurrent out-of-order sampling after source destruction is allocation-free");
}
void PendingNativeAndBudget() {
    Model source; const auto tracks = Tracks(); Write(source, tracks);
    source.record->boneKeys[0].position[0] = 123;
    auto scale = bone_scale::Key(source.raw(), int(source.Slot(20))); scale.v[0] = 1.75f;
    bone_scale::SetKey(source.raw(), int(source.Slot(20)), scale);
    const SourceState before(source);
    bc::Tracks reconciled;
    Check(bc::ReadDetached(source.raw(), 0, reconciled), "read private reconciliation oracle");
    before.Same(source);
    Model reference; Write(reference, reconciled);
    const auto frozen = Frozen::Capture(source.raw()); before.Same(source);
    for (double frame : {0., 1.25, 9., 12.75, 20.}) {
        Sample out; const auto expected = Reference(reference, reconciled, frame);
        Check(Read(*frozen, frame, out) && Near(out, expected), "capture includes unsynchronized native edits only in its owned tracks");
    }
    const auto bytes = frozen->RetainedBytes();
    Check(bytes > 0, "nonempty snapshot has nonzero retained charge");
    Check(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), bytes - 1); }), "one-byte-short budget refuses capture");
    before.Same(source);
    const auto exact = Frozen::Capture(source.raw(), bytes);
    Check(exact->RetainedBytes() == bytes, "exact retained budget succeeds"); before.Same(source);
    Check(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), 0); }), "zero budget cannot publish partial sampling state");
    before.Same(source);
    bool complete = false; unsigned failures = 0;
    for (long point = 0; point < 1024; ++point) {
        bool memory = false, other = false;
        std::unique_ptr<Frozen> candidate;
        failAfter = point;
        try { candidate = Frozen::Capture(source.raw()); }
        catch (const std::bad_alloc&) { memory = true; }
        catch (...) { other = true; }
        failAfter = -1;
        Check(!other, "capture allocation failure preserves bad_alloc category"); before.Same(source);
        Sample old;
        Check(Read(*frozen, 0, old) && old.position[0] == 123, "failed recapture leaves prior snapshot usable");
        if (!memory) { Check(candidate && candidate->Contains(0), "allocation sweep publishes a complete final snapshot"); complete = true; break; }
        ++failures;
        Check(!candidate, "allocation failure returns no partial snapshot");
    }
    Check(complete && failures > 0, "allocation sweep reaches first successful complete capture");
    std::printf("sampling allocation failure points=%u\n", failures);
}
void MissingStaleAndDeferred() {
    Check(Throws<std::invalid_argument>([] { Frozen::Capture(nullptr); }), "null source is invalid_argument");
    Model empty;
    const auto none = Frozen::Capture(empty.raw());
    Check(!none->Contains(0) && !none->Contains(1), "missing author produces empty sampling state");
    Sample output; const auto beforeOutput = output;
    Check(!Read(*none, 0, output) && output.position == beforeOutput.position && output.rotation == beforeOutput.rotation && output.scale == beforeOutput.scale,
        "empty snapshot cannot fabricate a bone sample");
    Check(Throws<std::length_error>([&] { Frozen::Capture(empty.raw(), 0); }), "empty snapshot overhead is budgeted");
    Model source, alternate; Write(source, Tracks());
    const auto original = Author::Capture(source.raw());
    auto* identity = source.record->boneTable;
    source.record->boneTable = alternate.bones.data();
    const SourceState stale(source);
    const auto staleSample = Frozen::Capture(source.raw());
    Check(!staleSample->Contains(0), "stale source identity produces empty snapshot"); stale.Same(source);
    source.record->boneTable = identity;
    Check(original->Equals(*Author::Capture(source.raw())), "stale capture does not prune old author storage");
    Check(Frozen::Capture(source.raw())->Contains(0), "restored source identity retains authored curves");
    {
        bc::RangeRetime deferred(source.raw(), 0, 20, 1);
        Check(deferred.Ready(), "prepare deferred source edit");
        const SourceState pending(source);
        Check(Throws<std::invalid_argument>([&] { Frozen::Capture(source.raw()); }), "active deferred edit cannot be frozen midway");
        pending.Same(source);
        Check(deferred.Complete(), "complete unchanged deferred edit after refused capture");
    }
    const SourceState intact(source);
    const auto last = source.Slot(20);
    const auto oldNext = source.record->boneKeys[last].next;
    source.record->boneKeys[last].next = unsigned(mdl::BoneKeyCapacity(source.record->boneKeys) + 1);
    const SourceState invalid(source);
    Check(Throws<std::invalid_argument>([&] { Frozen::Capture(source.raw()); }), "malformed native chain refuses the whole snapshot");
    invalid.Same(source);
    source.record->boneKeys[last].next = oldNext; intact.Same(source);
}
}
int main() {
    LifetimeAndConcurrency(); PendingNativeAndBudget(); MissingStaleAndDeferred();
    std::printf("PASS %u frozen bone curve sampling checks\n", checks);
    return 0;
}
