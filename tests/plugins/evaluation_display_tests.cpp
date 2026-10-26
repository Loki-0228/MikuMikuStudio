// Evaluation 1.2: frozen display channels, stable parents and old ABI prefixes.
#include "evaluation_store.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

static thread_local int failAfter = -1;
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

using namespace mikumikustudio::plugins;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
static constexpr uint32_t boneMask = MMS_EVALUATION_CAP_BONE_CHANNELS, morphMask = MMS_EVALUATION_CAP_MORPH_WEIGHTS;
static constexpr uint32_t displayMask = MMS_EVALUATION_CAP_DISPLAY, allMask = boneMask | morphMask | displayMask;
static constexpr uint64_t MiB = 1024ull * 1024;
static constexpr MmsHandle sentinel = 0x123456789abcdef0ull;
static constexpr MmsObjectRef object{{100, 21}, 7};
static constexpr MmsId boneId{110, 21}, morphId{120, 21}, targetBone{990, 14};
template<class T> static T Poison() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> static bool Unchanged(const T& value) { const auto expected = Poison<T>(); return !std::memcmp(&value, &expected, sizeof(value)); }
template<class T> static bool HeaderOk(const T& value, unsigned minor = 0) { return value.h.size == sizeof(T) && value.h.major == 1 && value.h.minor == minor; }
template<class E, class F> static bool Throws(F&& call) { try { call(); } catch (const E&) { return true; } catch (...) {} return false; }
static MmsEvaluationSourceQueryV1 Query(uint64_t budget = 0) { return {Header<MmsEvaluationSourceQueryV1>(), object, budget, 0, 0}; }
static MmsEvaluationApiV1_2 Bind(EvaluationStore& store, EvaluationStore::Client& client) { client.owner = &store; return store.Api12(client); }
static MmsBoneChannelsV1 Bone(double frame) {
    MmsBoneChannelsV1 value{}; value.h = Header<MmsBoneChannelsV1>(); value.position[0] = float(frame);
    value.rotation[3] = 1; value.scale[0] = value.scale[4] = value.scale[8] = 1; return value;
}
static MmsParentSelectionV1 Parent(double frame, uint64_t generation = 5) {
    MmsParentSelectionV1 value{}; value.h = Header<MmsParentSelectionV1>();
    if (frame < 10) value.windowEnd = 10;
    else if (frame < 20) {
        value.target = {{909, 14}, generation}; value.bone = targetBone;
        value.windowStart = 10; value.windowEnd = 20; value.flags = MMS_EVALUATION_PARENT_ACTIVE;
    } else value.windowStart = 20;
    return value;
}
static std::shared_ptr<EvaluationData> Data(const MmsEvaluationSourceQueryV1& query, uint32_t mask,
    uint64_t bytes = 8192, uint64_t parentGeneration = 5) {
    auto data = std::make_shared<EvaluationData>(); data->object = query.object;
    data->channels = mask; data->hostRevision = 39; data->retainedBytes = bytes;
    if (mask & boneMask) {
        data->bones = {{Header<MmsEvaluationBoneV1>(), boneId, MMS_EVALUATION_BONE_LOCAL, 0}};
        data->sample = [](uint32_t, double frame, auto& out) { out = Bone(frame); return MMS_OK; };
    }
    if (mask & morphMask) {
        data->morphs = {{Header<MmsEvaluationMorphV1>(), morphId, MMS_MORPH_VERTEX, 0}};
        data->sampleMorph = [](uint32_t, double frame, auto& out) { out = {Header<MmsMorphWeightV1>(), float(frame - 15), 0}; return MMS_OK; };
    }
    if (mask & displayMask) {
        // Repeated references are valid mappings, not new channel identities.
        data->ik = {{Header<MmsEvaluationIkV1>(), boneId}, {Header<MmsEvaluationIkV1>(), boneId}, {Header<MmsEvaluationIkV1>(), {111, 21}}};
        data->selectors = {{Header<MmsEvaluationSelectorV1>(), {}, MMS_EVALUATION_SELECTOR_GLOBAL, 0},
            {Header<MmsEvaluationSelectorV1>(), boneId, 0, 0}, {Header<MmsEvaluationSelectorV1>(), boneId, 0, 0}};
        data->sampleVisibility = [](double frame, auto& out) { out = {Header<MmsDisplayStateV1>(), frame < 10 ? 197u : 255u, 0}; return MMS_OK; };
        data->sampleIk = [](uint32_t index, double, auto& out) { out = {Header<MmsIkStateV1>(), index == 0 ? 255u : index == 1 ? 197u : 0u, 0}; return MMS_OK; };
        data->sampleParent = [parentGeneration](uint32_t, double frame, auto& out) { out = Parent(frame, parentGeneration); return MMS_OK; };
    }
    return data;
}
static MmsHandle Create(const MmsEvaluationApiV1_2& api, uint32_t mask = allMask, uint64_t budget = 0) {
    const auto query = Query(budget); MmsHandle handle = sentinel;
    CHECK(api.createSourceChannels(api.context, &query, mask, &handle) == MMS_OK && handle && handle != sentinel); return handle;
}
static MmsEvaluationSourceInfoV1_2 Describe(const MmsEvaluationApiV1_2& api, MmsHandle source) {
    auto info = Poison<MmsEvaluationSourceInfoV1_2>();
    CHECK(api.describeSourceDisplay(api.context, source, &info) == MMS_OK && HeaderOk(info, 2) && !info.reserved); return info;
}

