#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/model.hpp"
#include <cmath>
#include <cstring>

static_assert(MMD_ORIGINAL_PROFILE, "Original profile provider must not enter the full build");

namespace mikumikustudio::bone_scale {
// Keep the legacy records and their undo rings untouched. This provider owns
// no model, clipboard, registration or affine-physics side tables.
bool Identity(const Matrix& value) { return value == Matrix{}; }
bool Valid(const Matrix& value) { return Identity(value); }
bool Linear(const Curve& value) { return value[0] == value[1] && value[2] == value[3]; }
float CurveTime(const Curve&, float value) { return value; }
Matrix Linear(const float matrix[16]) {
    Matrix value;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) value.v[i * 3 + j] = matrix[i * 4 + j];
    return value;
}
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix value;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double sum = 0;
        for (int k = 0; k < 3; ++k) sum += double(a.v[i * 3 + k]) * b.v[k * 3 + j];
        value.v[i * 3 + j] = float(sum);
    }
    return value;
}
bool Inverse(const Matrix& a, Matrix& out) {
    const auto& m = a.v;
    const double d = double(m[0]) * (double(m[4]) * m[8] - double(m[5]) * m[7])
        - double(m[1]) * (double(m[3]) * m[8] - double(m[5]) * m[6])
        + double(m[2]) * (double(m[3]) * m[7] - double(m[4]) * m[6]);
    if (!std::isfinite(d) || std::abs(d) < 1.e-18) return false;
    const double cof[9] = {double(m[4])*m[8]-double(m[5])*m[7],double(m[2])*m[7]-double(m[1])*m[8],double(m[1])*m[5]-double(m[2])*m[4],
        double(m[5])*m[6]-double(m[3])*m[8],double(m[0])*m[8]-double(m[2])*m[6],double(m[2])*m[3]-double(m[0])*m[5],
        double(m[3])*m[7]-double(m[4])*m[6],double(m[1])*m[6]-double(m[0])*m[7],double(m[0])*m[4]-double(m[1])*m[3]};
    for (int i = 0; i < 9; ++i) out.v[i] = float(cof[i] / d);
    return true;
}
Matrix Interpolate(const Matrix&, const Matrix&, float) { return {}; }
Matrix Pose(const unsigned char*, int) { return {}; }
Matrix Key(const unsigned char*, int) { return {}; }
Matrix Sample(const unsigned char*, int, double) { return {}; }
bool SetPose(unsigned char*, int, const Matrix&) { return false; }
void SetKey(unsigned char*, int, const Matrix&) {}
void SetKeyFresh(unsigned char*, int, const Matrix&) {}
void ClearKey(unsigned char*, int) {}
Curve KeyCurve(const unsigned char*, int) { return LinearCurve(); }
void SetKeyCurve(unsigned char*, int, const Curve&) {}
void CopyKey(unsigned char*, int, int) {}
void CopyClipboard(const MMDApp*, int, const Matrix&, bool, const Curve&) {}
Matrix Clipboard(const MMDApp*, int, bool, bool) { return {}; }
Curve ClipboardCurve(const MMDApp*, int, bool) { return LinearCurve(); }
Registration::Registration(const Matrix&) : previous{}, previousCurve{}, previousExplicit(false) {}
Registration::Registration(const Matrix&, const Curve&) : Registration(Matrix{}) {}
Registration::~Registration() = default;
Matrix RegistrationValue() { return {}; }
Curve RegistrationCurve() { return LinearCurve(); }
DefaultCurveScope::DefaultCurveScope() : previousDefaultCurve(false) {}
DefaultCurveScope::~DefaultCurveScope() = default;
void ApplyRegistration(unsigned char*, int, bool) {}
void Register(unsigned char*, int, int) {}
void Evaluate(unsigned char*, double) {}
void EvaluatePlayback(unsigned char*, double) {}
Snapshot Capture(const unsigned char*) { return {}; }
std::uint64_t Revision(const unsigned char*) { return 0; }
bool Restore(unsigned char*, Snapshot value) { return value.pose.empty() && value.keys.empty() && value.curves.empty(); }
void CaptureUndo(unsigned char*) {}
bool StageBakeState(unsigned char*, Snapshot, std::size_t, StagedBakeState&) { return false; }
void PublishBakeState(StagedBakeState&&) noexcept {}
void Undo(unsigned char*, int) {}
void Redo(unsigned char*, int) {}
void Forget(unsigned char*) {}
void RestoreTransforms(unsigned char*) {}
void ApplyTransforms(unsigned char*) {}
bool HasPose(const unsigned char*) { return false; }
Values PoseValues(const unsigned char*) { return {}; }
void BuildTransforms(const unsigned char* model, const Values&, std::vector<Transform>& out,
                     const std::vector<Transform>* baseline) {
    out.clear();
    if (!model) return;
    const auto& record = *mdl::Mdl(model);
    if (!record.boneTable) return;
    if (baseline && baseline->size() == record.boneCount) { out = *baseline; return; }
    out.resize(record.boneCount);
    for (std::size_t i = 0; i < out.size(); ++i) std::memcpy(out[i].data(), record.boneTable[i].matInit, sizeof(Transform));
}
void CopyState(const unsigned char*, unsigned char*) {}
void ExchangeState(unsigned char*, unsigned char*) {}
void ResolvePhysics(unsigned char*, const std::map<int, Transform>&) {}
bool Enabled(MMDApp*) { return false; }
void Drag(MMDApp*, int) {}
void DragScreen(MMDApp*, int) {}
void DragScreenPlane(MMDApp*) {}
bool Scale(unsigned char*, int, const Matrix&, const float[3], float, bool) { return false; }
bool CaptureCurveTable(const unsigned char*, CurveTableState&) { return false; }
void SwapCurveTable(unsigned char*, CurveTableState&) noexcept {}

namespace gizmo {
int AxisOf(Handle handle) { const int axis = int(handle) - int(Handle::AxisX); return axis >= 0 && axis < 3 ? axis : -1; }
Handle HandleOfAxis(int axis) { return axis >= 0 && axis < 3 ? Handle(int(Handle::AxisX) + axis) : Handle::None; }
Highlight HighlightOf(MMDApp*) { return {}; }
Handle HitTest(const ScreenGeometry&, float, float) { return Handle::None; }
float ScreenDragAmount(const ScreenGeometry&, Handle, float, float) { return 0; }
bool Active(MMDApp*) { return false; }
Handle HitTest(MMDApp*, float, float) { return Handle::None; }
void Publish(MMDApp*, const ScreenGeometry&) {}
const ScreenGeometry& Published(const MMDApp*) { static const ScreenGeometry empty; return empty; }
}
}
