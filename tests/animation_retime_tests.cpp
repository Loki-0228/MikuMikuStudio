// MAINTENANCE: pure-CPU coverage for include/mikumikustudio/animation_retime.hpp
// and src/animation/animation_retime.cpp.  No Win32, no renderer, no GPU.
//
// The point of the suite is to pin the honest part of the contract: exactness is
// only claimed for an integral mapping, every other case has to report a sampled
// error and leave the output untouched on failure.  The counterexample cases
// below are the ones the design cannot represent exactly, and they assert that
// behaviour instead of claiming losslessness.
#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/animation_retime.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ac = mikumikustudio::animation_curve;
namespace rt = mikumikustudio::animation_retime;

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
    std::printf("FAIL %s: expected %.17g, got %.17g (tolerance %.3g)\n", text.c_str(), expected, actual, tolerance);
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

ac::Track MakeTrack(std::vector<ac::Key> keys, bool discrete = false) {
    ac::Track track;
    track.keys = std::move(keys);
    track.discrete = discrete;
    return track;
}

bool SameTrack(const ac::Track &left, const ac::Track &right) {
    if (left.discrete != right.discrete || left.keys.size() != right.keys.size()) return false;
    for (std::size_t index = 0; index < left.keys.size(); ++index) {
        const ac::Key &a = left.keys[index];
        const ac::Key &b = right.keys[index];
        if (a.frame != b.frame || a.value != b.value || a.inSlope != b.inSlope || a.outSlope != b.outSlope ||
            a.inWeight != b.inWeight || a.outWeight != b.outWeight || a.interpolation != b.interpolation ||
            a.tangent != b.tangent) {
            return false;
        }
    }
    return true;
}

// Largest value the track reaches on a dense frame sweep.
double SweepMaximum(const ac::Track &track, double first, double last, int steps = 4000) {
    double high = ac::Evaluate(track, first).value;
    for (int step = 0; step <= steps; ++step) {
        const double frame = first + (last - first) * step / steps;
        high = std::max(high, ac::Evaluate(track, frame).value);
    }
    return high;
}

// Value controls y1/y2 of one interval, recomputed from the stored fields.
void ValueControls(const ac::Track &track, std::size_t endKey, double &y1, double &y2) {
    const ac::Key &start = track.keys[endKey - 1];
    const ac::Key &end = track.keys[endKey];
    const double span = static_cast<double>(end.frame) - static_cast<double>(start.frame);
    y1 = start.value + start.outSlope * start.outWeight * span;
    y2 = end.value - end.inSlope * end.inWeight * span;
}

const rt::Options kOptions{};
const rt::Rates kDouble{30.0, 60.0};   // ratio 2
const rt::Rates kHalf{60.0, 30.0};     // ratio 1/2

// ---------------------------------------------------------------------------
// 1. Integral mapping: exact time-domain equivalence is claimed here and only
//    here, and the value controls must be bit-identical.
// ---------------------------------------------------------------------------
void IntegralMappingIsExact() {
    ac::Track input = MakeTrack({MakeKey(0, 0.0, 0.0, 2.0), MakeKey(5, 1.0, -1.0, 0.5), MakeKey(10, 0.0, 0.0, 0.0)});
    ac::Track output;
    rt::Report report;
    std::string error;
    Check(rt::Retime(input, kDouble, kOptions, output, report, error), "integral retime succeeds: " + error);
    Check(report.integralMapping, "an integer ratio with divisible frames is integral");
    Check(report.shapePreserved, "integral mapping preserves the time function");
    Check(report.curveShapePreserved, "integral mapping keeps the handles");
    Check(report.mergeGroups == 0 && report.mergedKeys == 0 && report.clippedSegments == 0,
          "integral mapping neither merges nor clips");
    Check(report.maxSampledValueError == 0.0, "integral mapping has no sampled error");
    Check(output.keys.size() == 3 && output.keys[0].frame == 0 && output.keys[1].frame == 10 &&
              output.keys[2].frame == 20,
          "frames scale by the ratio");
    CheckNear(output.keys[1].value, 1.0, 0.0, "key values are untouched");
    CheckNear(output.keys[1].outWeight, input.keys[1].outWeight, 0.0, "normalised weights are untouched");
    CheckNear(output.keys[1].inWeight, input.keys[1].inWeight, 0.0, "both normalised weights are untouched");
    CheckNear(output.keys[1].outSlope, 0.5 * 0.5, 1e-15, "the outgoing slope is scaled by oldSpan/newSpan");
    CheckNear(output.keys[1].inSlope, -1.0 * 0.5, 1e-15, "the incoming slope is scaled by oldSpan/newSpan");
    for (int step = 0; step <= 80; ++step) {
        const double frame = step * 0.25;
        CheckNear(ac::Evaluate(output, frame).value, ac::Evaluate(input, frame / 2.0).value, 1e-12,
                  "the retimed curve equals the original in the time domain");
    }
    // The reverse direction is integral only when every frame is divisible.
    ac::Track even = MakeTrack({MakeKey(0, 0.0, 0.0, 2.0), MakeKey(4, 1.0, -1.0, 0.5), MakeKey(8, 0.0, 0.0, 0.0)});
    ac::Track back;
    rt::Report backReport;
    Check(rt::Retime(even, kHalf, kOptions, back, backReport, error), "halving retime succeeds: " + error);
    Check(backReport.integralMapping, "ratio 1/2 with even frames is integral");
    Check(back.keys[2].frame == 4 && backReport.shapePreserved, "the halved track is exact as well");
    CheckNear(ac::Evaluate(back, 2.0).value, ac::Evaluate(even, 4.0).value, 1e-12,
              "the halved track equals the original in the time domain");
}

