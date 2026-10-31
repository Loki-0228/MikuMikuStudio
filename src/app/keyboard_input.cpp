#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/keyboard_input.hpp"
#include <cwchar>
#include <vector>

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

bool PollKeyInputEligibleWithForeground(const MMDApp* app, void* foreground) noexcept {
    if (!app) return false;
    // The samplers run between messages, so this is the same test
    // LetterHotkeyInputAllowed applies to the letter ladder.
    const HWND fg = static_cast<HWND>(foreground);
    if (fg == nullptr) return false;
    const HWND main = app->state.hwnd;
    const HWND floating = app->state.floatingWindow;
    return fg == main || (floating != nullptr && fg == floating);
}

namespace {
// Test seam: a window that should stand in for the live foreground window.
// The dialog-enter regression creates its owner on an inactive private desktop
// so real modal dialogs can open without disturbing the user's desktop, but an
// inactive desktop can never own the foreground - the poll would then be
// permanently ineligible and the case could only assert that nothing happens.
// Keyed by app so one case cannot leak into the next.
struct PollKeyForegroundOverride {
    const MMDApp* app;
    void* window;
};
PollKeyForegroundOverride& PollKeyForegroundOverrideSlot() {
    static PollKeyForegroundOverride slot{nullptr, nullptr};
    return slot;
}
}  // namespace

void SetPollKeyForegroundForTests(const MMDApp* app, void* foreground) noexcept {
    auto& slot = PollKeyForegroundOverrideSlot();
    slot.app = app;
    slot.window = foreground;
}

bool PollKeyInputEligible(const MMDApp* app) noexcept {
    const auto& slot = PollKeyForegroundOverrideSlot();
    if (app != nullptr && slot.app == app)
        return PollKeyInputEligibleWithForeground(app, slot.window);
    return PollKeyInputEligibleWithForeground(app, GetForegroundWindow());
}

namespace {
// One entry per live app.  The samplers run from the frame driver (UI thread),
// so a plain vector is enough; two apps in one process (the tests create one
// per case) each keep their own latch, and the entry is dropped on reset.
struct PollKeyEligibility {
    const MMDApp* app;
    void* hwnd;      // the main window this latch was taken for
    bool eligible;
    bool regained;
};
std::vector<PollKeyEligibility>& PollKeyEligibilityTable() {
    static std::vector<PollKeyEligibility> table;
    return table;
}
}  // namespace

bool AdvancePollKeyEligibility(const MMDApp* app, bool eligible) noexcept {
    if (app == nullptr) return false;
    auto& table = PollKeyEligibilityTable();
    const void* hwnd = app->state.hwnd;
    for (auto& entry : table) {
        if (entry.app != app || entry.hwnd != hwnd) continue;
        entry.regained = eligible && !entry.eligible;
        entry.eligible = eligible;
        return entry.regained;
    }
    // A new app (or a re-created main window): its first eligible frame counts as
    // a regain, so a key held at start-up cannot read as a new press either.
    // Reuse a dead entry instead of growing the table forever.
    for (auto& entry : table) {
        if (entry.app == app) {
            entry = {app, const_cast<void*>(hwnd), eligible, eligible};
            return eligible;
        }
    }
    table.push_back({app, const_cast<void*>(hwnd), eligible, eligible});
    return eligible;
}

bool PollKeyEligible(const MMDApp* app) noexcept {
    if (app == nullptr) return false;
    for (const auto& entry : PollKeyEligibilityTable())
        if (entry.app == app && entry.hwnd == app->state.hwnd) return entry.eligible;
    // No latch yet (a sampler ran before MouseInteractionBegin): use the live
    // foreground rule rather than freezing the cells off.
    return PollKeyInputEligible(app);
}

bool PollKeyRegained(const MMDApp* app) noexcept {
    if (app == nullptr) return false;
    for (const auto& entry : PollKeyEligibilityTable())
        if (entry.app == app && entry.hwnd == app->state.hwnd) return entry.regained;
    return false;
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
}
