// MMS addition: "Edit -> Correct by frame rate..."  (根据帧率修正).
//
// The feature converts every animation frame of the current project from one
// frame rate to another:
//
//     newFrame = originalFrame * currentFPS / originalFPS
//
// anchored at frame 0, so a project that is already at the target rate is left
// alone.  It covers the whole current project: every model, every accessory,
// the camera and light tracks and the MMV effect tracks.
//
// Flow, and where each side sits:
//
//   * The BACKEND (a separate translation unit) owns Prepare/Apply/Available
//     and RequestSaveAs.  Prepare inspects the project and returns an opaque
//     Plan plus the Report; it must not change the project.  Apply performs the
//     conversion in memory, transactionally: on failure the project is exactly
//     what it was and the reason comes back in `error`.  The converted project
//     stays in memory; the UI never writes a file.
//   * RequestSaveAs is called by the UI exactly once, right after a successful
//     Apply, so the backend can run the real Save As flow.  Its outcome is the
//     backend's business and the UI reports nothing about it: the report
//     deliberately never says the project was saved.  The UI does not know or
//     guess any menu id.
//   * The UI (frame_rate_dialog.cpp) owns the floating window, the three
//     languages, the parameter parsing, the report text and the lifetime of the
//     Plan.  Changing either rate drops the prepared Plan, Preview only shows
//     the summary, Apply is armed only by a successful Prepare, and Apply is
//     never called without the user pressing it.
//
// Plan is deliberately opaque here, so the UI can only pass it around and the
// backend is free to put anything in it.
//
// The one thing the conversion cannot promise is that every key keeps its own
// time slot: keys that land on the same frame merge, and the merged value is
// derived from the group, so a compression can lose changes.  The UI states
// that in every language; nothing here or in the backend may claim the
// conversion is lossless, or that the result has been saved.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mikumikustudio {
class MMDApp;

