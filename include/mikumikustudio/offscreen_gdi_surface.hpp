#pragma once
#include "mikumikustudio/offscreen_glyph_dc.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace mikumikustudio {
struct OffscreenSurfaceOps {
    void* context = nullptr;
    std::uintptr_t (*createDc)(void*, std::uintptr_t) noexcept = nullptr;
    std::uintptr_t (*createBitmap)(void*, std::uintptr_t, int, int, void**) noexcept = nullptr;
    bool (*selectBitmap)(void*, std::uintptr_t, std::uintptr_t) noexcept = nullptr;
    bool (*deleteBitmap)(void*, std::uintptr_t) noexcept = nullptr;
    OffscreenGlyphOps glyph{};
};
enum class OffscreenSurfaceInit { Rejected, Ready, FailedOwned };

// This object exclusively owns all adopted handles. The caller must retain it
// until Close succeeds. A failed operation leaves remaining handles available
// for retry. A borrowed window DC is used only as a creation reference.
class OffscreenGdiSurface final {
    OffscreenSurfaceOps ops_{};
    OffscreenGlyphDc glyph_;
    std::uintptr_t pendingDc_ = 0, bitmap_ = 0;
    void* pixels_ = nullptr;
    int width_ = 0, height_ = 0;
    bool ready_ = false;
public:
    explicit OffscreenGdiSurface(OffscreenSurfaceOps ops) noexcept : ops_(ops), glyph_(ops.glyph) {}
    OffscreenGdiSurface(const OffscreenGdiSurface&) = delete;
    OffscreenGdiSurface& operator=(const OffscreenGdiSurface&) = delete;
    OffscreenGdiSurface(OffscreenGdiSurface&&) = delete;
    OffscreenGdiSurface& operator=(OffscreenGdiSurface&&) = delete;
    OffscreenSurfaceInit Initialize(std::uintptr_t reference, int width, int height) noexcept {
        if (!reference || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > 64000000 ||
            pendingDc_ || glyph_.dc() || glyph_.font() || bitmap_ || !ops_.createDc || !ops_.createBitmap ||
            !ops_.selectBitmap || !ops_.deleteBitmap || !ops_.glyph.selectFont ||
            !ops_.glyph.restoreFont || !ops_.glyph.deleteDc || !ops_.glyph.deleteFont)
            return OffscreenSurfaceInit::Rejected;
        pendingDc_ = ops_.createDc(ops_.context, reference);
        if (!pendingDc_) return OffscreenSurfaceInit::Rejected;
        if (!glyph_.AdoptDc(pendingDc_)) return OffscreenSurfaceInit::FailedOwned;
        const auto dc = pendingDc_; pendingDc_ = 0;
        bitmap_ = ops_.createBitmap(ops_.context, reference, width, height, &pixels_);
        if (!bitmap_ || !pixels_ || !ops_.selectBitmap(ops_.context, dc, bitmap_)) {
            pixels_ = nullptr;
            return OffscreenSurfaceInit::FailedOwned;
        }
        width_ = width; height_ = height; ready_ = true;
        return OffscreenSurfaceInit::Ready;
    }
    OffscreenFontSelection SelectOwnedFont(std::uintptr_t font) noexcept {
        if (!ready_) return OffscreenFontSelection::Rejected;
        const auto result = glyph_.SelectOwnedFont(font);
        if (result == OffscreenFontSelection::FailedOwned) ready_ = false;
        return result;
    }
    bool RestoreFont() noexcept {
        if (!ready_) return false;
        if (!glyph_.RestoreFont()) { ready_ = false; return false; }
        return true;
    }
    bool Close() noexcept {
        ready_ = false; pixels_ = nullptr; width_ = 0; height_ = 0;
        if (pendingDc_) {
            if (!ops_.glyph.deleteDc(ops_.glyph.context, pendingDc_)) return false;
            pendingDc_ = 0;
        }
        if (!glyph_.Close()) return false;
        if (bitmap_) {
            if (!ops_.deleteBitmap(ops_.context, bitmap_)) return false;
            bitmap_ = 0;
        }
        return true;
    }
    bool ready() const noexcept { return ready_ && glyph_.ready(); }
    std::uintptr_t dc() const noexcept { return glyph_.dc() ? glyph_.dc() : pendingDc_; }
    std::uintptr_t bitmap() const noexcept { return bitmap_; }
    std::uintptr_t font() const noexcept { return glyph_.font(); }
    void* pixels() const noexcept { return ready() ? pixels_ : nullptr; }
    int width() const noexcept { return ready() ? width_ : 0; }
    int height() const noexcept { return ready() ? height_ : 0; }
};

