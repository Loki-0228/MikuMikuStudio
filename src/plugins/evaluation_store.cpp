#include "evaluation_store.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/evaluation_time.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mikumikustudio::plugins {
namespace {
constexpr uint64_t kMaxSourceBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxClientBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaxTotalBytes = 256ull * 1024 * 1024;
constexpr uint32_t kMaxHandlesPerClient = 128;
constexpr uint32_t kMaxTotalHandles = 1024;
constexpr uint32_t kMaxSources = 128;
constexpr uint64_t kMaxWorkspaceBytes = 128ull * 1024 * 1024;
constexpr uint64_t kMaxClientWorkspaceBytes = 256ull * 1024 * 1024;
constexpr uint64_t kMaxTotalWorkspaceBytes = 512ull * 1024 * 1024;
constexpr uint32_t kMaxMeshBatch = 4096;
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
bool ValidMeshInfo(const MmsEvaluationMeshInfoV1& info, const MmsObjectRef& object) noexcept {
    constexpr uint32_t flags = MMS_EVALUATION_MESH_TOPOLOGY | MMS_EVALUATION_MESH_CPU_VERTICES;
    return Same(info.object.id, object.id) && info.object.generation == object.generation &&
        info.flags == flags && !info.reserved && info.additionalUvCount <= 4 && info.indexCount % 3 == 0;
}

// Account the immutable identity map independently for every scene source and
// frame. It can outlive either payload, so sharing it never discounts a charge.
MmsResult SceneModelBytes(const EvaluationSceneModels& models, uint64_t& bytes) noexcept {
    if (models.size() > UINT32_MAX) return MMS_E_ARGUMENT;
    if (!AddBytes(bytes, 1, sizeof(EvaluationSceneModels)) ||
        !AddBytes(bytes, models.capacity(), sizeof(EvaluationSceneModelData))) return MMS_E_MEMORY;
    for (const auto& model : models) {
        if (model.bones.size() > UINT32_MAX) return MMS_E_ARGUMENT;
        if (!AddBytes(bytes, model.bones.capacity(), sizeof(MmsId))) return MMS_E_MEMORY;
    }
    return MMS_OK;
}
MmsResult SceneMaterialBytes(const EvaluationMaterialIds& materials, uint64_t& bytes) noexcept {
    if (materials.size()>UINT32_MAX || !AddBytes(bytes,1,sizeof(EvaluationMaterialIds)) ||
        !AddBytes(bytes,materials.capacity(),sizeof(std::vector<MmsId>))) return MMS_E_MEMORY;
    for (const auto& ids:materials)
        if (ids.size()>UINT32_MAX || !AddBytes(bytes,ids.capacity(),sizeof(MmsId))) return MMS_E_MEMORY;
    return MMS_OK;
}
MmsResult ValidateMaterialIds(const EvaluationSceneModels& models, const EvaluationMaterialIds& materials, uint64_t limit) {
    if (models.size()!=materials.size()) return MMS_E_FAILED;
    uint64_t count=models.size();
    for (size_t i=0;i<models.size();++i) {
        if (!AddBytes(count,models[i].bones.size(),1) || !AddBytes(count,materials[i].size(),1)) return MMS_E_MEMORY;
    }
    uint64_t bytes=sizeof(std::vector<MmsId>);
    if (!AddBytes(bytes,count,sizeof(MmsId)) || bytes>limit || count>SIZE_MAX) return MMS_E_MEMORY;
    std::vector<MmsId> ids;ids.reserve(static_cast<size_t>(count));
    bytes=sizeof(ids);if (!AddBytes(bytes,ids.capacity(),sizeof(MmsId)) || bytes>limit) return MMS_E_MEMORY;
    for (size_t i=0;i<models.size();++i) {
        ids.push_back(models[i].object.id);
        ids.insert(ids.end(),models[i].bones.begin(),models[i].bones.end());
        for (const auto& id:materials[i]) {if (Empty(id)) return MMS_E_FAILED;ids.push_back(id);}
    }
    std::sort(ids.begin(),ids.end(),IdLess{});
    for(size_t i=1;i<ids.size();++i) if(Same(ids[i-1],ids[i])) return MMS_E_FAILED;
    return MMS_OK;
}

MmsResult ValidateSceneModels(const EvaluationSceneModels& models, uint64_t scratchLimit) {
    // The existing identity registry assigns all object and element IDs in one
    // namespace. Validate that namespace with one bounded temporary array,
    // rather than allocating an unchecked tree for every model.
    uint64_t count = models.size();
    for (const auto& model : models)
        if (!AddBytes(count, model.bones.size(), 1)) return MMS_E_MEMORY;
    uint64_t bytes = sizeof(std::vector<MmsId>);
    if (!AddBytes(bytes, count, sizeof(MmsId)) || bytes > scratchLimit || count > SIZE_MAX) return MMS_E_MEMORY;
    std::vector<MmsId> identities;
    identities.reserve(static_cast<size_t>(count));
    bytes = sizeof(identities);
    if (!AddBytes(bytes, identities.capacity(), sizeof(MmsId)) || bytes > scratchLimit) return MMS_E_MEMORY;
    for (const auto& model : models) {
        if (Empty(model.object.id) || !model.object.generation) return MMS_E_ARGUMENT;
        identities.push_back(model.object.id);
        for (const auto& bone : model.bones) {
            if (Empty(bone)) return MMS_E_ARGUMENT;
            identities.push_back(bone);
        }
    }
    std::sort(identities.begin(), identities.end(), IdLess{});
    for (size_t index = 1; index < identities.size(); ++index)
        if (Same(identities[index - 1], identities[index])) return MMS_E_ARGUMENT;
    return MMS_OK;
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
    enum class Kind { Channels, SceneSource, SceneFrame };
    Kind kind = Kind::Channels;
    std::shared_ptr<const EvaluationData> data;
    std::shared_ptr<const EvaluationSceneSourceData> sceneSource;
    std::shared_ptr<const EvaluationSceneFrameData> sceneFrame;
    Index index, morphIndex;
    MmsObjectRef object{};
    uint64_t bytes = 0, captureId = 0, hostRevision = 0;
    uint32_t boneCount = 0, morphCount = 0, ikCount = 0, selectorCount = 0, flags = 0;
    uint64_t frameId = 0;
    MmsTime time{};
};

EvaluationStore::EvaluationStore(Capture capture) : capture_(std::move(capture)) {}
EvaluationStore::EvaluationStore(CaptureChannels captureChannels, uint32_t supportedChannels, CaptureScene scene, CaptureScene meshScene, CaptureScene materialScene)
    : captureChannels_(std::move(captureChannels)), captureScene_(std::move(scene)), captureMeshScene_(std::move(meshScene)), captureMaterialScene_(std::move(materialScene)), supportedChannels_(supportedChannels) {
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

MmsEvaluationApiV1_4 EvaluationStore::Api14(Client& client) {
    static_assert(offsetof(MmsEvaluationApiV1_4, getSceneCapabilities) == sizeof(MmsEvaluationApiV1_3),
        "evaluation 1.4 preserves the complete 1.3 prefix");
    static_assert(sizeof(MmsEvaluationApiV1_4) == sizeof(MmsEvaluationApiV1_3) + 9 * sizeof(void*),
        "evaluation 1.4 appends exactly nine functions");
    const auto base = Api13(client);
    MmsEvaluationApiV1_4 api{};
    api.h = MmsHeader{sizeof(MmsEvaluationApiV1_4), 1, 4};
#define MMS_EVALUATION_COPY_PREFIX(field) \
    static_assert(offsetof(MmsEvaluationApiV1_4, field) == offsetof(MmsEvaluationApiV1_3, field), \
        "evaluation prefix field moved"); api.field = base.field
    MMS_EVALUATION_COPY_PREFIX(context);
    MMS_EVALUATION_COPY_PREFIX(getCapabilities);
    MMS_EVALUATION_COPY_PREFIX(createSource);
    MMS_EVALUATION_COPY_PREFIX(describeSource);
    MMS_EVALUATION_COPY_PREFIX(boneAt);
    MMS_EVALUATION_COPY_PREFIX(sampleBone);
    MMS_EVALUATION_COPY_PREFIX(retain);
    MMS_EVALUATION_COPY_PREFIX(release);
    MMS_EVALUATION_COPY_PREFIX(createSourceChannels);
    MMS_EVALUATION_COPY_PREFIX(describeSourceChannels);
    MMS_EVALUATION_COPY_PREFIX(morphAt);
    MMS_EVALUATION_COPY_PREFIX(sampleMorph);
    MMS_EVALUATION_COPY_PREFIX(describeSourceDisplay);
    MMS_EVALUATION_COPY_PREFIX(ikAt);
    MMS_EVALUATION_COPY_PREFIX(selectorAt);
    MMS_EVALUATION_COPY_PREFIX(sampleVisibility);
    MMS_EVALUATION_COPY_PREFIX(sampleIk);
    MMS_EVALUATION_COPY_PREFIX(sampleParent);
    MMS_EVALUATION_COPY_PREFIX(sampleParentState);
#undef MMS_EVALUATION_COPY_PREFIX
    api.getSceneCapabilities = [](void* context, MmsEvaluationSceneCapsV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SceneCapabilities(client, out);
        });
    };
    api.createSceneSource = [](void* context, const MmsEvaluationSceneQueryV1* query, MmsHandle* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->CreateScene(client, query, out);
        });
    };
    api.describeSceneSource = [](void* context, MmsHandle source, MmsEvaluationSceneInfoV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->DescribeScene(client, source, out);
        });
    };
    api.sceneModelAt = [](void* context, MmsHandle source, uint32_t index, MmsEvaluationSceneModelV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SceneModelAt(client, source, index, out);
        });
    };
    api.sceneBoneAt = [](void* context, MmsHandle source, uint32_t model, uint32_t bone, MmsEvaluationSceneBoneV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->SceneBoneAt(client, source, model, bone, out);
        });
    };
    api.evaluateFrame = [](void* context, const MmsEvaluationFrameQueryV1* query, MmsHandle* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->EvaluateFrame(client, query, out);
        });
    };
    api.describeFrame = [](void* context, MmsHandle frame, MmsEvaluationFrameInfoV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->DescribeFrame(client, frame, out);
        });
    };
    api.frameModelAt = [](void* context, MmsHandle frame, uint32_t index, MmsEvaluationSceneModelV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->FrameModelAt(client, frame, index, out);
        });
    };
    api.readBonePose = [](void* context, MmsHandle frame, uint32_t model, uint32_t bone, MmsEvaluationScenePoseV1* out) {
        return Gateway([&] {
            if (!context) return MMS_E_ARGUMENT;
            auto& client = *static_cast<Client*>(context);
            if (!client.owner) return MMS_E_ARGUMENT;
            return client.owner->ReadBonePose(client, frame, model, bone, out);
        });
    };
    return api;
}

