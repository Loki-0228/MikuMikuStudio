#pragma once
// Native channel baking behind "Baking -> Bake keyframes".
//
// The bake dialog follows the page that is open in the main window: the
// camera/light/accessory page (MMDApp::CameraMode() != 0) offers the four
// scene-wide tracks and every loaded .x object, and the model page offers the
// tracks of the one selected model only.  Scene-wide tracks are read and
// written through curve_host, so the floating curve editor, the PMM extension
// payload and the native timeline stay the single source of truth and never
// drift apart.  Model tracks (morph weights and the 表示/IK display track) are
// read and written in the model's own native key pools.
//
// This module adds no curve formula of its own: every resampled value comes
// from animation_curve::Evaluate over a track built from existing storage, and
// the per-frame rule (a Step key for discrete tracks, a Linear key otherwise)
// is the one animation_curve::Bake already applies.  Bake itself is not called
// because it refuses a partial range, while baking here must keep every key
// outside [firstFrame, lastFrame] untouched.
//
// Nothing in this module touches MMV: native .x curves and VMD/PMM data bake
// with no MMV.dll loaded, and a physical simulation is never replaced by a
// local Bullet run here.  Physics stays in bake::Compute/Commit, and the caller
// composes both transactions through Prepare/Commit/Rollback/Discard.
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mikumikustudio { class MMDApp; }

