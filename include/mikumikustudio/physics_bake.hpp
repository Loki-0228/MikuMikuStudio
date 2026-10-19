#pragma once
#include "mikumikustudio/physics_bake_settings.hpp"
#include "mikumikustudio/channel_bake.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include <functional>
#include <vector>
#include <string>
namespace mikumikustudio { class MMDApp;
namespace bake {
struct Request {
    int firstFrame = 0, lastFrame = 0;
    bool physics = true, keyframes = false, selectedOnly = false;
    Settings settings;
    // Native channel bake, added 2026-09-27.  The fields above keep their order
    // and meaning, so every existing reader and every existing default still
    // behaves the same; these three are appended and read only by the bake
    // dialog and by channel_bake.  `keyframes` stays the bone switch and the
    // bone pools stay with Compute/Commit; everything else the dialog offers
    // (camera, light, self shadow, gravity, the .x accessories, and the selected
    // model's morph and 表示/IK tracks) is described by `channels`.
    int step = 1;
    channel_bake::Scope channelScope = channel_bake::Scope::Model;
    channel_bake::Selection channels;
    // Explicit bake scope, appended 2026-09-27 (second revision).  A source the
    // user ticked is never widened to the whole model: these two pairs record
    // exactly what the source lists selected, and `channels` carries the same
    // scope per channel (channel_bake::Choice::coverage / ::indices).
    // `boneCoverage` / `boneIndices` are what the channel bake resolves and
    // validates (channel_bake::ResolveBones, channel_bake::PreparedBones), so
    // the bone transaction and the per-bone FX 0/1 toggle keys can be driven
    // with exactly the checked bones.  `vertexCoverage` / `vertexIndices` are
    // recorded for the caller only: the mesh bake, its cache and the FX keys of
    // the mesh sources are integrated outside this module.  `selectedOnly` keeps
    // its existing physics meaning; when no explicit bone list is given,
    // boneCoverage mirrors it (Selected while selectedOnly is set).
    channel_bake::Coverage boneCoverage = channel_bake::Coverage::Whole;
    std::vector<int> boneIndices;
    channel_bake::Coverage vertexCoverage = channel_bake::Coverage::Whole;
    std::vector<int> vertexIndices;
    bool preview=false;
    // Record selected layer outputs without cancelling the object's FX.
    bool keepLayerEffects=false;
};
enum class Error { None, InvalidRequest, NoModel, NoBones, Capacity, InvalidScene, Cancelled, Allocation,
    // A layered bake refused an inexpressible combination; `layerError` and
    // `layerMessage` name the exact rule and object.  Nothing was written.
    LayerRefused };
// Refusal reason of a layered (bake-to-layer-x) bake.  Declared here because
// Result carries it; the full semantics are documented with the layer section.
enum class LayerError {
    None,
    NotSynchronized,     // MeshOnly output: mesh bake only
    GeneratedItem,       // generated item without a unique source bone
    UnmappedBone,        // sourceKey names no bone of the model
    AmbiguousBone,       // two layer bones claim one host bone
    MissingBone,         // a host bone has no layer bone
    BindingChanged,      // weights/binding tables differ from the source stage
    ExtraMeshDeform,     // the layer mesh is not the native skin of the state
    NoDeformedGeometry,  // snapshot lacks the geometry to prove the skin check
    MissingObject,       // the sampled output does not contain the model
    InvalidData,
};
struct Result {
    Error error = Error::None;
    std::vector<mdl::BoneKeySnapshot> changes;
    bone_scale::Values scales;
    int boneCount = 0, sampledFrames = 0;
    unsigned maxFrame = 0;
    std::uint64_t simulationSteps = 0;
    // Set only for Error::LayerRefused.
    LayerError layerError = LayerError::None;
    std::wstring layerMessage;
};
using Progress = std::function<bool(int frame, int lastFrame)>;
// Computes on private copies. No live model, timeline, or Bullet state is changed.
Result Compute(MMDApp&, const Request&, const Progress& = {});
struct ModelResult { unsigned char* model=nullptr; Result result; };
struct SceneResult { Error error=Error::None; std::vector<ModelResult> models; std::uint64_t simulationSteps=0; };
// One shared offline world; collect every loaded model's dynamic bones.
SceneResult ComputeScene(MMDApp&, const Request&, const Progress& = {});
// Prepare all models' undo/key data before publishing any model's keys.
bool CommitScene(MMDApp&, const SceneResult&, int undoFrame);
bool Commit(unsigned char*, const Result&, int undoFrame);
// Writes back an independent before-image of exactly the bone key slots a
// combined (layered) bake was about to change, plus the maxFrame it replaced.
// A failed transaction recovers through this call and never consumes the
// user's undo ring, which stays a user facility only.
void RestoreKeys(unsigned char*, const std::vector<mdl::BoneKeySnapshot>& before,
                 unsigned maxFrame) noexcept;
// The before-image itself: exactly the slots `result` would write (an empty
// record past the live pool) and the current maxFrame.  `valid` is false when
// the model has no key pool, in which case nothing may be written.
struct KeyPreImage {
    std::vector<mdl::BoneKeySnapshot> keys;
    unsigned maxFrame = 0;
    bool valid = false;
};
KeyPreImage CaptureKeys(unsigned char*, const Result& result);
bool HasExistingKeys(const unsigned char*, const Request&);
void RefreshMenu(MMDApp*);
bool HandleCommand(MMDApp*, unsigned command);
void ShowDialog(MMDApp*);
// One gate for both the "Bake keyframes" menu entry and ShowDialog, so the
// entry is enabled exactly when the dialog would open and a command dispatched
// from anywhere else is refused the same way.  Baking is allowed only while the
// scene is idle (nothing playing, no recording window, no ABC export in
// flight), a model is selected, that model's original bones are still live (no
// mesh bake cache is installed for it) and at least one bone can be targeted by
// the bake options.  With a mesh bake cache installed the picture comes from
// the baked geometry, so baking the live bones would change the motion without
// changing what is drawn; the cache has to be cleared first.
bool CanBake(MMDApp*);

// ---------------------------------------------------------------------------
// MMV layer cutoff bake (bake to layer x)
// ---------------------------------------------------------------------------
// A layered bake reads the OUTPUT stage of the sampled cutoff layer instead of
// running its own simulation: the pose source is what layers [maximum..x]
// published, mapped back to the host bones by sourceKey identity.  This module
// adds no second composition and no second physics integration.
//
// Expressibility is decided per object before anything is written (see
// .local/artifacts/mmv-layer-bake/cutoff-cache-plan.md):
//   * a MeshOnly output (flags without MMV_DEFORMER_STRUCTURE_SYNCHRONIZED) may
//     be baked as a mesh but never as bone/physics/morph keys;
//   * keys are writable only when the layer bones map 1:1 onto the host bones
//     through sourceKey (a generated bone keeps the identity of the original it
//     replaces; anything else is refused) and the binding tables did not
//     change - names and array indices are never used to guess a mapping;
//   * keys are writable only when the layer mesh is exactly the native linear
//     blend skinning of that same state, so no mesh deformation would be lost.
// One sampled frame of the cutoff state, mapped onto the model's own tables.
// `boneWorld` holds 16 floats per host bone, `morphValues` one value per morph;
// the valid flags say which entries the layer actually published.
struct LayerPoseFrame {
    int frame = 0;
    bool structureSynchronized = true;
    std::vector<float> boneWorld;
    std::vector<unsigned char> boneValid;
    std::vector<float> morphValues;
    std::vector<unsigned char> morphValid;
};
// Localized message for a refusal, for a message box.
const wchar_t* LayerErrorText(MMDApp& app, LayerError error);
// Maps one sampled layer OUTPUT object onto the model's bone and morph tables
// by sourceKey identity only.  Refuses MeshOnly output, generated structure it
// cannot express and any non-bijective mapping.
LayerError MapLayerPose(MMDApp& app, unsigned char* model, const native_mesh::LayerObject& object,
                        LayerPoseFrame& out, std::wstring& message);
// True (LayerError::None) when the object's layer mesh equals what the host's
// own production skinning (affine_skinning: BDEF1/2/4 and SDEF; QDEF and
// unknown weight types leave position and normal untouched) produces from that
// same state - positions AND normals - and when every channel a key bake cannot
// carry (UVs, edge data, materials, topology, visibility) is still identical
// to the SOURCE stage.  Anything else - including a snapshot without
// morph-deformed geometry - is refused, because the change would be lost when
// the FX are removed.
LayerError CheckLayerMeshIsNativeSkin(const native_mesh::LayerObject& output,
                                      const native_mesh::LayerObject& source, std::wstring& message);
// What the layers changed between the SOURCE stage (the host's own input) and
// the OUTPUT stage of the sampled cutoff layer.  A mesh bake can only store the
// mesh, so a cutoff that also changed structure, bindings, bone poses or morph
// values is refused when lower layers would still consume them.
struct LayerDifference {
    bool structure = false, binding = false, pose = false, morphs = false;
    bool Empty() const { return !structure && !binding && !pose && !morphs; }
};
LayerDifference CompareLayerStages(const native_mesh::LayerObject& output,
                                   const native_mesh::LayerObject& source);
// What the existing Alembic mesh cache can actually store: positions, normals,
// uv[2], triangle indices, the material index of every triangle and the
// source-vertex mapping.  Everything else - the full MMV material parameters
// (including textures), additional UVs, edgeScale and vertex flags - is
// rebuilt at replay time from the live source object, which is exact only when
// the layers changed none of them.  This check refuses a cutoff whose OUTPUT
// differs from the SOURCE stage on any field the cache cannot record, naming
// the field: even at layer 0 removing such an FX would silently lose its
// result.  Generated materials and generated vertices that carry non-neutral
// unrecordable fields are refused as well.
LayerError CheckLayerMeshCacheExpressible(const native_mesh::LayerObject& output,
                                          const native_mesh::LayerObject& source,
                                          std::wstring& message);
// While a layered bake samples through scene_sampling::Visit and an MMV solver
// owns the physics (native physics switched off through the solver takeover),
// the private offline world must not advance the same bodies a second time.
// The sampler suppresses its own stepping through this switch; with no solver
// active the offline world steps exactly as before, and its first step always
// follows one freshly evaluated animation input.
void SetLayerPhysicsSuppressed(bool suppressed);
bool LayerPhysicsSuppressed();

// Layered sampling: drives scene_sampling::Visit (the only path that lets MMV
// see the offline scene), queries the cutoff OUTPUT per sampled frame and
// builds the same Result the non-layered Compute produces.  `sampling` must be
// an open native_mesh::LayerSampling session; no private simulation result is
// used as a pose source here.  Each collected frame carries the mapped bone
// poses and morph values of that sample, for channel_bake's morph source.
Result ComputeLayered(MMDApp& app, const Request& request,
                      const native_mesh::LayerSampling& sampling,
                      std::vector<LayerPoseFrame>& frames,
                      const Progress& progress = {});

// Commands of the top-level Baking menu.  32800..32804 are unused across
// src/, include/ and res/; the neighbouring reservations are the skinning
// commands 32000..32002, the view items 32003/32004, the skinning adapter
// window 32100..32227 and the MMV menu 32600.
constexpr unsigned kBakeMenuCommand = 32800;
constexpr unsigned kExportMenuCommand = 32801;
// Id carried by the Baking popup itself, and the only thing RefreshMenu uses to
// recognise its own menu.  Ownership is never inferred from the commands inside
// a popup: the legacy Edit entry id 32100 also names a skinning adapter entry.
constexpr unsigned kBakingMenuId = 32802;
// "Clear mesh bake..." owns its own scope dialog and never reuses the scope
// checkbox of the ABC export dialog.
constexpr unsigned kClearMeshBakeMenuCommand = 32803;
// "Bake mesh..." also offers optional ABC export in the same dialog.
// It opens alembic_export::ShowBakeDialog, which owns the mesh source selection,
// the mesh cache and the staged FX keys.  It is a separate entry on purpose: the
// key bake never produces a mesh and the mesh bake never replaces the physics
// step, and neither entry may hand a global FX switch to a local Bullet run.
constexpr unsigned kBakeMeshMenuCommand = 32804;
}
}
