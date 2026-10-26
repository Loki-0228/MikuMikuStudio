#include "mikumikustudio/bone_motion_snapshot.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_sampling.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
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
    bool externalParent = false;
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
        track.externalParent = model.boneTable[bone].slotIndex >= 0;
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
    if (track.externalParent) return BoneMotionStatus::ExternalParent;
    if (!DirectType(track.type)) return BoneMotionStatus::UnsupportedBone;
    const auto after = std::upper_bound(track.keys.begin(), track.keys.end(), frame,
        [](double time, const Key& key) { return time < key.native.frame; });
    if (after == track.keys.begin()) return BoneMotionStatus::InvalidSample;
    const auto& lower = *(after - 1);
    BoneMotionSample sample;
    sample.physicsDisabled = lower.native.physicsDisabled != 0;
    sample.scale = lower.scale;
    if (after == track.keys.end() || frame == lower.native.frame) {
        std::memcpy(sample.position.data(), lower.native.position, sizeof(lower.native.position));
        std::memcpy(sample.rotation.data(), lower.native.rotation, sizeof(lower.native.rotation));
    } else {
        const auto& upper = *after;
        const float time = float(frame - lower.native.frame) / float(int(upper.native.frame) - int(lower.native.frame));
        bone_key_sampling::SeekSlerp(lower.native.rotation, upper.native.rotation,
            bone_key_sampling::Ease(upper.native, 3, time), sample.rotation.data());
        for (unsigned axis = 0; axis < 3; ++axis) {
            const float delta = upper.native.position[axis] - lower.native.position[axis];
            sample.position[axis] = delta == 0 ? lower.native.position[axis] :
                float(double(bone_key_sampling::Ease(upper.native, axis, time)) * delta + double(lower.native.position[axis]));
        }
    }
    if (after != track.keys.end()) {
        const auto& upper = *after;
        const float time = float(std::clamp((frame - lower.native.frame) /
            double(upper.native.frame - lower.native.frame), 0.0, 1.0));
        sample.scale = bone_scale::Interpolate(lower.scale, upper.scale, bone_scale::CurveTime(upper.controls, time));
    }
    if (impl_->curves->Contains(bone) && !impl_->curves->Sample(bone, frame,
        sample.position.data(), sample.rotation.data(), sample.scale)) return BoneMotionStatus::InvalidSample;
    if (!Finite(sample.position.data(), 3) || !ValidRotation(sample.rotation.data()) || !bone_scale::Valid(sample.scale))
        return BoneMotionStatus::InvalidSample;
    out = sample;
    return BoneMotionStatus::Ok;
}
}
#endif
