// MAINTENANCE: implementation of include/mikumikustudio/animation_retime.hpp.
// Pure CPU.  It links against the curve core (animation_curve.cpp) only, never
// against a renderer, the app state or the UI, so the host, the editor and the
// background tests can all use it.
//
// The full statement of what is and is not preserved lives in the header; this
// file repeats the three decisions that are easy to get wrong:
//
//   1. A segment without a collapsed neighbour is re-expressed by keeping its
//      key values and normalised weights and scaling each side slope by
//      oldSpan/newSpan.  The value controls y1/y2 are then identical to the
//      original ones, so the curve in Bezier space is unchanged and only the
//      timing inside the interval moves.  This is NOT time-domain equality.
//   2. A segment next to a collapsed group is clipped: the target frames are
//      inverse-mapped, clamped into the original outer segment, and the clipped
//      control polygon is mapped onto the surviving integer interval.  When an
//      endpoint takes the merged value, the neighbouring control on that side is
//      shifted by the same delta, so the tangent at the joint is not rotated.
//   3. Nothing is repaired silently.  If the result cannot be expressed inside
//      the core's key limits, the whole call fails and the output is untouched.
#include "mikumikustudio/animation_retime.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace mikumikustudio::animation_retime {
namespace {

using animation_curve::Interpolation;
using animation_curve::Key;
using animation_curve::Tangent;
using animation_curve::Track;

// The same limits the curve core enforces in animation_curve.cpp:36-40.
constexpr double kWeightMinimum = 0.0001;
constexpr double kWeightMaximum = 0.9999;
constexpr double kWeightSumMaximum = 0.9999;
// Samples per output interval for the error measurement, plus the analytic
// extrema of both sides of the comparison.  The count is fixed, so it does not
// grow with the track (the measurements themselves still cost a binary search
// per sample inside animation_curve::Evaluate).
constexpr int kSamplesPerInterval = 16;
constexpr double kRateTolerance = 1e-9;

bool Finite(double value) { return std::isfinite(value); }

// ---------------------------------------------------------------------------
// Frame mapping
// ---------------------------------------------------------------------------
struct Mapping {
    bool integer = false;          // both rates were whole numbers
    bool same = false;             // the two rates are equal, so nothing moves
    std::int64_t numerator = 1;    // currentFPS / gcd, used when integer
    std::int64_t denominator = 1;  // originalFPS / gcd
    long double ratio = 1.0L;      // currentFPS / originalFPS in both cases

    // Equal rates are an identity whatever their form: 29.97 to 29.97 must copy
    // the track field for field even though neither rate is a whole number.
    bool identity() const { return same; }

    // Exact (unrounded) position of one input frame in the output timeline.
    long double Exact(std::int64_t frame) const {
        if (integer) return (long double)(frame * numerator) / (long double)denominator;
        return (long double)frame * ratio;
    }

    // Position an output frame maps back to in the input timeline.
    long double Inverse(std::int32_t frame) const { return (long double)frame / ratio; }

    // True when the frame lands exactly on its scaled position.
    bool ExactPosition(std::int64_t frame) const {
        const long double exact = Exact(frame);
        return std::fabs(exact - std::floor(exact + 0.5L)) <= 1e-12L;
    }

