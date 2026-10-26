// Frozen evaluation ABI: ownership, quotas, immutable reads and admitted calls.
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
static constexpr uint64_t MiB = 1024ull * 1024;
static constexpr MmsHandle sentinel = 0x123456789abcdef0ull;
static constexpr MmsObjectRef object{{501, 77}, 9};
static constexpr MmsId firstBone{701, 77}, secondBone{702, 77};

template<class T> static T Poison() { T value; std::memset(&value, 0xa5, sizeof(value)); return value; }
template<class T> static bool Unchanged(const T& value) { const auto before = Poison<T>(); return !std::memcmp(&value, &before, sizeof(value)); }
template<class T> static bool HeaderOk(const T& value) { return value.h.size == sizeof(T) && value.h.major == 1 && value.h.minor == 0; }
static MmsEvaluationSourceQueryV1 Query(uint64_t budget = 0) { return {Header<MmsEvaluationSourceQueryV1>(), object, budget, 0, 0}; }
static MmsEvaluationApiV1 Bind(EvaluationStore& store, EvaluationStore::Client& client) { client.owner = &store; return store.Api(client); }
static MmsHandle Create(const MmsEvaluationApiV1& api, uint64_t budget = 0) {
    const auto query = Query(budget); MmsHandle handle = sentinel;
    CHECK(api.createSource(api.context, &query, &handle) == MMS_OK && handle && handle != sentinel);
    return handle;
}
static MmsEvaluationSourceInfoV1 Describe(const MmsEvaluationApiV1& api, MmsHandle handle) {
    auto info = Poison<MmsEvaluationSourceInfoV1>();
    CHECK(api.describeSource(api.context, handle, &info) == MMS_OK && HeaderOk(info));
    return info;
}
static MmsBoneChannelsV1 Channels(float origin, uint32_t bone, double frame) {
    MmsBoneChannelsV1 value{}; value.h = Header<MmsBoneChannelsV1>();
    value.position[0] = origin; value.position[1] = float(frame); value.position[2] = float(bone);
    value.rotation[3] = 1; value.scale[0] = value.scale[4] = value.scale[8] = 1;
    value.flags = frame >= 15 ? MMS_EVALUATION_PHYSICS_DISABLED : 0;
    return value;
}
static std::shared_ptr<const EvaluationData> Data(const MmsEvaluationSourceQueryV1& query,
    float origin = 3, uint64_t bytes = 256, uint64_t revision = 22) {
    auto data = std::make_shared<EvaluationData>();
    data->object = query.object; data->hostRevision = revision; data->retainedBytes = bytes;
    data->bones = {{Header<MmsEvaluationBoneV1>(), firstBone, MMS_EVALUATION_BONE_LOCAL, 0},
        {Header<MmsEvaluationBoneV1>(), secondBone, MMS_EVALUATION_BONE_LOCAL, 0}};
    data->sample = [origin](uint32_t index, double frame, MmsBoneChannelsV1& output) {
        output = Channels(origin, index, frame); return MMS_OK;
    };
    return data;
}

