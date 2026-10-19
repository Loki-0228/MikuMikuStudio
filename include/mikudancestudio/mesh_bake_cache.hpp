// Disk-backed cache of baked deformed meshes.
//
// Purpose
// -------
// Baking a scene into a PMM must not hold every per-frame mesh in memory, and
// it must not push that geometry into the 16 MiB PMM extension record either.
// This module keeps the geometry in an Alembic archive on disk and samples one
// frame at a time, so a baked PMM stays small and playback memory stays flat.
//
// Source archive
// --------------
// The archive is a file produced by alembic_export::Export.  Every top-level
// child of the archive is one drawn mesh and is named
//
//     model_<slot>_material_<m>_shader_<h>_pass_<p>
//     accessory_<slot>_material_<m>_shader_<h>_pass_<p>
//
// (see deformed_mesh_capture).  Children that share the prefix up to
// "_material_" belong to the same source object, so one object maps to a group
// of meshes and Sample returns that whole group.  Positions are right-handed
// Y-up with Z reflected from MMD, UVs are already 1-v, and every face is a
// triangle.  A convex face with more than three indices is fan-triangulated on
// read; a face with fewer than three is rejected.
//
// A child whose sample is a null (the exporter back-fills a frame where the
// mesh was not drawn) is returned with empty position, UV and index arrays, so
// the group keeps the same length and order at every frame.  A sample with no
// positions is empty in the same way and its index stream is ignored.
//
// Lifetime and files
// ------------------
// Install copies the input archive into a private directory owned by this
// process and keeps a long-lived std::ifstream plus the Ogawa archive open on
// that copy, so the caller may delete the input file at any time afterwards.
// The private directory is created on first use as
//
//     <system temporary directory>\MikuDanceStudio\mesh_bake_cache\
//         mesh-cache-<process id>-<32 random hex digits>\
//
// and is only accepted when this process created it, so several running
// instances never share one directory.  A private copy is removed together with
// the directory entry that owns it, and Clear removes the private directory
// only while it is empty; the shared root above it is never removed, not even
// recursively, so files that belong to another instance or to the user are
// never touched.
//
// Save publishes a persistent sidecar next to the PMM,
//
//     <pmm stem>.<32 hex digit content hash>.mdsmesh.abc
//
// and writes the slot/kind to relative-sidecar mapping into the PMM extension
// record "mds.mesh_cache.v1", or into "mds.mesh_cache.v2" once an entry carries a
// time scale other than 1.  The module never opens, truncates or rewrites the
// PMM file itself: Save only edits the pmm_extension::Document it is handed and
// copies sidecars, so a failed Save leaves the previous document and every
// previously published sidecar untouched.  Clear releases only the private
// temporary copies; a persistent sidecar is never deleted by the module.
//
// Time scale
// ----------
// Every entry carries a positive time scale that starts at 1.  Sample reads the
// archive at seconds / timeScale, and ActiveAt maps the inclusive frame range of
// a native archive through the same factor, rounded half up to a whole 30 fps
// frame.  A scale of 2 therefore plays a bake twice as fast and keeps the
// matching interval.  Ratios multiply, so a later retime cancels an earlier one.
// PrepareTimeScale and CommitTimeScale retime the whole cache; no source
// geometry in any archive is ever rewritten or re-exported.
//
// Record value layout ("mds.mesh_cache.v1", little-endian, no padding).
// New writes use the MMSC magic; the legacy MDSC form is also read.
//
//   uint8  magic[4]  = {'M','M','S','C'}
//   uint32 version   = 1
//   uint32 count     = number of entries, at most 512
//   entry[count]:
//     uint8  kind        0 = model, 1 = accessory
//     uint8  reserved    must be 0
//     uint16 reserved    must be 0
//     uint32 slot        model: < kModelSlotCount, accessory: < 255
//     uint32 pathBytes   1..512
//     char   path[pathBytes]  UTF-8 relative path, no NUL, ends in ".mdsmesh.abc"
//
// A cache whose entries all keep time scale 1 is written under that key in
// exactly that layout.  As soon as one entry carries a different scale, Save
// writes the version 2 layout under "mds.mesh_cache.v2" instead and erases
// "mds.mesh_cache.v1", so a project holds exactly one known record of this
// module.  Version 2 repeats the version 1 layout and appends the time scale of
// every entry as an IEEE 754 binary64 little-endian double:
//
//   uint8  magic[4]  = {'M','M','S','C'}
//   uint32 version   = 2
//   uint32 count     = number of entries, at most 512
//   entry[count]:
//     uint8  kind        0 = model, 1 = accessory
//     uint8  reserved    must be 0
//     uint16 reserved    must be 0
//     uint32 slot        model: < kModelSlotCount, accessory: < 255
//     uint32 pathBytes   1..512
//     char   path[pathBytes]  UTF-8 relative path, no NUL, ends in ".mdsmesh.abc"
//     double timeScale   IEEE 754 binary64, finite and greater than 0
//
// Load validates the value byte for byte (magic, version, count, entry sizes,
// slot range, reserved bytes, path shape, duplicates and time scales) before it
// touches any state, and then loads only the slot/kind pairs the record names: a
// sidecar that also contains other objects contributes none of them.
//
// Threading: every entry point serializes on one internal mutex, so Sample may
// be called from the render thread while the UI thread installs, saves or
// loads.  Sample reads the sample from disk while it holds that mutex.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mikudancestudio/deformed_mesh_capture.hpp"
#include "mikudancestudio/pmm_extension.hpp"

