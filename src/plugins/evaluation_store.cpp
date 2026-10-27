#include "evaluation_store.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/evaluation_time.hpp"
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
// The shared display sampler dispatches on these internal kinds; they never
// appear in a public record.
constexpr uint32_t kDisplayVisibility = 0, kDisplayIk = 1, kDisplayParent = 2;
// The 1.3 parent sampler extends the same shared display path; it is a second
// kind so both public versions validate against one canonical record.
constexpr uint32_t kDisplayParentState = 3;

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
// Morph records expose a registered weight kind plus BASE and panel flags.
bool ValidMorphFlags(uint32_t flags) noexcept {
    constexpr uint32_t known = MMS_MORPH_BASE | MMS_MORPH_PANEL_MASK;
    if (flags & ~known) return false;
    return ((flags & MMS_MORPH_PANEL_MASK) >> MMS_MORPH_PANEL_SHIFT) <= 4;
}
// Bone and morph samples share one frozen rational-second contract: exact
// comparison before conversion, normalization of equivalent fractions and no
// clamping. False means MMS_E_ARGUMENT.
bool FrameTime(MmsTime time, double& frame) noexcept {
    if (!time.denominator || time.numerator < 0) return false;
    if (RatioGreater(uint64_t(time.numerator), time.denominator, uint64_t(frame_limits::maximum), 30)) return false;
    time = Normalize(time);
    frame = static_cast<double>(time.numerator) * 30.0 / static_cast<double>(time.denominator);
    return std::isfinite(frame) && frame <= static_cast<double>(frame_limits::maximum);
}
// Display records are keyed on discrete integer frames, so a rational time
// selects the key at its floor. Converting through double first can round a
// value strictly below the next key up to it, so the floor stays in integer
// arithmetic. The bound is the one FrameTime applies; it runs before the
// division because it also proves whole*30 cannot leave the frame range.
bool DisplayFrame(MmsTime time, double& frame) noexcept {
    EvaluationTime resolved;
    if (!EvaluationTime::FromSeconds(time.numerator, time.denominator, resolved)) return false;
    frame = static_cast<double>(resolved.DiscreteFrame());
    return true;
}
// Produce one canonical parent state from whichever host sampler is present.
// The store's newer callback already reports kind and leaves identities empty
// outside MODEL. The legacy callback only knows active/inactive, and its
// flags/reserved and identity rules are validated before translation.
MmsResult CanonicalParent(const EvaluationData& data, uint32_t index, double frame,
    MmsParentStateV1& out) {
    if (!data.sampleParentState) {
        if (!data.sampleParent) return MMS_E_FAILED;
        MmsParentSelectionV1 legacy{};
        const MmsResult sampled = data.sampleParent(index, frame, legacy);
        if (sampled != MMS_OK) return sampled;
        if ((legacy.flags & ~MMS_EVALUATION_PARENT_ACTIVE) || legacy.reserved) return MMS_E_FAILED;
        const bool active = (legacy.flags & MMS_EVALUATION_PARENT_ACTIVE) != 0;
        if (active) {
            if (Empty(legacy.target.id) || !legacy.target.generation || Empty(legacy.bone)) return MMS_E_FAILED;
        } else if (!Empty(legacy.target.id) || legacy.target.generation || !Empty(legacy.bone)) {
            return MMS_E_FAILED;
        }
        MmsParentStateV1 value{};
        value.target = legacy.target;
        value.bone = legacy.bone;
        value.windowStart = legacy.windowStart;
        value.windowEnd = legacy.windowEnd;
        value.kind = active ? MMS_EVALUATION_PARENT_MODEL : MMS_EVALUATION_PARENT_INHERIT;
        out = value;
        return MMS_OK;
    }
    MmsParentStateV1 produced{};
    const MmsResult sampled = data.sampleParentState(index, frame, produced);
    if (sampled != MMS_OK) return sampled;
    out = produced;
    return MMS_OK;
}
// Canonical fields are checked once for both public versions. Windows keep
// their frozen boundaries and containment rule; only MODEL carries identities.
bool ValidParentState(const MmsParentStateV1& state, double frame) noexcept {
    const uint32_t maximum = static_cast<uint32_t>(frame_limits::maximum);
    // The window is frozen metadata: a zero end is unbounded, a bounded end
    // is exclusive, and both ends must contain the sampled frame.
    if (state.windowStart > maximum || state.windowEnd > maximum) return false;
    if (state.windowEnd && state.windowEnd <= state.windowStart) return false;
    if (frame < static_cast<double>(state.windowStart)) return false;
    if (state.windowEnd && !(frame < static_cast<double>(state.windowEnd))) return false;
    if (state.kind > MMS_EVALUATION_PARENT_GROUND || state.reserved) return false;
    if (state.kind == MMS_EVALUATION_PARENT_MODEL) {
        return !Empty(state.target.id) && state.target.generation && !Empty(state.bone);
    }
    // INHERIT and GROUND are states without target identities, not absent
    // targets disguised as MODEL records.
    return Empty(state.target.id) && !state.target.generation && Empty(state.bone);
}
}

