// Frozen registered morph weights: native seek parity and read-only ownership.
#define NOMINMAX
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_limits.hpp"
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
#include <string>
#include <thread>
#include <utility>
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
namespace mc = morph_curve;
namespace ac = animation_curve;
using Frozen = mc::SamplingState;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool SameBits(float a, float b) { return !std::memcmp(&a, &b, sizeof(a)); }
template<class Exception, class F> bool Throws(F&& call) {
    try { call(); } catch (const Exception&) { return true; } catch (...) {}
    return false;
}
struct Model {
    std::unique_ptr<mdl::ModelRecord> record = std::make_unique<mdl::ModelRecord>();
    std::array<mdl::MorphRecord, 2> morphs{};
    mdl::DisplayKey display{};
    Model() {
        record->morphCount = uint32_t(morphs.size()); record->morphs = morphs.data();
        record->morphKeys = mdl::MorphKeyPoolCreate(32);
        Check(record->morphKeys != nullptr, "allocate owned morph pool");
        record->displayKeys = &display; display.visible = 1;
        std::memcpy(morphs[0].name, "first", 6); std::memcpy(morphs[1].name, "second", 7);
        // Group/impulse types are still registered weights, not expanded here.
        morphs[0].type = 0; morphs[1].type = 9;
        morphs[0].panel = mdl::MorphPanel(0); morphs[1].panel = mdl::MorphPanel(3);
        auto* keys = record->morphKeys;
        keys[0].value = -3.125f; keys[0].next = 2;
        keys[2].frame = 7; keys[2].previous = 0; keys[2].next = 3; keys[2].value = .12345679f;
        keys[3].frame = 29; keys[3].previous = 2; keys[3].next = 4; keys[3].value = -9.0078f;
        keys[4].frame = 63; keys[4].previous = 3; keys[4].value = 2.75f;
        keys[1].value = .8f; keys[1].next = 5;
        keys[5].frame = 15; keys[5].previous = 1; keys[5].next = 6; keys[5].value = 4.125f;
        keys[6].frame = 63; keys[6].previous = 5; keys[6].value = -2.25f;
        for (unsigned i = 0; i < 7; ++i) keys[i].allocated = 1;
        record->maxFrame = 63;
        morphs[0].value = 321; morphs[1].value = -456;
    }
    unsigned char* raw() const { return reinterpret_cast<unsigned char*>(record.get()); }
    unsigned Slot(unsigned morph, unsigned frame) const {
        const auto capacity = mdl::MorphKeyCapacity(*record); unsigned slot = morph;
        for (std::size_t hops = 0; hops < capacity; ++hops) {
            Check(slot < capacity, "fixture slot stays in pool");
            const auto& key = record->morphKeys[slot];
            if (key.frame == frame) return slot;
            if (!key.next) break;
            slot = key.next;
        }
        Check(false, "fixture key exists"); return 0;
    }
    ~Model() { mc::Forget(raw()); mdl::MorphKeyPoolDestroy(record->morphKeys); }
};
struct SourceState {
    std::unique_ptr<mdl::ModelRecord> record;
    std::array<mdl::MorphRecord, 2> morphs;
    mdl::DisplayKey display;
    std::vector<mdl::MorphKey> keys;
    mc::AuthoredLayers layers;
    explicit SourceState(const Model& source) : record(std::make_unique<mdl::ModelRecord>(*source.record)),
        morphs(source.morphs), display(source.display),
        keys(source.record->morphKeys, source.record->morphKeys + mdl::MorphKeyCapacity(*source.record)) {
        std::string error; Check(mc::CaptureLayers(source.raw(), layers, error), "capture pristine and derived author layers");
    }
    void Same(const Model& source) const {
        Check(!std::memcmp(record.get(), source.record.get(), sizeof(*record)), "sampling leaves model metadata and cursors unchanged");
        Check(!std::memcmp(morphs.data(), source.morphs.data(), sizeof(morphs)), "sampling leaves unregistered morph pose and records unchanged");
        Check(!std::memcmp(&display, &source.display, sizeof(display)), "sampling leaves display state unchanged");
        Check(keys.size() == mdl::MorphKeyCapacity(*source.record) &&
            !std::memcmp(keys.data(), source.record->morphKeys, keys.size() * sizeof(mdl::MorphKey)), "sampling leaves all native key bytes unchanged");
        Check(mc::LayersUnchanged(source.raw(), layers), "sampling leaves pristine/effective/mirror/observed author layers unchanged");
    }
};
void MirrorNative(const Model& source, Model& oracle) {
    const auto capacity = mdl::MorphKeyCapacity(*source.record);
    Check(mdl::MorphKeyPoolEnsure(*oracle.record, capacity), "prepare independent oracle pool");
    std::memcpy(oracle.record->morphKeys, source.record->morphKeys, capacity * sizeof(mdl::MorphKey));
}
ac::Track FreeTrack(double factor = 1) {
    ac::Track track;
    for (const auto pair : std::array<std::pair<int, double>, 4>{{{0, -.25}, {12, .9}, {30, .4}, {60, -1.5}}}) {
        ac::Key key; key.frame = pair.first; key.value = factor * pair.second;
        key.interpolation = ac::Interpolation::Bezier; key.tangent = ac::Tangent::Free;
        key.inWeight = .31; key.outWeight = .27;
        key.inSlope = -.45 * factor; key.outSlope = .6 * factor;
        track.keys.push_back(key);
    }
    return track;
}
void Write(Model& source, const ac::Track& track) {
    ac::NamedTrack named; named.id = mc::StableId(0, source.morphs[0].name); named.track = track;
    std::string error; Check(mc::Write(source.raw(), {named}, error), "write real authored morph curve");
}

