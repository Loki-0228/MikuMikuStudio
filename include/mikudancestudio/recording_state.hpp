#pragma once

#include <cmath>
#include <cstdint>

namespace mikudancestudio::recording_state {

inline bool ValidFps(float fps) noexcept {
    return std::isfinite(fps) && fps >= .001f && fps <= 240.f;
}

inline std::int64_t FrameDuration(float fps) noexcept {
    return ValidFps(fps) ? static_cast<std::int64_t>(std::llround(10000000.0 / double(fps))) : 0;
}

inline std::uint64_t OutputFrameCount(std::int32_t first, std::int32_t last, float fps) noexcept {
    if (last < first || !ValidFps(fps)) return 0;
    const auto frames = std::int64_t(last) - first + 1;
    return static_cast<std::uint64_t>(std::ceil(double(frames) * double(fps) / 30.0));
}

enum class Operation { Finish, GraphTeardown };

// UI-thread scopes, with no allocation or changes to the original recorder ABI.
// A modal message loop may enter another application's cleanup, but cannot
// recursively stop the same recording or consume its completion event twice.
class Guard {
    inline static thread_local Guard* active_ = nullptr;
    Guard* previous_ = nullptr;
    const void* identity_;
    Operation operation_;
    bool entered_ = false;
public:
    static bool Active(const void* identity, Operation operation) noexcept {
        for (auto* item = active_; item; item = item->previous_)
            if (item->identity_ == identity && item->operation_ == operation) return true;
        return false;
    }
    Guard(const void* identity, Operation operation) noexcept
        : identity_(identity), operation_(operation) {
        if (identity && !Active(identity, operation)) {
            entered_ = true; previous_ = active_; active_ = this;
        }
    }
    ~Guard() { if (entered_) active_ = previous_; }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    explicit operator bool() const noexcept { return entered_; }
};

inline bool Stopping(const void* app) noexcept { return Guard::Active(app, Operation::Finish); }

} // namespace mikudancestudio::recording_state