MmsEvaluationApiV1_5 EvaluationStore::Api15(Client& client) {
    const auto base = Api14(client);
    MmsEvaluationApiV1_5 api{};
    static_assert(offsetof(MmsEvaluationApiV1_5, getMeshCapabilities) == sizeof(MmsEvaluationApiV1_4), "evaluation 1.4 prefix");
    std::memcpy(&api, &base, sizeof(base));
    api.h = MmsHeader{sizeof(api), 1, 5};
    api.getMeshCapabilities = [](void* ctx, MmsEvaluationMeshCapsV1* out) {
        return Gateway([&] { if (!ctx) return MMS_E_ARGUMENT; auto& c=*static_cast<Client*>(ctx); return c.owner ? c.owner->MeshCapabilities(c,out) : MMS_E_ARGUMENT; });
    };
    api.createMeshSceneSource = [](void* ctx,const MmsEvaluationSceneQueryV1* q,MmsHandle* out) {
        return Gateway([&] { if (!ctx) return MMS_E_ARGUMENT; auto& c=*static_cast<Client*>(ctx); return c.owner ? c.owner->CreateScene(c,q,out,true) : MMS_E_ARGUMENT; });
    };
    api.describeMesh = [](void* ctx,MmsHandle h,uint32_t m,MmsEvaluationMeshInfoV1* out) {
        return Gateway([&] { if (!ctx) return MMS_E_ARGUMENT; auto& c=*static_cast<Client*>(ctx); return c.owner ? c.owner->DescribeMesh(c,h,m,out) : MMS_E_ARGUMENT; });
    };
    api.readMeshIndices = [](void* ctx,MmsHandle h,uint32_t m,uint32_t first,uint32_t count,uint32_t* out) {
        return Gateway([&] { if (!ctx) return MMS_E_ARGUMENT; auto& c=*static_cast<Client*>(ctx); return c.owner ? c.owner->ReadMesh(c,h,m,first,count,out,0) : MMS_E_ARGUMENT; });
    };
    api.readMeshMaterials = [](void* ctx,MmsHandle h,uint32_t m,uint32_t first,uint32_t count,MmsEvaluationMeshMaterialV1* out) {
        return Gateway([&] { if (!ctx) return MMS_E_ARGUMENT; auto& c=*static_cast<Client*>(ctx); return c.owner ? c.owner->ReadMesh(c,h,m,first,count,out,1) : MMS_E_ARGUMENT; });
    };
    api.readMeshBindVertices = [](void* ctx,MmsHandle h,uint32_t m,uint32_t first,uint32_t count,MmsEvaluationMeshVertexV1* out) {
        return Gateway([&] { if (!ctx) return MMS_E_ARGUMENT; auto& c=*static_cast<Client*>(ctx); return c.owner ? c.owner->ReadMesh(c,h,m,first,count,out,2) : MMS_E_ARGUMENT; });
    };
    api.readMeshVertices = [](void* ctx,MmsHandle h,uint32_t m,uint32_t first,uint32_t count,MmsEvaluationMeshVertexV1* out) {
        return Gateway([&] { if (!ctx) return MMS_E_ARGUMENT; auto& c=*static_cast<Client*>(ctx); return c.owner ? c.owner->ReadMesh(c,h,m,first,count,out,3) : MMS_E_ARGUMENT; });
    };
    return api;
}

