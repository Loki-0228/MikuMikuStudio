// Evaluation 1.1: selected bone/morph channels share one immutable source.
#include "evaluation_store.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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
static constexpr uint32_t bones = MMS_EVALUATION_CAP_BONE_CHANNELS;
static constexpr uint32_t morphs = MMS_EVALUATION_CAP_MORPH_WEIGHTS;
static constexpr uint32_t both = bones | morphs;
static constexpr uint64_t MiB = 1024ull * 1024;
static constexpr MmsHandle sentinel = 0x123456789abcdef0ull;
static constexpr MmsObjectRef object{{100, 21}, 7};
static constexpr MmsId boneId{110, 21};
static MmsId MorphId(uint32_t index) { return {200 + index, 21}; }
template<class T> static T Poison() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> static bool Unchanged(const T& value) { const auto before = Poison<T>(); return !std::memcmp(&value, &before, sizeof(value)); }
template<class T> static bool HeaderOk(const T& value, unsigned minor = 0) { return value.h.size == sizeof(T) && value.h.major == 1 && value.h.minor == minor; }
static MmsEvaluationSourceQueryV1 Query(uint64_t budget = 0) { return {Header<MmsEvaluationSourceQueryV1>(), object, budget, 0, 0}; }
static MmsEvaluationApiV1_1 Bind(EvaluationStore& store, EvaluationStore::Client& client) { client.owner = &store; return store.Api11(client); }
static MmsBoneChannelsV1 BoneValue(float origin, uint32_t index, double frame) {
    MmsBoneChannelsV1 value{}; value.h = Header<MmsBoneChannelsV1>();
    value.position[0] = origin; value.position[1] = float(frame); value.position[2] = float(index);
    value.rotation[3] = 1; value.scale[0] = value.scale[4] = value.scale[8] = 1;
    return value;
}
static MmsMorphWeightV1 MorphValue(float origin, uint32_t index, double frame) {
    return {Header<MmsMorphWeightV1>(), float(double(origin) + index + frame - 10), 0};
}
static std::shared_ptr<EvaluationData> Data(const MmsEvaluationSourceQueryV1& query, uint32_t channels,
    uint64_t bytes = 8192, float origin = 2) {
    auto data = std::make_shared<EvaluationData>();
    data->object = query.object; data->hostRevision = 39; data->retainedBytes = bytes; data->channels = channels;
    if (channels & bones) {
        data->bones = {{Header<MmsEvaluationBoneV1>(), boneId, MMS_EVALUATION_BONE_LOCAL, 0}};
        data->sample = [origin](uint32_t index, double frame, auto& out) { out = BoneValue(origin, index, frame); return MMS_OK; };
    }
    if (channels & morphs) {
        for (uint32_t i = 0; i < 9; ++i)
            data->morphs.push_back({Header<MmsEvaluationMorphV1>(), MorphId(i), i,
                ((i % 5) << MMS_MORPH_PANEL_SHIFT) | ((i % 2) ? 0u : MMS_MORPH_BASE)});
        data->sampleMorph = [origin](uint32_t index, double frame, auto& out) { out = MorphValue(origin, index, frame); return MMS_OK; };
    }
    return data;
}
static MmsHandle Create(const MmsEvaluationApiV1_1& api, uint32_t channels = both, uint64_t budget = 0) {
    const auto query = Query(budget); MmsHandle handle = sentinel;
    CHECK(api.createSourceChannels(api.context, &query, channels, &handle) == MMS_OK && handle && handle != sentinel);
    return handle;
}
static MmsEvaluationSourceInfoV1_1 Describe(const MmsEvaluationApiV1_1& api, MmsHandle source) {
    auto info = Poison<MmsEvaluationSourceInfoV1_1>();
    CHECK(api.describeSourceChannels(api.context, source, &info) == MMS_OK && HeaderOk(info, 1));
    CHECK(!info.reserved); return info;
}