// ---------------------------------------------------------------------------
// 2. Rounded interior keys: an output interval can cover more than one original
//    segment, so exactness must NOT be claimed; the handles are still kept.
// ---------------------------------------------------------------------------
void RoundedInteriorKeyIsNotExact() {
    ac::Track input = MakeTrack({MakeKey(0, 0.0, 0.0, 1.0), MakeKey(1, 1.0, 0.0, 1.0),
                                 MakeKey(2, 0.5, 0.0, 1.0), MakeKey(3, 1.5, 0.0, 0.0)});
    const rt::Rates rates{30.0, 45.0};   // ratio 1.5: 0, 1.5, 3, 4.5 -> 0, 2, 3, 5
    ac::Track output;
    rt::Report report;
    std::string error;
    Check(rt::Retime(input, rates, kOptions, output, report, error), "the 1.5 ratio retime succeeds: " + error);
    Check(output.keys[1].frame == 2 && output.keys[2].frame == 3 && output.keys[3].frame == 5,
          "half frames round up");
    Check(!report.integralMapping && !report.shapePreserved, "a rounded interior key is not declared exact");
    Check(report.roundedKeys == 2, "the two half frames are counted as rounded");
    Check(report.mergeGroups == 0 && report.clippedSegments == 0, "no collision here, so nothing is merged");
    Check(report.curveShapePreserved, "without a collision the value controls are kept");
    Check(!report.shapePreserved, "a rounded frame is never declared exact");
    // The sampling still runs here: the handles are kept but the timing inside
    // the intervals is not the exact ratio, so an error has to be produced.
    Check(report.maxSampledValueError > 0.0, "a non-integral mapping is measured");
    // Interval [3,5] covers old frames [2, 3.33]: it reaches past the original key
    // at frame 3, which is exactly what one cubic cannot represent.
    const double oldEnd = 5.0 / 1.5;
    Check(oldEnd > 3.0, "the counterexample really crosses an original key frame");
    for (std::size_t index = 0; index + 1 < input.keys.size(); ++index) {
        double inY1 = 0, inY2 = 0, outY1 = 0, outY2 = 0;
        ValueControls(input, index + 1, inY1, inY2);
        ValueControls(output, index + 1, outY1, outY2);
        CheckNear(outY1, inY1, 1e-12, "the outgoing value control is preserved without a collision");
        CheckNear(outY2, inY2, 1e-12, "the incoming value control is preserved without a collision");
    }
    Check(report.maxSampledValueError > 0.0, "the sampled error of the rounded case is reported");
}

