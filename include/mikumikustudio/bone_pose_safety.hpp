#pragma once
#include <cmath>
#include <cstring>
#include <limits>

namespace mikumikustudio {
// PMM may contain the NaN pose produced by an earlier physics calculation.
// Preserve finite translations, including very small scientific-notation values.
// A damaged rotation has no recoverable direction; use the supplied live pose
// or the rest rotation. Well-scaled quaternions retain their exact float bits.
inline bool RepairBonePose(float position[3], float rotation[4],
                           const float* fallbackPosition = nullptr,
                           const float* fallbackRotation = nullptr) {
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(position[i])) {
            position[i] = fallbackPosition && std::isfinite(fallbackPosition[i])
                ? fallbackPosition[i] : 0.0f;
            changed = true;
        }
    }
    auto norm = [](const float* q) {
        double sum = 0.0;
        for (int i = 0; i < 4; ++i) sum += double(q[i]) * q[i];
        return sum;
    };
    double length2 = norm(rotation);
    if (!std::isfinite(length2) || length2 == 0.0) {
        const double fallbackLength2 = fallbackRotation ? norm(fallbackRotation) : 0.0;
        if (std::isfinite(fallbackLength2) && fallbackLength2 > 0.0) {
            std::memcpy(rotation, fallbackRotation, 4 * sizeof(float));
            length2 = fallbackLength2;
        } else {
            rotation[0] = rotation[1] = rotation[2] = 0.0f;
            rotation[3] = 1.0f;
            length2 = 1.0;
        }
        changed = true;
    }
    // Compute the norm in double so finite subnormal/large quaternions do not
    // underflow/overflow during normalization.
    if (length2 < (std::numeric_limits<float>::min)() ||
        length2 > (std::numeric_limits<float>::max)()) {
        const double scale = 1.0 / std::sqrt(length2);
        for (int i = 0; i < 4; ++i) rotation[i] = float(double(rotation[i]) * scale);
        changed = true;
    }
    return changed;
}
}  // namespace mikumikustudio
