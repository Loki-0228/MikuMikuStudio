// MAINTENANCE: production editor behind include/mikudancestudio/animation_curve_editor.hpp.
// This file owns the shared modeless Win32 curve window used by the host and by
// MMV.  It depends on the pure-CPU curve core (animation_curve.cpp) only; it
// does not include any other port header, so both callers can link it unchanged.
//
// Design notes
// ------------
// * Every mutation of the channel data goes through Source::apply.  A failed
//   apply leaves the editor state exactly as it was and only writes an error
//   line into the status control, so background tests never see a message box.
// * Undo/redo are local snapshot stacks; each step is still committed with the
//   same apply callback, so the source stays the single writer of its data.
// * Switching source and re-activating the window re-read the source.  The
//   freshly read channels are adopted unchanged; the local undo stacks are only
//   dropped when the read data actually differs, which is what keeps an
//   external edit visible instead of being overwritten by a stale copy.
// * The window is modeless and fully silent while hidden: Open(owner, session,
//   false) shows nothing, pops nothing and reports every diagnostic through the
//   status control.
// * Comparison display (Source::comparisonEnabled and friends) is read-only
//   sugar around the same single-writer model: the main curve is the current
//   channel and the only editable one, while the comparison curves are drawn
//   behind it from data the editor never writes.  Switching a comparison mode
//   therefore calls no Source::apply at all.  A source that leaves the
//   comparison members empty keeps the plain control set and draws its visible
//   background channels. See include/mikudancestudio/animation_curve_comparison.hpp
//   for the exact non-constant rule the range filter uses.
// * The channel tree groups tracks by stable collection paths. Checkboxes
//   control visibility, including all descendants of a collection; clicking a
//   leaf activates that curve without clearing other visible curves. Space
//   toggles visibility and Enter activates a leaf or expands a collection.
//   Rebuilding after a native read preserves expansion; reopening starts with
//   the source's requested bone branch. The active leaf carries a filled dot.
// * Comparison display is one segmented control of three touching push-like
//   segments in the visual order "same group - free - four groups".  The stored
//   mode values keep their meaning (1 same group, 2 four groups, 3 free), so
//   only the visual order is 1-3-2.  One click picks a segment, which is what
//   makes it the highlighted one.  The off state is internal only: it is what a
//   source without comparison support holds, and no window offers an off entry.
// * The display flag Channel::selected is the whole truth of what is drawn: a
//   tick means "this curve is on screen".  An automatic segment is a way to
//   compute that set, so picking it writes the result into the flags
//   (MaterializeCompareMask), keeps the main curve visible, and also restores
//   the active curve to Source::initialMainChannel.  A manual show/hide and a
//   double click leave the automatic segment: they move the display to the free
//   segment first, which carries the set that is on screen right now, and then
//   change only the row they name - the active curve is theirs to keep.  The
//   range filter button follows the same rule: one click, one run, its result
//   written into the flags and shown through the free segment, without moving
//   the active curve.  A refused mode write changes nothing at all and reports
//   the reason, and no mode path ever writes curve data.
// * The hint line above the status control explains the control under the
//   pointer.  It is a control of its own, so a hover can never overwrite the
//   persistent status or error text; leaving the control clears the hint only.
//
// Control ids (children of the editor window; the regression test in
// tests/animation_curve_editor_tests.cpp mirrors these numbers):
//   1001 source label        1002 source combo
//   1003 read motion         1004 write motion     1005 reserved
//   1006 view label          1007 view combo       1008 tool label
//   1009 tool combo          1010 fit all          1011 fit range
//   1012 shortcut toggle     1013 shortcut text
//   1014 channel label       1015 channel list     1016 select all channels
//   1020 curve canvas        1030 MMD label        1031 MMD panel
//   1032 MMD readout
//   1040 frame label         1041 frame edit       1042 value label
//   1043 value edit          1044 interpolation label 1045 interpolation combo
//   1046 tangent label       1047 tangent combo
//   1048 in slope label      1049 in slope edit
//   1050 out slope label     1051 out slope edit
//   1052 in weight label     1053 in weight edit
//   1054 out weight label    1055 out weight edit
//   1056 add key             1057 delete key        1058 apply key
//   1059 undo                1060 redo
//   1070 bake first label    1071 bake first edit  1072 bake last label
//   1073 bake last edit      1074 bake step label  1075 bake step edit
//   1076 bake run
//   1080 status text         1081 compare label
//   1082 compare segment: same group (left)
//   1087 compare segment: free (middle)
//   1086 compare segment: four groups (right)
//   1083 compare range filter 1085 hover hint text
//
// The comparison controls exist only while the current source declares
// comparison support; LayoutEditor hides them and leaves the other rows where
// they were for every other source.  The hint line exists for every source.
//
// Test hooks (WM_APP, answered by the editor window; read-only geometry on top
// of the real message paths used by background tests):
//   kHookPointForKey  = WM_APP+300  wParam = key index in the active channel,
//                                   returns MAKELPARAM(canvas client x, y) or
//                                   (-1,-1) when the key is not visible.
//   kHookPointForFrame= WM_APP+301  wParam = frame, returns the canvas client
//                                   point of that frame at the active key value.
//   kHookMmdControls  = WM_APP+302  lParam = double[4], returns TRUE when the
//                                   MMD panel can show the selected interval and
//                                   fills the displayed 0..127 controls.
//   kHookKeyCount     = WM_APP+303  wParam = channel index or -1 for the active
//                                   channel, returns the key count, or -1.
//   kHookView         = WM_APP+304  lParam = double[4] {timeStart, timeEnd,
//                                   valueLow, valueHigh}, returns viewMode.
//   kHookBarPoint     = WM_APP+305  wParam = 1 time bar / 2 value bar,
//                                   lParam = 0 centre, 1 low end, 2 high end,
//                                   returns MAKELPARAM(client x, y) of that
//                                   range-bar segment.
//   kHookFocusId      = WM_APP+306  returns the control id that currently has
//                                   the keyboard focus, 0 for the editor
//                                   window itself, -1 for no focus.
//   kHookCompare      = WM_APP+308  lParam = int[12]: [0] comparison mode,
//                                   [1] main channel index or -1, [2] drawn
//                                   comparison curve count, [3..10] their
//                                   channel indices, [11] hidden channel count.
//                                   Returns the mode.
//   kHookCompareRange = WM_APP+309  lParam = double[3] {first, last, source}
//                                   with source 1 for the selected keys, 2 for
//                                   the range fields and 0 for no span at all.
//                                   Returns 1 when a span exists.
//   kHookChannelVisible = WM_APP+310 wParam = channel index, returns 1 when the
//                                   curve is shown, 0 when it is hidden and -1
//                                   for an index outside the channel list.
//   kHookHoverHint    = WM_APP+311  returns the hint currently shown, 0 for
//                                   none: 1 channel list, 2 comparison mode,
//                                   3 range filter, 4 add key, 5 delete key,
//                                   6 apply key, 7 canvas, 8 MMD handles,
//                                   9 time bar, 10 value bar.
//
// Playhead: while the window is visible and Session::currentFrame is set, a
// WM_TIMER tick about 25 times a second repaints only the columns the playhead
// left and entered.  A hidden editor runs no timer and repaints nothing, and a
// tick never reads or writes channel data.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <windowsx.h>
#include <objidl.h>
#include "mikudancestudio/gdiplus_compat.hpp"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "mikudancestudio/animation_curve_comparison.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
#include "mikudancestudio/interpolation_link.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <locale.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "curve_editor_theme.hpp"
#include "curve_editor_selection.hpp"
#include "curve_editor_segments.hpp"
#include "curve_editor_scrub.hpp"
#include "curve_editor_axis.hpp"
#include "curve_editor_channel_scroll.hpp"
#include "ui_control_layout.hpp"


#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace mikudancestudio {
namespace curve_editor {
namespace {

using animation_curve::Interpolation;
using animation_curve::Key;
using animation_curve::Sample;
using animation_curve::Tangent;
using animation_curve::Track;
using curve_comparison::IsConstantOverRange;
using curve_comparison::IsVarying;

// ---------------------------------------------------------------------------
// Layout constants, all in logical pixels at 96 DPI.
// ---------------------------------------------------------------------------
constexpr int kMargin = 8;
constexpr int kGap = 6;             // never below the 4 logical pixel minimum
constexpr int kRowHeight = 22;
constexpr int kButtonHeight = 24;
constexpr int kStatusHeight = 20;
constexpr int kHintHeight = 20;
constexpr int kChannelWidth = 210;
constexpr int kBarThickness = 36;   // time bar at the bottom, value bar at the right
constexpr int kMmdSquare = 168;
constexpr int kMmdColumn = 340;
constexpr int kHelpHeight = 96;
constexpr int kMinimumClientWidth = 820;
constexpr int kMinimumClientHeight = 560;
constexpr int kDefaultClientWidth = 1180;
constexpr int kDefaultClientHeight = 760;
constexpr int kUndoDepth = 64;
constexpr double kMinimumTimeSpan = 1.0;
constexpr double kMinimumValueSpan = 1.0e-5;
constexpr double kFramesPerSecond = 30.0;
constexpr double kHandleHitRadius = 5.0;
constexpr int kHandleSize = 7;      // key square, logical pixels
// Three persistent handle modes, followed by four one-shot slope presets.
constexpr int kTangentAutomatic = 2;
constexpr int kTangentFlat = 3;
constexpr int kTangentStraight = 4;
constexpr int kTangentEaseIn = 5;
constexpr int kTangentEaseOut = 6;
constexpr int kGripSize = 9;        // range bar end grip, logical pixels

enum : int {
    kIdSourceLabel = 1001, kIdSourceCombo = 1002,
    kIdReadMotion = 1003, kIdWriteMotion = 1004,
    kIdViewLabel = 1006, kIdViewCombo = 1007, kIdToolLabel = 1008, kIdToolCombo = 1009,
    kIdFitAll = 1010, kIdFitRange = 1011, kIdShortcutToggle = 1012, kIdShortcutText = 1013,
    kIdChannelLabel = 1014, kIdChannelList = 1015, kIdChannelAll = 1016, kIdChannelInvert = 1017,
    kIdCanvas = 1020, kIdPlay = 1021, kIdPreviousFrame = 1022, kIdNextFrame = 1023,
    kIdTimelineFrame = 1024, kIdSeekFrame = 1025,
    kIdPreviousKey = 1026, kIdNextKey = 1027,
    kIdMmdLabel = 1030, kIdMmdPanel = 1031, kIdMmdReadout = 1032,
    kIdMmdX1 = 1096, kIdMmdY1 = 1097, kIdMmdX2 = 1098, kIdMmdY2 = 1099,
    kIdFrameLabel = 1040, kIdFrameEdit = 1041,
    kIdValueLabel = 1042, kIdValueEdit = 1043,
    kIdInterpolationLabel = 1044, kIdInterpolationCombo = 1045,
    kIdTangentLabel = 1046, kIdTangentCombo = 1047,
    kIdInSlopeLabel = 1048, kIdInSlopeEdit = 1049,
    kIdOutSlopeLabel = 1050, kIdOutSlopeEdit = 1051,
    kIdInWeightLabel = 1052, kIdInWeightEdit = 1053,
    kIdOutWeightLabel = 1054, kIdOutWeightEdit = 1055,
    kIdAddKey = 1056, kIdDeleteKey = 1057, kIdApplyKey = 1058,
    kIdUndo = 1059, kIdRedo = 1060,
    kIdBakeFirstLabel = 1070, kIdBakeFirstEdit = 1071,
    kIdBakeLastLabel = 1072, kIdBakeLastEdit = 1073,
    kIdBakeStepLabel = 1074, kIdBakeStepEdit = 1075,
    kIdBakeRun = 1076,
    kIdStatus = 1080,
    kIdCompareLabel = 1081, kIdCompareSameGroup = 1082,
    kIdCompareFilter = 1083,
    kIdHoverHint = 1085, kIdCompareAllGroups = 1086, kIdCompareFree = 1087,
    kIdMouseReadout = 1095,
};

// Where the span a filter run uses comes from.
enum : int { kSpanNone = 0, kSpanKeys = 1, kSpanFields = 2 };

constexpr UINT kHookPointForKey = WM_APP + 300;
constexpr UINT kHookPointForFrame = WM_APP + 301;
constexpr UINT kHookMmdControls = WM_APP + 302;
constexpr UINT kHookKeyCount = WM_APP + 303;
constexpr UINT kHookView = WM_APP + 304;
constexpr UINT kHookBarPoint = WM_APP + 305;
constexpr UINT kHookFocusId = WM_APP + 306;
constexpr UINT kHookPlayheadState = WM_APP + 307;
constexpr UINT kHookCompare = WM_APP + 308;
constexpr UINT kHookCompareRange = WM_APP + 309;
constexpr UINT kHookChannelVisible = WM_APP + 310;
constexpr UINT kHookHoverHint = WM_APP + 311;

// Playhead refresh.  The timer only repaints the columns the playhead leaves
// and enters; it never reads or writes channel data.
constexpr UINT_PTR kPlayheadTimer = 1;
constexpr UINT kPlayheadIntervalMs = 40;  // about 25 Hz
constexpr UINT kSourceChangedMessage = WM_APP + 401;
constexpr UINT_PTR kSourceChangedTimer = 2;
// Key frames are std::int32_t in the curve core, so this is the largest frame
// the editor accepts from an edit box or a drag.
constexpr long long kMaximumFrame = 2147483647;

const wchar_t kEditorClass[] = L"MikuMikuStudioCurveEditorWindow";
const wchar_t kCanvasClass[] = L"MikuMikuStudioCurveCanvasWindow";
const wchar_t kMmdClass[] = L"MikuMikuStudioCurveMmdWindow";

// ---------------------------------------------------------------------------
// Localisation.  Language 0 is Japanese, 1 English, 2 Chinese.
// ---------------------------------------------------------------------------
enum Str {
    StrTitle, StrSource, StrReadMotion, StrWriteMotion,
    StrView, StrViewValue, StrViewSpeed, StrTool, StrToolSelect, StrToolPan,
    StrFitAll, StrFitRange, StrShortcuts, StrShortcutsText,
    StrChannels, StrSelectAll, StrInvertSelection, StrMmdTitle, StrKeyFrame, StrKeyValue,
    StrInterpolation, StrTangent, StrInSlope, StrOutSlope, StrInWeight, StrOutWeight,
    StrAddKey, StrDeleteKey, StrApply, StrUndo, StrRedo,
    StrBakeFirst, StrBakeLast, StrBakeStep, StrBakeRun,
    StrStep, StrLinear, StrBezier,
    StrTanFree, StrTanAligned, StrTanFlat, StrTanLinear, StrTanAutomatic, StrTanEaseIn, StrTanEaseOut,
    StrReady, StrNoSource, StrNoRead, StrNoApply, StrReadFailed, StrApplyRejected, StrSourceChanged, StrSelectionChanged,
    StrInvalidTrack, StrNeedChannel, StrNoSelection, StrFrameExists, StrFrameSelected,
    StrInvalidFrame,
    StrInvalidNumber, StrDragConflict, StrAdded, StrDeleted, StrMoved, StrSelectedKeys,
    StrUndone, StrRedone, StrNothingUndo, StrNothingRedo, StrBakeDone, StrBakeFailed,
    StrNoChannels, StrMotionReadDone, StrMotionWriteDone, StrMotionFailed,
    StrMmdNoInterval, StrMmdDiscrete, StrMmdStep, StrMmdEqual, StrMmdOvershoot,
    StrDiscreteNoSpeed, StrNoKeys, StrSingleKeyForFrame, StrCleared,
    StrCompare, StrCompareSameGroup, StrCompareAllGroups, StrCompareFree,
    StrCompareFilter, StrCompareRangeHead, StrCompareFromKeys, StrCompareFromFields,
    StrCompareKeptHead, StrCompareKeptTail, StrCompareNoRange, StrCompareFilterFree,
    StrCompareEmptyCurve,
    StrCompareModeHead, StrCompareModeFailed,
    StrHiddenCurve,
    StrChannelShown, StrChannelHidden, StrActiveCurve,
    StrHintChannelList, StrHintCompareMode, StrHintCompareFilter,
    StrHintAddKey, StrHintDeleteKey, StrHintApplyKey, StrHintInterpolation, StrHintTangent,
    StrHintCanvas, StrHintMmdHandles, StrHintTimeBar, StrHintValueBar, StrHintNavigation, StrHintHistory
};

const wchar_t *T(unsigned language, Str id) {
    const bool zh = language == 2, en = language == 1;
    switch (id) {
    case StrTitle: return zh ? L"曲线编辑器" : en ? L"Curve editor" : L"カーブエディター";
    case StrInvertSelection: return zh ? L"反选" : en ? L"Invert" : L"反転";
    case StrSource: return zh ? L"源" : en ? L"Source" : L"ソース";
    case StrReadMotion: return zh ? L"读取动作" : en ? L"Read motion" : L"モーション読込";
    case StrWriteMotion: return zh ? L"写入动作" : en ? L"Write motion" : L"モーション書出";
    case StrView: return zh ? L"视图" : en ? L"View" : L"表示";
    case StrViewValue: return zh ? L"数值" : en ? L"Value" : L"数値";
    case StrViewSpeed: return zh ? L"速度" : en ? L"Speed" : L"速度";
    case StrTool: return zh ? L"工具" : en ? L"Tool" : L"ツール";
    case StrToolSelect: return zh ? L"选择" : en ? L"Select" : L"選択";
    case StrToolPan: return zh ? L"平移" : en ? L"Pan" : L"パン";
    case StrFitAll: return zh ? L"适配全部" : en ? L"Fit all" : L"全体表示";
    case StrFitRange: return zh ? L"适配区间" : en ? L"Fit range" : L"範囲表示";
    case StrShortcuts: return zh ? L"快捷键说明" : en ? L"Shortcuts" : L"ショートカット";
    case StrShortcutsText:
        return zh ? L"中键或空格加左键：双轴平移。滚轮纵移，Shift 横移，Alt 横缩，Ctrl 纵缩，Ctrl+Alt 双缩。Home 适配全部，F 适配当前区间，H 平移工具，V 选择工具。范围条中央拖动平移，两端拖动缩放；点击横条或纵条的中段或两端后，方向键执行同样的平移或缩放，按住 Shift 步长为十倍。Tab 在控件间移动焦点。快捷键不会写入输入框。"
                  : en ? L"Middle button or Space+drag: two-axis pan. Wheel pans vertically, Shift pans horizontally, Alt zooms time, Ctrl zooms value, Ctrl+Alt zooms both. Home fits all, F fits the current range, H selects the pan tool, V the select tool. Drag the range bar centre to pan and its ends to zoom; after clicking the middle or an end of the time or value bar the arrow keys pan or zoom the same way, ten times further with Shift. Tab moves the focus between controls. Shortcuts never reach an edit box."
                       : L"中ボタンまたはスペース＋ドラッグ：2 軸パン。ホイールは縦移動、Shift は横移動、Alt は時間ズーム、Ctrl は値ズーム、Ctrl+Alt は両方。Home で全体表示、F で範囲表示、H でパンツール、V で選択ツール。範囲バーの中央で移動、両端でズーム。時間バーまたは値バーの中央・両端をクリックした後は方向キーでも同じ移動・ズームができ、Shift で 10 倍になります。Tab でコントロール間を移動します。ショートカットは入力欄に入力されません。";
    case StrChannels: return zh ? L"通道" : en ? L"Channels" : L"チャンネル";
    case StrSelectAll: return zh ? L"全选" : en ? L"Select all" : L"全選択";
    case StrMmdTitle: return zh ? L"原版 0–127 手柄" : en ? L"MMD 0–127 handles" : L"0–127 ハンドル";
    case StrKeyFrame: return zh ? L"帧" : en ? L"Frame" : L"フレーム";
    case StrKeyValue: return zh ? L"数值" : en ? L"Value" : L"値";
    case StrInterpolation: return zh ? L"插值" : en ? L"Interpolation" : L"補間";
    case StrTangent: return zh ? L"切线" : en ? L"Tangent" : L"接線";
    case StrInSlope: return zh ? L"入斜率" : en ? L"In slope" : L"入力傾き";
    case StrOutSlope: return zh ? L"出斜率" : en ? L"Out slope" : L"出力傾き";
    case StrInWeight: return zh ? L"入权重" : en ? L"In weight" : L"入力重み";
    case StrOutWeight: return zh ? L"出权重" : en ? L"Out weight" : L"出力重み";
    case StrAddKey: return zh ? L"添加关键帧" : en ? L"Add key" : L"キー追加";
    case StrDeleteKey: return zh ? L"删除关键帧" : en ? L"Delete key" : L"キー削除";
    case StrApply: return zh ? L"应用" : en ? L"Apply" : L"適用";
    case StrUndo: return zh ? L"撤销" : en ? L"Undo" : L"元に戻す";
    case StrHintHistory:
        return zh?L"Ctrl+Z 撤销，Ctrl+Y 或 Ctrl+Shift+Z 重做。Enter 或 Insert 在当前帧插入关键帧，Delete 删除所选关键帧。输入框保留文字编辑快捷键。":
            en?L"Ctrl+Z: undo. Ctrl+Y / Ctrl+Shift+Z: redo. Enter / Insert: insert at the current frame. Delete: delete selected keys. Text fields keep their text-editing shortcuts.":
            L"Ctrl+Z：元に戻す。Ctrl+Y / Ctrl+Shift+Z：やり直す。Enter / Insert：現在フレームにキー挿入。Delete：選択キーを削除。入力欄では文字編集を優先します。";
    case StrRedo: return zh ? L"重做" : en ? L"Redo" : L"やり直し";
    case StrBakeFirst: return zh ? L"起始帧" : en ? L"Start" : L"開始";
    case StrBakeLast: return zh ? L"结束帧" : en ? L"End" : L"終了";
    case StrBakeStep: return zh ? L"步长" : en ? L"Step" : L"間隔";
    case StrBakeRun: return zh ? L"烘焙选中通道" : en ? L"Bake selected channels" : L"選択チャンネルをベイク";
    case StrStep: return zh ? L"阶跃" : en ? L"Step" : L"ステップ";
    case StrLinear: return zh ? L"线性" : en ? L"Linear" : L"リニア";
    case StrBezier: return zh ? L"贝塞尔" : en ? L"Bezier" : L"ベジェ";
    case StrTanFree: return zh ? L"自由" : en ? L"Free" : L"フリー";
    case StrTanAligned: return zh ? L"对齐" : en ? L"Aligned" : L"整列";
    case StrTanFlat: return zh ? L"水平" : en ? L"Flat" : L"水平";
    case StrTanLinear: return zh ? L"拉直" : en ? L"Straighten" : L"直線化";
    case StrTanAutomatic: return zh ? L"自动" : en ? L"Automatic" : L"自動";
    case StrTanEaseIn: return zh ? L"缓入" : en ? L"Ease in" : L"イーズイン";
    case StrTanEaseOut: return zh ? L"缓出" : en ? L"Ease out" : L"イーズアウト";
    case StrReady: return zh ? L"就绪。" : en ? L"Ready." : L"準備完了。";
    case StrNoSource: return zh ? L"没有可用的源。" : en ? L"No source is available." : L"利用できるソースがありません。";
    case StrNoRead: return zh ? L"该源没有读取回调，编辑已禁用。" : en ? L"The source has no read callback; editing is disabled." : L"このソースには読み込みコールバックがなく、編集できません。";
    case StrNoApply: return zh ? L"该源没有写入回调，编辑已禁用。" : en ? L"The source has no apply callback; editing is disabled." : L"このソースには書き込みコールバックがなく、編集できません。";
    case StrReadFailed: return zh ? L"读取失败：" : en ? L"Read failed: " : L"読み込みに失敗しました：";
    case StrApplyRejected: return zh ? L"写入被拒绝，编辑状态未更改：" : en ? L"The write was rejected; the editing state is unchanged: " : L"書き込みが拒否されました。編集状態は変更されていません：";
    case StrSourceChanged: return zh ? L"主界面中的关键帧已更改，已刷新曲线。请重试本次编辑。" : en ? L"The main window changed the keys. Curves have been refreshed; retry this edit." : L"メイン画面でキーが変更されました。カーブを更新しました。編集をやり直してください。";
    case StrSelectionChanged: return zh ? L"关键帧选区已更改，已刷新参数。请重试本次编辑。" : en ? L"The key selection changed. Fields have been refreshed; retry this edit." : L"キーの選択が変更されました。パラメーターを更新しました。編集をやり直してください。";
    case StrInvalidTrack: return zh ? L"修改无效：" : en ? L"Invalid change: " : L"変更が無効です：";
    case StrNeedChannel: return zh ? L"请先选择一个通道。" : en ? L"Select a channel first." : L"先にチャンネルを選択してください。";
    case StrNoSelection: return zh ? L"没有选中的关键帧。" : en ? L"No keyframe is selected." : L"選択されたキーがありません。";
    case StrFrameExists: return zh ? L"该帧已有关键帧：" : en ? L"A key already exists at frame " : L"このフレームには既にキーがあります：";
    case StrInvalidFrame: return zh ? L"帧号必须是 0 到 2147483647 之间的整数。" : en ? L"The frame must be an integer from 0 to 2147483647." : L"フレームは 0～2147483647 の整数で入力してください。";
    case StrFrameSelected:
        // Pressing add on an existing frame selects that key instead of
        // writing anything, so this is an informational line, not an error.
        return zh ? L"该帧已有关键帧，已选中它：帧 " : en ? L"A key already exists at frame " : L"このフレームには既にキーがあります。選択しました：フレーム ";
    case StrInvalidNumber: return zh ? L"请输入有效数字。" : en ? L"Enter a valid number." : L"有効な数値を入力してください。";
    case StrDragConflict: return zh ? L"拖动被拒绝：目标帧与现有键冲突，关键帧未移动。" : en ? L"Drag rejected: the target frames collide with existing keys; no key moved." : L"ドラッグは拒否されました。移動先が既存のキーと衝突するため、キーは移動していません。";
    case StrAdded: return zh ? L"已添加关键帧，帧 " : en ? L"Key added at frame " : L"キーを追加しました：フレーム ";
    case StrDeleted: return zh ? L"已删除关键帧，数量 " : en ? L"Keys deleted: " : L"キーを削除しました：";
    case StrMoved: return zh ? L"已移动关键帧，数量 " : en ? L"Keys moved: " : L"キーを移動しました：";
    case StrSelectedKeys: return zh ? L"已选中关键帧，数量 " : en ? L"Keys selected: " : L"キーを選択しました：";
    case StrUndone: return zh ? L"已撤销。" : en ? L"Undone." : L"元に戻しました。";
    case StrRedone: return zh ? L"已重做。" : en ? L"Redone." : L"やり直しました。";
    case StrNothingUndo: return zh ? L"没有可撤销的操作。" : en ? L"Nothing to undo." : L"元に戻す操作はありません。";
    case StrNothingRedo: return zh ? L"没有可重做的操作。" : en ? L"Nothing to redo." : L"やり直す操作はありません。";
    case StrBakeDone: return zh ? L"已烘焙通道，数量 " : en ? L"Channels baked: " : L"ベイクしたチャンネル数：";
    case StrBakeFailed: return zh ? L"烘焙失败：" : en ? L"Baking failed: " : L"ベイクに失敗しました：";
    case StrNoChannels: return zh ? L"请先选择至少一个通道。" : en ? L"Select at least one channel." : L"少なくとも 1 つのチャンネルを選択してください。";
    case StrMotionReadDone: return zh ? L"已读取动作并重新载入。" : en ? L"Motion read and reloaded." : L"モーションを読み込み、再読込しました。";
    case StrMotionWriteDone: return zh ? L"已写入动作。" : en ? L"Motion written." : L"モーションを書き出しました。";
    case StrMotionFailed: return zh ? L"动作操作失败：" : en ? L"Motion action failed: " : L"モーション操作に失敗しました：";
    case StrMmdNoInterval: return zh ? L"请选择区间结束的关键帧。" : en ? L"Select the key that ends the interval." : L"区間の終端キーを選択してください。";
    case StrMmdDiscrete: return zh ? L"离散通道没有贝塞尔手柄。" : en ? L"A discrete channel has no Bezier handles." : L"離散チャンネルにハンドルはありません。";
    case StrMmdStep: return zh ? L"阶跃区间无法用原版手柄表示。" : en ? L"A step interval cannot be shown with MMD handles." : L"ステップ区間はハンドルで表現できません。";
    case StrMmdEqual: return zh ? L"端点数值相等，原版按差值归一化，无法表示。" : en ? L"The endpoints are equal; MMD normalises by the difference, so this cannot be shown." : L"端点の値が等しく、差分で正規化するため表現できません。";
    case StrMmdOvershoot: return zh ? L"该区间超出手柄可表示范围（过冲或权重越界），请用完整曲线编辑。" : en ? L"This interval is outside the handle range (overshoot or weight limits); edit the full curve instead." : L"この区間はハンドルで表現できません（オーバーシュートまたは重み超過）。完全なカーブで編集してください。";
    case StrDiscreteNoSpeed: return zh ? L"离散通道不显示速度曲线。" : en ? L"A discrete channel has no speed curve." : L"離散チャンネルに速度カーブはありません。";
    case StrNoKeys: return zh ? L"当前通道没有关键帧。" : en ? L"The current channel has no keys." : L"現在のチャンネルにキーがありません。";
    case StrSingleKeyForFrame: return zh ? L"修改帧号需要只选中一个关键帧。" : en ? L"Editing the frame requires exactly one selected key." : L"フレームの変更は 1 つのキーだけを選択したときに行えます。";
    case StrCleared: return zh ? L"已清除选择。" : en ? L"Selection cleared." : L"選択を解除しました。";
    case StrCompare: return zh ? L"对比" : en ? L"Compare" : L"比較";
    case StrCompareSameGroup: return zh ? L"同组变化" : en ? L"Same group" : L"同グループ";
    case StrCompareAllGroups: return zh ? L"四组对比" : en ? L"Four groups" : L"4 グループ";
    case StrCompareFree: return zh ? L"自由显示" : en ? L"Free" : L"自由表示";
    case StrCompareFilter:
        return zh ? L"筛选非常值" : en ? L"Select non-constant" : L"変化を選択";
    case StrCompareRangeHead: return zh ? L"对比范围 " : en ? L"Comparison range " : L"比較範囲 ";
    case StrCompareFromKeys:
        return zh ? L"（来自选中关键帧）" : en ? L" (from the selected keys)" : L"（選択キーから）";
    case StrCompareFromFields:
        return zh ? L"（来自范围字段）" : en ? L" (from the range fields)" : L"（範囲欄から）";
    case StrCompareKeptHead: return zh ? L"：已勾选 " : en ? L": " : L"：変化するカーブ ";
    case StrCompareKeptTail:
        return zh ? L" 条非常值曲线，主曲线始终保留。"
                  : en ? L" non-constant curves; the main curve is always kept."
                       : L" 本を選択しました。メインカーブは常に残します。";
    case StrCompareNoRange:
        return zh ? L"没有可用的时间段：请选中至少两个关键帧，或在起止帧字段里填写帧号。"
                  : en ? L"No usable range: select at least two keys, or fill the start and end frame fields."
                       : L"利用できる範囲がありません。2 つ以上のキーを選択するか、開始・終了フレーム欄に入力してください。";
    case StrCompareFilterFree:
        return zh ? L"，显示已切到自由显示。" : en ? L", the display switched to the free mode."
                                               : L"。表示を自由表示に切り替えました。";
    case StrCompareEmptyCurve:
        return zh ? L"该曲线还没有关键帧，不参与对比显示。"
                  : en ? L"That curve has no keys yet and takes no part in the comparison display."
                       : L"そのカーブにはまだキーが無く、比較表示の対象外です。";
    case StrCompareModeHead: return zh ? L"对比模式：" : en ? L"Comparison mode: " : L"比較モード：";
    case StrCompareModeFailed:
        return zh ? L"对比模式保存失败，模式未更改："
                  : en ? L"The comparison mode could not be stored; the mode is unchanged: "
                       : L"比較モードを保存できませんでした。モードは変更していません：";
    case StrHiddenCurve:
        return zh ? L"当前活动曲线已在列表中隐藏，双击该行即可恢复显示。"
                  : en ? L"The active curve is hidden in the list; double-click its row to show it again."
                       : L"編集中のカーブは一覧で非表示です。行をダブルクリックすると再表示します。";
    case StrChannelShown: return zh ? L"已显示曲线：" : en ? L"Curve shown: " : L"表示：";
    case StrChannelHidden: return zh ? L"已隐藏曲线：" : en ? L"Curve hidden: " : L"非表示：";
    case StrActiveCurve: return zh ? L"活动曲线：" : en ? L"Active curve: " : L"編集カーブ：";
    // Hover hints: one short line per control, shown in the hint line above the
    // status control while the pointer rests on that control.
    case StrHintChannelList:
        return zh ? L"通道集合：展开物体、骨骼和类型；点击轴通道开始编辑，勾选框切换显示；集合勾选框批量显示或隐藏。空格切换显示，回车编辑通道或展开集合。"
                  : en ? L"Collections: expand object, bone and type; click a channel to edit, check to show. Group checks toggle all descendants. Space toggles visibility; Enter edits or expands."
                       : L"コレクション：物体・ボーン・種類を展開し、チャンネルをクリックして編集します。チェックで表示切替、グループで一括切替。Space で表示、Enter で編集または展開。";
    case StrHintCompareMode:
        return zh ? L"对比模式（三段）：左段同组变化显示当前曲线同分类的已登记变化曲线，中段自由显示只显示列表里勾选的曲线，右段四组对比显示四个分类的曲线；点任一段会立即选中该段，并把活动曲线恢复到打开窗口时的主表情并显示它。"
                  : en ? L"Comparison mode (three segments): the left segment shows the registered curves that change in this curve's own group, the middle one shows only what the list has ticked, the right one shows all four groups; clicking a segment selects it at once and returns the active curve to the expression the window was opened for, showing it."
                       : L"比較モード（3 分割）：左は同じ分類で変化する登録済みカーブ、中央は一覧で選択したカーブのみ、右は 4 分類のカーブを表示します。押した段がすぐ選択され、編集カーブは起動時のメイン表情に戻して表示します。";
    case StrHintCompareFilter:
        return zh ? L"筛选非常值（单击执行一次）：按选中两个以上关键帧的最小最大帧（否则用起止帧字段）勾选该范围内有变化的曲线，并把显示切到自由显示；活动曲线保持不变，状态栏会写明实际范围。"
                  : en ? L"Select non-constant (one click, one run): ticks the curves that change inside the span (the smallest and largest of two or more selected keys, else the start and end fields) and switches the display to the free mode; the active curve does not move and the status line names the span that was used."
                       : L"変化を選択（1 回だけ実行）：選択キー 2 つ以上の最小最大（なければ開始・終了欄）の区間で変化するカーブを選択し、表示を自由表示に切り替えます。編集カーブは変わりません。使用した範囲はステータス行に表示します。";
    case StrHintAddKey:
        return zh ? L"添加关键帧：在帧号处添加关键帧；该帧已有关键帧时改为选中它。"
                  : en ? L"Add key: inserts a key at the frame number; when that frame already has one, it is selected instead."
                       : L"キー追加：フレーム欄の位置にキーを追加します。既にキーがある場合はそれを選択します。";
    case StrHintDeleteKey:
        return zh ? L"删除关键帧：删除选中的关键帧，可用撤销恢复。"
                  : en ? L"Delete key: removes the selected keys; undo restores them."
                       : L"キー削除：選択中のキーを削除します。元に戻すで復元できます。";
    case StrHintApplyKey:
        return zh ? L"应用（Enter）：把帧号、数值、斜率和权重写入选中的关键帧，可撤销。插值与切线按钮点击后立即生效。"
                  : en ? L"Apply (Enter): writes frame, value, slopes and weights to the selected key. Interpolation and tangent buttons apply immediately. Undo is available."
                       : L"適用（Enter）：フレーム・値・傾き・重みを選択キーに書き込みます。補間と接線のボタンは即時適用します。元に戻せます。";
    case StrHintInterpolation:
        return zh ? L"插值立即应用到所选关键帧之前的区间，可撤销。阶跃和线性不读取手柄斜率；贝塞尔读取斜率与权重。"
                  : en ? L"Applies immediately to the interval before each selected key. Step and Linear ignore handles; Bezier uses slopes and weights. Undo is available."
                       : L"選択キーの前の区間へ即時適用します。ステップと線形はハンドルを使わず、ベジェは傾きと重みを使います。元に戻せます。";
    case StrHintTangent:
        return zh ? L"自由、对齐、自动是手柄模式；右侧水平、拉直、缓入、缓出是一次性预设。点击立即应用于所选关键帧两侧；需要手柄时会切换为贝塞尔，可撤销。"
                  : en ? L"Free, Aligned and Auto are handle modes. Flat, Straighten, Ease in and Ease out are one-shot presets. Applies to both sides immediately, enabling Bezier where needed. Undo is available."
                       : L"自由・整列・自動はハンドルモードです。水平・直線化・イーズイン・イーズアウトは一度だけ適用するプリセットです。両側へ即時適用し、必要な区間をベジェに切り替えます。元に戻せます。";
    case StrHintCanvas:
        return zh ? L"画布：单击选中关键帧，拖动移动，空白拖动框选；选中关键帧的圆形手柄可拖动调整曲线；Alt+单击关键帧在线性与双侧贝塞尔之间切换，Alt+单击手柄拉直该侧。滚轮纵移，Shift 横移，Alt 横缩，Ctrl 纵缩，Home 全部显示，F 区间显示，H/V 切换平移与选择。"
                  : en ? L"Canvas: click a key to select it, drag to move it, drag empty space to box-select; drag the round handles of selected keys to shape the curve; Alt-click a key to toggle linear / two-sided Bezier, Alt-click a handle to straighten that side. Wheel pans, Shift pans sideways, Alt zooms time, Ctrl zooms value, Home fits all, F fits the range, H and V pick the pan or select tool."
                       : L"キャンバス：キーをクリックで選択、ドラッグで移動、空白ドラッグで範囲選択。選択キーの丸いハンドルをドラッグでカーブ調整。Alt+クリックでキーを線形／両側ベジェ切替、ハンドルを Alt+クリックでその側を直線化。ホイール縦移動、Shift 横移動、Alt 時間ズーム、Ctrl 値ズーム、Home 全体表示、F 範囲表示、H/V でパンと選択を切替。";
    case StrHintMmdHandles:
        return zh ? L"控制点：拖动手柄同步坐标；输入整数后按 Enter 更新，可撤销。横坐标 1–126，纵坐标 0–127，起始点横坐标须小于终止点。"
                  : en ? L"Control points: drag handles to update coordinates, or enter integers and press Enter. X: 1-126, Y: 0-127; start X must be less than end X. Undo is available."
                       : L"制御点：ドラッグで座標を更新、整数入力後 Enter で適用。X: 1–126、Y: 0–127、始点 X < 終点 X。元に戻すも可能です。";
    case StrHintNavigation:
        return zh ? L"帧导航：从左至右为上一关键帧、上一帧、下一帧、下一关键帧。关键帧跳转跟随当前通道。"
                  : en ? L"Navigation: previous key, previous frame, next frame, next key. Key navigation uses the active channel."
                       : L"移動：前のキー、前のフレーム、次のフレーム、次のキー。キー移動は現在のチャンネルが対象です。";
    case StrHintTimeBar:
        return zh ? L"时间条：双击将滑块中心移到点击位置，缩放不变。拖中央平移，拖两端缩放；方向键同样有效，Shift 步长十倍。"
                  : en ? L"Time bar: double-click to centre the thumb here without zooming. Drag the middle to pan, ends to zoom; arrow keys do the same, Shift moves ten times further."
                       : L"時間バー：ダブルクリック位置へ中央を移動（倍率維持）。中央で移動、両端でズーム。方向キーも使用可、Shift で 10 倍。";
    case StrHintValueBar:
        return zh ? L"数值条：双击将滑块中心移到点击位置，缩放不变。拖中央平移，拖两端缩放；方向键同样有效，Shift 步长十倍。"
                  : en ? L"Value bar: double-click to centre the thumb here without zooming. Drag the middle to pan, ends to zoom; arrow keys do the same, Shift moves ten times further."
                       : L"値バー：ダブルクリック位置へ中央を移動（倍率維持）。中央で移動、両端でズーム。方向キーも使用可、Shift で 10 倍。";
    }
    return L"";
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
int Scale(int value, int dpi) { return MulDiv(value, dpi, 96); }

std::wstring Widen(const std::string &text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

std::wstring FormatNumber(double value) {
    wchar_t buffer[64] = {};
    std::swprintf(buffer, 64, L"%.6g", value);
    return buffer;
}

std::wstring FormatInteger(long long value) { return std::to_wstring(value); }

bool ParseDoubleText(const std::wstring &text, double &out) {
    std::wstring trimmed = text;
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    if (trimmed.empty()) return false;
    wchar_t *end = nullptr;
    _locale_t locale = _create_locale(LC_NUMERIC, "C");
    const double parsed = _wcstod_l(trimmed.c_str(), &end, locale);
    _free_locale(locale);
    if (!end || end == trimmed.c_str()) return false;
    while (*end && iswspace(*end)) ++end;
    if (*end) return false;
    if (!std::isfinite(parsed)) return false;
    out = parsed;
    return true;
}

bool ParseIntegerText(const std::wstring &text, long long &out) {
    double parsed = 0;
    if (!ParseDoubleText(text, parsed)) return false;
    if (parsed != std::floor(parsed)) return false;
    if (parsed < -2147483648.0 || parsed > 2147483647.0) return false;
    out = static_cast<long long>(parsed);
    return true;
}

std::wstring ControlText(HWND parent, int id) {
    HWND control = GetDlgItem(parent, id);
    if (!control) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, &text[0], length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void SetControlText(HWND parent, int id, const std::wstring &text) {
    if (HWND control = GetDlgItem(parent, id)) {
        if (ControlText(parent, id) != text) SetWindowTextW(control, text.c_str());
    }
}

double NiceStep(double span, int target) {
    if (!(span > 0.0) || target < 1) return 1.0;
    const double raw = span / target;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double normalised = raw / magnitude;
    const double step = normalised <= 1.0 ? 1.0 : normalised <= 2.0 ? 2.0 : normalised <= 5.0 ? 5.0 : 10.0;
    return step * magnitude;
}

bool SameTrack(const Track &left, const Track &right) {
    if (left.discrete != right.discrete) return false;
    if (left.keys.size() != right.keys.size()) return false;
    for (std::size_t index = 0; index < left.keys.size(); ++index) {
        const Key &a = left.keys[index];
        const Key &b = right.keys[index];
        if (a.frame != b.frame || a.value != b.value || a.inSlope != b.inSlope || a.outSlope != b.outSlope ||
            a.inWeight != b.inWeight || a.outWeight != b.outWeight || a.interpolation != b.interpolation ||
            a.tangent != b.tangent)
            return false;
    }
    return true;
}

// Decides whether a re-read is an external edit of the animation.  Channel
// labels, units and the Channel::selected view flag are deliberately ignored:
// a read that only renames or re-units channels must not throw away the local
// selection or the undo stacks, while any change of a key value, slope, weight,
// frame or discrete flag still counts as an external change.
bool SameCurveData(const std::vector<Channel> &left, const std::vector<Channel> &right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].id != right[index].id) return false;
        if (left[index].base != right[index].base) return false;
        if (left[index].minimum != right[index].minimum) return false;
        if (left[index].maximum != right[index].maximum) return false;
        if (!SameTrack(left[index].track, right[index].track) ||
            left[index].nativeControls != right[index].nativeControls ||
            left[index].controlGeometry != right[index].controlGeometry ||
            left[index].nativeOneFrameHolds != right[index].nativeOneFrameHolds) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// GDI+ lifetime.  The host never shuts GDI+ down, so the process keeps its
// token for the whole run and the launcher icon is always drawn antialiased.
// ---------------------------------------------------------------------------
ULONG_PTR GdiPlusToken() {
    static ULONG_PTR token = 0;
    static bool started = false;
    if (!started) {
        started = true;
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) token = 0;
    }
    return token;
}

// ---------------------------------------------------------------------------
// Editor state.
// ---------------------------------------------------------------------------
struct Snapshot {
    std::vector<Channel> channels;
    std::size_t active = 0;
    std::set<std::size_t> selection;
};

struct SourceState {
    bool loaded = false;
    bool valid = false;
    std::wstring reason;
    std::vector<Channel> channels;
    std::size_t active = 0;
    std::set<std::size_t> selection;
    std::vector<Snapshot> undo;
    std::vector<Snapshot> redo;
    std::wstring status;
    bool statusError = false;
    // Comparison display.  compareMode is the choice the host remembers for
    // this source; kCompareOff means the source offers no comparison at all,
    // which is the only place that state is used.
    int compareMode = kCompareOff;
    std::map<std::string, bool> expandedCollections;
};

enum class Drag { None, Seek, Pan, Keys, Rubber, TimeBarPan, TimeBarZoomLow, TimeBarZoomHigh,
                  ValueBarPan, ValueBarZoomLow, ValueBarZoomHigh, Handle };

struct PaintSurface {
    HDC dc=nullptr;
    HBITMAP bitmap=nullptr;
    HGDIOBJ previous=nullptr;
    int width=0,height=0;
    void Reset(){
        if(dc&&previous)SelectObject(dc,previous);
        if(bitmap)DeleteObject(bitmap);
        if(dc)DeleteDC(dc);
        dc=nullptr;bitmap=nullptr;previous=nullptr;width=height=0;
    }
    ~PaintSurface(){Reset();}
    bool Resize(HDC target,int w,int h){
        if(dc&&width==w&&height==h)return true;
        Reset();dc=CreateCompatibleDC(target);bitmap=CreateCompatibleBitmap(target,w,h);
        if(!dc||!bitmap){Reset();return false;}
        previous=SelectObject(dc,bitmap);width=w;height=h;return true;
    }
};
struct Editor {
    HWND window = nullptr;
    HWND canvas = nullptr;
    HWND mmd = nullptr;
    PaintSurface canvasSurface,mmdSurface;
    void *owner = nullptr;
    Session session;
    std::size_t sourceIndex = 0;
    std::vector<SourceState> states;
    std::set<std::string> pendingSources;
    bool sourceChangePosted = false;
    std::vector<HTREEITEM> channelItems;
    std::map<HTREEITEM, std::string> collectionItems;
    bool rebuildingChannels = false;
    std::string channelTreeSource;
    std::wstring channelTreeShape;
    int channelWidth = kChannelWidth; // Logical pixels; retained across window resizing.
    bool resizingChannels = false;
    int pendingChannelWidth = kChannelWidth;
    bool deferLayoutPaint = false;

    HFONT font = nullptr;
    HBRUSH themeShell = nullptr;
    bool advancedExpanded = true;
    int dpi = 96;
    bool helpExpanded = false;

    int viewMode = 0;  // 0 = value curve, 1 = analytic speed curve
    int tool = 0;      // 0 = select, 1 = pan
    double timeStart = 0, timeEnd = 100;
    double valueLow = -1, valueHigh = 1;
    double speedLow = -1, speedHigh = 1;

    // Canvas geometry in canvas client coordinates.
    RECT plot = {};
    RECT timeBar = {};
    RECT valueBar = {};
    POINT mousePoint = {};
    bool mouseInPlot = false;
    // Preserve the displayed overview after a double click, until the view
    // changes again. Otherwise auto-padding would move the thumb a second time.
    struct CenteredBar {
        bool valid=false;
        int mode=0;
        double low=0,high=1,start=0,end=1;
        bool Matches(double first,double last) const {
            // ClampView reconstructs endpoints around their centre; tolerate
            // its rounding only, not a meaningful navigation change.
            const auto same=[](double a,double b){return std::abs(a-b)<=
                8*std::numeric_limits<double>::epsilon()*std::max({1.0,std::abs(a),std::abs(b)});};
            return valid && same(start,first) && same(end,last);
        }
    } centeredTime,centeredValue;

    Drag drag = Drag::None;
    POINT dragOrigin = {};
    double savedTimeStart = 0, savedTimeEnd = 0, savedValueLow = 0, savedValueHigh = 0;
    double savedBarLow = 0, savedBarHigh = 1;
    int savedBarFirst = 0, savedBarLast = 1;
    std::vector<Key> dragBase;
    // Bezier handle drag on the canvas: key index and side (true = out).
    std::size_t handleKey = 0;
    bool handleOutSide = false;
    std::set<std::size_t> dragSelection;
    long long dragFrameDelta = 0;
    double dragValueDelta = 0;
    bool dragMoved = false;
    bool rubberAdd = false;
    RECT rubber = {};
    bool spaceDown = false;
    int hoverKey = -1;

    // MMD panel.
    bool mmdEnabled = false;
    std::wstring mmdReason;
    std::array<double, 4> mmdControls = {{0, 0, 0, 0}};
    int mmdEndKey = 0;
    int mmdDragHandle = -1;
    bool mmdFollowPlayhead = false;

    // Range-bar keyboard focus: 0 = none, 1 = time bar, 2 = value bar, with the
    // clicked segment in barZone (0 = centre pan, 1 = low edge, 2 = high edge).
    int barFocus = 0;
    int barZone = 0;

    // Playhead refresh.  playheadFrame is the frame as of the last tick and
    // lastPlayheadFrame the one before it, which is what the narrow repaint
    // band is built from.
    double playheadFrame = std::numeric_limits<double>::quiet_NaN();
    double lastPlayheadFrame = std::numeric_limits<double>::quiet_NaN();
    bool playheadTimerRunning = false;
    long long playheadTicks = 0;
    long long playheadInvalidations = 0;

    // Hover help.  hoverHint holds the Str value of the hint line currently
    // shown, or -1 when the pointer rests on nothing that explains itself.  The
    // hint never touches SourceState::status, so an error line stays readable.
    int hoverHint = -1;
};

std::mutex g_registryMutex;
std::vector<Editor *> g_editors;

Source &CurrentSource(Editor &editor) {
    static Source empty;
    if (editor.sourceIndex >= editor.session.sources.size()) return empty;
    return editor.session.sources[editor.sourceIndex];
}

SourceState &CurrentState(Editor &editor) {
    static SourceState empty;
    if (editor.sourceIndex >= editor.states.size()) return empty;
    return editor.states[editor.sourceIndex];
}

Channel *ActiveChannel(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) return nullptr;
    if (state.active >= state.channels.size()) state.active = state.channels.size() - 1;
    return &state.channels[state.active];
}

Track *ActiveTrack(Editor &editor) {
    Channel *channel = ActiveChannel(editor);
    return channel ? &channel->track : nullptr;
}

// The channel list tick is the display flag of that curve, so a hidden active
// curve is a curve the canvas does not draw - keys and tangent handles
// included.  Editing it stays possible through the key fields and the MMD
// panel, and one click or a double click in the list brings it back.
bool ActiveChannelVisible(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) return false;
    return state.channels[std::min(state.active, state.channels.size() - 1)].selected;
}

std::size_t ActiveKeyIndex(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if (state.selection.empty()) return static_cast<std::size_t>(-1);
    return *state.selection.rbegin();
}

void SetStatus(Editor &editor, const std::wstring &text, bool error) {
    SourceState &state = CurrentState(editor);
    state.status = text;
    state.statusError = error;
    if (HWND status = GetDlgItem(editor.window, kIdStatus)) SetWindowTextW(status, text.c_str());
    if (editor.window) InvalidateRect(GetDlgItem(editor.window, kIdStatus), nullptr, TRUE);
}

void ClampState(SourceState &state) {
    if (state.channels.empty()) {
        state.active = 0;
        state.selection.clear();
        return;
    }
    if (state.active >= state.channels.size()) state.active = state.channels.size() - 1;
    const std::size_t keyCount = state.channels[state.active].track.keys.size();
    for (auto it = state.selection.begin(); it != state.selection.end();) {
        if (*it >= keyCount)
            it = state.selection.erase(it);
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------
// Forward declarations of the interaction and painting entry points.
// ---------------------------------------------------------------------------
void LayoutEditor(Editor &editor);
void RefreshEditorsUi(Editor &editor);
void InvalidateCanvas(Editor &editor);
void UpdateMmdPanel(Editor &editor);
void ApplyMmdCoordinateFields(Editor &editor);
void OnPlayheadTimer(Editor &editor);
void FitAll(Editor &editor);
void ApplyLanguage(Editor &editor);
void PopulateChannels(Editor &editor);
void SyncSelectionToList(Editor &editor);
void UpdateWindowTitle(Editor &editor);
void FillSourceCombo(Editor &editor);
void UpdateCompareUi(Editor &editor);
void DrawComparisonCurves(Editor &editor, HDC dc);
void MaterializeCompareMask(Editor &editor);
void UpdateKeyFields(Editor &editor);
void UpdateBakeFields(Editor &editor);
void UpdateEnabledState(Editor &editor);
bool PullKeySelection(Editor &editor);
void PublishKeySelection(Editor &editor);

// ---------------------------------------------------------------------------
// Hover help.  The hint line is a control of its own above the status line:
// showing or clearing it never touches the persistent status or error text, so
// a failed write stays readable while the pointer explains a control.
// ---------------------------------------------------------------------------
// One hint per control that explains itself; -1 means "nothing to say".
int HintForControl(int id) {
    switch (id) {
    case kIdChannelList: return StrHintChannelList;
    case kIdCompareSameGroup:
    case kIdCompareAllGroups:
    case kIdCompareFree: return StrHintCompareMode;
    case kIdCompareFilter: return StrHintCompareFilter;
    case kIdAddKey: return StrHintAddKey;
    case kIdDeleteKey: return StrHintDeleteKey;
    case kIdApplyKey: return StrHintApplyKey;
    case kIdInterpolationCombo: return StrHintInterpolation;
    case kIdTangentCombo: return StrHintTangent;
    case kIdMmdX1: case kIdMmdY1: case kIdMmdX2: case kIdMmdY2: return StrHintMmdHandles;
    case kIdPreviousKey: case kIdPreviousFrame: case kIdNextFrame: case kIdNextKey: return StrHintNavigation;
    case kIdUndo:case kIdRedo:return StrHintHistory;
    default: return -1;
    }
}

// Stable numbers for the hint hook; 0 means no hint is shown.
int HoverHintCode(int hint) {
    switch (hint) {
    case StrHintChannelList: return 1;
    case StrHintCompareMode: return 2;
    case StrHintCompareFilter: return 3;
    case StrHintAddKey: return 4;
    case StrHintDeleteKey: return 5;
    case StrHintApplyKey: return 6;
    case StrHintCanvas: return 7;
    case StrHintMmdHandles: return 8;
    case StrHintTimeBar: return 9;
    case StrHintValueBar: return 10;
    case StrHintInterpolation: return 11;
    case StrHintTangent: return 12;
    default: return 0;
    }
}

void ApplyHoverHintText(Editor &editor) {
    const std::wstring text = editor.hoverHint >= 0 ? T(editor.session.language, static_cast<Str>(editor.hoverHint))
                                                    : std::wstring();
    SetControlText(editor.window, kIdHoverHint, text);
}

void SetHoverHint(Editor &editor, int hint) {
    if(hint==StrHintCanvas)hint=-1;
    if (editor.hoverHint == hint) return;
    editor.hoverHint = hint;
    ApplyHoverHintText(editor);
}

// Arms the WM_MOUSELEAVE notification, which is the only reliable "the pointer
// left this control" signal for a child that has no capture.
void TrackMouseLeave(HWND window) {
    TRACKMOUSEEVENT track = {};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = window;
    TrackMouseEvent(&track);
}

// ---------------------------------------------------------------------------
// Comparison display.  The source declares whether it takes part at all; the
// comparison curves themselves are always derived from the channel data, so a
// mode switch never writes anything back to the source.
// ---------------------------------------------------------------------------
bool ComparisonSupported(Editor &editor) {
    if (editor.sourceIndex >= editor.session.sources.size()) return false;
    const Source &source = editor.session.sources[editor.sourceIndex];
    return source.comparisonEnabled || source.readComparisonMode != nullptr ||
           source.writeComparisonMode != nullptr;
}

// Channels drawn behind the main curve, in channel order.  The display flag is
// the whole truth here: an automatic mode writes its result into those flags the
// moment it is picked (MaterializeCompareMask), so the list ticks, the canvas
// and this list always describe the same set of curves.  A curve that is not
// ticked is never drawn, and a curve without keys has nothing to draw.  Only a
// source that declares comparison support honours kCompareOff; a plain source
// (camera, bone, light ...) always shows its visible registered curves.
std::vector<std::size_t> ComparedChannels(Editor &editor) {
    std::vector<std::size_t> compared;
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) return compared;
    if (ComparisonSupported(editor) && state.compareMode == kCompareOff) return compared;
    const std::size_t main = std::min(state.active, state.channels.size() - 1);
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        if (index == main) continue;
        const Channel &channel = state.channels[index];
        if (!curve_comparison::IsRegistered(channel.track)) continue;
        if (!channel.selected) continue;
        compared.push_back(index);
    }
    return compared;
}

// ---------------------------------------------------------------------------
// Canvas transforms.
// ---------------------------------------------------------------------------
double PlotWidth(const Editor &editor) {
    return static_cast<double>(editor.plot.right - editor.plot.left);
}

double PlotHeight(const Editor &editor) {
    return static_cast<double>(editor.plot.bottom - editor.plot.top);
}

bool TimeToX(const Editor &editor, double frame, double &x) {
    const double span = editor.timeEnd - editor.timeStart;
    if (!(span > 0.0) || PlotWidth(editor) <= 0) return false;
    x = editor.plot.left + (frame - editor.timeStart) / span * PlotWidth(editor);
    return true;
}

bool XToTime(const Editor &editor, double x, double &frame) {
    const double span = editor.timeEnd - editor.timeStart;
    if (!(span > 0.0) || PlotWidth(editor) <= 0) return false;
    frame = editor.timeStart + (x - editor.plot.left) / PlotWidth(editor) * span;
    return true;
}

void ValueRange(const Editor &editor, double &low, double &high) {
    if (editor.viewMode == 0) {
        low = editor.valueLow;
        high = editor.valueHigh;
    } else {
        low = editor.speedLow;
        high = editor.speedHigh;
    }
}

bool ValueToY(const Editor &editor, double value, double &y) {
    double low = 0, high = 0;
    ValueRange(editor, low, high);
    if (!(high > low) || PlotHeight(editor) <= 0) return false;
    y = editor.plot.bottom - (value - low) / (high - low) * PlotHeight(editor);
    return true;
}

bool YToValue(const Editor &editor, double y, double &value) {
    double low = 0, high = 0;
    ValueRange(editor, low, high);
    if (!(high > low) || PlotHeight(editor) <= 0) return false;
    value = low + (editor.plot.bottom - y) / PlotHeight(editor) * (high - low);
    return true;
}

// Key position including the live drag preview.
POINT KeyPoint(const Editor &editor, std::size_t index) {
    POINT point = {-1, -1};
    if (editor.sourceIndex >= editor.states.size()) return point;
    SourceState &state = const_cast<Editor &>(editor).states[editor.sourceIndex];
    if (state.channels.empty()) return point;
    const Channel &active = state.channels[state.active];
    if (index >= active.track.keys.size()) return point;
    Key key = active.track.keys[index];
    if (editor.drag == Drag::Keys && editor.dragSelection.count(index)) {
        key.frame = static_cast<std::int32_t>(key.frame + editor.dragFrameDelta);
        key.value += editor.dragValueDelta;
    }
    double x = 0, y = 0;
    if (!TimeToX(editor, static_cast<double>(key.frame), x)) return point;
    if (editor.viewMode == 0) {
        if (!ValueToY(editor, key.value, y)) return point;
    } else {
        const Sample sample = active.sample ? active.sample(active, static_cast<double>(key.frame))
                                            : animation_curve::Evaluate(active.track, static_cast<double>(key.frame));
        if (!ValueToY(editor, sample.derivative * kFramesPerSecond, y)) return point;
    }
    point.x = static_cast<LONG>(std::lround(x));
    point.y = static_cast<LONG>(std::lround(y));
    return point;
}

int KeyAtPoint(Editor &editor, POINT point, double radius) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) return -1;
    const Channel &active = state.channels[state.active];
    int best = -1;
    double bestDistance = radius;
    for (std::size_t index = 0; index < active.track.keys.size(); ++index) {
        const POINT centre = KeyPoint(editor, index);
        if (centre.x < 0) continue;
        const double dx = static_cast<double>(point.x - centre.x);
        const double dy = static_cast<double>(point.y - centre.y);
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(index);
        }
    }
    return best;
}

