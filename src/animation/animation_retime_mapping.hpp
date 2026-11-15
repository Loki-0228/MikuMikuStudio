#pragma once

#include <cstdint>

namespace mikumikustudio::animation_retime::detail {

// Whole rates use a reduced fraction. The product fits exactly even in a
// double (int32 frame times a denominator of at most 1,000,000), so evaluating
// the fraction directly only rounds once, unlike dividing by a rounded ratio.
template <typename Float>
Float InverseWholeRateFrame(std::int32_t frame, std::int64_t numerator, std::int64_t denominator) {
    return static_cast<Float>(frame * denominator) / static_cast<Float>(numerator);
}

}  // namespace mikumikustudio::animation_retime::detail
