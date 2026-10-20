#include "mikumikustudio/background_save.hpp"
#pragma once
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <initializer_list>

namespace mikumikustudio::export_save_prompt {
inline constexpr wchar_t kExpanded[] = L"MMS.ExportSavePromptExpanded";

inline void Refresh(HWND dialog, MMDApp* app) {
    const HWND warning = GetDlgItem(dialog, panel::kExportUnsavedWarning);
    const HWND save = GetDlgItem(dialog, panel::kExportSaveProjectButton);
    // Match the ordinary close confirmation: a pristine empty project does
    // not need saving merely because it has no filename yet.
    const bool show = IsDlgButtonChecked(dialog, panel::kAviCloseAfterExportCheckbox) == BST_CHECKED &&
        app->SceneModified();
    EnableWindow(save, !SceneSaveInProgress());
    const bool expanded = GetPropW(dialog, kExpanded) != nullptr;
    if (!show && GetFocus() == save)
        SendMessageW(dialog, WM_NEXTDLGCTL,
            reinterpret_cast<WPARAM>(GetDlgItem(dialog, panel::kAviCloseAfterExportCheckbox)), TRUE);
    for (const HWND control : {warning, save})
        SetWindowPos(control, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (show == expanded) return;
    // Resources reserve 34 dialog units for this row. Collapse it when no
    // action is needed, using dialog units so the controls scale with DPI.
    RECT gap{0, 0, 0, 34}; MapDialogRect(dialog, &gap);
    const int delta = show ? gap.bottom : -gap.bottom;
    for (const int id : {IDOK, IDCANCEL}) {
        const HWND button = GetDlgItem(dialog, id);
        RECT rect{}; GetWindowRect(button, &rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(button, nullptr, rect.left, rect.top + delta, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT rect{}; GetWindowRect(dialog, &rect);
    SetWindowPos(dialog, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top + delta,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (show) SetPropW(dialog, kExpanded, reinterpret_cast<HANDLE>(1));
    else RemovePropW(dialog, kExpanded);
    InvalidateRect(dialog, nullptr, TRUE);
}

inline void Initialize(HWND dialog, MMDApp* app) {
    const wchar_t* warning[] = {
        L"未保存の変更があるため、出力後に自動終了できません。",
        L"Unsaved changes: MMS cannot close automatically after export.",
        L"有未保存的改动，导出后无法自动关闭 MMS。"};
    const wchar_t* save[] = {L"保存", L"Save project", L"保存工程"};
    const unsigned language = app->EnglishUI() <= 2 ? app->EnglishUI() : 1;
    SetDlgItemTextW(dialog, panel::kExportUnsavedWarning, warning[language]);
    SetDlgItemTextW(dialog, panel::kExportSaveProjectButton, save[language]);
    SetPropW(dialog, kExpanded, reinterpret_cast<HANDLE>(1));
    Refresh(dialog, app);
}

inline INT_PTR HandleMessage(HWND dialog, MMDApp* app, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CTLCOLORSTATIC &&
        reinterpret_cast<HWND>(lp) == GetDlgItem(dialog, panel::kExportUnsavedWarning)) {
        const auto dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(192, 32, 32));
        SetBkColor(dc, GetSysColor(COLOR_3DFACE));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
    }
    if (message == WM_TIMER && wp == background_save::kStatusTimer) {
        background_save::HandleMessage(app, message, wp);
        Refresh(dialog, app);
        if (!SceneSaveInProgress()) KillTimer(dialog, background_save::kStatusTimer);
        return TRUE;
    }
    if (message == WM_NCDESTROY) {
        KillTimer(dialog, background_save::kStatusTimer);
        RemovePropW(dialog, kExpanded);
    }
    if (message != WM_COMMAND || HIWORD(wp) != BN_CLICKED) return FALSE;
    if (LOWORD(wp) == panel::kExportSaveProjectButton) {
        // Keep Save As owned by the active export dialog, not its disabled
        // main window. Do not dismiss the export dialog or reset its settings.
        if (app->EnvFileName()[0]) background_save::Request(app, app->EnvFileName());
        else SaveSceneAs(app, dialog);
        SetTimer(dialog, background_save::kStatusTimer, 100, nullptr);
        Refresh(dialog, app);
        return TRUE;
    }
    if (LOWORD(wp) == panel::kAviCloseAfterExportCheckbox) {
        Refresh(dialog, app);
        return TRUE;
    }
    return FALSE;
}
} // namespace mikumikustudio::export_save_prompt
