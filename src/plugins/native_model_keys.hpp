#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <vector>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// One atomic model-key deletion across bone, morph and display/IK/parent
// tracks. Keyboard fallback is considered only when all families are unmarked.
// A null successful result is a no-op. The caller owns the serialized editing
// scope and budgets retained model resources separately from these records.
struct ModelKeySelection {
    std::vector<unsigned> bones, morphs, displays;
};
MmsResult PlanModelKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>&, bool selectedAtCurrentFrame = false,
    const ModelKeySelection* privateSelection = nullptr) noexcept;
// Finds redundant interior keys without writing live selection marks. The
// detached selection, authoring and all three key families publish together.
MmsResult PlanUnusedModelKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>&, std::uint32_t& deleted) noexcept;
struct ModelRangeScaleOptions {
    std::uint32_t start = 0, end = 0;
    double scale = 1;
    bool bones = false, morphs = false, display = false;
    bool automaticInterpolation = false;
};
MmsResult PlanModelRangeScale(MMDApp&, unsigned modelSlot, const ModelRangeScaleOptions&,
    std::shared_ptr<HistoryChange>&) noexcept;
}
#endif
