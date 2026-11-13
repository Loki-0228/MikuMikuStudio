#pragma once
#include "mikumikustudio/animation_curve.hpp"
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Shared native editor, statically linked into the host and MMV. These C++
// callbacks stay inside the module that creates them; none cross the DLL ABI.
namespace mikumikustudio::curve_editor {
// Comparison display modes.  The host stores exactly these integers through
// Source::readComparisonMode and Source::writeComparisonMode.  The window
// offers the three display modes as three direct buttons; kCompareOff is
// internal only - it is what a source without comparison support holds and
// what a stored "no comparison" value is read as, so no window ever shows an
// off entry. Plain sources still draw their visible background channels;
// this mode only disables comparison for sources with comparison controls.
constexpr int kCompareOff = 0;        // comparison controls off (internal)
constexpr int kCompareSameGroup = 1;  // curves of the main curve's own group
constexpr int kCompareAllGroups = 2;  // curves of the four groups
constexpr int kCompareFree = 3;       // curves ticked in the channel list
constexpr int kCompareModeCount = 4;

struct Collection {
    std::string id;
    std::wstring label;
};
// Exact native-camera evaluation retained after inserting authored keys. The
// displayed scalar handles remain editable; changing an endpoint/handle makes
// that interval's saved geometry cease to match and releases its native slice.
struct NativeCameraInterval {
    std::int32_t first = 0, last = 0, sourceFirst = 0, sourceLast = 0;
    double sampleFirst = 0, sampleLast = 0, sourceValueFirst = 0, sourceValueLast = 0;
    std::array<double,4> controls{};
    std::array<double,6> geometry{}; // a.value/outWeight/outSlope, b.value/inWeight/inSlope
    // Old/native intervals hold across a one-frame cut. MMSN can explicitly
    // retain a continuous authored interval and its near-diagonal timing rule.
    bool oneFrameHold = true;
    bool transportTiming = false;
    bool operator==(const NativeCameraInterval& other) const noexcept {
        return first==other.first&&last==other.last&&sourceFirst==other.sourceFirst&&sourceLast==other.sourceLast&&
            sampleFirst==other.sampleFirst&&sampleLast==other.sampleLast&&sourceValueFirst==other.sourceValueFirst&&
            sourceValueLast==other.sourceValueLast&&controls==other.controls&&geometry==other.geometry&&
            oneFrameHold==other.oneFrameHold&&transportTiming==other.transportTiming;
    }
};
struct Channel {
    std::string id;
    std::wstring label, unit;
    animation_curve::Track track;
    double base = 0;
    double minimum = -1.0e12, maximum = 1.0e12;
    // Display flag: the channel list tick.  A curve that is not selected is not
    // drawn (its own main-curve drawing included) and takes no part in a
    // comparison set; it is also the bake scope of the bake button. The tree
    // checkbox toggles it; clicking the channel activates it and shows it.
    bool selected = true;
    // Expression group 0..3, or -1 for a curve that takes no part in group
    // comparison.  Appended after `selected`, so every existing aggregate
    // initialisation of Channel stays valid; a curve that leaves it at -1 is
    // only reachable in the free mode.
    int group = -1;
    // Native transform lanes can share an easing curve, and require nonlinear
    // pose sampling (quaternion rotation or matrix scale) for the value graph.
    std::string interpolationGroup;
    std::vector<std::array<double, 4>> nativeControls;
    std::vector<std::array<double, 4>> controlGeometry;
    std::function<animation_curve::Sample(const Channel&, double)> sample;
    // Ordered tree ancestors. IDs are stable within each source; labels may
    // change with the UI language. Empty paths remain root-level channels.
    std::vector<Collection> collections;
    std::vector<NativeCameraInterval> preservedCameraIntervals;
    // Native bone lanes share whole keyframes. Moving one lane retimes the
    // whole group, while its values remain part of the original native pose.
    std::string keyFrameGroup;
};
struct Source {
    std::string id;
    std::wstring label;
    // Both callbacks validate lifetime/identity. Failed writes leave state intact.
    std::function<bool(std::vector<Channel>&, std::wstring&)> read;
    std::function<bool(const std::vector<Channel>&, std::wstring&)> apply;
    // Optional explicit motion actions, invoked with the editor as dialog owner.
    std::function<bool(void*, std::wstring&)> readMotion;
    std::function<bool(void*, const std::vector<Channel>&, std::wstring&)> writeMotion;
    // Optional.  Returns the source name for a UI language (0 Japanese,
    // 1 English, 2 Chinese).  When it is set the editor calls it on every
    // language change and keeps the result as the displayed label; a source
    // that leaves it empty keeps the plain label.  Appended last, so existing
    // aggregate initialisation of Source stays valid.
    std::function<std::wstring(unsigned)> localizedLabel;
    // Optional comparison display metadata, appended last so every existing
    // Source stays valid.  The editor offers the comparison controls only when
    // comparisonEnabled is set or one of the two mode callbacks is present.
    // Plain sources draw their visible channels behind the active curve without
    // adding expression-group comparison controls.
    //
    // The main curve is always the current channel and stays editable.  The
    // other members only describe which already registered curves are drawn
    // behind it; switching a mode never writes curve data.
    bool comparisonEnabled = false;
    // Channel id that becomes the main curve when the editor enters this
    // source.  An empty or unknown id keeps the current channel.
    std::string initialMainChannel;
    // Mode the host remembers, or a negative value when it has nothing stored.
    std::function<int()> readComparisonMode;
    // Stores a mode change.  A false return keeps the previous mode and puts
    // the error text into the status line, so a host that cannot persist the
    // choice never diverges from what the window shows.
    std::function<bool(int, std::wstring &)> writeComparisonMode;
    // Live host checkbox for the 0..127 MMD panel: link its adjacent interval
    // in one transaction. Value-curve handles follow their Free/Aligned mode.
    std::function<bool()> linkAdjacentControls;
    // Slash-joined collection IDs. Expand this branch and its ancestors when
    // entering a source; keep other bone branches collapsed.
    std::string initialExpandedCollection;
    // Native selection is addressed by frame, never by mutable pool slot.
    std::function<std::vector<int>(const std::string&)> readKeySelection;
    std::function<void(const std::string&, const std::vector<int>&)> writeKeySelection;
    // Optional native subdivision. Works on a detached channel set; the
    // normal apply callback remains the only publication/history boundary.
    std::function<bool(std::vector<Channel>&, std::size_t, int, std::size_t&, std::wstring&)> subdivide;
    // Opt in to Session::history. All opted-in sources share the host journal;
    // no local undo snapshots are retained. Missing callbacks disable editing.
    bool externalHistory = false;
};
struct Theme {
    // COLORREF values in MMD's in-memory order, already parsed by the host.
    std::array<std::uint32_t,35> colors{};
    std::uint32_t text=0;
    bool enabled=false;
};
struct HistoryState {
    std::uint64_t revision = 0;
    bool canUndo = false, canRedo = false;
};
struct ExternalHistory {
    // Read-only, including during temporary edit locks. A false result means
    // the current binding is unavailable, not that its history is empty.
    std::function<bool(HistoryState&, std::wstring&)> query;
    // True means exactly one traversal committed. The editor only re-reads
    // afterwards; refresh failure never repeats the traversal. False/throw
    // must leave the journal unchanged. The host enforces its mutation gate.
    std::function<bool(bool redo, std::wstring&)> traverse;
};
struct Session {
    std::string id;
    std::vector<Source> sources;
    std::size_t initialSource = 0;
    unsigned language = 2; // Japanese 0, English 1, Chinese 2.
    std::function<double()> currentFrame;
    std::function<void(int)> seek;
    std::function<bool()> isPlaying;
    std::function<void(bool)> setPlaying;
    Theme theme;
    ExternalHistory history;
};
// Modeless Win32 window. Background tests may create it hidden.
void* Open(void* owner, Session session, bool visible = true);
void CloseOwned(void* owner);
// An editor session can be replaced while a native file picker pumps messages.
// An empty token means window is not a registered editor. Locking it never
// owns an editor or extends validity; compare against the current registration.
std::weak_ptr<void> CaptureWindowSession(void* window);
bool IsWindowSessionCurrent(void* window, const std::weak_ptr<void>& session);
// Includes the modeless editor and its child controls. Its keyboard messages
// must not also reach the owner's per-frame editing shortcuts.
bool OwnsWindow(void* owner, void* window);
void SetLanguage(void* owner, unsigned language);
// Call on the UI thread after loading or changing the host's interface scale.
// Also sets the percentage used by editors subsequently opened on that thread.
void RefreshInterfaceScale(int percent);
// Queues a read of a matching source in an already open editor. Call on the
// owner's UI thread after native registration; this never writes key data.
void NotifySourceChanged(void* owner, const std::string& sourceId);
// Coalesces native command completion notifications on the same UI thread.
// Active source data is read automatically; inactive sources read on entry.
// Best effort: command completion and stack unwinding must never throw for
// a deferred UI refresh; pre-write checks also read the source directly.
void NotifyOwnedChanged(void* owner) noexcept;
bool ReadChannelControls(const Channel&, std::size_t endKey, std::array<double, 4>&);
bool SetChannelControls(Channel&, std::size_t endKey, const std::array<double, 4>&, std::string& error);
// Uses the exact checked-in curve-launcher SVG geometry and system colours.
// hdc is an HDC; rectangles and size are physical pixels supplied by the caller.
void DrawLauncher(void* hdc, int x, int y, int width, int height,
                  bool pressed, bool disabled, bool focused);
// Host command IDs are outside legacy ranges and are routed explicitly.
constexpr unsigned kAccessoryCurve = 32900;
constexpr unsigned kAccessoryMotionRead = 32901;  // retired panel button; id reserved
constexpr unsigned kMorphCurve = 32902;
constexpr unsigned kAccessoryMotionMenu = 32903;
constexpr unsigned kInterpolationCurve = 32907;
}
