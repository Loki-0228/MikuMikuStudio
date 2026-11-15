#pragma once

#include "mikumikustudio/subrecord_layout.hpp"
#include "mikumikustudio/build_profile.hpp"
#include <cmath>
#include <vector>

namespace mikumikustudio::pmx21 {

struct Impulse {
    std::int32_t rigid;
    std::uint8_t local;
    float velocity[3];
    float torque[3];
};

// PmxEditor TransformMorphing resolves flips before applying ordinary/group
// morphs. A selected entry replaces its target value; it is not a group sum.
// Work on a copy so previewing a flip never overwrites animation keys/sliders.
class MorphWeights {
    std::vector<float> values_;
public:
    MorphWeights(const mdl::MorphRecord* morphs, int count) {
#if !MMD_ORIGINAL_PROFILE
        if (!morphs || count <= 0) return;
        bool flips = false;
        for (int i = 0; i < count; ++i) flips |= morphs[i].type == 9;
        if (!flips) return;
        values_.reserve(count);
        for (int i = 0; i < count; ++i) values_.push_back(morphs[i].value);
        auto flip = [&](const mdl::MorphRecord& morph, float weight) {
            if (!morph.groupEntries || morph.groupCount <= 0 ||
                !std::isfinite(weight) || weight <= 0) return;
            const int selected = weight >= 1 ? morph.groupCount - 1 :
                static_cast<int>((double(morph.groupCount) + 1) * weight) - 1;
            if (selected < 0 || selected >= morph.groupCount) return;
            const auto& entry = morph.groupEntries[selected];
            if (entry.morphIndex >= 0 && entry.morphIndex < count)
                values_[entry.morphIndex] = entry.weight;
        };
        for (int i = 0; i < count; ++i) {
            const auto& morph = morphs[i];
            const float weight = values_[i];
            if (morph.type == 9) flip(morph, weight);
            if (morph.type != 0 || !morph.groupEntries) continue;
            for (int j = 0; j < morph.groupCount; ++j) {
                const auto& entry = morph.groupEntries[j];
                if (entry.morphIndex >= 0 && entry.morphIndex < count &&
                    morphs[entry.morphIndex].type == 9)
                    flip(morphs[entry.morphIndex], weight * entry.weight);
            }
        }
#endif
    }
    float Get(int index, float value) const { return values_.empty() ? value : values_[index]; }
};

} // namespace mikumikustudio::pmx21
