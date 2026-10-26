// CopyState must reconcile privately: copying is not an edit of either model.
#define NOMINMAX
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
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
        record->boneCount = 2; record->selectedBone = 0; record->boneTable = bones.data();
        record->boneKeys = mdl::BoneKeyPoolCreate(32);
        record->boneSelection = selection.data(); record->bonePhysicsState = physics.data();
        std::memcpy(bones[0].name, "first", 6); std::memcpy(bones[1].name, "second", 7);
        for (unsigned i = 0; i < 2; ++i) {
            bones[i].parent = -1; bones[i].rotQuat[3] = 1;
            for (unsigned k = 0; k < 16; ++k) bones[i].matInit[k] = k % 5 == 0 ? 1.f : 0.f;
            record->boneKeys[i].rotation[3] = 1; record->boneKeys[i].allocated = 1;
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
    unsigned Slot(unsigned bone, unsigned frame) const {
        for (unsigned slot = bone, hops = 0; hops < mdl::BoneKeyCapacity(record->boneKeys); ++hops) {
            if (record->boneKeys[slot].frame == frame) return slot;
            if (!record->boneKeys[slot].next) break;
            slot = record->boneKeys[slot].next;
        }
        Check(false, "fixture native key exists"); return 0;
    }
    ~Model() {
        bc::Forget(raw()); bone_scale::Forget(raw());
        mdl::BoneKeyPoolDestroy(record->boneKeys);
        for (auto& ring : record->undoRings) for (auto& entry : ring.slots) {
            undo_storage::Free(entry.bonePose); undo_storage::Free(entry.auxiliaryPose);
        }
    }
};

bc::Tracks Tracks(double value, bool sparse = true) {
    bc::Tracks tracks;
    for (int lane = 0; lane < 9; ++lane) {
        ac::Key first, last; last.frame = 20;
        first.tangent = last.tangent = ac::Tangent::Free;
        first.value = last.value = lane >= 6 ? 1 : 0;
        if (lane == 0) { last.value = value; first.outSlope = .2; last.inSlope = -.1; first.outWeight = .2; last.inWeight = .4; }
        if (lane == 2) last.value = .1234567890123;
        if (lane == 4) last.value = 450;
        tracks[lane].keys = {first, last};
    }
    if (sparse) {
        std::string error; std::size_t at = 0;
        Check(ac::InsertPreservingShape(tracks[0], 9, at, error) == ac::InsertResult::Inserted, "create independently timed lane");
    }
    return tracks;
}
void Write(Model& model, unsigned bone, double value) {
    std::string error;
    Check(bc::Write(model.raw(), int(bone), Tracks(value), error), "write production curve fixture");
}
std::vector<std::uint8_t> Encoded(const bc::Tracks& tracks) {
    std::vector<ac::NamedTrack> named;
    for (unsigned lane = 0; lane < tracks.size(); ++lane) named.push_back({std::to_string(lane), tracks[lane]});
    std::vector<std::uint8_t> result; std::string error;
    Check(ac::Encode(named, result, error), "encode exact comparison tracks"); return result;
}
void MirrorNative(const Model& source, Model& destination) {
    const auto count = mdl::BoneKeyCapacity(source.record->boneKeys);
    Check(mdl::BoneKeyPoolEnsure(*destination.record, count), "prepare destination native pool");
    std::memcpy(destination.record->boneKeys, source.record->boneKeys, count * sizeof(mdl::BoneKey));
    // bone_scale::CopyState calls the function being tested, so prepare its
    // independent prerequisite by copying only scale keys and their controls.
    for (std::size_t i = 0; i < count; ++i) {
        bone_scale::SetKey(destination.raw(), int(i), bone_scale::Key(source.raw(), int(i)));
        bone_scale::SetKeyCurve(destination.raw(), int(i), bone_scale::KeyCurve(source.raw(), int(i)));
    }
}
struct NativeState {
    std::unique_ptr<mdl::ModelRecord> record;
    std::array<mdl::BoneRecord, 2> bones;
    std::array<unsigned char, 2> selection, physics;
    std::vector<mdl::BoneKey> keys;
    bone_scale::Snapshot scale;
    bool applied;
    explicit NativeState(const Model& model) : record(std::make_unique<mdl::ModelRecord>(*model.record)),
        bones(model.bones), selection(model.selection), physics(model.physics),
        keys(model.record->boneKeys, model.record->boneKeys + mdl::BoneKeyCapacity(model.record->boneKeys)),
        scale(bone_scale::Capture(model.raw())), applied(bone_scale::TransformsApplied(model.raw())) {}
    void Same(const Model& model) const {
        const auto current = bone_scale::Capture(model.raw());
        Check(!std::memcmp(record.get(), model.record.get(), sizeof(*record)), "copy preserves native model and undo metadata");
        Check(!std::memcmp(bones.data(), model.bones.data(), sizeof(bones)), "copy preserves edited bone poses and matrices");
        Check(selection == model.selection && physics == model.physics, "copy preserves selection and physics state");
        Check(keys.size() == mdl::BoneKeyCapacity(model.record->boneKeys) &&
            !std::memcmp(keys.data(), model.record->boneKeys, keys.size() * sizeof(mdl::BoneKey)), "copy preserves entire native key pool");
        Check(scale.pose == current.pose && scale.keys == current.keys && scale.curves == current.curves &&
            applied == bone_scale::TransformsApplied(model.raw()), "copy preserves scale authoring and transform cache flag");
    }
};
void SameAuthor(const Model& model, const Author& expected) {
    Check(Author::Capture(model.raw())->Equals(expected), "live authoring remains byte-semantically identical");
}
template<class F> bool Refused(F&& call) {
    try { call(); } catch (const std::exception&) { return true; }
    return false;
}
void SameDetached(const Model& source, const Model& destination) {
    for (unsigned bone = 0; bone < 2; ++bone) {
        bc::Tracks a, b;
        Check(bc::ReadDetached(source.raw(), int(bone), a) && bc::ReadDetached(destination.raw(), int(bone), b), "read copied authored tracks without live reconciliation");
        Check(Encoded(a) == Encoded(b), "copied curves equal privately reconciled source including sparse keys and precision");
        for (double frame : {0., 1.25, 9., 12.75, 20.}) for (unsigned lane = 0; lane < 9; ++lane) {
            const auto x = ac::Evaluate(a[lane], frame), y = ac::Evaluate(b[lane], frame);
            Check(x.value == y.value && x.derivative == y.derivative, "copied fractional sampling matches source value and derivative");
        }
    }
}
void PendingNativeEdit(Model& source) {
    source.record->boneKeys[0].position[0] = 123;
    auto& end = source.record->boneKeys[source.Slot(0, 20)];
    end.rotation[1] = -end.rotation[1]; end.rotation[3] = -end.rotation[3];
    auto scale = bone_scale::Key(source.raw(), int(source.Slot(0, 20))); scale.v[0] = 1.75f;
    bone_scale::SetKey(source.raw(), int(source.Slot(0, 20)), scale);
    source.bones[0].trans[2] = 81.25f; source.bones[1].rotQuat[1] = .125f;
    source.selection[1] = 1; source.physics[0] = 1;
}