static void ChannelSelectionAndLegacyPrefix() {
    uint32_t expectedMask = bones; unsigned captures = 0; float liveOrigin = 2;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t channels, uint64_t limit, auto& out) {
        CHECK(channels == expectedMask && limit && limit <= 64 * MiB); ++captures;
        out = Data(query, channels, 8192, liveOrigin); return MMS_OK;
    }));
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto legacy = store.Api(client);
    CHECK(HeaderOk(api, 1) && HeaderOk(legacy) && api.context == legacy.context);
    MmsEvaluationCapsV1 caps{}; CHECK(api.getCapabilities(api.context, &caps) == MMS_OK && HeaderOk(caps) && caps.flags == both);
    CHECK(legacy.getCapabilities(legacy.context, &caps) == MMS_OK && caps.flags == both);
    MmsEvaluationApiV1 prefix{}; std::memcpy(&prefix, &api, sizeof(prefix));
    const auto query = Query(); MmsHandle oldSource = 0, prefixSource = 0;
    CHECK(legacy.createSource(legacy.context, &query, &oldSource) == MMS_OK);
    CHECK(prefix.createSource(prefix.context, &query, &prefixSource) == MMS_OK);
    CHECK(Describe(api, oldSource).flags == bones && Describe(api, prefixSource).morphCount == 0 && captures == 2);
    CHECK(prefix.release(prefix.context, prefixSource) == MMS_OK && api.release(api.context, oldSource) == MMS_OK);
    for (uint32_t mask : {bones, morphs, both}) {
        expectedMask = mask; const auto source = Create(api, mask); const auto info = Describe(api, source);
        CHECK(Same(info.object, object) && info.flags == mask && info.boneCount == ((mask & bones) ? 1u : 0u));
        CHECK(info.morphCount == ((mask & morphs) ? 9u : 0u) && info.hostRevision == 39 && info.captureId && info.retainedBytes >= 8192);
        // The old entry writes exactly the old 64-byte structure, even on a
        // mixed source. Its existing fields keep their original offsets.
        struct Guard { MmsEvaluationSourceInfoV1 info; std::array<unsigned char, 24> tail; } guarded;
        std::memset(&guarded, 0xa5, sizeof(guarded));
        CHECK(sizeof(guarded.info) == 64 && sizeof(info) == 72);
        CHECK(prefix.describeSource(prefix.context, source, &guarded.info) == MMS_OK && HeaderOk(guarded.info));
        for (auto byte : guarded.tail) CHECK(byte == 0xa5);
        CHECK(guarded.info.captureId == info.captureId && guarded.info.boneCount == info.boneCount &&
            guarded.info.flags == mask && guarded.info.retainedBytes == info.retainedBytes);
        auto bone = Poison<MmsEvaluationBoneV1>(); auto pose = Poison<MmsBoneChannelsV1>();
        if (mask & bones) {
            CHECK(api.boneAt(api.context, source, 0, &bone) == MMS_OK && Same(bone.id, boneId));
            CHECK(api.sampleBone(api.context, source, bone.id, {1, 4}, &pose) == MMS_OK && pose.position[1] == 7.5f);
        } else {
            CHECK(api.boneAt(api.context, source, 0, &bone) == MMS_E_UNSUPPORTED && Unchanged(bone));
            CHECK(api.sampleBone(api.context, source, boneId, {0, 1}, &pose) == MMS_E_UNSUPPORTED && Unchanged(pose));
        }
        for (uint32_t i = 0; i < ((mask & morphs) ? 9u : 1u); ++i) {
            auto morph = Poison<MmsEvaluationMorphV1>(); auto weight = Poison<MmsMorphWeightV1>();
            if (mask & morphs) {
                CHECK(api.morphAt(api.context, source, i, &morph) == MMS_OK && HeaderOk(morph) && sizeof(morph) == 32);
                CHECK(Same(morph.id, MorphId(i)) && morph.kind == i && ((morph.flags & MMS_MORPH_PANEL_MASK) >> MMS_MORPH_PANEL_SHIFT) == i % 5);
                CHECK(bool(morph.flags & MMS_MORPH_BASE) == (i % 2 == 0));
                CHECK(api.sampleMorph(api.context, source, morph.id, {0, 1}, &weight) == MMS_OK && HeaderOk(weight) && sizeof(weight) == 16);
                CHECK(weight.value == float(int(i) - 8) && weight.reserved == 0);
                CHECK(api.sampleMorph(api.context, source, morph.id, {1, 2}, &weight) == MMS_OK && weight.value == float(i + 7));
            } else {
                CHECK(api.morphAt(api.context, source, i, &morph) == MMS_E_UNSUPPORTED && Unchanged(morph));
                CHECK(api.sampleMorph(api.context, source, MorphId(i), {0, 1}, &weight) == MMS_E_UNSUPPORTED && Unchanged(weight));
            }
        }
        CHECK(api.release(api.context, source) == MMS_OK);
    }
    expectedMask = both; const auto old = Create(api); liveOrigin = 50; const auto updated = Create(api);
    MmsBoneChannelsV1 oldPose{}, newPose{}; MmsMorphWeightV1 oldWeight{}, newWeight{};
    CHECK(api.sampleBone(api.context, old, boneId, {0, 1}, &oldPose) == MMS_OK && api.sampleBone(api.context, updated, boneId, {0, 1}, &newPose) == MMS_OK);
    CHECK(api.sampleMorph(api.context, old, MorphId(0), {0, 1}, &oldWeight) == MMS_OK && api.sampleMorph(api.context, updated, MorphId(0), {0, 1}, &newWeight) == MMS_OK);
    CHECK(oldPose.position[0] == 2 && newPose.position[0] == 50 && oldWeight.value == -8 && newWeight.value == 40);
    CHECK(Describe(api, old).captureId != Describe(api, updated).captureId);
    CHECK(api.release(api.context, old) == MMS_OK && api.release(api.context, updated) == MMS_OK && !store.HasLeases(client));
}