static void BasicAndIsolation() {
    float liveValue = 3; uint64_t revision = 22; const MmsTime playhead{37, 30};
    unsigned captures = 0;
    EvaluationStore store([&](const auto& query, uint64_t budget, auto& output) {
        ++captures; CHECK(budget > 0 && budget <= 64 * MiB);
        output = Data(query, liveValue, 256, revision); return MMS_OK;
    });
    EvaluationStore::Client a, b; const auto api = Bind(store, a), other = Bind(store, b);
    CHECK(HeaderOk(api) && api.context == &a);
    auto caps = Poison<MmsEvaluationCapsV1>();
    CHECK(api.getCapabilities(api.context, &caps) == MMS_OK && HeaderOk(caps));
    CHECK(caps.flags == MMS_EVALUATION_CAP_BONE_CHANNELS && caps.maxSources == 128);
    CHECK(caps.maxSourceBytes == 64 * MiB && caps.maxClientBytes == 128 * MiB && caps.maxTotalBytes == 256 * MiB);
    const auto source = Create(api); const auto info = Describe(api, source);
    CHECK(Same(info.object, object) && info.hostRevision == revision && info.captureId && info.boneCount == 2);
    CHECK(info.flags == MMS_EVALUATION_CAP_BONE_CHANNELS && info.retainedBytes >= 256 && info.retainedBytes <= caps.maxSourceBytes);
    for (uint32_t index = 0; index < 2; ++index) {
        auto bone = Poison<MmsEvaluationBoneV1>();
        CHECK(api.boneAt(api.context, source, index, &bone) == MMS_OK && HeaderOk(bone));
        CHECK(Same(bone.id, index ? secondBone : firstBone) && bone.flags == MMS_EVALUATION_BONE_LOCAL && !bone.reserved);
        auto channels = Poison<MmsBoneChannelsV1>(); const auto expected = Channels(3, index, 7.5);
        const MmsTime time{1, 4};
        CHECK(api.sampleBone(api.context, source, bone.id, time, &channels) == MMS_OK && HeaderOk(channels));
        CHECK(!std::memcmp(&channels, &expected, sizeof(channels)) && time.numerator == 1 && time.denominator == 4);
    }
    auto foreignInfo = Poison<MmsEvaluationSourceInfoV1>(); auto foreignBone = Poison<MmsEvaluationBoneV1>();
    auto foreignSample = Poison<MmsBoneChannelsV1>(); MmsHandle foreignHandle = sentinel;
    CHECK(other.describeSource(other.context, source, &foreignInfo) == MMS_E_NOT_FOUND && Unchanged(foreignInfo));
    CHECK(other.boneAt(other.context, source, 0, &foreignBone) == MMS_E_NOT_FOUND && Unchanged(foreignBone));
    CHECK(other.sampleBone(other.context, source, firstBone, {0, 1}, &foreignSample) == MMS_E_NOT_FOUND && Unchanged(foreignSample));
    CHECK(other.retain(other.context, source, &foreignHandle) == MMS_E_NOT_FOUND && foreignHandle == sentinel);
    CHECK(other.release(other.context, source) == MMS_E_NOT_FOUND);
    MmsHandle retained = 0; CHECK(api.retain(api.context, source, &retained) == MMS_OK && retained != source);
    const auto retainedInfo = Describe(api, retained);
    CHECK(retainedInfo.captureId == info.captureId && retainedInfo.retainedBytes == info.retainedBytes && captures == 1);
    liveValue = 90; ++revision;
    const auto newer = Create(api); const auto newerInfo = Describe(api, newer);
    CHECK(newerInfo.captureId != info.captureId && newerInfo.hostRevision == revision);
    const auto sameRevision = Create(api);
    CHECK(Describe(api, sameRevision).captureId != newerInfo.captureId);
    MmsBoneChannelsV1 oldSample{}, newSample{};
    CHECK(api.sampleBone(api.context, retained, firstBone, {1, 2}, &oldSample) == MMS_OK);
    CHECK(api.sampleBone(api.context, newer, firstBone, {1, 2}, &newSample) == MMS_OK);
    CHECK(oldSample.position[0] == 3 && newSample.position[0] == 90 && oldSample.position[1] == 15);
    CHECK(oldSample.flags == MMS_EVALUATION_PHYSICS_DISABLED && playhead.numerator == 37 && playhead.denominator == 30);
    CHECK(api.release(api.context, source) == MMS_OK && api.release(api.context, source) == MMS_E_NOT_FOUND);
    CHECK(api.sampleBone(api.context, retained, firstBone, {0, 1}, &oldSample) == MMS_OK);
    CHECK(api.release(api.context, retained) == MMS_OK && api.release(api.context, newer) == MMS_OK && api.release(api.context, sameRevision) == MMS_OK);
    CHECK(!store.HasLeases(a) && !store.HasLeases(b));
}

