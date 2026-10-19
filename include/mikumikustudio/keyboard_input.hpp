#pragma once

#include <cstdint>

namespace mikumikustudio {
class MMDApp;

// Win32 letter virtual keys are uppercase values regardless of Shift/Caps
// Lock. Lowercase ASCII values name unrelated keys (e.g. 'a' is NUMPAD1).
inline constexpr int kLetterVirtualKeys[] = {
    'X', 'Z', 'C', 'V', 'D', 'A', 'B', 'G', 'S',
    'I', 'H', 'K', 'P', 'U', 'J', 'F', 'R', 'L'
};

// Runtime-only input history, separate from dialogFlags and the pinned blob.
// Losing input eligibility discards edges; a held key on reactivation is
// latched as held, never synthesized into a new press.
class LetterHotkeyState {
public:
    void Update(const unsigned char (&keys)[256], bool eligible) noexcept {
        for (int i = 0; i < 18; ++i) {
            const bool down = eligible && (keys[kLetterVirtualKeys[i]] & 0x80) != 0;
            const bool wasDown = values_[i] == 1 || values_[i] == 3;
            values_[i] = !eligible ? 0 : down ?
                (!eligible_ || wasDown ? 3 : 1) : (wasDown ? 2 : 0);
        }
        eligible_ = eligible;
    }
    bool Pressed(int slot) const noexcept { return values_[slot] == 1; }
    std::int32_t Value(int slot) const noexcept { return values_[slot]; }
private:
    std::int32_t values_[18]{};
    bool eligible_ = false;
};

// Input is eligible while the main (or floating) window owns the foreground
// and the focus window is neither a text field nor foreign to the application
// thread.  Windows an active input method creates for this thread (the
// composition / "Default IME" window) stay eligible, so the letter shortcuts
// keep working while a Chinese/Japanese IME is switched on; the poll combines
// the thread key state with the IME-independent physical key state.
bool LetterHotkeyInputAllowed(const MMDApp* app) noexcept;
// The focus half of that rule, split out so it is testable without owning the
// foreground window: a cleared focus (the panel pump used to call
// SetFocus(NULL)) is *eligible*, a foreign or text-entry focus is not.
bool FocusAllowsLetterHotkeys(void* main, void* floating, void* focus) noexcept;
void PollLetterHotkeys(MMDApp* app);
}
