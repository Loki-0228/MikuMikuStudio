#include "animation_curve_insert_checks.hpp"
// MAINTENANCE: unit coverage for the shared animation curve core
// (src/animation/animation_curve.cpp).  Pure CPU; no Win32, DirectX or GUI.
#include "mikudancestudio/animation_curve.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ac = mikudancestudio::animation_curve;

namespace {

int g_failures = 0;

void Check(bool pass, const std::string &text) {
    if (pass) return;
    ++g_failures;
    std::printf("FAIL %s\n", text.c_str());
}

void CheckNear(double actual, double expected, double tolerance, const std::string &text) {
    if (std::isfinite(actual) && std::fabs(actual - expected) <= tolerance) return;
    ++g_failures;
    std::printf("FAIL %s: expected %.17g, got %.17g (tolerance %.3g)\n", text.c_str(), expected, actual,
                tolerance);
}

ac::Key MakeKey(std::int32_t frame, double value, double inSlope = 0.0, double outSlope = 0.0,
                double inWeight = 1.0 / 3.0, double outWeight = 1.0 / 3.0,
                ac::Interpolation interpolation = ac::Interpolation::Bezier,
                ac::Tangent tangent = ac::Tangent::Free) {
    ac::Key key;
    key.frame = frame;
    key.value = value;
    key.inSlope = inSlope;
    key.outSlope = outSlope;
    key.inWeight = inWeight;
    key.outWeight = outWeight;
    key.interpolation = interpolation;
    key.tangent = tangent;
    return key;
}

// Independent evaluation of a cubic Bezier component, used to check the
// implementation against the definition instead of against itself.
double BezierComponent(double p0, double p1, double p2, double p3, double s) {
    const double t = 1.0 - s;
    return t * t * t * p0 + 3.0 * t * t * s * p1 + 3.0 * t * s * s * p2 + s * s * s * p3;
}

double BezierComponentDerivative(double p0, double p1, double p2, double p3, double s) {
    const double t = 1.0 - s;
    return 3.0 * t * t * (p1 - p0) + 6.0 * t * s * (p2 - p1) + 3.0 * s * s * (p3 - p2);
}

bool SameKey(const ac::Key &left, const ac::Key &right) {
    return left.frame == right.frame && left.value == right.value && left.inSlope == right.inSlope &&
           left.outSlope == right.outSlope && left.inWeight == right.inWeight &&
           left.outWeight == right.outWeight && left.interpolation == right.interpolation &&
           left.tangent == right.tangent;
}

bool SameTrack(const ac::Track &left, const ac::Track &right) {
    if (left.discrete != right.discrete || left.keys.size() != right.keys.size()) return false;
    for (std::size_t index = 0; index < left.keys.size(); ++index) {
        if (!SameKey(left.keys[index], right.keys[index])) return false;
    }
    return true;
}

bool SameTracks(const std::vector<ac::NamedTrack> &left, const std::vector<ac::NamedTrack> &right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].id != right[index].id || !SameTrack(left[index].track, right[index].track)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

void TestValidation() {
    ac::Track track;
    std::string error;
    track.keys = {MakeKey(0, 1.0), MakeKey(10, 2.0)};
    Check(ac::Validate(track, error), "a plain two-key track validates: " + error);
    Check(error.empty(), "Validate clears the error string on success");

    ac::Track empty;
    Check(ac::Validate(empty, error), "an empty track validates");

    track.keys[1].frame = 0;
    Check(!ac::Validate(track, error) && !error.empty(), "equal frames are rejected");
    track.keys[0].frame = 4;
    Check(!ac::Validate(track, error), "out-of-order frames are rejected");
    track.keys[0].frame = 0;
    track.keys[1].frame = 10;

    track.keys[0].frame = -3;
    Check(!ac::Validate(track, error), "a negative frame is rejected");
    track.keys[0].frame = 0;

    track.keys[0].value = std::numeric_limits<double>::quiet_NaN();
    Check(!ac::Validate(track, error), "a NaN value is rejected");
    track.keys[0].value = 1.0;

    track.keys[1].inSlope = std::numeric_limits<double>::infinity();
    Check(!ac::Validate(track, error), "an infinite slope is rejected");
    track.keys[1].inSlope = 0.0;

    track.keys[0].outWeight = 0.0;
    Check(!ac::Validate(track, error), "a zero handle weight is rejected");
    track.keys[0].outWeight = 1.0;
    Check(!ac::Validate(track, error), "a full handle weight is rejected");
    track.keys[0].outWeight = 1.0 / 3.0;

    track.keys[1].inWeight = std::numeric_limits<double>::quiet_NaN();
    Check(!ac::Validate(track, error), "a NaN handle weight is rejected");
    track.keys[1].inWeight = 1.0 / 3.0;

    track.keys[0].outWeight = 0.5;
    track.keys[1].inWeight = 0.5;
    Check(!ac::Validate(track, error), "adjacent handle weights summing to 1 are rejected");
    track.keys[0].outWeight = 0.4999;
    track.keys[1].inWeight = 0.5;
    Check(ac::Validate(track, error), "adjacent handle weights summing to 0.9999 validate: " + error);
    track.keys[1].inWeight = 0.5001;
    Check(!ac::Validate(track, error), "adjacent handle weights above 0.9999 are rejected");
    track.keys[0].outWeight = 1.0 / 3.0;
    track.keys[1].inWeight = 1.0 / 3.0;

    track.keys[0].interpolation = static_cast<ac::Interpolation>(99);
    Check(!ac::Validate(track, error), "an unknown interpolation value is rejected");
    track.keys[0].interpolation = ac::Interpolation::Bezier;
    track.keys[0].tangent = static_cast<ac::Tangent>(77);
    Check(!ac::Validate(track, error), "an unknown tangent value is rejected");
    track.keys[0].tangent = ac::Tangent::Free;

    ac::Track dense;
    dense.keys.resize(1000001);
    for (std::size_t i = 0; i < dense.keys.size(); ++i) dense.keys[i].frame = static_cast<int>(i);
    Check(ac::Validate(dense, error), "a curve can exceed the former million-key ceiling");

}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

void TestEvaluation() {
    ac::Track track;
    track.keys = {MakeKey(0, 1.0, 0.0, 0.5), MakeKey(10, 3.0, 0.5, -0.25), MakeKey(20, 2.0, -0.25, 0.0)};
    std::string error;
    Check(ac::Validate(track, error), "the evaluation fixture validates: " + error);

    CheckNear(ac::Evaluate(track, 0.0).value, 1.0, 0.0, "the first key evaluates exactly");
    CheckNear(ac::Evaluate(track, 10.0).value, 3.0, 0.0, "an interior key evaluates exactly");
    CheckNear(ac::Evaluate(track, 20.0).value, 2.0, 0.0, "the last key evaluates exactly");
    CheckNear(ac::Evaluate(track, 10.0).derivative, track.keys[1].outSlope, 0.0,
              "a key reports its outgoing slope");
    CheckNear(ac::Evaluate(track, 20.0).derivative, track.keys[2].inSlope, 0.0,
              "the last key reports its incoming slope");
    CheckNear(ac::Evaluate(track, -50.0).value, 1.0, 0.0, "frames before the track clamp to the first value");
    CheckNear(ac::Evaluate(track, -50.0).derivative, 0.0, 0.0, "clamping outside the track has no slope");
    CheckNear(ac::Evaluate(track, 500.0).value, 2.0, 0.0, "frames after the track clamp to the last value");
    CheckNear(ac::Evaluate(track, 500.0).derivative, 0.0, 0.0, "clamping past the track has no slope");
    CheckNear(ac::Evaluate(track, std::numeric_limits<double>::quiet_NaN()).value, 1.0, 0.0,
              "a NaN frame falls back to the first value");

    ac::Track single;
    single.keys = {MakeKey(7, 4.5, 2.0, 2.0)};
    CheckNear(ac::Evaluate(single, 7.0).value, 4.5, 0.0, "a single-key track evaluates at its key");
    CheckNear(ac::Evaluate(single, 7.0).derivative, 0.0, 0.0, "a single-key track has no slope");
    CheckNear(ac::Evaluate(single, 100.0).value, 4.5, 0.0, "a single-key track clamps");

    ac::Track none;
    CheckNear(ac::Evaluate(none, 12.0).value, 0.0, 0.0, "an empty track evaluates to zero");

    // Derivative versus a central difference over a curved, non-uniform track.
    ac::Track curved;
    curved.keys = {MakeKey(0, 0.0, 0.0, 1.2, 0.25, 0.3), MakeKey(10, 8.0, 0.9, -0.4, 0.4, 0.2),
                   MakeKey(25, 3.0, -0.7, 0.5, 0.33, 0.31), MakeKey(40, 12.0, 0.2, 0.0, 0.3, 0.3)};
    Check(ac::Validate(curved, error), "the curved fixture validates: " + error);
    const double h = 1e-3;
    for (std::size_t index = 0; index + 1 < curved.keys.size(); ++index) {
        const double start = curved.keys[index].frame;
        const double end = curved.keys[index + 1].frame;
        for (double frame = start + 0.5; frame < end; frame += 0.5) {
            const ac::Sample sample = ac::Evaluate(curved, frame);
            const double difference =
                (ac::Evaluate(curved, frame + h).value - ac::Evaluate(curved, frame - h).value) / (2.0 * h);
            CheckNear(sample.derivative, difference, 1e-5 * (1.0 + std::fabs(difference)),
                      "the analytic derivative matches a central difference at frame " +
                          std::to_string(frame));
        }
    }

    // Time is inverted, not assumed linear.
    ac::Track eased;
    eased.keys = {MakeKey(0, 0.0, 0.0, 0.0, 0.0001, 0.9), MakeKey(10, 10.0, 0.0, 0.0, 0.0001, 0.9)};
    Check(ac::Validate(eased, error), "the eased fixture validates: " + error);
    const double easedValue = ac::Evaluate(eased, 5.0).value;
    Check(easedValue < 3.0 && easedValue > 0.5,
          "a late-time handle pushes the midpoint value away from the linear midpoint, got " +
              std::to_string(easedValue));
    Check(ac::Evaluate(eased, 5.0).derivative > 0.0, "the eased curve still rises");

    // Defined interval shapes, checked against the Bezier definition.
    ac::Track bezier;
    bezier.keys = {MakeKey(4, 2.0, 0.0, 1.5, 1.0 / 3.0, 0.25),
                   MakeKey(14, -3.0, -0.5, 0.0, 0.4, 1.0 / 3.0)};
    Check(ac::Validate(bezier, error), "the Bezier fixture validates: " + error);
    const double x1 = bezier.keys[0].outWeight;
    const double x2 = 1.0 - bezier.keys[1].inWeight;
    const double span = 10.0;
    const double y0 = 2.0;
    const double y1 = y0 + bezier.keys[0].outSlope * bezier.keys[0].outWeight * span;
    const double y2 = -3.0 - bezier.keys[1].inSlope * bezier.keys[1].inWeight * span;
    const double y3 = -3.0;
    for (int step = 1; step < 10; ++step) {
        const double s = step / 10.0;
        const double frame = 4.0 + span * BezierComponent(0.0, x1, x2, 1.0, s);
        const double value = BezierComponent(y0, y1, y2, y3, s);
        const double slope = BezierComponentDerivative(y0, y1, y2, y3, s) /
                             (span * BezierComponentDerivative(0.0, x1, x2, 1.0, s));
        const ac::Sample sample = ac::Evaluate(bezier, frame);
        CheckNear(sample.value, value, 1e-9 * (1.0 + std::fabs(value)),
                  "the value at the inverted time matches the Bezier for s = " + std::to_string(s));
        CheckNear(sample.derivative, slope, 1e-9 * (1.0 + std::fabs(slope)),
                  "the derivative at the inverted time matches the Bezier for s = " + std::to_string(s));
    }
}

void TestStepAndLinear() {
    std::string error;
    ac::Track stepped;
    stepped.keys = {MakeKey(0, 1.0), MakeKey(10, 5.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0,
                                        ac::Interpolation::Step),
                    MakeKey(20, 9.0)};
    Check(ac::Validate(stepped, error), "the step fixture validates: " + error);
    CheckNear(ac::Evaluate(stepped, 0.0).value, 1.0, 0.0, "a step interval starts at the previous key value");
    CheckNear(ac::Evaluate(stepped, 9.999).value, 1.0, 0.0, "a step interval holds its value up to the key");
    CheckNear(ac::Evaluate(stepped, 10.0).value, 5.0, 0.0, "a step interval lands exactly on the key value");
    CheckNear(ac::Evaluate(stepped, 10.0).derivative, 0.0, 0.0, "a step interval has no slope");
    CheckNear(ac::Evaluate(stepped, 5.0).derivative, 0.0, 0.0, "the step interval carries no slope");

    ac::Track discrete = stepped;
    discrete.discrete = true;
    CheckNear(ac::Evaluate(discrete, 15.0).value, 5.0, 0.0, "a discrete track holds the previous key value");
    CheckNear(ac::Evaluate(discrete, 20.0).value, 9.0, 0.0, "a discrete track still lands on its keys");

    ac::Track linear;
    linear.keys = {MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier),
                   MakeKey(8, 4.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Linear)};
    Check(ac::Validate(linear, error), "the linear fixture validates: " + error);
    CheckNear(ac::Evaluate(linear, 4.0).value, 2.0, 0.0, "a linear interval is the exact chord");
    CheckNear(ac::Evaluate(linear, 4.0).derivative, 0.5, 0.0, "a linear interval has the secant slope");
    CheckNear(ac::Evaluate(linear, 8.0).value, 4.0, 0.0, "a linear interval ends on the key value");
}

