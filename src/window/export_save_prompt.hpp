#include "mikumikustudio/background_save.hpp"
#pragma once
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/ui_control_layout.hpp"
#include <array>
#include <initializer_list>

namespace mikumikustudio::export_save_prompt {
inline constexpr wchar_t kExpanded[] = L"MMS.ExportSavePromptExpanded";
inline constexpr wchar_t kUpdating[] = L"MMS.ExportSavePromptUpdating";

inline void Refresh(HWND dialog, MMDApp* app) {
    if (!app || !IsWindow(dialog) || GetPropW(dialog, kUpdating)) return;
    const HWND warning = GetDlgItem(dialog, panel::kExportUnsavedWarning);
    const HWND save = GetDlgItem(dialog, panel::kExportSaveProjectButton);
    const std::array<HWND, 4> children{warning, save, GetDlgItem(dialog, IDOK), GetDlgItem(dialog, IDCANCEL)};
    for (const auto child : children) if (!IsWindow(child)) return;
    // Match the ordinary close confirmation: a pristine empty project does
    // not need saving merely because it has no filename yet.
    const bool show = IsDlgButtonChecked(dialog, panel::kAviCloseAfterExportCheckbox) == BST_CHECKED &&
        app->SceneModified();
    const bool enabled = !SceneSaveInProgress();
    const bool enableChanged = (IsWindowEnabled(save) != FALSE) != enabled;
    const bool expanded = GetPropW(dialog, kExpanded) != nullptr;
    const auto visible = [](HWND window) { return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0; };
    const bool visibilityChanged = visible(warning) != show || visible(save) != show;
    if (show == expanded && !visibilityChanged && !enableChanged) return;
    if (!SetPropW(dialog, kUpdating, reinterpret_cast<HANDLE>(1))) return;
    struct Updating {
        HWND dialog;
        ~Updating() { if (IsWindow(dialog)) RemovePropW(dialog, kUpdating); }
    } updating{dialog};
    try {
        const WindowDpiScope dpi(dialog);
        std::array<RECT, 4> before{}, after{};
        std::array<bool, 4> wasVisible{}, moved{};
        std::array<UINT, 4> flags{};
        RECT dialogBefore{}, gap{0, 0, 0, 34};
        if (!GetWindowRect(dialog, &dialogBefore) || !MapDialogRect(dialog, &gap)) return;
        const int delta = show == expanded ? 0 : show ? gap.bottom : -gap.bottom;
        const HWND owner = GetParent(dialog);
        const RECT ownerBefore = ui::layout_detail::ActualRect(owner, dialog);
        int count = 0;
        for (unsigned i = 0; i < children.size(); ++i) {
            before[i] = after[i] = ui::layout_detail::ActualRect(dialog, children[i]);
            wasVisible[i] = visible(children[i]);
            if (i >= 2) { after[i].top += delta; after[i].bottom += delta; }
            flags[i] = ui::layout_detail::kMoveFlags | SWP_NOSIZE;
            if (EqualRect(&before[i], &after[i])) flags[i] |= SWP_NOMOVE;
            if (i < 2 && wasVisible[i] != show) flags[i] |= show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
            moved[i] = (flags[i] & SWP_NOMOVE) == 0 || (flags[i] & (SWP_SHOWWINDOW | SWP_HIDEWINDOW)) != 0;
            count += moved[i];
        }
        // Allocate and attach every guard before changing native state. The
        // dialog is an owned top-level window: its size uses screen units,
        // whereas all four deferred child positions use its client units.
        std::vector<ui::LayoutChange> changes;
        changes.reserve(5);
        ui::layout_detail::PaintCloseGuard dialogClose, ownerClose;
        if (!dialogClose.Attach(dialog) || (owner && !ownerClose.Attach(owner))) return;
        std::array<ui::layout_detail::PendingPaint, 5> guards;
        if (!guards[4].Attach(dialog)) return;
        for (unsigned i = 0; i < children.size(); ++i) if (!guards[i].Attach(children[i])) return;
        if ((!show || !enabled) && GetFocus() == save)
            SendMessageW(dialog, WM_NEXTDLGCTL,
                reinterpret_cast<WPARAM>(GetDlgItem(dialog, panel::kAviCloseAfterExportCheckbox)), TRUE);
        if (enableChanged) EnableWindow(save, enabled);
        bool applied = true;
        if (count) {
            HDWP deferred = BeginDeferWindowPos(count);
            for (unsigned i = 0; deferred && i < children.size(); ++i) if (moved[i])
                deferred = DeferWindowPos(deferred, children[i], nullptr, after[i].left, after[i].top, 0, 0, flags[i]);
            if (!deferred || !EndDeferWindowPos(deferred)) {
                for (unsigned i = 0; i < children.size(); ++i) if (moved[i])
                    if (!SetWindowPos(children[i], nullptr, after[i].left, after[i].top, 0, 0, flags[i])) applied = false;
            }
        }
        if (delta && !SetWindowPos(dialog, nullptr, 0, 0, dialogBefore.right - dialogBefore.left,
                dialogBefore.bottom - dialogBefore.top + delta, ui::layout_detail::kMoveFlags | SWP_NOMOVE)) applied = false;
        if (applied && show != expanded) {
            if (show) applied = SetPropW(dialog, kExpanded, reinterpret_cast<HANDLE>(1)) != FALSE;
            else applied = RemovePropW(dialog, kExpanded) != nullptr;
        }
        if (!applied) {
            // Retain the previous row state on a failed native move, and
            // repaint actual bounds even if a rollback call also fails.
            bool restored = true;
            for (unsigned i = 0; i < children.size(); ++i) if (moved[i])
                if (!SetWindowPos(children[i], nullptr, before[i].left, before[i].top, 0, 0,
                        ui::layout_detail::kMoveFlags | SWP_NOSIZE | (wasVisible[i] ? SWP_SHOWWINDOW : SWP_HIDEWINDOW))) restored = false;
            if (delta && !SetWindowPos(dialog, nullptr, 0, 0, dialogBefore.right - dialogBefore.left,
                    dialogBefore.bottom - dialogBefore.top, ui::layout_detail::kMoveFlags | SWP_NOMOVE)) restored = false;
            runtime_log::Write("Export save prompt layout failed; prior bounds restored=%u", restored ? 1u : 0u);
        }
        RECT damage{};
        for (unsigned i = 0; i < children.size(); ++i) if (moved[i] || (i == 1 && enableChanged)) {
            const auto actual = ui::layout_detail::ActualRect(dialog, children[i]);
            changes.push_back({children[i], dialog, before[i], actual});
            UnionRect(&damage, &damage, &before[i]); UnionRect(&damage, &damage, &actual);
        }
        if (delta) {
            // This entry carries only the exposed owner's damage. The owned
            // dialog's nonclient frame is invalidated separately below.
            changes.push_back({nullptr, owner, ownerBefore, ui::layout_detail::ActualRect(owner, dialog)});
            RECT client{}; GetClientRect(dialog, &client);
            damage.left = 0; damage.right = client.right; damage.bottom = client.bottom;
        }
        for (auto& guard : guards) guard.Release();
        // Native dialog chrome needs its normal erase inside the final paint;
        // repaint only the changed footer, not unrelated fields above it.
        if (delta || visibilityChanged)
            RedrawWindow(dialog, &damage, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN | (delta ? RDW_FRAME : 0));
        ui::RepaintLayout(dialog, changes, damage);
        // Completion hooks may have pumped a timer during native layout.
        // Reconcile once after this transaction rather than nesting a resize.
        if (IsWindow(dialog) && (show != (IsDlgButtonChecked(dialog, panel::kAviCloseAfterExportCheckbox) == BST_CHECKED &&
                app->SceneModified()) || enabled != !SceneSaveInProgress()))
            PostMessageW(dialog, WM_TIMER, background_save::kStatusTimer, 0);
    } catch (...) {
        runtime_log::Write("Export save prompt layout update failed");
    }
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
        RemovePropW(dialog, kUpdating);
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