// Store-owned half of a source. The host's EvaluationData is immutable and
// already includes its own snapshot and channel capacity in retainedBytes;
// this payload adds one ID index per captured channel and the shared table
// nodes that resolve those IDs.
struct EvaluationStore::Payload {
    std::shared_ptr<const EvaluationData> data;
    Index index, morphIndex;
    MmsObjectRef object{};
    uint64_t bytes = 0, captureId = 0, hostRevision = 0;
    uint32_t boneCount = 0, morphCount = 0, ikCount = 0, selectorCount = 0, flags = 0;
};

EvaluationStore::EvaluationStore(Capture capture) : capture_(std::move(capture)) {}
EvaluationStore::EvaluationStore(CaptureChannels captureChannels, uint32_t supportedChannels)
    : captureChannels_(std::move(captureChannels)), supportedChannels_(supportedChannels) {
    constexpr uint32_t known = MMS_EVALUATION_CAP_BONE_CHANNELS | MMS_EVALUATION_CAP_MORPH_WEIGHTS |
        MMS_EVALUATION_CAP_DISPLAY;
    if (!supportedChannels_ || (supportedChannels_ & ~known)) throw std::invalid_argument("invalid supported channels");
}
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

MmsEvaluationApiV1_1 EvaluationStore::Api11(Client& client) {
    // The first seven entries are the frozen V1 table, so the appended morph
    // operations extend one lease system rather than starting another. Only
    // the header version differs; Api11 still never writes Client::owner.
    const MmsEvaluationApiV1 base = Api(client);
    MmsEvaluationApiV1_1 api{};
    api.h = MmsHeader{sizeof(MmsEvaluationApiV1_1), 1, 1};
    api.context = base.context;
    api.getCapabilities = base.getCapabilities;
    api.createSource = base.createSource;
    api.describeSource = base.describeSource;
    api.boneAt = base.boneAt;
    api.sampleBone = base.sampleBone;
    api.retain = base.retain;
    api.release = base.release;
    api.createSourceChannels = [](void* context, const MmsEvaluationSourceQueryV1* query, uint32_t channels, MmsHandle* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->CreateChannels(client, query, channels, out);
        });
    };
    api.describeSourceChannels = [](void* context, MmsHandle source, MmsEvaluationSourceInfoV1_1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->DescribeChannels(client, source, out);
        });
    };
    api.morphAt = [](void* context, MmsHandle source, uint32_t index, MmsEvaluationMorphV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->MorphAt(client, source, index, out);
        });
    };
    api.sampleMorph = [](void* context, MmsHandle source, MmsId morph, MmsTime time, MmsMorphWeightV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SampleMorph(client, source, morph, time, out);
        });
    };
    return api;
}