static void LegacyCaptureAndEmptyChannels() {
    unsigned captures = 0;
    EvaluationStore legacyStore(EvaluationStore::Capture([&](const auto& query, uint64_t, auto& out) {
        ++captures; out = Data(query, bones); return MMS_OK;
    }));
    EvaluationStore::Client client; const auto api = Bind(legacyStore, client); MmsEvaluationCapsV1 caps{};
    CHECK(api.getCapabilities(api.context, &caps) == MMS_OK && caps.flags == bones);
    const auto query = Query(); MmsHandle output = sentinel;
    for (uint32_t mask : {morphs, both})
        CHECK(api.createSourceChannels(api.context, &query, mask, &output) == MMS_E_UNSUPPORTED && output == sentinel && captures == 0);
    const auto source = Create(api, bones); CHECK(captures == 1 && api.release(api.context, source) == MMS_OK);
    EvaluationStore emptyStore(EvaluationStore::CaptureChannels([](const auto& query, uint32_t channels, uint64_t, auto& out) {
        auto data = Data(query, channels); data->bones.clear(); data->morphs.clear(); out = data; return MMS_OK;
    }));
    EvaluationStore::Client emptyClient; const auto emptyApi = Bind(emptyStore, emptyClient); const auto empty = Create(emptyApi);
    const auto info = Describe(emptyApi, empty); CHECK(info.flags == both && info.boneCount == 0 && info.morphCount == 0);
    auto morph = Poison<MmsEvaluationMorphV1>(); auto weight = Poison<MmsMorphWeightV1>();
    auto bone = Poison<MmsEvaluationBoneV1>(); auto pose = Poison<MmsBoneChannelsV1>();
    CHECK(emptyApi.morphAt(emptyApi.context, empty, 0, &morph) == MMS_E_NOT_FOUND && Unchanged(morph));
    CHECK(emptyApi.sampleMorph(emptyApi.context, empty, MorphId(0), {0, 1}, &weight) == MMS_E_NOT_FOUND && Unchanged(weight));
    CHECK(emptyApi.boneAt(emptyApi.context, empty, 0, &bone) == MMS_E_NOT_FOUND && Unchanged(bone));
    CHECK(emptyApi.sampleBone(emptyApi.context, empty, boneId, {0, 1}, &pose) == MMS_E_NOT_FOUND && Unchanged(pose));
    CHECK(emptyApi.release(emptyApi.context, empty) == MMS_OK);
}

