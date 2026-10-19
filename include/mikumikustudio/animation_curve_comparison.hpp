// MAINTENANCE: production pure-CPU helper for the comparison display of
// src/window/animation_curve_editor.cpp.  It is header-only and includes no
// other port header, so the host, the MMV plugin and the background tests all
// use the same rule without a new library or link step.
//
// Why the test is exact instead of sampled
// ----------------------------------------
// The comparison filter and the automatic group sets ask one question: does a
// track keep a single value over a frame span?  Sampling a handful of frames
// answers it wrongly both ways.  Two keys with equal values and a Bezier bump
// between them look constant on every integer frame, and a step that jumps
// exactly on a sampled frame looks constant as well.  The test below therefore
// works on the polynomials the core itself evaluates:
//
//   * a Step interval (and every interval of a discrete track) holds the value
//     of its first key until the frame of its second key, where the second
//     value appears;
//   * a Linear interval is the secant of its two keys;
//   * a Bezier interval is the cubic SampleInterval builds in
//     src/animation/animation_curve.cpp, whose value controls are
//         y0 = start.value
//         y1 = start.value + start.outSlope * start.outWeight * span
//         y2 = end.value   - end.inSlope   * end.inWeight   * span
//         y3 = end.value
//
// A cubic is constant on a non-degenerate interval only when all four of its
// controls are equal, so that equality is the exact constancy test and no
// sub-frame extremum can hide behind it.  Key-point values and the clamped
// regions in front of the first key and behind the last key are taken from
// animation_curve::Evaluate, so a track whose keys vary also stays detected
// when the queried span reaches past them.
#pragma once
#include "mikumikustudio/animation_curve.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace mikumikustudio::curve_comparison {

using animation_curve::Interpolation;
using animation_curve::Key;
using animation_curve::Track;

// A track takes part in a comparison only when it is registered.  An empty
// track belongs to no expression, has no value at all and is never shown as a
// comparison curve, not even in the free mode.
inline bool IsRegistered(const Track &track) { return !track.keys.empty(); }

// Exact.  True when Evaluate(track, frame) is one and the same value for every
// frame of the closed span [first, last]; `value` receives that value.  The two
// bounds may arrive in either order.  An empty track reports false because it
// carries no value: use IsRegistered to tell it apart from a curve that varies.
// A span whose bounds are not finite reports false as well, which keeps such a
// curve visible instead of silently dropping it.
inline bool IsConstantOverRange(const Track &track, double first, double last, double &value) {
    const std::vector<Key> &keys = track.keys;
    if (keys.empty()) return false;
    if (!std::isfinite(first) || !std::isfinite(last)) return false;
    if (last < first) std::swap(first, last);

    const double firstFrame = static_cast<double>(keys.front().frame);
    const double lastFrame = static_cast<double>(keys.back().frame);

    // Every value the span passes through is folded into one reference value.
    // The first mismatch proves that the track varies.
    double reference = 0.0;
    bool known = false;
    const auto note = [&reference, &known](double candidate) {
        if (!known) {
            reference = candidate;
            known = true;
            return true;
        }
        return candidate == reference;
    };

    // Outside the key range Evaluate clamps, so each side holds a single value.
    if (first < firstFrame && !note(keys.front().value)) return false;
    if (last > lastFrame && !note(keys.back().value)) return false;

    for (std::size_t index = 0; index < keys.size(); ++index) {
        const Key &start = keys[index];
        const double frame = static_cast<double>(start.frame);
        // A key frame always evaluates to its own stored value.
        if (frame >= first && frame <= last && !note(start.value)) return false;
        if (index + 1 >= keys.size()) break;

        const Key &end = keys[index + 1];
        const double openLow = std::max(first, frame);
        const double openHigh = std::min(last, static_cast<double>(end.frame));
        // The interval is entered only when it carries more than its end frame.
        if (!(openHigh > openLow)) continue;

        const Interpolation interpolation = track.discrete ? Interpolation::Step : end.interpolation;
        if (interpolation == Interpolation::Step) {
            // The step holds the first key until the second frame is reached.
            if (!note(start.value)) return false;
            continue;
        }
        if (interpolation == Interpolation::Linear) {
            if (start.value != end.value) return false;
            if (!note(start.value)) return false;
            continue;
        }
        const double span = static_cast<double>(end.frame) - frame;
        const double out = start.value + start.outSlope * start.outWeight * span;
        const double in = end.value - end.inSlope * end.inWeight * span;
        if (!(start.value == out && out == in && in == end.value)) return false;
        if (!note(start.value)) return false;
    }

    value = known ? reference : keys.front().value;
    return true;
}

// Registered and varying somewhere inside [first, last].  This is what the
// range filter keeps: a curve that really changes inside the chosen span.
inline bool IsVariedInRange(const Track &track, double first, double last) {
    if (!IsRegistered(track)) return false;
    double value = 0.0;
    return !IsConstantOverRange(track, first, last, value);
}

// Registered and varying somewhere inside its own key span.  The automatic
// group sets use this: a curve whose keys all hold one value stays out of the
// background even when it is registered.
inline bool IsVarying(const Track &track) {
    if (!IsRegistered(track)) return false;
    return IsVariedInRange(track, static_cast<double>(track.keys.front().frame),
                           static_cast<double>(track.keys.back().frame));
}

}  // namespace mikumikustudio::curve_comparison