MmsEvaluationApiV1_2 EvaluationStore::Api12(Client& client) {
    // The first eleven entries are the frozen V1.1 table, so the display
    // operations extend one lease system rather than starting another. Only
    // the header version differs; Api12 still never writes Client::owner.
    const MmsEvaluationApiV1_1 base = Api11(client);
    MmsEvaluationApiV1_2 api{};
    api.h = MmsHeader{sizeof(MmsEvaluationApiV1_2), 1, 2};
    api.context = base.context;
    api.getCapabilities = base.getCapabilities;
    api.createSource = base.createSource;
    api.describeSource = base.describeSource;
    api.boneAt = base.boneAt;
    api.sampleBone = base.sampleBone;
    api.retain = base.retain;
    api.release = base.release;
    api.createSourceChannels = base.createSourceChannels;
    api.describeSourceChannels = base.describeSourceChannels;
    api.morphAt = base.morphAt;
    api.sampleMorph = base.sampleMorph;
    api.describeSourceDisplay = [](void* context, MmsHandle source, MmsEvaluationSourceInfoV1_2* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->DescribeDisplay(client, source, out);
        });
    };
    api.ikAt = [](void* context, MmsHandle source, uint32_t index, MmsEvaluationIkV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->IkAt(client, source, index, out);
        });
    };
    api.selectorAt = [](void* context, MmsHandle source, uint32_t index, MmsEvaluationSelectorV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SelectorAt(client, source, index, out);
        });
    };
    api.sampleVisibility = [](void* context, MmsHandle source, MmsTime time, MmsDisplayStateV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SampleVisibility(client, source, time, out);
        });
    };
    api.sampleIk = [](void* context, MmsHandle source, uint32_t index, MmsTime time, MmsIkStateV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SampleIk(client, source, index, time, out);
        });
    };
    api.sampleParent = [](void* context, MmsHandle source, uint32_t index, MmsTime time, MmsParentSelectionV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SampleParent(client, source, index, time, out);
        });
    };
    return api;
}