void ConstSourceAndHistory() {
    Model source, target;
    Write(source, 0, 7); Write(source, 1, 17);
    const auto oldSource = Author::Capture(source.raw()); bc::CaptureUndo(source.raw(), 3);
    Write(source, 0, 10); Write(source, 1, 20);
    Write(target, 0, 70); Write(target, 1, 80); bc::CaptureUndo(target.raw(), 7);
    Write(target, 0, 90);
    PendingNativeEdit(source); MirrorNative(source, target);
    target.bones[0].trans[0] = -52.f;
    const auto before = Author::Capture(source.raw());
    const NativeState sourceNative(source), targetNative(target);
    bc::CopyState(source.raw(), target.raw());
    SameAuthor(source, *before); sourceNative.Same(source); targetNative.Same(target); SameDetached(source, target);
    SameAuthor(source, *before);
    const auto copied = Author::Capture(target.raw());
    bc::Undo(target.raw(), 7); SameAuthor(target, *copied);
    bc::Undo(target.raw(), 3); SameAuthor(target, *copied);
    bc::Redo(target.raw(), 7); SameAuthor(target, *copied);
    bc::Redo(target.raw(), 3); SameAuthor(target, *copied);
    bc::CopyState(source.raw(), target.raw()); SameAuthor(source, *before); SameAuthor(target, *copied);
    bc::Undo(source.raw(), 3); SameAuthor(source, *oldSource);
    bc::Redo(source.raw(), 3); SameAuthor(source, *before);
    sourceNative.Same(source); targetNative.Same(target);
}

