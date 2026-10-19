#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mikumikustudio {
class MMDApp;
namespace mdl { struct AccessoryRecord; struct BoneRecord; }
}
namespace mikumikustudio::plugins {
class RetainedSceneObject;
struct FollowLightSample {
    std::array<float,3> direction{},color{};
};
struct FollowShadowSample {
    std::int32_t mode=0;
    float distance=0;
};
struct FollowGravitySample {
    float magnitude=0;
    std::array<float,3> direction{};
    // The noise parameter preserves its native signed-int bit pattern.
    // modeWord includes noiseEnabled and all three following native bytes.
    std::uint32_t noise=0,modeWord=0;
};
struct FollowAccessoryPose {
    std::uint8_t visible=0,shadowEnabled=0;
    std::int32_t parentModel=-1,parentBone=0;
    std::array<float,3> position{},rotation{};
    float scale=1,opacity=1;
};
struct FollowAccessoryParent {
    unsigned slot=0;
    std::int32_t bone=0;
    unsigned char* model=nullptr;
    mdl::BoneRecord* bones=nullptr;
    std::uint32_t boneCount=0;
    MmsId id{};
    std::uint64_t initialGeneration=0;
    std::shared_ptr<RetainedSceneObject> owner;
};
struct FollowAccessorySample {
    unsigned slot=0;
    mdl::AccessoryRecord* object=nullptr;
    MmsId id{};
    std::uint64_t initialGeneration=0;
    std::shared_ptr<RetainedSceneObject> owner;
    FollowAccessoryPose value;
    // Empty when value.parentModel==-1. No parent matrix is read or evaluated.
    FollowAccessoryParent parent;
};
struct FollowTrackSamples {
    FollowLightSample light;
    FollowShadowSample shadow;
    FollowGravitySample gravity;
    std::vector<FollowAccessorySample> accessories;
};
// A read-only enable-follow sample. Capturing and checking never publish an
// authored target, seek a model, update a renderer, or send a window message.
// Use IsCurrent only for the first publication; history replays saved numbers.
class CameraFollowSampleSource final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit CameraFollowSampleSource(std::unique_ptr<Impl>);
    friend MmsResult CaptureCameraFollowSamples(MMDApp&,
        std::shared_ptr<CameraFollowSampleSource>&) noexcept;
public:
    ~CameraFollowSampleSource();
    CameraFollowSampleSource(const CameraFollowSampleSource&)=delete;
    CameraFollowSampleSource& operator=(const CameraFollowSampleSource&)=delete;
    const FollowTrackSamples& Values() const noexcept;
    bool IsCurrent() const noexcept;
    std::uint64_t RetainedBytes() const noexcept;
};
MmsResult CaptureCameraFollowSamples(MMDApp&,
    std::shared_ptr<CameraFollowSampleSource>& out) noexcept;
}
#endif