MmsEvaluationApiV1_3 EvaluationStore::Api13(Client& client) {
    // The first seventeen entries are the frozen V1.2 table, so the appended
    // parent-state sampler extends the same lease system. Only the header
    // version differs; Api13 still never writes Client::owner.
    static_assert(offsetof(MmsEvaluationApiV1_3, sampleParentState) == sizeof(MmsEvaluationApiV1_2),
        "evaluation 1.3 appends the parent-state sampler to the 1.2 prefix");
    const MmsEvaluationApiV1_2 base = Api12(client);
    MmsEvaluationApiV1_3 api{};
    api.h = MmsHeader{sizeof(MmsEvaluationApiV1_3), 1, 3};
    api.context = base.context;
    api.getCapabilities = base.getCapabilities;
    api.createSource = base.createSource;
    api.describeSource = base.describeSource;
    api.boneAt = base.boneAt;
    api.sampleBone = base.sampleBone;
    api.retain = base.retain;
    api.release = base.release;
    api.createSourceChannels = base.createSourceChannels;
    api.describeSourceChannels = base.describeSourceChannels;
    api.morphAt = base.morphAt;
    api.sampleMorph = base.sampleMorph;
    api.describeSourceDisplay = base.describeSourceDisplay;
    api.ikAt = base.ikAt;
    api.selectorAt = base.selectorAt;
    api.sampleVisibility = base.sampleVisibility;
    api.sampleIk = base.sampleIk;
    api.sampleParent = base.sampleParent;
    api.sampleParentState = [](void* context, MmsHandle source, uint32_t index, MmsTime time, MmsParentStateV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SampleParentState(client, source, index, time, out);
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
    // Advertise exactly the channels this store was built to capture.
    caps.flags = supportedChannels_;
    caps.maxSources = kMaxSources;
    caps.maxSourceBytes = kMaxSourceBytes;
    caps.maxClientBytes = kMaxClientBytes;
    caps.maxTotalBytes = kMaxTotalBytes;
    *out = caps;
    return MMS_OK;
}

MmsResult EvaluationStore::Create(Client& client, const MmsEvaluationSourceQueryV1* query, MmsHandle* out) {
    // The frozen V1 entry keeps its bone-only meaning by selecting only that
    // channel through the shared lease path.
    return CreateChannels(client, query, MMS_EVALUATION_CAP_BONE_CHANNELS, out);
}

MmsResult EvaluationStore::CreateChannels(Client& client, const MmsEvaluationSourceQueryV1* query,
    uint32_t channels, MmsHandle* out) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!out) return MMS_E_ARGUMENT;
    if (!Valid(query)) return MMS_E_ARGUMENT;
    if (query->flags || query->reserved) return MMS_E_ARGUMENT;
    if (Empty(query->object.id) || !query->object.generation) return MMS_E_ARGUMENT;
    if (query->maxRetainedBytes > kMaxSourceBytes) return MMS_E_ARGUMENT;
    constexpr uint32_t known = MMS_EVALUATION_CAP_BONE_CHANNELS | MMS_EVALUATION_CAP_MORPH_WEIGHTS |
        MMS_EVALUATION_CAP_DISPLAY;
    // A zero mask or a bit this version does not define is a malformed request.
    if (!channels || (channels & ~known)) return MMS_E_ARGUMENT;
    const bool bones = (channels & MMS_EVALUATION_CAP_BONE_CHANNELS) != 0;
    const bool morphs = (channels & MMS_EVALUATION_CAP_MORPH_WEIGHTS) != 0;
    const bool display = (channels & MMS_EVALUATION_CAP_DISPLAY) != 0;
    const uint64_t bound = query->maxRetainedBytes ? query->maxRetainedBytes : kMaxSourceBytes;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        // Selection is limited to the channels this store was built to freeze,
        // so a known but unadvertised bit is a capability mismatch. Retirement
        // still refuses every new operation first.
        if (channels & ~supportedChannels_) return MMS_E_UNSUPPORTED;
        if (!capture_ && !captureChannels_) return MMS_E_NOT_READY;
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
        // Both channels are frozen by one atomic callback, so a source either
        // carries the whole requested set or publishes nothing.
        const MmsResult captured = captureChannels_
            ? captureChannels_(*query, channels, bound, data)
            : capture_(*query, bound, data);
        if (captured != MMS_OK) return captured;
        if (!data) return MMS_E_FAILED;
        if (!Same(data->object, query->object)) return MMS_E_ARGUMENT;
        // The capture reports the channels it froze; a mismatch means records
        // may not correspond to the requested source.
        if (data->channels != channels) return MMS_E_ARGUMENT;
        if (data->bones.size() > UINT32_MAX || data->morphs.size() > UINT32_MAX ||
            data->ik.size() > UINT32_MAX || data->selectors.size() > UINT32_MAX) return MMS_E_ARGUMENT;
        // An unselected channel may not carry records or a callback, while a
        // selected channel must supply its sampler even when it froze nothing.
        if (!bones && (!data->bones.empty() || data->sample)) return MMS_E_ARGUMENT;
        if (!morphs && (!data->morphs.empty() || data->sampleMorph)) return MMS_E_ARGUMENT;
        if (!display && (!data->ik.empty() || !data->selectors.empty() ||
            data->sampleVisibility || data->sampleIk || data->sampleParent ||
            data->sampleParentState)) return MMS_E_ARGUMENT;
        if (bones && !data->sample) return MMS_E_FAILED;
        if (morphs && !data->sampleMorph) return MMS_E_FAILED;
        // Display captures one visibility record, one IK switch per chain and
        // one parent selection per target, so those samplers are required even
        // when both display vectors are empty. Either parent sampler version
        // supplies the canonical state; the newer one takes priority.
        if (display && (!data->sampleVisibility || !data->sampleIk ||
            (!data->sampleParent && !data->sampleParentState))) return MMS_E_FAILED;

        // Build every store-owned index before taking the mutex. A failed
        // validation or allocation below publishes nothing. A morph ID that
        // duplicates another morph or aliases a bone ID is ambiguous.
        Index index, morphIndex;
        for (std::size_t n = 0; n < data->bones.size(); ++n) {
            const auto& bone = data->bones[n];
            if (!Valid(&bone) || bone.reserved || !ValidBoneFlags(bone.flags) || Empty(bone.id)) return MMS_E_ARGUMENT;
            if (!index.emplace(bone.id, static_cast<uint32_t>(n)).second) return MMS_E_ARGUMENT;
        }
        for (std::size_t n = 0; n < data->morphs.size(); ++n) {
            const auto& morph = data->morphs[n];
            if (!Valid(&morph) || Empty(morph.id)) return MMS_E_ARGUMENT;
            if (morph.kind > MMS_MORPH_MATERIAL) return MMS_E_ARGUMENT;
            if (!ValidMorphFlags(morph.flags)) return MMS_E_ARGUMENT;
            if (index.find(morph.id) != index.end()) return MMS_E_ARGUMENT;
            if (!morphIndex.emplace(morph.id, static_cast<uint32_t>(n)).second) return MMS_E_ARGUMENT;
        }
        // IK chains and selectors are sampled by index only, so duplicate bone
        // IDs and bones shared with the other channels are legal. A selector is
        // either global to the source model or bound to one nonempty bone.
        for (const auto& ik : data->ik) {
            if (!Valid(&ik) || Empty(ik.bone)) return MMS_E_ARGUMENT;
        }
        for (const auto& selector : data->selectors) {
            if (!Valid(&selector) || selector.reserved) return MMS_E_ARGUMENT;
            if (selector.flags & ~MMS_EVALUATION_SELECTOR_GLOBAL) return MMS_E_ARGUMENT;
            if ((selector.flags & MMS_EVALUATION_SELECTOR_GLOBAL) ? !Empty(selector.bone) : Empty(selector.bone)) return MMS_E_ARGUMENT;
        }

        // The host reports its own snapshot through retainedBytes; the store
        // adds its payload and one conservative node per channel entry plus
        // each tree sentinel. A host that under-reports still pays for the
        // capacities this store can verify.
        uint64_t charge = sizeof(EvaluationData);
        if (!AddBytes(charge, data->bones.capacity(), sizeof(MmsEvaluationBoneV1))) return MMS_E_MEMORY;
        if (!AddBytes(charge, data->morphs.capacity(), sizeof(MmsEvaluationMorphV1))) return MMS_E_MEMORY;
        if (!AddBytes(charge, data->ik.capacity(), sizeof(MmsEvaluationIkV1))) return MMS_E_MEMORY;
        if (!AddBytes(charge, data->selectors.capacity(), sizeof(MmsEvaluationSelectorV1))) return MMS_E_MEMORY;
        if (data->retainedBytes > charge) charge = data->retainedBytes;
        if (!AddBytes(charge, 1, sizeof(Payload))) return MMS_E_MEMORY;
        if (!AddBytes(charge, static_cast<uint64_t>(index.size()) + 1, kIndexNodeBytes)) return MMS_E_MEMORY;
        if (!AddBytes(charge, static_cast<uint64_t>(morphIndex.size()) + 1, kIndexNodeBytes)) return MMS_E_MEMORY;
        if (charge > bound) return MMS_E_MEMORY;

        auto payload = std::make_shared<Payload>();
        payload->data = data;
        payload->index = std::move(index);
        payload->morphIndex = std::move(morphIndex);
        payload->object = data->object;
        payload->hostRevision = data->hostRevision;
        payload->boneCount = static_cast<uint32_t>(data->bones.size());
        payload->morphCount = static_cast<uint32_t>(data->morphs.size());
        payload->ikCount = static_cast<uint32_t>(data->ik.size());
        payload->selectorCount = static_cast<uint32_t>(data->selectors.size());
        payload->flags = channels;
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

