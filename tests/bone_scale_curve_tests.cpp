// MAINTENANCE: regression-keep — native bone scale interpolation curve.
// Curve storage, the 0..127 time mapping, key-slot identity and the clipboard
// and undo paths.  Pure CPU: no device, no window, no file.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"

using namespace mikumikustudio;
namespace bs = bone_scale;

namespace {

void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void Near(float a, float b, const char* message) {
    Check(std::abs(a - b) < 0.001f, message);
}

void Identity(float* m) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// A one-bone model with two allocated keys, linked head -> key.
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> record =
        std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;
    std::vector<mdl::BoneKey> keys;
    std::vector<unsigned char> selected, dirty;
    explicit Fixture() : bones(1), keys(mdl::kBoneKeyCapacity), selected(1),
                         dirty(1) {
        record->boneCount = 1;
        record->boneTable = bones.data();
        record->boneKeys = keys.data();
        record->boneSelection = selected.data();
        record->bonePhysicsState = dirty.data();
        record->selectedBone = 0;
        Identity(bones[0].matInit);
        bones[0].parent = -1;
        for (int i = 0; i < 3; ++i)
            keys[i].interpolation[i] = keys[i].interpolation[4 + i] = 20,
            keys[i].interpolation[8 + i] = keys[i].interpolation[12 + i] = 107;
        keys[0].frame = 0;
        keys[0].allocated = 1;
        keys[0].next = 1;
        keys[1].frame = 10;
        keys[1].allocated = 1;
        keys[1].previous = 0;
        for (int c = 0; c < 4; ++c)
            keys[1].interpolation[c] = keys[1].interpolation[4 + c] = 20,
            keys[1].interpolation[8 + c] = keys[1].interpolation[12 + c] = 107;
    }
    unsigned char* data() { return reinterpret_cast<unsigned char*>(record.get()); }
    ~Fixture() { bs::Forget(data()); }
};

// x1/x2 = 20 push the X polynomial ahead of the identity mapping (X(0.5) is
// about 0.243), so the easing runs ahead of time and the eased midpoint lands
// above one half.  kSlow is its mirror: x1/x2 = 120 delay X (X(0.5) is about
// 1.18), so u settles below one half and the eased midpoint lands below it.
const bs::Curve kEase = {20, 20, 20, 107};
const bs::Curve kSlow = {120, 20, 120, 107};

// ---- 1. the 0..127 time mapping -------------------------------------------
void CurveTimeMapping() {
    Check(bs::Linear(bs::LinearCurve()), "the default curve is linear");
    Check(bs::Linear({20, 20, 107, 107}), "20/20/107/107 is linear");
    Check(!bs::Linear(kEase), "an eased tuple is not linear");
    Check(!bs::Linear(kSlow), "the mirrored tuple is not linear either");
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
        Near(bs::CurveTime(bs::LinearCurve(), t), t,
             "a linear curve maps time unchanged");
    Near(bs::CurveTime(kEase, 0.0f), 0.0f, "an eased curve starts at 0");
    Near(bs::CurveTime(kEase, 1.0f), 1.0f, "an eased curve ends at 1");
    // kEase runs ahead of time: the raw = 0.5 midpoint maps to the absolute
    // position 0.79, which is what MMD's own x64 BoneEase returns for these
    // control bytes (the assertion is the number, not a range).
    const float half = bs::CurveTime(kEase, 0.5f);
    Near(half, 0.7917f, "the ahead-of-time tuple maps 0.5 to 0.7917");
    Check(half > 0.5f,
          "x1 = x2 = 20 with y1 = 20 runs the midpoint ahead of one half");
    float previous = -1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float t = static_cast<float>(i) / 100.0f;
        const float eased = bs::CurveTime(kEase, t);
        Check(eased >= previous - 1.0e-4f, "the mapping is monotone");
        previous = eased;
    }
    // The mirrored tuple delays time instead, so it eases the other way.  Both
    // ends are approximate: 12 bisection steps leave about 5.8e-5 at the first
    // and last sample, which is the resolution MMD's own BoneEase has.
    Near(bs::CurveTime(kSlow, 0.0f), 0.0f,
         "the delayed tuple still starts at 0");
    Near(bs::CurveTime(kSlow, 1.0f), 1.0f,
         "the delayed tuple still ends at 1");
    const float slowHalf = bs::CurveTime(kSlow, 0.5f);
    Near(slowHalf, 0.1711f, "the delayed tuple maps 0.5 to 0.1711");
    Check(slowHalf < 0.5f,
          "x1 = x2 = 120 holds the midpoint behind one half");
    std::puts("PASS scale curve time mapping");
}

