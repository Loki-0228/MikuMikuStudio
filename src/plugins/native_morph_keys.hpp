#pragma once
#include "mikudancestudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins {
// Register one morph's current weight at an absolute frame. Matches the native
// current-value registrar (including its local free-slot scan and unchanged
// searchCursor), then clears all three key families' selection marks. Only an
// actual native key-data change creates a record; mark-only changes are no-ops.
// Pristine morph curves survive and derive against the registered native keys.
// First publication preserves the live weight; undo/redo sample only this morph
// at the then-current playhead. Other weights/bone poses and old rings survive.
// The app outlives the retained scene owner/journal. As for bone registration,
// resident model/pool resources are budgeted separately from this record.
MmsResult PlanMorphRegistration(MMDApp&, unsigned modelSlot, int morphIndex, int frame,
    std::shared_ptr<HistoryChange>& out) noexcept;
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
struct BoneRangeScaleOptions;
struct RangeScaleResult;
// Internal aggregate participant. Simulates the native slot-order deletion /
// reinsertion even when every mapped frame is identical; the caller decides
// whether the complete multi-family operation is a no-op. The result carries
// cursor/maximum/marks for the aggregate, never writes shared scalar fields or
// another family's marks, and never grows the live key pool.
MmsResult PlanMorphRangeScale(MMDApp&, unsigned modelSlot,
    const BoneRangeScaleOptions&, RangeScaleResult&) noexcept;
}
#endif
