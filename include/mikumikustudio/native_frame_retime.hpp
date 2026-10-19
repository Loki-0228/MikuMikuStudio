// MMS addition: the native half of "Edit -> Correct by frame rate..."
// (include/mikumikustudio/frame_rate_correction.hpp).
//
// That feature converts a whole project from one frame rate to another with
//
//     newFrame = round(originalFrame * currentFPS / originalFPS)
//
// anchored at frame 0.  This module is the part of the backend that rewrites the
// port's OWN animation tables; the MMV effect tracks and the mesh bake caches
// are another module's business and are deliberately not touched here.
//
// What is converted
// -----------------
// Everything a project actually has loaded, selected or not:
//
//   * every model in a slot, including a model that is not the selected one and
//     a model whose bones or morphs are hidden - the conversion is a property of
//     the file, not of the current selection;
//   * per model: the bone position/rotation/scale tracks, the morph tracks, the
//     display/IK/selector tracks and the two external-parent fields a bone key
//     carries;
//   * the application-wide tables: camera, light, self shadow and gravity;
//   * every loaded accessory and its own timeline;
//   * the authored layers that sit on top of those tracks, so a frame-rate
//     conversion never looks like a user edit to them:
//       - the per-morph authored/mirrored expression curves (morph_curve),
//       - the per-key-slot bone scale curves (bone_scale),
//       - the authoring the shared curve host keeps for the global and accessory
//         tracks (curve_host).
//
// Frame mapping, collisions and what is exact
// -------------------------------------------
// The mapping itself is animation_retime's: exact integer arithmetic for whole
// rates, long double for fractional ones, and two equal rates are an identity.
//
// Two keys of one track can land on the same frame.  A collision is merged, over
// the whole group, and the group's value is the generic Retime merged-time value
// sampled on the group's original curve; the two outer segments are clipped with
// De Casteljau against the adjacent original segment.  A collision never
// averages and never picks one key's value at random.  For a switch, an enum or
// any other discrete/Step group the merged value is the group's LAST state, and
// the group holding the original frame-zero key keeps frame 0 and its baseline
// value.
//
// A track with no collision keeps every record byte for byte - value, the
// sixteen interpolation bytes, `allocated`, `physicsDisabled` - and only its
// `frame` field changes.  A merged in/out interval is re-quantised into the
// native 0..127 control tuples.  That domain is not closed under clipping, so a
// merged interval can be representable only approximately: the module then keeps
// the closest expressible tuple, counts the record as approximated in the
// Report, and says so.  Nothing here may claim the conversion is lossless, and
// no whole track is ever linearised to make a merge fit.
//
// Rotation is a quaternion.  A merged rotation is the native slerp/eased value
// at the merged time, obtained from the track's own curve; the four components
// are never averaged, and `q` and `-q` are brought into the same hemisphere
// before the value is stored so a merged quaternion never flips its sign.
//
// Bone scale is a nine-component matrix that shares ONE curve timing with its
// key: the module retimes that timing once and never creates nine parallel
// interpolations for it.
//
// Structure, and the rules that keep it valid
// -------------------------------------------
// The native tables are linked slot pools, not frame-indexed arrays.
//
//   * Occupancy is chain membership.  A record is part of its chain when the
//     chain reaches it, which for a non-head slot is `frame != 0`.  The
//     `allocated` byte is the timeline selection and the VMD export mark - it is
//     NEVER used as an occupancy test, and a key that is not marked is converted
//     like any other.
//   * A model's bone pool capacity is dynamic (`mdl::BoneKeyCapacity(model)`),
//     so it is resolved once per model and never assumed to be the initial
//     600000 slots.
//   * The global and accessory tables are 10000-slot linked pools whose frames
//     run to 10000000; they are not indexed by frame and are never resized.
//   * A display key owns two heap payloads (`ikStates`, `selectorStates`).  The
//     conversion preserves per-slot ownership: a surviving slot keeps its own
//     payloads, a merged-away slot's payloads are released exactly once, and a
//     payload is never shared between two surviving slots.
//   * Scaling frames alone cannot reorder a chain, but the chain still has to be
//     reconnected, the per-track cursors cleared, and `model.maxFrame` plus
//     `app.LastRegisteredFrame()` recomputed.
//
// The transaction
// ---------------
// Prepare(MMDApp&, Rates, PlanPtr&, Report&, error)
//
//   Fully read-only.  It examines the project, builds every candidate pool, side
//   table and Report entry, and records the identity and the content of every
//   object it examined.  It may allocate as much as it needs.  On failure it
//   returns false with a message in `error` and leaves `plan` and `report`
//   untouched.  A malformed project - a capacity the pool cannot hold, a broken
//   chain, a non-finite value, an overflow beyond 10000000 frames - is rejected
//   here, so Commit never has to discover it half way.
//
// Commit(MMDApp&, const PlanPtr&, error, beforeFinalize)
//
//   1. Verifies first.  Every pool pointer, side table pointer, model identity,
//      slot and content digest captured by Prepare is compared with the live
//      project.  Any difference - a deleted model, a key added or moved, an
//      edited curve, a different object at a recycled address - fails the commit
//      and leaves the project untouched.
//   2. Writes the prebuilt candidates.  Only the slots that are actually
//      occupied are patched; an empty slot of a 600000-slot pool is never
//      written.
//   3. Calls `beforeFinalize` when one was supplied.  A callback that returns
//      false, or that leaves a message in its argument, rolls the whole
//      conversion back: the native pools, the side tables, `maxFrame` and the
//      scene-modified flag are restored to exactly what they were.  The rollback
//      swaps in the objects Prepare already built, so it performs NO allocation
//      and cannot fail.
//   4. After the callback has returned true only noexcept steps remain: the
//      replaced content is released, merged-away slots are cleared, `maxFrame`
//      and the last registered frame are recomputed, and the scene is marked
//      modified.  No failure is reported after this point because there is
//      nothing left that can fail.
//
// A Plan is committed at most once; a second Commit fails with a message and
// changes nothing.  Rollback is a snapshot swap: it never goes through a save/
// load round trip and never depends on being able to allocate.
//
// Counters and the detail list
// ----------------------------
// The Report is frame_rate_correction::Report, and it is filled the way that
// header requires.  Every counter is PER NATIVE KEY RECORD: a record that holds
// four quaternion components, a position and a scale is one key in
// `inputKeys`, exactly as one morph key is.  The per-track counters that
// animation_retime produces are therefore aggregated once per record, not once
// per component.  `models` and `accessories` count what was examined, and
// `mmvTracks` and `meshCaches` stay 0 because those belong to another module.
//
// `changes` is COMPLETE, not sampled and not capped: it holds one entry for
// every key whose exact frame needed rounding and one for every key that lost
// its slot to a merge, so its length always equals roundedKeys + mergedKeys and
// `detailsTruncated` is always false.  A key that lands exactly on its scaled
// frame is an ordinary move and is not reported; the tolerance is
// animation_retime's own (1e-12 on the exact position), so the list cannot
// disagree with the counts.  Because the list is complete it can be large: if
// its memory cannot be obtained, Prepare FAILS with a message and leaves the
// project and both outputs untouched, because recording fewer entries and
// carrying on would be the silent omission the Report exists to prevent.
//
// Threading
// ---------
// Single-threaded, for the UI thread.  Prepare and Commit must not run while
// playback, an undo transaction or a bake is touching the same project, and one
// MMDApp must not have two plans in flight.
#pragma once

