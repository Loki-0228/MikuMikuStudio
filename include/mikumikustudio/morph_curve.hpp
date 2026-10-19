#include <algorithm>
#include "mikumikustudio/frame_limits.hpp"
// Native morph (expression) curves for one model's own motion tracks.
//
// A model's morph animation lives in the original 20-byte linked MorphKey
// records ("model+0x2798"): one linear weight per key and no interpolation
// curve, which is all a VMD morph section can carry.  The curve editor works
// with animation_curve::Track (Bezier keys).  This module owns the bridge
// between the two:
//
//   * a model morph track is read as an animation_curve::Track of linear keys,
//   * an authored exact curve is mirrored into the native morph keys at that
//     curve's own key frames, so the standard VMD morph section keeps the same
//     record layout and an original MMD reader still plays a piecewise-linear
//     approximation of it,
//   * the exact curve is persisted next to the native keys: in a PMM v2
//     extension record, and in a block appended to the model VMD after the
//     complete standard body, and
//   * while an authored curve exists, the live morph weight comes from that
//     curve on every frame (morph_curve::Evaluate), overriding the native
//     linear interpolation.
//
// Ordinary keyframe work keeps working, and it no longer destroys anything.
// Registration, deletion, movement, drag editing and the channel bake all write
// the native pool, and every read and evaluation derives an effective curve from
// the pool and the pristine authoring: a native key that appears, disappears or
// changes value wins at its own frame, and the authored ease of the interval
// that key touches falls back to the native (linear) interpolation.  A curve
// that no longer matches the timeline is therefore never applied.
//
// The author's data is kept underneath.  The effective curve is always derived
// afresh from the pristine authored curve and the native chain that curve was
// installed with, so an edit only bends the curve for as long as the pool keeps
// that edit: put the pool back the way it was - a host undo, a hand-deleted key,
// a re-import of the same file - and the authored curve, custom tangents
// included, is exactly in force again.  Nothing here hooks the undo ring; the
// restore is a consequence of the derivation.  What plays, what Read returns and
// what CapturePmm/CaptureVmd write is the effective curve, so a file always
// stores what was heard.  Re-applying a curve from the editor replaces the
// pristine layer.
//
// Identity is checked on every access.  The authoring of a model is keyed by
// the model pointer plus the pointers, count and names of its morph tables, so
// a deleted model, a model reloaded into the same slot, or a different object
// at a recycled address can never inherit another model's curves.
//
// Nothing here depends on MMV.  The module is CPU only: it never touches the
// renderer, the undo rings, the bone/IK state or the bone scale.
//
// Wire layouts (every field little-endian, written one by one, no C++ struct is
// ever dumped to disk).
//
// Payload, the value of one PMM record and the body of one VMD block:
//   char  magic[4]    = "MMSC"
//   uint16 version    = 1
//   uint16 reserved   = 0
//   uint32 curveBytes
//   uint8 curveBytes[] = animation_curve::Encode() of the track list
// Track ids are morph identities, see StableId().  PMM stores absolute model
// frames (the standard PMM body does the same); a VMD block stores the file
// frames of the standard VMD morph records, that is the absolute frame minus
// the save's minimum frame, and a reader adds the import offset back.
//
// Model VMD block, written immediately after the last standard section and
// before the bone-scale extension, so the VMD still ends with the container
// footer bone_scale_io owns and the container's own standard-prefix CRC covers
// this block as well:
//   char   magic[8]   = "MMSMCRV1"
//   uint32 totalBytes             whole block, header included
//   uint32 crc32                  IEEE CRC32 (reflected, poly 0xEDB88320) of the body
//   uint8  body[totalBytes - 16]  the payload above
// A reader that does not know the magic ignores the block, and the declared
// totalBytes lets a reader that finds a damaged body still step over it, so the
// bone-scale tail behind it keeps its boundary either way.

#pragma once

#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/pmm_extension.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mikumikustudio {
class MMDApp;