// Screen point of a key's Bezier handle on one side (value view only).  False
// when the side has no curved interval: a straight side shows no handle.
bool HandlePoint(const Editor &editor, const Track &track, std::size_t key, bool outSide, POINT &point) {
    if (editor.viewMode != 0 || key >= track.keys.size() || track.discrete) return false;
    const std::size_t end = outSide ? key + 1 : key;
    if (end == 0 || end >= track.keys.size()) return false;
    if (track.keys[end].interpolation != Interpolation::Bezier) return false;
    if (animation_curve::SideIsStraight(track, key, outSide)) return false;
    const Key &k = track.keys[key];
    const double span = static_cast<double>(track.keys[end].frame) - track.keys[end - 1].frame;
    if (!(span > 0.0)) return false;
    const double weight = outSide ? k.outWeight : k.inWeight;
    const double slope = outSide ? k.outSlope : k.inSlope;
    const double dt = weight * span * (outSide ? 1.0 : -1.0);
    double x = 0, y = 0;
    if (!TimeToX(editor, k.frame + dt, x) || !ValueToY(editor, k.value + slope * dt, y)) return false;
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 1e6 || std::fabs(y) > 1e6) return false;
    point.x = static_cast<LONG>(std::lround(x));
    point.y = static_cast<LONG>(std::lround(y));
    return true;
}

// Handle of a selected key under the pointer: returns the key, or -1.
int HandleAtPoint(Editor &editor, POINT point, double radius, bool &outSide) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty() || editor.viewMode != 0) return -1;
    const Track &track = state.channels[state.active].track;
    int best = -1;
    double bestDistance = radius;
    for (const std::size_t index : state.selection) {
        for (bool side : {false, true}) {
            POINT handle{};
            if (!HandlePoint(editor, track, index, side, handle)) continue;
            const double distance = std::hypot(double(point.x - handle.x), double(point.y - handle.y));
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = static_cast<int>(index);
                outSide = side;
            }
        }
    }
    return best;
}
// ---------------------------------------------------------------------------
// Editing transactions.
// ---------------------------------------------------------------------------
void ReadRegisteredSource(Editor &editor);

bool CheckSourceCurrent(Editor &editor) {
    Source &source = CurrentSource(editor);
    std::vector<Channel> latest;
    std::wstring error;
    if (!source.read || !source.read(latest, error)) {
        SetStatus(editor, std::wstring(T(editor.session.language, StrReadFailed)) + error, true);
        return false;
    }
    editor.pendingSources.erase(source.id);
    if (SameCurveData(CurrentState(editor).channels, latest)) return true;
    // Native keyboard/mouse edits may bypass the menu notification. Compare
    // with the host before every write, including undo and redo.
    ReadRegisteredSource(editor);
    SetStatus(editor, T(editor.session.language, StrSourceChanged), true);
    return false;
}

void AdoptWrittenChannels(Editor &editor, std::vector<Channel> channels) {
    SourceState &state = CurrentState(editor);
    std::vector<Channel> canonical;
    std::wstring error;
    if (CurrentSource(editor).read(canonical, error)) {
        std::map<std::string, bool> display;
        for (const auto &channel : channels) display[channel.id] = channel.selected;
        for (auto &channel : canonical) {
            const auto found = display.find(channel.id);
            if (found != display.end()) channel.selected = found->second;
        }
        channels = std::move(canonical);
        state.valid = true;
        state.reason.clear();
    } else {
        state.valid = false;
        state.reason = std::wstring(T(editor.session.language, StrReadFailed)) + error;
    }
    // Native controls can be quantized on write. Keep the accepted data as
    // the next comparison baseline, so our own write is not an external edit.
    state.channels = std::move(channels);
}

bool CommitChannels(Editor &editor, std::vector<Channel> next, const std::wstring &success,
                    bool linkMmdHandles = false) {
    if (!CheckSourceCurrent(editor)) return false;
    SourceState &state = CurrentState(editor);
    Source &source = CurrentSource(editor);
    // Value-curve tangents are governed by Free/Aligned, not the legacy
    // 0..127 panel's automatic adjacent-control setting.
    if (linkMmdHandles && source.linkAdjacentControls && source.linkAdjacentControls()) {
        // Detect explicit edits before linking: derived neighbour changes must
        // not cascade along the entire track or overwrite another edited handle.
        const auto explicitChannels = next;
        for (std::size_t c=0;c<next.size()&&c<state.channels.size();++c) {
            const auto& old=state.channels[c].track;const auto& edited=explicitChannels[c].track;
            if(old.discrete||old.keys.size()!=edited.keys.size())continue;
            bool sameKeys=true;
            for(std::size_t k=0;k<old.keys.size();++k)
                if(old.keys[k].frame!=edited.keys[k].frame||old.keys[k].value!=edited.keys[k].value)sameKeys=false;
            if(!sameKeys)continue;
            for(std::size_t end=1;end<edited.keys.size();++end) {
                std::array<double,4> before{},after{};
                if(!ReadChannelControls(state.channels[c],end,before)||!ReadChannelControls(explicitChannels[c],end,after))continue;
                for(bool start : {true,false}) {
                    const int pair=start?0:2;
                    if(before[pair]==after[pair]&&before[pair+1]==after[pair+1])continue;
                    if((start&&end==1)||(!start&&end+1>=edited.keys.size()))continue;
                    const auto adjacent=start?end-1:end+1;
                    std::array<double,4> neighbor{},oldNeighbor{},explicitNeighbor{};
                    if(!ReadChannelControls(next[c],adjacent,neighbor)||
                       !ReadChannelControls(state.channels[c],adjacent,oldNeighbor)||
                       !ReadChannelControls(explicitChannels[c],adjacent,explicitNeighbor))continue;
                    const int target=start?2:0;
                    if(oldNeighbor[target]!=explicitNeighbor[target]||oldNeighbor[target+1]!=explicitNeighbor[target+1])continue;
                    if(!interpolation_link::Mirror(neighbor,after,start))continue;
                    std::string detail;
                    if(!SetChannelControls(next[c],adjacent,neighbor,detail)) {
                        SetStatus(editor,Widen(detail),true);return false;
                    }
                }
            }
        }
    }
    // Rotation and scale axes share one native easing lane. Apply a handle
    // change to its siblings before the single host write.
    for (std::size_t c = 0; c < next.size() && c < state.channels.size(); ++c) {
        if (next[c].interpolationGroup.empty()) continue;
        for (std::size_t end = 1; end < next[c].track.keys.size(); ++end) {
            std::array<double,4> before{}, after{};
            if (!ReadChannelControls(state.channels[c], end, before) ||
                !ReadChannelControls(next[c], end, after) || before == after) continue;
            for (Channel &sibling : next) {
                if (sibling.interpolationGroup != next[c].interpolationGroup) continue;
                std::string detail;
                if (!SetChannelControls(sibling, end, after, detail)) {
                    SetStatus(editor, Widen(detail), true);
                    return false;
                }
            }
        }
    }
    if (!state.valid) {
        SetStatus(editor, state.reason.empty() ? T(editor.session.language, StrNoRead) : state.reason, true);
        return false;
    }
    for (const Channel &channel : next) {
        std::string error;
        if (!animation_curve::Validate(channel.track, error)) {
            SetStatus(editor, std::wstring(T(editor.session.language, StrInvalidTrack)) + Widen(error), true);
            return false;
        }
    }
    if (!source.apply) {
        SetStatus(editor, T(editor.session.language, StrNoApply), true);
        return false;
    }
    std::wstring error;
    if (!source.apply(next, error)) {
        std::wstring message = T(editor.session.language, StrApplyRejected);
        if (!error.empty()) message += error;
        else if (state.reason.empty()) message += T(editor.session.language, StrReady);
        SetStatus(editor, message, true);
        return false;
    }
    state.undo.push_back(Snapshot{state.channels, state.active, state.selection});
    if (state.undo.size() > static_cast<std::size_t>(kUndoDepth)) state.undo.erase(state.undo.begin());
    state.redo.clear();
    AdoptWrittenChannels(editor, std::move(next));
    ClampState(state);
    PublishKeySelection(editor);
    RefreshEditorsUi(editor);
    SetStatus(editor, state.valid ? success : state.reason, !state.valid);
    return true;
}

std::vector<Channel> CopyChannels(Editor &editor) { return CurrentState(editor).channels; }

bool ApplySnapshot(Editor &editor, Snapshot snapshot, Snapshot &previous) {
    if (!CheckSourceCurrent(editor)) return false;
    SourceState &state = CurrentState(editor);
    Source &source = CurrentSource(editor);
    if (!source.apply) {
        SetStatus(editor, T(editor.session.language, StrNoApply), true);
        return false;
    }
    previous = Snapshot{state.channels, state.active, state.selection};
    std::wstring error;
    if (!source.apply(snapshot.channels, error)) {
        std::wstring message = T(editor.session.language, StrApplyRejected);
        if (!error.empty()) message += error;
        SetStatus(editor, message, true);
        return false;
    }
    AdoptWrittenChannels(editor, std::move(snapshot.channels));
    state.active = snapshot.active;
    state.selection = snapshot.selection;
    ClampState(state);
    PublishKeySelection(editor);
    return true;
}

void UndoOnce(Editor &editor) {
    if (!CheckSourceCurrent(editor)) return;
    SourceState &state = CurrentState(editor);
    if (state.undo.empty()) {
        SetStatus(editor, T(editor.session.language, StrNothingUndo), true);
        return;
    }
    Snapshot snapshot = std::move(state.undo.back());
    state.undo.pop_back();
    Snapshot previous;
    if (!ApplySnapshot(editor, std::move(snapshot), previous)) return;
    state.redo.push_back(std::move(previous));
    RefreshEditorsUi(editor);
    SetStatus(editor, T(editor.session.language, StrUndone), false);
}

void RedoOnce(Editor &editor) {
    if (!CheckSourceCurrent(editor)) return;
    SourceState &state = CurrentState(editor);
    if (state.redo.empty()) {
        SetStatus(editor, T(editor.session.language, StrNothingRedo), true);
        return;
    }
    Snapshot snapshot = std::move(state.redo.back());
    state.redo.pop_back();
    Snapshot previous;
    if (!ApplySnapshot(editor, std::move(snapshot), previous)) return;
    state.undo.push_back(std::move(previous));
    RefreshEditorsUi(editor);
    SetStatus(editor, T(editor.session.language, StrRedone), false);
}

// Reads the current source.  `adopted` reports whether the freshly read data
// differs from the local copy, which is how an external edit is detected.
bool ReadCurrentSource(Editor &editor, bool *adopted) {
    Source &source = CurrentSource(editor);
    SourceState &state = CurrentState(editor);
    if (!source.read) {
        state.valid = false;
        state.reason = T(editor.session.language, StrNoRead);
        if (adopted) *adopted = false;
        return false;
    }
    std::vector<Channel> channels;
    std::wstring error;
    if (!source.read(channels, error)) {
        state.valid = false;
        state.reason = std::wstring(T(editor.session.language, StrReadFailed)) + (error.empty() ? std::wstring() : error);
        if (adopted) *adopted = false;
        return false;
    }
    const bool changed = !state.loaded || !SameCurveData(state.channels, channels);
    state.channels = std::move(channels);
    state.loaded = true;
    state.valid = true;
    state.reason.clear();
    ClampState(state);
    // The host hands its own display flags over with the channels.  A read is
    // the one place they enter the editor, so this is where an automatic
    // comparison mode re-establishes its mask: otherwise a re-read would leave
    // the list claiming curves the canvas does not draw.
    MaterializeCompareMask(editor);
    if (adopted) *adopted = changed;
    return true;
}

void ReadAndAdopt(Editor &editor, bool dropHistoryOnChange) {
    SourceState &state = CurrentState(editor);
    bool changed = false;
    const bool ok = ReadCurrentSource(editor, &changed);
    if (!ok) {
        RefreshEditorsUi(editor);
        InvalidateCanvas(editor);
        // An invalid source keeps whatever it last had, disables editing and
        // states the reason in the status line.
        SetStatus(editor, state.reason, true);
        return;
    }
    if (changed && dropHistoryOnChange) {
        state.undo.clear();
        state.redo.clear();
        state.selection.clear();
    }
    // A read that only renamed channels keeps the local selection, the undo
    // stacks and every key value; the new labels are simply adopted.
    RefreshEditorsUi(editor);
    UpdateWindowTitle(editor);
    FillSourceCombo(editor);
    FitAll(editor);
    if (state.status.empty()) SetStatus(editor, T(editor.session.language, StrReady), false);
}