MmsResult EvaluationStore::DescribeChannels(Client& client, MmsHandle source, MmsEvaluationSourceInfoV1_1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.data;
    MmsEvaluationSourceInfoV1_1 info{};
    info.h = MmsHeader{sizeof(MmsEvaluationSourceInfoV1_1), 1, 1};
    info.object = payload.object;
    info.captureId = payload.captureId;
    info.hostRevision = payload.hostRevision;
    info.retainedBytes = payload.bytes;
    info.boneCount = payload.boneCount;
    info.flags = payload.flags;
    info.morphCount = payload.morphCount;
    info.reserved = 0;
    *out = info;
    return MMS_OK;
}

MmsResult EvaluationStore::DescribeDisplay(Client& client, MmsHandle source, MmsEvaluationSourceInfoV1_2* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.data;
    // The V1.2 description keeps the V1.1 fields and appends the two display
    // counts, so one capture is described through every table version.
    MmsEvaluationSourceInfoV1_2 info{};
    info.h = MmsHeader{sizeof(MmsEvaluationSourceInfoV1_2), 1, 2};
    info.object = payload.object;
    info.captureId = payload.captureId;
    info.hostRevision = payload.hostRevision;
    info.retainedBytes = payload.bytes;
    info.boneCount = payload.boneCount;
    info.flags = payload.flags;
    info.morphCount = payload.morphCount;
    info.reserved = 0;
    info.ikCount = payload.ikCount;
    info.selectorCount = payload.selectorCount;
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
    if (!(payload.flags & MMS_EVALUATION_CAP_BONE_CHANNELS)) return MMS_E_UNSUPPORTED;
    if (index >= payload.boneCount) return MMS_E_NOT_FOUND;
    MmsEvaluationBoneV1 bone = payload.data->bones[index];
    bone.h = Header<MmsEvaluationBoneV1>();
    bone.reserved = 0;
    *out = bone;
    return MMS_OK;
}