static void InvalidInputs() {
    unsigned calls = 0;
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) { ++calls; output = Data(query); return MMS_OK; });
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto valid = Query();
    auto rejected = [&](const MmsEvaluationSourceQueryV1* query, MmsResult expected) {
        MmsHandle output = sentinel; CHECK(api.createSource(api.context, query, &output) == expected && output == sentinel);
    };
    CHECK(api.getCapabilities(api.context, nullptr) == MMS_E_ARGUMENT);
    rejected(nullptr, MMS_E_ARGUMENT);
    CHECK(api.createSource(api.context, &valid, nullptr) == MMS_E_ARGUMENT);
    auto query = valid; query.h.size = sizeof(query) - 1; rejected(&query, MMS_E_ARGUMENT);
    query = valid; query.h.major = 2; rejected(&query, MMS_E_ARGUMENT);
    query = valid; query.h.minor = 1; rejected(&query, MMS_E_ARGUMENT);
    query = valid; query.flags = 1; rejected(&query, MMS_E_ARGUMENT);
    query = valid; query.reserved = 1; rejected(&query, MMS_E_ARGUMENT);
    query = valid; query.object.id = {}; rejected(&query, MMS_E_ARGUMENT);
    query = valid; query.object.generation = 0; rejected(&query, MMS_E_ARGUMENT);
    CHECK(calls == 0 && !store.HasLeases(client));
    const auto source = Create(api);
    CHECK(api.describeSource(api.context, source, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.boneAt(api.context, source, 0, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.sampleBone(api.context, source, firstBone, {0, 1}, nullptr) == MMS_E_ARGUMENT);
    CHECK(api.retain(api.context, source, nullptr) == MMS_E_ARGUMENT);
    for (MmsHandle bad : {MmsHandle(0), sentinel}) {
        auto info = Poison<MmsEvaluationSourceInfoV1>(); auto bone = Poison<MmsEvaluationBoneV1>();
        auto sample = Poison<MmsBoneChannelsV1>(); MmsHandle retained = sentinel;
        CHECK(api.describeSource(api.context, bad, &info) == MMS_E_NOT_FOUND && Unchanged(info));
        CHECK(api.boneAt(api.context, bad, 0, &bone) == MMS_E_NOT_FOUND && Unchanged(bone));
        CHECK(api.sampleBone(api.context, bad, firstBone, {0, 1}, &sample) == MMS_E_NOT_FOUND && Unchanged(sample));
        CHECK(api.retain(api.context, bad, &retained) == MMS_E_NOT_FOUND && retained == sentinel);
        CHECK(api.release(api.context, bad) == MMS_E_NOT_FOUND);
    }
    for (uint32_t index : {2u, UINT32_MAX}) {
        auto bone = Poison<MmsEvaluationBoneV1>();
        CHECK(api.boneAt(api.context, source, index, &bone) == MMS_E_NOT_FOUND && Unchanged(bone));
    }
    auto sample = Poison<MmsBoneChannelsV1>();
    CHECK(api.sampleBone(api.context, source, {999, 77}, {0, 1}, &sample) == MMS_E_NOT_FOUND && Unchanged(sample));
    for (const MmsTime time : std::array<MmsTime, 4>{{{0, 0}, {-1, 1}, {INT64_MAX, 1}, {2147483647, 30}}}) {
        CHECK(api.sampleBone(api.context, source, firstBone, time, &sample) == MMS_E_ARGUMENT && Unchanged(sample));
    }
    MmsResult workerResult = MMS_OK; MmsHandle workerOutput = sentinel;
    std::thread worker([&] { workerResult = api.createSource(api.context, &valid, &workerOutput); }); worker.join();
    CHECK(workerResult == MMS_E_THREAD && workerOutput == sentinel && calls == 1);
    CHECK(api.release(api.context, source) == MMS_OK);
}

static void UnsupportedBones() {
    for (uint32_t flags : {MMS_EVALUATION_BONE_EXTERNAL_PARENT, MMS_EVALUATION_BONE_DERIVED}) {
        unsigned sampled = 0;
        EvaluationStore store([&](const auto& query, uint64_t, auto& output) {
            auto data = std::make_shared<EvaluationData>(*Data(query)); data->bones[0].flags = flags;
            data->sample = [&](uint32_t, double, MmsBoneChannelsV1&) { ++sampled; return MMS_OK; };
            output = data; return MMS_OK;
        });
        EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
        auto sample = Poison<MmsBoneChannelsV1>(); MmsEvaluationBoneV1 bone{};
        CHECK(api.boneAt(api.context, source, 0, &bone) == MMS_OK && bone.flags == flags);
        CHECK(api.sampleBone(api.context, source, firstBone, {0, 1}, &sample) == MMS_E_UNSUPPORTED && Unchanged(sample) && sampled == 0);
        CHECK(api.release(api.context, source) == MMS_OK);
    }
}

static void InvalidCapturedData() {
    unsigned mode = 0;
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) {
        auto data = std::make_shared<EvaluationData>(*Data(query));
        switch (mode) {
        case 0: data->object.id.lo += 1; break;
        case 1: data->object.generation += 1; break;
        case 2: data->bones[0].id = {}; break;
        case 3: data->bones[1].id = data->bones[0].id; break;
        case 4: data->bones[0].h.size -= 1; break;
        case 5: data->bones[0].h.major = 2; break;
        case 6: data->bones[0].h.minor = 1; break;
        case 7: data->bones[0].reserved = 1; break;
        case 8: data->bones[0].flags = 0x80000000u; break;
        case 9: data->sample = {}; break;
        }
        output = data; return MMS_OK;
    });
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto query = Query();
    for (mode = 0; mode < 10; ++mode) {
        MmsHandle handle = sentinel;
        CHECK(api.createSource(api.context, &query, &handle) == (mode == 9 ? MMS_E_FAILED : MMS_E_ARGUMENT));
        CHECK(handle == sentinel && !store.HasLeases(client));
    }
    mode = 10; const auto valid = Create(api); CHECK(api.release(api.context, valid) == MMS_OK);
}