// ---------------------------------------------------------------------------
// Tangents
// ---------------------------------------------------------------------------

void TestTangents() {
    std::string error;
    ac::Track track;
    track.keys = {MakeKey(0, 0.0, 0.0, 1.5), MakeKey(10, 4.0, 1.0, -2.0), MakeKey(20, 6.0, 0.4, 0.0)};
    Check(ac::SetTangent(track, 1, ac::Tangent::Aligned, error), "an aligned tangent is accepted: " + error);
    CheckNear(track.keys[1].inSlope, -0.5, 0.0, "an aligned tangent averages the two slopes");
    CheckNear(track.keys[1].outSlope, -0.5, 0.0, "an aligned tangent writes the same slope on both sides");
    Check(track.keys[1].tangent == ac::Tangent::Aligned, "the aligned tag is stored");

    Check(ac::SetTangent(track, 1, ac::Tangent::Flat, error), "a flat tangent is accepted: " + error);
    CheckNear(track.keys[1].inSlope, 0.0, 0.0, "a flat tangent zeroes the incoming slope");
    CheckNear(track.keys[1].outSlope, 0.0, 0.0, "a flat tangent zeroes the outgoing slope");
    CheckNear(ac::Evaluate(track, 10.0).derivative, 0.0, 0.0, "a flat key has no slope");

    Check(ac::SetTangent(track, 1, ac::Tangent::Free, error), "a free tangent is accepted: " + error);
    CheckNear(track.keys[1].inSlope, 0.0, 0.0, "a free tangent keeps the stored slope");

    Check(!ac::SetTangent(track, 9, ac::Tangent::Free, error), "an out-of-range key index is rejected");
    Check(!error.empty(), "the rejected index carries an error message");
    Check(!ac::SetTangent(track, 0, static_cast<ac::Tangent>(77), error),
          "an unknown tangent value is rejected");

    // Linear must rewrite the facing slope of both neighbours, otherwise the
    // neighbouring intervals would keep their curvature.
    ac::Track curve;
    curve.keys = {MakeKey(0, 0.0, 0.0, 0.0), MakeKey(10, 10.0, 0.0, 0.0), MakeKey(20, 30.0, 0.0, 0.0)};
    Check(ac::SetTangent(curve, 1, ac::Tangent::Linear, error), "a linear tangent is accepted: " + error);
    CheckNear(curve.keys[1].inSlope, 1.0, 0.0, "the linear key takes the left secant");
    CheckNear(curve.keys[1].outSlope, 2.0, 0.0, "the linear key takes the right secant");
    CheckNear(curve.keys[0].outSlope, 1.0, 0.0, "the previous key takes the same left secant");
    CheckNear(curve.keys[2].inSlope, 2.0, 0.0, "the next key takes the same right secant");
    CheckNear(ac::Evaluate(curve, 5.0).value, 5.0, 1e-9, "the left interval becomes exactly linear");
    CheckNear(ac::Evaluate(curve, 5.0).derivative, 1.0, 1e-9, "the left interval keeps the secant slope");
    CheckNear(ac::Evaluate(curve, 15.0).value, 20.0, 1e-9, "the right interval becomes exactly linear");
    CheckNear(ac::Evaluate(curve, 15.0).derivative, 2.0, 1e-9, "the right interval keeps the secant slope");

    // A single-key track has no neighbouring interval to straighten.
    ac::Track single;
    single.keys = {MakeKey(4, 2.0, 3.0, 3.0)};
    Check(ac::SetTangent(single, 0, ac::Tangent::Linear, error), "a linear tangent on a lone key is accepted");
    CheckNear(single.keys[0].inSlope, 3.0, 0.0, "a lone key keeps its slope");

    // Leaving the automatic mode freezes the slope the mode produced.
    ac::Track automatic;
    automatic.keys = {MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                              ac::Tangent::Automatic),
                      MakeKey(10, 10.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                              ac::Tangent::Automatic),
                      MakeKey(20, 30.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                              ac::Tangent::Automatic)};
    ac::UpdateAutomatic(automatic);
    const double frozen = automatic.keys[1].outSlope;
    Check(ac::SetTangent(automatic, 1, ac::Tangent::Free, error), "freezing an automatic key is accepted");
    ac::UpdateAutomatic(automatic);
    CheckNear(automatic.keys[1].outSlope, frozen, 0.0, "a frozen key keeps the automatic slope");
    Check(automatic.keys[0].tangent == ac::Tangent::Automatic, "the other automatic keys keep their mode");

    // Requesting linear on an automatic key frees the neighbours it rewrites.
    ac::Track neighbours;
    neighbours.keys = {MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                               ac::Tangent::Automatic),
                       MakeKey(10, 10.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                               ac::Tangent::Automatic),
                       MakeKey(20, 30.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                               ac::Tangent::Automatic)};
    ac::UpdateAutomatic(neighbours);
    Check(ac::SetTangent(neighbours, 1, ac::Tangent::Linear, error), "linear on an automatic key is accepted");
    Check(neighbours.keys[0].tangent == ac::Tangent::Free,
          "the rewritten previous key leaves the automatic mode");
    Check(neighbours.keys[2].tangent == ac::Tangent::Free, "the rewritten next key leaves the automatic mode");
    CheckNear(ac::Evaluate(neighbours, 15.0).value, 20.0, 1e-9, "the linear request survives the file state");
}

