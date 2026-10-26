#include "evaluation_store.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
constexpr uint64_t kMaxSourceBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxClientBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaxTotalBytes = 256ull * 1024 * 1024;
constexpr uint32_t kMaxHandlesPerClient = 128;
constexpr uint32_t kMaxTotalHandles = 1024;
constexpr uint32_t kMaxSources = 128;
// Handles start above the 62-bit mark so small integers from other services
// cannot alias a source. Reaching the 63-bit bound is exhaustion, not memory
// pressure, because the sequence itself cannot allocate another handle.
constexpr uint64_t kHandleLimit = 0x7fffffffffffffffull;

using Index = std::map<MmsId, uint32_t, IdLess>;
// The supported MSVC tree allocates a separate sentinel node and holds its link
// fields outside value_type. One extra node plus four pointer-sized slots is a
// conservative allowance for each actual entry.
constexpr uint64_t kIndexNodeBytes = sizeof(Index::value_type) + 4 * sizeof(void*);

// Cross-DLL calls may only receive MmsResult. Own and host exceptions stop here;
// length_error is a normal budget rejection in the host capture helper.
template<class F> MmsResult Gateway(F&& f) noexcept {
    try { return f(); }
    catch (const std::bad_alloc&) { return MMS_E_MEMORY; }
    catch (const std::length_error&) { return MMS_E_MEMORY; }
    catch (const std::invalid_argument&) { return MMS_E_ARGUMENT; }
    catch (...) { return MMS_E_FAILED; }
}

// Saturating arithmetic is not enough here: an unrepresentable retained size is
// a budget failure, not a large budget.
bool AddBytes(uint64_t& total, uint64_t count, uint64_t stride) noexcept {
    if (stride && count > (UINT64_MAX - total) / stride) return false;
    total += count * stride;
    return true;
}

// LOCAL excludes the external-parent and derived markers, and every non-local
// record must say why it cannot supply a local channel.
bool ValidBoneFlags(uint32_t flags) noexcept {
    constexpr uint32_t excluded = MMS_EVALUATION_BONE_EXTERNAL_PARENT | MMS_EVALUATION_BONE_DERIVED;
    constexpr uint32_t known = MMS_EVALUATION_BONE_LOCAL | excluded;
    if (flags & ~known) return false;
    if ((flags & MMS_EVALUATION_BONE_LOCAL) && (flags & excluded)) return false;
    return (flags & known) != 0;
}
// Compare nonnegative rational values without overflowing either cross product.
bool RatioGreater(uint64_t a, uint64_t b, uint64_t c, uint64_t d) noexcept {
    bool reverse = false;
    for (;;) {
        const auto aq = a / b, cq = c / d;
        if (aq != cq) return reverse ? aq < cq : aq > cq;
        const auto ar = a % b, cr = c % d;
        if (!ar || !cr) return reverse ? ar < cr : ar > cr;
        a = b; b = ar; c = d; d = cr; reverse = !reverse;
    }
}
}

// Store-owned half of a source. The host's EvaluationData is immutable and
// already includes its own snapshot and bone capacity in retainedBytes; this
// payload adds the ID index and the shared table node that resolve bone IDs.
struct EvaluationStore::Payload {
    std::shared_ptr<const EvaluationData> data;
    Index index;
    MmsObjectRef object{};
    uint64_t bytes = 0, captureId = 0, hostRevision = 0;
    uint32_t boneCount = 0, flags = 0;
};

EvaluationStore::EvaluationStore(Capture capture) : capture_(std::move(capture)) {}
EvaluationStore::~EvaluationStore() = default;