static void CapabilityNegotiationAndMasks() {
    EvaluationStore::CaptureChannels capture = [](const auto& query, uint32_t mask, uint64_t, auto& out) { out = Data(query, mask); return MMS_OK; };
    CHECK(Throws<std::invalid_argument>([&] { EvaluationStore invalid(capture, 0); }));
    CHECK(Throws<std::invalid_argument>([&] { EvaluationStore invalid(capture, 8); }));
    CHECK(Throws<std::invalid_argument>([&] { EvaluationStore invalid(capture, 15); }));
    {
        EvaluationStore legacy(EvaluationStore::Capture([](const auto& query, uint64_t, auto& out) { out = Data(query, boneMask); return MMS_OK; }));
        EvaluationStore::Client client; const auto api = Bind(legacy, client); MmsEvaluationCapsV1 caps{};
        CHECK(api.getCapabilities(api.context, &caps) == MMS_OK && caps.flags == boneMask);
    }
    {
        unsigned called = 0; EvaluationStore old(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
            ++called; out = Data(query, mask); return MMS_OK;
        }));
        EvaluationStore::Client client; const auto api = Bind(old, client); MmsEvaluationCapsV1 caps{};
        CHECK(api.getCapabilities(api.context, &caps) == MMS_OK && caps.flags == 3);
        MmsHandle rejected = sentinel; const auto query = Query();
        CHECK(api.createSourceChannels(api.context, &query, displayMask, &rejected) == MMS_E_UNSUPPORTED && rejected == sentinel && called == 0);
        old.Retire(client);
        CHECK(api.createSourceChannels(api.context, &query, displayMask, &rejected) == MMS_E_NOT_READY && rejected == sentinel);
    }
    for (uint32_t supported = 1; supported <= allMask; ++supported) {
        uint32_t expectedMask = 0; unsigned calls = 0;
        EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t limit, auto& out) {
            CHECK(mask == expectedMask && limit && limit <= 64 * MiB); ++calls; out = Data(query, mask); return MMS_OK;
        }), supported);
        EvaluationStore::Client client; const auto api = Bind(store, client);
        CHECK(HeaderOk(api, 2) && sizeof(api) == 152); MmsEvaluationCapsV1 caps{};
        CHECK(api.getCapabilities(api.context, &caps) == MMS_OK && HeaderOk(caps) && caps.flags == supported);
        const auto query = Query();
        for (uint32_t mask = 1; mask <= allMask; ++mask) {
            expectedMask = mask; const auto before = calls; MmsHandle handle = sentinel;
            const auto result = api.createSourceChannels(api.context, &query, mask, &handle);
            if (mask & ~supported) CHECK(result == MMS_E_UNSUPPORTED && handle == sentinel && calls == before);
            else {
                CHECK(result == MMS_OK && calls == before + 1); const auto info = Describe(api, handle);
                CHECK(sizeof(info) == 80 && info.flags == mask && info.boneCount == ((mask & boneMask) ? 1u : 0u) &&
                    info.morphCount == ((mask & morphMask) ? 1u : 0u) && info.ikCount == ((mask & displayMask) ? 3u : 0u) && info.selectorCount == info.ikCount);
                CHECK(api.release(api.context, handle) == MMS_OK);
            }
        }
        for (uint32_t mask : {0u, 8u, 15u, UINT32_MAX}) {
            MmsHandle handle = sentinel; const auto before = calls;
            CHECK(api.createSourceChannels(api.context, &query, mask, &handle) == MMS_E_ARGUMENT && handle == sentinel && calls == before);
        }
        CHECK(!store.HasLeases(client));
    }
}

