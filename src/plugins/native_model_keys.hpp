#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// One atomic model-key deletion across bone, morph and display/IK/parent
// tracks. Keyboard fallback is considered only when all families are unmarked.
// A null successful result is a no-op. The caller owns the serialized editing
// scope and budgets retained model resources separately from these records.
MmsResult PlanModelKeyDeletion(MMDApp&, unsigned modelSlot, bool automaticInterpolation,
    std::shared_ptr<HistoryChange>&, bool selectedAtCurrentFrame = false) noexcept;
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
