#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <Windows.h>
#include <cstdint>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Private authoring routes. Both fields belong to one history participant;
// model selection resets the stored bone to zero, as native control 474 does.
MmsResult PlanAccessoryParentModel(MMDApp&, std::int32_t, std::shared_ptr<HistoryChange>&) noexcept;
MmsResult PlanAccessoryParentBone(MMDApp&, std::int32_t, std::shared_ptr<HistoryChange>&) noexcept;
MmsResult ApplyBoundAccessoryParentModel(MMDApp&, std::int32_t, bool& changed) noexcept;
MmsResult ApplyBoundAccessoryParentBone(MMDApp&, std::int32_t, bool& changed) noexcept;
// Bound notifications are consumed on refusal too. Never fall back to an
// unjournalled legacy write after stale, busy, allocation or budget failure.
bool TrySelectBoundAccessoryParentModel(MMDApp*, HWND) noexcept;
bool TrySelectBoundAccessoryParentBone(MMDApp*, HWND) noexcept;
// Readout of these two combos only. Numeric EDIT drafts are never touched.
void RefreshBoundAccessoryParentReadout(MMDApp&) noexcept;
}
#endif
