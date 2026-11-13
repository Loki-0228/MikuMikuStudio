#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <algorithm>

#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_control_layout.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_scale.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "native_dialog_backdrop.hpp"
#include "settings_json.hpp"

namespace mikumikustudio {
void PanelPaint(MMDApp*);
}

namespace mikumikustudio::ui_scale {
namespace {

constexpr const char* kSection = "uiScaling";
constexpr int kMin = 100, kMax = 200;
constexpr UINT kApplyPreview = WM_APP + 0x61;
int currentPercent = kMin;
bool popupOpen = false;

const wchar_t* Text(MMDApp* app, const wchar_t* zh, const wchar_t* en, const wchar_t* ja) {
    return app->EnglishUI() == kUiChinese ? zh : app->EnglishUI() == kUiJapanese ? ja : en;
}

const wchar_t* Title(MMDApp* app) {
    return Text(app, L"界面缩放", L"Interface Scale", L"UI の拡大率");
}

HMENU HelpMenu(HWND window) {
    const HMENU bar = GetMenu(window);
    for (int i = 0; i < GetMenuItemCount(bar); ++i) {
        const HMENU popup = GetSubMenu(bar, i);
        if (popup && GetMenuState(popup, 201, MF_BYCOMMAND) != UINT(-1)) return popup;
    }
    return nullptr;
}

void ApplyPercent(MMDApp* app, int percent) {
    if (percent == currentPercent) return;
    currentPercent = percent;
    {
        ui::LayoutBatch batch;
        HandleWindowSize(app);
        batch.Commit();
    }
    if (percent == kMin) PanelPaint(app);
    if (app->state.waveEnabled) TimelineDrawTicks(app->TimelineStartFrame(), app->SidebarWidth());
    constexpr UINT repaint = RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN;
    RedrawWindow(app->MainWindow(), nullptr, nullptr, repaint);
    if (const HWND floating = app->FloatingWindow()) RedrawWindow(floating, nullptr, nullptr, repaint);
    if constexpr (!build_profile::original) curve_editor::RefreshInterfaceScale(percent);
    PresentViewportPreview(app);
}

struct Session {
    MMDApp* app;
    int initial, selected;
    bool previewPosted = false;
    int dragStart = kMin;
    bool dragging = false;
};

void UpdateReadout(HWND dialog, int percent) {
    wchar_t text[12]{};
    swprintf_s(text, L"%d%%", percent);
    SetDlgItemTextW(dialog, kReadout, text);
}

void PreviewSelection(HWND dialog, Session& session, int percent) {
    session.selected = percent;
    UpdateReadout(dialog, percent);
    if (!session.previewPosted) {
        session.previewPosted = PostMessageW(dialog, kApplyPreview, 0, 0) != FALSE;
        if (!session.previewPosted) ApplyPercent(session.app, percent);
    }
}

LRESULT CALLBACK ScaleSliderProc(HWND slider, UINT message, WPARAM wp, LPARAM lp,
                                UINT_PTR id, DWORD_PTR data) {
    auto& session = *reinterpret_cast<Session*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(slider, ScaleSliderProc, id);
        return DefSubclassProc(slider, message, wp, lp);
    }
    if (message == WM_LBUTTONDOWN && !session.dragging) {
        session.dragStart = session.selected;
        session.dragging = true;
        const auto result = DefSubclassProc(slider, message, wp, lp);
        if (GetCapture() != slider) session.dragging = false;
        return result;
    }
    if (message == WM_LBUTTONUP) session.dragging = false;
    if (session.dragging && (message == WM_CAPTURECHANGED || message == WM_CANCELMODE)) {
        session.dragging = false;
        const auto result = DefSubclassProc(slider, message, wp, lp);
        if (GetCapture() == slider) ReleaseCapture();
        SendMessageW(slider, TBM_SETPOS, TRUE, session.dragStart);
        PreviewSelection(GetParent(slider), session, session.dragStart);
        return result;
    }
    return DefSubclassProc(slider, message, wp, lp);
}

INT_PTR CALLBACK ScaleDialogProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto* session = reinterpret_cast<Session*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        session = reinterpret_cast<Session*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, Title(session->app));
        SetDlgItemTextW(dialog, IDOK, Text(session->app, L"确定", L"OK", L"OK"));
        SetDlgItemTextW(dialog, IDCANCEL, Text(session->app, L"取消", L"Cancel", L"キャンセル"));
        SendDlgItemMessageW(dialog, kSlider, TBM_SETRANGE, TRUE, MAKELPARAM(kMin, kMax));
        SendDlgItemMessageW(dialog, kSlider, TBM_SETTICFREQ, 25, 0);
        SendDlgItemMessageW(dialog, kSlider, TBM_SETPAGESIZE, 0, 5);
        SendDlgItemMessageW(dialog, kSlider, TBM_SETPOS, TRUE, session->initial);
        UpdateReadout(dialog, session->initial);
        if (!SetWindowSubclass(GetDlgItem(dialog, kSlider), ScaleSliderProc, 1,
                               reinterpret_cast<DWORD_PTR>(session))) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        // Keep the thumb and the popup in place throughout a preview.
        native_dialog_backdrop::Attach(dialog, session->initial);
        return TRUE;
    }
    if (!session) return FALSE;
    if (message == WM_CANCELMODE && session->dragging) {
        SendDlgItemMessageW(dialog, kSlider, WM_CANCELMODE, 0, 0);
        return TRUE;
    }
    if (message == WM_HSCROLL && reinterpret_cast<HWND>(lp) == GetDlgItem(dialog, kSlider)) {
        PreviewSelection(dialog, *session, static_cast<int>(SendDlgItemMessageW(dialog, kSlider, TBM_GETPOS, 0, 0)));
        return TRUE;
    }
    if (message == kApplyPreview) {
        session->previewPosted = false;
        ApplyPercent(session->app, session->selected);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND || LOWORD(wp) != IDOK) return FALSE;
    if (session->selected != session->initial &&
        system_settings::WriteSection(kSection, "{\"percent\":" + std::to_string(session->selected) + "}") !=
            system_settings::Result::Ok) {
        MessageBoxW(dialog, Text(session->app, L"无法保存界面缩放设置。", L"Could not save the UI scaling setting.",
            L"UI 拡大設定を保存できません。"), L"MMS", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    EndDialog(dialog, IDOK);
    return TRUE;
}

}  // namespace

