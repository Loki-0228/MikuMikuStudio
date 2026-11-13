#include "playback_timing_ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/main_panel_layout.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/wave_audio_context.hpp"

#include <charconv>
#include <cmath>
#include <clocale>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace mikumikustudio::playback_timing_ui {
namespace {

constexpr UINT kPlaybackFps = 32922;
constexpr UINT kFollowDisplayCap = 32923;
constexpr UINT kInterpolate = 32924;
constexpr UINT kPhysicsSettings = 32925;

constexpr int kFps = 100;
constexpr int kErp = 101;
constexpr int kCfm = 102;
constexpr int kSubsteps = 103;
constexpr int kMultiplier = 104;
constexpr int kBaseRate = 105;
constexpr int kFollowPhysics = 106;
constexpr int kResetPhysics = 107;

struct Text {
    const wchar_t* zh;
    const wchar_t* en;
    const wchar_t* ja;
};

const wchar_t* Tr(const MMDApp& app, Text text) {
    return app.EnglishUI() == kUiChinese ? text.zh
        : app.EnglishUI() == kUiEnglish ? text.en : text.ja;
}

constexpr Text kPlaybackTitle{L"播放帧率", L"Playback frame rate", L"再生フレームレート"};
constexpr Text kPhysicsTitle{L"实时物理设置", L"Real-time physics settings", L"リアルタイム物理演算設定"};

bool SettingsAvailable(MMDApp& app) {
    return !app.PlaybackActive() && !app.RecordingWindow();
}

int Position(HMENU menu, UINT id) {
    const int count = GetMenuItemCount(menu);
    for (int pos = 0; pos < count; ++pos)
        if (GetMenuItemID(menu, pos) == id) return pos;
    return -1;
}

void SetItem(HMENU menu, UINT id, const wchar_t* text, bool enabled, bool checked = false) {
    ModifyMenuW(menu, id, MF_BYCOMMAND | MF_STRING |
        (enabled ? MF_ENABLED : MF_GRAYED) | (checked ? MF_CHECKED : MF_UNCHECKED), id, text);
}

void InsertSeparator(HMENU menu, int pos) {
    InsertMenuW(menu, pos, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
}

void InsertItem(HMENU menu, int pos, UINT id) {
    InsertMenuW(menu, pos, MF_BYPOSITION | MF_STRING, id, L"");
}

struct Dialog {
    MMDApp& app;
    bool physics;
};

void Control(HWND dialog, const wchar_t* type, const wchar_t* text, DWORD style,
             int id, int x, int y, int width, int height, DWORD exStyle = 0) {
    RECT rect{x, y, x + width, y + height};
    MapDialogRect(dialog, &rect);
    const HWND control = CreateWindowExW(exStyle, type, text, WS_CHILD | WS_VISIBLE | style,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
}

void Edit(HWND dialog, int id, const wchar_t* label, const wchar_t* value, int y, bool integer) {
    Control(dialog, L"STATIC", label, SS_LEFT, -1, 10, y + 2, 135, 12);
    Control(dialog, L"EDIT", value, WS_TABSTOP | ES_AUTOHSCROLL | (integer ? ES_NUMBER : 0),
            id, 149, y, 61, 14, WS_EX_CLIENTEDGE);
    SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, 32, 0);
}

std::wstring Number(float value) {
    char text[32];
    const auto result = std::to_chars(text, text + sizeof text, value);
    return std::wstring(text, result.ptr);
}

void RefreshBaseRate(HWND window) {
    EnableWindow(GetDlgItem(window, kBaseRate),
                 IsDlgButtonChecked(window, kFollowPhysics) != BST_CHECKED);
}

void SetPhysicsFields(HWND window, const playback_timing::Settings& settings) {
    const auto& physics = settings.physics;
    SetDlgItemTextW(window, kErp, Number(physics.erp).c_str());
    SetDlgItemTextW(window, kCfm, Number(physics.cfm).c_str());
    SetDlgItemInt(window, kSubsteps, physics.substeps, FALSE);
    SetDlgItemInt(window, kMultiplier, physics.rateMultiplier, FALSE);
    SetDlgItemInt(window, kBaseRate, physics.baseRateHz, FALSE);
    CheckDlgButton(window, kFollowPhysics, settings.followPlaybackFps ? BST_CHECKED : BST_UNCHECKED);
    RefreshBaseRate(window);
}

bool ReadInteger(HWND dialog, int id, int minimum, int maximum, int& value) {
    char text[34]{};
    GetDlgItemTextA(dialog, id, text, static_cast<int>(std::size(text)));
    return frame_limits::Parse(text, value) && value >= minimum && value <= maximum;
}

bool ReadReal(HWND dialog, int id, float maximum, float& value) {
    wchar_t text[34]{};
    GetDlgItemTextW(dialog, id, text, static_cast<int>(std::size(text)));
    const auto locale = _create_locale(LC_NUMERIC, "C");
    if (!locale) return false;
    wchar_t* end = nullptr;
    const double parsed = _wcstod_l(text, &end, locale);
    _free_locale(locale);
    if (end == text || *end != L'\0' || !std::isfinite(parsed) || parsed < 0 || parsed > maximum)
        return false;
    value = static_cast<float>(parsed);
    return true;
}

bool Reject(HWND window, Dialog& dialog, int id, Text message) {
    MessageBoxW(window, Tr(dialog.app, message),
        Tr(dialog.app, dialog.physics ? kPhysicsTitle : kPlaybackTitle), MB_OK | MB_ICONWARNING);
    SetFocus(GetDlgItem(window, id));
    SendDlgItemMessageW(window, id, EM_SETSEL, 0, -1);
    return false;
}

bool Apply(HWND window, Dialog& dialog) {
    auto& settings = dialog.app.PlaybackTiming();
    if (!SettingsAvailable(dialog.app)) return false;
    auto proposed = settings;
    if (!dialog.physics) {
        if (!ReadInteger(window, kFps, 1, 240, proposed.playbackFps) || !playback_timing::Valid(proposed))
            return Reject(window, dialog, kFps,
                {L"请输入 1 到 240 之间的整数帧率。", L"Enter a whole frame rate from 1 to 240.",
                 L"1～240 の整数を入力してください。"});
        settings.playbackFps = proposed.playbackFps;
        return true;
    }

    auto& physics = proposed.physics;
    proposed.followPlaybackFps = IsDlgButtonChecked(window, kFollowPhysics) == BST_CHECKED;
    if (!ReadReal(window, kErp, 1.f, physics.erp))
        return Reject(window, dialog, kErp,
            {L"ERP 必须在 0 到 1 之间。", L"Enter an ERP from 0 to 1.", L"ERP は 0～1 を入力してください。"});
    if (!ReadReal(window, kCfm, 1000000.f, physics.cfm))
        return Reject(window, dialog, kCfm,
            {L"CFM 必须在 0 到 1000000 之间。", L"Enter a CFM from 0 to 1000000.",
             L"CFM は 0～1000000 を入力してください。"});
    if (!ReadInteger(window, kSubsteps, 1, 16, physics.substeps))
        return Reject(window, dialog, kSubsteps,
            {L"请输入 1 到 16 个子步。", L"Enter 1 to 16 substeps.", L"サブステップ数は 1～16 を入力してください。"});
    if (!ReadInteger(window, kMultiplier, 1, 16, physics.rateMultiplier))
        return Reject(window, dialog, kMultiplier,
            {L"请输入 1 到 16 之间的整数倍率。", L"Enter a whole multiplier from 1 to 16.",
             L"計算倍率は 1～16 の整数を入力してください。"});
    if (!proposed.followPlaybackFps && !ReadInteger(window, kBaseRate, 1, 1000, physics.baseRateHz))
        return Reject(window, dialog, kBaseRate,
            {L"请输入 1 到 1000 Hz 之间的整数基准频率。", L"Enter a whole base rate from 1 to 1000 Hz.",
             L"基本周波数は 1～1000 Hz の整数を入力してください。"});
    if (!playback_timing::Valid(proposed)) return false;
    settings = std::move(proposed);
    return true;
}

void Init(HWND window, Dialog& dialog) {
    auto& app = dialog.app;
    const auto& settings = app.PlaybackTiming();
    SetWindowTextW(window, Tr(app, dialog.physics ? kPhysicsTitle : kPlaybackTitle));
    if (dialog.physics) {
        Edit(window, kErp, Tr(app, {L"误差修正（ERP）", L"Error reduction (ERP)", L"誤差補正（ERP）"}),
             L"", 10, false);
        Edit(window, kCfm, Tr(app, {L"约束柔化（CFM）", L"Constraint softness (CFM)", L"拘束の柔らかさ（CFM）"}),
             L"", 29, false);
        Edit(window, kSubsteps, Tr(app, {L"物理子步数", L"Physics substeps", L"物理サブステップ数"}),
             L"", 48, true);
        Edit(window, kMultiplier, Tr(app, {L"内部计算倍率", L"Simulation rate multiplier", L"内部計算倍率"}),
             L"", 67, true);
        Edit(window, kBaseRate, Tr(app, {L"基准频率（Hz）", L"Base rate (Hz)", L"基本周波数（Hz）"}),
             L"", 86, true);
        Control(window, L"BUTTON", Tr(app, {L"跟随播放帧率", L"Follow playback frame rate", L"再生フレームレートに従う"}),
                WS_TABSTOP | BS_AUTOCHECKBOX, kFollowPhysics, 10, 105, 200, 13);
        SetPhysicsFields(window, settings);
    } else {
        Edit(window, kFps, Tr(app, {L"播放帧率（FPS）", L"Playback frame rate (FPS)", L"再生フレームレート（FPS）"}),
             std::to_wstring(settings.playbackFps).c_str(), 10, true);
    }
    const int buttonY = dialog.physics ? 127 : 33;
    if (dialog.physics) {
        Control(window, L"BUTTON", Tr(app, {L"恢复默认", L"Restore defaults", L"既定値に戻す"}),
                WS_TABSTOP | BS_PUSHBUTTON, kResetPhysics, 10, buttonY, 86, 15);
    }
    Control(window, L"BUTTON", Tr(app, {L"确定", L"OK", L"OK"}),
            WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 102, buttonY, 50, 15);
    Control(window, L"BUTTON", Tr(app, {L"取消", L"Cancel", L"キャンセル"}),
            WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, 160, buttonY, 50, 15);
    SendMessageW(window, DM_SETDEFID, IDOK, 0);
}

INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        Init(window, *reinterpret_cast<Dialog*>(lParam));
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!dialog) return FALSE;
    switch (LOWORD(wParam)) {
    case IDOK:
        if (Apply(window, *dialog)) EndDialog(window, IDOK);
        return TRUE;
    case IDCANCEL:
        EndDialog(window, IDCANCEL);
        return TRUE;
    case kFollowPhysics:
        RefreshBaseRate(window);
        return TRUE;
    case kResetPhysics:
        if (!dialog->physics || HIWORD(wParam) != BN_CLICKED) return FALSE;
        SetPhysicsFields(window, playback_timing::Settings{});
        return TRUE;
    default:
        return FALSE;
    }
}

