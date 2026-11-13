#pragma once
#include <cstdint>

namespace mikumikustudio {
// Narrow ownership state for a dedicated off-screen DC and one temporary font.
// Callers supply native operations; handles are opaque and zero is invalid.
// This type makes no calls on a borrowed window DC and does not draw text.
struct OffscreenGlyphOps {
    void* context = nullptr;
    // Selects the font; success stores the previous object in previous.
    bool (*selectFont)(void*, std::uintptr_t dc, std::uintptr_t font, std::uintptr_t* previous) noexcept = nullptr;
    bool (*restoreFont)(void*, std::uintptr_t dc, std::uintptr_t previous) noexcept = nullptr;
    bool (*deleteDc)(void*, std::uintptr_t dc) noexcept = nullptr;
    bool (*deleteFont)(void*, std::uintptr_t font) noexcept = nullptr;
};

enum class OffscreenFontSelection {
    Rejected,    // Ownership remains with the caller.
    Selected,    // Ownership transferred and the font is selected.
    FailedOwned  // Ownership transferred; Close must release it.
};

// Explicit lifetime is required: a failed Close retains handles for retry.
// The owner must keep this object alive until Close succeeds, including when
// drawing fails. Never pass a borrowed, window-owned DC to AdoptDc.
class OffscreenGlyphDc final {
    OffscreenGlyphOps ops_{};
    std::uintptr_t dc_ = 0, font_ = 0, previous_ = 0;
    bool selected_ = false, poisoned_ = false;
public:
    explicit OffscreenGlyphDc(OffscreenGlyphOps ops) noexcept : ops_(ops) {}
    OffscreenGlyphDc(const OffscreenGlyphDc&) = delete;
    OffscreenGlyphDc& operator=(const OffscreenGlyphDc&) = delete;
    OffscreenGlyphDc(OffscreenGlyphDc&&) = delete;
    OffscreenGlyphDc& operator=(OffscreenGlyphDc&&) = delete;
    bool AdoptDc(std::uintptr_t dc) noexcept {
        if (!dc || dc_ || font_ || !ops_.selectFont || !ops_.restoreFont || !ops_.deleteDc || !ops_.deleteFont) return false;
        dc_ = dc; poisoned_ = false; return true;
    }
    // The font must be exclusively owned and not selected into any other DC.
    // Rejected leaves ownership with the caller. Both other results transfer
    // ownership; FailedOwned requires Close and prevents further drawing.
    OffscreenFontSelection SelectOwnedFont(std::uintptr_t font) noexcept {
        if (!dc_ || !font || font_ || poisoned_) return OffscreenFontSelection::Rejected;
        font_ = font; std::uintptr_t previous = 0;
        if (!ops_.selectFont(ops_.context, dc_, font_, &previous) || !previous) {
            poisoned_ = true; return OffscreenFontSelection::FailedOwned;
        }
        previous_ = previous; selected_ = true; return OffscreenFontSelection::Selected;
    }
    bool RestoreFont() noexcept {
        if (!dc_ || !font_ || !selected_ || poisoned_) return false;
        if (!ops_.restoreFont(ops_.context, dc_, previous_)) { poisoned_ = true; return false; }
        selected_ = false; previous_ = 0;
        if (!ops_.deleteFont(ops_.context, font_)) { poisoned_ = true; return false; }
        font_ = 0; return true;
    }
    // Delete the owning DC first, even if restoration failed; only after that
    // is the font definitely no longer selected into this DC. A failed native
    // operation never clears its handle. Calling Close again retries it.
    bool Close() noexcept {
        if (dc_) {
            if (!ops_.deleteDc(ops_.context, dc_)) { poisoned_ = true; return false; }
            dc_ = 0; selected_ = false; previous_ = 0;
        }
        if (font_) {
            if (!ops_.deleteFont(ops_.context, font_)) { poisoned_ = true; return false; }
            font_ = 0;
        }
        poisoned_ = false; return true;
    }
    std::uintptr_t dc() const noexcept { return dc_; }
    std::uintptr_t font() const noexcept { return font_; }
    bool ready() const noexcept { return dc_ && !poisoned_; }
};
}
