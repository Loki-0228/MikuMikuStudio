#pragma once

#include "mikumikustudio/offscreen_gdi_surface.hpp"
#include <array>
#include <cstdint>
#include <utility>

namespace mikumikustudio {
// The thread-token callback must identify the current UI thread for the
// owner's entire lifetime. Its context and every surface operation context
// must outlive the owner and all pending resources.
struct OffscreenOwnerThread {
    void* context = nullptr;
    std::uint64_t (*current)(void*) noexcept = nullptr;
};

// Single-threaded lifetime core. A process-lifetime holder must keep this
// object alive until every pending Close succeeds; this class does not install
// a window, timer, global owner, or shutdown hook. Reentrant operations are
// rejected while a native operation is active.
template<std::size_t N, std::uint64_t MaxGeneration = (std::numeric_limits<std::uint64_t>::max)()>
class OffscreenGdiOwner final {
    struct Entry {
        OffscreenSurfaceHandle handle{};
        bool pending = false;
        // Defensive ownership if an accepted font cannot be passed to the
        // pool. It has never been selected into the DC.
        std::uintptr_t orphanFont = 0;
        OffscreenGlyphOps glyphOps{};
    };
    OffscreenGdiSurfacePool<N, MaxGeneration> pool_;
    std::array<Entry, N> entries_{};
    OffscreenOwnerThread thread_{};
    std::uint64_t ownerThread_ = 0;
    bool busy_ = false, closing_ = false;

    bool OnThread() const noexcept {
        return thread_.current && ownerThread_ && thread_.current(thread_.context) == ownerThread_;
    }
    struct Work { bool& busy; explicit Work(bool& b) noexcept : busy(b) { busy = true; } ~Work() { busy = false; } };
    Entry* Find(OffscreenSurfaceHandle h) noexcept {
        if (!h.generation || h.index >= N) return nullptr;
        auto& e = entries_[h.index];
        return e.handle.generation == h.generation ? &e : nullptr;
    }
    bool CloseEntry(Entry& entry) noexcept {
        entry.pending = true;
        if (entry.orphanFont) {
            if (!entry.glyphOps.deleteFont || !entry.glyphOps.deleteFont(entry.glyphOps.context, entry.orphanFont)) return false;
            entry.orphanFont = 0;
        }
        if (!pool_.Release(entry.handle)) return false;
        entry = {}; return true;
    }
    OffscreenFontSelection CreateAndSelectLocked(Entry& entry, OffscreenSurfaceHandle h,
        std::uintptr_t (*create)(void*) noexcept, void* context) noexcept {
        OffscreenSurfaceView view{};
        if (!create || !pool_.Read(h, view) || !view.ready || view.font) return OffscreenFontSelection::Rejected;
        const auto font = create(context);
        if (!font) return OffscreenFontSelection::Rejected;
        const auto result = pool_.SelectOwnedFont(h, font);
        if (result == OffscreenFontSelection::Rejected) {
            entry.orphanFont = font; entry.pending = true;
            return OffscreenFontSelection::FailedOwned;
        }
        if (result == OffscreenFontSelection::FailedOwned) entry.pending = true;
        return result;
    }
    bool RestoreFontLocked(Entry& entry, OffscreenSurfaceHandle h) noexcept {
        OffscreenSurfaceView view{};
        if (!pool_.Read(h, view) || !view.ready || !view.font) return false;
        if (pool_.RestoreFont(h)) return true;
        entry.pending = true; return false;
    }
public:
    explicit OffscreenGdiOwner(OffscreenOwnerThread thread) noexcept : thread_(thread) {
        if (thread_.current) ownerThread_ = thread_.current(thread_.context);
    }
    OffscreenGdiOwner(const OffscreenGdiOwner&) = delete;
    OffscreenGdiOwner& operator=(const OffscreenGdiOwner&) = delete;
    OffscreenGdiOwner(OffscreenGdiOwner&&) = delete;
    OffscreenGdiOwner& operator=(OffscreenGdiOwner&&) = delete;

    // The session is a synchronous, trusted internal borrow. A successful
    // Read keeps dc, bitmap, pixels and dimensions stable through subsequent
    // successful session operations until the WithDraw callback returns. The
    // pixel contents may change during drawing. After any session failure or
    // Fail(), previously borrowed values must no longer be used. The font
    // field is valid only until the next font operation. Neither the session
    // nor any borrowed handle or pointer may escape WithDraw. Callbacks and
    // factories must not destroy the owner or independently mutate the DC.
    class DrawSession final {
        friend class OffscreenGdiOwner;
        OffscreenGdiOwner& owner_; Entry& entry_; OffscreenSurfaceHandle handle_;
        bool failed_ = false, operating_ = false;
        DrawSession(OffscreenGdiOwner& owner, Entry& entry, OffscreenSurfaceHandle h) noexcept
            : owner_(owner), entry_(entry), handle_(h) {}
        bool Ready() const noexcept { return owner_.OnThread() && owner_.busy_ && !owner_.closing_ && !entry_.pending && !failed_ && !operating_; }
    public:
        DrawSession(const DrawSession&) = delete;
        DrawSession& operator=(const DrawSession&) = delete;
        DrawSession(DrawSession&&) = delete;
        DrawSession& operator=(DrawSession&&) = delete;
        bool Read(OffscreenSurfaceView& out) noexcept {
            if (!Ready()) { failed_ = true; return false; }
            Work work(operating_);
            if (owner_.pool_.Read(handle_, out)) return true;
            failed_ = true; return false;
        }
        OffscreenFontSelection CreateAndSelect(std::uintptr_t (*create)(void*) noexcept, void* context) noexcept {
            if (!Ready()) { failed_ = true; return OffscreenFontSelection::Rejected; }
            Work work(operating_);
            const auto result = owner_.CreateAndSelectLocked(entry_, handle_, create, context);
            if (result != OffscreenFontSelection::Selected) failed_ = true;
            return result;
        }
        bool RestoreFont() noexcept {
            if (!Ready()) { failed_ = true; return false; }
            Work work(operating_);
            if (owner_.RestoreFontLocked(entry_, handle_)) return true;
            failed_ = true; return false;
        }
        // Call after any GDI, allocation, clipping, flush or pixel-read error.
        void Fail() noexcept { failed_ = true; }
    };