static void Quotas() {
    // Calibrate store overhead above the verifiable data-capacity floor. That
    // floor can grow when optional channels are added to EvaluationData.
    const auto fixture = Data(Query());
    uint64_t reportedBytes = sizeof(EvaluationData) +
        fixture->bones.capacity() * sizeof(MmsEvaluationBoneV1) +
        fixture->morphs.capacity() * sizeof(MmsEvaluationMorphV1);
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) { output = Data(query, 3, reportedBytes); return MMS_OK; });
    std::array<EvaluationStore::Client, 9> clients;
    std::array<MmsEvaluationApiV1, 9> apis;
    for (unsigned i = 0; i < apis.size(); ++i) apis[i] = Bind(store, clients[i]);
    const auto calibration = Create(apis[0]); const auto calibrationInfo = Describe(apis[0], calibration);
    CHECK(calibrationInfo.retainedBytes >= reportedBytes);
    const uint64_t overhead = calibrationInfo.retainedBytes - reportedBytes;
    CHECK(apis[0].release(apis[0].context, calibration) == MMS_OK);
    auto query = Query(calibrationInfo.retainedBytes - 1); MmsHandle failed = sentinel;
    CHECK(apis[0].createSource(apis[0].context, &query, &failed) == MMS_E_MEMORY && failed == sentinel);
    const auto exact = Create(apis[0], calibrationInfo.retainedBytes);
    CHECK(Describe(apis[0], exact).retainedBytes == calibrationInfo.retainedBytes);
    CHECK(apis[0].release(apis[0].context, exact) == MMS_OK);
    CHECK(overhead < 64 * MiB); reportedBytes = 64 * MiB - overhead;
    const auto one = Create(apis[0]), two = Create(apis[0]);
    CHECK(Describe(apis[0], one).retainedBytes == 64 * MiB);
    query = Query(); failed = sentinel;
    CHECK(apis[0].createSource(apis[0].context, &query, &failed) == MMS_E_MEMORY && failed == sentinel);
    MmsHandle retained = 0; CHECK(apis[0].retain(apis[0].context, one, &retained) == MMS_OK);
    CHECK(apis[0].release(apis[0].context, one) == MMS_OK);
    CHECK(apis[0].createSource(apis[0].context, &query, &failed) == MMS_E_MEMORY && failed == sentinel);
    const auto three = Create(apis[1]), four = Create(apis[1]);
    CHECK(apis[2].createSource(apis[2].context, &query, &failed) == MMS_E_MEMORY && failed == sentinel);
    CHECK(apis[0].release(apis[0].context, retained) == MMS_OK);
    const auto replacement = Create(apis[2]);
    CHECK(apis[0].release(apis[0].context, two) == MMS_OK);
    CHECK(apis[1].release(apis[1].context, three) == MMS_OK && apis[1].release(apis[1].context, four) == MMS_OK);
    CHECK(apis[2].release(apis[2].context, replacement) == MMS_OK);
    ++reportedBytes;
    CHECK(apis[0].createSource(apis[0].context, &query, &failed) == MMS_E_MEMORY && failed == sentinel);
    reportedBytes = 256;
    std::array<std::array<MmsHandle, 128>, 8> handles{};
    for (unsigned c = 0; c < handles.size(); ++c) {
        handles[c][0] = Create(apis[c]);
        for (unsigned i = 1; i < handles[c].size(); ++i)
            CHECK(apis[c].retain(apis[c].context, handles[c][0], &handles[c][i]) == MMS_OK && handles[c][i] != handles[c][0]);
        failed = sentinel;
        CHECK(apis[c].retain(apis[c].context, handles[c][0], &failed) == MMS_E_BUSY && failed == sentinel);
    }
    failed = sentinel;
    CHECK(apis[8].createSource(apis[8].context, &query, &failed) == MMS_E_BUSY && failed == sentinel);
    CHECK(apis[0].release(apis[0].context, handles[0][127]) == MMS_OK); handles[0][127] = 0;
    const auto globalReplacement = Create(apis[8]); CHECK(apis[8].release(apis[8].context, globalReplacement) == MMS_OK);
    for (unsigned c = 0; c < handles.size(); ++c) {
        for (auto h : handles[c]) if (h) CHECK(apis[c].release(apis[c].context, h) == MMS_OK);
        CHECK(!store.HasLeases(clients[c]));
    }
}

