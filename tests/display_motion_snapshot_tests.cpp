// Frozen display/IK/parent channels compared with the real native seek path.
#define NOMINMAX
#include "mikumikustudio/display_motion_snapshot.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
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

namespace { thread_local int failAfter = -1; }
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
using Frozen = DisplayMotionSnapshot;
using Status = DisplayMotionStatus;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* text) {
    ++checks; if (!ok) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
template<class E, class F> bool Throws(F&& f) {
    try { f(); } catch (const E&) { return true; } catch (...) {}
    return false;
}
bool Equal(const DisplaySelectorSample& a, const DisplaySelectorSample& b) {
    return a.modelIndex == b.modelIndex && a.boneIndex == b.boneIndex &&
        a.windowStart == b.windowStart && a.windowEnd == b.windowEnd;
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::IkChain, 2> ik{};
    std::array<mdl::BoneOrderEntry, 4> order{};
    std::array<mdl::BoneRecord, 3> pose{};
    std::vector<std::array<uint8_t, 2>> switches;
    std::vector<std::array<mdl::BoneReference, 4>> parents;
    std::unique_ptr<mdl::DisplayKey[]> borrowed;
    mdl::DisplayKey* owned = nullptr;
    std::size_t capacity;
    explicit Model(std::size_t count = 8, bool registered = true) : switches(count), parents(count), capacity(count) {
        Check(count >= 8 && (registered || count == 1000), "display fixture pool size");
        if (registered) owned = mdl::KeyPoolCreate<mdl::DisplayKey>(count);
        else borrowed = std::make_unique<mdl::DisplayKey[]>(count);
        record->displayKeys = owned ? owned : borrowed.get();
        record->boneCount = uint32_t(pose.size()); record->boneTable = pose.data();
        record->ikChainCount = uint32_t(ik.size()); record->ikChains = ik.data();
        record->boneOrderCount = uint32_t(order.size()); record->boneOrderTable = order.data();
        ik[0].boneIndex = 2; ik[1].boneIndex = 0;
        for (unsigned i = 0; i < order.size(); ++i) {
            order[i].boneIndex = int(i) - 1; order[i].linkedModel = 88; order[i].linkedBone = 99;
            order[i].windowStart = 111; order[i].windowEnd = 222;
        }
        for (auto& chain : ik) chain.enabled = 231;
        for (unsigned i = 0; i < pose.size(); ++i) { pose[i].trans[0] = float(i + 700); pose[i].slotIndex = -1; }
        record->loadComplete = 199; record->displayKeyCursor = 7; record->displayTrackActive = 0;
        record->displayKeyframesPresent = 0; record->searchCursor = 987; record->maxFrame = 999;
        for (std::size_t i = 0; i < count; ++i) {
            auto& key = record->displayKeys[i]; key.ikStates = switches[i].data(); key.selectorStates = parents[i].data();
        }
        const unsigned slots[]{0, 4, 2, 6, 1, 3}; const unsigned frames[]{0, 5, 10, 15, 20, 30};
        for (unsigned i = 0; i < 6; ++i) {
            auto& key = record->displayKeys[slots[i]];
            key.frame = frames[i]; key.previous = i ? slots[i-1] : 7; key.next = i + 1 < 6 ? slots[i+1] : 0;
            key.visible = uint8_t(i * 41); key.allocated = uint8_t(i % 2);
            key.reserved0[1] = uint8_t(i + 21); key.reserved1[2] = uint8_t(i + 31);
            switches[slots[i]] = {uint8_t(i * 50), uint8_t(255 - i * 30)};
            parents[slots[i]][0] = {-1, i < 2 || i == 5 ? 4 : 7};
            parents[slots[i]][1] = {i < 4 ? 2 : 3, 9};
            parents[slots[i]][2] = {INT32_MIN, INT32_MAX};
            parents[slots[i]][3] = {i % 2 ? -7 : 8000, -300};
        }
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
    ~Model() { if (owned) mdl::KeyPoolDestroy(owned); }
};
struct SourceState {
    std::unique_ptr<mdl::ModelRecord> model;
    std::vector<mdl::DisplayKey> keys;
    decltype(Model::switches) switches;
    decltype(Model::parents) parents;
    decltype(Model::ik) ik;
    decltype(Model::order) order;
    decltype(Model::pose) pose;
    explicit SourceState(const Model& m) : model(std::make_unique<mdl::ModelRecord>(*m.record)),
        keys(m.record->displayKeys, m.record->displayKeys + m.capacity), switches(m.switches), parents(m.parents),
        ik(m.ik), order(m.order), pose(m.pose) {}
    void Same(const Model& m) const {
        Check(!std::memcmp(model.get(), m.record.get(), sizeof(*model)), "capture/sample leave full source record and cursor flags unchanged");
        Check(!std::memcmp(keys.data(), m.record->displayKeys, keys.size() * sizeof(mdl::DisplayKey)), "source pool bytes and nested addresses unchanged");
        Check(!std::memcmp(switches.data(), m.switches.data(), switches.size() * sizeof(switches[0])), "source nested IK bytes unchanged");
        Check(!std::memcmp(parents.data(), m.parents.data(), parents.size() * sizeof(parents[0])), "source nested parent pairs unchanged");
        Check(!std::memcmp(ik.data(), m.ik.data(), sizeof(ik)) && !std::memcmp(order.data(), m.order.data(), sizeof(order)),
            "source current IK flags, selector windows and mappings unchanged");
        Check(!std::memcmp(pose.data(), m.pose.data(), sizeof(pose)), "source unregistered pose unchanged");
    }
};
void Mirror(const Model& source, Model& oracle) {
    Check(source.capacity <= oracle.capacity, "independent oracle capacity");
    for (std::size_t i = 0; i < source.capacity; ++i) {
        auto& target = oracle.record->displayKeys[i]; target = source.record->displayKeys[i];
        target.ikStates = oracle.switches[i].data(); target.selectorStates = oracle.parents[i].data();
        oracle.switches[i] = source.switches[i]; oracle.parents[i] = source.parents[i];
    }
    oracle.ik = source.ik; oracle.order = source.order;
    oracle.record->ikChainCount = source.record->ikChainCount; oracle.record->boneOrderCount = source.record->boneOrderCount;
    // Invoke the real SeekModelFrame, but isolate its display section. Bone and
    // morph loops do not contribute to the display/IK/selector values compared.
    oracle.record->boneCount = 0; oracle.record->morphCount = 0;
}
struct Sample {
    uint8_t visibility = 0;
    std::array<uint8_t, 2> ik{};
    std::array<DisplaySelectorSample, 4> parents{};
};
bool Read(const Frozen& frozen, double frame, Sample& out) noexcept {
    if (frozen.SampleVisibility(frame, out.visibility) != Status::Ok) return false;
    for (unsigned i = 0; i < out.ik.size(); ++i) if (frozen.SampleIk(int(i), frame, out.ik[i]) != Status::Ok) return false;
    for (unsigned i = 0; i < out.parents.size(); ++i) if (frozen.SampleSelector(int(i), frame, out.parents[i]) != Status::Ok) return false;
    return true;
}
bool Equal(const Sample& a, const Sample& b) {
    if (a.visibility != b.visibility || a.ik != b.ik) return false;
    for (unsigned i = 0; i < a.parents.size(); ++i) if (!Equal(a.parents[i], b.parents[i])) return false;
    return true;
}
void CompareSeek(const Frozen& frozen, Model& oracle, double frame) {
    SeekModelFrame(oracle.raw(), int(std::floor(frame)), 0);
    Sample sample; Check(Read(frozen, frame, sample), "sample full frozen display state");
    Check(sample.visibility == oracle.record->loadComplete, "visibility byte matches real native seek");
    for (unsigned i = 0; i < sample.ik.size(); ++i) Check(sample.ik[i] == oracle.ik[i].enabled, "IK byte matches real native seek");
    for (unsigned i = 0; i < sample.parents.size(); ++i) {
        const auto& expected = oracle.order[i];
        Check(Equal(sample.parents[i], {expected.linkedModel, expected.linkedBone, expected.windowStart, expected.windowEnd}),
            "raw parent pair and contiguous windows match real native seek");
    }
}

void NativeParityAndMappings() {
    Model source, oracle; Mirror(source, oracle); const SourceState before(source);
    const auto frozen = Frozen::Capture(source.raw());
    Check(frozen->KeyCount() == 6 && frozen->IkCount() == 2 && frozen->SelectorCount() == 4 && frozen->RetainedBytes() > 0,
        "snapshot describes reachable keys and captured mappings");
    for (unsigned i = 0; i < 2; ++i) { int32_t bone = 99; Check(frozen->IkBone(i, bone) && bone == source.ik[i].boneIndex, "IK maps to original bone slot"); }
    for (unsigned i = 0; i < 4; ++i) { int32_t bone = 99; Check(frozen->SelectorBone(i, bone) && bone == int(i)-1, "global and per-bone selector mappings are frozen"); }
    for (int frame = 0; frame < 42; ++frame) { CompareSeek(*frozen, oracle, frame); CompareSeek(*frozen, oracle, frame + .375); }
    CompareSeek(*frozen, oracle, frame_limits::maximum);
    CompareSeek(*frozen, oracle, double(frame_limits::maximum) - .25);
    DisplaySelectorSample parent;
    Check(frozen->SampleSelector(0, 9.75, parent) == Status::Ok && parent.modelIndex == -1 && parent.boneIndex == 4 && parent.windowEnd == 10,
        "disabled raw parents with distinct bone indices still end their matching interval");
    Check(frozen->SampleSelector(2, 15, parent) == Status::Ok && parent.modelIndex == INT32_MIN && parent.boneIndex == INT32_MAX &&
        parent.windowStart == 0 && parent.windowEnd == 0, "raw parent slots are retained without live identity resolution");
    before.Same(source);
    for (std::size_t i = 0; i < source.capacity; ++i) source.record->displayKeys[i].allocated = 0;
    source.record->displayTrackActive = 255; source.record->displayKeyframesPresent = 255; source.record->displayKeyCursor = 6;
    source.record->boneTable = nullptr; // Capture does not read current bone pose.
    const SourceState marked(source); const auto changedFlags = Frozen::Capture(source.raw());
    for (double frame : {0., .125, 10., 19.75, 30., 100.}) {
        Sample a, b; Check(Read(*frozen, frame, a) && Read(*changedFlags, frame, b) && Equal(a, b), "selection/active flags and current pose do not change registered samples");
    }
    marked.Same(source);
}

void LongMatchingIntervals() {
    Model source(160), oracle(160);
    for (unsigned i = 0; i < 130; ++i) {
        auto& key = source.record->displayKeys[i]; key.frame = i * 3; key.previous = i ? i-1 : 159; key.next = i < 129 ? i+1 : 0;
        key.visible = uint8_t(i * 17); key.allocated = uint8_t(i % 2);
        source.switches[i] = {uint8_t(i * 13), uint8_t(255-i)};
        for (unsigned selector = 0; selector < 4; ++selector)
            source.parents[i][selector] = {-1, int32_t(selector + (i < 100 ? 7 : 11))};
    }
    Mirror(source, oracle); const SourceState before(source); const auto frozen = Frozen::Capture(source.raw());
    Check(frozen->KeyCount() == 130, "long display chain captured");
    for (int frame = 0; frame <= 400; ++frame) CompareSeek(*frozen, oracle, frame);
    for (double frame : {2.999, 150.25, 299.999, 300., 386.75, 387.125}) CompareSeek(*frozen, oracle, frame);
    DisplaySelectorSample parent;
    Check(frozen->SampleSelector(0, 150.25, parent) == Status::Ok && parent.windowStart == 0 && parent.windowEnd == 300,
        "IK and visibility changes do not split identical parent intervals");
    Check(frozen->SampleSelector(0, 300, parent) == Status::Ok && parent.windowStart == 300 && parent.windowEnd == 0,
        "exact parent change begins a new terminal interval"); before.Same(source);
}

void InvalidRequestsAndEmptyPayloads() {
    Model source; const auto frozen = Frozen::Capture(source.raw());
    constexpr uint8_t byte = 197; const DisplaySelectorSample sentinel{-654, 321, 789, 456};
    for (double frame : {-1., -.001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), double(frame_limits::maximum) + .125}) {
        auto visibility = byte, ik = byte; auto parent = sentinel;
        Check(frozen->SampleVisibility(frame, visibility) == Status::InvalidTime && visibility == byte, "invalid visibility time preserves output");
        Check(frozen->SampleIk(0, frame, ik) == Status::InvalidTime && ik == byte, "invalid IK time preserves output");
        Check(frozen->SampleSelector(0, frame, parent) == Status::InvalidTime && Equal(parent, sentinel), "invalid selector time preserves output");
    }
    for (int index : {-1, 2, INT32_MAX}) { auto value = byte; Check(frozen->SampleIk(index, 0, value) == Status::InvalidIndex && value == byte, "invalid IK index preserves output"); }
    for (int index : {-1, 4, INT32_MAX}) { auto value = sentinel; Check(frozen->SampleSelector(index, 0, value) == Status::InvalidIndex && Equal(value, sentinel), "invalid selector index preserves output"); }
    for (uint32_t index : {2u, UINT32_MAX}) { int32_t out = -99; Check(!frozen->IkBone(index, out) && out == -99, "invalid IK mapping preserves output"); }
    for (uint32_t index : {4u, UINT32_MAX}) { int32_t out = -99; Check(!frozen->SelectorBone(index, out) && out == -99, "invalid selector mapping preserves output"); }
    Model empty; empty.record->ikChainCount = empty.record->boneOrderCount = empty.record->boneCount = 0;
    empty.record->ikChains = nullptr; empty.record->boneOrderTable = nullptr; empty.record->boneTable = nullptr;
    for (std::size_t i = 0; i < empty.capacity; ++i) { empty.record->displayKeys[i].ikStates = nullptr; empty.record->displayKeys[i].selectorStates = nullptr; }
    const SourceState before(empty); const auto noNested = Frozen::Capture(empty.raw()); auto visible = byte;
    Check(noNested->KeyCount() == 6 && noNested->IkCount() == 0 && noNested->SelectorCount() == 0 &&
        noNested->SampleVisibility(20, visible) == Status::Ok && visible == 164, "zero nested counts require no arrays but retain visibility");
    auto parent = sentinel; auto ik = byte;
    Check(noNested->SampleIk(0, 0, ik) == Status::InvalidIndex && ik == byte, "empty IK channel rejects index");
    Check(noNested->SampleSelector(0, 0, parent) == Status::InvalidIndex && Equal(parent, sentinel), "empty selector channel rejects index"); before.Same(empty);
}

void InvalidSourcesAndMappings() {
    Check(Throws<std::invalid_argument>([] { Frozen::Capture(nullptr); }), "null model rejects capture");
    for (unsigned mode = 0; mode < 19; ++mode) {
        Model source; auto* keys = source.record->displayKeys;
        switch (mode) {
        case 0: keys[0].frame = 1; break;
        case 1: keys[0].next = 8; break;
        case 2: keys[4].previous = 2; break;
        case 3: keys[4].next = 4; break;
        case 4: keys[2].frame = keys[4].frame; break;
        case 5: keys[2].frame = uint32_t(frame_limits::maximum) + 1; break;
        case 6: keys[0].previous = 8; break;
        case 7: keys[4].ikStates = nullptr; break;
        case 8: keys[4].selectorStates = nullptr; break;
        case 9: source.record->ikChains = nullptr; break;
        case 10: source.record->boneOrderTable = nullptr; break;
        case 11: source.ik[0].boneIndex = -1; break;
        case 12: source.ik[0].boneIndex = 3; break;
        case 13: source.order[0].boneIndex = 0; break;
        case 14: source.order[1].boneIndex = -1; break;
        case 15: source.order[1].boneIndex = 3; break;
        case 16: source.record->boneCount = 0; break;
        case 17: keys[3].next = 4; break;
        case 18: source.record->boneCount = uint32_t(INT32_MAX) + 1; break;
        }
        const SourceState before(source);
        Check(Throws<std::invalid_argument>([&] { Frozen::Capture(source.raw()); }), "malformed chain, payload or mapping rejects capture"); before.Same(source);
    }
    Model missing; auto* keys = missing.record->displayKeys; missing.record->displayKeys = nullptr;
    Check(Throws<std::invalid_argument>([&] { Frozen::Capture(missing.raw()); }) && missing.record->displayKeys == nullptr, "missing display pool is not repaired by capture");
    missing.record->displayKeys = keys;
    for (bool ik : {false, true}) {
        Model excessive;
        if (ik) excessive.record->ikChainCount = 1000000;
        else excessive.record->boneOrderCount = 1000000;
        const SourceState before(excessive);
        // Deliberately retain tiny real tables. The mandatory retained-size
        // lower bound must reject this request before traversing either table.
        Check(Throws<std::length_error>([&] { Frozen::Capture(excessive.raw(), 1024); }),
            "impossible mapping and first payload-row budget rejects before reading oversized declared tables");
        before.Same(excessive);
    }
}

void RuntimeCapacityAndReachability() {
    Model source; const auto clean = Frozen::Capture(source.raw());
    auto& unused = source.record->displayKeys[7]; unused.frame = UINT32_MAX; unused.next = 999999; unused.previous = UINT32_MAX;
    unused.ikStates = nullptr; unused.selectorStates = nullptr; unused.allocated = 255;
    const SourceState before(source); const auto ignored = Frozen::Capture(source.raw(), clean->RetainedBytes());
    Check(clean->RetainedBytes() == ignored->RetainedBytes() && ignored->KeyCount() == 6, "unreachable malformed keys are not copied or charged");
    for (double frame : {0., 4.5, 10., 29.25, 30., 999.}) { Sample a, b; Check(Read(*clean, frame, a) && Read(*ignored, frame, b) && Equal(a, b), "unreachable payload garbage cannot affect samples"); }
    before.Same(source);
    for (bool registered : {false, true}) {
        Model extended(registered ? 1004 : 1000, registered), oracle(registered ? 1004 : 1000);
        const unsigned last = registered ? 1003u : 999u;
        auto& tail = extended.record->displayKeys[last]; tail.frame = 100; tail.previous = 3; tail.next = 0;
        tail.visible = 249; extended.switches[last] = {197, 234};
        extended.parents[last] = {{{42, -700}, {-1, -8}, {INT32_MAX, INT32_MIN}, {-1, 0}}};
        extended.record->displayKeys[3].next = last;
        Check(mdl::KeyPoolCapacity(extended.record->displayKeys) == extended.capacity, "fixture resolves dynamic or borrowed legacy capacity");
        Check(bool(mdl::KeyPoolIdentity(extended.record->displayKeys)) == registered, "borrowed and registered pools are distinct cases");
        Mirror(extended, oracle); const SourceState original(extended); const auto frozen = Frozen::Capture(extended.raw());
        Check(frozen->KeyCount() == 7, "last key beyond legacy bound is retained when registered");
        for (double frame : {29.5, 30., 99.5, 100., 100.125, 200.}) CompareSeek(*frozen, oracle, frame);
        original.Same(extended);
    }
}

void LifetimeAndConcurrentReads() {
    auto source = std::make_unique<Model>(); const auto frozen = Frozen::Capture(source->raw());
    constexpr std::array<double, 8> times{0., .125, 5., 9.75, 15., 20.25, 30., 100.}; std::array<Sample, times.size()> expected;
    for (std::size_t i = 0; i < times.size(); ++i) Check(Read(*frozen, times[i], expected[i]), "prepare lifetime oracle before source edits");
    source->record->displayKeys[3].visible = 13; source->switches[3] = {17, 19}; source->parents[3][0] = {-1, 999};
    source->ik[0].boneIndex = 1; source->order[1].boneIndex = 2;
    const auto changed = Frozen::Capture(source->raw()); Sample oldSample, newSample;
    Check(Read(*frozen, 100, oldSample) && Read(*changed, 100, newSample) && oldSample.visibility == 205 && newSample.visibility == 13 &&
        oldSample.ik[0] == 250 && newSample.ik[0] == 17 && oldSample.parents[0].boneIndex == 4 && newSample.parents[0].boneIndex == 999,
        "new capture sees edits while old snapshot owns original nested payloads");
    source.reset();
    std::atomic<unsigned> failures{0}; std::array<std::thread, 4> workers;
    try {
        for (auto& worker : workers) worker = std::thread([&] {
            failAfter = 0;
            for (unsigned repeat = 0; repeat < 300; ++repeat) for (std::size_t i = 0; i < times.size(); ++i) {
                Sample actual; int32_t ik = 99, selector = 99;
                if (!Read(*frozen, times[i], actual) || !Equal(actual, expected[i]) || !frozen->IkBone(0, ik) || ik != 2 ||
                    !frozen->SelectorBone(1, selector) || selector != 0) ++failures;
            }
            failAfter = -1;
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (auto& worker : workers) worker.join();
    Check(failures == 0, "destroyed source supports four concurrent allocation-free readers");
    int32_t ik = 99, selector = 99;
    Check(Read(*changed, 100, newSample) && newSample.visibility == 13 && changed->IkBone(0, ik) && ik == 1 &&
        changed->SelectorBone(1, selector) && selector == 2, "new capture retains its own mappings after source destruction");
}

void BudgetAndAllocationFailures() {
    Model source; const SourceState before(source); const auto frozen = Frozen::Capture(source.raw()); const auto bytes = frozen->RetainedBytes();
    Check(bytes > 0 && bytes <= 64ull * 1024 * 1024, "snapshot reports retained storage");
    Check(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), 0); }), "zero retained budget rejects capture");
    Check(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), bytes-1); }), "one byte below retained capacity rejects capture");
    Check(Frozen::Capture(source.raw(), bytes)->RetainedBytes() == bytes, "exact retained budget succeeds"); before.Same(source);
    bool completed = false; unsigned failures = 0;
    for (int limit = 0; limit < 2048; ++limit) {
        std::unique_ptr<Frozen> result; bool memory = false, unexpected = false; failAfter = limit;
        try { result = Frozen::Capture(source.raw()); } catch (const std::bad_alloc&) { memory = true; } catch (...) { unexpected = true; }
        failAfter = -1; Check(!unexpected, "allocation failure remains bad_alloc"); before.Same(source);
        if (result) {
            Sample a, b; Check(Read(*frozen, 10.25, a) && Read(*result, 10.25, b) && Equal(a, b), "successful allocation sweep returns complete independent data");
            completed = true; break;
        }
        Check(memory, "failure cannot return partial snapshot"); ++failures;
    }
    Check(completed && failures > 0, "allocation sweep reaches success");
    std::printf("display snapshot capture allocation failures=%u\n", failures);
}
}

int main() {
    NativeParityAndMappings(); LongMatchingIntervals(); InvalidRequestsAndEmptyPayloads(); InvalidSourcesAndMappings();
    RuntimeCapacityAndReachability(); LifetimeAndConcurrentReads(); BudgetAndAllocationFailures();
    std::printf("PASS %u display snapshot checks\n", checks); return 0;
}
