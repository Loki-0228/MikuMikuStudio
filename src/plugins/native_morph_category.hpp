#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Menu 226/228/231 use MorphPanel: mouth=3, eye=2, brow=1.
// Planning is read-only. The candidate owns native keys, zero-frame data,
// affected author layers and the weights explicitly discarded by the command.
MmsResult PlanMorphCategoryPurge(MMDApp&, unsigned modelSlot, unsigned panel,
    std::shared_ptr<HistoryChange>&) noexcept;
// A bound route consumes cancellation, no-op and refusal. Unbound callers
// retain the legacy confirmation and immediate command behavior.
bool TryPurgeBoundMorphCategory(MMDApp*, unsigned panel) noexcept;
}
#endif
