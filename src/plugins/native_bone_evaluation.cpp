#include "native_bone_evaluation.hpp"
#include "native_bone_registration.hpp"
#include "mikumikustudio/bone_key_sampling.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>

namespace mikumikustudio::plugins {
namespace {
struct Pose {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
};
Pose ReadPose(const mdl::BoneRecord& bone) noexcept {
    Pose pose;
    std::memcpy(pose.position.data(), bone.trans, sizeof(bone.trans));
    std::memcpy(pose.rotation.data(), bone.rotQuat, sizeof(bone.rotQuat));
    return pose;
}
Pose KeyPose(const mdl::BoneKey& key) noexcept {
    Pose pose;
    std::memcpy(pose.position.data(), key.position, sizeof(key.position));
    std::memcpy(pose.rotation.data(), key.rotation, sizeof(key.rotation));
    return pose;
}
bool SamePose(const Pose& a, const Pose& b) noexcept {
    return std::memcmp(a.position.data(), b.position.data(), sizeof(a.position)) == 0 &&
        std::memcmp(a.rotation.data(), b.rotation.data(), sizeof(a.rotation)) == 0;
}
bool NativeType(mdl::BoneType type) noexcept {
    return static_cast<std::int8_t>(type) <= static_cast<std::int8_t>(mdl::BoneType::Effector) ||
        type == mdl::BoneType::FixedAxis;
}
using bone_key_sampling::Ease;
using bone_key_sampling::SeekSlerp;
struct Interval {
    unsigned lowerSlot = 0, upperSlot = 0;
    mdl::BoneKey lower{}, upper{};
    std::uint64_t nativeHash = 14695981039346656037ull;
};
bool ReadInterval(const BoneEvaluationView& view, unsigned bone, unsigned boneCount,
    unsigned frame, Interval& interval) noexcept {
    unsigned slot = bone, previous = 0, previousFrame = 0;
    const auto hash = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size--) { interval.nativeHash ^= *bytes++; interval.nativeHash *= 1099511628211ull; }
    };
    for (std::size_t hop = 0; hop < view.keyCapacity; ++hop) {
        if (slot >= view.keyCapacity || (hop && slot < boneCount)) return false;
        const auto* source = view.key(view.context, slot); if (!source) return false;
        auto key = *source;
        if (key.frame > unsigned(frame_limits::maximum) || key.previous >= view.keyCapacity ||
            (!hop && key.frame) || (hop && (key.previous != previous || key.frame <= previousFrame))) return false;
        if (key.frame <= frame) { interval.lowerSlot = slot; interval.lower = key; }
        else if (!interval.upperSlot) { interval.upperSlot = slot; interval.upper = key; }
        key.allocated = 0; key.reserved[0] = key.reserved[1] = 0;
        hash(&key, sizeof(key));
        const auto scale = view.scale->Key(int(slot)); const auto curve = view.scale->KeyCurve(int(slot));
        hash(scale.v.data(), sizeof(float) * 9); hash(curve.data(), curve.size());
        if (!key.next) return true;
        previous = slot; previousFrame = key.frame; slot = key.next;
    }
    return false;
}
struct BoneSample {
    unsigned index = 0;
    unsigned char dirty = 0, rigid = 0, physics = 0;
    mdl::BoneType type{};
    int selector = -1;
    bool window = false, writePose = false;
    unsigned windowStart = 0, windowEnd = 0;
    mdl::BoneOrderEntry sourceWindow{};
    Pose before, after;
};
MmsResult SampleNative(BoneSample& result, const Interval& interval, unsigned frame, int physicsMode) noexcept {
    const auto& lower = interval.lower; const auto& upper = interval.upper;
    const bool terminal = !interval.upperSlot && frame > lower.frame;
    const bool exact = frame == lower.frame;
    if (result.rigid) {
        // Mode 3's enabled-to-disabled interval starts from simulated IK
        // backups and arms the solver. It cannot be reduced to key sampling.
        if (physicsMode == 3 && interval.upperSlot && lower.physicsDisabled == 0 && upper.physicsDisabled == 1)
            return MMS_E_NOT_READY;
        const bool notifies = terminal ? physicsMode >= 2 : physicsMode == 2 || physicsMode == 3;
        if (notifies && result.physics != lower.physicsDisabled) return MMS_E_NOT_READY;
    }
    if (terminal || exact) {
        result.after = KeyPose(lower);
        if (terminal && result.window && lower.frame < result.windowStart && result.windowStart <= frame)
            result.after = Pose{};
        return MMS_OK;
    }
    if (!interval.upperSlot || upper.frame <= lower.frame) return MMS_E_ARGUMENT;
    if (result.window) {
        bool snap = false; unsigned reference = 0;
        if (result.windowStart < upper.frame && lower.frame < result.windowStart) {
            snap = true; reference = result.windowStart;
        } else if (!(upper.frame < result.windowEnd || lower.frame >= result.windowEnd)) {
            snap = true; reference = result.windowEnd;
        }
        if (snap) { result.after = KeyPose(frame >= reference ? upper : lower); return MMS_OK; }
    }
    const float time = float(frame - lower.frame) / float(int(upper.frame) - int(lower.frame));
    SeekSlerp(lower.rotation, upper.rotation, Ease(upper, 3, time), result.after.rotation.data());
    for (unsigned axis = 0; axis < 3; ++axis) {
        const float delta = upper.position[axis] - lower.position[axis];
        result.after.position[axis] = delta == 0 ? lower.position[axis] :
            float(double(Ease(upper, axis, time)) * delta + double(lower.position[axis]));
    }
    return MMS_OK;
}
class Evaluation final : public PreparedHistoryChange {
public:
    MMDApp& app;
    unsigned slot;
    unsigned char* model;
    mdl::BoneRecord* bones;
    unsigned char* dirty;
    void* order;
    unsigned count, orderCount;
    int frame, physicsMode;
    bool published = false;
    std::vector<BoneSample> samples;
    bone_scale::Values beforeScale, afterScale;
    Evaluation(MMDApp& owner, unsigned modelSlot) : app(owner), slot(modelSlot), model(app.ModelSlot(slot)),
        bones(mdl::Mdl(model)->boneTable), dirty(mdl::Mdl(model)->bonePhysicsState),
        order(mdl::Mdl(model)->boneOrderTable), count(mdl::Mdl(model)->boneCount),
        orderCount(mdl::Mdl(model)->boneOrderCount), frame(app.CurrentFrame()), physicsMode(app.PlaybackPhysicsMode()),
        beforeScale(bone_scale::PoseValues(model)), afterScale(beforeScale) {}
    bool IsCurrent() const noexcept override {
        if (published || app.ModelSlot(slot) != model || app.CurrentFrame() != frame || app.PlaybackPhysicsMode() != physicsMode)
            return false;
        const auto& current = *mdl::Mdl(model);
        if (current.boneTable != bones || current.boneCount != count || current.bonePhysicsState != dirty ||
            current.boneOrderTable != order || current.boneOrderCount != orderCount ||
            (!afterScale.empty() && !bone_scale::HasPoseStorage(model)) || !bone_scale::PoseValuesEqual(model, beforeScale)) return false;
        for (const auto& sample : samples) {
            const auto& bone = bones[sample.index];
            if (dirty[sample.index] != sample.dirty || bone.type != sample.type || bone.hasRigidBody != sample.rigid ||
                bone.physicsDisabled != sample.physics || bone.slotIndex != sample.selector || !SamePose(ReadPose(bone), sample.before)) return false;
            if (sample.window) {
                const auto& window = static_cast<const mdl::BoneOrderEntry*>(order)[sample.selector];
                if (std::memcmp(&window, &sample.sourceWindow, sizeof(window))) return false;
            }
        }
        return true;
    }
    void Publish() noexcept override {
        if (!IsCurrent()) std::terminate();
        if (!bone_scale::ExchangePoseValues(model, afterScale)) std::terminate();
        for (const auto& sample : samples) if (sample.writePose) {
            auto& bone = bones[sample.index];
            std::memcpy(bone.trans, sample.after.position.data(), sizeof(bone.trans));
            std::memcpy(bone.rotQuat, sample.after.rotation.data(), sizeof(bone.rotQuat));
        }
        published = true;
    }
};
}
std::uint64_t BoneDeletionEvaluationStorageBytes(std::size_t count) noexcept {
    try {
        model_delete_budget::Bytes bytes;
        bytes.Add(sizeof(Evaluation));
        bytes.Array<BoneSample>(count);
        bytes.Array<unsigned>(count);
        return bytes.value;
    }catch(...){return UINT64_MAX;}
}
MmsResult SampleBoneEvaluation(const mdl::BoneRecord& bone, unsigned index,
    unsigned boneCount, int physicsMode, int sampleFrame, const BoneEvaluationView& view,
    BoneEvaluationSample& out) noexcept {
    if (!view.key || !view.scale || !view.curves || view.keyCapacity > std::size_t(INT32_MAX) ||
        boneCount > view.keyCapacity || index >= boneCount || sampleFrame < 0 || sampleFrame > frame_limits::maximum)
        return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        Interval interval;
        if (!ReadInterval(view, index, boneCount, unsigned(sampleFrame), interval)) return MMS_E_ARGUMENT;
        BoneSample sample;
        sample.rigid = bone.hasRigidBody; sample.physics = bone.physicsDisabled;
        sample.after = ReadPose(bone);
        if (NativeType(bone.type)) {
            if (bone.slotIndex >= 0) {
                if (!view.selectors || std::size_t(bone.slotIndex) >= view.selectorCount) return MMS_E_ARGUMENT;
                const auto& window = view.selectors[bone.slotIndex];
                sample.window = true; sample.windowStart = window.windowStart; sample.windowEnd = window.windowEnd;
            }
            if (const auto status = SampleNative(sample, interval, unsigned(sampleFrame), physicsMode)) return status;
            sample.writePose = true;
        }
        auto scale = view.scale->Key(int(interval.lowerSlot));
        if (interval.upperSlot) {
            const float time = float(std::clamp((double(sampleFrame) - interval.lower.frame) /
                double(interval.upper.frame - interval.lower.frame), 0.0, 1.0));
            scale = bone_scale::Interpolate(scale, view.scale->Key(int(interval.upperSlot)),
                bone_scale::CurveTime(view.scale->KeyCurve(int(interval.upperSlot)), time));
        }
        sample.writePose |= view.curves->Sample(int(index), sampleFrame, interval.nativeHash,
            sample.after.position.data(), sample.after.rotation.data(), scale);
        if (!bone_scale::Valid(scale)) return MMS_E_ARGUMENT;
        if (sample.writePose) {
            for (float value : sample.after.position) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
            for (float value : sample.after.rotation) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
        }
        BoneEvaluationSample result;
        result.position = sample.after.position; result.rotation = sample.after.rotation;
        result.scale = scale; result.writePose = sample.writePose;
        out = result;
        return MMS_OK;
    });
}
MmsResult PrepareBoneEvaluation(MMDApp& app, unsigned slot, const std::vector<unsigned>& affected,
    const BoneEvaluationView& view, std::unique_ptr<PreparedHistoryChange>& out) noexcept {
    out.reset();
    if (slot >= kModelSlotCount || !app.ModelSlot(slot) || !view.key || !view.scale || !view.curves ||
        view.keyCapacity > std::size_t(INT32_MAX) || app.CurrentFrame() < 0 || app.CurrentFrame() > frame_limits::maximum)
        return MMS_E_ARGUMENT;
    const auto& model = *mdl::Mdl(app.ModelSlot(slot));
    if (!model.boneTable || !model.bonePhysicsState || model.boneCount > view.keyCapacity) return MMS_E_ARGUMENT;
    return Boundary([&]() -> MmsResult {
        auto candidate = std::make_unique<Evaluation>(app, slot);
        auto indices = affected;
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        candidate->samples.reserve(indices.size());
        for (auto index : indices) {
            if (index >= model.boneCount) return MMS_E_ARGUMENT;
            const auto& bone = model.boneTable[index];
            BoneSample sample; sample.index = index; sample.dirty = model.bonePhysicsState[index];
            sample.rigid = bone.hasRigidBody; sample.physics = bone.physicsDisabled;
            sample.type = bone.type; sample.selector = bone.slotIndex; sample.before = sample.after = ReadPose(bone);
            if (!sample.dirty) {
                auto target = view;
                if (NativeType(bone.type)) {
                    if (sample.selector >= 0) {
                        if (!candidate->order || unsigned(sample.selector) >= candidate->orderCount) return MMS_E_ARGUMENT;
                        sample.sourceWindow = static_cast<const mdl::BoneOrderEntry*>(candidate->order)[sample.selector];
                        if (view.selectors && view.selectorCount != candidate->orderCount) return MMS_E_ARGUMENT;
                        if (!target.selectors) {
                            target.selectors = static_cast<const mdl::BoneOrderEntry*>(candidate->order);
                            target.selectorCount = candidate->orderCount;
                        }
                        sample.window = true;
                    }
                }
                BoneEvaluationSample result;
                if (const auto status = SampleBoneEvaluation(bone, index, model.boneCount, candidate->physicsMode,
                    candidate->frame, target, result)) return status;
                sample.after.position = result.position; sample.after.rotation = result.rotation;
                sample.writePose = result.writePose;
                if (bone_scale::Identity(result.scale)) candidate->afterScale.erase(int(index));
                else candidate->afterScale[int(index)] = result.scale;
            }
            candidate->samples.push_back(sample);
        }
        if (!candidate->afterScale.empty() && !bone_scale::HasPoseStorage(candidate->model)) return MMS_E_NOT_READY;
        if (!candidate->IsCurrent()) return MMS_E_STALE;
        out = std::move(candidate); return MMS_OK;
    });
}
}
#endif