struct Gate {
    std::mutex mutex; std::condition_variable cv; bool entered = false, proceed = false;
    void Pause() { std::unique_lock<std::mutex> lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return proceed; }); }
    bool Wait() { std::unique_lock<std::mutex> lock(mutex); return cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }); }
    void Open() { std::lock_guard<std::mutex> lock(mutex); proceed = true; cv.notify_all(); }
};
static void AdmittedReadAndRetirement() {
    Gate gate;
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) {
        auto data = std::make_shared<EvaluationData>(*Data(query));
        data->sample = [&](uint32_t index, double frame, auto& value) { gate.Pause(); value = Channels(3, index, frame); return MMS_OK; };
        output = data; return MMS_OK;
    });
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
    MmsResult result = MMS_E_FAILED; auto sample = Poison<MmsBoneChannelsV1>();
    std::thread worker([&] { result = api.sampleBone(api.context, source, firstBone, {1, 4}, &sample); });
    CHECK(gate.Wait()); store.Retire(client); CHECK(store.HasLeases(client));
    CHECK(api.release(api.context, source) == MMS_OK && store.HasLeases(client));
    auto caps = Poison<MmsEvaluationCapsV1>(); auto info = Poison<MmsEvaluationSourceInfoV1>(); auto bone = Poison<MmsEvaluationBoneV1>();
    auto refused = Poison<MmsBoneChannelsV1>(); MmsHandle handle = sentinel; const auto query = Query();
    CHECK(api.getCapabilities(api.context, &caps) == MMS_E_NOT_READY && Unchanged(caps));
    CHECK(api.createSource(api.context, &query, &handle) == MMS_E_NOT_READY && handle == sentinel);
    CHECK(api.describeSource(api.context, source, &info) == MMS_E_NOT_READY && Unchanged(info));
    CHECK(api.boneAt(api.context, source, 0, &bone) == MMS_E_NOT_READY && Unchanged(bone));
    CHECK(api.sampleBone(api.context, source, firstBone, {0, 1}, &refused) == MMS_E_NOT_READY && Unchanged(refused));
    CHECK(api.retain(api.context, source, &handle) == MMS_E_NOT_READY && handle == sentinel);
    gate.Open(); worker.join();
    CHECK(result == MMS_OK && sample.position[1] == 7.5f && !store.HasLeases(client));
    store.Retire(client); CHECK(!store.HasLeases(client));
}