MmsResult EvaluationStore::MorphAt(Client& client, MmsHandle source, uint32_t index, MmsEvaluationMorphV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.data;
    if (!(payload.flags & MMS_EVALUATION_CAP_MORPH_WEIGHTS)) return MMS_E_UNSUPPORTED;
    if (index >= payload.morphCount) return MMS_E_NOT_FOUND;
    MmsEvaluationMorphV1 morph = payload.data->morphs[index];
    morph.h = Header<MmsEvaluationMorphV1>();
    *out = morph;
    return MMS_OK;
}

MmsResult EvaluationStore::IkAt(Client& client, MmsHandle source, uint32_t index, MmsEvaluationIkV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.data;
    if (!(payload.flags & MMS_EVALUATION_CAP_DISPLAY)) return MMS_E_UNSUPPORTED;
    if (index >= payload.ikCount) return MMS_E_NOT_FOUND;
    MmsEvaluationIkV1 ik = payload.data->ik[index];
    ik.h = Header<MmsEvaluationIkV1>();
    *out = ik;
    return MMS_OK;
}

MmsResult EvaluationStore::SelectorAt(Client& client, MmsHandle source, uint32_t index, MmsEvaluationSelectorV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const Payload& payload = *found->second.data;
    if (!(payload.flags & MMS_EVALUATION_CAP_DISPLAY)) return MMS_E_UNSUPPORTED;
    if (index >= payload.selectorCount) return MMS_E_NOT_FOUND;
    MmsEvaluationSelectorV1 selector = payload.data->selectors[index];
    selector.h = Header<MmsEvaluationSelectorV1>();
    selector.reserved = 0;
    *out = selector;
    return MMS_OK;
}

