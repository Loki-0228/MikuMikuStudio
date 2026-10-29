#pragma once
#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio {
class MMDApp;
namespace sequence_scatter {
constexpr unsigned kMenuCommand = 32921;
struct Channel {
    int morph = 0;
    animation_curve::NamedTrack original;
    animation_curve::Track clip;
};
struct Source {
    unsigned char* model = nullptr;
    int first = 0, last = 0;
    std::size_t keyCount = 0;
    std::vector<Channel> channels;
    std::vector<mdl::MorphKey> pool;
    struct BoneChannel { int bone; std::vector<int> original, clip; };
    std::vector<BoneChannel> bones;
    std::vector<mdl::BoneKey> bonePool;
    bone_scale::Snapshot scales;
};
struct Settings {
    int first = 0, last = 300;
    double gapMean = 90, gapVariance = 225;
    double speedMean = 1, speedVariance = .04;
    std::uint32_t seed = 1;
};
struct Plan {
    std::vector<animation_curve::NamedTrack> tracks;
    std::vector<mdl::BoneKey> bonePool;
    bone_scale::Snapshot scales;
    std::size_t copies = 0, keys = 0, conflicts = 0, compressed = 0;
};
#if !MMD_ORIGINAL_PROFILE
bool Capture(unsigned char*, Source&, std::string& error);
bool Prepare(const Source&, const Settings&, Plan&, std::string& error);
bool Apply(MMDApp&, const Source&, const Plan&, std::string& error);
// Operation 6 owns a malloc-compatible bone/morph snapshot in each undo slot.
bool Restore(unsigned char*, const void* snapshot) noexcept;
void RefreshMenu(MMDApp*);
bool HandleCommand(MMDApp*, unsigned command);
#else
inline bool Restore(unsigned char*, const void*) noexcept { return false; }
inline void RefreshMenu(MMDApp*) {}
inline bool HandleCommand(MMDApp*, unsigned) { return false; }
#endif
}
}