// External registration changes data, while the editor's view remains local.
// Match selection by frame because insertion can change every following index.
void ReadRegisteredSource(Editor &editor) {
    SourceState &state = CurrentState(editor);
    std::map<std::string, bool> display;
    std::string activeId;
    std::set<std::int32_t> selectedFrames;
    for (const Channel &channel : state.channels) display[channel.id] = channel.selected;
    if (state.active < state.channels.size()) {
        const Channel &active = state.channels[state.active];
        activeId = active.id;
        for (std::size_t index : state.selection)
            if (index < active.track.keys.size()) selectedFrames.insert(active.track.keys[index].frame);
    }
    bool changed = false;
    if (!ReadCurrentSource(editor, &changed)) {
        RefreshEditorsUi(editor);
        InvalidateCanvas(editor);
        SetStatus(editor, state.reason, true);
        return;
    }
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        Channel &channel = state.channels[index];
        const auto previous = display.find(channel.id);
        if (previous != display.end()) channel.selected = previous->second;
        if (channel.id == activeId) state.active = index;
    }
    if (changed) {
        state.undo.clear();
        state.redo.clear();
    }
    state.selection.clear();
    if (state.active < state.channels.size()) {
        const auto &keys = state.channels[state.active].track.keys;
        for (std::size_t index = 0; index < keys.size(); ++index)
            if (selectedFrames.count(keys[index].frame)) state.selection.insert(index);
    }
    PullKeySelection(editor);
    RefreshEditorsUi(editor);
    UpdateWindowTitle(editor);
    FillSourceCombo(editor);
    InvalidateCanvas(editor);
    if (changed || state.statusError) SetStatus(editor, T(editor.session.language, StrReady), false);
}

void ProcessSourceChanges(Editor &editor) {
    editor.sourceChangePosted = false;
    if (editor.drag != Drag::None || editor.mmdDragHandle >= 0) {
        // Only poll while a drag delays an actual pending update.
        SetTimer(editor.window, kSourceChangedTimer, kPlayheadIntervalMs, nullptr);
        return;
    }
    KillTimer(editor.window, kSourceChangedTimer);
    std::set<std::string> pending;
    pending.swap(editor.pendingSources);
    for (std::size_t index = 0; index < editor.session.sources.size(); ++index) {
        if (!pending.count(editor.session.sources[index].id)) continue;
        if (index == editor.sourceIndex) ReadRegisteredSource(editor);
        // SwitchSource reads inactive sources on entry and only drops history
        // when their native data actually changed.
    }
}

// ---------------------------------------------------------------------------
// Geometry, fitting and layout.
// ---------------------------------------------------------------------------
const Channel *ChannelForTrack(Editor &editor, const Track &track) {
    for (const Channel &channel : CurrentState(editor).channels)
        if (&channel.track == &track) return &channel;
    return nullptr;
}
Sample SampleTrack(Editor &editor, const Track &track, double frame) {
    const Channel *channel = ChannelForTrack(editor, track);
    return channel && channel->sample ? channel->sample(*channel, frame) : animation_curve::Evaluate(track, frame);
}
struct TrackExtent {
    bool valid = false;
    double first = 0, last = 100;
    double low = -1, high = 1;
    double speedLow = -1, speedHigh = 1;
};

TrackExtent ExtentOf(const Track &track, const Channel *channel = nullptr) {
    TrackExtent extent;
    if (track.keys.empty()) return extent;
    extent.valid = true;
    extent.first = std::max(0.0, double(track.keys.front().frame));
    extent.last = std::max(extent.first, double(track.keys.back().frame));
    if (extent.last - extent.first < 1.0) {
        extent.first = std::max(0.0, extent.first - 1.0);
        extent.last += 1.0;
    }
    double low = track.keys.front().value, high = track.keys.front().value;
    double speedLow = 0, speedHigh = 0;
    bool first = true;
    for (const Key &key : track.keys) {
        low = std::min(low, key.value);
        high = std::max(high, key.value);
        const Sample sample = channel && channel->sample ? channel->sample(*channel, static_cast<double>(key.frame))
                                                        : animation_curve::Evaluate(track, static_cast<double>(key.frame));
        const double speed = sample.derivative * kFramesPerSecond;
        if (first) {
            speedLow = speedHigh = speed;
            first = false;
        } else {
            speedLow = std::min(speedLow, speed);
            speedHigh = std::max(speedHigh, speed);
        }
    }
    // Sample interior points so an overshooting automatic curve still fits.
    for(std::size_t end=1;end<track.keys.size();++end)for(int step=1;step<16;++step){
        // Whole-track sampling misses short intervals between distant keys,
        // especially their speed peaks. Fit each actual interval instead.
        const double frame=track.keys[end-1].frame+double(track.keys[end].frame-track.keys[end-1].frame)*step/16;
        const Sample sample = channel && channel->sample ? channel->sample(*channel, frame)
                                                        : animation_curve::Evaluate(track, frame);
        low = std::min(low, sample.value);
        high = std::max(high, sample.value);
        const double speed = sample.derivative * kFramesPerSecond;
        speedLow = std::min(speedLow, speed);
        speedHigh = std::max(speedHigh, speed);
    }
    const double padding = std::max(1.0e-4, (high - low) * 0.12);
    extent.low = low - padding;
    extent.high = high + padding;
    const double speedPadding = std::max(1.0e-3, (speedHigh - speedLow) * 0.12);
    extent.speedLow = speedLow - speedPadding;
    extent.speedHigh = speedHigh + speedPadding;
    return extent;
}

TrackExtent VisibleExtent(Editor &editor) {
    TrackExtent extent;
    const auto merge = [&](const TrackExtent &other) {
        if (!other.valid) return;
        if (!extent.valid) { extent = other; return; }
        extent.first = std::min(extent.first, other.first);
        extent.last = std::max(extent.last, other.last);
        extent.low = std::min(extent.low, other.low);
        extent.high = std::max(extent.high, other.high);
        extent.speedLow = std::min(extent.speedLow, other.speedLow);
        extent.speedHigh = std::max(extent.speedHigh, other.speedHigh);
    };
    const Track *active = ActiveTrack(editor);
    if (active && ActiveChannelVisible(editor)) merge(ExtentOf(*active, ChannelForTrack(editor, *active)));
    const SourceState &state = CurrentState(editor);
    for (const std::size_t index : ComparedChannels(editor))
        merge(ExtentOf(state.channels[index].track, &state.channels[index]));
    if (!extent.valid && active) merge(ExtentOf(*active, ChannelForTrack(editor, *active)));
    return extent;
}

void FitAll(Editor &editor) {
    const TrackExtent extent = VisibleExtent(editor);
    if (!extent.valid) {
        editor.timeStart = 0;
        editor.timeEnd = 100;
        editor.valueLow = -1;
        editor.valueHigh = 1;
        editor.speedLow = -1;
        editor.speedHigh = 1;
    } else {
        editor.timeStart = extent.first;
        editor.timeEnd = extent.last;
        editor.valueLow = extent.low;
        editor.valueHigh = extent.high;
        editor.speedLow = extent.speedLow;
        editor.speedHigh = extent.speedHigh;
    }
    InvalidateCanvas(editor);
}

void FitRange(Editor &editor) {
    SourceState &state = CurrentState(editor);
    Track *track = ActiveTrack(editor);
    if (!track || track->keys.empty() || state.selection.empty()) {
        FitAll(editor);
        return;
    }
    double low = 0, high = 0, speedLow = 0, speedHigh = 0;
    bool first = true;
    for (const std::size_t index : state.selection) {
        if (index >= track->keys.size()) continue;
        const Key &key = track->keys[index];
        const double speed = SampleTrack(editor, *track, static_cast<double>(key.frame)).derivative * kFramesPerSecond;
        if (first) {
            low = high = key.value;
            speedLow = speedHigh = speed;
            first = false;
        } else {
            low = std::min(low, key.value);
            high = std::max(high, key.value);
            speedLow = std::min(speedLow, speed);
            speedHigh = std::max(speedHigh, speed);
        }
    }
    if (first) {
        FitAll(editor);
        return;
    }
    const double pad = std::max(0.5, (high - low) * 0.2);
    editor.valueLow = low - pad;
    editor.valueHigh = high + pad;
    if (!(editor.valueHigh > editor.valueLow)) {
        editor.valueLow -= 0.5;
        editor.valueHigh += 0.5;
    }
    const double speedPad = std::max(1.0e-3, (speedHigh - speedLow) * 0.2);
    editor.speedLow = speedLow - speedPad;
    editor.speedHigh = speedHigh + speedPad;
    InvalidateCanvas(editor);
}

void CanvasMetrics(Editor &editor) {
    if (!editor.canvas) return;
    RECT client = {};
    GetClientRect(editor.canvas, &client);
    const int bar = Scale(kBarThickness, editor.dpi);
    editor.plot.left = client.left + Scale(46, editor.dpi);
    // Mouse coordinates share the existing status row; only the ruler remains here.
    editor.plot.top = client.top + Scale(24, editor.dpi);
    editor.plot.right = client.right - bar;
    editor.plot.bottom = client.bottom - bar;
    if (editor.plot.right < editor.plot.left + 20) editor.plot.right = editor.plot.left + 20;
    if (editor.plot.bottom < editor.plot.top + 20) editor.plot.bottom = editor.plot.top + 20;
    editor.timeBar = {editor.plot.left,editor.plot.bottom,editor.plot.right,client.bottom};
    editor.valueBar = {editor.plot.right,editor.plot.top,client.right,editor.plot.bottom};
}

bool MouseCoordinates(Editor& editor,double& frame,double& value) {
    return editor.mouseInPlot && PtInRect(&editor.plot,editor.mousePoint) &&
        XToTime(editor,editor.mousePoint.x,frame) && YToValue(editor,editor.mousePoint.y,value) &&
        std::isfinite(frame) && std::isfinite(value);
}

void RefreshMouseReadout(Editor& editor) {
    const bool zh=editor.session.language==2,en=editor.session.language==1;
    const wchar_t* mouse=zh?L"鼠标":en?L"Mouse":L"マウス";
    const wchar_t* frameLabel=zh?L"帧":en?L"Frame":L"フレーム";
    const wchar_t* valueLabel=T(editor.session.language,editor.viewMode==0?StrViewValue:StrViewSpeed);
    double frame=0,value=0;
    wchar_t text[256]{};
    if(MouseCoordinates(editor,frame,value))
        swprintf_s(text,L"%s    %s %.2f    %s %.3f",mouse,frameLabel,frame,valueLabel,value);
    else
        swprintf_s(text,L"%s    %s --    %s --",mouse,frameLabel,valueLabel);
    if(HWND control=GetDlgItem(editor.window,kIdMouseReadout)) {
        wchar_t previous[256]{};GetWindowTextW(control,previous,256);
        if(wcscmp(previous,text)!=0)SetWindowTextW(control,text);
    }
}

void UpdateMouseReadout(Editor& editor,POINT point,bool inside=true) {
    editor.mousePoint=point;
    editor.mouseInPlot=inside && PtInRect(&editor.plot,point);
    RefreshMouseReadout(editor);
}

// ---------------------------------------------------------------------------
// Canvas painting.
// ---------------------------------------------------------------------------
// Builds the screen polyline of one track.  The main curve and every
// background comparison curve go through this one function, so they can never
// drift apart in how they are sampled.
bool CurvePoints(Editor &editor, const Track &track, std::vector<POINT> &points) {
    points.clear();
    if (track.keys.empty()) return false;
    if (editor.viewMode == 1 && track.discrete) return false;
    const int width = static_cast<int>(PlotWidth(editor));
    if (width <= 1) return false;
    points.reserve(static_cast<std::size_t>(width) + 1);
    for (int offset = 0; offset <= width; ++offset) {
        const double x = editor.plot.left + offset;
        double frame = 0;
        if (!XToTime(editor, x, frame)) continue;
        const Sample sample = SampleTrack(editor, track, frame);
        const double value = editor.viewMode == 0 ? sample.value : sample.derivative * kFramesPerSecond;
        double y = 0;
        if (!ValueToY(editor, value, y)) continue;
        if (!std::isfinite(y)) continue;
        const double clamped = std::max(-20000.0, std::min(20000.0, y));
        points.push_back(POINT{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(clamped))});
    }
    return points.size() >= 2;
}

COLORREF ChannelColour(const Channel&channel){
    const auto separator=channel.id.find_last_of('/');
    const std::string axis=separator==std::string::npos?channel.id:channel.id.substr(separator+1);
    if(axis=="x"||axis=="rx"||axis=="sx")return curve_editor_theme::Curve(RGB(222,76,104));
    if(axis=="y"||axis=="ry"||axis=="sy")return curve_editor_theme::Curve(RGB(116,161,43));
    if(axis=="z"||axis=="rz"||axis=="sz")return curve_editor_theme::Curve(RGB(55,139,222));
    if(axis=="distance")return curve_editor_theme::Curve(RGB(157,113,226));
    if(axis=="fov")return curve_editor_theme::Curve(RGB(219,155,45));
    unsigned hash=2166136261u;
    for(unsigned char c:channel.id){hash^=c;hash*=16777619u;}
    // Scatter related channel IDs across the full hue circle while keeping
    // each channel's colour stable across refreshes and editor openings.
    hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;hash*=0x846ca68bu;hash^=hash>>16;
    const double hue=double(hash&0xffffu)/65536.0*6.0;
    const double saturation=0.56+double((hash>>16)&255u)/255.0*0.18;
    const double value=0.76+double((hash>>24)&255u)/255.0*0.12;
    const double chroma=value*saturation;
    const double x=chroma*(1.0-std::fabs(std::fmod(hue,2.0)-1.0)),m=value-chroma;
    double r=0,g=0,b=0;
    switch(int(hue)){
    case 0:r=chroma;g=x;break;
    case 1:r=x;g=chroma;break;
    case 2:g=chroma;b=x;break;
    case 3:g=x;b=chroma;break;
    case 4:r=x;b=chroma;break;
    default:r=chroma;b=x;break;
    }
    return RGB(std::lround((r+m)*255.0),std::lround((g+m)*255.0),std::lround((b+m)*255.0));
}

void DrawCurve(Editor &editor, HDC dc) {
    if (!ActiveChannelVisible(editor)) return;
    Track *track = ActiveTrack(editor);
    if (!track) return;
    std::vector<POINT> points;
    if (!CurvePoints(editor, *track, points)) return;
    const auto&state=CurrentState(editor);
    const COLORREF colour=ChannelColour(state.channels[state.active]);
    GdiFlush();
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetClip(Gdiplus::Rect(editor.plot.left,editor.plot.top,
                                  editor.plot.right-editor.plot.left,editor.plot.bottom-editor.plot.top));
    std::vector<Gdiplus::Point> line;line.reserve(points.size());
    for(const auto& point:points)line.emplace_back(point.x,point.y);
    Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(colour),GetGValue(colour),GetBValue(colour)),1.6f);
    graphics.DrawLines(&pen,line.data(),int(line.size()));
}

// The comparison curves are background: they are drawn before the main curve,
// carry no keys and take no part in any mouse hit test, so they can only ever
// be read. They keep their channel colour at a fraction of its opacity, so
// the grid already painted into the DC keeps showing through and the fully
// opaque main curve stays in front.  The translucency is real GDI+ alpha
// blending (SourceOver) over the inherited DC content, clipped to the plot
// rect so nothing leaks into the bars.
void DrawComparisonCurves(Editor &editor, HDC dc) {
    const std::vector<std::size_t> compared = ComparedChannels(editor);
    if (compared.empty()) return;
    SourceState &state = CurrentState(editor);
    GdiPlusToken();  // idempotent: make sure GDI+ is up before the first stroke
    GdiFlush();      // commit earlier GDI drawing (grid) so it shows through
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetClip(Gdiplus::Rect(editor.plot.left, editor.plot.top,
                                   editor.plot.right - editor.plot.left,
                                   editor.plot.bottom - editor.plot.top));
    std::vector<POINT> points;
    std::vector<Gdiplus::Point> line;
    for (const std::size_t index : compared) {
        if (index >= state.channels.size()) continue;
        if (!CurvePoints(editor, state.channels[index].track, points)) continue;
        const COLORREF colour=ChannelColour(state.channels[index]);
        Gdiplus::Pen pen(Gdiplus::Color(172,GetRValue(colour),GetGValue(colour),GetBValue(colour)),1.2f);
        line.clear();
        line.reserve(points.size());
        for (const POINT &point : points)
            line.push_back(Gdiplus::Point(static_cast<INT>(point.x), static_cast<INT>(point.y)));
        graphics.DrawLines(&pen, line.data(), static_cast<INT>(line.size()));
    }
}

void DrawGrid(Editor &editor, HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, curve_editor_theme::muted);
    HPEN grid = CreatePen(PS_SOLID, 1, curve_editor_theme::grid);
    HPEN zero = CreatePen(PS_SOLID, 1, curve_editor_theme::zero);
    HGDIOBJ oldPen = SelectObject(dc, grid);

    // Grid lines are counted by integer index, never by `x += step`: far from the
    // origin a step below the value's precision leaves the sum unchanged, and the
    // loop - and with it the paint and MMS - never finished.
    constexpr long long kMaxGridLines = 64;
    double low = 0, high = 0;
    ValueRange(editor, low, high);
    if (high > low && std::isfinite(low) && std::isfinite(high)) {
        const double step = NiceStep(high - low, 5);
        const double firstIndex = std::ceil(low / step);
        const double lastIndex = std::floor(high / step);
        if (std::isfinite(firstIndex) && lastIndex - firstIndex < kMaxGridLines &&
            firstIndex + 1.0 != firstIndex)
        for (double index = firstIndex; index <= lastIndex; index += 1.0) {
            const double value = index * step;
            double y = 0;
            if (!ValueToY(editor, value, y)) continue;
            const int row = static_cast<int>(std::lround(y));
            SelectObject(dc,value==0?zero:grid);
            MoveToEx(dc, editor.plot.left, row, nullptr);
            LineTo(dc, editor.plot.right, row);
            const std::wstring label = FormatNumber(value);
            RECT text = {Scale(2,editor.dpi),row-Scale(7,editor.dpi),editor.plot.left-Scale(7,editor.dpi),row+Scale(8,editor.dpi)};
            DrawTextW(dc, label.c_str(), -1, &text, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    if (editor.timeEnd > editor.timeStart && std::isfinite(editor.timeStart) &&
        std::isfinite(editor.timeEnd)) {
        const double step = std::max(1.0, NiceStep(editor.timeEnd - editor.timeStart, 10));
        const double firstIndex = std::ceil(editor.timeStart / step);
        const double lastIndex = std::floor(editor.timeEnd / step);
        if (std::isfinite(firstIndex) && lastIndex - firstIndex < kMaxGridLines &&
            firstIndex + 1.0 != firstIndex)
        for (double index = firstIndex; index <= lastIndex; index += 1.0) {
            const double frame = index * step;
            double x = 0;
            if (!TimeToX(editor, frame, x)) continue;
            const int column = static_cast<int>(std::lround(x));
            SelectObject(dc,frame==0?zero:grid);
            MoveToEx(dc, column, editor.plot.top, nullptr);
            LineTo(dc, column, editor.plot.bottom);
            const std::wstring label = FormatInteger(static_cast<long long>(std::llround(frame)));
            RECT text = {column-Scale(20,editor.dpi),editor.plot.top-Scale(19,editor.dpi),column+Scale(20,editor.dpi),editor.plot.top-Scale(2,editor.dpi)};
            DrawTextW(dc, label.c_str(), -1, &text, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(grid);
    DeleteObject(zero);
}

void DrawKeys(Editor &editor, HDC dc) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) return;
    if (!ActiveChannelVisible(editor)) return;
    const Track &track = state.channels[state.active].track;
    const float radius=float(Scale(kHandleSize,editor.dpi))/2.0f;
    const COLORREF colour=ChannelColour(state.channels[state.active]);
    GdiFlush();Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetClip(Gdiplus::Rect(editor.plot.left-Scale(4,editor.dpi),editor.plot.top-Scale(4,editor.dpi),
        editor.plot.right-editor.plot.left+Scale(8,editor.dpi),editor.plot.bottom-editor.plot.top+Scale(8,editor.dpi)));
    Gdiplus::Pen outline(Gdiplus::Color(255,GetRValue(colour),GetGValue(colour),GetBValue(colour)),1.4f);
    Gdiplus::Pen selectedOutline(Gdiplus::Color(255,204,145,19),1.2f);
    Gdiplus::SolidBrush paper(Gdiplus::Color(255,GetRValue(curve_editor_theme::paper),GetGValue(curve_editor_theme::paper),GetBValue(curve_editor_theme::paper)));
    Gdiplus::SolidBrush selectedFill(Gdiplus::Color(255,255,210,69));
    Gdiplus::Pen hoverOutline(Gdiplus::Color(255,80,101,133),1.0f);
    for (std::size_t index = 0; index < track.keys.size(); ++index) {
        const POINT centre = KeyPoint(editor, index);
        if (centre.x < 0) continue;
        const bool selected = state.selection.count(index) != 0;
        const Gdiplus::RectF shape(float(centre.x)-radius,float(centre.y)-radius,radius*2,radius*2);
        graphics.FillEllipse(selected?&selectedFill:&paper,shape);
        graphics.DrawEllipse(selected?&selectedOutline:&outline,shape);
    }
    if (editor.hoverKey >= 0 && static_cast<std::size_t>(editor.hoverKey) < track.keys.size() &&
        !state.selection.count(static_cast<std::size_t>(editor.hoverKey))) {
        const POINT centre = KeyPoint(editor, static_cast<std::size_t>(editor.hoverKey));
        if (centre.x >= 0) {
            graphics.DrawEllipse(&hoverOutline,float(centre.x)-radius-2,float(centre.y)-radius-2,radius*2+4,radius*2+4);
        }
    }
}

void DrawTangents(Editor &editor, HDC dc) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) return;
    if (!ActiveChannelVisible(editor)) return;
    const Track &track = state.channels[state.active].track;
    if (editor.viewMode != 0) return;
    // Each selected key shows the handle of every curved side: a line from the
    // key and a round grip.  Drag a grip to shape that side; Alt-click it to
    // straighten the side.
    HPEN pen = CreatePen(PS_SOLID, 1, curve_editor_theme::arm);
    HBRUSH fill = CreateSolidBrush(curve_editor_theme::paper);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    const int radius = std::max(3, Scale(kHandleSize, editor.dpi) / 2);
    for (const std::size_t index : state.selection) {
        if (index >= track.keys.size()) continue;
        const POINT key = KeyPoint(editor, index);
        if (key.x < 0) continue;
        for (bool outSide : {false, true}) {
            POINT handle{};
            if (!HandlePoint(editor, track, index, outSide, handle)) continue;
            MoveToEx(dc, key.x, key.y, nullptr);
            LineTo(dc, handle.x, handle.y);
            Ellipse(dc, handle.x - radius, handle.y - radius, handle.x + radius + 1, handle.y + radius + 1);
        }
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(pen);
}

void DrawPlayhead(Editor &editor, HDC dc) {
    if (!editor.session.currentFrame) return;
    const double frame = editor.session.currentFrame();
    if (!std::isfinite(frame)) return;
    double x = 0;
    if (!TimeToX(editor, frame, x)) return;
    if (x < editor.plot.left || x > editor.plot.right) return;
    HPEN pen = CreatePen(PS_SOLID, 1, curve_editor_theme::accent);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, static_cast<int>(std::lround(x)), editor.plot.top, nullptr);
    LineTo(dc, static_cast<int>(std::lround(x)), editor.plot.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    const std::wstring caption=FormatInteger(static_cast<long long>(std::llround(frame)));
    SIZE size{};GetTextExtentPoint32W(dc,caption.c_str(),int(caption.size()),&size);
    const int width=std::max(Scale(20,editor.dpi),int(size.cx)+Scale(8,editor.dpi));
    const int left=std::clamp(int(std::lround(x))-width/2,int(editor.plot.left),int(editor.plot.right)-width);
    RECT flag={left,editor.plot.top-Scale(20,editor.dpi),left+width,editor.plot.top-Scale(2,editor.dpi)};
    HBRUSH fill=CreateSolidBrush(curve_editor_theme::accent);FillRect(dc,&flag,fill);DeleteObject(fill);
    const COLORREF previous=SetTextColor(dc,curve_editor_theme::accentText);SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,caption.c_str(),-1,&flag,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SetTextColor(dc,previous);
}

struct BarRange {
    double fullLow = 0, fullHigh = 1;
};

BarRange TimeBarRange(const Editor &editor) {
    BarRange range;
    if (editor.drag == Drag::TimeBarPan || editor.drag == Drag::TimeBarZoomLow ||
        editor.drag == Drag::TimeBarZoomHigh)
        return {editor.savedBarLow, editor.savedBarHigh};
    const auto& centered=editor.centeredTime;
    if(centered.Matches(editor.timeStart,editor.timeEnd))
        return {centered.low,centered.high};
    double low = editor.timeStart, high = editor.timeEnd;
    const TrackExtent extent = VisibleExtent(const_cast<Editor &>(editor));
    if (extent.valid) {
        low = std::min(low, extent.first);
        high = std::max(high, extent.last);
    }
    const double padding = std::max(1.0, (high - low) * 0.05);
    range.fullLow = std::max(0.0, low - padding);
    range.fullHigh = high + padding;
    return range;
}

BarRange ValueBarRange(const Editor &editor) {
    BarRange range;
    if (editor.drag == Drag::ValueBarPan || editor.drag == Drag::ValueBarZoomLow ||
        editor.drag == Drag::ValueBarZoomHigh)
        return {editor.savedBarLow, editor.savedBarHigh};
    double low = 0, high = 0;
    ValueRange(editor, low, high);
    const auto& centered=editor.centeredValue;
    if(centered.mode==editor.viewMode && centered.Matches(low,high))
        return {centered.low,centered.high};
    const TrackExtent extent = VisibleExtent(const_cast<Editor &>(editor));
    if (extent.valid) {
        low = std::min(low, editor.viewMode == 0 ? extent.low : extent.speedLow);
        high = std::max(high, editor.viewMode == 0 ? extent.high : extent.speedHigh);
    }
    const double padding = std::max(1.0e-4, (high - low) * 0.5);
    range.fullLow = low - padding;
    range.fullHigh = high + padding;
    return range;
}

void BarPixels(const Editor &editor, bool time, int &first, int &last) {
    const bool dragging = time
        ? editor.drag == Drag::TimeBarPan || editor.drag == Drag::TimeBarZoomLow || editor.drag == Drag::TimeBarZoomHigh
        : editor.drag == Drag::ValueBarPan || editor.drag == Drag::ValueBarZoomLow || editor.drag == Drag::ValueBarZoomHigh;
    if (dragging) {
        first = editor.savedBarFirst;
        last = editor.savedBarLast;
        return;
    }
    const RECT &bar = time ? editor.timeBar : editor.valueBar;
    first = (time ? bar.left : bar.top) + Scale(4, editor.dpi);
    last = std::max(first + 1, static_cast<int>(time ? bar.right : bar.bottom) - Scale(4, editor.dpi));
}

double MapToBar(double value, double low, double high, int first, int last) {
    if (!(high > low)) return first;
    const double t = (value - low) / (high - low);
    return first + t * (last - first);
}

double MapFromBar(double position, double low, double high, int first, int last) {
    if (last == first) return low;
    const double t = (position - first) / static_cast<double>(last - first);
    return low + t * (high - low);
}

// Screen rows of the value bar window: `top` carries the upper view value and
// `bottom` the lower one, so the mapping descends as the value rises.
void ValueBarEdges(const Editor &editor, int &top, int &bottom) {
    RECT inner = editor.valueBar;
    InflateRect(&inner, -Scale(3, editor.dpi), -Scale(2, editor.dpi));
    const BarRange range = ValueBarRange(editor);
    double low = 0, high = 0;
    ValueRange(editor, low, high);
    int first = 0, last = 0;
    BarPixels(editor, false, first, last);
    top = static_cast<int>(std::lround(MapToBar(high, range.fullLow, range.fullHigh, last, first)));
    bottom = static_cast<int>(std::lround(MapToBar(low, range.fullLow, range.fullHigh, last, first)));
}

// The range bars are overview rulers: ticks cover the complete data/view
// range, rather than repeating the zoomed plot scale. Choose 1/2/5 intervals
// from the pixel length and the widest label so resizing never crowds labels.
void DrawBarTicks(Editor& editor,HDC dc,bool time){
    const RECT& bar=time?editor.timeBar:editor.valueBar;
    const BarRange range=time?TimeBarRange(editor):ValueBarRange(editor);
    const double span=range.fullHigh-range.fullLow;
    if(!(span>0)||!std::isfinite(span))return;
    int first=0,last=0;BarPixels(editor,time,first,last);
    const int length=last-first;
    if(length<=0)return;
    const int saved=SaveDC(dc);
    IntersectClipRect(dc,bar.left,bar.top,bar.right,bar.bottom);
    LOGFONTW font{};
    GetObjectW(editor.font,sizeof(font),&font);
    font.lfHeight=-Scale(10,editor.dpi);
    font.lfEscapement=font.lfOrientation=0;
    HFONT horizontal=CreateFontIndirectW(&font);
    if(horizontal)SelectObject(dc,horizontal);
    SIZE labelSize{};
    for(double value:{range.fullLow,range.fullHigh}){
        const std::wstring label=time?FormatInteger(static_cast<long long>(std::lround(value))):FormatNumber(value);
        SIZE size{};GetTextExtentPoint32W(dc,label.c_str(),int(label.size()),&size);
        labelSize.cx=std::max(labelSize.cx,size.cx);
    }
    const int spacing=std::max(Scale(48,editor.dpi),int(labelSize.cx)+Scale(16,editor.dpi));
    const int count=std::clamp(length/std::max(1,spacing),1,32);
    const double major=time?std::max(1.0,NiceStep(span,count)):NiceStep(span,count);
    if(!(major>0)||!std::isfinite(major)){RestoreDC(dc,saved);if(horizontal)DeleteObject(horizontal);return;}
    double minor=major/5.0;
    if(time&&minor<1.0)minor=major;
    const double begin=std::ceil(range.fullLow/minor),end=std::floor(range.fullHigh/minor);
    HPEN pen=CreatePen(PS_SOLID,1,curve_editor_theme::border);
    SelectObject(dc,pen);SetTextColor(dc,curve_editor_theme::muted);SetBkMode(dc,TRANSPARENT);
    HFONT vertical=nullptr;
    if(!time){font.lfEscapement=font.lfOrientation=900;vertical=CreateFontIndirectW(&font);if(vertical)SelectObject(dc,vertical);SetTextAlign(dc,TA_LEFT|TA_BASELINE);}
    // Bound every floating-point loop, including tiny or extreme value spans.
    for(int tick=0;tick<192;++tick){
        const double index=begin+tick;
        if(index>end||(tick>0&&index==begin))break;
        const double value=index*minor;
        const bool primary=std::abs(value/major-std::round(value/major))<1.0e-6;
        const int position=int(std::lround(MapToBar(value,range.fullLow,range.fullHigh,time?first:last,time?last:first)));
        const int size=Scale(primary?4:2,editor.dpi);
        if(time){
            const int y=bar.top+Scale(18,editor.dpi);
            MoveToEx(dc,position,y,nullptr);LineTo(dc,position,y+size);
        }else{
            const int x=bar.left+Scale(21,editor.dpi);
            MoveToEx(dc,x,position,nullptr);LineTo(dc,x+size,position);
        }
        if(!primary)continue;
        const std::wstring label=time?FormatInteger(static_cast<long long>(std::lround(value))):FormatNumber(value);
        SIZE sizeText{};GetTextExtentPoint32W(dc,label.c_str(),int(label.size()),&sizeText);
        if(time){
            RECT text={std::max(first,std::min(position-int(sizeText.cx)/2,last-int(sizeText.cx))),bar.top+Scale(22,editor.dpi),last,bar.bottom};
            DrawTextW(dc,label.c_str(),int(label.size()),&text,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
        }else{
            const int half=int(sizeText.cx)/2;
            if(position-half<first||position+half>last)continue;
            TextOutW(dc,bar.right-Scale(2,editor.dpi),position+half,label.c_str(),int(label.size()));
        }
    }
    RestoreDC(dc,saved);
    if(vertical)DeleteObject(vertical);
    if(horizontal)DeleteObject(horizontal);
    DeleteObject(pen);
}
void DrawTimeBar(Editor &editor, HDC dc) {
    const RECT &bar = editor.timeBar;
    RECT inner = bar;
    inner.bottom=inner.top+Scale(16,editor.dpi);
    HBRUSH back = CreateSolidBrush(curve_editor_theme::track);
    FillRect(dc, &inner, back);
    DeleteObject(back);
    FrameRect(dc, &inner, GetSysColorBrush(COLOR_3DSHADOW));

    const BarRange range = TimeBarRange(editor);
    int first = 0, last = 0;
    BarPixels(editor, true, first, last);
    const int left = static_cast<int>(std::lround(MapToBar(editor.timeStart, range.fullLow, range.fullHigh, first, last)));
    const int right = static_cast<int>(std::lround(MapToBar(editor.timeEnd, range.fullLow, range.fullHigh, first, last)));
    RECT window = {left, inner.top + 1, std::max(right, left + 2), inner.bottom - 1};
    HBRUSH fill = CreateSolidBrush(curve_editor_theme::selection);
    FillRect(dc, &window, fill);
    DeleteObject(fill);
    FrameRect(dc, &window, GetSysColorBrush(COLOR_3DSHADOW));
    const int grip = Scale(kGripSize, editor.dpi);
    RECT lowGrip = {window.left - grip / 2, window.top, window.left + grip / 2 + 1, window.bottom + 1};
    RECT highGrip = {window.right - grip / 2, window.top, window.right + grip / 2 + 1, window.bottom + 1};
    HBRUSH gripBrush = CreateSolidBrush(curve_editor_theme::grip);
    FillRect(dc, &lowGrip, gripBrush);
    FillRect(dc, &highGrip, gripBrush);
    DeleteObject(gripBrush);
    DrawBarTicks(editor,dc,true);
}

void DrawValueBar(Editor &editor, HDC dc) {
    const RECT &bar = editor.valueBar;
    RECT inner = bar;
    inner.right=inner.left+Scale(16,editor.dpi);
    HBRUSH back = CreateSolidBrush(curve_editor_theme::track);
    FillRect(dc, &inner, back);
    DeleteObject(back);
    FrameRect(dc, &inner, GetSysColorBrush(COLOR_3DSHADOW));

    const BarRange range = ValueBarRange(editor);
    (void)range;
    int top = 0, bottom = 0;
    ValueBarEdges(editor, top, bottom);
    RECT window = {inner.left + 1, top, inner.right - 1, std::max(bottom, top + 2)};
    HBRUSH fill = CreateSolidBrush(curve_editor_theme::selection);
    FillRect(dc, &window, fill);
    DeleteObject(fill);
    FrameRect(dc, &window, GetSysColorBrush(COLOR_3DSHADOW));
    const int grip = Scale(kGripSize, editor.dpi);
    RECT lowGrip = {window.left, window.bottom - grip / 2, window.right + 1, window.bottom + grip / 2 + 1};
    RECT highGrip = {window.left, window.top - grip / 2, window.right + 1, window.top + grip / 2 + 1};
    HBRUSH gripBrush = CreateSolidBrush(curve_editor_theme::grip);
    FillRect(dc, &lowGrip, gripBrush);
    FillRect(dc, &highGrip, gripBrush);
    DeleteObject(gripBrush);
    DrawBarTicks(editor,dc,false);
}

void PaintCanvas(Editor &editor, HDC dc) {
    RECT client = {};
    GetClientRect(editor.canvas, &client);
    // The complete axis surround follows the ruler theme, including corners.
    HBRUSH background = CreateSolidBrush(curve_editor_theme::ruler);
    FillRect(dc, &client, background);
    DeleteObject(background);
    CanvasMetrics(editor);

    curve_editor_theme::Fill(dc,editor.plot,curve_editor_theme::paper);
    HGDIOBJ oldFont = SelectObject(dc, editor.font ? editor.font : GetStockObject(DEFAULT_GUI_FONT));
    DrawGrid(editor, dc);
    DrawComparisonCurves(editor, dc);
    DrawTangents(editor, dc);
    DrawCurve(editor, dc);
    DrawPlayhead(editor, dc);
    DrawKeys(editor, dc);

    const Track *track = ActiveTrack(editor);
    std::wstring notice;
    if (track && !ActiveChannelVisible(editor)) notice = T(editor.session.language, StrHiddenCurve);
    else if (!track || track->keys.empty()) notice = T(editor.session.language, StrNoKeys);
    else if (editor.viewMode == 1 && track->discrete) notice = T(editor.session.language, StrDiscreteNoSpeed);
    if (!notice.empty()) {
        SetTextColor(dc, curve_editor_theme::muted);
        RECT text = editor.plot;
        text.left += Scale(6, editor.dpi);
        text.top += Scale(6, editor.dpi);
        DrawTextW(dc, notice.c_str(), -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    if (editor.drag == Drag::Rubber) {
        HPEN pen = CreatePen(PS_DOT, 1, RGB(64, 64, 64));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, editor.rubber.left, editor.rubber.top, editor.rubber.right, editor.rubber.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    DrawTimeBar(editor, dc);
    DrawValueBar(editor, dc);

    RECT frame = client;
    FrameRect(dc, &frame, GetSysColorBrush(COLOR_3DSHADOW));
    SelectObject(dc, oldFont);
}

// ---------------------------------------------------------------------------
// MMD 0..127 panel.
// ---------------------------------------------------------------------------
void ComputeMmdControls(Editor &editor) {
    editor.mmdEnabled = false;
    editor.mmdReason.clear();
    editor.mmdEndKey = -1;
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) {
        editor.mmdReason = T(editor.session.language, StrNeedChannel);
        return;
    }
    const Track &track = state.channels[state.active].track;
    if (state.selection.empty() && !editor.mmdFollowPlayhead) {
        editor.mmdReason = T(editor.session.language, StrNoSelection);
        return;
    }
    std::size_t endKey = ActiveKeyIndex(editor);
    if(editor.mmdFollowPlayhead && track.keys.size()>1 && editor.session.currentFrame){
        const double frame=editor.session.currentFrame();
        if(std::isfinite(frame)){
            auto at=std::lower_bound(track.keys.begin()+1,track.keys.end(),frame,[](const Key& key,double f){return key.frame<f;});
            endKey=at==track.keys.end()?track.keys.size()-1:std::size_t(at-track.keys.begin());
        }
    }
    if (endKey == 0 || endKey >= track.keys.size()) {
        editor.mmdReason = T(editor.session.language, StrMmdNoInterval);
        return;
    }
    const Key &start = track.keys[endKey - 1];
    const Key &end = track.keys[endKey];
    editor.mmdEndKey = static_cast<int>(endKey);
    if (track.discrete) {
        editor.mmdReason = T(editor.session.language, StrMmdDiscrete);
        return;
    }
    const Channel &channel = state.channels[state.active];
    if (!channel.nativeControls.empty()) {
        std::array<double,4> native{};
        if (ReadChannelControls(channel, endKey, native)) {
            if(!std::all_of(native.begin(),native.end(),[](double v){return std::isfinite(v)&&v>=0&&v<=127;})){
                editor.mmdReason=T(editor.session.language,StrMmdOvershoot);return;
            }
            editor.mmdControls = native;
            editor.mmdEnabled = true;
            return;
        }
    }
    if (end.interpolation == Interpolation::Step) {
        editor.mmdReason = T(editor.session.language, StrMmdStep);
        return;
    }
    if (end.value == start.value) {
        editor.mmdReason = T(editor.session.language, StrMmdEqual);
        return;
    }
    std::array<double, 4> controls = {{0, 0, 0, 0}};
    if (end.interpolation == Interpolation::Linear) {
        // The core stores a straight interval as Interpolation::Linear and the
        // stored slopes may still be zero, so the diagonal handles are derived
        // from the weights here.  Displaying the raw slopes would show an S
        // curve that the source does not actually have.
        const double x1 = std::floor(start.outWeight * 127.0 + 0.5);
        const double x2 = std::floor((1.0 - end.inWeight) * 127.0 + 0.5);
        if (x1 >= 1.0 && x2 <= 126.0 && x1 < x2) {
            controls = {{x1, x1, x2, x2}};
            editor.mmdControls = controls;
            editor.mmdEnabled = true;
            return;
        }
        editor.mmdReason = T(editor.session.language, StrMmdOvershoot);
        return;
    }
    if (!animation_curve::GetMmdControls(track, endKey, controls)) {
        editor.mmdReason = T(editor.session.language, StrMmdOvershoot);
        return;
    }
    editor.mmdControls = controls;
    editor.mmdEnabled = true;
}

void PaintMmd(Editor &editor, HDC dc) {
    RECT client = {};
    GetClientRect(editor.mmd, &client);
    HBRUSH background = CreateSolidBrush(curve_editor_theme::shell);
    FillRect(dc, &client, background);
    DeleteObject(background);
    HGDIOBJ oldFont = SelectObject(dc, editor.font ? editor.font : GetStockObject(DEFAULT_GUI_FONT));

    const int margin = Scale(6, editor.dpi);
    RECT square = {margin, margin, client.right - margin, client.bottom - margin};
    const int side = std::min(square.right - square.left, square.bottom - square.top);
    square.right = square.left + side;
    square.bottom = square.top + side;

    HBRUSH paper = CreateSolidBrush(curve_editor_theme::paper);
    FillRect(dc, &square, paper);
    DeleteObject(paper);
    if (editor.mmdEnabled) {
        FrameRect(dc, &square, GetSysColorBrush(COLOR_3DSHADOW));
    } else {
        FrameRect(dc, &square, GetSysColorBrush(COLOR_GRAYTEXT));
    }
    const auto toPoint = [&](double u, double v) {
        return POINT{square.left + static_cast<LONG>(std::lround(u / 127.0 * side)),
                     square.bottom - static_cast<LONG>(std::lround(v / 127.0 * side))};
    };
    HPEN diagonal = CreatePen(PS_SOLID, 1, curve_editor_theme::grid);
    HGDIOBJ oldPen = SelectObject(dc, diagonal);
    // "far" is a legacy Windows macro, so the second point needs its own name.
    const POINT origin = toPoint(0, 0);
    const POINT opposite = toPoint(127, 127);
    MoveToEx(dc, origin.x, origin.y, nullptr);
    LineTo(dc, opposite.x, opposite.y);
    SelectObject(dc, oldPen);
    DeleteObject(diagonal);

    if (editor.mmdEnabled) {
        const std::array<double, 4> &c = editor.mmdControls;
        const POINT p0 = toPoint(0, 0);
        const POINT p3 = toPoint(127, 127);
        const POINT p1 = toPoint(c[0], c[1]);
        const POINT p2 = toPoint(c[2], c[3]);
        const auto&state=CurrentState(editor);
        HPEN curve = CreatePen(PS_SOLID, 1, editor.mmdEnabled&&state.active<state.channels.size() ? ChannelColour(state.channels[state.active]) : RGB(140, 140, 140));
        oldPen = SelectObject(dc, curve);
        HPEN handlePen = CreatePen(PS_SOLID, 1, curve_editor_theme::arm);
        HGDIOBJ previous = SelectObject(dc, handlePen);
        MoveToEx(dc, p0.x, p0.y, nullptr);
        LineTo(dc, p1.x, p1.y);
        MoveToEx(dc, p3.x, p3.y, nullptr);
        LineTo(dc, p2.x, p2.y);
        SelectObject(dc, previous);
        DeleteObject(handlePen);
        POINT points[4] = {p0, p1, p2, p3};
        PolyBezier(dc, points, 4);
        SelectObject(dc, oldPen);
        DeleteObject(curve);
        const int half = Scale(4, editor.dpi);
        const POINT handles[2] = {p1, p2};
        for (const POINT &handle : handles) {
            RECT box = {handle.x - half, handle.y - half, handle.x + half + 1, handle.y + half + 1};
            HBRUSH brush = CreateSolidBrush(RGB(255,210,69));
            FillRect(dc, &box, brush);
            DeleteObject(brush);
            FrameRect(dc, &box, GetSysColorBrush(COLOR_3DSHADOW));
        }
    } else {
        SetTextColor(dc, curve_editor_theme::muted);
        RECT text = square;
        text.left += Scale(4, editor.dpi);
        text.top += Scale(4, editor.dpi);
        DrawTextW(dc, L"—", -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
}

// ---------------------------------------------------------------------------
// Control synchronisation.
// ---------------------------------------------------------------------------
std::wstring SourceLabel(const Session &session, std::size_t index) {
    if (index < session.sources.size()) return session.sources[index].label;
    return std::wstring();
}

void UpdateWindowTitle(Editor &editor) {
    std::wstring title = T(editor.session.language, StrTitle);
    const std::wstring label = SourceLabel(editor.session, editor.sourceIndex);
    if (!label.empty()) title += L" — " + label;
    if (editor.sourceIndex < editor.session.sources.size()) {
        const std::string &id = editor.session.sources[editor.sourceIndex].id;
        if (!id.empty()) title += L" [" + Widen(id) + L"]";
    }
    SetWindowTextW(editor.window, title.c_str());
}

void SyncMmdCoordinateFields(Editor &editor) {
    const int ids[]={kIdMmdX1,kIdMmdY1,kIdMmdX2,kIdMmdY2};
    for(int i=0;i<4;++i)
        SetControlText(editor.window,ids[i],editor.mmdEnabled?FormatNumber(editor.mmdControls[i]):L"");
}

void UpdateMmdPanel(Editor &editor) {
    ComputeMmdControls(editor);
    SyncMmdCoordinateFields(editor);
    std::wstring readout;
    if (editor.mmdEnabled) {
        if (const Track* track=ActiveTrack(editor);track && editor.mmdEndKey>0 &&
            std::size_t(editor.mmdEndKey)<track->keys.size())
            readout=std::wstring(editor.session.language==2?L"区间 ":editor.session.language==1?L"Range ":L"区間 ")+
                FormatInteger(track->keys[editor.mmdEndKey-1].frame)+L"–"+FormatInteger(track->keys[editor.mmdEndKey].frame)+
                (editor.session.language==2?L" 帧":L"");
    } else {
        readout = editor.mmdReason;
    }
    SetControlText(editor.window, kIdMmdReadout, readout);
    if (editor.mmd) InvalidateRect(editor.mmd, nullptr, FALSE);
}

void UpdateKeyFields(Editor &editor) {
    SourceState &state = CurrentState(editor);
    Track *track = ActiveTrack(editor);
    const std::size_t index = ActiveKeyIndex(editor);
    const bool hasKey = track && index != static_cast<std::size_t>(-1) && index < track->keys.size();
    long long frame = 0;
    double value = 0;
    if (editor.session.currentFrame) {
        const double current = editor.session.currentFrame();
        if (std::isfinite(current)) frame = static_cast<long long>(std::llround(current));
    }
    if (hasKey) {
        frame = track->keys[index].frame;
        value = track->keys[index].value;
    } else if (track && !track->keys.empty()) {
        value = SampleTrack(editor, *track, static_cast<double>(frame)).value;
    }
    SetControlText(editor.window, kIdFrameEdit, FormatInteger(frame));
    SetControlText(editor.window, kIdValueEdit, FormatNumber(value));
    if (hasKey) {
        const Key &key = track->keys[index];
        SetControlText(editor.window, kIdInSlopeEdit, FormatNumber(key.inSlope));
        SetControlText(editor.window, kIdOutSlopeEdit, FormatNumber(key.outSlope));
        SetControlText(editor.window, kIdInWeightEdit, FormatNumber(key.inWeight));
        SetControlText(editor.window, kIdOutWeightEdit, FormatNumber(key.outWeight));
        if (HWND combo = GetDlgItem(editor.window, kIdInterpolationCombo)) {
            int row = static_cast<int>(key.interpolation);
            for (const auto selected : state.selection)
                if (selected < track->keys.size() && track->keys[selected].interpolation != key.interpolation) row = -1;
            SendMessageW(combo, CB_SETCURSEL, row, 0);
        }
        if (HWND combo = GetDlgItem(editor.window, kIdTangentCombo)) {
            const auto modeRow = [](Tangent mode) {
                return mode == Tangent::Automatic ? kTangentAutomatic : mode == Tangent::Aligned ? 1 : 0;
            };
            int row = modeRow(key.tangent);
            for (const auto selected : state.selection)
                if (selected < track->keys.size() && modeRow(track->keys[selected].tangent) != row) row = -1;
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(row), 0);
        }
    } else {
        for (int id : {kIdInterpolationCombo, kIdTangentCombo})
            SendMessageW(GetDlgItem(editor.window, id), CB_SETCURSEL, WPARAM(-1), 0);
    }
    (void)state;
}

void SyncSelectionToList(Editor &editor) {
    HWND list = GetDlgItem(editor.window, kIdChannelList);
    if (!list) return;
    SourceState &state = CurrentState(editor);
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        if(index<editor.channelItems.size()) TreeView_SetCheckState(list, editor.channelItems[index], state.channels[index].selected);
    }
    if(state.active<editor.channelItems.size()) TreeView_SelectItem(list,editor.channelItems[state.active]);
}

// The same, but without moving the caret: a click on one row may only change
// that row, so the rows the user did not touch keep their state and their
// caret stays where the pointer left it.
void SyncVisibilityToList(Editor &editor) {
    HWND list = GetDlgItem(editor.window, kIdChannelList);
    if (!list) return;
    SourceState &state = CurrentState(editor);
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        if(index<editor.channelItems.size()) TreeView_SetCheckState(list,editor.channelItems[index],state.channels[index].selected);
    }
    std::map<HTREEITEM,std::pair<unsigned,unsigned>> totals;
    for(std::size_t index=0;index<editor.channelItems.size();++index)
        for(HTREEITEM parent=TreeView_GetParent(list,editor.channelItems[index]);parent;parent=TreeView_GetParent(list,parent)){
            auto&count=totals[parent];++count.first;if(state.channels[index].selected)++count.second;
        }
    for(const auto&entry:editor.collectionItems){const auto count=totals[entry.first];TreeView_SetCheckState(list,entry.first,count.first&&count.first==count.second);}
}

// The row text.  A filled dot marks the active editable curve, because the
// ticks now mean "shown" and no longer move the active curve on their own.
std::wstring ChannelRowText(const Channel &channel, bool active) {
    std::wstring text = active ? L"\u25CF " : L"";
    text += channel.label.empty() ? Widen(channel.id) : channel.label;
    if (!channel.unit.empty()) text += L" (" + channel.unit + L")";
    return text;
}

void PopulateChannels(Editor &editor) {
    HWND list = GetDlgItem(editor.window, kIdChannelList);
    if (!list) return;
    SourceState &state = CurrentState(editor);
    const std::size_t active = state.channels.empty() ? 0 : std::min(state.active, state.channels.size() - 1);
    if(editor.rebuildingChannels)return;
    std::wstring shape=Widen(CurrentSource(editor).id);
    for(const auto& channel:state.channels){
        shape+=L"\n"+Widen(channel.id)+L"\t"+channel.label+L"\t"+channel.unit;
        for(const auto& group:channel.collections)shape+=L"\r"+Widen(group.id)+L"\t"+group.label;
    }
    if(shape==editor.channelTreeShape&&editor.channelItems.size()==state.channels.size()){
        editor.rebuildingChannels=true;
        for(std::size_t index=0;index<state.channels.size();++index){
            const std::wstring text=ChannelRowText(state.channels[index],index==active)+L"    ";
            TVITEMW item{};item.mask=TVIF_TEXT;item.hItem=editor.channelItems[index];item.pszText=const_cast<wchar_t*>(text.c_str());
            SendMessageW(list,TVM_SETITEMW,0,reinterpret_cast<LPARAM>(&item));
        }
        SyncVisibilityToList(editor);
        editor.rebuildingChannels=false;
        return;
    }
    editor.channelTreeShape=std::move(shape);
    editor.rebuildingChannels=true;
    if(editor.channelTreeSource==CurrentSource(editor).id)
        for(const auto&entry:editor.collectionItems)
            state.expandedCollections[entry.second]=(TreeView_GetItemState(list,entry.first,TVIS_EXPANDED)&TVIS_EXPANDED)!=0;
    editor.channelTreeSource=CurrentSource(editor).id;
    SendMessageW(list,WM_SETREDRAW,FALSE,0);
    TreeView_DeleteAllItems(list);
    editor.collectionItems.clear();editor.channelItems.assign(state.channels.size(),nullptr);
    std::map<std::string,HTREEITEM> groups;
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        HTREEITEM parent=TVI_ROOT;std::string path;
        for(const auto&collection:state.channels[index].collections){
            if(!path.empty())path+="/";path+=collection.id;
            const auto found=groups.find(path);
            if(found!=groups.end()){parent=found->second;continue;}
            TVINSERTSTRUCTW insert{};insert.hParent=parent;insert.hInsertAfter=TVI_LAST;
            insert.item.mask=TVIF_TEXT|TVIF_PARAM;
            const std::wstring label=collection.label+L"    ";
            insert.item.pszText=const_cast<wchar_t*>(label.c_str());insert.item.lParam=0;
            parent=reinterpret_cast<HTREEITEM>(SendMessageW(list,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&insert)));
            groups[path]=parent;editor.collectionItems[parent]=path;
        }
        const std::wstring text = ChannelRowText(state.channels[index], index == active)+L"    ";
        TVINSERTSTRUCTW insert{};insert.hParent=parent;insert.hInsertAfter=TVI_LAST;
        insert.item.mask=TVIF_TEXT|TVIF_PARAM;insert.item.pszText=const_cast<wchar_t*>(text.c_str());insert.item.lParam=index+1;
        editor.channelItems[index]=reinterpret_cast<HTREEITEM>(SendMessageW(list,TVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&insert)));
    }
    const std::string&initial=CurrentSource(editor).initialExpandedCollection;
    for(const auto&entry:groups){
        const auto remembered=state.expandedCollections.find(entry.first);
        const bool initialBranch=initial.empty()||initial==entry.first||
            initial.rfind(entry.first+"/",0)==0||entry.first.rfind(initial+"/",0)==0;
        if(remembered!=state.expandedCollections.end()?remembered->second:initialBranch)
            TreeView_Expand(list,entry.second,TVE_EXPAND);
    }
    SyncVisibilityToList(editor);
    if(active<editor.channelItems.size()){
        bool visible=true;
        for(HTREEITEM parent=TreeView_GetParent(list,editor.channelItems[active]);parent;parent=TreeView_GetParent(list,parent))
            visible=visible&&((TreeView_GetItemState(list,parent,TVIS_EXPANDED)&TVIS_EXPANDED)!=0);
        if(visible)TreeView_SelectItem(list,editor.channelItems[active]);
    }
    editor.rebuildingChannels=false;
    SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,nullptr,FALSE);
}