// ---- 2. storage, identity and slot reuse ---------------------------------
void CurveStorage() {
    Fixture fixture;
    unsigned char* model = fixture.data();
    Check(bs::Linear(bs::KeyCurve(model, 1)),
          "an untouched key slot reports the linear default");

    bs::SetKeyCurve(model, 1, kEase);
    Check(bs::KeyCurve(model, 1) == kEase, "a written curve reads back");
    Check(bs::Linear(bs::KeyCurve(model, 0)),
          "another slot keeps its own default");

    // A registration (SetKey) preserves the curve; an identity value only
    // drops the matrix.
    bs::SetKey(model, 1, {});
    Check(bs::KeyCurve(model, 1) == kEase,
          "SetKey preserves the curve of the key it writes");

    // A slot the pool hands out again must not inherit it.
    bs::SetKeyFresh(model, 1, {});
    Check(bs::Linear(bs::KeyCurve(model, 1)),
          "SetKeyFresh clears a reused slot's curve");

    bs::SetKeyCurve(model, 1, kEase);
    bs::SetKeyCurve(model, 1, bs::LinearCurve());
    Check(bs::Linear(bs::KeyCurve(model, 1)),
          "writing the linear tuple is the same state as no entry");
    bs::SetKeyCurve(model, 1, kEase);
    bs::ClearKey(model, 1);
    Check(bs::Linear(bs::KeyCurve(model, 1)),
          "ClearKey drops the curve with the matrix");

    // CopyKey carries a curve from one slot to another, clipboard likewise.
    bs::SetKeyCurve(model, 1, kEase);
    bs::SetKey(model, 1, {{2, 0, 0, 0, 2, 0, 0, 0, 2}});
    bs::CopyKey(model, 2, 1);
    Check(bs::KeyCurve(model, 2) == kEase, "CopyKey copies the curve");
    Near(bs::Key(model, 2).v[0], 2.0f, "CopyKey copies the matrix");

    auto app = std::make_unique<MMDApp>();
    app->state = MMDAppState{};
    bs::CopyClipboard(app.get(), 0, bs::Key(model, 1), false,
                      bs::KeyCurve(model, 1));
    Check(bs::ClipboardCurve(app.get(), 0, false) == kEase,
          "the key clipboard carries the curve");
    Check(bs::Clipboard(app.get(), 0, false).v[0] == 2.0f,
          "the key clipboard carries the matrix");
    bs::CopyClipboard(app.get(), 0, {}, false);
    Check(bs::Linear(bs::ClipboardCurve(app.get(), 0, false)),
          "a new ordinary copy clears the stale clipboard curve");

    // Payload semantics: an ordinary registration keeps the curve it rewrites,
    // an explicit (clipboard / import) payload writes its curve even when that
    // curve is linear, and a motion import always restores the default.
    bs::SetKeyCurve(model, 2, kEase);
    {
        bs::Registration payload(bs::Key(model, 1));
        bs::ApplyRegistration(model, 2, false);
    }
    Check(bs::KeyCurve(model, 2) == kEase,
          "an ordinary registration keeps the target's curve");
    {
        bs::Registration payload(bs::Key(model, 1), bs::LinearCurve());
        bs::ApplyRegistration(model, 2, false);
    }
    Check(bs::Linear(bs::KeyCurve(model, 2)),
          "an explicit linear payload clears the target's curve");
    bs::SetKeyCurve(model, 2, kEase);
    {
        bs::DefaultCurveScope import;
        bs::Registration payload(bs::Key(model, 1));
        bs::ApplyRegistration(model, 2, false);
    }
    Check(bs::Linear(bs::KeyCurve(model, 2)),
          "a motion import restores the default linear curve");
    bs::SetKeyCurve(model, 2, kEase);
    {
        bs::Registration payload(bs::Key(model, 1), {20, 107, 20, 107});
        bs::ApplyRegistration(model, 2, false);
    }
    Check(bs::KeyCurve(model, 2) == bs::Curve({20, 107, 20, 107}),
          "an explicit payload carries its own curve");
    std::puts("PASS scale curve storage, identity and reuse");
}