// ---------------------------------------------------------------------------
// 3. Bezier spike: kept exactly without a collision, and measured by the
//    analytic extrema when the spike is inside a collapsed group.
// ---------------------------------------------------------------------------
void BezierSpike() {
    // On [1,2] the controls give Y(s) = 4 s (1 - s): an interior maximum of 1.
    ac::Track spike = MakeTrack({MakeKey(0, 0.0, 0.0, 0.0), MakeKey(1, 0.0, 0.0, 4.0), MakeKey(2, 0.0, -4.0, 0.0)});
    CheckNear(SweepMaximum(spike, 1.0, 2.0), 1.0, 1e-6, "the crafted spike really peaks at 1");

    ac::Track kept;
    rt::Report keptReport;
    std::string error;
    Check(rt::Retime(spike, kDouble, kOptions, kept, keptReport, error), "the spike retime with ratio 2 succeeds");
    Check(keptReport.curveShapePreserved, "a spike without a collision keeps its handles");
    double inY1 = 0, inY2 = 0, outY1 = 0, outY2 = 0;
    ValueControls(spike, 2, inY1, inY2);
    ValueControls(kept, 2, outY1, outY2);
    CheckNear(outY1, inY1, 1e-12, "the spike control is preserved exactly");
    CheckNear(outY2, inY2, 1e-12, "the spike control is preserved exactly on the other side");
    CheckNear(SweepMaximum(kept, 2.0, 4.0), 1.0, 1e-6, "the spike height survives the retime");
    Check(keptReport.maxCollapsedValueRange == 0.0, "nothing was collapsed");

    // Same curve halved: keys 1 and 2 collapse, so the spike lives inside a group
    // and only its span can be reported.
    ac::Track collapsed;
    rt::Report collapsedReport;
    Check(rt::Retime(spike, kHalf, kOptions, collapsed, collapsedReport, error),
          "the spike retime with ratio 1/2 succeeds");
    Check(collapsedReport.mergeGroups == 1 && collapsedReport.outputKeys == 2,
          "keys 1 and 2 collapse into one");
    CheckNear(collapsedReport.maxCollapsedValueRange, 1.0, 1e-9,
              "the collapsed span comes from the analytic extrema, not from a grid");
    Check(!collapsedReport.shapePreserved, "a collapsed group is never called exact");
}

// ---------------------------------------------------------------------------
// 4. A collision with a surviving key on its left: the outer segment clips at a
//    position strictly inside the original segment, and the joint keeps the
//    tangent of the clipped curve because the neighbouring control is shifted by
//    the same delta as the endpoint value.
// ---------------------------------------------------------------------------
void CollisionClipsOuterSegment() {
    // Keys at frames 3, 4, 5, 6 with ratio 0.7 map to 2, 3, 4, 4 : the last two
    // collapse, and the surviving segment between frames 3 and 4 covers the
    // original segment [4, 5] from 3/0.7 = 4.2857 onwards.
    ac::Track input = MakeTrack({MakeKey(3, 0.0, 0.0, 1.0), MakeKey(4, 1.0, 0.5, 0.5),
                                 MakeKey(5, 2.0, 0.5, 0.5), MakeKey(6, 3.0, 0.5, 0.0)});
    const rt::Rates rates{30.0, 21.0};   // ratio 7/10
    ac::Track output;
    rt::Report report;
    std::string error;
    Check(rt::Retime(input, rates, kOptions, output, report, error), "the collision retime succeeds: " + error);
    Check(output.keys.size() == 3, "four keys become three");
    Check(output.keys[0].frame == 2 && output.keys[1].frame == 3 && output.keys[2].frame == 4,
          "the surviving frames are the rounded ones");
    Check(report.mergeGroups == 1 && report.mergedKeys == 1 && report.outputKeys == 3,
          "one group of two keys is reported");
    Check(report.clippedSegments == 1, "only the segment next to the group is clipped");
    Check(report.degenerateClamps == 0, "that clip is not the empty-clamp fallback");
    Check(report.maxCollapsedValueRange > 0.0, "the collapsed span is reported for the preview");
    // A continuous group takes the sampled original at newFrame / ratio.
    const double sample = ac::Evaluate(input, 4.0 / 0.7).value;
    CheckNear(output.keys[2].value, sample, 0.0, "a continuous group takes Evaluate(input, frame / ratio)");
    Check(sample > input.keys[2].value && sample < input.keys[3].value,
          "the sampled value lies inside the original interval, not on a key");
    Check(sample != input.keys[3].value, "a continuous group is not replaced by the group's last state");
    // The clip runs from 3/0.7 to the end of the original segment, so its span is
    // 5/7 of a frame; the recovered slope is the clipped slope rescaled from the
    // clipped old span to the new span.  Without the same-delta shift of the
    // neighbouring value control the merged value would rotate that tangent.
    const double clipLow = 3.0 / 0.7, clipHigh = 5.0, newSpan = 1.0;
    CheckNear(clipLow, 4.285714285714286, 1e-12, "the clip really starts inside the original segment");
    const double factor = (clipHigh - clipLow) / newSpan;
    CheckNear(output.keys[2].inSlope, input.keys[2].inSlope * factor, 1e-9,
              "the joint keeps the clipped tangent, rescaled to the new span");
    Check(report.maxSampledValueError > 0.0, "the deviation of the collapsed window is sampled and reported");
}