void TestAutomatic() {
    std::string error;
    ac::Track bump;
    bump.keys = {MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                         ac::Tangent::Automatic),
                 MakeKey(3, 10.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                         ac::Tangent::Automatic),
                 MakeKey(6, 5.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                         ac::Tangent::Automatic)};
    ac::UpdateAutomatic(bump);
    CheckNear(bump.keys[1].inSlope, 0.0, 0.0, "an automatic local maximum is horizontal");
    CheckNear(bump.keys[1].outSlope, 0.0, 0.0, "an automatic local maximum is horizontal on both sides");
    CheckNear(bump.keys[0].outSlope, 10.0 / 3.0, 1e-12, "the first automatic key uses the one-sided secant");
    CheckNear(bump.keys[2].inSlope, -5.0 / 3.0, 1e-12, "the last automatic key uses the one-sided secant");
    double lowest = 1e300;
    double highest = -1e300;
    for (double frame = 0.0; frame <= 6.0; frame += 0.01) {
        const double value = ac::Evaluate(bump, frame).value;
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
    }
    Check(lowest >= -1e-9, "an automatic bump does not undershoot its keys, got " + std::to_string(lowest));
    Check(highest <= 10.0 + 1e-9,
          "an automatic bump does not overshoot its keys, got " + std::to_string(highest));

    ac::Track ramp;
    ramp.keys = {MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                         ac::Tangent::Automatic),
                 MakeKey(10, 1.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                         ac::Tangent::Automatic),
                 MakeKey(20, 9.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                         ac::Tangent::Automatic),
                 MakeKey(30, 30.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                         ac::Tangent::Automatic)};
    ac::UpdateAutomatic(ramp);
    CheckNear(ramp.keys[1].inSlope, ramp.keys[1].outSlope, 0.0,
              "an automatic interior key has one slope on both sides");
    double previous = -1e300;
    bool monotone = true;
    for (double frame = 0.0; frame <= 30.0; frame += 0.05) {
        const double value = ac::Evaluate(ramp, frame).value;
        if (value < previous - 1e-9) monotone = false;
        previous = value;
    }
    Check(monotone, "an automatic monotone ramp stays monotone between its keys");

    ac::Track mixed;
    mixed.keys = {MakeKey(0, 0.0, 0.0, 7.0), MakeKey(10, 4.0, -3.0, -3.0),
                  MakeKey(20, 2.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                          ac::Tangent::Automatic)};
    ac::UpdateAutomatic(mixed);
    CheckNear(mixed.keys[0].outSlope, 7.0, 0.0, "UpdateAutomatic leaves a free key alone");
    CheckNear(mixed.keys[1].inSlope, -3.0, 0.0, "UpdateAutomatic leaves a free key alone on both sides");
    Check(ac::Validate(mixed, error), "the mixed fixture still validates: " + error);
}

