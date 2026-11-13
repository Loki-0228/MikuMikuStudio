#pragma once

#include "mikumikustudio/offscreen_gdi_owner.hpp"
#include <mutex>
#include <optional>

namespace mikumikustudio {
struct OffscreenRuntimeHandle {
    std::uint64_t epoch = 0;
    OffscreenSurfaceHandle surface{};
};

// The acquired native thread handle is intentionally retained for the process
// lifetime. Unlike an ID alone, a live kernel handle distinguishes a stopped
// thread from a later thread that happens to receive the same numeric ID.
struct OffscreenRuntimeThreadOps {
    void* context = nullptr;
    std::uint64_t (*current)(void*) noexcept = nullptr;
    std::uintptr_t (*acquireCurrent)(void*) noexcept = nullptr;
    bool (*isAlive)(void*, std::uintptr_t) noexcept = nullptr;
};
class OffscreenRuntimeThreadBinding final {
    OffscreenRuntimeThreadOps ops_{};
    std::mutex mutex_;
    std::uint64_t id_ = 0;
    std::uintptr_t handle_ = 0;
public:
    explicit OffscreenRuntimeThreadBinding(OffscreenRuntimeThreadOps ops) noexcept : ops_(ops) {}
    OffscreenRuntimeThreadBinding(const OffscreenRuntimeThreadBinding&) = delete;
    OffscreenRuntimeThreadBinding& operator=(const OffscreenRuntimeThreadBinding&) = delete;
    std::uint64_t Current() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!ops_.current || !ops_.acquireCurrent || !ops_.isAlive) return 0;
            const auto current = ops_.current(ops_.context);
            if (!current) return 0;
            if (!handle_) {
                const auto acquired = ops_.acquireCurrent(ops_.context);
                if (!acquired) return 0;
                handle_ = acquired; id_ = current;
            }
            return current == id_ && ops_.isAlive(ops_.context, handle_) ? current : 0;
        } catch (...) { return 0; }
    }
    OffscreenOwnerThread Thread() noexcept {
        return {this, [](void* p) noexcept { return static_cast<OffscreenRuntimeThreadBinding*>(p)->Current(); }};
    }
};

// UI-thread lifetime core. Every method, including read-only queries, must be
// called on that thread; concurrent callers are not supported. Background work
// must post requests to the UI queue. The enclosing holder and operation
// contexts must outlive every resource, including failed Close operations.
// RetryAtSafePoint is explicit: callers schedule it outside plugin callbacks,
// drawing and layout. No window, timer or thread is created here.
template<std::size_t N, std::uint64_t MaxEpoch = (std::numeric_limits<std::uint64_t>::max)()>
class OffscreenGdiRuntimeCore final {
    static_assert(MaxEpoch > 0, "epoch must be nonzero");
    using Owner = OffscreenGdiOwner<N>;
    OffscreenOwnerThread thread_{};
    std::optional<Owner> owner_;
    std::uint64_t threadId_ = 0, epoch_ = 0;
    bool closing_ = false;

    bool OnThread() const noexcept {
        return thread_.current && threadId_ && thread_.current(thread_.context) == threadId_;
    }
    bool Valid(OffscreenRuntimeHandle h) const noexcept {
        return owner_ && OnThread() && h.epoch == epoch_ && h.epoch && h.surface.generation;
    }
public:
    explicit OffscreenGdiRuntimeCore(OffscreenOwnerThread thread) noexcept : thread_(thread) {}
    OffscreenGdiRuntimeCore(const OffscreenGdiRuntimeCore&) = delete;
    OffscreenGdiRuntimeCore& operator=(const OffscreenGdiRuntimeCore&) = delete;
    // Reopen only after shutdown has removed every resource. The epoch makes
    // handles from an earlier owner invalid even when slot generations repeat.
    bool Start() noexcept {
        if (!thread_.current) return false;
        const auto current = thread_.current(thread_.context);
        if (!current || (threadId_ && current != threadId_)) return false;
        if (owner_) {
            std::size_t occupied = 0, pending = 0; bool closing = false;
            if (!owner_->Counts(occupied, pending, closing)) return false;
            if (!closing_) return !closing;
            if (!closing || occupied || pending) return false;
        }
        if (epoch_ == MaxEpoch) return false;
        owner_.emplace(thread_);
        threadId_ = current; ++epoch_; closing_ = false;
        return true;
    }
    OffscreenRuntimeHandle Reserve(OffscreenSurfaceOps ops) noexcept {
        if (!owner_ || !OnThread() || closing_) return {};
        const auto h = owner_->Reserve(ops);
        return h.generation ? OffscreenRuntimeHandle{epoch_, h} : OffscreenRuntimeHandle{};
    }
    OffscreenSurfaceInit Initialize(OffscreenRuntimeHandle h, std::uintptr_t reference, int w, int hgt) noexcept {
        if (!Valid(h) || closing_) return OffscreenSurfaceInit::Rejected;
        return owner_->Initialize(h.surface, reference, w, hgt);
    }
    template<class Draw> bool WithDraw(OffscreenRuntimeHandle h, Draw&& draw) noexcept {
        if (!Valid(h) || closing_) return false;
        return owner_->WithDraw(h.surface, std::forward<Draw>(draw));
    }
    bool Release(OffscreenRuntimeHandle h) noexcept {
        return Valid(h) && owner_->Release(h.surface);
    }
    bool RetryAtSafePoint() noexcept {
        return owner_ && OnThread() && owner_->Retry();
    }
    bool ShutdownAtSafePoint() noexcept {
        if (!owner_ || !OnThread()) return false;
        // Counts rejects reentrant calls while a draw/native operation is in
        // progress. Do not change runtime state on that path.
        std::size_t occupied = 0, pending = 0; bool closing = false;
        if (!owner_->Counts(occupied, pending, closing)) return false;
        closing_ = true;
        return owner_->Shutdown();
    }
    bool Counts(std::size_t& occupied, std::size_t& pending, bool& closing) const noexcept {
        if (!owner_ || !OnThread()) return false;
        return owner_->Counts(occupied, pending, closing);
    }
};

// Lazy process-lifetime holder. It has no destructor and its Win32 operation
// callbacks have no external context. The caller must call Start on the UI
// thread and schedule retries at safe points; merely obtaining the pointer
// does not create a surface or install an automatic retry mechanism.
OffscreenGdiRuntimeCore<16>* ProcessOffscreenGdiRuntime() noexcept;
OffscreenSurfaceOps ProcessOffscreenGdiOps() noexcept;
}