// ---------------------------------------------------------------------------
// 5. Everything into one frame, plus the frame-zero rule.
// ---------------------------------------------------------------------------
void AllKeysCollapseAndFrameZero() {
    ac::Track input = MakeTrack({MakeKey(0, 0.25, 0.0, 0.5), MakeKey(1, 1.0, 0.5, 0.5), MakeKey(2, 0.5, 0.5, 0.0)});
    const rt::Rates rates{30.0, 1.0};   // ratio 1/30: everything lands on frame 0
    ac::Track output;
    rt::Report report;
    std::string error;
    Check(rt::Retime(input, rates, kOptions, output, report, error), "the all-collapse retime succeeds: " + error);
    Check(output.keys.size() == 1 && output.keys[0].frame == 0, "every key collapses into frame 0");
    Check(report.mergedKeys == 2 && report.mergeGroups == 1, "the counters describe the collapse");
    CheckNear(output.keys[0].value, input.keys[0].value, 0.0,
              "a group that holds the original frame-zero key keeps its value");
    Check(output.discrete == false, "the discrete flag is copied");
    Check(report.maxSampledValueError > 0.0,
          "a track that collapses into a single key still reports a sampled error");

    // A track whose first key is not frame zero keeps its rounded position.
    ac::Track late = MakeTrack({MakeKey(6, 1.0, 0.0, 0.0), MakeKey(8, 2.0, 0.0, 0.0)});
    ac::Track lateOutput;
    rt::Report lateReport;
    Check(rt::Retime(late, kDouble, kOptions, lateOutput, lateReport, error), "the late-first-key retime succeeds");
    Check(lateOutput.keys.front().frame == 12, "no frame-zero key is invented");
    Check(lateReport.frozenTangents == 0, "nothing was frozen on the late example");
}