#include "mikumikustudio/frame_rate_correction.hpp"

#include <functional>
#include <memory>
#include <string>

namespace mikumikustudio {

class MMDApp;

namespace native_frame_retime {

// Opaque conversion plan built by Prepare.  It owns the candidate pools, the
// candidate side tables and the snapshots the rollback swaps back in, so the
// caller only has to keep the shared pointer alive between Prepare and Commit.
class Plan;
using PlanPtr = std::shared_ptr<Plan>;

// Inspects `app` for a conversion from `rates.originalFPS` to `rates.currentFPS`
// and fills `plan` and `report`.  It changes nothing: every pool, side table,
// `maxFrame`, the scene-modified flag and the live pose are the same before and
// after a successful call.  Returns false with a localized message in `error`
// and leaves `plan` and `report` untouched when the project cannot be converted.
bool Prepare(MMDApp& app, const frame_rate_correction::Rates& rates, PlanPtr& plan,
             frame_rate_correction::Report& report, std::wstring& error);

// Performs the conversion `plan` describes, in memory and transactionally, as
// described in the header comment.  `beforeFinalize` may be empty; when it is
// not, it is called after the writes and before the conversion is final, and a
// false return rolls everything back with no allocation.
bool Commit(MMDApp& app, const PlanPtr& plan, std::wstring& error,
            const std::function<bool(std::wstring&)>& beforeFinalize = {});

}  // namespace native_frame_retime
}  // namespace mikumikustudio