int Percent() noexcept {
    return build_profile::original ? kMin : currentPercent;
}

int Pixels(int designPixels) noexcept {
    return MulDiv(designPixels, Percent(), 100);
}

void ApplyAtStartup() {
    if (build_profile::original) return;
    settings_json::Json section;
    system_settings::Result result{};
    currentPercent = kMin;
    if (settings_json::Read(kSection, section, result)) {
        const int value = settings_json::Integer(section, "percent", kMin);
        if (value >= kMin && value <= kMax) currentPercent = value;
    }
    curve_editor::RefreshInterfaceScale(currentPercent);
}

void RefreshMenu(MMDApp* app) {
    if (build_profile::original) return;
    const HMENU help = HelpMenu(app->MainWindow());
    if (!help) return;
    const auto caption = std::wstring(Title(app)) + L"…";
    if (GetMenuState(help, kMenuCommand, MF_BYCOMMAND) == UINT(-1))
        AppendMenuW(help, MF_STRING, kMenuCommand, caption.c_str());
    else
        ModifyMenuW(help, kMenuCommand, MF_BYCOMMAND | MF_STRING, kMenuCommand, caption.c_str());
}

bool HandleCommand(MMDApp* app, unsigned command) {
    if (build_profile::original || command != kMenuCommand) return false;
    if (popupOpen || app->RecordingWindow() || app->SceneMutationInProgress() || app->sceneRecoveryPending) return true;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&controls)) return true;
    struct Guard {
        Guard() { popupOpen = true; }
        ~Guard() { popupOpen = false; }
    } guard;
    Session session{app, Percent(), Percent()};
    const auto result = UiDialogBoxParamW(static_cast<HINSTANCE>(app->HInstance()), MAKEINTRESOURCEW(kDialog),
        app->MainWindow(), ScaleDialogProc, reinterpret_cast<LPARAM>(&session));
    if (IsWindow(app->MainWindow())) ApplyPercent(app, result == IDOK ? session.selected : session.initial);
    return true;
}

}  // namespace mikumikustudio::ui_scale
