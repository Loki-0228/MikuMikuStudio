#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
#include <memory>

namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// One explicit source-path publication for a model object. Begin captures the
// stable model identity/owner, both complete 256-character path buffers and the
// initial scene/history versions. The successful enhanced-PMD file publication
// is never reopened by undo or redo: both directions only memcpy a retained
// buffer. A plan keeps its state after Take so the final file check can call
// IsCurrent(); Take itself may run only once, including for a no-op path.
class NativeModelSourcePlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    NativeModelSourcePlan() noexcept = default;
    NativeModelSourcePlan(const NativeModelSourcePlan&) = delete;
    NativeModelSourcePlan& operator=(const NativeModelSourcePlan&) = delete;
    MmsResult Begin(MMDApp&, unsigned slot) noexcept;
    MmsResult Set(const wchar_t* path) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>& out) noexcept;
    bool IsCurrent() const noexcept;
    // Captured pointers only; neither getter grants dereference authority.
    unsigned char* Object() const noexcept;
    SharedHistory* Journal() const noexcept;
    SharedHistory* PermissionHistory() const noexcept;
};
}
#endif