// Refreshes the source combo in place.  The language switch and a re-read both
// need the current labels without disturbing the view/tool/key combos.
void FillSourceCombo(Editor &editor) {
    HWND combo = GetDlgItem(editor.window, kIdSourceCombo);
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const Source &source : editor.session.sources) {
        const std::wstring text = source.label.empty() ? Widen(source.id) : source.label;
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(editor.sourceIndex), 0);
}

// The optional Source::localizedLabel owns the displayed source name; a source
// without it keeps the plain label, which is what the current MMV plugin does.
void RefreshSourceLabels(Editor &editor) {
    const unsigned language = editor.session.language;
    for (Source &source : editor.session.sources) {
        if (!source.localizedLabel) continue;
        std::wstring localized = source.localizedLabel(language);
        if (!localized.empty()) source.label = std::move(localized);
    }
}

void UpdateBakeFields(Editor &editor) {
    Track *track = ActiveTrack(editor);
    if (!track || track->keys.empty()) return;
    SetControlText(editor.window, kIdBakeFirstEdit, FormatInteger(track->keys.front().frame));
    SetControlText(editor.window, kIdBakeLastEdit, FormatInteger(track->keys.back().frame));
}

void UpdateEnabledState(Editor &editor) {
    SourceState &state = CurrentState(editor);
    Source &source = CurrentSource(editor);
    const bool valid = state.valid;
    const bool writable = valid && source.apply != nullptr;
    const bool hasChannel = !state.channels.empty();
    const bool hasSelection = !state.selection.empty();

    // Editing controls: disabled while the source is invalid or read-only so a
    // refused write can never be started, and the reason stays in the status.
    for (int id : {kIdFrameEdit, kIdValueEdit, kIdInterpolationCombo, kIdTangentCombo, kIdInSlopeEdit,
                   kIdOutSlopeEdit, kIdInWeightEdit, kIdOutWeightEdit, kIdApplyKey, kIdAddKey, kIdDeleteKey,
                   kIdUndo, kIdRedo, kIdBakeFirstEdit, kIdBakeLastEdit, kIdBakeStepEdit, kIdBakeRun}) {
        EnableWindow(GetDlgItem(editor.window, id), (writable && hasChannel) ? TRUE : FALSE);
    }
    // Navigation and view selection only change what is on screen, so they stay
    // available for a source that can be read but not written.
    for (int id : {kIdFitAll, kIdFitRange, kIdViewCombo, kIdToolCombo, kIdChannelList, kIdChannelAll, kIdChannelInvert}) {
        EnableWindow(GetDlgItem(editor.window, id), hasChannel ? TRUE : FALSE);
    }
    EnableWindow(GetDlgItem(editor.window, kIdDeleteKey), (writable && hasSelection) ? TRUE : FALSE);
    EnableWindow(GetDlgItem(editor.window, kIdApplyKey), (writable && hasSelection) ? TRUE : FALSE);
    const Track* track = ActiveTrack(editor);
    const bool handles = writable && hasSelection && track && !track->discrete;
    const bool hasInterval = handles && std::any_of(state.selection.begin(), state.selection.end(),
        [track](std::size_t index) { return index > 0 && index < track->keys.size(); });
    EnableWindow(GetDlgItem(editor.window, kIdInterpolationCombo), hasInterval);
    EnableWindow(GetDlgItem(editor.window, kIdTangentCombo), handles && track->keys.size() > 1);
    EnableWindow(GetDlgItem(editor.window, kIdBakeRun), writable ? TRUE : FALSE);
    EnableWindow(GetDlgItem(editor.window, kIdReadMotion), (valid && source.readMotion) ? TRUE : FALSE);
    EnableWindow(GetDlgItem(editor.window, kIdWriteMotion), (valid && source.writeMotion) ? TRUE : FALSE);
    if (editor.mmd) {
        EnableWindow(editor.mmd, editor.mmdEnabled && writable ? TRUE : FALSE);
        InvalidateRect(editor.mmd, nullptr, FALSE);
    }
    for(int id:{kIdMmdX1,kIdMmdY1,kIdMmdX2,kIdMmdY2})
        EnableWindow(GetDlgItem(editor.window,id),editor.mmdEnabled && writable);
    // Comparison depends on what is on screen, never on the ability to write,
    // so a read-only source that offers comparison keeps the controls usable.
    UpdateCompareUi(editor);
}

// ---------------------------------------------------------------------------
// Comparison display controls.
// ---------------------------------------------------------------------------
std::wstring CompareModeLabel(Editor &editor, int mode) {
    switch (mode) {
    case kCompareAllGroups: return T(editor.session.language, StrCompareAllGroups);
    case kCompareFree: return T(editor.session.language, StrCompareFree);
    default: return T(editor.session.language, StrCompareSameGroup);
    }
}

// The span the filter button would use right now: the smallest and largest
// frame of the selected keys while at least two of them are selected, and the
// two range fields otherwise.  The two returned bounds are always ordered.
bool CompareSpan(Editor &editor, double &first, double &last, int &source) {
    SourceState &state = CurrentState(editor);
    first = last = 0;
    source = kSpanNone;
    if (state.channels.empty()) return false;
    const std::size_t active = std::min(state.active, state.channels.size() - 1);
    const Track &track = state.channels[active].track;
    if (state.selection.size() >= 2 && !track.keys.empty()) {
        bool any = false;
        double low = 0, high = 0;
        for (const std::size_t index : state.selection) {
            if (index >= track.keys.size()) continue;
            const double frame = static_cast<double>(track.keys[index].frame);
            if (!any) {
                low = high = frame;
                any = true;
            } else {
                low = std::min(low, frame);
                high = std::max(high, frame);
            }
        }
        if (any) {
            first = low;
            last = high;
            source = kSpanKeys;
            return true;
        }
    }
    long long low = 0, high = 0;
    if (ParseIntegerText(ControlText(editor.window, kIdBakeFirstEdit), low) &&
        ParseIntegerText(ControlText(editor.window, kIdBakeLastEdit), high)) {
        if (low > high) std::swap(low, high);
        first = static_cast<double>(low);
        last = static_cast<double>(high);
        source = kSpanFields;
        return true;
    }
    return false;
}

// The three mode buttons act as one radio group, so the checked state is
// written for all of them at once; BM_SETCHECK alone would leave a stale check
// on the previous button.
void UpdateCompareUi(Editor &editor) {
    const bool supported = ComparisonSupported(editor);
    SourceState &state = CurrentState(editor);
    const BOOL usable = (supported && !state.channels.empty()) ? TRUE : FALSE;
    const struct {
        int id;
        int mode;
    } buttons[] = {
        {kIdCompareSameGroup, kCompareSameGroup},
        {kIdCompareAllGroups, kCompareAllGroups},
        {kIdCompareFree, kCompareFree},
    };
    for (const auto &button : buttons) {
        HWND control = GetDlgItem(editor.window, button.id);
        if (!control) continue;
        const bool pressed = usable && state.compareMode == button.mode;
        SendMessageW(control, BM_SETCHECK, pressed ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(control, usable);
    }
    EnableWindow(GetDlgItem(editor.window, kIdCompareFilter), usable);
}

// Enters a source: the main curve named by the source, then the comparison
// mode the host remembers for it, then the controls that show both.
// Puts the main curve back on the channel Source::initialMainChannel names and
// makes sure that channel is shown again.  A source that names none, or names
// an unknown id, keeps the curve the user selected.  Returns true when the
// active channel moved.
bool ApplyInitialMainChannel(Editor &editor) {
    SourceState &state = CurrentState(editor);
    Source &source = CurrentSource(editor);
    if (source.initialMainChannel.empty() || state.channels.empty()) return false;
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        if (state.channels[index].id != source.initialMainChannel) continue;
        const bool moved = state.active != index;
        state.active = index;
        // Activation always shows the row: it may have been hidden by a click.
        state.channels[index].selected = true;
        return moved;
    }
    return false;
}

// Writes the result of an automatic mode into the display flags.  This is what
// keeps the list ticks and the canvas in step: an automatic mode is a way to
// compute a set of curves, and the set it computes becomes the visible set.  A
// tick therefore always means "this curve is on screen".  The main curve stays
// visible; a curve without keys has nothing to draw and stays unticked.
void MaterializeCompareMask(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if (!ComparisonSupported(editor)) return;
    if (state.compareMode == kCompareOff || state.compareMode == kCompareFree) return;
    if (state.channels.empty()) return;
    const std::size_t main = std::min(state.active, state.channels.size() - 1);
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        if (index == main) {
            state.channels[index].selected = true;
            continue;
        }
        Channel &channel = state.channels[index];
        bool visible = curve_comparison::IsRegistered(channel.track);
        if (visible && state.compareMode == kCompareSameGroup) {
            visible = channel.group >= 0 && state.channels[main].group >= 0 &&
                      channel.group == state.channels[main].group;
        } else if (visible && state.compareMode == kCompareAllGroups) {
            visible = channel.group >= 0;
        }
        if (visible && !IsVarying(channel.track)) visible = false;
        channel.selected = visible;
    }
}

void LoadCompareMode(Editor &editor) {
    SourceState &state = CurrentState(editor);
    Source &source = CurrentSource(editor);
    if (!ComparisonSupported(editor)) {
        state.compareMode = kCompareOff;
        return;
    }
    // The window offers no "off" mode, so a host that has nothing stored, or
    // stored the internal off state, starts on the same-group display.
    int mode = source.readComparisonMode ? source.readComparisonMode() : -1;
    if (mode < kCompareSameGroup || mode >= kCompareModeCount) mode = kCompareSameGroup;
    state.compareMode = mode;
}

void EnterSource(Editor &editor) {
    ApplyInitialMainChannel(editor);
    LoadCompareMode(editor);
    LayoutEditor(editor);
    RefreshEditorsUi(editor);
}

// Stores a mode with the host and adopts it only when the host accepted it, so
// what the window shows and what the host remembers can never drift apart.  A
// refused write leaves the mode, the display flags and the active curve exactly
// as they were.
//
// An automatic mode (same group, four groups) is also the documented way back to
// the main expression: it restores that curve, shows it, and writes its result
// into the display flags.  The free display keeps the active curve and the
// display set the user has: it is the mode a manual show/hide and a double click
// enter, and neither of those may be undone by picking the segment.
bool SetCompareMode(Editor &editor, int mode, std::wstring &error) {
    error.clear();
    if (mode < kCompareSameGroup || mode >= kCompareModeCount) return false;
    if (!ComparisonSupported(editor)) return false;
    SourceState &state = CurrentState(editor);
    Source &source = CurrentSource(editor);
    if (mode != state.compareMode && source.writeComparisonMode && !source.writeComparisonMode(mode, error))
        return false;
    state.compareMode = mode;
    if (mode == kCompareFree) {
        UpdateCompareUi(editor);
        InvalidateCanvas(editor);
        return true;
    }
    const bool moved = ApplyInitialMainChannel(editor);
    if (moved) state.selection.clear();
    RefreshEditorsUi(editor);
    if (moved) FitAll(editor);
    return true;
}

// A segment was pressed.  An automatic segment always re-applies, so pressing it
// again regenerates its set after manual show/hide work moved the display to the
// free segment.
void ChangeCompareMode(Editor &editor, int mode) {
    if (!ComparisonSupported(editor)) return;
    SourceState &state = CurrentState(editor);
    if (mode == state.compareMode && mode == kCompareFree) {
        UpdateCompareUi(editor);
        return;
    }
    std::wstring error;
    if (!SetCompareMode(editor, mode, error)) {
        std::wstring message = T(editor.session.language, StrCompareModeFailed);
        if (!error.empty()) message += error;
        SetStatus(editor, message, true);
        UpdateCompareUi(editor);
        return;
    }
    SetStatus(editor, std::wstring(T(editor.session.language, StrCompareModeHead)) + CompareModeLabel(editor, mode),
              false);
}

// A manual show/hide and a double click leave the automatic modes: the display
// moves to the free segment, which carries the set that is on screen right now.
// Nothing an automatic mode had hidden comes back, and the active curve is not
// touched.  A refused mode write keeps mode, flags and active curve as they were
// and reports the reason, so the manual change never half-applies.
bool EnterFreeForManualDisplay(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if (!ComparisonSupported(editor)) return true;
    if (state.compareMode == kCompareOff || state.compareMode == kCompareFree) return true;
    std::wstring error;
    if (SetCompareMode(editor, kCompareFree, error)) return true;
    std::wstring message = T(editor.session.language, StrCompareModeFailed);
    if (!error.empty()) message += error;
    SetStatus(editor, message, true);
    UpdateCompareUi(editor);
    return false;
}

// One click, one run: the filter shows and ticks every curve that really
// changes inside the span, reports the span it used in the status line and
// hands the result to the free mode.  The active curve stays where it is, and
// only the view flag Channel::selected changes, so no curve data is written.
void RunCompareFilter(Editor &editor) {
    SourceState &state = CurrentState(editor);
    const unsigned language = editor.session.language;
    double first = 0, last = 0;
    int source = kSpanNone;
    if (!CompareSpan(editor, first, last, source)) {
        SetStatus(editor, T(language, StrCompareNoRange), true);
        return;
    }

    const std::size_t main = state.channels.empty() ? 0 : std::min(state.active, state.channels.size() - 1);
    int kept = 0;
    for (std::size_t index = 0; index < state.channels.size(); ++index) {
        Channel &channel = state.channels[index];
        double held = 0;
        const bool registered = curve_comparison::IsRegistered(channel.track);
        const bool constant = registered && IsConstantOverRange(channel.track, first, last, held);
        // The main curve keeps its tick whatever the span says; a curve without
        // keys has nothing to draw and is never ticked into the display, so a
        // tick keeps meaning "this curve is on screen".
        channel.selected = (index == main) || (registered && !constant);
        if (index != main && registered && !constant) ++kept;
    }
    SyncSelectionToList(editor);

    const std::wstring head = T(language, StrCompareRangeHead) + FormatNumber(first) + L" – " +
                              FormatNumber(last) +
                              T(language, source == kSpanKeys ? StrCompareFromKeys : StrCompareFromFields);
    std::wstring message = head + T(language, StrCompareKeptHead) + FormatInteger(kept) +
                           T(language, StrCompareKeptTail);
    // The filter result is exactly what the free mode shows, so the display
    // follows it; a host that refuses to store that mode keeps the previous one
    // and the message says so instead of pretending the switch happened.
    std::wstring error;
    if (SetCompareMode(editor, kCompareFree, error)) {
        message += T(language, StrCompareFilterFree);
        SetStatus(editor, message, false);
    } else if (!error.empty()) {
        message += std::wstring(T(language, StrCompareModeFailed)) + error;
        SetStatus(editor, message, true);
    } else {
        SetStatus(editor, message, false);
    }
    UpdateCompareUi(editor);
    InvalidateCanvas(editor);
}

void RefreshEditorsUi(Editor &editor) {
    // An automatic mode owns the display flags.  Re-applying its mask here also
    // covers a re-read from the host, which hands back the host's own flags:
    // the list must never claim a curve the canvas does not draw.  In the free
    // segment the flags are the user's and this call does nothing.
    MaterializeCompareMask(editor);
    PopulateChannels(editor);
    UpdateKeyFields(editor);
    UpdateMmdPanel(editor);
    UpdateEnabledState(editor);
    UpdateCompareUi(editor);
    InvalidateCanvas(editor);
}

void ClampView(Editor &editor);

void InvalidateCanvas(Editor &editor) {
    // Every view change repaints through here, so the view bounds hold for
    // the wheel, the range bars and their keys alike.
    ClampView(editor);
    RefreshMouseReadout(editor);
    if (editor.canvas) InvalidateRect(editor.canvas, nullptr, FALSE);
}

// Repaints only the columns the playhead left and entered.  The playhead is
// read from the session at paint time, so nothing here touches channel data.
void InvalidatePlayhead(Editor &editor) {
    if (!editor.canvas) return;
    CanvasMetrics(editor);
    bool any = false;
    RECT band = {};
    const double frames[2] = {editor.lastPlayheadFrame, editor.playheadFrame};
    for (const double frame : frames) {
        if (!std::isfinite(frame)) continue;
        double x = 0;
        if (!TimeToX(editor, frame, x)) continue;
        // Include both the old and new frame flags in the ruler as well as
        // their plot columns. The caption can be wider than the playhead line.
        const auto caption=FormatInteger(static_cast<long long>(std::llround(frame)));
        const int flagWidth=Scale(8+10*int(caption.size()),editor.dpi);
        RECT piece = {static_cast<LONG>(std::lround(x)) - flagWidth, 0,
                      static_cast<LONG>(std::lround(x)) + flagWidth, editor.plot.bottom};
        if (piece.left < editor.plot.left) piece.left = editor.plot.left;
        if (piece.right > editor.plot.right) piece.right = editor.plot.right;
        if (piece.right <= piece.left) continue;  // the column is off screen
        if (!any) {
            band = piece;
            any = true;
        } else {
            UnionRect(&band, &band, &piece);
        }
    }
    if (!any) return;
    InvalidateRect(editor.canvas, &band, FALSE);
}

// The playhead timer exists only while the window is on screen and the session
// reports a current frame: a hidden editor costs nothing and never repaints.
void UpdatePlayheadTimer(Editor &editor) {
    const bool wanted = editor.window && editor.session.currentFrame &&
                        IsWindowVisible(editor.window) != FALSE;
    if (wanted && !editor.playheadTimerRunning) {
        if (SetTimer(editor.window, kPlayheadTimer, kPlayheadIntervalMs, nullptr) != 0)
            editor.playheadTimerRunning = true;
    } else if (!wanted && editor.playheadTimerRunning) {
        KillTimer(editor.window, kPlayheadTimer);
        editor.playheadTimerRunning = false;
    }
}

