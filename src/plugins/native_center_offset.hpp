#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <array>
#include <memory>
#include <mms/plugin.h>
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
class SharedHistory;
class HistoryChange;
// Internal planner participant. It adds root-parent dependencies without
// widening the field ownership of other bone-registration transactions.
MmsResult WrapCenterOffsetParentDependencies(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>&) noexcept;
// Internal menu-219 session. Modal input never owns the journal reservation.
// The app must outlive the session; a replaced scene or binding rejects Apply.
class CenterOffsetEdit {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    CenterOffsetEdit();
    ~CenterOffsetEdit();
    MmsResult Begin(MMDApp&, SharedHistory&) noexcept;
    // Consumes the session on every result, including a no-op or failed edit.
    MmsResult Apply(const std::array<float, 3>&) noexcept;
};
// Bound success, cancellation and refusal are all consumed without old-ring
// fallback. The unbound and original-profile command retains its old owner.
bool TryEditBoundCenterOffset(MMDApp*) noexcept;
}
#endif
