#pragma once

#include <cmath>
#include <cstdint>

namespace mikumikustudio::recording_state {

inline bool ValidFps(float fps) noexcept {
    return std::isfinite(fps) && fps >= .001f && fps <= 240.f;
}

inline std::int64_t FrameDuration(float fps) noexcept {
    return ValidFps(fps) ? static_cast<std::int64_t>(std::llround(10000000.0 / double(fps))) : 0;
}

inline std::uint64_t OutputFrameCount(std::int32_t first, std::int32_t last, float fps,
                                     int playbackFps = 30) noexcept {
    if (last < first || !ValidFps(fps) || playbackFps <= 0) return 0;
    const auto frames = std::int64_t(last) - first + 1;
    return static_cast<std::uint64_t>(std::ceil(double(frames) * double(fps) / playbackFps));
}

enum class Operation { Finish, GraphTeardown, Start, GraphBuild };

// UI-thread scopes, with no allocation or changes to the original recorder ABI.
// A modal message loop may enter another application's cleanup, but cannot
// recursively stop the same recording or consume its completion event twice.
class Guard {
    inline static thread_local Guard* active_ = nullptr;
    Guard* previous_ = nullptr;
    const void* identity_;
    Operation operation_;
    bool entered_ = false;
    std::uintptr_t deferredClose_ = 0;
    bool deferredFinish_ = false;
public:
    // A graph builder may own borrowed COM pointers across a message callback.
    // Finish must wait until the builder returns before releasing those objects.
    static bool DeferFinish(const void* identity) noexcept {
        for (auto* item = active_; item; item = item->previous_)
            if (item->identity_ == identity && item->operation_ == Operation::GraphBuild) {
                item->deferredFinish_ = true;
                return true;
            }
        return false;
    }
    static bool Active(const void* identity, Operation operation) noexcept {
        for (auto* item = active_; item; item = item->previous_)
            if (item->identity_ == identity && item->operation_ == operation) return true;
        return false;
    }
    static bool DeferClose(const void* identity, Operation operation, std::uintptr_t window) noexcept {
        for (auto* item = active_; item; item = item->previous_)
            if (item->identity_ == identity && item->operation_ == operation) {
                item->deferredClose_ = window;
                return true;
            }
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
    std::uintptr_t DeferredClose() const noexcept { return deferredClose_; }
    bool DeferredFinish() const noexcept { return deferredFinish_; }
};

inline bool Stopping(const void* app) noexcept { return Guard::Active(app, Operation::Finish); }

} // namespace mikumikustudio::recording_state