// ---------------------------------------------------------------------------
// MMD handle bridge
// ---------------------------------------------------------------------------

void TestMmdControls() {
    std::string error;
    std::array<double, 4> controls{};

    ac::Track flat;
    flat.keys = {MakeKey(0, 3.0, 0.0, 1.0), MakeKey(10, 3.0, 1.0, 0.0)};
    Check(!ac::GetMmdControls(flat, 1, controls), "equal-valued endpoints are refused");
    Check(!ac::GetMmdControls(flat, 0, controls), "the first key has no interval ending at it");

    ac::Track stepped;
    stepped.keys = {MakeKey(0, 0.0), MakeKey(10, 5.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0,
                                        ac::Interpolation::Step)};
    Check(!ac::GetMmdControls(stepped, 1, controls), "a step interval is refused");

    ac::Track steep;
    steep.keys = {MakeKey(0, 0.0, 0.0, 1.5, 1.0 / 3.0, 0.5), MakeKey(10, 10.0, 0.0, 0.0)};
    Check(ac::Validate(steep, error), "the steep fixture validates: " + error);
    Check(ac::GetMmdControls(steep, 1, controls), "a moderate handle set is readable");
    CheckNear(controls[0], 64.0, 0.0, "x1 comes from the outgoing handle weight");
    CheckNear(controls[1], 95.0, 0.0, "y1 comes from the outgoing handle slope");

    ac::Track overshoot;
    overshoot.keys = {MakeKey(0, 0.0, 0.0, 20.0, 0.5, 0.4999), MakeKey(10, 10.0, 0.0, 0.0, 0.4999, 0.5)};
    Check(ac::Validate(overshoot, error), "the overshoot fixture validates: " + error);
    Check(!ac::GetMmdControls(overshoot, 1, controls), "a value handle above 1 is refused");

    ac::Track discrete;
    discrete.keys = {MakeKey(0, 0.0), MakeKey(10, 5.0)};
    discrete.discrete = true;
    Check(!ac::GetMmdControls(discrete, 1, controls), "a discrete track has no MMD controls");
    Check(!ac::SetMmdControls(discrete, 1, {32.0, 0.0, 96.0, 127.0}, error),
          "setting controls on a discrete track is refused");

    ac::Track track;
    track.keys = {MakeKey(0, 0.0), MakeKey(10, 10.0)};
    const std::array<double, 4> expected{32.0, 0.0, 96.0, 127.0};
    Check(ac::SetMmdControls(track, 1, expected, error), "MMD controls are accepted: " + error);
    Check(ac::Validate(track, error), "the track stays valid after setting controls: " + error);
    CheckNear(track.keys[0].outWeight, 32.0 / 127.0, 0.0, "x1 becomes the outgoing handle weight");
    CheckNear(track.keys[1].inWeight, 31.0 / 127.0, 0.0, "x2 becomes the incoming handle weight");
    CheckNear(track.keys[0].outSlope, 0.0, 0.0, "y1 = 0 gives a zero outgoing slope");
    CheckNear(track.keys[1].inSlope, 0.0, 0.0, "y2 = 127 gives a zero incoming slope");
    Check(track.keys[1].interpolation == ac::Interpolation::Bezier, "a curved handle set stays Bezier");
    Check(ac::GetMmdControls(track, 1, controls), "the controls can be read back");
    Check(controls == expected, "the controls survive the round trip");

    // The stored curve must match the same cubic evaluated independently.
    {
        const double x1 = 32.0 / 127.0;
        const double x2 = 96.0 / 127.0;
        double low = 0.0;
        double high = 1.0;
        for (int iteration = 0; iteration < 200; ++iteration) {
            const double middle = 0.5 * (low + high);
            if (BezierComponent(0.0, x1, x2, 1.0, middle) < 0.5) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const double independent = BezierComponent(0.0, 0.0, 1.0, 1.0, 0.5 * (low + high));
        CheckNear(ac::Evaluate(track, 5.0).value, 10.0 * independent, 1e-9,
                  "the interval follows the MMD cubic through (0,0), (x1,y1), (x2,y2), (1,1)");
    }

    const std::array<double, 4> diagonal{42.0, 42.0, 85.0, 85.0};
    Check(ac::SetMmdControls(track, 1, diagonal, error), "diagonal controls are accepted: " + error);
    Check(track.keys[1].interpolation == ac::Interpolation::Linear,
          "controls on the diagonal mark the interval linear");
    CheckNear(ac::Evaluate(track, 5.0).value, 5.0, 1e-9, "a diagonal control set is the exact chord");
    Check(ac::GetMmdControls(track, 1, controls), "the diagonal controls can be read back");
    Check(controls == diagonal, "the diagonal controls survive the round trip");

    ac::Track curved;
    curved.keys = {MakeKey(0, 0.0), MakeKey(10, 10.0)};
    Check(!ac::SetMmdControls(curved, 1, {96.0, 0.0, 32.0, 127.0}, error),
          "controls with x1 >= x2 are rejected");
    Check(!error.empty(), "the rejected control order carries an error message");
    Check(!ac::SetMmdControls(curved, 1, {32.0, 0.0, 140.0, 127.0}, error),
          "controls above 127 are rejected");
    Check(!ac::SetMmdControls(curved, 1, {32.0, 0.0, 96.0, std::numeric_limits<double>::quiet_NaN()},
                              error),
          "non-finite controls are rejected");
    Check(ac::SetMmdControls(curved, 1, {32.0, 0.0, 96.0, 127.0}, error),
          "a representable handle set is accepted: " + error);
    ac::Track equal;
    equal.keys = {MakeKey(0, 2.0), MakeKey(10, 2.0)};
    Check(!ac::SetMmdControls(equal, 1, {32.0, 0.0, 96.0, 127.0}, error),
          "equal-valued endpoints are refused when setting controls");
    Check(!ac::SetMmdControls(curved, 5, {32.0, 0.0, 96.0, 127.0}, error),
          "an out-of-range key index is refused when setting controls");
}

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

ac::Track MakeBakeFixture() {
    ac::Track track;
    track.keys = {MakeKey(0, 0.0, 0.0, 1.5), MakeKey(10, 10.0, 1.0, -1.0), MakeKey(20, 4.0, -0.6, 0.0)};
    return track;
}

void TestBake() {
    std::string error;
    const ac::Track curve = MakeBakeFixture();
    Check(ac::Validate(curve, error), "the bake fixture validates: " + error);

    ac::Track baked;
    Check(ac::Bake(curve, 0, 20, 1, baked, error), "a full-range bake succeeds: " + error);
    Check(baked.keys.size() == 21, "a full-range bake covers every frame");
    Check(ac::Validate(baked, error), "the baked track validates: " + error);
    for (std::size_t index = 0; index < baked.keys.size(); ++index) {
        Check(baked.keys[index].frame == static_cast<std::int32_t>(index), "baked frames are consecutive");
        CheckNear(baked.keys[index].value, ac::Evaluate(curve, static_cast<double>(index)).value, 0.0,
                  "the baked key keeps the original value at frame " + std::to_string(index));
    }
    CheckNear(baked.keys[0].outSlope, baked.keys[1].value - baked.keys[0].value, 1e-12,
              "baked keys carry the chord slope so the segments stay straight");
    Check(baked.discrete == false, "a non-discrete track bakes to a non-discrete track");

    ac::Track coarse;
    Check(ac::Bake(curve, 0, 20, 7, coarse, error), "a coarse bake succeeds: " + error);
    Check(coarse.keys.size() == 4, "a coarse bake keeps the final frame");
    Check(coarse.keys.front().frame == 0 && coarse.keys.back().frame == 20,
          "a coarse bake spans the whole range");
    Check(coarse.keys[1].frame == 7 && coarse.keys[2].frame == 14, "a coarse bake follows the step");

    ac::Track discreteCurve = curve;
    discreteCurve.discrete = true;
    ac::Track discreteBaked;
    Check(ac::Bake(discreteCurve, 0, 20, 1, discreteBaked, error), "a discrete bake succeeds: " + error);
    Check(discreteBaked.discrete, "baking keeps the discrete flag");
    Check(discreteBaked.keys[1].interpolation == ac::Interpolation::Step,
          "a discrete bake writes step intervals");

    ac::Track sentinel;
    sentinel.keys = {MakeKey(99, 99.0)};
    ac::Track output = sentinel;
    Check(!ac::Bake(curve, 0, 10, 1, output, error), "a partial range is refused");
    Check(error.find("whole track") != std::string::npos,
          "the refusal explains that the range must cover the whole track, got \"" + error + "\"");
    Check(SameTrack(output, sentinel), "a refused bake leaves the output untouched");
    output = sentinel;
    Check(!ac::Bake(curve, 20, 30, 1, output, error), "a range that starts late is refused");
    Check(SameTrack(output, sentinel), "the late range leaves the output untouched");
    output = sentinel;
    Check(!ac::Bake(curve, 0, 20, 0, output, error), "a zero step is refused");
    Check(SameTrack(output, sentinel), "the zero step leaves the output untouched");
    output = sentinel;
    Check(!ac::Bake(curve, 10, 5, 1, output, error), "an inverted range is refused");
    Check(SameTrack(output, sentinel), "the inverted range leaves the output untouched");
    output = sentinel;
    Check(!ac::Bake(curve, -5, 20, 1, output, error), "a negative first frame is refused");
    Check(SameTrack(output, sentinel), "the negative range leaves the output untouched");
    output = sentinel;
    Check(ac::Bake(curve, 0, 1000001, 1, output, error), "baking can exceed a million keys");
    Check(output.keys.size() == 1000002 && output.keys.back().frame == 1000001,
          "large bake keeps every requested frame");

    ac::Track empty;
    Check(!ac::Bake(empty, 0, 10, 1, output, error), "baking an empty track is refused");
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

const std::size_t kKeyFieldOffsets[8] = {0, 4, 12, 20, 28, 36, 44, 48};

struct RawKey {
    std::int32_t frame = 0;
    double value = 0.0;
    double inSlope = 0.0;
    double outSlope = 0.0;
    double inWeight = 1.0 / 3.0;
    double outWeight = 1.0 / 3.0;
    std::uint32_t interpolation = 2;
    std::uint32_t tangent = 0;
};

struct RawTrack {
    std::string id;
    bool discrete = false;
    std::vector<RawKey> keys;
};

void PushU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PushU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void PushF64(std::vector<std::uint8_t> &out, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((bits >> shift) & 0xFFu));
    }
}