static void PrefixesAndDisplayRecords() {
    uint64_t generation = 5; uint32_t expectedMask = boneMask;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
        CHECK(mask == expectedMask); out = Data(query, mask, 8192, generation); return MMS_OK;
    }), allMask);
    EvaluationStore::Client client; const auto api = Bind(store, client);
    const auto old = store.Api(client); const auto mid = store.Api11(client);
    CHECK(HeaderOk(old) && HeaderOk(mid, 1) && old.context == api.context && mid.context == api.context);
    MmsEvaluationApiV1 prefix0{}; MmsEvaluationApiV1_1 prefix1{};
    std::memcpy(&prefix0, &api, sizeof(prefix0)); std::memcpy(&prefix1, &api, sizeof(prefix1));
    const auto query = Query(); MmsHandle first = 0, second = 0;
    CHECK(prefix0.createSource(prefix0.context, &query, &first) == MMS_OK && Describe(api, first).flags == boneMask);
    CHECK(prefix1.createSource(prefix1.context, &query, &second) == MMS_OK && Describe(api, second).flags == boneMask);
    CHECK(old.release(old.context, first) == MMS_OK && mid.release(mid.context, second) == MMS_OK);
    for (uint32_t mask : {displayMask, allMask}) {
        expectedMask = mask; const auto source = Create(api, mask); const auto full = Describe(api, source);
        struct OldGuard { MmsEvaluationSourceInfoV1 info; std::array<uint8_t, 32> tail; } oldGuard;
        struct MidGuard { MmsEvaluationSourceInfoV1_1 info; std::array<uint8_t, 32> tail; } midGuard;
        std::memset(&oldGuard, 0xa5, sizeof(oldGuard)); std::memset(&midGuard, 0xa5, sizeof(midGuard));
        CHECK(sizeof(oldGuard.info) == 64 && sizeof(midGuard.info) == 72);
        CHECK(prefix0.describeSource(prefix0.context, source, &oldGuard.info) == MMS_OK && HeaderOk(oldGuard.info));
        CHECK(prefix1.describeSourceChannels(prefix1.context, source, &midGuard.info) == MMS_OK && HeaderOk(midGuard.info, 1));
        for (auto byte : oldGuard.tail) CHECK(byte == 0xa5); for (auto byte : midGuard.tail) CHECK(byte == 0xa5);
        CHECK(oldGuard.info.captureId == full.captureId && midGuard.info.captureId == full.captureId && oldGuard.info.flags == mask &&
            midGuard.info.flags == mask && midGuard.info.retainedBytes == full.retainedBytes && midGuard.info.reserved == 0);
        for (uint32_t index = 0; index < 3; ++index) {
            MmsEvaluationIkV1 ik{}; MmsEvaluationSelectorV1 selector{};
            CHECK(api.ikAt(api.context, source, index, &ik) == MMS_OK && HeaderOk(ik));
            CHECK(Same(ik.bone, index < 2 ? boneId : MmsId{111, 21}));
            CHECK(api.selectorAt(api.context, source, index, &selector) == MMS_OK && HeaderOk(selector) && selector.reserved == 0);
            CHECK(index ? (selector.flags == 0 && Same(selector.bone, boneId)) : (selector.flags == MMS_EVALUATION_SELECTOR_GLOBAL && Empty(selector.bone)));
            MmsIkStateV1 enabled{};
            CHECK(api.sampleIk(api.context, source, index, {1, 3}, &enabled) == MMS_OK && HeaderOk(enabled) && !enabled.reserved);
            CHECK(enabled.enabled == (index == 0 ? 255u : index == 1 ? 197u : 0u));
        }
        for (MmsTime time : std::array<MmsTime, 5>{{{0, 1}, {1, 4}, {1, 3}, {2, 3}, {2147483646, 30}}}) {
            MmsDisplayStateV1 visibility{}; MmsParentSelectionV1 parent{};
            const double frame = double(time.numerator) * 30 / double(time.denominator); const auto expected = Parent(frame);
            CHECK(api.sampleVisibility(api.context, source, time, &visibility) == MMS_OK && HeaderOk(visibility) && visibility.reserved == 0);
            CHECK(visibility.visible == (frame < 10 ? 197u : 255u));
            CHECK(api.sampleParent(api.context, source, 0, time, &parent) == MMS_OK && HeaderOk(parent));
            CHECK(Same(parent.target, expected.target) && Same(parent.bone, expected.bone) && parent.flags == expected.flags &&
                parent.windowStart == expected.windowStart && parent.windowEnd == expected.windowEnd && parent.reserved == 0);
        }
        CHECK(api.release(api.context, source) == MMS_OK);
    }
    expectedMask = displayMask; const auto oldSource = Create(api, displayMask); generation = 77;
    const auto newSource = Create(api, displayMask); MmsParentSelectionV1 oldParent{}, newParent{};
    CHECK(api.sampleParent(api.context, oldSource, 1, {1, 3}, &oldParent) == MMS_OK && api.sampleParent(api.context, newSource, 1, {1, 3}, &newParent) == MMS_OK);
    CHECK(oldParent.target.generation == 5 && newParent.target.generation == 77 && Same(oldParent.target.id, newParent.target.id));
    CHECK(Describe(api, oldSource).captureId != Describe(api, newSource).captureId);
    CHECK(api.release(api.context, oldSource) == MMS_OK && api.release(api.context, newSource) == MMS_OK && !store.HasLeases(client));
}

