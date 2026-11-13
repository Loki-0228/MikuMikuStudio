#pragma once

// Isolated, allocation-free state machine. It owns neither the application nor
// the transaction. Callers must keep both alive until FinishDetach succeeds.
// Calls must be serialized on the UI thread; WrongThread checks do not make
// concurrent access safe. AbortPreview does not undo any caller writes.
#include <cstdint>
#include <cstring>
#include <limits>

namespace mikumikustudio::plugins::background_gesture_core {
enum class Result { Ok, Invalid, WrongThread, Busy, Stale, Exhausted };
enum class Phase { Idle, Active, Preview, Detached };
struct Values { std::int32_t x, y; float scale; };
struct Domain { std::uint64_t instance, media; };
struct Detached {
    std::uint64_t serial = 0;
    std::uintptr_t transaction = 0;
    Values restore{};
    unsigned restoreMask = 0; // 1: x, 2: y, 4: scale
};
inline bool Same(float a, float b) noexcept { return std::memcmp(&a, &b, sizeof(float)) == 0; }
inline bool Same(Values a, Values b) noexcept { return a.x == b.x && a.y == b.y && Same(a.scale, b.scale); }
inline bool Same(Domain a, Domain b) noexcept { return a.instance == b.instance && a.media == b.media; }

class Gesture final {
public:
    Result Begin(std::uint64_t thread, Domain domain, Values values, std::uintptr_t transaction) noexcept {
        if (!thread || !domain.instance || !domain.media || !transaction) return Result::Invalid;
        if (phase_ != Phase::Idle) return owner_ != thread ? Result::WrongThread : Result::Busy;
        if (serial_ == (std::numeric_limits<std::uint64_t>::max)()) return Result::Exhausted;
        ++serial_; owner_ = thread; domain_ = domain; before_ = after_ = values;
        transaction_ = transaction; phase_ = Phase::Active;
        return Result::Ok;
    }
    // The caller checks observed state, writes the sample without calling out,
    // then immediately supplies the actual result. No external callback is
    // permitted between this check and writing the sample.
    Result ReservePreview(std::uint64_t thread, Domain domain, Values observed) noexcept {
        if (phase_ != Phase::Idle && owner_ != thread) return Result::WrongThread;
        if (phase_ != Phase::Active) return phase_ == Phase::Idle ? Result::Stale : Result::Busy;
        if (!Same(domain_, domain) || !Same(after_, observed)) return Result::Stale;
        phase_ = Phase::Preview;
        return Result::Ok;
    }
    Result AcceptPreview(std::uint64_t thread, Domain domain, Values previous, Values next) noexcept {
        if (phase_ != Phase::Idle && owner_ != thread) return Result::WrongThread;
        if (phase_ != Phase::Preview) return phase_ == Phase::Idle ? Result::Stale : Result::Busy;
        if (!Same(domain_, domain) || !Same(after_, previous)) return Result::Stale;
        after_ = next;
        phase_ = Phase::Active;
        return Result::Ok;
    }
    Result AbortPreview(std::uint64_t thread) noexcept {
        if (phase_ != Phase::Idle && owner_ != thread) return Result::WrongThread;
        if (phase_ != Phase::Preview) return phase_ == Phase::Idle ? Result::Stale : Result::Busy;
        phase_ = Phase::Active;
        return Result::Ok;
    }
    // No writes, allocation, callbacks, handle destruction or OS calls here.
    // The caller must apply the returned field mask without an intervening
    // callback, only while the supplied domain remains current. A different
    // domain has no owned fields, even if its values compare equal.
    Result Detach(std::uint64_t thread, Domain domain, Values observed, bool cancel, Detached& out) noexcept {
        if (phase_ != Phase::Idle && owner_ != thread) return Result::WrongThread;
        if (phase_ != Phase::Active) return phase_ == Phase::Idle ? Result::Stale : Result::Busy;
        Detached next{}; next.serial = serial_; next.transaction = transaction_; next.restore = before_;
        if (cancel && Same(domain_, domain)) {
            if (before_.x != after_.x && observed.x == after_.x) next.restoreMask |= 1;
            if (before_.y != after_.y && observed.y == after_.y) next.restoreMask |= 2;
            if (!Same(before_.scale, after_.scale) && Same(observed.scale, after_.scale)) next.restoreMask |= 4;
        }
        transaction_ = 0; phase_ = Phase::Detached; out = next;
        return Result::Ok;
    }
    // Invoke only after the caller has applied or declined the returned
    // rollback and released the detached transaction and capture. A stale
    // cleanup receipt cannot unlock a later gesture.
    Result FinishDetach(std::uint64_t thread, std::uint64_t serial) noexcept {
        if (phase_ != Phase::Idle && owner_ != thread) return Result::WrongThread;
        if (phase_ != Phase::Detached) return phase_ == Phase::Idle ? Result::Stale : Result::Busy;
        if (!serial || serial != serial_) return Result::Stale;
        phase_ = Phase::Idle; owner_ = 0; return Result::Ok;
    }
    Phase phase() const noexcept { return phase_; }
private:
    Phase phase_ = Phase::Idle;
    std::uint64_t owner_ = 0, serial_ = 0;
    Domain domain_{};
    Values before_{}, after_{};
    std::uintptr_t transaction_ = 0;
};
}