// Writes the version 1 layout from the specification, independently of the
// implementation under test.
std::vector<std::uint8_t> BuildPayload(const std::vector<RawTrack> &tracks, std::uint16_t version = 1,
                                       std::uint16_t flags = 0) {
    std::vector<std::uint8_t> out;
    out.push_back('M');
    out.push_back('M');
    out.push_back('S');
    out.push_back('C');
    PushU16(out, version);
    PushU16(out, flags);
    PushU32(out, static_cast<std::uint32_t>(tracks.size()));
    for (const RawTrack &track : tracks) {
        PushU32(out, static_cast<std::uint32_t>(track.id.size()));
        out.insert(out.end(), track.id.begin(), track.id.end());
        out.push_back(track.discrete ? 1 : 0);
        PushU32(out, static_cast<std::uint32_t>(track.keys.size()));
        for (const RawKey &key : track.keys) {
            PushU32(out, static_cast<std::uint32_t>(key.frame));
            PushF64(out, key.value);
            PushF64(out, key.inSlope);
            PushF64(out, key.outSlope);
            PushF64(out, key.inWeight);
            PushF64(out, key.outWeight);
            PushU32(out, key.interpolation);
            PushU32(out, key.tangent);
        }
    }
    return out;
}

std::size_t KeyFieldOffset(const std::string &id, std::size_t keyIndex, std::size_t fieldIndex) {
    return 12 + 4 + id.size() + 1 + 4 + keyIndex * 52 + kKeyFieldOffsets[fieldIndex];
}