static void UnsupportedEmptyAndForeign() {
    bool empty = false;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
        auto data = Data(query, mask); if (empty) { data->ik.clear(); data->selectors.clear(); } out = data; return MMS_OK;
    }), allMask);
    EvaluationStore::Client a, b; const auto api = Bind(store, a), foreign = Bind(store, b);
    auto ik = Poison<MmsEvaluationIkV1>(); auto selector = Poison<MmsEvaluationSelectorV1>(); auto visibility = Poison<MmsDisplayStateV1>();
    auto enabled = Poison<MmsIkStateV1>(); auto parent = Poison<MmsParentSelectionV1>(); auto info = Poison<MmsEvaluationSourceInfoV1_2>();
    const auto checkRejected = [&](const MmsEvaluationApiV1_2& table, MmsHandle source, MmsResult result) {
        CHECK(table.ikAt(table.context, source, 0, &ik) == result && Unchanged(ik));
        CHECK(table.selectorAt(table.context, source, 0, &selector) == result && Unchanged(selector));
        CHECK(table.sampleVisibility(table.context, source, {0, 1}, &visibility) == result && Unchanged(visibility));
        CHECK(table.sampleIk(table.context, source, 0, {0, 1}, &enabled) == result && Unchanged(enabled));
        CHECK(table.sampleParent(table.context, source, 0, {0, 1}, &parent) == result && Unchanged(parent));
    };
    const auto noDisplay = Create(api, 3); checkRejected(api, noDisplay, MMS_E_UNSUPPORTED);
    CHECK(api.release(api.context, noDisplay) == MMS_OK);
    const auto source = Create(api, displayMask); checkRejected(foreign, source, MMS_E_NOT_FOUND);
    CHECK(foreign.describeSourceDisplay(foreign.context, source, &info) == MMS_E_NOT_FOUND && Unchanged(info));
    CHECK(api.describeSourceDisplay(api.context, source, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.ikAt(api.context, source, 0, nullptr) == MMS_E_ARGUMENT && api.selectorAt(api.context, source, 0, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.sampleVisibility(api.context, source, {0, 1}, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.sampleIk(api.context, source, 0, {0, 1}, nullptr) == MMS_E_ARGUMENT && api.sampleParent(api.context, source, 0, {0, 1}, nullptr) == MMS_E_ARGUMENT);
    for (uint32_t index : {3u, UINT32_MAX}) {
        CHECK(api.ikAt(api.context, source, index, &ik) == MMS_E_NOT_FOUND && Unchanged(ik));
        CHECK(api.selectorAt(api.context, source, index, &selector) == MMS_E_NOT_FOUND && Unchanged(selector));
        CHECK(api.sampleIk(api.context, source, index, {0, 1}, &enabled) == MMS_E_NOT_FOUND && Unchanged(enabled));
        CHECK(api.sampleParent(api.context, source, index, {0, 1}, &parent) == MMS_E_NOT_FOUND && Unchanged(parent));
    }
    for (MmsTime time : std::array<MmsTime, 4>{{{0, 0}, {-1, 1}, {INT64_MAX, 1}, {2147483646000000001ll, 30000000000ull}}}) {
        CHECK(api.sampleVisibility(api.context, source, time, &visibility) == MMS_E_ARGUMENT && Unchanged(visibility));
        CHECK(api.sampleIk(api.context, source, 0, time, &enabled) == MMS_E_ARGUMENT && Unchanged(enabled));
        CHECK(api.sampleParent(api.context, source, 0, time, &parent) == MMS_E_ARGUMENT && Unchanged(parent));
    }
    CHECK(api.release(api.context, source) == MMS_OK); checkRejected(api, source, MMS_E_NOT_FOUND);
    empty = true; const auto emptySource = Create(api, displayMask); const auto emptyInfo = Describe(api, emptySource);
    CHECK(emptyInfo.ikCount == 0 && emptyInfo.selectorCount == 0 && emptyInfo.flags == displayMask);
    CHECK(api.sampleVisibility(api.context, emptySource, {0, 1}, &visibility) == MMS_OK && visibility.visible == 197);
    CHECK(api.ikAt(api.context, emptySource, 0, &ik) == MMS_E_NOT_FOUND && Unchanged(ik));
    CHECK(api.selectorAt(api.context, emptySource, 0, &selector) == MMS_E_NOT_FOUND && Unchanged(selector));
    CHECK(api.sampleIk(api.context, emptySource, 0, {0, 1}, &enabled) == MMS_E_NOT_FOUND && Unchanged(enabled));
    CHECK(api.sampleParent(api.context, emptySource, 0, {0, 1}, &parent) == MMS_E_NOT_FOUND && Unchanged(parent));
    CHECK(api.release(api.context, emptySource) == MMS_OK);
}

static void MalformedCaptureIsAtomic() {
    unsigned mode = 0;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) -> MmsResult {
        auto data = Data(query, mask); const auto extra = [&] { return Data(query, displayMask); };
        switch (mode) {
        case 0: data->ik[0].bone = {}; break;
        case 1: --data->ik[0].h.size; break;
        case 2: data->ik[0].h.major = 2; break;
        case 3: data->ik[0].h.minor = 1; break;
        case 4: --data->selectors[0].h.size; break;
        case 5: data->selectors[0].h.major = 2; break;
        case 6: data->selectors[0].h.minor = 1; break;
        case 7: data->selectors[0].bone = boneId; break;
        case 8: data->selectors[1].bone = {}; break;
        case 9: data->selectors[1].flags = 2; break;
        case 10: data->selectors[1].reserved = 1; break;
        case 11: data->sampleVisibility = {}; break;
        case 12: data->sampleIk = {}; break;
        case 13: data->sampleParent = {}; break;
        case 14: data->channels ^= displayMask; break;
        case 15: data->object.generation += 1; break;
        case 16: out = data; return MMS_E_CANCELLED;
        case 17: return MMS_OK;
        case 18: data->ik = extra()->ik; break;
        case 19: data->selectors = extra()->selectors; break;
        case 20: data->sampleVisibility = extra()->sampleVisibility; break;
        case 21: data->sampleIk = extra()->sampleIk; break;
        case 22: data->sampleParent = extra()->sampleParent; break;
        case 23: data->bones[0].id = {}; break;
        case 24: data->morphs[0].id = {}; break;
        case 25: throw std::bad_alloc();
        case 26: throw std::runtime_error("combined capture failure");
        }
        out = data; return MMS_OK;
    }), allMask);
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto query = Query();
    for (mode = 0; mode <= 26; ++mode) {
        const uint32_t mask = mode >= 18 && mode <= 22 ? 3 : allMask; MmsHandle handle = sentinel;
        const auto expected = mode == 25 ? MMS_E_MEMORY : mode == 16 ? MMS_E_CANCELLED :
            ((mode >= 11 && mode <= 13) || mode == 17 || mode == 26) ? MMS_E_FAILED : MMS_E_ARGUMENT;
        CHECK(api.createSourceChannels(api.context, &query, mask, &handle) == expected && handle == sentinel);
        CHECK(!store.HasLeases(client) && client.bytes == 0 && client.active == 0 && client.handles == 0);
    }
    mode = 27; const auto source = Create(api); CHECK(api.release(api.context, source) == MMS_OK);
}

