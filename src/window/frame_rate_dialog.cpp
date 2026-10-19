// MMS addition: the floating window behind "Edit -> Correct by frame rate...".
//
// The window owns the two frame-rate fields, the Preview and Apply buttons, the
// report area and the Copy report button; the conversion itself lives in the
// backend declared by frame_rate_correction.hpp.  This file never applies
// anything on its own and never writes a file:
//
//   * Preview parses the two values and calls Prepare.  A successful Prepare
//     stores the Plan and the Report and arms Apply; the report area then shows
//     the summary only, and the project is still untouched.
//   * Apply runs only from the Apply button and only while a plan is stored.  A
//     failed Apply keeps the plan (the user may retry) and shows the message; a
//     successful Apply drops the plan, renders the detailed per-key report,
//     states that the result is in memory only, and then calls RequestSaveAs
//     once so the backend can run the real Save As flow.  No second
//     confirmation is shown and nothing here claims the project was saved.
//   * Editing either frame rate drops the stored plan, clears the report area
//     and disarms Apply.
//   * Close (and the system close button) only closes the window.
//
// Hints are short: one line at the top for the scope and the frame-0 anchor,
// and one line at the bottom saying what Apply does and that same-frame keys
// merge.  The conversion formula is deliberately not shown: a tooltip is the
// only place it would belong, and the comctl32 tooltip in this environment
// implements only the ANSI message set (TTM_ADDTOOLW registers nothing,
// TTM_SETMAXTIPWIDTH answers -1), so it cannot carry Japanese or Chinese text.
// The report carries the numbers that matter instead: the source and current
// frame rate, and for every key its exact frame and the frame it took.
//
// The report area is a scrollable read-only edit holding the whole detail list:
// the UI never trims it and Copy report copies that same complete text.  Only
// the backend may leave entries out, and it says so through
// Report::detailsTruncated and Report::omittedDetailCount, which the report
// prints.  The window is a plain Win32 window created by this module, so it
// needs no host message loop support and no core library beyond MMDApp's inline
// accessors; a background test can compile just this file with a stub backend.
// It uses user32 and gdi32 only.
//
// Layout: every string is measured in all three languages and the window is
// sized once for the widest of them, so switching the UI language never moves a
// control and no language is clipped.  The border between the client edge and
// the content is the drag area (the caption drags as usual).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "mikudancestudio/frame_rate_correction.hpp"
#include "mikudancestudio/mmd_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mikudancestudio {
namespace frame_rate_correction {
namespace {

// ---------------------------------------------------------------------------
// Control ids (private to this window; the ids are not part of the interface).
// ---------------------------------------------------------------------------
enum : int {
    kIdTop = 1300,
    kIdOriginalLabel,
    kIdOriginalEdit,
    kIdCurrentLabel,
    kIdCurrentEdit,
    kIdPreview,
    kIdApply,
    kIdCopy,
    kIdClose,
    kIdReport,
    kIdStatus,
    kIdHint,
};

// ---------------------------------------------------------------------------
// Text.  Language 0 Japanese, 1 English, 2 Chinese - the same order as
// MMDApp::EnglishUI() and the rest of the port.
// ---------------------------------------------------------------------------
struct Text {
    const wchar_t* ja;
    const wchar_t* en;
    const wchar_t* zh;
};

const wchar_t* Tr(unsigned language, const Text& text) {
    return language == 0 ? text.ja : language == 1 ? text.en : text.zh;
}

const Text kTitle{L"フレームレートで補正", L"Correct by frame rate",
                  L"根据帧率修正"};
const Text kMenuText{L"フレームレートで補正…", L"Correct by frame rate…",
                     L"根据帧率修正…"};
const Text kOriginalLabel{L"元のFPS", L"Original FPS", L"原帧率"};
const Text kCurrentLabel{L"現在のFPS", L"Current FPS", L"现帧率"};
const Text kPreviewText{L"プレビュー", L"Preview", L"预览"};
const Text kApplyText{L"適用", L"Apply", L"应用"};
const Text kCopyText{L"レポートをコピー", L"Copy report", L"复制报告"};
const Text kCloseText{L"閉じる", L"Close", L"关闭"};

// The one line at the top: the scope and the anchor, nothing else.
const Text kTopLine{L"プロジェクト全体を換算します（拡縮の中心はフレーム 0）。",
                    L"Converts the whole project; scaling is centered on frame 0.",
                    L"换算整个工程，缩放中心为帧 0。"};
// The one line at the bottom, kept deliberately short: what happens after
// Apply, and the one thing about the conversion a user has to know up front.
const Text kHint{
    L"適用後は自動で「名前を付けて保存」を開きます。同じフレームのキーは統合されます。",
    L"Save As opens automatically after applying; keys on the same frame merge.",
    L"应用后自动另存为；同帧关键帧会合并。"};

const Text kReady{L"「プレビュー」で変更内容を確認できます。",
                  L"Press Preview to see what the conversion would change.",
                  L"点击预览查看将要发生的改变。"};
const Text kPreviewOk{L"プレビュー完了。まだ何も変更していません。",
                      L"Preview ready. Nothing has been changed yet.",
                      L"预览完成，尚未改动工程。"};
const Text kConverted{
    L"修正が完了しました。「名前を付けて保存」をキャンセルしても修正は取り消されません。",
    L"Correction complete; cancelling Save As does not undo it.",
    L"修正完成；取消另存为不会撤销修正。"};
const Text kBadNumber{L"0 より大きく 1000 以下の数値を入力してください（例：29.97）。",
                      L"Enter a number greater than 0 and at most 1000 (for example 29.97).",
                      L"请输入大于 0 且不超过 1000 的数值（例如 29.97）。"};
const Text kPrepareFailed{L"プレビューに失敗しました。", L"The preview failed.",
                          L"预览失败。"};
const Text kApplyFailed{
    L"適用に失敗しました。プロジェクトは変更されていません。",
    L"The apply failed. The project is unchanged.", L"应用失败，工程未改动。"};
const Text kBadClipboard{L"レポートをコピーできませんでした。",
                         L"The report could not be copied.",
                         L"无法复制报告。"};
const Text kCopied{L"レポートをコピーしました。", L"Report copied.",
                   L"报告已复制。"};
// A report that cannot be built is refused before anything is applied: the
// conversion happens after the text exists, so a failure here leaves the project
// exactly as it was.
const Text kOutOfMemory{
    L"レポートが大きすぎて作成できません（メモリ不足）。今回は何も適用していません。",
    L"The report is too large to build (out of memory); nothing was applied.",
    L"报告过大，内存不足；本次修正未执行。"};
const Text kReportTooLarge{L"レポートが大きすぎて表示できません。",
                           L"The report is too large to display.",
                           L"报告过大，无法显示。"};

const Text kInputKeys{L"入力キー", L"Input keys", L"输入关键帧"};
const Text kOutputKeys{L"出力キー", L"Output keys", L"输出关键帧"};
const Text kMergeGroups{L"統合グループ", L"Merge groups", L"合并组"};
const Text kMergedKeys{L"統合されたキー", L"Merged keys", L"合并的键"};
const Text kRoundedKeys{L"整数化したキー", L"Rounded keys", L"取整的关键帧"};
const Text kMaxError{L"最大サンプル誤差", L"Max sampled value error",
                     L"最大采样误差"};
const Text kWorstChannel{L"最大誤差のチャンネル",
                         L"Channel with the largest error", L"误差最大的通道"};
const Text kExamined{L"対象", L"Examined", L"检查范围"};
const Text kModels{L"モデル", L"models", L"模型"};
const Text kAccessories{L"アクセサリ", L"accessories", L"附件"};
const Text kMmvTracks{L"MMV トラック", L"MMV tracks", L"MMV 轨道"};
const Text kMeshCaches{L"メッシュキャッシュ", L"mesh caches", L"网格缓存"};

const Text kSummaryOnly{
    L"まだ適用していません。適用すると、丸められたフレームと統合されたキーの詳細を表示します。",
    L"Not applied yet; Apply to see the rounded frames and the merged keys in detail.",
    L"尚未应用；应用后显示取整的帧与被合并的键的明细。"};
const Text kDetailHeader{L"変更の詳細：", L"Change details:", L"变更明细："};
const Text kExactWord{L"正確", L"exact", L"精确"};
// A report row is built as channel + separator + this tail.  The channel is
// deliberately not part of a format string: a long Chinese model name plus a
// tab name plus an FX path can exceed any fixed buffer, and swprintf_s would
// fail (and abort through its invalid parameter handler) on the overflow, so
// only the short numbers are formatted into a fixed-size buffer.
const wchar_t kRowSeparator[] = L" · ";
const Text kRoundedRow{L"丸め · フレーム %d → %d（%ls %ls）",
                       L"Rounded · frame %d → %d (%ls %ls)",
                       L"取整 · 帧 %d → %d（%ls %ls）"};
const Text kMergedRow{
    L"統合 · キー フレーム %d → 統合先 %d（グループ %d–%d、%ls %ls）",
    L"Merged · key frame %d → target %d (group %d–%d, %ls %ls)",
    L"合并 · 原键帧 %d → 目标帧 %d（组 %d–%d，%ls %ls）"};
const Text kBackendCapNote{
    L"バックエンドの上限により %llu 件の詳細が省略されています。",
    L"The backend's budget omitted %llu detail entries.",
    L"后端预算省略了 %llu 条明细。"};
// The report is not a statement about the file: Apply corrects the project in
// memory and then asks for Save As, and the report stays on screen whether the
// user saves or cancels.  It must therefore never assume either outcome.
const Text kReportNote{
    L"このレポートは今回の修正の記録です。保存されたかどうかは「名前を付けて保存」の結果によります。",
    L"This report records the correction; whether it is saved depends on the Save As result.",
    L"本报告记录本次修正；保存状态以另存为结果为准。"};

// ---------------------------------------------------------------------------
// Layout constants.  kPad is both the outer padding and the drag margin.
// ---------------------------------------------------------------------------
constexpr int kPad = 14;
constexpr int kGap = 10;
constexpr int kRowGap = 6;
constexpr int kRowH = 24;
constexpr int kLabelFieldGap = 8;
constexpr int kButtonH = 26;
constexpr int kButtonGap = 8;
constexpr int kButtonPad = 18;
constexpr int kMinInnerW = 460;
constexpr int kReportLines = 12;
// A rate field is a client-edged box (WS_EX_CLIENTEDGE, 2 px per side) with
// kFieldTextMargin pixels of text margin on both sides, plus slack, so the
// number never touches the frame of the box.  kRowH leaves the same room
// vertically for the font plus the edge.
constexpr int kClientEdge = 2;
constexpr int kFieldTextMargin = 4;
constexpr int kFieldPad = 2 * (kClientEdge + kFieldTextMargin) + 12;
constexpr int kLabelPad = 8;

const wchar_t kClassName[] = L"MikuMikuStudioFrameRateCorrection";

// ---------------------------------------------------------------------------
// Fonts: the port picks the panel family by UI language (see
// src/window/ui_panel_sweep.cpp); the same three keep Latin, kana and han
// legible, and the size comes from the system message font.
// ---------------------------------------------------------------------------
HFONT MakeFont(unsigned language) {
    LOGFONTW font{};
    NONCLIENTMETRICSW metrics{sizeof(NONCLIENTMETRICSW)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                             0))
        font = metrics.lfMessageFont;
    if (font.lfHeight == 0) font.lfHeight = -12;
    const wchar_t* face = language == 0   ? L"MS PGothic"
                          : language == 1 ? L"Arial"
                                          : L"Microsoft YaHei UI";
    wcscpy_s(font.lfFaceName, face);
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    return CreateFontIndirectW(&font);
}

void ApplyFont(HWND window, HFONT font) {
    if (font == nullptr) return;
    for (HWND child = GetWindow(window, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

// Text padding inside the two rate fields, on top of their client edge.  It is
// applied again after every font change, because setting the font makes the
// control recompute its own margins.
void ApplyFieldMargins(HWND window) {
    for (const int id : {kIdOriginalEdit, kIdCurrentEdit})
        SendMessageW(GetDlgItem(window, id), EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(kFieldTextMargin, kFieldTextMargin));
}

// ---------------------------------------------------------------------------
// Numbers.  "29.97" must survive the round trip; a trailing unit, a comma or a
// non-finite value is refused before the backend is ever called.
// ---------------------------------------------------------------------------
bool ParseRate(const std::wstring& text, double& out) {
    const std::size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const std::size_t last = text.find_last_not_of(L" \t\r\n");
    const std::wstring body = text.substr(first, last - first + 1);
    if (body.empty() || body.size() > 32) return false;
    wchar_t* end = nullptr;
    const double value = std::wcstod(body.c_str(), &end);
    if (end == nullptr || *end != L'\0') return false;
    if (!std::isfinite(value) || value <= 0.0 || value > 1000.0) return false;
    out = value;
    return true;
}

std::wstring FormatRate(double value) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.10g", value);
    return buffer;
}

std::wstring FormatNumber(double value) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.6g", value);
    return buffer;
}

std::wstring ControlText(HWND control) {
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return std::wstring();
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int got = GetWindowTextW(control, text.data(), length + 1);
    text.resize(got > 0 ? static_cast<std::size_t>(got) : 0u);
    return text;
}

// ---------------------------------------------------------------------------
// Report text.
// ---------------------------------------------------------------------------
std::wstring SummaryText(unsigned language, const Rates& rates,
                         const Report& report) {
    const auto number = [](std::uint64_t value) {
        return static_cast<unsigned long long>(value);
    };
    wchar_t line[512]{};
    std::wstring text;
    swprintf_s(line, L"%ls %s \u2192 %ls %s", Tr(language, kOriginalLabel),
               FormatRate(rates.originalFPS).c_str(),
               Tr(language, kCurrentLabel),
               FormatRate(rates.currentFPS).c_str());
    text += line;
    text += L"\r\n";
    swprintf_s(line, L"%ls %llu \u2192 %ls %llu", Tr(language, kInputKeys),
               number(report.inputKeys), Tr(language, kOutputKeys),
               number(report.outputKeys));
    text += line;
    text += L"\r\n";
    swprintf_s(line, L"%ls %llu / %ls %llu", Tr(language, kMergeGroups),
               number(report.mergeGroups), Tr(language, kMergedKeys),
               number(report.mergedKeys));
    text += line;
    text += L"\r\n";
    swprintf_s(line, L"%ls %llu", Tr(language, kRoundedKeys),
               number(report.roundedKeys));
    text += line;
    text += L"\r\n";
    swprintf_s(line, L"%ls %s", Tr(language, kMaxError),
               FormatNumber(report.maxSampledValueError).c_str());
    text += line;
    if (!report.worstChannel.empty()) {
        text += L" \u00b7 ";
        text += Tr(language, kWorstChannel);
        text += L": ";
        text += report.worstChannel;
    }
    text += L"\r\n";
    swprintf_s(line, L"%ls %ls %llu \u00b7 %ls %llu \u00b7 %ls %llu \u00b7 %ls %llu",
               Tr(language, kExamined), Tr(language, kModels),
               number(report.models), Tr(language, kAccessories),
               number(report.accessories), Tr(language, kMmvTracks),
               number(report.mmvTracks), Tr(language, kMeshCaches),
               number(report.meshCaches));
    text += line;
    text += L"\r\n";
    return text;
}

std::wstring ChangeRow(unsigned language, const FrameChange& change) {
    // Only the numbers go into the fixed buffer, and the channel is appended by
    // concatenation, so a channel of any length survives intact.
    wchar_t tail[256]{};
    if (change.kind == FrameChange::ChangeKind::Merged)
        swprintf_s(tail, Tr(language, kMergedRow), change.originalFrame,
                   change.newFrame, change.mergeFirstFrame,
                   change.mergeLastFrame, Tr(language, kExactWord),
                   FormatNumber(change.exactFrame).c_str());
    else
        swprintf_s(tail, Tr(language, kRoundedRow), change.originalFrame,
                   change.newFrame, Tr(language, kExactWord),
                   FormatNumber(change.exactFrame).c_str());
    std::wstring row = change.channel;
    row += kRowSeparator;
    row += tail;
    return row;
}

// The summary alone, as Preview shows it.
std::wstring PreviewText(unsigned language, const Rates& rates,
                         const Report& report) {
    std::wstring text = SummaryText(language, rates, report);
    text += Tr(language, kSummaryOnly);
    return text;
}

// The whole thing, as a successful Apply shows it: every rounded key and every
// key that lost its own slot, what the backend's budget left out, and an
// explicit reminder that nothing has been written to a file yet.  The list is
// never trimmed here: the report area scrolls, and Copy report copies exactly
// this text.
std::wstring DetailText(unsigned language, const Rates& rates,
                        const Report& report) {
    std::wstring text = SummaryText(language, rates, report);
    text += Tr(language, kDetailHeader);
    text += L"\r\n";
    wchar_t line[512]{};
    for (const FrameChange& change : report.changes) {
        text += ChangeRow(language, change);
        text += L"\r\n";
    }
    if (report.detailsTruncated) {
        swprintf_s(line, Tr(language, kBackendCapNote),
                   static_cast<unsigned long long>(report.omittedDetailCount));
        text += line;
        text += L"\r\n";
    }
    text += Tr(language, kReportNote);
    return text;
}

enum class StatusKind {
    Ready,
    PreviewOk,
    Converted,
    Copied,
    CopyFailed,
    BadNumber,
    PrepareFailed,
    ApplyFailed,
    OutOfMemory,
    Backend,  // the message the backend returned, shown verbatim
};

std::wstring StatusText(const std::wstring& backendMessage,
                        StatusKind status, unsigned language) {
    switch (status) {
        case StatusKind::Ready: return Tr(language, kReady);
        case StatusKind::PreviewOk: return Tr(language, kPreviewOk);
        case StatusKind::Converted: return Tr(language, kConverted);
        case StatusKind::Copied: return Tr(language, kCopied);
        case StatusKind::CopyFailed: return Tr(language, kBadClipboard);
        case StatusKind::BadNumber: return Tr(language, kBadNumber);
        case StatusKind::PrepareFailed: return Tr(language, kPrepareFailed);
        case StatusKind::ApplyFailed: return Tr(language, kApplyFailed);
        case StatusKind::OutOfMemory: return Tr(language, kOutOfMemory);
        default: return backendMessage;
    }
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool copied = false;
    if (EmptyClipboard()) {
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory != nullptr) {
            void* target = GlobalLock(memory);
            if (target != nullptr) {
                std::memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                // The clipboard owns the block once this succeeds.
                copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
                if (!copied) GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
    }
    CloseClipboard();
    return copied;
}

// ---------------------------------------------------------------------------
// Window metrics, measured once for the widest language.
// ---------------------------------------------------------------------------
struct Metric {
    int clientW = 0;
    int clientH = 0;
    RECT top{};
    RECT originalLabel{};
    RECT originalEdit{};
    RECT currentLabel{};
    RECT currentEdit{};
    RECT preview{};
    RECT apply{};
    RECT copy{};
    RECT close{};
    RECT report{};
    RECT hint{};
    RECT status{};
};

int TextWidth(HDC dc, const wchar_t* text) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    return size.cx;
}

int WrappedHeight(HDC dc, const wchar_t* text, int width) {
    RECT rect{0, 0, width, 0};
    DrawTextW(dc, text, -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    return rect.bottom - rect.top;
}

int ButtonWidth(HDC dc, unsigned language, const Text& text) {
    return TextWidth(dc, Tr(language, text)) + 2 * kButtonPad;
}

Metric ComputeMetric() {
    Metric metric;
    HDC dc = GetDC(nullptr);
    int labelW = 0, fieldW = 64, buttonRowW = 0, lineH = 16, wordH = 0;
    int previewW = 0, applyW = 0, copyW = 0, closeW = 0;
    for (unsigned language = 0; language < 3; ++language) {
        HFONT font = MakeFont(language);
        HGDIOBJ previous = SelectObject(dc, font);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        lineH = std::max<int>(lineH, tm.tmHeight + 2);
        labelW = std::max(labelW, TextWidth(dc, Tr(language, kOriginalLabel)));
        labelW = std::max(labelW, TextWidth(dc, Tr(language, kCurrentLabel)));
        fieldW = std::max(fieldW, TextWidth(dc, L"1000") + kFieldPad);
        previewW = std::max(previewW, ButtonWidth(dc, language, kPreviewText));
        applyW = std::max(applyW, ButtonWidth(dc, language, kApplyText));
        copyW = std::max(copyW, ButtonWidth(dc, language, kCopyText));
        closeW = std::max(closeW, ButtonWidth(dc, language, kCloseText));
        buttonRowW = std::max(buttonRowW, previewW + applyW + copyW + closeW +
                                               3 * kButtonGap);
        SelectObject(dc, previous);
        DeleteObject(font);
    }
    const int labelColumn = labelW + kLabelPad;
    const int innerW = std::max(
        kMinInnerW, std::max(labelColumn + kLabelFieldGap + fieldW, buttonRowW));
    for (unsigned language = 0; language < 3; ++language) {
        HFONT font = MakeFont(language);
        HGDIOBJ previous = SelectObject(dc, font);
        wordH = std::max(wordH, WrappedHeight(dc, Tr(language, kTopLine), innerW));
        wordH = std::max(wordH, WrappedHeight(dc, Tr(language, kHint), innerW));
        SelectObject(dc, previous);
        DeleteObject(font);
    }
    ReleaseDC(nullptr, dc);
    const int reportH = kReportLines * lineH;
    const int statusH = lineH + 6;
    int y = kPad;
    metric.top = {kPad, y, kPad + innerW, y + wordH + 2};
    y += wordH + 2 + kGap;
    metric.originalLabel = {kPad, y + 2, kPad + labelColumn, y + kRowH};
    metric.originalEdit = {kPad + labelColumn + kLabelFieldGap, y,
                           kPad + labelColumn + kLabelFieldGap + fieldW, y + kRowH};
    y += kRowH + kRowGap;
    metric.currentLabel = {kPad, y + 2, kPad + labelColumn, y + kRowH};
    metric.currentEdit = {kPad + labelColumn + kLabelFieldGap, y,
                          kPad + labelColumn + kLabelFieldGap + fieldW, y + kRowH};
    y += kRowH + kGap;
    int x = kPad;
    metric.preview = {x, y, x + previewW, y + kButtonH};
    x += previewW + kButtonGap;
    metric.apply = {x, y, x + applyW, y + kButtonH};
    x += applyW + kButtonGap;
    metric.copy = {x, y, x + copyW, y + kButtonH};
    x += copyW + kButtonGap;
    metric.close = {x, y, x + closeW, y + kButtonH};
    y += kButtonH + kGap;
    metric.report = {kPad, y, kPad + innerW, y + reportH};
    y += reportH + kGap;
    metric.hint = {kPad, y, kPad + innerW, y + wordH + 2};
    y += wordH + 2 + kGap;
    metric.status = {kPad, y, kPad + innerW, y + statusH};
    y += statusH + kPad;
    metric.clientW = kPad * 2 + innerW;
    metric.clientH = y;
    return metric;
}

// The metric depends only on the system fonts, so it is measured once.
const Metric& Metrics() {
    static const Metric metric = ComputeMetric();
    return metric;
}

void Place(HWND control, const RECT& rect) {
    if (control == nullptr) return;
    SetWindowPos(control, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplyLayout(HWND window) {
    const Metric& metric = Metrics();
    Place(GetDlgItem(window, kIdTop), metric.top);
    Place(GetDlgItem(window, kIdOriginalLabel), metric.originalLabel);
    Place(GetDlgItem(window, kIdOriginalEdit), metric.originalEdit);
    Place(GetDlgItem(window, kIdCurrentLabel), metric.currentLabel);
    Place(GetDlgItem(window, kIdCurrentEdit), metric.currentEdit);
    Place(GetDlgItem(window, kIdPreview), metric.preview);
    Place(GetDlgItem(window, kIdApply), metric.apply);
    Place(GetDlgItem(window, kIdCopy), metric.copy);
    Place(GetDlgItem(window, kIdClose), metric.close);
    Place(GetDlgItem(window, kIdReport), metric.report);
    Place(GetDlgItem(window, kIdHint), metric.hint);
    Place(GetDlgItem(window, kIdStatus), metric.status);
}

// ---------------------------------------------------------------------------
// One instance per application.
// ---------------------------------------------------------------------------
struct Instance {
    HWND window = nullptr;
    MMDApp* app = nullptr;
    HFONT font = nullptr;
    unsigned language = 1;
    bool ready = false;  // controls exist and the initial texts are in place
    PlanPtr plan;
    Report report;
    Rates preparedRates;
    bool hasReport = false;
    bool detailed = false;  // the report area shows the detailed list
    // The detail text is built before Apply and kept here, so a successful
    // conversion never has to allocate a large string on its way to Save As.
    std::wstring detailText;
    std::wstring backendMessage;
    StatusKind status = StatusKind::Ready;
};

std::map<MMDApp*, Instance>& Registry() {
    static std::map<MMDApp*, Instance> registry;
    return registry;
}

std::mutex& RegistryLock() {
    static std::mutex lock;
    return lock;
}

Instance* Find(MMDApp* app) {
    if (app == nullptr) return nullptr;
    const auto it = Registry().find(app);
    return it == Registry().end() ? nullptr : &it->second;
}

Instance* InstanceOf(HWND window) {
    return Find(reinterpret_cast<MMDApp*>(
        GetWindowLongPtrW(window, GWLP_USERDATA)));
}

// The status line is never worth a crash: it is called from inside window
// procedures, so an allocation failure falls back to an empty line instead of
// escaping into the host's message loop.
void SetStatus(HWND window, Instance& instance, StatusKind status,
               const std::wstring& backendMessage = std::wstring()) {
    try {
        instance.status = status;
        instance.backendMessage = backendMessage;
        SetWindowTextW(GetDlgItem(window, kIdStatus),
                       StatusText(instance.backendMessage, instance.status,
                                  instance.language)
                           .c_str());
    } catch (...) {
        SetWindowTextW(GetDlgItem(window, kIdStatus), L"");
    }
}

void UpdateEnabledState(HWND window, Instance& instance) {
    const bool available = Available(instance.app);
    EnableWindow(GetDlgItem(window, kIdPreview), available ? TRUE : FALSE);
    EnableWindow(GetDlgItem(window, kIdApply),
                 (available && instance.plan != nullptr) ? TRUE : FALSE);
    // Copying is useful from the moment a report is on screen.
    EnableWindow(GetDlgItem(window, kIdCopy),
                 instance.hasReport ? TRUE : FALSE);
    EnableWindow(GetDlgItem(window, kIdClose), TRUE);
}

// Builds the whole detail text.  This runs before anything is applied, and it is
// the only place where a report of arbitrary size is turned into a string: a
// failure here must be handled before the project is touched, never after.
bool TryBuildDetailText(unsigned language, const Rates& rates,
                        const Report& report, std::wstring& text) {
    try {
        text = DetailText(language, rates, report);
        return true;
    } catch (...) {
        text.clear();
        return false;
    }
}

// Shows the report.  The detailed text is normally the one built before Apply;
// the fallback path (a language change, or a report shown without a prebuilt
// text) rebuilds it, and any allocation failure degrades to a short message
// rather than escaping into a window procedure.
void ShowReport(HWND window, Instance& instance) {
    HWND report = GetDlgItem(window, kIdReport);
    if (!instance.hasReport) {
        SetWindowTextW(report, L"");
        return;
    }
    try {
        const std::wstring text =
            instance.detailed && !instance.detailText.empty()
                ? instance.detailText
                : (instance.detailed
                       ? DetailText(instance.language, instance.preparedRates,
                                    instance.report)
                       : PreviewText(instance.language, instance.preparedRates,
                                     instance.report));
        SetWindowTextW(report, text.c_str());
    } catch (...) {
        SetWindowTextW(report, Tr(instance.language, kReportTooLarge));
    }
}

void SetLanguage(HWND window, Instance& instance, unsigned language) {
    instance.language = language;
    HFONT font = MakeFont(language);
    if (font != nullptr) {
        if (instance.font != nullptr) DeleteObject(instance.font);
        instance.font = font;
        ApplyFont(window, font);
        ApplyFieldMargins(window);
    }
    SetWindowTextW(window, Tr(language, kTitle));
    SetWindowTextW(GetDlgItem(window, kIdTop), Tr(language, kTopLine));
    SetWindowTextW(GetDlgItem(window, kIdHint), Tr(language, kHint));
    SetWindowTextW(GetDlgItem(window, kIdOriginalLabel),
                   Tr(language, kOriginalLabel));
    SetWindowTextW(GetDlgItem(window, kIdCurrentLabel),
                   Tr(language, kCurrentLabel));
    SetWindowTextW(GetDlgItem(window, kIdPreview), Tr(language, kPreviewText));
    SetWindowTextW(GetDlgItem(window, kIdApply), Tr(language, kApplyText));
    SetWindowTextW(GetDlgItem(window, kIdCopy), Tr(language, kCopyText));
    SetWindowTextW(GetDlgItem(window, kIdClose), Tr(language, kCloseText));
    SetWindowTextW(GetDlgItem(window, kIdStatus),
                   StatusText(instance.backendMessage, instance.status, language)
                       .c_str());
    // The prebuilt text belongs to the language it was built in; dropping it
    // makes ShowReport render the list again in the new language.
    instance.detailText.clear();
    ShowReport(window, instance);
    UpdateEnabledState(window, instance);
}

void CenterOnOwner(HWND window, HWND owner) {
    RECT windowRect{};
    if (!GetWindowRect(window, &windowRect)) return;
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    RECT ownerRect{};
    RECT area{};
    if (owner != nullptr && IsWindow(owner) && GetWindowRect(owner, &ownerRect))
        area = ownerRect;
    else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0))
        area = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN)};
    HMONITOR monitor = MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(MONITORINFO)};
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) area = info.rcWork;
    int x = area.left + ((area.right - area.left) - width) / 2;
    int y = area.top + ((area.bottom - area.top) - height) / 2;
    x = std::max<int>(area.left, std::min<int>(x, area.right - width));
    y = std::max<int>(area.top, std::min<int>(y, area.bottom - height));
    SetWindowPos(window, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// Commands.
// ---------------------------------------------------------------------------
// Drops the prepared plan: any parameter change makes it describe a conversion
// the user no longer asked for.
void InvalidatePlan(HWND window, Instance& instance) {
    instance.plan.reset();
    instance.hasReport = false;
    instance.detailed = false;
    instance.report = Report{};
    instance.detailText.clear();
    SetWindowTextW(GetDlgItem(window, kIdReport), L"");
    SetStatus(window, instance, StatusKind::Ready);
    UpdateEnabledState(window, instance);
}

void OnParameterChanged(HWND window, Instance& instance) {
    if (!instance.ready) return;  // the initial texts are not a user edit
    InvalidatePlan(window, instance);
}

void OnPreview(HWND window, Instance& instance) {
    if (!Available(instance.app)) return;
    double original = 0.0, current = 0.0;
    if (!ParseRate(ControlText(GetDlgItem(window, kIdOriginalEdit)), original) ||
        !ParseRate(ControlText(GetDlgItem(window, kIdCurrentEdit)), current)) {
        InvalidatePlan(window, instance);
        SetStatus(window, instance, StatusKind::BadNumber);
        return;
    }
    Rates rates;
    rates.originalFPS = original;
    rates.currentFPS = current;
    PlanPtr plan;
    Report report;
    std::wstring error;
    if (!Prepare(*instance.app, rates, plan, report, error)) {
        // A refused Prepare leaves nothing behind to apply.
        instance.plan.reset();
        instance.hasReport = false;
        instance.detailed = false;
        instance.report = Report{};
        instance.detailText.clear();
        SetWindowTextW(GetDlgItem(window, kIdReport), L"");
        SetStatus(window, instance,
                  error.empty() ? StatusKind::PrepareFailed : StatusKind::Backend,
                  error);
        UpdateEnabledState(window, instance);
        return;
    }
    // Copying the report is the second place a large detail list is allocated
    // (after the text itself), and it happens before anything is applied.
    try {
        instance.report = report;
    } catch (...) {
        InvalidatePlan(window, instance);
        SetStatus(window, instance, StatusKind::OutOfMemory);
        return;
    }
    instance.plan = plan;
    instance.preparedRates = rates;
    instance.hasReport = true;
    instance.detailed = false;  // Preview shows the summary only
    instance.detailText.clear();
    ShowReport(window, instance);
    SetStatus(window, instance, instance.plan != nullptr ? StatusKind::PreviewOk
                                                        : StatusKind::PrepareFailed);
    UpdateEnabledState(window, instance);
}

void OnApply(HWND window, Instance& instance) {
    if (instance.plan == nullptr) {
        UpdateEnabledState(window, instance);
        return;
    }
    if (!Available(instance.app)) {
        UpdateEnabledState(window, instance);
        return;
    }
    // Build the whole detail report first.  It is the largest allocation this
    // window ever makes, and it must succeed before the project is converted: a
    // failure afterwards would leave the correction applied but unseen, inside a
    // window procedure, and would never reach RequestSaveAs.
    std::wstring detail;
    if (!TryBuildDetailText(instance.language, instance.preparedRates,
                            instance.report, detail)) {
        SetStatus(window, instance, StatusKind::OutOfMemory);
        UpdateEnabledState(window, instance);
        return;  // the project is untouched
    }
    std::wstring error;
    if (!Apply(*instance.app, instance.plan, error)) {
        // The backend left the project alone; the plan stays so the user may
        // correct the cause and press Apply again.  Nothing is shown as a modal
        // box: the message goes to the status line.  The report area still holds
        // the preview, because the detail text is installed only on success.
        SetStatus(window, instance,
                  error.empty() ? StatusKind::ApplyFailed : StatusKind::Backend,
                  error);
        UpdateEnabledState(window, instance);
        return;
    }
    instance.plan.reset();  // never apply one plan twice
    // Put up the text that was built before the conversion, without allocating a
    // second large buffer here, and only then ask for Save As: a cancelled Save
    // As still leaves the report readable.
    instance.detailed = true;
    instance.detailText = std::move(detail);
    SetWindowTextW(GetDlgItem(window, kIdReport), instance.detailText.c_str());
    SetStatus(window, instance, StatusKind::Converted);
    UpdateEnabledState(window, instance);
    // One call, on success only, and the backend owns the real Save As flow.
    RequestSaveAs(*instance.app);
}

void OnCopy(HWND window, Instance& instance) {
    if (!instance.hasReport) {
        UpdateEnabledState(window, instance);
        return;
    }
    const std::wstring text = ControlText(GetDlgItem(window, kIdReport));
    SetStatus(window, instance,
              CopyTextToClipboard(window, text) ? StatusKind::Copied
                                                : StatusKind::CopyFailed);
}

// ---------------------------------------------------------------------------
// The window itself.
// ---------------------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(window, message, wparam, lparam);
        }
        case WM_CREATE: {
            Instance* instance = InstanceOf(window);
            if (instance == nullptr) return -1;
            const DWORD staticStyle = WS_CHILD | WS_VISIBLE;
            const auto add = [window](const wchar_t* className,
                                      const wchar_t* text, DWORD style,
                                      DWORD exStyle, int id) {
                return CreateWindowExW(
                    exStyle, className, text, style, 0, 0, 10, 10, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                    GetModuleHandleW(nullptr), nullptr);
            };
            add(L"STATIC", L"", staticStyle | SS_LEFT, 0, kIdTop);
            add(L"STATIC", L"", staticStyle | SS_LEFT, 0, kIdOriginalLabel);
            // The two rate fields are the only editable controls, so they are the
            // only ones that carry a client edge: an editable box has to look
            // like a box even before the caret is in it.
            add(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 ES_AUTOHSCROLL | ES_LEFT,
                WS_EX_CLIENTEDGE, kIdOriginalEdit);
            add(L"STATIC", L"", staticStyle | SS_LEFT, 0, kIdCurrentLabel);
            add(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 ES_AUTOHSCROLL | ES_LEFT,
                WS_EX_CLIENTEDGE, kIdCurrentEdit);
            add(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_PUSHBUTTON,
                0, kIdPreview);
            add(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_PUSHBUTTON,
                0, kIdApply);
            add(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_PUSHBUTTON,
                0, kIdCopy);
            add(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                    BS_PUSHBUTTON,
                0, kIdClose);
            add(L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                 ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                0, kIdReport);
            add(L"STATIC", L"", staticStyle | SS_LEFT, 0, kIdHint);
            add(L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_READONLY |
                                 ES_AUTOHSCROLL,
                0, kIdStatus);
            // Four pixels of text padding inside each rate field, on top of the
            // client edge, so the number never touches the frame of the box.
            ApplyFieldMargins(window);
            // The report holds the whole detail list, so lift the edit control's
            // default text limit: a long conversion must not be cut off here.
            // Copy report reads this same control, so what is copied is what is
            // on screen, in full.
            SendMessageW(GetDlgItem(window, kIdReport), EM_SETLIMITTEXT, 0, 0);
            // The dialog has no getter for the project's rates, so both fields
            // start from the Rates defaults; the user edits them from there.
            SetWindowTextW(GetDlgItem(window, kIdOriginalEdit),
                           FormatRate(Rates{}.originalFPS).c_str());
            SetWindowTextW(GetDlgItem(window, kIdCurrentEdit),
                           FormatRate(Rates{}.currentFPS).c_str());
            ApplyLayout(window);
            SetLanguage(window, *instance, instance->language);
            instance->ready = true;
            return 0;
        }
        case WM_COMMAND: {
            Instance* instance = InstanceOf(window);
            if (instance == nullptr) return 0;
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (id == kIdPreview && code == BN_CLICKED) {
                OnPreview(window, *instance);
                return 0;
            }
            if (id == kIdApply && code == BN_CLICKED) {
                OnApply(window, *instance);
                return 0;
            }
            if (id == kIdCopy && code == BN_CLICKED) {
                OnCopy(window, *instance);
                return 0;
            }
            if (id == kIdClose && code == BN_CLICKED) {
                DestroyWindow(window);
                return 0;
            }
            if ((id == kIdOriginalEdit || id == kIdCurrentEdit) &&
                code == EN_CHANGE) {
                OnParameterChanged(window, *instance);
                return 0;
            }
            return 0;
        }
        case WM_ACTIVATE: {
            if (LOWORD(wparam) != WA_INACTIVE) {
                if (Instance* instance = InstanceOf(window); instance != nullptr)
                    UpdateEnabledState(window, *instance);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client,
                     GetSysColorBrush(COLOR_BTNFACE));
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            // A read-only EDIT reports here as well: keep the field background
            // of the report and status areas, the rest of the text is
            // transparent over the dialog face.
            if (control == GetDlgItem(window, kIdReport) ||
                control == GetDlgItem(window, kIdStatus)) {
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(window, message, wparam, lparam);
            if (hit != HTCLIENT) return hit;
            POINT point{static_cast<short>(LOWORD(lparam)),
                        static_cast<short>(HIWORD(lparam))};
            ScreenToClient(window, &point);
            RECT client{};
            GetClientRect(window, &client);
            // The padding between the client edge and the content drags the
            // window; the controls keep their own hit testing.
            if (point.x < kPad || point.y < kPad ||
                point.x >= client.right - kPad || point.y >= client.bottom - kPad)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCDESTROY: {
            MMDApp* app = reinterpret_cast<MMDApp*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            std::lock_guard<std::mutex> guard(RegistryLock());
            const auto it = Registry().find(app);
            if (it != Registry().end()) {
                if (it->second.font != nullptr) DeleteObject(it->second.font);
                Registry().erase(it);
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// The window class belongs to this module.  Registration happens before the
// first window exists, so WM_ERASEBKGND has to paint the face colour.
bool RegisterDialogClass() {
    static const bool registered = [] {
        WNDCLASSEXW cls{sizeof(WNDCLASSEXW)};
        cls.style = CS_HREDRAW | CS_VREDRAW;
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        // The build does not define UNICODE, so IDC_ARROW is an ANSI resource
        // id; the cast is the same idiom the other host windows use.
        cls.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        cls.hbrBackground = nullptr;
        cls.lpszClassName = kClassName;
        return RegisterClassExW(&cls) != 0;
    }();
    return registered;
}

}  // namespace

void ShowDialog(MMDApp* app, bool visible) {
    if (app == nullptr) return;
    if (!RegisterDialogClass()) return;
    HWND owner = app->MainWindow();
    const Metric& metric = Metrics();
    Instance* instance = nullptr;
    {
        std::lock_guard<std::mutex> guard(RegistryLock());
        const auto existing = Registry().find(app);
        if (existing != Registry().end()) instance = &existing->second;
    }
    if (instance == nullptr) {
        // The instance goes in first, because WM_CREATE runs inside
        // CreateWindowExW and has to find it.  The window is created outside
        // the lock: a destroyed-on-create window would otherwise re-enter
        // WM_NCDESTROY while the lock is held.
        Instance fresh;
        fresh.app = app;
        fresh.language = app->EnglishUI();
        {
            std::lock_guard<std::mutex> guard(RegistryLock());
            const auto inserted = Registry().emplace(app, std::move(fresh));
            instance = &inserted.first->second;
        }
        const DWORD style =
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        const DWORD exStyle = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW;
        RECT outer{0, 0, metric.clientW, metric.clientH};
        AdjustWindowRectEx(&outer, style, FALSE, exStyle);
        HWND window = CreateWindowExW(
            exStyle, kClassName, L"", style, 0, 0, outer.right - outer.left,
            outer.bottom - outer.top, owner, nullptr, GetModuleHandleW(nullptr),
            app);
        if (window == nullptr) {
            std::lock_guard<std::mutex> guard(RegistryLock());
            Registry().erase(app);
            return;
        }
        instance->window = window;
        CenterOnOwner(window, owner);
    }
    if (instance->window == nullptr) return;
    const unsigned language = app->EnglishUI();
    if (instance->language != language)
        SetLanguage(instance->window, *instance, language);
    UpdateEnabledState(instance->window, *instance);
    if (visible) {
        ShowWindow(instance->window, SW_SHOW);
        SetForegroundWindow(instance->window);
    }
}

void RefreshMenu(MMDApp* app) {
    if (app == nullptr) return;
    const unsigned language = app->EnglishUI();
    HWND main = app->MainWindow();
    if (main != nullptr) {
        HMENU bar = GetMenu(main);
        HMENU edit = bar != nullptr ? GetSubMenu(bar, 1) : nullptr;
        if (edit != nullptr) {
            const wchar_t* text = Tr(language, kMenuText);
            const UINT flags =
                MF_STRING | (Available(app) ? MF_ENABLED : MF_GRAYED);
            if (GetMenuState(edit, kMenuCommand, MF_BYCOMMAND) ==
                static_cast<UINT>(-1))
                AppendMenuW(edit, flags, kMenuCommand, text);
            else
                ModifyMenuW(edit, kMenuCommand, MF_BYCOMMAND | flags,
                            kMenuCommand, text);
        }
    }
    // An open dialog follows the host language; this is the hook the host's
    // Localize pass already calls.
    if (Instance* instance = Find(app);
        instance != nullptr && instance->window != nullptr) {
        if (instance->language != language)
            SetLanguage(instance->window, *instance, language);
        UpdateEnabledState(instance->window, *instance);
    }
}

bool HandleCommand(MMDApp* app, unsigned id) {
    if (app == nullptr || id != kMenuCommand) return false;
    ShowDialog(app);
    return true;
}

}  // namespace frame_rate_correction
}  // namespace mikudancestudio