void CheckDecodeFails(const std::vector<std::uint8_t> &payload, const std::string &text,
                      const std::string &fragment = std::string()) {
    ac::NamedTrack sentinel;
    sentinel.id = "sentinel";
    sentinel.track.keys = {MakeKey(3, 3.0)};
    std::vector<ac::NamedTrack> target = {sentinel};
    std::string error;
    const bool ok = ac::Decode(payload.data(), payload.size(), target, error);
    if (ok) {
        Check(false, text + ": decode should have failed");
        return;
    }
    Check(!error.empty(), text + ": a failure carries an error message");
    if (!fragment.empty()) {
        Check(error.find(fragment) != std::string::npos,
              text + ": the error should mention \"" + fragment + "\", got \"" + error + "\"");
    }
    Check(target.size() == 1 && target[0].id == "sentinel" && target[0].track.keys.size() == 1 &&
              target[0].track.keys[0].frame == 3,
          text + ": the output stays untouched");
}

void TestCodecRoundTrip() {
    std::vector<ac::NamedTrack> tracks;
    ac::NamedTrack expression;
    expression.id = "\u8868\u60c5/\u307e\u3070\u305f\u304d";
    expression.track.keys = {MakeKey(0, 1.0, 0.0, 0.25, 0.25, 0.3, ac::Interpolation::Bezier,
                                     ac::Tangent::Aligned),
                             MakeKey(12, 1.75, -0.5, 0.0, 0.4, 0.1, ac::Interpolation::Linear,
                                     ac::Tangent::Flat),
                             MakeKey(30, 0.25, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step,
                                     ac::Tangent::Automatic)};
    tracks.push_back(expression);
    ac::NamedTrack parameter;
    parameter.id = "MMV.param.1";
    parameter.track.discrete = true;
    parameter.track.keys = {MakeKey(4, -2.5, 1.0, 1.0, 0.5, 0.4999), MakeKey(5, 7.25, -0.5, 0.0)};
    tracks.push_back(parameter);
    tracks.push_back(ac::NamedTrack{"empty", ac::Track{}});
    std::string error;
    Check(ac::Validate(tracks[0].track, error), "round-trip fixture 0 validates: " + error);
    Check(ac::Validate(tracks[1].track, error), "round-trip fixture 1 validates: " + error);

    std::vector<std::uint8_t> bytes;
    Check(ac::Encode(tracks, bytes, error), "encoding succeeds: " + error);
    Check(bytes.size() == 12 + (4 + expression.id.size() + 1 + 4 + 3 * 52) +
                                (4 + parameter.id.size() + 1 + 4 + 2 * 52) + (4 + 5 + 1 + 4),
          "the encoded size matches the documented layout, got " + std::to_string(bytes.size()));

    std::vector<ac::NamedTrack> decoded;
    Check(ac::Decode(bytes.data(), bytes.size(), decoded, error), "decoding succeeds: " + error);
    Check(SameTracks(decoded, tracks), "the decoded tracks equal the encoded tracks");
    for (const ac::NamedTrack &track : decoded) {
        Check(ac::Validate(track.track, error), "a decoded track validates: " + error);
    }

    std::vector<std::uint8_t> emptyBytes;
    Check(ac::Encode({}, emptyBytes, error), "encoding an empty list succeeds: " + error);
    Check(emptyBytes.size() == 12, "an empty list encodes to the bare header");
    std::vector<ac::NamedTrack> emptyDecoded = {{"stale", {}}};
    Check(ac::Decode(emptyBytes.data(), emptyBytes.size(), emptyDecoded, error),
          "the bare header decodes: " + error);
    Check(emptyDecoded.empty(), "the bare header decodes to no tracks");
}

void TestCodecLayout() {
    std::string error;
    std::vector<ac::NamedTrack> tracks;
    ac::NamedTrack named;
    named.id = "a";
    named.track.keys = {MakeKey(7, 1.0, 0.0, 0.0, 0.5, 0.5, ac::Interpolation::Bezier,
                                ac::Tangent::Automatic)};
    tracks.push_back(named);
    std::vector<std::uint8_t> bytes;
    Check(ac::Encode(tracks, bytes, error), "the layout fixture encodes: " + error);

    const std::uint8_t expectedHeader[22] = {'M', 'M', 'S', 'C', 1, 0, 0, 0, 1, 0, 0,
                                             0,   1,   0,   0,   0, 'a', 0, 1, 0, 0, 0};
    Check(bytes.size() == 22 + 52, "the layout fixture is one header plus one key record");
    Check(std::memcmp(bytes.data(), expectedHeader, sizeof(expectedHeader)) == 0,
          "the header and track prefix are little-endian and in the documented order");
    const std::uint8_t expectedFrame[4] = {7, 0, 0, 0};
    Check(std::memcmp(bytes.data() + KeyFieldOffset("a", 0, 0), expectedFrame, 4) == 0,
          "the frame is a little-endian 32-bit value");
    const std::uint8_t expectedValue[8] = {0, 0, 0, 0, 0, 0, 0xF0, 0x3F};
    Check(std::memcmp(bytes.data() + KeyFieldOffset("a", 0, 1), expectedValue, 8) == 0,
          "the value is a little-endian IEEE-754 double");
    const std::uint8_t expectedWeight[8] = {0, 0, 0, 0, 0, 0, 0xE0, 0x3F};
    Check(std::memcmp(bytes.data() + KeyFieldOffset("a", 0, 4), expectedWeight, 8) == 0,
          "the in-weight is a little-endian IEEE-754 double");
    const std::uint8_t expectedInterpolation[4] = {2, 0, 0, 0};
    Check(std::memcmp(bytes.data() + KeyFieldOffset("a", 0, 6), expectedInterpolation, 4) == 0,
          "the interpolation is stored as its enum value");
    const std::uint8_t expectedTangent[4] = {4, 0, 0, 0};
    Check(std::memcmp(bytes.data() + KeyFieldOffset("a", 0, 7), expectedTangent, 4) == 0,
          "the tangent is stored as its enum value");

    RawTrack raw;
    raw.id = "a";
    raw.keys = {{7, 1.0, 0.0, 0.0, 0.5, 0.5, 2, 4}};
    Check(BuildPayload({raw}) == bytes, "the encoder output matches the specification byte for byte");
}

