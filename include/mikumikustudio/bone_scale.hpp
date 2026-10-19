#pragma once
#include "mikumikustudio/build_profile.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace mikumikustudio {
class MMDApp;
namespace bone_scale {
// Row-vector convention, in the bone's unposed model-space basis. Kept outside
// the legacy PMM/PMD structures so their byte layout remains unchanged.
struct Matrix {
    std::array<float, 9> v{1,0,0, 0,1,0, 0,0,1};
    bool operator==(const Matrix& other) const { return v==other.v; }
    bool operator!=(const Matrix& other) const { return !(*this==other); }
};
// MMS addition: the scale channel's 0..127 Bezier handles, in the same domain
// and the same order (x1, y1, x2, y2) as the four bone interpolation channels
// and the six camera channels.  Linear is 20/20/107/107, the tuple the legacy
// channels use for a linear interval.
//
// ---- frozen public API (2026-09-27) ---------------------------------------
// The curve support is frozen for the persistence module that follows:
//   Curve, LinearCurve, Linear, CurveTime
//   Snapshot::curves, KeyCurve, SetKeyCurve, ClearKey, SetKeyFresh, CopyKey
//   CopyClipboard, Clipboard, ClipboardCurve
//   Registration, RegistrationValue, RegistrationCurve, DefaultCurveScope,
//   ApplyRegistration, Register
//   kInterpScaleChannel, kInterpBoneAll, kInterpCameraAll
// bone_scale_io must read and write curves through these entry points only;
// the wire formats stay inside the codec and never extend a legacy structure.
using Curve = std::array<std::uint8_t, 4>;
constexpr std::uint8_t kLinearHandleA = 20;   // x1 and y1 of a linear curve
constexpr std::uint8_t kLinearHandleB = 107;  // x2 and y2 of a linear curve
inline Curve LinearCurve() {
    return {kLinearHandleA, kLinearHandleA, kLinearHandleB, kLinearHandleB};
}
bool Linear(const Curve& curve);
// Raw 0..1 -> eased 0..1, the time mapping the bone channels use (BoneEase,
// VA 0x004A05A0): x1/x2 describe the X polynomial, y1/y2 the Y polynomial.  A
// linear curve returns t unchanged.
float CurveTime(const Curve& curve, float t);
using Values = std::map<int, Matrix>;
// Per-key-slot scale curves.  A slot without an entry is linear.
using Curves = std::map<int, Curve>;
// The interpolation-channel selector's bone layout: 0..3 are the four legacy
// channels, 4 is the bone scale channel and 5 is the trailing "all" entry.  The
// camera panel keeps its own layout (six channels, "all" at 6).
constexpr int kInterpScaleChannel = 4;
constexpr int kInterpBoneAll = MMD_ORIGINAL_PROFILE ? 4 : 5;
constexpr int kInterpCameraAll = 6;
using Transform = std::array<float, 16>;
struct Snapshot { Values pose, keys; Curves curves; };
bool Valid(const Matrix& value);
bool Identity(const Matrix& value);

// ---- MMS addition: native frame retime (2026-09-27) ------------------------
// Capture matrices and interpolation curves before retiming key slots. A merge
// replaces the surviving slot's matrix and curve and clears the discarded slots.
// SwapCurveTable exchanges both tables and the revision without allocating, so
// the same operation installs a prepared result or restores a rejected commit.
//
// `revision` travels with the curves because a curve change is an edit: a
// cache that keys on Revision() must not keep serving the pre-conversion
// curves after the conversion, and a rollback must restore the old number.
struct CurveTableState {
    Curves curves;
    Values keys;
    std::uint64_t revision = 0;
    const unsigned char* model = nullptr;
};
// Copies the model's key matrices, curves and revision without changing it.
// Returns false only when the model has no scale state.
bool CaptureCurveTable(const unsigned char* model, CurveTableState& out);
// Exchanges the model's live key matrices, curves and revision with `state`. Never
// allocates, never throws, and leaves `state` holding what the model had.
void SwapCurveTable(unsigned char* model, CurveTableState& state) noexcept;
Matrix Multiply(const Matrix& a, const Matrix& b);
bool Inverse(const Matrix& value, Matrix& inverse);
Matrix Linear(const float matrix[16]);
Matrix Interpolate(const Matrix& a, const Matrix& b, float t);
Matrix Pose(const unsigned char* model, int bone);
Matrix Key(const unsigned char* model, int index);
// Read a track without changing the live pose, undo state or bone matrices.
Matrix Sample(const unsigned char* model, int bone, double frame);
bool SetPose(unsigned char* model, int bone, const Matrix& value);
// Writes a key's scale matrix.  The key's curve (if any) is preserved, so a
// registration that overwrites an existing keyframe keeps the curve the user
// set; an identity value only drops the matrix.
void SetKey(unsigned char* model, int index, const Matrix& value);
// MMS addition: the same write for a key slot that is being populated from
// scratch (a slot the pool just handed out).  Unlike SetKey it also drops the
// curve, so a reused slot can never hand its predecessor's curve to a new key.
void SetKeyFresh(unsigned char* model, int index, const Matrix& value);
// MMS addition: the key is gone - drop both its scale matrix and its curve.
void ClearKey(unsigned char* model, int index);
// MMS addition: the key slot's curve, linear when the slot carries none.
Curve KeyCurve(const unsigned char* model, int index);
// MMS addition: writes a key slot's curve.  A linear curve is stored as "no
// entry", so the default state and an explicitly linear curve are the same.
void SetKeyCurve(unsigned char* model, int index, const Curve& curve);
void CopyKey(unsigned char* model, int destination, int source);
// The curve defaults to linear, which is what a pose clipboard (no keys) uses.
void CopyClipboard(const MMDApp* app, int index, const Matrix& value, bool pose,
                   const Curve& curve = LinearCurve());
Matrix Clipboard(const MMDApp* app, int index, bool pose, bool mirror = false);
Curve ClipboardCurve(const MMDApp* app, int index, bool pose);
// A scoped payload accompanies the legacy 84-byte registrar record without
// extending its ABI. Ordinary VMD/VPD callers use the identity default.  MMS
// addition: the payload also carries the scale curve, so a paste that passes
// on a clipboard curve restores it with the matrix; the default is linear.
class Registration {
public:
    explicit Registration(const Matrix& value);
    // A payload that carries a curve is explicit: a linear curve then means
    // "the default linear curve" and clears the target's older curve, which is
    // what a clipboard paste or an import needs.  The one-argument form is an
    // ordinary registration and leaves an existing curve alone.
    Registration(const Matrix& value, const Curve& curve);
    ~Registration();
private:
    Matrix previous;
    Curve previousCurve;
    bool previousExplicit;
};
Matrix RegistrationValue();
Curve RegistrationCurve();
// MMS addition: marks a motion import that replaces the keyframes it registers.
// While the scope is active a registered key takes the default linear curve
// instead of keeping whatever curve a previous session left on the slot it
// overwrites; a VMD tail extension that carries curves overwrites it again
// afterwards.  Ordinary in-app registration does not use this scope, so it
// keeps the curve of the key it rewrites.
class DefaultCurveScope {
public:
    DefaultCurveScope();
    ~DefaultCurveScope();
private:
    bool previousDefaultCurve;
};
// MMS addition: applies the current registration payload (matrix + curve) to
// one key slot.  `fresh` is true when the pool just handed the slot out, which
// drops the slot's previous curve first; a linear payload curve leaves an
// existing curve alone, except under DefaultCurveScope.
void ApplyRegistration(unsigned char* model, int index, bool fresh);
void Register(unsigned char* model, int bone, int key);
void Evaluate(unsigned char* model, double frame);
// Call immediately after native playback has advanced the model's bone cursors.
// Reuses those intervals; direct seeks and offline sampling still use Evaluate/Sample.
void EvaluatePlayback(unsigned char* model, double frame);
Snapshot Capture(const unsigned char* model);
// Changes only for edits/import/undo, not frame evaluation or rendering.
std::uint64_t Revision(const unsigned char* model);
bool Restore(unsigned char* model, Snapshot snapshot);
void CaptureUndo(unsigned char* model);
struct StagedBakeState { unsigned char* model=nullptr; unsigned ring=0; Snapshot undo,current; };
// Allocate scale/undo copies before a multi-model bake publishes any keys.
bool StageBakeState(unsigned char* model, Snapshot current, std::size_t capacity, StagedBakeState&);
void PublishBakeState(StagedBakeState&&) noexcept;
void Undo(unsigned char* model, int ring);
void Redo(unsigned char* model, int ring);
void Forget(unsigned char* model);
// Restore the rigid matrices before the next IK/physics evaluation. Apply once
// after that evaluation and before skinning; repeated Apply calls are harmless.
void RestoreTransforms(unsigned char* model);
void ApplyTransforms(unsigned char* model);
bool HasPose(const unsigned char* model);
Values PoseValues(const unsigned char* model);
// Child pivots follow the parent's deformation. Inherited scale acts in the
// unposed model basis before each child's rotation, preserving its shape.
void BuildTransforms(const unsigned char* model, const Values& pose, std::vector<Transform>& out,
                     const std::vector<Transform>* rigidBaseline=nullptr);
// Copy animation state for an isolated simulator, undoing any display-only
// affine matrices in its copied bone array without touching the source.
void CopyState(const unsigned char* source, unsigned char* destination);
// Swap complete runtime state while a private export pose borrows a live
// object identity. Unlike Restore, this preserves undo and revision counters.
void ExchangeState(unsigned char* first, unsigned char* second);
// Factor simulated full world matrices back into rigid bone poses and native
// scale, so rendering and baking apply the physical deformation exactly once.
void ResolvePhysics(unsigned char* model, const std::map<int, Transform>& world);
bool Enabled(MMDApp* app);
// axis -1 is uniform; 0/1/2 are X/Y/Z in the existing coordinate selector.
// The amount is the vertical pointer delta: this is the HUD row's rule and it
// stays that way.  The viewport handles below use the projected drag instead.
void Drag(MMDApp* app, int axis);
// MMS addition: scale along one coordinate-selector axis, with the amount taken
// from this frame's pointer delta measured along the axis handle's projected
// screen direction (bone_scale_gizmo.hpp).  A horizontal axis is therefore
// driven by a horizontal drag; an unavailable or degenerate handle applies
// nothing.
void DragScreen(MMDApp* app, int axis);
// MMS addition: the white view-plane square.  Both view-plane axes scale by one
// factor and the amount runs along the square's projected diagonal, so a
// horizontal and a vertical drag both work.
void DragScreenPlane(MMDApp* app);
// Full world-basis conjugation. basis holds the unscaled posed bone transform
// including any inherited deformation; direction is a unit world vector.
bool Scale(unsigned char* model, int bone, const Matrix& basis,
           const float direction[3], float factor, bool uniform);
}
}