MmsEvaluationApiV1_6 EvaluationStore::Api16(Client& client) {
    const auto base=Api15(client); MmsEvaluationApiV1_6 api{};
    static_assert(offsetof(MmsEvaluationApiV1_6,getMaterialCapabilities)==sizeof(base), "evaluation 1.5 prefix");
    std::memcpy(&api,&base,sizeof(base)); api.h=MmsHeader{sizeof(api),1,6};
    api.getMaterialCapabilities=[](void* ctx,MmsEvaluationMaterialCapsV1* out) {
        return Gateway([&] {if(!ctx)return MMS_E_ARGUMENT;auto& c=*static_cast<Client*>(ctx);return c.owner?c.owner->MaterialCapabilities(c,out):MMS_E_ARGUMENT;});
    };
    api.createMaterialSceneSource=[](void* ctx,const MmsEvaluationSceneQueryV1* q,MmsHandle* out) {
        return Gateway([&] {if(!ctx)return MMS_E_ARGUMENT;auto& c=*static_cast<Client*>(ctx);return c.owner?c.owner->CreateScene(c,q,out,2):MMS_E_ARGUMENT;});
    };
    api.materialAt=[](void* ctx,MmsHandle h,uint32_t model,uint32_t index,MmsEvaluationMaterialIdV1* out) {
        return Gateway([&] {if(!ctx)return MMS_E_ARGUMENT;auto& c=*static_cast<Client*>(ctx);return c.owner?c.owner->MaterialAt(c,h,model,index,out):MMS_E_ARGUMENT;});
    };
    api.readFrameMaterials=[](void* ctx,MmsHandle h,uint32_t model,uint32_t first,uint32_t count,MmsEvaluationMaterialV1* out) {
        return Gateway([&] {if(!ctx)return MMS_E_ARGUMENT;auto& c=*static_cast<Client*>(ctx);return c.owner?c.owner->ReadMesh(c,h,model,first,count,out,4):MMS_E_ARGUMENT;});
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

    // A rejected callback may still have supplied a candidate. Destroy it
    // before removing the admission pin, and outside mutex_ so host cleanup
    // can take time or reenter without blocking lease inspection. Successful
    // publication already cleared this reference under the publication lock.
    data.reset();
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
    if (payload.kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    if (payload.kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    if (payload.kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    if (payload.kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    if (payload.kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    if (payload.kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    if (payload.kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
        if (candidate->kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    }
    FinishMeshRead(payload,client,0);
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
        if (candidate->kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    }
    FinishMeshRead(payload,client,0);
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
        if (candidate->kind != Payload::Kind::Channels) return MMS_E_UNSUPPORTED;
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
    }
    FinishMeshRead(payload,client,0);
    return result;
}

MmsResult EvaluationStore::SceneCapabilities(Client& client, MmsEvaluationSceneCapsV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    MmsEvaluationSceneCapsV1 caps{};
    caps.h = Header<MmsEvaluationSceneCapsV1>();
    caps.maxSourceBytes = caps.maxFrameBytes = kMaxSourceBytes;
    caps.maxClientBytes = kMaxClientBytes;
    caps.maxTotalBytes = kMaxTotalBytes;
    caps.maxWorkspaceBytes = kMaxWorkspaceBytes;
    caps.maxClientWorkspaceBytes = kMaxClientWorkspaceBytes;
    caps.maxTotalWorkspaceBytes = kMaxTotalWorkspaceBytes;
    caps.flags = captureScene_ ? MMS_EVALUATION_SCENE_SKELETON : 0;
    *out = caps;
    return MMS_OK;
}

MmsResult EvaluationStore::MeshCapabilities(Client& client, MmsEvaluationMeshCapsV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    MmsEvaluationMeshCapsV1 value{}; value.h = Header<MmsEvaluationMeshCapsV1>();
    value.flags = captureMeshScene_ ? MMS_EVALUATION_MESH_TOPOLOGY | MMS_EVALUATION_MESH_CPU_VERTICES : 0;
    value.maxItemsPerRead = kMaxMeshBatch; *out = value; return MMS_OK;
}

MmsResult EvaluationStore::MaterialCapabilities(Client& client, MmsEvaluationMaterialCapsV1* out) {
    if(!out)return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);if(client.retired)return MMS_E_NOT_READY;
    MmsEvaluationMaterialCapsV1 value{};value.h=Header<MmsEvaluationMaterialCapsV1>();
    value.flags=captureMaterialScene_ ? MMS_EVALUATION_MATERIAL_CAP_CPU_VALUES|MMS_EVALUATION_MATERIAL_CAP_PMD|MMS_EVALUATION_MATERIAL_CAP_PMX : 0;
    value.maxItemsPerRead=kMaxMeshBatch;*out=value;return MMS_OK;
}

MmsResult EvaluationStore::MaterialAt(Client& client,MmsHandle source,uint32_t model,uint32_t index,MmsEvaluationMaterialIdV1* out) {
    if(!out)return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);if(client.retired)return MMS_E_NOT_READY;
    const auto found=sources_.find(source);
    if(found==sources_.end()||found->second.owner!=&client)return MMS_E_NOT_FOUND;
    const auto& payload=*found->second.data;
    if(payload.kind!=Payload::Kind::SceneSource || !payload.sceneSource->materialIds)return MMS_E_UNSUPPORTED;
    const auto& materials=*payload.sceneSource->materialIds;
    if(model>=materials.size()||index>=materials[model].size())return MMS_E_NOT_FOUND;
    MmsEvaluationMaterialIdV1 value{};value.h=Header<MmsEvaluationMaterialIdV1>();value.material=materials[model][index];value.index=index;
    *out=value;return MMS_OK;
}

MmsResult EvaluationStore::DescribeMesh(Client& client, MmsHandle source, uint32_t model, MmsEvaluationMeshInfoV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::shared_ptr<const Payload> payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found=sources_.find(source);
        if (found==sources_.end() || found->second.owner!=&client) return MMS_E_NOT_FOUND;
        if (found->second.data->kind!=Payload::Kind::SceneSource || !found->second.data->sceneSource->meshInfo) return MMS_E_UNSUPPORTED;
        if (model>=found->second.data->sceneSource->models->size()) return MMS_E_NOT_FOUND;
        payload=found->second.data; ++client.active;
    }
    MmsEvaluationMeshInfoV1 value{};
    const auto result=Gateway([&]() -> MmsResult {
        const auto status=payload->sceneSource->meshInfo(model,value);
        if (status!=MMS_OK) return status;
        if (!ValidMeshInfo(value,(*payload->sceneSource->models)[model].object)) return MMS_E_FAILED;
        value.h=Header<MmsEvaluationMeshInfoV1>(); return MMS_OK;
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result==MMS_OK) *out=value;
    }
    FinishMeshRead(payload,client,0);
    return result;
}

MmsResult EvaluationStore::ReadMesh(Client& client, MmsHandle handle, uint32_t model,
    uint32_t first, uint32_t count, void* out, uint32_t kind) {
    if (kind>4 || count>kMaxMeshBatch || (count && !out)) return MMS_E_ARGUMENT;
    std::shared_ptr<const Payload> payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found=sources_.find(handle);
        if (found==sources_.end() || found->second.owner!=&client) return MMS_E_NOT_FOUND;
        const auto& p=found->second.data;
        if (kind==4) {
            if(p->kind!=Payload::Kind::SceneFrame || !p->sceneFrame->material || !p->sceneFrame->materialIds)return MMS_E_UNSUPPORTED;
            if(model>=p->sceneFrame->models->size() || model>=p->sceneFrame->materialIds->size())return MMS_E_NOT_FOUND;
        } else if (kind==3) {
            if (p->kind!=Payload::Kind::SceneFrame || !p->sceneFrame->meshVertex) return MMS_E_UNSUPPORTED;
            if (model>=p->sceneFrame->models->size()) return MMS_E_NOT_FOUND;
            if (model>=p->sceneFrame->meshVertexCounts.size() || model>=p->sceneFrame->meshAdditionalUvCounts.size()) return MMS_E_FAILED;
        } else {
            if (p->kind!=Payload::Kind::SceneSource || !p->sceneSource->meshInfo) return MMS_E_UNSUPPORTED;
            if (model>=p->sceneSource->models->size()) return MMS_E_NOT_FOUND;
        }
        payload=p;++client.active;
    }
    uint64_t workspace=0;
    const auto result=Gateway([&]() -> MmsResult {
        MmsEvaluationMeshInfoV1 info{};uint32_t extent=0,uvCount=0;
        if (kind==4) extent=static_cast<uint32_t>((*payload->sceneFrame->materialIds)[model].size());
        else if (kind==3) { extent=payload->sceneFrame->meshVertexCounts[model]; uvCount=payload->sceneFrame->meshAdditionalUvCounts[model]; }
        else {
            auto status=payload->sceneSource->meshInfo(model,info);
            if (status!=MMS_OK) return status;
            if (!ValidMeshInfo(info,(*payload->sceneSource->models)[model].object)) return MMS_E_FAILED;
            uvCount=info.additionalUvCount;
            extent=kind==0 ? info.indexCount : kind==1 ? info.materialCount : info.vertexCount;
        }
        if (first>extent || count>extent-first) return MMS_E_ARGUMENT;
        if (!count) return MMS_OK;
        const uint64_t stride=kind==0 ? sizeof(uint32_t) : kind==1 ? sizeof(MmsEvaluationMeshMaterialV1) : kind==4 ? sizeof(MmsEvaluationMaterialV1) : sizeof(MmsEvaluationMeshVertexV1);
        workspace=64;
        if (!AddBytes(workspace,count,stride)) return MMS_E_MEMORY;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (client.workspaceBytes>kMaxClientWorkspaceBytes-workspace || totalWorkspaceBytes_>kMaxTotalWorkspaceBytes-workspace) { workspace=0; return MMS_E_MEMORY; }
            client.workspaceBytes+=workspace;totalWorkspaceBytes_+=workspace;
        }
        auto copy=[&](auto tag) -> MmsResult {
            using T=decltype(tag);
            std::vector<T> values(count);
            if (values.capacity()>count) return MMS_E_MEMORY;
            for (uint32_t i=0;i<count;++i) {
                MmsResult status=MMS_E_FAILED;
                if constexpr (std::is_same<T,uint32_t>::value) {
                    status=payload->sceneSource->meshIndex(model,first+i,values[i]);
                    if (status==MMS_OK && values[i]>=info.vertexCount) return MMS_E_FAILED;
                } else if constexpr (std::is_same<T,MmsEvaluationMeshMaterialV1>::value) {
                    auto& value=values[i];status=payload->sceneSource->meshMaterial(model,first+i,value);
                    if (status==MMS_OK) {
                        if (value.reserved[0] || value.reserved[1] || value.firstIndex>info.indexCount || value.indexCount>info.indexCount-value.firstIndex || value.firstIndex%3 || value.indexCount%3) return MMS_E_FAILED;
                        value.h=Header<MmsEvaluationMeshMaterialV1>();
                    }
                } else if constexpr (std::is_same<T,MmsEvaluationMaterialV1>::value) {
                    auto& value=values[i];status=payload->sceneFrame->material(model,first+i,value);
                    if(status==MMS_OK) {
                        if(value.reserved[0]||value.reserved[1] || (value.flags!=MMS_EVALUATION_MATERIAL_FORMAT_PMD && value.flags!=MMS_EVALUATION_MATERIAL_FORMAT_PMX))return MMS_E_FAILED;
                        for(float n:value.nativeDiffuse)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.diffuseMirrorAndAlpha)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.specular)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.ambient)if(!std::isfinite(n))return MMS_E_FAILED;
                        if(!std::isfinite(value.specularPower)||!std::isfinite(value.edgeSize))return MMS_E_FAILED;
                        for(float n:value.edgeColor)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.textureAdd)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.textureMultiply)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.sphereAdd)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.sphereMultiply)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.toonAdd)if(!std::isfinite(n))return MMS_E_FAILED;
                        for(float n:value.toonMultiply)if(!std::isfinite(n))return MMS_E_FAILED;
                        value.h=Header<MmsEvaluationMaterialV1>();value.index=first+i;value.material=(*payload->sceneFrame->materialIds)[model][first+i];
                    }
                } else {
                    auto& value=values[i];status=kind==2 ? payload->sceneSource->meshBindVertex(model,first+i,value) : payload->sceneFrame->meshVertex(model,first+i,value);
                    if (status==MMS_OK) {
                        if (value.reserved[0] || value.reserved[1]) return MMS_E_FAILED;
                        for (float n:value.position) if (!std::isfinite(n)) return MMS_E_FAILED;
                        for (float n:value.normal) if (!std::isfinite(n)) return MMS_E_FAILED;
                        for (float n:value.uv) if (!std::isfinite(n)) return MMS_E_FAILED;
                        for (float n:value.additionalUv) if (!std::isfinite(n)) return MMS_E_FAILED;
                        for (uint32_t j=uvCount*4;j<16;++j) value.additionalUv[j]=0;
                        value.h=Header<MmsEvaluationMeshVertexV1>();
                    }
                }
                if (status!=MMS_OK) return status;
            }
            std::memcpy(out,values.data(),values.size()*sizeof(T));return MMS_OK;
        };
        if (kind==0) return copy(uint32_t{});
        if (kind==1) return copy(MmsEvaluationMeshMaterialV1{});
        if (kind==4) return copy(MmsEvaluationMaterialV1{});
        return copy(MmsEvaluationMeshVertexV1{});
    });
    FinishMeshRead(payload,client,workspace);
    return result;
}