static void SampleValidationAndExactTime() {
    unsigned mode = 0; std::array<double, 3> received{-1, -1, -1}; std::array<unsigned, 3> calls{};
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
        auto data = Data(query, mask);
        data->sampleVisibility = [&](double frame, MmsDisplayStateV1& value) -> MmsResult {
            received[0] = frame; ++calls[0]; value = {Header<MmsDisplayStateV1>(), 197, 0};
            if (mode == 1) value.visible = 256; if (mode == 2) value.reserved = 1;
            if (mode == 3) throw std::bad_alloc(); if (mode == 4) return MMS_E_CANCELLED; return MMS_OK;
        };
        data->sampleIk = [&](uint32_t, double frame, MmsIkStateV1& value) -> MmsResult {
            received[1] = frame; ++calls[1]; value = {Header<MmsIkStateV1>(), 255, 0};
            if (mode == 1) value.enabled = 256; if (mode == 2) value.reserved = 1;
            if (mode == 3) throw std::bad_alloc(); if (mode == 4) return MMS_E_CANCELLED; return MMS_OK;
        };
        data->sampleParent = [&](uint32_t, double frame, MmsParentSelectionV1& value) -> MmsResult {
            received[2] = frame; ++calls[2]; value = Parent(frame);
            switch (mode) {
            case 1: value.target.id = {}; break;
            case 2: value.target.generation = 0; break;
            case 3: value.bone = {}; break;
            case 4: value.flags = 0; break;
            case 5: value.flags = 0; value.target = {}; break;
            case 6: value.flags = 0; value.target.id = {}; value.bone = {}; break;
            case 7: value.flags = 2; break;
            case 8: value.reserved = 1; break;
            case 9: value.windowStart = 2147483647u; break;
            case 10: value.windowEnd = 2147483647u; break;
            case 11: value.windowEnd = value.windowStart; break;
            case 12: value.windowStart = 11; value.windowEnd = 10; break;
            case 13: value.windowStart = 11; break;
            case 14: value.windowStart = 0; value.windowEnd = 10; break;
            case 15: throw std::bad_alloc();
            case 16: throw std::runtime_error("parent sample failure");
            case 17: return MMS_E_UNSUPPORTED;
            }
            return MMS_OK;
        };
        out = data; return MMS_OK;
    }), allMask);
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api, displayMask);
    const MmsTime third{9007199254740993ll, 27021597764222979ull};
    MmsDisplayStateV1 visibility{}; MmsIkStateV1 enabled{}; MmsParentSelectionV1 parent{};
    CHECK(api.sampleVisibility(api.context, source, third, &visibility) == MMS_OK && visibility.visible == 197);
    CHECK(api.sampleIk(api.context, source, 0, third, &enabled) == MMS_OK && enabled.enabled == 255);
    CHECK(api.sampleParent(api.context, source, 0, third, &parent) == MMS_OK && parent.windowStart == 10 && parent.windowEnd == 20);
    CHECK(received[0] == 10 && received[1] == 10 && received[2] == 10);
    const auto before = calls; const MmsTime beyond{2147483646000000001ll, 30000000000ull};
    auto rejectedVisible = Poison<MmsDisplayStateV1>(); auto rejectedIk = Poison<MmsIkStateV1>(); auto rejectedParent = Poison<MmsParentSelectionV1>();
    CHECK(api.sampleVisibility(api.context, source, beyond, &rejectedVisible) == MMS_E_ARGUMENT && Unchanged(rejectedVisible));
    CHECK(api.sampleIk(api.context, source, 0, beyond, &rejectedIk) == MMS_E_ARGUMENT && Unchanged(rejectedIk));
    CHECK(api.sampleParent(api.context, source, 0, beyond, &rejectedParent) == MMS_E_ARGUMENT && Unchanged(rejectedParent) && calls == before);
    for (mode = 1; mode <= 4; ++mode) {
        const auto expected = mode == 3 ? MMS_E_MEMORY : mode == 4 ? MMS_E_CANCELLED : MMS_E_FAILED;
        CHECK(api.sampleVisibility(api.context, source, third, &rejectedVisible) == expected && Unchanged(rejectedVisible));
        CHECK(api.sampleIk(api.context, source, 0, third, &rejectedIk) == expected && Unchanged(rejectedIk));
    }
    for (mode = 1; mode <= 17; ++mode) {
        const auto expected = mode == 15 ? MMS_E_MEMORY : mode == 17 ? MMS_E_UNSUPPORTED : MMS_E_FAILED;
        CHECK(api.sampleParent(api.context, source, 0, third, &rejectedParent) == expected && Unchanged(rejectedParent));
    }
    mode = 0; failAfter = 0;
    const auto v = api.sampleVisibility(api.context, source, third, &visibility);
    const auto i = api.sampleIk(api.context, source, 0, third, &enabled);
    const auto p = api.sampleParent(api.context, source, 0, third, &parent); failAfter = -1;
    CHECK(v == MMS_OK && i == MMS_OK && p == MMS_OK);
    CHECK(api.release(api.context, source) == MMS_OK && client.active == 0 && !store.HasLeases(client));
}