void NativeParityAndMarks() {
    Model source, oracle; MirrorNative(source, oracle);
    const SourceState initial(source); const auto frozen = Frozen::Capture(source.raw());
    Check(frozen->MorphCount() == 2 && frozen->RetainedBytes() > 0, "capture all registered morph types and panels");
    for (int frame = 0; frame < 81; ++frame) {
        SeekModelFrame(oracle.raw(), frame, 0);
        for (int morph = 0; morph < 2; ++morph) {
            float sampled = 99;
            Check(frozen->Sample(morph, frame, sampled), "sample native integer frame");
            Check(SameBits(sampled, oracle.morphs[morph].value), "native integer weight matches real SeekModelFrame bits");
        }
    }
    SeekModelFrame(oracle.raw(), frame_limits::maximum, 0);
    for (int morph = 0; morph < 2; ++morph) {
        float sampled = 99;
        Check(frozen->Sample(morph, frame_limits::maximum, sampled) && SameBits(sampled, oracle.morphs[morph].value),
            "last supported frame holds terminal registered weight");
    }
    initial.Same(source);
    for (unsigned slot = 0; slot < 7; ++slot) source.record->morphKeys[slot].allocated = 0;
    const SourceState unmarked(source); const auto noMarks = Frozen::Capture(source.raw());
    for (double frame : {0., .125, 6.75, 7., 15.5, 29., 62.75, 100.}) for (int morph = 0; morph < 2; ++morph) {
        float a = 0, b = 0;
        Check(frozen->Sample(morph, frame, a) && noMarks->Sample(morph, frame, b) && SameBits(a, b),
            "export/selection marks do not remove native keys from sampling");
    }
    unmarked.Same(source);
    Model linear;
    linear.record->morphKeys[0].value = -2.5f;
    auto& end = linear.record->morphKeys[2]; end.frame = 8; end.value = 5.5f;
    const auto fraction = Frozen::Capture(linear.raw());
    for (double frame : {0., .125, .25, 1.5, 7.875, 8.}) {
        float value = 0;
        Check(fraction->Sample(0, frame, value) && SameBits(value, float(frame - 2.5)), "fractional native frame is continuous and not clamped to weight range");
    }
}