void PublishKeySelection(Editor& editor){
    auto* channel=ActiveChannel(editor);auto& source=CurrentSource(editor);
    if(!channel||!source.writeKeySelection)return;
    std::vector<int> frames;
    for(auto index:CurrentState(editor).selection)
        if(index<channel->track.keys.size())frames.push_back(channel->track.keys[index].frame);
    source.writeKeySelection(channel->id,frames);
}
bool PullKeySelection(Editor& editor){
    if(editor.drag!=Drag::None||editor.mmdDragHandle>=0)return false;
    auto* channel=ActiveChannel(editor);auto& source=CurrentSource(editor);
    if(!channel||!source.readKeySelection)return false;
    const auto frames=source.readKeySelection(channel->id);
    auto& state=CurrentState(editor);
    // The usual timer tick observes the same selection. Compare its frame
    // addresses directly, without allocating another set of every baked key.
    if(frames.size()==state.selection.size()){
        auto frame=frames.begin();bool same=true;
        for(auto index:state.selection){
            if(index>=channel->track.keys.size()||channel->track.keys[index].frame!=*frame++){same=false;break;}
        }
        if(same)return false;
    }
    FrameSelectionCursor wanted(frames);
    std::set<std::size_t> selected;
    for(std::size_t index=0;index<channel->track.keys.size();++index)
        if(wanted.Contains(channel->track.keys[index].frame))selected.emplace_hint(selected.end(),index);
    if(selected==state.selection)return false;
    state.selection=std::move(selected);
    UpdateKeyFields(editor);UpdateMmdPanel(editor);UpdateEnabledState(editor);InvalidateCanvas(editor);
    return true;
}
const wchar_t* PlayCaption(Editor& editor){
    const bool playing=editor.session.isPlaying&&editor.session.isPlaying();
    return editor.session.language==0?(playing?L"停止":L"再生"):editor.session.language==1?(playing?L"Stop":L"Play"):(playing?L"停止":L"播放");
}
void SeekTimeline(Editor& editor,int frame){
    frame=std::clamp(frame,0,int(kMaximumFrame));
    const bool playing=editor.session.isPlaying&&editor.session.isPlaying();
    if((playing||!editor.session.currentFrame||editor.session.currentFrame()!=frame)&&editor.session.seek)editor.session.seek(frame);
    const double current=editor.session.currentFrame?editor.session.currentFrame():frame;
    if(!std::isfinite(current))return;
    SetControlText(editor.window,kIdTimelineFrame,FormatInteger(static_cast<long long>(std::llround(current))));
    editor.lastPlayheadFrame=editor.playheadFrame;editor.playheadFrame=current;
    InvalidatePlayhead(editor);
    editor.mmdFollowPlayhead=true;
    UpdateMmdPanel(editor);
}
void SeekAtPoint(Editor& editor,POINT point){
    double frame=0;
    if(XToTime(editor,std::clamp(point.x,editor.plot.left,editor.plot.right),frame))
        SeekTimeline(editor,int(std::clamp(std::round(frame),0.0,double(kMaximumFrame))));
}
void SeekAdjacentKey(Editor& editor,bool next){
    const Track* track=ActiveTrack(editor);
    if(!track || track->keys.empty() || !editor.session.currentFrame || !editor.session.seek)return;
    const double frame=editor.session.currentFrame();
    if(!std::isfinite(frame))return;
    if(next){
        const auto it=std::upper_bound(track->keys.begin(),track->keys.end(),frame,
            [](double value,const Key& key){return value<key.frame;});
        if(it!=track->keys.end())SeekTimeline(editor,it->frame);
    }else{
        const auto it=std::lower_bound(track->keys.begin(),track->keys.end(),frame,
            [](const Key& key,double value){return key.frame<value;});
        if(it!=track->keys.begin())SeekTimeline(editor,std::prev(it)->frame);
    }
    OnPlayheadTimer(editor);
}
void OnPlayheadTimer(Editor &editor) {
    // A hidden editor does no work at all: no frame query, no repaint.
    if (!editor.window || IsWindowVisible(editor.window) == FALSE) return;
    if (!editor.session.currentFrame) return;
    PullKeySelection(editor);
    SetControlText(editor.window,kIdPlay,PlayCaption(editor));
    ++editor.playheadTicks;
    const double frame = editor.session.currentFrame();
    if (!std::isfinite(frame)) return;
    if(GetFocus()!=GetDlgItem(editor.window,kIdTimelineFrame))
        SetControlText(editor.window,kIdTimelineFrame,FormatInteger(static_cast<long long>(std::lround(frame))));
    if (std::isfinite(editor.playheadFrame) && frame == editor.playheadFrame) return;
    editor.lastPlayheadFrame = editor.playheadFrame;
    editor.playheadFrame = frame;
    if(editor.mmdDragHandle<0){editor.mmdFollowPlayhead=true;UpdateMmdPanel(editor);}
    ++editor.playheadInvalidations;
    InvalidatePlayhead(editor);
}

// ---------------------------------------------------------------------------
// Editing commands.
// ---------------------------------------------------------------------------
void AddKey(Editor &editor) {
    if (!CheckSourceCurrent(editor)) return;
    Track *track = ActiveTrack(editor);
    if (!track) {
        SetStatus(editor, T(editor.session.language, StrNeedChannel), true);
        return;
    }
    long long frame = 0;
    double value = 0;
    if (!ParseIntegerText(ControlText(editor.window, kIdFrameEdit), frame) || frame < 0 || frame > kMaximumFrame) {
        SetStatus(editor, T(editor.session.language, StrInvalidFrame), true);
        return;
    }
    if (!ParseDoubleText(ControlText(editor.window, kIdValueEdit), value)) {
        SetStatus(editor, T(editor.session.language, StrInvalidNumber), true);
        return;
    }
    for (std::size_t existing = 0; existing < track->keys.size(); ++existing) {
        if (track->keys[existing].frame == static_cast<std::int32_t>(frame)) {
            // Adding over an existing frame never writes: it selects that key
            // and loads its values, which is what the user expects here.
            SourceState &state = CurrentState(editor);
            state.selection.clear();
            state.selection.insert(existing);
            UpdateKeyFields(editor);
            UpdateMmdPanel(editor);
            UpdateEnabledState(editor);
            InvalidateCanvas(editor);
            SetStatus(editor,
                      std::wstring(T(editor.session.language, StrFrameSelected)) + FormatInteger(frame), false);
            return;
        }
    }
    std::vector<Channel> next = CopyChannels(editor);
    Track &working = next[CurrentState(editor).active].track;
    Key key;
    key.frame = static_cast<std::int32_t>(frame);
    key.value = value;
    auto position = std::lower_bound(working.keys.begin(), working.keys.end(), key.frame,
                                     [](const Key &candidate, std::int32_t frame) { return candidate.frame < frame; });
    const std::size_t index = static_cast<std::size_t>(position - working.keys.begin());
    working.keys.insert(position, key);
    animation_curve::UpdateAutomatic(working);
    const std::wstring message = std::wstring(T(editor.session.language, StrAdded)) + FormatInteger(frame);
    if (CommitChannels(editor, std::move(next), message)) {
        SourceState &state = CurrentState(editor);
        state.selection.clear();
        state.selection.insert(index);
        UpdateKeyFields(editor);
        UpdateMmdPanel(editor);
        UpdateEnabledState(editor);
        InvalidateCanvas(editor);
    }
}

void DeleteSelectedKeys(Editor &editor) {
    if (!CheckSourceCurrent(editor)) return;
    PullKeySelection(editor);
    SourceState &state = CurrentState(editor);
    Track *track = ActiveTrack(editor);
    if (!track || state.selection.empty()) {
        SetStatus(editor, T(editor.session.language, StrNoSelection), true);
        return;
    }
    std::vector<Channel> next = CopyChannels(editor);
    Track &working = next[state.active].track;
    std::vector<Key> kept;
    kept.reserve(working.keys.size());
    for (std::size_t index = 0; index < working.keys.size(); ++index) {
        if (state.selection.count(index)) continue;
        kept.push_back(working.keys[index]);
    }
    const std::size_t removed = working.keys.size() - kept.size();
    working.keys = std::move(kept);
    // Removing a key changes the neighbourhood of the remaining automatic keys,
    // so their slopes are recomputed before the transaction is committed.
    animation_curve::UpdateAutomatic(working);
    const std::wstring message = std::wstring(T(editor.session.language, StrDeleted)) + FormatInteger(static_cast<long long>(removed));
    if (CommitChannels(editor, std::move(next), message)) {
        state.selection.clear();
        UpdateKeyFields(editor);
        UpdateMmdPanel(editor);
        UpdateEnabledState(editor);
    }
}

// Handle editing must not leave Linear/Step intervals ignoring the new slopes.
// Seed newly enabled Bezier intervals with their chord, preserving the other
// handle of a neighbouring key before changing the requested key's mode.
void EnableBezierHandles(Track& track, std::size_t key) {
    if (track.discrete || key >= track.keys.size()) return;
    for (const std::size_t end : {key, key + 1}) {
        if (end == 0 || end >= track.keys.size() || track.keys[end].interpolation == Interpolation::Bezier) continue;
        std::string error;
        for (const auto index : {end - 1, end}) {
            if (track.keys[index].tangent == Tangent::Automatic)
                animation_curve::SetTangent(track, index, Tangent::Free, error);
            track.keys[index].tangent = Tangent::Free;
        }
        const double slope = (track.keys[end].value - track.keys[end - 1].value) /
            (double(track.keys[end].frame) - track.keys[end - 1].frame);
        track.keys[end - 1].outSlope = track.keys[end].inSlope = slope;
        track.keys[end].interpolation = Interpolation::Bezier;
    }
}

void ApplyCurveChoice(Editor& editor, int id) {
    const int row = int(SendMessageW(GetDlgItem(editor.window, id), CB_GETCURSEL, 0, 0));
    if (!CheckSourceCurrent(editor)) return;
    PullKeySelection(editor);
    SourceState& state = CurrentState(editor);
    Track* track = ActiveTrack(editor);
    if (!track || track->discrete || state.selection.empty() ||
        !IsWindowEnabled(GetDlgItem(editor.window, id)) || row < 0) {
        UpdateKeyFields(editor);
        return;
    }
    auto next = CopyChannels(editor);
    Track& working = next[state.active].track;
    std::string error;
    for (const auto key : state.selection) {
        if (key >= working.keys.size()) continue;
        if (id == kIdInterpolationCombo) {
            if (key > 0 && row <= int(Interpolation::Bezier))
                working.keys[key].interpolation = static_cast<Interpolation>(row);
            continue;
        }
        if (row > kTangentEaseOut) continue;
        EnableBezierHandles(working, key);
        if (row == kTangentEaseIn || row == kTangentEaseOut) {
            animation_curve::ApplyEase(working, key, row == kTangentEaseIn);
        } else {
            const Tangent tangent = row == 0 ? Tangent::Free : row == 1 ? Tangent::Aligned :
                row == kTangentAutomatic ? Tangent::Automatic : row == kTangentFlat ? Tangent::Flat : Tangent::Linear;
            if (!animation_curve::SetTangent(working, key, tangent, error)) {
                SetStatus(editor, Widen(error), true);
                UpdateKeyFields(editor);
                return;
            }
            if (row == kTangentStraight) {
                if (key > 0) working.keys[key].interpolation = Interpolation::Linear;
                if (key + 1 < working.keys.size()) working.keys[key + 1].interpolation = Interpolation::Linear;
            }
            if (row >= kTangentFlat) working.keys[key].tangent = Tangent::Free;
        }
    }
    animation_curve::UpdateAutomatic(working);
    // Re-selecting a mode (or reapplying an unchanged preset) must not create
    // an undo entry or discard redo. Compare every selected key's result, not
    // just the highlighted row, because a multi-selection can contain different modes.
    if (!SameTrack(*track, working))
        CommitChannels(editor, std::move(next), T(editor.session.language, StrReady));
    // A refusal must restore the highlight too; presets never stay selected.
    UpdateKeyFields(editor);
    UpdateMmdPanel(editor);
    UpdateEnabledState(editor);
}

void ApplyKeyFields(Editor &editor) {
    if (!CheckSourceCurrent(editor)) return;
    if (PullKeySelection(editor)) {
        SetStatus(editor, T(editor.session.language, StrSelectionChanged), true);
        return;
    }
    SourceState &state = CurrentState(editor);
    Track *track = ActiveTrack(editor);
    if (!track) {
        SetStatus(editor, T(editor.session.language, StrNeedChannel), true);
        return;
    }
    if (state.selection.size() != 1) {
        SetStatus(editor, T(editor.session.language, StrSingleKeyForFrame), true);
        return;
    }
    const std::size_t index = *state.selection.begin();
    if (index >= track->keys.size()) return;
    long long frame = 0;
    double value = 0, inSlope = 0, outSlope = 0, inWeight = 0, outWeight = 0;
    if (!ParseIntegerText(ControlText(editor.window, kIdFrameEdit), frame) || frame < 0 || frame > kMaximumFrame) {
        SetStatus(editor, T(editor.session.language, StrInvalidFrame), true);
        return;
    }
    if (!ParseDoubleText(ControlText(editor.window, kIdValueEdit), value) ||
        !ParseDoubleText(ControlText(editor.window, kIdInSlopeEdit), inSlope) ||
        !ParseDoubleText(ControlText(editor.window, kIdOutSlopeEdit), outSlope) ||
        !ParseDoubleText(ControlText(editor.window, kIdInWeightEdit), inWeight) ||
        !ParseDoubleText(ControlText(editor.window, kIdOutWeightEdit), outWeight)) {
        SetStatus(editor, T(editor.session.language, StrInvalidNumber), true);
        return;
    }

    std::vector<Channel> next = CopyChannels(editor);
    Track &working = next[state.active].track;
    for (std::size_t other = 0; other < working.keys.size(); ++other) {
        if (other == index) continue;
        if (working.keys[other].frame == static_cast<std::int32_t>(frame)) {
            SetStatus(editor, std::wstring(T(editor.session.language, StrFrameExists)) + FormatInteger(frame), true);
            return;
        }
    }
    Key moved = working.keys[index];
    const bool inChanged = ControlText(editor.window, kIdInSlopeEdit) != FormatNumber(moved.inSlope);
    const bool outChanged = ControlText(editor.window, kIdOutSlopeEdit) != FormatNumber(moved.outSlope);
    moved.frame = static_cast<std::int32_t>(frame);
    moved.value = value;
    if (inChanged) moved.inSlope = inSlope;
    if (outChanged) moved.outSlope = outSlope;
    moved.inWeight = inWeight;
    moved.outWeight = outWeight;
    if (inChanged || outChanged) {
        if (moved.tangent == Tangent::Aligned) {
            const double slope = inChanged && outChanged ? 0.5 * (inSlope + outSlope) : inChanged ? inSlope : outSlope;
            moved.inSlope = moved.outSlope = slope;
        } else moved.tangent = Tangent::Free;
    }

    // Rebuild the key vector so the frame order stays strictly increasing.
    std::vector<Key> rebuilt;
    rebuilt.reserve(working.keys.size());
    std::size_t insertion = 0;
    bool placed = false;
    for (std::size_t other = 0; other < working.keys.size(); ++other) {
        if (other == index) continue;
        if (!placed && working.keys[other].frame > moved.frame) {
            insertion = rebuilt.size();
            placed = true;
        }
        rebuilt.push_back(working.keys[other]);
    }
    if (!placed) insertion = rebuilt.size();
    rebuilt.insert(rebuilt.begin() + static_cast<std::ptrdiff_t>(insertion), moved);
    working.keys = std::move(rebuilt);

    if (inChanged || outChanged) {
        EnableBezierHandles(working, insertion);
        working.keys[insertion].inSlope = moved.inSlope;
        working.keys[insertion].outSlope = moved.outSlope;
        working.keys[insertion].tangent = moved.tangent;
    }
    animation_curve::UpdateAutomatic(working);
    if (CommitChannels(editor, std::move(next), T(editor.session.language, StrReady))) {
        state.selection.clear();
        state.selection.insert(insertion);
        UpdateKeyFields(editor);
        UpdateMmdPanel(editor);
        UpdateEnabledState(editor);
    }
}

void SelectAllChannels(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if(!EnterFreeForManualDisplay(editor))return;
    for (Channel &channel : state.channels) channel.selected = !ComparisonSupported(editor)||curve_comparison::IsRegistered(channel.track);
    SyncVisibilityToList(editor);UpdateCompareUi(editor);InvalidateCanvas(editor);
    SetStatus(editor, T(editor.session.language, StrReady), false);
}

void InvertChannels(Editor &editor) {
    if(!EnterFreeForManualDisplay(editor))return;
    SourceState& state=CurrentState(editor);
    for(Channel& channel:state.channels){
        channel.selected=!channel.selected;
        if(ComparisonSupported(editor)&&!curve_comparison::IsRegistered(channel.track))channel.selected=false;
    }
    SyncVisibilityToList(editor);UpdateCompareUi(editor);InvalidateCanvas(editor);
    SetStatus(editor,T(editor.session.language,StrReady),false);
}

void RunBake(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty()) {
        SetStatus(editor, T(editor.session.language, StrNeedChannel), true);
        return;
    }
    long long first = 0, last = 0, step = 0;
    if (!ParseIntegerText(ControlText(editor.window, kIdBakeFirstEdit), first) ||
        !ParseIntegerText(ControlText(editor.window, kIdBakeLastEdit), last) ||
        !ParseIntegerText(ControlText(editor.window, kIdBakeStepEdit), step)) {
        SetStatus(editor, T(editor.session.language, StrInvalidNumber), true);
        return;
    }
    bool any = false;
    for (const Channel &channel : state.channels) any = any || channel.selected;
    if (!any) {
        SetStatus(editor, T(editor.session.language, StrNoChannels), true);
        return;
    }
    std::vector<Channel> next = CopyChannels(editor);
    int baked = 0;
    for (Channel &channel : next) {
        if (!channel.selected) continue;
        Track output;
        std::string error;
        // The core refuses a partial range and reports it; the message is shown
        // verbatim instead of being replaced by a generic failure text.
        if (!animation_curve::Bake(channel.track, static_cast<std::int32_t>(first), static_cast<std::int32_t>(last),
                                   static_cast<std::int32_t>(step), output, error)) {
            SetStatus(editor, std::wstring(T(editor.session.language, StrBakeFailed)) + Widen(error), true);
            return;
        }
        channel.track = std::move(output);
        ++baked;
    }
    const std::wstring message = std::wstring(T(editor.session.language, StrBakeDone)) + FormatInteger(baked);
    if (CommitChannels(editor, std::move(next), message)) {
        UpdateBakeFields(editor);
        InvalidateCanvas(editor);
    }
}

void RunReadMotion(Editor &editor) {
    Source &source = CurrentSource(editor);
    if (!source.readMotion) return;
    std::wstring error;
    if (!source.readMotion(editor.window, error)) {
        SetStatus(editor, std::wstring(T(editor.session.language, StrMotionFailed)) + error, true);
        return;
    }
    ReadAndAdopt(editor, true);
    SetStatus(editor, T(editor.session.language, StrMotionReadDone), false);
}

void RunWriteMotion(Editor &editor) {
    if (!CheckSourceCurrent(editor)) return;
    Source &source = CurrentSource(editor);
    if (!source.writeMotion) return;
    SourceState &state = CurrentState(editor);
    std::wstring error;
    if (!source.writeMotion(editor.window, state.channels, error)) {
        SetStatus(editor, std::wstring(T(editor.session.language, StrMotionFailed)) + error, true);
        return;
    }
    SetStatus(editor, T(editor.session.language, StrMotionWriteDone), false);
}

// ---------------------------------------------------------------------------
// Source switching and refresh.
// ---------------------------------------------------------------------------
void SwitchSource(Editor &editor, std::size_t index) {
    if (index >= editor.session.sources.size()) return;
    HWND tree=GetDlgItem(editor.window,kIdChannelList);
    for(const auto&entry:editor.collectionItems)
        CurrentState(editor).expandedCollections[entry.second]=(TreeView_GetItemState(tree,entry.first,TVIS_EXPANDED)&TVIS_EXPANDED)!=0;
    editor.sourceIndex = index;
    SourceState &state = CurrentState(editor);
    if (!state.loaded) {
        ReadAndAdopt(editor, false);
        // Entering a source is where its initial main channel and its
        // remembered comparison mode apply; later reads keep both.
        EnterSource(editor);
    } else {
        // Re-reading keeps external writes visible; the local history is only
        // discarded when the source actually changed under us.
        bool changed = false;
        if (ReadCurrentSource(editor, &changed) && changed) {
            state.undo.clear();
            state.redo.clear();
            state.selection.clear();
        }
        LayoutEditor(editor);
        RefreshEditorsUi(editor);
        FitAll(editor);
        if (!state.valid) SetStatus(editor, state.reason, true);
    }
    UpdateWindowTitle(editor);
    UpdateBakeFields(editor);
}

// ---------------------------------------------------------------------------
// Canvas interaction.
// ---------------------------------------------------------------------------
// The view is bounded by the active track: MMD frames and values are modest,
// and an unbounded multiplicative zoom ran into magnitudes where drawing
// lost precision.  Time spans one frame up to three times the key span (at
// least 300 frames); values 1/1000 up to 50 times the data span, measured
// against at least one unit.  The view centre stays within one maximum span
// of the data, so panning cannot drift to extreme coordinates either.
namespace {
void BoundSpan(double &low, double &high, double minSpan, double maxSpan,
               double dataLow, double dataHigh) {
    if (!std::isfinite(low) || !std::isfinite(high) || !(high > low)) {
        low = dataLow;
        high = dataHigh;
    }
    double span = std::clamp(high - low, minSpan, maxSpan);
    double centre = 0.5 * (low + high);
    centre = std::clamp(centre, dataLow - maxSpan, dataHigh + maxSpan);
    low = centre - span * 0.5;
    high = centre + span * 0.5;
}
}  // namespace

void ClampTimeView(Editor &editor) {
    const TrackExtent extent = VisibleExtent(editor);
    const double first = extent.valid ? extent.first : 0.0;
    const double last = extent.valid ? extent.last : 100.0;
    const double maxSpan = std::max(300.0, 3.0 * (last - first));
    BoundSpan(editor.timeStart, editor.timeEnd, kMinimumTimeSpan, maxSpan, first, last);
    if (editor.timeStart < 0.0) {
        editor.timeEnd -= editor.timeStart;
        editor.timeStart = 0.0;
    }
}

void ClampValueView(Editor &editor) {
    const TrackExtent extent = VisibleExtent(editor);
    const auto bound = [](double &low, double &high, double dataLow, double dataHigh) {
        const double reference = std::max(1.0, dataHigh - dataLow);
        BoundSpan(low, high, std::max(kMinimumValueSpan, reference * 1.0e-3),
                  reference * 50.0, dataLow, dataHigh);
    };
    if (editor.viewMode == 0)
        bound(editor.valueLow, editor.valueHigh, extent.valid ? extent.low : -1.0,
              extent.valid ? extent.high : 1.0);
    else
        bound(editor.speedLow, editor.speedHigh, extent.valid ? extent.speedLow : -1.0,
              extent.valid ? extent.speedHigh : 1.0);
}

void ClampView(Editor &editor) {
    ClampTimeView(editor);
    ClampValueView(editor);
}

void ZoomTime(Editor &editor, double anchorFrame, double factor) {
    editor.timeStart = anchorFrame + (editor.timeStart - anchorFrame) * factor;
    editor.timeEnd = anchorFrame + (editor.timeEnd - anchorFrame) * factor;
    ClampTimeView(editor);
}

void ZoomValue(Editor &editor, double anchorValue, double factor) {
    if (editor.viewMode == 0) {
        editor.valueLow = anchorValue + (editor.valueLow - anchorValue) * factor;
        editor.valueHigh = anchorValue + (editor.valueHigh - anchorValue) * factor;
    } else {
        editor.speedLow = anchorValue + (editor.speedLow - anchorValue) * factor;
        editor.speedHigh = anchorValue + (editor.speedHigh - anchorValue) * factor;
    }
    ClampValueView(editor);
}

void PanTime(Editor &editor, double deltaFrames) {
    deltaFrames = std::max(deltaFrames, -editor.timeStart);
    editor.timeStart += deltaFrames;
    editor.timeEnd += deltaFrames;
}

void PanValue(Editor &editor, double deltaValue) {
    if (editor.viewMode == 0) {
        editor.valueLow += deltaValue;
        editor.valueHigh += deltaValue;
    } else {
        editor.speedLow += deltaValue;
        editor.speedHigh += deltaValue;
    }
}

void CommitKeyDrag(Editor &editor) {
    SourceState &state = CurrentState(editor);
    if (!editor.dragMoved || editor.dragSelection.empty()) return;
    Track *track = ActiveTrack(editor);
    if (!track) return;
    std::vector<Channel> next = CopyChannels(editor);
    Track &working = next[state.active].track;

    std::set<std::int32_t> targets;
    for (std::size_t index = 0; index < working.keys.size(); ++index) {
        if (editor.dragSelection.count(index)) {
            const long long frame = static_cast<long long>(working.keys[index].frame) + editor.dragFrameDelta;
            if (frame < 0 || frame > kMaximumFrame) {
                SetStatus(editor, T(editor.session.language, StrInvalidFrame), true);
                return;
            }
            if (!targets.insert(static_cast<std::int32_t>(frame)).second) {
                SetStatus(editor, T(editor.session.language, StrDragConflict), true);
                return;
            }
        }
    }
    for (std::size_t index = 0; index < working.keys.size(); ++index) {
        if (editor.dragSelection.count(index)) continue;
        if (targets.count(working.keys[index].frame)) {
            SetStatus(editor, T(editor.session.language, StrDragConflict), true);
            return;
        }
    }

    // Applying one uniform delta keeps the relative order of the moved keys,
    // but a key dragged past a neighbour has to change places with it, so the
    // rebuilt vector is sorted again and the selection follows its keys.
    struct Entry {
        Key key;
        std::size_t origin;
    };
    std::vector<Entry> entries;
    entries.reserve(working.keys.size());
    for (std::size_t index = 0; index < working.keys.size(); ++index) {
        Key key = working.keys[index];
        if (editor.dragSelection.count(index)) {
            key.frame = static_cast<std::int32_t>(key.frame + editor.dragFrameDelta);
            key.value += editor.dragValueDelta;
        }
        entries.push_back(Entry{key, index});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &left, const Entry &right) { return left.key.frame < right.key.frame; });
    std::vector<Key> movedKeys;
    movedKeys.reserve(entries.size());
    std::map<std::size_t, std::size_t> movedIndex;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        movedKeys.push_back(entries[index].key);
        movedIndex[entries[index].origin] = index;
    }
    working.keys = std::move(movedKeys);
    std::set<std::size_t> keep;
    for (const std::size_t index : editor.dragSelection) {
        const auto found = movedIndex.find(index);
        if (found != movedIndex.end()) keep.insert(found->second);
    }
    const std::wstring message =
        std::wstring(T(editor.session.language, StrMoved)) + FormatInteger(static_cast<long long>(keep.size()));
    if (CommitChannels(editor, std::move(next), message)) {
        state.selection = keep;
        UpdateKeyFields(editor);
        UpdateMmdPanel(editor);
        UpdateEnabledState(editor);
    }
}

// One undoable shape edit of the active track (Alt-click on a key or handle),
// leaving `key` selected so its handles stay visible.
template <typename Edit>
void EditKeyShape(Editor &editor, std::size_t key, Edit edit) {
    SourceState &state = CurrentState(editor);
    if (state.channels.empty() || state.channels[state.active].track.discrete) return;
    std::vector<Channel> next = CopyChannels(editor);
    Track &working = next[state.active].track;
    if (key >= working.keys.size()) return;
    edit(working);
    if (CommitChannels(editor, std::move(next), T(editor.session.language, StrReady))) {
        state.selection.clear();
        state.selection.insert(key);
        UpdateKeyFields(editor);
        UpdateMmdPanel(editor);
        UpdateEnabledState(editor);
    }
    InvalidateCanvas(editor);
}

void CancelDrag(Editor &editor) {
    if (editor.drag == Drag::Keys && editor.dragMoved) InvalidateCanvas(editor);
    if (editor.drag == Drag::Handle) {
        // The handle drag previews in place; put the original keys back.
        if (Track *track = ActiveTrack(editor)) track->keys = editor.dragBase;
        InvalidateCanvas(editor);
    }
    editor.drag = Drag::None;
    editor.dragMoved = false;
    editor.dragSelection.clear();
    editor.dragFrameDelta = 0;
    editor.dragValueDelta = 0;
    editor.mmdDragHandle = -1;
    if (GetCapture() == editor.canvas) ReleaseCapture();
}

void SelectKeyRange(Editor &editor, const RECT &box, bool additive) {
    SourceState &state = CurrentState(editor);
    Track *track = ActiveTrack(editor);
    if (!track) return;
    if (!additive) state.selection.clear();
    for (std::size_t index = 0; index < track->keys.size(); ++index) {
        const POINT centre = KeyPoint(editor, index);
        if (centre.x < 0) continue;
        if (centre.x >= box.left && centre.x <= box.right && centre.y >= box.top && centre.y <= box.bottom)
            state.selection.insert(index);
    }
    UpdateKeyFields(editor);
    UpdateMmdPanel(editor);
    UpdateEnabledState(editor);
    InvalidateCanvas(editor);
    SetStatus(editor, std::wstring(T(editor.session.language, StrSelectedKeys)) +
                          FormatInteger(static_cast<long long>(state.selection.size())),
              false);
}

int TimeBarZone(Editor &editor, POINT point) {
    // 0 = pan, 1 = low (left/top) grip, 2 = high (right/bottom) grip, -1 = miss.
    const RECT &bar = editor.timeBar;
    if (!PtInRect(&bar,point)) return -1;
    RECT inner = bar;
    InflateRect(&inner, -Scale(2, editor.dpi), -Scale(3, editor.dpi));
    const BarRange range = TimeBarRange(editor);
    int first = 0, last = 0;
    BarPixels(editor, true, first, last);
    const int left = static_cast<int>(std::lround(MapToBar(editor.timeStart, range.fullLow, range.fullHigh, first, last)));
    const int right = static_cast<int>(std::lround(MapToBar(editor.timeEnd, range.fullLow, range.fullHigh, first, last)));
    const int grip = std::max(Scale(kGripSize, editor.dpi), Scale(4, editor.dpi));
    if (std::abs(point.x - left) <= grip) return 1;
    if (std::abs(point.x - right) <= grip) return 2;
    if (point.x > left && point.x < right) return 0;
    return -1;
}

int ValueBarZone(Editor &editor, POINT point) {
    const RECT &bar = editor.valueBar;
    if (!PtInRect(&bar,point)) return -1;
    RECT inner = bar;
    InflateRect(&inner, -Scale(3, editor.dpi), -Scale(2, editor.dpi));
    int top = 0, bottom = 0;
    ValueBarEdges(editor, top, bottom);
    const int grip = std::max(Scale(kGripSize, editor.dpi), Scale(4, editor.dpi));
    // Zone 1 is the low-value (bottom) edge and zone 2 the high-value (top)
    // edge, matching the time bar where zone 1 is the left edge.
    if (std::abs(point.y - bottom) <= grip) return 1;
    if (std::abs(point.y - top) <= grip) return 2;
    if (point.y > top && point.y < bottom) return 0;
    return -1;
}

bool CenterBarAtPoint(Editor& editor,POINT point) {
    CanvasMetrics(editor);
    const bool time=PtInRect(&editor.timeBar,point)!=FALSE;
    if(!time && !PtInRect(&editor.valueBar,point))return false;
    SetFocus(editor.canvas);
    CancelDrag(editor);
    const BarRange range=time?TimeBarRange(editor):ValueBarRange(editor);
    int first=0,last=0;BarPixels(editor,time,first,last);
    double low=editor.timeStart,high=editor.timeEnd;
    if(!time)ValueRange(editor,low,high);
    const double span=high-low;
    if(!(span>0) || !std::isfinite(span) || !std::isfinite(range.fullHigh-range.fullLow))return true;
    const double center=MapFromBar(time?point.x:point.y,
        time?range.fullLow:range.fullHigh,time?range.fullHigh:range.fullLow,first,last);
    // Clamp the translation, never either edge independently: span is invariant.
    const double next=std::clamp(center-span*.5,range.fullLow,std::max(range.fullLow,range.fullHigh-span));
    if(time){editor.timeStart=next;editor.timeEnd=next+span;}
    else if(editor.viewMode==0){editor.valueLow=next;editor.valueHigh=next+span;}
    else {editor.speedLow=next;editor.speedHigh=next+span;}
    auto& centered=time?editor.centeredTime:editor.centeredValue;
    centered={true,editor.viewMode,range.fullLow,range.fullHigh,next,next+span};
    editor.barFocus=time?1:2;editor.barZone=0;
    UpdateMouseReadout(editor,point);
    InvalidateCanvas(editor);
    return true;
}

