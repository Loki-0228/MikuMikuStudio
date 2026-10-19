#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/unicode_window.hpp"

namespace mikudancestudio {
namespace {
constexpr wchar_t kPreviousProc[] = L"MikuMikuStudio.UnicodeTitle.PreviousProc";

LRESULT CALLBACK UnicodeTitleProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto previous = reinterpret_cast<WNDPROC>(GetPropW(window, kPreviousProc));
    // Do not let an ANSI subclass convert a UTF-16 text response back to ACP.
    if (message == WM_GETTEXT || message == WM_GETTEXTLENGTH)
        return DefWindowProcW(window, message, wp, lp);
    const LRESULT result = CallWindowProcW(previous, window, message, wp, lp);
    if (message == WM_SETTEXT)
        return DefWindowProcW(window, message, wp, lp);
    if (message == WM_NCDESTROY)
        RemovePropW(window, kPreviousProc);
    return result;
}
}

void SetUnicodeWindowTitle(HWND window, const wchar_t* title) {
    // GetWindowLongPtrW returns the correct thunk for an ANSI predecessor;
    // CallWindowProcW preserves its original message convention.
    // A plugin may report a Unicode procedure while converting text internally.
    // Protect both kinds of predecessor, not just IsWindowUnicode() == FALSE.
    if (!GetPropW(window, kPreviousProc)) {
        auto previous = GetWindowLongPtrW(window, GWLP_WNDPROC);
        if (SetPropW(window, kPreviousProc, reinterpret_cast<HANDLE>(previous))) {
            SetLastError(ERROR_SUCCESS);
            if (!SetWindowLongPtrW(window, GWLP_WNDPROC,
                                   reinterpret_cast<LONG_PTR>(UnicodeTitleProc)) && GetLastError())
                RemovePropW(window, kPreviousProc);
        }
    }
    SetWindowTextW(window, title);
}
}