static void SharedQuotaAndFaultSweep() {
    uint64_t reported = 8192;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) { out = Data(query, mask, reported); return MMS_OK; }), allMask);
    EvaluationStore::Client a, b, c; const auto api = Bind(store, a), other = Bind(store, b), third = Bind(store, c);
    const auto first = Create(api); const auto initial = Describe(api, first);
    CHECK(initial.flags == allMask && initial.retainedBytes >= reported && api.release(api.context, first) == MMS_OK);
    const auto overhead = initial.retainedBytes - reported;
    auto query = Query(initial.retainedBytes - 1); MmsHandle failed = sentinel;
    CHECK(api.createSourceChannels(api.context, &query, allMask, &failed) == MMS_E_MEMORY && failed == sentinel && !store.HasLeases(a));
    const auto exact = Create(api, allMask, initial.retainedBytes); CHECK(Describe(api, exact).retainedBytes == initial.retainedBytes);
    CHECK(api.release(api.context, exact) == MMS_OK);
    reported = 64 * MiB - overhead; const auto one = Create(api), two = Create(api); query = Query();
    MmsHandle retained = 0; CHECK(api.retain(api.context, one, &retained) == MMS_OK && retained != one && a.bytes == 128 * MiB);
    CHECK(Describe(api, retained).captureId == Describe(api, one).captureId && api.release(api.context, one) == MMS_OK && a.bytes == 128 * MiB);
    CHECK(api.createSourceChannels(api.context, &query, allMask, &failed) == MMS_E_MEMORY && failed == sentinel);
    const auto three = Create(other), four = Create(other);
    CHECK(third.createSourceChannels(third.context, &query, allMask, &failed) == MMS_E_MEMORY && failed == sentinel);
    CHECK(api.release(api.context, retained) == MMS_OK); const auto replacement = Create(third);
    CHECK(api.release(api.context, two) == MMS_OK && other.release(other.context, three) == MMS_OK && other.release(other.context, four) == MMS_OK &&
        third.release(third.context, replacement) == MMS_OK && a.bytes == 0 && b.bytes == 0 && c.bytes == 0);
    reported = 8192; bool completed = false; unsigned failures = 0;
    for (int limit = 0; limit < 512; ++limit) {
        MmsHandle handle = sentinel; failAfter = limit;
        const auto result = api.createSourceChannels(api.context, &query, allMask, &handle); failAfter = -1;
        if (result == MMS_OK) {
            const auto info = Describe(api, handle); CHECK(info.flags == allMask && info.ikCount == 3 && info.selectorCount == 3 && info.boneCount == 1 && info.morphCount == 1);
            CHECK(api.release(api.context, handle) == MMS_OK); completed = true; break;
        }
        CHECK(result == MMS_E_MEMORY && handle == sentinel && a.bytes == 0 && a.active == 0 && a.handles == 0 && !store.HasLeases(a)); ++failures;
    }
    CHECK(completed && failures > 0);
}