static void InvalidInputsAndOwnership() {
    unsigned captures = 0;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) { ++captures; out = Data(query, mask); return MMS_OK; }));
    EvaluationStore::Client a, b; const auto api = Bind(store, a), other = Bind(store, b); const auto valid = Query();
    auto rejected = [&](const MmsEvaluationSourceQueryV1* query, uint32_t mask) {
        MmsHandle output = sentinel;
        CHECK(api.createSourceChannels(api.context, query, mask, &output) == MMS_E_ARGUMENT && output == sentinel);
    };
    for (uint32_t mask : {0u, 4u, 7u, UINT32_MAX}) rejected(&valid, mask);
    rejected(nullptr, both); CHECK(api.createSourceChannels(api.context, &valid, both, nullptr) == MMS_E_ARGUMENT);
    for (unsigned field = 0; field < 8; ++field) {
        auto query = valid;
        if (field == 0) --query.h.size;
        if (field == 1) query.h.major = 2;
        if (field == 2) query.h.minor = 1;
        if (field == 3) query.flags = 1;
        if (field == 4) query.reserved = 1;
        if (field == 5) query.object.id = {};
        if (field == 6) query.object.generation = 0;
        if (field == 7) query.maxRetainedBytes = 64 * MiB + 1;
        rejected(&query, both);
    }
    CHECK(captures == 0 && !store.HasLeases(a));
    MmsResult workerResult = MMS_OK; MmsHandle workerOutput = sentinel;
    std::thread worker([&] { workerResult = api.createSourceChannels(api.context, &valid, both, &workerOutput); }); worker.join();
    CHECK(workerResult == MMS_E_THREAD && workerOutput == sentinel && captures == 0);
    const auto source = Create(api);
    CHECK(api.describeSourceChannels(api.context, source, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.morphAt(api.context, source, 0, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.sampleMorph(api.context, source, MorphId(0), {0, 1}, nullptr) == MMS_E_ARGUMENT);
    auto info = Poison<MmsEvaluationSourceInfoV1_1>(); auto morph = Poison<MmsEvaluationMorphV1>(); auto weight = Poison<MmsMorphWeightV1>();
    CHECK(other.describeSourceChannels(other.context, source, &info) == MMS_E_NOT_FOUND && Unchanged(info));
    CHECK(other.morphAt(other.context, source, 0, &morph) == MMS_E_NOT_FOUND && Unchanged(morph));
    CHECK(other.sampleMorph(other.context, source, MorphId(0), {0, 1}, &weight) == MMS_E_NOT_FOUND && Unchanged(weight));
    MmsHandle retained = sentinel;
    CHECK(other.retain(other.context, source, &retained) == MMS_E_NOT_FOUND && retained == sentinel);
    CHECK(other.release(other.context, source) == MMS_E_NOT_FOUND);
    CHECK(api.morphAt(api.context, source, 9, &morph) == MMS_E_NOT_FOUND && Unchanged(morph));
    CHECK(api.morphAt(api.context, source, UINT32_MAX, &morph) == MMS_E_NOT_FOUND && Unchanged(morph));
    for (MmsId bad : {MmsId{}, boneId, MmsId{999, 21}})
        CHECK(api.sampleMorph(api.context, source, bad, {0, 1}, &weight) == MMS_E_NOT_FOUND && Unchanged(weight));
    auto pose = Poison<MmsBoneChannelsV1>();
    CHECK(api.sampleBone(api.context, source, MorphId(0), {0, 1}, &pose) == MMS_E_NOT_FOUND && Unchanged(pose));
    for (const MmsTime time : std::array<MmsTime, 4>{{{0, 0}, {-1, 1}, {INT64_MAX, 1}, {2147483646000000001ll, 30000000000ull}}})
        CHECK(api.sampleMorph(api.context, source, MorphId(0), time, &weight) == MMS_E_ARGUMENT && Unchanged(weight));
    CHECK(api.release(api.context, source) == MMS_OK);
    CHECK(api.describeSourceChannels(api.context, source, &info) == MMS_E_NOT_FOUND && Unchanged(info));
    CHECK(api.morphAt(api.context, source, 0, &morph) == MMS_E_NOT_FOUND && Unchanged(morph));
    CHECK(api.sampleMorph(api.context, source, MorphId(0), {0, 1}, &weight) == MMS_E_NOT_FOUND && Unchanged(weight));
}

static void MalformedCapturedData() {
    unsigned mode = 0;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
        auto data = Data(query, mask);
        switch (mode) {
        case 0: data->channels = 0; break;
        case 1: data->channels = bones; break;
        case 2: data->channels = both | 4; break;
        case 3: data->morphs[0].id = {}; break;
        case 4: data->morphs[1].id = data->morphs[0].id; break;
        case 5: data->morphs[0].id = data->bones[0].id; break;
        case 6: --data->morphs[0].h.size; break;
        case 7: data->morphs[0].h.major = 2; break;
        case 8: data->morphs[0].h.minor = 1; break;
        case 9: data->morphs[0].kind = 9; break;
        case 10: data->morphs[0].flags = 2; break;
        case 11: data->morphs[0].flags = 5 << MMS_MORPH_PANEL_SHIFT; break;
        case 12: data->morphs[0].flags = 0x80000000u; break;
        case 13: data->object.generation += 1; break;
        case 14: data->sampleMorph = {}; break;
        case 15: data->sample = {}; break;
        case 16: return MMS_E_CANCELLED;
        case 17: data->morphs = Data(query, morphs)->morphs; break; // Unrequested vector.
        case 18: data->sampleMorph = Data(query, morphs)->sampleMorph; break;
        case 19: data->bones = Data(query, bones)->bones; break;
        case 20: data->sample = Data(query, bones)->sample; break;
        }
        out = data; return MMS_OK;
    }));
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto query = Query();
    for (mode = 0; mode <= 20; ++mode) {
        MmsHandle source = sentinel;
        const uint32_t mask = mode == 17 || mode == 18 ? bones : mode == 19 || mode == 20 ? morphs : both;
        const MmsResult expected = mode == 14 || mode == 15 ? MMS_E_FAILED : mode == 16 ? MMS_E_CANCELLED : MMS_E_ARGUMENT;
        CHECK(api.createSourceChannels(api.context, &query, mask, &source) == expected && source == sentinel);
        CHECK(!store.HasLeases(client) && client.bytes == 0 && client.active == 0 && client.handles == 0);
    }
    mode = 21; const auto source = Create(api); CHECK(api.release(api.context, source) == MMS_OK);
}