MmsEvaluationApiV1 EvaluationStore::Api(Client& client) {
    // The caller binds Client::owner before publication. Api only builds the
    // table and never mutates client state, so repeated queries remain safe
    // against workers that are already reading the owner.
    MmsEvaluationApiV1 api{Header<MmsEvaluationApiV1>(), &client};
    api.getCapabilities = [](void* context, MmsEvaluationCapsV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->Capabilities(client, out);
        });
    };
    api.createSource = [](void* context, const MmsEvaluationSourceQueryV1* query, MmsHandle* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->Create(client, query, out);
        });
    };
    api.describeSource = [](void* context, MmsHandle source, MmsEvaluationSourceInfoV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->Describe(client, source, out);
        });
    };
    api.boneAt = [](void* context, MmsHandle source, uint32_t index, MmsEvaluationBoneV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->BoneAt(client, source, index, out);
        });
    };
    api.sampleBone = [](void* context, MmsHandle source, MmsId bone, MmsTime time, MmsBoneChannelsV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->Sample(client, source, bone, time, out);
        });
    };
    api.retain = [](void* context, MmsHandle source, MmsHandle* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->Retain(client, source, out);
        });
    };
    api.release = [](void* context, MmsHandle source) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->Release(client, source);
        });
    };
    return api;
}

void EvaluationStore::Retire(Client& client) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        client.retired = true;
    } catch (...) {}
}

bool EvaluationStore::HasLeases(Client& client) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // A capture or sample in flight keeps the client alive after its source
    // handle is gone.
    return client.handles != 0 || client.active != 0;
}

MmsResult EvaluationStore::Capabilities(Client& client, MmsEvaluationCapsV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    MmsEvaluationCapsV1 caps{};
    caps.h = Header<MmsEvaluationCapsV1>();
    caps.flags = MMS_EVALUATION_CAP_BONE_CHANNELS;
    caps.maxSources = kMaxSources;
    caps.maxSourceBytes = kMaxSourceBytes;
    caps.maxClientBytes = kMaxClientBytes;
    caps.maxTotalBytes = kMaxTotalBytes;
    *out = caps;
    return MMS_OK;
}

