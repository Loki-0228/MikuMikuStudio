#pragma once
#include "mikumikustudio/frame_limits.hpp"
#include <cmath>
#include <cstdint>
#include <numeric>

namespace mikumikustudio {
// A validated nonnegative sample time. Continuous interpolation may round to
// the next integer, so discrete tracks must use DiscreteFrame(), never floor
// Frame(). No ABI types or OS/device dependencies are needed by this helper.
class EvaluationTime final {
    double frame_ = 0;
    std::uint32_t discrete_ = 0;
public:
    EvaluationTime() = default; // frame zero
    double Frame() const noexcept { return frame_; }
    std::uint32_t DiscreteFrame() const noexcept { return discrete_; }
    static bool FromFrame(double frame, EvaluationTime& out) noexcept {
        if (!std::isfinite(frame) || frame < 0 || frame > frame_limits::maximum) return false;
        EvaluationTime value;
        value.frame_ = frame;
        value.discrete_ = static_cast<std::uint32_t>(frame);
        out = value;
        return true;
    }
    // Seconds at the native 30 fps rate. Integer long division avoids overflow
    // in numerator*30 and preserves the exact side of every registered key.
    // Rejected inputs leave out unchanged, including fractions above maximum.
    static bool FromSeconds(std::int64_t numerator, std::uint64_t denominator,
                            EvaluationTime& out) noexcept {
        if (numerator < 0 || !denominator) return false;
        const auto n = static_cast<std::uint64_t>(numerator);
        const auto whole = n / denominator;
        const auto remainder = n % denominator;
        constexpr auto maximum = static_cast<std::uint64_t>(frame_limits::maximum);
        if (whole > maximum / 30) return false;
        std::uint64_t discrete = whole * 30, carried = 0;
        for (unsigned i = 0; i < 30; ++i) {
            if (remainder >= denominator - carried) {
                carried -= denominator - remainder;
                ++discrete;
            } else carried += remainder;
        }
        if (discrete > maximum || (discrete == maximum && carried)) return false;
        EvaluationTime value;
        value.discrete_ = static_cast<std::uint32_t>(discrete);
        const auto divisor = std::gcd(carried, denominator);
        value.frame_ = static_cast<double>(discrete) +
            static_cast<double>(carried / divisor) / static_cast<double>(denominator / divisor);
        out = value;
        return true;
    }
};
}