void CanvasMouseDown(Editor &editor, UINT flags, POINT point) {
    SetFocus(editor.canvas);
    CanvasMetrics(editor);
    editor.hoverKey = -1;
    const bool space = (GetKeyState(VK_SPACE) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    if (editor.mmdDragHandle >= 0) editor.mmdDragHandle = -1;

    if(editor.session.seek&&point.x>=editor.plot.left&&point.x<=editor.plot.right&&
       ((point.y>=0&&point.y<editor.plot.top)||
        (point.y>=editor.plot.bottom-Scale(18,editor.dpi)&&point.y<editor.plot.bottom))){
        editor.drag=Drag::Seek;SetCapture(editor.canvas);SeekAtPoint(editor,point);return;
    }
    const int timeZone = TimeBarZone(editor, point);
    if (timeZone >= 0) {
        editor.barFocus = 1;
        editor.barZone = timeZone;
        editor.savedTimeStart = editor.timeStart;
        editor.savedTimeEnd = editor.timeEnd;
        editor.savedValueLow = editor.valueLow;
        editor.savedValueHigh = editor.valueHigh;
        const BarRange range = TimeBarRange(editor);
        editor.savedBarLow = range.fullLow;
        editor.savedBarHigh = range.fullHigh;
        BarPixels(editor, true, editor.savedBarFirst, editor.savedBarLast);
        editor.drag = timeZone == 0 ? Drag::TimeBarPan : timeZone == 1 ? Drag::TimeBarZoomLow : Drag::TimeBarZoomHigh;
        editor.dragOrigin = point;
        SetCapture(editor.canvas);
        return;
    }
    const int valueZone = ValueBarZone(editor, point);
    if (valueZone >= 0) {
        editor.barFocus = 2;
        editor.barZone = valueZone;
        editor.savedTimeStart = editor.timeStart;
        editor.savedTimeEnd = editor.timeEnd;
        ValueRange(editor, editor.savedValueLow, editor.savedValueHigh);
        const BarRange range = ValueBarRange(editor);
        editor.savedBarLow = range.fullLow;
        editor.savedBarHigh = range.fullHigh;
        BarPixels(editor, false, editor.savedBarFirst, editor.savedBarLast);
        editor.drag = valueZone == 0 ? Drag::ValueBarPan : valueZone == 1 ? Drag::ValueBarZoomLow : Drag::ValueBarZoomHigh;
        editor.dragOrigin = point;
        SetCapture(editor.canvas);
        return;
    }

    const bool inPlot = point.x >= editor.plot.left && point.x <= editor.plot.right && point.y >= editor.plot.top &&
                        point.y <= editor.plot.bottom;
    if (!inPlot) return;

    // Middle button and Space+left take the navigation branch even when the
    // pointer is over a key, so hovering a key never hijacks a pan.
    editor.barFocus = 0;
    if (editor.tool == 1 || space) {
        editor.savedTimeStart = editor.timeStart;
        editor.savedTimeEnd = editor.timeEnd;
        editor.savedValueLow = editor.valueLow;
        editor.savedValueHigh = editor.valueHigh;
        editor.drag = Drag::Pan;
        editor.dragOrigin = point;
        SetCapture(editor.canvas);
        return;
    }

    SourceState &state = CurrentState(editor);
    Track *track = ActiveTrack(editor);
    if (!track) return;
    const double hitRadius = kHandleHitRadius * editor.dpi / 96.0 + 2.0;

    // Bezier handles of the selected keys come first: they can sit on top of
    // another key.  Alt-click straightens that side; a plain press drags it.
    bool handleOut = false;
    const int handleHit = HandleAtPoint(editor, point, hitRadius, handleOut);
    if (handleHit >= 0) {
        if (alt) {
            EditKeyShape(editor, static_cast<std::size_t>(handleHit), [&](Track &working) {
                animation_curve::StraightenSide(working, static_cast<std::size_t>(handleHit), handleOut);
            });
            return;
        }
        editor.drag = Drag::Handle;
        editor.dragBase = track->keys;
        editor.handleKey = static_cast<std::size_t>(handleHit);
        editor.handleOutSide = handleOut;
        editor.dragOrigin = point;
        editor.dragMoved = false;
        SetCapture(editor.canvas);
        return;
    }

    const int hit = KeyAtPoint(editor, point, hitRadius);
    // Alt-click on a key: a linear key gets two Bezier handles, a curved key
    // becomes linear.
    if (hit >= 0 && alt) {
        const std::size_t key = static_cast<std::size_t>(hit);
        const bool linear = animation_curve::KeyIsLinear(*track, key);
        EditKeyShape(editor, key, [&](Track &working) {
            if (linear) animation_curve::MakeKeyBezier(working, key);
            else animation_curve::MakeKeyLinear(working, key);
        });
        return;
    }
    const bool additive = (flags & MK_CONTROL) != 0 || (flags & MK_SHIFT) != 0;
    if (hit >= 0) {
        if (!state.selection.count(static_cast<std::size_t>(hit))) {
            if (!additive) state.selection.clear();
            state.selection.insert(static_cast<std::size_t>(hit));
        } else if (additive) {
            state.selection.erase(static_cast<std::size_t>(hit));
            PublishKeySelection(editor);
            UpdateKeyFields(editor);
            UpdateMmdPanel(editor);
            UpdateEnabledState(editor);
            InvalidateCanvas(editor);
            return;
        }
        SeekTimeline(editor,track->keys[static_cast<std::size_t>(hit)].frame);
        editor.drag = Drag::Keys;
        editor.dragBase = track->keys;
        editor.dragSelection = state.selection;
        editor.dragOrigin = point;
        editor.dragMoved = false;
        editor.dragFrameDelta = 0;
        editor.dragValueDelta = 0;
        SetCapture(editor.canvas);
        UpdateKeyFields(editor);
        UpdateMmdPanel(editor);
        UpdateEnabledState(editor);
        InvalidateCanvas(editor);
        SetStatus(editor, std::wstring(T(editor.session.language, StrSelectedKeys)) +
                              FormatInteger(static_cast<long long>(state.selection.size())),
                  false);
        return;
    }
    editor.drag = Drag::Rubber;
    editor.rubberAdd = additive;
    editor.rubber.left = editor.rubber.right = point.x;
    editor.rubber.top = editor.rubber.bottom = point.y;
    editor.dragOrigin = point;
    SetCapture(editor.canvas);
}

void CanvasMouseMove(Editor &editor, POINT point) {
    if(editor.drag==Drag::Seek){SeekAtPoint(editor,point);return;}
    if (editor.drag == Drag::None) {
        const int hit = KeyAtPoint(editor, point, kHandleHitRadius * editor.dpi / 96.0 + 2.0);
        if (hit != editor.hoverKey) {
            editor.hoverKey = hit;
            InvalidateCanvas(editor);
        }
        return;
    }
    if (editor.drag == Drag::Pan) {
        double anchorFrame = 0, anchorValue = 0;
        XToTime(editor, editor.dragOrigin.x, anchorFrame);
        YToValue(editor, editor.dragOrigin.y, anchorValue);
        double frame = 0, value = 0;
        XToTime(editor, point.x, frame);
        YToValue(editor, point.y, value);
        PanTime(editor, anchorFrame - frame);
        PanValue(editor, value - anchorValue);
        InvalidateCanvas(editor);
        return;
    }
    if (editor.drag == Drag::TimeBarPan || editor.drag == Drag::TimeBarZoomLow || editor.drag == Drag::TimeBarZoomHigh) {
        const BarRange range = TimeBarRange(editor);
        int first = 0, last = 0;
        BarPixels(editor, true, first, last);
        const double moved = MapFromBar(point.x, range.fullLow, range.fullHigh, first, last);
        const double anchor = MapFromBar(editor.dragOrigin.x, range.fullLow, range.fullHigh, first, last);
        const double delta = moved - anchor;
        if (editor.drag == Drag::TimeBarPan) {
            const double bounded = std::clamp(delta, range.fullLow - editor.savedTimeStart,
                                              range.fullHigh - editor.savedTimeEnd);
            editor.timeStart = editor.savedTimeStart + bounded;
            editor.timeEnd = editor.savedTimeEnd + bounded;
        } else {
            const double fixed = editor.drag == Drag::TimeBarZoomLow ? editor.savedTimeEnd : editor.savedTimeStart;
            const double moving = editor.drag == Drag::TimeBarZoomLow
                ? std::clamp(editor.savedTimeStart + delta, range.fullLow, fixed - kMinimumTimeSpan)
                : std::clamp(editor.savedTimeEnd + delta, fixed + kMinimumTimeSpan, range.fullHigh);
            if (editor.drag == Drag::TimeBarZoomLow) {
                editor.timeEnd = fixed;
                editor.timeStart = moving;
            } else {
                editor.timeStart = fixed;
                editor.timeEnd = moving;
            }
            if (editor.timeEnd - editor.timeStart < kMinimumTimeSpan) {
                if (editor.drag == Drag::TimeBarZoomLow) editor.timeStart = editor.timeEnd - kMinimumTimeSpan;
                else editor.timeEnd = editor.timeStart + kMinimumTimeSpan;
            }
        }
        InvalidateCanvas(editor);
        return;
    }
    if (editor.drag == Drag::ValueBarPan || editor.drag == Drag::ValueBarZoomLow || editor.drag == Drag::ValueBarZoomHigh) {
        const BarRange range = ValueBarRange(editor);
        int first = 0, last = 0;
        BarPixels(editor, false, first, last);
        const double moved = MapFromBar(point.y, range.fullHigh, range.fullLow, first, last);
        const double anchor = MapFromBar(editor.dragOrigin.y, range.fullHigh, range.fullLow, first, last);
        const double delta = moved - anchor;
        const double savedLow = editor.savedValueLow;
        const double savedHigh = editor.savedValueHigh;
        if (editor.drag == Drag::ValueBarPan) {
            const double bounded = std::clamp(delta, range.fullLow - savedLow, range.fullHigh - savedHigh);
            if (editor.viewMode == 0) {
                editor.valueLow = savedLow + bounded;
                editor.valueHigh = savedHigh + bounded;
            } else {
                editor.speedLow = savedLow + bounded;
                editor.speedHigh = savedHigh + bounded;
            }
        } else {
            const double fixed = editor.drag == Drag::ValueBarZoomLow ? savedHigh : savedLow;
            const double moving = editor.drag == Drag::ValueBarZoomLow
                ? std::clamp(savedLow + delta, range.fullLow, fixed - kMinimumValueSpan)
                : std::clamp(savedHigh + delta, fixed + kMinimumValueSpan, range.fullHigh);
            if (editor.drag == Drag::ValueBarZoomLow) {
                if (editor.viewMode == 0) {
                    editor.valueHigh = fixed;
                    editor.valueLow = moving;
                } else {
                    editor.speedHigh = fixed;
                    editor.speedLow = moving;
                }
            } else {
                if (editor.viewMode == 0) {
                    editor.valueLow = fixed;
                    editor.valueHigh = moving;
                } else {
                    editor.speedLow = fixed;
                    editor.speedHigh = moving;
                }
            }
            double currentLow = 0, currentHigh = 0;
            ValueRange(editor, currentLow, currentHigh);
            if (currentHigh - currentLow < kMinimumValueSpan) {
                if (editor.viewMode == 0) {
                    if (editor.drag == Drag::ValueBarZoomLow) editor.valueLow = editor.valueHigh - kMinimumValueSpan;
                    else editor.valueHigh = editor.valueLow + kMinimumValueSpan;
                } else {
                    if (editor.drag == Drag::ValueBarZoomLow) editor.speedLow = editor.speedHigh - kMinimumValueSpan;
                    else editor.speedHigh = editor.speedLow + kMinimumValueSpan;
                }
            }
        }
        InvalidateCanvas(editor);
        return;
    }
    if (editor.drag == Drag::Keys) {
        double startFrame = 0, currentFrame = 0;
        XToTime(editor, editor.dragOrigin.x, startFrame);
        XToTime(editor, point.x, currentFrame);
        long long deltaFrame = static_cast<long long>(std::llround(currentFrame - startFrame));
        double deltaValue = 0;
        if (editor.viewMode == 0) {
            double startValue = 0, currentValue = 0;
            YToValue(editor, editor.dragOrigin.y, startValue);
            YToValue(editor, point.y, currentValue);
            deltaValue = currentValue - startValue;
        }
        long long minimumDelta = -static_cast<long long>(kMaximumFrame);
        long long maximumDelta = static_cast<long long>(kMaximumFrame);
        for (const std::size_t index : editor.dragSelection) {
            if (index >= editor.dragBase.size()) continue;
            const long long frame = editor.dragBase[index].frame;
            minimumDelta = std::max(minimumDelta, -frame);
            maximumDelta = std::min(maximumDelta, static_cast<long long>(kMaximumFrame) - frame);
        }
        deltaFrame = std::clamp(deltaFrame, minimumDelta, maximumDelta);
        if (deltaFrame != editor.dragFrameDelta || deltaValue != editor.dragValueDelta) {
            editor.dragFrameDelta = deltaFrame;
            editor.dragValueDelta = deltaValue;
            editor.dragMoved = deltaFrame != 0 || deltaValue != 0.0;
            InvalidateCanvas(editor);
        }
        return;
    }
    if (editor.drag == Drag::Rubber) {
        editor.rubber.left = std::min(editor.dragOrigin.x, point.x);
        editor.rubber.right = std::max(editor.dragOrigin.x, point.x);
        editor.rubber.top = std::min(editor.dragOrigin.y, point.y);
        editor.rubber.bottom = std::max(editor.dragOrigin.y, point.y);
        InvalidateCanvas(editor);
        return;
    }
    if (editor.drag == Drag::Handle) {
        Track *track = ActiveTrack(editor);
        double frame = 0, value = 0;
        if (!track || !XToTime(editor, point.x, frame) || !YToValue(editor, point.y, value)) return;
        // Preview from the original keys each move, so clamping never drifts.
        Track preview = *track;
        preview.keys = editor.dragBase;
        if (animation_curve::MoveHandle(preview, editor.handleKey, editor.handleOutSide, frame, value)) {
            track->keys = std::move(preview.keys);
            editor.dragMoved = true;
            InvalidateCanvas(editor);
        }
        return;
    }
}

void CanvasMouseUp(Editor &editor, POINT point) {
    const Drag drag = editor.drag;
    if(drag==Drag::Seek)SeekAtPoint(editor,point);
    if (drag == Drag::Rubber) {
        const bool trivial = std::abs(point.x - editor.dragOrigin.x) < 4 && std::abs(point.y - editor.dragOrigin.y) < 4;
        if (trivial) {
            if (!editor.rubberAdd) {
                CurrentState(editor).selection.clear();
                UpdateKeyFields(editor);
                UpdateMmdPanel(editor);
                UpdateEnabledState(editor);
                InvalidateCanvas(editor);
                SetStatus(editor, T(editor.session.language, StrCleared), false);
            }
        } else {
            SelectKeyRange(editor, editor.rubber, editor.rubberAdd);
        }
    } else if (drag == Drag::Keys) {
        if (editor.dragMoved) CommitKeyDrag(editor);
        else InvalidateCanvas(editor);
    } else if (drag == Drag::Handle) {
        Track *track = ActiveTrack(editor);
        if (track && editor.dragMoved) {
            // Commit the previewed keys as one undo step: restore the original
            // first so the undo snapshot holds the pre-drag curve.
            std::vector<Key> moved = std::move(track->keys);
            track->keys = editor.dragBase;
            std::vector<Channel> next = CopyChannels(editor);
            next[CurrentState(editor).active].track.keys = std::move(moved);
            if (CommitChannels(editor, std::move(next), T(editor.session.language, StrReady))) {
                UpdateKeyFields(editor);
                UpdateMmdPanel(editor);
            }
        }
    }
    editor.drag = Drag::None;
    editor.dragMoved = false;
    editor.dragSelection.clear();
    editor.dragFrameDelta = 0;
    editor.dragValueDelta = 0;
    if (GetCapture() == editor.canvas) ReleaseCapture();
    if(drag==Drag::Keys||drag==Drag::Rubber)PublishKeySelection(editor);
    InvalidateCanvas(editor);
}

void CanvasWheel(Editor &editor, WPARAM wparam, POINT point) {
    const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
    if (delta == 0) return;
    CanvasMetrics(editor);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const double steps = static_cast<double>(delta) / WHEEL_DELTA;
    double anchorFrame = 0, anchorValue = 0;
    XToTime(editor, point.x, anchorFrame);
    YToValue(editor, point.y, anchorValue);
    const double zoom = std::pow(0.85, steps);
    if (alt && control) {
        ZoomTime(editor, anchorFrame, zoom);
        ZoomValue(editor, anchorValue, zoom);
    } else if (alt) {
        ZoomTime(editor, anchorFrame, zoom);
    } else if (control) {
        ZoomValue(editor, anchorValue, zoom);
    } else if (shift) {
        PanTime(editor, -steps * (editor.timeEnd - editor.timeStart) * 0.1);
    } else {
        PanValue(editor, steps * (editor.viewMode == 0 ? editor.valueHigh - editor.valueLow
                                                       : editor.speedHigh - editor.speedLow) *
                              0.1);
    }
    InvalidateCanvas(editor);
}

// ---------------------------------------------------------------------------
// Range-bar keyboard control.  Clicking the centre or an end of either bar
// focuses that segment; the arrow keys then pan or drag that edge by one frame
// (time bar) or 2% of the current span (value bar), ten times further with
// Shift.  A span never shrinks below the minimum the mouse gestures use.
// ---------------------------------------------------------------------------
void BarArrowKey(Editor &editor, WPARAM key, bool shift) {
    if (editor.barFocus == 0) return;
    const double factor = shift ? 10.0 : 1.0;
    if (editor.barFocus == 1) {
        if (key != VK_LEFT && key != VK_RIGHT) return;
        const double direction = key == VK_LEFT ? -1.0 : 1.0;
        const double step = factor * direction;
        if (editor.barZone == 0) {
            PanTime(editor, step);
        } else if (editor.barZone == 1) {
            editor.timeStart = std::max(0.0, editor.timeStart + step);
            if (editor.timeEnd - editor.timeStart < kMinimumTimeSpan) editor.timeStart = editor.timeEnd - kMinimumTimeSpan;
        } else {
            editor.timeEnd += step;
            if (editor.timeEnd - editor.timeStart < kMinimumTimeSpan) editor.timeEnd = editor.timeStart + kMinimumTimeSpan;
        }
    } else {
        if (key != VK_UP && key != VK_DOWN) return;
        const double direction = key == VK_UP ? 1.0 : -1.0;
        double low = 0, high = 0;
        ValueRange(editor, low, high);
        const double step = factor * direction * (high - low) * 0.02;
        if (editor.barZone == 0) {
            PanValue(editor, step);
        } else if (editor.barZone == 1) {
            if (editor.viewMode == 0) {
                editor.valueLow += step;
                if (editor.valueHigh - editor.valueLow < kMinimumValueSpan)
                    editor.valueLow = editor.valueHigh - kMinimumValueSpan;
            } else {
                editor.speedLow += step;
                if (editor.speedHigh - editor.speedLow < kMinimumValueSpan)
                    editor.speedLow = editor.speedHigh - kMinimumValueSpan;
            }
        } else {
            if (editor.viewMode == 0) {
                editor.valueHigh += step;
                if (editor.valueHigh - editor.valueLow < kMinimumValueSpan)
                    editor.valueHigh = editor.valueLow + kMinimumValueSpan;
            } else {
                editor.speedHigh += step;
                if (editor.speedHigh - editor.speedLow < kMinimumValueSpan)
                    editor.speedHigh = editor.speedLow + kMinimumValueSpan;
            }
        }
    }
    InvalidateCanvas(editor);
}

// ---------------------------------------------------------------------------
// Tab navigation.  The host message loop never calls IsDialogMessage, so the
// controls are subclassed locally: a Tab press moves the focus itself instead
// of being forwarded to the control.  The order is the reading order of the
// window rather than the z-order the controls happen to have.
// ---------------------------------------------------------------------------
const int kTabOrder[] = {
    kIdSourceCombo, kIdReadMotion, kIdWriteMotion,
    kIdPreviousKey,kIdPreviousFrame,kIdNextFrame,kIdNextKey,kIdPlay,kIdTimelineFrame,kIdSeekFrame,
    kIdViewCombo, kIdToolCombo, kIdFitAll, kIdFitRange,
    kIdCompareSameGroup, kIdCompareFree, kIdCompareAllGroups, kIdCompareFilter,
    kIdChannelList, kIdChannelInvert, kIdChannelAll, kIdCanvas, kIdMmdPanel,
    kIdMmdX1,kIdMmdY1,kIdMmdX2,kIdMmdY2,
    kIdFrameEdit, kIdValueEdit, kIdInterpolationCombo, kIdTangentCombo,
    1090,
    kIdInSlopeEdit, kIdOutSlopeEdit, kIdInWeightEdit, kIdOutWeightEdit,
    kIdApplyKey, kIdAddKey, kIdDeleteKey, kIdUndo, kIdRedo,
    kIdBakeFirstEdit, kIdBakeLastEdit, kIdBakeStepEdit, kIdBakeRun,
};

bool IsTabStop(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    if (!control) return false;
    if (IsWindowVisible(control) == FALSE) return false;
    if (IsWindowEnabled(control) == FALSE) return false;
    return (GetWindowLongPtrW(control, GWL_STYLE) & WS_TABSTOP) != 0;
}

void MoveFocusByTab(Editor &editor, HWND current, bool backwards) {
    if (!editor.window) return;
    const int count = static_cast<int>(sizeof(kTabOrder) / sizeof(kTabOrder[0]));
    int start = 0;
    for (int index = 0; index < count; ++index) {
        if (GetDlgItem(editor.window, kTabOrder[index]) == current) {
            start = index;
            break;
        }
    }
    const int direction = backwards ? -1 : 1;
    for (int step = 1; step <= count; ++step) {
        const int index = ((start + direction * step) % count + count) % count;
        if (!IsTabStop(editor.window, kTabOrder[index])) continue;
        HWND target = GetDlgItem(editor.window, kTabOrder[index]);
        SetFocus(target);
        // Dialog behaviour: a text field is selected so typing replaces it.
        wchar_t className[32] = {};
        GetClassNameW(target, className, 32);
        if (wcscmp(className, L"Edit") == 0) SendMessageW(target, EM_SETSEL, 0, -1);
        return;
    }
}

std::mutex g_subclassMutex;
std::map<HWND, WNDPROC> g_subclassed;

WNDPROC PreviousSubclassProc(HWND hwnd) {
    std::lock_guard<std::mutex> guard(g_subclassMutex);
    const auto found = g_subclassed.find(hwnd);
    return found == g_subclassed.end() ? nullptr : found->second;
}

Editor *EditorForChild(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    return parent ? reinterpret_cast<Editor *>(GetWindowLongPtrW(parent, GWLP_USERDATA)) : nullptr;
}

void InsertAtPlayhead(Editor& editor);
void RegisterFromShortcut(Editor& editor,LPARAM keyData){
    if((keyData&(LPARAM(1)<<30))||(keyData&(LPARAM(1)<<29))||
       (GetKeyState(VK_MENU)&0x8000)||editor.drag!=Drag::None||editor.mmdDragHandle>=0)return;
    if(IsWindowEnabled(GetDlgItem(editor.window,kIdAddKey)))InsertAtPlayhead(editor);
}
void ApplyFromRegisterShortcut(Editor& editor,LPARAM keyData) {
    // MMS PumpEnterRegisterFrame uses Enter, excludes Alt+Enter and held-key
    // repeats. Keep this local to the curve editor's existing Apply operation.
    if((keyData&(LPARAM(1)<<30)) || (keyData&(LPARAM(1)<<29)) ||
       (GetKeyState(VK_MENU)&0x8000) || editor.drag!=Drag::None || editor.mmdDragHandle>=0)return;
    if(IsWindowEnabled(GetDlgItem(editor.window,kIdApplyKey)))
        SendMessageW(editor.window,WM_COMMAND,MAKEWPARAM(kIdApplyKey,BN_CLICKED),0);
}

void DeleteFromShortcut(Editor& editor,LPARAM keyData) {
    if((keyData&(LPARAM(1)<<30)) || (keyData&(LPARAM(1)<<29)) ||
       (GetKeyState(VK_MENU)&0x8000) || editor.drag!=Drag::None || editor.mmdDragHandle>=0)return;
    if(IsWindowEnabled(GetDlgItem(editor.window,kIdDeleteKey)))
        SendMessageW(editor.window,WM_COMMAND,MAKEWPARAM(kIdDeleteKey,BN_CLICKED),0);
}

bool HandleEditorShortcut(Editor& editor,HWND target,WPARAM key,LPARAM keyData){
    wchar_t className[64]{};GetClassNameW(target,className,64);
    if(_wcsicmp(className,L"EDIT")==0)return false;
    const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0;
    const bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
    int command=0;
    if(control&&key=='Z')command=shift?kIdRedo:kIdUndo;
    else if(control&&key=='Y')command=kIdRedo;
    else if(!control&&!shift&&key==VK_INSERT)command=kIdAddKey;
    else return false;
    if((keyData&(LPARAM(1)<<30))||(keyData&(LPARAM(1)<<29))||
       (GetKeyState(VK_MENU)&0x8000)||editor.drag!=Drag::None||editor.mmdDragHandle>=0)return true;
    if(IsWindowEnabled(GetDlgItem(editor.window,command))){
        if(command==kIdAddKey)InsertAtPlayhead(editor);
        else SendMessageW(editor.window,WM_COMMAND,MAKEWPARAM(command,BN_CLICKED),0);
    }
    return true;
}

// Shared by every subclassed control: Tab keeps moving the focus, hovering
// explains the control in the hint line, and destruction drops the bookkeeping
// entry.  Returns true when the message is fully answered by `result`.
bool HandleChildMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, LRESULT &result) {
    const int id=GetDlgCtrlID(hwnd);
    if(message==WM_KEYDOWN)
        if(auto* editor=EditorForChild(hwnd);editor&&HandleEditorShortcut(*editor,hwnd,wparam,lparam)){result=0;return true;}
    if(message==WM_KEYDOWN && wparam==VK_DELETE){
        wchar_t className[64]{};GetClassNameW(hwnd,className,64);
        if(_wcsicmp(className,L"EDIT")!=0){
            if(auto* editor=EditorForChild(hwnd))DeleteFromShortcut(*editor,lparam);
            result=0;return true;
        }
    }
    const bool coordinate=id>=kIdMmdX1&&id<=kIdMmdY2;
    if(coordinate && message==WM_KEYDOWN && wparam==VK_RETURN){
        if(!(lparam&(LPARAM(1)<<30)) && !(lparam&(LPARAM(1)<<29)) && !(GetKeyState(VK_MENU)&0x8000))
            if(auto* editor=EditorForChild(hwnd))ApplyMmdCoordinateFields(*editor);
        result=0;return true;
    }
    if(coordinate && message==WM_CHAR && wparam==L'\r'){result=0;return true;}
    // The mode strips handle Enter themselves. Routing them through numeric
    // Apply would submit unrelated text fields instead of the focused choice.
    const bool parameter=id==kIdFrameEdit||id==kIdValueEdit||
        id==kIdInSlopeEdit||id==kIdOutSlopeEdit||id==kIdInWeightEdit||id==kIdOutWeightEdit;
    if(parameter && message==WM_KEYDOWN && wparam==VK_RETURN){
        if(auto* editor=EditorForChild(hwnd))ApplyFromRegisterShortcut(*editor,lparam);
        result=0;return true;
    }
    if(parameter && message==WM_CHAR && wparam==L'\r'){result=0;return true;}
    if(message==WM_KEYDOWN&&wparam==VK_RETURN&&GetDlgCtrlID(hwnd)==kIdTimelineFrame){
        SendMessageW(GetParent(hwnd),WM_COMMAND,MAKEWPARAM(kIdSeekFrame,BN_CLICKED),0);
        result=0;return true;
    }
    if (message == WM_KEYDOWN && wparam == VK_TAB) {
        if (Editor *editor = EditorForChild(hwnd)) {
            MoveFocusByTab(*editor, hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            result = 0;
            return true;
        }
    }
    if (message == WM_GETDLGCODE && lparam) {
        const auto *msg = reinterpret_cast<const MSG *>(lparam);
        if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB) {
            result = DLGC_WANTTAB;
            return true;
        }
    }
    if (message == WM_MOUSEMOVE) {
        Editor *editor = EditorForChild(hwnd);
        const int hint = editor ? HintForControl(GetDlgCtrlID(hwnd)) : -1;
        if (editor && hint >= 0) {
            TrackMouseLeave(hwnd);
            SetHoverHint(*editor, hint);
        }
    } else if (message == WM_MOUSELEAVE) {
        if (Editor *editor = EditorForChild(hwnd)) SetHoverHint(*editor, -1);
    }
    return false;
}

// The row under a client point, or -1 when the point is past the last row.
int TreeChannel(HWND tree, HTREEITEM item) {
    if(!item)return -1;
    TVITEMW value{};value.mask=TVIF_PARAM;value.hItem=item;
    if(!SendMessageW(tree,TVM_GETITEMW,0,reinterpret_cast<LPARAM>(&value))||value.lParam<=0)return -1;
    return static_cast<int>(value.lParam-1);
}
HTREEITEM ChannelTreeHit(HWND tree,POINT point,UINT&flags){
    TVHITTESTINFO hit{};hit.pt=point;
    TreeView_HitTest(tree,&hit);flags=hit.flags;return hit.hItem;
}
void ToggleCollection(Editor&editor,HTREEITEM item){
    const auto found=editor.collectionItems.find(item);
    if(found==editor.collectionItems.end())return;
    const std::string path=found->second;
    if(!EnterFreeForManualDisplay(editor))return;
    item=nullptr;
    for(const auto&entry:editor.collectionItems)if(entry.second==path){item=entry.first;break;}
    if(!item)return;
    HWND tree=GetDlgItem(editor.window,kIdChannelList);
    const bool show=!TreeView_GetCheckState(tree,item);
    SourceState&state=CurrentState(editor);
    for(std::size_t index=0;index<editor.channelItems.size();++index){
        HTREEITEM parent=editor.channelItems[index];
        while(parent&&parent!=item)parent=TreeView_GetParent(tree,parent);
        if(parent)state.channels[index].selected=show;
    }
    SyncVisibilityToList(editor);UpdateCompareUi(editor);InvalidateCanvas(editor);
}

// One click shows or hides exactly one curve.  The active editing curve, the key
// selection and every other row stay as they were.  In an automatic mode the
// click first moves the display to the free segment, which carries the set that
// is already on screen, so the click only ever changes the curve it names.
void ToggleChannelVisible(Editor &editor, int item) {
    SourceState &state = CurrentState(editor);
    if (item < 0 || static_cast<std::size_t>(item) >= state.channels.size()) return;
    Channel &channel = state.channels[static_cast<std::size_t>(item)];
    // A curve without keys has nothing to draw, so showing it would promise a
    // picture that never appears.  Only a source with the comparison display is
    // held to this: for a plain source the tick is the bake scope.
    if (ComparisonSupported(editor) && state.compareMode != kCompareOff &&
        !curve_comparison::IsRegistered(channel.track) && !channel.selected) {
        SetStatus(editor, T(editor.session.language, StrCompareEmptyCurve), true);
        return;
    }
    if (!EnterFreeForManualDisplay(editor)) return;
    channel.selected = !channel.selected;
    // Re-syncing the ticks can move the caret of a list box that does not hold
    // the focus, so it is saved and put back: showing or hiding a curve must
    // never move the caret away from the row the user is on.
    HWND list = GetDlgItem(editor.window, kIdChannelList);
    const HTREEITEM caret = list ? TreeView_GetSelection(list) : nullptr;
    SyncVisibilityToList(editor);
    if (list && caret) TreeView_SelectItem(list, caret);
    UpdateCompareUi(editor);
    InvalidateCanvas(editor);
    SetStatus(editor, T(editor.session.language, channel.selected ? StrChannelShown : StrChannelHidden) +
                          ChannelRowText(channel, false),
              false);
}

// Clicking a leaf or pressing Enter activates and shows it. Other visibility
// flags stay intact; manual activation switches automatic comparison to free.
void ActivateChannel(Editor &editor, int item) {
    SourceState &state = CurrentState(editor);
    if (item < 0 || static_cast<std::size_t>(item) >= state.channels.size()) return;
    if (!EnterFreeForManualDisplay(editor)) return;
    const std::size_t index = static_cast<std::size_t>(item);
    state.channels[index].selected = true;
    if (state.active != index) {
        state.active = index;
        state.selection.clear();
    }
    PullKeySelection(editor);
    PopulateChannels(editor);
    UpdateKeyFields(editor);
    UpdateMmdPanel(editor);
    UpdateBakeFields(editor);
    UpdateEnabledState(editor);
    FitAll(editor);
    SetStatus(editor, T(editor.session.language, StrActiveCurve) + ChannelRowText(state.channels[index], false), false);
}

// Keyboard parity with the mouse: the arrow keys only move the caret, Space
// shows or hides the caret row and Enter activates it.
bool ChannelListKey(Editor &editor, HWND tree, WPARAM key) {
    HTREEITEM item=TreeView_GetSelection(tree);
    const int channel=TreeChannel(tree,item);
    if(key==VK_SPACE){
        if(channel>=0)ToggleChannelVisible(editor,channel);else ToggleCollection(editor,item);
        return true;
    }
    if(key==VK_RETURN){
        if(channel>=0)ActivateChannel(editor,channel);
        else if(item)TreeView_Expand(tree,item,TVE_TOGGLE);
        return true;
    }
    return false;
}
LRESULT CALLBACK ChannelListSubclassProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    WNDPROC previous=PreviousSubclassProc(hwnd);
    LRESULT shared=0;
    if(HandleChildMessage(hwnd,message,wparam,lparam,shared))return shared;
    Editor*editor=EditorForChild(hwnd);
    if(editor&&!editor->rebuildingChannels){
        if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK){
            UINT flags=0;HTREEITEM item=ChannelTreeHit(hwnd,POINT{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)},flags);
            if(item&&(flags&TVHT_ONITEMSTATEICON)){
                SetFocus(hwnd);TreeView_SelectItem(hwnd,item);
                if(message==WM_LBUTTONDOWN){
                    const int channel=TreeChannel(hwnd,item);
                    if(channel>=0)ToggleChannelVisible(*editor,channel);else ToggleCollection(*editor,item);
                }
                return 0;
            }
            if(item&&(flags&(TVHT_ONITEMLABEL|TVHT_ONITEMICON))){
                const int channel=TreeChannel(hwnd,item);
                if(channel>=0){SetFocus(hwnd);TreeView_SelectItem(hwnd,item);ActivateChannel(*editor,channel);return 0;}
            }
        }
        if(message==WM_KEYDOWN&&ChannelListKey(*editor,hwnd,wparam))return 0;
    }
    if(message==WM_NCDESTROY){std::lock_guard<std::mutex>guard(g_subclassMutex);g_subclassed.erase(hwnd);}
    if(message==WM_DESTROY){
        HIMAGELIST states=TreeView_GetImageList(hwnd,TVSIL_STATE);
        TreeView_SetImageList(hwnd,nullptr,TVSIL_STATE);
        if(states)ImageList_Destroy(states);
    }
    return previous?CallWindowProcW(previous,hwnd,message,wparam,lparam):DefWindowProcW(hwnd,message,wparam,lparam);
}

LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    WNDPROC previous = PreviousSubclassProc(hwnd);
    LRESULT shared = 0;
    if (HandleChildMessage(hwnd, message, wparam, lparam, shared)) return shared;
    if (message == WM_NCDESTROY) {
        std::lock_guard<std::mutex> guard(g_subclassMutex);
        g_subclassed.erase(hwnd);
    }
    if (!previous) return DefWindowProcW(hwnd, message, wparam, lparam);
    return CallWindowProcW(previous, hwnd, message, wparam, lparam);
}

void SubclassControl(Editor &editor, int id, WNDPROC procedure) {
    HWND control = GetDlgItem(editor.window, id);
    if (!control) return;
    std::lock_guard<std::mutex> guard(g_subclassMutex);
    if (g_subclassed.count(control)) return;
    WNDPROC previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(control, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(procedure)));
    if (previous) g_subclassed[control] = previous;
}

void InstallTabNavigation(Editor &editor) {
    for (const int id : kTabOrder) {
        SubclassControl(editor, id, id == kIdChannelList ? &ChannelListSubclassProc : &TabSubclassProc);
    }
}

// ---------------------------------------------------------------------------
// MMD panel interaction.
// ---------------------------------------------------------------------------
RECT MmdSquare(Editor &editor) {
    RECT client = {};
    GetClientRect(editor.mmd, &client);
    const int margin = Scale(6, editor.dpi);
    RECT square = {margin, margin, client.right - margin, client.bottom - margin};
    const int side = std::min(square.right - square.left, square.bottom - square.top);
    square.right = square.left + side;
    square.bottom = square.top + side;
    return square;
}

POINT MmdHandlePoint(const Editor &editor, int index) {
    Editor &mutableEditor = const_cast<Editor &>(editor);
    const RECT square = MmdSquare(mutableEditor);
    const int side = square.right - square.left;
    const double u = editor.mmdControls[static_cast<std::size_t>(index) * 2];
    const double v = editor.mmdControls[static_cast<std::size_t>(index) * 2 + 1];
    POINT point = {};
    point.x = square.left + static_cast<LONG>(std::lround(u / 127.0 * side));
    point.y = square.bottom - static_cast<LONG>(std::lround(v / 127.0 * side));
    return point;
}

void MmdMouseDown(Editor &editor, POINT point) {
    SetFocus(editor.mmd);
    if (!editor.mmdEnabled) return;
    const int radius = Scale(7, editor.dpi);
    for (int index = 0; index < 2; ++index) {
        const POINT handle = MmdHandlePoint(editor, index);
        if (std::abs(point.x - handle.x) <= radius && std::abs(point.y - handle.y) <= radius) {
            editor.mmdDragHandle = index;
            SetCapture(editor.mmd);
            return;
        }
    }
}

void MmdMouseMove(Editor &editor, POINT point) {
    if (editor.mmdDragHandle < 0 || !editor.mmdEnabled) return;
    const RECT square = MmdSquare(editor);
    const int side = std::max(1, static_cast<int>(square.right - square.left));
    const double u = std::max(0.0, std::min(127.0, (point.x - square.left) * 127.0 / side));
    const double v = std::max(0.0, std::min(127.0, (square.bottom - point.y) * 127.0 / side));
    // Only the dragged axis pair moves, so the handles stay independent.
    if (editor.mmdDragHandle == 0) {
        editor.mmdControls[0] = std::floor(u + 0.5);
        editor.mmdControls[1] = std::floor(v + 0.5);
    } else {
        editor.mmdControls[2] = std::floor(u + 0.5);
        editor.mmdControls[3] = std::floor(v + 0.5);
    }
    SyncMmdCoordinateFields(editor);
    if (editor.mmd) InvalidateRect(editor.mmd, nullptr, FALSE);
}

void ApplyMmdCoordinateFields(Editor &editor) {
    SourceState &state=CurrentState(editor);
    if(!editor.mmdEnabled || !state.valid || !CurrentSource(editor).apply ||
       editor.drag!=Drag::None || editor.mmdDragHandle>=0)return;
    const std::size_t endKey=ActiveKeyIndex(editor);
    const Track* track=ActiveTrack(editor);
    if(!track || endKey==0 || endKey>=track->keys.size())return;
    std::array<double,4> controls{};
    const int ids[]={kIdMmdX1,kIdMmdY1,kIdMmdX2,kIdMmdY2};
    for(int i=0;i<4;++i){
        long long value=0;
        if(!ParseIntegerText(ControlText(editor.window,ids[i]),value) || value<0 || value>127 ||
           (i%2==0 && (value<1 || value>126))){
            SetStatus(editor,L"控制点坐标需为整数：横坐标 1–126，纵坐标 0–127。",true);
            SetFocus(GetDlgItem(editor.window,ids[i]));
            SendMessageW(GetDlgItem(editor.window,ids[i]),EM_SETSEL,0,-1);
            return;
        }
        controls[i]=double(value);
    }
    if(controls[0]>=controls[2]){
        SetStatus(editor,L"起始点横坐标必须小于终止点横坐标，请调整后按 Enter。",true);
        return;
    }
    std::vector<Channel> next=CopyChannels(editor);
    std::string error;
    if(!SetChannelControls(next[state.active],endKey,controls,error)){
        SetStatus(editor,std::wstring(T(editor.session.language,StrApplyRejected))+Widen(error),true);
        return;
    }
    if(CommitChannels(editor,std::move(next),T(editor.session.language,StrReady),true)){
        UpdateKeyFields(editor);UpdateMmdPanel(editor);UpdateEnabledState(editor);
    }
}

void MmdMouseUp(Editor &editor) {
    if (editor.mmdDragHandle < 0) return;
    const int handle = editor.mmdDragHandle;
    editor.mmdDragHandle = -1;
    if (GetCapture() == editor.mmd) ReleaseCapture();
    SourceState &state = CurrentState(editor);
    Track *track = ActiveTrack(editor);
    if (!track || state.selection.empty()) return;
    const std::size_t endKey = ActiveKeyIndex(editor);
    if (endKey == 0 || endKey >= track->keys.size()) return;
    std::array<double, 4> controls = editor.mmdControls;
    // A dragged handle pair must stay strictly separated in time, otherwise the
    // core rejects it and the panel keeps the previous display.
    if (controls[0] >= controls[2]) {
        if (handle == 0) controls[0] = controls[2] - 1;
        else controls[2] = controls[0] + 1;
    }
    controls[0] = std::max(1.0, std::min(126.0, controls[0]));
    controls[2] = std::max(1.0, std::min(126.0, controls[2]));
    std::vector<Channel> next = CopyChannels(editor);
    Track &working = next[state.active].track;
    std::string error;
    if (!SetChannelControls(next[state.active], endKey, controls, error)) {
        SetStatus(editor, std::wstring(T(editor.session.language, StrApplyRejected)) + Widen(error), true);
        UpdateMmdPanel(editor);
        return;
    }
    if (CommitChannels(editor, std::move(next), T(editor.session.language, StrReady), true)) {
        UpdateKeyFields(editor);
        UpdateMmdPanel(editor);
        UpdateEnabledState(editor);
    } else {
        UpdateMmdPanel(editor);
    }
}

