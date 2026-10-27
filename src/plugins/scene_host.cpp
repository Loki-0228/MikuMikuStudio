#include "scene_identity.hpp"
#include "scene_geometry.hpp"
#include "scene_definitions.hpp"
#include "scene_history.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_layout.hpp"
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/text_encoding.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "native_pose_scope.hpp"
#include "evaluation_store.hpp"
#include "mikumikustudio/bone_motion_snapshot.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/morph_definition_snapshot.hpp"
#include "mikumikustudio/display_motion_snapshot.hpp"
#include "mikumikustudio/bone_scene_evaluation.hpp"
#include "mikumikustudio/bone_transform_context.hpp"
#endif
#include <algorithm>
#include <array>
#include <new>
#include <set>

namespace mikumikustudio::plugins {
namespace {
constexpr char identityKey[] = "org.mikumikustudio.plugin.objectIdentity.v1";
SceneIdentity identities;
uint64_t revision = 1;
std::vector<unsigned char> previous;
bool opaqueIdentity = false;
std::map<const void*, GeometryDigests> geometryDigests;
void Identity(float* matrix) { std::fill(matrix, matrix + 16, 0.f); matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1; }
template<class T> void Append(std::vector<unsigned char>& bytes, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "identity scalar");
    const auto* first = reinterpret_cast<const unsigned char*>(&value); bytes.insert(bytes.end(), first, first + sizeof(value));
}
void Append(std::vector<unsigned char>& bytes, MmsString text) {
    Append(bytes, text.bytes); bytes.insert(bytes.end(), text.data, text.data + text.bytes);
}
struct Reader {
    const std::vector<uint8_t>& bytes; size_t offset = 0;
    template<class T> T Get() {
        if (bytes.size() - offset < sizeof(T)) throw std::invalid_argument("truncated identity record");
        T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); offset += sizeof(value); return value;
    }
};
const void* Token(const MMDApp& app, uint32_t kind, uint32_t slot) {
    if (slot >= 255 || (kind != MMS_OBJECT_MODEL && kind != MMS_OBJECT_ACCESSORY)) throw std::invalid_argument("identity object slot");
    return kind == MMS_OBJECT_MODEL ? static_cast<const void*>(app.ModelSlot(slot)) : app.AccessorySlot(slot);
}
void EnsureBones(const void* token, uint32_t kind) {
    identities.Get(token);
    if (kind == MMS_OBJECT_MODEL) {
        const auto* model = mdl::Mdl(static_cast<const unsigned char*>(token));
        for (uint32_t i = 0; i < model->boneCount; ++i) identities.Element(token, MMS_CHANNEL_BONES, i);
    }
}
MmsResult Capture(MMDApp* app, const MmsSceneQueryV1& query, std::shared_ptr<const SceneData>& out) {
    if (!app || app->SceneMutationInProgress() || !app->WindowLayoutReady()) return MMS_E_NOT_READY;
    // Each additional stage is published separately after its actual pipeline
    // boundary is captured. Bind data never substitutes for an evaluated pose.
    constexpr auto supported = bindGeometryChannels | bindDefinitionChannels | MMS_CHANNEL_BONES;
    if (query.stage != MMS_STAGE_BIND || (query.channels & ~supported)) return MMS_E_UNSUPPORTED;
    auto data = std::make_shared<SceneData>();
    data->time = Normalize({app->CurrentFrame(), 30}); data->supported = supported;
    if (!(query.flags & MMS_SCENE_CURRENT_TIME)) {
        const auto requestedTime = Normalize(query.time);
        if (requestedTime.numerator != data->time.numerator || requestedTime.denominator != data->time.denominator) return MMS_E_NOT_READY;
    }
    auto nextDigests = geometryDigests; bool geometryChanged = false;
    std::vector<unsigned char> fingerprint;
    for (uint32_t kind : {MMS_OBJECT_MODEL, MMS_OBJECT_ACCESSORY}) for (uint32_t slot = 0; slot < 255; ++slot) {
        const void* token = Token(*app, kind, slot); if (!token) continue;
        EnsureBones(token, kind);
        SceneData::Channels record;
        auto& object = record.object; object.h = Header<MmsObjectV1>(); object.object = identities.Get(token).object;
        object.kind = kind; Identity(object.world);
        std::vector<MmsBoneV1> bones;
        if (kind == MMS_OBJECT_MODEL) {
            const auto& model = *mdl::Mdl(static_cast<const unsigned char*>(token));
            object.name = data->Keep(Utf8(text_encoding::ModelName(model, false)));
            object.sourcePath = data->Keep(Utf8(model.path)); object.visible = model.loadComplete != 0;
            if (model.boneCount && !model.boneTable) return MMS_E_NOT_READY;
            bones.resize(model.boneCount);
            for (uint32_t i = 0; i < model.boneCount; ++i) {
                const auto& source = model.boneTable[i]; auto& bone = bones[i]; bone.h = Header<MmsBoneV1>();
                bone.id = bone.sourceId = identities.Element(token, MMS_CHANNEL_BONES, i);
                bone.name = data->Keep(Utf8(text_encoding::BoneName(source, false))); bone.flags = source.flags;
                for (auto* matrix : {bone.local, bone.world, bone.bindWorld, bone.inverseBindWorld}) Identity(matrix);
                const bool parent = source.parent >= 0 && uint32_t(source.parent) < model.boneCount;
                if (parent) bone.parentId = identities.Element(token, MMS_CHANNEL_BONES, uint32_t(source.parent));
                for (unsigned axis = 0; axis < 3; ++axis) {
                    bone.world[12 + axis] = bone.bindWorld[12 + axis] = source.position[axis];
                    bone.inverseBindWorld[12 + axis] = -source.position[axis];
                    bone.local[12 + axis] = source.position[axis] - (parent ? model.boneTable[source.parent].position[axis] : 0);
                }
                Append(fingerprint, bone.id); Append(fingerprint, bone.parentId); Append(fingerprint, bone.name);
                Append(fingerprint, bone.flags); Append(fingerprint, bone.bindWorld);
            }
        } else {
            const auto& accessory = *static_cast<const mdl::AccessoryRecord*>(token);
            object.sourcePath = data->Keep(Utf8(accessory.sourcePath));
            if (!std::memchr(accessory.name, 0, sizeof accessory.name)) return MMS_E_NOT_READY;
            const auto label = accessory.name[0] ? text_encoding::Display(accessory.name) : fs::path(accessory.sourcePath).stem().native();
            if (accessory.name[0] && label.empty()) return MMS_E_NOT_READY;
            object.name = data->Keep(Utf8(label)); object.visible = accessory.visible;
            // Stage zero is source binding space. Scene attachment transforms
            // belong to the evaluated stages and are not read from live bones.
        }
        Append(fingerprint, object.object); Append(fingerprint, kind); Append(fingerprint, object.visible);
        Append(fingerprint, object.name); Append(fingerprint, object.sourcePath);
        record.values.emplace(MMS_CHANNEL_BONES, data->Keep(std::move(bones)));
        bool selected = query.objects.count == 0;
        for (size_t i = 0; !selected && i < query.objects.count; ++i) {
            MmsObjectRef requested;
            std::memcpy(&requested, static_cast<const unsigned char*>(query.objects.data) + i * query.objects.stride, sizeof(requested));
            selected = Same(requested, object.object);
        }
        if (selected) {
            GeometryDigests current;
            const auto geometry = CaptureBindGeometry(identities, *data, record, token, kind, query.channels, current);
            if (geometry != MMS_OK) return geometry;
            const auto definitions = CaptureBindDefinitions(identities, *data, record, token, kind, query.channels, current);
            if (definitions != MMS_OK) return definitions;
            auto& baseline = nextDigests[token];
            for (const auto& entry : current) {
                const auto found = baseline.find(entry.first);
                if (found != baseline.end() && found->second != entry.second) geometryChanged = true;
                baseline[entry.first] = entry.second;
            }
        }
        data->objects.push_back(std::move(record));
    }
    if (fingerprint != previous || geometryChanged) { ++revision; previous = std::move(fingerprint); }
    geometryDigests.swap(nextDigests);
    data->revision = data->structure = revision; data->pose = 1;
#if !MMD_ORIGINAL_PROFILE
    // A controlled editing host must publish the same revision that begin()
    // validates. Bind-geometry revisions remain separate from edit history.
    if (auto* history = BoundNativePoseHistory(app)) {
        if (auto result = history->Revision(data->revision)) return result;
    }
#endif
    for (auto& record : data->objects) {
        record.object.structureRevision = revision; record.object.meshRevision = revision;
        record.object.materialRevision = revision; record.object.poseRevision = 1;
    }
    out = std::move(data); return MMS_OK;
}
#if !MMD_ORIGINAL_PROFILE
void ChargeSceneEvaluation(uint64_t& bytes, uint64_t count, uint64_t stride, uint64_t limit) {
    if (bytes > limit || (stride && count > (limit - bytes) / stride))
        throw std::length_error("scene evaluation host budget");
    bytes += count * stride;
}
bool SameSceneIdentity(const BoneSceneIdentity& internal, const MmsObjectRef& object) noexcept {
    return internal.lo == object.id.lo && internal.hi == object.id.hi &&
        internal.generation == object.generation;
}
MmsResult CaptureEvaluationScene(MMDApp* app, uint64_t limit,
    std::shared_ptr<const EvaluationSceneSourceData>& out) {
    if (!app || app->SceneMutationInProgress() || !app->WindowLayoutReady()) return MMS_E_NOT_READY;
    uint64_t hostRevision = 0;
    if (auto* history = BoundNativePoseHistory(app)) {
        if (auto status = history->CheckNativeNavigation()) return status;
        if (auto status = history->Revision(hostRevision)) return status;
    }
    // Includes source closure/control blocks and the private math context's
    // object, implementation and DLL lease bookkeeping. The shared OS DLL
    // image itself is not a per-source allocation or part of retained storage.
    constexpr uint64_t sourceOverhead = sizeof(EvaluationSceneSourceData) + 1024;
    uint64_t bytes = 0;
    ChargeSceneEvaluation(bytes, 1, sourceOverhead, limit);
    uint64_t identityBytes = sizeof(EvaluationSceneModels) + 256;
    ChargeSceneEvaluation(bytes, 1, identityBytes, limit);
    uint64_t morphDefinitionMinimum = 0;
    uint32_t count = 0;
    // Preflight all public metadata before allocating model or bone vectors.
    // Slot scans include invisible models and future-only dependency targets.
    for (uint32_t slot = 0; slot < kModelSlotCount; ++slot) {
        const auto* raw = app->ModelSlot(slot); if (!raw) continue;
        const auto* known = identities.Retained(raw);
        if (known && (!known->present || !known->published)) return MMS_E_STALE;
        const auto& model = *mdl::Mdl(raw);
        if (model.boneCount && !model.boneTable) return MMS_E_NOT_READY;
        if (model.morphCount > INT32_MAX || (model.morphCount && !model.morphs)) return MMS_E_STALE;
        ChargeSceneEvaluation(bytes, 1, sizeof(EvaluationSceneModelData), limit);
        ChargeSceneEvaluation(bytes, model.boneCount, sizeof(MmsId), limit);
        // Check a retained-data lower bound before scanning headers. The
        // actual snapshot later owns and charges these records, so this
        // temporary bound must not also enter the wrapper's retained bytes.
        ChargeSceneEvaluation(morphDefinitionMinimum, model.morphCount, sizeof(MorphDefinition), limit - bytes);
        for (uint32_t morph = 0; morph < model.morphCount; ++morph) {
            const auto& definition = model.morphs[morph];
            if ((model.physicsMode == 2 && definition.type > 8) ||
                definition.impulseEntries || definition.impulse2Entries) return MMS_E_UNSUPPORTED;
        }
        ++count;
    }
    auto models = std::make_shared<EvaluationSceneModels>();
    models->resize(count);
    ChargeSceneEvaluation(bytes, models->capacity() - count, sizeof(EvaluationSceneModelData), limit);
    // The input list is temporary, bounded by the host's fixed slot table;
    // neither it nor its live pointers enter a published source or callback.
    std::array<BoneSceneModelInput, kModelSlotCount> inputs{};
    uint32_t index = 0;
    for (uint32_t slot = 0; slot < kModelSlotCount; ++slot) {
        const auto* raw = app->ModelSlot(slot); if (!raw) continue;
        const auto& model = *mdl::Mdl(raw);
        auto& record = (*models)[index];
        record.object = identities.Get(raw).object;
        record.bones.resize(model.boneCount);
        ChargeSceneEvaluation(bytes, record.bones.capacity() - model.boneCount, sizeof(MmsId), limit);
        for (uint32_t bone = 0; bone < model.boneCount; ++bone)
            record.bones[bone] = identities.Element(raw, MMS_CHANNEL_BONES, bone);
        inputs[index++] = {slot, {record.object.id.lo, record.object.id.hi, record.object.generation}, raw};
    }
    // Everything charged so far except the source wrapper belongs to the
    // immutable identity map. Frames pay this full amount independently.
    identityBytes = bytes - sourceOverhead;
    auto math = BoneTransformContext::Create();
    if (!math) return MMS_E_NOT_READY;
    std::shared_ptr<const BoneSceneSource> source;
    try {
        source = BoneSceneSource::Capture(inputs.data(), count, limit - bytes);
    } catch (const std::invalid_argument&) {
        // The plugin supplies only budgets; invalid frozen model definitions
        // or parent targets describe stale host data, not malformed arguments.
        return MMS_E_STALE;
    }
    if (!source || source->ModelCount() != count) return MMS_E_FAILED;
    ChargeSceneEvaluation(bytes, 1, source->RetainedBytes(), limit);
    for (uint32_t i = 0; i < count; ++i) {
        BoneSceneModelInfo info{};
        if (!source->ModelAt(i, info) || info.slot != inputs[i].slot ||
            !SameSceneIdentity(info.identity, (*models)[i].object) ||
            info.boneCount != (*models)[i].bones.size()) return MMS_E_FAILED;
    }
    auto data = std::make_shared<EvaluationSceneSourceData>();
    data->models = std::move(models);
    data->hostRevision = hostRevision;
    data->retainedBytes = bytes;
    data->evaluate = [source = std::move(source), math = std::move(math),
        models = data->models, identityBytes](const EvaluationTime& time, uint64_t frameLimit,
        uint64_t workspaceLimit, std::shared_ptr<const EvaluationSceneFrameData>& result) -> MmsResult {
        // Candidate frame metadata and shared identity storage stay covered
        // while the internal evaluator uses the remainder of the reservation.
        // Nested solver scratch remains subject to its documented limitations.
        uint64_t wrapperBytes = 0;
        const auto bound = (std::min)(frameLimit, workspaceLimit);
        ChargeSceneEvaluation(wrapperBytes, 1, sizeof(EvaluationSceneFrameData) + 512, bound);
        ChargeSceneEvaluation(wrapperBytes, 1, identityBytes, bound);
        ChargeSceneEvaluation(wrapperBytes, models->size(), sizeof(uint8_t), bound);
        auto candidate = std::make_shared<EvaluationSceneFrameData>();
        candidate->models = models;
        candidate->visibility.resize(models->size());
        ChargeSceneEvaluation(wrapperBytes, candidate->visibility.capacity() - models->size(), sizeof(uint8_t), bound);
        std::shared_ptr<const BoneSceneFrame> frame = source->EvaluateAt(time, *math,
            frameLimit - wrapperBytes, workspaceLimit - wrapperBytes);
        if (!frame || frame->ModelCount() != models->size() || frame->Frame() != time.Frame()) return MMS_E_FAILED;
        uint64_t frameBytes = wrapperBytes;
        ChargeSceneEvaluation(frameBytes, 1, frame->RetainedBytes(), frameLimit);
        for (uint32_t model = 0; model < frame->ModelCount(); ++model) {
            BoneSceneModelInfo info{}, captured{}; const auto* child = frame->ModelFrame(model);
            if (!source->ModelAt(model, captured) || !frame->ModelAt(model, info) || info.slot != captured.slot ||
                !SameSceneIdentity(info.identity, (*models)[model].object) ||
                info.boneCount != (*models)[model].bones.size() || !child ||
                child->BoneCount() != info.boneCount || child->Frame() != time.Frame()) return MMS_E_FAILED;
            candidate->visibility[model] = child->Visibility();
        }
        candidate->retainedBytes = frameBytes;
        // This closure deliberately retains only the independent frame, never
        // the source, its accounting Payload, the math context or live host.
        candidate->sample = [frame = std::move(frame)](uint32_t model, uint32_t bone,
            MmsEvaluationScenePoseV1& out) -> MmsResult {
            const auto* child = frame->ModelFrame(model);
            BoneEvaluationPose pose;
            if (!child || !child->BoneAt(bone, pose)) return MMS_E_NOT_FOUND;
            MmsEvaluationScenePoseV1 value{}; value.h = Header<MmsEvaluationScenePoseV1>();
            std::copy(pose.rigid.begin(), pose.rigid.end(), value.rigid);
            std::copy(pose.scaled.begin(), pose.scaled.end(), value.scaled);
            std::copy(pose.attachment[0].begin(), pose.attachment[0].end(), value.attachmentBefore);
            std::copy(pose.attachment[1].begin(), pose.attachment[1].end(), value.attachmentAfter);
            std::copy(pose.jointPosition.begin(), pose.jointPosition.end(), value.jointPosition);
            // The store fills the public bone ID from the same frozen map.
            out = value; return MMS_OK;
        };
        result = std::move(candidate); return MMS_OK;
    };
    out = std::move(data); return MMS_OK;
}
#endif
}
std::shared_ptr<SceneStore> CreateSceneStore(MMDApp* app) {
    return std::make_shared<SceneStore>([app](const auto& query, auto& out) { return Capture(app, query, out); },
        [](MmsId id) { return identities.Known(id); });
}
std::shared_ptr<EvaluationStore> CreateEvaluationStore(MMDApp* app) {
#if MMD_ORIGINAL_PROFILE
    return {};
#else
    return std::make_shared<EvaluationStore>([app](const MmsEvaluationSourceQueryV1& query,
        uint32_t channels, uint64_t limit, std::shared_ptr<const EvaluationData>& out) -> MmsResult {
        if (!app || app->SceneMutationInProgress() || !app->WindowLayoutReady()) return MMS_E_NOT_READY;
        uint64_t hostRevision = 0;
        if (auto* history = BoundNativePoseHistory(app)) {
            if (auto status = history->CheckNativeNavigation()) return status;
            if (auto status = history->Revision(hostRevision)) return status;
        }
        const unsigned char* source = nullptr;
        for (uint32_t kind : {MMS_OBJECT_MODEL, MMS_OBJECT_ACCESSORY}) for (uint32_t slot = 0; slot < 255; ++slot) {
            const auto* token = Token(*app, kind, slot); if (!token) continue;
            const auto* entry = identities.Retained(token);
            if (!entry || !entry->present || !entry->published || !Same(entry->object.id, query.object.id)) continue;
            if (!Same(entry->object, query.object)) return MMS_E_STALE;
            if (kind != MMS_OBJECT_MODEL) return MMS_E_UNSUPPORTED;
            source = static_cast<const unsigned char*>(token);
        }
        if (!source) return identities.Known(query.object.id) ? MMS_E_STALE : MMS_E_NOT_FOUND;
        const auto& model = *mdl::Mdl(source);
        const bool bones = (channels & MMS_EVALUATION_CAP_BONE_CHANNELS) != 0;
        const bool morphs = (channels & MMS_EVALUATION_CAP_MORPH_WEIGHTS) != 0;
        if (bones && model.boneCount && !model.boneTable) return MMS_E_NOT_READY;
        if (morphs && model.morphCount && !model.morphs) return MMS_E_NOT_READY;
        // The callback closure and shared ownership blocks receive conservative
        // allowances. The store separately charges its payload and ID index.
        uint64_t bytes = sizeof(EvaluationData) + 256;
        if (bytes > limit) return MMS_E_MEMORY;
        auto data = std::make_shared<EvaluationData>();
        data->object = query.object; data->hostRevision = hostRevision; data->channels = channels;
        if (bones) {
            if (model.boneCount > (limit - bytes) / sizeof(MmsEvaluationBoneV1)) return MMS_E_MEMORY;
            data->bones.resize(model.boneCount);
            if (data->bones.capacity() > (limit - bytes) / sizeof(MmsEvaluationBoneV1)) return MMS_E_MEMORY;
            bytes += data->bones.capacity() * sizeof(MmsEvaluationBoneV1);
        }
        if (morphs) {
            if (model.morphCount > (limit - bytes) / sizeof(MmsEvaluationMorphV1)) return MMS_E_MEMORY;
            data->morphs.resize(model.morphCount);
            if (data->morphs.capacity() > (limit - bytes) / sizeof(MmsEvaluationMorphV1)) return MMS_E_MEMORY;
            bytes += data->morphs.capacity() * sizeof(MmsEvaluationMorphV1);
        }
        if (bones) {
            std::shared_ptr<BoneMotionSnapshot> motion;
            try { motion = BoneMotionSnapshot::Capture(source, limit - bytes); }
            catch (const std::length_error&) { return MMS_E_MEMORY; }
            if (motion->RetainedBytes() > limit - bytes) return MMS_E_MEMORY;
            bytes += motion->RetainedBytes();
            for (unsigned i = 0; i < model.boneCount; ++i) {
                const auto& bone = model.boneTable[i]; auto& target = data->bones[i];
                target.h = Header<MmsEvaluationBoneV1>();
                target.id = identities.Element(source, MMS_CHANNEL_BONES, i);
                if (bone.slotIndex >= 0) target.flags |= MMS_EVALUATION_BONE_EXTERNAL_PARENT;
                if (unsigned(bone.type) > unsigned(mdl::BoneType::Effector) && bone.type != mdl::BoneType::FixedAxis)
                    target.flags |= MMS_EVALUATION_BONE_DERIVED;
                if (!target.flags) target.flags = MMS_EVALUATION_BONE_LOCAL;
            }
            data->sample = [motion = std::move(motion)](uint32_t bone, double frame, MmsBoneChannelsV1& out) {
                BoneMotionSample sample;
                switch (motion->SampleChannels(int(bone), frame, sample)) {
                case BoneMotionStatus::InvalidBone: return MMS_E_NOT_FOUND;
                case BoneMotionStatus::InvalidTime: return MMS_E_ARGUMENT;
                case BoneMotionStatus::ExternalParent:
                case BoneMotionStatus::UnsupportedBone: return MMS_E_UNSUPPORTED;
                case BoneMotionStatus::InvalidSample: return MMS_E_FAILED;
                case BoneMotionStatus::Ok: break;
                }
                MmsBoneChannelsV1 value{}; value.h = Header<MmsBoneChannelsV1>();
                std::copy(sample.position.begin(), sample.position.end(), value.position);
                std::copy(sample.rotation.begin(), sample.rotation.end(), value.rotation);
                std::copy(sample.scale.v.begin(), sample.scale.v.end(), value.scale);
                value.flags = sample.physicsDisabled ? MMS_EVALUATION_PHYSICS_DISABLED : 0;
                out = value; return MMS_OK;
            };
        }
        if (morphs) {
            std::shared_ptr<morph_curve::SamplingState> motion = morph_curve::SamplingState::Capture(source, limit - bytes);
            if (motion->RetainedBytes() > limit - bytes) return MMS_E_MEMORY;
            bytes += motion->RetainedBytes();
            for (unsigned i = 0; i < model.morphCount; ++i) {
                const auto& morph = model.morphs[i]; auto& target = data->morphs[i];
                target.h = Header<MmsEvaluationMorphV1>();
                target.id = identities.Element(source, MMS_CHANNEL_MORPHS, i);
                target.kind = model.physicsMode == 2 ? morph.type : MMS_MORPH_VERTEX;
                target.flags = uint32_t(morph.panel) << MMS_MORPH_PANEL_SHIFT;
                if (model.physicsMode != 2 && !i) target.flags |= MMS_MORPH_BASE;
            }
            data->sampleMorph = [motion = std::move(motion)](uint32_t morph, double frame, MmsMorphWeightV1& out) {
                float weight = 0;
                if (!motion->Sample(int(morph), frame, weight)) return MMS_E_FAILED;
                out = {Header<MmsMorphWeightV1>(), weight, 0}; return MMS_OK;
            };
        }
        if (channels & MMS_EVALUATION_CAP_DISPLAY) {
            std::shared_ptr<DisplayMotionSnapshot> motion = DisplayMotionSnapshot::Capture(source, limit - bytes);
            if (motion->RetainedBytes() > limit - bytes) return MMS_E_MEMORY;
            bytes += motion->RetainedBytes();
            const auto charge = [&](uint64_t count, uint64_t stride) {
                if (count > (limit - bytes) / stride) throw std::length_error("display evaluation source budget");
                bytes += count * stride;
            };
            charge(motion->IkCount(), sizeof(MmsEvaluationIkV1));
            charge(motion->SelectorCount(), sizeof(MmsEvaluationSelectorV1));
            data->ik.resize(motion->IkCount()); data->selectors.resize(motion->SelectorCount());
            charge(data->ik.capacity() - data->ik.size(), sizeof(MmsEvaluationIkV1));
            charge(data->selectors.capacity() - data->selectors.size(), sizeof(MmsEvaluationSelectorV1));
            for (uint32_t i = 0; i < motion->IkCount(); ++i) {
                int32_t bone = -1;
                if (!motion->IkBone(i, bone)) return MMS_E_FAILED;
                data->ik[i] = {Header<MmsEvaluationIkV1>(), identities.Element(source, MMS_CHANNEL_BONES, uint32_t(bone))};
            }
            for (uint32_t i = 0; i < motion->SelectorCount(); ++i) {
                int32_t bone = -1;
                if (!motion->SelectorBone(i, bone)) return MMS_E_FAILED;
                auto& target = data->selectors[i]; target.h = Header<MmsEvaluationSelectorV1>();
                if (bone == -1) target.flags = MMS_EVALUATION_SELECTOR_GLOBAL;
                else target.bone = identities.Element(source, MMS_CHANNEL_BONES, uint32_t(bone));
            }
            struct Target { MmsObjectRef object; MmsId bone; };
            using Targets = std::map<std::pair<int32_t, int32_t>, Target>;
            // Map tree nodes, its sentinel and the shared ownership/closures
            // are conservatively charged into the same source, not a new lease.
            constexpr auto nodeBytes = sizeof(Targets::value_type) + 4 * sizeof(void*);
            charge(1, sizeof(Targets) + nodeBytes + 256);
            auto targets = std::make_shared<Targets>();
            for (uint32_t key = 0; key < motion->KeyCount(); ++key) {
                uint32_t frame = 0;
                if (!motion->KeyFrame(key, frame)) return MMS_E_FAILED;
                for (uint32_t i = 0; i < motion->SelectorCount(); ++i) {
                    DisplaySelectorSample parent;
                    if (motion->SampleSelector(int(i), frame, parent) != DisplayMotionStatus::Ok) return MMS_E_FAILED;
                    // Disabled references (-1) and native ground (-2) are
                    // parent states, not target references. Both still retain
                    // the original window; only other negative slots are stale.
                    if (parent.modelIndex == -1 || parent.modelIndex == -2) continue;
                    const auto pair = std::make_pair(parent.modelIndex, parent.boneIndex);
                    if (targets->find(pair) != targets->end()) continue;
                    if (parent.modelIndex < 0 || parent.modelIndex >= kModelSlotCount || parent.boneIndex < 0) return MMS_E_STALE;
                    const auto* targetModel = app->ModelSlot(parent.modelIndex);
                    if (!targetModel) return MMS_E_STALE;
                    const auto* identity = identities.Retained(targetModel);
                    if (!identity || !identity->present || !identity->published) return MMS_E_STALE;
                    const auto& targetRecord = *mdl::Mdl(targetModel);
                    if (!targetRecord.boneTable || uint32_t(parent.boneIndex) >= targetRecord.boneCount) return MMS_E_STALE;
                    charge(1, nodeBytes);
                    const Target target{identity->object, identities.Element(targetModel, MMS_CHANNEL_BONES, uint32_t(parent.boneIndex))};
                    targets->emplace(pair, target);
                }
            }
            data->sampleVisibility = [motion](double frame, MmsDisplayStateV1& out) {
                uint8_t visible = 0;
                if (motion->SampleVisibility(frame, visible) != DisplayMotionStatus::Ok) return MMS_E_FAILED;
                out = {Header<MmsDisplayStateV1>(), visible, 0}; return MMS_OK;
            };
            data->sampleIk = [motion](uint32_t index, double frame, MmsIkStateV1& out) {
                uint8_t enabled = 0;
                if (motion->SampleIk(int(index), frame, enabled) != DisplayMotionStatus::Ok) return MMS_E_FAILED;
                out = {Header<MmsIkStateV1>(), enabled, 0}; return MMS_OK;
            };
            data->sampleParentState = [motion, targets = std::shared_ptr<const Targets>(std::move(targets))]
                (uint32_t index, double frame, MmsParentStateV1& out) {
                DisplaySelectorSample parent;
                if (motion->SampleSelector(int(index), frame, parent) != DisplayMotionStatus::Ok) return MMS_E_FAILED;
                MmsParentStateV1 value{}; value.h = Header<MmsParentStateV1>();
                value.windowStart = parent.windowStart; value.windowEnd = parent.windowEnd;
                // Kind comes from the captured slot; target identities only
                // exist for MODEL and always come from the frozen capture.
                switch (parent.modelIndex) {
                case -1: value.kind = MMS_EVALUATION_PARENT_INHERIT; break;
                case -2: value.kind = MMS_EVALUATION_PARENT_GROUND; break;
                default: {
                    // Capture checked every reachable key, so an unvalidated
                    // negative slot is malformed rather than another state.
                    if (parent.modelIndex < 0) return MMS_E_STALE;
                    const auto found = targets->find({parent.modelIndex, parent.boneIndex});
                    if (found == targets->end()) return MMS_E_FAILED;
                    value.target = found->second.object; value.bone = found->second.bone;
                    value.kind = MMS_EVALUATION_PARENT_MODEL;
                    break;
                }
                }
                out = value; return MMS_OK;
            };
        }
        data->retainedBytes = bytes;
        out = std::move(data); return MMS_OK;
    }, MMS_EVALUATION_CAP_BONE_CHANNELS | MMS_EVALUATION_CAP_MORPH_WEIGHTS | MMS_EVALUATION_CAP_DISPLAY,
    [app](const MmsEvaluationSceneQueryV1&, uint64_t limit,
        std::shared_ptr<const EvaluationSceneSourceData>& out) {
        return CaptureEvaluationScene(app, limit, out);
    });
#endif
}
void ForgetSceneObject(const void* token) {
    if (build_profile::original) {
        // Recording snapshots also retain runtime identities in this profile.
        identities.Forget(token);
        return;
    }
    const auto* entry = identities.Retained(token);
    const bool visible = (entry && entry->present) || geometryDigests.count(token) != 0;
    identities.Forget(token); geometryDigests.erase(token);
    if (visible) { ++revision; previous.clear(); }
}
MmsObjectRef SceneObjectIdentity(const void* token) { return identities.Get(token).object; }
bool FindSceneObjectIdentity(const void* token, MmsObjectRef& out) noexcept {
    const auto* entry = identities.Retained(token);
    if (!entry || !entry->present || !entry->published) return false;
    out = entry->object;
    return true;
}
MmsObjectRef CreateAbsentSceneObject(const void* token) { return identities.CreateAbsent(token).object; }
bool SceneObjectPresent(const void* token, MmsId id) noexcept {
    const auto* entry = identities.Retained(token);
    return entry && entry->present && Same(entry->object.id, id);
}
std::unique_ptr<PreparedHistoryChange> PrepareObjectPresence(const void* token, MmsId id, bool present) {
    const auto* entry = identities.Retained(token);
    if (!entry || !Same(entry->object.id, id)) throw std::invalid_argument("retained scene identity");
    struct Candidate final : PreparedHistoryChange {
        SceneIdentity::Presence presence;
        explicit Candidate(SceneIdentity::Presence value) : presence(std::move(value)) {}
        bool IsCurrent() const noexcept override { return presence.IsCurrent(); }
        void Publish() noexcept override { presence.Publish(); }
    };
    return std::make_unique<Candidate>(identities.PreparePresence(token, present));
}
void ResetSceneIdentity() { identities.Clear(); opaqueIdentity = false; previous.clear(); geometryDigests.clear(); ++revision; }
void SaveSceneIdentity(const MMDApp& app, pmm_extension::Document& document) {
    if (build_profile::original || opaqueIdentity) return;
    std::vector<uint8_t> bytes;
    Append(bytes, uint32_t(1)); const size_t countOffset = bytes.size(); Append(bytes, uint32_t(0)); uint32_t count = 0;
    for (uint32_t kind : {MMS_OBJECT_MODEL, MMS_OBJECT_ACCESSORY}) for (uint32_t slot = 0; slot < 255; ++slot) {
        const auto* token = Token(app, kind, slot); if (!token) continue;
        EnsureBones(token, kind); const auto& entry = identities.Get(token);
        Append(bytes, kind); Append(bytes, slot); Append(bytes, entry.object.id.lo); Append(bytes, entry.object.id.hi);
        Append(bytes, uint32_t(entry.elements.size()));
        for (const auto& element : entry.elements) { Append(bytes, element.first); Append(bytes, element.second.lo); Append(bytes, element.second.hi); }
        ++count;
    }
    std::memcpy(bytes.data() + countOffset, &count, sizeof(count)); pmm_extension::Set(document, identityKey, std::move(bytes));
}
void LoadSceneIdentity(MMDApp& app, const pmm_extension::Document& document,
                       const std::vector<int>& adaptedModelSlots) {
    if (build_profile::original) return;
    const auto* record = pmm_extension::Find(document, identityKey); if (!record) return;
    auto result = Boundary([&]() -> MmsResult {
        Reader reader{record->value}; if (reader.Get<uint32_t>() != 1) return MMS_E_VERSION;
        const auto count = reader.Get<uint32_t>(); if (count > 510) return MMS_E_ARGUMENT;
        std::vector<SceneIdentity::Restore> restored; std::set<uint64_t> slots; std::set<MmsId, IdLess> ids;
        for (uint32_t i = 0; i < count; ++i) {
            const auto kind = reader.Get<uint32_t>(), slot = reader.Get<uint32_t>();
            const auto* token = Token(app, kind, slot);
            if (!slots.insert(uint64_t(kind) << 32 | slot).second) return MMS_E_ARGUMENT;
            SceneIdentity::Entry entry;
            entry.object.id = {reader.Get<uint64_t>(), reader.Get<uint64_t>()};
            if (Empty(entry.object.id) || !ids.insert(entry.object.id).second) return MMS_E_ARGUMENT;
            const auto elements = reader.Get<uint32_t>();
            // Bound by the already decoded record, not a fixed vertex limit.
            // Dense imported meshes must not save identities we cannot reopen.
            if (elements > (record->value.size() - reader.offset) / 24) return MMS_E_ARGUMENT;
            for (uint32_t n = 0; n < elements; ++n) {
                auto key = reader.Get<uint64_t>(); MmsId id{reader.Get<uint64_t>(), reader.Get<uint64_t>()};
                if (Empty(id) || !ids.insert(id).second || !entry.elements.emplace(key, id).second) return MMS_E_ARGUMENT;
            }
            const bool adapted = kind == MMS_OBJECT_MODEL &&
                std::find(adaptedModelSlots.begin(), adaptedModelSlots.end(), static_cast<int>(slot)) != adaptedModelSlots.end();
            if (token) restored.push_back({token, std::move(entry), !adapted});
        }
        if (reader.offset != record->value.size()) return MMS_E_ARGUMENT;
        identities.Replace(restored); previous.clear(); geometryDigests.clear(); ++revision; return MMS_OK;
    });
    // A transient allocation failure cannot leave a valid old positional
    // record beside the adapted standard model. Saving that opaque record
    // would make it applicable again on the next (now matching) reopen.
    if (result == MMS_E_MEMORY && !adaptedModelSlots.empty()) throw std::bad_alloc();
    if (result) { opaqueIdentity = true; runtime_log::Write("PLUGIN_ABI identity record retained without applying: %d", result); }
}
}