void AuthoredAndPendingNative() {
    Model source, oracle; const auto track = FreeTrack(); Write(source, track); Write(oracle, track);
    const SourceState pristine(source); const auto first = Frozen::Capture(source.raw());
    bool overshoot = false;
    for (double frame : {0., .125, 3., 6., 9.75, 12., 19.125, 29.875, 30., 40.5, 60., 80.}) {
        const auto expected = float(ac::Evaluate(track, frame).value); float value = 0;
        Check(first->Sample(0, frame, value) && SameBits(value, expected), "free curve retains exact author evaluation until float conversion");
        overshoot = overshoot || value > 1;
    }
    Check(overshoot, "free curve fixture actually overshoots one"); pristine.Same(source);
    // Do not call Read or Evaluate on source after the native edit: they sync.
    source.record->morphKeys[source.Slot(0, 12)].value = 1.2345678f;
    source.morphs[0].value = -987;
    Check(mc::LayersUnchanged(source.raw(), pristine.layers), "native edit remains pending in original author maps");
    MirrorNative(source, oracle); ac::Track expected; std::string error;
    Check(mc::Read(oracle.raw(), 0, expected, error), "derive pending-native oracle in a different model");
    const SourceState pending(source); const auto updated = Frozen::Capture(source.raw());
    for (double frame : {0., .125, 6.5, 11.75, 12., 17.5, 30., 45.25, 60.}) {
        float value = 0;
        Check(updated->Sample(0, frame, value) && SameBits(value, float(ac::Evaluate(expected, frame).value)),
            "capture privately derives pending native edits without synchronizing source authoring");
    }
    pending.Same(source);
    float oldValue = 0, newValue = 0;
    Check(first->Sample(0, 12, oldValue) && updated->Sample(0, 12, newValue) && oldValue == float(.9) && newValue == 1.2345678f,
        "new native edit changes only a newly captured snapshot");
    for (std::size_t slot = 0; slot < mdl::MorphKeyCapacity(*source.record); ++slot) source.record->morphKeys[slot].allocated = 0;
    const SourceState unmarked(source); const auto afterMarks = Frozen::Capture(source.raw());
    for (double frame : {.125, 6.5, 17.5, 45.25}) {
        float a = 0, b = 0;
        Check(updated->Sample(0, frame, a) && afterMarks->Sample(0, frame, b) && SameBits(a, b), "selection changes preserve effective authored motion");
    }
    unmarked.Same(source);

    // First let the ordinary reader cache the changed native value. Restoring
    // the installed value must re-derive the pristine curve privately; copying
    // the cached effective curve would keep the stale linear fallback.
    ac::Track cached;
    Check(mc::Read(source.raw(), 0, cached, error), "cache edited native morph in live effective layer");
    source.record->morphKeys[source.Slot(0, 12)].value = float(.9);
    const SourceState restoredNative(source);
    const auto restoredCurve = Frozen::Capture(source.raw());
    for (double frame : {.125, 6.5, 12., 17.5, 45.25}) {
        float value = 0;
        Check(restoredCurve->Sample(0, frame, value) && SameBits(value, float(ac::Evaluate(track, frame).value)),
            "restoring installed native values restores pristine free curve in new snapshot");
    }
    restoredNative.Same(source);
}

void StaleIdentityIsReadOnly() {
    Model source, nativeOracle; Write(source, FreeTrack()); MirrorNative(source, nativeOracle);
    const SourceState before(source);
    const char original = source.morphs[0].name[0]; source.morphs[0].name[0] = 'X';
    const SourceState stale(source); const auto frozen = Frozen::Capture(source.raw());
    for (int frame : {0, 6, 12, 21, 30, 48, 60}) {
        SeekModelFrame(nativeOracle.raw(), frame, 0); float value = 0;
        Check(frozen->Sample(0, frame, value) && SameBits(value, nativeOracle.morphs[0].value), "stale identity falls back to registered native weights");
    }
    stale.Same(source);
    source.morphs[0].name[0] = original;
    before.Same(source); // Reveals an accidental erase that stale comparison cannot detect.
    const auto restored = Frozen::Capture(source.raw()); float value = 0;
    Check(restored->Sample(0, 6, value) && SameBits(value, float(ac::Evaluate(FreeTrack(), 6).value)), "restoring identity finds untouched pristine authoring");
}

