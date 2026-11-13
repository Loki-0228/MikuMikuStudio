#pragma once
#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Detached representation only. Registration, sampling and persistence must
// explicitly adopt this data before it can be published by a production edit.
namespace mikumikustudio::negative_frames::authoring {
enum class Family { Bone, Morph, Camera, Light, Shadow, Gravity, Accessory };
enum class Mode { LegacyLinear, Native, Authored };

struct NativeData {
    bool boneControlsPresent = false, scaleControlsPresent = false, cameraControlsPresent = false;
    bool physicsPresent = false, physicsDisabled = false;
    std::array<std::uint8_t,16> boneControls{};
    std::array<std::uint8_t,4> scaleControls{};
    // Row-major native interpolation[4][6], not six consecutive four-byte curves.
    std::array<std::uint8_t,24> cameraControls{};
};
struct Lane {
    unsigned index = 0;
    animation_curve::Track track;
    std::string interpolationGroup;
    std::vector<std::array<double,4>> nativeControls, controlGeometry;
    std::vector<curve_editor::NativeCameraInterval> preservedCameraIntervals;
    // A future sampler must use native camera timing without a live callback.
    bool nativeCameraTiming = false;
};
struct Track {
    Family family = Family::Bone;
    std::vector<Lane> lanes;
};

namespace detail {
inline unsigned LaneCount(Family family) {
    switch (family) {
    case Family::Bone: return 9;
    case Family::Morph: return 1;
    case Family::Camera: return 11;
    case Family::Light: return 6;
    case Family::Shadow: return 2;
    case Family::Gravity: return 6;
    case Family::Accessory: return 12;
    }
    throw std::invalid_argument("unknown negative authoring family");
}
inline bool Discrete(Family family, unsigned lane) noexcept {
    return (family == Family::Camera && lane >= 8) ||
        (family == Family::Shadow && lane == 0) ||
        (family == Family::Gravity && lane >= 4) ||
        (family == Family::Accessory && lane >= 8);
}
inline bool Frame(std::int32_t frame) noexcept {
    return frame >= frame_limits::minimum && frame <= frame_limits::maximum;
}
template<std::size_t N> inline bool Finite(const std::array<double,N>& values) noexcept {
    for (double value : values) if (!std::isfinite(value)) return false;
    return true;
}
inline bool Controls(const std::array<double,4>& values) noexcept {
    for (double value : values)
        if (!std::isfinite(value) || value < 0 || value > 127) return false;
    // Native time inversion permits crossed X and endpoints 0/127.
    return true;
}
inline void ValidateIntervals(const Lane& lane) {
    const auto& keys = lane.track.keys;
    bool previous = false;
    std::int32_t previousEnd = 0;
    for (const auto& value : lane.preservedCameraIntervals) {
        const auto span = std::int64_t(value.last) - std::int64_t(value.first);
        const auto sourceSpan = std::int64_t(value.sourceLast) - std::int64_t(value.sourceFirst);
        if (!Frame(value.first) || !Frame(value.last) || !Frame(value.sourceFirst) || !Frame(value.sourceLast) ||
            span <= 0 || sourceSpan <= 0 || (previous && value.first < previousEnd) ||
            !std::isfinite(value.sampleFirst) || !std::isfinite(value.sampleLast) ||
            value.sampleFirst < value.sourceFirst || value.sampleLast > value.sourceLast ||
            value.sampleFirst >= value.sampleLast || !std::isfinite(value.sourceValueFirst) ||
            !std::isfinite(value.sourceValueLast) || !Controls(value.controls) || !Finite(value.geometry) ||
            (!value.oneFrameHold && !value.transportTiming))
            throw std::invalid_argument("invalid negative authoring camera interval");
        const auto end = std::lower_bound(keys.begin(), keys.end(), value.last,
            [](const animation_curve::Key& key, std::int32_t frame) { return key.frame < frame; });
        if (end == keys.end() || end == keys.begin() || end->frame != value.last ||
            (end - 1)->frame != value.first || end->interpolation != animation_curve::Interpolation::Bezier)
            throw std::invalid_argument("negative camera interval endpoints do not match keys");
        const auto& a = *(end - 1);
        const std::array<double,6> geometry{a.value, a.outWeight, a.outSlope,
            end->value, end->inWeight, end->inSlope};
        if (geometry != value.geometry)
            throw std::invalid_argument("negative camera interval geometry does not match keys");
        previous = true;
        previousEnd = value.last;
    }
}
inline void Validate(const Track& data) {
    if (data.lanes.size() != LaneCount(data.family))
        throw std::invalid_argument("incomplete negative authoring lanes");
    std::string error;
    for (unsigned index = 0; index < data.lanes.size(); ++index) {
        const auto& lane = data.lanes[index];
        if (lane.index != index || lane.track.keys.empty() ||
            lane.track.discrete != Discrete(data.family, index))
            throw std::invalid_argument("invalid negative authoring lane");
        if (!animation_curve::ValidateSigned(lane.track, error))
            throw std::invalid_argument(error);
        for (const auto& key : lane.track.keys)
            if (!Frame(key.frame) || (lane.track.discrete && std::floor(key.value) != key.value))
                throw std::invalid_argument("invalid negative authoring signed key");
        const bool camera = data.family == Family::Camera && index < 8;
        if ((!camera && lane.nativeCameraTiming) ||
            (!lane.preservedCameraIntervals.empty() && (!camera || !lane.nativeCameraTiming)))
            throw std::invalid_argument("invalid negative authoring native camera owner");
        // Metadata can be inactive after scalar editing. Preserve every row,
        // including finite geometry sentinels, without inventing missing rows.
        for (std::size_t row = 0; row < lane.nativeControls.size(); ++row)
            for (double control : lane.nativeControls[row])
                // The first key has no incoming interval. Preserve its raw
                // byte metadata, just as the positive copy source does; this
                // does not admit those bytes as usable incoming controls.
                if (!std::isfinite(control) || control < 0 || control > (row ? 127 : 255))
                    throw std::invalid_argument("invalid negative authoring native controls");
        for (const auto& geometry : lane.controlGeometry)
            if (!Finite(geometry)) throw std::invalid_argument("invalid negative authoring control geometry");
        ValidateIntervals(lane);
    }
}
template<std::size_t N> inline void ValidateBytes(const std::array<std::uint8_t,N>& bytes, bool present) {
    for (auto value : bytes)
        if (value > 127 || (!present && value != 0))
            throw std::invalid_argument("invalid or unmarked negative native controls");
}
inline void ValidateNative(Family family, const NativeData& native) {
    ValidateBytes(native.boneControls, native.boneControlsPresent);
    ValidateBytes(native.scaleControls, native.scaleControlsPresent);
    ValidateBytes(native.cameraControls, native.cameraControlsPresent);
    if ((!native.physicsPresent && native.physicsDisabled) ||
        (family != Family::Bone && (native.boneControlsPresent || native.scaleControlsPresent || native.physicsPresent)) ||
        (family != Family::Camera && native.cameraControlsPresent) ||
        (native.boneControlsPresent != native.scaleControlsPresent))
        throw std::invalid_argument("invalid negative native field presence");
}
inline bool SameNative(const NativeData& a, const NativeData& b) noexcept {
    return a.boneControlsPresent == b.boneControlsPresent && a.scaleControlsPresent == b.scaleControlsPresent &&
        a.cameraControlsPresent == b.cameraControlsPresent && a.physicsPresent == b.physicsPresent &&
        a.physicsDisabled == b.physicsDisabled && a.boneControls == b.boneControls &&
        a.scaleControls == b.scaleControls && a.cameraControls == b.cameraControls;
}
inline bool SameCurve(const animation_curve::Track& a, const animation_curve::Track& b) noexcept {
    if (a.discrete != b.discrete || a.keys.size() != b.keys.size()) return false;
    for (std::size_t i = 0; i < a.keys.size(); ++i) {
        const auto& x = a.keys[i];
        const auto& y = b.keys[i];
        if (x.frame != y.frame || x.value != y.value || x.inSlope != y.inSlope || x.outSlope != y.outSlope ||
            x.inWeight != y.inWeight || x.outWeight != y.outWeight ||
            x.interpolation != y.interpolation || x.tangent != y.tangent) return false;
    }
    return true;
}
inline bool SameTrack(const Track& a, const Track& b) noexcept {
    if (a.family != b.family || a.lanes.size() != b.lanes.size()) return false;
    for (std::size_t i = 0; i < a.lanes.size(); ++i) {
        const auto& x = a.lanes[i];
        const auto& y = b.lanes[i];
        if (x.index != y.index || !SameCurve(x.track, y.track) || x.interpolationGroup != y.interpolationGroup ||
            x.nativeControls != y.nativeControls || x.controlGeometry != y.controlGeometry ||
            x.preservedCameraIntervals != y.preservedCameraIntervals || x.nativeCameraTiming != y.nativeCameraTiming)
            return false;
    }
    return true;
}
inline void Charge(std::uint64_t& bytes, std::uint64_t count, std::uint64_t stride = 1) noexcept {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    bytes = stride && count > (maximum - bytes) / stride ? maximum : bytes + count * stride;
}
// Conservative control-block allowance for the supported standard libraries;
// charged separately even if an allocator co-locates it with the object.
inline constexpr std::uint64_t sharedOverhead = 8 * sizeof(void*);
}

class Curves;
using CurvesPtr = std::shared_ptr<const Curves>;
inline CurvesPtr Freeze(const Track&);
class Curves final {
public:
    const Track& Data() const noexcept { return data_; }
    std::uint64_t RetainedBytes() const noexcept {
        std::uint64_t bytes = sizeof(Curves);
        detail::Charge(bytes, detail::sharedOverhead);
        detail::Charge(bytes, data_.lanes.capacity(), sizeof(Lane));
        for (const auto& lane : data_.lanes) {
            detail::Charge(bytes, lane.track.keys.capacity(), sizeof(animation_curve::Key));
            detail::Charge(bytes, lane.interpolationGroup.capacity());
            detail::Charge(bytes, 1); // terminator; SSO is deliberately overcounted
            detail::Charge(bytes, lane.nativeControls.capacity(), sizeof(std::array<double,4>));
            detail::Charge(bytes, lane.controlGeometry.capacity(), sizeof(std::array<double,4>));
            detail::Charge(bytes, lane.preservedCameraIntervals.capacity(), sizeof(curve_editor::NativeCameraInterval));
        }
        return bytes;
    }
    Curves(const Curves&) = delete;
    Curves& operator=(const Curves&) = delete;
private:
    friend CurvesPtr Freeze(const Track&);
    explicit Curves(const Track& data) : data_(data) {}
    const Track data_;
};
inline CurvesPtr Freeze(const Track& data) {
    detail::Validate(data);
    // shared_ptr also deletes the complete object if its control block fails.
    return CurvesPtr(new const Curves(data));
}

class KeyData;
using Ptr = std::shared_ptr<const KeyData>;
inline Ptr Make(Family, Mode, const NativeData&, CurvesPtr = {});
class KeyData final {
public:
    Family FamilyKind() const noexcept { return family_; }
    Mode Semantics() const noexcept { return mode_; }
    const NativeData& Native() const noexcept { return native_; }
    const CurvesPtr& Author() const noexcept { return curves_; }
    KeyData(const KeyData&) = delete;
    KeyData& operator=(const KeyData&) = delete;
private:
    friend Ptr Make(Family, Mode, const NativeData&, CurvesPtr);
    KeyData(Family family, Mode mode, const NativeData& native, CurvesPtr curves)
        : family_(family), mode_(mode), native_(native), curves_(std::move(curves)) {}
    const Family family_;
    const Mode mode_;
    const NativeData native_;
    const CurvesPtr curves_;
};
inline Ptr Make(Family family, Mode mode, const NativeData& native, CurvesPtr curves) {
    detail::LaneCount(family);
    detail::ValidateNative(family, native);
    switch (mode) {
    case Mode::LegacyLinear:
        if (native.boneControlsPresent || native.scaleControlsPresent || native.cameraControlsPresent ||
            native.physicsPresent || curves)
            throw std::invalid_argument("legacy negative authoring must be empty");
        return {};
    case Mode::Native:
        if (curves || (family == Family::Bone && !native.boneControlsPresent) ||
            (family == Family::Camera && !native.cameraControlsPresent))
            throw std::invalid_argument("incomplete negative native authoring");
        break;
    case Mode::Authored:
        if (!curves || curves->Data().family != family)
            throw std::invalid_argument("negative authored curves missing or wrong family");
        break;
    default:
        throw std::invalid_argument("unknown negative authoring mode");
    }
    return Ptr(new const KeyData(family, mode, native, std::move(curves)));
}
inline bool Same(const Ptr& a, const Ptr& b) noexcept {
    if (!a || !b) return !a && !b;
    if (a->FamilyKind() != b->FamilyKind() || a->Semantics() != b->Semantics() ||
        !detail::SameNative(a->Native(), b->Native())) return false;
    const auto& x = a->Author();
    const auto& y = b->Author();
    return x && y ? detail::SameTrack(x->Data(), y->Data()) : !x && !y;
}
inline std::uint64_t RetainedBytes(const Ptr& value) noexcept {
    if (!value) return 0;
    std::uint64_t bytes = sizeof(KeyData);
    detail::Charge(bytes, detail::sharedOverhead);
    // Charge each row independently even when several rows share these curves.
    if (value->Author()) detail::Charge(bytes, value->Author()->RetainedBytes());
    return bytes;
}
}
