#include "mikumikustudio/frame_limits.hpp"
// MAINTENANCE: production pure-CPU frame-rate retiming for
// animation_curve::Track.  It is header-only in spirit: the implementation in
// src/animation/animation_retime.cpp depends on the core curve module only, so
// the host, the editor and background tests can all use it.
//
// What one run does
// -----------------
// Every key frame f becomes round(f * currentFPS / originalFPS); frames are
// computed with exact integer arithmetic whenever both rates are whole numbers,
// and with long double otherwise, so fractional rates such as 29.97 work
// without a special case.  Two rates that are equal are an identity and copy
// the track field for field, whatever form they have.
//
// A run of one key keeps its value.  A merged run does not: it takes one value
// derived from the input curve (sampled at the inverse-mapped frame, or the
// group's last state for Step and discrete groups), so values are never
// silently copied from a single key when the run spanned several.
//
// What is guaranteed, and what is not
// -----------------------------------
// A rounding that moves an interior key can make one output interval cover a
// stretch of the ORIGINAL key list that crosses an original key frame.  The
// original curve there is two cubic segments, and one cubic cannot represent it
// exactly.  The same is true of every collapsed group.  Therefore:
//
//   * integralMapping (every key lands exactly on its scaled position) means
//     the output curve equals the input curve in the time domain at every real
//     frame: values and normalised weights are untouched and each side slope is
//     multiplied by its own interval factor oldSpan/newSpan.  Only this case
//     claims exact equivalence.
//   * curveShapePreserved means every surviving interval kept its value
//     controls exactly (no clip, no merge): the curve as a function of the
//     Bezier parameter is unchanged and only the timing inside the intervals is
//     scaled.  It is still NOT exact in the time domain.
//   * otherwise maxSampledValueError is a SAMPLED measurement of
//     |output(tau) - input(tau/ratio)|.  It is not a strict bound and must not
//     be presented as one.  The one case that is not sampled at all is an
//     integral mapping with no merge and no clip: there the equality is
//     guaranteed by construction, the report marks shapePreserved and leaves
//     maxSampledValueError at zero, and a six-hundred-thousand key model does
//     not pay for a full sampling pass.  Every other case - any rounded frame,
//     any merge, any clip - is measured.
//   * a collapsed group can lose a value jump or a value excursion; the exact
//     span of what it covered is reported per group (maxCollapsedValueRange) so
//     a caller can preview the loss.  A discrete track always keeps the last
//     state of a collapsed group, and a continuous track does that only when
//     every folded interval inside the group is a Step interval; otherwise the
//     value is sampled.  Nothing is ever averaged and no centre value is used.
//   * only the group that holds the original frame-zero key AND lands on frame
//     zero keeps that baseline value; a first key that merely rounds to frame
//     zero is an ordinary merge.
//
// Rules the implementation follows
// --------------------------------
//   * A segment between two groups uses the original outer segment that starts
//     at the left group's last input key and ends at the right group's first
//     input key; those two keys are adjacent by construction.
//   * Only segments adjacent to a collapsed group are clipped.  The clip
//     position is the inverse-mapped target frame, clamped into that original
//     segment, and the clipped control polygon is mapped onto the surviving
//     integer interval.
//   * When a shared endpoint takes the merged value, the same-side neighbouring
//     value control is shifted by the same delta, so the incoming or outgoing
//     tangent at the joint is not rotated by the join.
//   * A segment without a collapsed neighbour keeps its key values and
//     normalised weights and scales its two side slopes by oldSpan/newSpan.
//   * Nothing is repaired silently: an unrepresentable result fails the whole
//     call and leaves the output untouched.  There is no widening of merge
//     groups and no whole-track degradation.
//   * The input and the output may be the same object.
//   * Tangent::Automatic keys are switched to Tangent::Free (and counted) when
//     a later automatic recomputation would move them, so the retimed shape
//     cannot be undone by the next edit.  Identity rates copy the track
//     field for field, Automatic included.
//
// Cost: the module's own bookkeeping is linear, but every sample calls
// animation_curve::Evaluate, which finds its interval with a binary search over
// the keys, so a full run is O(keys * log keys) plus the fixed sample count per
// interval.
#pragma once
#include "mikumikustudio/animation_curve.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mikumikustudio::animation_retime {

// Frames per second.  Both values must be finite and positive; fractional rates
// are allowed.  newFrame = round(frame * currentFPS / originalFPS).
struct Rates {
    double originalFPS = 30.0;
    double currentFPS = 30.0;
};

// Limits of one run.  maximumFrame is the largest frame the caller accepts; a
// key that would land beyond it fails the call instead of being clamped.
struct Options {
    std::int32_t maximumFrame = mikumikustudio::frame_limits::maximum;
};

// One run of input keys that map to the same output frame.  first and last
// index the input key vector; frame is the output frame of the whole run.  A
// group with last > first is a merge.
struct Group {
    std::size_t first = 0;
    std::size_t last = 0;
    std::int32_t frame = 0;
};

struct Report {
    // All counters are PER TRACK.  A caller that aggregates several component
    // tracks of one native record (a bone's four rotation channels, for example)
    // must count that record once instead of once per component.
    //
    // Required counters and flags.
    std::size_t inputKeys = 0;
    std::size_t outputKeys = 0;
    std::size_t mergedKeys = 0;   // keys removed by merging (inputKeys - outputKeys)
    std::size_t mergeGroups = 0;  // groups that hold more than one input key
    std::size_t roundedKeys = 0;  // keys whose frame was moved by rounding
    std::size_t frozenTangents = 0;
    bool integralMapping = false;
    bool shapePreserved = false;      // exact in the time domain; integral mapping only
    double maxSampledValueError = 0.0;  // sampled, NOT a strict bound
    double maxCollapsedValueRange = 0.0;
    // Additional diagnostics.
    bool curveShapePreserved = false;   // every surviving interval kept its value controls
    std::size_t clippedSegments = 0;    // segments that used De Casteljau clipping
    std::size_t degenerateClamps = 0;   // clips that clamped to the whole original segment
    double maxFrameError = 0.0;         // max |newSpan - ratio * oldSpan| in frames
};

// Rounds one frame.  `maximum` bounds the result; on failure `out` is left
// alone and `error` explains why.
bool MapFrame(std::int64_t frame, const Rates &rates, std::int32_t maximum, std::int32_t &out, std::string &error);

// Groups a strictly increasing frame list.  `groups` and `report` are only
// written when the call succeeds; the counters inputKeys, outputKeys,
// mergedKeys, mergeGroups, roundedKeys and integralMapping are filled here.
bool GroupFrames(const std::vector<std::int32_t> &frames, const Rates &rates, const Options &options,
                 std::vector<Group> &groups, Report &report, std::string &error);

// The whole operation.  On failure `output` and `report` are untouched.
bool Retime(const animation_curve::Track &input, const Rates &rates, const Options &options,
            animation_curve::Track &output, Report &report, std::string &error);

}  // namespace mikumikustudio::animation_retime