MmsResult EvaluationStore::Create(Client& client, const MmsEvaluationSourceQueryV1* query, MmsHandle* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!out) return MMS_E_ARGUMENT;
    if (!Valid(query)) return MMS_E_ARGUMENT;
    if (query->flags || query->reserved) return MMS_E_ARGUMENT;
    if (Empty(query->object.id) || !query->object.generation) return MMS_E_ARGUMENT;
    if (query->maxRetainedBytes > kMaxSourceBytes) return MMS_E_ARGUMENT;
    if (!capture_) return MMS_E_NOT_READY;
    const uint64_t bound = query->maxRetainedBytes ? query->maxRetainedBytes : kMaxSourceBytes;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        if (acquiring_) return MMS_E_BUSY;
        if (client.handles >= kMaxHandlesPerClient || sources_.size() >= kMaxTotalHandles) return MMS_E_BUSY;
        if (next_ >= kHandleLimit) return MMS_E_BUSY;
        // Register the pin before the callback so the manager cannot unload or
        // destroy caller-owned state while the capture is running.
        acquiring_ = true;
        ++client.active;
    }

    std::shared_ptr<const EvaluationData> data;
    const MmsResult result = Gateway([&]() -> MmsResult {
        const MmsResult captured = capture_(*query, bound, data);
        if (captured != MMS_OK) return captured;
        // A successful capture must supply immutable sampling data. Malformed
        // records are the caller's contract violation and stay argument errors.
        if (!data || !data->sample) return MMS_E_FAILED;
        if (!Same(data->object, query->object)) return MMS_E_ARGUMENT;
        if (data->bones.size() > UINT32_MAX) return MMS_E_ARGUMENT;

        // Build the complete store-owned snapshot before taking the mutex. A
        // failed validation or allocation below publishes nothing.
        Index index;
        for (std::size_t n = 0; n < data->bones.size(); ++n) {
            const auto& bone = data->bones[n];
            if (!Valid(&bone) || bone.reserved || !ValidBoneFlags(bone.flags) || Empty(bone.id)) return MMS_E_ARGUMENT;
            if (!index.emplace(bone.id, static_cast<uint32_t>(n)).second) return MMS_E_ARGUMENT;
        }

        // The host reports its own snapshot through retainedBytes; the store
        // adds its payload and one conservative node per index entry plus the
        // tree sentinel. A host that under-reports still pays for the two
        // capacities this store can verify.
        uint64_t charge = sizeof(EvaluationData);
        if (!AddBytes(charge, data->bones.capacity(), sizeof(MmsEvaluationBoneV1))) return MMS_E_MEMORY;
        if (data->retainedBytes > charge) charge = data->retainedBytes;
        if (!AddBytes(charge, 1, sizeof(Payload))) return MMS_E_MEMORY;
        if (!AddBytes(charge, static_cast<uint64_t>(index.size()) + 1, kIndexNodeBytes)) return MMS_E_MEMORY;
        if (charge > bound) return MMS_E_MEMORY;

        auto payload = std::make_shared<Payload>();
        payload->data = data;
        payload->index = std::move(index);
        payload->object = data->object;
        payload->hostRevision = data->hostRevision;
        payload->boneCount = static_cast<uint32_t>(data->bones.size());
        payload->flags = MMS_EVALUATION_CAP_BONE_CHANNELS;
        payload->bytes = charge;

        std::lock_guard<std::mutex> lock(mutex_);
        // A capture admitted before Retire may finish, but it may not publish a
        // source after the client was retired. The active pin is released by
        // the caller below.
        if (client.retired) return MMS_E_NOT_READY;
        // Callback work may have released sources, so the quotas are rechecked
        // against the current table rather than the admission snapshot.
        if (client.handles >= kMaxHandlesPerClient || sources_.size() >= kMaxTotalHandles) return MMS_E_BUSY;
        if (next_ >= kHandleLimit) return MMS_E_BUSY;
        if (!captureId_) return MMS_E_BUSY;
        if (charge > kMaxClientBytes || client.bytes > kMaxClientBytes - charge) return MMS_E_MEMORY;
        if (charge > kMaxTotalBytes || totalBytes_ > kMaxTotalBytes - charge) return MMS_E_MEMORY;
        const MmsHandle handle = next_;
        const uint64_t captureId = captureId_;
        payload->captureId = captureId;
        // No counter or output is touched until the table node exists. A
        // throwing emplace leaves next_, captureId_ and *out unchanged.
        if (!sources_.emplace(handle, Entry{&client, payload}).second) return MMS_E_FAILED;
        next_ = handle + 1;
        captureId_ = captureId + 1;
        ++client.handles;
        client.bytes += charge;
        totalBytes_ += charge;
        // A worker can release the handle as soon as this lock is left. Drop
        // preparation references here so that last release settles its charge.
        payload.reset();
        data.reset();
        *out = handle;
        return MMS_OK;
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        acquiring_ = false;
        if (client.active) --client.active;
    }
    return result;
}

MmsResult EvaluationStore::Describe(Client& client, MmsHandle source, MmsEvaluationSourceInfoV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.data;
    MmsEvaluationSourceInfoV1 info{};
    info.h = Header<MmsEvaluationSourceInfoV1>();
    info.object = payload.object;
    info.captureId = payload.captureId;
    info.hostRevision = payload.hostRevision;
    info.retainedBytes = payload.bytes;
    info.boneCount = payload.boneCount;
    info.flags = payload.flags;
    *out = info;
    return MMS_OK;
}

MmsResult EvaluationStore::BoneAt(Client& client, MmsHandle source, uint32_t index, MmsEvaluationBoneV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.data;
    if (index >= payload.boneCount) return MMS_E_NOT_FOUND;
    MmsEvaluationBoneV1 bone = payload.data->bones[index];
    bone.h = Header<MmsEvaluationBoneV1>();
    bone.reserved = 0;
    *out = bone;
    return MMS_OK;
}