void SelfAndMissingSource() {
    Model source, target, unregistered;
    Write(source, 0, 5); const auto undo = Author::Capture(source.raw()); bc::CaptureUndo(source.raw(), 2);
    Write(source, 0, 10); PendingNativeEdit(source);
    const auto before = Author::Capture(source.raw()); const NativeState native(source);
    failAfter = 0; bool threw = false;
    try { bc::CopyState(source.raw(), source.raw()); bc::CopyState(source.raw(), nullptr); bc::CopyState(nullptr, nullptr); }
    catch (...) { threw = true; }
    failAfter = -1;
    Check(!threw, "self and null-destination copy are allocation-free no-ops");
    SameAuthor(source, *before); native.Same(source);
    bc::Undo(source.raw(), 2); SameAuthor(source, *undo);
    bc::Redo(source.raw(), 2); SameAuthor(source, *before);
    Write(target, 0, 30); bc::CaptureUndo(target.raw(), 4);
    const NativeState targetNative(target);
    bc::CopyState(unregistered.raw(), target.raw());
    bc::Tracks tracks;
    Check(!Author::Capture(target.raw())->Read(0, tracks), "missing source clears target curve authoring");
    targetNative.Same(target); SameAuthor(source, *before);
    bc::Undo(target.raw(), 4); Check(!Author::Capture(target.raw())->Read(0, tracks), "missing source does not leave destination rings behind");
    Write(target, 0, 31); const NativeState another(target);
    bc::CopyState(nullptr, target.raw());
    Check(!Author::Capture(target.raw())->Read(0, tracks), "null source clears destination curves"); another.Same(target);
}

void StaleIdentityAndDeferred() {
    Model source, target, different;
    Write(source, 0, 10); Write(source, 1, 20); Write(target, 0, 70);
    const auto original = Author::Capture(source.raw());
    auto* identity = source.record->boneTable; source.record->boneTable = different.bones.data();
    const auto stale = Author::Capture(source.raw()); const NativeState srcNative(source), dstNative(target);
    bc::CopyState(source.raw(), target.raw());
    SameAuthor(source, *stale); srcNative.Same(source); dstNative.Same(target);
    bc::Tracks tracks; Check(!Author::Capture(target.raw())->Read(0, tracks), "stale source identity clears destination");
    source.record->boneTable = identity; SameAuthor(source, *original);
    Write(target, 0, 80);
    {
        bc::RangeRetime deferred(source.raw(), 0, 20, 1);
        Check(deferred.Ready(), "create active deferred native edit");
        const auto sourceBefore = Author::Capture(source.raw()), targetBefore = Author::Capture(target.raw());
        const NativeState sourceState(source), targetState(target);
        Check(Refused([&] { bc::CopyState(source.raw(), target.raw()); }), "deferred source cannot publish an intermediate curve copy");
        SameAuthor(source, *sourceBefore); SameAuthor(target, *targetBefore);
        sourceState.Same(source); targetState.Same(target);
        bc::CopyState(source.raw(), source.raw()); SameAuthor(source, *sourceBefore);
        Check(deferred.Complete(), "complete unchanged deferred edit after refused copy");
    }
}

void DeferredDestination() {
    for (bool complete : {false, true}) {
        Model source, target;
        Write(source, 0, 5); Write(source, 1, 15);
        const auto sourceUndo = Author::Capture(source.raw()); bc::CaptureUndo(source.raw(), 6);
        Write(source, 0, 10);
        Write(target, 0, 35); Write(target, 1, 45);
        const auto targetUndo = Author::Capture(target.raw()); bc::CaptureUndo(target.raw(), 8);
        Write(target, 0, 70);
        const auto sourceBefore = Author::Capture(source.raw()), targetBefore = Author::Capture(target.raw());
        const NativeState sourceNative(source), targetNative(target);
        {
            bc::RangeRetime deferred(target.raw(), 0, 20, 1);
            Check(deferred.Ready(), "create active destination range edit");
            for (const auto* input : std::array<const unsigned char*, 2>{source.raw(), nullptr}) {
                Check(Refused([&] { bc::CopyState(input, target.raw()); }),
                    "active destination refuses both copy and null-source clearing");
                SameAuthor(source, *sourceBefore); SameAuthor(target, *targetBefore);
                sourceNative.Same(source); targetNative.Same(target);
            }
            if (complete) {
                Check(deferred.Complete(), "refused copy leaves destination range edit completable");
                Check(deferred.Complete(), "completed destination range edit remains idempotent");
            }
        }
        SameAuthor(source, *sourceBefore); SameAuthor(target, *targetBefore);
        sourceNative.Same(source); targetNative.Same(target);
        // A copied-away/decremented ownership count would leave this new scope
        // blocked. Exercise both explicit completion and destructor recovery.
        {
            bc::RangeRetime next(target.raw(), 0, 20, 1);
            Check(next.Ready() && next.Complete(), "destination deferred count is balanced after refused copy");
        }
        bc::Undo(source.raw(), 6); SameAuthor(source, *sourceUndo);
        bc::Redo(source.raw(), 6); SameAuthor(source, *sourceBefore);
        bc::Undo(target.raw(), 8); SameAuthor(target, *targetUndo);
        bc::Redo(target.raw(), 8); SameAuthor(target, *targetBefore);
        sourceNative.Same(source); targetNative.Same(target);
        // Once the range owner has left, copying and clearing must work again.
        MirrorNative(source, target); bc::CopyState(source.raw(), target.raw()); SameDetached(source, target);
        bc::CopyState(nullptr, target.raw()); bc::Tracks tracks;
        Check(!Author::Capture(target.raw())->Read(0, tracks), "destination accepts clearing after deferred edit ends");
        SameAuthor(source, *sourceBefore);
    }
}

