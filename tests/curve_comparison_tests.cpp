// MAINTENANCE: unit coverage for the comparison filter helper
// (include/mikumikustudio/animation_curve_comparison.hpp), the rule the curve
// editor uses to decide whether a curve really changes inside a frame span.
// Pure CPU; no Win32, DirectX or GUI.
//
// The interesting cases are the ones a handful of sampled frames answers
// wrongly: two keys with equal values and a Bezier bump between them, and a
// step that jumps on the frame after the last sample.  The last section cross
// checks the exact rule against dense sampling of animation_curve::Evaluate,
// so the two can only disagree in the safe direction: dense sampling may miss a
// variation the exact rule finds, never the other way round.
#include "mikumikustudio/animation_curve.hpp"
#include "mikumikustudio/animation_curve_comparison.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ac = mikumikustudio::animation_curve;
namespace cc = mikumikustudio::curve_comparison;

namespace {

int g_failures = 0;

void Check(bool pass, const std::string &text) {
    if (pass) return;
    ++g_failures;
    std::printf("FAIL %s\n", text.c_str());
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

// Wrapper that hides the out parameter for the readability of the assertions.
bool Constant(const ac::Track &track, double first, double last) {
    double value = 0.0;
    return cc::IsConstantOverRange(track, first, last, value);
}

bool Varied(const ac::Track &track, double first, double last) {
    return cc::IsVariedInRange(track, first, last);
}

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------
void Registration() {
    const ac::Track empty;
    Check(!cc::IsRegistered(empty), "a track without keys is not registered");
    Check(!cc::IsVarying(empty), "an unregistered track is never a comparison curve");
    Check(!Varied(empty, 0.0, 10.0), "an unregistered track is filtered out of every span");

    const ac::Track one = MakeTrack({MakeKey(5, 2.0)});
    Check(cc::IsRegistered(one), "a track with a key is registered");
    Check(!cc::IsVarying(one), "a single key holds one value and is not varying");
    Check(Constant(one, 5.0, 5.0), "a single key is constant over its own frame");
    Check(Constant(one, 0.0, 100.0), "a single key is constant over any span");
    Check(!cc::IsVarying(MakeTrack({MakeKey(0, 1.0), MakeKey(10, 1.0)})),
          "two equal keys without handles are not varying");
}

// ---------------------------------------------------------------------------
// Step and discrete intervals hold the value of their first key.
// ---------------------------------------------------------------------------
void StepIntervals() {
    const ac::Track step = MakeTrack({MakeKey(0, 0.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                      MakeKey(10, 5.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step)});
    // Step with two different values really changes on the second frame.
    Check(!Constant(step, 0.0, 10.0), "a step between different values is not constant");
    Check(Constant(step, 0.0, 9.0), "a step that is only entered before its end frame is constant");
    Check(Constant(step, 0.0, 0.0), "the first frame of a step alone is constant");
    // A single frame cannot vary, but it must report the value that frame
    // really evaluates to: the end frame of a step already carries the new one.
    double held = 0;
    Check(cc::IsConstantOverRange(step, 10.0, 10.0, held) && held == 5.0,
          "the end frame of a step alone reports the new value");
    Check(Constant(step, -5.0, 0.0), "before the first key a step is clamped to its first value");

    const ac::Track flat = MakeTrack({MakeKey(0, 4.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                      MakeKey(10, 4.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                      MakeKey(20, 4.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step)});
    Check(Constant(flat, 0.0, 20.0), "a step that holds one value is constant");

    // A discrete track interpolates as a step whatever its keys ask for.
    const ac::Track discrete = MakeTrack({MakeKey(0, 1.0), MakeKey(10, 0.0)}, true);
    Check(!Constant(discrete, 0.0, 10.0), "a discrete channel that changes is not constant");
    Check(Constant(discrete, 0.0, 5.0), "a discrete channel holds its value until the next key");
    Check(!cc::IsVarying(MakeTrack({MakeKey(0, 3.0), MakeKey(10, 3.0)}, true)),
          "a discrete channel with equal values is not varying");
}

// ---------------------------------------------------------------------------
// Linear intervals are the secant of their two keys.
// ---------------------------------------------------------------------------
void LinearIntervals() {
    const ac::Track rising = MakeTrack({MakeKey(0, 0.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Linear),
                                        MakeKey(10, 5.0, 0, 0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Linear)});
    Check(!Constant(rising, 0.0, 10.0), "a rising linear interval is not constant");
    Check(!Constant(rising, 2.0, 4.0), "a part of a rising linear interval is not constant");

    const ac::Track flat = MakeTrack({MakeKey(0, 2.0, 4.0, 7.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Linear),
                                      MakeKey(10, 2.0, -3.0, 9.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Linear)});
    // The stored slopes are irrelevant to a linear interval: the core ignores
    // them and draws the secant, so equal keys stay constant.
    Check(Constant(flat, 0.0, 10.0), "a linear interval with equal keys is constant whatever its slopes say");
}

// ---------------------------------------------------------------------------
// Bezier intervals: the cubic decides, not the endpoints.
// ---------------------------------------------------------------------------
void BezierIntervals() {
    // Equal endpoints and equal value handles: the cubic is identically that
    // one value, so the curve is constant.
    const ac::Track flat = MakeTrack({MakeKey(0, 3.0, 0.0, 0.0), MakeKey(10, 3.0, 0.0, 0.0)});
    Check(Constant(flat, 0.0, 10.0), "a Bezier with one value and flat handles is constant");
    Check(!cc::IsVarying(flat), "a Bezier with one value and flat handles is not a comparison curve");

    // The case the requirement names: equal endpoints, but the handles pull the
    // middle of the interval away from them.
    const ac::Track bump = MakeTrack({MakeKey(0, 0.0, 0.0, 2.0), MakeKey(10, 0.0, -2.0, 0.0)});
    Check(!Constant(bump, 0.0, 10.0), "a Bezier with equal endpoints but a moved middle is not constant");
    Check(!Constant(bump, 1.0, 9.0), "the middle of that Bezier is not constant either");
    Check(cc::IsVarying(bump), "that Bezier is a comparison curve");
    Check(ac::Evaluate(bump, 0.0).value == 0.0 && ac::Evaluate(bump, 10.0).value == 0.0,
          "both endpoints of that Bezier really evaluate to the same value");

    // A peak that lives entirely between two frames: sampling the integer
    // frames alone cannot see it, the cubic can.
    const ac::Track spike = MakeTrack({MakeKey(0, 0.0, 0.0, 1.0), MakeKey(1, 0.0, -1.0, 0.0)});
    Check(!Constant(spike, 0.0, 1.0), "a sub-frame peak between two equal keys is detected");
    Check(ac::Evaluate(spike, 0.5).value > 0.1, "that peak really rises between the two frames");

    // Handle weights alone cannot make a flat interval vary: as long as the
    // four value controls stay equal the cubic is that one value.
    const ac::Track wideHandles =
        MakeTrack({MakeKey(0, 1.0, 0.0, 0.0, 0.05, 0.9), MakeKey(10, 1.0, 0.0, 0.0, 0.05, 0.9)});
    Check(Constant(wideHandles, 0.0, 10.0), "handle weights cannot make a flat Bezier vary");

    // A span that only touches the flat part of a varying curve stays constant.
    const ac::Track late = MakeTrack({MakeKey(0, 0.0, 0.0, 0.0), MakeKey(10, 0.0, 0.0, 0.0),
                                      MakeKey(20, 0.0, -2.0, 0.0)});
    Check(Constant(late, 0.0, 10.0), "a span in front of the moving interval is constant");
    Check(!Constant(late, 15.0, 20.0), "a span inside the moving interval is not constant");
}

// ---------------------------------------------------------------------------
// Spans: partial overlap, clamped regions and degenerate bounds.
// ---------------------------------------------------------------------------
void Spans() {
    const ac::Track track = MakeTrack({MakeKey(0, 0.0, 0.0, 0.0), MakeKey(10, 0.0, 0.0, 0.0),
                                       MakeKey(20, 5.0, 0.0, 0.0)});
    Check(Constant(track, 0.0, 10.0), "the flat leading interval is constant");
    Check(!Constant(track, 10.0, 20.0), "the rising interval is not constant");
    Check(!Constant(track, 5.0, 15.0), "a span that enters a rising interval is not constant");
    Check(Constant(track, 10.0, 10.0), "a single frame in the middle is constant");
    Check(Constant(track, -50.0, 0.0), "a span before the first key is clamped and constant");
    Check(Constant(track, 20.0, 90.0), "a span after the last key is clamped and constant");
    Check(!Constant(track, -50.0, 90.0), "a span that covers the whole curve is not constant");

    // The bounds may arrive in either order.
    Check(Varied(track, 20.0, 10.0), "swapped bounds describe the same span");
    Check(Constant(track, 10.0, 0.0), "swapped bounds of a constant span stay constant");
    Check(!Constant(track, 15.0, 5.0), "swapped bounds of a varying span stay varying");

    // Non-finite bounds cannot be analysed, so such a curve stays visible
    // instead of being dropped from the comparison.
    Check(Varied(MakeTrack({MakeKey(0, 1.0), MakeKey(10, 1.0)}), 0.0,
                 std::numeric_limits<double>::quiet_NaN()),
          "a non-finite span keeps an equal-valued curve visible");
    double value = 0.0;
    const ac::Track nan = MakeTrack({MakeKey(0, 1.0), MakeKey(10, 2.0)});
    Check(!cc::IsConstantOverRange(nan, std::numeric_limits<double>::infinity(), 0.0, value),
          "an infinite bound is refused instead of being reported as constant");
}

// ---------------------------------------------------------------------------
// Consistency with the core sampler.
// ---------------------------------------------------------------------------
double DenseMinMax(const ac::Track &track, double first, double last, double &low, double &high) {
    low = high = ac::Evaluate(track, first).value;
    const int steps = 4000;
    for (int step = 0; step <= steps; ++step) {
        const double frame = first + (last - first) * step / steps;
        const double value = ac::Evaluate(track, frame).value;
        low = std::min(low, value);
        high = std::max(high, value);
    }
    return high - low;
}

// What the naive "sample a few frames" test sees: the integer frames of the
// span and nothing between them.
bool IntegerFramesLookFlat(const ac::Track &track, double first, double last) {
    const long long lowFrame = static_cast<long long>(std::ceil(first));
    const long long highFrame = static_cast<long long>(std::floor(last));
    double low = 0, high = 0;
    bool any = false;
    for (long long frame = lowFrame; frame <= highFrame; ++frame) {
        const double value = ac::Evaluate(track, static_cast<double>(frame)).value;
        if (!any) {
            low = high = value;
            any = true;
        } else {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    return !any || high - low <= 1.0e-12;
}

void MatchesTheSampler() {
    std::vector<ac::Track> tracks;
    tracks.push_back(MakeTrack({MakeKey(0, 0.0, 0.0, 0.0), MakeKey(10, 0.0, 0.0, 0.0)}));
    tracks.push_back(MakeTrack({MakeKey(0, 0.0, 0.0, 2.0), MakeKey(10, 0.0, -2.0, 0.0)}));
    tracks.push_back(MakeTrack({MakeKey(0, 0.0, 0.0, 1.0), MakeKey(1, 0.0, -1.0, 0.0)}));
    tracks.push_back(MakeTrack({MakeKey(0, 0.0, 0.0, 0.0), MakeKey(10, 5.0, 3.0, 0.0), MakeKey(20, 0.0, -4.0, 0.0)}));
    tracks.push_back(MakeTrack({MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step),
                                MakeKey(10, 5.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Step)}));
    tracks.push_back(MakeTrack({MakeKey(0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Linear),
                                MakeKey(10, 5.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, ac::Interpolation::Linear)}));
    tracks.push_back(MakeTrack({MakeKey(0, 2.0), MakeKey(10, 2.0)}, true));
    tracks.push_back(MakeTrack({MakeKey(0, 2.0, 0.0, 0.0), MakeKey(10, 2.0, 0.0, 0.0),
                                MakeKey(20, 2.0, 0.0, 0.0)}));

    const double spans[][2] = {{0.0, 20.0}, {0.0, 10.0}, {5.0, 15.0}, {-5.0, 5.0}, {15.0, 25.0},
                               {9.5, 10.5}, {0.0, 0.5}, {19.0, 20.0}};
    int agreements = 0;
    int coarseMisses = 0;
    for (const ac::Track &track : tracks) {
        for (const auto &span : spans) {
            double low = 0, high = 0;
            const double spread = DenseMinMax(track, span[0], span[1], low, high);
            const bool constant = Constant(track, span[0], span[1]);
            // Dense sampling finding a spread proves the curve varies; the
            // exact rule may never call that constant.
            Check(!(spread > 1.0e-9 && constant),
                  "the exact rule agrees with the sampler wherever the sampler sees a change");
            if (spread > 1.0e-9) ++agreements;
            if (constant && spread <= 1.0e-9) ++agreements;
            // The case the exact rule exists for: every integer frame of the
            // span holds one value while the curve really moves in between.
            if (!constant && IntegerFramesLookFlat(track, span[0], span[1])) ++coarseMisses;
        }
    }
    Check(agreements > 40, "the cross check covers a broad set of spans");
    Check(coarseMisses > 0, "at least one span varies only between the integer frames");
    std::printf("comparison cross check: %d agreements, %d frame-sampled misses\n", agreements, coarseMisses);
}

}  // namespace

int main() {
    Registration();
    StepIntervals();
    LinearIntervals();
    BezierIntervals();
    Spans();
    MatchesTheSampler();
    if (g_failures != 0) {
        std::printf("Curve comparison tests failed (%d checks)\n", g_failures);
        return 1;
    }
    std::printf("Curve comparison tests passed\n");
    return 0;
}
