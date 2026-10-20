#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Menu 286 fixes scene membership before its modal picker. Both actual PMD
// outline resources are retained; WRITEONLY outline memory is never read.
// Prepare reconstructs positions from the readable, completed main buffer and
// reserves every resource before publication. PMX retains its material colors.
class NativeModelOutlineColorPlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    NativeModelOutlineColorPlan() noexcept = default;
    NativeModelOutlineColorPlan(const NativeModelOutlineColorPlan&) = delete;
    NativeModelOutlineColorPlan& operator=(const NativeModelOutlineColorPlan&) = delete;
    MmsResult Begin(MMDApp&) noexcept;
    MmsResult Set(int red, int green, int blue) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>&) noexcept;
};
// A bound route consumes cancellation and all errors, never falling back to
// the immediate legacy vertex writer. Unbound and original profiles keep it.
bool TryEditBoundModelOutlineColor(MMDApp*, void* ownerWindow) noexcept;
}
#endif
