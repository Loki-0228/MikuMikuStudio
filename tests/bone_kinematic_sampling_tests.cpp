// Frozen direct-seek inputs only: no hierarchy, IK solve or physics execution.
#define NOMINMAX
#include "mikumikustudio/bone_motion_snapshot.hpp"
#include "mikumikustudio/display_motion_snapshot.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
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
#include <thread>
#include <vector>

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
namespace bc = bone_curve;
namespace ac = animation_curve;
using Motion = BoneMotionSnapshot;
using Display = DisplayMotionSnapshot;
using Status = BoneMotionStatus;
namespace {
unsigned checks = 0;
const char* currentCase = "setup";
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %s line %d: %s\n", currentCase, __LINE__, #x); std::exit(1); } } while (false)
template<class T> bool Bits(const T& a, const T& b) { return !std::memcmp(&a, &b, sizeof(T)); }
bool Equal(const BoneMotionSample& a, const BoneMotionSample& b) {
    return Bits(a.position, b.position) && Bits(a.rotation, b.rotation) &&
        Bits(a.scale, b.scale) && a.physicsDisabled == b.physicsDisabled;
}
bool Near(float actual, double expected, double tolerance = 2e-5) { return std::abs(double(actual) - expected) <= tolerance; }
BoneMotionSample Sentinel() {
    BoneMotionSample value; value.position = {101,102,103}; value.rotation = {104,105,106,107};
    value.scale.v = {2,3,4,5,6,7,8,9,10}; value.physicsDisabled = true; return value;
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord, 2> bones{};
    std::array<unsigned char, 2> selected{}, physics{};
    std::array<mdl::BoneOrderEntry, 3> selectors{};
    std::array<std::array<mdl::BoneReference, 3>, 8> parents{};
    Model() {
        record->boneCount = 2; record->boneTable = bones.data();
        record->boneKeys = mdl::BoneKeyPoolCreate(32);
        record->displayKeys = mdl::KeyPoolCreate<mdl::DisplayKey>(8);
        record->boneSelection = selected.data(); record->bonePhysicsState = physics.data();
        record->boneOrderCount = 3; record->boneOrderTable = selectors.data();
        record->maxFrame = 50; record->searchCursor = 777; record->displayKeyCursor = 5;
        for (unsigned i = 0; i < 3; ++i) {
            selectors[i].boneIndex = int(i) - 1; selectors[i].linkedModel = 900;
            selectors[i].linkedBone = 901; selectors[i].windowStart = 888; selectors[i].windowEnd = 999;
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto& bone = bones[i]; bone.parent = -1; bone.slotIndex = i ? -1 : 1;
            bone.type = mdl::BoneType::RotateMove; bone.rotQuat[3] = 1;
            bone.trans[0] = 900 + float(i); std::snprintf(bone.name, 20, "sample-%u", i);
            for (unsigned c = 0; c < 16; ++c) bone.matInit[c] = c % 5 == 0 ? 1.f : 0.f;
            auto& key = record->boneKeys[i]; key.allocated = 1; key.rotation[3] = 1;
        }
        auto& first = record->boneKeys[0]; first.next = 2;
        first.position[0] = 2; first.position[1] = -3; first.position[2] = .125f;
        first.rotation[0] = .38268343f; first.rotation[3] = .9238795f;
        record->boneKeys[2] = first; auto& middle = record->boneKeys[2];
        middle.previous = 0; middle.next = 3; middle.frame = 20; middle.physicsDisabled = 1;
        middle.position[0] = 22; middle.position[1] = 7; middle.position[2] = -.5f;
        middle.rotation[0] = 0; middle.rotation[1] = .70710677f; middle.rotation[3] = .70710677f;
        record->boneKeys[3] = middle; auto& last = record->boneKeys[3];
        last.previous = 2; last.next = 0; last.frame = 40; last.physicsDisabled = 0;
        last.position[0] = 42; last.position[1] = -11; last.position[2] = 1.5f;
        last.rotation[1] = 0; last.rotation[2] = -.70710677f;
        record->boneKeys[1].next = 4; record->boneKeys[4] = record->boneKeys[1];
        record->boneKeys[4].previous = 1; record->boneKeys[4].next = 0;
        record->boneKeys[4].frame = 20; record->boneKeys[4].position[0] = 20;
        bone_scale::Matrix scale; scale.v = {2,.125f,0,0,3,0,0,0,.5f};
        bone_scale::SetKey(raw(), 2, scale); scale.v = {4,.25f,0,0,2,0,0,0,1.5f}; bone_scale::SetKey(raw(), 3, scale);
        bone_scale::SetKeyCurve(raw(), 2, {10,90,90,115});
        const unsigned frames[]{0,5,7,12,20,25,45,50};
        for (unsigned i = 0; i < 8; ++i) {
            auto& key = record->displayKeys[i]; key.frame = frames[i]; key.previous = i ? i - 1 : 0;
            key.next = i < 7 ? i + 1 : 0; key.allocated = 1; key.visible = uint8_t(i * 31);
            key.selectorStates = parents[i].data();
            parents[i][0] = {int(i % 2), int(i)}; // Global changes must not affect slotIndex == -1.
            parents[i][1] = i == 0 ? mdl::BoneReference{-1,0} : i < 3 ? mdl::BoneReference{2,8} :
                i < 5 ? mdl::BoneReference{3,9} : i == 5 ? mdl::BoneReference{-1,2} : mdl::BoneReference{-1,3};
            parents[i][2] = {-1,0};
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
    ~Model() {
        bc::Forget(raw()); bone_scale::Forget(raw());
        mdl::BoneKeyPoolDestroy(record->boneKeys); mdl::KeyPoolDestroy(record->displayKeys);
        for (auto& ring : record->undoRings) for (auto& entry : ring.slots) {
            undo_storage::Free(entry.bonePose); undo_storage::Free(entry.auxiliaryPose);
        }
    }
};
struct SourceState {
    std::unique_ptr<mdl::ModelRecord> record;
    decltype(Model::bones) bones;
    decltype(Model::selected) selected, physics;
    decltype(Model::selectors) selectors;
    decltype(Model::parents) parents;
    std::vector<mdl::BoneKey> keys;
    std::array<mdl::DisplayKey, 8> display;
    bone_scale::Snapshot scale;
    std::unique_ptr<bc::AuthoringState> author;
    explicit SourceState(const Model& source) : record(std::make_unique<mdl::ModelRecord>(*source.record)),
        bones(source.bones), selected(source.selected), physics(source.physics), selectors(source.selectors), parents(source.parents),
        keys(source.record->boneKeys, source.record->boneKeys + mdl::BoneKeyCapacity(source.record->boneKeys)),
        scale(bone_scale::Capture(source.raw())), author(bc::AuthoringState::Capture(source.raw())) {
        std::memcpy(display.data(), source.record->displayKeys, sizeof(display));
    }
    void Check(const Model& source) const {
        CHECK(Bits(*record, *source.record) && Bits(bones, source.bones));
        CHECK(Bits(selected, source.selected) && Bits(physics, source.physics));
        CHECK(Bits(selectors, source.selectors) && Bits(parents, source.parents));
        CHECK(!std::memcmp(display.data(), source.record->displayKeys, sizeof(display)));
        CHECK(keys.size() == mdl::BoneKeyCapacity(source.record->boneKeys));
        CHECK(!std::memcmp(keys.data(), source.record->boneKeys, keys.size() * sizeof(mdl::BoneKey)));
        const auto now = bone_scale::Capture(source.raw());
        CHECK(scale.pose == now.pose && scale.keys == now.keys && scale.curves == now.curves);
        CHECK(bc::AuthoringState::Capture(source.raw())->Equals(*author));
    }
};
void Mirror(const Model& source, Model& oracle) {
    const auto capacity = mdl::BoneKeyCapacity(source.record->boneKeys);
    CHECK(mdl::BoneKeyPoolEnsure(*oracle.record, capacity));
    std::memcpy(oracle.record->boneKeys, source.record->boneKeys, capacity * sizeof(mdl::BoneKey));
    oracle.bones = source.bones; oracle.selectors = source.selectors; oracle.parents = source.parents;
    oracle.record->boneOrderCount = source.record->boneOrderCount;
    for (unsigned i = 0; i < 8; ++i) {
        oracle.record->displayKeys[i] = source.record->displayKeys[i];
        oracle.record->displayKeys[i].selectorStates = oracle.parents[i].data();
    }
    for (std::size_t i = 0; i < capacity; ++i) {
        bone_scale::SetKey(oracle.raw(), int(i), bone_scale::Key(source.raw(), int(i)));
        bone_scale::SetKeyCurve(oracle.raw(), int(i), bone_scale::KeyCurve(source.raw(), int(i)));
    }
    bc::CopyState(source.raw(), oracle.raw());
}
void CompareSeek(const Motion& motion, const Display& display, Model& oracle, int frame) {
    // Native types 7/9 intentionally preserve the existing pose. Independent
    // direct seek defines that pre-existing pose as fresh zero/identity.
    for (auto& bone : oracle.bones) {
        std::memset(bone.trans, 0, sizeof(bone.trans)); std::memset(bone.rotQuat, 0, sizeof(bone.rotQuat)); bone.rotQuat[3] = 1;
    }
    SeekModelFrame(oracle.raw(), frame, 0);
    for (int bone = 0; bone < 2; ++bone) {
        BoneMotionSample sample;
        failAfter = 0;
        const auto status = motion.PrepareKinematicChannels(bone, frame, display, sample);
        failAfter = -1;
        CHECK(status == Status::Ok);
        CHECK(!std::memcmp(sample.position.data(), oracle.bones[bone].trans, sizeof(float) * 3));
        CHECK(!std::memcmp(sample.rotation.data(), oracle.bones[bone].rotQuat, sizeof(float) * 4));
        CHECK(sample.scale == bone_scale::Pose(oracle.raw(), bone));
    }
}
void NativeParityAllTypes() {
    currentCase = "native seek parity for types 0 through 9";
    for (unsigned type = 0; type <= 9; ++type) for (unsigned curved = 0; curved < 2; ++curved) {
        Model source, oracle; source.bones[0].type = static_cast<mdl::BoneType>(type);
        if (curved) for (unsigned slot : {2u,3u}) {
            auto& key = source.record->boneKeys[slot];
            for (unsigned lane = 0; lane < 4; ++lane) {
                key.interpolation[lane] = uint8_t(13 + lane * 3); key.interpolation[lane + 4] = uint8_t(89 - lane * 7);
                key.interpolation[lane + 8] = uint8_t(75 + lane * 4); key.interpolation[lane + 12] = uint8_t(118 - lane * 3);
            }
            for (float& value : key.rotation) value = -value;
        }
        // One continuous inactive-parent interval permits real eased sampling;
        // the other variant exercises the window snap paths and exact copies.
        if (curved) for (auto& row : source.parents) row[1] = {-1,3};
        // Source pose is deliberately not the fresh baseline used by type 7/9.
        source.bones[0].trans[1] = -987; source.bones[0].rotQuat[2] = .25f;
        Mirror(source, oracle); const SourceState before(source);
        const auto motion = Motion::Capture(source.raw()); const auto display = Display::Capture(source.raw());
        for (int frame = 0; frame <= 55; ++frame) CompareSeek(*motion, *display, oracle, frame);
        CompareSeek(*motion, *display, oracle, frame_limits::maximum); before.Check(source);
        auto old = Sentinel(); const auto sentinel = old;
        CHECK(motion->SampleChannels(0, 6, old) == Status::ExternalParent && Equal(old, sentinel));
    }
}
void FractionalWindowBoundaries() {
    currentCase = "fractional selector windows and independent scale";
    Model source; const SourceState before(source);
    const auto motion = Motion::Capture(source.raw()); const auto display = Display::Capture(source.raw());
    auto checkKey = [&](double frame, int slot) {
        BoneMotionSample sample; CHECK(motion->PrepareKinematicChannels(0, frame, *display, sample) == Status::Ok);
        if (slot < 0) {
            const std::array<float,3> zero{}; const std::array<float,4> identity{0,0,0,1};
            CHECK(Bits(sample.position, zero) && Bits(sample.rotation, identity));
        } else {
            const auto& key = source.record->boneKeys[slot];
            CHECK(!std::memcmp(sample.position.data(), key.position, sizeof(key.position)));
            CHECK(!std::memcmp(sample.rotation.data(), key.rotation, sizeof(key.rotation)));
        }
        CHECK(sample.scale == bone_scale::Sample(source.raw(), 0, frame));
        CHECK(sample.physicsDisabled == (frame >= 20 && frame < 40));
    };
    // These expectations enumerate the fixture's actual discrete changes; no
    // copy of the production window-selection algorithm computes this oracle.
    checkKey(0,0); checkKey(std::nextafter(5.,0.),0); checkKey(5,2); checkKey(std::nextafter(5.,6.),2);
    checkKey(6,2); // Start 5 wins over end 12 within the same [0,20] interval.
    checkKey(std::nextafter(7.,0.),2); checkKey(7,2); checkKey(std::nextafter(7.,8.),2);
    checkKey(std::nextafter(12.,0.),2); checkKey(12,2); checkKey(std::nextafter(12.,13.),2);
    checkKey(std::nextafter(20.,0.),2); checkKey(20,2); checkKey(std::nextafter(20.,21.),2);
    checkKey(std::nextafter(25.,0.),2); checkKey(25,3); checkKey(std::nextafter(25.,26.),3);
    checkKey(std::nextafter(40.,0.),3); checkKey(40,3); checkKey(std::nextafter(40.,41.),3);
    checkKey(std::nextafter(45.,0.),3); checkKey(45,-1); checkKey(std::nextafter(45.,46.),-1);
    checkKey(50,-1); checkKey(frame_limits::maximum,-1);
    DisplaySelectorSample interval;
    CHECK(display->SampleSelector(1,7,interval) == DisplayMotionStatus::Ok && interval.windowStart == 5 && interval.windowEnd == 12);
    CHECK(display->SampleSelector(1,50,interval) == DisplayMotionStatus::Ok && interval.modelIndex == -1 && interval.windowStart == 45 && interval.windowEnd == 0);
    // End exactly at the upper key: interpolation stays at the lower key until
    // the exact key wins. Subsequent terminal seeks have start == last frame.
    for (unsigned i = 0; i < 8; ++i) source.parents[i][1] = i < 4 ? mdl::BoneReference{-1,0} : mdl::BoneReference{-1,1};
    const auto endingMotion = Motion::Capture(source.raw());
    const auto ending = Display::Capture(source.raw());
    BoneMotionSample sample;
    CHECK(endingMotion->PrepareKinematicChannels(0,std::nextafter(20.,0.),*ending,sample) == Status::Ok);
    CHECK(!std::memcmp(sample.position.data(),source.record->boneKeys[0].position,12));
    CHECK(endingMotion->PrepareKinematicChannels(0,20,*ending,sample) == Status::Ok && sample.position[0] == 22);
    // A global parent never creates a local selector window for bone 1.
    for (double frame : {.25,4.5,5.,6.,7.5,12.,19.25,20.,45.}) {
        CHECK(motion->PrepareKinematicChannels(1,frame,*display,sample) == Status::Ok);
        CHECK(Near(sample.position[0], frame < 20 ? frame : 20));
    }
    // Compare source only after undoing the deliberate display input edit.
    source.parents = before.parents; before.Check(source);
}
bc::Tracks FreeTracks(double offset) {
    bc::Tracks tracks;
    for (unsigned lane = 0; lane < 9; ++lane) {
        ac::Key first, last; last.frame = 40; first.tangent = last.tangent = ac::Tangent::Free;
        first.interpolation = last.interpolation = ac::Interpolation::Linear;
        const double value[]{100 + offset,-8.5,.125,0,0,0,2,3,.5};
        first.value = last.value = value[lane];
        if (lane == 0) last.value += 40;
        if (lane == 4) last.value = 450;
        tracks[lane].keys = {first,last};
    }
    return tracks;
}
void WriteFree(Model& source, double offset = 0) {
    std::string error; CHECK(bc::Write(source.raw(),0,FreeTracks(offset),error));
}
void FreeOverrideAndDerivedDefaults() {
    currentCase = "free curves override snap and derived baseline";
    for (unsigned type : {0u,7u,9u}) {
        Model source, oracle; source.bones[0].type = static_cast<mdl::BoneType>(type); WriteFree(source);
        Mirror(source,oracle); const SourceState before(source);
        const auto motion = Motion::Capture(source.raw()); const auto display = Display::Capture(source.raw());
        for (int frame : {0,4,5,6,7,12,20,25,39,40,45,50}) CompareSeek(*motion,*display,oracle,frame);
        for (double frame : {.125,5.,6.,7.5,12.,19.75,25.,39.5,40.,45.}) {
            BoneMotionSample sample; CHECK(motion->PrepareKinematicChannels(0,frame,*display,sample) == Status::Ok);
            const double clamped = frame < 40 ? frame : 40;
            CHECK(Near(sample.position[0],100 + clamped) && sample.position[1] == -8.5f && sample.position[2] == .125f);
            const double half = clamped * (450. / 40.) * 3.14159265358979323846 / 360.;
            CHECK(Near(sample.rotation[0],0) && Near(sample.rotation[1],std::sin(half)) && Near(sample.rotation[2],0) && Near(sample.rotation[3],std::cos(half)));
            // Authored XYZ scale controls basis-row lengths. The native path
            // preserves the direction/shear of each full 3x3 row; it does not
            // replace this fixture's off-diagonal entries with a diagonal.
            const auto basis = bone_scale::Sample(source.raw(),0,frame);
            const double lengths[]{2,3,.5};
            for (unsigned row = 0; row < 3; ++row) {
                double squared = 0, basisSquared = 0, dot = 0;
                for (unsigned column = 0; column < 3; ++column) {
                    const double value = sample.scale.v[row*3+column];
                    const double original = basis.v[row*3+column];
                    squared += value*value; basisSquared += original*original; dot += value*original;
                }
                CHECK(std::abs(std::sqrt(squared)-lengths[row]) < 2e-5);
                CHECK(std::abs(dot/std::sqrt(squared*basisSquared)-1.) < 2e-6);
            }
            CHECK(sample.scale.v[1] > 0); // Preserve the actual shear too.
            // Real direct-seek scale/free-curve passes support fractional
            // input independently of SeekModelFrame's integer wrapper.
            bone_scale::Evaluate(oracle.raw(),frame); bc::Evaluate(oracle.raw(),frame);
            CHECK(sample.scale == bone_scale::Pose(oracle.raw(),0));
        }
        before.Check(source);
    }
    for (unsigned type : {7u,9u}) {
        Model source; source.bones[0].type = static_cast<mdl::BoneType>(type); source.bones[0].slotIndex = -1;
        const auto motion = Motion::Capture(source.raw()); const auto display = Display::Capture(source.raw());
        for (double frame : {0.,5.5,20.,45.}) {
            auto sample = Sentinel(); CHECK(motion->PrepareKinematicChannels(0,frame,*display,sample) == Status::Ok);
            const std::array<float,3> zero{}; const std::array<float,4> identity{0,0,0,1};
            CHECK(Bits(sample.position,zero) && Bits(sample.rotation,identity));
            CHECK(sample.scale == bone_scale::Sample(source.raw(),0,frame));
            const auto sentinel = Sentinel(); sample = sentinel;
            CHECK(motion->SampleChannels(0,frame,sample) == Status::UnsupportedBone && Equal(sample,sentinel));
        }
    }
}
void InvalidInputs() {
    currentCase = "invalid request preserves complete output";
    Model source; auto motion = Motion::Capture(source.raw()); auto display = Display::Capture(source.raw());
    const auto sentinel = Sentinel();
    for (int bone : {-1,2,INT32_MAX}) {
        auto out = sentinel; CHECK(motion->PrepareKinematicChannels(bone,0,*display,out) == Status::InvalidBone && Equal(out,sentinel));
    }
    for (double frame : {-1.,-.125,std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),double(frame_limits::maximum)+.5}) {
        auto out = sentinel; CHECK(motion->PrepareKinematicChannels(0,frame,*display,out) == Status::InvalidTime && Equal(out,sentinel));
    }
    source.selectors[1].boneIndex = 1; auto mismatched = Display::Capture(source.raw());
    auto out = sentinel; CHECK(motion->PrepareKinematicChannels(0,6,*mismatched,out) == Status::InvalidSample && Equal(out,sentinel));
    source.record->boneOrderCount = 1; auto missing = Display::Capture(source.raw());
    out = sentinel; CHECK(motion->PrepareKinematicChannels(0,6,*missing,out) == Status::InvalidSample && Equal(out,sentinel));
    source.record->boneOrderCount = 0; auto empty = Display::Capture(source.raw());
    CHECK(motion->PrepareKinematicChannels(1,6,*empty,out) == Status::Ok && out.position[0] == 6);
    source.record->boneOrderCount = 3; source.selectors[1].boneIndex = 0;
    source.bones[0].slotIndex = 0; motion = Motion::Capture(source.raw());
    out = sentinel; CHECK(motion->PrepareKinematicChannels(0,6,*display,out) == Status::InvalidSample && Equal(out,sentinel));
    source.bones[0].slotIndex = -1;
    for (unsigned type : {10u,127u,255u}) {
        source.bones[0].type = static_cast<mdl::BoneType>(type); motion = Motion::Capture(source.raw());
        out = sentinel; CHECK(motion->PrepareKinematicChannels(0,6,*display,out) == Status::UnsupportedBone && Equal(out,sentinel));
    }
    source.bones[0].type = mdl::BoneType::RotateMove;
    source.record->boneKeys[0].position[0] = -(std::numeric_limits<float>::max)();
    source.record->boneKeys[2].position[0] = (std::numeric_limits<float>::max)();
    motion = Motion::Capture(source.raw()); out = sentinel;
    CHECK(motion->PrepareKinematicChannels(0,10,*display,out) == Status::InvalidSample && Equal(out,sentinel));
    // Native types 7/9 do not consume their selector at all. Unused selector
    // mappings must not prevent the fresh baseline or free override.
    for (unsigned type : {7u,9u}) for (unsigned authored = 0; authored < 2; ++authored) {
        Model derived; derived.bones[0].type = static_cast<mdl::BoneType>(type);
        if (authored) WriteFree(derived);
        const auto derivedMotion = Motion::Capture(derived.raw());
        derived.selectors[1].boneIndex = 1; const auto wrong = Display::Capture(derived.raw());
        derived.record->boneOrderCount = 0; const auto absent = Display::Capture(derived.raw());
        BoneMotionSample wrongResult, absentResult;
        CHECK(derivedMotion->PrepareKinematicChannels(0,6,*wrong,wrongResult) == Status::Ok);
        CHECK(derivedMotion->PrepareKinematicChannels(0,6,*absent,absentResult) == Status::Ok);
        CHECK(Equal(wrongResult,absentResult));
        CHECK(wrongResult.position[0] == (authored ? 106.f : 0.f));
        if (!authored) {
            const std::array<float,4> identity{0,0,0,1}; CHECK(Bits(wrongResult.rotation,identity));
        }
    }
}
void LifetimeAndConcurrentNoAllocation() {
    currentCase = "frozen local preparation lifetime and concurrent reads";
    auto source = std::make_unique<Model>(); WriteFree(*source);
    const auto oldMotion = Motion::Capture(source->raw()); const auto oldDisplay = Display::Capture(source->raw());
    constexpr std::array<double,8> times{0,.125,5,6.5,20,25.25,40,45};
    std::array<BoneMotionSample,8> expected{};
    for (unsigned i = 0; i < times.size(); ++i) CHECK(oldMotion->PrepareKinematicChannels(0,times[i],*oldDisplay,expected[i]) == Status::Ok);
    WriteFree(*source,200);
    for (auto& row : source->parents) row[1] = {-1,99};
    const auto newMotion = Motion::Capture(source->raw()); const auto newDisplay = Display::Capture(source->raw());
    BoneMotionSample newer, older;
    CHECK(newMotion->PrepareKinematicChannels(0,45,*newDisplay,newer) == Status::Ok && newer.position[0] == 340);
    CHECK(oldMotion->PrepareKinematicChannels(0,45,*oldDisplay,older) == Status::Ok && older.position[0] == 140);
    DisplaySelectorSample oldParent, newParent;
    CHECK(oldDisplay->SampleSelector(1,6,oldParent) == DisplayMotionStatus::Ok && oldParent.windowStart == 5);
    CHECK(newDisplay->SampleSelector(1,6,newParent) == DisplayMotionStatus::Ok && newParent.windowStart == 0 && newParent.windowEnd == 0);
    source.reset();
    std::atomic<unsigned> failures{0}; std::array<std::thread,4> workers;
    try {
        for (auto& worker : workers) worker = std::thread([&] {
            failAfter = 0;
            for (unsigned repeat = 0; repeat < 300; ++repeat) for (unsigned i = 0; i < times.size(); ++i) {
                BoneMotionSample sample;
                if (oldMotion->PrepareKinematicChannels(0,times[i],*oldDisplay,sample) != Status::Ok || !Equal(sample,expected[i])) ++failures;
                if (newMotion->PrepareKinematicChannels(0,45,*newDisplay,sample) != Status::Ok || sample.position[0] != 340) ++failures;
            }
            failAfter = -1;
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (auto& worker : workers) worker.join(); CHECK(failures == 0);
}
EvaluationTime NearIntegerTime(int frame, int side) {
    // Exact time is frame + side/1e17. All tested frames fit the numerator,
    // while their before/after values round to the same continuous double.
    EvaluationTime time;
    CHECK(EvaluationTime::FromSeconds(std::int64_t(frame)*100000000000000000ll+side,
                                     3000000000000000000ull,time));
    CHECK(time.Frame()==frame&&time.DiscreteFrame()==unsigned(side<0?frame-1:frame));
    return time;
}
void ExactRationalWindowSelection() {
    currentCase="exact rational selector snap, terminal zero-fill and physics switches";
    Model source; const SourceState before(source);
    const auto motion=Motion::Capture(source.raw());const auto display=Display::Capture(source.raw());
    const auto checkKey=[&](const EvaluationTime& time,int slot,bool disabled){
        BoneMotionSample sample;failAfter=0;
        const auto status=motion->PrepareKinematicChannelsAt(0,time,*display,sample);failAfter=-1;
        CHECK(status==Status::Ok&&sample.physicsDisabled==disabled);
        if(slot<0){const std::array<float,3> zero{};const std::array<float,4> identity{0,0,0,1};
            CHECK(Bits(sample.position,zero)&&Bits(sample.rotation,identity));
        }else{const auto& key=source.record->boneKeys[slot];CHECK(!std::memcmp(sample.position.data(),key.position,12)&&!std::memcmp(sample.rotation.data(),key.rotation,16));}
        // Native eased scale belongs to the exact interval. CurveTime's finite
        // iteration at t=1 may differ from directly reading the next key, so a
        // rounded key time cannot serve as the oracle for its left-hand limit.
        const double referenceFrame=time.Frame()==double(time.DiscreteFrame())+1?
            std::nextafter(time.Frame(),0.0):time.Frame();
        CHECK(sample.scale==bone_scale::Sample(source.raw(),0,referenceFrame));return sample;
    };
    // A new local window starts at 5: before it we retain key 0, from it we
    // snap to key 20. Merely passing the rounded double starts the window early.
    const auto atFive=checkKey(NearIntegerTime(5,-1),0,false);
    checkKey(NearIntegerTime(5,0),2,false);checkKey(NearIntegerTime(5,1),2,false);
    BoneMotionSample rounded;CHECK(motion->PrepareKinematicChannels(0,5,*display,rounded)==Status::Ok);
    CHECK(!Bits(atFive.position,rounded.position)&&rounded.position[0]==22);
    checkKey(NearIntegerTime(25,-1),2,true);checkKey(NearIntegerTime(25,0),3,true);checkKey(NearIntegerTime(25,1),3,true);
    const auto beforeTerminal=checkKey(NearIntegerTime(45,-1),3,false);
    checkKey(NearIntegerTime(45,0),-1,false);checkKey(NearIntegerTime(45,1),-1,false);
    CHECK(motion->PrepareKinematicChannels(0,45,*display,rounded)==Status::Ok&&rounded.position[0]==0);
    CHECK(!Bits(beforeTerminal.position,rounded.position));
    // Window snapping does not change which registered bone key supplies the
    // discrete physics flag. Both positions may round/snap to the same value.
    const auto beforeTwenty=checkKey(NearIntegerTime(20,-1),2,false);
    checkKey(NearIntegerTime(20,0),2,true);checkKey(NearIntegerTime(20,1),2,true);
    CHECK(motion->PrepareKinematicChannels(0,20,*display,rounded)==Status::Ok&&rounded.physicsDisabled);
    CHECK(!beforeTwenty.physicsDisabled&&Bits(beforeTwenty.position,rounded.position));
    CHECK(!(beforeTwenty.scale==rounded.scale)); // The native left interval differs from the exact key.
    checkKey(NearIntegerTime(40,-1),3,true);checkKey(NearIntegerTime(40,0),3,false);checkKey(NearIntegerTime(40,1),3,false);
    // The old public sampler still refuses local selectors, even for a valid
    // rounded frame. An internal rational overload must not widen that ABI.
    auto denied=Sentinel();const auto original=denied;
    CHECK(motion->SampleChannels(0,5,denied)==Status::ExternalParent&&Equal(denied,original));
    for(int badBone:{-1,2}){denied=original;CHECK(motion->PrepareKinematicChannelsAt(badBone,NearIntegerTime(5,-1),*display,denied)==Status::InvalidBone&&Equal(denied,original));}
    before.Check(source);

    currentCase="exact upper-key priority must not apply to a rational just before that key";
    Model ending;
    for(unsigned i=0;i<8;++i)ending.parents[i][1]=i<4?mdl::BoneReference{-1,0}:mdl::BoneReference{-1,1};
    const SourceState endingBefore(ending);const auto endMotion=Motion::Capture(ending.raw());const auto endDisplay=Display::Capture(ending.raw());
    BoneMotionSample rational,exact;
    CHECK(endMotion->PrepareKinematicChannelsAt(0,NearIntegerTime(20,-1),*endDisplay,rational)==Status::Ok);
    CHECK(!std::memcmp(rational.position.data(),ending.record->boneKeys[0].position,12)&&!rational.physicsDisabled);
    CHECK(endMotion->PrepareKinematicChannelsAt(0,NearIntegerTime(20,0),*endDisplay,exact)==Status::Ok&&exact.position[0]==22&&exact.physicsDisabled);
    CHECK(endMotion->PrepareKinematicChannels(0,20,*endDisplay,rounded)==Status::Ok&&Equal(rounded,exact));
    CHECK(!Bits(rational.position,rounded.position));endingBefore.Check(ending);
}
void RationalContinuousChannels() {
    currentCase="rational discrete time does not quantize native or authored continuous channels";
    Model source;
    // Bone 1 has no local selector. Give it linear position, Y rotation and
    // diagonal scale, each with an independent fractional analytic oracle.
    source.record->boneKeys[4].rotation[1]=.70710677f;
    source.record->boneKeys[4].rotation[3]=.70710677f;
    bone_scale::Matrix scale;scale.v={2,0,0,0,3,0,0,0,4};
    bone_scale::SetKey(source.raw(),4,scale);bone_scale::SetKeyCurve(source.raw(),4,{0,0,127,127});
    auto tracks=FreeTracks(0);tracks[6].keys.back().value=4;
    // Keep the native frame-20 registration (and its physics switch) when the
    // authored tracks write their compatibility mirror. The added point is
    // collinear, so the analytic continuous position remains 100 + frame.
    auto middle=tracks[0].keys.front();middle.frame=20;middle.value=120;
    tracks[0].keys.insert(tracks[0].keys.begin()+1,middle);
    std::string error;CHECK(bc::Write(source.raw(),0,tracks,error));const SourceState before(source);
    const auto motion=Motion::Capture(source.raw());const auto display=Display::Capture(source.raw());
    for(const auto numerator:{1ll,11ll,27ll,39ll}){
        EvaluationTime time;CHECK(EvaluationTime::FromSeconds(numerator,60,time));const double frame=double(numerator)*.5;
        CHECK(time.Frame()==frame&&time.DiscreteFrame()==unsigned(frame));
        BoneMotionSample direct,authored,oldDirect;
        failAfter=0;const auto dr=motion->PrepareKinematicChannelsAt(1,time,*display,direct);
        const auto ar=motion->PrepareKinematicChannelsAt(0,time,*display,authored);failAfter=-1;CHECK(dr==Status::Ok&&ar==Status::Ok);
        CHECK(Near(direct.position[0],frame)&&direct.position[1]==0&&direct.position[2]==0);
        const double nativeHalf=frame/20*3.14159265358979323846/4;
        CHECK(Near(direct.rotation[0],0)&&Near(direct.rotation[1],std::sin(nativeHalf))&&Near(direct.rotation[2],0)&&Near(direct.rotation[3],std::cos(nativeHalf)));
        CHECK(Near(direct.scale.v[0],1+frame/20)&&Near(direct.scale.v[4],1+2*frame/20)&&Near(direct.scale.v[8],1+3*frame/20));
        for(unsigned index:{1u,2u,3u,5u,6u,7u})CHECK(direct.scale.v[index]==0);
        CHECK(motion->SampleChannels(1,frame,oldDirect)==Status::Ok&&Equal(oldDirect,direct));
        CHECK(Near(authored.position[0],100+frame)&&authored.position[1]==-8.5f);
        const double authoredHalf=frame*(450./40.)*3.14159265358979323846/360.;
        CHECK(Near(authored.rotation[1],std::sin(authoredHalf))&&Near(authored.rotation[3],std::cos(authoredHalf)));
        double rowLength=0;for(unsigned column=0;column<3;++column)rowLength+=double(authored.scale.v[column])*authored.scale.v[column];
        CHECK(Near(float(std::sqrt(rowLength)),2+frame/20));
        BoneMotionSample integer;CHECK(motion->PrepareKinematicChannels(1,time.DiscreteFrame(),*display,integer)==Status::Ok);
        CHECK(!Bits(direct.position,integer.position)&&!Bits(direct.rotation,integer.rotation)&&!Bits(direct.scale,integer.scale));
    }
    // Even where the continuous double rounds to key 20, a free override uses
    // that continuous time while the physics switch keeps exact key ownership.
    BoneMotionSample exactBefore,rounded;
    CHECK(motion->PrepareKinematicChannelsAt(0,NearIntegerTime(20,-1),*display,exactBefore)==Status::Ok);
    CHECK(motion->PrepareKinematicChannels(0,20,*display,rounded)==Status::Ok);
    CHECK(Bits(exactBefore.position,rounded.position)&&Bits(exactBefore.rotation,rounded.rotation)&&Bits(exactBefore.scale,rounded.scale));
    CHECK(Near(exactBefore.position[0],120)&&!exactBefore.physicsDisabled&&rounded.physicsDisabled);before.Check(source);
}
}
int main() {
    try {
        NativeParityAllTypes(); FractionalWindowBoundaries(); FreeOverrideAndDerivedDefaults(); InvalidInputs(); LifetimeAndConcurrentNoAllocation();
        ExactRationalWindowSelection(); RationalContinuousChannels();
        std::printf("PASS %u bone kinematic sampling checks\n",checks); return 0;
    } catch (const std::exception& error) {
        failAfter = -1; std::fprintf(stderr,"FAIL %s unexpected exception: %s\n",currentCase,error.what()); return 1;
    }
}