namespace mikumikustudio::channel_bake {

// Which page of the main window owns the bake.
enum class Scope {
    Scene,  // camera / light / accessory page (optflag[0] != 0)
    Model   // model page (optflag[0] == 0)
};

// One bakeable track category.  Scene carries the global tracks and one entry
// per loaded .x object; Model carries the selected model's own tracks.
enum class Kind {
    Camera,
    Light,
    SelfShadow,
    Gravity,
    Accessory,
    // Model page.  Bones is not rewritten here: the existing
    // bake::Compute/Commit path owns the bone pools, and this entry only tells
    // the caller that the bone channel was requested.  Morph and Display are
    // rewritten by this module in the model's native key pools.
    Bones,
    Morph,
    Display,
};

// One sub-parameter of a track, in the order curve_host or the native pool
// reports it.  A discrete field is baked with Step keys.
struct Field {
    std::wstring id;
    std::wstring label;
    bool discrete = false;
    double minimum = -1.0e12;
    double maximum = 1.0e12;
};

// A selectable track.  `slot` is the accessory slot, the morph index, or 0.
struct Group {
    Kind kind = Kind::Camera;
    int slot = 0;
    std::wstring label;
    std::vector<Field> fields;
};

// How much of a track a bake covers.  A source a caller ticked is never widened
// to the whole model on its own: Whole means every entry of the track, Selected
// means the entries the scene itself marks as selected (the model's bone
// selection), and Explicit means exactly `indices`.
enum class Coverage {
    Whole,
    Selected,
    Explicit,
};

// One selected track and, inside it, the sub-parameters to resample.  A field
// left false keeps its keys exactly as they are.
struct Choice {
    Kind kind = Kind::Camera;
    int slot = 0;
    std::vector<bool> fields;
    // Which entries of the track the bake covers.  Only the bone channel reads
    // it today: every other channel covers its whole track, and its own scope
    // (which accessory, which morph, which IK chain) is `slot` and `fields`.
    Coverage coverage = Coverage::Whole;
    // Bone indices, ascending and unique, for Kind::Bones with
    // Coverage::Explicit.  A checked source list fills this in; it is never
    // inferred from the model.
    std::vector<int> indices;
};

struct Selection {
    std::vector<Choice> choices;
    bool Empty() const;
    bool Has(Kind kind, int slot) const;
    const Choice* Find(Kind kind, int slot) const;
    // Enables a track, keeping the sub-parameter mask of an existing entry.
    // An entry created this way has every sub-parameter enabled.  `fields`
    // holds one flag per Field of the group; an entry whose mask is empty means
    // every sub-parameter.
    Choice& Touch(Kind kind, int slot, std::size_t fieldCount);
    void SetField(Kind kind, int slot, std::size_t field, bool on);
    void Remove(Kind kind, int slot);
    // True when the track is selected and at least one sub-parameter is on.
    bool Active(Kind kind, int slot) const;
};

struct Request {
    Scope scope = Scope::Model;
    int firstFrame = 0;
    int lastFrame = 0;
    int step = 1;
    Selection selection;
    // Optional replacement of the Morph channel's resample source.  When set,
    // every selected morph's baked values come from this callback (the MMV
    // layer cutoff OUTPUT) instead of the native morph track, while the keys
    // outside the range and the range boundary shapes stay exactly as they are.
    // Returning false for a frame refuses the bake for that morph: a missing
    // layer value is never filled from the old track silently.
    std::function<bool(int morph, int frame, float& value)> morphSource;
};

enum class Error {
    None,
    InvalidRequest,  // range, step or selection the dialog would refuse
    Unavailable,     // track storage is missing (no pool, no per-slot arrays)
    NoSelection,     // nothing usable was selected
    Capacity,        // a native pool cannot hold the baked keys
    Identity,        // the object changed between Prepare and Commit
    Memory,
    // A cubic range boundary cannot be reproduced exactly in the key format:
    // the subdivided piece would need a handle weight outside [0.0001, 0.9999],
    // or one whose sum with its neighbour exceeds 0.9999.  The curve outside the
    // range would change if the bake went ahead with sampled keys, so nothing is
    // written and the caller has to move the range or fix the curve.
    Unrepresentable,
};

// A prepared, not yet applied bake.  Prepare reads and resamples on private
// copies only; the live scene, model pools and curve_host state are untouched
// until Commit, and Commit either applies every selected track or applies none.
class Plan {
public:
    Plan();
    ~Plan();
    Plan(Plan&&) noexcept;
    Plan& operator=(Plan&&) noexcept;
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;
    // True while a prepared bake is held.
    bool Valid() const noexcept;
    // Opaque state; only channel_bake.cpp defines it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    friend Error Prepare(MMDApp&, const Request&, Plan&, std::wstring&);
    friend Error Commit(MMDApp&, Plan&, std::wstring&);
    friend bool Rollback(MMDApp&, Plan&) noexcept;
    friend bool PreparedBones(const Plan&, std::vector<int>&);
    friend void Discard(Plan&) noexcept;
};

// The scope the main window is showing right now.
Scope CurrentScope(MMDApp& app);

// Lists what the given scope can bake, in the order the dialog shows it.
// Labels follow the UI language of `app`.
Error Enumerate(MMDApp& app, Scope scope, std::vector<Group>& groups,
                std::wstring& message);

// Selects every sub-parameter of the tracks a fresh dialog should start with:
// the camera track on the scene page, the bone channel on the model page.
// Everything else stays available but unselected, so opening the dialog never
// silently rewrites tracks the user did not ask for.
void DefaultSelection(MMDApp& app, Scope scope, const std::vector<Group>& groups,
                      Selection& selection);

// Short human-readable list of what is selected, for the dialog summary line.
std::wstring Summary(MMDApp& app, Scope scope, const std::vector<Group>& groups,
                     const Selection& selection);

// Highest frame of the scope's own tracks, for the "Last" button.
int LastFrame(MMDApp& app, Scope scope);

// Entire native scene timeline, including every loaded model, for bake ranges.
int SceneLastFrame(MMDApp& app);

// Resolves the bone channel of `selection` against the selected model and
// returns, ascending and unique, exactly the bone indices a bake would cover.
// Coverage::Whole is every bone of that model, Coverage::Selected is the set
// the scene marks as selected (the same set bake::Compute uses with
// selectedOnly), and Coverage::Explicit is the recorded list, checked against
// the model so an out-of-range index is refused instead of being clamped.
// An empty result is not an error: it means the channel covers no bone.
// A selection without a bone channel leaves `bones` empty and returns
// Error::None.
Error ResolveBones(MMDApp& app, const Selection& selection, std::vector<int>& bones,
                   std::wstring& message);

// Reads and resamples every selected track onto private copies.  Anything that
// would make the bake fail - an invalid range, a missing pool, a missing
// per-slot array, a pool that cannot hold the baked keys, or an object that
// changed since Enumerate - is reported here, before a single key is written.
Error Prepare(MMDApp& app, const Request& request, Plan& plan,
              std::wstring& message);

// Applies a prepared bake.  The identity of every target object is verified
// again first; on any failure every track written so far is restored, so a
// failed commit leaves the scene byte-identical.  The plan keeps its rollback
// data afterwards, so a caller that combines this with another transaction
// (bake::Commit for the bone pools) can still undo it.
Error Commit(MMDApp& app, Plan& plan, std::wstring& message);

// Restores everything a successful Commit wrote.  True when the plan had been
// committed and the restore succeeded.
bool Rollback(MMDApp& app, Plan& plan) noexcept;

// The bone indices a prepared bake resolved, so a caller that also drives the
// bone pools (bake::Compute/Commit) and the per-bone FX toggle keys writes
// exactly the checked bones and nothing else.  False when no bone channel was
// prepared.
bool PreparedBones(const Plan& plan, std::vector<int>& bones);

// Releases the prepared data (cancel).
void Discard(Plan& plan) noexcept;

// Localized text of an error, for a message box.
const wchar_t* ErrorText(MMDApp& app, Error error);

}