void EvaluationStore::FinishMeshRead(std::shared_ptr<const Payload>& payload,
                                     Client& client, uint64_t workspace) noexcept {
    uint64_t finalBytes=0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Other handles or admitted reads are protected by this mutex. If a
        // reference remains, dropping ours cannot run the payload destructor.
        if (payload && payload.use_count()==1) finalBytes=payload->bytes;
        else payload.reset();
        if (!finalBytes) {
            client.workspaceBytes-=workspace;totalWorkspaceBytes_-=workspace;
            --client.active;return;
        }
    }
    // A last payload can own arbitrary host closures. Destroy it outside the
    // store mutex while retaining both the charge and the admission pin.
    payload.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client.bytes=client.bytes>=finalBytes ? client.bytes-finalBytes : 0;
        totalBytes_=totalBytes_>=finalBytes ? totalBytes_-finalBytes : 0;
        client.workspaceBytes-=workspace;totalWorkspaceBytes_-=workspace;
        --client.active;
    }
}

MmsResult EvaluationStore::CreateScene(Client& client, const MmsEvaluationSceneQueryV1* query, MmsHandle* out, uint32_t kind) {
    if (GetCurrentThreadId() != thread_) return MMS_E_THREAD;
    if (!out || !Valid(query) || kind>2) return MMS_E_ARGUMENT;
    const auto request = *query;
    if (request.flags || request.reserved || request.maxRetainedBytes > kMaxSourceBytes) return MMS_E_ARGUMENT;
    const uint64_t bound = request.maxRetainedBytes ? request.maxRetainedBytes : kMaxSourceBytes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        if (!(kind==2 ? captureMaterialScene_ : kind==1 ? captureMeshScene_ : captureScene_)) return MMS_E_UNSUPPORTED;
        if (acquiring_) return MMS_E_BUSY;
        if (client.handles >= kMaxHandlesPerClient || sources_.size() >= kMaxTotalHandles ||
            next_ >= kHandleLimit || !captureId_) return MMS_E_BUSY;
        acquiring_ = true;
        ++client.active;
    }

    std::shared_ptr<const EvaluationSceneSourceData> data;
    std::shared_ptr<Payload> payload;
    const auto result = Gateway([&]() -> MmsResult {
        const auto captured = (kind==2 ? captureMaterialScene_ : kind==1 ? captureMeshScene_ : captureScene_)(request, bound, data);
        if (captured != MMS_OK) return captured;
        if (!data || !data.use_count() || !data->models || !data->models.use_count() || !data->evaluate) return MMS_E_FAILED;
        if (kind && (!data->meshInfo || !data->meshIndex || !data->meshMaterial || !data->meshBindVertex)) return MMS_E_FAILED;
        if ((kind==2) != bool(data->materialIds)) return MMS_E_FAILED;
        uint64_t verified = sizeof(EvaluationSceneSourceData);
        const auto measured = SceneModelBytes(*data->models, verified);
        if (measured != MMS_OK) return measured;
        if (data->materialIds) {
            const auto size=SceneMaterialBytes(*data->materialIds,verified);
            if(size!=MMS_OK)return size;
            const auto valid=ValidateMaterialIds(*data->models,*data->materialIds,bound);
            if(valid!=MMS_OK)return valid;
            for(uint32_t i=0;i<data->models->size();++i) {
                MmsEvaluationMeshInfoV1 info{};const auto status=data->meshInfo(i,info);
                if(status!=MMS_OK)return status;
                if(!ValidMeshInfo(info,(*data->models)[i].object)||info.materialCount!=(*data->materialIds)[i].size())return MMS_E_FAILED;
            }
        }
        if (data->retainedBytes < verified) return MMS_E_FAILED;
        uint64_t charge = data->retainedBytes;
        // Payload owns the two empty legacy ID trees even for scene kinds.
        if (!AddBytes(charge, 1, sizeof(Payload)) ||
            !AddBytes(charge, 2, kIndexNodeBytes) || charge > bound) return MMS_E_MEMORY;
        // Reject excessive metadata before allocating duplicate-check scratch.
        const auto valid = ValidateSceneModels(*data->models, bound);
        if (valid != MMS_OK) return valid;
        payload = std::make_shared<Payload>();
        payload->kind = Payload::Kind::SceneSource;
        payload->sceneSource = data;
        payload->hostRevision = data->hostRevision;
        payload->bytes = charge;
        payload->flags = MMS_EVALUATION_SCENE_SKELETON;

        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        if (client.handles >= kMaxHandlesPerClient || sources_.size() >= kMaxTotalHandles ||
            next_ >= kHandleLimit || !captureId_) return MMS_E_BUSY;
        if (charge > kMaxClientBytes || client.bytes > kMaxClientBytes - charge ||
            charge > kMaxTotalBytes || totalBytes_ > kMaxTotalBytes - charge) return MMS_E_MEMORY;
        const auto handle = next_;
        payload->captureId = captureId_;
        if (!sources_.emplace(handle, Entry{&client, payload}).second) return MMS_E_FAILED;
        ++next_;
        ++captureId_;
        ++client.handles;
        client.bytes += charge;
        totalBytes_ += charge;
        // Publication and dropping preparation references are indivisible to
        // release/retain, so the last reference always settles its byte charge.
        payload.reset();
        data.reset();
        *out = handle;
        return MMS_OK;
    });
    // On failure host-owned candidate destructors may reenter the store. Keep
    // admission pinned but release candidates outside the mutex.
    payload.reset();
    data.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        acquiring_ = false;
        --client.active;
    }
    return result;
}