void TestCodecCorruption() {
    std::string error;
    RawTrack first;
    first.id = "alpha";
    first.keys = {{0, 0.0, 0.0, 0.5, 1.0 / 3.0, 1.0 / 3.0, 2, 0},
                  {10, 4.0, 0.5, 0.0, 1.0 / 3.0, 1.0 / 3.0, 1, 0}};
    RawTrack second;
    second.id = "beta";
    second.discrete = true;
    second.keys = {{3, 1.5, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, 2, 4}};
    const std::vector<std::uint8_t> good = BuildPayload({first, second});

    std::vector<ac::NamedTrack> decoded;
    Check(ac::Decode(good.data(), good.size(), decoded, error), "the corruption fixture decodes: " + error);
    Check(decoded.size() == 2, "the corruption fixture holds two tracks");

    CheckDecodeFails({}, "an empty payload");
    CheckDecodeFails(std::vector<std::uint8_t>{'M', 'D', 'S'}, "a payload below the header size");
    CheckDecodeFails(std::vector<std::uint8_t>(12, 0), "a payload without the magic", "magic");
    CheckDecodeFails(BuildPayload({first}, 2), "a future version", "version");
    CheckDecodeFails(BuildPayload({first}, 1, 1), "a set reserved flag", "flags");

    for (const std::size_t length : {std::size_t(1), std::size_t(11), std::size_t(12), std::size_t(20),
                                     good.size() / 2, good.size() - 1}) {
        std::vector<std::uint8_t> truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length));
        CheckDecodeFails(truncated, "a payload truncated to " + std::to_string(length) + " bytes");
    }

    std::vector<std::uint8_t> trailing = good;
    trailing.push_back(0);
    CheckDecodeFails(trailing, "trailing bytes after the last track", "trailing");

    RawTrack duplicate = second;
    duplicate.id = first.id;
    CheckDecodeFails(BuildPayload({first, duplicate}), "a duplicate track id", "duplicate");

    RawTrack noId = first;
    noId.id.clear();
    CheckDecodeFails(BuildPayload({noId}), "an empty track id", "empty");

    RawTrack badUtf8 = first;
    badUtf8.id = std::string("a\xFF\xFE");
    CheckDecodeFails(BuildPayload({badUtf8}), "a non UTF-8 track id", "UTF-8");

    RawTrack nonFinite = first;
    nonFinite.keys[0].value = std::numeric_limits<double>::quiet_NaN();
    CheckDecodeFails(BuildPayload({nonFinite}), "a NaN value", "finite");

    RawTrack infinite = first;
    infinite.keys[1].inSlope = std::numeric_limits<double>::infinity();
    CheckDecodeFails(BuildPayload({infinite}), "an infinite slope", "finite");

    RawTrack unsorted = first;
    unsorted.keys[1].frame = 0;
    CheckDecodeFails(BuildPayload({unsorted}), "frames that do not increase", "strictly");

    RawTrack negative = first;
    negative.keys[0].frame = -1;
    CheckDecodeFails(BuildPayload({negative}), "a negative frame", "negative");

    RawTrack badInterpolation = first;
    badInterpolation.keys[0].interpolation = 99;
    CheckDecodeFails(BuildPayload({badInterpolation}), "an unknown interpolation value", "interpolation");

    RawTrack badTangent = first;
    badTangent.keys[0].tangent = 99;
    CheckDecodeFails(BuildPayload({badTangent}), "an unknown tangent value", "tangent");

    RawTrack zeroWeight = first;
    zeroWeight.keys[0].outWeight = 0.0;
    CheckDecodeFails(BuildPayload({zeroWeight}), "a zero handle weight", "outWeight");

    RawTrack fullWeight = first;
    fullWeight.keys[1].inWeight = 1.0;
    CheckDecodeFails(BuildPayload({fullWeight}), "a full handle weight", "inWeight");

    RawTrack summingWeights = first;
    summingWeights.keys[0].outWeight = 0.6;
    summingWeights.keys[1].inWeight = 0.6;
    CheckDecodeFails(BuildPayload({summingWeights}), "handle weights that sum above 0.9999", "0.9999");

    std::vector<std::uint8_t> oversizedKeys = good;
    const std::size_t alphaCount = KeyFieldOffset("alpha", 0, 0) - 4;
    oversizedKeys[alphaCount + 0] = 0x10;
    oversizedKeys[alphaCount + 1] = 0x27;
    oversizedKeys[alphaCount + 2] = 0x00;
    oversizedKeys[alphaCount + 3] = 0x00;  // 10000 keys, far more than the payload holds
    CheckDecodeFails(oversizedKeys, "a key count beyond the payload", "remain");

    std::vector<std::uint8_t> beyondLimit = good;
    beyondLimit[alphaCount + 0] = 0x41;
    beyondLimit[alphaCount + 1] = 0x42;
    beyondLimit[alphaCount + 2] = 0x0F;
    beyondLimit[alphaCount + 3] = 0x00;  // 1000000 + 1
    CheckDecodeFails(beyondLimit, "a large count without enough payload", "remain");

    std::vector<std::uint8_t> hugeTrackCount(good);
    hugeTrackCount[8] = 0x64;
    hugeTrackCount[9] = 0x00;
    hugeTrackCount[10] = 0x00;
    hugeTrackCount[11] = 0x00;  // 100 tracks in a payload that cannot hold them
    CheckDecodeFails(hugeTrackCount, "a track count beyond the payload", "remaining");

    ac::NamedTrack sentinel;
    sentinel.id = "sentinel";
    std::vector<ac::NamedTrack> target = {sentinel};
    Check(!ac::Decode(nullptr, 0, target, error), "a null payload is refused");
    Check(target.size() == 1 && target[0].id == "sentinel", "a null payload leaves the output untouched");
}