// ---- 3. evaluation and undo ----------------------------------------------
void CurveEvaluationAndUndo() {
    Fixture fixture;
    unsigned char* model = fixture.data();
    bs::SetKey(model, 0, {{1, 0, 0, 0, 1, 0, 0, 0, 1}});
    bs::SetKey(model, 1, {{4, 0, 0, 0, 1, 0, 0, 0, 1}});
    // Linear interval: the midpoint is the plain interpolation.
    bs::Evaluate(model, 5.0f);
    Near(bs::Pose(model, 0).v[0], 2.5f, "a linear interval interpolates plainly");

    // The destination key's curve drives the interval, and because kEase runs
    // ahead of time the eased midpoint sits above the plain 2.5, not below it.
    bs::SetKeyCurve(model, 1, kEase);
    bs::Evaluate(model, 5.0f);
    const float expected = bs::Interpolate({{1, 0, 0, 0, 1, 0, 0, 0, 1}},
                                           {{4, 0, 0, 0, 1, 0, 0, 0, 1}},
                                           bs::CurveTime(kEase, 0.5f)).v[0];
    Near(bs::Pose(model, 0).v[0], expected,
         "the destination key's curve maps the interval time");
    Check(bs::Pose(model, 0).v[0] > 2.5f,
          "the ahead-of-time midpoint stays above the linear one");

    // The start key's own curve does not own the interval.  kSlow is written on
    // the start key and the sample must not move towards its slower mapping.
    bs::SetKeyCurve(model, 0, kSlow);
    bs::Evaluate(model, 5.0f);
    Near(bs::Pose(model, 0).v[0], expected,
         "the start key's curve does not govern the interval");

    // Sample() reads the track without touching the live pose.
    bs::Pose(model, 0);
    Near(bs::Sample(model, 0, 5.0f).v[0], expected,
         "Sample applies the same curve without a live pose");

    // Undo and redo carry the curve with the matrix.
    bs::CaptureUndo(model);
    bs::SetKeyCurve(model, 1, bs::LinearCurve());
    Check(bs::Linear(bs::KeyCurve(model, 1)), "the curve was cleared");
    bs::Undo(model, mdl::Mdl(model)->undoState[0]);
    Check(bs::KeyCurve(model, 1) == kEase, "undo restores the curve");
    bs::Redo(model, mdl::Mdl(model)->undoState[0]);
    Check(bs::Linear(bs::KeyCurve(model, 1)), "redo re-applies the edit");

    // The snapshot API carries the curves too (PMM/VMD capture path).  Key 0
    // holds kSlow at this point; a linear curve is stored as "no entry", so the
    // snapshot must name key 0 while key 1 stays linear.
    bs::Snapshot snapshot = bs::Capture(model);
    Check(snapshot.curves.find(0) != snapshot.curves.end(),
          "Capture carries the curves of the model");
    Check(snapshot.curves.find(1) == snapshot.curves.end(),
          "Capture leaves a linear key without a curve entry");
    bs::SetKeyCurve(model, 0, bs::LinearCurve());
    Check(bs::Restore(model, snapshot), "the snapshot restores");
    Check(bs::KeyCurve(model, 0) == kSlow,
          "Restore brings the curves back");
    std::puts("PASS scale curve evaluation, undo and snapshot");
}

// ---- 4. panel channel layout ---------------------------------------------
void PanelChannelLayout() {
    Check(bs::kInterpScaleChannel == 4, "the scale channel sits after rotation");
    Check(bs::kInterpBoneAll == 5, "the model panel's all entry follows it");
    Check(bs::kInterpCameraAll == 6,
          "the camera panel keeps its own all entry");
    Check(bs::kInterpScaleChannel < bs::kInterpBoneAll &&
              bs::kInterpBoneAll < bs::kInterpCameraAll,
          "the three indices are distinct and ordered");
    std::puts("PASS scale curve panel channel layout");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CurveTimeMapping();
    CurveStorage();
    CurveEvaluationAndUndo();
    PanelChannelLayout();
    std::puts("PASS: native bone scale interpolation curve");
}