MmsResult EvaluationStore::DescribeScene(Client& client, MmsHandle source, MmsEvaluationSceneInfoV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const auto& payload = *found->second.data;
    if (payload.kind != Payload::Kind::SceneSource) return MMS_E_UNSUPPORTED;
    MmsEvaluationSceneInfoV1 value{};
    value.h = Header<MmsEvaluationSceneInfoV1>();
    value.captureId = payload.captureId;
    value.hostRevision = payload.hostRevision;
    value.retainedBytes = payload.bytes;
    value.modelCount = static_cast<uint32_t>(payload.sceneSource->models->size());
    value.flags = MMS_EVALUATION_SCENE_SKELETON;
    *out = value;
    return MMS_OK;
}

MmsResult EvaluationStore::SceneModelAt(Client& client, MmsHandle source, uint32_t index, MmsEvaluationSceneModelV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const auto& payload = *found->second.data;
    if (payload.kind != Payload::Kind::SceneSource) return MMS_E_UNSUPPORTED;
    const auto& models = *payload.sceneSource->models;
    if (index >= models.size()) return MMS_E_NOT_FOUND;
    MmsEvaluationSceneModelV1 value{};
    value.h = Header<MmsEvaluationSceneModelV1>();
    value.object = models[index].object;
    value.boneCount = static_cast<uint32_t>(models[index].bones.size());
    *out = value;
    return MMS_OK;
}