void InvalidRequestsAndEmptyModel() {
    Model source; const auto frozen = Frozen::Capture(source.raw()); constexpr float sentinel = 987.25f;
    for (int morph : {-1, 2, (std::numeric_limits<int>::max)()}) {
        float out = sentinel; Check(!frozen->Sample(morph, 0, out) && SameBits(out, sentinel), "invalid morph preserves output");
    }
    for (double frame : {-1., -.125, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), double(frame_limits::maximum) + .5}) {
        float out = sentinel; Check(!frozen->Sample(0, frame, out) && SameBits(out, sentinel), "invalid time preserves output");
    }
    auto empty = std::make_unique<mdl::ModelRecord>();
    const auto zero = Frozen::Capture(reinterpret_cast<const unsigned char*>(empty.get()));
    float out = sentinel;
    Check(zero->MorphCount() == 0 && !zero->Sample(0, 0, out) && SameBits(out, sentinel), "empty model captures without inventing a morph");
    Check(Throws<std::invalid_argument>([] { Frozen::Capture(nullptr); }), "null model is not an empty model");
    Model extreme;
    extreme.record->morphKeys[0].value = -(std::numeric_limits<float>::max)();
    extreme.record->morphKeys[2].value = (std::numeric_limits<float>::max)();
    const auto overflowing = Frozen::Capture(extreme.raw()); out = sentinel;
    Check(!overflowing->Sample(0, 3.5, out) && SameBits(out, sentinel), "finite native endpoints with overflowing float delta preserve output");
    Check(overflowing->Sample(0, 0, out) && out == -(std::numeric_limits<float>::max)(), "exact native endpoint remains representable");
    Model freeExtreme; auto huge = FreeTrack(); huge.keys[0].outSlope = 1e40;
    Write(freeExtreme, huge); const auto freeOverflow = Frozen::Capture(freeExtreme.raw()); out = sentinel;
    Check(!freeOverflow->Sample(0, 6, out) && SameBits(out, sentinel), "finite double free curve exceeding float range preserves output");
}

void InvalidChainsAndReachability() {
    for (unsigned scenario = 0; scenario < 13; ++scenario) {
        Model source; auto* keys = source.record->morphKeys;
        switch (scenario) {
        case 0: keys[0].next = 1000; break;
        case 1: keys[0].next = 1; break;
        case 2: keys[1].next = 2; break;
        case 3: keys[2].previous = 1; break;
        case 4: keys[3].next = 2; break;
        case 5: keys[2].frame = 0; break;
        case 6: keys[0].frame = 1; break;
        case 7: keys[3].frame = keys[2].frame; break;
        case 8: keys[2].frame = uint32_t(frame_limits::maximum) + 1; break;
        case 9: keys[2].value = std::numeric_limits<float>::quiet_NaN(); break;
        case 10: keys[2].value = std::numeric_limits<float>::infinity(); break;
        case 11: keys[0].previous = 1000; break;
        case 12: keys[4].next = 1000; break;
        }
        const SourceState before(source);
        Check(Throws<std::invalid_argument>([&] { Frozen::Capture(source.raw()); }), "invalid chain, foreign head, shared slot, frame or value rejects capture");
        before.Same(source);
    }
    Model missing; auto* pool = missing.record->morphKeys;
    missing.record->morphKeys = nullptr;
    Check(Throws<std::invalid_argument>([&] { Frozen::Capture(missing.raw()); }), "nonempty model requires native pool");
    Check(missing.record->morphKeys == nullptr, "missing pool is not repaired by capture"); missing.record->morphKeys = pool;
    missing.record->morphs = nullptr;
    Check(Throws<std::invalid_argument>([&] { Frozen::Capture(missing.raw()); }), "nonempty model requires morph records");
    Check(missing.record->morphs == nullptr, "missing table is not repaired by capture"); missing.record->morphs = missing.morphs.data();
    missing.record->morphCount = 33;
    Check(Throws<std::invalid_argument>([&] { Frozen::Capture(missing.raw()); }), "morph heads cannot exceed pool capacity");
    missing.record->morphCount = 2;
    Model source; const auto original = Frozen::Capture(source.raw());
    auto& orphan = source.record->morphKeys[31]; orphan.frame = 123; orphan.previous = 123456; orphan.next = 31;
    orphan.value = std::numeric_limits<float>::quiet_NaN(); orphan.allocated = 1;
    const SourceState dirty(source); const auto reachable = Frozen::Capture(source.raw(), original->RetainedBytes());
    Check(reachable->RetainedBytes() == original->RetainedBytes(), "unreachable storage is not retained or charged");
    for (double frame : {0., .125, 7., 28.5, 63., 99.}) for (int morph = 0; morph < 2; ++morph) {
        float a = 0, b = 0;
        Check(original->Sample(morph, frame, a) && reachable->Sample(morph, frame, b) && SameBits(a, b), "unreachable malformed storage never enters sampled motion");
    }
    dirty.Same(source);
}