static void ExactTimeAndBadSamples() {
    double received = -1; unsigned called = 0, mode = 0;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
        auto data = Data(query, mask);
        data->sampleMorph = [&](uint32_t index, double frame, MmsMorphWeightV1& value) -> MmsResult {
            received = frame; ++called; value = MorphValue(2, index, frame);
            if (mode == 1) value.value = std::numeric_limits<float>::quiet_NaN();
            if (mode == 2) value.value = std::numeric_limits<float>::infinity();
            if (mode == 3) value.value = -std::numeric_limits<float>::infinity();
            if (mode == 4) value.reserved = 1;
            if (mode == 5) throw std::bad_alloc();
            if (mode == 6) throw std::runtime_error("morph sampling failure");
            if (mode == 7) return MMS_E_UNSUPPORTED;
            return MMS_OK;
        };
        out = data; return MMS_OK;
    }));
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
    MmsMorphWeightV1 weight{};
    const MmsTime third{9007199254740993ll, 27021597764222979ull};
    CHECK(api.sampleMorph(api.context, source, MorphId(0), third, &weight) == MMS_OK && received == 10 && weight.value == 2);
    auto failed = Poison<MmsMorphWeightV1>(); const auto before = called;
    CHECK(api.sampleMorph(api.context, source, MorphId(0), {2147483646000000001ll, 30000000000ull}, &failed) == MMS_E_ARGUMENT);
    CHECK(Unchanged(failed) && called == before);
    for (mode = 1; mode <= 7; ++mode) {
        failed = Poison<MmsMorphWeightV1>();
        const auto expected = mode == 5 ? MMS_E_MEMORY : mode == 7 ? MMS_E_UNSUPPORTED : MMS_E_FAILED;
        CHECK(api.sampleMorph(api.context, source, MorphId(0), {0, 1}, &failed) == expected && Unchanged(failed));
    }
    mode = 0; auto desc = Poison<MmsEvaluationMorphV1>(); failAfter = 0;
    const auto described = api.morphAt(api.context, source, 0, &desc);
    const auto sampled = api.sampleMorph(api.context, source, MorphId(0), {1, 4}, &weight);
    failAfter = -1;
    CHECK(described == MMS_OK && sampled == MMS_OK && weight.value == -.5f && HeaderOk(weight));
    CHECK(api.release(api.context, source) == MMS_OK && client.active == 0 && !store.HasLeases(client));
}

