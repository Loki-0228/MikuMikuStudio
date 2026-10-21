#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <Windows.h>
#include <cstdint>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Internal scalar authoring route. The retained model and app outlive replay;
// no camera/bone evaluation or registration is part of this adjustment.
MmsResult PlanCameraParentBone(MMDApp&, std::int32_t, std::shared_ptr<HistoryChange>&) noexcept;
MmsResult ApplyBoundCameraParentBone(MMDApp&, std::int32_t, bool& changed) noexcept;
// True also consumes an invalid, stale, busy or failed bound notification.
bool TrySelectBoundCameraParentBone(MMDApp*, HWND combo) noexcept;
// 449 owns its complete legacy attachment adjustment, including the one-time
// detach pivot. It does not toggle follow, seek or register a camera key.
MmsResult PlanCameraParentModel(MMDApp&, std::int32_t, std::shared_ptr<HistoryChange>&) noexcept;
MmsResult ApplyBoundCameraParentModel(MMDApp&, std::int32_t, bool& changed) noexcept;
bool TrySelectBoundCameraParentModel(MMDApp*, HWND combo) noexcept;
// Derived 449/450 readout only. Preserves every other selection and EDIT draft.
void RefreshBoundCameraParentBoneReadout(MMDApp&) noexcept;
}
#endif