void ShowDialog(MMDApp& app, bool physics) {
    Dialog dialog{app, physics};
    const int previousFps = app.PlaybackTiming().playbackFps;
    DLGTEMPLATE layout{};
    layout.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER;
    layout.cx = 220;
    layout.cy = physics ? 152 : 58;
    std::vector<WORD> data((sizeof(layout) + 1) / 2);
    std::memcpy(data.data(), &layout, sizeof(layout));
    data.insert(data.end(), {0, 0, 0, 9});
    constexpr wchar_t font[] = L"Segoe UI";
    data.insert(data.end(), std::begin(font), std::end(font));
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(data.data()),
                           app.MainWindow(), DialogProc, reinterpret_cast<LPARAM>(&dialog));
    if (app.PlaybackTiming().playbackFps != previousFps) {
        if (app.Renderer() && app.PanelDC()) PanelPaint(&app);
        if (app.WaveEnabled() && app.Audio() && app.Audio()->timelineDC) {
            TimelineDrawTicks(app.TimelineStartFrame(), app.SidebarWidth());
            const RECT strip = TimelineRulerDamage(&app);
            InvalidateRect(app.MainWindow(), &strip, FALSE);
        }
    }
}

}  // namespace

void RefreshMenu(MMDApp* app) {
    if (!app || !app->MainWindow()) return;
    const HMENU bar = GetMenu(app->MainWindow());
    if (!bar) return;
    const auto& settings = app->PlaybackTiming();
    const bool enabled = SettingsAvailable(*app);

    const HMENU view = GetSubMenu(bar, 2);
    if (view) {
        const int capEnd = Position(view, 236);
        const int cpuPower = Position(view, 299);
        if (capEnd >= 0 && cpuPower > capEnd && Position(view, kPlaybackFps) < 0) {
            InsertItem(view, capEnd + 1, kFollowDisplayCap);
            const int pos = cpuPower + 1;
            InsertItem(view, pos, kPlaybackFps);
            InsertSeparator(view, pos + 1);
            InsertItem(view, pos + 2, kInterpolate);
            InsertSeparator(view, pos + 3);
        }
        SetItem(view, kFollowDisplayCap,
            Tr(*app, {L"采用播放帧率作为上限", L"Use playback frame rate as limit", L"再生フレームレートを上限にする"}),
            enabled, settings.displayFollowsPlaybackFps);
        SetItem(view, kPlaybackFps,
            Tr(*app, {L"播放帧率…", L"Playback frame rate…", L"再生フレームレート…"}), enabled);
        SetItem(view, kInterpolate,
            Tr(*app, {L"连续帧超帧率优化", L"Smooth playback above timeline FPS", L"再生フレーム間を補間する"}),
            enabled, settings.interpolate);
        for (const auto& [id, limit] : {std::pair{234u, 1000.f}, {235u, 30.f}, {236u, 60.f}})
            CheckMenuItem(view, id, MF_BYCOMMAND |
                (!settings.displayFollowsPlaybackFps && app->state.fpsLimit == limit ? MF_CHECKED : MF_UNCHECKED));
    }

    const HMENU physics = GetSubMenu(bar, 5);
    if (physics) {
        const int reset = Position(physics, 268);
        if (reset >= 0 && Position(physics, kPhysicsSettings) < 0)
            InsertItem(physics, reset + 1, kPhysicsSettings);
        SetItem(physics, kPhysicsSettings,
            Tr(*app, {L"实时物理设置…", L"Real-time physics settings…", L"リアルタイム物理演算設定…"}), enabled);
    }
}

bool HandleCommand(MMDApp* app, unsigned command) {
    if (!app || command < kPlaybackFps || command > kPhysicsSettings) return false;
    auto& settings = app->PlaybackTiming();
    if (SettingsAvailable(*app)) {
        if (command == kFollowDisplayCap) settings.displayFollowsPlaybackFps = true;
        else if (command == kInterpolate) settings.interpolate = !settings.interpolate;
        else ShowDialog(*app, command == kPhysicsSettings);
    }
    RefreshMenu(app);
    return true;
}
}