// ---------------------------------------------------------------------------
// 6. The frame-zero rule and the Step rule apply to the folded group only.
// ---------------------------------------------------------------------------
void MergedGroupRules() {
    // A first key that is not frame zero must not be read as the baseline: the
    // frames 14 and 15 at ratio 0.07 land on frame 1 and the value is the curve
    // sampled at 1 / 0.07.
    ac::Track late = MakeTrack({MakeKey(14, 0.0, 0.0, 1.0), MakeKey(15, 1.0, 0.5, 0.0)});
    const rt::Rates slow{100.0, 7.0};   // ratio 0.07
    ac::Track output;
    rt::Report report;
    std::string error;
    Check(rt::Retime(late, slow, kOptions, output, report, error), "the 0.07 retime succeeds: " + error);
    Check(output.keys.size() == 1 && output.keys[0].frame == 1, "frames 14 and 15 land on frame 1");
    Check(report.mergeGroups == 1 && report.mergedKeys == 1, "the two keys form one merged group");
    const double sampled = ac::Evaluate(late, 1.0 / 0.07).value;
    CheckNear(output.keys[0].value, sampled, 0.0, "a merged first group takes the sampled value");
    Check(output.keys[0].value != late.keys[0].value,
          "a first key that only rounds to frame 1 is not the frame-zero baseline");
    CheckNear(1.0 / 0.07, 14.285714285714286, 1e-9, "the sample really sits inside the folded window");

    // The interpolation of the group's first key belongs to the OUTER segment,
    // so a mixed track must not be read as "the whole group is Step": the folded
    // interval decides.
    ac::Track mixed = MakeTrack({MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                 MakeKey(1, 0.5, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                 MakeKey(2, 1.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                 MakeKey(3, 2.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier)});
    const rt::Rates quarter{30.0, 12.0};   // ratio 0.4: 0, 0, 1, 1
    ac::Track mixedOutput;
    rt::Report mixedReport;
    Check(rt::Retime(mixed, quarter, kOptions, mixedOutput, mixedReport, error),
          "the mixed retime succeeds: " + error);
    Check(mixedOutput.keys.size() == 2 && mixedOutput.keys[0].frame == 0 && mixedOutput.keys[1].frame == 1,
          "the mixed track becomes two groups on frames 0 and 1");
    const double folded = ac::Evaluate(mixed, 1.0 / 0.4).value;
    CheckNear(mixedOutput.keys[1].value, folded, 0.0, "a mixed group samples the folded Bezier interval");
    Check(mixedOutput.keys[1].value != mixed.keys[3].value,
          "the last state is not used when the folded interval is a Bezier interval");
    Check(folded > 1.0 && folded < 2.0, "the sample sits inside the folded interval");
}

// ---------------------------------------------------------------------------
// 7. Step and discrete groups keep the last state, never an average.
// ---------------------------------------------------------------------------
void StepKeepsLastState() {
    ac::Track step = MakeTrack({MakeKey(0, 0.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                MakeKey(1, 1.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                MakeKey(2, 0.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step)});
    ac::Track output;
    rt::Report report;
    std::string error;
    Check(rt::Retime(step, kHalf, kOptions, output, report, error), "the Step retime succeeds: " + error);
    Check(output.keys.size() == 2 && output.keys[1].frame == 1, "keys 1 and 2 collapse");
    CheckNear(output.keys[1].value, 0.0, 0.0, "a Step group keeps the LAST state (0), not the mean (0.5)");
    Check(output.keys[1].interpolation == ac::Interpolation::Step, "the Step interval stays a Step interval");
    Check(output.keys[0].interpolation == ac::Interpolation::Step, "every Step key stays Step");

    ac::Track discrete = MakeTrack({MakeKey(0, 1.0), MakeKey(1, 0.0), MakeKey(2, 1.0)}, true);
    ac::Track discreteOutput;
    rt::Report discreteReport;
    Check(rt::Retime(discrete, kHalf, kOptions, discreteOutput, discreteReport, error),
          "the discrete retime succeeds");
    Check(discreteOutput.discrete, "the discrete flag survives");
    CheckNear(discreteOutput.keys[1].value, 1.0, 0.0, "a discrete group keeps its last state too");
    // The unused incoming side of the first key is copied verbatim; every real
    // interval of a discrete track is a Step interval.
    for (std::size_t index = 1; index < discreteOutput.keys.size(); ++index) {
        Check(discreteOutput.keys[index].interpolation == ac::Interpolation::Step,
              "a discrete track never gains a Bezier interval");
    }
}

// ---------------------------------------------------------------------------
// 8. Failure leaves the output untouched, and there is no silent degradation.
// ---------------------------------------------------------------------------
void FailureLeavesOutputUntouched() {
    ac::Track input = MakeTrack({MakeKey(0, 0.0, 0.0, 1.0), MakeKey(5, 5.0, 0.0, 0.0)});
    ac::Track guard;
    rt::Report guardReport;
    std::string error;
    Check(rt::Retime(input, kDouble, kOptions, guard, guardReport, error), "the guard retime succeeds");
    const ac::Track expected = guard;

    // A frame that would land past the accepted maximum fails the whole call.
    rt::Options tight;
    tight.maximumFrame = 8;
    ac::Track output = expected;
    rt::Report report;
    Check(!rt::Retime(input, kDouble, tight, output, report, error), "a frame past the maximum fails");
    Check(!error.empty(), "the failure explains itself");
    Check(SameTrack(output, expected), "a failed call leaves the output untouched");

    // Bad rates fail as well.
    const rt::Rates badRates[3] = {{0.0, 30.0}, {30.0, -1.0}, {30.0, std::nan("")}};
    for (const rt::Rates &rates : badRates) {
        output = expected;
        Check(!rt::Retime(input, rates, kOptions, output, report, error), "an invalid rate fails");
        Check(SameTrack(output, expected), "an invalid rate leaves the output untouched");
    }

    // An invalid input fails before anything is computed.
    ac::Track broken = MakeTrack({MakeKey(3, 0.0), MakeKey(3, 1.0)});
    output = expected;
    Check(!rt::Retime(broken, kDouble, kOptions, output, report, error), "a broken input fails");
    Check(SameTrack(output, expected), "a broken input leaves the output untouched");

    // No key of a failed call is silently linearised: the guard track is intact.
    Check(expected.keys[1].interpolation == ac::Interpolation::Bezier,
          "the failure path does not degrade the intervals");
}

// ---------------------------------------------------------------------------
// 9. MapFrame and GroupFrames.
// ---------------------------------------------------------------------------
void FrameMappingAndGrouping() {
    std::string error;
    std::int32_t frame = -1;
    Check(rt::MapFrame(5, rt::Rates{30.0, 24.0}, 10000000, frame, error) && frame == 4,
          "30 -> 24 maps frame 5 to frame 4 exactly");
    Check(rt::MapFrame(3, rt::Rates{24.0, 30.0}, 10000000, frame, error) && frame == 4,
          "24 -> 30 rounds 3.75 up to 4");
    Check(rt::MapFrame(500, rt::Rates{30.0, 29.97}, 10000000, frame, error) && frame == 500,
          "a fractional rate such as 29.97 works without a special case");
    Check(rt::MapFrame(100, rt::Rates{30.0, 29.97}, 10000000, frame, error) && frame == 100,
          "a fractional rate keeps a sub-frame offset");
    Check(!rt::MapFrame(100000, rt::Rates{30.0, 60.0}, 100000, frame, error), "the maximum frame is enforced");
    Check(!rt::MapFrame(-1, rt::Rates{30.0, 60.0}, 100000, frame, error), "a negative frame is refused");
    Check(!rt::MapFrame(1, rt::Rates{30.0, 0.0}, 100000, frame, error), "a zero rate is refused");

    // Robustness: nothing may overflow or format an out-of-range integer.
    Check(!rt::MapFrame(std::numeric_limits<std::int64_t>::max(), rt::Rates{30.0, 60.0}, 1000, frame, error),
          "an enormous frame is refused instead of overflowing");
    Check(!error.empty(), "the enormous frame rejection explains itself");
    Check(!rt::MapFrame(1, rt::Rates{30.0, 1e300}, 10000000, frame, error),
          "an enormous rate is refused in long double");
    Check(!error.empty(), "the enormous rate rejection explains itself");
    // A finite position far past int32: the old narrowing cast was undefined, so
    // this checks that the range test comes first.
    Check(!rt::MapFrame(1, rt::Rates{1.0, 1e30}, 10000000, frame, error),
          "a finite position far past the maximum is refused before any cast");
    Check(!error.empty(), "the far position rejection explains itself");
    Check(!rt::MapFrame(1, rt::Rates{30.0, 60.0}, -1, frame, error), "a negative maximum frame is refused");
    Check(rt::MapFrame(1, rt::Rates{2.0, 3.0}, 1000, frame, error) && frame == 2, "a half frame rounds up");
    Check(rt::MapFrame(1, rt::Rates{2.0, 1.0}, 1000, frame, error) && frame == 1, "0.5 rounds up as well");
    Check(rt::MapFrame(2, rt::Rates{2.0, 1.0}, 1000, frame, error) && frame == 1, "a whole frame stays exact");

    std::vector<rt::Group> groups;
    rt::Report report;
    const std::vector<std::int32_t> frames{0, 1, 2, 3};
    Check(rt::GroupFrames(frames, kHalf, kOptions, groups, report, error), "grouping succeeds");
    Check(groups.size() == 3 && groups[0].last == 0 && groups[1].first == 1 && groups[1].last == 2 &&
              groups[2].frame == 2,
          "the runs of equal frames are grouped");
    Check(report.inputKeys == 4 && report.outputKeys == 3 && report.mergedKeys == 1 && report.mergeGroups == 1,
          "the counters describe one merge");
    Check(report.roundedKeys == 2 && !report.integralMapping, "the two half frames are counted as rounded");

    rt::Report unchanged = report;
    Check(!rt::GroupFrames(std::vector<std::int32_t>{2, 2}, kHalf, kOptions, groups, report, error),
          "a frame list that does not increase is refused");
    Check(report.inputKeys == unchanged.inputKeys && report.roundedKeys == unchanged.roundedKeys,
          "a refused grouping leaves the report alone");
}

// ---------------------------------------------------------------------------
// 10. Identity rates copy the track field for field, aliasing is safe, and the
//     Automatic protection counts what it freezes.
// ---------------------------------------------------------------------------
void IdentityAliasingAndAutomatic() {
    ac::Track input = MakeTrack({MakeKey(0, 0.0, 0.0, 1.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                                         ac::Tangent::Automatic),
                                 MakeKey(5, 1.0, 0.5, 0.5), MakeKey(10, 0.0, 0.0, 0.0)});
    ac::Track output = input;
    rt::Report report;
    std::string error;
    Check(rt::Retime(input, rt::Rates{30.0, 30.0}, kOptions, output, report, error), "identity rates succeed");
    Check(SameTrack(output, input), "identity rates copy the track field for field");
    Check(report.shapePreserved && report.integralMapping && report.frozenTangents == 0,
          "identity rates report exactness and freeze nothing");
    Check(output.keys[0].tangent == ac::Tangent::Automatic, "identity rates keep Automatic");

    // Equal fractional rates are an identity too, and must not freeze anything.
    ac::Track fractional = MakeTrack(
        {MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier, ac::Tangent::Automatic),
         MakeKey(5, 1.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier, ac::Tangent::Automatic)});
    ac::Track fractionalOutput;
    rt::Report fractionalReport;
    Check(rt::Retime(fractional, rt::Rates{29.97, 29.97}, kOptions, fractionalOutput, fractionalReport, error),
          "equal fractional rates succeed");
    Check(SameTrack(fractionalOutput, fractional), "equal fractional rates copy the track field for field");
    Check(fractionalReport.integralMapping && fractionalReport.frozenTangents == 0,
          "equal fractional rates report exactness and freeze nothing");

    // Aliasing: input and output are the same object.
    ac::Track shared = input;
    ac::Track reference;
    rt::Report referenceReport;
    Check(rt::Retime(input, kDouble, kOptions, reference, referenceReport, error), "the reference retime succeeds");
    rt::Report sharedReport;
    Check(rt::Retime(shared, kDouble, kOptions, shared, sharedReport, error), "the aliased retime succeeds");
    Check(SameTrack(shared, reference), "an aliased call produces the same track");
    Check(sharedReport.outputKeys == referenceReport.outputKeys, "an aliased call reports the same counters");

    // An Automatic key whose new neighbours would compute another slope becomes
    // Free, so a later UpdateAutomatic cannot undo the retime.
    ac::Track automatic = MakeTrack({MakeKey(0, 0.0, 0.0, 1.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                                             ac::Tangent::Automatic),
                                     MakeKey(3, 1.0, 1.0, 1.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                                             ac::Tangent::Automatic),
                                     MakeKey(6, 0.0, -1.0, -1.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                                             ac::Tangent::Automatic)});
    ac::Track frozen;
    rt::Report frozenReport;
    Check(rt::Retime(automatic, kDouble, kOptions, frozen, frozenReport, error), "the Automatic retime succeeds");
    Check(frozenReport.frozenTangents > 0, "an Automatic key that would move is counted");
    std::size_t freeCount = 0;
    for (const ac::Key &key : frozen.keys) {
        if (key.tangent == ac::Tangent::Free) ++freeCount;
    }
    Check(freeCount == frozenReport.frozenTangents, "every counted key really became Free");

    // With slopes the core itself computed, an integral ratio keeps Automatic:
    // the automatic formula scales exactly like the retime does.  A rounding
    // ratio makes the two spans unequal and the key has to be frozen instead.
    ac::Track consistent = MakeTrack({MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                                              ac::Tangent::Automatic),
                                      MakeKey(3, 1.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                                              ac::Tangent::Automatic),
                                      MakeKey(6, 2.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Bezier,
                                              ac::Tangent::Automatic)});
    ac::UpdateAutomatic(consistent);
    ac::Track exact;
    rt::Report exactReport;
    Check(rt::Retime(consistent, kDouble, kOptions, exact, exactReport, error), "the automatic retime succeeds");
    Check(exactReport.integralMapping && exactReport.frozenTangents == 0,
          "an integral ratio leaves a consistent Automatic key alone");
    Check(exact.keys[1].tangent == ac::Tangent::Automatic, "the mode really stayed Automatic");
    const rt::Rates rounded{30.0, 45.0};
    ac::Track moved;
    rt::Report movedReport;
    Check(rt::Retime(consistent, rounded, kOptions, moved, movedReport, error), "the rounded retime succeeds");
    Check(movedReport.frozenTangents > 0, "a rounding ratio freezes the key whose neighbours moved");
}

// ---------------------------------------------------------------------------
// 11. A long track completes.  Every sample is a binary search inside
//     animation_curve::Evaluate, so a run is O(keys * log keys) plus the fixed
//     sample count: this case proves completion and validity on a large input,
//     not linear cost.
// ---------------------------------------------------------------------------
void LongTrackCompletes() {
    std::vector<ac::Key> keys;
    keys.reserve(50000);
    for (int index = 0; index < 50000; ++index) {
        keys.push_back(MakeKey(index, std::sin(index * 0.01), 0.5, 0.5));
    }
    ac::Track input = MakeTrack(std::move(keys));
    ac::Track output;
    rt::Report report;
    std::string error;
    Check(rt::Retime(input, kDouble, kOptions, output, report, error), "a 50000 key track retimes: " + error);
    Check(output.keys.size() == input.keys.size(), "no key is lost at this ratio");
    Check(ac::Validate(output, error), "the long output validates: " + error);
    Check(report.clippedSegments == 0, "no collision at ratio 2 means no clipping");
}

// ---------------------------------------------------------------------------
// 12. Timing on 100000 keys.  The exact case skips the sampling pass; the
//     fractional case pays for it.  These are CPU wall-clock numbers measured
//     on the machine that ran the test: they are not a frame rate and not a
//     budget, they only show the difference the skip makes.
// ---------------------------------------------------------------------------
void HundredThousandKeyTiming() {
    std::vector<ac::Key> keys;
    keys.reserve(100000);
    for (int index = 0; index < 100000; ++index) {
        keys.push_back(MakeKey(index * 3, std::sin(index * 0.001), 0.25, 0.25));
    }
    ac::Track input = MakeTrack(std::move(keys));
    const auto measure = [](const ac::Track &source, const rt::Rates &rates, bool &ok) {
        ac::Track output;
        rt::Report report;
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        ok = rt::Retime(source, rates, kOptions, output, report, error);
        const auto stop = std::chrono::steady_clock::now();
        const double milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
        std::printf("retime timing: %zu keys, %g -> %g fps: %.1f ms (sampled error %.6g)\n", source.keys.size(),
                    rates.originalFPS, rates.currentFPS, milliseconds, report.maxSampledValueError);
        return std::make_pair(milliseconds, report);
    };
    bool integralOk = false;
    const auto integral = measure(input, rt::Rates{30.0, 60.0}, integralOk);
    Check(integralOk, "the integral 100000 key retime succeeds");
    Check(integral.second.integralMapping && integral.second.shapePreserved,
          "the integral case reports exact time-domain equivalence");
    Check(integral.second.maxSampledValueError == 0.0,
          "the integral case is exact by construction, so it is not sampled at all");

    bool fractionalOk = false;
    const auto fractional = measure(input, rt::Rates{24.0, 30.0}, fractionalOk);
    Check(fractionalOk, "the fractional 100000 key retime succeeds");
    Check(!fractional.second.integralMapping, "the fractional case really rounds frames");
    Check(fractional.second.maxSampledValueError > 0.0, "the fractional case is measured, not skipped");
    std::printf("retime timing: the measured case took %.2fx the exact case\n",
                integral.first > 0.0 ? fractional.first / integral.first : 0.0);
    Check(integral.first > 0.0 && fractional.first > 0.0, "both timings are positive");
}

}  // namespace

int main() {
    IntegralMappingIsExact();
    RoundedInteriorKeyIsNotExact();
    BezierSpike();
    CollisionClipsOuterSegment();
    AllKeysCollapseAndFrameZero();
    MergedGroupRules();
    StepKeepsLastState();
    FailureLeavesOutputUntouched();
    FrameMappingAndGrouping();
    IdentityAliasingAndAutomatic();
    LongTrackCompletes();
    HundredThousandKeyTiming();
    if (g_failures != 0) {
        std::printf("Animation retime tests failed (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("Animation retime tests passed\n");
    return 0;
}
