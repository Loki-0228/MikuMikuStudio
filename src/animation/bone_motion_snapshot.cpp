#include "mikumikustudio/bone_motion_snapshot.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_sampling.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/display_motion_snapshot.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

namespace mikumikustudio {
namespace {
struct Key {
    mdl::BoneKey native{};
    bone_scale::Matrix scale;
    bone_scale::Curve controls{};
};
struct Track {
    std::vector<Key> keys;
    mdl::BoneType type{};
    int selector = -1;
};
void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride, std::uint64_t limit) {
    if (bytes > limit || (stride && count > (limit - bytes) / stride))
        throw std::length_error("bone motion snapshot budget");
    bytes += count * stride;
}
bool Finite(const float* values, unsigned count) noexcept {
    for (unsigned i = 0; i < count; ++i) if (!std::isfinite(values[i])) return false;
    return true;
}
bool ValidRotation(const float* rotation) noexcept {
    if (!Finite(rotation, 4)) return false;
    double squared = 0;
    for (unsigned i = 0; i < 4; ++i) squared += double(rotation[i]) * rotation[i];
    return squared > 1e-20 && squared < 1e20;
}
bool DirectType(mdl::BoneType type) noexcept {
    return unsigned(type) <= unsigned(mdl::BoneType::Effector) || type == mdl::BoneType::FixedAxis;
}
}
struct BoneMotionSnapshot::Impl {
    std::vector<Track> bones;
    std::unique_ptr<bone_curve::SamplingState> curves;
    std::uint64_t bytes = 0;
};
namespace {
void CopyPose(const Key& key, BoneMotionSample& sample) noexcept {
    std::memcpy(sample.position.data(), key.native.position, sizeof(key.native.position));
    std::memcpy(sample.rotation.data(), key.native.rotation, sizeof(key.native.rotation));
}
// Shared native-key, selector-window and free-curve sampling. window is null
// for a bone without a selector window, so only registered values apply.
// selectionFrame is the exact discrete time that chooses the surrounding native
// keys, physics state and selector window; frame is the continuous time used
// only for interpolation and free curves. The legacy entry passes the same
// value for both, so its previous fractional-frame behavior is preserved.
BoneMotionStatus SampleChannelsOf(const Track& track,
    const bone_curve::SamplingState& curves, unsigned bone, double frame,
    double selectionFrame, const DisplaySelectorSample* window,
    BoneMotionSample& out) noexcept {
    const auto after = std::upper_bound(track.keys.begin(), track.keys.end(),
        selectionFrame,
        [](double time, const Key& key) { return time < key.native.frame; });
    if (after == track.keys.begin()) return BoneMotionStatus::InvalidSample;
    const auto& lower = *(after - 1);
    BoneMotionSample sample;
    sample.physicsDisabled = lower.native.physicsDisabled != 0;
    sample.scale = lower.scale;
    if (DirectType(track.type)) {
        if (after == track.keys.end() || frame == lower.native.frame) {
            const bool zeroFill = window && after == track.keys.end() &&
                lower.native.frame < window->windowStart &&
                window->windowStart <= selectionFrame;
            if (!zeroFill) CopyPose(lower, sample);
        } else {
            const auto& upper = *after;
            bool snap = false;
            std::uint32_t snapReference = 0;
            if (window) {
                if (window->windowStart < upper.native.frame &&
                    lower.native.frame < window->windowStart) {
                    snap = true;
                    snapReference = window->windowStart;
                } else if (!(upper.native.frame < window->windowEnd ||
                             lower.native.frame >= window->windowEnd)) {
                    snap = true;
                    snapReference = window->windowEnd;
                }
            }
            if (snap) {
                CopyPose(selectionFrame >= static_cast<double>(snapReference)
                             ? upper : lower, sample);
            } else {
                const float time = float(frame - lower.native.frame) /
                    float(int(upper.native.frame) - int(lower.native.frame));
                bone_key_sampling::SeekSlerp(lower.native.rotation, upper.native.rotation,
                    bone_key_sampling::Ease(upper.native, 3, time), sample.rotation.data());
                for (unsigned axis = 0; axis < 3; ++axis) {
                    const float delta = upper.native.position[axis] - lower.native.position[axis];
                    sample.position[axis] = delta == 0 ? lower.native.position[axis] :
                        float(double(bone_key_sampling::Ease(upper.native, axis, time)) * delta + double(lower.native.position[axis]));
                }
            }
        }
    } else {
        // InertTip/CoRotate own no local pose track; scale still applies.
        sample.position.fill(0.0f);
    }
    if (after != track.keys.end()) {
        const auto& upper = *after;
        const float time = float(std::clamp((frame - lower.native.frame) /
            double(upper.native.frame - lower.native.frame), 0.0, 1.0));
        sample.scale = bone_scale::Interpolate(lower.scale, upper.scale, bone_scale::CurveTime(upper.controls, time));
    }
    if (curves.Contains(bone) && !curves.Sample(bone, frame,
        sample.position.data(), sample.rotation.data(), sample.scale)) return BoneMotionStatus::InvalidSample;
    if (!Finite(sample.position.data(), 3) || !ValidRotation(sample.rotation.data()) || !bone_scale::Valid(sample.scale))
        return BoneMotionStatus::InvalidSample;
    out = sample;
    return BoneMotionStatus::Ok;
}
}
BoneMotionSnapshot::BoneMotionSnapshot() = default;
BoneMotionSnapshot::~BoneMotionSnapshot() = default;
std::unique_ptr<BoneMotionSnapshot> BoneMotionSnapshot::Capture(const unsigned char* source,
    std::uint64_t limit) {
    if (!source) throw std::invalid_argument("bone motion snapshot source");
    const auto& model = *mdl::Mdl(source);
    const auto capacity = mdl::BoneKeyCapacity(model);
    if ((model.boneCount && (!model.boneTable || !model.boneKeys)) ||
        model.boneCount > capacity || capacity > std::uint64_t(INT32_MAX))
        throw std::invalid_argument("bone motion snapshot pool");
    std::uint64_t bytes = 0;
    Charge(bytes, 1, sizeof(BoneMotionSnapshot) + sizeof(Impl), limit);
    // Refuse impossible retained budgets before allocating a bone table.
    Charge(bytes, model.boneCount, sizeof(Track) + sizeof(Key), limit);
    auto result = std::unique_ptr<BoneMotionSnapshot>(new BoneMotionSnapshot);
    result->impl_ = std::make_unique<Impl>();
    auto& value = *result->impl_;
    value.bones.resize(model.boneCount);
    bytes = sizeof(BoneMotionSnapshot) + sizeof(Impl);
    Charge(bytes, value.bones.capacity(), sizeof(Track), limit);
    std::set<unsigned> ownedSlots;
    for (unsigned bone = 0; bone < model.boneCount; ++bone) {
        auto& track = value.bones[bone];
        track.type = model.boneTable[bone].type;
        track.selector = model.boneTable[bone].slotIndex;
        unsigned slot = bone, previous = 0, previousFrame = 0;
        for (std::size_t hop = 0;; ++hop) {
            if (hop >= capacity || slot >= capacity || (hop && slot < model.boneCount) ||
                !ownedSlots.insert(slot).second)
                throw std::invalid_argument("bone motion snapshot chain");
            Key key; key.native = model.boneKeys[slot];
            const auto& native = key.native;
            if (native.frame > unsigned(frame_limits::maximum) || native.previous >= capacity ||
                (!hop && native.frame != 0) || (hop && (native.previous != previous || native.frame <= previousFrame)) ||
                !Finite(native.position, 3) || !ValidRotation(native.rotation))
                throw std::invalid_argument("bone motion snapshot key");
            for (unsigned i = 0; i < 16; ++i) if (native.interpolation[i] > 127)
                throw std::invalid_argument("bone motion snapshot controls");
            key.scale = bone_scale::Key(source, int(slot));
            key.controls = bone_scale::KeyCurve(source, int(slot));
            if (!bone_scale::Valid(key.scale)) throw std::invalid_argument("bone motion snapshot scale");
            for (auto control : key.controls) if (control > 127)
                throw std::invalid_argument("bone motion snapshot scale controls");
            const auto oldCapacity = track.keys.capacity();
            // The final capacity is charged below; this guards the minimum
            // next element before asking the allocator for growth.
            if (track.keys.size() == oldCapacity && sizeof(Key) > limit - bytes)
                throw std::length_error("bone motion snapshot keys budget");
            track.keys.push_back(key);
            Charge(bytes, track.keys.capacity() - oldCapacity, sizeof(Key), limit);
            if (!native.next) break;
            previous = slot; previousFrame = native.frame; slot = native.next;
        }
    }
    value.curves = bone_curve::SamplingState::Capture(source, limit - bytes);
    Charge(bytes, 1, value.curves->RetainedBytes(), limit);
    value.bytes = bytes;
    return result;
}
std::uint32_t BoneMotionSnapshot::BoneCount() const noexcept {
    return impl_ ? static_cast<std::uint32_t>(impl_->bones.size()) : 0;
}
std::uint64_t BoneMotionSnapshot::RetainedBytes() const noexcept {
    return impl_ ? impl_->bytes : sizeof(*this);
}
BoneMotionStatus BoneMotionSnapshot::SampleChannels(int bone, double frame, BoneMotionSample& out) const noexcept {
    if (!impl_ || bone < 0 || std::size_t(bone) >= impl_->bones.size()) return BoneMotionStatus::InvalidBone;
    if (!std::isfinite(frame) || frame < 0 || frame > frame_limits::maximum) return BoneMotionStatus::InvalidTime;
    const auto& track = impl_->bones[bone];
    if (track.selector >= 0) return BoneMotionStatus::ExternalParent;
    if (!DirectType(track.type)) return BoneMotionStatus::UnsupportedBone;
    return SampleChannelsOf(track, *impl_->curves, unsigned(bone), frame, frame,
        nullptr, out);
}
BoneMotionStatus BoneMotionSnapshot::PrepareKinematicChannels(int bone, double frame,
    const DisplayMotionSnapshot& display, BoneMotionSample& out) const noexcept {
    // The legacy double contract keeps its original invalid bone/time refusals
    // before the validated discrete time is handed to the exact path.
    if (!impl_ || bone < 0 || std::size_t(bone) >= impl_->bones.size()) return BoneMotionStatus::InvalidBone;
    if (!std::isfinite(frame) || frame < 0 || frame > frame_limits::maximum) return BoneMotionStatus::InvalidTime;
    EvaluationTime time;
    if (!EvaluationTime::FromFrame(frame, time)) return BoneMotionStatus::InvalidTime;
    return PrepareKinematicChannelsAt(bone, time, display, out);
}
BoneMotionStatus BoneMotionSnapshot::PrepareKinematicChannelsAt(int bone,
    const EvaluationTime& time, const DisplayMotionSnapshot& display,
    BoneMotionSample& out) const noexcept {
    if (!impl_ || bone < 0 || std::size_t(bone) >= impl_->bones.size()) return BoneMotionStatus::InvalidBone;
    const auto& track = impl_->bones[bone];
    if (track.type > mdl::BoneType::CoRotate) return BoneMotionStatus::UnsupportedBone;
    const double selectionFrame = static_cast<double>(time.DiscreteFrame());
    DisplaySelectorSample window{};
    const bool windowed = DirectType(track.type) && track.selector >= 0;
    if (windowed) {
        std::int32_t mapped = 0;
        if (!display.SelectorBone(unsigned(track.selector), mapped) || mapped != bone)
            return BoneMotionStatus::InvalidSample;
        if (display.SampleSelector(track.selector, selectionFrame, window) != DisplayMotionStatus::Ok)
            return BoneMotionStatus::InvalidSample;
    }
    // Continuous position/rotation/scale/free curves use Frame(); every discrete
    // track (native keys, physicsDisabled, selector window snap, zero-fill)
    // uses DiscreteFrame().
    return SampleChannelsOf(track, *impl_->curves, unsigned(bone), time.Frame(),
        selectionFrame, windowed ? &window : nullptr, out);
}
}
#endif
