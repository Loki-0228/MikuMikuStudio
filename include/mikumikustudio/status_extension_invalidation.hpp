#pragma once

#include <cstdint>
#include <limits>

namespace mikumikustudio {
// A UI-thread-only coordinator for one presenter lifetime. It does not own a
// window or call a plugin. A caller must synchronously hide and suspend the
// existing status presenter before it permits a changed title to be painted.
// Invalidate returns false when that could not be guaranteed, including during
// reentry; the caller must defer painting in that case.
class StatusExtensionInvalidation final {
    std::uint64_t revision_ = 0;
    bool dirty_ = false, hidden_ = true, queued_ = false, busy_ = false, closing_ = false;
    struct Work {
        bool& flag;
        explicit Work(bool& f) noexcept : flag(f) { flag = true; }
        ~Work() { flag = false; }
    };
public:
    StatusExtensionInvalidation() = default;
    StatusExtensionInvalidation(const StatusExtensionInvalidation&) = delete;
    StatusExtensionInvalidation& operator=(const StatusExtensionInvalidation&) = delete;

    // hide() must synchronously suspend the existing geometry and window. A
    // failed hide never authorizes painting or publishing a new geometry.
    template<class Hide> bool Invalidate(Hide&& hide) noexcept {
        if (closing_) return false;
        if (revision_ == (std::numeric_limits<std::uint64_t>::max)()) {
            closing_ = true; dirty_ = true; hidden_ = false; return false;
        }
        ++revision_; dirty_ = true; hidden_ = false;
        if (busy_) return false;
        Work work(busy_);
        try { hidden_ = static_cast<bool>(hide()); } catch (...) { hidden_ = false; }
        return hidden_;
    }

    // PostMessage is asynchronous. On failure queued remains false so the
    // caller's existing safe-point/timer mechanism can retry without spinning.
    template<class Post> bool TryPost(Post&& post) noexcept {
        if (closing_ || busy_ || !dirty_) return false;
        if (queued_) return true;
        Work work(busy_); queued_ = true;
        bool sent = false;
        try { sent = static_cast<bool>(post()); } catch (...) {}
        if (!sent) queued_ = false;
        return sent;
    }

    // Consume a delivered message even when the UI is not at a safe point.
    // The caller must retry scheduling if dirty remains set.
    void MessageDelivered() noexcept { queued_ = false; }

    // prepare() must stage measurements and layout without exposing them.
    // publish() may only expose that staged result. If either callback causes
    // reentry, Invalidate returns false to the nested caller; that caller must
    // defer painting. A stale or failed publication is hidden again before
    // this function returns. Neither callback may destroy this coordinator.
    template<class Prepare, class Publish, class Hide>
    bool Recalculate(bool safe, Prepare&& prepare, Publish&& publish, Hide&& hide) noexcept {
        if (closing_ || busy_ || !safe || !dirty_ || !hidden_) return false;
        Work work(busy_); const auto version = revision_;
        bool ready = false, published = false;
        try { ready = static_cast<bool>(prepare()); } catch (...) {}
        if (ready && revision_ == version && !closing_) {
            try { published = static_cast<bool>(publish()); } catch (...) {}
        }
        if (published && revision_ == version && !closing_) {
            dirty_ = false; hidden_ = false; return true;
        }
        dirty_ = true;
        try { hidden_ = static_cast<bool>(hide()); } catch (...) { hidden_ = false; }
        return false;
    }

    // Caller hides the native presenter before invoking Close. No new work is
    // admitted afterward; stale posted messages are harmless.
    bool Close() noexcept {
        if (busy_ || !hidden_) return false;
        closing_ = true; dirty_ = false; queued_ = false; return true;
    }
    bool dirty() const noexcept { return dirty_; }
    bool hidden() const noexcept { return hidden_; }
    bool queued() const noexcept { return queued_; }
    bool closing() const noexcept { return closing_; }
    std::uint64_t revision() const noexcept { return revision_; }
};
}