MmsResult EvaluationStore::SceneBoneAt(Client& client, MmsHandle source, uint32_t model, uint32_t bone, MmsEvaluationSceneBoneV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(source);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const auto& payload = *found->second.data;
    if (payload.kind != Payload::Kind::SceneSource) return MMS_E_UNSUPPORTED;
    const auto& models = *payload.sceneSource->models;
    if (model >= models.size() || bone >= models[model].bones.size()) return MMS_E_NOT_FOUND;
    MmsEvaluationSceneBoneV1 value{};
    value.h = Header<MmsEvaluationSceneBoneV1>();
    value.bone = models[model].bones[bone];
    value.index = bone;
    *out = value;
    return MMS_OK;
}

MmsResult EvaluationStore::EvaluateFrame(Client& client, const MmsEvaluationFrameQueryV1* query, MmsHandle* out) {
    if (!out || !Valid(query)) return MMS_E_ARGUMENT;
    const auto request = *query;
    if (request.flags || request.reserved || request.maxRetainedBytes > kMaxSourceBytes ||
        request.maxWorkspaceBytes > kMaxWorkspaceBytes) return MMS_E_ARGUMENT;
    EvaluationTime time;
    if (!EvaluationTime::FromSeconds(request.time.numerator, request.time.denominator, time)) return MMS_E_ARGUMENT;
    const auto normalized = Normalize(request.time);
    const uint64_t bound = request.maxRetainedBytes ? request.maxRetainedBytes : kMaxSourceBytes;
    const uint64_t workspace = request.maxWorkspaceBytes ? request.maxWorkspaceBytes : kMaxWorkspaceBytes;
    std::shared_ptr<const Payload> source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found = sources_.find(request.source);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        if (found->second.data->kind != Payload::Kind::SceneSource) return MMS_E_UNSUPPORTED;
        if (client.handles >= kMaxHandlesPerClient || sources_.size() >= kMaxTotalHandles ||
            next_ >= kHandleLimit || !captureId_) return MMS_E_BUSY;
        if (client.workspaceBytes > kMaxClientWorkspaceBytes - workspace ||
            totalWorkspaceBytes_ > kMaxTotalWorkspaceBytes - workspace) return MMS_E_MEMORY;
        source = found->second.data;
        client.workspaceBytes += workspace;
        totalWorkspaceBytes_ += workspace;
        ++client.active;
    }

    std::shared_ptr<const EvaluationSceneFrameData> data;
    std::shared_ptr<Payload> payload;
    const auto result = Gateway([&]() -> MmsResult {
        const auto evaluated = source->sceneSource->evaluate(time, bound, workspace, data);
        if (evaluated != MMS_OK) return evaluated;
        if (!data || !data.use_count() || !data->models || !data->models.use_count() || !data->sample) return MMS_E_FAILED;
        if (source->sceneSource->meshInfo && (!data->meshVertex || data->meshVertexCounts.size() != data->models->size() || data->meshAdditionalUvCounts.size() != data->models->size())) return MMS_E_FAILED;
        if (!source->sceneSource->meshInfo && (data->meshVertex || !data->meshVertexCounts.empty() || !data->meshAdditionalUvCounts.empty())) return MMS_E_FAILED;
        const auto& materialIds=source->sceneSource->materialIds;
        if(materialIds) {
            if(!data->material || !data->materialIds || data->materialIds.get()!=materialIds.get() ||
               data->materialIds.owner_before(materialIds) || materialIds.owner_before(data->materialIds))return MMS_E_FAILED;
        } else if(data->material || data->materialIds) return MMS_E_FAILED;
        const auto& identities = source->sceneSource->models;
        // Matching contents alone cannot establish the same frozen capture.
        // Check both address and shared ownership, without copying that lease.
        if (data->models.get() != identities.get() || data->models.owner_before(identities) ||
            identities.owner_before(data->models)) return MMS_E_FAILED;
        if (source->sceneSource->meshInfo) {
            for (uint32_t i=0;i<identities->size();++i) {
                MmsEvaluationMeshInfoV1 info{};
                const auto status=source->sceneSource->meshInfo(i,info);
                if (status!=MMS_OK) return status;
                if (!ValidMeshInfo(info,(*identities)[i].object) || info.vertexCount!=data->meshVertexCounts[i] || info.additionalUvCount!=data->meshAdditionalUvCounts[i]) return MMS_E_FAILED;
            }
        }
        if (data->visibility.size() != identities->size()) return MMS_E_FAILED;
        uint64_t verified = sizeof(EvaluationSceneFrameData);
        const auto measured = SceneModelBytes(*data->models, verified);
        if (measured != MMS_OK) return measured;
        if (!AddBytes(verified, data->visibility.capacity(), sizeof(uint8_t))) return MMS_E_MEMORY;
        if (!AddBytes(verified, data->meshVertexCounts.capacity(), sizeof(uint32_t))) return MMS_E_MEMORY;
        if (!AddBytes(verified, data->meshAdditionalUvCounts.capacity(), sizeof(uint32_t))) return MMS_E_MEMORY;
        if (data->materialIds) {
            const auto size=SceneMaterialBytes(*data->materialIds,verified);if(size!=MMS_OK)return size;
        }
        if (data->retainedBytes < verified) return MMS_E_FAILED;
        uint64_t charge = data->retainedBytes;
        if (!AddBytes(charge, 1, sizeof(Payload)) || !AddBytes(charge, 2, kIndexNodeBytes) ||
            charge > bound || charge > workspace) return MMS_E_MEMORY;
        payload = std::make_shared<Payload>();
        payload->kind = Payload::Kind::SceneFrame;
        payload->sceneFrame = data;
        payload->captureId = source->captureId;
        payload->hostRevision = source->hostRevision;
        payload->bytes = charge;
        payload->time = normalized;
        payload->flags = MMS_EVALUATION_SCENE_SKELETON;

        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        if (client.handles >= kMaxHandlesPerClient || sources_.size() >= kMaxTotalHandles ||
            next_ >= kHandleLimit || !captureId_) return MMS_E_BUSY;
        if (charge > kMaxClientBytes || client.bytes > kMaxClientBytes - charge ||
            charge > kMaxTotalBytes || totalBytes_ > kMaxTotalBytes - charge) return MMS_E_MEMORY;
        const auto handle = next_;
        payload->frameId = captureId_;
        if (!sources_.emplace(handle, Entry{&client, payload}).second) return MMS_E_FAILED;
        ++next_;
        ++captureId_;
        ++client.handles;
        client.bytes += charge;
        totalBytes_ += charge;
        payload.reset();
        data.reset();
        *out = handle;
        return MMS_OK;
    });
    // Unpublished candidates can own host closures. Destroy them outside the
    // mutex while the source, admission pin and workspace remain charged.
    payload.reset();
    data.reset();
    FinishMeshRead(source,client,workspace);
    return result;
}

