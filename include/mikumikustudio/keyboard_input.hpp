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

// Foreground-owner history for the polled keyboard path, owned by MMDApp.
// The original port kept these latches in a translation-unit global keyed by
// raw HWND; that allocates inside a noexcept poll and can retain entries for
// windows that no longer exist.  This is a plain value member instead: no
// allocation, no lookup, and no identity outliving the window that produced
// it, because Advance with changed owner identities starts from ineligible.
// Every member is value-initialized, so a default-constructed state reports
// ineligible until the first Advance.
class PolledKeyState {
public:
    // Stores this poll's owner identities and eligibility, and reports whether
    // eligibility was regained: eligible now while the previous poll - for the
    // same pair of owner windows - was not.  A changed main or floating HWND
    // counts as a prior ineligible poll, so a rebuilt window cannot inherit a
    // stale eligible latch.
    bool Advance(void* main, void* floating, bool eligible) noexcept {
        const bool sameOwner = main == mainWindow_ && floating == floatingWindow_;
        const bool wasEligible = sameOwner && eligible_;
        mainWindow_ = main;
        floatingWindow_ = floating;
        eligible_ = eligible;
        regained_ = eligible && !wasEligible;
        return regained_;
    }
    bool Eligible() const noexcept { return eligible_; }
    bool Regained() const noexcept { return regained_; }
    // Test seam: an enabled override replaces GetForegroundWindow().  nullptr
    // is a meaningful override value (no foreground window), so only
    // ClearForegroundOverride turns the override off; setting it again keeps
    // it enabled.
    void SetForegroundOverride(void* foreground) noexcept {
        foregroundOverrideEnabled_ = true;
        foregroundOverride_ = foreground;
    }
    void ClearForegroundOverride() noexcept {
        foregroundOverrideEnabled_ = false;
        foregroundOverride_ = nullptr;
    }
    bool HasForegroundOverride() const noexcept { return foregroundOverrideEnabled_; }
    void* ForegroundOverride() const noexcept { return foregroundOverride_; }
private:
    void* mainWindow_ = nullptr;
    void* floatingWindow_ = nullptr;
    bool eligible_ = false;
    bool regained_ = false;
    bool foregroundOverrideEnabled_ = false;
    void* foregroundOverride_ = nullptr;
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

// Runtime eligibility source for the polled keyboard sampler.  The main or
// floating window must own the foreground; the decision uses only the app's
// own owner identities and, without an override, GetForegroundWindow().  These
// helpers allocate nothing and consult no registry.
bool PollKeyInputEligibleWithForeground(const MMDApp* app, void* foreground) noexcept;
bool PollKeyInputEligible(const MMDApp* app) noexcept;
void AdvancePollKeyEligibility(MMDApp* app, bool eligible) noexcept;
bool PollKeyEligible(const MMDApp* app) noexcept;
bool PollKeyRegained(const MMDApp* app) noexcept;
// Test seams: the override substitutes for GetForegroundWindow(), including
// nullptr to simulate "no foreground window".  Clearing it restores the real
// foreground query; the latched eligibility value is left untouched.
void SetPollKeyForegroundForTests(MMDApp* app, void* foreground) noexcept;
void ClearPollKeyForegroundForTests(MMDApp* app) noexcept;

// Test-only raw key source for the polled keyboard path.  The regression
// fixture read the thread key-state table back as zero after SetKeyboardState
// reported success, so tests seed the source directly instead of relying on OS
// injection.  The seam replaces the raw source only: foreground eligibility and
// the edge cells stay untouched.  The override is thread-local and keeps no
// pointer to the caller's bytes, and the disabled state forwards the original
// Win32 calls, so production behaviour is unchanged unless a test opts in.
// While enabled, byte bit 0x80 reads back as SHORT bit 0x8000, byte bit 0x01 as
// the toggle bit, and an index outside 0..255 as 0.
std::int16_t ReadPolledKeyState(int virtualKey) noexcept;
std::int16_t ReadPolledAsyncKeyState(int virtualKey) noexcept;
void SetPolledKeyStateForTests(const unsigned char (&source)[256]) noexcept;
void ClearPolledKeyStateForTests() noexcept;
bool HasPolledKeyStateForTests() noexcept;
}