static void RetireWithAllSamplersInFlight() {
    struct Gate {
        std::mutex mutex; std::condition_variable cv; unsigned entered = 0; bool proceed = false;
        void Pause() { std::unique_lock<std::mutex> lock(mutex); ++entered; cv.notify_all(); cv.wait(lock, [&] { return proceed; }); }
        bool Wait() { std::unique_lock<std::mutex> lock(mutex); return cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered == 5; }); }
        void Open() { std::lock_guard<std::mutex> lock(mutex); proceed = true; cv.notify_all(); }
    };
    for (unsigned round = 0; round < 16; ++round) {
        Gate gate;
        EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
            auto data = Data(query, mask);
            const auto bone = data->sample;
            const auto morph = data->sampleMorph;
            const auto visible = data->sampleVisibility; const auto ik = data->sampleIk; const auto parent = data->sampleParent;
            data->sample = [&, bone](uint32_t i, double f, auto& v) { gate.Pause(); return bone(i, f, v); };
            data->sampleMorph = [&, morph](uint32_t i, double f, auto& v) { gate.Pause(); return morph(i, f, v); };
            data->sampleVisibility = [&, visible](double f, auto& v) { gate.Pause(); return visible(f, v); };
            data->sampleIk = [&, ik](uint32_t i, double f, auto& v) { gate.Pause(); return ik(i, f, v); };
            data->sampleParent = [&, parent](uint32_t i, double f, auto& v) { gate.Pause(); return parent(i, f, v); };
            out = data; return MMS_OK;
        }), allMask);
        EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
        std::array<MmsResult, 5> results{MMS_E_FAILED, MMS_E_FAILED, MMS_E_FAILED, MMS_E_FAILED, MMS_E_FAILED};
        MmsBoneChannelsV1 bone{}; MmsMorphWeightV1 morph{}; MmsDisplayStateV1 visible{}; MmsIkStateV1 ik{}; MmsParentSelectionV1 parent{};
        std::array<std::thread, 5> workers;
        try {
            workers[0] = std::thread([&] { results[0] = api.sampleBone(api.context, source, boneId, {1, 3}, &bone); });
            workers[1] = std::thread([&] { results[1] = api.sampleMorph(api.context, source, morphId, {1, 3}, &morph); });
            workers[2] = std::thread([&] { results[2] = api.sampleVisibility(api.context, source, {1, 3}, &visible); });
            workers[3] = std::thread([&] { results[3] = api.sampleIk(api.context, source, 0, {1, 3}, &ik); });
            workers[4] = std::thread([&] { results[4] = api.sampleParent(api.context, source, 0, {1, 3}, &parent); });
        } catch (...) { gate.Open(); for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
        CHECK(gate.Wait()); store.Retire(client);
        CHECK(api.release(api.context, source) == MMS_OK && store.HasLeases(client) && client.bytes != 0);
        auto out = Poison<MmsEvaluationSourceInfoV1_2>(); auto v = Poison<MmsDisplayStateV1>(); auto i = Poison<MmsIkStateV1>(); auto p = Poison<MmsParentSelectionV1>();
        auto ikDesc = Poison<MmsEvaluationIkV1>(); auto selector = Poison<MmsEvaluationSelectorV1>();
        CHECK(api.describeSourceDisplay(api.context, source, &out) == MMS_E_NOT_READY && Unchanged(out));
        CHECK(api.ikAt(api.context, source, 0, &ikDesc) == MMS_E_NOT_READY && Unchanged(ikDesc));
        CHECK(api.selectorAt(api.context, source, 0, &selector) == MMS_E_NOT_READY && Unchanged(selector));
        CHECK(api.sampleVisibility(api.context, source, {0, 1}, &v) == MMS_E_NOT_READY && Unchanged(v));
        CHECK(api.sampleIk(api.context, source, 0, {0, 1}, &i) == MMS_E_NOT_READY && Unchanged(i));
        CHECK(api.sampleParent(api.context, source, 0, {0, 1}, &p) == MMS_E_NOT_READY && Unchanged(p));
        gate.Open(); for (auto& worker : workers) worker.join();
        for (const auto result : results) CHECK(result == MMS_OK);
        CHECK(bone.position[0] == 10 && morph.value == -5 && visible.visible == 255 && ik.enabled == 255 && parent.target.generation == 5);
        CHECK(client.bytes == 0 && client.active == 0 && client.handles == 0 && !store.HasLeases(client));
    }
}

