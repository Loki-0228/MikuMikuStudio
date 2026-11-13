#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/keyboard_input.hpp"
#include <cwchar>

namespace mikumikustudio {
namespace {
// An active input method (Chinese/Japanese IME) does not deliver the letters
// the way a plain English layout does: the keys are routed through the IME's
// composition window - a window this thread owns but which is not rooted at
// the main window - and an IME that filters keys with its own hook can keep
// them out of the thread key-state table entirely.  Both cases used to kill
// every letter shortcut until the user switched the IME back to English
// (Ctrl+Space on a Chinese system), which is why P could not stop a running
// playback.
//
// Two independent signals are therefore combined, and the focus window is
// accepted whenever it belongs to the same thread as the main window:
//   * GetKeyboardState  - the original polling source (thread key state);
//   * GetAsyncKeyState  - the physical key state, unaffected by the IME.
// The text-entry suppression below is kept: a focused Edit / RichEdit control
// still swallows the letters, IME or not.
bool FocusAcceptsLetterHotkeys(HWND main, HWND floating, HWND focus) noexcept {
    if (focus == nullptr) return false;
    const HWND root = GetAncestor(focus, GA_ROOT);
    if (root == main || root == floating) return true;
    // Not rooted at our window: accept it only when the input method created
    // it for this thread (composition / candidate / "Default IME" window).
    const DWORD focusThread = GetWindowThreadProcessId(focus, nullptr);
    const DWORD mainThread = GetWindowThreadProcessId(main, nullptr);
    return focusThread != 0 && focusThread == mainThread;
}

bool IsTextEntryWindow(HWND focus) noexcept {
    wchar_t className[64]{};
    if (!GetClassNameW(focus, className, _countof(className))) return false;
    return _wcsicmp(className, L"Edit") == 0 ||
           _wcsnicmp(className, L"RichEdit", 8) == 0;
}
}  // namespace

bool FocusAllowsLetterHotkeys(void* main, void* floating, void* focus) noexcept {
    const HWND mainWindow = static_cast<HWND>(main);
    const HWND floatingWindow = static_cast<HWND>(floating);
    const HWND focusWindow = static_cast<HWND>(focus);
    // A cleared thread focus must not disable the letter shortcuts: the panel
    // pump parks focus on the panel root through GetDlgItem(main, 0), which is
    // NULL in this port (no id-0 control exists), so SetFocus(NULL) left the
    // whole letter ladder - and the Delete gate that also tests focus == main -
    // dead until the user clicked a control again.
    if (focusWindow == nullptr) return true;
    if (!FocusAcceptsLetterHotkeys(mainWindow, floatingWindow, focusWindow))
        return false;
    return !IsTextEntryWindow(focusWindow);
}

bool LetterHotkeyInputAllowed(const MMDApp* app) noexcept {
    if (!app) return false;
    const HWND foreground = GetForegroundWindow();
    const HWND main = app->state.hwnd;
    const HWND floating = app->state.floatingWindow;
    if (!foreground || (foreground != main && foreground != floating)) return false;
    return FocusAllowsLetterHotkeys(main, floating, GetFocus());
}

void PollLetterHotkeys(MMDApp* app) {
    unsigned char keys[256]{};
    const bool eligible = LetterHotkeyInputAllowed(app) && GetKeyboardState(keys);
    if (eligible) {
        // Physical state as well: an IME may consume the keystroke before the
        // thread key-state table records it.
        for (int vk : kLetterVirtualKeys) {
            if ((GetAsyncKeyState(vk) & 0x8000) != 0) keys[vk] |= 0x80;
        }
    }
    auto& input = app->LetterHotkeys();
    input.Update(keys, eligible);
    for (int i = 0; i < 18; ++i) {
        // Retain the command/dialog compatibility cells, but shortcut
        // consumers read only the independent physical-key history.
        app->state.dialogFlags[i] = input.Value(i);
        if (input.Value(i) == 1 || input.Value(i) == 2)
            app->state.messageSeen = 1;
    }
}

// Polled keyboard eligibility.  The latch lives in the app (PolledKeys()) so
// each window owns exactly one history and a destroyed window leaves nothing
// behind: the next Advance for a different owner pair starts from ineligible.
// An override, when enabled for tests, stands in for GetForegroundWindow() so
// the rules can be exercised without owning the foreground.
bool PollKeyInputEligibleWithForeground(const MMDApp* app, void* foreground) noexcept {
    if (!app || !foreground) return false;
    const void* main = app->state.hwnd;
    const void* floating = app->state.floatingWindow;
    return foreground == main || (floating != nullptr && foreground == floating);
}

bool PollKeyInputEligible(const MMDApp* app) noexcept {
    if (!app) return false;
    const PolledKeyState& polled = app->PolledKeys();
    void* foreground = polled.HasForegroundOverride()
        ? polled.ForegroundOverride()
        : static_cast<void*>(GetForegroundWindow());
    return PollKeyInputEligibleWithForeground(app, foreground);
}

void AdvancePollKeyEligibility(MMDApp* app, bool eligible) noexcept {
    if (!app) return;
    app->PolledKeys().Advance(static_cast<void*>(app->state.hwnd),
                              static_cast<void*>(app->state.floatingWindow),
                              eligible);
}

bool PollKeyEligible(const MMDApp* app) noexcept {
    return app && app->PolledKeys().Eligible();
}

bool PollKeyRegained(const MMDApp* app) noexcept {
    return app && app->PolledKeys().Regained();
}

void SetPollKeyForegroundForTests(MMDApp* app, void* foreground) noexcept {
    if (app) app->PolledKeys().SetForegroundOverride(foreground);
}

void ClearPollKeyForegroundForTests(MMDApp* app) noexcept {
    if (app) app->PolledKeys().ClearForegroundOverride();
}

namespace {
// Test-only raw key source, one copy per thread.  These bytes are what the read
// helpers report while enabled; the setter copies the caller's array, so
// nothing here points into a test's storage.
thread_local bool enabled = false;
thread_local unsigned char keys[256]{};

// One polled key byte as the Win32 SHORT: bit 0x80 becomes 0x8000, bit 0x01 is
// copied as the toggle bit, and the remaining bits are zero.  An index outside
// 0..255 reports 0.
std::int16_t PolledKeyShort(int virtualKey) noexcept {
    if (virtualKey < 0 || virtualKey > 255) return 0;
    const std::uint16_t bits = static_cast<std::uint16_t>(
        ((keys[virtualKey] & 0x80) != 0 ? 0x8000u : 0x0000u) |
        static_cast<unsigned>(keys[virtualKey] & 0x01));
    return static_cast<std::int16_t>(bits);
}
}  // namespace

std::int16_t ReadPolledKeyState(int virtualKey) noexcept {
    if (!enabled) return GetKeyState(virtualKey);
    return PolledKeyShort(virtualKey);
}

std::int16_t ReadPolledAsyncKeyState(int virtualKey) noexcept {
    if (!enabled) return GetAsyncKeyState(virtualKey);
    return PolledKeyShort(virtualKey);
}

void SetPolledKeyStateForTests(const unsigned char (&source)[256]) noexcept {
    for (int i = 0; i < 256; ++i) keys[i] = source[i];
    enabled = true;
}

void ClearPolledKeyStateForTests() noexcept {
    enabled = false;
    for (int i = 0; i < 256; ++i) keys[i] = 0;
}

bool HasPolledKeyStateForTests() noexcept { return enabled; }
}