static void CaptureReentryAndRetirement() {
    EvaluationStore::Client client; MmsEvaluationApiV1 api{}; MmsResult nested = MMS_OK;
    MmsHandle nestedOutput = sentinel; bool retireDuringCapture = false, pinned = false;
    EvaluationStore* owner = nullptr;
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) {
        pinned = owner->HasLeases(client);
        nested = api.createSource(api.context, &query, &nestedOutput);
        if (retireDuringCapture) { owner->Retire(client); pinned = pinned && owner->HasLeases(client); }
        output = Data(query); return MMS_OK;
    });
    owner = &store; api = Bind(store, client);
    const auto source = Create(api);
    CHECK(nested == MMS_E_BUSY && nestedOutput == sentinel && pinned);
    CHECK(api.release(api.context, source) == MMS_OK);
    retireDuringCapture = true; const auto query = Query(); MmsHandle refused = sentinel;
    CHECK(api.createSource(api.context, &query, &refused) == MMS_E_NOT_READY && refused == sentinel);
    CHECK(pinned && !store.HasLeases(client));
}

static void ParallelReadsAndQueries() {
    EvaluationStore store([](const auto& query, uint64_t, auto& output) { output = Data(query); return MMS_OK; });
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
    const auto initial = Describe(api, source); std::atomic<unsigned> failures{0};
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        for (unsigned i = 0; i < 250; ++i) {
            MmsHandle retained = 0; MmsEvaluationSourceInfoV1 info{}; MmsBoneChannelsV1 sample{};
            if (api.retain(api.context, source, &retained) != MMS_OK) { ++failures; continue; }
            if (api.describeSource(api.context, retained, &info) != MMS_OK || info.captureId != initial.captureId ||
                api.sampleBone(api.context, retained, secondBone, {1, 4}, &sample) != MMS_OK ||
                sample.position[1] != 7.5f || sample.position[2] != 1) ++failures;
            if (api.release(api.context, retained) != MMS_OK) ++failures;
        }
    });
    for (unsigned i = 0; i < 1000; ++i) {
        // Do not call Bind here: owner is immutable after publication.
        const auto queried = store.Api(client);
        CHECK(queried.context == api.context && client.owner == &store && queried.sampleBone == api.sampleBone);
    }
    for (auto& worker : workers) worker.join();
    CHECK(failures == 0 && api.release(api.context, source) == MMS_OK && !store.HasLeases(client));
}

static void NoAllocationReadsAndRetainedRetirement() {
    EvaluationStore store([](const auto& query, uint64_t, auto& output) { output = Data(query); return MMS_OK; });
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
    MmsEvaluationCapsV1 caps{}; MmsEvaluationSourceInfoV1 info{}; MmsEvaluationBoneV1 bone{}; MmsBoneChannelsV1 sample{};
    failAfter = 0;
    const auto capResult = api.getCapabilities(api.context, &caps);
    const auto infoResult = api.describeSource(api.context, source, &info);
    const auto boneResult = api.boneAt(api.context, source, 0, &bone);
    const auto sampleResult = api.sampleBone(api.context, source, firstBone, {2147483646, 30}, &sample);
    failAfter = -1;
    CHECK(capResult == MMS_OK && infoResult == MMS_OK && boneResult == MMS_OK && sampleResult == MMS_OK);
    CHECK(sample.position[1] == float(2147483646.0));
    MmsHandle retained = 0; CHECK(api.retain(api.context, source, &retained) == MMS_OK);
    store.Retire(client); CHECK(store.HasLeases(client));
    CHECK(api.release(api.context, source) == MMS_OK && store.HasLeases(client));
    CHECK(api.release(api.context, retained) == MMS_OK && !store.HasLeases(client));
}