void TestEncodeFailures() {
    std::string error;
    std::vector<std::uint8_t> bytes = {1, 2, 3};

    std::vector<ac::NamedTrack> duplicate;
    duplicate.push_back(ac::NamedTrack{"same", {}});
    duplicate.push_back(ac::NamedTrack{"same", {}});
    Check(!ac::Encode(duplicate, bytes, error), "duplicate ids are refused when encoding");
    Check(bytes == std::vector<std::uint8_t>({1, 2, 3}), "a refused encode leaves the buffer untouched");

    std::vector<ac::NamedTrack> unnamed;
    unnamed.push_back(ac::NamedTrack{"", {}});
    Check(!ac::Encode(unnamed, bytes, error), "an empty id is refused when encoding");

    ac::NamedTrack broken;
    broken.id = "broken";
    broken.track.keys = {MakeKey(5, 0.0), MakeKey(1, 1.0)};
    Check(!ac::Encode({broken}, bytes, error), "an invalid track is refused when encoding");

    std::vector<ac::NamedTrack> dense;
    dense.push_back(ac::NamedTrack{"large", {}});
    dense[0].track.keys.resize(1000001);
    for (std::size_t i = 0; i < dense[0].track.keys.size(); ++i)
        dense[0].track.keys[i].frame = static_cast<int>(i);
    Check(ac::Encode(dense, bytes, error), "more than a million keys can be encoded");

}

// Per-side Bezier editing: the operations behind Alt-click on keys and handles,
// the ease options and handle dragging in the curve editor.
void TestSideEditing() {
    std::string error;
    ac::Track track;
    track.keys = {MakeKey(0, 0.0, 0, 0.5, 1.0 / 3, 1.0 / 3, ac::Interpolation::Linear),
                  MakeKey(10, 5.0, 0.5, 1.5, 1.0 / 3, 1.0 / 3, ac::Interpolation::Linear),
                  MakeKey(20, 20.0, 1.5, 0, 1.0 / 3, 1.0 / 3, ac::Interpolation::Linear)};
    Check(ac::KeyIsLinear(track, 1), "a key between linear intervals is linear");

    ac::MakeKeyBezier(track, 1);
    Check(ac::Validate(track, error), "MakeKeyBezier keeps the track valid: " + error);
    Check(track.keys[1].interpolation == ac::Interpolation::Bezier &&
              track.keys[2].interpolation == ac::Interpolation::Bezier,
          "both sides of the key become Bezier intervals");
    Check(!ac::KeyIsLinear(track, 1), "the key now has curved handles");
    Check(ac::SideIsStraight(track, 0, true) && ac::SideIsStraight(track, 2, false),
          "the neighbours' facing handles stay on the chord");
    CheckNear(ac::Evaluate(track, 10.0).value, 5.0, 1e-12, "the key still evaluates to its value");

    ac::StraightenSide(track, 1, false);
    Check(ac::SideIsStraight(track, 1, false) && !ac::SideIsStraight(track, 1, true),
          "straightening the in handle leaves the out handle curved");
    Check(track.keys[1].interpolation == ac::Interpolation::Linear,
          "with both handles on the chord the in interval becomes linear");
    ac::StraightenSide(track, 1, true);
    Check(ac::KeyIsLinear(track, 1), "straightening the other handle makes the key linear again");
    Check(track.keys[2].interpolation == ac::Interpolation::Linear, "and its out interval linear");
    Check(ac::Validate(track, error), "straightened track validates: " + error);

    ac::ApplyEase(track, 1, true);
    Check(!ac::SideIsStraight(track, 1, false) && ac::SideIsStraight(track, 1, true),
          "ease in curves the in side only");
    CheckNear(track.keys[1].inSlope, 0.0, 0.0, "ease in arrives flat");
    ac::ApplyEase(track, 1, false);
    Check(ac::SideIsStraight(track, 1, false) && !ac::SideIsStraight(track, 1, true),
          "ease out curves the out side only");
    Check(ac::Validate(track, error), "eased track validates: " + error);

    ac::MakeKeyLinear(track, 1);
    Check(ac::KeyIsLinear(track, 1), "MakeKeyLinear straightens every side");

    // Collinear neighbours would give the chord slope; the key gets a flat,
    // visibly curved two-sided handle instead.
    ac::Track line;
    line.keys = {MakeKey(0, 0.0, 0, 0, 1.0 / 3, 1.0 / 3, ac::Interpolation::Linear),
                 MakeKey(10, 5.0, 0, 0, 1.0 / 3, 1.0 / 3, ac::Interpolation::Linear),
                 MakeKey(20, 10.0, 0, 0, 1.0 / 3, 1.0 / 3, ac::Interpolation::Linear)};
    ac::MakeKeyBezier(line, 1);
    Check(!ac::KeyIsLinear(line, 1), "a key on a straight line still becomes curved");

    // Handle dragging.
    ac::MakeKeyBezier(track, 1);
    Check(ac::MoveHandle(track, 1, true, 15.0, 10.0), "the out handle moves");
    CheckNear(track.keys[1].outWeight, 0.5, 1e-12, "handle weight follows the frame");
    CheckNear(track.keys[1].outSlope, 1.0, 1e-12, "handle slope follows the value");
    Check(track.keys[1].tangent == ac::Tangent::Free, "a moved handle frees the tangent");
    Check(ac::MoveHandle(track, 1, true, 100.0, 10.0), "an overlong drag is clamped");
    Check(track.keys[1].outWeight + track.keys[2].inWeight <= 0.9999 + 1e-12,
          "the dragged handle leaves room for the facing one");
    track.keys[1].tangent = ac::Tangent::Aligned;
    Check(ac::MoveHandle(track, 1, false, 5.0, 3.0), "the in handle moves");
    CheckNear(track.keys[1].outSlope, track.keys[1].inSlope, 0.0, "an aligned key keeps both slopes equal");
    Check(ac::Validate(track, error), "dragged track validates: " + error);
    ac::MakeKeyLinear(track, 1);
    Check(!ac::MoveHandle(track, 0, false, -5.0, 0.0), "the first key has no in handle");
}

}  // namespace

int main() {
    g_failures += RunSplitChecks();
    TestValidation();
    TestEvaluation();
    TestStepAndLinear();
    TestTangents();
    TestAutomatic();
    TestSideEditing();
    TestMmdControls();
    TestBake();
    TestCodecRoundTrip();
    TestCodecLayout();
    TestCodecCorruption();
    TestEncodeFailures();
    if (g_failures == 0) {
        std::printf("animation_curve_tests: all checks passed\n");
        return 0;
    }
    std::printf("animation_curve_tests: %d check(s) failed\n", g_failures);
    return 1;
}
