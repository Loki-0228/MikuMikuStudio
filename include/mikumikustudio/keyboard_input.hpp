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

// The foreground half of the same rule, shared by every per-frame key-edge
// sampler (frame_modes.cpp PollKey, model_query_gaps.cpp KeyEdgeScan).  Only
// while our main or floating window owns the foreground does the thread
// key-state table describe *this* application's keys; while another window owns
// it, GetKeyState() keeps reporting the last state our thread saw and a key
// released in the other application never produces a matching key-up here.
// Sampling edges in that state synthesizes a fresh press on the first frame
// after the window is re-activated - which is issue #41: selected camera/bone
// keyframes were deleted the moment MMS regained focus, because a stale
// VK_DELETE read as a new press edge and PumpDeleteRebuild deleted the marked
// keys.  The samplers therefore reset their cells while this returns false and
// latch a still-held key as held (3), never pressed (1), on the first eligible
// frame.  The injected form exists so tests can drive both branches without
// owning the user's desktop foreground window.
bool PollKeyInputEligible(const MMDApp* app) noexcept;
bool PollKeyInputEligibleWithForeground(const MMDApp* app, void* foreground) noexcept;
// Test seam: substitute the window that stands in for the live foreground.
// A regression that needs real modal dialogs runs on an inactive private
// desktop, which can never own the foreground, so the poll would be
// permanently ineligible and the case could only prove that nothing happens.
// Pass the app's own window to make the poll eligible, or nullptr to clear it.
void SetPollKeyForegroundForTests(const MMDApp* app, void* foreground) noexcept;

// Per-frame edge-sampler state shared by every sampler that walks the same key
// cells (MouseInteractionBegin and PollKeyboardStates can both run in one
// pump iteration, so the state cannot live inside either one):
//   * AdvancePollKeyEligibility  latches this frame's eligibility and returns
//     true on the first eligible frame after a non-eligible stretch.  A held
//     key on that frame must become 3 (held), never 1 (pressed).
//   * PollKeyEligible            the latched value current samplers should use.
// Keyed by the app so two windows (main + floating) and tests never share it.
bool AdvancePollKeyEligibility(const MMDApp* app, bool eligible) noexcept;
bool PollKeyEligible(const MMDApp* app) noexcept;
// True on the first eligible frame after a non-eligible stretch, i.e. the frame
// on which a held key must be latched as 3 (held) instead of 1 (pressed).
bool PollKeyRegained(const MMDApp* app) noexcept;
}