namespace frame_rate_correction {

// The two values the user edits.  Both are frame rates per second and both may
// be fractional (29.97 is the case this exists for); the backend decides what a
// reasonable range is, the dialog accepts any finite value in (0, 1000].
struct Rates {
    double originalFPS = 30.0;
    double currentFPS = 30.0;
};

// One key the conversion affected, for the detailed report.  The conventions,
// frozen for the native and MMV subtasks that fill this in:
//
//   * `channel` names the source the key belongs to.  Every native track
//     (bone, morph, camera, light, self shadow, gravity, accessory, ...) and
//     every MMV track is addressed by this name; the backend owns the spelling,
//     the UI prints it verbatim.
//   * `originalFrame` is the key's own frame before the conversion and
//     `exactFrame` is that frame times currentFPS / originalFPS, unrounded.
//   * ChangeKind::Rounded - this key's frame needed rounding: `exactFrame` is not
//     an integer frame, so `newFrame` is round(exactFrame) and the offset
//     `exactFrame - newFrame` is what makes it a rounding.  A proportional move
//     that lands exactly on a frame (30 -> 60, 30 -> 15, ...) is a normal move
//     and is NOT reported.  The backend applies the tolerance (a key counts as
//     rounded when the offset is larger than it), which is the same rule
//     animation_retime counts with.  `mergeFirstFrame` and `mergeLastFrame`
//     stay 0.
//   * ChangeKind::Merged - the key lost its own time slot because the group it
//     belongs to collapsed onto one frame: `newFrame` is that shared target and
//     `mergeFirstFrame`..`mergeLastFrame` is the group's range of original
//     frames.  This covers the second key of a group through the last one; the
//     group's first key keeps its slot and is never reported as Merged.
//   * The two kinds are independent, not exclusive.  One input key can produce
//     both a Rounded entry (its frame needed rounding) and a Merged entry (it
//     lost its slot), so a complete list may hold one of each for the same
//     channel, and neither list is a subset of the other.
//   * Keeping the value of a merged key is not a claim about its time slot: the
//     merged value is derived from the whole group.
//   * A complete detail list holds one entry per rounded key and one per
//     merged-away key, so the Rounded count matches `roundedKeys` and the
//     Merged count matches `mergedKeys`.
struct FrameChange {
    std::wstring channel;
    std::int32_t originalFrame = 0;
    std::int32_t newFrame = 0;
    double exactFrame = 0.0;
    enum class ChangeKind { Rounded, Merged };
    ChangeKind kind = ChangeKind::Rounded;
    std::int32_t mergeFirstFrame = 0;
    std::int32_t mergeLastFrame = 0;
};

// What one Prepare examined and what the conversion would change.  The UI shows
// the counts as a summary after Preview and the whole thing, including
// `changes`, after a successful Apply:
//
//   inputKeys / outputKeys  keyframes before / after the conversion,
//   mergeGroups / mergedKeys  groups of keys that collide on one frame and how
//                            many keys they merge away,
//   roundedKeys            keys whose frame needed rounding, that is whose
//                          exact frame was not an integer frame.  A normal
//                          proportional move is not a rounding,
//   models / accessories / mmvTracks / meshCaches  how much was examined,
//   maxSampledValueError   the largest value error introduced by resampling,
//   worstChannel           the channel name that error belongs to (empty when
//                          the conversion is exact),
//   changes                the per-key detail, see FrameChange.  Rounded and
//                          Merged entries are independent, so the same key may
//                          appear once in each,
//   detailsTruncated / omittedDetailCount  retained for reporting a rejected or
//                          externally supplied incomplete report. Production
//                          preparation builds every detail or fails without
//                          changing the project; it never applies a detail cap.
struct Report {
    std::uint64_t inputKeys = 0;
    std::uint64_t outputKeys = 0;
    std::uint64_t mergeGroups = 0;
    std::uint64_t mergedKeys = 0;
    std::uint64_t roundedKeys = 0;
    std::uint64_t models = 0;
    std::uint64_t accessories = 0;
    std::uint64_t mmvTracks = 0;
    std::uint64_t meshCaches = 0;
    double maxSampledValueError = 0.0;
    std::wstring worstChannel;
    std::vector<FrameChange> changes;
    bool detailsTruncated = false;
    std::uint64_t omittedDetailCount = 0;
};

// Opaque conversion plan.  Only the backend constructs one; the UI stores the
// shared pointer, passes it back to Apply and drops it when the parameters
// change or after a successful Apply.
class Plan;
using PlanPtr = std::shared_ptr<Plan>;

// Inspects `app` for a conversion from `rates.originalFPS` to `rates.currentFPS`
// and fills `plan` and `report` (including the detail list, so the UI can show
// it after Apply without asking again).  Returns false with a message in
// `error` and leaves both outputs untouched; a successful call must not change
// the project.
bool Prepare(MMDApp& app, const Rates& rates, PlanPtr& plan, Report& report,
             std::wstring& error);

// Performs the conversion described by `plan`, in memory, transactionally.
// Returns false with a message in `error` and leaves the project unchanged on
// any failure.  A plan must not be applied twice.  On success the caller is
// expected to call RequestSaveAs; Apply itself writes nothing.
bool Apply(MMDApp& app, const PlanPtr& plan, std::wstring& error);

// Runs the real "save as" flow for the converted project.  Called by the UI
// exactly once after a successful Apply, with no second confirmation and no
// return value: whether the user saves, cancels or fails is reported by the
// host's own save flow, and the report must not claim the project was saved.
void RequestSaveAs(MMDApp& app);

// True when the feature may act right now (nothing is playing or baking, a
// project is loaded, ...).  The backend owns the meaning; the menu item and the
// dialog's Preview button are gated on it.
bool Available(MMDApp* app);

// Shows (or, with visible == false, only creates) the modeless floating window
// for `app`.  The window is created once per application, centered on the main
// window, and re-shown on later calls.  The window's own language follows
// app->EnglishUI().
void ShowDialog(MMDApp* app, bool visible = true);

// Adds the command to the end of the Edit submenu (GetSubMenu(mainMenu, 1)) on
// the first call and updates its text and enabled state afterwards, so a
// repeated call never adds a second item.  An already open dialog is
// re-localized as well, which makes this the hook for a language change.
void RefreshMenu(MMDApp* app);

// Routes the menu command: true when `id` is kMenuCommand (the dialog is then
// shown), false for every other id so the caller's switch keeps looking.
bool HandleCommand(MMDApp* app, unsigned id);

// Command id of the Edit menu item.  32920 is outside every range the port
// already occupies (304..308 transparent sort, 400..567 panel controls,
// 32000..32227 skinning, 32600 MMV, 32800..32804 baking, 32900..32906 curve
// launchers) and outside 32910..32919, so it collides with nothing.
constexpr unsigned kMenuCommand = 32920;

}  // namespace frame_rate_correction
}  // namespace mikumikustudio
