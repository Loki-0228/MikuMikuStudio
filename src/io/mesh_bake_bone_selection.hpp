#pragma once

#include "mikumikustudio/subrecord_layout.hpp"

namespace mikumikustudio::alembic_export {

struct BoneSelectionView {
    const unsigned char* selected;
    std::uint32_t count;

    bool Contains(std::int32_t bone) const noexcept {
        return selected && bone >= 0 && static_cast<std::uint32_t>(bone) < count
            && selected[bone] != 0;
    }
};

inline bool HasSelectedBoneInfluence(const mdl::PmxVertex& vertex,
                                     BoneSelectionView selection) noexcept {
    // Read only the lanes written by the PMX loader for this weight type.
    switch (vertex.weightType) {
    case mdl::PmxWeightType::bdef1:
        return selection.Contains(vertex.bone[0]);
    case mdl::PmxWeightType::bdef2:
    case mdl::PmxWeightType::sdef: {
        const float first = vertex.weight[0];
        return (first > 0.0f && selection.Contains(vertex.bone[0]))
            || (1.0f - first > 0.0f && selection.Contains(vertex.bone[1]));
    }
    case mdl::PmxWeightType::bdef4:
        for (unsigned i = 0; i < 4; ++i)
            if (vertex.weight[i] > 0.0f && selection.Contains(vertex.bone[i]))
                return true;
        return false;
    default:
        // QDEF and unknown types are not skinned by the native PMX path.
        return false;
    }
}

inline bool HasSelectedBoneInfluence(const mdl::PmdVertex& vertex,
                                     BoneSelectionView selection) noexcept {
    return (vertex.weightPercent > 0 && selection.Contains(vertex.bone[0]))
        || (vertex.weightPercent < 100 && selection.Contains(vertex.bone[1]));
}

}  // namespace mikumikustudio::alembic_export
