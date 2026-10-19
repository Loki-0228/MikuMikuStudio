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
}