static void ExactRationalTime() {
    double receivedFrame = -1; unsigned sampled = 0;
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) {
        auto data = std::make_shared<EvaluationData>(*Data(query));
        data->sample = [&](uint32_t index, double frame, auto& value) {
            receivedFrame = frame; ++sampled; value = Channels(3, index, frame); return MMS_OK;
        };
        output = data; return MMS_OK;
    });
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto source = Create(api);
    auto sample = Poison<MmsBoneChannelsV1>();
    // Normalize the integer fraction before conversion: casting the numerator
    // first loses its low bit and can send an exact key into interpolation.
    const MmsTime third{9007199254740993ll, 27021597764222979ull};
    CHECK(api.sampleBone(api.context, source, firstBone, third, &sample) == MMS_OK);
    CHECK(sampled == 1 && receivedFrame == 10.0 && sample.position[1] == 10.f);
    CHECK(third.numerator == 9007199254740993ll && third.denominator == 27021597764222979ull);
    sample = Poison<MmsBoneChannelsV1>();
    // The exact frame is 2147483646 + 1e-9, even though double rounding can
    // make it equal the last allowed frame. Reject before floating conversion.
    const MmsTime beyondLast{2147483646000000001ll, 30000000000ull};
    CHECK(api.sampleBone(api.context, source, firstBone, beyondLast, &sample) == MMS_E_ARGUMENT);
    CHECK(Unchanged(sample) && sampled == 1 && receivedFrame == 10.0);
    CHECK(api.release(api.context, source) == MMS_OK && !store.HasLeases(client));
}

static void ConcurrentLastReferenceSettlement() {
    struct PairGate {
        std::mutex mutex; std::condition_variable cv; unsigned entered = 0; bool proceed = false;
        void Reset() { std::lock_guard<std::mutex> lock(mutex); entered = 0; proceed = false; }
        void Pause() {
            std::unique_lock<std::mutex> lock(mutex); ++entered; cv.notify_all();
            cv.wait(lock, [&] { return proceed; });
        }
        bool WaitForBoth() {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered == 2; });
        }
        void Open() { std::lock_guard<std::mutex> lock(mutex); proceed = true; cv.notify_all(); }
    } gate;
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) {
        auto data = std::make_shared<EvaluationData>(*Data(query));
        data->sample = [&](uint32_t index, double frame, auto& value) {
            gate.Pause(); value = Channels(3, index, frame); return MMS_OK;
        };
        output = data; return MMS_OK;
    });
    EvaluationStore::Client client; const auto api = Bind(store, client);
    for (unsigned repetition = 0; repetition < 128; ++repetition) {
        gate.Reset(); const auto source = Create(api);
        std::array<MmsResult, 2> results{MMS_E_FAILED, MMS_E_FAILED};
        std::array<MmsBoneChannelsV1, 2> output{};
        std::array<std::thread, 2> workers;
        for (unsigned i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
            results[i] = api.sampleBone(api.context, source, i ? secondBone : firstBone, {1, 4}, &output[i]);
        });
        CHECK(gate.WaitForBoth());
        CHECK(api.release(api.context, source) == MMS_OK && store.HasLeases(client));
        // Both active samples now own the last references. Releasing their
        // pins concurrently must charge back the source exactly once.
        gate.Open(); for (auto& worker : workers) worker.join();
        for (unsigned i = 0; i < results.size(); ++i) {
            CHECK(results[i] == MMS_OK && output[i].position[1] == 7.5f && output[i].position[2] == float(i));
        }
        CHECK(client.bytes == 0 && client.active == 0 && client.handles == 0 && !store.HasLeases(client));
    }
}