// ---------------------------------------------------------------------------
// Font, layout and language.
// ---------------------------------------------------------------------------
int ResolveDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
    static GetDpiForWindowFn getter = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (getter && window) {
        const UINT dpi = getter(window);
        if (dpi >= 48) return static_cast<int>(dpi);
    }
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi >= 48 ? dpi : 96;
}

HFONT CreateUiFont(int dpi) {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    using SpiForDpiFn = BOOL(WINAPI *)(int, UINT, void *, UINT, UINT);
    static SpiForDpiFn forDpi = reinterpret_cast<SpiForDpiFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi")));
    bool ok = false;
    if (forDpi) ok = forDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, static_cast<UINT>(dpi)) != FALSE;
    if (!ok) ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;
    if (!ok) return nullptr;
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

int MeasureTextWidth(Editor &editor, const wchar_t *text) {
    HDC dc = GetDC(editor.window);
    if (!dc) return Scale(40, editor.dpi);
    HGDIOBJ oldFont = SelectObject(dc, editor.font ? editor.font : GetStockObject(DEFAULT_GUI_FONT));
    SIZE size = {};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(dc, oldFont);
    ReleaseDC(editor.window, dc);
    return size.cx;
}

void MoveControl(Editor &editor, int id, int x, int y, int width, int height) {
    ui::MoveLayoutControl(editor.window, id, x, y, width, height, editor.deferLayoutPaint);
}

int ChannelWidthLimit(const Editor& editor) {
    RECT client{}; GetClientRect(editor.window, &client);
    return std::max(180, std::min(440, MulDiv(client.right, 96, editor.dpi) - 24 - 8 - 560));
}

LRESULT CALLBACK ChannelSplitterProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto& editor = *reinterpret_cast<Editor*>(data);
    auto setWidth = [&](int width) {
        const int next = std::clamp(width, 180, ChannelWidthLimit(editor));
        if (editor.channelWidth == next) return;
        editor.channelWidth = next;
        editor.deferLayoutPaint = true;
        LayoutEditor(editor);
        editor.deferLayoutPaint = false;
        RedrawWindow(editor.window, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    };
    switch (message) {
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEWE))); return TRUE;
    case WM_LBUTTONDOWN:
        editor.pendingChannelWidth = editor.channelWidth;
        editor.resizingChannels = true; SetFocus(hwnd); SetCapture(hwnd); return 0;
    case WM_MOUSEMOVE:
        if (editor.resizingChannels) {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; MapWindowPoints(hwnd, editor.window, &point, 1);
            editor.pendingChannelWidth = std::clamp(MulDiv(point.x, 96, editor.dpi) - 12 - 4, 180, ChannelWidthLimit(editor));
            SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEWE)));
        } return 0;
    case WM_LBUTTONUP:
        if (editor.resizingChannels) {
            // Mouse-up may be newer than the last coalesced mouse move.
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; MapWindowPoints(hwnd, editor.window, &point, 1);
            editor.pendingChannelWidth = std::clamp(MulDiv(point.x, 96, editor.dpi) - 12 - 4, 180, ChannelWidthLimit(editor));
            editor.resizingChannels = false;
            setWidth(editor.pendingChannelWidth);
        }
        if (GetCapture() == hwnd) ReleaseCapture(); return 0;
    case WM_LBUTTONDBLCLK: setWidth(kChannelWidth); return 0;
    case WM_CAPTURECHANGED: editor.resizingChannels = false; return 0;
    case WM_CANCELMODE:
        editor.resizingChannels = false;
        if (GetCapture() == hwnd) ReleaseCapture(); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && editor.resizingChannels) {SendMessageW(hwnd, WM_CANCELMODE, 0, 0); return 0;}
        if (wp == VK_LEFT || wp == VK_RIGHT) {setWidth(editor.channelWidth + (wp == VK_LEFT ? -1 : 1) * ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 10)); return 0;}
        if (wp == VK_HOME) {setWidth(kChannelWidth); return 0;}
        break;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps{}; HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wp);
        RECT r{}; GetClientRect(hwnd, &r); curve_editor_theme::Fill(dc, r, curve_editor_theme::shell);
        // The left panel's right edge is draggable without a visible divider.
        if (message == WM_PAINT) EndPaint(hwnd, &ps); return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: InvalidateRect(hwnd, nullptr, FALSE); break;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, ChannelSplitterProc, id); break;
    }
    LRESULT shared = 0;
    if (HandleChildMessage(hwnd, message, wp, lp, shared)) return shared;
    return DefSubclassProc(hwnd, message, wp, lp);
}

void LayoutEditor(Editor &editor) {
    if(!editor.window)return;
    RECT client{};GetClientRect(editor.window,&client);
    auto S=[&](int v){return Scale(v,editor.dpi);};
    const int margin=S(12),gap=S(8),height=S(26),pitch=height+gap;
    const int sidebar=S(std::clamp(editor.channelWidth,180,ChannelWidthLimit(editor)));
    const int left=margin+sidebar+gap,right=client.right-margin;
    const int width=right-left,label=S(48),fieldGap=S(16);
    auto put=[&](int id,int x,int y,int w,int h){
        HWND c=GetDlgItem(editor.window,id);
        if(id==1002||id==1007||id==1009||id==1045||id==1047)
            SendMessageW(c,CB_SETITEMHEIGHT,WPARAM(-1),S(20));
        MoveControl(editor,id,x,y,w,h);
    };
    auto tag=[&](int id,int x,int y,int w,int h){
        HWND c=GetDlgItem(editor.window,id);
        SetWindowLongPtrW(c,GWL_STYLE,GetWindowLongPtrW(c,GWL_STYLE)|SS_CENTERIMAGE);
        put(id,x,y,w,h);
    };
    auto field=[&](int lid,int cid,int x,int y,int w){
        tag(lid,x,y,label,height);put(cid,x+label+gap,y,w-label-gap,height);
    };
    // Toolbar groups stay fixed while the panel divider moves below them.
    const int first=margin+S(40),second=margin+S(kChannelWidth)+gap,secondBox=second+S(40),group=S(160);
    const int groupEnd=secondBox+group,action=groupEnd+S(16),actionWidth=S(client.right<900?76:88);
    int y=margin;
    tag(kIdSourceLabel,margin,y,S(32),height);
    put(kIdSourceCombo,first,y,groupEnd-first,height);
    put(kIdReadMotion,action,y,actionWidth,height);
    put(kIdWriteMotion,action+actionWidth+gap,y,actionWidth,height);
    y+=pitch;
    tag(kIdViewLabel,margin,y,S(32),height);put(kIdViewCombo,first,y,group,height);
    tag(kIdToolLabel,second,y,S(32),height);put(kIdToolCombo,secondBox,y,group,height);
    put(kIdFitAll,action,y,actionWidth,height);
    put(kIdFitRange,action+actionWidth+gap,y,actionWidth,height);
    y+=pitch;
    bool transport=editor.session.seek!=nullptr;
    int x=first;
    const int transportIds[]={kIdPreviousKey,kIdPreviousFrame,kIdNextFrame,kIdNextKey,kIdPlay,kIdTimelineFrame,kIdSeekFrame};
    for(int id:transportIds)ShowWindow(GetDlgItem(editor.window,id),transport?SW_SHOW:SW_HIDE);
    const int available=right-(action+2*actionWidth+gap)-S(16);
    const int transportWidth=std::min(S(320),available),transportLeft=right-transportWidth;
    ShowWindow(GetDlgItem(editor.window,kIdShortcutToggle),SW_HIDE);
    if(transport){
        const int playWidth=S(transportWidth<S(260)?56:72),stepGap=S(4);
        const int navigationWidth=transportWidth-playWidth-gap,usable=navigationWidth-3*stepGap;
        const int steps[]={kIdPreviousKey,kIdPreviousFrame,kIdNextFrame,kIdNextKey};
        for(int i=0;i<4;++i)put(steps[i],transportLeft+usable*i/4+i*stepGap,margin,usable*(i+1)/4-usable*i/4,height);
        put(kIdPlay,right-playWidth,margin,playWidth,height);
        put(kIdTimelineFrame,transportLeft,margin+pitch,navigationWidth,height);
        put(kIdSeekFrame,right-playWidth,margin+pitch,playWidth,height);
        EnableWindow(GetDlgItem(editor.window,kIdPlay),editor.session.setPlaying!=nullptr);
    }
    bool compare=ComparisonSupported(editor);
    for(int id:{kIdCompareLabel,kIdCompareSameGroup,kIdCompareFree,kIdCompareAllGroups,kIdCompareFilter})ShowWindow(GetDlgItem(editor.window,id),compare?SW_SHOW:SW_HIDE);
    if(compare){tag(kIdCompareLabel,margin,y,S(32),height);x=first;for(int id:{kIdCompareSameGroup,kIdCompareFree,kIdCompareAllGroups}){put(id,x,y,S(104),height);x+=S(104);}put(kIdCompareFilter,x+gap,y,S(136),height);y+=pitch;}

    const int statusTop=client.bottom-margin-S(kStatusHeight);
    const int hintTop=statusTop-gap-S(kHintHeight),bakeTop=hintTop-S(12)-height;
    const int columns=width>=S(840)?4:2;
    const int advancedRows=columns==4?1:2;
    const int advancedHeight=std::max(S(36),advancedRows*pitch-gap);
    const int actionOffset=3*pitch+S(30)+(editor.advancedExpanded?advancedHeight+gap:0);
    const int coordinateBlock=2*pitch+S(32);
    // Use the free space beside the baking row for the coordinate fields.
    // This enlarges the square without needlessly shortening the main plot.
    const int leftExtra=hintTop-gap-(bakeTop-S(12));
    const int miniLimit=bakeTop-S(12)-y-S(100)-S(12)-coordinateBlock+leftExtra;
    const int preferredMini=editor.advancedExpanded?std::clamp(sidebar-S(42),S(138),S(240)):S(148);
    const int miniSide=std::min(preferredMini,std::max(S(100),miniLimit));
    const int stripHeight=std::max(miniSide+coordinateBlock-leftExtra,actionOffset+height);
    const int stripTop=bakeTop-S(12)-stripHeight;
    const int helpHeight=0;
    const int canvasBottom=stripTop-S(12)-(helpHeight?helpHeight+gap:0);
    tag(kIdChannelLabel,margin,y,sidebar-2*S(48)-2*gap,height);
    put(kIdChannelInvert,margin+sidebar-2*S(48)-gap,y,S(48),height);
    put(kIdChannelAll,margin+sidebar-S(48),y,S(48),height);
    put(kIdChannelList,margin,y+pitch,sidebar,std::max(height,canvasBottom-y-pitch));
    curve_editor_channel_scroll::Layout(GetDlgItem(editor.window,kIdChannelList));
    put(1111,margin+sidebar,y,gap,std::max(height,hintTop-y-gap));
    put(kIdCanvas,left,y,width,std::max(S(100),canvasBottom-y));
    ShowWindow(GetDlgItem(editor.window,kIdShortcutText),helpHeight?SW_SHOW:SW_HIDE);
    if(helpHeight)put(kIdShortcutText,left,canvasBottom+gap,width,helpHeight);

    ShowWindow(GetDlgItem(editor.window,kIdMmdLabel),SW_HIDE);
    int side=miniSide,panelY=stripTop;
    put(kIdMmdPanel,margin+(sidebar-side)/2,panelY,side,side);
    const int coordinateY=panelY+side+gap;
    for(int row=0;row<2;++row){
        const int cy=coordinateY+row*pitch;
        tag(1100+row*3,margin,cy,S(58),height);
        const int input=(sidebar-S(90))/2;
        put(kIdMmdX1+row*2,margin+S(58),cy,input,height);
        tag(1101+row*3,margin+S(58)+input,cy,S(16),height);
        put(kIdMmdY1+row*2,margin+S(74)+input,cy,input,height);
        tag(1102+row*3,margin+S(74)+2*input,cy,S(16),height);
    }
    put(kIdMmdReadout,margin,coordinateY+2*pitch,sidebar,S(24));
    int half=(width-fieldGap)/2;
    field(kIdFrameLabel,kIdFrameEdit,left,stripTop,half);
    field(kIdValueLabel,kIdValueEdit,left+half+fieldGap,stripTop,width-half-fieldGap);
    field(kIdInterpolationLabel,kIdInterpolationCombo,left,stripTop+pitch,width);
    field(kIdTangentLabel,kIdTangentCombo,left,stripTop+2*pitch,width);

    int moreY=stripTop+3*pitch;
    SetControlText(editor.window,1090,editor.advancedExpanded?L"收起斜率与权重":L"展开斜率与权重");
    put(1090,left,moreY,S(136),S(22));
    SetControlText(editor.window,1094,editor.advancedExpanded?L"上下拖动 · Ctrl 精调 · Shift 快调 · Enter 应用":L"Enter 应用当前参数");
    ShowWindow(GetDlgItem(editor.window,1094),SW_SHOW);
    tag(1094,left+S(148),moreY,std::max(S(100),width-S(148)),S(22));
    const int lids[]={kIdInSlopeLabel,kIdOutSlopeLabel,kIdInWeightLabel,kIdOutWeightLabel};
    const int cids[]={kIdInSlopeEdit,kIdOutSlopeEdit,kIdInWeightEdit,kIdOutWeightEdit};
    const int applyWidth=S(editor.session.language==1?60:32),fieldsRight=right-applyWidth-gap;
    int cell=(fieldsRight-left-(columns-1)*fieldGap)/columns;
    for(int i=0;i<4;++i){
        ShowWindow(GetDlgItem(editor.window,lids[i]),editor.advancedExpanded?SW_SHOW:SW_HIDE);
        ShowWindow(GetDlgItem(editor.window,cids[i]),editor.advancedExpanded?SW_SHOW:SW_HIDE);
        int column=i%columns,row=i/columns,fx=left+column*(cell+fieldGap);
        int fw=column==columns-1?fieldsRight-fx:cell;
        const int rowOffset=advancedRows==1?(advancedHeight-height)/2:0;
        field(lids[i],cids[i],fx,moreY+S(30)+row*pitch+rowOffset,fw);
    }
    ShowWindow(GetDlgItem(editor.window,kIdApplyKey),editor.advancedExpanded?SW_SHOW:SW_HIDE);
    put(kIdApplyKey,right-applyWidth,moreY+S(30),applyWidth,advancedHeight);
    int buttons[]={kIdAddKey,kIdDeleteKey,kIdUndo,kIdRedo};
    int usable=width-3*gap;
    for(int i=0;i<4;++i){int bx=left+usable*i/4+i*gap;put(buttons[i],bx,stripTop+actionOffset,usable*(i+1)/4-usable*i/4,height);}

    int bakeButton=S(132),pairsWidth=width-bakeButton-gap,pair=pairsWidth/3;
    int bakeLabels[]={kIdBakeFirstLabel,kIdBakeLastLabel,kIdBakeStepLabel};
    int bakeEdits[]={kIdBakeFirstEdit,kIdBakeLastEdit,kIdBakeStepEdit};
    for(int i=0;i<3;++i)field(bakeLabels[i],bakeEdits[i],left+i*pair,bakeTop,pair-gap);
    put(kIdBakeRun,right-bakeButton,bakeTop,bakeButton,height);
    put(kIdHoverHint,margin,hintTop,client.right-2*margin,S(kHintHeight));
    const int readoutWidth=std::min(S(340),(int(client.right)-2*margin)/2);
    put(kIdStatus,margin,statusTop,client.right-2*margin-readoutWidth-gap,S(kStatusHeight));
    put(kIdMouseReadout,right-readoutWidth,statusTop,readoutWidth,S(kStatusHeight));
    if(editor.canvas){CanvasMetrics(editor);InvalidateCanvas(editor);}
    if(editor.mmd)InvalidateRect(editor.mmd,nullptr,FALSE);
}


void FillCombos(Editor &editor) {
    const unsigned language = editor.session.language;
    if (HWND combo = GetDlgItem(editor.window, kIdViewCombo)) {
        const int previous = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrViewValue)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrViewSpeed)));
        SendMessageW(combo, CB_SETCURSEL, previous < 0 ? static_cast<WPARAM>(editor.viewMode) : static_cast<WPARAM>(previous), 0);
    }
    if (HWND combo = GetDlgItem(editor.window, kIdToolCombo)) {
        const int previous = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrToolSelect)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrToolPan)));
        SendMessageW(combo, CB_SETCURSEL, previous < 0 ? static_cast<WPARAM>(editor.tool) : static_cast<WPARAM>(previous), 0);
    }
    if (HWND combo = GetDlgItem(editor.window, kIdInterpolationCombo)) {
        const int previous = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrStep)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrLinear)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrBezier)));
        SendMessageW(combo, CB_SETCURSEL, previous < 0 ? 2 : static_cast<WPARAM>(previous), 0);
    }
    if (HWND combo = GetDlgItem(editor.window, kIdTangentCombo)) {
        SetPropW(combo, L"CurveLanguage", reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(language)));
        const int previous = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrTanFree)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrTanAligned)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrTanAutomatic)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrTanFlat)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrTanLinear)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrTanEaseIn)));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(language, StrTanEaseOut)));
        SendMessageW(combo, CB_SETCURSEL, previous < 0 ? kTangentAutomatic : static_cast<WPARAM>(previous), 0);
    }
    FillSourceCombo(editor);
}

void ApplyLanguage(Editor &editor) {
    const unsigned language = editor.session.language;
    SetControlText(editor.window,kIdPlay,PlayCaption(editor));
    SetControlText(editor.window,kIdSeekFrame,language==0?L"移動":language==1?L"Go":L"跳转");
    // A source that offers localizedLabel renames itself for this language; a
    // source without it keeps the plain label, so both callers work.
    RefreshSourceLabels(editor);
    SetControlText(editor.window, kIdSourceLabel, T(language, StrSource));
    SetControlText(editor.window, kIdViewLabel, T(language, StrView));
    SetControlText(editor.window, kIdToolLabel, T(language, StrTool));
    SetControlText(editor.window, kIdChannelLabel, T(language, StrChannels));
    SetControlText(editor.window, kIdMmdLabel, T(language, StrMmdTitle));
    SetControlText(editor.window, kIdFrameLabel, T(language, StrKeyFrame));
    SetControlText(editor.window, kIdValueLabel, T(language, StrKeyValue));
    SetControlText(editor.window, kIdInterpolationLabel, T(language, StrInterpolation));
    SetControlText(editor.window, kIdTangentLabel, T(language, StrTangent));
    SetControlText(editor.window, kIdInSlopeLabel, T(language, StrInSlope));
    SetControlText(editor.window, kIdOutSlopeLabel, T(language, StrOutSlope));
    SetControlText(editor.window, kIdInWeightLabel, T(language, StrInWeight));
    SetControlText(editor.window, kIdOutWeightLabel, T(language, StrOutWeight));
    SetControlText(editor.window, kIdBakeFirstLabel, T(language, StrBakeFirst));
    SetControlText(editor.window, kIdBakeLastLabel, T(language, StrBakeLast));
    SetControlText(editor.window, kIdBakeStepLabel, T(language, StrBakeStep));
    SetControlText(editor.window, kIdReadMotion, T(language, StrReadMotion));
    SetControlText(editor.window, kIdWriteMotion, T(language, StrWriteMotion));
    SetControlText(editor.window, kIdFitAll, T(language, StrFitAll));
    SetControlText(editor.window, kIdFitRange, T(language, StrFitRange));
    SetControlText(editor.window, kIdShortcutToggle, T(language, StrShortcuts));
    SetControlText(editor.window, kIdChannelAll, T(language, StrSelectAll));
    SetControlText(editor.window, kIdChannelInvert, T(language, StrInvertSelection));
    SetControlText(editor.window, kIdAddKey, T(language, StrAddKey));
    SetControlText(editor.window, kIdDeleteKey, T(language, StrDeleteKey));
    SetControlText(editor.window, kIdApplyKey, T(language, StrApply));
    SetControlText(editor.window, kIdUndo, T(language, StrUndo));
    SetControlText(editor.window, kIdRedo, T(language, StrRedo));
    SetControlText(editor.window, kIdBakeRun, T(language, StrBakeRun));
    SetControlText(editor.window, kIdShortcutText, T(language, StrShortcutsText));
    SetControlText(editor.window, kIdCompareLabel, T(language, StrCompare));
    SetControlText(editor.window, kIdCompareSameGroup, T(language, StrCompareSameGroup));
    SetControlText(editor.window, kIdCompareFree, T(language, StrCompareFree));
    SetControlText(editor.window, kIdCompareAllGroups, T(language, StrCompareAllGroups));
    SetControlText(editor.window, kIdCompareFilter, T(language, StrCompareFilter));
    FillCombos(editor);
    UpdateWindowTitle(editor);
    LayoutEditor(editor);
    UpdateMmdPanel(editor);
    // The option lists and the range readout follow the language too.
    UpdateCompareUi(editor);
    // A hint that is on screen while the language changes is retranslated.
    ApplyHoverHintText(editor);
    // Repaint the status with the current text under the new language rules.
    SourceState &state = CurrentState(editor);
    if (state.status.empty()) SetStatus(editor, T(language, StrReady), false);
    else {
        bool modeStatus = false;
        for (unsigned previous = 0; previous < 3; ++previous)
            modeStatus = modeStatus || state.status.rfind(T(previous, StrCompareModeHead), 0) == 0;
        if (modeStatus)
            SetStatus(editor, std::wstring(T(language, StrCompareModeHead)) + CompareModeLabel(editor, state.compareMode), false);
        else SetStatus(editor, state.status, state.statusError);
    }
}

// ---------------------------------------------------------------------------
// Control creation.
// ---------------------------------------------------------------------------
HWND MakeControl(Editor &editor, const wchar_t *className, const wchar_t *text, DWORD style, int id) {
    const bool edit = wcscmp(className, L"EDIT") == 0;
    HWND control = CreateWindowExW(edit ? WS_EX_CLIENTEDGE : 0, className, text,
                                   WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, editor.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (!control) return nullptr;
    if (editor.font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(editor.font), TRUE);
    if (edit) {
        SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Scale(4, editor.dpi), Scale(4, editor.dpi)));
        SendMessageW(control, EM_SETLIMITTEXT, 64, 0);
    }
    return control;
}

void CreateControls(Editor &editor) {
    const unsigned language = editor.session.language;
    MakeControl(editor,L"BUTTON",PlayCaption(editor),WS_TABSTOP|BS_PUSHBUTTON,kIdPlay);
    MakeControl(editor,L"BUTTON",L"上一关键帧",WS_TABSTOP|BS_PUSHBUTTON,kIdPreviousKey);
    MakeControl(editor,L"BUTTON",L"上一帧",WS_TABSTOP|BS_PUSHBUTTON,kIdPreviousFrame);
    MakeControl(editor,L"BUTTON",L"下一帧",WS_TABSTOP|BS_PUSHBUTTON,kIdNextFrame);
    MakeControl(editor,L"BUTTON",L"下一关键帧",WS_TABSTOP|BS_PUSHBUTTON,kIdNextKey);
    MakeControl(editor,L"EDIT",L"0",WS_TABSTOP|ES_AUTOHSCROLL|ES_NUMBER,kIdTimelineFrame);
    MakeControl(editor,L"BUTTON",language==0?L"移動":language==1?L"Go":L"跳转",WS_TABSTOP|BS_PUSHBUTTON,kIdSeekFrame);

    // The library lives in both the EXE and MMV.dll. Custom child classes must
    // use their parent's module, not an implicit EXE instance.
    const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(editor.window, GWLP_HINSTANCE));
    MakeControl(editor, L"STATIC", T(language, StrSource), SS_LEFT, kIdSourceLabel);
    MakeControl(editor, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kIdSourceCombo);
    MakeControl(editor, L"BUTTON", T(language, StrReadMotion), WS_TABSTOP | BS_PUSHBUTTON, kIdReadMotion);
    MakeControl(editor, L"BUTTON", T(language, StrWriteMotion), WS_TABSTOP | BS_PUSHBUTTON, kIdWriteMotion);
    MakeControl(editor, L"STATIC", T(language, StrView), SS_LEFT, kIdViewLabel);
    MakeControl(editor, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kIdViewCombo);
    MakeControl(editor, L"STATIC", T(language, StrTool), SS_LEFT, kIdToolLabel);
    MakeControl(editor, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kIdToolCombo);
    MakeControl(editor, L"BUTTON", T(language, StrFitAll), WS_TABSTOP | BS_PUSHBUTTON, kIdFitAll);
    MakeControl(editor, L"BUTTON", T(language, StrFitRange), WS_TABSTOP | BS_PUSHBUTTON, kIdFitRange);
    MakeControl(editor, L"BUTTON", T(language, StrShortcuts), WS_TABSTOP | BS_AUTOCHECKBOX, kIdShortcutToggle);
    MakeControl(editor, L"STATIC", T(language, StrShortcutsText), SS_LEFT, kIdShortcutText);
    MakeControl(editor, L"STATIC", T(language, StrCompare), SS_LEFT, kIdCompareLabel);
    // One segmented control: three touching push-like radio buttons in the
    // visual order same group - free - four groups.  WS_GROUP on the first and
    // on the filter button closes the radio group around the three segments.
    MakeControl(editor, L"BUTTON", T(language, StrCompareSameGroup),
                WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, kIdCompareSameGroup);
    MakeControl(editor, L"BUTTON", T(language, StrCompareFree),
                WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, kIdCompareFree);
    MakeControl(editor, L"BUTTON", T(language, StrCompareAllGroups),
                WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, kIdCompareAllGroups);
    MakeControl(editor, L"BUTTON", T(language, StrCompareFilter), WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON,
                kIdCompareFilter);
    MakeControl(editor, L"STATIC", T(language, StrChannels), SS_LEFT, kIdChannelLabel);
    MakeControl(editor, WC_TREEVIEWW, L"", WS_TABSTOP | WS_VSCROLL | WS_BORDER | WS_CLIPSIBLINGS | TVS_NOHSCROLL | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_CHECKBOXES, kIdChannelList);
    SendMessageW(GetDlgItem(editor.window,kIdChannelList),CCM_SETUNICODEFORMAT,TRUE,0);
    MakeControl(editor,L"STATIC",language==2?L"调整通道栏宽度":language==1?L"Resize channel panel":L"チャンネル欄の幅を変更",SS_NOTIFY,1111);
    SetWindowSubclass(GetDlgItem(editor.window,1111),ChannelSplitterProc,1111,reinterpret_cast<DWORD_PTR>(&editor));
    MakeControl(editor, L"BUTTON", T(language, StrSelectAll), WS_TABSTOP | BS_PUSHBUTTON, kIdChannelAll);
    MakeControl(editor, L"BUTTON", T(language, StrInvertSelection), WS_TABSTOP | BS_PUSHBUTTON, kIdChannelInvert);

    editor.canvas = CreateWindowExW(0, kCanvasClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                    editor.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCanvas)), instance, nullptr);
    SetWindowLongPtrW(editor.canvas, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&editor));

    MakeControl(editor, L"STATIC", T(language, StrMmdTitle), SS_LEFT, kIdMmdLabel);
    editor.mmd = CreateWindowExW(0, kMmdClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, editor.window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdMmdPanel)), instance, nullptr);
    SetWindowLongPtrW(editor.mmd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&editor));
    MakeControl(editor, L"STATIC", L"", SS_LEFT, kIdMmdReadout);
    for(int row=0;row<2;++row){
        MakeControl(editor,L"STATIC",row?L"终止点（":L"起始点（",SS_LEFT,1100+row*3);
        MakeControl(editor,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,kIdMmdX1+row*2);
        MakeControl(editor,L"STATIC",L"，",SS_CENTER,1101+row*3);
        MakeControl(editor,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,kIdMmdY1+row*2);
        MakeControl(editor,L"STATIC",L"）",SS_LEFT,1102+row*3);
    }

    MakeControl(editor, L"STATIC", T(language, StrKeyFrame), SS_LEFT, kIdFrameLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdFrameEdit);
    MakeControl(editor, L"STATIC", T(language, StrKeyValue), SS_LEFT, kIdValueLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdValueEdit);
    MakeControl(editor, L"STATIC", T(language, StrInterpolation), SS_LEFT, kIdInterpolationLabel);
    MakeControl(editor, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kIdInterpolationCombo);
    MakeControl(editor, L"STATIC", T(language, StrTangent), SS_LEFT, kIdTangentLabel);
    MakeControl(editor, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kIdTangentCombo);
    MakeControl(editor, L"STATIC", T(language, StrInSlope), SS_LEFT, kIdInSlopeLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdInSlopeEdit);
    MakeControl(editor, L"STATIC", T(language, StrOutSlope), SS_LEFT, kIdOutSlopeLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdOutSlopeEdit);
    MakeControl(editor, L"STATIC", T(language, StrInWeight), SS_LEFT, kIdInWeightLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdInWeightEdit);
    MakeControl(editor, L"STATIC", T(language, StrOutWeight), SS_LEFT, kIdOutWeightLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdOutWeightEdit);
    MakeControl(editor, L"BUTTON", T(language, StrAddKey), WS_TABSTOP | BS_PUSHBUTTON, kIdAddKey);
    MakeControl(editor, L"BUTTON", T(language, StrDeleteKey), WS_TABSTOP | BS_PUSHBUTTON, kIdDeleteKey);
    MakeControl(editor, L"BUTTON", T(language, StrApply), WS_TABSTOP | BS_PUSHBUTTON, kIdApplyKey);
    MakeControl(editor, L"BUTTON", T(language, StrUndo), WS_TABSTOP | BS_PUSHBUTTON, kIdUndo);
    MakeControl(editor, L"BUTTON", T(language, StrRedo), WS_TABSTOP | BS_PUSHBUTTON, kIdRedo);

    MakeControl(editor, L"STATIC", T(language, StrBakeFirst), SS_LEFT, kIdBakeFirstLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdBakeFirstEdit);
    MakeControl(editor, L"STATIC", T(language, StrBakeLast), SS_LEFT, kIdBakeLastLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdBakeLastEdit);
    MakeControl(editor, L"STATIC", T(language, StrBakeStep), SS_LEFT, kIdBakeStepLabel);
    MakeControl(editor, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdBakeStepEdit);
    MakeControl(editor, L"BUTTON", T(language, StrBakeRun), WS_TABSTOP | BS_PUSHBUTTON, kIdBakeRun);
    MakeControl(editor,L"BUTTON",L"收起斜率与权重",WS_TABSTOP|BS_PUSHBUTTON,1090);
    MakeControl(editor,L"STATIC",L"上下拖动 · Ctrl 精调 · Shift 快调",SS_LEFT,1094);
    MakeControl(editor, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kIdHoverHint);
    MakeControl(editor, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kIdStatus);
    MakeControl(editor, L"STATIC", L"", SS_RIGHT | SS_CENTERIMAGE | SS_ENDELLIPSIS, kIdMouseReadout);

    FillCombos(editor);
}

// ---------------------------------------------------------------------------
// Command dispatch.
// ---------------------------------------------------------------------------
void HandleCommand(Editor &editor, WPARAM wparam) {
    const int id = LOWORD(wparam);
    const int code = HIWORD(wparam);
    switch (id) {
    case kIdSourceCombo:
        if (code == CBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(GetDlgItem(editor.window, kIdSourceCombo), CB_GETCURSEL, 0, 0));
            if (selected >= 0) SwitchSource(editor, static_cast<std::size_t>(selected));
            UpdateBakeFields(editor);
            UpdateEnabledState(editor);
        }
        return;
    case kIdViewCombo:
        if (code == CBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(GetDlgItem(editor.window, kIdViewCombo), CB_GETCURSEL, 0, 0));
            if (selected >= 0) editor.viewMode = selected;
            UpdateMmdPanel(editor);
            InvalidateCanvas(editor);
        }
        return;
    case kIdToolCombo:
        if (code == CBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(GetDlgItem(editor.window, kIdToolCombo), CB_GETCURSEL, 0, 0));
            if (selected >= 0) editor.tool = selected;
        }
        return;
    case kIdInterpolationCombo:
    case kIdTangentCombo:
        if (code == CBN_SELCHANGE) ApplyCurveChoice(editor, id);
        return;
    case kIdChannelList:
        // The list never reaches WM_COMMAND: ChannelListSubclassProc answers its
        // mouse and caret keys itself, because the built-in list box selection
        // would clear the other rows and would not toggle an already shown one.
        return;
    case kIdAddKey: AddKey(editor); return;
    case kIdDeleteKey: DeleteSelectedKeys(editor); return;
    case kIdApplyKey: ApplyKeyFields(editor); return;
    case kIdUndo: UndoOnce(editor); return;
    case kIdRedo: RedoOnce(editor); return;
    case kIdPlay:
        if(editor.session.setPlaying)editor.session.setPlaying(!(editor.session.isPlaying&&editor.session.isPlaying()));
        SetControlText(editor.window,kIdPlay,PlayCaption(editor));return;
    case kIdPreviousFrame: case kIdNextFrame:
        if(editor.session.currentFrame){
            const double current=editor.session.currentFrame();
            if(std::isfinite(current))SeekTimeline(editor,int(std::clamp(std::round(current)+(id==kIdNextFrame?1:-1),0.0,double(kMaximumFrame))));
        }
        OnPlayheadTimer(editor);
        return;
    case kIdPreviousKey: case kIdNextKey: SeekAdjacentKey(editor,id==kIdNextKey);return;
    case kIdSeekFrame:{
        long long frame=0;
        if(ParseIntegerText(ControlText(editor.window,kIdTimelineFrame),frame)&&frame>=0&&frame<=kMaximumFrame){SeekTimeline(editor,int(frame));OnPlayheadTimer(editor);}
        return;
    }
    case kIdFitAll: FitAll(editor); return;
    case kIdFitRange: FitRange(editor); return;
    case kIdShortcutToggle:
        if (code == BN_CLICKED) {
            editor.helpExpanded = IsDlgButtonChecked(editor.window, kIdShortcutToggle) == BST_CHECKED;
            LayoutEditor(editor);
        }
        return;
    case kIdCompareSameGroup:
        if (code == BN_CLICKED) ChangeCompareMode(editor, kCompareSameGroup);
        return;
    case kIdCompareAllGroups:
        if (code == BN_CLICKED) ChangeCompareMode(editor, kCompareAllGroups);
        return;
    case kIdCompareFree:
        if (code == BN_CLICKED) ChangeCompareMode(editor, kCompareFree);
        return;
    case kIdCompareFilter:
        if (code == BN_CLICKED) RunCompareFilter(editor);
        return;
    case kIdChannelAll: SelectAllChannels(editor); return;
    case kIdChannelInvert: InvertChannels(editor); return;
    case kIdBakeRun: RunBake(editor); return;
    case kIdReadMotion: RunReadMotion(editor); return;
    case kIdWriteMotion: RunWriteMotion(editor); return;
    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// Window procedures.