static void ConcurrentDisplayReads() {
    EvaluationStore store(EvaluationStore::CaptureChannels([](const auto& query, uint32_t mask, uint64_t, auto& out) { out = Data(query, mask); return MMS_OK; }), allMask);
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
    const auto original = Describe(api, source); std::atomic<unsigned> failures{0}; std::array<std::thread, 4> workers;
    try {
        for (auto& worker : workers) worker = std::thread([&] {
            for (unsigned repeat = 0; repeat < 250; ++repeat) {
                MmsHandle retained = 0; MmsEvaluationSourceInfoV1_2 info{}; MmsDisplayStateV1 visible{}; MmsIkStateV1 ik{}; MmsParentSelectionV1 parent{};
                if (api.retain(api.context, source, &retained) != MMS_OK) { ++failures; continue; }
                if (api.describeSourceDisplay(api.context, retained, &info) != MMS_OK || info.captureId != original.captureId ||
                    api.sampleVisibility(api.context, retained, {1, 3}, &visible) != MMS_OK || visible.visible != 255 ||
                    api.sampleIk(api.context, retained, 1, {1, 3}, &ik) != MMS_OK || ik.enabled != 197 ||
                    api.sampleParent(api.context, retained, 2, {1, 3}, &parent) != MMS_OK || parent.target.generation != 5 || !Same(parent.bone, targetBone)) ++failures;
                if (api.release(api.context, retained) != MMS_OK) ++failures;
            }
        });
    } catch (...) { for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
        const auto again = store.Api12(client); CHECK(HeaderOk(again, 2) && again.context == api.context && client.owner == &store);
    }
    for (auto& worker : workers) worker.join();
    CHECK(failures == 0 && api.release(api.context, source) == MMS_OK && client.bytes == 0 && client.active == 0 && !store.HasLeases(client));
}

int main() {
    CapabilityNegotiationAndMasks(); PrefixesAndDisplayRecords(); UnsupportedEmptyAndForeign(); MalformedCaptureIsAtomic();
    SampleValidationAndExactTime(); SharedQuotaAndFaultSweep(); RetireWithAllSamplersInFlight(); ConcurrentDisplayReads();
    std::printf("PASS %u evaluation display checks\n", checks); return 0;
}