MmsResult EvaluationStore::Sample(Client& client, MmsHandle source, MmsId bone, MmsTime time, MmsBoneChannelsV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    if (!time.denominator || time.numerator < 0) return MMS_E_ARGUMENT;
    if (RatioGreater(uint64_t(time.numerator), time.denominator, uint64_t(frame_limits::maximum), 30)) return MMS_E_ARGUMENT;
    time = Normalize(time);
    const double frame = static_cast<double>(time.numerator) * 30.0 / static_cast<double>(time.denominator);
    if (!std::isfinite(frame) || frame > static_cast<double>(frame_limits::maximum)) return MMS_E_ARGUMENT;

    std::shared_ptr<const Payload> payload;
    uint32_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found = sources_.find(source);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        const auto& candidate = found->second.data;
        const auto located = candidate->index.find(bone);
        if (located == candidate->index.end()) return MMS_E_NOT_FOUND;
        index = located->second;
        // Only a registered local channel may reach the host callback.
        if (!(candidate->data->bones[index].flags & MMS_EVALUATION_BONE_LOCAL)) return MMS_E_UNSUPPORTED;
        // The copy and the pin are registered together, so Release cannot free
        // the payload between lookup and the first callback instruction.
        payload = candidate;
        ++client.active;
    }

    MmsBoneChannelsV1 value{};
    const MmsResult result = Gateway([&]() -> MmsResult {
        // The host writes into a private local record. Nothing reaches the
        // caller until every produced field has been validated.
        MmsBoneChannelsV1 produced{};
        const MmsResult sampled = payload->data->sample(index, frame, produced);
        if (sampled != MMS_OK) return sampled;
        for (float channel : produced.position) if (!std::isfinite(channel)) return MMS_E_FAILED;
        for (float channel : produced.rotation) if (!std::isfinite(channel)) return MMS_E_FAILED;
        for (float channel : produced.scale) if (!std::isfinite(channel)) return MMS_E_FAILED;
        if (produced.flags & ~static_cast<uint32_t>(MMS_EVALUATION_PHYSICS_DISABLED)) return MMS_E_FAILED;
        if (produced.reserved) return MMS_E_FAILED;
        MmsBoneChannelsV1 published = produced;
        published.h = Header<MmsBoneChannelsV1>();
        published.reserved = 0;
        value = published;
        return MMS_OK;
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result == MMS_OK) *out = value;
        // If Release erased the entry while the callback was running, this is
        // the last payload reference and the byte charge is returned here.
        Settle(payload, client);
        payload.reset();
        if (client.active) --client.active;
    }
    return result;
}

MmsResult EvaluationStore::Retain(Client& client, MmsHandle source, MmsHandle* out) {
    if (!out) return MMS_E_ARGUMENT;
    return Gateway([&]() -> MmsResult {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found = sources_.find(source);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        if (client.handles >= kMaxHandlesPerClient || sources_.size() >= kMaxTotalHandles) return MMS_E_BUSY;
        if (next_ >= kHandleLimit) return MMS_E_BUSY;
        const auto payload = found->second.data;
        const MmsHandle handle = next_;
        // The retained handle shares the payload, so it adds no byte charge.
        if (!sources_.emplace(handle, Entry{&client, payload}).second) return MMS_E_FAILED;
        next_ = handle + 1;
        ++client.handles;
        *out = handle;
        return MMS_OK;
    });
}

MmsResult EvaluationStore::Release(Client& client, MmsHandle source) {
    return Gateway([&]() -> MmsResult {
        std::lock_guard<std::mutex> lock(mutex_);
        // Release is the only call allowed after Retire: plugins drain handles
        // there, and the manager waits on HasLeases.
        const auto found = sources_.find(source);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        auto payload = found->second.data;
        sources_.erase(found);
        if (client.handles) --client.handles;
        Settle(payload, client);
        payload.reset();
        return MMS_OK;
    });
}

void EvaluationStore::Settle(const std::shared_ptr<const Payload>& payload, Client& client) noexcept {
    // The caller holds mutex_ and has already erased or is finalizing the last
    // reference it owns. One remaining reference means no handle and no call in
    // flight still uses the payload.
    if (!payload || payload.use_count() != 1) return;
    const uint64_t bytes = payload->bytes;
    client.bytes = client.bytes >= bytes ? client.bytes - bytes : 0;
    totalBytes_ = totalBytes_ >= bytes ? totalBytes_ - bytes : 0;
}
}
