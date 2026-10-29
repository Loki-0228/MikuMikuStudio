#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/sequence_scatter.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstdio>
#include <cstring>

namespace mikumikustudio::sequence_scatter {
namespace {
struct Text { const wchar_t* jp; const wchar_t* en; const wchar_t* zh; };
const wchar_t* Tr(unsigned language, Text t) { return language == 0 ? t.jp : language == 2 ? t.zh : t.en; }
constexpr Text title{L"選択フレーム列を散布", L"Scatter selected frame sequence", L"选中帧序列散布"};
constexpr Text selection{L"モデルを選び、異なるフレームのボーン・表情キーを2個以上選択してください。再生中は使用できません。",
    L"Select a model and bone or expression keys on at least two different frames. Stop playback before using this tool.",
    L"请选中模型，并在时间轴上选择至少两个不同帧的骨骼或表情关键帧。使用前请停止播放。"};
struct Dialog {
    MMDApp* app; Source source; Plan plan; Settings settings;
    bool ready = false, prepared = false;
};
void Label(HWND window, const wchar_t* text, int id, int x, int y, int w, int h,
           const wchar_t* type = L"STATIC", DWORD style = 0) {
    RECT rect{x, y, x + w, y + h}; MapDialogRect(window, &rect);
    HWND control = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, text,
        WS_CHILD | WS_VISIBLE | style, rect.left, rect.top, rect.right-rect.left, rect.bottom-rect.top,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, SendMessageW(window, WM_GETFONT, 0, 0), TRUE);
    if (wcscmp(type, L"EDIT") == 0) SendMessageW(control, EM_SETLIMITTEXT, 24, 0);
}
bool Number(HWND window, int id, double& value) {
    wchar_t text[64]{}; GetDlgItemTextW(window, id, text, 64);
    wchar_t* end = nullptr; value = std::wcstod(text, &end);
    if (end == text) return false;
    while (iswspace(*end)) ++end;
    return *end == 0 && std::isfinite(value);
}
void Status(HWND window, Dialog& d, const wchar_t* text) {
    SetDlgItemTextW(window, 5202, text);
    EnableWindow(GetDlgItem(window, IDOK), d.prepared && d.plan.copies > 0);
}
bool Preview(HWND window, Dialog& d, bool reroll) {
    d.prepared = false;
    double first, last;
    if (!Number(window, 5101, first) || !Number(window, 5102, last) || first < 0 || last < first ||
        last > frame_limits::maximum || std::floor(first) != first || std::floor(last) != last ||
        !Number(window, 5103, d.settings.gapMean) || !Number(window, 5104, d.settings.gapVariance) ||
        !Number(window, 5105, d.settings.speedMean) || !Number(window, 5106, d.settings.speedVariance) ||
        d.settings.gapMean < 1 || d.settings.gapMean > frame_limits::maximum ||
        d.settings.gapVariance < 0 || d.settings.gapVariance > 1e12 ||
        d.settings.speedMean < .05 || d.settings.speedMean > 20 ||
        d.settings.speedVariance < 0 || d.settings.speedVariance > 100) {
        Status(window, d, Tr(d.app->EnglishUI(), {L"範囲は0以上の整数、間隔は1以上、速度は0.05～20倍。分散は間隔0～10¹²、速度0～100にしてください。",
            L"Use whole frames ≥ 0, gap ≥ 1 and speed 0.05–20×. Variance limits: gap 0–10¹²; speed 0–100.",
            L"帧范围需为非负整数，间隔至少1帧，速度为0.05–20倍。间隔方差范围0–10¹²，速度方差范围0–100。"}));
        return false;
    }
    d.settings.first = static_cast<int>(first); d.settings.last = static_cast<int>(last);
    if (reroll) ++d.settings.seed;
    std::string error;
    if (!Prepare(d.source, d.settings, d.plan, error)) {
        Status(window, d, Tr(d.app->EnglishUI(), {L"散布を作成できません。範囲を短くするか間隔を広げて再試行してください。",
            L"Could not prepare placements. Shorten the range or increase the gap and try again.",
            L"无法生成散布结果。请缩短目标范围或增大间隔后重试。"}));
        return false;
    }
    d.prepared = true;
    wchar_t summary[512]{};
    swprintf_s(summary, Tr(d.app->EnglishUI(), {L"挿入予定：%zu回（%zuキー）\nスキップ：既存キーと重複 %zu回 ／ 高速すぎる %zu回",
        L"Preview: %zu copies (%zu keys)\nSkipped: %zu conflicts / %zu with keys compressed onto one frame",
        L"预计插入 %zu 次，共 %zu 个关键帧\n跳过：与已有关键帧冲突 %zu 次，速度过快导致重帧 %zu 次"}),
        d.plan.copies, d.plan.keys, d.plan.conflicts, d.plan.compressed);
    Status(window, d, summary); return true;
}
INT_PTR CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        d = reinterpret_cast<Dialog*>(l); SetWindowLongPtrW(window, DWLP_USER, l);
        const unsigned language = d->app->EnglishUI();
        SetWindowTextW(window, Tr(language, title));
        wchar_t source[256]{};
        swprintf_s(source, Tr(language, {L"選択したキー：%d～%dフレーム\n%zuボーン・%zu表情 ／ %zuキー",
            L"Selected sequence: frames %d–%d\n%zu bones / %zu expressions / %zu keys",
            L"选中的片段：第 %d–%d 帧\n%zu 个骨骼、%zu 个表情，共 %zu 个关键帧"}),
            d->source.first, d->source.last, d->source.bones.size(), d->source.channels.size(), d->source.keyCount);
        Label(window, source, 5001, 10, 8, 280, 24);
        Label(window, Tr(language, {L"散布範囲", L"Destination range", L"目标帧范围"}), 5002, 10, 39, 124, 16);
        Label(window, L"", 5101, 138, 36, 66, 16, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL);
        Label(window, L"—", 5003, 209, 39, 12, 16);
        Label(window, L"", 5102, 224, 36, 66, 16, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL);
        Label(window, Tr(language, {L"平均", L"Mean", L"均值"}), 5004, 138, 58, 66, 12);
        Label(window, Tr(language, {L"分散", L"Variance", L"方差"}), 5005, 224, 58, 66, 12);
        Label(window, Tr(language, {L"コピー間隔（フレーム）", L"Gap between copies (frames)", L"片段间隔（帧）"}), 5006, 10, 77, 124, 16);
        Label(window, L"90", 5103, 138, 74, 66, 16, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL);
        Label(window, L"225", 5104, 224, 74, 66, 16, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL);
        Label(window, Tr(language, {L"再生速度（倍）", L"Playback speed (×)", L"播放速度（倍）"}), 5007, 10, 97, 124, 16);
        Label(window, L"1", 5105, 138, 94, 66, 16, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL);
        Label(window, L"0.04", 5106, 224, 94, 66, 16, L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL);
        Label(window, Tr(language, {L"分散0で固定。元のキーは保持し、重複や範囲外のコピーはスキップします。\n正規乱数：間隔1フレーム以上、速度0.05～20倍。",
            L"Variance 0 is fixed. Source keys stay intact; conflicts and incomplete copies are skipped.\nNormal randomness: gap ≥ 1 frame; speed 0.05–20×.",
            L"方差0为固定值。保留原片段，跳过冲突和不完整的片段。\n正态随机：间隔至少1帧，速度0.05–20倍。"}), 5008, 10, 120, 280, 30);
        Label(window, L"", 5202, 10, 156, 280, 28);
        Label(window, Tr(language, {L"再ランダム化", L"Preview / reroll", L"预览 / 重新随机"}), 5201, 10, 194, 94, 19, L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON);
        Label(window, Tr(language, {L"散布", L"Scatter", L"散布"}), IDOK, 150, 194, 64, 19, L"BUTTON", WS_TABSTOP | BS_DEFPUSHBUTTON);
        Label(window, Tr(language, {L"キャンセル", L"Cancel", L"取消"}), IDCANCEL, 222, 194, 68, 19, L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON);
        SetDlgItemInt(window, 5101, std::min(d->source.last + 1, frame_limits::maximum), FALSE);
        SetDlgItemInt(window, 5102, std::min<std::int64_t>(std::int64_t(d->source.last) + 300, frame_limits::maximum), FALSE);
        d->settings.seed = GetTickCount(); d->ready = true; Preview(window, *d, false);
        RECT owner{}, rect{}; GetWindowRect(d->app->MainWindow(), &owner); GetWindowRect(window, &rect);
        SetWindowPos(window, nullptr, owner.left + (owner.right-owner.left-(rect.right-rect.left))/2,
            owner.top + (owner.bottom-owner.top-(rect.bottom-rect.top))/2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        return TRUE;
    }
    if (!d) return FALSE;
    if (message == WM_COMMAND) {
        const int id = LOWORD(w);
        if (id == IDCANCEL) { EndDialog(window, IDCANCEL); return TRUE; }
        if (id >= 5101 && id <= 5106 && HIWORD(w) == EN_CHANGE && d->ready) {
            d->prepared = false;
            Status(window, *d, Tr(d->app->EnglishUI(), {L"設定が変わりました。「再ランダム化」で確認してください。",
                L"Settings changed. Click Preview / reroll to update the result.", L"参数已修改。请点击「预览 / 重新随机」更新结果。"})); return TRUE;
        }
        if (id == 5201) { Preview(window, *d, true); return TRUE; }
        if (id == IDOK && d->prepared && d->plan.copies) {
            std::string error;
            if (Apply(*d->app, d->source, d->plan, error)) EndDialog(window, IDOK);
            else { d->prepared = false; Status(window, *d, Tr(d->app->EnglishUI(), {
                L"挿入できませんでした。キーは変更されていません。閉じてキーを選び直してください。",
                L"No keys were inserted. Close this window and select the sequence again.",
                L"未能插入，关键帧未改变。请关闭窗口，重新选择片段后再试。"})); }
            return TRUE;
        }
    }
    if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
    return FALSE;
}
}
void RefreshMenu(MMDApp* app) {
    if (!app || !app->MainWindow()) return;
    const HMENU edit = GetSubMenu(GetMenu(app->MainWindow()), 1);
    if (!edit) return;
    if (GetMenuState(edit, kMenuCommand, MF_BYCOMMAND) == UINT(-1)) {
        // Insert after the existing expression multiplier, using its command ID.
        for (int i = 0; i < GetMenuItemCount(edit); ++i) if (GetMenuItemID(edit, i) == 252) {
            InsertMenuW(edit, i + 1, MF_BYPOSITION | MF_STRING, kMenuCommand, Tr(app->EnglishUI(), title)); return;
        }
        AppendMenuW(edit, MF_STRING, kMenuCommand, Tr(app->EnglishUI(), title));
    } else ModifyMenuW(edit, kMenuCommand, MF_BYCOMMAND | MF_STRING, kMenuCommand, Tr(app->EnglishUI(), title));
}
bool HandleCommand(MMDApp* app, unsigned command) {
    if (!app || command != kMenuCommand) return false;
    Dialog d{}; d.app = app; std::string error;
    if (app->CameraMode() || app->PlaybackActive() || !Capture(app->SelectedModel(), d.source, error)) {
        MessageBoxW(app->MainWindow(), Tr(app->EnglishUI(), selection), Tr(app->EnglishUI(), title), MB_OK | MB_ICONINFORMATION); return true;
    }
    DLGTEMPLATE layout{};
    layout.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    layout.cx = 300; layout.cy = 223;
    std::vector<WORD> data((sizeof(layout) + 1) / 2);
    std::memcpy(data.data(), &layout, sizeof(layout));
    data.insert(data.end(), {0, 0, 0, 9}); // menu, class, title, font size
    const wchar_t* font = L"Segoe UI";
    do { data.push_back(*font); } while (*font++);
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(data.data()),
        app->MainWindow(), Proc, reinterpret_cast<LPARAM>(&d));
    return true;
}
}
