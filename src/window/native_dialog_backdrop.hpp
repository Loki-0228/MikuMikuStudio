#pragma once
// Shared scaling and backdrop for native dialogs inside the application.
#include "mikumikustudio/build_profile.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commctrl.h>
#include <cstdint>

#if !MMD_ORIGINAL_PROFILE
#include <new>
#include <algorithm>
#include <utility>
#include <vector>
#include "mikumikustudio/ui_control_layout.hpp"
#include <native_ui/win32_host.hpp>

namespace mikumikustudio::native_dialog_backdrop {
namespace detail {
// Dialogs keep their native template, children and dialog procedures. This
// subclass only owns the parent client background: one private PanelBuffer per
// live HWND. On-screen client paint is damage-local and clipped away from the
// native children; offscreen composition paints the complete parent background
// so the default child print pass has a baseline. Business context stays in
// DWLP_USER.
inline constexpr UINT_PTR kBackdropSubclass = 0x4D4D5331;
inline constexpr UINT kDpiChangedMessage = 0x02E0; // WM_DPICHANGED, hidden at WINVER 0x0601.

struct DialogBackdrop {
    HWND window = nullptr;
    native_ui::PanelBuffer buffer;
    HRGN update = nullptr; // exact damage captured before BeginPaint validates it
    HRGN replay = nullptr; // copy handed back to the update region on fallback
    uint64_t epoch = 0;
    unsigned entries = 0;
    bool retired = false;
    bool nativePaint = false;
    std::vector<std::pair<HFONT, HFONT>> fonts;
    ~DialogBackdrop() {
        if (update) DeleteObject(update);
        if (replay) DeleteObject(replay);
        for (const auto& font : fonts) if (font.second) DeleteObject(font.second);
    }
};

inline int FitOrigin(int position, int extent, int start, int end) noexcept {
    return std::clamp(position, start, (std::max)(start, end - extent));
}

inline void ScaleAtOpen(DialogBackdrop& state, int requested, int minimum) {
    if (requested < minimum) return;
    const HWND window = state.window;
    const WindowDpiScope dpi(window);
    RECT outer{}, client{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetWindowRect(window, &outer) || !GetClientRect(window, &client) || IsRectEmpty(&client) ||
        !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const int oldWidth = outer.right - outer.left, oldHeight = outer.bottom - outer.top;
    const int frameWidth = oldWidth - client.right, frameHeight = oldHeight - client.bottom;
    const auto& work = monitor.rcWork;
    const int percent = (std::min)({requested,
        int((work.right - work.left - frameWidth) * 100 / client.right),
        int((work.bottom - work.top - frameHeight) * 100 / client.bottom)});
    if (percent < minimum) return;

    const auto scale = [percent](int value) { return MulDiv(value, percent, 100); };
    const int width = frameWidth + scale(client.right), height = frameHeight + scale(client.bottom);
    const int x = FitOrigin(outer.left - (width - oldWidth) / 2, width, work.left, work.right);
    const int y = FitOrigin(outer.top - (height - oldHeight) / 2, height, work.top, work.bottom);
    if (percent == 100) {
        if (x != outer.left || y != outer.top)
            SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    struct Control { HWND window; RECT bounds; HFONT font; };
    std::vector<Control> children;
    const auto fontFor = [&](HWND control) -> HFONT {
        const auto original = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        if (!original || state.retired) return nullptr;
        for (const auto& font : state.fonts) if (font.first == original) return font.second;
        LOGFONTW font{};
        if (!GetObjectW(original, sizeof(font), &font)) return nullptr;
        font.lfHeight = MulDiv(font.lfHeight, percent, 100);
        font.lfWidth = MulDiv(font.lfWidth, percent, 100);
        state.fonts.emplace_back(original, nullptr);
        return state.fonts.back().second = CreateFontIndirectW(&font);
    };
    const auto dialogFont = fontFor(window);
    if (!dialogFont) return;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto bounds = ui::layout_detail::ActualRect(window, child);
        const auto font = fontFor(child);
        if (!font || state.retired || IsRectEmpty(&bounds)) return;
        children.push_back({child, bounds, font});
    }
    if (!SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE) || state.retired) return;
    ui::LayoutBatch layout;
    ui::SetLayoutFont(window, dialogFont);
    for (const auto& child : children) {
        if (state.retired) return;
        const int left = scale(child.bounds.left), top = scale(child.bounds.top);
        ui::SetLayoutFont(child.window, child.font);
        if (state.retired) return;
        ui::MoveLayoutWindow(child.window, left, top, scale(child.bounds.right) - left,
                             scale(child.bounds.bottom) - top, true);
    }
    layout.Commit();
}

struct ConfirmationPlacement {
    HWND owner;
    const wchar_t* title;
    bool placed = false;
};
inline thread_local ConfirmationPlacement* activeConfirmation = nullptr;

inline void FitConfirmation(HWND window, HWND owner) noexcept {
    const WindowDpiScope dpi(owner);
    RECT bounds{}, anchor{};
    MONITORINFO monitor{sizeof monitor};
    if (!GetWindowRect(window, &bounds) || !GetWindowRect(owner, &anchor) ||
        !GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto& work = monitor.rcWork;
    if (bounds.left >= work.left && bounds.top >= work.top &&
        bounds.right <= work.right && bounds.bottom <= work.bottom) return;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const int x = FitOrigin(anchor.left + (anchor.right - anchor.left - width) / 2,
        width, work.left, work.right);
    const int y = FitOrigin(anchor.top + (anchor.bottom - anchor.top - height) / 2,
        height, work.top, work.bottom);
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

inline LRESULT CALLBACK PlaceConfirmation(int code, WPARAM wp, LPARAM lp) {
    auto* placement = activeConfirmation;
    if (code == HCBT_ACTIVATE && placement && !placement->placed) {
        const auto window = reinterpret_cast<HWND>(wp);
        wchar_t title[128]{}, windowClass[16]{};
        if (GetWindow(window, GW_OWNER) == placement->owner &&
            GetClassNameW(window, windowClass, 16) && lstrcmpW(windowClass, L"#32770") == 0 &&
            GetDlgItem(window, IDYES) && GetDlgItem(window, IDNO) &&
            GetWindowTextW(window, title, 128) && lstrcmpW(title, placement->title) == 0) {
            placement->placed = true;
            FitConfirmation(window, placement->owner);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline UINT WindowDpi(HWND window) noexcept {
    using DpiForWindow = UINT(WINAPI*)(HWND);
    static DpiForWindow dpiForWindow = reinterpret_cast<DpiForWindow>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    return dpiForWindow ? dpiForWindow(window) : 96u;
}

inline uint64_t BackdropKey(HWND window, const DialogBackdrop& state) noexcept {
    const uint64_t color = static_cast<uint64_t>(GetSysColor(COLOR_3DFACE)) & 0xFFFFFF;
    return ((state.epoch & 0xFFFFFF) << 40) | ((static_cast<uint64_t>(WindowDpi(window)) & 0xFFFF) << 24) | color;
}

// Saves the target DC state and clips it to the client for every target.
// Immediate visible children are excluded only for the genuine live window DC:
// on-screen paint owns their pixels, while a supplied memory/print DC receives
// the complete parent background and the default WM_PRINT pass draws the native
// children over it. Returns the saved state, 0 if nothing may be changed, or -1
// when the client has no paint area. A 0 result leaves the DC untouched.
inline int ClipBackdrop(HWND window, HDC target, RECT& client) noexcept {
    if (!GetClientRect(window, &client)) return 0;
    if (IsRectEmpty(&client)) return -1;
    const int saved = SaveDC(target);
    if (!saved) return 0;
    if (IntersectClipRect(target, client.left, client.top, client.right, client.bottom) == ERROR) {
        RestoreDC(target, saved);
        return 0;
    }
    // Supplied composition DCs keep their clip/mapping and get the full background.
    if (GetObjectType(target) != OBJ_DC || WindowFromDC(target) != window) return saved;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        RECT bounds{};
        if (!GetWindowRect(child, &bounds)) { RestoreDC(target, saved); return 0; }
        SetLastError(0);
        const int mapped = MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&bounds), 2);
        if (mapped == 0 && GetLastError() != ERROR_SUCCESS) { RestoreDC(target, saved); return 0; }
        if (IsRectEmpty(&bounds)) continue;
        if (ExcludeClipRect(target, bounds.left, bounds.top, bounds.right, bounds.bottom) == ERROR) {
            RestoreDC(target, saved);
            return 0;
        }
    }
    return saved;
}

// No call here sends a message while the buffer DC is borrowed, so no reentrant
// paint can reset that DC mid-frame. The entry lease in BackdropProc keeps the
// state and its GDI objects alive through nested destruction.
inline bool FillBackdrop(HWND window, DialogBackdrop& state, HDC target, const RECT& damage) noexcept {
    RECT client{};
    const int saved = ClipBackdrop(window, target, client);
    if (saved < 0) return true;
    if (!saved) return false;
    const HDC memory = state.buffer.Begin(target, client.right - client.left, client.bottom - client.top,
        BackdropKey(window, state), GetSysColor(COLOR_3DFACE));
    bool painted = false;
    if (memory) painted = state.buffer.Copy(target, damage);
    if (!painted) painted = FillRect(target, &damage, GetSysColorBrush(COLOR_3DFACE)) != FALSE;
    RestoreDC(target, saved);
    return painted;
}

inline LRESULT CALLBACK BackdropProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference) {
    auto* state = reinterpret_cast<DialogBackdrop*>(reference);
    if (!state || state->window != window) return DefSubclassProc(window, message, wp, lp);
    ++state->entries;
    bool forward = true;
    LRESULT result = 0;
    switch (message) {
    case WM_NCDESTROY:
        // Drop only this subclass; the entry lease frees the state afterwards.
        state->retired = true;
        RemoveWindowSubclass(window, BackdropProc, id);
        result = DefSubclassProc(window, message, wp, lp);
        forward = false;
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        // Capture the exact damage before BeginPaint validates it so a failed
        // buffered clip can hand that same region back to the native dialog.
        const int regionType = state->update ? GetUpdateRgn(window, state->update, FALSE) : ERROR;
        const HDC target = BeginPaint(window, &paint);
        if (!target) break;
        // BeginPaint dispatches WM_ERASEBKGND; that re-entry may have destroyed
        // the dialog, so liveness must be rechecked before the buffer is used.
        const bool live = !state->retired && IsWindow(window);
        const bool painted = live && FillBackdrop(window, *state, target, paint.rcPaint);
        EndPaint(window, &paint);
        if (!live) { forward = false; break; }
        if (painted) { forward = false; break; }
        // EndPaint validated the damage. Restore the captured region, then let
        // the original dialog procedure paint it natively with erase enabled.
        bool replayed = false;
        if (regionType != ERROR && state->replay &&
            CombineRgn(state->replay, state->update, nullptr, RGN_COPY) != ERROR)
            replayed = InvalidateRgn(window, state->replay, TRUE) != FALSE;
        if (!replayed) InvalidateRect(window, &paint.rcPaint, TRUE);
        state->nativePaint = true;
        result = DefSubclassProc(window, WM_PAINT, wp, lp);
        state->nativePaint = false;
        forward = false;
        break;
    }
    case WM_ERASEBKGND: {
        const HDC target = reinterpret_cast<HDC>(wp);
        if (state->nativePaint) {
            // Native fallback: ClipBackdrop saves the target DC, restricts it to
            // the client and, for the genuine live window DC, removes the visible
            // immediate children, so the default erase cannot repaint a control.
            // Supplied memory/print DCs only gain the client clip here and keep
            // their complete-background contract. A target or clip that cannot be
            // established falls through to the unchanged native erase.
            RECT client{};
            const int saved = target ? ClipBackdrop(window, target, client) : 0;
            if (saved < 0) { result = 1; forward = false; break; }
            if (!saved) break;
            result = DefSubclassProc(window, message, wp, lp);
            RestoreDC(target, saved);
            forward = false;
            break;
        }
        RECT client{};
        if (target && GetClientRect(window, &client)) {
            if (IsRectEmpty(&client)) { result = 1; forward = false; }
            else if (GetObjectType(target) == OBJ_DC && WindowFromDC(target) == window) {
                // The client paint below always fills the whole damage.
                result = 1; forward = false;
            } else if (FillBackdrop(window, *state, target, client)) {
                // Supplied memory/print DC: paint the complete background now.
                result = 1; forward = false;
            }
        }
        break;
    }
    case WM_PRINTCLIENT: {
        // Supplied composition DC: full parent background under the children;
        // the default WM_PRINT pass draws the native controls afterwards.
        const HDC target = reinterpret_cast<HDC>(wp);
        RECT client{};
        if (target && GetClientRect(window, &client) && !IsRectEmpty(&client) &&
            FillBackdrop(window, *state, target, client)) forward = false;
        break;
    }
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_FONTCHANGE:
    case WM_SETFONT:
    case kDpiChangedMessage:
        ++state->epoch;
        break;
    default:
        break;
    }
    if (forward) result = DefSubclassProc(window, message, wp, lp);
    // This frame still holds a lease, so a nested WM_NCDESTROY cannot have
    // freed the state before this last reference is dropped.
    if (--state->entries == 0 && state->retired) delete state;
    return result;
}
} // namespace detail

// Keep the native buttons and modality. Only the matching confirmation may
// move, and only when its system position falls outside the owner's work area.
inline int ConfirmYesNo(HWND owner, const wchar_t* text, const wchar_t* title) noexcept {
    detail::ConfirmationPlacement placement{owner, title};
    auto* previous = std::exchange(detail::activeConfirmation, &placement);
    const HHOOK hook = SetWindowsHookExW(WH_CBT, detail::PlaceConfirmation, nullptr, GetCurrentThreadId());
    const int result = MessageBoxW(owner, text, title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (hook) UnhookWindowsHookEx(hook);
    detail::activeConfirmation = previous;
    return result;
}

// Installs the shared parent-client backdrop once per dialog HWND. The state is
// released by this subclass's WM_NCDESTROY lease; reattaching is a no-op.
// A dialog can opt into a smaller work-area scale after validating its text,
// native control chrome and hit targets at that percentage.
inline void Attach(HWND window, int interfacePercent = 100, int minimumPercent = 100) noexcept {
    if (!window || !IsWindow(window)) return;
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(window, detail::BackdropProc, detail::kBackdropSubclass, &existing)) return;
    auto* state = new (std::nothrow) detail::DialogBackdrop;
    if (!state) return;
    state->window = window;
    state->update = CreateRectRgn(0, 0, 0, 0);
    state->replay = CreateRectRgn(0, 0, 0, 0);
    if (!state->update || !state->replay ||
        !SetWindowSubclass(window, detail::BackdropProc, detail::kBackdropSubclass, reinterpret_cast<DWORD_PTR>(state))) {
        delete state;
        return;
    }
    // Hold the same lease used by the subclass while layout can pump messages.
    ++state->entries;
    try { detail::ScaleAtOpen(*state, interfacePercent, (std::max)(1, minimumPercent)); } catch (...) {}
    if (--state->entries == 0 && state->retired) delete state;
}
} // namespace mikumikustudio::native_dialog_backdrop

#else

namespace mikumikustudio::native_dialog_backdrop {
// Original profile: the untouched template paints every dialog itself and the
// private buffer is not part of that build.
inline void Attach(HWND, int = 100, int = 100) noexcept {}
inline int ConfirmYesNo(HWND owner, const wchar_t* text, const wchar_t* title) noexcept {
    return MessageBoxW(owner, text, title, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
}
} // namespace mikumikustudio::native_dialog_backdrop

#endif
