#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace mikumikustudio::negative_sampling {

// Sample one negative-frame track out of a read-only key range.
//
// Keys is a range whose elements expose "frame" and "values" (a Values).
// Predicate selects the keys of one track and receives each key. The range may
// arrive in any order; the caller guarantees unique negative key frames, a
// negative requested frame, and finite values. The caller also supplies the
// track's independent zero values, pinned at frame 0.
//
// The scan keeps the earliest key, the nearest key at or before frame, and the
// nearest key after it. It allocates nothing, sorts nothing, and touches no
// application or global state. Only Values copies for the returned sample are
// made.
//
// Rules:
//   * no key matches: zero
//   * frame before the earliest key: earliest key values
//   * frame on a key: that key's values
//   * interpolate == false: nearest key at or before frame (step)
//   * interpolate == true, key after frame: linear between both keys
//   * interpolate == true, past the last key: linear from that key to zero,
//     with zero pinned at frame 0
//
// Frame differences are int64, so neighbouring int extremes cannot overflow.
//
// quaternion == true restricts the shortest-arc nlerp to values[3..6]: when the
// raw dot product is negative, the later quaternion is negated before the lerp.
// The lerped quaternion is normalized only while its squared norm stays above
// 1e-20; at or below that threshold the unnormalized values are returned,
// matching negative_frames.cpp EvaluateModel. All other components stay linear.
template <class Keys, class Values, class Predicate>
Values Sample(const Keys& keys, int frame, const Values& zero, bool interpolate,
              bool quaternion, Predicate matches) {
    const Values* earliest = nullptr;
    const Values* previous = nullptr;
    const Values* next = nullptr;
    int earliest_frame = 0;
    int previous_frame = 0;
    int next_frame = 0;

    for (const auto& key : keys) {
        if (!matches(key)) continue;
        const int key_frame = key.frame;
        if (!earliest || key_frame < earliest_frame) {
            earliest = &key.values;
            earliest_frame = key_frame;
        }
        if (key_frame <= frame) {
            if (!previous || key_frame > previous_frame) {
                previous = &key.values;
                previous_frame = key_frame;
            }
        } else if (!next || key_frame < next_frame) {
            next = &key.values;
            next_frame = key_frame;
        }
    }

    if (!earliest) return zero;
    if (frame < earliest_frame) return *earliest;
    // frame >= earliest_frame guarantees that at least the earliest key is at
    // or before frame, so previous is set.
    if (frame == previous_frame) return *previous;
    if (!interpolate) return *previous;

    const Values& end = next ? *next : zero;
    const int end_frame = next ? next_frame : 0;
    const std::int64_t span = std::int64_t(end_frame) - std::int64_t(previous_frame);
    const std::int64_t offset = std::int64_t(frame) - std::int64_t(previous_frame);
    const double t = double(offset) / double(span);

    Values out = *previous;
    const std::size_t count = out.size();
    for (std::size_t i = 0; i < count; ++i) out[i] = out[i] + t * (end[i] - out[i]);
    if (quaternion) {
        double dot = 0;
        for (int i = 3; i <= 6; ++i) dot += (*previous)[i] * end[i];
        const double sign = dot < 0 ? -1.0 : 1.0;
        double norm = 0;
        for (int i = 3; i <= 6; ++i) {
            out[i] = (1.0 - t) * (*previous)[i] + t * (sign * end[i]);
            norm += out[i] * out[i];
        }
        if (norm > 1e-20) {
            const double length = std::sqrt(norm);
            for (int i = 3; i <= 6; ++i) out[i] = out[i] / length;
        }
    }
    return out;
}

}