void InvalidChains() {
    for (bool badSource : {true, false}) {
        Model source, target;
        Write(source, 0, 10); Write(source, 1, 20); Write(target, 0, 70); Write(target, 1, 80);
        const auto targetUndo = Author::Capture(target.raw()); bc::CaptureUndo(target.raw(), 5); Write(target, 0, 90);
        MirrorNative(source, target);
        auto& bad = badSource ? source : target;
        // Break bone 1 after bone 0 has already been prepared privately.
        bad.record->boneKeys[1].next = unsigned(mdl::BoneKeyCapacity(bad.record->boneKeys) + 1);
        const auto a = Author::Capture(source.raw()), b = Author::Capture(target.raw());
        const NativeState beforeSource(source), beforeTarget(target);
        Check(Refused([&] { bc::CopyState(source.raw(), target.raw()); }), "invalid source or destination native chain rejects entire copy");
        SameAuthor(source, *a); SameAuthor(target, *b); beforeSource.Same(source); beforeTarget.Same(target);
        bc::Undo(target.raw(), 5); SameAuthor(target, *targetUndo);
        bc::Redo(target.raw(), 5); SameAuthor(target, *b);
    }
}

void AllocationFailures(bool destinationExists) {
    Model source, target;
    Write(source, 0, 8); Write(source, 1, 18);
    const auto sourceUndo = Author::Capture(source.raw()); bc::CaptureUndo(source.raw(), 3);
    Write(source, 0, 10); Write(source, 1, 20);
    std::unique_ptr<Author> targetUndo;
    if (destinationExists) {
        Write(target, 0, 70); Write(target, 1, 80);
        targetUndo = Author::Capture(target.raw()); bc::CaptureUndo(target.raw(), 5); Write(target, 0, 90);
    }
    PendingNativeEdit(source); MirrorNative(source, target);
    const auto sourceBefore = Author::Capture(source.raw()), targetBefore = Author::Capture(target.raw());
    const NativeState sourceNative(source), targetNative(target);
    bool succeeded = false; unsigned failures = 0;
    for (long limit = 0; limit < 1024; ++limit) {
        bool memory = false, other = false;
        failAfter = limit;
        try { bc::CopyState(source.raw(), target.raw()); }
        catch (const std::bad_alloc&) { memory = true; }
        catch (...) { other = true; }
        failAfter = -1;
        Check(!other, "allocation failure remains bad_alloc rather than a partial-success result");
        SameAuthor(source, *sourceBefore); sourceNative.Same(source); targetNative.Same(target);
        if (!memory) { succeeded = true; break; }
        ++failures; SameAuthor(target, *targetBefore);
        // The public author snapshot intentionally excludes legacy history;
        // exercise those rings separately after every failed allocation point.
        bc::Undo(source.raw(), 3); SameAuthor(source, *sourceUndo);
        bc::Redo(source.raw(), 3); SameAuthor(source, *sourceBefore);
        if (targetUndo) {
            bc::Undo(target.raw(), 5); SameAuthor(target, *targetUndo);
            bc::Redo(target.raw(), 5); SameAuthor(target, *targetBefore);
        }
    }
    Check(succeeded && failures != 0, "allocation sweep reaches first successful complete copy");
    SameDetached(source, target); SameAuthor(source, *sourceBefore);
    const auto copied = Author::Capture(target.raw());
    bc::Undo(target.raw(), 3); bc::Undo(target.raw(), 5); SameAuthor(target, *copied);
    bc::Redo(target.raw(), 3); bc::Redo(target.raw(), 5); SameAuthor(target, *copied);
    std::printf("copy allocation sweep destination=%s failures=%u\n", destinationExists ? "existing" : "missing", failures);
}
}

int main() {
    ConstSourceAndHistory(); SelfAndMissingSource(); StaleIdentityAndDeferred(); DeferredDestination(); InvalidChains();
    AllocationFailures(false); AllocationFailures(true);
    std::printf("PASS %u bone curve copy checks\n", checks);
    return 0;
}