namespace mikudancestudio {

class MMDApp;

namespace mesh_bake_cache {

// PMM extension record that Save writes and Load reads.  Save picks the version
// 2 key as soon as one entry carries a time scale other than 1, and Load prefers
// it while both are present.
inline constexpr char kRecordKey[] = "mds.mesh_cache.v1";
inline constexpr char kRecordKeyV2[] = "mds.mesh_cache.v2";
// Version 3 repeats the version 2 layout and appends the cutoff layer of every
// entry as a little-endian uint32: kNoCutoffLayer for a legacy full-chain bake
// (played back through the host's Source replacement), or the MMV layer number
// of a bake-to-layer-x (played back through the MMV bakedMeshes service at that
// layer, never through Source).  Save writes exactly one record of this module.
inline constexpr char kRecordKeyV3[] = "mds.mesh_cache.v3";

// A legacy entry without a layer cutoff: its mesh is the final composite and
// the host replaces the Source mesh with it.  A cutoff entry is offered to the
// MMV runtime as that layer's lowest-priority mesh output instead.
inline constexpr std::uint32_t kNoCutoffLayer = 0xffffffffu;

// Identity of one cache entry for the bakedMeshes service: the MMV layer the
// mesh belongs to and the host object generation it was baked from.  The
// generation is a session value and is never written to the project record:
// an entry starts unbound (0) and binds to the object it first plays back on
// exactly once through BindGeneration.  A later object replacement makes the
// entry stale instead of rebinding, and a stale cache is hard-rejected by the
// host - never silently dropped and never rebound every frame.
struct EntryInfo {
    std::uint32_t cutoffLayer = kNoCutoffLayer;
    std::uint64_t sourceGeneration = 0;
};

// Suffix of a persistent sidecar published next to the PMM file.
inline constexpr char kSidecarSuffix[] = ".mdsmesh.abc";

// Copies `abc` into the module's private temporary directory, opens it and
// associates every mesh it contains with the model or accessory that currently
// occupies the named slot.  Only objects that are present in the scene are
// bound, and only objects named by this archive are replaced: cache entries for
// other object pointers survive.  On failure nothing is changed and the
// temporary copy is removed.  Returns false with a message in `error` when the
// file cannot be read as an Alembic archive or when the archive names no mesh
// that is currently in the scene.
//
// `beforeCommit` is an optional last chance to veto the install.  It runs at
// most once, and only when everything that can fail has already succeeded: the
// archive has been validated, its private copy exists, at least one candidate
// entry was bound, and the complete next cache state has been allocated.  The
// callback is the place for work that must happen exactly when the cache is
// about to change - the application uses it to disable MME/MMV effects, because
// the baked geometry already contains their deformation.  Returning false, or
// throwing, aborts the install: the previous cache is left exactly as it was and
// the candidate's private copy is removed.  A false return may put the reason
// into the string it is given; an empty string yields a generic message.  After
// the callback returns true the install commits with a noexcept container swap,
// and nothing that follows can fail it.  The callback runs while the module
// holds its internal lock, so it must not call back into this module.
//
// `cutoffLayer` describes the bake behind this archive: kNoCutoffLayer for a
// legacy full-chain bake, otherwise the MMV layer x whose output the meshes
// are.  Every entry installed by this call carries it; each entry binds its own
// object generation separately through BindGeneration, because one bake can
// cover several objects whose generations differ.
bool Install(MMDApp& app, const std::filesystem::path& abc, std::string& error,
             const std::function<bool(std::string&)>& beforeCommit = {},
             std::uint32_t cutoffLayer = kNoCutoffLayer);

// Cutoff layer and source generation of one cached object, for the bakedMeshes
// service.  False when the object is not cached.
bool Info(const void* object, EntryInfo& out);

// Binds the generation of the object a cache entry plays back on.  An unbound
// entry (0) takes the generation exactly once; a later call with another
// generation returns false, which is the stale-cache case the host must refuse
// loudly.  False also for an unknown object or a zero generation.
bool BindGeneration(const void* object, std::uint64_t generation);

// True when `object` (a model or accessory record pointer as returned by
// MMDApp::ModelSlot / MMDApp::AccessorySlot) has baked meshes.  A null pointer
// is never cached.
bool Has(const void* object);
// New native caches carry an inclusive frame range in the ABC metadata.
// Legacy archives retain their historical endpoint-clamping behaviour.  For a
// retimed entry the range is mapped through its time scale and snapped half up
// to a whole 30 fps frame, so `seconds` is a time on the retimed timeline.
bool ActiveAt(const void* object, double seconds);
bool RangeLimited(const void* object);

// Reads every mesh cached for `object` at `seconds` using the nearest sample at
// or before that time; a time outside the recorded range clamps to the first or
// last sample.  `seconds` is a time on the retimed timeline: the archive itself
// is read at seconds / timeScale.  `meshes` receives the group in archive child
// order and is cleared first.  Returns false with a message in `error` when the
// object is not cached or the sample cannot be read.
bool Sample(const void* object, double seconds,
            std::vector<deformed_mesh_capture::Mesh>& meshes, std::string& error);

// Drops the cache entry of exactly this object pointer and releases the
// archive it used when no other entry still needs it.  Unknown pointers are
// ignored, and entries of other objects keep their meshes.
void Forget(const void* object);

// Drops every entry and removes the private copies this module created.  Only
// files it created are deleted, and the private directory is removed only while
// it is empty; the shared root and anything this module does not own stay.
// Persistent sidecars published by Save stay on disk.
void Clear();

// True when no object is cached.
bool Empty();

// Publishes the cache as sidecars next to `pmm` and writes the mapping into
// `document` under kRecordKey.  Slots are resolved against the current scene
// through `app`, so an entry whose object has since been unloaded is not
// recorded.  A save that ends up with nothing to record erases a stale record of
// either known version.  On failure `document` is left exactly as it was passed
// in and every previously published sidecar stays in place.
//
// A retimed entry switches the record to kRecordKeyV2, and the other known key
// of this module is erased so that one project never carries two records of it.
// Records of other modules, including unknown "mds.mesh_cache.vN" keys this
// build does not know, are left exactly as they are.
bool Save(MMDApp& app, const std::filesystem::path& pmm,
          pmm_extension::Document& document, std::string& error);

// Reads the mesh cache record from `document`, validates it, and loads the
// referenced sidecars relative to the directory of `pmm`.  A kRecordKeyV2 record
// takes precedence over kRecordKey; the internal version must agree with the key
// it was read from, and every v2 time scale must be finite and greater than 0.
// Only the slot/kind pairs named by the record are bound, and only to objects
// that are currently in the scene; entries for other slots are skipped and
// objects cached from other archives keep their data.  A missing record is a
// successful no-op.  A malformed record, a missing or unreadable sidecar, a
// sidecar that does not contain a named mesh, or a record whose objects are all
// absent from the scene leaves the cache unchanged and returns false with a
// message in `error`.
bool Load(MMDApp& app, const std::filesystem::path& pmm,
          const pmm_extension::Document& document, std::string& error);

// ---------------------------------------------------------------------------
// Frame-rate retime
// ---------------------------------------------------------------------------

// A retime runs in two steps, so that everything that can still fail happens
// while the current cache is untouched: PrepareTimeScale reads the cache and
// builds the complete next state of every entry, CommitTimeScale applies it.
// Both serialize on the module lock against Install, Save, Load and Sample.
struct TimeScalePlan;
using TimeScalePlanPtr = std::shared_ptr<TimeScalePlan>;

// Builds the retime `ratio` would apply to the cache as it is right now: every
// entry keeps its archive but gets its time scale multiplied by `ratio`, and
// `count` receives the number of entries in the plan.  `ratio` must be finite
// and greater than 0, every resulting time scale must stay finite and greater
// than 0, and the frame range of a native archive must stay inside the
// 10000000 frame limit of the timeline; anything else fails with a message in
// `error` and leaves `plan` empty.  An empty cache succeeds with a plan of
// `count` 0.  The call changes nothing: it copies the entries, shares the open
// archives, records the cache revision it was built from, and neither creates a
// private copy nor opens an archive again.
bool PrepareTimeScale(double ratio, TimeScalePlanPtr& plan, std::size_t& count,
                      std::string& error);

// Applies a plan built by PrepareTimeScale.  The plan is rejected when it was
// already applied, or when the cache changed since it was prepared (Install,
// Load, Forget, Clear or another retime), so a plan can never overwrite a cache
// it was not built from.  `beforeCommit` is the same last chance to veto as in
// Install, and it runs at most once whenever one is given - including for a plan
// whose cache is empty, because the caller uses this commit as the transaction
// that wraps its own native and MMV work and that transaction has to be offered
// even when no geometry is cached.  It runs while the module holds its lock, so
// it must not call back into this module.  Everything that can still fail - the
// plan check and the allocation of the next map - happens before the callback;
// after it returns true the commit is a noexcept container swap and the plan
// counts as consumed.  A refused or failed commit leaves the previous cache and
// the plan exactly as they were, so the same plan can be committed again.
bool CommitTimeScale(const TimeScalePlanPtr& plan, std::string& error,
                     const std::function<bool(std::string&)>& beforeCommit = {});

}  // namespace mesh_bake_cache
}  // namespace mikudancestudio