static void ExceptionsAndAllocationAtomicity() {
    unsigned mode = 0;
    EvaluationStore store([&](const auto& query, uint64_t, auto& output) -> MmsResult {
        if (mode == 1) throw std::bad_alloc();
        if (mode == 2) throw std::runtime_error("capture failure");
        if (mode == 3) return MMS_E_BUSY;
        if (mode == 4) return MMS_OK; // Missing result is never a successful capture.
        auto data = std::make_shared<EvaluationData>(*Data(query));
        if (mode == 5) data->sample = [](uint32_t, double, auto& out) -> MmsResult { out = Channels(99, 0, 0); throw std::bad_alloc(); };
        if (mode == 6) data->sample = [](uint32_t, double, auto& out) -> MmsResult { out = Channels(99, 0, 0); throw std::runtime_error("sample failure"); };
        if (mode == 7) data->sample = [](uint32_t, double, auto& out) -> MmsResult { out = Channels(99, 0, 0); return MMS_E_UNSUPPORTED; };
        output = data; return MMS_OK;
    });
    EvaluationStore::Client client; const auto api = Bind(store, client); const auto query = Query();
    for (mode = 1; mode <= 4; ++mode) {
        const MmsResult expected[] = {MMS_OK, MMS_E_MEMORY, MMS_E_FAILED, MMS_E_BUSY, MMS_E_FAILED};
        MmsHandle handle = sentinel;
        CHECK(api.createSource(api.context, &query, &handle) == expected[mode] && handle == sentinel && !store.HasLeases(client));
    }
    for (mode = 5; mode <= 7; ++mode) {
        const auto source = Create(api); auto out = Poison<MmsBoneChannelsV1>();
        const auto expected = mode == 5 ? MMS_E_MEMORY : mode == 6 ? MMS_E_FAILED : MMS_E_UNSUPPORTED;
        CHECK(api.sampleBone(api.context, source, firstBone, {0, 1}, &out) == expected && Unchanged(out));
        CHECK(api.release(api.context, source) == MMS_OK && !store.HasLeases(client));
    }
    mode = 0; bool success = false; unsigned failedAllocations = 0;
    for (int limit = 0; limit < 256; ++limit) {
        MmsHandle handle = sentinel; failAfter = limit;
        const auto result = api.createSource(api.context, &query, &handle); failAfter = -1;
        if (result == MMS_OK) { CHECK(api.release(api.context, handle) == MMS_OK); success = true; break; }
        CHECK(result == MMS_E_MEMORY && handle == sentinel && !store.HasLeases(client)); ++failedAllocations;
    }
    CHECK(success && failedAllocations > 0);
    const auto source = Create(api); const auto before = Describe(api, source); success = false;
    for (int limit = 0; limit < 32; ++limit) {
        MmsHandle retained = sentinel; failAfter = limit;
        const auto result = api.retain(api.context, source, &retained); failAfter = -1;
        if (result == MMS_OK) { CHECK(api.release(api.context, retained) == MMS_OK); success = true; break; }
        CHECK(result == MMS_E_MEMORY && retained == sentinel);
        const auto after = Describe(api, source); CHECK(after.captureId == before.captureId && after.retainedBytes == before.retainedBytes);
    }
    CHECK(success && api.release(api.context, source) == MMS_OK && !store.HasLeases(client));
}

int main() {
    BasicAndIsolation(); InvalidInputs(); UnsupportedBones(); InvalidCapturedData(); Quotas();
    AdmittedReadAndRetirement(); CaptureReentryAndRetirement(); ParallelReadsAndQueries();
    NoAllocationReadsAndRetainedRetirement(); ExactRationalTime(); ConcurrentLastReferenceSettlement();
    ExceptionsAndAllocationAtomicity();
    std::printf("PASS %u evaluation store checks\n", checks); return 0;
}
