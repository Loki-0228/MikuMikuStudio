#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Deletes only marked morph keys, with the native root/sentinel rules. Other
// families' marks reject by default; allowOtherMarks is for a caller that
// prepares and validates all families before publishing one aggregate change.
// No other family's marks, undo rings, cursor or maximum frame are changed.
//
// Each direction samples only affected morphs at the current playhead, using
// the untouched pristine curves and the target native tracks. Morph weights
// have no unregistered-edit flag: like native delete/undo, affected weights are
// replaced by evaluation; unrelated morph weights remain as they were.
// Registered pool identity survives growth. The record retains its scene
// object; the app must outlive the journal. Resident resources are budgeted
// separately from this record's payload. Planning/first Prepare are serialized.
// A successful null result is a no-op and must not discard redo.
MmsResult PlanMorphKeyDeletion(MMDApp&, unsigned modelSlot,
    std::shared_ptr<HistoryChange>& out, bool allowOtherMarks = false) noexcept;
}
#endif
