#include "mikumikustudio/viewport_render_scale.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/frame_rate.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/build_profile.hpp"
#include <CommCtrl.h>
#include <algorithm>
#include <chrono>
#include <cwchar>

namespace mikumikustudio::viewport_render_scale {
namespace {
using Clock = std::chrono::steady_clock;
constexpr wchar_t kPreviewSession[] = L"MMS.ViewportScalePreview";
constexpr UINT_PTR kPreviewSubclass = 9140;
struct PreviewSession {
    MMDApp* app;
    HWND dialog = nullptr;
    Clock::time_point lastFrame{};
};
const wchar_t* Text(MMDApp* app, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    return app->EnglishUI() == 2 ? zh : app->EnglishUI() == 0 ? ja : en;
}
const wchar_t* Title(MMDApp* app) {
    return Text(app, L"视口渲染比例", L"Viewport resolution scale", L"描画解像度の倍率");
}
void WritePercent(HWND dialog, double percent) {
    wchar_t text[64]{};
    swprintf_s(text, L"%.12g", percent);
    SetDlgItemTextW(dialog, Percent, text);
    SendDlgItemMessageW(dialog, Slider, TBM_SETPOS, TRUE, LPARAM(std::clamp(percent, 25.0, 100.0)));
}
bool ReadPercent(HWND dialog, double& percent) {
    wchar_t text[128]{};
    GetDlgItemTextW(dialog, Percent, text, 128);
    return ParsePercent(text, percent);
}
void Show4KWarning(HWND dialog, MMDApp* app, bool above4K) {
    SetDlgItemTextW(dialog, Warning, above4K ? Text(app,
        L"警告：超过 4K", L"Warning: above 4K", L"警告：4Kを超えています") : L"");
    ShowWindow(GetDlgItem(dialog, Warning), above4K ? SW_SHOWNA : SW_HIDE);
}
void UpdateReadout(HWND dialog, MMDApp* app) {
    const bool automatic = IsDlgButtonChecked(dialog, Automatic) == BST_CHECKED;
    EnableWindow(GetDlgItem(dialog, Slider), !automatic);
    EnableWindow(GetDlgItem(dialog, Percent), !automatic);
    EnableWindow(GetDlgItem(dialog, Apply), !automatic);
    double percent = app->viewportRender.percent;
    if (!automatic && !ReadPercent(dialog, percent)) {
        SetDlgItemTextW(dialog, Dimensions, Text(app, L"请输入大于 0 的有效百分比。",
            L"Enter a percentage greater than 0.", L"0より大きい数値を入力してください。"));
        Show4KWarning(dialog, app, false);
        return;
    }
    const Size size = PreviewSize(app, automatic, percent);
    wchar_t text[180]{};
    swprintf_s(text, Text(app, L"渲染尺寸：%u × %u 像素", L"Render resolution: %u × %u px",
        L"描画解像度：%u × %u px"), size.width, size.height);
    SetDlgItemTextW(dialog, Dimensions, text);
    Show4KWarning(dialog, app, Above4K(size));
}
bool RefreshPreview(MMDApp* app) {
    auto* renderer = app->Renderer();
    if (!renderer || !renderer->device || FAILED(renderer->device->TestCooperativeLevel())) return false;
    auto* session = static_cast<PreviewSession*>(GetPropW(app->MainWindow(), kPreviewSession));
    if (session) session->lastFrame = Clock::now();
    const auto previous = app->state.messageSeen;
    const float delta = app->DeltaTime();
    app->state.messageSeen = 1;
    // The modal preview redraws the current scene without advancing playback,
    // physics or effect time. Its frame rate counts these real scene presents.
    app->DeltaTime() = 0;
    frame_rate::Tick(app);
    PrepareFrameTextOverlay(app);
    RenderFrameScene(app);
    app->state.messageSeen = previous;
    app->DeltaTime() = delta;
    // RenderFrameScene already cleared the margins before drawing the HUD.
    const RECT rect = FramePresentationRect(app);
    const bool presented = SUCCEEDED(renderer->device->Present(&rect, &rect,
        app->FloatingWindow() ? app->FloatingWindow() : app->MainWindow(), nullptr));
    frame_rate::Presented(app, presented);
    return presented;
}
LRESULT CALLBACK PreviewIdle(HWND window, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR data) {
    auto& session = *reinterpret_cast<PreviewSession*>(data);
    if (message == WM_ENTERIDLE && wp == MSGF_DIALOGBOX &&
        reinterpret_cast<HWND>(lp) == session.dialog && IsWindow(session.dialog)) {
        const double cap = session.app->FpsLimit();
        const double elapsed = std::chrono::duration<double>(Clock::now() - session.lastFrame).count();
        const double remaining = cap > 0 ? 1.0 / cap - elapsed : 0;
        const DWORD waitMs = remaining > 0 ? DWORD(remaining * 1000) : 0;
        if (waitMs) {
            // Let user input wake the wait; do not spin or use a 16-ms timer
            // that would impose its own frame-rate cap on an uncapped scene.
            MsgWaitForMultipleObjectsEx(0, nullptr, waitMs,
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else if (!RefreshPreview(session.app)) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        // DialogBox sends ENTERIDLE only once per empty queue. A no-op wakes
        // its message loop so the next idle iteration can render another frame.
        PostMessageW(session.dialog, WM_NULL, 0, 0);
        return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
}
bool ApplyValue(HWND dialog, MMDApp* app) {
    const bool automatic = IsDlgButtonChecked(dialog, Automatic) == BST_CHECKED;
    double percent = app->viewportRender.percent;
    if (!automatic && !ReadPercent(dialog, percent)) { UpdateReadout(dialog, app); return false; }
    std::wstring error;
    if (!Configure(app, automatic, percent, error)) {
        CheckDlgButton(dialog, Automatic, app->viewportRender.automatic ? BST_CHECKED : BST_UNCHECKED);
        UpdateReadout(dialog, app);
        MessageBoxW(dialog, error.c_str(), Title(app), MB_OK | MB_ICONWARNING);
        return false;
    }
    WritePercent(dialog, percent);
    UpdateReadout(dialog, app);
    RefreshPreview(app);
    return true;
}
}
INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<MMDApp*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        app = reinterpret_cast<MMDApp*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        if (auto* session = static_cast<PreviewSession*>(GetPropW(app->MainWindow(), kPreviewSession)))
            session->dialog = dialog;
        SetWindowTextW(dialog, Title(app));
        SetDlgItemTextW(dialog, Automatic, Text(app, L"自动：匹配当前窗口尺寸",
            L"Automatic: match window size", L"自動：ウィンドウサイズに合わせる"));
        SetDlgItemTextW(dialog, Apply, Text(app, L"应用", L"Apply", L"適用"));
        // The slider and Apply button commit immediately; this button only closes.
        SetDlgItemTextW(dialog, IDCANCEL, Text(app, L"关闭", L"Close", L"閉じる"));
        SendDlgItemMessageW(dialog, Slider, TBM_SETRANGE, TRUE, MAKELPARAM(25, 100));
        SendDlgItemMessageW(dialog, Slider, TBM_SETPAGESIZE, 0, 5);
        SendDlgItemMessageW(dialog, Percent, EM_SETLIMITTEXT, 120, 0);
        WritePercent(dialog, app->viewportRender.percent);
        CheckDlgButton(dialog, Automatic, app->viewportRender.automatic ? BST_CHECKED : BST_UNCHECKED);
        UpdateReadout(dialog, app);
        return TRUE;
    }
    if (!app) return FALSE;
    if (message == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(lp) == GetDlgItem(dialog, Warning)) {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(192, 0, 0));
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
    }
    if (message == WM_HSCROLL && reinterpret_cast<HWND>(lp) == GetDlgItem(dialog, Slider)) {
        if (!IsWindowEnabled(GetDlgItem(dialog, Slider))) return TRUE;
        if (LOWORD(wp) != TB_ENDTRACK) {
            WritePercent(dialog, double(SendDlgItemMessageW(dialog, Slider, TBM_GETPOS, 0, 0)));
            UpdateReadout(dialog, app);
        }
        // ENDTRACK covers mouse release and keyboard adjustment. Target
        // allocation never runs while the slider thumb is still held down.
        if (LOWORD(wp) == TB_ENDTRACK) ApplyValue(dialog, app);
        return TRUE;
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(wp)) {
    case Percent:
        if (HIWORD(wp) == EN_CHANGE) UpdateReadout(dialog, app);
        return TRUE;
    case Automatic:
        ApplyValue(dialog, app); return TRUE;
    case Apply:
    case IDOK:
        ApplyValue(dialog, app); return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL); return TRUE;
    }
    return FALSE;
}
void RefreshMenu(MMDApp* app) {
    if (build_profile::original || !app || !IsWindow(app->MainWindow())) return;
    const HMENU bar = GetMenu(app->MainWindow());
    const std::wstring caption = std::wstring(Title(app)) + L"…";
    for (int i = 0; i < GetMenuItemCount(bar); ++i) {
        HMENU view = GetSubMenu(bar, i);
        if (!view || GetMenuState(view, 212, MF_BYCOMMAND) == UINT(-1)) continue;
        if (GetMenuState(view, kCommand, MF_BYCOMMAND) == UINT(-1)) {
            for (int pos = 0; pos < GetMenuItemCount(view); ++pos) {
                if (GetMenuItemID(view, pos) == 212) {
                    InsertMenuW(view, pos + 1, MF_BYPOSITION | MF_STRING, kCommand, caption.c_str()); break;
                }
            }
        }
        ModifyMenuW(view, kCommand, MF_BYCOMMAND | MF_STRING, kCommand, caption.c_str());
        break;
    }
}
bool HandleCommand(MMDApp* app, unsigned command) {
    if (build_profile::original || command != kCommand) return false;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    PreviewSession session{app};
    const HWND owner = app->MainWindow();
    SetPropW(owner, kPreviewSession, &session);
    SetWindowSubclass(owner, PreviewIdle, kPreviewSubclass, reinterpret_cast<DWORD_PTR>(&session));
    UiDialogBoxParamW(static_cast<HINSTANCE>(app->HInstance()), MAKEINTRESOURCEW(kDialog),
        owner, DialogProc, reinterpret_cast<LPARAM>(app));
    RemoveWindowSubclass(owner, PreviewIdle, kPreviewSubclass);
    RemovePropW(owner, kPreviewSession);
    return true;
}
}
