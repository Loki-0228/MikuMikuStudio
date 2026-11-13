#pragma once

#include <Windows.h>
#include <CommCtrl.h>
#include <functional>
#include "mikumikustudio/ui_scale.hpp"

namespace ui_scale_popup_fixture {
namespace detail {
inline constexpr UINT kMove = WM_APP + 0x72, kFinish = WM_APP + 0x73;
struct Selection {
    HWND owner;
    int target;
    bool accept;
    const std::function<void(HWND)>& inspect;
    bool opened = false, previewed = false;
    bool finishAfterInspect = true;
};
inline thread_local Selection* selection = nullptr;

inline LRESULT CALLBACK Drive(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto& action = *reinterpret_cast<Selection*>(data);
    if (message == kMove) {
        const HWND slider = GetDlgItem(window, mikumikustudio::ui_scale::kSlider);
        SendMessageW(slider, TBM_SETPOS, TRUE, action.target);
        SendMessageW(window, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, action.target), reinterpret_cast<LPARAM>(slider));
        if (!PostMessageW(window, kFinish, 0, 0)) EndDialog(window, IDCANCEL);
        return 0;
    }
    if (message == kFinish) {
        action.previewed = mikumikustudio::ui_scale::Percent() == action.target;
        if (action.inspect) action.inspect(window);
        if (!action.finishAfterInspect) return 0;
        const auto command = action.accept ? IDOK : IDCANCEL;
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED),
            reinterpret_cast<LPARAM>(GetDlgItem(window, command)));
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Drive, id);
    return DefSubclassProc(window, message, wp, lp);
}

inline LRESULT CALLBACK Initialized(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && selection && !selection->opened) {
        const auto& event = *reinterpret_cast<const CWPRETSTRUCT*>(lp);
        if (event.message == WM_INITDIALOG && GetWindow(event.hwnd, GW_OWNER) == selection->owner &&
            GetDlgItem(event.hwnd, mikumikustudio::ui_scale::kSlider)) {
            selection->opened = true;
            if (!SetWindowSubclass(event.hwnd, Drive, 1, reinterpret_cast<DWORD_PTR>(selection)) ||
                !PostMessageW(event.hwnd, kMove, 0, 0)) EndDialog(event.hwnd, IDCANCEL);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}

inline bool Choose(HWND owner, int target, bool accept = true, const std::function<void(HWND)>& inspect = {},
                   bool finishAfterInspect = true) {
    const int initial = mikumikustudio::ui_scale::Percent();
    detail::Selection action{owner, target, accept, inspect};
    action.finishAfterInspect = finishAfterInspect;
    const auto hook = SetWindowsHookExW(WH_CALLWNDPROCRET, detail::Initialized, nullptr, GetCurrentThreadId());
    if (!hook) return false;
    detail::selection = &action;
    SendMessageW(owner, WM_COMMAND, mikumikustudio::ui_scale::kMenuCommand, 0);
    detail::selection = nullptr;
    UnhookWindowsHookEx(hook);
    return action.opened && action.previewed &&
        (!finishAfterInspect || mikumikustudio::ui_scale::Percent() == (accept ? target : initial));
}
}