namespace morph_curve {

// The native morph key pool grows on demand.
// Legacy initial-size constant; never use as a runtime pool bound.
inline constexpr std::size_t kMaximumMorphKeys = 20000;
inline constexpr std::int32_t kMaximumFrame = mikumikustudio::frame_limits::maximum;
// Upper bound of one payload, and therefore of one PMM record value and of one
// VMD block body.  A payload above the limit fails the save visibly instead of
// being truncated.
inline constexpr std::size_t kMaximumPayloadBytes = (std::min)(std::size_t(UINT32_MAX - 64), std::size_t(PTRDIFF_MAX - 64));

// One morph of a model, as offered to a curve editor.
//
// The two flags are deliberately separate, because the native pool keeps two
// different things apart: physical occupancy is the record being linked into
// the morph's chain (the ports' own "free slot" test is `frame != 0`), while the
// `allocated` byte is the timeline's selection / export mark that
// src/io/vmd_save.cpp filters the standard morph section on.  Unmarking a key
// never removes it from the track.
struct MorphInfo {
    int index = -1;
    // MorphRecord::panel: 1 eyebrow, 2 eye, 3 mouth, 4 other.  Panel 0 is a
    // system morph: the model keeps it, the four-panel UI hides it, and the
    // caller must keep the existing hidden-morph protection before offering
    // one for editing.
    std::uint8_t panel = 0;
    std::string id;       // stable identity, see StableId()
    std::wstring label;   // display name for the requested language
    // The native track carries at least one key record.  A continuation counts
    // whatever its mark says; the frame-zero head counts only when it is marked,
    // because that mark is what distinguishes a real frame-zero key from the
    // untouched head every morph starts with.
    bool keyed = false;
    // At least one record carries the export mark, so this morph appears in the
    // VMD morph section of a save.  A morph can be `keyed` and not `marked`.
    bool marked = false;
};

// Identity of one morph inside one model: the morph index plus its twenty raw
// CP932 name bytes.  Hex is used because animation_curve::Encode requires a
// valid UTF-8 id while a model name is not UTF-8; hex keeps the identity
// byte-exact for Japanese, English and Chinese names alike and needs no code
// page conversion.  `name` must point at a MorphRecord::name field.
std::string StableId(int index, const char* name);
bool ParseStableId(const std::string& id, int& index, std::string& name);

// Label of one morph panel for language 0 Japanese, 1 English, 2 Chinese.
const wchar_t* PanelLabel(std::uint8_t panel, unsigned language);

// Curve editor channel group of one morph: eyebrow 0, eye 1, mouth 2, other 3.
// A system morph (panel 0) returns -1: it is not part of the four panels, and
// the caller must keep the existing hidden-morph protection and not offer it
// for editing.  host_ui copies this value into curve_editor::Channel::group, so
// this module never has to see the editor header.
int PanelGroup(std::uint8_t panel);

// Every morph of the model, in model order.  Nothing is filtered: panel 0
// morphs are returned with panel 0.  An unloaded or empty model yields an empty
// list.  This never reads or changes the authored state.
std::vector<MorphInfo> Enumerate(const unsigned char* model, unsigned language = 0);
// The same list for the selected model of the application; an empty list when
// no model is selected.
std::vector<MorphInfo> Enumerate(const MMDApp& app, unsigned language = 0);

// The native weight track of one morph as a curve: one linear key per native
// record, including the frame-zero chain head.  Neither call changes the live
// pose, the morph cursors or the native pool.
bool ReadNative(const unsigned char* model, int morph, animation_curve::Track& out,
                std::string& error);
// The authored exact curve when one exists, the native track otherwise.
bool Read(const unsigned char* model, int morph, animation_curve::Track& out, std::string& error);
// The same read for the selected model of the application, with a localized
// message on failure.  Both Read() overloads take the morph index from
// MorphInfo::index.
bool Read(const MMDApp& app, int morph, animation_curve::Track& out, std::wstring& error);
// One native synchronization for an entire editor read. Results follow the
// requested index order; a failure clears out. Native edits are still checked
// on every call, including writes made without an editor notification.
bool ReadMany(const MMDApp& app, const std::vector<int>& morphs,
              std::vector<animation_curve::Track>& out, std::wstring& error);

// Authored-state queries.  They check the model identity but do not derive
// against the native pool, so they stay cheap enough for a UI refresh.  They
// answer for the effective curve, that is for what is in force right now; the
// pristine layer underneath is retained whether or not it is currently bent.
bool HasCurve(const unsigned char* model, int morph);
bool AnyCurve(const unsigned char* model);
std::size_t CurveCount(const unsigned char* model);
bool HasCurve(const MMDApp& app, int morph);

// Transactional write of exact curves.  Only the morphs named in `tracks` are
// touched; every other native track and every other authored curve survives.
// Each track must carry a StableId() of this model whose index and name bytes
// both match the model morph table, must be continuous, must be valid for
// animation_curve::Validate, must keep a key at frame 0 and must stay inside
// kMaximumFrame.  Every check runs before anything is written and the native
// pool is restored whole on a late failure, so a rejected write changes
// nothing.  The pure overload leaves cursor recovery to the caller; the
// MMDApp overload re-seeks the selected model and sets the scene bookkeeping.
//
// The mirror marks every key it writes for export.  The mark is the selection
// vmd_save filters the standard morph section on, and the VMD curve block
// carries exactly that section's frames, so a key left unmarked would be absent
// from both and would not survive a VMD roundtrip.  Applying a curve is
// therefore an explicit "these keys are part of the motion" action, and it
// selects them; the other morphs of the model are never touched, and their
// marks are never rewritten.
bool Write(unsigned char* model, const std::vector<animation_curve::NamedTrack>& tracks,
           std::string& error);
bool Write(MMDApp& app, const std::vector<animation_curve::NamedTrack>& tracks,
           std::wstring& error);

// Drops the authored curve of one morph, or of the whole model.  Native keys
// are left alone in both cases.
void Forget(unsigned char* model, int morph);
void Forget(unsigned char* model);
// Drops the authoring of every model currently in the application's slots.  A
// null pointer clears the module.  Model disposal calls Forget() through the
// model lifecycle; this entry covers a new project and a full reload.
void Reset(MMDApp* app);
// Carries the authoring of one model onto a copy of it, the way
// bone_scale::CopyState carries the scale into an offline simulator.  The copy
// gets its own identity and its own baselines read from its own chain; a source
// without authoring leaves the destination without any.  The physics bake copy
// (src/physics/physics_bake.cpp, ModelCopy) is the caller this exists for.
void CopyState(const unsigned char* source, unsigned char* destination);

// ---- MMS addition: native frame retime (2026-09-27) ------------------------
// A frame-rate conversion moves the native morph keys, so it has to move the
// authored layers with them: a mirror left on the old frames would make the
// derivation read the conversion as a user edit and fall back to the native
// interpolation, which would silently discard the author's ease.
//
// The layers are exchanged as one object.  CaptureLayers copies the whole
// authoring of a model (the pristine curves, the derived layer, the mirror and
// the observed chain), LayersTracks exposes the pristine curves for retiming,
// RebuildLayers builds the replacement from the retimed curves against the
// model's already-retimed pool, and SwapLayers exchanges the live layers with
// whatever the holder currently carries.  Installing is one swap and rolling a
// commit back is the same swap called a second time, so a rejected commit
// restores the previous authoring without allocating.
class AuthoredLayers {
public:
    AuthoredLayers();
    ~AuthoredLayers();
    AuthoredLayers(AuthoredLayers&&) noexcept;
    AuthoredLayers& operator=(AuthoredLayers&&) noexcept;
    AuthoredLayers(const AuthoredLayers&) = delete;
    AuthoredLayers& operator=(const AuthoredLayers&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bool CaptureLayers(const unsigned char*, AuthoredLayers&, std::string&);
    friend bool LayersTracks(const AuthoredLayers&, std::vector<animation_curve::NamedTrack>&,
                             std::string&);
    friend bool RebuildLayers(const unsigned char*, const std::vector<animation_curve::NamedTrack>&,
                              AuthoredLayers&, std::string&);
    friend void SwapLayers(unsigned char*, AuthoredLayers&) noexcept;
    friend bool LayersUnchanged(const unsigned char*, const AuthoredLayers&);
};

// Copies the authoring of one model.  Returns true with an empty layer set when
// the model has no authoring at all, which is not an error: there is then
// nothing to retime.  Read-only.
bool CaptureLayers(const unsigned char* model, AuthoredLayers& out, std::string& error);
// The pristine curves, one named track per authored morph.  Read-only.
bool LayersTracks(const AuthoredLayers& layers, std::vector<animation_curve::NamedTrack>& out,
                  std::string& error);
// Builds the replacement layers from the retimed pristine curves.  `model` must
// already carry the retimed native pool, because the mirror and the observed
// chain are read back from it.  The returned object replaces the live layers
// wholesale; it never merges with them.
bool RebuildLayers(const unsigned char* model, const std::vector<animation_curve::NamedTrack>& authored,
                   AuthoredLayers& out, std::string& error);
// Exchanges the live layers of `model` with `layers`.  Never allocates, never
// throws.
void SwapLayers(unsigned char* model, AuthoredLayers& layers) noexcept;
// True while the live layers still carry exactly what `layers` was captured
// from and the model still has the same morph tables.
bool LayersUnchanged(const unsigned char* model, const AuthoredLayers& layers);

// Applies the authored exact curves to the live morph weights.  It is called
// after the native morph tracks have been evaluated, right where the model
// frame seek and the playback advance already apply the bone scale.  Without
// an authored curve it does nothing at all.
void Evaluate(unsigned char* model, double frame);

// ---- PMM v2 records ------------------------------------------------------

// "org.mikudancestudio.model.<slot>.morphCurve.v1"
std::string PmmKey(int slot);
// True for any version of this slot's record.  `version` receives the suffix
// after ".morphCurve."; a suffix other than "v1" is a version this reader does
// not know and its raw bytes must be preserved verbatim.
bool ParsePmmKey(const std::string& key, int& slot, std::string& version);

// Writes or replaces the per-slot records at the end of a PMM capture.  A slot
// with no model, or with a model that no longer has authored curves, loses its
// known record so a new model can never inherit it; a record this reader
// cannot parse is kept verbatim while the model it describes still matches.
void CapturePmm(const MMDApp& app, pmm_extension::Document& document);
// Applies the per-slot records after the standard PMM body has been loaded.
// A record is validated as a whole (identity of every track against the loaded
// model, key invariants) before anything is installed; a record that cannot be
// applied is ignored and keeps its raw bytes in the document.
void LoadPmm(const MMDApp& app, const pmm_extension::Document& document);

// ---- model VMD block -----------------------------------------------------

// Builds the block for the model's effective curves.  The exported keys are
// exactly the ones the standard morph section of that same save carries: the
// frames marked for export in the native pool, rebased by `minFrame` just like
// the standard records.  A curve whose keys are all unmarked exports nothing,
// and a curve whose first frames are unmarked still exports its marked ones
// instead of disappearing.  The authored ease of an interval survives while
// both of its keys are exported and adjacent in the curve; where keys were
// skipped, or at the start of the exported track, the interval is written with
// the native interpolation so the block never claims an ease it cannot back.
// Keys are never invented for frames the standard section does not have.
// Returns true with an empty `out` when there is nothing to write.  A payload
// over kMaximumPayloadBytes returns false, which the caller must report instead
// of saving a motion without its curves.
bool CaptureVmd(const unsigned char* model, std::uint32_t minFrame,
                std::vector<std::uint8_t>& out, std::string& error);
// The block size the header declares, or 0 when the buffer does not start with
// the block magic or the declared size is unusable.  Only the first 16 bytes are
// read, so a loader can probe the bytes that follow a standard VMD body before
// committing to read the block - src/model/vmd_load.cpp does exactly that.  The
// declared size is bounded by the payload limit; whether the file really holds
// that many bytes is decided by the reader (a short read means "no block").
std::size_t VmdBlockSize(const std::uint8_t* data, std::size_t size);
// Installs the curves of one block after a VMD import.  Every track is checked
// before anything is installed: its name must match exactly one morph of the
// model, and each of its frames must carry an allocated native record that this
// very import created, with a value that agrees at the precision the standard
// record stores.  A track that fails any check is skipped whole - it can never
// rewrite frames it did not come with - and an ambiguous name is skipped rather
// than guessed.  The imported keys are then merged into the target's own curve:
// keys outside the imported set survive, a target key the import does not name
// survives, an imported key wins at its own frame with its own interpolation,
// and a target interval an imported key splits falls back to the native
// interpolation.  When the merged track would not start at frame zero, its
// frame-zero anchor takes the target's own value at frame zero rather than a
// copy of the imported first key.  Frames are shifted by the same `frameOffset`
// the native registrar used.  Returns the number of installed tracks; a block
// that is not ours, is damaged, or holds no applicable track returns 0.
std::size_t ApplyVmd(unsigned char* model, const std::uint8_t* data, std::size_t size,
                     int frameOffset);

// ---- payload codec -------------------------------------------------------

// The payload both containers carry.  Exposed so a test or another container
// can pin the format without going through a PMM document or a file.
bool EncodePayload(const std::vector<animation_curve::NamedTrack>& tracks,
                   std::vector<std::uint8_t>& out, std::string& error);
bool DecodePayload(const std::uint8_t* data, std::size_t size,
                   std::vector<animation_curve::NamedTrack>& out, std::string& error);
// Wraps one payload into a self-delimiting VMD block.
bool EncodeVmdBlock(const std::vector<std::uint8_t>& payload, std::vector<std::uint8_t>& out,
                    std::string& error);
// Extracts the body of one VMD block.  The whole block must be in the buffer
// (a header-only buffer is refused, so a truncated read can never be decoded).
// Returns the block's byte count, or 0 when the buffer does not start with the
// block magic, does not hold the declared block, the CRC does not match, or the
// buffer exceeds the limits.
std::size_t DecodeVmdBlock(const std::uint8_t* data, std::size_t size,
                           std::vector<std::uint8_t>& payload);

}  // namespace morph_curve
}  // namespace mikumikustudio