MmsResult EvaluationStore::DescribeFrame(Client& client, MmsHandle frame, MmsEvaluationFrameInfoV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(frame);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const auto& payload = *found->second.data;
    if (payload.kind != Payload::Kind::SceneFrame) return MMS_E_UNSUPPORTED;
    MmsEvaluationFrameInfoV1 value{};
    value.h = Header<MmsEvaluationFrameInfoV1>();
    value.captureId = payload.captureId;
    value.frameId = payload.frameId;
    value.hostRevision = payload.hostRevision;
    value.retainedBytes = payload.bytes;
    value.time = payload.time;
    value.modelCount = static_cast<uint32_t>(payload.sceneFrame->models->size());
    value.flags = MMS_EVALUATION_SCENE_SKELETON;
    *out = value;
    return MMS_OK;
}

MmsResult EvaluationStore::FrameModelAt(Client& client, MmsHandle frame, uint32_t index, MmsEvaluationSceneModelV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::lock_guard<std::mutex> lock(mutex_);
    if (client.retired) return MMS_E_NOT_READY;
    const auto found = sources_.find(frame);
    if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
    const auto& payload = *found->second.data;
    if (payload.kind != Payload::Kind::SceneFrame) return MMS_E_UNSUPPORTED;
    const auto& models = *payload.sceneFrame->models;
    if (index >= models.size()) return MMS_E_NOT_FOUND;
    MmsEvaluationSceneModelV1 value{};
    value.h = Header<MmsEvaluationSceneModelV1>();
    value.object = models[index].object;
    value.boneCount = static_cast<uint32_t>(models[index].bones.size());
    value.flags = MMS_EVALUATION_SCENE_VISIBILITY_VALID;
    value.visibility = payload.sceneFrame->visibility[index];
    *out = value;
    return MMS_OK;
}