    bool Round(std::int64_t frame, std::int32_t maximum, std::int32_t &out, std::string &error) const {
        if (frame < 0) {
            error = "negative frame " + std::to_string(frame) + " cannot be retimed";
            return false;
        }
        if (maximum < 0) {
            error = "the accepted maximum frame must not be negative";
            return false;
        }
        const long double limit = static_cast<long double>(maximum);
        if (integer) {
            // Round half up through quotient and remainder, so frame * numerator
            // and 2 * frame * numerator are never formed and cannot overflow.
            const std::int64_t quotient = frame / denominator;
            const std::int64_t remainder = frame % denominator;
            const std::int64_t largest = std::numeric_limits<std::int64_t>::max();
            if (numerator != 0 && quotient > (largest - denominator) / numerator) {
                error = "frame " + std::to_string(frame) + " maps far past the accepted maximum " +
                        std::to_string(maximum);
                return false;
            }
            const std::int64_t whole = quotient * numerator;
            const std::int64_t extra = (2 * (remainder * numerator) + denominator) / (2 * denominator);
            if (whole > static_cast<std::int64_t>(maximum) ||
                extra > static_cast<std::int64_t>(maximum) - whole) {
                error = "frame " + std::to_string(frame) + " maps past the accepted maximum " +
                        std::to_string(maximum);
                return false;
            }
            out = static_cast<std::int32_t>(whole + extra);
            return true;
        }
        // Fractional rates: long double throughout.  The range is checked in
        // long double before any conversion, so a huge position never reaches a
        // narrowing cast or an out-of-range integer format.
        const long double exact = Exact(frame);
        if (!(exact >= 0.0L) || !(exact <= limit + 0.5L)) {
            error = "frame " + std::to_string(frame) + " maps past the accepted maximum " +
                    std::to_string(maximum);
            return false;
        }
        const long double rounded = std::floor(exact + 0.5L);
        if (rounded > limit) {
            error = "frame " + std::to_string(frame) + " maps past the accepted maximum " +
                    std::to_string(maximum);
            return false;
        }
        out = static_cast<std::int32_t>(rounded);
        return true;
    }
};

bool BuildMapping(const Rates &rates, Mapping &mapping, std::string &error) {
    if (!Finite(rates.originalFPS) || !Finite(rates.currentFPS) || !(rates.originalFPS > 0.0) ||
        !(rates.currentFPS > 0.0)) {
        error = "both frame rates must be finite and positive";
        return false;
    }
    mapping.same = rates.currentFPS == rates.originalFPS;
    const double roundedOriginal = std::floor(rates.originalFPS + 0.5);
    const double roundedCurrent = std::floor(rates.currentFPS + 0.5);
    const bool whole = std::fabs(rates.originalFPS - roundedOriginal) <= kRateTolerance &&
                       std::fabs(rates.currentFPS - roundedCurrent) <= kRateTolerance;
    if (whole && roundedOriginal >= 1.0 && roundedCurrent >= 1.0 && roundedOriginal <= 1.0e6 &&
        roundedCurrent <= 1.0e6) {
        std::int64_t numerator = static_cast<std::int64_t>(roundedCurrent);
        std::int64_t denominator = static_cast<std::int64_t>(roundedOriginal);
        const std::int64_t divisor = std::gcd(numerator, denominator);
        mapping.integer = true;
        mapping.numerator = numerator / divisor;
        mapping.denominator = denominator / divisor;
        mapping.ratio = (long double)mapping.numerator / (long double)mapping.denominator;
        return true;
    }
    // Fractional rates: long double throughout, documented as such.
    mapping.integer = false;
    mapping.ratio = (long double)rates.currentFPS / (long double)rates.originalFPS;
    if (!(mapping.ratio > 0.0L)) {
        error = "the frame rate ratio is not positive";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Grouping
// ---------------------------------------------------------------------------
bool BuildGroups(const std::vector<std::int32_t> &frames, const Mapping &mapping, const Options &options,
                 std::vector<Group> &groups, Report &report, std::string &error) {
    std::vector<Group> result;
    Report counters;
    if (frames.empty()) {
        error = "cannot retime a track without keys";
        return false;
    }
    if (options.maximumFrame < 0) {
        error = "the accepted maximum frame must not be negative";
        return false;
    }
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (index > 0 && frames[index] <= frames[index - 1]) {
            error = "key frames must increase strictly: frame " + std::to_string(frames[index]) +
                    " follows frame " + std::to_string(frames[index - 1]);
            return false;
        }
        std::int32_t mapped = 0;
        if (!mapping.Round(frames[index], options.maximumFrame, mapped, error)) return false;
        if (!mapping.ExactPosition(frames[index])) ++counters.roundedKeys;
        if (!result.empty() && result.back().frame == mapped) {
            result.back().last = index;
            continue;
        }
        if (!result.empty() && mapped < result.back().frame) {
            error = "the frame mapping inverted at frame " + std::to_string(frames[index]);
            return false;
        }
        Group group;
        group.first = index;
        group.last = index;
        group.frame = mapped;
        result.push_back(group);
    }
    counters.inputKeys = frames.size();
    counters.outputKeys = result.size();
    counters.mergedKeys = counters.inputKeys - counters.outputKeys;
    counters.integralMapping = counters.roundedKeys == 0;
    for (const Group &group : result) {
        if (group.last > group.first) ++counters.mergeGroups;
    }
    groups = std::move(result);
    report = counters;
    return true;
}

// ---------------------------------------------------------------------------
// Interval geometry (mirrors animation_curve.cpp:113-141)
// ---------------------------------------------------------------------------
struct Interval {
    bool valid = false;
    bool step = false;    // Step or a discrete track
    bool linear = false;
    long double span = 0.0L;      // frames
    long double x1 = 0.0L, x2 = 0.0L;
    long double y[4] = {0.0L, 0.0L, 0.0L, 0.0L};
    double frame0 = 0.0, frame1 = 0.0;
    double value0 = 0.0, value1 = 0.0;
};

Interval MakeInterval(const Track &track, std::size_t endKey) {
    Interval interval;
    if (endKey == 0 || endKey >= track.keys.size()) return interval;
    const Key &start = track.keys[endKey - 1];
    const Key &end = track.keys[endKey];
    const double span = static_cast<double>(end.frame) - static_cast<double>(start.frame);
    if (!(span > 0.0)) return interval;
    interval.valid = true;
    interval.frame0 = start.frame;
    interval.frame1 = end.frame;
    interval.value0 = start.value;
    interval.value1 = end.value;
    interval.span = span;
    interval.step = track.discrete || end.interpolation == Interpolation::Step;
    interval.linear = !interval.step && end.interpolation == Interpolation::Linear;
    interval.x1 = start.outWeight;
    interval.x2 = 1.0 - end.inWeight;
    interval.y[0] = start.value;
    interval.y[1] = start.value + start.outSlope * start.outWeight * span;
    interval.y[2] = end.value - end.inSlope * end.inWeight * span;
    interval.y[3] = end.value;
    return interval;
}

long double Lerp(long double a, long double b, long double s) { return a + (b - a) * s; }

// Frame of a normalised Bezier parameter, forward direction.
long double FrameAt(const Interval &interval, long double s) {
    const long double t = 1.0L - s;
    const long double x = 3.0L * t * t * s * interval.x1 + 3.0L * t * s * s * interval.x2 + s * s * s;
    return interval.frame0 + interval.span * x;
}

// Mirrors animation_curve.cpp:86-108: the caller guarantees 0 < x1 < x2 < 1.
long double SolveNormalisedTime(long double x1, long double x2, long double u) {
    const long double cubic = 1.0L - 3.0L * x2 + 3.0L * x1;
    const long double quadratic = 3.0L * x2 - 6.0L * x1;
    const long double linear = 3.0L * x1;
    const auto evaluate = [&](long double s) { return ((cubic * s + quadratic) * s + linear) * s; };
    const auto derivative = [&](long double s) { return (3.0L * cubic * s + 2.0L * quadratic) * s + linear; };
    long double low = 0.0L, high = 1.0L, s = u;
    for (int iteration = 0; iteration < 96; ++iteration) {
        const long double residual = evaluate(s) - u;
        if (residual > 0.0L)
            high = s;
        else
            low = s;
        if (high - low <= 1e-18L) break;
        const long double slope = derivative(s);
        long double next = slope > 1e-15L ? s - residual / slope : 0.5L * (low + high);
        if (!(next > low && next < high)) next = 0.5L * (low + high);
        s = next;
    }
    return s;
}

struct Point {
    long double x = 0.0L, y = 0.0L;
};

void SplitCubic(const Point control[4], long double s, Point left[4], Point right[4]) {
    Point a{Lerp(control[0].x, control[1].x, s), Lerp(control[0].y, control[1].y, s)};
    Point b{Lerp(control[1].x, control[2].x, s), Lerp(control[1].y, control[2].y, s)};
    Point c{Lerp(control[2].x, control[3].x, s), Lerp(control[2].y, control[3].y, s)};
    Point d{Lerp(a.x, b.x, s), Lerp(a.y, b.y, s)};
    Point e{Lerp(b.x, c.x, s), Lerp(b.y, c.y, s)};
    Point f{Lerp(d.x, e.x, s), Lerp(d.y, e.y, s)};
    left[0] = control[0];
    left[1] = a;
    left[2] = d;
    left[3] = f;
    right[0] = f;
    right[1] = e;
    right[2] = c;
    right[3] = control[3];
}

// Parameters in (0,1) where the interval value polynomial has a local extremum.
int ValueExtrema(const long double y[4], long double out[2]) {
    const long double a = y[1] - y[0];
    const long double b = y[2] - y[1];
    const long double c = y[3] - y[2];
    const long double qa = a - 2.0L * b + c;   // s^2
    const long double qb = 2.0L * (b - a);     // s
    const long double qc = a;                  // constant
    int count = 0;
    const auto keep = [&](long double s) {
        if (s > 0.0L && s < 1.0L && count < 2) out[count++] = s;
    };
    if (std::fabs(qa) < 1e-18L) {
        if (std::fabs(qb) > 1e-18L) keep(-qc / qb);
        return count;
    }
    const long double discriminant = qb * qb - 4.0L * qa * qc;
    if (discriminant < 0.0L) return 0;
    const long double root = std::sqrt(discriminant);
    keep((-qb + root) / (2.0L * qa));
    keep((-qb - root) / (2.0L * qa));
    return count;
}

// Value range of one original interval, extrema included and no sampling.
void IntervalValueRange(const Interval &interval, double &low, double &high) {
    low = std::min(interval.value0, interval.value1);
    high = std::max(interval.value0, interval.value1);
    if (interval.step || interval.linear) return;
    long double parameters[2] = {0.0L, 0.0L};
    const int count = ValueExtrema(interval.y, parameters);
    for (int index = 0; index < count; ++index) {
        const long double s = parameters[index];
        const long double t = 1.0L - s;
        const long double value = t * t * t * interval.y[0] + 3.0L * t * t * s * interval.y[1] +
                                  3.0L * t * s * s * interval.y[2] + s * s * s * interval.y[3];
        low = std::min(low, (double)value);
        high = std::max(high, (double)value);
    }
}

// Mirrors animation_curve.cpp:148-167 so an Automatic key can be compared with
// what a later UpdateAutomatic would compute for its new neighbours.
double SecantSlope(const Key &start, const Key &end) {
    const double span = static_cast<double>(end.frame) - static_cast<double>(start.frame);
    return span > 0.0 ? (end.value - start.value) / span : 0.0;
}

double AutomaticSlope(const Track &track, std::size_t index) {
    const std::vector<Key> &keys = track.keys;
    if (keys.size() < 2) return 0.0;
    if (index == 0) return SecantSlope(keys[0], keys[1]);
    if (index + 1 >= keys.size()) return SecantSlope(keys[index - 1], keys[index]);
    const Key &key = keys[index];
    const double previousSpan = static_cast<double>(key.frame) - static_cast<double>(keys[index - 1].frame);
    const double nextSpan = static_cast<double>(keys[index + 1].frame) - static_cast<double>(key.frame);
    if (!(previousSpan > 0.0) || !(nextSpan > 0.0)) return 0.0;
    const double previousSlope = SecantSlope(keys[index - 1], key);
    const double nextSlope = SecantSlope(key, keys[index + 1]);
    if (previousSlope * nextSlope <= 0.0) return 0.0;
    const double previousWeight = 2.0 * nextSpan + previousSpan;
    const double nextWeight = nextSpan + 2.0 * previousSpan;
    return (previousWeight + nextWeight) / (previousWeight / previousSlope + nextWeight / nextSlope);
}

// ---------------------------------------------------------------------------
// Merge values
// ---------------------------------------------------------------------------
bool MergeValue(const Track &input, const std::vector<Group> &groups, std::size_t index, const Mapping &mapping,
                double &out) {
    const Group &group = groups[index];
    // Only a group that really holds the original frame-zero key AND lands on
    // frame zero keeps that baseline value.  A first key that merely rounds to
    // frame zero (frames 14 and 15 at ratio 0.07 land on frame 1) is an ordinary
    // merged group and takes the sampled value.
    if (input.keys[group.first].frame == 0 && group.frame == 0) {
        out = input.keys[group.first].value;
        return true;
    }
    // A discrete track always keeps the group's last state.  A continuous track
    // does that only when every folded interval inside the group is a Step
    // interval: the interpolation of the group's first key belongs to the OUTER
    // segment and says nothing about the intervals inside the group.
    bool everyFoldedIntervalIsStep = true;
    for (std::size_t member = group.first; member < group.last; ++member) {
        if (input.keys[member + 1].interpolation != Interpolation::Step) {
            everyFoldedIntervalIsStep = false;
            break;
        }
    }
    if (input.discrete || everyFoldedIntervalIsStep) {
        out = input.keys[group.last].value;
        return true;
    }
    const double frame = static_cast<double>(mapping.Inverse(group.frame));
    out = animation_curve::Evaluate(input, frame).value;
    return Finite(out);
}

// ---------------------------------------------------------------------------
// Retiming
// ---------------------------------------------------------------------------
bool BuildSegment(const Track &input, const std::vector<Group> &groups, std::size_t index, const Mapping &mapping,
                  Track &work, Report &report, std::string &error) {
    const Group &left = groups[index];
    const Group &right = groups[index + 1];
    const std::size_t first = left.last;   // the outer segment starts at the left group's last key
    const std::size_t second = right.first;  // and ends at the right group's first key
    if (second != first + 1 || second >= input.keys.size()) {
        error = "internal error: groups are not adjacent at output key " + std::to_string(index + 1);
        return false;
    }
    const Key &start = input.keys[first];
    const Key &end = input.keys[second];
    Key &startKey = work.keys[index];
    Key &endKey = work.keys[index + 1];

    const Interpolation interpolation = input.discrete ? Interpolation::Step : end.interpolation;
    endKey.interpolation = interpolation;
    const long double oldSpan = static_cast<long double>(end.frame) - static_cast<long double>(start.frame);
    const long double newSpan = static_cast<long double>(right.frame) - static_cast<long double>(left.frame);
    if (!(oldSpan > 0.0L) || !(newSpan > 0.0L)) {
        error = "interval ending at output key " + std::to_string(index + 1) + " has no duration";
        return false;
    }
    report.maxFrameError = std::max(report.maxFrameError, std::fabs((double)(newSpan - mapping.ratio * oldSpan)));

    if (interpolation == Interpolation::Step) {
        // The held value comes from the start key and the end frame takes the
        // end value, exactly as the core evaluates a Step interval.
        startKey.outWeight = 1.0 / 3.0;
        startKey.outSlope = 0.0;
        endKey.inWeight = 1.0 / 3.0;
        endKey.inSlope = 0.0;
        return true;
    }

    const bool mergedSide = left.last > left.first || right.last > right.first;
    if (!mergedSide) {
        // No collision next to this segment: keep both key values and the
        // normalised weights, scale each side slope by oldSpan/newSpan.  The
        // value controls are then unchanged.
        const double factor = static_cast<double>(oldSpan / newSpan);
        startKey.outWeight = start.outWeight;
        startKey.outSlope = start.outSlope * factor;
        endKey.inWeight = end.inWeight;
        endKey.inSlope = end.inSlope * factor;
        return true;
    }

    if (interpolation == Interpolation::Linear) {
        // A restricted secant is the original secant on the clamped window, so
        // the endpoint values are all that matter here.
        const double factor = static_cast<double>(oldSpan / newSpan);
        startKey.outWeight = start.outWeight;
        startKey.outSlope = start.outSlope * factor;
        endKey.inWeight = end.inWeight;
        endKey.inSlope = end.inSlope * factor;
        return true;
    }

    // A collapsed group touches this segment: clip the original outer segment.
    const Interval interval = MakeInterval(input, second);
    if (!interval.valid) {
        error = "interval ending at input key " + std::to_string(second) + " cannot be read";
        return false;
    }
    const long double f0 = interval.frame0;
    const long double f1 = interval.frame1;
    const long double targetLow = mapping.Inverse(left.frame);
    const long double targetHigh = mapping.Inverse(right.frame);
    long double clipLow = std::min(std::max(targetLow, f0), f1);
    long double clipHigh = std::min(std::max(targetHigh, f0), f1);
    if (!(clipHigh > clipLow)) {
        // The clamped window is empty: the only defined choice is the whole
        // original segment.  Recorded, never used to hide a failure.
        clipLow = f0;
        clipHigh = f1;
        ++report.degenerateClamps;
    }
    Point control[4] = {{f0, interval.y[0]},
                        {f0 + interval.span * interval.x1, interval.y[1]},
                        {f0 + interval.span * interval.x2, interval.y[2]},
                        {f1, interval.y[3]}};
    const long double startU = (clipLow - f0) / interval.span;
    const long double endU = (clipHigh - f0) / interval.span;
    const long double startS = SolveNormalisedTime(interval.x1, interval.x2, startU);
    const long double endS = SolveNormalisedTime(interval.x1, interval.x2, endU);
    Point leftPart[4], rightPart[4];
    SplitCubic(control, startS, leftPart, rightPart);
    const long double inner = (1.0L - startS) > 1e-18L ? (endS - startS) / (1.0L - startS) : 1.0L;
    Point clipped[4], discarded[4];
    SplitCubic(rightPart, std::min(std::max(inner, 0.0L), 1.0L), clipped, discarded);

    const long double clippedSpan = clipped[3].x - clipped[0].x;
    if (!(clippedSpan > 0.0L)) {
        error = "the clip of the interval ending at output key " + std::to_string(index + 1) + " is empty";
        return false;
    }
    // Endpoint values come from the merged keys; the neighbouring control on the
    // same side moves by the same delta so the joint keeps its tangent.
    const long double startValue = static_cast<long double>(startKey.value);
    const long double endValue = static_cast<long double>(endKey.value);
    const long double startDelta = startValue - clipped[0].y;
    const long double endDelta = endValue - clipped[3].y;
    const long double scale = newSpan / clippedSpan;
    const long double x0 = static_cast<long double>(left.frame);
    const long double x1 = x0 + (clipped[1].x - clipped[0].x) * scale;
    const long double x2 = x0 + (clipped[2].x - clipped[0].x) * scale;
    const long double x3 = static_cast<long double>(right.frame);
    const long double y0 = startValue;
    const long double y1 = clipped[1].y + startDelta;
    const long double y2 = clipped[2].y + endDelta;
    const long double y3 = endValue;

    const long double outWeight = (x1 - x0) / newSpan;
    const long double inWeight = (x3 - x2) / newSpan;
    if (!(outWeight >= kWeightMinimum && outWeight <= kWeightMaximum && inWeight >= kWeightMinimum &&
          inWeight <= kWeightMaximum && outWeight + inWeight <= kWeightSumMaximum)) {
        error = "the clip of the interval ending at output key " + std::to_string(index + 1) +
                " needs handle weights outside the core's range";
        return false;
    }
    const long double outSlope = (y1 - y0) / (x1 - x0);
    const long double inSlope = (y3 - y2) / (x3 - x2);
    if (!Finite((double)outSlope) || !Finite((double)inSlope)) {
        error = "the clip of the interval ending at output key " + std::to_string(index + 1) +
                " produces a non-finite slope";
        return false;
    }
    startKey.outWeight = (double)outWeight;
    startKey.outSlope = (double)outSlope;
    endKey.inWeight = (double)inWeight;
    endKey.inSlope = (double)inSlope;
    ++report.clippedSegments;
    return true;
}

// Sampled, NOT a strict bound: the difference of the two curves is measured on
// a fixed grid over BOTH sides plus the analytic value extrema, the one-sided
// limits of every Step jump, and the clamp regions outside both key spans.
// Walking the input intervals as well is what catches a track that collapses
// into a single output key (it has no output interval at all) and the parts the
// output compresses outside its own key span.
void MeasureSampledError(const Track &input, const Track &output, const Mapping &mapping, Report &report) {
    const auto compare = [&](long double frame, double &worst) {
        const double outValue = animation_curve::Evaluate(output, (double)frame).value;
        const double inValue = animation_curve::Evaluate(input, (double)(frame / mapping.ratio)).value;
        if (!Finite(outValue) || !Finite(inValue)) return;
        worst = std::max(worst, std::fabs(outValue - inValue));
    };
    double worst = 0.0;
    // Output side: the frames the retimed curve actually owns.
    for (std::size_t index = 0; index + 1 < output.keys.size(); ++index) {
        const long double low = output.keys[index].frame;
        const long double high = output.keys[index + 1].frame;
        for (int step = 0; step <= kSamplesPerInterval; ++step) {
            compare(low + (high - low) * step / kSamplesPerInterval, worst);
        }
        const Interval outputInterval = MakeInterval(output, index + 1);
        if (outputInterval.valid && !outputInterval.step && !outputInterval.linear) {
            long double parameters[2] = {0.0L, 0.0L};
            const int count = ValueExtrema(outputInterval.y, parameters);
            for (int position = 0; position < count; ++position)
                compare(FrameAt(outputInterval, parameters[position]), worst);
        }
    }
    // Input side: every original interval, mapped through the ratio.
    for (std::size_t index = 0; index + 1 < input.keys.size(); ++index) {
        const Interval interval = MakeInterval(input, index + 1);
        if (!interval.valid) continue;
        for (int step = 0; step <= kSamplesPerInterval; ++step) {
            compare((interval.frame0 + interval.span * step / kSamplesPerInterval) * mapping.ratio, worst);
        }
        if (interval.step) {
            // Both one-sided limits of the hold: just before the end frame and
            // exactly on it.
            const long double before = std::max((long double)interval.frame1 - 1e-6L, (long double)interval.frame0);
            compare(before * mapping.ratio, worst);
            compare(static_cast<long double>(interval.frame1) * mapping.ratio, worst);
        } else if (!interval.linear) {
            long double parameters[2] = {0.0L, 0.0L};
            const int count = ValueExtrema(interval.y, parameters);
            for (int position = 0; position < count; ++position)
                compare(FrameAt(interval, parameters[position]) * mapping.ratio, worst);
        }
    }
    // The clamp regions in front of the first key and behind the last one.
    if (!input.keys.empty()) {
        compare(static_cast<long double>(input.keys.front().frame) * mapping.ratio - 1.0L, worst);
        compare(static_cast<long double>(input.keys.back().frame) * mapping.ratio + 1.0L, worst);
    }
    report.maxSampledValueError = std::max(report.maxSampledValueError, worst);
}

// Exact span of what each collapsed group covered, extrema included, so a
// caller can preview the loss before applying the retime.
void MeasureCollapsedRanges(const Track &input, const std::vector<Group> &groups, Report &report) {
    for (const Group &group : groups) {
        if (group.last <= group.first) continue;
        double low = input.keys[group.first].value, high = low;
        for (std::size_t index = group.first; index <= group.last; ++index) {
            low = std::min(low, input.keys[index].value);
            high = std::max(high, input.keys[index].value);
        }
        for (std::size_t index = group.first; index < group.last; ++index) {
            const Interval interval = MakeInterval(input, index + 1);
            if (!interval.valid) continue;
            double intervalLow = 0.0, intervalHigh = 0.0;
            IntervalValueRange(interval, intervalLow, intervalHigh);
            low = std::min(low, intervalLow);
            high = std::max(high, intervalHigh);
        }
        report.maxCollapsedValueRange = std::max(report.maxCollapsedValueRange, high - low);
    }
}

// Automatic keys whose new neighbours would compute a different slope become
// Free, so a later UpdateAutomatic cannot undo the retimed shape.  Only the
// sides an interval actually uses count: the incoming side of the first key and
// the outgoing side of the last key belong to no interval and are copied
// verbatim, so they must not trigger a freeze on their own.
void FreezeAutomatic(Track &track, Report &report) {
    for (std::size_t index = 0; index < track.keys.size(); ++index) {
        Key &key = track.keys[index];
        if (key.tangent != Tangent::Automatic) continue;
        const double automatic = AutomaticSlope(track, index);
        const double tolerance = 1e-9 * std::max(1.0, std::fabs(automatic));
        bool moved = false;
        if (index > 0 && std::fabs(automatic - key.inSlope) > tolerance) moved = true;
        if (index + 1 < track.keys.size() && std::fabs(automatic - key.outSlope) > tolerance) moved = true;
        if (moved) {
            key.tangent = Tangent::Free;
            ++report.frozenTangents;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
bool MapFrame(std::int64_t frame, const Rates &rates, std::int32_t maximum, std::int32_t &out, std::string &error) {
    error.clear();
    Mapping mapping;
    if (!BuildMapping(rates, mapping, error)) return false;
    std::int32_t mapped = 0;
    if (!mapping.Round(frame, maximum, mapped, error)) return false;
    out = mapped;
    return true;
}

bool GroupFrames(const std::vector<std::int32_t> &frames, const Rates &rates, const Options &options,
                 std::vector<Group> &groups, Report &report, std::string &error) {
    error.clear();
    Mapping mapping;
    if (!BuildMapping(rates, mapping, error)) return false;
    std::vector<Group> result;
    Report counters;
    if (!BuildGroups(frames, mapping, options, result, counters, error)) return false;
    groups = std::move(result);
    report = counters;
    return true;
}

bool Retime(const Track &input, const Rates &rates, const Options &options, Track &output, Report &report,
            std::string &error) {
    error.clear();
    if (!animation_curve::Validate(input, error)) return false;
    Mapping mapping;
    if (!BuildMapping(rates, mapping, error)) return false;

    std::vector<Group> groups;
    Report local;
    std::vector<std::int32_t> frames;
    frames.reserve(input.keys.size());
    for (const Key &key : input.keys) frames.push_back(key.frame);
    if (!BuildGroups(frames, mapping, options, groups, local, error)) return false;

    if (mapping.identity()) {
        // Identity rates copy the track field for field, Automatic included.
        output = input;
        local.shapePreserved = true;
        local.curveShapePreserved = true;
        report = local;
        return true;
    }

    Track work;
    work.discrete = input.discrete;
    work.keys.resize(groups.size());
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const Group &group = groups[index];
        const Key &reference = input.keys[group.first];
        Key &key = work.keys[index];
        key.frame = group.frame;
        key.tangent = reference.tangent;
        key.interpolation = reference.interpolation;
        key.inWeight = reference.inWeight;
        key.outWeight = reference.outWeight;
        key.inSlope = reference.inSlope;
        key.outSlope = reference.outSlope;
        if (group.last > group.first) {
            // A merged key has no single original tangent mode; Free keeps every
            // slope the segments below compute.
            key.tangent = Tangent::Free;
            for (std::size_t member = group.first; member <= group.last; ++member) {
                if (input.keys[member].tangent != reference.tangent) key.tangent = Tangent::Free;
            }
        }
        if (group.last == group.first) {
            // A run of one key keeps its value: no collision touched it.
            key.value = input.keys[group.first].value;
        } else if (!MergeValue(input, groups, index, mapping, key.value)) {
            error = "the merged value of output key " + std::to_string(index + 1) + " is not finite";
            return false;
        }
    }
    // The first group's incoming side is never evaluated, but it still has to be
    // valid for Validate; the reference key supplies it.
    for (std::size_t index = 0; index + 1 < groups.size(); ++index) {
        if (!BuildSegment(input, groups, index, mapping, work, local, error)) return false;
    }
    if (!animation_curve::Validate(work, error)) {
        error = "the retimed track is not representable: " + error;
        return false;
    }
    FreezeAutomatic(work, local);
    // Integral mapping with no merge and no clip is exact by construction: every
    // key lands on its scaled position, the value controls are untouched and the
    // slopes are scaled exactly.  Nothing is sampled in that case - a full
    // sampling pass would dominate a six-hundred-thousand key model - and the
    // report says so by leaving maxSampledValueError at zero.
    const bool exactShape = local.integralMapping && local.mergeGroups == 0 && local.clippedSegments == 0;
    local.shapePreserved = exactShape;
    local.curveShapePreserved = local.mergeGroups == 0 && local.clippedSegments == 0;
    if (exactShape) {
        local.maxSampledValueError = 0.0;
    } else {
        // Every other case is measured: rounded frames, merges and clips can all
        // move the curve, so the sampled error has to be produced.
        MeasureCollapsedRanges(input, groups, local);
        MeasureSampledError(input, work, mapping, local);
    }
    output = std::move(work);
    report = local;
    return true;
}

}  // namespace mikumikustudio::animation_retime