void LifetimeAndWorkers() {
    auto source = std::make_unique<Model>(); Write(*source, FreeTrack());
    const auto frozen = Frozen::Capture(source->raw());
    constexpr std::array<double, 8> times{0., .125, 6.5, 12., 19.25, 30., 60., 100.};
    std::array<std::array<float, 8>, 2> expected{};
    for (int morph = 0; morph < 2; ++morph) for (std::size_t i = 0; i < times.size(); ++i)
        Check(frozen->Sample(morph, times[i], expected[morph][i]), "capture parallel baseline");
    Write(*source, FreeTrack(2)); const auto changed = Frozen::Capture(source->raw());
    float oldValue = 0, newValue = 0;
    Check(frozen->Sample(0, 60, oldValue) && changed->Sample(0, 60, newValue) && oldValue == -1.5f && newValue == -3.f,
        "old and new captures keep different authored motion");
    source.reset();
    std::atomic<unsigned> errors{0}; std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        failAfter = 0;
        for (unsigned repeat = 0; repeat < 300; ++repeat) for (int morph = 0; morph < 2; ++morph)
            for (std::size_t i = 0; i < times.size(); ++i) {
                float actual = 0;
                if (!frozen->Sample(morph, times[i], actual) || !SameBits(actual, expected[morph][i])) ++errors;
            }
        failAfter = -1;
    });
    for (auto& worker : workers) worker.join();
    Check(errors == 0, "four concurrent readers survive source destruction without allocation");
    Check(changed->Sample(0, 60, newValue) && newValue == -3.f, "newer snapshot also survives source destruction");
}

void BudgetAndAllocationAtomicity() {
    Model source; Write(source, FreeTrack());
    source.record->morphKeys[source.Slot(0, 12)].value = 1.2345678f;
    const SourceState before(source); const auto initial = Frozen::Capture(source.raw());
    const auto bytes = initial->RetainedBytes();
    Check(bytes > 0 && bytes <= 64ull * 1024 * 1024, "retained byte accounting is nonzero and fits default cap");
    Check(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), 0); }), "zero retained budget is rejected");
    Check(Throws<std::length_error>([&] { Frozen::Capture(source.raw(), bytes - 1); }), "one byte below actual retained amount is rejected");
    Check(Frozen::Capture(source.raw(), bytes)->RetainedBytes() == bytes, "exact retained budget succeeds");
    before.Same(source);
    bool success = false; unsigned failures = 0;
    for (int limit = 0; limit < 2048; ++limit) {
        std::unique_ptr<Frozen> captured; bool memory = false, other = false;
        failAfter = limit;
        try { captured = Frozen::Capture(source.raw()); }
        catch (const std::bad_alloc&) { memory = true; }
        catch (...) { other = true; }
        failAfter = -1;
        Check(!other, "injected allocation failure remains bad_alloc"); before.Same(source);
        if (captured) {
            float value = 0;
            Check(captured->Sample(0, 12, value) && value == 1.2345678f, "successful sweep captures pending native edit privately");
            success = true; break;
        }
        Check(memory, "allocation failure cannot return a partial snapshot"); ++failures;
    }
    Check(success && failures > 0, "allocation sweep reaches successful complete capture");
    std::printf("morph sampling capture allocation failures=%u\n", failures);
}
}

int main() {
    NativeParityAndMarks(); AuthoredAndPendingNative(); StaleIdentityIsReadOnly(); InvalidRequestsAndEmptyModel();
    InvalidChainsAndReachability(); LifetimeAndWorkers(); BudgetAndAllocationAtomicity();
    std::printf("PASS %u morph sampling checks\n", checks); return 0;
}