MmsResult EvaluationStore::ReadBonePose(Client& client, MmsHandle frame, uint32_t model, uint32_t bone,
    MmsEvaluationScenePoseV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    std::shared_ptr<const Payload> payload;
    MmsId identity{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client.retired) return MMS_E_NOT_READY;
        const auto found = sources_.find(frame);
        if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
        const auto& candidate = found->second.data;
        if (candidate->kind != Payload::Kind::SceneFrame) return MMS_E_UNSUPPORTED;
        const auto& models = *candidate->sceneFrame->models;
        if (model >= models.size() || bone >= models[model].bones.size()) return MMS_E_NOT_FOUND;
        identity = models[model].bones[bone];
        payload = candidate;
        ++client.active;
    }
    MmsEvaluationScenePoseV1 value{};
    const auto result = Gateway([&]() -> MmsResult {
        const auto sampled = payload->sceneFrame->sample(model, bone, value);
        if (sampled != MMS_OK) return sampled;
        if (value.reserved) return MMS_E_FAILED;
        for (float n : value.rigid) if (!std::isfinite(n)) return MMS_E_FAILED;
        for (float n : value.scaled) if (!std::isfinite(n)) return MMS_E_FAILED;
        for (float n : value.attachmentBefore) if (!std::isfinite(n)) return MMS_E_FAILED;
        for (float n : value.attachmentAfter) if (!std::isfinite(n)) return MMS_E_FAILED;
        for (float n : value.jointPosition) if (!std::isfinite(n)) return MMS_E_FAILED;
        value.h = Header<MmsEvaluationScenePoseV1>();
        value.bone = identity;
        return MMS_OK;
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result == MMS_OK) *out = value;
    }
    FinishMeshRead(payload,client,0);
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
        std::shared_ptr<const Payload> payload;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Release remains available after Retire. Keep an admission pin
            // until a final host-owned payload has finished destruction.
            const auto found = sources_.find(source);
            if (found == sources_.end() || found->second.owner != &client) return MMS_E_NOT_FOUND;
            payload = found->second.data;
            sources_.erase(found);
            if (client.handles) --client.handles;
            ++client.active;
        }
        FinishMeshRead(payload, client, 0);
        return MMS_OK;
    });
}

}