static void SharedBudgetAndAllocationFailures() {
    uint64_t reported = 8192;
    EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) { out = Data(query, mask, reported); return MMS_OK; }));
    EvaluationStore::Client a, b, c; const auto api = Bind(store, a), other = Bind(store, b), third = Bind(store, c);
    const auto initial = Create(api); const auto info = Describe(api, initial);
    CHECK(info.flags == both && info.boneCount == 1 && info.morphCount == 9 && info.retainedBytes >= reported);
    const auto overhead = info.retainedBytes - reported; CHECK(api.release(api.context, initial) == MMS_OK);
    auto query = Query(info.retainedBytes - 1); MmsHandle refused = sentinel;
    CHECK(api.createSourceChannels(api.context, &query, both, &refused) == MMS_E_MEMORY && refused == sentinel && !store.HasLeases(a));
    const auto exact = Create(api, both, info.retainedBytes); CHECK(Describe(api, exact).retainedBytes == info.retainedBytes);
    CHECK(api.release(api.context, exact) == MMS_OK);
    reported = 64 * MiB - overhead; const auto one = Create(api), two = Create(api);
    CHECK(Describe(api, one).retainedBytes == 64 * MiB && a.bytes == 128 * MiB);
    MmsHandle retained = 0; CHECK(api.retain(api.context, one, &retained) == MMS_OK && retained != one);
    CHECK(Describe(api, retained).captureId == Describe(api, one).captureId && a.bytes == 128 * MiB);
    CHECK(api.release(api.context, one) == MMS_OK && a.bytes == 128 * MiB);
    query = Query();
    CHECK(api.createSourceChannels(api.context, &query, both, &refused) == MMS_E_MEMORY && refused == sentinel);
    const auto three = Create(other), four = Create(other);
    CHECK(third.createSourceChannels(third.context, &query, both, &refused) == MMS_E_MEMORY && refused == sentinel);
    CHECK(api.release(api.context, retained) == MMS_OK && a.bytes == 64 * MiB);
    const auto replacement = Create(third);
    CHECK(api.release(api.context, two) == MMS_OK && other.release(other.context, three) == MMS_OK &&
        other.release(other.context, four) == MMS_OK && third.release(third.context, replacement) == MMS_OK);
    CHECK(a.bytes == 0 && b.bytes == 0 && c.bytes == 0);
    reported = 8192;
    bool success = false; unsigned failures = 0;
    for (int limit = 0; limit < 512; ++limit) {
        MmsHandle source = sentinel; failAfter = limit;
        const auto result = api.createSourceChannels(api.context, &query, both, &source); failAfter = -1;
        if (result == MMS_OK) {
            const auto completed = Describe(api, source); CHECK(completed.flags == both && completed.boneCount == 1 && completed.morphCount == 9);
            CHECK(api.release(api.context, source) == MMS_OK); success = true; break;
        }
        CHECK(result == MMS_E_MEMORY && source == sentinel && a.bytes == 0 && a.active == 0 && a.handles == 0 && !store.HasLeases(a)); ++failures;
    }
    CHECK(success && failures > 0);
}