MmsResult EvaluationStore::Sample(Client& client, MmsHandle source, MmsId bone, MmsTime time, MmsBoneChannelsV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    double frame = 0;
    if (!FrameTime(time, frame)) return MMS_E_ARGUMENT;

    std::shared_ptr<const Payload> payload;
    uint32_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found = sources_.find(source);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        const auto& candidate = found->second.data;
        if (!(candidate->flags & MMS_EVALUATION_CAP_BONE_CHANNELS)) return MMS_E_UNSUPPORTED;
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

MmsResult EvaluationStore::SampleMorph(Client& client, MmsHandle source, MmsId morph, MmsTime time, MmsMorphWeightV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    double frame = 0;
    if (!FrameTime(time, frame)) return MMS_E_ARGUMENT;

    std::shared_ptr<const Payload> payload;
    uint32_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found = sources_.find(source);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        const auto& candidate = found->second.data;
        if (!(candidate->flags & MMS_EVALUATION_CAP_MORPH_WEIGHTS)) return MMS_E_UNSUPPORTED;
        const auto located = candidate->morphIndex.find(morph);
        if (located == candidate->morphIndex.end()) return MMS_E_NOT_FOUND;
        index = located->second;
        // The copy and the pin are registered together, so Release cannot free
        // the payload between lookup and the first callback instruction.
        payload = candidate;
        ++client.active;
    }

    MmsMorphWeightV1 value{};
    const MmsResult result = Gateway([&]() -> MmsResult {
        // The host writes into a private local record. Nothing reaches the
        // caller until every produced field has been validated. The weight is
        // a registered direct value, so it is published without clamping.
        MmsMorphWeightV1 produced{};
        const MmsResult sampled = payload->data->sampleMorph(index, frame, produced);
        if (sampled != MMS_OK) return sampled;
        if (!std::isfinite(produced.value)) return MMS_E_FAILED;
        if (produced.reserved) return MMS_E_FAILED;
        MmsMorphWeightV1 published = produced;
        published.h = Header<MmsMorphWeightV1>();
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

MmsResult EvaluationStore::SampleVisibility(Client& client, MmsHandle source, MmsTime time, MmsDisplayStateV1* out) {
    return ReadDisplay(client, source, kDisplayVisibility, 0, time, out);
}

MmsResult EvaluationStore::SampleIk(Client& client, MmsHandle source, uint32_t index, MmsTime time, MmsIkStateV1* out) {
    return ReadDisplay(client, source, kDisplayIk, index, time, out);
}

MmsResult EvaluationStore::SampleParent(Client& client, MmsHandle source, uint32_t index, MmsTime time, MmsParentSelectionV1* out) {
    return ReadDisplay(client, source, kDisplayParent, index, time, out);
}

MmsResult EvaluationStore::SampleParentState(Client& client, MmsHandle source, uint32_t index, MmsTime time, MmsParentStateV1* out) {
    return ReadDisplay(client, source, kDisplayParentState, index, time, out);
}

MmsResult EvaluationStore::ReadDisplay(Client& client, MmsHandle source, uint32_t kind, uint32_t index,
    MmsTime time, void* out) {
    if (!out || kind > kDisplayParentState) return MMS_E_ARGUMENT;
    double frame = 0;
    if (!DisplayFrame(time, frame)) return MMS_E_ARGUMENT;

    std::shared_ptr<const Payload> payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found = sources_.find(source);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        const auto& candidate = found->second.data;
        if (!(candidate->flags & MMS_EVALUATION_CAP_DISPLAY)) return MMS_E_UNSUPPORTED;
        // Visibility is one source-wide record; IK and parent selections resolve
        // by index inside the same frozen capture. Both parent kinds share the
        // selector table, so one index check covers them.
        if (kind == kDisplayIk && index >= candidate->ikCount) return MMS_E_NOT_FOUND;
        if ((kind == kDisplayParent || kind == kDisplayParentState) && index >= candidate->selectorCount) {
            return MMS_E_NOT_FOUND;
        }
        // The copy and the pin are registered together, so Release cannot free
        // the payload between lookup and the first callback instruction.
        payload = candidate;
        ++client.active;
    }

    MmsDisplayStateV1 visibility{};
    MmsIkStateV1 ikState{};
    MmsParentSelectionV1 parentSelection{};
    MmsParentStateV1 parentState{};
    const MmsResult result = Gateway([&]() -> MmsResult {
        if (kind == kDisplayVisibility) {
            // The host writes into a private local record. Nothing reaches the
            // caller until every produced field has been validated.
            MmsDisplayStateV1 produced{};
            const MmsResult sampled = payload->data->sampleVisibility(frame, produced);
            if (sampled != MMS_OK) return sampled;
            if (produced.visible > 255 || produced.reserved) return MMS_E_FAILED;
            produced.h = Header<MmsDisplayStateV1>();
            produced.reserved = 0;
            visibility = produced;
            return MMS_OK;
        }
        if (kind == kDisplayIk) {
            MmsIkStateV1 produced{};
            const MmsResult sampled = payload->data->sampleIk(index, frame, produced);
            if (sampled != MMS_OK) return sampled;
            if (produced.enabled > 255 || produced.reserved) return MMS_E_FAILED;
            produced.h = Header<MmsIkStateV1>();
            produced.reserved = 0;
            ikState = produced;
            return MMS_OK;
        }
        // Both parent versions sample one canonical record, so kind, window and
        // identity rules are validated once before either output is written.
        MmsParentStateV1 canonical{};
        const MmsResult sampled = CanonicalParent(*payload->data, index, frame, canonical);
        if (sampled != MMS_OK) return sampled;
        if (!ValidParentState(canonical, frame)) return MMS_E_FAILED;
        if (kind == kDisplayParentState) {
            canonical.h = Header<MmsParentStateV1>();
            canonical.reserved = 0;
            parentState = canonical;
            return MMS_OK;
        }
        // The 1.2 record cannot express ground, so it refuses the state instead
        // of collapsing it into ACTIVE or an inherited parent. The caller
        // output stays untouched and the shared cleanup still runs.
        if (canonical.kind == MMS_EVALUATION_PARENT_GROUND) return MMS_E_UNSUPPORTED;
        MmsParentSelectionV1 produced{};
        produced.windowStart = canonical.windowStart;
        produced.windowEnd = canonical.windowEnd;
        if (canonical.kind == MMS_EVALUATION_PARENT_MODEL) {
            produced.target = canonical.target;
            produced.bone = canonical.bone;
            produced.flags = MMS_EVALUATION_PARENT_ACTIVE;
        }
        produced.h = Header<MmsParentSelectionV1>();
        produced.reserved = 0;
        parentSelection = produced;
        return MMS_OK;
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The caller-owned output is written only after the whole record passed
        // validation, so a rejection leaves it untouched.
        if (result == MMS_OK) {
            if (kind == kDisplayVisibility) *static_cast<MmsDisplayStateV1*>(out) = visibility;
            else if (kind == kDisplayIk) *static_cast<MmsIkStateV1*>(out) = ikState;
            else if (kind == kDisplayParentState) *static_cast<MmsParentStateV1*>(out) = parentState;
            else *static_cast<MmsParentSelectionV1*>(out) = parentSelection;
        }
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
