#pragma once
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "shared_history.hpp"
namespace mikumikustudio { class MMDApp; }
namespace mikumikustudio::plugins {
// Menu-253 edge-thickness dialog (edit 646, trackbar 647) as one journal edit.
// The target is fixed by slot, live pointer, retained owner and stable object
// id. Camera mode, the selected model and dialog/capture handles are never
// consulted, so a redone edit cannot move to another object or window.
//
// Begin captures before/after plus the target identity and current revision.
// Set stages a value while the plan is neither previewed nor published; finite
// values are kept verbatim, including negatives and values above two. Take
// returns the same shared state when before/after differ in raw float bytes and
// leaves out empty otherwise, so the caller keeps redo. The controller reserves
// PrepareNative(Adjustment) first and calls Preview for each live sample:
// Preview performs no allocation, sends no message and writes only
// ModelRecord::edgeScale. It refuses once the plan is published or when the
// scene generation, retained owner, slot pointer, stable id, initial object
// generation, bound journal generation, history revision or current edgeScale
// (before first, this plan's after afterwards) no longer match. CommitNative
// publishes the current mutable after, not the first preview sample.
//
// Undo and redo revalidate that the state still names the same live object and
// that the generation observed during Prepare did not change before Publish.
// Only the first Prepare must match Begin's generation and revision; a redo
// may follow a controlled delete/undo restore of the same id with a new
// generation. Publish writes edgeScale only, switches applied/published/
// preview, increments serial and terminates if the source no longer matches.
// RequiresPhysicsReset is false.
//
// Cancel (also from the destructor) never touches history resources. It
// restores before only while the original scene, the retained live object and
// its edgeScale still equal this plan's after; a foreign same-field writer or
// a replaced scene is left untouched. A changed binding does not block that
// rollback, because Cancel never dereferences the journal. A published change
// is never rolled back. Cancelling nulls the wrapper; the shared state remains
// valid for every journal node that owns it. Dropping the wrapper before any
// Preview leaves a staged HistoryChange publishable, because no live field was
// written yet and the caller may still reserve it.
class NativeModelOutlinePlan {
    struct State;
    std::shared_ptr<State> state_;
public:
    NativeModelOutlinePlan() noexcept = default;
    ~NativeModelOutlinePlan();
    NativeModelOutlinePlan(const NativeModelOutlinePlan&) = delete;
    NativeModelOutlinePlan& operator=(const NativeModelOutlinePlan&) = delete;
    MmsResult Begin(MMDApp&, unsigned slot) noexcept;
    MmsResult Set(float after) noexcept;
    MmsResult Take(std::shared_ptr<HistoryChange>& out) noexcept;
    MmsResult Preview(float after) noexcept;
    // True when no live preview remains owned. False when a preview could not
    // be restored (foreign value or replaced scene) or the change is published.
    bool Cancel() noexcept;
    bool Changed() const noexcept;
};
}
#endif
