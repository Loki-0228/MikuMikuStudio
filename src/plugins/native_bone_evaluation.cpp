#include "native_bone_evaluation.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/frame_limits.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>

namespace mikudancestudio::plugins {
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
float Ease(const mdl::BoneKey& key, unsigned channel, float time) {
    // CurveTime is the same architecture-specific byte Bezier as BoneEase,
    // and accepts the private target bytes without borrowing a live model.
    return bone_scale::CurveTime({key.interpolation[channel], key.interpolation[channel + 4],
        key.interpolation[channel + 8], key.interpolation[channel + 12]}, time);
}
void SeekSlerp(const float* previous, const float* next, float time, float* out) noexcept {
    // Keep the direct-seek operation order. The playback routine uses a
    // reciprocal multiply instead of this divide and is not interchangeable.
#if defined(_M_IX86)
    const float dot = float(double(previous[0]) * next[0] + double(previous[1]) * next[1] +
        double(previous[2]) * next[2] + double(previous[3]) * next[3]);
    if (float(1.0 - double(dot) * dot) == 0.0f) {
        std::memcpy(out, previous, 4 * sizeof(float)); return;
    }
    const float clamped = dot > 1.0f ? 0.999999f : dot < -1.0f ? -0.999999f : dot;
    const float theta = float(std::acos(double(clamped)));
    const bool reverse = double(theta) > double(3.141592f) * 0.5 && clamped < 0;
    const float angle = reverse ? float(std::acos(-double(clamped))) : theta;
    const float sine = float(std::sin(double(angle)));
    const float a0 = float((1.0 - double(time)) * angle), a1 = float(double(time) * angle);
    const float w0 = float(double(float(std::sin(double(a0)))) / sine);
    const float w1 = float(double(float(std::sin(double(a1)))) / sine);
    for (unsigned n = 0; n < 4; ++n)
        out[n] = reverse ? float(double(previous[n]) * w0 - double(w1) * next[n]) :
            float(double(previous[n]) * w0 + double(w1) * next[n]);
#else
    const float dot = previous[0] * next[0] + previous[1] * next[1] +
        previous[2] * next[2] + previous[3] * next[3];
    if (1.0f - dot * dot == 0.0f) {
        std::memcpy(out, previous, 4 * sizeof(float)); return;
    }
    const float clamped = dot > 1.0f ? 0.999999f : dot < -1.0f ? -0.999999f : dot;
    const float theta = std::acos(clamped);
    const bool reverse = theta > 1.570796012878418f && clamped < 0;
    const float angle = reverse ? std::acos(-clamped) : theta;
    const float sine = std::sin(angle);
    const float w0 = std::sin((1.0f - time) * angle) / sine;
    const float w1 = std::sin(time * angle) / sine;
    for (unsigned n = 0; n < 4; ++n)
        out[n] = reverse ? previous[n] * w0 - w1 * next[n] : previous[n] * w0 + w1 * next[n];
#endif
}
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
                if (window.windowStart != sample.windowStart || window.windowEnd != sample.windowEnd) return false;
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
                Interval interval;
                if (!ReadInterval(view, index, model.boneCount, unsigned(candidate->frame), interval)) return MMS_E_ARGUMENT;
                if (NativeType(bone.type)) {
                    if (sample.selector >= 0) {
                        if (!candidate->order || unsigned(sample.selector) >= candidate->orderCount) return MMS_E_ARGUMENT;
                        const auto& window = static_cast<const mdl::BoneOrderEntry*>(candidate->order)[sample.selector];
                        sample.window = true; sample.windowStart = window.windowStart; sample.windowEnd = window.windowEnd;
                    }
                    const auto status = SampleNative(sample, interval, unsigned(candidate->frame), candidate->physicsMode);
                    if (status != MMS_OK) return status;
                    sample.writePose = true;
                }
                auto scale = view.scale->Key(int(interval.lowerSlot));
                if (interval.upperSlot) {
                    const float time = float(std::clamp((double(candidate->frame) - interval.lower.frame) /
                        double(interval.upper.frame - interval.lower.frame), 0.0, 1.0));
                    scale = bone_scale::Interpolate(scale, view.scale->Key(int(interval.upperSlot)),
                        bone_scale::CurveTime(view.scale->KeyCurve(int(interval.upperSlot)), time));
                }
                sample.writePose |= view.curves->Sample(int(index), candidate->frame, interval.nativeHash,
                    sample.after.position.data(), sample.after.rotation.data(), scale);
                if (!bone_scale::Valid(scale)) return MMS_E_ARGUMENT;
                if (sample.writePose) {
                    for (float value : sample.after.position) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
                    for (float value : sample.after.rotation) if (!std::isfinite(value)) return MMS_E_ARGUMENT;
                }
                if (bone_scale::Identity(scale)) candidate->afterScale.erase(int(index));
                else candidate->afterScale[int(index)] = scale;
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