    OffscreenSurfaceHandle Reserve(OffscreenSurfaceOps ops) noexcept {
        if (!OnThread() || busy_ || closing_) return {};
        Work work(busy_); auto h = pool_.Reserve(ops);
        if (h.generation) entries_[h.index] = {h, false, 0, ops.glyph};
        return h;
    }
    OffscreenSurfaceInit Initialize(OffscreenSurfaceHandle h, std::uintptr_t reference, int width, int height) noexcept {
        if (!OnThread() || busy_ || closing_) return OffscreenSurfaceInit::Rejected;
        auto* e = Find(h); if (!e || e->pending) return OffscreenSurfaceInit::Rejected;
        Work work(busy_); const auto result = pool_.Initialize(h, reference, width, height);
        if (result == OffscreenSurfaceInit::FailedOwned) e->pending = true;
        return result;
    }
    OffscreenFontSelection SelectOwnedFont(OffscreenSurfaceHandle h, std::uintptr_t font) noexcept {
        if (!OnThread() || busy_ || closing_) return OffscreenFontSelection::Rejected;
        auto* e = Find(h); if (!e || e->pending) return OffscreenFontSelection::Rejected;
        Work work(busy_); const auto result = pool_.SelectOwnedFont(h, font);
        if (result == OffscreenFontSelection::FailedOwned) e->pending = true;
        return result;
    }
    // The factory returns a newly owned font, or zero without transferring
    // ownership. Once nonzero is returned, the owner is responsible even if
    // selection fails. The factory and its context must not destroy this
    // owner or independently manipulate the reserved surface.
    OffscreenFontSelection CreateAndSelect(OffscreenSurfaceHandle h,
        std::uintptr_t (*create)(void*) noexcept, void* context) noexcept {
        if (!OnThread() || busy_ || closing_ || !create) return OffscreenFontSelection::Rejected;
        auto* e = Find(h); if (!e || e->pending) return OffscreenFontSelection::Rejected;
        Work work(busy_); return CreateAndSelectLocked(*e, h, create, context);
    }
    bool RestoreFont(OffscreenSurfaceHandle h) noexcept {
        if (!OnThread() || busy_ || closing_) return false;
        auto* e = Find(h); if (!e || e->pending) return false;
        Work work(busy_); return RestoreFontLocked(*e, h);
    }
    // Callback must stage any externally visible result and publish it only
    // after true is returned. An exception, failed session operation, false
    // callback result, or outstanding selected font retires the surface.
    template<class Draw> bool WithDraw(OffscreenSurfaceHandle h, Draw&& draw) noexcept {
        if (!OnThread() || busy_ || closing_) return false;
        auto* e = Find(h); if (!e || e->pending) return false;
        Work work(busy_); OffscreenSurfaceView before{};
        if (!pool_.Read(h, before) || !before.ready || before.font) return false;
        DrawSession session(*this, *e, h); bool drawn = false;
        try { drawn = static_cast<bool>(std::forward<Draw>(draw)(session)); }
        catch (...) { session.failed_ = true; }
        OffscreenSurfaceView after{};
        if (!drawn || session.failed_ || e->pending || !pool_.Read(h, after) || !after.ready || after.font) {
            e->pending = true; return false;
        }
        return true;
    }
    // A successful view is borrowed only until the next operation for this
    // handle. Failed reads leave the caller's output unchanged.
    bool Read(OffscreenSurfaceHandle h, OffscreenSurfaceView& out) noexcept {
        if (!OnThread() || busy_ || closing_) return false;
        auto* e = Find(h); if (!e || e->pending) return false;
        Work work(busy_); return pool_.Read(h, out);
    }
    bool Release(OffscreenSurfaceHandle h) noexcept {
        if (!OnThread() || busy_) return false;
        auto* e = Find(h); if (!e) return false;
        Work work(busy_); return CloseEntry(*e);
    }
    // Retry only retired entries. A false return means at least one resource
    // remains owned and requires a later call on the same thread.
    bool Retry() noexcept {
        if (!OnThread() || busy_) return false;
        Work work(busy_); bool complete = true;
        for (auto& e : entries_) if (e.handle.generation && e.pending && !CloseEntry(e)) complete = false;
        return complete;
    }
    // Permanently disallow admission before attempting any cleanup.
    bool Shutdown() noexcept {
        if (!OnThread() || busy_) return false;
        Work work(busy_); closing_ = true; bool complete = true;
        for (auto& e : entries_) if (e.handle.generation && !CloseEntry(e)) complete = false;
        return complete;
    }
    bool Counts(std::size_t& occupied, std::size_t& pending, bool& closing) const noexcept {
        if (!OnThread() || busy_) return false;
        std::size_t count = 0, retired = 0;
        for (const auto& e : entries_) if (e.handle.generation) { ++count; if (e.pending) ++retired; }
        occupied = count; pending = retired; closing = closing_; return true;
    }
};
}