static void RetireWithBothChannelsInFlight() {
    struct Gate {
        std::mutex mutex; std::condition_variable cv; unsigned entered = 0; bool proceed = false;
        void Pause() { std::unique_lock<std::mutex> lock(mutex); ++entered; cv.notify_all(); cv.wait(lock, [&] { return proceed; }); }
        bool Wait() { std::unique_lock<std::mutex> lock(mutex); return cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered == 2; }); }
        void Open() { std::lock_guard<std::mutex> lock(mutex); proceed = true; cv.notify_all(); }
    };
    for (unsigned round = 0; round < 32; ++round) {
        Gate gate;
        EvaluationStore store(EvaluationStore::CaptureChannels([&](const auto& query, uint32_t mask, uint64_t, auto& out) {
            auto data = Data(query, mask);
            data->sample = [&](uint32_t index, double frame, auto& value) { gate.Pause(); value = BoneValue(2, index, frame); return MMS_OK; };
            data->sampleMorph = [&](uint32_t index, double frame, auto& value) { gate.Pause(); value = MorphValue(2, index, frame); return MMS_OK; };
            out = data; return MMS_OK;
        }));
        EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
        MmsBoneChannelsV1 pose{}; MmsMorphWeightV1 weight{}; MmsResult boneResult = MMS_E_FAILED, morphResult = MMS_E_FAILED;
        std::thread boneWorker([&] { boneResult = api.sampleBone(api.context, source, boneId, {1, 4}, &pose); });
        std::thread morphWorker([&] { morphResult = api.sampleMorph(api.context, source, MorphId(0), {1, 4}, &weight); });
        CHECK(gate.Wait()); store.Retire(client);
        CHECK(api.release(api.context, source) == MMS_OK && store.HasLeases(client) && client.bytes != 0);
        auto desc = Poison<MmsEvaluationMorphV1>(); auto refused = Poison<MmsMorphWeightV1>(); auto info = Poison<MmsEvaluationSourceInfoV1_1>();
        MmsHandle created = sentinel; const auto query = Query();
        CHECK(api.createSourceChannels(api.context, &query, both, &created) == MMS_E_NOT_READY && created == sentinel);
        CHECK(api.describeSourceChannels(api.context, source, &info) == MMS_E_NOT_READY && Unchanged(info));
        CHECK(api.morphAt(api.context, source, 0, &desc) == MMS_E_NOT_READY && Unchanged(desc));
        CHECK(api.sampleMorph(api.context, source, MorphId(0), {0, 1}, &refused) == MMS_E_NOT_READY && Unchanged(refused));
        gate.Open(); boneWorker.join(); morphWorker.join();
        CHECK(boneResult == MMS_OK && morphResult == MMS_OK && pose.position[1] == 7.5f && weight.value == -.5f);
        CHECK(client.bytes == 0 && client.active == 0 && client.handles == 0 && !store.HasLeases(client));
    }
}

static void ParallelMixedReads() {
    EvaluationStore store(EvaluationStore::CaptureChannels([](const auto& query, uint32_t mask, uint64_t, auto& out) { out = Data(query, mask); return MMS_OK; }));
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
    const auto before = Describe(api, source); std::atomic<unsigned> failures{0}; std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned repeat = 0; repeat < 300; ++repeat) {
            MmsHandle retained = 0; MmsEvaluationSourceInfoV1_1 info{}; MmsBoneChannelsV1 pose{}; MmsMorphWeightV1 weight{};
            if (api.retain(api.context, source, &retained) != MMS_OK) { ++failures; continue; }
            if (api.describeSourceChannels(api.context, retained, &info) != MMS_OK || info.captureId != before.captureId || info.flags != both ||
                api.sampleBone(api.context, retained, boneId, {1, 4}, &pose) != MMS_OK || pose.position[1] != 7.5f ||
                api.sampleMorph(api.context, retained, MorphId(8), {1, 4}, &weight) != MMS_OK || weight.value != 7.5f) ++failures;
            if (api.release(api.context, retained) != MMS_OK) ++failures;
        }
    });
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
        const auto again = store.Api11(client); CHECK(HeaderOk(again, 1) && again.context == api.context && client.owner == &store);
    }
    for (auto& worker : workers) worker.join();
    CHECK(failures == 0 && api.release(api.context, source) == MMS_OK && client.bytes == 0 && client.active == 0 && !store.HasLeases(client));
}

int main() {
    ChannelSelectionAndLegacyPrefix(); LegacyCaptureAndEmptyChannels(); InvalidInputsAndOwnership();
    MalformedCapturedData(); ExactTimeAndBadSamples(); SharedBudgetAndAllocationFailures();
    RetireWithBothChannelsInFlight(); ParallelMixedReads();
    std::printf("PASS %u evaluation channel checks\n", checks); return 0;
}