struct OffscreenSurfaceHandle { std::uint32_t index = 0; std::uint64_t generation = 0; };
struct OffscreenSurfaceView {
    std::uintptr_t dc = 0, bitmap = 0, font = 0;
    void* pixels = nullptr;
    bool ready = false;
    int width = 0, height = 0;
};

// Reserve a fixed slot before acquiring native handles. Every operation checks
// the generation; a slot whose generation is exhausted is never reused.
// A view contains borrowed native handles/pixels, valid only while that exact
// generation remains live and until the next operation on it. Callers must not
// retain or operate on those borrowed values after an operation or Release.
// The pool itself must outlive every occupied slot and every failed Close.
template<std::size_t N, std::uint64_t MaxGeneration = (std::numeric_limits<std::uint64_t>::max)()>
class OffscreenGdiSurfacePool final {
    static_assert(N > 0 && N <= 16, "bounded off-screen surface count");
    static_assert(MaxGeneration > 0, "generation must be nonzero");
    struct Slot { std::optional<OffscreenGdiSurface> value; std::uint64_t generation=0; };
    std::array<Slot, N> slots_{};
    OffscreenGdiSurface* Lookup(OffscreenSurfaceHandle handle) noexcept {
        if (!handle.generation || handle.index >= N) return nullptr;
        auto& slot=slots_[handle.index];
        return slot.value && slot.generation==handle.generation ? &*slot.value : nullptr;
    }
public:
    OffscreenGdiSurfacePool() = default;
    OffscreenGdiSurfacePool(const OffscreenGdiSurfacePool&) = delete;
    OffscreenGdiSurfacePool& operator=(const OffscreenGdiSurfacePool&) = delete;
    OffscreenSurfaceHandle Reserve(OffscreenSurfaceOps ops) noexcept {
        for (std::uint32_t i=0; i<N; ++i) {
            auto& slot=slots_[i];
            if (!slot.value && slot.generation<MaxGeneration) {
                ++slot.generation; slot.value.emplace(ops);
                return {i,slot.generation};
            }
        }
        return {};
    }
    OffscreenSurfaceInit Initialize(OffscreenSurfaceHandle handle, std::uintptr_t reference, int width, int height) noexcept {
        auto* value=Lookup(handle); return value ? value->Initialize(reference,width,height) : OffscreenSurfaceInit::Rejected;
    }
    OffscreenFontSelection SelectOwnedFont(OffscreenSurfaceHandle handle, std::uintptr_t font) noexcept {
        auto* value=Lookup(handle); return value ? value->SelectOwnedFont(font) : OffscreenFontSelection::Rejected;
    }
    bool RestoreFont(OffscreenSurfaceHandle handle) noexcept {
        auto* value=Lookup(handle); return value && value->RestoreFont();
    }
    bool Read(OffscreenSurfaceHandle handle, OffscreenSurfaceView& out) noexcept {
        auto* value=Lookup(handle); if (!value) return false;
        out={value->dc(),value->bitmap(),value->font(),value->pixels(),value->ready(),value->width(),value->height()}; return true;
    }
    bool Release(OffscreenSurfaceHandle handle) noexcept {
        auto* value=Lookup(handle); if (!value || !value->Close()) return false;
        slots_[handle.index].value.reset(); return true;
    }
    std::size_t occupied() const noexcept {
        std::size_t count = 0; for (const auto& slot : slots_) if (slot.value) ++count; return count;
    }
};

// Implemented by the Win32 adapter; no borrowed reference is retained.
OffscreenSurfaceOps Win32OffscreenSurfaceOps() noexcept;
}