// ---------------------------------------------------------------------------
void BufferedPaint(Editor& editor,HWND hwnd,HDC dc,const RECT& dirty,bool miniature){
    RECT client{};GetClientRect(hwnd,&client);
    auto& surface=miniature?editor.mmdSurface:editor.canvasSurface;
    if(!surface.Resize(dc,std::max(1L,client.right),std::max(1L,client.bottom))){
        if(miniature)PaintMmd(editor,dc);else PaintCanvas(editor,dc);return;
    }
    const int saved=SaveDC(surface.dc);
    IntersectClipRect(surface.dc,dirty.left,dirty.top,dirty.right,dirty.bottom);
    if(miniature)PaintMmd(editor,surface.dc);else PaintCanvas(editor,surface.dc);
    RestoreDC(surface.dc,saved);
    BitBlt(dc,dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top,surface.dc,dirty.left,dirty.top,SRCCOPY);
}
void InsertCurveKey(Editor& editor,std::size_t hit,int frame){
    if(!CheckSourceCurrent(editor))return;
    auto& state=CurrentState(editor);
    if(!state.valid||!CurrentSource(editor).apply||hit>=state.channels.size())return;
    const auto& keys=state.channels[hit].track.keys;
    const auto existing=std::lower_bound(keys.begin(),keys.end(),frame,
        [](const Key& key,int f){return key.frame<f;});
    std::size_t index=std::size_t(existing-keys.begin());
    if(existing==keys.end() || existing->frame!=frame){
        std::vector<Channel> next=CopyChannels(editor);
        auto& source=CurrentSource(editor);
        if(source.insertPreservingShape){
            std::wstring error;
            if(!source.insertPreservingShape(next,hit,frame,error)){
                SetStatus(editor,error.empty()?T(editor.session.language,StrInvalidFrame):error,true);return;
            }
        }else{
            const auto& channel=next[hit];
            if(channel.sample||!channel.nativeControls.empty()||!channel.interpolationGroup.empty()){
                SetStatus(editor,editor.session.language==0?L"このネイティブトラックではキー挿入に対応していません。":
                    editor.session.language==1?L"Key insertion is unavailable in this native track.":L"此原生通道暂不支持保形插帧。",true);return;
            }
            std::string error;
            if(animation_curve::InsertPreservingShape(next[hit].track,frame,index,error)!=animation_curve::InsertResult::Inserted){
                SetStatus(editor,Widen(error),true);return;
            }
        }
        const auto& inserted=next[hit].track.keys;
        const auto key=std::lower_bound(inserted.begin(),inserted.end(),frame,[](const Key& k,int f){return k.frame<f;});
        if(key==inserted.end()||key->frame!=frame)return;
        index=std::size_t(key-inserted.begin());
        if(!CommitChannels(editor,std::move(next),T(editor.session.language,StrReady)))return;
    }else SetStatus(editor,T(editor.session.language,StrFrameSelected)+FormatInteger(frame),false);
    state.active=hit;state.selection={index};
    RefreshEditorsUi(editor);PublishKeySelection(editor);SeekTimeline(editor,frame);
}
void InsertAtPlayhead(Editor& editor){
    if(!editor.session.currentFrame)return;
    const double frame=editor.session.currentFrame();
    if(std::isfinite(frame))InsertCurveKey(editor,CurrentState(editor).active,
        int(std::clamp(std::round(frame),0.0,double(kMaximumFrame))));
}
void InsertCurveKeyAtPoint(Editor& editor,POINT point,UINT flags) {
    CanvasMetrics(editor);
    if(!PtInRect(&editor.plot,point) || editor.tool==1 ||
       (flags&(MK_CONTROL|MK_SHIFT)) || (GetKeyState(VK_SPACE)&0x8000) || (GetKeyState(VK_MENU)&0x8000))return;
    CancelDrag(editor);SetFocus(editor.canvas);
    auto& state=CurrentState(editor);
    if(!state.valid || !CurrentSource(editor).apply)return;
    std::vector<std::size_t> candidates=ComparedChannels(editor);
    if(ActiveChannelVisible(editor))candidates.insert(candidates.begin(),state.active);
    const int radius=Scale(6,editor.dpi);
    double best=double(radius*radius)+1,hitFrame=0;
    std::size_t hit=state.channels.size();
    // Match the visible curve near the pointer, preferring the active curve at
    // intersections. Sampling a small pixel band also works on steep slopes.
    for(const std::size_t c:candidates){
        const auto& track=state.channels[c].track;
        if(track.keys.empty() || (editor.viewMode==1&&track.discrete))continue;
        for(int x=std::max(int(editor.plot.left),int(point.x)-radius);x<=std::min(int(editor.plot.right),int(point.x)+radius);++x){
            double frame=0,y=0;if(!XToTime(editor,x,frame))continue;
            const Sample value=SampleTrack(editor,track,frame);
            if(!ValueToY(editor,editor.viewMode?value.derivative*kFramesPerSecond:value.value,y)||!std::isfinite(y))continue;
            const double distance=double(x-point.x)*(x-point.x)+(y-point.y)*(y-point.y);
            if(distance+.25<best){best=distance;hitFrame=frame;hit=c;}
        }
    }
    if(hit==state.channels.size())return;
    const Channel& channel=state.channels[hit];
    int frame=int(std::clamp(std::round(hitFrame),0.0,double(kMaximumFrame)));
    // A visible key wins over neighbouring rounded frames when zoomed out.
    for(const Key& key:channel.track.keys){
        double x=0,y=0;
        const Sample sample=SampleTrack(editor,channel.track,key.frame);
        if(TimeToX(editor,key.frame,x)&&ValueToY(editor,editor.viewMode?sample.derivative*kFramesPerSecond:key.value,y)&&
           std::hypot(x-point.x,y-point.y)<=radius){frame=key.frame;break;}
    }
    InsertCurveKey(editor,hit,frame);
}

LRESULT CALLBACK CanvasProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!editor) return DefWindowProcW(hwnd, message, wparam, lparam);
    const auto arrow = [] { return LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)); };
    const auto sizeAll = [] { return LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_SIZEALL)); };
    const auto hand = [] { return LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_HAND)); };
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        BufferedPaint(*editor,hwnd,dc,paint.rcPaint,false);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        // Standard repaint-into-a-DC entry point: the host can composite the
        // canvas and the hidden regression exercises the whole painting path.
        PaintCanvas(*editor, reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_SIZE:
        CanvasMetrics(*editor);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN:
        editor->mmdFollowPlayhead=false;
        CanvasMouseDown(*editor, static_cast<UINT>(wparam), POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_LBUTTONDBLCLK:
        if(!CenterBarAtPoint(*editor,POINT{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}))
            InsertCurveKeyAtPoint(*editor,POINT{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)},UINT(wparam));
        return 0;
    case WM_MBUTTONDOWN: {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetFocus(hwnd);
        CanvasMetrics(*editor);
        editor->savedTimeStart = editor->timeStart;
        editor->savedTimeEnd = editor->timeEnd;
        editor->savedValueLow = editor->valueLow;
        editor->savedValueHigh = editor->valueHigh;
        editor->drag = Drag::Pan;
        editor->dragOrigin = point;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        CanvasMouseUp(*editor, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        TrackMouseLeave(hwnd);
        CanvasMetrics(*editor);
        // The three canvas regions explain themselves separately: the plot
        // area carries the keys and the navigation gestures, the time bar the
        // horizontal range and the value bar the vertical one.
        if (PtInRect(&editor->timeBar, point))
            SetHoverHint(*editor, StrHintTimeBar);
        else if (PtInRect(&editor->valueBar, point))
            SetHoverHint(*editor, StrHintValueBar);
        else if (PtInRect(&editor->plot, point))
            SetHoverHint(*editor, StrHintCanvas);
        else
            SetHoverHint(*editor, -1);
        CanvasMouseMove(*editor, point);
        UpdateMouseReadout(*editor,point);
        return 0;
    }
    case WM_MOUSELEAVE:
        SetHoverHint(*editor, -1);
        UpdateMouseReadout(*editor,{},false);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &point);
        CanvasWheel(*editor, wparam, point);
        UpdateMouseReadout(*editor,point);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if(message==WM_KEYDOWN&&HandleEditorShortcut(*editor,hwnd,wparam,lparam))return 0;
        switch (wparam) {
        case VK_RETURN:
            if(message==WM_KEYDOWN)RegisterFromShortcut(*editor,lparam);
            return 0;
        case VK_DELETE:
            if(message==WM_KEYDOWN)DeleteFromShortcut(*editor,lparam);
            return 0;
        case VK_TAB:
            // The canvas is a tab stop too, so Tab leaves it towards the next
            // control in the reading order.
            MoveFocusByTab(*editor, hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            BarArrowKey(*editor, wparam, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        case VK_SPACE:
            editor->spaceDown = true;
            SetCursor(sizeAll());
            return 0;
        case 'V':
            editor->tool = 0;
            SendMessageW(GetDlgItem(editor->window, kIdToolCombo), CB_SETCURSEL, 0, 0);
            return 0;
        case 'H':
            editor->tool = 1;
            SendMessageW(GetDlgItem(editor->window, kIdToolCombo), CB_SETCURSEL, 1, 0);
            return 0;
        case 'F':
            FitRange(*editor);
            return 0;
        case VK_HOME:
            FitAll(*editor);
            return 0;
        case VK_ESCAPE:
            CancelDrag(*editor);
            InvalidateCanvas(*editor);
            return 0;
        default:
            return 0;
        }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wparam == VK_SPACE) {
            editor->spaceDown = false;
            SetCursor(arrow());
            return 0;
        }
        return 0;
    case WM_CAPTURECHANGED:
        // Losing the capture always clears the transient drag state so the next
        // press cannot continue a stale gesture.
        if (editor->drag != Drag::None) {
            editor->drag = Drag::None;
            editor->dragMoved = false;
            editor->dragSelection.clear();
            editor->dragFrameDelta = 0;
            editor->dragValueDelta = 0;
            InvalidateCanvas(*editor);
        }
        return 0;
    case WM_KILLFOCUS:
        editor->spaceDown = false;
        CancelDrag(*editor);
        InvalidateCanvas(*editor);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            if (editor->tool == 1 || editor->spaceDown) {
                SetCursor(sizeAll());
                return TRUE;
            }
            POINT point = {};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            if (editor->drag == Drag::Keys || (editor->drag == Drag::None && KeyAtPoint(*editor, point, 6.0) >= 0)) {
                SetCursor(hand());
                return TRUE;
            }
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK MmdProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!editor) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_KEYDOWN:
        if(HandleEditorShortcut(*editor,hwnd,wparam,lparam))return 0;
        if(wparam==VK_RETURN){RegisterFromShortcut(*editor,lparam);return 0;}
        if(wparam==VK_DELETE){DeleteFromShortcut(*editor,lparam);return 0;}
        if(wparam==VK_ESCAPE && editor->mmdDragHandle>=0){
            editor->mmdDragHandle=-1;
            if(GetCapture()==hwnd)ReleaseCapture();
            UpdateMmdPanel(*editor);return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        BufferedPaint(*editor,hwnd,dc,paint.rcPaint,true);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        PaintMmd(*editor, reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_LBUTTONDOWN:
        MmdMouseDown(*editor, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_MOUSEMOVE:
        TrackMouseLeave(hwnd);
        SetHoverHint(*editor, StrHintMmdHandles);
        MmdMouseMove(*editor, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    case WM_MOUSELEAVE:
        SetHoverHint(*editor, -1);
        return 0;
    case WM_LBUTTONUP:
        MmdMouseUp(*editor);
        return 0;
    case WM_CAPTURECHANGED:
        if(editor->mmdDragHandle>=0){editor->mmdDragHandle=-1;UpdateMmdPanel(*editor);}
        return 0;
    case WM_KILLFOCUS:
        if(editor->mmdDragHandle>=0){
            editor->mmdDragHandle=-1;
            if(GetCapture()==hwnd)ReleaseCapture();
            UpdateMmdPanel(*editor);
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT HandleHook(Editor &editor, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_APP+394: {
        if(!lparam)return FALSE;
        CanvasMetrics(editor);
        auto& out=*reinterpret_cast<CurveEditorAxisState*>(lparam);
        out.plot=editor.plot;out.timeBar=editor.timeBar;out.valueBar=editor.valueBar;
        BarPixels(editor,true,out.timeFirst,out.timeLast);
        BarPixels(editor,false,out.valueFirst,out.valueLast);
        const auto time=TimeBarRange(editor),value=ValueBarRange(editor);
        out.timeLow=time.fullLow;out.timeHigh=time.fullHigh;
        out.valueLow=value.fullLow;out.valueHigh=value.fullHigh;
        out.timeStart=editor.timeStart;out.timeEnd=editor.timeEnd;
        ValueRange(editor,out.viewLow,out.viewHigh);
        out.mouseValid=MouseCoordinates(editor,out.mouseFrame,out.mouseValue);
        out.viewMode=editor.viewMode;
        return TRUE;
    }
    case kHookPointForKey: {
        CanvasMetrics(editor);
        const POINT point = KeyPoint(editor, static_cast<std::size_t>(wparam));
        return MAKELPARAM(point.x, point.y);
    }
    case kHookPointForFrame: {
        CanvasMetrics(editor);
        double value = 0;
        if (Track *track = ActiveTrack(editor)){
            const auto sample=SampleTrack(editor,*track,static_cast<double>(static_cast<long long>(wparam)));
            value=editor.viewMode?sample.derivative*kFramesPerSecond:sample.value;
        }
        double x = 0, y = 0;
        if (!TimeToX(editor, static_cast<double>(static_cast<long long>(wparam)), x) || !ValueToY(editor, value, y))
            return MAKELPARAM(-1, -1);
        return MAKELPARAM(static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)));
    }
    case kHookMmdControls: {
        ComputeMmdControls(editor);
        if (lparam) {
            auto *out = reinterpret_cast<double *>(lparam);
            for (std::size_t index = 0; index < 4; ++index) out[index] = editor.mmdControls[index];
        }
        return editor.mmdEnabled ? TRUE : FALSE;
    }
    case kHookKeyCount: {
        SourceState &state = CurrentState(editor);
        const long long index = static_cast<long long>(wparam);
        if (index < 0) {
            if (state.channels.empty()) return -1;
            return static_cast<LRESULT>(state.channels[state.active].track.keys.size());
        }
        if (static_cast<std::size_t>(index) >= state.channels.size()) return -1;
        return static_cast<LRESULT>(state.channels[static_cast<std::size_t>(index)].track.keys.size());
    }
    case kHookView: {
        CanvasMetrics(editor);
        if (lparam) {
            auto *out = reinterpret_cast<double *>(lparam);
            out[0] = editor.timeStart;
            out[1] = editor.timeEnd;
            out[2] = editor.valueLow;
            out[3] = editor.valueHigh;
        }
        return static_cast<LRESULT>(editor.viewMode);
    }
    case kHookBarPoint: {
        // wParam = 1 time bar / 2 value bar, lParam = 0 centre, 1 low end,
        // 2 high end.  Returns the canvas client point of that segment.
        CanvasMetrics(editor);
        const int barKind = static_cast<int>(wparam);
        const int segment = static_cast<int>(lparam);
        if (barKind == 1) {
            RECT inner = editor.timeBar;
            InflateRect(&inner, -Scale(2, editor.dpi), -Scale(3, editor.dpi));
            const BarRange range = TimeBarRange(editor);
            int first = 0, last = 0;
            BarPixels(editor, true, first, last);
            const int left =
                static_cast<int>(std::lround(MapToBar(editor.timeStart, range.fullLow, range.fullHigh, first, last)));
            const int right =
                static_cast<int>(std::lround(MapToBar(editor.timeEnd, range.fullLow, range.fullHigh, first, last)));
            const int x = segment == 0 ? (left + right) / 2 : segment == 1 ? left : right;
            return MAKELPARAM(x, (editor.timeBar.top + editor.timeBar.bottom) / 2);
        }
        if (barKind == 2) {
            int top = 0, bottom = 0;
            ValueBarEdges(editor, top, bottom);
            // Segment 1 is the low-value (bottom) edge, 2 the high-value (top).
            const int y = segment == 0 ? (top + bottom) / 2 : segment == 1 ? bottom : top;
            return MAKELPARAM((editor.valueBar.left + editor.valueBar.right) / 2, y);
        }
        return MAKELPARAM(-1, -1);
    }
    case kHookCompare: {
        // Read-only view of the comparison state; see the header comment for
        // the layout of the int[12] the caller provides.
        const std::vector<std::size_t> compared = ComparedChannels(editor);
        SourceState &state = CurrentState(editor);
        if (lparam) {
            auto *out = reinterpret_cast<int *>(lparam);
            for (int index = 0; index < 12; ++index) out[index] = 0;
            out[0] = state.compareMode;
            out[1] = state.channels.empty() ? -1 : static_cast<int>(std::min(state.active, state.channels.size() - 1));
            out[2] = static_cast<int>(compared.size());
            for (std::size_t position = 0; position < compared.size() && position < 8; ++position)
                out[3 + position] = static_cast<int>(compared[position]);
            int hidden = 0;
            for (const Channel &channel : state.channels) hidden += channel.selected ? 0 : 1;
            out[11] = hidden;
        }
        return static_cast<LRESULT>(state.compareMode);
    }
    case kHookCompareRange: {
        // lParam = double[3] {first, last, source}; source 1 means the selected
        // keys, 2 the range fields and 0 that no span is available at all.
        double first = 0, last = 0;
        int source = kSpanNone;
        const bool available = CompareSpan(editor, first, last, source);
        if (lparam) {
            auto *out = reinterpret_cast<double *>(lparam);
            out[0] = first;
            out[1] = last;
            out[2] = static_cast<double>(source);
        }
        return available ? 1 : 0;
    }
    case kHookChannelVisible: {
        // wParam = channel index: 1 shown, 0 hidden, -1 outside the list.
        SourceState &state = CurrentState(editor);
        const long long index = static_cast<long long>(wparam);
        if (index < 0 || static_cast<std::size_t>(index) >= state.channels.size()) return -1;
        return state.channels[static_cast<std::size_t>(index)].selected ? 1 : 0;
    }
    case kHookHoverHint:
        return static_cast<LRESULT>(HoverHintCode(editor.hoverHint));
    case kHookFocusId: {
        HWND focused = GetFocus();
        if (!focused) return -1;
        for (const int id : kTabOrder) {
            if (GetDlgItem(editor.window, id) == focused) return static_cast<LRESULT>(id);
        }
        return 0;
    }
    case kHookPlayheadState: {
        // lParam = long long[3]: timer running, ticks that passed the
        // visibility gate, and ticks that repainted something.
        if (lparam) {
            auto *out = reinterpret_cast<long long *>(lparam);
            out[0] = editor.playheadTimerRunning ? 1 : 0;
            out[1] = editor.playheadTicks;
            out[2] = editor.playheadInvalidations;
        }
        return 0;
    }
    default:
        return 0;
    }
}

LRESULT DrawChannelMarker(Editor&editor,NMTVCUSTOMDRAW&draw){
    if(draw.nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
    if(draw.nmcd.dwDrawStage==CDDS_ITEMPREPAINT){
        if(curve_editor_theme::loaded){
            const bool selected=(draw.nmcd.uItemState&CDIS_SELECTED)!=0;
            draw.clrText=curve_editor_theme::text;
            draw.clrTextBk=selected?curve_editor_theme::selection:curve_editor_theme::paper;
            draw.nmcd.uItemState&=~CDIS_SELECTED;
        }
        return CDRF_NOTIFYPOSTPAINT;
    }
    if(draw.nmcd.dwDrawStage!=CDDS_ITEMPOSTPAINT)return CDRF_DODEFAULT;
    HWND tree=draw.nmcd.hdr.hwndFrom;
    HTREEITEM item=reinterpret_cast<HTREEITEM>(draw.nmcd.dwItemSpec);
    // Collections use the first descendant's colour; leaves are exact legends.
    HTREEITEM leaf=item;
    while(leaf&&TreeChannel(tree,leaf)<0)leaf=TreeView_GetChild(tree,leaf);
    const int index=TreeChannel(tree,leaf);
    const auto&state=CurrentState(editor);
    if(index<0||std::size_t(index)>=state.channels.size())return CDRF_DODEFAULT;
    RECT text{};
    if(!TreeView_GetItemRect(tree,item,&text,TRUE))return CDRF_DODEFAULT;
    const int radius=std::max(2,Scale(3,editor.dpi));
    const int x=text.right-Scale(6,editor.dpi),y=(text.top+text.bottom)/2;
    const int saved=SaveDC(draw.nmcd.hdc);
    RECT client{};GetClientRect(tree,&client);
    IntersectClipRect(draw.nmcd.hdc,client.left,client.top,client.right,client.bottom);
    HBRUSH brush=CreateSolidBrush(ChannelColour(state.channels[index]));
    SelectObject(draw.nmcd.hdc,brush);SelectObject(draw.nmcd.hdc,GetStockObject(NULL_PEN));
    Ellipse(draw.nmcd.hdc,x-radius,y-radius,x+radius+1,y+radius+1);
    RestoreDC(draw.nmcd.hdc,saved);DeleteObject(brush);
    return CDRF_DODEFAULT;
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Editor *editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        editor = static_cast<Editor *>(create->lpCreateParams);
        editor->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
        return TRUE;
    }
    if (!editor) return DefWindowProcW(hwnd, message, wparam, lparam);

    switch (message) {
    case WM_NOTIFY:{
        auto*header=reinterpret_cast<NMHDR*>(lparam);
        if(header&&header->idFrom==kIdChannelList&&header->code==NM_CUSTOMDRAW)
            return DrawChannelMarker(*editor,*reinterpret_cast<NMTVCUSTOMDRAW*>(lparam));
        break;
    }
    case WM_KEYDOWN:
        if(HandleEditorShortcut(*editor,hwnd,wparam,lparam))return 0;
        if(wparam==VK_RETURN){RegisterFromShortcut(*editor,lparam);return 0;}
        if(wparam==VK_DELETE){DeleteFromShortcut(*editor,lparam);return 0;}
        break;
    case WM_APP+393: {
        auto* bounds=reinterpret_cast<double*>(lparam);
        Track* track=ActiveTrack(*editor);std::size_t key=ActiveKeyIndex(*editor);
        bounds[0]=0.0001;bounds[1]=0.9999;
        if(track&&key<track->keys.size()){
            if(wparam==kIdInWeightEdit&&key>0)bounds[1]=0.9999-track->keys[key-1].outWeight;
            if(wparam==kIdOutWeightEdit&&key+1<track->keys.size())bounds[1]=0.9999-track->keys[key+1].inWeight;
        }
        bounds[1]=std::max(bounds[0],bounds[1]);return 1;
    }
    case kSourceChangedMessage:
        ProcessSourceChanges(*editor);
        return 0;
    case WM_CREATE:
        editor->dpi = ResolveDpi(hwnd);
        editor->font = CreateUiFont(editor->dpi);
        curve_editor_theme::Load(editor->session.theme);
        editor->themeShell = CreateSolidBrush(curve_editor_theme::shell);
        CreateControls(*editor);
        curve_editor_theme::Apply(hwnd);
        curve_editor_segments::Apply(hwnd);
        curve_editor_scrub::Apply(hwnd);
        TreeView_SetBkColor(GetDlgItem(hwnd,kIdChannelList),curve_editor_theme::paper);
        TreeView_SetTextColor(GetDlgItem(hwnd,kIdChannelList),curve_editor_theme::text);
        TreeView_SetLineColor(GetDlgItem(hwnd,kIdChannelList),curve_editor_theme::border);
        TreeView_SetItemHeight(GetDlgItem(hwnd,kIdChannelList),Scale(23,editor->dpi));
        InstallTabNavigation(*editor);
        curve_editor_channel_scroll::Attach(GetDlgItem(hwnd,kIdChannelList));
        ApplyLanguage(*editor);
        ReadAndAdopt(*editor, false);
        UpdateBakeFields(*editor);
        SetControlText(editor->window, kIdBakeStepEdit, L"1");
        LayoutEditor(*editor);
        SetStatus(*editor, T(editor->session.language, StrReady), false);
        UpdatePlayheadTimer(*editor);
        return 0;
    case WM_SHOWWINDOW:
        UpdatePlayheadTimer(*editor);
        return 0;
    case WM_TIMER:
        if (wparam == kSourceChangedTimer) {
            ProcessSourceChanges(*editor);
            return 0;
        }
        if (wparam == kPlayheadTimer) {
            OnPlayheadTimer(*editor);
            return 0;
        }
        break;
    case WM_SIZE:
        LayoutEditor(*editor);
        return 0;
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
        const int dpi = editor->dpi ? editor->dpi : ResolveDpi(hwnd);
        info->ptMinTrackSize.x = Scale(kMinimumClientWidth, dpi) + Scale(16, dpi);
        info->ptMinTrackSize.y = Scale(kMinimumClientHeight, dpi) + Scale(39, dpi);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==1090){editor->advancedExpanded=!editor->advancedExpanded;LayoutEditor(*editor);return 0;}
        HandleCommand(*editor, wparam);
        if(LOWORD(wparam)>=kIdAddKey&&LOWORD(wparam)<=kIdRedo)PublishKeySelection(*editor);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            NotifySourceChanged(editor->owner, CurrentSource(*editor).id);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // Forward a wheel that the parent received to the canvas under the
        // pointer, so navigation works while an edit box holds the focus.
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        HWND target = WindowFromPoint(point);
        if (editor->canvas && (target == editor->canvas || IsChild(editor->canvas, target))) {
            SendMessageW(editor->canvas, WM_MOUSEWHEEL, wparam, lparam);
            return 0;
        }
        break;
    }
    case WM_ERASEBKGND:
    case WM_PRINTCLIENT: {
        RECT client{};GetClientRect(hwnd,&client);
        FillRect(reinterpret_cast<HDC>(wparam),&client,editor->themeShell?editor->themeShell:GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc,curve_editor_theme::text);
        const bool field=message==WM_CTLCOLOREDIT||message==WM_CTLCOLORLISTBOX;
        SetBkColor(dc,field?curve_editor_theme::paper:curve_editor_theme::shell);
        if (field&&curve_editor_theme::paperBrush)return reinterpret_cast<LRESULT>(curve_editor_theme::paperBrush);
        if (control == GetDlgItem(hwnd, kIdStatus) && CurrentState(*editor).statusError) SetTextColor(dc, curve_editor_theme::error);
        // The hover hint is grey helper text, never an error line.
        else if (control == GetDlgItem(hwnd, kIdHoverHint)) SetTextColor(dc, curve_editor_theme::muted);
        return reinterpret_cast<LRESULT>(editor->themeShell?editor->themeShell:GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY: {
        KillTimer(hwnd, kSourceChangedTimer);
        if (editor->playheadTimerRunning) {
            KillTimer(hwnd, kPlayheadTimer);
            editor->playheadTimerRunning = false;
        }
        if (editor->font) DeleteObject(editor->font);
        if (editor->themeShell) DeleteObject(editor->themeShell);
        {
            std::lock_guard<std::mutex> guard(g_registryMutex);
            g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), editor), g_editors.end());
            if(g_editors.empty())curve_editor_theme::Release();
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete editor;
        return 0;
    }
    default:
        break;
    }
    if (message >= WM_APP && message <= WM_APP + 400) return HandleHook(*editor, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HINSTANCE ModuleHandleFor(void *address) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module))
        return module;
    return GetModuleHandleW(nullptr);
}

bool RegisterEditorClasses() {
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        const HINSTANCE instance = ModuleHandleFor(reinterpret_cast<void *>(&EditorProc));
        WNDCLASSEXW editorClass = {};
        editorClass.cbSize = sizeof(editorClass);
        editorClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        editorClass.lpfnWndProc = EditorProc;
        editorClass.hInstance = instance;
        editorClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        editorClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        editorClass.lpszClassName = kEditorClass;
        if (!RegisterClassExW(&editorClass)) return;

        WNDCLASSEXW canvasClass = {};
        canvasClass.cbSize = sizeof(canvasClass);
        canvasClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        canvasClass.lpfnWndProc = CanvasProc;
        canvasClass.hInstance = instance;
        canvasClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        canvasClass.hbrBackground = nullptr;
        canvasClass.lpszClassName = kCanvasClass;
        if (!RegisterClassExW(&canvasClass)) return;

        WNDCLASSEXW mmdClass = {};
        mmdClass.cbSize = sizeof(mmdClass);
        mmdClass.style = CS_HREDRAW | CS_VREDRAW;
        mmdClass.lpfnWndProc = MmdProc;
        mmdClass.hInstance = instance;
        mmdClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        mmdClass.hbrBackground = nullptr;
        mmdClass.lpszClassName = kMmdClass;
        if (!RegisterClassExW(&mmdClass)) return;
        registered = true;
    });
    return registered;
}

Editor *FindEditor(void *owner) {
    std::lock_guard<std::mutex> guard(g_registryMutex);
    for (Editor *editor : g_editors) {
        if (editor->owner == owner) return editor;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points.
// ---------------------------------------------------------------------------
bool ReadChannelControls(const Channel &channel, std::size_t end, std::array<double,4> &out) {
    if (end > 0 && end < channel.track.keys.size() && end < channel.nativeControls.size() &&
        end < channel.controlGeometry.size()) {
        const Key &a = channel.track.keys[end-1], &b = channel.track.keys[end];
        const std::array<double,4> geometry = {a.outWeight, a.outSlope, b.inWeight, b.inSlope};
        if (geometry == channel.controlGeometry[end]) { out = channel.nativeControls[end]; return true; }
        return animation_curve::GetMmdControls(channel.track, end, out);
    }
    if (animation_curve::GetMmdControls(channel.track, end, out)) return true;
    if (end == 0 || end >= channel.track.keys.size() || end >= channel.nativeControls.size()) return false;
    out = channel.nativeControls[end];
    return true;
}
bool SetChannelControls(Channel &channel, std::size_t end, const std::array<double,4> &controls, std::string &error) {
    if (end == 0 || end >= channel.track.keys.size()) return false;
    if (channel.track.keys[end].value != channel.track.keys[end-1].value) {
        if (!animation_curve::SetMmdControls(channel.track, end, controls, error)) return false;
    } else {
        Track timing = channel.track;
        for (Key &key : timing.keys) key.value = key.frame;
        if (!animation_curve::SetMmdControls(timing, end, controls, error)) return false;
        channel.track.keys[end-1].outWeight = timing.keys[end-1].outWeight;
        channel.track.keys[end].inWeight = timing.keys[end].inWeight;
        channel.track.keys[end-1].outSlope = channel.track.keys[end].inSlope = 0;
        channel.track.keys[end].interpolation = Interpolation::Bezier;
        channel.track.keys[end-1].tangent = channel.track.keys[end].tangent = animation_curve::Tangent::Free;
    }
    if (!channel.nativeControls.empty()) {
        channel.nativeControls[end] = controls;
        const Key &a = channel.track.keys[end-1], &b = channel.track.keys[end];
        channel.controlGeometry[end] = {a.outWeight, a.outSlope, b.inWeight, b.inSlope};
    }
    return true;
}
void *Open(void *owner, Session session, bool visible) {
    INITCOMMONCONTROLSEX common = {};
    common.dwSize = sizeof(common);
    common.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common);
    GdiPlusToken();
    if (!RegisterEditorClasses()) return nullptr;

    if (Editor *existing = FindEditor(owner)) {
        KillTimer(existing->window, kSourceChangedTimer);
        existing->pendingSources.clear();
        existing->session = std::move(session);
        curve_editor_theme::Load(existing->session.theme);
        if(existing->themeShell)DeleteObject(existing->themeShell);
        existing->themeShell=CreateSolidBrush(curve_editor_theme::shell);
        curve_editor_theme::Apply(existing->window);
        existing->sourceIndex = existing->session.initialSource < existing->session.sources.size()
                                    ? existing->session.initialSource
                                    : 0;
        existing->states.assign(existing->session.sources.size(), SourceState());
        existing->channelTreeSource.clear();
        PopulateChannels(*existing);
        ApplyLanguage(*existing);
        ReadAndAdopt(*existing, false);
        // Re-opening an existing window is how the host hands it a new morph,
        // so the initial main channel and the remembered mode are re-read here.
        EnterSource(*existing);
        UpdateBakeFields(*existing);
        SetControlText(existing->window, kIdBakeStepEdit, L"1");
        if (visible) {
            ShowWindow(existing->window, SW_SHOW);
            SetForegroundWindow(existing->window);
        }
        return existing->window;
    }

    auto *editor = new Editor();
    editor->owner = owner;
    editor->session = std::move(session);
    editor->sourceIndex =
        editor->session.initialSource < editor->session.sources.size() ? editor->session.initialSource : 0;
    editor->states.assign(editor->session.sources.size(), SourceState());
    {
        std::lock_guard<std::mutex> guard(g_registryMutex);
        g_editors.push_back(editor);
    }

    const HINSTANCE instance = ModuleHandleFor(reinterpret_cast<void *>(&EditorProc));
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const DWORD exStyle = WS_EX_CONTROLPARENT;
    const int dpi = ResolveDpi(nullptr);
    HWND window = CreateWindowExW(exStyle, kEditorClass, L"", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  Scale(kDefaultClientWidth, dpi) + Scale(16, dpi),
                                  Scale(kDefaultClientHeight, dpi) + Scale(39, dpi), static_cast<HWND>(owner), nullptr,
                                  instance, editor);
    if (!window) {
        std::lock_guard<std::mutex> guard(g_registryMutex);
        g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), editor), g_editors.end());
        delete editor;
        return nullptr;
    }
    // WM_CREATE already read the source; this is where its initial main channel
    // and its remembered comparison mode are applied.
    EnterSource(*editor);
    if (visible) {
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
    }
    return window;
}

void CloseOwned(void *owner) {
    std::vector<HWND> windows;
    {
        std::lock_guard<std::mutex> guard(g_registryMutex);
        for (Editor *editor : g_editors) {
            if (editor->owner == owner) windows.push_back(editor->window);
        }
    }
    for (HWND window : windows) {
        if (IsWindow(window)) DestroyWindow(window);
    }
}

void SetLanguage(void *owner, unsigned language) {
    Editor *editor = FindEditor(owner);
    if (!editor) return;
    if (language > 2) language = 2;
    editor->session.language = language;
    ApplyLanguage(*editor);
}

bool OwnsWindow(void* owner, void* window) {
    Editor* editor = FindEditor(owner);
    const HWND target = static_cast<HWND>(window);
    return editor && target && IsWindow(editor->window) &&
        (target == editor->window || IsChild(editor->window, target));
}

void NotifySourceChanged(void *owner, const std::string &sourceId) {
    Editor *editor = FindEditor(owner);
    if (!editor || !IsWindow(editor->window)) return;
    if (GetWindowThreadProcessId(editor->window, nullptr) != GetCurrentThreadId()) return;
    const auto match = std::find_if(editor->session.sources.begin(), editor->session.sources.end(),
                                   [&](const Source &source) { return source.id == sourceId; });
    if (match == editor->session.sources.end()) return;
    editor->pendingSources.insert(sourceId);
    if (!editor->sourceChangePosted)
        editor->sourceChangePosted = PostMessageW(editor->window, kSourceChangedMessage, 0, 0) != FALSE;
}
void NotifyOwnedChanged(void *owner) {
    Editor *editor = FindEditor(owner);
    if (!editor) return;
    for (const Source &source : editor->session.sources) NotifySourceChanged(owner, source.id);
}

void DrawLauncher(void *hdcPointer, int x, int y, int width, int height, bool pressed, bool disabled, bool focused) {
    HDC dc = static_cast<HDC>(hdcPointer);
    if (!dc || width <= 0 || height <= 0) return;
    GdiPlusToken();

    RECT button = {x, y, x + width, y + height};
    DWORD frameState = DFCS_BUTTONPUSH;
    if (pressed) frameState |= DFCS_PUSHED;
    if (disabled) frameState |= DFCS_INACTIVE;
    DrawFrameControl(dc, &button, DFC_BUTTON, frameState);

    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    if (dpi < 48) dpi = 96;
    const int side = std::min(width, height);
    int logical = static_cast<int>(std::lround(side * 96.0 / dpi));
    if (logical < 1) logical = 1;
    const bool regularAsset = logical >= 21;
    int inset;
    if (logical == 18 || logical == 20 || logical == 22 || logical == 24)
        inset = std::max(1, MulDiv(2, dpi, 96));
    else
        inset = std::max(1, (side + 5) / 11);

    if (focused) {
        RECT focusRect = button;
        InflateRect(&focusRect, -inset, -inset);
        if (focusRect.right > focusRect.left && focusRect.bottom > focusRect.top) DrawFocusRect(dc, &focusRect);
    }

    const int offset = pressed ? std::max(1, MulDiv(1, dpi, 96)) : 0;
    const int iconSize = side - 2 * inset;
    const int iconLeft = x + (width - iconSize) / 2 + offset;
    const int iconTop = y + (height - iconSize) / 2 + offset;
    RECT icon = {iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize};
    const int iconSide = icon.right - icon.left;
    if (iconSide <= 2) return;
    const double viewport = regularAsset ? 20.0 : 14.0;
    const double scale = iconSide / viewport;

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const COLORREF colour = GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);
    const Gdiplus::Color penColour(255, GetRValue(colour), GetGValue(colour), GetBValue(colour));
    const auto map = [&](double u, double v) {
        return Gdiplus::PointF(static_cast<Gdiplus::REAL>(icon.left + u * scale),
                               static_cast<Gdiplus::REAL>(icon.top + v * scale));
    };

    Gdiplus::GraphicsPath boxPath;
    const double box = regularAsset ? 18.5 : 12.5;
    const Gdiplus::PointF corners[4] = {map(1.5, 1.5), map(box, 1.5), map(box, box), map(1.5, box)};
    boxPath.AddLine(corners[0], corners[1]);
    boxPath.AddLine(corners[1], corners[2]);
    boxPath.AddLine(corners[2], corners[3]);
    boxPath.CloseFigure();
    Gdiplus::Pen boxPen(penColour, static_cast<Gdiplus::REAL>(1.0 * scale));
    boxPen.SetLineJoin(Gdiplus::LineJoinMiter);
    graphics.DrawPath(&boxPen, &boxPath);

    Gdiplus::GraphicsPath curvePath;
    if (regularAsset) {
        curvePath.AddBezier(map(4.0, 13.5), map(11.5, 13.5), map(8.5, 6.5), map(16.0, 6.5));
    } else {
        curvePath.AddBezier(map(4.0, 9.0), map(7.75, 9.0), map(6.25, 5.0), map(10.0, 5.0));
    }
    Gdiplus::Pen curvePen(penColour, static_cast<Gdiplus::REAL>(1.5 * scale));
    curvePen.SetStartCap(Gdiplus::LineCapRound);
    curvePen.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawPath(&curvePen, &curvePath);
}

}  // namespace curve_editor
}  // namespace mikudancestudio
