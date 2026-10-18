#pragma once
#include "mikudancestudio/build_profile.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
#include "mikudancestudio/pmm_extension.hpp"
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace mikudancestudio { class MMDApp; }
namespace mikudancestudio::plugins::resource_budget { class Collector; }
namespace mikudancestudio::curve_host {
enum class Kind { Camera, Light, Shadow, Gravity, Accessory };
bool Read(MMDApp&, Kind, int slot, std::vector<curve_editor::Channel>&, std::wstring&);
bool Write(MMDApp&, Kind, int slot, const std::vector<curve_editor::Channel>&, std::wstring&);
// Private-copy subdivision. Native camera intervals keep their original float
// evaluator; unrelated scalar channels use the common Bezier subdivision.
bool InsertPreservingShape(std::vector<curve_editor::Channel>&, std::size_t channel, int frame,
                           std::size_t& index, std::wstring& error);
// Converts only channels carrying exact native slices to integer-frame scalar
// proxies for the existing VMD writer. Does not change the standard VMD codec.
bool PrepareMotionChannels(const std::vector<curve_editor::Channel>&,
                           std::vector<curve_editor::Channel>&, std::wstring& error, bool selectedOnly = true,
                           std::uint64_t maximumSampleBytes = 64ull * 1024 * 1024);
// Carries exact native sampling through a scalar retime with one-to-one keys.
// Merging/deleting exact slice boundaries requires an explicit bake first.
bool RetimePreservedChannel(const curve_editor::Channel& source,
                            curve_editor::Channel& transformed, std::wstring& error);
void Apply(MMDApp*, Kind, double frame, int slot = 0);
bool Has(MMDApp*, Kind, int slot = 0);
// Legacy registration only: reconcile an explicit native frame on a detached
// authoring copy, then publish it. May throw before authoring publication;
// native key registration has already happened. Controlled history callers
// must use TrackAuthoring::Prepare instead, never this post-write operation.
void ReconcileRegisteredFrame(MMDApp&, Kind, int slot, int frame);
// Same legacy-only operation for an entire import/paste. One detached track
// copy and one reconciliation regardless of how many rows were registered.
void ReconcileRegisteredFrames(MMDApp&, Kind, int slot, const std::vector<int>& frames);
// Best-effort bridge for completed, unbound legacy native registration only.
// It may update authoring and must never be used after a controlled commit.
void NotifyLegacyRegistered(MMDApp*, Kind, int slot, int frame) noexcept;
// Collects completed legacy registrations until the next curve read/evaluation
// or scope exit. Nested scopes for one app share the outermost collector.
// Construction does not allocate; live native keys remain owned by the app.
class LegacyRegistrationBatch {
    MMDApp* app_ = nullptr;
    LegacyRegistrationBatch* previous_ = nullptr;
    std::array<std::vector<int>, 259> frames_;
    bool flushing_ = false;
    static thread_local LegacyRegistrationBatch* active_;
    void Flush() noexcept;
    friend void NotifyLegacyRegistered(MMDApp*, Kind, int, int) noexcept;
    friend void FlushLegacyRegistrations(MMDApp*) noexcept;
public:
    explicit LegacyRegistrationBatch(MMDApp*) noexcept;
    ~LegacyRegistrationBatch() noexcept;
    LegacyRegistrationBatch(const LegacyRegistrationBatch&) = delete;
    LegacyRegistrationBatch& operator=(const LegacyRegistrationBatch&) = delete;
    LegacyRegistrationBatch(LegacyRegistrationBatch&&) = delete;
    LegacyRegistrationBatch& operator=(LegacyRegistrationBatch&&) = delete;
};
// Read/evaluation boundary: synchronizes queued frames once per affected track
// without moving the playhead or writing native keys. Retains batch capacity.
void FlushLegacyRegistrations(MMDApp*) noexcept;
void Reset(MMDApp*);
void ForgetAccessory(MMDApp*, int slot);
// Keeps precisely one accessory's authored curves while its slot is empty.
// Prepared exchanges require serialized edits and retain the owner until used.
// The application and native pools must outlive every exchange. Publish before
// replacing the native slot/pool, within the same scene mutation gate.
class AccessoryAuthoring {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    class Exchange {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        friend class AccessoryAuthoring;
        explicit Exchange(std::unique_ptr<Impl>);
    public:
        ~Exchange();
        bool IsCurrent() const noexcept;
        bool Publish() noexcept;
    };
    AccessoryAuthoring(MMDApp&, int slot, bool initiallyPresent = true);
    ~AccessoryAuthoring();
    AccessoryAuthoring(const AccessoryAuthoring&) = delete;
    AccessoryAuthoring& operator=(const AccessoryAuthoring&) = delete;
    std::unique_ptr<Exchange> Prepare(bool present);
#if !MMD_ORIGINAL_PROFILE
    void MeasureResources(plugins::resource_budget::Collector&) const;
#endif
};
void Capture(MMDApp&, pmm_extension::Document&);
void Load(MMDApp&, const pmm_extension::Document&);
void Open(MMDApp*, bool visible = true);
// Opens the shared editor on the interpolation page's current camera channel
// (camera mode) or selected bone (model mode).
void OpenInterpolation(MMDApp*, bool visible = true);
// Notifies an already open editor after native key registration completes.
void NotifyRegistered(MMDApp*, Kind, int slot = 0);
void NotifyModelRegistered(MMDApp*, bool morph);
void CreateControls(MMDApp*, void* window);
void LayoutControls(MMDApp*);
void Localize(MMDApp*);
void ShowControls(MMDApp*, bool show);
void DrawLabels(MMDApp*, void* hdc);
bool Command(MMDApp*, unsigned id);

// ---------------------------------------------------------------------------
// Expression (morph) curves.  One model, one Source: every expression of the
// four user panels is a channel, and Channel::group is morph_curve::PanelGroup
// of its panel, so the editor's same-group / four-group / free comparison modes
// work directly.  A model's hidden system morphs (panel 0) stay out of the
// list, which is the same protection the timeline already keeps for them.
// ---------------------------------------------------------------------------

// Launcher button of each of the four expression-panel selectors (504, 509, 514
// and 519).  32903 belongs to the accessory VMD menu item, so the four ids are
// 32902, 32904, 32905 and 32906.
inline constexpr int kMorphLaneCount = 4;
inline constexpr unsigned kMorphCurveLane[kMorphLaneCount] = {
    curve_editor::kMorphCurve, 32904u, 32905u, 32906u};

// Opens the shared curve editor on the selected model's expressions, starting
// on the morph that panel `lane` currently has selected.
void OpenMorph(MMDApp*, int lane, bool visible = true);
// Re-places the four launcher buttons after the expression band moved.  Called
// once, after the band's own MoveWindow calls, because the band narrows the
// selectors and must not be narrowed twice.
void LayoutMorphControls(MMDApp*);

// The rules above, exposed so a background test can drive them without a window
// and without a renderer. ReadMorphChannels enumerates visible panels and
// reads them in one synchronized morph_curve::ReadMany batch.
bool ReadMorphChannels(MMDApp&, std::vector<curve_editor::Channel>& out,
                       std::wstring& error);
// Tracks that differ from `baseline`, matched by channel id.  Only these reach
// the backend, so merely browsing the window never registers an untouched
// native curve as authored.  An empty result means "nothing to write".
std::vector<animation_curve::NamedTrack> ChangedMorphTracks(
    const std::vector<curve_editor::Channel>& next,
    const std::vector<curve_editor::Channel>& baseline);

// Identity of the model a morph source was opened on.  A raw pointer is not
// enough: a deleted model's allocator can hand the same address to a different
// model, and then both the slot lookup and the address comparison still match.
// The backend keys its authoring on the model pointer, the morph table
// pointers, the morph count and a digest of every morph's name, panel and type;
// this struct captures all of that and adds the bone table, the bone count and
// the resolved model path, so the host is at least as strict as the backend.
struct MorphModelIdentity {
    unsigned char* model = nullptr;
    const void* morphs = nullptr;
    const void* keys = nullptr;
    std::uint64_t keyPoolIdentity = 0;
    std::uint32_t morphCount = 0;
    const void* bones = nullptr;
    std::uint32_t boneCount = 0;
    wchar_t path[256]{};
    std::uint64_t digest = 0;
};

// Captures the identity of one loaded model.  A null model, or one that is not
// in a slot, yields an identity that matches nothing.
MorphModelIdentity MorphIdentityOf(const MMDApp&, unsigned char* model);
// True only for a live model that is in a slot, is still the selected one and
// still matches every captured field.  Every source callback checks this before
// it dereferences the captured model.
bool MorphIdentityCurrent(const MMDApp&, const MorphModelIdentity&);

// The morph the launcher of `lane` opens on, or an empty id when that panel's
// selector does not currently name a morph of that panel.  An empty result
// disables that launcher, and OpenMorph refuses to open on it, so the editor is
// never left to fall back to the first channel of the list.
std::string MorphLaneInitial(MMDApp&, int lane);

// Comparison mode of the expression sources, persisted in
// Data/curve_editor.ini next to the executable.  Legal values are
// kCompareSameGroup..kCompareFree; anything else reads as kCompareSameGroup.
int ReadMorphComparisonMode();
bool WriteMorphComparisonMode(unsigned language, int mode, std::wstring& error);
// The source's apply path: identity check, diff, then one backend write.
bool WriteMorphChannels(MMDApp&, const MorphModelIdentity&,
                        const std::vector<curve_editor::Channel>& next,
                        const std::vector<curve_editor::Channel>& baseline, std::wstring& error);

// ---------------------------------------------------------------------------
// MMS addition: native frame retime (2026-09-27).
//
// This host keeps an authored layer per global and accessory track, keyed by
// the app and the track, together with a baseline read of the native pool and a
// fingerprint of that pool.  A frame-rate conversion that rewrote only the
// native pools would leave the baseline on the old frames, and the next read
// would reconcile the whole authored curve away.  The conversion therefore
// retimes the authoring as well, through the two calls below.
//
// The authoring of one application is exchanged as a whole table.  Capture
// copies it, AuthoringEntries exposes it for retiming, RebuildAuthoring builds
// the replacement against the already-retimed native pools (which is where the
// new identity and fingerprint come from), and SwapAuthoring exchanges the live
// table with whatever the holder currently carries.  Installing is one swap and
// rolling a commit back is the same swap called a second time, so a rejected
// commit restores the previous authoring without allocating.
// ---------------------------------------------------------------------------
class AuthoringTable {
public:
    AuthoringTable();
    ~AuthoringTable();
    AuthoringTable(AuthoringTable&&) noexcept;
    AuthoringTable& operator=(AuthoringTable&&) noexcept;
    AuthoringTable(const AuthoringTable&) = delete;
    AuthoringTable& operator=(const AuthoringTable&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bool CaptureAuthoring(MMDApp&, AuthoringTable&, std::wstring&);
    friend bool AuthoringEntries(const AuthoringTable&, std::vector<struct AuthoringEntry>&);
    friend bool RebuildAuthoring(MMDApp&, const std::vector<struct AuthoringEntry>&,
                                AuthoringTable&, std::wstring&);
    friend void SwapAuthoring(MMDApp&, AuthoringTable&) noexcept;
    friend bool AuthoringUnchanged(MMDApp&, const AuthoringTable&);
};

// One authored track of one application: the kind, the accessory slot (ignored
// for the four global kinds) and the edited channels with the native baseline
// they were derived from.
struct AuthoringEntry {
    Kind kind = Kind::Camera;
    int slot = 0;
    std::vector<curve_editor::Channel> channels;
    std::vector<curve_editor::Channel> baseline;
};

// Copies the authoring this host holds for `app`.  An app with no authoring
// yields an empty table, which is not an error.  Read-only.
bool CaptureAuthoring(MMDApp& app, AuthoringTable& out, std::wstring& error);
// The authored tracks of the table, in a stable order.  Read-only.
bool AuthoringEntries(const AuthoringTable& table, std::vector<AuthoringEntry>& out);
// Samples authored values into one staged native record. Preserves its frame,
// links and interpolation bytes; the caller supplies a detached record copy.
bool SampleRetimedKey(const AuthoringEntry& entry, void* record, std::size_t bytes);
// Builds the replacement table from retimed entries.  `app` must already carry
// the retimed native pools, because the baseline and the fingerprint are read
// back from them.  An entry whose identity no longer matches the app is
// dropped, exactly as a foreign authoring is dropped on a read.
bool RebuildAuthoring(MMDApp& app, const std::vector<AuthoringEntry>& entries,
                      AuthoringTable& out, std::wstring& error);
// Exchanges the live authoring of `app` with `table`.  Never allocates, never
// throws.
void SwapAuthoring(MMDApp& app, AuthoringTable& table) noexcept;
// True while the live authoring still carries exactly what `table` holds and
// the native pools it was captured against are unchanged.
bool AuthoringUnchanged(MMDApp& app, const AuthoringTable& table);
#if !MMD_ORIGINAL_PROFILE
// Read-only access to a privately planned native track. Every returned record
// has the layout of kind; pointers remain valid through target preparation.
struct NativeKeyView {
    const void* context = nullptr;
    std::size_t capacity = 0;
    const void* (*key)(const void*, std::uint32_t) noexcept = nullptr;
};
// Fully detached replacement for an authored global/accessory curve source.
// Construction validates and mirrors the input into private native records.
// Neither construction nor preparation calls Write or changes a live pool.
class AuthoredReplacement {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    AuthoredReplacement(MMDApp&,Kind,int slot,const std::vector<curve_editor::Channel>&);
    ~AuthoredReplacement();
    bool Changed() const noexcept;
    bool IsCurrent() const noexcept;
    NativeKeyView TargetKeys() const noexcept;
    const std::vector<curve_editor::Channel>& BeforeChannels() const noexcept;
    class Target {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        explicit Target(std::unique_ptr<Impl>);
        friend class AuthoredReplacement;
    public:
        ~Target();
        bool IsCurrent() const noexcept;
        bool Authored() const noexcept;
        bool Sample(double frame,std::array<double,12>&) const;
        // Caller validates all participants first. Map-node exchange only;
        // pass the final native pool, or accessory object, after publication.
        void Publish(void* finalIdentity) noexcept;
    };
    std::unique_ptr<Target> Prepare(const NativeKeyView&,bool forward) const;
    std::uint64_t RetainedBytes() const noexcept;
};
// Retains pristine authoring for one history record. Normal reads may reconcile
// live curves; both history directions still derive from this original layer.
class TrackAuthoring {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    TrackAuthoring(MMDApp&, Kind, int slot = 0);
    ~TrackAuthoring();
    bool IsCurrent() const noexcept;
    // Read-only source plans can also reject changes within the same author
    // lineage. Compares the pristine captured revision and curve contents;
    // unavailable after RetimingFrames has replaced the private seed.
    bool CapturedCurrent() const noexcept;
    class Target {
        struct Impl;
        std::unique_ptr<Impl> impl_;
        explicit Target(std::unique_ptr<Impl>);
        friend class TrackAuthoring;
    public:
        ~Target();
        bool IsCurrent() const noexcept;
        bool Authored() const noexcept;
        // Pose values in Fields order, including discrete bindings. Detached
        // output; no renderer, app writes, synchronization or callbacks in Publish.
        bool Sample(double frame, std::array<double, 12>& out) const;
        // Validate the entire transaction before any write. Pass the final
        // pool pointer for global tracks, or object pointer for an accessory.
        // Publication retains displaced curve storage until Target destruction.
        void Publish(void* finalIdentity) noexcept;
    };
    std::unique_ptr<Target> Prepare(const NativeKeyView&) const;
    // Borrowed frames are absolute, may be repeated or unsorted, and must be
    // present in the forward native target. They are never retained by this
    // object. The history record owns and budgets its input frame container.
    // Undo derives from the original layer without forcing these insertions.
    std::unique_ptr<Target> Prepare(const NativeKeyView&,
        const std::vector<int>& explicitlyRegisteredFrames, bool forward = true) const;
    // Freezes an effective, private source layer on first use; never writes
    // live authoring. Returns the sorted union of authored and baseline frames,
    // including authored-only frames absent from the native pool. Repeated
    // calls keep the same source for exact undo/redo.
    void RetimingFrames(std::vector<int>& out) const;
    // Complete old->new mapping for RetimingFrames(), optionally including
    // additional source native rows. -1 deletes a source frame. Surviving
    // targets must be unique and strictly increasing in source-frame order;
    // every target native row needs a surviving source. Frame zero may be
    // replaced by another source frame. Authored-only targets need no native
    // row. Retains independent lanes, normalized handles and valid interval
    // metadata. Native destination controls must be preserved by the retime.
    // forward=false restores the frozen source, without applying the map.
    // The map is borrowed only during preparation and is budgeted by its owner.
    std::unique_ptr<Target> PrepareRetimed(const NativeKeyView&,
        const std::vector<std::pair<int,int>>& frameMap, bool forward = true) const;
    // True when an original authored lane lacks any explicitly registered
    // frame, even if the shared native row already has identical payload.
    // No authoring means false. Invalid frames or a stale record throw.
    bool HasRegistrationChanges(const std::vector<int>& explicitlyRegisteredFrames) const;
    std::uint64_t RetainedBytes() const noexcept;
};
#endif
}
