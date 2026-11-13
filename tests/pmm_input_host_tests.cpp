// Real hidden-host PMM descriptor regressions. V1/V2 native parser bodies and
// the success tail remain real. Only tail notifications/errors are faulted;
// CRT observers always forward the real operation and never hide invalid fds.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#include <MinHook.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "mikumikustudio/accessory_import.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/cache_manager.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/mmv4mms.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/native_mesh.hpp"
#include "mikumikustudio/original_file_encoding.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/update_coordinator.hpp"
#include "mikumikustudio/recording_state.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include "mikumikustudio/ui_language.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/texture_prepare_cache.hpp"
#include "../src/io/pmm_read_buffer.hpp"
#include "../src/io/project_open.hpp"
#include "../src/io/project_relocation_context.hpp"
#include "../src/model/model_geometry_cache.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "model_geometry_reference.hpp"
#if !MMD_ORIGINAL_PROFILE
#include <vfw.h>
#include "../src/plugins/history_runtime.hpp"
#include "../src/plugins/project_storage.hpp"
#include "../src/plugins/background_identity.hpp"
#include "mikumikustudio/background_picture_asset.hpp"
#include "mikumikustudio/d3dx_dyn.hpp"
#include "mikumikustudio/system_settings.hpp"
#include "../src/app/settings_json.hpp"
#endif

using namespace mikumikustudio;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0, tailCalls = 0, messages = 0, missingDialogs = 0, resetThrows = 0;
MMDApp* app = nullptr;
fs::path directory, candidate, sentinelPath;
struct RelocationDialogs {
    std::vector<fs::path> choices;
    size_t prompts = 0, picks = 0;
    bool expectBadModel = false, sawBadModel = false;
    INT_PTR terminalAnswer = 1;
    bool expectAbort = false;
    project_relocation::Ticket ticket;
};
RelocationDialogs* relocationDialogs = nullptr;
struct V2RelocationReply {
    ProjectModelIssue issue;
    const wchar_t* label;
    INT_PTR answer;
    fs::path selection;
};
struct V2RelocationDialogs {
    std::vector<V2RelocationReply> replies;
    size_t next = 0;
    const V2RelocationReply* awaitingPicker = nullptr;
    const wchar_t* projectRoot = nullptr;
    const wchar_t* originalModelDirectory = nullptr;
    const wchar_t* expectedPickerDirectory = nullptr;
};
V2RelocationDialogs* v2Relocation = nullptr;
// ---- Real V2 missing-accessory recovery loop --------------------------------
// MMS_TEST_V2_ACCESSORY_RECOVERY selects one isolated case group: 1 for the
// missing-accessory loop, 2 for duplicate slots, 3 for the original-profile
// material refusal, or 4 for an exception while reading an acquired buffer.
// This reply state is armed only around the group's loads, so the existing V2
// model relocation replies and every legacy accessory fault keep their exact
// behavior. The production parser, LoadAccessoryObject and writer all stay real;
// only the modal dialogs and the picker are driven from the reply list.
struct V2AccessoryReply {
    INT_PTR answer;
    fs::path selection;
};
enum class V2AbortReset { Forward, Refuse, ThrowStandard, ThrowUnknown };

struct V2AccessoryDialogs {
    std::vector<V2AccessoryReply> replies;
    std::size_t next = 0;
    std::size_t prompts = 0, picks = 0;
    const wchar_t* expectedLabel = nullptr;
    const wchar_t* followingLabel = nullptr;
    std::vector<std::wstring> pickerDirectories;
    const wchar_t* projectRoot = nullptr;
    const wchar_t* originalAccessoryDirectory = nullptr;
    const V2AccessoryReply* awaitingPicker = nullptr;
    std::vector<std::wstring> loadPaths;
    std::vector<bool> loadResults;
    std::vector<void*> loadObjects;
    fs::path disappearingCandidate;
    void* failedObject = nullptr;
    const fs::path* partialFailureSource = nullptr;
    unsigned partialRefusals = 0, partialDisposals = 0;
    // Explicit 4/2 answers run the production accessory abort: the real
    // ResetAppState and the shell's later real LocalizeUI both call
    // PostViewRefresh outside the parser success tail. The original-profile
    // observer classifies those two exact real call scopes separately.
    bool expectNormalAbort = false;
    bool normalAbort = false;
    unsigned normalAbortResets = 0;
    unsigned firstAbortResetFailures = 0;
    V2AbortReset firstAbortReset = V2AbortReset::Forward;
    V2AbortReset retryAbortReset = V2AbortReset::Forward;
    bool observeFinalAbortNotice = false;
    unsigned abortLanguage = 0;
    unsigned abortResetRefreshes = 0;
    unsigned abortLocalizeRefreshes = 0;
    unsigned localizeCalls = 0;
    bool inRealAbortReset = false;
    bool inAbortLocalize = false;
    bool duplicateSlot = false;
    unsigned invalidNotices = 0;
    unsigned disposedLoadedRecords = 0;
};
V2AccessoryDialogs* v2Accessory = nullptr;
struct V2ModelAbortThrow {
    V2AbortReset action;
    unsigned language;
    bool persistent;
    unsigned resets = 0;
    unsigned invalidNotices = 0;
};
V2ModelAbortThrow* v2ModelAbortThrow = nullptr;
struct MidResetFault {
    bool persistent;
    bool unknown;
    unsigned failures = 0;
    unsigned partialRedraws = 0;
    unsigned unprotectedRedraws = 0;
};
MidResetFault* midResetFault = nullptr;
decltype(&ReleaseGlobalTimelineTracks) realReleaseGlobalTracks = nullptr;
decltype(&RedrawWindow) realResetRedraw = nullptr;
// Fixed production picker text, indexed by app->EnglishUI(): 0 Japanese,
// 1 English, 2 Chinese. These are the exact strings pmm_load_v2.cpp hands to
// GetOpenFileNameW; the hook below compares them, never rebuilds the dialog.
constexpr wchar_t kAccessoryPickerFilterJp[] =
    L"\x8AAD\x8FBC\x53EF\x80FD\x30D5\x30A1\x30A4\x30EB(*.x,*.vac)";
constexpr wchar_t kAccessoryPickerFilterEn[] = L"accessory file(*.x,*.vac)";
constexpr wchar_t kAccessoryPickerTitleJp[] =
    L"\x30A2\x30AF\x30BB\x30B5\x30EA\x8AAD\x307F\x8FBC\x307F";
constexpr wchar_t kAccessoryPickerTitleCn[] = L"\x52A0\x8F7D\x9644\x4EF6";
bool AccessoryPickerFilterMatches(const wchar_t* filter, unsigned language) {
    const wchar_t* first = language == 0 ? kAccessoryPickerFilterJp : kAccessoryPickerFilterEn;
    const std::size_t length = std::wcslen(first);
    return filter != nullptr && !std::wcsncmp(filter, first, length) &&
        !std::wcscmp(filter + length + 1, L"*.x;*.vac");
}
const wchar_t* AccessoryPickerTitle(unsigned language) {
    if (language == 0) return kAccessoryPickerTitleJp;
    if (language == 2) return kAccessoryPickerTitleCn;
    return L"Load accessory";
}
// Real D3DX-loadable text X fixtures. The triangle is the minimal mesh from
// tests/accessory_feedback_checks.hpp:131 with explicit floating-point vertex
// tokens, so a successful load proves the production D3DX parser accepted it.
constexpr char kAccessoryTriangleA[] =
    "xof 0303txt 0032\n"
    "Mesh tri {3;-5.0;5.0;0.0;,0.0;15.0;0.0;,5.0;5.0;0.0;;1;3;0,1,2;;"
    "MeshMaterialList {1;1;0;;Material {0.8;0.3;0.1;1.0;;1.0;0;0;0;;0;0;0;;}}}";
constexpr char kAccessoryTriangleB[] =
    "xof 0303txt 0032\n"
    "Mesh tri {3;-6.0;6.0;0.0;,0.0;18.0;0.0;,6.0;6.0;0.0;;1;3;0,1,2;;"
    "MeshMaterialList {1;1;0;;Material {0.8;0.3;0.1;1.0;;1.0;0;0;0;;0;0;0;;}}}";
constexpr char kAccessoryTriangleGood[] =
    "xof 0303txt 0032\n"
    "Mesh tri {3;-7.0;7.0;0.0;,0.0;21.0;0.0;,7.0;7.0;0.0;;1;3;0,1,2;;"
    "MeshMaterialList {1;1;0;;Material {0.2;0.3;0.1;1.0;;1.0;0;0;0;;0;0;0;;}}}";
struct AccessoryPayload {
    std::array<void*, 4> pointers{};
    std::uint32_t materialCount = 0;

    bool Allocated() const {
        return materialCount == 1 && std::all_of(pointers.begin(), pointers.end(),
            [](const void* pointer) { return pointer != nullptr; });
    }
    bool Empty() const { return materialCount == 0 && pointers == AccessoryPayload{}.pointers; }
};
AccessoryPayload ReadAccessoryPayload(const mdl::AccessoryRecord& record) {
    return {{record.mesh, record.materials, record.texturePaths, record.textureTypes},
        record.materialCount};
}
struct V1AccessoryMaterialRetry {
    fs::path source, replacement;
    mdl::AccessoryRecord* record = nullptr;
    unsigned refusals = 0, prompts = 0;
    unsigned failedDisposals = 0, resetDisposals = 0;
    AccessoryPayload failed, disposedFailed, beforeRetry, loaded;
};
V1AccessoryMaterialRetry* v1MaterialRetry = nullptr;
enum class Fault { None, Memory, Runtime, AbortReset, ReadFailure, AbortResetFalse, AbortResetQuarantine, ReadResetQuarantine, ReadResetThrow, AccessoryResetFalse };
unsigned recoveryResetCalls = 0;
bool recoveryThrowsPersist = false, recoveryThrowsUnknown = false;
Fault fault = Fault::None;
bool monitor = false, reuse = false, inputOpen = false, sentinelLive = false, faultThrown = false;
bool expectedPictureError = false;
bool expectStandaloneMissingAccessory = false;
unsigned standaloneMissingAccessoryNotices = 0;
bool testCloseCancel = false;
bool testCloseAccept = false;
unsigned closeDialogs = 0;
// One-shot false return of the actual V2 missing-model Abort's ResetAppState
// call. The detour below stands in for the real reset exactly once, after the
// PMM stream inside the abort is closed and before any success tail, and writes
// a non-zero playhead: only the real ResetAppState (reset_app_state.cpp:232)
// clears that residue, so it shows whether the ignored boolean left the already
// replaced scene half-reset.
bool resetFalseInjected = false;
unsigned resetFalseRefusals = 0, expectedMissingAbortDialogs = 0;
std::int32_t resetFalseResidualFrame = 0;
constexpr std::int32_t kResetFalseResidualFrame = 4242;
// One-shot false return of the actual V1 accessory-retry ResetAppState call.
// Unlike the V2 Abort above, this loader inspects the boolean and raises
// SceneResetRefused, so the shell cancels the prepared transaction and runs the
// real reset once. The picker returns an absolute replacement path that is
// already gone, so the retry's own real LoadAccessoryObject fails as well.
// Every counter here stays independent from the V2 missing-model stages.
bool accessoryResetInjected = false;
// Persisting variant of the same one-shot refusal: the actual V1 accessory
// reset site, the shell's post-replacement retry and the pending-recovery gate
// before the next open all refuse. The real ResetAppState never runs, so the
// replaced scene keeps the real slot-0 accessory and the sentinel playhead
// until a later open clears the injection and runs the real reset. The once
// case above still refuses exactly once and forwards exactly one real reset.
bool accessoryResetPersist = false;
void* accessoryRetainedSlot = nullptr;
unsigned accessoryResetRefusals = 0, accessoryRecoveryForwards = 0;
unsigned accessoryPicks = 0, accessoryLoads = 0, accessoryLoadFailures = 0;
unsigned accessoryDialogs = 0;
std::int32_t accessoryResidualFrame = 0;
constexpr std::int32_t kAccessoryResidualFrame = 4343;
fs::path accessorySourcePath, accessorySubstitutePath;
wchar_t accessoryLoadPath[2][260]{};
// Second missing-model round: the actual V2 Abort's Reset and the bounded
// recovery retry both refuse, so the already replaced scene stays quarantined
// (MMDApp::sceneRecoveryPending) instead of being reset. Only a later real
// ResetAppState completes that quarantine; until then every real save route and
// the next open must refuse. The first refusal leaves a non-zero playhead so
// each later refusal can show that no real reset ran in between.
unsigned quarantineRefusals = 0, expectedQuarantineAbortDialogs = 0;
std::int32_t quarantineResidualFrame = 0;
constexpr std::int32_t kQuarantineResidualFrame = 5150;
// Real File > New (WM_COMMAND 204) failure-recovery regression. This mode is
// enabled only around direct MainWndProc calls; its counters stay independent
// from Fault, the missing-model dialogs and the exit stage. Every command
// resets them first.
bool newSceneMode = false;
enum class NewSceneReset { Disabled, False, Runtime, Unknown, Forward };
NewSceneReset newSceneReset = NewSceneReset::Disabled;
unsigned newSceneResetCalls = 0;
unsigned newSceneConfirmDialogs = 0, newSceneRefusalDialogs = 0;
unsigned newSceneEscapes = 0;
int newSceneConfirmReply = IDOK;
// One-shot modal callbacks for the File > New confirmation and refusal. The
// prompt hooks move the requested callback out and clear it before running it,
// so a callback may dispatch the real MainWndProc WM_COMMAND 204 entry again
// without recursing through itself. Each case arms at most one of them and
// PrepareNewSceneCommand disarms both.
std::function<void()> newSceneConfirmAction, newSceneRefusalAction;
// ---- Real pending-recovery gate before a new open (sceneRecoveryPending) -----
// The isolated gate mode is off by default. While armed, the single real
// ResetAppState call at LoadSceneFile's pending-recovery gate takes the
// requested outcome, records the actual host and caller language, and switches
// the UI language so only production can restore it. The failure actions are
// accumulated below so the unfixed production still runs every case in one RED
// pass; only the real Forward action runs the real reset.
enum class RecoveryGateReset { Disabled, False, Runtime, Unknown, StickyTrue, Forward };
bool recoveryGateMode = false;
RecoveryGateReset recoveryGateReset = RecoveryGateReset::Disabled;
unsigned recoveryGateResets = 0;
bool recoveryGateActualApp = false;
unsigned recoveryGateLanguageBefore = 0;
unsigned recoveryGateNotices = 0, recoveryGateNativeNotices = 0;
unsigned recoveryGateMonitorNotices[3] = {0, 0, 0};
unsigned recoveryGateMonitorNativeNotices[3] = {0, 0, 0};
int recoveryGateNoticeExpectedLanguage = -1, recoveryGateNoticeActualLanguage = -1;
bool recoveryGateNoticeTextOk = false, recoveryGateNoticeFlagsOk = false;
bool recoveryGateActualOwner = false, recoveryGateActualPending = false;
bool recoveryGateActualInputClosed = false;
bool recoveryGateNativeMessage = false;
unsigned recoveryGateNestedRuns = 0;
bool recoveryGateNestedBlocked = false;
fs::path recoveryGateReopenPath;
struct RecoveryGateNoticeText { const wchar_t* title; const wchar_t* body; };
// Fixed production text for UiMessage::ProjectRecoveryPending, indexed like
// kUiJapanese / kUiEnglish / kUiChinese. This test never builds the body or
// bypasses the real ShowUiMessage selection.
constexpr RecoveryGateNoticeText recoveryGateNoticeText[3] = {
    {L"プロジェクトの復旧", L"現在のシーンを復旧できていないため、編集や保存はできません。\n選択したプロジェクトの読み込みは開始していません。\n［ファイル］の［新規作成］または［開く］で復旧を再試行するか、アプリケーションを終了してください。"},
    {L"Project recovery", L"The current scene has not recovered and cannot be edited or saved.\nThe selected project has not started loading.\nUse File > New or File > Open to retry recovery, or exit the application."},
    {L"工程恢复", L"当前场景尚未恢复，暂不能编辑或保存。\n本次选择的工程尚未开始加载。\n可通过“文件 → 新建”或“文件 → 打开”重试恢复，或退出程序。"},
};
decltype(&ShowUiMessage) realShowUiMessage = nullptr;
// ---- Real post-load final-recovery feedback (RecoverSceneResetRefusal) -----
// The isolated feedback mode is off by default and shares no counter with the
// pending-recovery gate, File > New, or the old Fault injections. It arms only
// around a direct call to the production RecoverSceneResetRefusal so the test
// can observe the final-failure notice the helper owes when its bounded retry
// cannot restore an editable scene. The explicitly limited legacy mode reuses
// the same native observation for the two void loader entry points.
enum class PostloadReset { Disabled, False, Runtime, Unknown, StickyTrue, Forward };
bool postloadMode = false;
PostloadReset postloadReset = PostloadReset::Disabled;
unsigned postloadResets = 0;
bool postloadActualApp = false;
unsigned postloadLanguageBefore = 0;
int postloadExpectedLanguage = -1;
unsigned postloadNotices = 0, postloadNativeNotices = 0;
bool postloadLegacyMode = false;
bool postloadNativeMessage = false, postloadNativeObserved = false;
int postloadNoticeActualLanguage = -1;
bool postloadNoticeTextOk = false, postloadNoticeFlagsOk = false;
bool postloadNoticeOwnerOk = false, postloadNoticePending = false;
bool postloadNoticeInputClosed = false, postloadNoticeMutationGuard = false;
unsigned postloadNestedRuns = 0, postloadNestedPendingNotices = 0;
bool postloadNestedLoaded = true, postloadNestedEscaped = false;
bool postloadNestedBlocked = false, postloadNestedSceneIntact = false;
fs::path postloadReopenPath;
struct PostloadNoticeText { const wchar_t* title; const wchar_t* body; };
// Fixed production text for UiMessage::ProjectRecoveryFailed, the last accepted
// formatter entry, indexed like kUiJapanese / kUiEnglish / kUiChinese. This test
// never builds that body and never bypasses the real ShowUiMessage selection.
constexpr PostloadNoticeText postloadNoticeText[3] = {
    {L"プロジェクトの復旧", L"プロジェクトの読み込みは中止されましたが、編集可能な状態への復旧に失敗しました。\n以前のシーンは復元されておらず、編集や保存はできません。\n［ファイル］の［新規作成］または［開く］で復旧を再試行するか、アプリケーションを終了してください。"},
    {L"Project recovery", L"Project loading stopped, but the scene could not be restored to an editable state.\nThe previous scene was not restored; editing and saving are unavailable.\nUse File > New or File > Open to retry recovery, or exit the application."},
    {L"工程恢复", L"工程加载已停止，但未能恢复到可编辑状态。\n原先的场景未被还原，暂不能编辑或保存。\n可通过“文件 → 新建”或“文件 → 打开”重试恢复，或退出程序。"},
};
unsigned ResetActivity() {
    // Every reset path this file can reach counts here, so a nested open that a
    // missing guard lets through shows up even when another fault branch refuses
    // it before any read or success tail.
    return recoveryResetCalls + resetThrows + resetFalseRefusals + quarantineRefusals +
        accessoryResetRefusals + postloadResets + newSceneResetCalls + recoveryGateResets +
        (v2Accessory ? v2Accessory->normalAbortResets : 0) +
        (v2ModelAbortThrow ? v2ModelAbortThrow->resets : 0);
}
void ResetPostloadObservation(int expectedLanguage) {
    postloadExpectedLanguage = expectedLanguage;
    postloadNativeMessage = false;
    postloadNativeObserved = false;
    postloadNoticeActualLanguage = -1;
    postloadNoticeTextOk = false;
    postloadNoticeFlagsOk = false;
    postloadNoticeOwnerOk = false;
    postloadNoticePending = false;
    postloadNoticeInputClosed = false;
    postloadNoticeMutationGuard = false;
    postloadNestedRuns = 0;
    postloadNestedPendingNotices = 0;
    postloadNestedLoaded = true;
    postloadNestedEscaped = false;
    postloadNestedBlocked = false;
    postloadNestedSceneIntact = false;
}
void ArmPostload(PostloadReset action, int expectedLanguage) {
    postloadMode = true;
    postloadReset = action;
    postloadResets = 0;
    postloadActualApp = false;
    postloadLanguageBefore = 0;
    ResetPostloadObservation(expectedLanguage);
}
void DisarmPostload() {
    postloadMode = false;
    postloadReset = PostloadReset::Disabled;
    postloadNativeMessage = false;
    postloadExpectedLanguage = -1;
}
void ArmRecoveryGate(RecoveryGateReset action, int expectedLanguage) {
    recoveryGateReset = action;
    recoveryGateMode = true;
    recoveryGateResets = 0;
    recoveryGateNativeMessage = false;
    recoveryGateNestedRuns = 0;
    recoveryGateNestedBlocked = false;
    recoveryGateNoticeTextOk = false;
    recoveryGateNoticeFlagsOk = false;
    recoveryGateActualOwner = false;
    recoveryGateActualPending = false;
    recoveryGateActualInputClosed = false;
    recoveryGateNoticeExpectedLanguage = expectedLanguage;
    recoveryGateNoticeActualLanguage = -1;
}
void DisarmRecoveryGate() {
    recoveryGateMode = false;
    recoveryGateReset = RecoveryGateReset::Disabled;
    recoveryGateNativeMessage = false;
    recoveryGateNoticeExpectedLanguage = -1;
}
#if !MMD_ORIGINAL_PROFILE
bool failDeferredClosePost = false;
decltype(&PostMessageW) realPostMessageW = nullptr;
BOOL WINAPI ObservePostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (failDeferredClosePost && app && window == app->MainWindow() && message == kSceneCloseReplayMessage) {
        failDeferredClosePost = false;
        SetLastError(ERROR_NOT_ENOUGH_QUOTA);
        return FALSE;
    }
    return realPostMessageW(window, message, wparam, lparam);
}
#endif
bool tailObserved = false;
unsigned viewRefreshCalls = 0, preCloseRefreshCalls = 0, recoveryRefreshCalls = 0;
int inputDescriptor = -1, sentinelDescriptor = -1;
unsigned inputCloses = 0, sentinelCloses = 0;
int checkedAtClose = -2, checkedBefore = -1;
using Open = errno_t (__cdecl*)(int*, const wchar_t*, int, int, int);
using Close = int (__cdecl*)(int);
Open realOpen = nullptr;
Close realClose = nullptr;
using Read = int (__cdecl*)(int, void*, unsigned int);
using Seek = __int64 (__cdecl*)(int, __int64, int);
Read realRead = nullptr;
Seek realSeek = nullptr;
std::uint64_t inputReads = 0, bodyRewinds = 0, expectedBoundary = 0;
unsigned extensionCalls = 0;
bool insideExtension = false, refuseSync = false;
#if !MMD_ORIGINAL_PROFILE
decltype(&pmm_extension::Read) realExtensionRead = nullptr;
#endif
decltype(&ShowWindow) realShow = nullptr;
#if MMD_ORIGINAL_PROFILE
decltype(&PostViewRefresh) realViewRefresh = nullptr;
constexpr const char* tailSource = "PostViewRefresh in native SuccessTail";
#else
decltype(&native_mesh::ProjectOpened) realProjectOpened = nullptr;
constexpr const char* tailSource = "native_mesh::ProjectOpened";
#endif
decltype(&ResetAppState) realReset = nullptr;
// Main-window input regressions: the production entries are counted and
// forwarded, so a live handler cannot move the desktop cursor or load a file
// while the message gate is under test.
unsigned mouseMoves = 0, dropFiles = 0, dragFinishes = 0;
HDROP finishedDrop = nullptr;
decltype(&HandleMouseMove) realMouseMove = nullptr;
decltype(&HandleDropFiles) realDropFiles = nullptr;
decltype(&DragFinish) realDragFinish = nullptr;
decltype(&SetWindowPos) realSetWindowPos = nullptr;
decltype(&PanelPaint) realPanelPaint = nullptr;
decltype(&HandlePaletteChanged2) realFloatingPalette = nullptr;
unsigned panelPaints = 0, floatingPalettePaints = 0;
decltype(&PostQuitMessage) realPostQuit = nullptr;
unsigned quitPosts = 0;
int quitCode = -1;
void WINAPI ObservePostQuit(int code) { ++quitPosts; quitCode = code; realPostQuit(code); }

BOOL WINAPI HiddenPosition(HWND window, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!(GetWindowLongW(window, GWL_STYLE) & WS_CHILD))
        flags = (flags & ~SWP_SHOWWINDOW) | SWP_NOACTIVATE;
    return realSetWindowPos(window, after, x, y, cx, cy, flags);
}
void ObservePanelPaint(MMDApp* value) { ++panelPaints; realPanelPaint(value); }
void ObserveFloatingPalette(HDC dc) { ++floatingPalettePaints; realFloatingPalette(dc); }

void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); }
}
using RelocationTicket = project_relocation::Ticket;
constexpr wchar_t ownerRow[] = LR"(R:\old-owner\Effects\row.fx)";
constexpr wchar_t resolvedOwnerRow[] = LR"(S:\new-owner\Effects\row.fx)";
bool WriteThrough(const RelocationTicket& ticket) {
    return ticket.Confirm(project_model::Reference{{ownerRow}}, resolvedOwnerRow);
}
void CheckRetired(const RelocationTicket& ticket) {
    Check(!WriteThrough(ticket) && ticket.Infer(ownerRow).status == project_relocation::Status::NoMatch,
          "retired capture cannot learn or resolve even its own confirmed row");
}
RelocationTicket recoveryTicket, localizeTicket;
struct LocalizeFault {};
bool failFinalLocalize = false;
void CacheHostFinalLocalize(MMDApp*);
void CacheHostParserTail();
decltype(&LocalizeUI) realLocalize = nullptr;
void ObserveLocalize(MMDApp* value) {
    if (v2Accessory && v2Accessory->normalAbort && monitor) {
        // The shell's replaced-scene LocalizeUI runs after the production abort
        // reset. Its bounded refresh belongs to this exact real call scope, not
        // to the parser success tail or to any empty-path/global-state filter.
        ++v2Accessory->localizeCalls;
        struct AbortLocalizeScope {
            bool& flag;
            bool previous;
            explicit AbortLocalizeScope(bool& value)
                : flag(value), previous(value) { flag = true; }
            ~AbortLocalizeScope() { flag = previous; }
        } abortLocalizeScope(v2Accessory->inAbortLocalize);
        realLocalize(value);
    } else {
        realLocalize(value);
    }
    if (monitor && tailObserved && !inputOpen) CacheHostFinalLocalize(value);
    if (failFinalLocalize && monitor && tailObserved && !inputOpen && inputCloses == 1) {
        failFinalLocalize = false;
        localizeTicket = RelocationTicket::Capture(*value);
        Check(WriteThrough(localizeTicket), "post-parser localization still sees this tentative owner");
        throw LocalizeFault{};
    }
}
template<class Function, class Replacement>
void Hook(Function function, Replacement replacement, void** original = nullptr) {
    auto* address = reinterpret_cast<void*>(function);
    Check(MH_CreateHook(address, reinterpret_cast<void*>(replacement), original) == MH_OK,
        "create a passive observer or isolated fault hook");
    Check(MH_EnableHook(address) == MH_OK, "enable an isolated host hook");
}
#if !MMD_ORIGINAL_PROFILE
// Switchable observer of the real plugin frame boundary for the half-loaded
// scene regression. Unarmed it forwards plugins::PluginFrameBegin; armed it
// records the hit and throws its own type, so the two explicit FrameDriver
// calls below stop before plugin frame work, scene evaluation or render.
struct FrameBoundaryStop {};
bool frameBoundaryArmed = false;
unsigned frameBoundaryHits = 0;
decltype(&plugins::PluginFrameBegin) realFrameBegin = nullptr;
void ObserveFrameBegin(MMDApp* value) {
    if (!frameBoundaryArmed) { realFrameBegin(value); return; }
    ++frameBoundaryHits;
    throw FrameBoundaryStop{};
}
#endif
// The real entries run after the count; the observers prove the quarantined
// scene drops the message before them and the recovered scene still reaches
// them, without synthesizing OS mouse input.
void ObserveMouseMove(std::uint32_t lParam, int mouseY) {
    ++mouseMoves; realMouseMove(lParam, mouseY);
}
void ObserveDropFiles(HDROP drop) {
    ++dropFiles; realDropFiles(drop);
}
void WINAPI ObserveDragFinish(HDROP drop) {
    ++dragFinishes; finishedDrop = drop; realDragFinish(drop);
}
BOOL WINAPI Hidden(HWND window, int) { return realShow(window, SW_HIDE); }
LRESULT CALLBACK NoActivation(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        auto* creation = reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
        if (!(creation->style & WS_CHILD)) creation->style &= ~WS_VISIBLE;
    }
    if (code == HCBT_ACTIVATE) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}
errno_t __cdecl ObserveOpen(int* descriptor, const wchar_t* path, int flags, int sharing, int mode) {
    const auto status = realOpen(descriptor, path, flags, sharing, mode);
    // Observe the shell's exact read-only binary open, not a hash/sidecar
    // std::ifstream used by plugin pair preparation before that open.
    if (monitor && status == 0 && flags == 0x8000 && sharing == 0x40 && mode == 0x80 &&
        path && !std::wcscmp(path, candidate.c_str())) {
        Check(inputDescriptor == -1 && descriptor && *descriptor >= 0,
            "observe the actual shell PMM open once without replacing the parser");
        inputDescriptor = *descriptor; inputOpen = true;
    }
    return status;
}
int __cdecl ObserveClose(int descriptor) {
    if (monitor && descriptor == inputDescriptor) {
        if (inputOpen) {
            ++inputCloses; checkedAtClose = pmm_io::checkedDescriptor; inputOpen = false;
        } else if (sentinelLive) ++sentinelCloses;
    }
    // An invalid close is deliberately forwarded, so a CRT invalid-parameter
    // failure still fails the child process. No handler masks the defect.
    return realClose(descriptor);
}
int __cdecl ObserveRead(int descriptor, void* data, unsigned int size) {
    if (monitor && inputOpen && descriptor == inputDescriptor && !insideExtension) ++inputReads;
    return realRead(descriptor, data, size);
}
__int64 __cdecl ObserveSeek(int descriptor, __int64 offset, int origin) {
    const auto* state = monitor && inputOpen && descriptor == inputDescriptor && !insideExtension
        ? pmm_io::FindReadState(descriptor) : nullptr;
    const bool handoff = state && state->buffered && state->cursor < state->filled &&
        origin == SEEK_SET && offset == state->logical;
    if (handoff && refuseSync) { ++bodyRewinds; errno = EIO; return -1; }
    const auto result = realSeek(descriptor, offset, origin);
    if (handoff && result >= 0) ++bodyRewinds;
    return result;
}
#if !MMD_ORIGINAL_PROFILE
pmm_extension::ReadResult ObserveExtensionRead(int descriptor, pmm_extension::Document& document, std::uint64_t& standard) {
    if (!monitor || !inputOpen || descriptor != inputDescriptor) return realExtensionRead(descriptor, document, standard);
    const auto position = _telli64(descriptor);
    Check(!refuseSync && position == pmm_io::Position(descriptor) &&
        (!expectedBoundary || position == static_cast<__int64>(expectedBoundary)),
        "the genuine extension reader receives the same fd at the consumed standard-body boundary");
    ++extensionCalls;
    struct Scope { Scope() { insideExtension = true; } ~Scope() { insideExtension = false; } } scope;
    const auto result = realExtensionRead(descriptor, document, standard);
    Check(_telli64(descriptor) == position && pmm_io::Position(descriptor) == position,
        "the real extension reader restores the entry position after every normal result");
    if (result == pmm_extension::ReadResult::Loaded && expectedBoundary)
        Check(standard == expectedBoundary, "the decoded MMS standard-size field matches the actual parser boundary");
    return result;
}
#endif
enum class MotionDialogAction { None, Accept, Cancel, SwitchModel, SwitchFrame, ResetScene, SwitchAtConfirmation };
MotionDialogAction motionDialogAction = MotionDialogAction::None;
fs::path motionDialogPath;
unsigned motionPicks = 0, motionNotices = 0;
unsigned char motionOtherSlot = 0;
const wchar_t* expectedMotionSaveNotice = nullptr;
std::function<int(UiMessage, const wchar_t*)> preflightOpenNotice;
bool CacheSaveFailureMessage(HWND, LPCWSTR, LPCWSTR, UINT);
int WINAPI ErrorW(HWND window, LPCWSTR text, LPCWSTR caption, UINT flags) {
    if (motionDialogAction != MotionDialogAction::None && caption && !std::wcscmp(caption, L"VMD")) {
        Check(window == app->MainWindow() && expectedMotionSaveNotice && text &&
              std::wcsstr(text, expectedMotionSaveNotice) && (flags & MB_ICONERROR),
              "the production export reports the expected VMD failure to the owner");
        ++motionNotices;
        return IDOK;
    }
    if (motionDialogAction != MotionDialogAction::None && caption && !std::wcscmp(caption, L"Load motion data")) {
        ++motionNotices;
        if (motionDialogAction == MotionDialogAction::SwitchAtConfirmation && (flags & MB_OKCANCEL))
            app->SetSelectedModelSlot(motionOtherSlot);
        return IDOK;
    }
    if (CacheSaveFailureMessage(window, text, caption, flags)) return IDOK;
    if (expectStandaloneMissingAccessory) {
        Check(window == app->MainWindow() && flags == MB_OK && text && caption &&
              !std::wcscmp(caption, L"load accessory") && std::wcsstr(text, L"Cannot find xfile!!"),
            "the standalone X importer keeps its own missing-file notice");
        ++standaloneMissingAccessoryNotices;
        return IDOK;
    }
    if (postloadNativeMessage && !postloadNativeObserved) {
        // Scoped to the exact notice the feedback mode forwarded through the
        // real formatter. Only here are the production-selected native text,
        // flags, owner, language and quarantine state read; the notice is then
        // answered IDOK without a synthetic desktop click.
        postloadNativeObserved = true;
        ++postloadNativeNotices;
        const unsigned language = app && app->EnglishUI() <= 2 ? app->EnglishUI() : 1;
        postloadNoticeActualLanguage = int(language);
        const int expected = postloadExpectedLanguage >= 0
            ? postloadExpectedLanguage : int(language);
        postloadNoticeOwnerOk = app && window == app->MainWindow();
        postloadNoticeFlagsOk = flags == (MB_OK | MB_ICONWARNING);
        postloadNoticeTextOk = postloadNoticeOwnerOk && text && caption &&
            !std::wcscmp(caption, postloadNoticeText[expected].title) &&
            !std::wcscmp(text, postloadNoticeText[expected].body);
        postloadNoticePending = app && app->sceneRecoveryPending;
        postloadNoticeInputClosed = !inputOpen;
        postloadNoticeMutationGuard = app && app->SceneMutationInProgress();
        // One nested real load from inside the notice. The ScopedSceneMutation
        // protection production adds around the final-failure notice must refuse
        // it before any reset, read, close or success tail; any exception is
        // recorded rather than swallowed.
        void* const modelBefore = app ? static_cast<void*>(app->ModelSlot(0)) : nullptr;
        void* const accessoryBefore = app ? static_cast<void*>(app->AccessorySlot(0)) : nullptr;
        mdl::CameraKey* const cameraKeysBefore = app ? app->CameraKeys() : nullptr;
        mdl::LightKey* const lightKeysBefore = app ? app->LightKeys() : nullptr;
        const std::int32_t frameBefore = app ? app->CurrentFrame() : 0;
        const std::uint64_t generationBefore = app ? app->SceneGeneration() : 0;
        ++postloadNestedRuns;
        const auto resetsBefore = ResetActivity();
        const auto readsBefore = inputReads;
        const auto closesBefore = inputCloses;
        const auto tailsBefore = tailCalls;
        bool nestedLoaded = true, nestedEscaped = false;
        try { nestedLoaded = LoadSceneFile(postloadReopenPath.c_str()); }
        catch (...) { nestedEscaped = true; }
        postloadNestedLoaded = nestedLoaded;
        postloadNestedEscaped = nestedEscaped;
        postloadNestedBlocked = !nestedLoaded && !nestedEscaped &&
            ResetActivity() == resetsBefore && inputReads == readsBefore &&
            inputCloses == closesBefore && tailCalls == tailsBefore;
        postloadNestedSceneIntact = app &&
            static_cast<void*>(app->ModelSlot(0)) == modelBefore &&
            static_cast<void*>(app->AccessorySlot(0)) == accessoryBefore &&
            app->CameraKeys() == cameraKeysBefore && app->LightKeys() == lightKeysBefore &&
            app->CurrentFrame() == frameBefore && app->SceneGeneration() == generationBefore;
        return IDOK;
    }
    if (recoveryGateNativeMessage) {
        recoveryGateNativeMessage = false;
        ++recoveryGateNativeNotices;
        const unsigned language = app && app->EnglishUI() <= 2 ? app->EnglishUI() : 1;
        recoveryGateNoticeActualLanguage = int(language);
        const int expected = recoveryGateNoticeExpectedLanguage >= 0
            ? recoveryGateNoticeExpectedLanguage : int(language);
        recoveryGateActualOwner = window == app->MainWindow();
        recoveryGateNoticeFlagsOk = flags == (MB_OK | MB_ICONWARNING);
        recoveryGateNoticeTextOk = window == app->MainWindow() && text && caption &&
            !std::wcscmp(caption, recoveryGateNoticeText[expected].title) &&
            !std::wcscmp(text, recoveryGateNoticeText[expected].body);
        if (recoveryGateMode) {
            // Controlled modal callback, not a synthetic desktop click: a second
            // real load from inside the notice must be refused by the open
            // reentrancy guard before any reset, read or success tail.
            ++recoveryGateNestedRuns;
            const auto resets = recoveryGateResets;
            const auto reads = inputReads;
            const auto tails = tailCalls;
            bool nestedLoaded = true, nestedEscaped = false;
            try { nestedLoaded = LoadSceneFile(recoveryGateReopenPath.c_str()); }
            catch (...) { nestedEscaped = true; }
            recoveryGateNestedBlocked = !nestedLoaded && !nestedEscaped &&
                recoveryGateResets == resets && inputReads == reads && tailCalls == tails;
        }
        return IDOK;
    }
    if (v2Accessory && v2Accessory->partialFailureSource) {
        auto& d = *v2Accessory;
        auto* record = app->AccessorySlot(0);
        Check(monitor && inputOpen && d.loadObjects.size() == 1 &&
              d.loadObjects.front() == record && d.loadResults.empty() &&
              !d.prompts && record && ReadAccessoryPayload(*record).Allocated() &&
              fs::path(record->sourcePath) == *d.partialFailureSource &&
              window == app->MainWindow() && text && caption && !std::wcscmp(caption, L"MMD") &&
              flags == (MB_OK | MB_ICONERROR) &&
              std::wcsstr(text, L"exceeds the CP932 character or field-length limits") &&
              ++d.partialRefusals == 1,
            "the original profile rejects the V2 source texture name after real mesh and material acquisition");
        return IDOK;
    }
    if (v1MaterialRetry) {
        auto& retry = *v1MaterialRetry;
        Check(monitor && inputOpen && accessoryLoads == 1 && accessoryPicks == 0 &&
              window == app->MainWindow() && text && caption,
            "V1 material retry reports its compatibility refusal during the first load");
        Check(++retry.refusals == 1 && retry.prompts == 0 &&
              !std::wcscmp(caption, L"MMD") && flags == (MB_OK | MB_ICONERROR) &&
              std::wcsstr(text, L"exceeds the CP932 character or field-length limits"),
            "the real original-profile import refuses the texture filename");
        return IDOK;
    }
    if (expectedPictureError) { ++messages; return IDOK; }
    ++messages;
    if (!monitor || fault != Fault::Memory || !faultThrown) {
        std::fwprintf(stderr, L"Unexpected PMM modal: %ls / %ls\n", caption ? caption : L"", text ? text : L"");
        Check(false, "only the requested post-close allocation error may show the native PMM memory message");
    }
    return IDOK;
}
int WINAPI ErrorA(HWND window, LPCSTR text, LPCSTR caption, UINT flags) {
    if (relocationDialogs && relocationDialogs->expectBadModel && !relocationDialogs->sawBadModel &&
        relocationDialogs->prompts == 1 && relocationDialogs->picks == 1 && monitor && inputOpen &&
        fault == Fault::None && window == app->MainWindow() && flags == MB_OK && text && caption &&
        !std::strcmp(caption, "open file") && !std::strcmp(text, "This file is not for Ploygon Movie Maker !!")) {
        relocationDialogs->sawBadModel = true;
        return IDOK;
    }
    if (expectedPictureError) { ++messages; return IDOK; }
    std::fprintf(stderr, "Unexpected PMM ANSI modal: %s / %s\n", caption ? caption : "", text ? text : "");
    Check(false, "no unexpected native ANSI dialog blocks the hidden fixture"); return IDOK;
}
int ErrorMessage(MMDApp* value, HWND window, UiMessage message, UINT flags,
                 const wchar_t* detail) {
    if (preflightOpenNotice) return preflightOpenNotice(message, detail);
    if (v2Accessory && v2Accessory->duplicateSlot) {
        Check(monitor && value == app && window == app->MainWindow() && !inputOpen &&
              message == UiMessage::InvalidProjectFile && flags == (MB_OK | MB_ICONERROR) &&
              detail && fs::path(detail) == candidate && app->EnglishUI() == kUiChinese,
              "the duplicate-slot load reports the real invalid PMM with the caller's path and language after closing input");
        ++v2Accessory->invalidNotices;
        return IDOK;
    }
    if (newSceneMode) {
        // Only the File > New confirmation and its refusal may run in this
        // mode; any other prompt keeps the existing strict policy below.
        Check(value == app && window == app->MainWindow(),
            "each real File > New prompt belongs to the actual hidden main host");
        Check(app->EnglishUI() == 2,
            "the File > New prompt keeps the caller language captured before the command");
        if (message == UiMessage::NewScene) {
            ++newSceneConfirmDialogs;
            auto action = std::move(newSceneConfirmAction);
            newSceneConfirmAction = nullptr;
            if (action) action();
            return newSceneConfirmReply;
        }
        if (message == UiMessage::NewSceneRefused) {
            ++newSceneRefusalDialogs;
            auto action = std::move(newSceneRefusalAction);
            newSceneRefusalAction = nullptr;
            if (action) action();
            return IDOK;
        }
        std::fprintf(stderr, "DIAG PMM new-scene unexpected modal=%d\n", static_cast<int>(message));
        Check(false, "the File > New regression accepts only its own confirmation and refusal prompts");
        return IDOK;
    }
    if (message == UiMessage::ProjectRecoveryFailed) {
        // The independent final-recovery notice. Only the explicit post-load
        // feedback mode, the limited legacy caller context, or the four monitor
        // failures that genuinely end in a refused final recovery may show it;
        // every other context stays an unexpected modal.
        const bool monitorFinalFailure = monitor && (fault == Fault::AbortResetQuarantine ||
            fault == Fault::ReadResetQuarantine ||
            (fault == Fault::ReadResetThrow && recoveryThrowsPersist) ||
            (fault == Fault::AccessoryResetFalse && accessoryResetPersist) ||
            (v2Accessory && v2Accessory->retryAbortReset != V2AbortReset::Forward && v2Accessory->normalAbortResets == 2));
        const bool observe = postloadMode || postloadLegacyMode || v2ModelAbortThrow ||
            (v2Accessory && v2Accessory->observeFinalAbortNotice);
        if (!observe && !monitorFinalFailure) {
            Check(false,
                "the final-recovery notice is allowed only in the explicit post-load feedback mode, the limited legacy caller context, or the four genuine final-recovery failures");
            return IDOK;
        }
        Check(value == app && window == app->MainWindow(),
            "the final-recovery notice belongs to the actual hidden host and its main window");
        if (v2Accessory && v2Accessory->retryAbortReset != V2AbortReset::Forward)
            Check(value->sceneRecoveryPending && value->SceneMutationInProgress() && !inputOpen &&
                  value->EnglishUI() == v2Accessory->abortLanguage && flags == (MB_OK | MB_ICONWARNING),
                "the accessory final notice sees the closed input, active isolation and caller language");
        ++postloadNotices;
        if (!observe) return IDOK;
        // Forward the real formatter and dialog. The temporary flag scopes the
        // native MessageBoxW hook to this exact notice so it reads the title,
        // body, flags, owner, language and quarantine state production chose.
        postloadNativeMessage = true;
        const int notice = realShowUiMessage(value, window, message, flags, detail);
        postloadNativeMessage = false;
        return notice;
    }
    if (postloadNativeMessage && message == UiMessage::ProjectRecoveryPending) {
        // A production that has not yet added the ScopedSceneMutation protection
        // lets the nested load inside the new final-failure notice reach the
        // pending gate. Record it instead of aborting the delayed pass; the
        // missing guard is already a target difference through postloadNestedBlocked.
        ++postloadNestedPendingNotices;
        return IDOK;
    }
    if (message == UiMessage::ProjectRecoveryPending) {
        // Only the armed pending-gate fixture or its three existing refusing
        // monitor reopens may show this notice. Every other context stays an
        // unexpected-modal failure through the strict policy below.
        const bool gateContext = recoveryGateMode ||
            (monitor && (fault == Fault::AbortResetQuarantine ||
                         fault == Fault::ReadResetQuarantine ||
                         fault == Fault::AccessoryResetFalse));
        if (!gateContext) {
            Check(false,
                "the recovery-pending notice is allowed only at the pending-recovery gate and its three refusing monitor reopens");
            return IDOK;
        }
        // Its old error counters stay untouched: the separate notice counters
        // below are what the gate regression checks.
        Check(value == app && window == app->MainWindow(),
            "the recovery-pending notice belongs to the actual hidden host and its main window");
        Check(flags == (MB_OK | MB_ICONWARNING),
            "the recovery-pending notice keeps the fixed warning flags");
        Check(app->sceneRecoveryPending && !inputOpen,
            "the recovery-pending notice is shown only for a still-quarantined scene with no open input");
        recoveryGateActualOwner = value == app && window == app->MainWindow();
        recoveryGateNoticeFlagsOk = flags == (MB_OK | MB_ICONWARNING);
        recoveryGateActualPending = app->sceneRecoveryPending;
        recoveryGateActualInputClosed = !inputOpen;
        ++recoveryGateNotices;
        // Forward the real formatter and dialog. The temporary flag scopes the
        // native MessageBoxW hook to this exact notice; that hook returns IDOK
        // only here and reads the title and body production actually selected.
        recoveryGateNativeMessage = true;
        const int notice = realShowUiMessage(value, window, message, flags, detail);
        recoveryGateNativeMessage = false;
        return notice;
    }
    if (testCloseAccept && message == UiMessage::QuitUnsaved) {
        Check(app->sceneRecoveryPending && window == app->MainWindow(),
            "the exit confirmation belongs to the actually quarantined main host");
        ++closeDialogs;
        return IDOK;
    }
    if (testCloseCancel && message == UiMessage::QuitUnsaved) {
        ++closeDialogs;
        SendMessageW(window, WM_CLOSE, 0, 0);
        return IDCANCEL;
    }
    ++messages;
    if (v2ModelAbortThrow && message == UiMessage::InvalidProjectFile) {
        ++v2ModelAbortThrow->invalidNotices;
        return IDOK;
    }
    const auto expected = (fault == Fault::ReadFailure || fault == Fault::ReadResetQuarantine || fault == Fault::ReadResetThrow)
        ? UiMessage::IncompleteProjectFile : UiMessage::InvalidProjectFile;
    if (monitor && fault == Fault::ReadFailure && message == expected) faultThrown = true;
    Check(monitor && faultThrown && (fault == Fault::Runtime || fault == Fault::AbortReset || fault == Fault::ReadFailure || fault == Fault::ReadResetQuarantine || fault == Fault::ReadResetThrow) &&
        message == expected, "the real shell reports each post-close fault through its exact existing error branch");
    return IDOK;
}
INT_PTR MissingModel(MMDApp* value, HINSTANCE, HWND window, ProjectModelIssue issue, const wchar_t* label) {
    if (v1MaterialRetry) {
        auto& retry = *v1MaterialRetry;
        Check(value == app && window == app->MainWindow() && monitor && inputOpen &&
              issue == ProjectModelIssue::MissingAccessory && label &&
              !std::wcscmp(label, L"material-retry") && accessoryLoads == 1 &&
              retry.refusals == 1 && ++retry.prompts == 1,
            "V1 offers accessory recovery after the real compatibility refusal");
        return 1;
    }
    if (monitor && fault == Fault::AccessoryResetFalse) {
        Check(value == app && window == app->MainWindow() && inputOpen &&
              issue == ProjectModelIssue::MissingAccessory && label && !std::wcscmp(label, L"accessory") &&
              accessoryDialogs < 2 && accessoryLoadFailures == accessoryDialogs + 1,
            "the V1 project recovery prompt follows each real malformed X");
        return ++accessoryDialogs == 1 ? 1 : 4;
    }
    if (relocationDialogs) {
        auto& d = *relocationDialogs;
        const bool terminal = d.picks == d.choices.size();
        Check(value == app && window == app->MainWindow() && monitor && inputOpen &&
              issue == ProjectModelIssue::MissingV1Model && label && !std::wcscmp(label, L"relocation") &&
              d.prompts == d.picks && (!terminal || d.terminalAnswer != 1),
              "the real V1 loader requests the original model with its exact name");
        ++d.prompts;
        if (d.expectAbort && d.prompts == 2) {
            const auto* first = mdl::Mdl(app->ModelSlot(0));
            Check(first && first->vertexCount == 3, "cancellation follows a successful real model replacement");
            d.ticket = project_relocation::Ticket::Capture(*app);
            Check(WriteThrough(d.ticket), "the accepted V1 load owns a tentative relocation before cancellation");
        }
        // Classify the upcoming reset as an abort in the original-profile
        // observer, which uses the same readout boundary as the success tail.
        if (terminal) faultThrown = true;
        return terminal ? d.terminalAnswer : 1;
    }
    if (v2Accessory && (!v2Relocation || issue == ProjectModelIssue::MissingAccessory)) {
        // The isolated missing-accessory group uses this real project dialog
        // hook; every reply comes from the current scenario, so a prompt at an
        // unexpected point fails instead of being answered implicitly.
        auto& d = *v2Accessory;
        if (d.projectRoot)
            Check(!std::wcscmp(app->PathWorkspace().projectDirectory, d.projectRoot),
                "a malformed replacement must leave the PMM resource root intact at the next accessory prompt");
        if (d.originalAccessoryDirectory)
            Check(!std::wcscmp(app->DirAccs(), d.originalAccessoryDirectory),
                "a malformed replacement must leave the previous accessory picker folder intact");
        Check(value == app && window == app->MainWindow() && monitor && inputOpen &&
              fault == Fault::None && !d.awaitingPicker && d.next < d.replies.size() &&
              issue == ProjectModelIssue::MissingAccessory,
            "the V2 accessory recovery prompts only for the expected missing accessory inside the active PMM load");
        const auto* expectedLabel = d.next && d.followingLabel ? d.followingLabel : d.expectedLabel;
        if (!label || !expectedLabel || std::wcscmp(label, expectedLabel))
            std::fprintf(stderr, "Accessory recovery prompt %zu actual=%ls expected=%ls\n", d.next,
                         label ? label : L"(null)", expectedLabel ? expectedLabel : L"(null)");
        Check(label && expectedLabel && !std::wcscmp(label, expectedLabel),
            "the V2 accessory recovery prompt carries the original accessory name");
        if (d.failedObject != nullptr) {
            // The production recovery loop owes DisposeAccessory + reset before
            // it asks again after a real X parse failure. Check the exact record
            // the failed load used; do not synthesize this cleanup.
            const auto& record = *mdl::Accessory(d.failedObject);
            Check(record.mesh == nullptr && record.materials == nullptr &&
                  record.texturePaths == nullptr && record.textureTypes == nullptr &&
                  record.sourcePath[0] == L'\0',
                "the failed X attempt is discarded before the recovery prompt");
            if (d.partialFailureSource)
                Check(d.partialDisposals == 1 && record.materialCount == 0,
                    "the V2 source failure disposes its acquired payload once and resets the count before prompting");
            d.failedObject = nullptr;
        }
        const auto& reply = d.replies[d.next++];
        ++d.prompts;
        if (reply.answer == 1) d.awaitingPicker = &reply;
        if (reply.answer == 4 || reply.answer == 2) d.expectNormalAbort = true;
        return reply.answer;
    }
    if (v2Relocation) {
        auto& d = *v2Relocation;
        if (d.projectRoot)
            Check(!std::wcscmp(app->PathWorkspace().projectDirectory, d.projectRoot),
                "a model replacement must leave the PMM resource root intact at later model prompts");
        if (issue == ProjectModelIssue::StructureMismatch && d.originalModelDirectory)
            Check(!std::wcscmp(app->DirModel(), d.originalModelDirectory),
                "a mismatched candidate must leave the previous model picker folder intact");
        Check(value == app && window == app->MainWindow() && monitor && inputOpen && fault == Fault::None &&
              !d.awaitingPicker && d.next < d.replies.size(), "V2 recovery dialog in the expected active PMM load");
        const auto& reply = d.replies[d.next++];
        if (issue != reply.issue || !label || std::wcscmp(label, reply.label))
            std::fprintf(stderr, "V2 dialog %zu actual=%d/%ls expected=%d/%ls\n", d.next,
                         int(issue), label ? label : L"(null)", int(reply.issue), reply.label);
        Check(issue == reply.issue && label && !std::wcscmp(label, reply.label), "V2 dialog matches exact model and issue");
        if ((issue == ProjectModelIssue::MissingFile && reply.answer == 1) ||
            (issue == ProjectModelIssue::StructureMismatch && reply.answer == 3)) d.awaitingPicker = &reply;
        return reply.answer;
    }
    Check(monitor && (v2ModelAbortThrow || fault == Fault::AbortReset || fault == Fault::AbortResetFalse ||
        fault == Fault::AbortResetQuarantine) && issue == ProjectModelIssue::MissingFile,
        "the real V2 model loader reaches its missing-source abort dialog");
    if (fault == Fault::AbortResetFalse) {
        recoveryTicket = RelocationTicket::Capture(*app);
        Check(WriteThrough(recoveryTicket), "record has a working owner before selecting Abort");
    }
    ++missingDialogs; return 2;
}
void ReuseInputNumber() {
    Check(inputDescriptor >= 0 && !inputOpen && inputCloses == 1 && !sentinelLive,
        "the real native body has consumed the PMM stream before reusing its descriptor number");
    int descriptor = -1;
    Check(_wsopen_s(&descriptor, sentinelPath.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_TRUNC,
        _SH_DENYNO, _S_IREAD | _S_IWRITE) == 0 && descriptor >= 0, "open an independent real CRT sentinel");
    if (descriptor != inputDescriptor) {
        Check(_dup2(descriptor, inputDescriptor) == 0, "force the sentinel into the exact consumed PMM descriptor slot");
        Check(_close(descriptor) == 0, "close only the extra sentinel duplicate");
        descriptor = inputDescriptor;
    }
    sentinelDescriptor = descriptor; sentinelLive = true;
    Check(sentinelDescriptor == inputDescriptor, "the post-close sentinel uses the actual observed shell descriptor number");
}
void TailDiagnostic(const char* stage, MMDApp* value, const wchar_t* path) {
    const bool appMatches = value == app;
    const bool pathMatches = path && !std::wcscmp(path, candidate.c_str());
    std::fprintf(stderr,
        "DIAG PMM tail stage=%s source=%s app=%p expectedApp=%p appMatches=%u "
        "path=%p pathMatches=%u descriptor=%d inputOpen=%u inputCloses=%u "
        "checkedAtClose=%d checkedBefore=%d checkedNow=%d monitor=%u fault=%u "
        "faultThrown=%u tailObserved=%u sentinelLive=%u sentinelCloses=%u "
        "viewRefreshCalls=%u preCloseRefreshCalls=%u recoveryRefreshCalls=%u\n",
        stage, tailSource, static_cast<void*>(value), static_cast<void*>(app), unsigned(appMatches),
        static_cast<const void*>(path), unsigned(pathMatches), inputDescriptor, unsigned(inputOpen), inputCloses,
        checkedAtClose, checkedBefore, pmm_io::checkedDescriptor, unsigned(monitor), unsigned(fault),
        unsigned(faultThrown), unsigned(tailObserved), unsigned(sentinelLive), sentinelCloses,
        viewRefreshCalls, preCloseRefreshCalls, recoveryRefreshCalls);
    std::fprintf(stderr, "DIAG PMM tail actualPath=%ls candidate=%ls\n", path ? path : L"(null)", candidate.c_str());
}
void PostCloseTail(MMDApp* value, const wchar_t* path) {
    ++tailCalls;
    const bool valid = value == app && path && !std::wcscmp(path, candidate.c_str()) && inputDescriptor >= 0 &&
        !inputOpen && inputCloses == 1 && checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore;
    if (!valid) TailDiagnostic("post-close assertion", value, path);
    Check(valid,
#if MMD_ORIGINAL_PROFILE
        "the real PMM parser reaches SuccessTail PostViewRefresh after its sole close and TLS read-context release");
#else
        "the real PMM parser reaches ProjectOpened after its sole close and TLS read-context release");
#endif
    tailObserved = true;
    CacheHostParserTail();
    if (fault == Fault::Memory || fault == Fault::Runtime) {
        if (fault == Fault::Runtime) {
            recoveryTicket = RelocationTicket::Capture(*value);
            Check(WriteThrough(recoveryTicket), "real success-tail hook holds the tentative owner before throwing");
        }
        if (reuse) ReuseInputNumber();
        faultThrown = true;
        if (fault == Fault::Memory) throw std::bad_alloc();
        throw std::runtime_error(tailSource);
    }
    Check(fault == Fault::None, "a missing-model abort cannot accidentally reach the successful tail");
}
#if MMD_ORIGINAL_PROFILE
void ViewRefresh(MMDApp* value) {
    // Both original-profile ProjectOpened notifications and recent-project
    // Record are empty and can share one ICF RET with unrelated stubs. Keep
    // a real, non-noexcept C++ call boundary with its exact one-argument ABI.
    realViewRefresh(value);
    if (!monitor || value != app) return;
    ++viewRefreshCalls;
    if (v2Accessory && (v2Accessory->inRealAbortReset || v2Accessory->inAbortLocalize)) {
        // The explicit cancel/close answers call the real ResetAppState and then
        // the shell's real LocalizeUI on the replaced scene. Classify a refresh
        // only while one of those exact production call scopes is active: not
        // by an empty path, not by global normalAbort state, not by faultThrown,
        // and never as the parser success tail.
        if (v2Accessory->inRealAbortReset)
            ++v2Accessory->abortResetRefreshes;
        else
            ++v2Accessory->abortLocalizeRefreshes;
        Check(!inputOpen && inputCloses == 1 && checkedAtClose == checkedBefore &&
              pmm_io::checkedDescriptor == checkedBefore,
            "the normal accessory abort refresh runs only after the real PMM input is closed and the TLS read context is released");
        Check(!tailObserved,
            "the normal accessory abort refresh is classified by the exact real ResetAppState or LocalizeUI call scope, never as the parser success tail");
        return;
    }
    // Reset's error-recovery readout is not the parser success tail. The
    // missing-model case faults Reset itself before this readout can run.
    if (faultThrown) { ++recoveryRefreshCalls; return; }
    if (inputOpen || inputCloses != 1) { ++preCloseRefreshCalls; return; }
    if (!tailObserved) PostCloseTail(value, value->EnvFileName());
}
#else
void ProjectOpened(MMDApp* value, const wchar_t* path) {
    if (monitor) PostCloseTail(value, path);
    realProjectOpened(value, path);
}
#endif
bool Reset(MMDApp* value) {
    if (postloadMode) {
        // The dedicated final-recovery retry action runs before every old Fault
        // branch. It records the actual host and caller language, then switches
        // the language so only production can restore it. StickyTrue reports
        // success without clearing the quarantine, matching the defect where the
        // retry value was mistaken for a completed reset.
        ++postloadResets;
        postloadActualApp = value == app;
        if (!value) return false;
        postloadLanguageBefore = value->EnglishUI();
        value->EnglishUI() = static_cast<unsigned char>((postloadLanguageBefore + 1) % 3);
        if (postloadReset == PostloadReset::False) return false;
        if (postloadReset == PostloadReset::Runtime)
            throw std::runtime_error("post-load recovery reset failed");
        if (postloadReset == PostloadReset::Unknown) throw 17;
        if (postloadReset == PostloadReset::StickyTrue) return true;
        Check(postloadReset == PostloadReset::Forward,
            "the post-load feedback mode selects one reset outcome first");
        const bool completed = realReset(value);
        value->EnglishUI() = static_cast<unsigned char>((postloadLanguageBefore + 2) % 3);
        return completed;
    }
    if (recoveryGateMode) {
        // The dedicated pending-gate action runs before every old Fault branch:
        // the fixture keeps its previous fault value and still must not take it
        // here. Each call records the actual host and caller language, then
        // switches the language to a different valid value so only production
        // can restore it. No scene data or isolation flag is touched.
        ++recoveryGateResets;
        recoveryGateActualApp = value == app;
        if (!value) return false;
        recoveryGateLanguageBefore = value->EnglishUI();
        value->EnglishUI() = static_cast<unsigned char>((recoveryGateLanguageBefore + 1) % 3);
        if (recoveryGateReset == RecoveryGateReset::False) return false;
        if (recoveryGateReset == RecoveryGateReset::Runtime)
            throw std::runtime_error("pending-recovery gate reset failed");
        if (recoveryGateReset == RecoveryGateReset::Unknown) throw 17;
        if (recoveryGateReset == RecoveryGateReset::StickyTrue) return true;
        Check(recoveryGateReset == RecoveryGateReset::Forward,
            "the pending-recovery gate selects one reset outcome first");
        const bool completed = realReset(value);
        value->EnglishUI() = static_cast<unsigned char>((recoveryGateLanguageBefore + 2) % 3);
        return completed;
    }
    if (monitor && (fault == Fault::AbortResetFalse || fault == Fault::Runtime)) {
        CheckRetired(recoveryTicket);
        CheckRetired(RelocationTicket::Capture(*value));
    }
    if (newSceneMode) {
        // The real CmdResetState calls this after the File > New confirmation.
        // Record the attempt, then take the one outcome the current case asks
        // for. The throwing outcomes break the caller language first so the
        // prompt hook can prove production recovery restored it.
        ++newSceneResetCalls;
        Check(value == app, "the File > New reset hook receives the actual host");
        if (newSceneReset == NewSceneReset::False) return false;
        if (newSceneReset == NewSceneReset::Forward) return realReset(value);
        Check(newSceneReset == NewSceneReset::Runtime || newSceneReset == NewSceneReset::Unknown,
            "each File > New command selects one reset outcome first");
        value->EnglishUI() = 0;
        if (newSceneReset == NewSceneReset::Unknown) throw 17;
        throw std::runtime_error("new-scene recovery reset failed");
    }
    if (monitor && fault == Fault::ReadResetThrow) {
        Check(value == app && !tailObserved && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            value->EnglishUI() == 2 && value->sceneRecoveryPending == (recoveryResetCalls != 0),
            "each recovery attempt follows input release and preserves the caller language and isolation state");
        if (recoveryResetCalls == 0) value->CurrentFrame() = kQuarantineResidualFrame;
        ++recoveryResetCalls; faultThrown = true;
        if (recoveryThrowsPersist || recoveryResetCalls == 1) {
            value->EnglishUI() = 0;
            if (recoveryThrowsUnknown) throw 17;
            throw std::runtime_error("reset threw while handling a genuine PMM short read");
        }
        return realReset(value);
    }
    if (monitor && fault == Fault::ReadResetQuarantine) {
        Check(value == app && !tailObserved && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            value->sceneRecoveryPending == (quarantineRefusals != 0),
            "V1 short-read recovery closes the stream before each refused reset");
        if (quarantineRefusals == 0) {
            value->CurrentFrame() = kQuarantineResidualFrame;
            quarantineResidualFrame = value->CurrentFrame();
        } else Check(value->CurrentFrame() == quarantineResidualFrame,
            "a refused V1 recovery leaves the isolated playhead unchanged");
        ++quarantineRefusals; faultThrown = true;
        return false;
    }
    if (monitor && fault == Fault::ReadFailure) {
        Check(value == app && !inputOpen && inputCloses == 1 && pmm_io::checkedDescriptor == checkedBefore,
            "a genuine strict short read releases its sole owner before real reset and UI recovery");
        faultThrown = true;
    }
    if (monitor && v2ModelAbortThrow) {
        auto& probe = *v2ModelAbortThrow;
        Check(value == app && !inputOpen && inputCloses == 1 && checkedAtClose == checkedBefore &&
              pmm_io::checkedDescriptor == checkedBefore && !tailObserved,
            "the real V2 model cancel closed its input and restored the read context before resetting");
        ++probe.resets;
        if (midResetFault || (probe.resets > 1 && !probe.persistent)) return realReset(value);
        faultThrown = true;
        value->CurrentFrame() = kQuarantineResidualFrame;
        value->EnglishUI() = (probe.language + 1) % 3;
        if (probe.action == V2AbortReset::ThrowStandard)
            throw std::runtime_error("test: model cancellation reset failed");
        throw 17;
    }
    if (monitor && fault == Fault::AbortReset && !faultThrown) {
        Check(value == app && !inputOpen && inputCloses == 1 && checkedAtClose == checkedBefore &&
            pmm_io::checkedDescriptor == checkedBefore && missingDialogs == 1,
            "the real model Abort closes its stream and consumes workspace storage before Reset");
        if (reuse) ReuseInputNumber();
        faultThrown = true; ++resetThrows;
        throw std::runtime_error("one-shot Reset failure after the real V2 missing-model Abort");
    }
    if (monitor && fault == Fault::AbortResetFalse && !resetFalseInjected) {
        // The real V2 Abort never inspects this boolean. Return false once and
        // leave the native scene exactly as the abort left it, except for a
        // non-zero playhead that only the real ResetAppState zeros
        // (reset_app_state.cpp:232). The stream is already closed here and the
        // success tail is unreachable, so this is the actual Abort call site.
        Check(value == app && !tailObserved && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            missingDialogs == expectedMissingAbortDialogs,
            "the one-shot Reset false is taken only by the actual V2 missing-model Abort after its close");
        resetFalseInjected = true; faultThrown = true; ++resetFalseRefusals;
        value->CurrentFrame() = kResetFalseResidualFrame;
        resetFalseResidualFrame = value->CurrentFrame();
        return false;
    }
    if (monitor && fault == Fault::AbortResetQuarantine) {
        // The missing-model Abort calls Reset after closing its
        // stream, its bounded recovery retries it once, and the
        // pending-recovery gate retries it again before a later load. Refuse
        // all three so the replaced scene keeps the first refusal's sentinel
        // playhead and its partially allocated model slot until a real
        // ResetAppState finally runs.
        const bool sawAbort = relocationDialogs
            ? relocationDialogs->expectAbort && relocationDialogs->prompts == 2 && relocationDialogs->picks == 2
            : missingDialogs == expectedQuarantineAbortDialogs;
        Check(value == app && !tailObserved && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            sawAbort &&
            value->sceneRecoveryPending == (quarantineRefusals != 0),
            "each quarantine refusal follows the real model abort, its retry or the recovery gate with the input already closed");
        if (quarantineRefusals == 0) {
            value->CurrentFrame() = kQuarantineResidualFrame;
            quarantineResidualFrame = value->CurrentFrame();
        } else Check(value->CurrentFrame() == quarantineResidualFrame,
            "no refused reset runs the real reset or disturbs the planted sentinel playhead");
        ++quarantineRefusals; faultThrown = true;
        return false;
    }
    if (monitor && fault == Fault::AccessoryResetFalse && accessoryResetPersist) {
        // The actual V1 accessory reset site refuses every time: once in the
        // loader, once from the shell's post-replacement retry and once from
        // the pending-recovery gate before the next open. The real
        // ResetAppState never runs, so the replaced scene keeps the real slot-0
        // accessory, the 4343 playhead and the restored TLS read context.
        ++accessoryResetRefusals;
        faultThrown = true;
        if (accessoryResetRefusals == 1) {
            Check(value == app && !tailObserved && !inputOpen && inputCloses == 1 &&
                checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
                !value->sceneRecoveryPending && accessoryLoads == 2 &&
                accessoryLoadFailures == 2 && accessoryPicks == 1 && accessoryDialogs == 2 &&
                !std::wcscmp(accessoryLoadPath[1], accessorySubstitutePath.c_str()) &&
                fs::is_regular_file(accessorySubstitutePath) && value->AccessorySlot(0) != nullptr,
                "the persistent first refusal is the actual V1 accessory reset after both real loads failed, with the real slot-0 accessory still present");
            accessoryRetainedSlot = static_cast<void*>(value->AccessorySlot(0));
            accessoryResetInjected = true;
            value->CurrentFrame() = kAccessoryResidualFrame;
            accessoryResidualFrame = value->CurrentFrame();
        } else {
            Check(value == app && accessoryResetRefusals <= 3 && !tailObserved &&
                !inputOpen && inputCloses == 1 && checkedAtClose == checkedBefore &&
                pmm_io::checkedDescriptor == checkedBefore &&
                value->sceneRecoveryPending && value->CurrentFrame() == accessoryResidualFrame &&
                static_cast<void*>(value->AccessorySlot(0)) == accessoryRetainedSlot &&
                accessoryLoads == 2 && accessoryPicks == 1 && accessoryDialogs == 2,
                "each later persistent refusal runs no real reset and keeps the same slot-0 accessory, sentinel playhead, closed input and TLS read context");
        }
        return false;
    }
    if (monitor && fault == Fault::AccessoryResetFalse) {
        if (!accessoryResetInjected) {
            // The real V1 accessory branch closed its sole stream and raised
            // SceneResetRefused only after this refusal. Both real loads already
            // failed and the picker ran once; the read context is back to its
            // pre-load state. Plant a playhead that only the real reset clears.
            Check(value == app && !tailObserved && !inputOpen && inputCloses == 1 &&
                checkedAtClose == checkedBefore &&
                pmm_io::checkedDescriptor == checkedBefore &&
                !value->sceneRecoveryPending && accessoryLoads == 2 &&
                accessoryLoadFailures == 2 && accessoryPicks == 1 &&
                accessoryDialogs == 2 &&
                !std::wcscmp(accessoryLoadPath[1], accessorySubstitutePath.c_str()) &&
                fs::is_regular_file(accessorySubstitutePath),
                "the one-shot Reset false is taken only after both real accessory loads failed, the sole stream closed and the TLS read context was restored");
            accessoryResetInjected = true; faultThrown = true;
            ++accessoryResetRefusals;
            value->CurrentFrame() = kAccessoryResidualFrame;
            accessoryResidualFrame = value->CurrentFrame();
            return false;
        }
        // The shell cancelled its prepared transaction and now retries the
        // reset once. Forward it so the real ResetAppState clears the planted
        // playhead, the recovery flag and every model/accessory slot.
        ++accessoryRecoveryForwards;
        Check(value == app && accessoryRecoveryForwards == 1 &&
            value->sceneRecoveryPending && !inputOpen && inputCloses == 1 &&
            value->CurrentFrame() == accessoryResidualFrame,
            "the outer transaction cancellation forwards the real reset on the retained playhead sentinel");
        return realReset(value);
    }
    if (v2Accessory && monitor && value == app && v2Accessory->expectNormalAbort &&
        !tailObserved && !inputOpen && inputCloses == 1 &&
        checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore) {
        // The explicit cancel/close answers run the production accessory abort:
        // it closes the PMM input and calls the real ResetAppState, whose own
        // PostViewRefresh is not the parser success tail. Mark exactly this real
        // reset call range for the original-profile observer below; the reset
        // itself remains the production function and its result is forwarded.
        auto& d = *v2Accessory;
        d.normalAbort = true;
        ++d.normalAbortResets;
        if (d.retryAbortReset != V2AbortReset::Forward) {
            Check(d.loadObjects.size() == 1 && value->AccessorySlot(0) == d.loadObjects.front() &&
                  value->AccessorySlot(0)->mesh,
                "each refused accessory reset sees the same real mesh in the earlier slot");
        }
        const auto action = d.normalAbortResets == 1 ? d.firstAbortReset : d.retryAbortReset;
        if (d.firstAbortReset != V2AbortReset::Forward) {
            Check(value->EnglishUI() == d.abortLanguage &&
                  value->sceneRecoveryPending == (d.normalAbortResets > 1),
                  "the abort retry keeps the caller language and starts in quarantine");
            if (d.normalAbortResets == 1) {
                ++d.firstAbortResetFailures;
                value->CurrentFrame() = kQuarantineResidualFrame;
            }
        }
        if (action != V2AbortReset::Forward)
            value->EnglishUI() = (d.abortLanguage + 1) % 3;
        switch (action) {
        case V2AbortReset::Refuse: return false;
        case V2AbortReset::ThrowStandard: throw std::runtime_error("accessory abort reset failed");
        case V2AbortReset::ThrowUnknown: throw 17;
        case V2AbortReset::Forward: break;
        }
        struct AbortResetScope {
            bool& flag;
            bool previous;
            explicit AbortResetScope(bool& value)
                : flag(value), previous(value) { flag = true; }
            ~AbortResetScope() { flag = previous; }
        } abortResetScope(d.inRealAbortReset);
        return realReset(value);
    }
    return realReset(value);
}

// Observe the real load and disposal calls used by the accessory fixtures.
decltype(&accessory_import::LoadObject) realAccessoryLoad = nullptr;
decltype(&DisposeAccessory) realDisposeAccessory = nullptr;
void ObserveDisposeAccessory(void* accessory) {
    if (v1MaterialRetry && v1MaterialRetry->record && accessory == v1MaterialRetry->record) {
        auto& retry = *v1MaterialRetry;
        const auto before = ReadAccessoryPayload(*retry.record);
        if (monitor && accessoryLoads == 1) {
            ++retry.failedDisposals;
            retry.disposedFailed = before;
        } else {
            Check(!monitor && accessoryLoads == 2 && before.Allocated() &&
                  before.pointers == retry.loaded.pointers,
                "the real scene reset disposes the loaded replacement's allocations");
            ++retry.resetDisposals;
        }
        realDisposeAccessory(accessory);
        Check(ReadAccessoryPayload(*retry.record).pointers == AccessoryPayload{}.pointers,
            "the real disposer releases the mesh and all three material allocations");
        return;
    }
    if (v2Accessory && v2Accessory->partialFailureSource &&
        accessory && accessory == v2Accessory->failedObject) {
        auto& d = *v2Accessory;
        auto& record = *mdl::Accessory(accessory);
        Check(monitor && inputOpen && !d.prompts && d.partialRefusals == 1 &&
              d.loadResults.size() == 1 && !d.loadResults.front() &&
              ReadAccessoryPayload(record).Allocated() && ++d.partialDisposals == 1,
            "V2 disposes the actual failed load's mesh and material arrays before recovery");
        realDisposeAccessory(accessory);
        Check(ReadAccessoryPayload(record).pointers == AccessoryPayload{}.pointers,
            "the real V2 disposer clears the rejected mesh and all three arrays");
        return;
    }
    if (v2Accessory && v2Accessory->duplicateSlot &&
        !v2Accessory->loadObjects.empty() && accessory == v2Accessory->loadObjects.front()) {
        auto& record = *mdl::Accessory(accessory);
        Check(record.mesh && record.materials && record.texturePaths && record.textureTypes,
              "the rejected duplicate leaves the first fully allocated record reachable for normal disposal");
        realDisposeAccessory(accessory);
        Check(!record.mesh && !record.materials && !record.texturePaths && !record.textureTypes,
              "normal disposal releases the first record's mesh and material arrays");
        ++v2Accessory->disposedLoadedRecords;
        return;
    }
    realDisposeAccessory(accessory);
}

bool ObserveAccessoryLoad(MMDApp* value, void* accessory, const wchar_t* path,
                          accessory_import::MissingFileNotice notice) {
    if (v1MaterialRetry) {
        auto& retry = *v1MaterialRetry;
        const unsigned index = accessoryLoads++;
        auto* record = mdl::Accessory(accessory);
        const auto& expected = index == 0 ? retry.source : retry.replacement;
        Check(index < 2 && monitor && inputOpen && value == app && record &&
              record == app->AccessorySlot(0) && path && expected == fs::path(path) &&
              fs::is_regular_file(expected),
            "the V1 loader receives each existing X file at the real slot-0 record");
        if (index == 0) {
            retry.record = record;
        } else {
            Check(record == retry.record && retry.refusals == 1 && retry.prompts == 1 && accessoryPicks == 1,
                "the existing V1 picker retries once on the very same accessory record");
            retry.beforeRetry = ReadAccessoryPayload(*record);
        }
        const bool loaded = realAccessoryLoad(value, accessory, path, notice);
        const auto payload = ReadAccessoryPayload(*record);
        std::fprintf(stderr,
            "DIAG V1 material X load %u path=%ls loaded=%u refusals=%u mesh=%p materials=%p paths=%p types=%p count=%u\n",
            index + 1, path, unsigned(loaded), retry.refusals, payload.pointers[0],
            payload.pointers[1], payload.pointers[2], payload.pointers[3], payload.materialCount);
        if (index == 0) {
            retry.failed = payload;
            Check(!loaded && retry.refusals == 1 &&
                  (payload.Allocated() || retry.disposedFailed.Allocated()),
                "the first X reaches D3DX mesh/material allocation and returns a real CP932 refusal");
        } else {
            retry.loaded = payload;
            Check(loaded && payload.Allocated() && record->textureTypes[0] == 3,
                "the real replacement X loader parses its mesh and screen.bmp material");
        }
        return loaded;
    }
    if (v2Accessory) {
        auto& d = *v2Accessory;
        Check(value == app && accessory != nullptr && path != nullptr,
            "the real V2 accessory load targets the production's own slot record with a concrete path");
        d.loadObjects.push_back(accessory);
        d.loadPaths.emplace_back(path);
        const bool disappears = !d.disappearingCandidate.empty() && d.disappearingCandidate == fs::path(path);
        fs::path parked;
        if (disappears) {
            parked = d.disappearingCandidate;
            parked.replace_extension(L".parked");
            Check(fs::is_regular_file(d.disappearingCandidate) && MoveFileW(path, parked.c_str()),
                "move the owned replacement after PMM path validation but before the real import opens it");
        }
        const bool loaded = realAccessoryLoad(value, accessory, path, notice);
        if (disappears) {
            Check(MoveFileW(parked.c_str(), path) && !loaded,
                "the real importer refuses the disappeared replacement and the fixture is restored");
        }
        d.loadResults.push_back(loaded);
        if (!loaded) d.failedObject = accessory;
        return loaded;
    }
    if (!(monitor && fault == Fault::AccessoryResetFalse))
        return realAccessoryLoad(value, accessory, path, notice);
    const unsigned index = accessoryLoads++;
    Check(index < 2, "the V1 accessory retry path loads exactly two real accessory objects");
    Check(value == app && accessory != nullptr &&
        accessory == static_cast<void*>(app->AccessorySlot(0)),
        "each real accessory load targets the production's own slot-0 record");
    wcsncpy_s(accessoryLoadPath[index], 260, path ? path : L"", _TRUNCATE);
    if (index == 0) {
        Check(path && !std::wcscmp(path, accessorySourcePath.c_str()) && fs::is_regular_file(path),
            "the first real load receives the existing malformed X source");
    } else {
        Check(path && !std::wcscmp(path, accessorySubstitutePath.c_str()) &&
            fs::is_regular_file(accessorySubstitutePath),
            "the second real load receives the selected malformed X replacement");
    }
    const bool loaded = realAccessoryLoad(value, accessory, path, notice);
    if (!loaded) ++accessoryLoadFailures;
    return loaded;
}
// The real picker is never shown: this stage supplies one absolute replacement
// path and reports a successful selection. Other stages keep the real
// native dialog.
decltype(&GetUiOpenFileNameW) realUiOpenFileName = nullptr;
BOOL ObserveUiOpenFileName(LPOPENFILENAMEW parameters) {
    if (motionDialogAction != MotionDialogAction::None) {
        Check(parameters->hwndOwner == app->MainWindow() && parameters->lpstrDefExt &&
              !std::wcscmp(parameters->lpstrDefExt, L"vmd"), "the production motion menu requests a VMD");
        ++motionPicks;
        if (motionDialogAction == MotionDialogAction::ResetScene)
            Check(ResetAppState(app), "replace the scene in the VMD picker");
        if (motionDialogAction == MotionDialogAction::SwitchModel) app->SetSelectedModelSlot(motionOtherSlot);
        if (motionDialogAction == MotionDialogAction::SwitchFrame) app->CurrentFrame() = 20;
        if (motionDialogAction == MotionDialogAction::Cancel) return FALSE;
        return wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, motionDialogPath.c_str()) == 0;
    }
    if (v2Accessory && (!v2Relocation || v2Accessory->awaitingPicker)) {
        auto& d = *v2Accessory;
        const auto* reply = d.awaitingPicker;
        Check(reply && reply->answer == 1 && monitor && inputOpen && parameters &&
              parameters->lpstrFile,
            "the real V2 accessory picker runs only for the armed locate reply inside the active PMM load");
        const HWND owner = app->state.floatingWindow
            ? reinterpret_cast<HWND>(app->state.floatingWindow) : app->MainWindow();
        const unsigned language = app->EnglishUI() <= 2 ? app->EnglishUI() : 1;
        Check(parameters->hwndOwner == owner &&
              parameters->lpstrDefExt != nullptr && !std::wcscmp(parameters->lpstrDefExt, L"x") &&
              (parameters->Flags & OFN_FILEMUSTEXIST) != 0 &&
              parameters->lpstrFile[0] == L'\0' &&
              parameters->nMaxFile == 0x100,
            "the production accessory picker keeps the floating/main owner, .x default extension, file-must-exist flag and empty name buffer");
        Check(AccessoryPickerFilterMatches(parameters->lpstrFilter, language) &&
              parameters->lpstrTitle != nullptr &&
              !std::wcscmp(parameters->lpstrTitle, AccessoryPickerTitle(language)),
            "the production accessory picker keeps its exact filter and title for the active UI language");
        d.pickerDirectories.emplace_back(parameters->lpstrInitialDir ? parameters->lpstrInitialDir : L"");
        d.awaitingPicker = nullptr;
        ++d.picks;
        if (reply->selection.empty()) return FALSE;
        Check(wcscpy_s(parameters->lpstrFile, parameters->nMaxFile,
              reply->selection.c_str()) == 0,
            "return the exact owned accessory selection from the picker freeze");
        return TRUE;
    }
    if (v2Relocation) {
        const auto* reply = v2Relocation->awaitingPicker;
        Check(reply && monitor && inputOpen && parameters && parameters->lpstrFile &&
              parameters->hwndOwner == (app->state.floatingWindow ? reinterpret_cast<HWND>(app->state.floatingWindow) : app->MainWindow()) &&
              (parameters->Flags & OFN_FILEMUSTEXIST) && parameters->lpstrDefExt &&
              !std::wcscmp(parameters->lpstrDefExt, L"pmd;pmx"), "V2 opens the requested model picker");
        if (const auto* expected = v2Relocation->expectedPickerDirectory)
            Check(parameters->lpstrInitialDir && !std::wcscmp(parameters->lpstrInitialDir, expected),
                  "the next open's model picker starts at the previous committed directory");
        v2Relocation->awaitingPicker = nullptr;
        if (reply->selection.empty()) return FALSE;
        Check(fs::is_regular_file(reply->selection) &&
              wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, reply->selection.c_str()) == 0,
              "return the exact owned V2 model selection");
        return TRUE;
    }
    if (relocationDialogs) {
        auto& dialogs = *relocationDialogs;
        Check(dialogs.picks < dialogs.choices.size() && dialogs.prompts == dialogs.picks + 1 &&
              parameters && parameters->lpstrFile && parameters->lpstrDefExt &&
              !std::wcscmp(parameters->lpstrDefExt, L"pmd;pmx"), "only answer the expected V1 model picker");
        const auto& selected = dialogs.choices[dialogs.picks++];
        if (selected.empty()) {
            Check(dialogs.expectAbort, "only the cancellation fixture dismisses a V1 model picker");
            faultThrown = true;
            return FALSE;
        }
        Check(wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, selected.c_str()) == 0,
              "return exact owned model path to V1");
        return TRUE;
    }
    if (!v1MaterialRetry && !(monitor && fault == Fault::AccessoryResetFalse))
        return realUiOpenFileName(parameters);
    ++accessoryPicks;
    Check(accessoryPicks == 1, "the accessory retry path requests exactly one replacement selection");
    Check(parameters && parameters->lpstrFile && parameters->nMaxFile >= 256,
        "the real retry dialog exposes its fixed open-file buffer");
    const auto& replacement = v1MaterialRetry ? v1MaterialRetry->replacement : accessorySubstitutePath;
    if (v1MaterialRetry) {
        Check(monitor && inputOpen && accessoryLoads == 1 && v1MaterialRetry->prompts == 1 &&
              fs::is_regular_file(replacement) && parameters->lpstrDefExt &&
              !std::wcscmp(parameters->lpstrDefExt, L"x"),
            "the real V1 material retry picker selects an existing X file after its prompt");
    } else {
        Check(fs::is_regular_file(replacement),
            "the replacement is a real malformed X file in the isolated directory");
    }
    Check(wcscpy_s(parameters->lpstrFile, parameters->nMaxFile,
        replacement.c_str()) == 0, "the existing picker reply supplies the exact absolute replacement path");
    return TRUE;
}

struct Wire {
    std::vector<unsigned char> bytes;
    template<class T> void Value(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* first = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(value));
    }
    void Zero(std::size_t count) { bytes.insert(bytes.end(), count, 0); }
    void Text(const char* text, std::size_t extent) {
        const auto length = std::strlen(text); Check(length < extent, "legacy fixture text fits its actual wire field");
        bytes.insert(bytes.end(), text, text + length); Zero(extent - length);
    }
    void Header(const char* version) {
        char header[30]{}; std::memcpy(header, "Polygon Movie maker ", 20); std::memcpy(header + 20, version, 4);
        for (const auto byte : header) Value(byte);
    }
    void Write(const fs::path& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close(); Check(bool(output), "write the independently assembled PMM wire fixture");
    }
};

HGLOBAL DropFileBlock(const fs::path& path);
std::vector<unsigned char> Bytes(const fs::path& path);

BOOL ObserveMotionSavePicker(LPOPENFILENAMEW parameters) {
    Check(motionDialogAction != MotionDialogAction::None && parameters->hwndOwner == app->MainWindow() &&
          parameters->lpstrDefExt && !std::wcscmp(parameters->lpstrDefExt, L"vmd") &&
          (parameters->Flags & OFN_OVERWRITEPROMPT), "the real save command requests VMD with overwrite confirmation");
    ++motionPicks;
    if (motionDialogAction == MotionDialogAction::Cancel) return FALSE;
    if (motionDialogAction == MotionDialogAction::SwitchModel) app->SetSelectedModelSlot(motionOtherSlot);
    if (motionDialogAction == MotionDialogAction::ResetScene)
        Check(ResetAppState(app), "replace the scene while the VMD save picker is open");
    return wcscpy_s(parameters->lpstrFile, parameters->nMaxFile, motionDialogPath.c_str()) == 0;
}

void MotionSaveCases() {
    const auto exportDir = directory / L"\u52a8\u4f5c";
    Check(fs::create_directory(exportDir), "create a private VMD output directory");
    const auto output = exportDir / L"existing.vmd";
    const auto original = directory / L"original.vmd";
    Wire motion;
    motion.Text("Vocaloid Motion Data 0002", 30);
    motion.Text("fixture", 20);
    motion.Value(std::uint32_t{0});
    motion.Zero(5 * sizeof(std::uint32_t));
    motion.Write(original);
    fs::copy_file(original, output);
    const auto sentinel = Bytes(output);

    for (const auto& path : {directory / L"first.pmd", directory / L"second.pmd"}) {
        Wire model;
        model.bytes = geometry_test_reference::Pmd(3, true).bytes;
        std::memset(model.bytes.data() + 7, 0, 20);
        std::memcpy(model.bytes.data() + 7, "fixture", 7);
        model.Write(path);
        LoadModelFile(app, path.c_str());
    }
    Check(app->ModelSlot(0) && app->ModelSlot(1), "load two real PMDs for VMD export and reimport");
    app->EnglishUI() = 1;
    app->CameraMode() = 0;
    app->CurrentFrame() = 0;
    app->SceneModified() = 0;
    motionOtherSlot = 1;
    auto& source = *mdl::Mdl(app->ModelSlot(0));
    source.boneKeys[0].allocated = 1;
    source.boneKeys[0].position[0] = 7.f;
    mdl::BoneKeys(app->ModelSlot(1))[0].allocated = 1;
    mdl::BoneKeys(app->ModelSlot(1))[0].position[0] = 81.f;

    const auto main = app->MainWindow();
    Check(CheckMenuItem(GetMenu(main), 0x12D, MF_BYCOMMAND | MF_CHECKED) != UINT(-1),
          "enable remembered motion folders through the production menu");
    const auto remembered = directory.native();
    const auto projectDir = (directory / L"project-resources").native();
    auto run = [&](MotionDialogAction action, const wchar_t* notice, bool saved = false,
                   const fs::path& destination = {}) {
        app->SetSelectedModelSlot(0);
        wcscpy_s(app->DirMotion(), kPathFieldCapacity, remembered.c_str());
        wcscpy_s(app->PathWorkspace().projectDirectory, kPathFieldCapacity, projectDir.c_str());
        const auto generation = app->SceneGeneration();
        motionDialogAction = action;
        motionDialogPath = destination.empty() ? output : destination;
        expectedMotionSaveNotice = notice;
        motionPicks = motionNotices = 0;
        SendMessageW(main, WM_COMMAND, 210, 0);
        motionDialogAction = MotionDialogAction::None;
        expectedMotionSaveNotice = nullptr;
        Check(motionPicks == 1 && motionNotices == (notice ? 1u : 0u), "observe only the expected export dialogs");
        Check(!app->SceneModified(), "export does not mark the scene modified");
        Check((app->SceneGeneration() != generation) == (action == MotionDialogAction::ResetScene),
              "only the explicit modal replacement changes the scene generation");
        if (action != MotionDialogAction::ResetScene) {
            Check(std::wstring(app->DirMotion()) == (saved ? exportDir.native() : remembered),
                  "only a completed export updates the remembered motion folder");
            Check(app->PathWorkspace().projectDirectory == projectDir,
                  "the motion folder never replaces the project resource directory");
        } else {
            Check(app->DirMotion() != exportDir.native(), "an obsolete picker cannot update the new scene's motion folder");
        }
        Check((Bytes(output) != sentinel) == saved, "only a completed export replaces the old VMD");
        for (const auto& entry : fs::directory_iterator(exportDir))
            Check(entry.path() == output, "no staged file remains after the save command returns");
    };
    run(MotionDialogAction::Cancel, nullptr);
    run(MotionDialogAction::SwitchModel, nullptr);
    const auto missingDirectory = directory / L"missing-motion-folder";
    run(MotionDialogAction::Accept, L"Check the folder and its write permissions", false,
        missingDirectory / L"existing.vmd");
    Check(!fs::exists(missingDirectory), "a vanished output folder stays absent after the failed export");
    if constexpr (build_profile::original) {
        auto& bone = source.boneTable[0];
        const auto wide = bone.jpText;
        std::array<char, sizeof bone.name> name{};
        std::memcpy(name.data(), bone.name, name.size());
        bone.jpText = nullptr;
        strcpy_s(bone.name, "abcdefghijklmnop");
        run(MotionDialogAction::Accept, L"CP932 field exactly");
        std::memcpy(bone.name, name.data(), name.size());
        bone.jpText = wide;
    }
    const HANDLE locked = CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "deny replacement of the existing output with real file sharing");
    run(MotionDialogAction::Accept, L"existing file was preserved");
    Check(CloseHandle(locked), "release the output before retrying");
    run(MotionDialogAction::Accept, nullptr, true);

    app->SetSelectedModelSlot(1);
    app->CurrentFrame() = 14;
    motionDialogAction = MotionDialogAction::Accept;
    motionPicks = motionNotices = 0;
    SendMessageW(main, WM_COMMAND, 209, 0);
    motionDialogAction = MotionDialogAction::None;
    const auto* keys = mdl::BoneKeys(app->ModelSlot(1));
    const auto next = keys[0].next;
    Check(motionPicks == 1 && motionNotices == 0 && next && keys[next].frame == 14 && keys[next].position[0] == 7.f,
          "the production load menu imports the exported key from the original selection");
    fs::copy_file(original, output, fs::copy_options::overwrite_existing);
    app->SceneModified() = 0;
    run(MotionDialogAction::ResetScene, nullptr);
    std::puts("PASS VMD save cancellation, stale selection, old-file preservation and retry with reimport");
}

void MotionModalCases() {
    const auto importDir = directory / L"motions";
    Check(fs::create_directory(importDir), "create a private motion directory");
    const auto valid = importDir / L"root.vmd", mismatch = importDir / L"other.vmd";
    const auto invalid = importDir / L"invalid.vmd";
    for (const auto& path : {valid, mismatch}) {
        Wire motion;
        motion.Text("Vocaloid Motion Data 0002", 30);
        motion.Text(path == valid ? "fixture" : "other model", 20);
        motion.Value(std::uint32_t{1});
        motion.Text("root", 15); motion.Value(std::uint32_t{14});
        for (const auto value : {7.f, 8.f, 9.f, 0.f, 0.f, 0.f, 1.f}) motion.Value(value);
        motion.Zero(64 + 5 * sizeof(std::uint32_t));
        motion.Write(path);
    }
    std::ofstream(invalid, std::ios::binary) << "not a VMD";
    for (const auto& path : {directory / L"first.pmd", directory / L"second.pmd"}) {
        Wire model;
        model.bytes = geometry_test_reference::Pmd(3, true).bytes;
        std::memset(model.bytes.data() + 7, 0, 20);
        std::memcpy(model.bytes.data() + 7, "fixture", 7);
        model.Write(path);
        LoadModelFile(app, path.c_str());
    }
    Check(app->ModelSlot(0) && app->ModelSlot(1), "load two real matching models");
    app->EnglishUI() = 1;
    app->CameraMode() = 0;
    app->CurrentFrame() = 0;
    motionOtherSlot = 1;
    const auto main = app->MainWindow();
    const HMENU menu = GetMenu(main);
    Check(CheckMenuItem(menu, 0x12D, MF_BYCOMMAND | MF_CHECKED) != UINT(-1), "enable motion directory memory");
    const auto currentDirectory = directory.native();
    auto run = [&](MotionDialogAction action, const fs::path& path, bool imported, unsigned notices) {
        app->SetSelectedModelSlot(0);
        app->CurrentFrame() = 0;
        app->SceneModified() = 0;
        app->PhysicsResetPending() = 0;
        wcscpy_s(app->DirMotion(), 1000, currentDirectory.c_str());
        const auto generation = app->SceneGeneration();
        motionDialogAction = action;
        motionDialogPath = path;
        motionPicks = motionNotices = 0;
        SendMessageW(main, WM_COMMAND, 209, 0);
        motionDialogAction = MotionDialogAction::None;
        Check(motionPicks == 1 && motionNotices == notices, "the motion command reaches only the expected dialogs");
        Check((app->SceneGeneration() != generation) == (action == MotionDialogAction::ResetScene),
              "observe the expected scene replacement");
        if (action == MotionDialogAction::ResetScene) {
            Check(!app->SceneModified() && std::wstring(app->DirMotion()) != importDir.native(),
                  "a stale picker does not mark the replacement scene or remember its path");
            return;
        }
        Check(bool(app->SceneModified()) == imported && bool(app->PhysicsResetPending()) == imported &&
              std::wstring(app->DirMotion()) == (imported ? importDir.native() : currentDirectory),
              "only a successful import marks the project, resets physics and remembers the directory");
        for (int slot : {0, 1}) {
            const auto* keys = mdl::BoneKeys(app->ModelSlot(slot));
            const auto next = keys[0].next;
            Check((imported && slot == 0) ? next && keys[next].frame == 14 && keys[next].position[0] == 7 : !next,
                  "the motion is applied only to the target that owns the successful retry");
        }
    };
    run(MotionDialogAction::Cancel, valid, false, 0);
    run(MotionDialogAction::SwitchModel, valid, false, 0);
    run(MotionDialogAction::SwitchFrame, valid, false, 0);
    run(MotionDialogAction::SwitchAtConfirmation, mismatch, false, 1);
    run(MotionDialogAction::Accept, invalid, false, 1);
    run(MotionDialogAction::Accept, valid, true, 0);
    app->SceneModified() = 0;
    wcscpy_s(app->DirMotion(), 1000, currentDirectory.c_str());
    motionDialogAction = MotionDialogAction::Accept;
    motionNotices = 0;
    SendMessageW(main, WM_DROPFILES, reinterpret_cast<WPARAM>(DropFileBlock(invalid)), 0);
    motionDialogAction = MotionDialogAction::None;
    Check(motionNotices == 1 && !app->SceneModified() && app->DirMotion() == currentDirectory,
          "the real drop command also preserves the motion directory and dirty flag after rejection");
    run(MotionDialogAction::ResetScene, valid, false, 0);
    std::puts("PASS VMD picker and model confirmation scene boundaries");
}
mdl::CameraKey CameraRoot() {
    mdl::CameraKey key{}; key.parentModel = -1; key.distance = -31.5f;
    key.target[0] = 2.25f; key.target[1] = -4.5f; key.target[2] = 8.75f;
    key.eye[0] = .125f; key.eye[1] = -.25f; key.eye[2] = .375f;
    key.fov = 43; key.perspective = 1;
    for (unsigned lane = 0; lane < 6; ++lane) {
        key.interpolation[0][lane] = key.interpolation[1][lane] = static_cast<unsigned char>(20 + lane);
        key.interpolation[2][lane] = key.interpolation[3][lane] = static_cast<unsigned char>(107 - lane);
    }
    return key;
}
mdl::LightKey LightRoot() {
    mdl::LightKey key{}; key.color[0] = .25f; key.color[1] = .5f; key.color[2] = .75f;
    key.direction[0] = .125f; key.direction[1] = -1; key.direction[2] = .375f; return key;
}
void WriteV1(const fs::path& path, const char* picture = "", std::int32_t pictureX = 0,
             std::int32_t pictureY = 0, float pictureScale = 0, int pictureVisible = -1) {
    // A genuine V1 no-model/no-accessory stream. V1 has six option bytes and
    // a 70-byte camera record without parent dwords. Stop precisely at the
    // first optional ReadGatedTail field, where ordinary V1 EOF is supported.
    Wire wire; wire.Header("0001");
    wire.Value(std::int32_t(640)); wire.Value(std::int32_t(480)); wire.Value(std::int32_t(360)); wire.Value(43.f);
    for (const unsigned char option : {1, 0, 1, 0, 0, 0}) wire.Value(option);
    wire.Value(std::uint8_t(0)); wire.Value(std::uint8_t(0)); // selected model, model count
    const auto camera = CameraRoot();
    const auto* head = reinterpret_cast<const unsigned char*>(&camera);
    wire.bytes.insert(wire.bytes.end(), head, head + 40);
    for (unsigned lane = 0; lane < 6; ++lane) for (unsigned row = 0; row < 4; ++row) wire.Value(camera.interpolation[row][lane]);
    wire.Value(camera.perspective); wire.Value(camera.fov); wire.Value(std::uint8_t(0)); wire.Value(std::int32_t(0));
    for (const float value : {2.25f, -4.5f, 8.75f, 0.f, 0.f, 31.5f, .125f, -.25f, .375f}) wire.Value(value);
    wire.Value(std::uint8_t(1));
    const auto light = LightRoot();
    wire.Value(light.frame); wire.Value(light.previous); wire.Value(light.next);
    for (const auto value : light.color) wire.Value(value);
    for (const auto value : light.direction) wire.Value(value);
    wire.Value(std::uint8_t(0)); wire.Value(std::int32_t(0));
    for (const auto value : light.color) wire.Value(value);
    for (const auto value : light.direction) wire.Value(value);
    wire.Value(std::uint8_t(0)); wire.Value(std::int32_t(0)); wire.Value(std::uint8_t(0)); // accessory selection/scroll/count
    wire.Value(std::int32_t(12)); wire.Value(std::int32_t(0)); wire.Value(std::int32_t(0));
    wire.Value(std::int32_t(0)); // edit mode
    for (unsigned i = 0; i < 4; ++i) wire.Value(std::uint8_t(0)); // camera reference and playback flags
    wire.Value(std::int32_t(0)); wire.Value(std::int32_t(0));
    wire.Value(std::uint8_t(0)); wire.Zero(256); // no wave
    wire.Zero(12); wire.Zero(256); wire.Value(std::int32_t(0)); // no AVI: 3 dwords/path/flag
    wire.Value(pictureX); wire.Value(pictureY); wire.Value(pictureScale);
    wire.Text(picture, 256); wire.Value(std::uint8_t(pictureVisible < 0 ? (picture[0] ? 1 : 0) : pictureVisible));
    wire.Value(std::uint8_t(0)); wire.Value(std::uint8_t(1)); wire.Value(std::uint8_t(0)); // FPS/grid/shadow
    wire.Value(30.f); wire.Value(std::int32_t(0)); // FPS limit and capture mode
    Check(wire.bytes.size() == 1073, "V1 fixture has the exact mandatory no-object layout and a legal optional-tail EOF");
    wire.Write(path);
}
void WriteMissingV2(const fs::path& path) {
    // Valid V2 prefix through the first missing model path. The real missing
    // model dialog selects Abort before subsequent model records are read.
    Wire wire; wire.Header("0002");
    wire.Value(std::int32_t(640)); wire.Value(std::int32_t(480)); wire.Value(std::int32_t(360)); wire.Value(43.f);
    for (const unsigned char option : {1, 0, 1, 0, 0, 0, 0}) wire.Value(option);
    wire.Value(std::uint8_t(0)); wire.Value(std::uint8_t(1)); wire.Value(std::uint8_t(0)); // selected/count/model slot
    constexpr char name[] = "missing";
    wire.Value(std::uint8_t(sizeof(name) - 1));
    for (unsigned i = 0; i < sizeof(name) - 1; ++i) wire.Value(name[i]);
    wire.Value(std::uint8_t(sizeof(name) - 1));
    for (unsigned i = 0; i < sizeof(name) - 1; ++i) wire.Value(name[i]);
    wire.Text("missing-input-host-model.pmd", 256); wire.Write(path);
}
std::vector<unsigned char> Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); Check(bool(input), "read a disposable source for exact unchanged-file comparison");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string WirePathText(const fs::path& path) {
    const auto wide = path.wstring();
    std::string text(wide.size() * 2 + 1, '\0');
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, text.data(),
        static_cast<int>(text.size()), nullptr, nullptr);
    Check(length > 0, "encode the isolated accessory source path");
    text.resize(static_cast<std::size_t>(length - 1));
    for (const unsigned char byte : text)
        Check(byte < 0x80, "the accessory source path stays ASCII inside the isolated directory");
    Check(text.size() < 256, "the accessory source path fits its fixed PMM field");
    return text;
}
void WriteV1AccessoryResetFalse(const fs::path& path) {
    // Reuse the existing legal 1073-byte V1 fixture without changing WriteV1:
    // keep its mandatory 236-byte prefix (ending in the zero accessory count),
    // set that count to one, and append the single record up to the source path.
    // The retry fails on that path, before any accessory payload byte is read.
    WriteV1(path);
    Wire wire; wire.bytes = Bytes(path);
    Check(wire.bytes.size() == 1073 && wire.bytes[235] == 0,
        "the existing legal V1 fixture ends its mandatory prefix with an empty accessory count");
    wire.bytes.resize(236);
    wire.bytes[235] = 1;
    wire.Text("shadow", 100);
    wire.Value(std::uint8_t(0));
    wire.Text("accessory", 100);
    const auto source = WirePathText(accessorySourcePath);
    wire.Text(source.c_str(), 256);
    Check(wire.bytes.size() == 693,
        "the accessory fixture stops after the source path before the retried load can read its payload");
    wire.Write(path);
}
constexpr std::size_t kV1AccessoryCountOffset = 235;

void WriteV1AccessoryBlock(Wire& wire, const fs::path& source, const fs::path& secondSource = {}) {
    wire.Value(std::uint8_t(secondSource.empty() ? 1 : 2));
    wire.Text("material-retry", 100); // shadow name table
    if (!secondSource.empty()) wire.Text("following", 100);
    const auto appendAccessory = [&](std::uint8_t slot, const char* name, const fs::path& file, float x) {
        wire.Value(slot);
        wire.Text(name, 100);
        wire.Text(WirePathText(file).c_str(), 256);
        wire.Value(slot); // order
        wire.Zero(12); // root frame, previous, next
        wire.Value(std::uint8_t(1)); // visible and opaque
        wire.Value(std::int32_t(-1)); // parent model
        wire.Value(std::int32_t(0)); // parent bone
        for (const float value : {x, 0.f, 0.f, 0.f, 0.f, 0.f, 1.25f}) wire.Value(value);
        wire.Value(std::uint8_t(1)); // shadow enabled
        wire.Value(std::uint8_t(0)); // key selection
        wire.Value(std::int32_t(0)); // no sparse keys
        wire.Value(std::uint8_t(1)); // current visibility and opacity
        wire.Value(std::int32_t(-1)); // parent model
        wire.Value(std::int32_t(0)); // parent bone
        for (const float value : {0.f, 0.f, 0.f, 1.25f, x, 0.f, 0.f}) wire.Value(value);
        wire.Value(std::uint8_t(1)); // the single V1 trailing shadow byte
    };
    appendAccessory(0, "material-retry", source, 2.5f);
    if (!secondSource.empty()) appendAccessory(1, "following", secondSource, 7.5f);
}

void WriteV1AccessoryMaterialRetry(const fs::path& path, const fs::path& source,
                                   const fs::path& secondSource = {}) {
    WriteV1(path);
    const auto empty = Bytes(path);
    Check(empty.size() == 1073 && empty[kV1AccessoryCountOffset] == 0,
        "splice the accessory into the complete no-accessory V1 wire fixture");
    Wire wire;
    wire.bytes.assign(empty.begin(), empty.begin() + kV1AccessoryCountOffset);
    WriteV1AccessoryBlock(wire, source, secondSource);
    wire.bytes.insert(wire.bytes.end(), empty.begin() + kV1AccessoryCountOffset + 1, empty.end());
    wire.Write(path);
}
void Released(const fs::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "the PMM CRT stream and plugin read-lock are both released after shell return");
    CloseHandle(file);
    const auto moved = fs::path(path.native() + L".move-probe");
    Check(MoveFileExW(path.c_str(), moved.c_str(), 0) && MoveFileExW(moved.c_str(), path.c_str(), 0),
        "a fully released project can be renamed and restored without touching its bytes");
    const auto lock = fs::path(path.native() + L".mms-save.lock");
    const HANDLE pairLock = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    Check(pairLock != INVALID_HANDLE_VALUE, "the plugin PMM/sidecar pair recovery lock is not leaked");
    CloseHandle(pairLock);
#if !MMD_ORIGINAL_PROFILE
    const auto sidecar = plugins::PluginSidecar(path);
    if (fs::exists(sidecar)) {
        const HANDLE side = CreateFileW(sidecar.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(side != INVALID_HANDLE_VALUE, "the real plugin sidecar is also independently accessible"); CloseHandle(side);
    }
#endif
}
HGLOBAL DropFileBlock(const fs::path& path) {
    // A real shell drop allocates one movable global block whose DROPFILES
    // header offsets a double-null-terminated wide name list. Build the same
    // valid block so the isolated window receives an actual WM_DROPFILES handle.
    const auto name = path.wstring();
    const SIZE_T bytes = sizeof(DROPFILES) + (name.size() + 2) * sizeof(wchar_t);
    const HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    Check(block != nullptr, "allocate a valid DROPFILES block for the isolated main window");
    auto* drop = static_cast<DROPFILES*>(GlobalLock(block));
    Check(drop != nullptr, "lock the valid DROPFILES block");
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<unsigned char*>(drop) + drop->pFiles, name.c_str(),
                name.size() * sizeof(wchar_t));
    GlobalUnlock(block);
    wchar_t listed[260]{};
    Check(DragQueryFileW(static_cast<HDROP>(block), 0, listed, 260) == static_cast<UINT>(name.size()) &&
        !std::wcscmp(listed, name.c_str()),
        "the isolated DROPFILES block is a valid single-file drop");
    return block;
}
void Arm(const fs::path& path, Fault target, bool reuseNumber) {
    candidate = path; fault = target; reuse = reuseNumber; inputDescriptor = sentinelDescriptor = -1;
    inputCloses = sentinelCloses = 0; checkedAtClose = -2; inputOpen = sentinelLive = faultThrown = false;
    tailObserved = false; viewRefreshCalls = preCloseRefreshCalls = recoveryRefreshCalls = 0;
    inputReads = bodyRewinds = expectedBoundary = 0; extensionCalls = 0; insideExtension = refuseSync = false;
    pmm_io::readStatistics = {};
    resetFalseInjected = false; resetFalseRefusals = 0; resetFalseResidualFrame = 0;
    quarantineRefusals = 0; quarantineResidualFrame = 0;
    recoveryResetCalls = 0;
    accessoryPicks = accessoryLoads = accessoryLoadFailures = accessoryDialogs = 0;
    accessoryResetInjected = false; accessoryResetRefusals = accessoryRecoveryForwards = 0;
    accessoryResidualFrame = 0; accessoryRetainedSlot = nullptr; accessoryResetPersist = false;
    checkedBefore = pmm_io::checkedDescriptor; monitor = true;
}
void Finish(bool loaded, bool success, bool quarantined = false, bool partialModel = true,
            bool partialAccessory = false) {
    monitor = false;
    const bool valid = loaded == success && inputDescriptor >= 0 && !inputOpen && inputCloses == 1 &&
        checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore;
    if (!valid) {
        std::fprintf(stderr, "DIAG PMM shell loaded=%u expectedSuccess=%u\n", unsigned(loaded), unsigned(success));
        TailDiagnostic("shell-return assertion", app, app->EnvFileName());
    }
    Check(valid,
        "real PMM success and failure routes close their stream once and restore the prior read context");
    Check(!sentinelCloses, "shell cleanup never closes the independently owned sentinel during the exact reuse window");
    if (sentinelLive) {
        constexpr char text[] = "sentinel-survives";
        Check(_write(sentinelDescriptor, text, sizeof(text) - 1) == sizeof(text) - 1 &&
            _lseek(sentinelDescriptor, 0, SEEK_SET) == 0, "the reused descriptor remains writable after actual parser-tail failure");
        char actual[32]{}; bool strict = false; int count = -1;
        try { count = pmm_io::Rd(sentinelDescriptor, actual, sizeof(actual)); }
        catch (const pmm_io::ShortRead&) { strict = true; }
        Check(!strict && count == sizeof(text) - 1 && std::memcmp(actual, text, sizeof(text) - 1) == 0,
            "the reused descriptor remains readable without inheriting the consumed PMM TLS check");
        Check(_close(sentinelDescriptor) == 0, "the sentinel is closed only after the shell observation window by its real test owner");
        sentinelDescriptor = -1; sentinelLive = false;
    }
    Check(!app->SceneMutationInProgress() && !IsWindowVisible(app->MainWindow()), "the actual host leaves its mutation scope and stays hidden");
    if (!success) {
        wchar_t title[256]{}; GetWindowTextW(app->MainWindow(), title, 256);
        Check(faultThrown && !app->EnvFileName()[0] && !app->PathWorkspace().projectDirectory[0] &&
            !std::wcscmp(title, L"MikuMikuDance"), "failure after real replacement leaves an unnamed scene and never restores the old save target");
        if (quarantined) {
            if (partialAccessory) {
                Check(app->sceneRecoveryPending && app->CurrentFrame() == kAccessoryResidualFrame &&
                    !app->ModelSlot(0) && app->AccessorySlot(0),
                    "the persistently refused accessory reset leaves the replaced scene quarantined with its real slot-0 accessory and sentinel playhead");
                for (unsigned slot = 1; slot < 255; ++slot)
                    Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot), "the accessory-quarantined scene retains only the real slot-0 accessory");
            } else {
                Check(app->sceneRecoveryPending && app->CurrentFrame() == kQuarantineResidualFrame &&
                    (app->ModelSlot(0) != nullptr) == partialModel && !app->AccessorySlot(0),
                    "the double-refused reset leaves the replaced scene quarantined with its sentinel playhead and expected model ownership");
                for (unsigned slot = 1; slot < 255; ++slot)
                    Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot), "the quarantined scene retains only the partially read model slot");
            }
        } else {
            for (unsigned slot = 0; slot < 255; ++slot)
                Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot), "the real error-reset leaves no partially loaded native objects");
        }
    }
#if !MMD_ORIGINAL_PROFILE
    auto* history = plugins::NativeHistory(app);
    if (quarantined) {
        // The replaced scene is still outside the shared journal until a real
        // reset completes, so the same gate that refuses native edits and
        // saves reports BUSY here instead of OK.
        Check(history && history->CheckNativeEdit() == MMS_E_BUSY &&
            !history->UndoCount() && !history->RedoCount(),
            "the quarantined scene keeps the real shared journal isolated after the shell returns");
    } else {
        Check(history && history->CheckNativeEdit() == MMS_OK && !history->UndoCount() && !history->RedoCount(),
            "the real project transaction releases its plugin/history gates after both outcomes");
    }
#endif
    Released(candidate);
}
void WriteTextureFixture(const fs::path& texture, DWORD rgb) {
    std::array<unsigned char, 18 + 8 * 8 * 4> bytes{};
    bytes[2] = 2; bytes[12] = 8; bytes[14] = 8; bytes[16] = 32; bytes[17] = 8;
    for (std::size_t i = 18; i < bytes.size(); i += 4) {
        bytes[i] = static_cast<unsigned char>(rgb);
        bytes[i + 1] = static_cast<unsigned char>(rgb >> 8);
        bytes[i + 2] = static_cast<unsigned char>(rgb >> 16);
        bytes[i + 3] = 255;
    }
    std::ofstream output(texture, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    Check(bool(output), "write an actual constant-color texture");
}

void WriteTexturedAccessoryFixture(const fs::path& mesh) {
    std::string text = kAccessoryTriangleA;
    text.insert(text.size() - 3, "TextureFilename {\"image.tga\";}");
    std::ofstream file(mesh, std::ios::binary);
    file << text;
    file.close();
    Check(bool(file), "write a textured X mesh accepted by D3DX");
}

DWORD AccessoryTextureColor(const fs::path& texture) {
    const auto* accessory = app->AccessorySlot(0);
    Check(accessory && accessory->mesh && accessory->materialCount == 1 && accessory->texturePaths,
          "the loaded scene has a real textured accessory");
    const auto* path = reinterpret_cast<const wchar_t*>(accessory->texturePaths[0]);
    const auto* entry = FindTextureResource(app->Renderer(), path);
    Check(fs::path(path) == texture && entry && entry->comObject, "the material resolves the same exact texture path");
    auto* value = static_cast<IDirect3DTexture9*>(entry->comObject);
    D3DLOCKED_RECT pixels{};
    Check(SUCCEEDED(value->LockRect(0, &pixels, nullptr, D3DLOCK_READONLY)) && pixels.pBits,
          "read pixels from the actual texture used by the scene");
    const auto rgb = *static_cast<const DWORD*>(pixels.pBits) & 0xffffff;
    Check(SUCCEEDED(value->UnlockRect(0)), "unlock the scene texture");
    return rgb;
}

void TextureReopenCases() {
    namespace cache = texture_prepare_cache;
    const auto root = directory / L"texture-reopen";
    Check(fs::create_directory(root), "create an isolated PMM texture reload directory");
    const auto mesh = root / L"textured.x", texture = root / L"image.tga";
    const auto v1 = root / L"old.pmm", v2 = root / L"new.pmm";
    WriteTexturedAccessoryFixture(mesh);
    constexpr DWORD first = 0xb74713, second = 0x3288c5, third = 0x72ad41;
    WriteTextureFixture(texture, first);
    const auto stamp = fs::last_write_time(texture);
    const auto length = fs::file_size(texture);
    Check(ResetAppState(app), "reset the actual hidden host for texture preparation");
    LoadAccessoryFile(mesh.c_str());
    Check(AccessoryTextureColor(texture) == first, "initial import uses the original source pixels");
    Check(SaveSceneCopy(app, v2.c_str()), "write the V2 project with the actual accessory");
    WriteV1AccessoryMaterialRetry(v1, mesh);
    const auto v1Bytes = Bytes(v1), v2Bytes = Bytes(v2);
    const auto reopen = [&](const fs::path& project, DWORD expected, bool warm) {
        const auto before = cache::GetStatistics();
        Arm(project, Fault::None, false);
        const bool loaded = LoadSceneFile(project.c_str());
        Finish(loaded, true);
        Check(AccessoryTextureColor(texture) == expected, "reopening uses current source pixels instead of the previous scene's texture");
        const auto after = cache::GetStatistics();
        if (build_profile::original) {
            Check(after.hits == before.hits && after.stores == before.stores, "original profile stays on the native texture path");
        } else if (warm) {
            Check(after.hits > before.hits && after.nativeCalls == before.nativeCalls,
                  "an unchanged reopen uses the rebuilt disk cache without decoding again");
        } else {
            Check(after.stores > before.stores && after.nativeCalls > before.nativeCalls,
                  "a changed texture is decoded and publishes a new disk cache entry");
        }
        std::printf("TEXTURE-REOPEN %s rgb=%06lx hits=%llu native=%llu stores=%llu\n", warm ? "warm" : "changed",
                    static_cast<unsigned long>(expected), after.hits - before.hits,
                    after.nativeCalls - before.nativeCalls, after.stores - before.stores);
    };
    const auto replaceTexture = [&](DWORD rgb) {
        WriteTextureFixture(texture, rgb);
        fs::last_write_time(texture, stamp);
        Check(fs::file_size(texture) == length && fs::last_write_time(texture) == stamp,
              "the texture content changes without changing size or modification time");
    };
    replaceTexture(second);
    reopen(v2, second, false);
    reopen(v2, second, true);
    replaceTexture(third);
    reopen(v1, third, false);
    reopen(v1, third, true);
    Check(ResetAppState(app) && !FindTextureResource(app->Renderer(), texture.c_str()),
          "New releases the retired scene texture without deleting its disk cache");
    reopen(v2, third, true);
    Check(Bytes(v1) == v1Bytes && Bytes(v2) == v2Bytes, "reopening never changes the source projects");
}

void ModelAndTextureReopenCases() {
    namespace geometry = model_geometry_cache;
    namespace textures = texture_prepare_cache;
    const auto root = directory / L"asset-reopen";
    Check(fs::create_directory(root), "create a private project and asset directory");
    const auto project = root / L"assets.pmm";
    const auto modelPath = root / L"model.pmx";
    const auto mesh = root / L"textured.x";
    const auto texture = root / L"image.tga";
    auto fixture = geometry_test_reference::Pmx(3, 0, 2, 2, true);
    Wire model{std::move(fixture.bytes)};
    model.Write(modelPath);
    WriteTexturedAccessoryFixture(mesh);
    constexpr DWORD initialRgb = 0x1189c1, changedRgb = 0xe1326a;
    constexpr float changedX = 42.25f;
    WriteTextureFixture(texture, initialRgb);
    const auto modelStamp = fs::last_write_time(modelPath), textureStamp = fs::last_write_time(texture);
    const auto modelSize = fs::file_size(modelPath), textureSize = fs::file_size(texture);
    const auto meshBytes = Bytes(mesh);

    Check(SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE", L"1") &&
          SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", L"1"),
          "seed the project without populating either preparation cache");
    Check(ResetAppState(app), "start with an empty isolated scene");
    LoadModelFile(app, modelPath.c_str(), false);
    LoadAccessoryFile(mesh.c_str());
    Check(app->ModelSlot(0) && AccessoryTextureColor(texture) == initialRgb &&
          SaveSceneCopy(app, project.c_str()), "save a real project containing the PMX and textured accessory");
    const auto projectBytes = Bytes(project);
    Check(ResetAppState(app) && !app->ModelSlot(0) && !FindTextureResource(app->Renderer(), texture.c_str()),
          "retire the seed scene before the first open");
    Check(SetEnvironmentVariableW(L"MMS_MODEL_GEOMETRY_CACHE_DISABLE", L"0") &&
          SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", L"0"),
          "allow both caches for the actual opens");

    const auto reopen = [&](const char* stage, float expectedX, DWORD expectedRgb, bool warm) {
        const auto geometryBefore = geometry::GetProcessStatistics();
        const auto textureBefore = textures::GetStatistics();
        Arm(project, Fault::None, false);
        const bool loaded = LoadSceneFile(project.c_str());
        Finish(loaded, true);
        const auto* record = mdl::Mdl(app->ModelSlot(0));
        Check(record && fs::path(record->path) == modelPath && record->pmxVertices &&
              record->vertexCount == fixture.vertices && record->pmxVertices[0].position[0] == expectedX,
              "the project uses the expected geometry from the same PMX path");
        auto* buffer = static_cast<IDirect3DVertexBuffer9*>(record->vertexBuffer);
        void* pixels = nullptr;
        Check(buffer && SUCCEEDED(buffer->Lock(0, sizeof(float), &pixels, D3DLOCK_READONLY)) && pixels,
              "read the actual main D3D9 vertex buffer");
        float gpuX = 0;
        std::memcpy(&gpuX, pixels, sizeof(gpuX));
        Check(SUCCEEDED(buffer->Unlock()) && gpuX == expectedX, "the vertex buffer uses the current PMX geometry");
        Check(AccessoryTextureColor(texture) == expectedRgb, "the accessory uses the current source texture pixels");
        const auto geometryAfter = geometry::GetProcessStatistics();
        const auto textureAfter = textures::GetStatistics();
        const auto geometryHits = geometryAfter.hits - geometryBefore.hits;
        const auto geometryStores = geometryAfter.stores - geometryBefore.stores;
        const auto textureHits = textureAfter.hits - textureBefore.hits;
        const auto textureStores = textureAfter.stores - textureBefore.stores;
        const auto textureDecodes = textureAfter.nativeCalls - textureBefore.nativeCalls;
        std::printf("ASSET-REOPEN %s x=%.2f rgb=%06lx geometryHits=%llu geometryStores=%llu "
                    "textureHits=%llu textureDecodes=%llu textureStores=%llu\n",
                    stage, gpuX, static_cast<unsigned long>(expectedRgb), geometryHits, geometryStores,
                    textureHits, textureDecodes, textureStores);
        if (build_profile::original) {
            Check(!geometryHits && !geometryStores && !textureHits && !textureStores,
                  "the original-compatible profile uses uncached source loading");
        } else if (warm) {
            Check(geometryHits == 1 && !geometryStores && textureHits >= 1 && !textureDecodes && !textureStores,
                  "unchanged reopen reuses both caches without republishing or decoding");
        } else {
            Check(!geometryHits && geometryStores == 1 && !textureHits && textureDecodes >= 1 && textureStores >= 1,
                  "uncached source contents are read and published by both caches");
        }
    };

    reopen("cold", 0.f, initialRgb, false);
    reopen("warm", 0.f, initialRgb, true);
    std::memcpy(model.bytes.data() + fixture.vertexStart, &changedX, sizeof(changedX));
    model.Write(modelPath);
    WriteTextureFixture(texture, changedRgb);
    fs::last_write_time(modelPath, modelStamp);
    fs::last_write_time(texture, textureStamp);
    Check(fs::file_size(modelPath) == modelSize && fs::last_write_time(modelPath) == modelStamp &&
          fs::file_size(texture) == textureSize && fs::last_write_time(texture) == textureStamp,
          "replace both sources in place without changing their sizes or modification times");
    reopen("changed", changedX, changedRgb, false);
    reopen("changed-warm", changedX, changedRgb, true);
    Check(Bytes(project) == projectBytes && Bytes(mesh) == meshBytes && Bytes(modelPath) == model.bytes,
          "reopening preserves the project, accessory and updated model bytes");
}

void MatchRoots(const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto& actual = app->CameraKeys()[0]; const auto& actualLight = app->LightKeys()[0];
    Check(actual.frame == camera.frame && actual.previous == camera.previous && actual.next == camera.next &&
        actual.distance == camera.distance && actual.fov == camera.fov && actual.perspective == camera.perspective &&
        std::memcmp(actual.target, camera.target, sizeof(camera.target)) == 0 &&
        std::memcmp(actual.eye, camera.eye, sizeof(camera.eye)) == 0 &&
        std::memcmp(actual.interpolation, camera.interpolation, sizeof(camera.interpolation)) == 0,
        "normal real V1/V2 parsing preserves the camera payload and all 24 interpolation bytes");
    Check(actualLight.frame == light.frame && actualLight.previous == light.previous && actualLight.next == light.next &&
        std::memcmp(actualLight.color, light.color, sizeof(light.color)) == 0 &&
        std::memcmp(actualLight.direction, light.direction, sizeof(light.direction)) == 0,
        "normal real V1/V2 parsing preserves the light root payload");
}
void Normal(const fs::path& path, const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto before = Bytes(path); const auto tails = tailCalls; Arm(path, Fault::None, false);
    const bool loaded = LoadSceneFile(path.c_str()); Finish(loaded, true);
    const bool valid = tailCalls == tails + 1 && !std::wcscmp(app->EnvFileName(), path.c_str()) && app->CurrentFrame() == 12;
    if (!valid) {
        std::fprintf(stderr, "DIAG PMM normal tailsBefore=%u tailsAfter=%u frame=%u\n", tails, tailCalls, unsigned(app->CurrentFrame()));
        TailDiagnostic("normal-tail assertion", app, app->EnvFileName());
    }
    Check(valid,
        "a normal real PMM load reaches the untouched success tail and accepts its exact path and playhead");
    MatchRoots(camera, light); Check(Bytes(path) == before, "a real normal PMM open never rewrites its source");
}

void ProjectPreflightRefusalCases() {
    const auto healthy = directory / L"preflight-current.pmm";
    const auto saved = directory / L"preflight-edited.pmm";
    const auto invalid = directory / L"preflight-invalid.pmm";
    const auto unsupported = directory / L"preflight-unsupported.pmm";
    const auto missing = directory / L"preflight-missing.pmm";
    WriteV1(healthy);
    std::ofstream(invalid, std::ios::binary) << "not a project";
    Wire newer;
    newer.Header("9999");
    newer.Write(unsupported);
    Normal(healthy, CameraRoot(), LightRoot());
    const auto original = Bytes(healthy);
    const auto resourceRoot = std::wstring(app->PathWorkspace().projectDirectory);
    const auto remembered = std::wstring(app->DirUser());
    const auto generation = app->SceneGeneration();
    const auto frame = app->CurrentFrame();
    for (const auto& path : {invalid, unsupported, missing}) {
        const bool editDuringNotice = path == unsupported;
        const bool missingFile = path == missing;
        const auto expected = missingFile ? UiMessage::OpenProjectError :
            editDuringNotice ? UiMessage::UnsupportedProjectVersion : UiMessage::InvalidProjectFile;
        const auto candidateBytes = missingFile ? decltype(Bytes(path)){} : Bytes(path);
        auto revision = app->SceneRevision();
        unsigned notices = 0;
        bool inputReleased = false;
        preflightOpenNotice = [&](UiMessage message, const wchar_t* detail) {
            const bool correctPath = detail && (missingFile ?
                std::wstring_view(detail).find(path.native() + L"\n") == 0 : path == detail);
            Check(message == expected && correctPath && ++notices == 1,
                  "report the exact refused project and reason once");
            if (!missingFile) {
                const HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                                  FILE_ATTRIBUTE_NORMAL, nullptr);
                inputReleased = writer != INVALID_HANDLE_VALUE;
                if (inputReleased) CloseHandle(writer);
            }
            if (editDuringNotice) {
                app->CameraKeys()[0].distance = -53.5f;
                app->SceneModified() = 1;
                revision = app->SceneRevision();
            }
            return IDOK;
        };
        const bool loaded = LoadSceneFile(path.c_str());
        preflightOpenNotice = nullptr;
        std::printf("PREFLIGHT edit=%d inputReleased=%d dirty=%d revisionRetained=%d\n",
                    editDuringNotice, inputReleased, bool(app->SceneModified()), app->SceneRevision() == revision);
        Check(!loaded && notices == 1 && app->SceneGeneration() == generation &&
              app->CurrentFrame() == frame && fs::path(app->EnvFileName()) == healthy &&
              app->PathWorkspace().projectDirectory == resourceRoot && app->DirUser() == remembered,
              "preflight refusal keeps the current project, resources and playhead");
        Check(bool(app->SceneModified()) == (editDuringNotice || missingFile) && app->SceneRevision() == revision,
              "preflight refusal preserves the scene revision and edits made in the error dialog");
        Check((missingFile ? !fs::exists(path) : inputReleased && Bytes(path) == candidateBytes) &&
              Bytes(healthy) == original,
              "both candidate readers release the refused file before the notice without modifying either file");
    }
    Check(SaveSceneCopy(app, saved.c_str()) && LoadSceneFile(saved.c_str()) &&
          app->CameraKeys()[0].distance == -53.5f && Bytes(healthy) == original,
          "save and reopen the edit retained after the failed open");
    std::puts("PASS rejected PMM header and version preserve the current project");
}

void V2ModelCancelThrowCases() {
    const auto rejected = directory / L"model-cancel.pmm";
    const auto healthy = directory / L"healthy.pmm";
    const auto blocked = directory / L"blocked.pmm";
    WriteMissingV2(rejected);
    WriteV1(healthy);
    const auto source = Bytes(rejected);
    const auto camera = CameraRoot();
    const auto light = LightRoot();
    bool allCorrect = true;
    for (const auto action : {V2AbortReset::ThrowStandard, V2AbortReset::ThrowUnknown}) {
        for (const bool persistent : {false, true}) {
            Normal(healthy, camera, light);
            const auto language = persistent ? kUiChinese : kUiEnglish;
            app->EnglishUI() = language;
            V2ModelAbortThrow probe{action, language, persistent};
            const auto prompts = missingDialogs, tails = tailCalls, finalNotices = postloadNotices;
            Arm(rejected, Fault::None, false);
            ResetPostloadObservation(language);
            postloadReopenPath = healthy;
            v2ModelAbortThrow = &probe;
            bool loaded = false, escaped = false;
            try { loaded = LoadSceneFile(rejected.c_str()); }
            catch (...) { escaped = true; }
            v2ModelAbortThrow = nullptr;
            monitor = false;
            Check(!loaded && missingDialogs == prompts + 1 && tailCalls == tails &&
                  inputCloses == 1 && !inputOpen && Bytes(rejected) == source,
                "the real V2 model cancel stops after one prompt, closes input and preserves the source");
            const bool correct = !escaped && !probe.invalidNotices && probe.resets == 2 &&
                app->EnglishUI() == language && app->sceneRecoveryPending == persistent &&
                postloadNotices == finalNotices + unsigned(persistent);
            std::printf("MODEL_CANCEL_THROW type=%s persistent=%u escaped=%u resets=%u invalid=%u pending=%u correct=%u\n",
                action == V2AbortReset::ThrowStandard ? "std" : "unknown", unsigned(persistent),
                unsigned(escaped), probe.resets, probe.invalidNotices, unsigned(app->sceneRecoveryPending), unsigned(correct));
            allCorrect = allCorrect && correct;
            if (!correct) {
                Check(ResetAppState(app), "recover the isolated baseline after recording the mismatch");
                continue;
            }
            Finish(loaded, false, persistent);
            if (!persistent) continue;

            Check(postloadNativeObserved && postloadNoticeTextOk && postloadNoticeFlagsOk &&
                  postloadNoticePending && postloadNoticeInputClosed && postloadNoticeMutationGuard &&
                  postloadNestedRuns == 1 && postloadNestedBlocked && postloadNestedSceneIntact,
                "the final notice preserves the caller language and blocks reentrant open");
            const auto* retained = app->ModelSlot(0);
            Check(!fs::exists(blocked) && !SaveSceneCopy(app, blocked.c_str()) && !fs::exists(blocked),
                "the partially loaded model scene cannot be saved");
            const auto reads = inputReads;
            const auto pendingNotices = recoveryGateNotices;
            recoveryGateReopenPath = healthy;
            ArmRecoveryGate(action == V2AbortReset::ThrowStandard ? RecoveryGateReset::Runtime : RecoveryGateReset::Unknown,
                            language);
            const bool reopened = LoadSceneFile(healthy.c_str());
            Check(!reopened && recoveryGateResets == 1 && recoveryGateNotices == pendingNotices + 1 &&
                  recoveryGateNoticeTextOk && recoveryGateNestedBlocked && inputReads == reads &&
                  app->ModelSlot(0) == retained && app->CurrentFrame() == kQuarantineResidualFrame &&
                  app->EnglishUI() == language && app->sceneRecoveryPending && !app->EnvFileName()[0],
                "a throwing pending reset rejects the new project before reading it and preserves isolation");
            DisarmRecoveryGate();
            Normal(healthy, camera, light);
            Check(!app->sceneRecoveryPending && !app->ModelSlot(0) && Bytes(rejected) == source,
                "the next genuine reset opens the healthy project, releases the model and preserves the rejected source");
        }
    }
    Check(allCorrect, "V2 model cancellation contains standard and unknown reset exceptions without misreporting project corruption");
}

void ReleaseTracksAndFailReset(MMDApp& owner) {
    const bool owned = &owner == app && midResetFault &&
        (recoveryGateMode || (v2ModelAbortThrow && v2ModelAbortThrow->resets != 0));
    const auto generation = owner.SceneGeneration();
    realReleaseGlobalTracks(owner);
    if (!owned || (!midResetFault->persistent && midResetFault->failures)) return;
    Check(owner.SceneMutationInProgress() && owner.SceneGeneration() == generation &&
        !owner.ModelSlot(0) && !owner.CameraKeys() && !owner.LightKeys() &&
        !owner.ShadowKeys() && !owner.GravityKeys(),
        "fail inside the real reset after native model and global track teardown");
    ++midResetFault->failures;
    faultThrown = true;
    if (midResetFault->unknown) throw 17;
    throw std::bad_alloc();
}

BOOL WINAPI ObserveResetRedraw(HWND window, const RECT* rect, HRGN region, UINT flags) {
    if (midResetFault && midResetFault->failures && window == app->MainWindow() &&
        !app->CameraKeys() && !app->LightKeys() && (flags & RDW_UPDATENOW)) {
        ++midResetFault->partialRedraws;
        if (!app->sceneRecoveryPending && !app->SceneMutationInProgress())
            ++midResetFault->unprotectedRedraws;
    }
    return realResetRedraw(window, rect, region, flags);
}

void MidResetFailureCases() {
    Hook(&ReleaseGlobalTimelineTracks, &ReleaseTracksAndFailReset, reinterpret_cast<void**>(&realReleaseGlobalTracks));
    Hook(&RedrawWindow, &ObserveResetRedraw, reinterpret_cast<void**>(&realResetRedraw));
    const auto rejected = directory / L"mid-reset-cancel.pmm";
    const auto healthy = directory / L"mid-reset-healthy.pmm";
    const auto blocked = directory / L"mid-reset-blocked.pmm";
    WriteMissingV2(rejected);
    WriteV1(healthy);
    const auto source = Bytes(rejected);
    const auto camera = CameraRoot();
    const auto light = LightRoot();
    bool allProtected = true;
    for (const bool legacy : {false, true}) {
        for (const bool persistent : {false, true}) {
            Normal(healthy, camera, light);
            const auto language = persistent ? kUiChinese : kUiEnglish;
            app->EnglishUI() = language;
            V2ModelAbortThrow dialog{V2AbortReset::Forward, language, persistent};
            MidResetFault failure{persistent, legacy};
            const auto notices = postloadNotices, prompts = missingDialogs;
            if (legacy) {
                Check(plugins::PrepareProjectLoad(app, rejected.c_str()), "prepare the legacy caller's project load");
                wcscpy_s(app->EnvFileName(), 256, rejected.c_str());
            }
            Arm(rejected, Fault::None, false);
            ResetPostloadObservation(language);
            postloadReopenPath = healthy;
            midResetFault = &failure;
            v2ModelAbortThrow = &dialog;
            bool loaded = false;
            if (legacy) {
                int fd = -1;
                Check(_wsopen_s(&fd, rejected.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IWRITE) == 0,
                    "open the legacy input with production read-only flags");
                char header[30]{};
                Check(_read(fd, header, sizeof(header)) == sizeof(header), "read the real V2 header before calling the legacy entry");
                LoadSceneV2(app, fd);
                plugins::CancelProjectLoad(app);
            } else {
                loaded = LoadSceneFile(rejected.c_str());
            }
            v2ModelAbortThrow = nullptr;
            monitor = false;
            Check(!loaded && missingDialogs == prompts + 1 && dialog.resets == 2 &&
                !dialog.invalidNotices && inputCloses == 1 && !inputOpen &&
                checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
                failure.failures == (persistent ? 2u : 1u) && app->EnglishUI() == language &&
                app->sceneRecoveryPending == persistent && postloadNotices == notices + unsigned(persistent),
                "cancellation closes its input and tries the real reset twice with correct feedback");
            if (persistent) {
                Check(!app->CameraKeys() && !app->LightKeys() && postloadNestedBlocked && postloadNoticePending,
                    "the persistent failure isolates missing tracks and refuses reentry from its warning");
                Check(!SaveSceneCopy(app, blocked.c_str()) && !fs::exists(blocked),
                    "saving the partially reset project creates no output");
                const auto reads = inputReads;
                const auto opens = recoveryGateNotices;
                ArmRecoveryGate(RecoveryGateReset::Forward, language);
                Check(!LoadSceneFile(healthy.c_str()) && recoveryGateResets == 1 &&
                    recoveryGateNotices == opens + 1 && inputReads == reads && !inputOpen &&
                    failure.failures == 3 && app->sceneRecoveryPending && !app->CameraKeys(),
                    "opening another project fails before reading it when the real reset still cannot finish");
                DisarmRecoveryGate();
            } else {
                Check(app->CameraKeys() && app->LightKeys() && app->ShadowKeys() && app->GravityKeys(),
                    "the one-shot failure is cleared by the bounded retry");
            }
            std::printf("MID_RESET legacy=%u persistent=%u faults=%u partial_redraws=%u unprotected=%u\n",
                unsigned(legacy), unsigned(persistent), failure.failures, failure.partialRedraws, failure.unprotectedRedraws);
            allProtected = allProtected && failure.unprotectedRedraws == 0 &&
                (!legacy || failure.partialRedraws != 0);
            midResetFault = nullptr;
            Normal(healthy, camera, light);
            Check(!app->sceneRecoveryPending && Bytes(rejected) == source,
                "after the fault is removed, the healthy project opens and the rejected PMM bytes remain intact");
        }
    }
    Check(allProtected, "reset must quarantine partial state before any unguarded synchronous redraw");
}
// Exercise the actual legacy entry contract: the caller owns preparation and
// project identity, and passes the single descriptor just past its PMM header.
void LegacyRelocationOpen(const fs::path& path, bool v1) {
    std::printf("STAGE relocation legacy V%u direct open\n", v1 ? 1u : 2u);
    Check(plugins::PrepareProjectLoad(app, path.c_str()), "prepare the legacy parser caller's project");
    wcscpy_s(app->EnvFileName(), 256, path.c_str());
    Arm(path, Fault::None, false);
    int fd = -1;
    Check(_wsopen_s(&fd, path.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IWRITE) == 0,
          "open the legacy PMM with the shell's read-only flags");
    char header[30]{};
    Check(_read(fd, header, sizeof header) == sizeof header &&
          !std::memcmp(header + 20, v1 ? "0001" : "0002", 4), "consume the actual legacy header");
    if (v1) LoadSceneV1(app, fd); else LoadSceneV2(app, fd);
    Finish(tailObserved, true);
}

#include "pmm_cache_lifecycle_host_checks.inc"
#include "pmm_cache_save_host_checks.inc"

void TextureCacheOwnerCases(std::wstring_view step) {
    namespace manager = cache_manager;
    const auto root = directory / L"texture-owner";
    const auto cache = directory / L"texture-owner-store" / L"Cache";
    const auto primary = root / L"primary.pmm", secondary = root / L"secondary.pmm";
    const auto mesh = root / L"textured.x", texture = root / L"image.tga";
    constexpr DWORD expectedColor = 0x3288c5;
    const auto camera = CameraRoot();
    const auto light = LightRoot();
    if (step == L"prepare") {
        Check(!fs::exists(cache) && fs::create_directory(root), "own a fresh cross-process fixture and disk cache");
        WriteTextureFixture(texture, expectedColor);
        WriteTexturedAccessoryFixture(mesh);
        WriteV1AccessoryMaterialRetry(secondary, mesh);
        Normal(secondary, camera, light);
        Check(AccessoryTextureColor(texture) == expectedColor && SaveSceneCopy(app, primary.c_str()) &&
              !fs::exists(cache), "prepare both PMM versions with the same real texture without populating the disk cache");
        const auto output = Bytes(primary);
        Check(output.size() > 30 && std::memcmp(output.data(), "Polygon Movie maker 0002", 24) == 0,
              "the primary fixture is the production writer's V2 PMM");
        return;
    }
    Check(fs::is_regular_file(primary) && fs::is_regular_file(secondary), "read only the staged project fixtures");
    const bool cold = step == L"cold", share = step == L"share", verify = step == L"verify";
    const bool repair = step == L"repair", repairedWarm = step == L"repaired-warm";
    const auto inspect = [&](bool shared, const fs::path& current) {
        const auto view = manager::Inspect();
        Check(view.error.empty() && view.effectBytes == 0 && view.textureBytes > 0 &&
              view.projects.size() == (shared ? 2u : 1u),
              "the real persisted inventory contains exactly the expected projects and texture bytes");
        bool sawPrimary = false, sawSecondary = false;
        for (const auto& row : view.projects) {
            const fs::path owner(row.path);
            sawPrimary |= owner == primary;
            sawSecondary |= owner == secondary;
            Check(row.lastOpened > 0 && row.current == (owner == current) && row.totalBytes == view.textureBytes &&
                  row.sharedBytes == (shared ? view.textureBytes : 0) &&
                  row.reclaimableBytes == (shared || !current.empty() ? 0 : view.textureBytes),
                "the inventory attributes shared bytes and makes only the inactive exclusive texture releasable");
        }
        Check(sawPrimary && sawSecondary == shared, "the actual cache owners have the exact staged PMM paths");
        std::printf("TEXTURE-OWNER inventory projects=%zu bytes=%llu shared=%u current=%ls\n",
            view.projects.size(), view.textureBytes, unsigned(shared), current.filename().c_str());
    };
    if (cold || build_profile::original) {
        Check(!fs::exists(cache), "cold and original-profile starts have no disk cache");
    } else if (!repair) {
        // Inspect from a fresh process before any PMM load can create or bind the rows again.
        inspect(verify || repairedWarm, {});
    }
    if (verify) return;
    const auto& project = share ? secondary : primary;
    Check(!FindTextureResource(app->Renderer(), texture.c_str()), "the new renderer has no texture from this project yet");
    const auto before = texture_prepare_cache::GetStatistics();
    Normal(project, camera, light);
    const auto after = texture_prepare_cache::GetStatistics();
    const auto hits = after.hits - before.hits;
    const auto native = after.nativeCalls - before.nativeCalls;
    const auto stores = after.stores - before.stores;
    const auto rgb = AccessoryTextureColor(texture);
    const auto* key = app->AccessoryKeys(0);
    Check(rgb == expectedColor && key && key[0].position[0] == 2.5f && key[0].scale == 1.25f &&
          !app->AccessorySlot(1) && !app->ModelSlot(0),
        "cold and warm PMM loads agree on the texture, accessory key, and scene objects");
    std::printf("TEXTURE-OWNER load step=%ls project=%ls rgb=%06lx hits=%llu native=%llu stores=%llu\n",
        std::wstring(step).c_str(), project.filename().c_str(), static_cast<unsigned long>(rgb), hits, native, stores);
    if (build_profile::original) {
        Check(!hits && !native && !stores && manager::Root().empty() && !fs::exists(cache),
            "original-profile output matches with disk caching disabled");
    } else {
        Check(cold || repair ? hits == 0 && native == 1 && stores == 1 : hits == 1 && native == 0 && stores == 0,
            "a missing or damaged entry decodes and stores once, and a healthy entry only hits disk");
        inspect(share || repair || repairedWarm, project);
    }
}

void OwnerLifetimeCases(const fs::path& project, const mdl::CameraKey& camera, const mdl::LightKey& light) {
    std::puts("STAGE relocation ticket identity across nested scopes and independent apps");
    const auto live = RelocationTicket::Capture(*app);
    Check(WriteThrough(live), "completed host can learn a concrete row");
    RelocationTicket outerTicket, innerTicket;
    constexpr wchar_t innerRow[] = LR"(T:\nested-scene\Effects\row.fx)";
    {
        auto other = std::make_unique<MMDApp>();
        CheckRetired(RelocationTicket::Capture(*other));
        // Exercise the scope identity comparison without reentering a guarded
        // UI open. A superseding generation must survive the outer failure.
        using Scope = project_relocation::LoadScope;
        {
            Scope outer(*other);
            const bool result = outer.Run([&] {
                other->AdvanceSceneGeneration(); Scope::Activate(*other);
                outerTicket = RelocationTicket::Capture(*other);
                Check(WriteThrough(outerTicket), "independent app establishes its own tentative mapping");
                Scope inner(*other);
                Check(inner.Run([&] {
                    other->AdvanceSceneGeneration(); Scope::Activate(*other);
                    innerTicket = RelocationTicket::Capture(*other);
                    Check(innerTicket.Infer(ownerRow).status == project_relocation::Status::NoMatch,
                          "new generation starts with empty roots");
                    return innerTicket.Confirm(project_model::Reference{{ownerRow}}, innerRow);
                }), "inner owner accepts a distinct mapping");
                inner.Complete();
                return false;
            });
            Check(!result, "outer attempt takes its failure exit");
        }
        CheckRetired(outerTicket);
        Check(innerTicket.Infer(ownerRow).path == innerRow &&
              innerTicket.Confirm(project_model::Reference{{ownerRow}}, innerRow),
              "outer cleanup leaves only the superseding owner's mapping writable");
    }
    CheckRetired(innerTicket);
    Check(live.Infer(ownerRow).path == resolvedOwnerRow, "independent app destruction leaves host owner intact");
    std::puts("STAGE relocation final shell localization failure");
    Arm(project, Fault::None, false);
    failFinalLocalize = true;
    bool sawFault = false;
    try { (void)LoadSceneFile(project.c_str()); }
    catch (const LocalizeFault&) { sawFault = true; }
    monitor = false;
    Check(sawFault && !failFinalLocalize && tailObserved && inputCloses == 1 && !inputOpen &&
          app->EnvFileName()[0] == 0 && !app->SceneMutationInProgress(), "final localization fault unwinds the shell");
    CheckRetired(live);
    CheckRetired(localizeTicket);
    CheckRetired(RelocationTicket::Capture(*app));
    Normal(project, camera, light);
    const auto healthy = RelocationTicket::Capture(*app);
    std::puts("STAGE relocation accepted and refused reset boundaries");
    Check(WriteThrough(healthy), "fresh completed project owns its own roots after failure");
#if !MMD_ORIGINAL_PROFILE
    Check(plugins::PrepareProjectLoad(app, project.c_str()), "occupy the real plugin load transaction");
    const auto beforeRefusal = app->SceneGeneration();
    Check(!LoadSceneFile(project.c_str()) && app->SceneGeneration() == beforeRefusal &&
          RelocationTicket::Capture(*app).Infer(ownerRow).path == resolvedOwnerRow && WriteThrough(healthy),
          "rejected shell preparation leaves the completed project owner usable");
    plugins::CancelProjectLoad(app);
    {
        auto* history = plugins::NativeHistory(app);
        std::unique_ptr<plugins::SharedHistory::ResetTransaction> held;
        Check(history && history->PrepareReset(held) == MMS_OK && held, "hold a genuine competing history reset");
        const auto gen = app->SceneGeneration();
        Check(!ResetAppState(app) && app->SceneGeneration() == gen && healthy.Infer(ownerRow).path == resolvedOwnerRow &&
              WriteThrough(healthy), "healthy independent reset refusal retains its existing writable owner");
    }
#endif
    Check(ResetAppState(app), "accept independent reset");
    CheckRetired(healthy);
    CheckRetired(RelocationTicket::Capture(*app));
    Normal(project, camera, light);
}

void WriteV1ModelProject(const fs::path& path, const fs::path& emptyV1,
                         const std::vector<fs::path>& originals, const fs::path& accessory = {},
                         const fs::path& secondAccessory = {}) {
    const auto base = Bytes(emptyV1);
    constexpr size_t modelCount = 30 + 16 + 6 + 1;
    Check(base.size() == 1073 && base[modelCount] == 0, "start from the checked no-object V1 stream");
    Wire wire;
    wire.bytes.assign(base.begin(), base.begin() + modelCount);
    wire.Value(static_cast<std::uint8_t>(originals.size()));
    for (size_t i = 0; i < originals.size(); ++i) wire.Text("relocation", 20);
    for (size_t i = 0; i < originals.size(); ++i) {
        wire.Value(static_cast<std::uint8_t>(i));
        wire.Text("relocation", 20);
        wire.Text(WirePathText(originals[i]).c_str(), 256);
        wire.Value(static_cast<std::uint8_t>(i + 1)); wire.Value(std::uint8_t(1)); // order, visible
        wire.Zero(5 * 4); wire.Value(std::uint8_t(0)); wire.Zero(2 * 4); // selectors, groups, scroll/frame
        // One real PMD bone, no morphs and no IK. Give each dense key a
        // distinct translation so swapping identical filenames is observable.
        wire.Zero(12 + 16);
        wire.Value(float(10 + i)); wire.Zero(8 + 12); wire.Value(1.f);
        wire.Value(std::uint8_t(1)); wire.Zero(4 + 4); // allocated, sparse bone/morph counts
        wire.Zero(12); wire.Value(std::uint8_t(1)); wire.Value(std::uint8_t(1)); wire.Zero(4); // display root/count
        wire.Zero(12 + 12); wire.Value(1.f); wire.Zero(4 + 2); // current bone pose, repeated X, flags
    }
    if (accessory.empty()) {
        wire.bytes.insert(wire.bytes.end(), base.begin() + modelCount + 1, base.end());
    } else {
        Check(base[kV1AccessoryCountOffset] == 0, "the base V1 stream has an empty accessory block");
        wire.bytes.insert(wire.bytes.end(), base.begin() + modelCount + 1, base.begin() + kV1AccessoryCountOffset);
        WriteV1AccessoryBlock(wire, accessory, secondAccessory);
        wire.bytes.insert(wire.bytes.end(), base.begin() + kV1AccessoryCountOffset + 1, base.end());
    }
    wire.Write(path);
}

void SplitRootRelocationCases(const fs::path& emptyV1, const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto root = directory / L"split-roots";
    Check(fs::create_directory(root), "create a fresh split-root model fixture");
    const auto model = [](const fs::path& base, const wchar_t* group, const wchar_t* subdir) {
        return base / group / subdir / L"shared.pmd";
    };
    const auto write = [](const fs::path& path, unsigned vertices) {
        fs::create_directories(path.parent_path());
        Check(!fs::exists(path) && path.native().size() < 256, "write only a new bounded model fixture");
        Wire{geometry_test_reference::Pmd(vertices, true).bytes}.Write(path);
    };
    const auto old = root / L"old", other = root / L"other";
    const auto first = root / L"first", second = root / L"second", third = root / L"third";
    const auto independent = root / L"independent";
    const std::vector<fs::path> originals{
        model(old, L"Models", L"A"), model(old, L"Models", L"B"), model(old, L"Models", L"C"),
        model(old, L"Models", L"D"), model(old, L"Models", L"A"),
        model(other, L"Models", L"A"), model(other, L"Models", L"B")};
    const std::array<fs::path, 7> expected{
        model(first, L"Models", L"A"), model(second, L"Models", L"B"), model(third, L"Cast", L"C"),
        model(third, L"Cast", L"D"), model(first, L"Models", L"A"),
        model(independent, L"Models", L"A"), model(independent, L"Models", L"B")};
    constexpr std::array<unsigned, 7> counts{3, 6, 9, 12, 3, 15, 18};
    const auto v1 = root / L"source-v1.pmm", v2 = root / L"source-v2.pmm";
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u}) write(originals[i], counts[i]);
    WriteV1ModelProject(v1, emptyV1, originals);
    Normal(v1, camera, light);
    Check(SaveSceneCopy(app, v2.c_str()), "save the same model records using the production V2 writer");
    Normal(emptyV1, camera, light);
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u}) {
        fs::create_directories(expected[i].parent_path());
        fs::rename(originals[i], expected[i]);
    }
    write(model(first, L"Models", L"C"), 21);
    write(model(second, L"Models", L"C"), 24);
    write(model(third, L"Cast", L"A"), 27);
    const std::array<size_t, 4> manual{0, 1, 2, 5};
    for (const bool buffered : {false, true}) {
        pmm_io::bufferedReads = buffered;
        for (const auto& project : {v1, v2}) {
            std::printf("SPLIT-ROOTS %ls %s\n", project.filename().c_str(), buffered ? "buffered" : "direct");
            if (project == v1) {
                RelocationDialogs dialogs;
                for (auto i : manual) dialogs.choices.push_back(expected[i]);
                relocationDialogs = &dialogs;
                Normal(project, camera, light);
                relocationDialogs = nullptr;
                Check(dialogs.prompts == manual.size() && dialogs.picks == manual.size(),
                      "V1 asks only for the two roots, the ambiguous model and the independent root");
            } else {
                V2RelocationDialogs dialogs;
                for (auto i : manual) dialogs.replies.push_back({ProjectModelIssue::MissingFile, L"cache-model", 1, expected[i]});
                v2Relocation = &dialogs;
                Normal(project, camera, light);
                v2Relocation = nullptr;
                Check(dialogs.next == manual.size() && !dialogs.awaitingPicker,
                      "V2 asks only for the two roots, the ambiguous model and the independent root");
            }
            for (size_t i = 0; i < expected.size(); ++i) {
                auto* raw = app->ModelSlot(static_cast<int>(i));
                Check(raw && mdl::BoneKeys(raw)[0].position[0] == float(10 + i),
                      "keep the serialized pose on the corresponding model slot");
                const auto& record = *mdl::Mdl(raw);
                if (record.path != expected[i].native())
                    std::fwprintf(stderr, L"SPLIT-ROOTS wrong slot=%zu actual=%ls expected=%ls\n",
                                  i, record.path, expected[i].c_str());
                Check(record.path == expected[i].native() && record.vertexCount == counts[i],
                      "each slot uses the correct same-name file and geometry after recovering split roots");
            }
            const auto ticket = RelocationTicket::Capture(*app);
            Normal(emptyV1, camera, light);
            CheckRetired(ticket);
        }
    }
    std::puts("PASS split roots and repeated model paths reopen in V1/V2");
}

void V1RelocationCases(const fs::path& emptyV1, const mdl::CameraKey& camera, const mdl::LightKey& light) {
    const auto root = directory / (L"rel-" + std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(root), "own a fresh V1 relocation fixture root");
    const auto old = root / L"old", moved = root / L"new" / L"UserFile" / L"rel";
    const auto project = root / L"UserFile" / L"scene.pmm";
    const fs::path a = L"Models/A/rel04.pmd", b = L"Models/B/rel04.pmd",
                   c = L"Models/C/rel04.pmd", d = L"Models/D/rel04.pmd";
    const auto writeModel = [](fs::path path, unsigned vertices) {
        path.make_preferred();
        fs::create_directories(path.parent_path());
        Check(!fs::exists(path) && path.native().size() < 256, "own each exact model path within native capacity");
        Wire model; model.bytes = geometry_test_reference::Pmd(vertices, true).bytes; model.Write(path);
    };
    writeModel(moved / a, 3); writeModel(moved / b, 6);
    writeModel(old / c, 9); writeModel(moved / c, 12);
    const auto decoys = root / L"UserFile" / L"rel";
    writeModel(decoys / a, 15); writeModel(decoys / b, 18);
    const auto separate = root / L"separate" / d;
    writeModel(separate, 21);
    writeModel(root / d, 24); // plausible candidate only if the legacy fallback below were learned
    const fs::path e = L"Models/E/rel04.pmd";
    const auto repaired = root / L"repair.pmd";
    writeModel(repaired, 27);
    fs::create_directories((moved / e).parent_path());
    Wire bad; bad.Text("invalid-model", 20); bad.Write(moved / e);
    auto preferred = [](fs::path path) { path.make_preferred(); return path; };
    PathResolutionWorkspace probe = app->PathWorkspace();
    wcscpy_s(probe.projectDirectory, (root.native() + L"\\").c_str());
    Check(ResolveUserFilePath(probe, preferred(moved / b).c_str()) == preferred(decoys / b).native(),
          "control proves ordinary UserFile resolution would open the wrong real PMD");
    const auto verify = [&](unsigned slot, fs::path expected, unsigned vertices) {
        expected.make_preferred();
        auto raw = app->ModelSlot(slot);
        Check(raw && mdl::Mdl(raw)->path == expected.native() && mdl::Mdl(raw)->vertexCount == vertices &&
              mdl::Mdl(raw)->boneCount == 1 && mdl::BoneKeys(raw) &&
              mdl::BoneKeys(raw)[0].position[0] == float(10 + slot),
              "actual V1 slot keeps the exact path, distinct real geometry and its own serialized key");
    };
    const auto load = [&](std::vector<fs::path> originals, std::vector<fs::path> picks, bool badModel = false,
                          bool legacy = false) {
        for (auto& path : originals) path.make_preferred();
        for (auto& path : picks) path.make_preferred();
        WriteV1ModelProject(project, emptyV1, originals);
        RelocationDialogs dialogs{std::move(picks)};
        dialogs.expectBadModel = badModel;
        relocationDialogs = &dialogs;
        if (legacy) LegacyRelocationOpen(project, true); else Normal(project, camera, light);
        relocationDialogs = nullptr;
        Check(dialogs.picks == dialogs.choices.size() && dialogs.prompts == dialogs.picks,
              "only missing models without a usable inferred file needed a manual pick");
        Check(dialogs.sawBadModel == badModel, "the real PMD loader diagnosed the invalid mapped file exactly when expected");
    };
    const auto language = app->EnglishUI(); app->EnglishUI() = 1;
    load({old / a, old / b, old / c}, {moved / a});
    verify(0, moved / a, 3); verify(1, moved / b, 6); verify(2, old / c, 9);
    const auto fx = old / L"Effects" / L"row.fx", actualFx = moved / L"Effects" / L"row.fx";
    const auto v1Ticket = RelocationTicket::Capture(*app);
    Check(v1Ticket.Infer(fx.native()).path == actualFx.native(), "same-generation consumer reads real V1 model mapping");
    load({old / a, old / b}, {moved / a}, false, true);
    CheckRetired(v1Ticket);
    Check(RelocationTicket::Capture(*app).Infer(fx.native()).path == actualFx.native(),
          "direct V1 legacy success retains its mapping after the wrapper returns");
    // Reopening requires a fresh anchor. One inferred file has an invalid
    // header, the other disappeared. Their manual replacements must load.
    load({old / a, old / e, old / d}, {moved / a, repaired, separate}, true);
    verify(0, moved / a, 3); verify(1, repaired, 27); verify(2, separate, 21);
    load({old / a}, {moved / e, moved / a}, true);
    verify(0, moved / a, 3);
    // An ordinary UserFile fallback can load slot 0. Only a real manual
    // confirmation may teach old -> root and suppress slot 1's dialog.
    load({old / L"UserFile/rel" / a, old / d}, {separate});
    verify(0, decoys / a, 15); verify(1, separate, 21);

    std::puts("STAGE V1 cancel after real model relocation");
    const auto canceled = root / L"UserFile" / L"v1-canceled.pmm";
    WriteV1ModelProject(canceled, emptyV1, {preferred(old / a), preferred(old / L"missing.pmd")});
    const auto sourceBytes = Bytes(canceled);
    const std::wstring priorDirectory = app->DirModel();
    const auto recent = directory / L"recent.txt";
    for (const INT_PTR answer : {1, 2, 4}) {
        const auto recentBefore = fs::exists(recent) ? Bytes(recent) : std::vector<unsigned char>{};
        const auto tails = tailCalls, priorMessages = messages;
        RelocationDialogs dialogs{{preferred(moved / a)}};
        dialogs.expectAbort = true;
        dialogs.terminalAnswer = answer;
        if (answer == 1) dialogs.choices.emplace_back();
        relocationDialogs = &dialogs;
        Arm(canceled, Fault::None, false);
        const bool loaded = LoadSceneFile(canceled.c_str());
        Finish(loaded, false);
        relocationDialogs = nullptr;
        Check(!app->sceneRecoveryPending && tailCalls == tails && messages == priorMessages &&
              dialogs.prompts == 2 && dialogs.picks == dialogs.choices.size() &&
              Bytes(canceled) == sourceBytes && app->DirModel() == priorDirectory &&
              (fs::exists(recent) ? Bytes(recent) : std::vector<unsigned char>{}) == recentBefore,
              "picker cancellation, prompt close and explicit abort clear the partial scene without saving or recording the rejected path");
        CheckRetired(dialogs.ticket);
        Normal(emptyV1, camera, light);
    }

    RelocationDialogs dialogs{{preferred(moved / a), {}}};
    dialogs.expectAbort = true;
    const auto oldNotices = postloadNotices, oldTails = tailCalls;
    relocationDialogs = &dialogs;
    Arm(canceled, Fault::AbortResetQuarantine, false);
    const bool loaded = LoadSceneFile(canceled.c_str());
    monitor = false;
    relocationDialogs = nullptr;
    Check(!loaded && app->sceneRecoveryPending && inputCloses == 1 && !inputOpen &&
          pmm_io::checkedDescriptor == checkedBefore && quarantineRefusals == 2 &&
          app->ModelSlot(0) && !app->EnvFileName()[0] && !app->PathWorkspace().projectDirectory[0] &&
          postloadNotices == oldNotices + 1 && tailCalls == oldTails && Bytes(canceled) == sourceBytes,
          "V1 cancel with two refused resets quarantines the partial scene and reports recovery failure once");
    CheckRetired(dialogs.ticket);
    const auto blockedSave = root / L"must-not-save.pmm";
    Check(!SaveSceneCopy(app, blockedSave.c_str()) && !fs::exists(blockedSave),
          "the V1 cancellation quarantine blocks the real save writer");
    app->EnglishUI() = language;
    Normal(emptyV1, camera, light);
    Check(!app->sceneRecoveryPending && !app->ModelSlot(0),
          "a subsequent successful reset and open clears the V1 cancellation quarantine");
    const auto recoveredSave = root / L"recovered.pmm";
    Check(SaveSceneCopy(app, recoveredSave.c_str()) && fs::is_regular_file(recoveredSave),
          "the reopened editor can save to a new path after V1 cancellation");
    Released(canceled);
}
#include "pmm_v2_relocation_checks.inc"
#if !MMD_ORIGINAL_PROFILE
void WritePictureBitmap(const fs::path& path) {
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = 16; info.biHeight = 8;
    info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = 16 * 8 * 3;
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = file.bfOffBits + info.biSizeImage;
    std::array<unsigned char, 16 * 8 * 3> pixels{};
    for (std::size_t i = 0; i < pixels.size(); i += 3) { pixels[i] = 23; pixels[i + 1] = 67; pixels[i + 2] = 149; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file)); output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size()); output.close();
    Check(bool(output), "write a real small BMP for native PMM background loading");
}
void CheckPictureAsset(const media::BackgroundPictureAssetPtr& asset, const std::vector<unsigned char>& bytes) {
    Check(asset && asset->Id() && asset->Source() && asset->Source()->Size() == bytes.size() &&
          std::memcmp(asset->Source()->Data(), bytes.data(), bytes.size()) == 0,
          "PMM picture asset retains exact encoded BMP bytes independently of file lifetime");
    auto& api = d3dx::Get(); Check(api.Load() && api.fromMemEx, "resolve real memory decoder for retained PMM picture");
    struct Info { UINT width, height, depth, levels; D3DFORMAT format; D3DRESOURCETYPE resource; UINT fileFormat; } info{};
    static_assert(sizeof(Info) == 28, "D3DX image info ABI");
    IDirect3DTexture9* texture = nullptr;
    Check(SUCCEEDED(api.fromMemEx(app->Renderer()->device, asset->Source()->Data(), asset->Source()->Size(),
          16, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, 1, 1, 0, &info, nullptr, &texture)) && texture &&
          info.width == 16 && info.height == 8, "real D3DX restores PMM source after its original file is gone");
    D3DLOCKED_RECT pixels{}; Check(SUCCEEDED(texture->LockRect(0, &pixels, nullptr, D3DLOCK_READONLY)), "lock retained PMM image pixels");
    for (unsigned y = 0; y < 8; ++y) {
        const auto* row = reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(pixels.pBits) + y * pixels.Pitch);
        for (unsigned x = 0; x < 16; ++x) Check((row[x] & 0xffffff) == 0x954317, "retained PMM asset decodes the exact expected pixel color");
    }
    texture->UnlockRect(0); texture->Release();
}
class PictureVertexInspection {
    IDirect3DVertexBuffer9* original_ = nullptr;
public:
    PictureVertexInspection() {
        // Keep a real managed D3D buffer with the production size and vertex
        // format; remove WRITEONLY so the test can inspect production writes.
        // No refresh is called here: native loading must populate the vertices.
        auto*& current = app->PictureOverlayVertices();
        D3DVERTEXBUFFER_DESC description{};
        Check(current && SUCCEEDED(current->GetDesc(&description)) && description.Size == 168,
              "actual host initialized the complete picture overlay vertex buffer");
        IDirect3DVertexBuffer9* readable = nullptr;
        Check(SUCCEEDED(app->Renderer()->device->CreateVertexBuffer(description.Size,
              description.Usage & ~D3DUSAGE_WRITEONLY, description.FVF, description.Pool, &readable, nullptr)) && readable,
              "create equivalent readable D3D buffer for native picture geometry observation");
        original_ = current; current = readable;
    }
    ~PictureVertexInspection() {
        auto*& current = app->PictureOverlayVertices();
        if (current) current->Release();
        current = original_;
    }
    PictureVertexInspection(const PictureVertexInspection&) = delete;
    PictureVertexInspection& operator=(const PictureVertexInspection&) = delete;
};
void CheckPicturePresentation(bool visible) {
    const UINT state = GetMenuState(GetMenu(app->MainWindow()), 233, MF_BYCOMMAND);
    Check(state != UINT(-1) && ((state & MF_CHECKED) != 0) == visible,
          "native picture menu check matches project visibility including hidden pictures");
    Check(!app->RecordingWindow() && app->Renderer() && app->Renderer()->viewScale > 0,
          "picture geometry observation uses the real main-window viewport");
    struct Vertex { float x, y, z, reciprocalW; DWORD color; float u, v; };
    static_assert(sizeof(Vertex) == 28, "native picture vertex layout");
    const double ratio = app->Renderer()->viewScale / 1.2000000476837158;
    const float left = static_cast<float>(app->ViewportRect().left + 23 * ratio - 1);
    const float top = static_cast<float>(app->ViewportRect().top - 11 * ratio - 1);
    const float right = static_cast<float>(app->ViewportRect().left + 23 * ratio - 1 + 16 * .75 * ratio + 1);
    const float bottom = static_cast<float>(app->ViewportRect().top - 11 * ratio - 1 + 8 * .75 * ratio + 1);
    const Vertex expected[] = {
        {right, top, 0, 1, 0xffffffff, 1, 0}, {right, bottom, 0, 1, 0xffffffff, 1, 1},
        {left, top, 0, 1, 0xffffffff, 0, 0}, {left, top, 0, 1, 0xffffffff, 0, 0},
        {right, bottom, 0, 1, 0xffffffff, 1, 1}, {left, bottom, 0, 1, 0xffffffff, 0, 1}
    };
    void* memory = nullptr;
    auto* buffer = app->PictureOverlayVertices();
    Check(buffer && SUCCEEDED(buffer->Lock(0, sizeof(expected), &memory, D3DLOCK_READONLY)) && memory,
          "read actual native picture geometry after PMM load without requesting a refresh");
    Vertex actual[6]{}; std::memcpy(actual, memory, sizeof(actual));
    Check(SUCCEEDED(buffer->Unlock()), "release observed picture vertex buffer");
    for (unsigned i = 0; i != 6; ++i)
        Check(std::fabs(actual[i].x - expected[i].x) < .001f && std::fabs(actual[i].y - expected[i].y) < .001f &&
              actual[i].z == 0 && actual[i].reciprocalW == 1 && actual[i].color == 0xffffffff &&
              actual[i].u == expected[i].u && actual[i].v == expected[i].v,
              "visible and hidden picture quads already reflect saved placement in every native vertex");
}
std::string PictureWirePath(const fs::path& path) {
    char text[256]{};
    Check(WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, text, sizeof(text), nullptr, nullptr) > 0,
          "encode owned PMM picture fixture path");
    return text;
}
void SavePictureV2(const fs::path& target, const fs::path& picture, bool visible) {
    wcscpy_s(app->PictureBackgroundPath(), 256, picture.c_str());
    app->PictureOffsetX() = 23; app->PictureOffsetY() = -11; app->PictureScale() = .75f;
    app->PictureBackgroundEnabled() = visible ? 1 : 0;
    wcscpy_s(app->EnvFileName(), 256, target.c_str()); app->SceneModified() = 1;
    SaveSceneFile(app);
    Check(!app->SceneModified() && Bytes(target).size() > 30, "real writer saves complete V2 picture fixture");
}
void WritePictureCompanionAvi(const fs::path& path) {
    // Sixteen uncompressed frames include the PMM fixture's current frame 12.
    // No codec installation, file-size padding, or fake video loader is involved.
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = 16; info.biHeight = 8;
    info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = 16 * 8 * 3;
    PAVIFILE file = nullptr;
    Check(AVIFileOpenW(&file, path.c_str(), OF_WRITE | OF_CREATE, nullptr) == 0,
          "create isolated real AVI companion fixture");
    AVISTREAMINFOW description{};
    description.fccType = streamtypeVIDEO; description.dwScale = 1; description.dwRate = 30;
    description.rcFrame = {0, 0, 16, 8};
    PAVISTREAM stream = nullptr;
    Check(AVIFileCreateStreamW(file, &stream, &description) == 0,
          "create companion AVI video stream");
    Check(AVIStreamSetFormat(stream, 0, &info, sizeof(info)) == 0,
          "set companion AVI uncompressed bitmap format");
    std::array<unsigned char, 16 * 8 * 3> pixels{};
    for (int frame = 0; frame != 16; ++frame) {
        for (std::size_t pixel = 0; pixel != pixels.size(); pixel += 3) {
            pixels[pixel] = static_cast<unsigned char>(frame + 11);
            pixels[pixel + 1] = 73; pixels[pixel + 2] = 181;
        }
        Check(AVIStreamWrite(stream, frame, 1, pixels.data(), static_cast<LONG>(pixels.size()),
                             AVIIF_KEYFRAME, nullptr, nullptr) == 0,
              "write actual companion AVI frame");
    }
    AVIStreamRelease(stream); AVIFileRelease(file);
}
void PictureAndVideoProjectCases(const fs::path& bitmap, const std::vector<unsigned char>& encoded,
                                 const fs::path& emptyV2, const mdl::CameraKey& camera,
                                 const mdl::LightKey& light) {
    namespace identity = plugins::background_identity;
    Normal(emptyV2, camera, light);
    Check(!app->AviFile() && !app->AviStream() && !app->AviFrameReader(),
          "companion AVI fixture starts without an active video source");
    AVIFileInit();
    const auto video = directory / L"picture-companion.avi";
    WritePictureCompanionAvi(video);
    const auto instance = identity::Instance(app);
    for (bool visible : {true, false}) {
        const auto project = directory / (visible ? L"picture-video-visible-v2.pmm" : L"picture-video-hidden-v2.pmm");
        wcscpy_s(app->AviBackgroundPath(), 256, video.c_str());
        const auto beforeMessages = messages;
        LoadAviFile(app);
        Check(messages == beforeMessages && app->AviBackgroundEnabled() && app->AviFile() &&
              app->AviStream() && app->AviFrameReader(),
              "production video loader opens the real AVI before combined project save");
        app->AviOffsetX() = -7; app->AviOffsetY() = 19; app->AviScale() = .625f;
        app->AviBackgroundEnabled() = visible ? 1 : 0;
        Check(TryReplaceBackgroundPicture(app, bitmap.c_str()),
              "production image loader creates the combined project's picture");
        SavePictureV2(project, bitmap, visible);
        const auto oldTicket = identity::Acquire(app, identity::Kind::Picture);
        const auto oldAsset = identity::CurrentPictureAsset(app, instance, oldTicket);
        Normal(project, camera, light);
        const auto ticket = identity::Acquire(app, identity::Kind::Picture);
        const auto asset = identity::CurrentPictureAsset(app, instance, ticket);
        Check(messages == beforeMessages && !app->SceneModified() && app->AviFile() &&
              app->AviStream() && app->AviFrameReader() && app->AviFrameWidth() == 16 &&
              app->AviFrameHeight() == 8 && app->AviStreamStartFrame() == 0 &&
              app->AviStreamEndFrame() == 16 && app->AviBackgroundEnabled() == unsigned(visible) &&
              app->AviOffsetX() == -7 && app->AviOffsetY() == 19 && app->AviScale() == .625f &&
              std::wstring(app->AviBackgroundPath()) == video.wstring(),
              "real V2 parser retains loaded AVI resources placement visibility and clean state");
        const auto frame = static_cast<const BITMAPINFOHEADER*>(
            AVIStreamGetFrame(static_cast<PGETFRAME>(app->AviFrameReader()), app->CurrentFrame()));
        Check(frame && frame->biWidth == 16 && frame->biHeight == 8,
              "loaded combined project's AVI can decode its actual current frame");
        Check(ticket && ticket != oldTicket && !identity::IsCurrent(app, identity::Kind::Picture, oldTicket) &&
              oldAsset && asset && asset != oldAsset && asset->Id() != oldAsset->Id() &&
              asset->Source() == identity::CurrentPictureSource(app, instance, ticket) &&
              std::wstring(asset->Path()) == bitmap.wstring() && asset->Width() == 16 && asset->Height() == 8 &&
              app->PictureBackgroundTexture() && app->PictureBackgroundEnabled() == unsigned(visible) &&
              app->PictureOffsetX() == 23 && app->PictureOffsetY() == -11 && app->PictureScale() == .75f &&
              std::wstring(app->PictureBackgroundPath()) == bitmap.wstring(),
              "legitimate nested AVI mutation still publishes the V2 project's independent picture asset and placement");
        CheckPicturePresentation(visible);
        CheckPictureAsset(asset, encoded); CheckPictureAsset(oldAsset, encoded);
        Normal(emptyV2, camera, light);
        Check(!app->AviFile() && !app->AviStream() && !app->AviFrameReader() && !app->AviBackgroundEnabled(),
              "real project reset releases companion AVI handles before fixture reuse or deletion");
    }
    Check(DeleteFileW(video.c_str()) != FALSE, "delete owned AVI only after production closes every video handle");
    AVIFileExit();
    std::puts("PASS real V2 combined AVI and picture assets: visible hidden nested loading and clean placement");
}
void PictureProjectCases(const fs::path& emptyV1, const fs::path& emptyV2,
                         const mdl::CameraKey& camera, const mdl::LightKey& light, bool budgetMode) {
    namespace identity = plugins::background_identity;
    const auto bitmap = directory / L"retained-picture.bmp";
    WritePictureBitmap(bitmap); const auto encoded = Bytes(bitmap); const auto wirePath = PictureWirePath(bitmap);
    const auto instance = identity::Instance(app);
    Check(instance && app->Renderer() && app->Renderer()->device, "PMM picture suite uses the real registered production graphics host");
    PictureVertexInspection vertexInspection;
    for (bool visible : {true, false}) {
        const auto v1 = directory / (visible ? L"picture-visible-v1.pmm" : L"picture-hidden-v1.pmm");
        const auto v2 = directory / (visible ? L"picture-visible-v2.pmm" : L"picture-hidden-v2.pmm");
        WriteV1(v1, wirePath.c_str(), 23, -11, .75f, visible ? 1 : 0);
        Check(TryReplaceBackgroundPicture(app, bitmap.c_str()), "seed a genuine picture before native V2 save");
        SavePictureV2(v2, bitmap, visible);
        for (const auto& path : {v1, v2}) {
            Check(TryReplaceBackgroundPicture(app, bitmap.c_str()), "seed the previous project's genuine texture and asset");
            const auto oldTicket = identity::Acquire(app, identity::Kind::Picture);
            const auto oldAsset = identity::CurrentPictureAsset(app, instance, oldTicket);
            Check(oldAsset != nullptr, "previous project has an externally retained asset");
            const auto beforeMessages = messages;
            Normal(path, camera, light);
            const auto ticket = identity::Acquire(app, identity::Kind::Picture);
            const auto asset = identity::CurrentPictureAsset(app, instance, ticket);
            Check(messages == beforeMessages && !app->SceneModified() && app->PictureBackgroundTexture() &&
                  app->PictureBackgroundEnabled() == unsigned(visible) && app->PictureOffsetX() == 23 &&
                  app->PictureOffsetY() == -11 && app->PictureScale() == .75f &&
                  app->PictureWidth() == 16 && app->PictureHeight() == 8 &&
                  std::wstring(app->PictureBackgroundPath()) == bitmap.wstring(),
                  "actual V1/V2 picture parsing preserves nondefault placement visibility dimensions path and clean scene");
            Check(ticket && ticket != oldTicket && !identity::IsCurrent(app, identity::Kind::Picture, oldTicket) &&
                  !identity::CurrentPictureAsset(app, instance, oldTicket) && asset && asset != oldAsset &&
                  asset->Id() != oldAsset->Id() && asset->Width() == 16 && asset->Height() == 8 &&
                  std::wstring(asset->Path()) == bitmap.wstring() &&
                  asset->Source() == identity::CurrentPictureSource(app, instance, ticket),
                  "PMM replacement retires the old ticket and publishes its own complete immutable asset even when hidden");
            CheckPicturePresentation(visible);
            Check(DeleteFileW(bitmap.c_str()) != FALSE, "remove only fixture BMP after native PMM loading");
            CheckPictureAsset(asset, encoded); CheckPictureAsset(oldAsset, encoded);
            WritePictureBitmap(bitmap);
        }
    }
    PictureAndVideoProjectCases(bitmap, encoded, emptyV2, camera, light);
    const auto invalid = directory / L"broken-picture.bmp";
    { std::ofstream output(invalid, std::ios::binary | std::ios::trunc); output << "not an image"; output.close(); Check(bool(output), "write invalid image fixture"); }
    const auto badV1 = directory / L"broken-picture-v1.pmm", badV2 = directory / L"broken-picture-v2.pmm";
    const auto invalidWire = PictureWirePath(invalid);
    WriteV1(badV1, invalidWire.c_str()); SavePictureV2(badV2, invalid, true);
    const auto missing = directory / L"absent-picture.bmp";
    Check(!fs::exists(missing), "missing-picture fixture path genuinely does not exist");
    const auto missingV1 = directory / L"missing-picture-v1.pmm", missingV2 = directory / L"missing-picture-v2.pmm";
    const auto missingWire = PictureWirePath(missing);
    WriteV1(missingV1, missingWire.c_str()); SavePictureV2(missingV2, missing, true);
    const auto rejectPicture = [&](const fs::path& path, bool error) {
        Check(TryReplaceBackgroundPicture(app, bitmap.c_str()), "seed genuine old picture before empty or failed PMM image replacement");
        const auto oldTicket = identity::Acquire(app, identity::Kind::Picture);
        const auto oldAsset = identity::CurrentPictureAsset(app, instance, oldTicket);
        const auto charged = media::GlobalPictureSourceBudget().Usage().used;
        const auto beforeMessages = messages;
        expectedPictureError = error; Normal(path, camera, light); expectedPictureError = false;
        const auto ticket = identity::Acquire(app, identity::Kind::Picture);
        Check(messages == beforeMessages + unsigned(error) && !app->SceneModified() &&
              !app->PictureBackgroundTexture() && !app->PictureBackgroundPath()[0] && !app->PictureBackgroundEnabled() &&
              ticket && ticket != oldTicket && !identity::IsCurrent(app, identity::Kind::Picture, oldTicket) &&
              !identity::CurrentPictureAsset(app, instance, ticket) && !identity::CurrentPictureSource(app, instance, ticket),
              "empty or rejected V1/V2 picture leaves no old texture path enabled flag source or asset while other PMM content loads cleanly");
        Check(media::GlobalPictureSourceBudget().Usage().used == charged,
              "empty or failed PMM picture retains only the external old lease and leaks no candidate budget");
        CheckPictureAsset(oldAsset, encoded);
    };
    rejectPicture(emptyV1, false); rejectPicture(emptyV2, false);
    rejectPicture(badV1, true); rejectPicture(badV2, true);
    rejectPicture(missingV1, true); rejectPicture(missingV2, true);
    if (budgetMode) {
        constexpr std::uint64_t MiB = 1024ull * 1024;
        const auto oldLimit = media::GlobalPictureSourceBudget().Usage().limit;
        Check(system_settings::WriteSection("resources", "{\"backgroundPictureSourcesMiB\":16,\"pmmFutureSetting\":{\"keep\":47}}") == system_settings::Result::Ok,
              "configure budget only inside the explicitly isolated installation");
        const auto large = directory / L"over-budget-picture.bmp"; WritePictureBitmap(large);
        HANDLE file = CreateFileW(large.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "open owned BMP for bounded seventeen-MiB padding");
        LARGE_INTEGER size{}; size.QuadPart = 17 * MiB;
        const bool padded = SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
        CloseHandle(file); Check(padded, "pad encoded source without allocating seventeen MiB in test memory");
        const auto largeV1 = directory / L"over-budget-picture-v1.pmm", largeV2 = directory / L"over-budget-picture-v2.pmm";
        const auto largeWire = PictureWirePath(large);
        WriteV1(largeV1, largeWire.c_str()); SavePictureV2(largeV2, large, true);
        rejectPicture(largeV1, true); rejectPicture(largeV2, true);
        Check(media::GlobalPictureSourceBudget().Usage().limit == 16 * MiB,
              "actual PMM image loader applies the common configured source budget");
        Check(system_settings::WriteSection("resources", "{\"backgroundPictureSourcesMiB\":128}") == system_settings::Result::Ok,
              "restore default budget without replacing unknown resource members");
        std::string resources; Check(system_settings::ReadSection("resources", resources) == system_settings::Result::Ok &&
              settings_json::Object(resources).At("pmmFutureSetting").At("keep").UInt() == 47,
              "restoring source budget preserves future resource settings");
        media::GlobalPictureSourceBudget().Configure(oldLimit);
        Check(DeleteFileW(large.c_str()) != FALSE, "remove only the owned padded BMP");
    }
    Check(DeleteFileW(bitmap.c_str()) && DeleteFileW(invalid.c_str()), "remove owned small picture fixtures");
    Normal(emptyV2, camera, light); Normal(emptyV1, camera, light);
    std::puts("PASS real PMM picture assets: V1/V2 visible hidden empty invalid, retained bytes and actual memory decode");
}
#endif
void TailFailure(const fs::path& path, Fault type, bool reuseNumber) {
    const auto before = Bytes(path); const auto tails = tailCalls; const auto oldMessages = messages;
    Arm(path, type, reuseNumber);
    const bool loaded = LoadSceneFile(path.c_str()); Finish(loaded, false);
    Check(tailCalls == tails + 1 && messages == oldMessages + 1 && Bytes(path) == before,
        "the actual post-close tail fault takes exactly one existing error branch and preserves the PMM file");
}

void V1AccessoryMaterialRetryCase() {
    std::puts("STAGE real V1 X material refusal, replacement and reset");
    wchar_t savedCwd[32768]{};
    Check(GetCurrentDirectoryW(32768, savedCwd) != 0,
        "capture the process directory before the accessory loader changes it");
    const auto root = directory / (L"v1x-" + std::to_wstring(GetTickCount64()));
    Check(fs::create_directory(root), "own a fresh V1 material retry fixture directory");
    V1AccessoryMaterialRetry retry{root / L"bad.x", root / L"good.x"};
    const auto project = root / L"retry.pmm";
    Check(project.native().size() < 256, "the V1 material retry files fit the fixed path ABI");

    // Keep the quoted string short. 0x81 is a CP932 lead byte and 0x30
    // cannot trail it. The live D3DX load must preserve this byte sequence.
    constexpr char badTexture[] = "\x81" "0.bmp";
    std::string encoded;
    Check(!original_file_encoding::Name(badTexture, sizeof badTexture - 1, nullptr, 2047, encoded),
        "the on-disk texture filename fails the original CP932 field check");
    const auto writeX = [](const fs::path& path, std::string mesh, const char* texture) {
        mesh.insert(mesh.size() - 3, std::string("TextureFilename {\"") + texture + "\";}");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(mesh.data(), static_cast<std::streamsize>(mesh.size()));
        output.close();
        Check(bool(output), "write a real textured X triangle fixture");
    };
    writeX(retry.source, kAccessoryTriangleA, badTexture);
    writeX(retry.replacement, kAccessoryTriangleGood, "screen.bmp");
    WriteV1AccessoryMaterialRetry(project, retry.source);

    app->EnglishUI() = kUiEnglish;
    const auto tails = tailCalls;
    Arm(project, Fault::None, false);
    v1MaterialRetry = &retry;
    const bool loaded = LoadSceneFile(project.c_str());
    Finish(loaded, true);
    const auto* keys = app->AccessoryKeys(0);
    Check(accessoryLoads == 2 && accessoryPicks == 1 && retry.refusals == 1 && retry.prompts == 1 &&
          tailCalls == tails + 1 && app->CurrentFrame() == 12 && app->AccessorySlot(0) == retry.record &&
          retry.record && !std::wcscmp(retry.record->sourcePath, retry.replacement.c_str()) &&
          keys && keys[0].position[0] == 2.5f && keys[0].scale == 1.25f,
        "V1 completes the real retry, parses the accessory key and reaches the success tail");

    const auto generation = app->SceneGeneration();
    Check(ResetAppState(app), "the real scene reset succeeds after the V1 retry");
    v1MaterialRetry = nullptr;
    Check(retry.resetDisposals == 1 && app->SceneGeneration() != generation &&
          app->CurrentFrame() == 0 && !app->sceneRecoveryPending,
        "the reset disposes the replacement exactly once and restores the scene defaults");
    for (unsigned slot = 0; slot < 255; ++slot)
        Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot),
            "the real reset clears every model and accessory slot");
    Check(SetCurrentDirectoryW(savedCwd) != FALSE, "restore the directory changed by the X loader");

    // Check the retry boundary after the real parser and reset have both run.
    const auto& before = retry.beforeRetry;
    std::fprintf(stderr,
        "DIAG V1 before retry mesh=%p materials=%p paths=%p types=%p count=%u failedDisposals=%u resetDisposals=%u\n",
        before.pointers[0], before.pointers[1], before.pointers[2], before.pointers[3],
        before.materialCount, retry.failedDisposals, retry.resetDisposals);
    Check(before.Empty() && retry.failedDisposals == 1 && retry.disposedFailed.Allocated() &&
          (!retry.failed.Allocated() || retry.disposedFailed.pointers == retry.failed.pointers),
        "V1 must release the rejected mesh and material allocations and zero materialCount before retrying");
}

// ---- Real V2 missing-accessory recovery loop --------------------------------
// The production loop is exercised end to end: a genuinely missing .x source,
// a real existing but unparsable candidate, a cancelled picker, a missing or
// directory selection, the explicit skip and the two abort answers. Every load
// is the real LoadSceneFile/LoadAccessoryObject/D3DX parser and every save is
// the real PMM writer; only the two modal dialogs are driven from the replies.
unsigned V2AccessoryRecoveryMode() {
    wchar_t value[8]{};
    const DWORD length = GetEnvironmentVariableW(
        L"MMS_TEST_V2_ACCESSORY_RECOVERY", value, 8);
    return length == 1 && value[0] >= L'1' && value[0] <= L'6' ? value[0] - L'0' : 0;
}

struct V2MaterialBufferProbe;
thread_local V2MaterialBufferProbe* v2MaterialBufferProbe = nullptr;
d3dx::FnLoadMeshFromXW realV2MaterialMeshLoad = nullptr;

HRESULT WINAPI ObserveV2MaterialMeshLoad(LPCWSTR path, DWORD options, IDirect3DDevice9* device,
    void** adjacency, void** materials, void** effects, DWORD* count, void** mesh);

// Api::Load refreshes the static imports, so hook the resolved function
// around the one rejected open. The real D3DX mesh is published unchanged.
struct V2MaterialBufferProbe final : IUnknown {
    explicit V2MaterialBufferProbe(const fs::path& sourcePath) : source(sourcePath) {
        auto& api = d3dx::Get();
        Check(api.Load() && api.loadMeshFromXW, "resolve the real D3DX mesh loader for the scoped fault");
        target = api.loadMeshFromXW;
        Hook(target, &ObserveV2MaterialMeshLoad, reinterpret_cast<void**>(&realV2MaterialMeshLoad));
        v2MaterialBufferProbe = this;
    }

    V2MaterialBufferProbe(const V2MaterialBufferProbe&) = delete;
    V2MaterialBufferProbe& operator=(const V2MaterialBufferProbe&) = delete;

    ~V2MaterialBufferProbe() {
        v2MaterialBufferProbe = nullptr;
        Check(MH_RemoveHook(reinterpret_cast<void*>(target)) == MH_OK,
            "remove the D3DX detour before the healthy open");
        // Clean up the real reference on the red path after observing that
        // production left it outstanding. This bypasses the proxy's counter.
        if (buffer) buffer->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
        *output = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        Check(false, "the scoped material probe transfers one owned D3DX buffer reference");
        return 0;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        Check(buffer && releases == 0, "the loader releases its D3DX buffer reference once");
        ++releases;
        return std::exchange(buffer, nullptr)->Release();
    }

    // Slot 3 of ID3DXBuffer. This callback runs in host code after the real
    // D3DX call has returned and the scene record owns its material arrays.
    virtual void* STDMETHODCALLTYPE GetBufferPointer() {
        auto* record = app->AccessorySlot(0);
        Check(monitor && inputOpen && fault == Fault::Memory && !faultThrown &&
              buffer && loadedMesh && record && record->mesh == loadedMesh &&
              ReadAccessoryPayload(*record).Allocated(),
            "inject only after the V2 loader acquires the real mesh, buffer and material arrays");
        faultThrown = true;
        throw std::bad_alloc();
    }

    const fs::path& source;
    d3dx::FnLoadMeshFromXW target = nullptr;
    IUnknown* buffer = nullptr;
    void* loadedMesh = nullptr;
    unsigned releases = 0;
};

HRESULT WINAPI ObserveV2MaterialMeshLoad(LPCWSTR path, DWORD options, IDirect3DDevice9* device,
    void** adjacency, void** materials, void** effects, DWORD* count, void** mesh) {
    const HRESULT result = realV2MaterialMeshLoad(path, options, device, adjacency,
        materials, effects, count, mesh);
    auto* probe = v2MaterialBufferProbe;
    if (probe && path && !std::wcscmp(path, probe->source.c_str())) {
        Check(SUCCEEDED(result) && materials && *materials && count && *count == 1 &&
              mesh && *mesh && !probe->buffer && !probe->loadedMesh,
            "the selected X produces one real D3DX material buffer and mesh");
        probe->buffer = static_cast<IUnknown*>(*materials);
        probe->loadedMesh = *mesh;
        *materials = static_cast<IUnknown*>(probe);
    }
    return result;
}

// The normal two abort answers (4 = cancel, 2 = close) run the production
// ResetAppState and return false from LoadSceneFile without the old fault
// injection, so this is an independent assertion instead of Finish's
// faultThrown-based failure helper.
void FinishV2AccessoryAbort(bool loaded, const fs::path& source,
                            const std::vector<unsigned char>& sourceBytes) {
    monitor = false;
    const bool closed = !loaded && inputDescriptor >= 0 && !inputOpen &&
        inputCloses == 1 && checkedAtClose == checkedBefore &&
        pmm_io::checkedDescriptor == checkedBefore;
    if (!closed) {
        std::fprintf(stderr,
            "DIAG V2 accessory abort loaded=%u closes=%u open=%u checked=%d/%d expected=%d\n",
            unsigned(loaded), inputCloses, unsigned(inputOpen), checkedAtClose,
            pmm_io::checkedDescriptor, checkedBefore);
    }
    Check(closed,
        "the real accessory abort closes its PMM stream once and restores the prior read context");
    bool empty = !app->ModelSlot(0) && !app->AccessorySlot(0);
    for (unsigned slot = 1; slot < 255; ++slot)
        empty = empty && !app->ModelSlot(slot) && !app->AccessorySlot(slot);
    wchar_t title[256]{};
    GetWindowTextW(app->MainWindow(), title, 256);
    const bool reset = !tailObserved && !faultThrown && !app->sceneRecoveryPending &&
        app->CurrentFrame() == 0 && app->SceneModified() == 0 && empty &&
        !app->EnvFileName()[0] && !std::wcscmp(title, L"MikuMikuDance") &&
        !app->SceneMutationInProgress() && !IsWindowVisible(app->MainWindow());
    Check(reset,
        "the real accessory abort runs the normal reset without a success tail, leaves the empty scene editable and unquarantined, and clears the rejected save target");
    Check(Bytes(source) == sourceBytes,
        "the aborted source PMM stays byte-identical");
    Released(source);
}

void RecoveryPickerDirectoryCases() {
    const auto root = directory / L"picker-dirs";
    const auto originalModel = root / L"old-model" / L"rig.pmd";
    const auto movedModel = root / L"new-model" / L"rig.pmd";
    const auto oldFirst = root / L"old-acc" / L"first.x";
    const auto oldSecond = root / L"old-acc" / L"second.x";
    const auto newFirst = root / L"new-acc" / L"first.x";
    const auto newSecond = root / L"other-acc" / L"second.x";
    const auto empty = root / L"empty.pmm";
    const auto v1 = root / L"v1.pmm";
    const auto v2 = root / L"v2.pmm";
    for (const auto& path : {originalModel, movedModel, oldFirst, newFirst, newSecond})
        fs::create_directories(path.parent_path());
    const auto writeX = [](const fs::path& path) {
        std::ofstream output(path, std::ios::binary);
        output << kAccessoryTriangleGood;
        output.close();
        Check(bool(output), "write the owned accessory fixture");
    };
    writeX(oldFirst);
    writeX(oldSecond);
    Wire model;
    model.bytes = geometry_test_reference::Pmd(3, true).bytes;
    std::memset(model.bytes.data() + 7, 0, 20);
    std::memcpy(model.bytes.data() + 7, "picker-rig", 10);
    model.Write(originalModel);
    WriteV1(empty);
    WriteV1ModelProject(v1, empty, {originalModel}, oldFirst, oldSecond);
    Arm(v1, Fault::None, false);
    Finish(LoadSceneFile(v1.c_str()), true);
    Check(SaveSceneCopy(app, v2.c_str()), "produce a V2 project with the same model and two accessories");
    Check(SetCurrentDirectoryW(root.c_str()), "release the fixture resource directories before moving files");
    fs::rename(oldFirst, newFirst);
    fs::rename(oldSecond, newSecond);
    fs::rename(originalModel, movedModel);
    const std::wstring priorModel = root.native();
    const std::wstring priorAccessory = directory.native();
    constexpr UINT rememberFolder = 0x12D;
    const auto menu = GetMenu(app->MainWindow());
    const auto previousCheck = CheckMenuItem(menu, rememberFolder, MF_BYCOMMAND | MF_CHECKED);
    Check(previousCheck != UINT(-1), "enable the real remember-folder option");
    for (const auto& project : {v2, v1}) {
        const bool v2Project = project == v2;
        if (!v2Project) fs::rename(movedModel, originalModel);
        CopyDirPathW(app->DirModel(), priorModel.c_str());
        CopyDirPathW(app->DirAccs(), priorAccessory.c_str());
        const auto bytes = Bytes(project);
        for (bool cancel : {true, false}) {
            std::printf("STAGE recovery picker directories V%d cancel=%d\n", v2Project ? 2 : 1, int(cancel));
            V2RelocationDialogs models{{{ProjectModelIssue::MissingFile, L"picker-rig", 1, movedModel}}};
            models.expectedPickerDirectory = priorModel.c_str();
            V2AccessoryDialogs accessories;
            accessories.expectedLabel = L"material-retry";
            accessories.followingLabel = L"following";
            accessories.replies = {{1, newFirst}, {cancel ? INT_PTR(4) : INT_PTR(1), cancel ? fs::path{} : newSecond}};
            v2Relocation = v2Project ? &models : nullptr;
            v2Accessory = &accessories;
            Arm(project, Fault::None, false);
            const bool loaded = LoadSceneFile(project.c_str());
            if (cancel) FinishV2AccessoryAbort(loaded, project, bytes);
            else Finish(loaded, true);
            v2Accessory = nullptr;
            v2Relocation = nullptr;
            Check(models.next == unsigned(v2Project) && accessories.prompts == 2 &&
                  accessories.picks == (cancel ? 1u : 2u), "the open consumes only the expected recovery decisions");
            const std::wstring expectedModel = !cancel && v2Project ? movedModel.parent_path().native() : priorModel;
            const std::wstring expectedAccessory = cancel ? priorAccessory : newSecond.parent_path().native();
            if (cancel) {
                const bool modelFolderKept = app->DirModel() == expectedModel;
                const bool accessoryFolderKept = app->DirAccs() == expectedAccessory;
                std::printf("DIAG cancellation leaves model folder=%d accessory folder=%d\n",
                            int(modelFolderKept), int(accessoryFolderKept));
                Check(modelFolderKept && accessoryFolderKept,
                      "cancelled project must not publish either remembered picker directory");
                Check(RelocationTicket::Capture(*app).Infer(oldFirst.native()).status == project_relocation::Status::NoMatch,
                      "cancel retires the temporary accessory relocation before the retry");
            } else {
                Check(app->DirModel() == expectedModel && app->DirAccs() == expectedAccessory,
                      "successful retry publishes the last accepted picker folders");
                Check(app->AccessorySlot(0) && app->AccessorySlot(1) && app->AccessorySlot(0)->mesh &&
                      app->AccessorySlot(1)->mesh && fs::path(app->AccessorySlot(0)->sourcePath) == newFirst &&
                      fs::path(app->AccessorySlot(1)->sourcePath) == newSecond && app->ModelSlot(0),
                      "successful retry restores the model and both selected accessories");
            }
            std::vector<std::wstring> expectedPickerDirectories{priorAccessory};
            if (!cancel) expectedPickerDirectories.push_back(newFirst.parent_path().native());
            Check(accessories.pickerDirectories == expectedPickerDirectories,
                  "the retry starts from the old folder and later picks use the current open's staged folder");
            Check(Bytes(project) == bytes, "the original PMM is unchanged by cancellation or retry");
        }
    }
    CheckMenuItem(menu, rememberFolder, MF_BYCOMMAND | previousCheck);
    std::puts("PASS cancelled project recovery retains picker directories and successful retry publishes them");
}

void V1AccessoryRecoveryCases() {
    std::puts("STAGE real V1 missing accessory recovery, save/reopen, skip and cancel");
    const auto root = directory / L"v1-accessory-recovery";
    Check(fs::create_directory(root), "create a private V1 accessory fixture directory");
    const auto project = root / L"missing.pmm";
    const auto missing = root / L"missing.x";
    const auto invalid = root / L"invalid.x";
    const auto disappearing = root / L"disappearing.x";
    const auto replacement = root / L"replacement.x";
    const auto following = root / L"following.x";
    for (const auto& path : {invalid, disappearing, replacement, following}) {
        std::ofstream output(path, std::ios::binary);
        output << (path == invalid ? "not an X mesh" : kAccessoryTriangleGood);
        Check(bool(output), "write a real accessory fixture");
    }
    WriteV1AccessoryMaterialRetry(project, missing, following);
    const auto source = Bytes(project);
    const auto originalLanguage = app->EnglishUI();
    const auto originalBuffered = pmm_io::bufferedReads;
    for (const bool buffered : {false, true}) {
        pmm_io::bufferedReads = buffered;
        for (const unsigned char language : {kUiJapanese, kUiEnglish, kUiChinese}) {
            const auto cleanStart = [&] {
                Check(ResetAppState(app), "start the next V1 recovery operation with a clean scene");
                app->EnglishUI() = language;
            };
            const auto checkLoaded = [&](bool skipped) {
                const auto* first = app->AccessorySlot(0);
                const auto* second = app->AccessorySlot(1);
                const auto* firstKey = app->AccessoryKeys(0);
                const auto* secondKey = app->AccessoryKeys(1);
                Check(first && (skipped ? !first->mesh && !first->sourcePath[0]
                                       : first->mesh && fs::path(first->sourcePath) == replacement) &&
                      second && second->mesh && fs::path(second->sourcePath) == following &&
                      firstKey && firstKey[0].position[0] == 2.5f &&
                      secondKey && secondKey[0].position[0] == 7.5f &&
                      app->CurrentFrame() == 12 && Bytes(project) == source,
                    "V1 keeps accessory identity, both tracks, the playhead and the source bytes");
            };
            cleanStart();
            V2AccessoryDialogs retry;
            retry.expectedLabel = L"material-retry";
            retry.disappearingCandidate = disappearing;
            retry.replies = {{1, {}}, {1, invalid}, {1, disappearing}, {1, replacement}};
            const auto retryMessages = messages;
            Arm(project, Fault::None, false);
            v2Accessory = &retry;
            const bool recovered = LoadSceneFile(project.c_str());
            v2Accessory = nullptr;
            Finish(recovered, true);
            Check(retry.prompts == 4 && retry.picks == 4 && messages == retryMessages &&
                  retry.loadPaths == std::vector<std::wstring>{invalid.native(), disappearing.native(),
                      replacement.native(), following.native()} &&
                  retry.loadResults == std::vector<bool>{false, false, true, true},
                "V1 returns to recovery after picker cancel, malformed X or disappearance, then opens the valid selection");
            checkLoaded(false);
            const auto saved = root / (L"recovered-" + std::to_wstring(buffered) + L"-" +
                std::to_wstring(language) + L".pmm");
            Check(SaveSceneCopy(app, saved.c_str()), "save a copy of the recovered project");
            Arm(saved, Fault::None, false);
            Finish(LoadSceneFile(saved.c_str()), true);
            checkLoaded(false);

            cleanStart();
            V2AccessoryDialogs skip;
            skip.expectedLabel = L"material-retry";
            skip.disappearingCandidate = disappearing;
            skip.replies = {{1, missing}, {1, root}, {1, disappearing}, {3, {}}};
            const auto skipMessages = messages;
            Arm(project, Fault::None, false);
            v2Accessory = &skip;
            const bool continued = LoadSceneFile(project.c_str());
            v2Accessory = nullptr;
            Finish(continued, true);
            Check(skip.prompts == 4 && skip.picks == 3 && messages == skipMessages &&
                  skip.loadPaths == std::vector<std::wstring>{disappearing.native(), following.native()} &&
                  skip.loadResults == std::vector<bool>{false, true},
                "missing, directory and disappeared candidates add no native error before a V1 skip");
            checkLoaded(true);

            for (const INT_PTR answer : {INT_PTR{4}, INT_PTR{2}}) {
                cleanStart();
                V2AccessoryDialogs cancel;
                cancel.expectedLabel = L"material-retry";
                cancel.disappearingCandidate = disappearing;
                cancel.replies = {{1, invalid}, {1, disappearing}, {answer, {}}};
                const auto cancelMessages = messages;
                const auto cancelTails = tailCalls;
                Arm(project, Fault::None, false);
                v2Accessory = &cancel;
                const bool loaded = LoadSceneFile(project.c_str());
                FinishV2AccessoryAbort(loaded, project, source);
                v2Accessory = nullptr;
                Check(cancel.prompts == 3 && cancel.picks == 2 && cancel.normalAbortResets == 1 &&
                      cancel.loadPaths == std::vector<std::wstring>{invalid.native(), disappearing.native()} &&
                      cancel.loadResults == std::vector<bool>{false, false} &&
                      messages == cancelMessages && tailCalls == cancelTails,
                    "V1 cancel and close reset exactly once after a disappeared replacement");
                Arm(saved, Fault::None, false);
                Finish(LoadSceneFile(saved.c_str()), true);
                checkLoaded(false);
            }
        }
    }
    pmm_io::bufferedReads = originalBuffered;
    app->EnglishUI() = kUiEnglish;
    mdl::AccessoryRecord standalone{};
    expectStandaloneMissingAccessory = true;
    const bool loaded = LoadAccessoryObject(app, &standalone, missing.c_str());
    expectStandaloneMissingAccessory = false;
    Check(!loaded && standaloneMissingAccessoryNotices == 1,
        "opening a missing X outside project recovery still shows the standalone notice exactly once");
    app->EnglishUI() = originalLanguage;
    std::puts("PASS V1 disappeared replacement: retry, save/reopen, skip and cancel");
}

void VacRelocationCases(bool v1) {
    std::puts("STAGE VAC initial import and PMM save");
    const auto root = directory / L"vac-root";
    const auto original = root / L"old";
    const auto moved = root / L"moved";
    const auto projectRoot = root / L"project";
    const fs::path descriptor = L"UserFile/rig/placement.vac";
    const fs::path mesh = L"UserFile/rig/UserFile/meshes/shape.x";
    const fs::path decoy = L"UserFile/meshes/shape.x";
    const auto project = projectRoot / L"before.pmm";
    const auto saved = projectRoot / L"after.pmm";
    const auto autoRoot = root / L"automatic";
    const auto autoProject = autoRoot / L"UserFile" / L"after.pmm";
    const auto executableDecoy = directory.parent_path().parent_path() / decoy;
    Check(fs::create_directories((original / mesh).parent_path()) &&
          fs::create_directories((projectRoot / decoy).parent_path()), "create fresh VAC fixture roots");
    const auto write = [](const fs::path& path, const std::string& contents) {
        std::ofstream file(path, std::ios::binary);
        file << contents;
        file.close();
        Check(bool(file), "write owned VAC or X fixture");
    };
    write(original / descriptor, "placement\nUserFile\\meshes\\shape.x\n2.5\n1,2,3\n0,0,0\nnone\n0\n");
    std::string textured = kAccessoryTriangleGood;
    textured.insert(textured.size() - 3, "TextureFilename {\"image.tga\";}");
    write(original / mesh, textured);
    constexpr DWORD textureRgb = 0x3288c5;
    WriteTextureFixture((original / mesh).parent_path() / L"image.tga", textureRgb);

    Check(ResetAppState(app), "reset the isolated host before importing VAC");
    app->EnglishUI() = kUiEnglish;
    LoadAccessoryFile((original / descriptor).c_str());
    const auto* initial = app->AccessorySlot(0);
    Check(initial && initial->mesh && fs::path(initial->sourcePath) == original / descriptor &&
          initial->scale == 2.5f && initial->position[1] == 2.f,
          "ordinary VAC import stores the descriptor and the authored placement");
    if (v1) WriteV1AccessoryMaterialRetry(project, original / descriptor);
    else Check(SaveSceneCopy(app, project.c_str()), "save the original VAC project through the real PMM writer");
    const auto projectBytes = Bytes(project);
    Check(projectBytes.size() >= 24 && !std::memcmp(projectBytes.data() + 20, v1 ? "0001" : "0002", 4),
          "open the fixture through the requested PMM format");
    const auto descriptorBytes = Bytes(original / descriptor);
    const auto meshBytes = Bytes(original / mesh);
    Check(SetCurrentDirectoryW(directory.c_str()), "release the fixture working directory before moving its assets");
    fs::rename(original, moved);
    write(projectRoot / decoy, kAccessoryTriangleA);
    Check(fs::create_directories(executableDecoy.parent_path()), "create the isolated host UserFile decoy directory");
    write(executableDecoy, kAccessoryTriangleA);

    const auto checkAccessory = [&](const fs::path& assets) {
        const auto* object = app->AccessorySlot(0);
        Check(object && object->mesh && object->materialCount == 1 && object->materials &&
              fs::path(object->sourcePath) == assets / descriptor,
              "the recovered object retains the exact VAC descriptor");
        const auto color = static_cast<const D3DMATERIAL9*>(object->materials)->Ambient.r;
        std::printf("VAC mesh ambient.r=%.3f expected=0.200\n", color);
        Check(std::fabs(color - .2f) < .001f, "the VAC-relative X wins over the other UserFile decoys");
        const auto meshDirectory = (assets / mesh).parent_path();
        Check(fs::equivalent(object->directory, meshDirectory) &&
              AccessoryTextureColor(meshDirectory / L"image.tga") == textureRgb,
              "the actual material loads its texture from beside the resolved X");
        const auto* key = app->AccessoryKeys(0);
        Check(key && (v1 ? key[0].scale == 1.25f && key[0].position[0] == 2.5f
                         : key[0].scale == 2.5f && key[0].position[1] == 2.f),
              "recovery keeps the PMM's authored accessory root key placement");
        Check(Bytes(assets / descriptor) == descriptorBytes && Bytes(assets / mesh) == meshBytes,
              "the descriptor and mesh bytes stay intact");
    };
    std::puts("STAGE missing VAC recovered by selecting its new location");
    V2AccessoryDialogs recover;
    recover.expectedLabel = v1 ? L"material-retry" : L"shape.x";
    recover.replies = {{1, moved / descriptor}};
    Arm(project, Fault::None, false);
    v2Accessory = &recover;
    const bool recovered = LoadSceneFile(project.c_str());
    v2Accessory = nullptr;
    Finish(recovered, true);
    Check(recover.prompts == 1 && recover.picks == 1, "recover the external VAC with one picker selection");
    checkAccessory(moved);
    Check(Bytes(project) == projectBytes && SaveSceneCopy(app, saved.c_str()),
          "keep the source PMM intact and save the recovered descriptor to a new PMM");
    const auto savedBytes = Bytes(saved);
    Check(savedBytes.size() >= 24 && !std::memcmp(savedBytes.data() + 20, "0002", 4),
          "save the recovered VAC through the normal V2 writer");
    Arm(saved, Fault::None, false);
    Finish(LoadSceneFile(saved.c_str()), true);
    checkAccessory(moved);

    std::puts("STAGE automatic VAC relocation when a second project root contains the same hierarchy");
    Check(fs::create_directories((autoRoot / decoy).parent_path()), "create the second project UserFile root");
    Check(SetCurrentDirectoryW(directory.c_str()), "release the recovered asset directory before moving it again");
    fs::rename((moved / descriptor).parent_path(), (autoRoot / descriptor).parent_path());
    fs::copy_file(saved, autoProject);
    write(autoRoot / decoy, kAccessoryTriangleA);
    Arm(autoProject, Fault::None, false);
    Finish(LoadSceneFile(autoProject.c_str()), true);
    checkAccessory(autoRoot);
    Check(Bytes(saved) == savedBytes && Bytes(autoProject) == savedBytes,
          "automatic relocation and reopening leave both PMM copies intact");
    if (v1) {
        const auto autoV1 = autoProject.parent_path() / L"legacy.pmm";
        fs::copy_file(project, autoV1);
        Arm(autoV1, Fault::None, false);
        Finish(LoadSceneFile(autoV1.c_str()), true);
        checkAccessory(autoRoot);
        Check(Bytes(autoV1) == projectBytes, "automatic relocation also preserves the original V1 PMM bytes");
    }
    const auto uppercase = (autoRoot / descriptor).parent_path() / L"second.VAC";
    fs::copy_file(autoRoot / descriptor, uppercase);
    LoadAccessoryFile(uppercase.c_str());
    const auto* second = app->AccessorySlot(1);
    Check(second && second->mesh && fs::path(second->sourcePath) == uppercase &&
          fs::equivalent(second->directory, (autoRoot / mesh).parent_path()),
          "ordinary import recognizes an uppercase VAC extension and its nested mesh");
    std::printf("PASS VAC relocation V%d: one manual pick, automatic root recovery, exact mesh and texture, saved PMM reopens\n",
                v1 ? 1 : 2);
}

void V2AccessoryRecoveryCases(unsigned mode) {
    std::puts("STAGE real V2 missing-accessory recovery loop");
    const auto savedLanguage = app->EnglishUI();
    wchar_t savedCwd[32768]{};
    Check(GetCurrentDirectoryW(32768, savedCwd) != 0,
        "capture the process directory before the real accessory loader changes it");
    // A unique, previously absent fixture root. Nothing in this group removes
    // or overwrites it, so an independent acceptance run can inspect the exact
    // X/PMM files this run produced.
    wchar_t uniqueSuffix[48]{};
    Check(swprintf_s(uniqueSuffix, 48, L"v2acc-%lu-%llu",
          GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64())) > 0,
        "compose a unique V2 accessory recovery fixture directory name");
    const auto root = directory / uniqueSuffix;
    Check(!fs::exists(root) && root.native().size() < 256,
        "the unique V2 accessory recovery fixture root is new and fits the PMM path ABI");
    Check(fs::create_directory(root),
        "own a fresh isolated V2 accessory recovery fixture root");
    const auto projectFolder = mode == 1 ? root / L"UserFile" : root;
    const auto sourceA = root / L"a.x", sourceB = projectFolder / L"b.x";
    const auto bad = (mode == 1 ? root / L"invalid" : root) / L"bad.x";
    const auto good = (mode == 1 ? root / L"external" / L"UserFile" / L"replacement" : root) / L"good.x";
    const auto decoy = projectFolder / L"replacement" / L"good.x";
    const auto directoryCandidate = root / L"dir.x";
    const auto absent = root / L"gone.x";
    const auto disappearing = bad.parent_path() / L"disappearing.x";
    const auto movedSource = root / L"moved-a.x";
    const auto project = projectFolder / L"p.pmm";
    const auto healthy = root / L"h.pmm";
    for (const auto* path : {&sourceA, &sourceB, &bad, &good,
                             &directoryCandidate, &absent, &disappearing, &movedSource,
                             &project, &healthy})
        Check(path->native().size() < 256,
            "each generated accessory recovery fixture path fits the PMM path ABI");
    const auto writeX = [](const fs::path& path, const char* text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text, static_cast<std::streamsize>(std::strlen(text)));
        output.close();
        Check(bool(output), "write a real isolated X accessory fixture");
    };
    if (mode == 1) {
        Check(fs::create_directories(decoy.parent_path()) && fs::create_directory(bad.parent_path()) &&
              fs::create_directories(good.parent_path()),
            "isolate the project and external UserFile trees with the same replacement suffix");
        writeX(decoy, kAccessoryTriangleA);
    }
    writeX(sourceA, kAccessoryTriangleA);
    writeX(sourceB, kAccessoryTriangleB);
    writeX(good, kAccessoryTriangleGood);
    writeX(bad, "this file exists but is not a parseable DirectX mesh\n");
    if (mode == 1) writeX(disappearing, kAccessoryTriangleA);
    Check(fs::create_directory(directoryCandidate),
        "own a real directory candidate for the accessory picker");
    Check(!fs::exists(absent),
        "the absent replacement path genuinely does not exist");

    // Build the fixture through the real importer and writer: two actual X
    // accessories, one allocated root key plus one sparse key each, and the
    // same camera/light root the ordinary V2 fixture uses.
    Check(ResetAppState(app), "reset the actual hidden host before the V2 accessory fixture");
    app->state.optflag[0] = 1;
    app->SetSelectedModelSlot(0);
    app->CurrentFrame() = 12;
    const auto camera = CameraRoot();
    const auto light = LightRoot();
    app->CameraKeys()[0] = camera;
    app->LightKeys()[0] = light;
    app->CameraFov() = 43.f;
    for (unsigned i = 0; i < 3; ++i) {
        app->LightColor()[i] = light.color[i];
        app->LightDirection()[i] = light.direction[i];
    }
    LoadAccessoryFile(sourceA.c_str());
    auto* first = app->AccessorySlot(0);
    Check(first && first->mesh, "the fixture imports the first real X accessory");
    LoadAccessoryFile(sourceB.c_str());
    auto* second = app->AccessorySlot(1);
    Check(second && second->mesh && !app->AccessorySlot(2),
        "the fixture imports the second real X accessory into its own slot");
    Check(!std::wcscmp(first->sourcePath, sourceA.c_str()) &&
          !std::wcscmp(second->sourcePath, sourceB.c_str()),
        "the fixture's real accessory loads keep their exact resolved source paths");
    Check(!std::strcmp(first->name, "a.x") && !std::strcmp(second->name, "b.x"),
        "the fixture's two real accessory records carry the names the recovery prompt will use");
    // The importer already allocated each slot's 10000-key pool with a seeded
    // root record. Fill that root and one index-1 sparse key directly with a
    // legal chain (frame 1 round-trips back to index 1), using the current
    // mdl::AccessoryKey fields; do not synthesize PMM offsets.
    auto* firstKeys = app->AccessoryKeys(0);
    auto* secondKeys = app->AccessoryKeys(1);
    Check(firstKeys && secondKeys &&
          firstKeys[0].frame == 0 && secondKeys[0].frame == 0,
        "the fixture's real accessory imports own their allocated root keyframe records");
    const auto fillKey = [](mdl::AccessoryKey& key, unsigned frame, unsigned previous,
                            unsigned next, const float* position, const float* rotation,
                            float scale, float opacity, int parentModel,
                            unsigned char visible, unsigned char shadow) {
        key.frame = frame;
        key.previous = previous;
        key.next = next;
        key.visible = visible;
        key.shadowEnabled = shadow;
        key.parentModel = parentModel;
        key.parentBone = 0;
        key.selected = 1;
        std::memcpy(key.position, position, sizeof key.position);
        std::memcpy(key.rotation, rotation, sizeof key.rotation);
        key.scale = scale;
        key.opacity = opacity;
    };
    const float firstRotation[3] = {.25f, -.5f, .75f};
    const float secondRotation[3] = {-.25f, .5f, -.75f};
    const float firstRootPosition[3] = {11.f, 0.f, 0.f};
    const float firstSparsePosition[3] = {11.f, -7.5f, 0.f};
    const float secondRootPosition[3] = {21.f, 4.5f, 0.f};
    const float secondSparsePosition[3] = {21.f, 4.5f, 3.75f};
    fillKey(firstKeys[0], 0, 0, 1, firstRootPosition, firstRotation, 1.25f, .75f, -1, 1, 1);
    fillKey(firstKeys[1], 1, 0, 0, firstSparsePosition, firstRotation, 1.25f, .5f, -1, 1, 1);
    fillKey(secondKeys[0], 0, 0, 1, secondRootPosition, secondRotation, .5f, .25f, -1, 1, 0);
    fillKey(secondKeys[1], 1, 0, 0, secondSparsePosition, secondRotation, .5f, .25f, -1, 1, 0);
    first->visible = 1; first->shadowEnabled = 1;
    first->parentModel = -1; first->parentBone = 0;
    std::memcpy(first->position, firstSparsePosition, sizeof first->position);
    std::memcpy(first->rotation, firstRotation, sizeof first->rotation);
    first->scale = 1.25f; first->opacity = .5f;
    second->visible = 1; second->shadowEnabled = 0;
    second->parentModel = -1; second->parentBone = 0;
    std::memcpy(second->position, secondSparsePosition, sizeof second->position);
    std::memcpy(second->rotation, secondRotation, sizeof second->rotation);
    second->scale = .5f; second->opacity = .25f;
    app->CurrentFrame() = 12;
    if (mode == 1)
        wcscpy_s(second->sourcePath, L"UserFile\\b.x");
    Check(SaveSceneCopy(app, project.c_str()) && fs::exists(project),
        "the real writer produces the complete V2 project with two real accessories");
    if (mode == 1)
        wcscpy_s(second->sourcePath, sourceB.c_str());
    const auto sourceBytes = Bytes(project);
    Check(sourceBytes.size() > 30 &&
          !std::memcmp(sourceBytes.data(), "Polygon Movie maker 0002", 24),
        "the accessory recovery fixture is a genuine V2 PMM stream");

    // Mode 3 changes only the saved fixture's first X source, after releasing
    // its seed mesh. The original-profile check rejects this texture filename
    // after D3DX has loaded the mesh and the material arrays have been allocated.
    if (mode == 3) {
        std::puts("STAGE real V2 accessory failure after mesh and material acquisition");
        Check(ResetAppState(app), "release the seed accessories before replacing the isolated X source");
        constexpr char badTexture[] = "\x81" "0.bmp";
        std::string encoded;
        Check(!original_file_encoding::Name(badTexture, sizeof badTexture - 1, nullptr, 2047, encoded),
            "the V2 fixture texture filename fails the original CP932 field check");
        std::string mesh = kAccessoryTriangleA;
        mesh.insert(mesh.size() - 3, std::string("TextureFilename {\"") + badTexture + "\";}");
        writeX(sourceA, mesh.c_str());

        Hook(&DisposeAccessory, &ObserveDisposeAccessory, reinterpret_cast<void**>(&realDisposeAccessory));
        app->EnglishUI() = kUiEnglish;
        V2AccessoryDialogs dialogs;
        dialogs.partialFailureSource = &sourceA;
        dialogs.expectedLabel = L"a.x";
        dialogs.replies = {{1, good}};
        v2Accessory = &dialogs;
        Normal(project, camera, light);
        v2Accessory = nullptr;

        auto* recovered = app->AccessorySlot(0);
        auto* following = app->AccessorySlot(1);
        const auto* keys = app->AccessoryKeys(0);
        Check(dialogs.partialRefusals == 1 && dialogs.partialDisposals == 1 &&
              dialogs.prompts == 1 && dialogs.picks == 1 &&
              dialogs.loadResults == std::vector<bool>{false, true, true} &&
              dialogs.loadObjects.size() == 3 &&
              dialogs.loadObjects[0] == recovered && dialogs.loadObjects[1] == recovered &&
              dialogs.loadObjects[2] == following,
            "the V2 retry reuses the cleaned slot and then loads the second accessory");
        Check(recovered && following && ReadAccessoryPayload(*recovered).Allocated() &&
              ReadAccessoryPayload(*following).Allocated() &&
              fs::path(recovered->sourcePath) == good && fs::path(following->sourcePath) == sourceB &&
              keys && keys[0].next == 1 && keys[1].previous == 0 &&
              keys[0].position[0] == 11.f && keys[1].position[1] == -7.5f,
            "the V2 replacement owns its new payload and the real parser keeps the saved keys");
        Check(Bytes(project) == sourceBytes, "the V2 partial-failure recovery preserves the fixture PMM");
        Check(ResetAppState(app), "reset the host after the V2 partial-failure recovery");
        app->EnglishUI() = savedLanguage;
        Check(SetCurrentDirectoryW(savedCwd) != FALSE, "restore the real accessory loader's process directory");
        return;
    }

    if (mode == 4) {
        std::puts("STAGE real V2 accessory material-buffer exception");
        app->EnglishUI() = kUiEnglish;
        const auto tails = tailCalls, errors = messages;
        bool releasedByLoader = false;
        {
            V2MaterialBufferProbe probe(sourceA);
            Arm(project, Fault::Memory, false);
            const bool loaded = LoadSceneFile(project.c_str());
            Finish(loaded, false);
            Check(faultThrown && !tailObserved && tailCalls == tails && messages == errors + 1 &&
                  !app->sceneRecoveryPending && app->CurrentFrame() == 0,
                "the V2 shell rejects the injected exception and resets the replaced scene");
            releasedByLoader = probe.releases == 1 && !probe.buffer;
        }
        Check(Bytes(project) == sourceBytes, "the rejected V2 open preserves the source PMM");
        Normal(project, camera, light);
        const auto* reopened = app->AccessorySlot(0);
        const auto* following = app->AccessorySlot(1);
        Check(reopened && following && ReadAccessoryPayload(*reopened).Allocated() &&
              ReadAccessoryPayload(*following).Allocated(),
            "the same V2 project reopens with both real accessory payloads after removing the fault");
        Check(ResetAppState(app), "reset the host after the healthy follow-up open");
        app->EnglishUI() = savedLanguage;
        Check(SetCurrentDirectoryW(savedCwd) != FALSE, "restore the real accessory loader's process directory");
        std::printf("V2_MATERIAL_BUFFER_EXCEPTION released_by_loader=%u healthy_reopen=1\n",
            unsigned(releasedByLoader));
        Check(releasedByLoader, "the V2 loader releases the acquired D3DX material buffer on exception");
        return;
    }

    // A healthy V2 control project proves the host keeps loading and saving
    // after an explicit accessory abort.
    Check(ResetAppState(app), "reset the host before writing the healthy V2 control");
    app->CurrentFrame() = 12;
    app->CameraKeys()[0] = camera;
    app->LightKeys()[0] = light;
    app->CameraFov() = 43.f;
    for (unsigned i = 0; i < 3; ++i) {
        app->LightColor()[i] = light.color[i];
        app->LightDirection()[i] = light.direction[i];
    }
    Check(SaveSceneCopy(app, healthy.c_str()) && fs::exists(healthy),
        "the real writer produces the healthy V2 control project");

    if (mode == 5 || mode == 6) {
        const auto movedSecond = root / L"moved-b.x";
        const auto blockedSave = root / L"blocked.pmm";
        const auto recoveredSave = root / L"recovered.pmm";
        const auto v1Project = root / L"v1.pmm";
        const auto firstBytes = Bytes(sourceA), secondBytes = Bytes(sourceB);
        if (mode == 6) WriteV1AccessoryMaterialRetry(v1Project, sourceA, sourceB);
        Check(MoveFileW(sourceB.c_str(), movedSecond.c_str()) && !fs::exists(sourceB),
            "make only the second saved X source missing inside the fixture");

        const auto cancel = [&](const fs::path& rejected, const wchar_t* label, V2AbortReset action,
                                RecoveryGateReset gate, bool explicitClose) {
            const auto rejectedBytes = Bytes(rejected);
            const auto language = explicitClose ? kUiEnglish : kUiChinese;
            app->EnglishUI() = language;
            V2AccessoryDialogs dialogs;
            if (mode == 6) dialogs.replies.push_back({1, {}}); // cancel the file picker first
            dialogs.replies.push_back({explicitClose ? INT_PTR{2} : INT_PTR{4}, {}});
            dialogs.expectedLabel = label;
            dialogs.abortLanguage = language;
            dialogs.firstAbortReset = dialogs.retryAbortReset = action;
            dialogs.observeFinalAbortNotice = mode == 6;
            const auto previousPostloadPath = postloadReopenPath;
            postloadReopenPath = healthy;
            ResetPostloadObservation(language);
            v2Accessory = &dialogs;
            const auto tails = tailCalls, errors = messages, finalNotices = postloadNotices;
            Arm(rejected, Fault::None, false);
            const bool loaded = LoadSceneFile(rejected.c_str());
            monitor = false;
            v2Accessory = nullptr;
            postloadReopenPath = previousPostloadPath;
            const auto* retained = app->AccessorySlot(0);
            Check(!loaded && !tailObserved && tailCalls == tails && messages == errors &&
                  postloadNotices == finalNotices + 1 && dialogs.prompts == dialogs.replies.size() &&
                  dialogs.picks == unsigned(mode == 6) &&
                  dialogs.normalAbortResets == 2 && dialogs.firstAbortResetFailures == 1 &&
                  dialogs.loadResults == std::vector<bool>{true} && dialogs.loadObjects.front() == retained &&
                  retained && retained->mesh && app->sceneRecoveryPending &&
                  app->CurrentFrame() == kQuarantineResidualFrame && app->EnglishUI() == language,
                "two failed resets leave the earlier attachment quarantined without a spurious invalid-PMM error");
            if (dialogs.observeFinalAbortNotice)
                Check(postloadNativeObserved && postloadNoticeTextOk && postloadNoticeFlagsOk &&
                      postloadNoticePending && postloadNoticeInputClosed && postloadNoticeMutationGuard &&
                      postloadNestedRuns == 1 && postloadNestedBlocked && postloadNestedSceneIntact,
                    "the native final notice uses the caller language and blocks reentrant open with no scene change");
            Check(inputCloses == 1 && !inputOpen && checkedAtClose == checkedBefore &&
                  pmm_io::checkedDescriptor == checkedBefore && !app->SceneMutationInProgress() &&
                  !app->EnvFileName()[0] && !app->PathWorkspace().projectDirectory[0],
                "the failed open releases its input and does not adopt the rejected project identity");
            Released(rejected);
            Check(!fs::exists(blockedSave) && !SaveSceneCopy(app, blockedSave.c_str()) && !fs::exists(blockedSave),
                "the production writer refuses the partially loaded scene without creating its target");

            const auto reads = inputReads;
            const auto pendingNotices = recoveryGateNotices;
            const auto previousGatePath = recoveryGateReopenPath;
            recoveryGateReopenPath = healthy;
            candidate = healthy;
            ArmRecoveryGate(gate, language);
            monitor = true;
            const bool reopened = LoadSceneFile(healthy.c_str());
            monitor = false;
            Check(!reopened && recoveryGateResets == 1 && recoveryGateNotices == pendingNotices + 1 &&
                  recoveryGateNoticeTextOk && recoveryGateNoticeFlagsOk && recoveryGateNestedBlocked &&
                  inputReads == reads && inputCloses == 1 && tailCalls == tails && app->sceneRecoveryPending &&
                  app->AccessorySlot(0) == retained && app->CurrentFrame() == kQuarantineResidualFrame &&
                  app->EnglishUI() == language && !app->EnvFileName()[0] && !app->PathWorkspace().projectDirectory[0],
                "the failing recovery gate blocks a valid project and reentry without touching the half scene");
            DisarmRecoveryGate();
            recoveryGateReopenPath = previousGatePath;

            Normal(healthy, camera, light);
            Check(!app->sceneRecoveryPending && !app->AccessorySlot(0) && !app->AccessorySlot(1) &&
                  Bytes(rejected) == rejectedBytes,
                "the next genuine reset releases the partial attachment and opens the healthy scene without modifying the rejected source");
        };
        if (mode == 5) {
            cancel(project, L"b.x", V2AbortReset::Refuse, RecoveryGateReset::False, false);
        } else {
            const bool bufferedBefore = pmm_io::bufferedReads;
            for (const auto& rejected : {v1Project, project}) {
                pmm_io::bufferedReads = false;
                const auto label = rejected == v1Project ? L"following" : L"b.x";
                cancel(rejected, label, V2AbortReset::ThrowUnknown, RecoveryGateReset::Unknown, false);
                pmm_io::bufferedReads = true;
                cancel(rejected, label, V2AbortReset::ThrowStandard, RecoveryGateReset::Runtime, true);
                std::printf("PASS accessory cancel throwing resets: %s direct/unknown and buffered/std exception\n",
                            rejected == v1Project ? "V1" : "V2");
            }
            pmm_io::bufferedReads = bufferedBefore;
        }
        Check(MoveFileW(movedSecond.c_str(), sourceB.c_str()), "restore the isolated second X source");
        if (mode == 6) {
            const auto v1Bytes = Bytes(v1Project);
            Arm(v1Project, Fault::None, false);
            Finish(LoadSceneFile(v1Project.c_str()), true);
            Check(!app->sceneRecoveryPending && app->AccessorySlot(0) && app->AccessorySlot(0)->mesh &&
                  app->AccessorySlot(1) && app->AccessorySlot(1)->mesh && Bytes(v1Project) == v1Bytes,
                "repairing the source also reopens the previously rejected V1 project with both real meshes");
        }
        Normal(project, camera, light);
        Check(!fs::exists(recoveredSave) && SaveSceneCopy(app, recoveredSave.c_str()),
            "the original project opens after repairing its source and can be saved to a new path");
        Normal(recoveredSave, camera, light);
        Check(app->AccessorySlot(0) && app->AccessorySlot(0)->mesh &&
              app->AccessorySlot(1) && app->AccessorySlot(1)->mesh &&
              Bytes(project) == sourceBytes && Bytes(sourceA) == firstBytes && Bytes(sourceB) == secondBytes,
            "both real accessories reopen and every source remains byte-identical");
        Check(ResetAppState(app), "leave the fixture host empty after the recovered reopen");
        app->EnglishUI() = savedLanguage;
        Check(SetCurrentDirectoryW(savedCwd) != FALSE, "restore the fixture working directory");
        std::puts("PASS V2 late accessory cancel: quarantine, blocked save and open, recovery and roundtrip");
        return;
    }

    if (mode == 2) {
        // Locate the second writer record by its name and full source path.
        // The separate display-name table has no path following its entry.
        const auto pathBytes = sourceB.string();
        auto corrupt = sourceBytes;
        std::size_t slotOffset = 0;
        for (std::size_t i = 30; i + 101 + pathBytes.size() < corrupt.size(); ++i) {
            if (corrupt[i] != 1 || std::memcmp(corrupt.data() + i + 1, "b.x", 4) ||
                std::memcmp(corrupt.data() + i + 101, pathBytes.c_str(), pathBytes.size() + 1))
                continue;
            Check(!slotOffset, "the second accessory record is uniquely identified");
            slotOffset = i;
        }
        Check(slotOffset != 0, "find the second accessory slot in the real PMM output");
        corrupt[slotOffset] = 0;
        const auto userFiles = root / L"UserFile";
        Check(fs::create_directory(userFiles), "own a UserFile directory to exercise the rejected project base");
        const auto duplicate = userFiles / L"duplicate.pmm";
        Check(duplicate.native().size() < 256, "the duplicate PMM path fits the native field");
        std::ofstream output(duplicate, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(corrupt.data()), static_cast<std::streamsize>(corrupt.size()));
        output.close();
        Check(bool(output), "write the isolated PMM with a duplicate accessory slot");
        Hook(&DisposeAccessory, &ObserveDisposeAccessory, reinterpret_cast<void**>(&realDisposeAccessory));
        for (const bool legacy : {false, true}) {
            app->EnglishUI() = kUiChinese;
            HandleWindowSize(app);
            Check(app->WindowLayoutReady(), "the layout is ready before replacing the scene");
            if (legacy)
                Check(plugins::PrepareProjectLoad(app, duplicate.c_str()),
                      "the legacy caller prepares its own PMM transaction");
            V2AccessoryDialogs observer;
            observer.expectNormalAbort = true;
            observer.duplicateSlot = true;
            v2Accessory = &observer;
            Arm(duplicate, Fault::None, false);
            bool loaded = false;
            if (legacy) {
                int fd = -1;
                Check(_wsopen_s(&fd, duplicate.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IWRITE) == 0,
                      "open the duplicate PMM at the legacy V2 parser entry");
                char header[30]{};
                Check(_read(fd, header, sizeof header) == sizeof header &&
                      !std::memcmp(header + 20, "0002", 4), "consume the real V2 PMM header before calling the legacy parser");
                wcscpy_s(app->EnvFileName(), 256, duplicate.c_str());
                LoadSceneV2(app, fd);
                plugins::CancelProjectLoad(app);
            } else {
                loaded = LoadSceneFile(duplicate.c_str());
            }
            std::printf("DUPLICATE_ACCESSORY legacy=%d loaded=%d nativeLoads=%zu disposed=%u notices=%u resets=%u\n",
                        int(legacy), int(loaded), observer.loadResults.size(), observer.disposedLoadedRecords,
                        observer.invalidNotices, observer.normalAbortResets);
            FinishV2AccessoryAbort(loaded, duplicate, corrupt);
            Check(app->WindowLayoutReady() && !app->PathWorkspace().projectDirectory[0],
                  "failed replacement restores the layout gate and clears its UserFile base");
            Check(observer.loadResults == std::vector<bool>{true} && observer.prompts == 0 &&
                  observer.invalidNotices == 1 && observer.disposedLoadedRecords == 1 &&
                  observer.normalAbortResets == 1,
                  "duplicate slot rejection closes once, disposes the already-loaded X once and never loads the colliding X");
            v2Accessory = nullptr;
            Normal(project, camera, light);
            Check(app->AccessorySlot(0) && app->AccessorySlot(0)->mesh &&
                  app->AccessorySlot(1) && app->AccessorySlot(1)->mesh &&
                  Bytes(project) == sourceBytes,
                  "the same host reopens the untouched valid project with both real accessories");
        }
        Check(ResetAppState(app), "leave an editable scene after duplicate-slot recovery");
        app->EnglishUI() = savedLanguage;
        Check(SetCurrentDirectoryW(savedCwd) != FALSE, "restore the real accessory loader's process directory");
        std::puts("PASS real V2 duplicate accessory slot rejection and recovery");
        return;
    }

    // Only the first source disappears: the second accessory stays loadable
    // without any prompt, so the recovery loop and the ordinary path coexist
    // in one project.
    Check(!fs::exists(movedSource),
        "the moved first fixture X has a previously absent destination");
    Check(MoveFileW(sourceA.c_str(), movedSource.c_str()) != FALSE &&
          !fs::exists(sourceA) && fs::exists(movedSource) && fs::exists(sourceB) &&
          !fs::exists(absent),
        "move only the first fixture X so its PMM source path is genuinely missing while the second stays and the missing-selection path stays absent");

    const auto samePath = [](const wchar_t* actual, const fs::path& expected) {
        return actual != nullptr &&
            fs::path(actual).lexically_normal() == expected.lexically_normal();
    };
    PathResolutionWorkspace legacyPaths = app->PathWorkspace();
    wcscpy_s(legacyPaths.projectDirectory, (root.native() + L"\\").c_str());
    Check(samePath(ResolveUserFilePath(legacyPaths, good.c_str()), decoy),
        "the legacy lookup can really open a different X with the same UserFile suffix");
    const auto checkExactReplacement = [&](const mdl::AccessoryRecord* object) {
        Check(object && object->mesh && object->materialCount == 1 && object->materials &&
              samePath(object->sourcePath, good) &&
              std::fabs(static_cast<const D3DMATERIAL9*>(object->materials)->Ambient.r - .2f) < .001f,
            "the loaded accessory uses the chosen absolute X path and material instead of the UserFile decoy");
    };
    const auto checkRecovered = [&](const fs::path& expectedSource) {
        auto* a0 = app->AccessorySlot(0);
        auto* a1 = app->AccessorySlot(1);
        if (expectedSource == good) checkExactReplacement(a0);
        Check(a0 && a1 && a0->mesh && a1->mesh &&
              a0->materialCount == 1 && a1->materialCount == 1,
            "the recovered project keeps both real accessory objects and their material buffers");
        Check(samePath(a0->sourcePath, expectedSource) &&
              samePath(a1->sourcePath, sourceB),
            "the recovered first accessory owns the selected replacement path and the second keeps its original path");
        auto* k0 = app->AccessoryKeys(0);
        auto* k1 = app->AccessoryKeys(1);
        Check(k0 && k1 && k0[0].frame == 0 && k0[1].frame == 1 &&
              k1[0].frame == 0 && k1[1].frame == 1 &&
              k0[0].next == 1 && k0[1].previous == 0 &&
              k1[0].next == 1 && k1[1].previous == 0,
            "both recovered accessories keep their allocated root plus a legal index-1 sparse keyframe chain");
        Check(k0[0].visible == 1 && k0[0].shadowEnabled == 1 &&
              k0[0].position[0] == 11.f && k0[0].position[1] == 0.f &&
              k0[0].rotation[0] == .25f && k0[0].rotation[1] == -.5f &&
              k0[0].rotation[2] == .75f && k0[0].scale == 1.25f &&
              k0[0].opacity == .75f &&
              k0[1].position[0] == 11.f && k0[1].position[1] == -7.5f &&
              k0[1].opacity == .5f,
            "the first accessory's root and index-1 keyframes preserve their exact values");
        Check(k1[0].position[0] == 21.f && k1[0].position[1] == 4.5f &&
              k1[0].position[2] == 0.f && k1[0].scale == .5f &&
              k1[0].opacity == .25f &&
              k1[1].position[2] == 3.75f,
            "the second accessory's root and index-1 keyframes preserve their exact values");
    };
    const auto checkSkippedFirst = [&]() {
        auto* a0 = app->AccessorySlot(0);
        auto* a1 = app->AccessorySlot(1);
        Check(a0 && a1 && a0->mesh == nullptr && a0->materials == nullptr &&
              a0->texturePaths == nullptr && a0->textureTypes == nullptr &&
              a0->sourcePath[0] == L'\0' &&
              a1->mesh && a1->materialCount == 1,
            "explicitly skipping the missing first accessory leaves its real empty record with no mesh or failed-candidate residue while the second accessory still loads");
        auto* k0 = app->AccessoryKeys(0);
        auto* k1 = app->AccessoryKeys(1);
        Check(k0 && k1 && k0[0].frame == 0 && k0[1].frame == 1 &&
              k1[0].frame == 0 && k1[1].frame == 1 &&
              k0[0].position[0] == 11.f && k0[1].position[1] == -7.5f &&
              k1[0].position[0] == 21.f && k1[1].position[2] == 3.75f,
            "skipping still consumes the original record and reads every stored accessory keyframe");
    };

    for (const unsigned language : {0u, 1u, 2u}) {
        app->EnglishUI() = language;
        const std::wstring languageTag = std::to_wstring(language);
        std::printf("STAGE real V2 accessory recovery language=%u\n", language);

        // 1) Retry real malformed and disappeared X candidates before accepting the chosen X.
        {
            Check(ResetAppState(app), "reset the replaced scene before the substitute recovery scenario");
            app->EnglishUI() = language;
            V2AccessoryDialogs dialogs;
            dialogs.expectedLabel = L"a.x";
            dialogs.disappearingCandidate = disappearing;
            dialogs.replies = {{1, bad}, {1, disappearing}, {1, good}};
            const auto projectRoot = root.native() + L"\\";
            const auto previousDirectory = projectFolder.native();
            dialogs.projectRoot = projectRoot.c_str();
            dialogs.originalAccessoryDirectory = previousDirectory.c_str();
            constexpr UINT rememberFolder = 0x12D;
            const auto menu = GetMenu(app->MainWindow());
            const auto previousCheck = CheckMenuItem(menu, rememberFolder, MF_BYCOMMAND | MF_CHECKED);
            Check(previousCheck != UINT(-1), "enable the real remember-folder option");
            CopyDirPathW(app->DirAccs(), previousDirectory.c_str());
            v2Accessory = &dialogs;
            const auto messagesBefore = messages;
            const auto missingBefore = missingDialogs;
            Normal(project, camera, light);
            v2Accessory = nullptr;
            CheckMenuItem(GetMenu(app->MainWindow()), rememberFolder, MF_BYCOMMAND | previousCheck);
            Check(!std::wcscmp(app->PathWorkspace().projectDirectory, projectRoot.c_str()) &&
                  !std::wcscmp(app->DirAccs(), good.parent_path().c_str()),
                "a loaded replacement updates only the accessory picker folder and preserves the PMM resource root");
            Check(dialogs.prompts == 3 && dialogs.picks == 3 &&
                  dialogs.loadPaths.size() == 4 && dialogs.loadResults.size() == 4 &&
                  !dialogs.loadResults[0] && !dialogs.loadResults[1] &&
                  dialogs.loadResults[2] && dialogs.loadResults[3] &&
                  samePath(dialogs.loadPaths[0].c_str(), bad) &&
                  samePath(dialogs.loadPaths[1].c_str(), disappearing) &&
                  samePath(dialogs.loadPaths[2].c_str(), good) &&
                  samePath(dialogs.loadPaths[3].c_str(), sourceB) &&
                  dialogs.loadObjects.size() == 4 &&
                  dialogs.loadObjects[0] == static_cast<void*>(app->AccessorySlot(0)) &&
                  dialogs.loadObjects[1] == static_cast<void*>(app->AccessorySlot(0)) &&
                  dialogs.loadObjects[2] == static_cast<void*>(app->AccessorySlot(0)) &&
                  dialogs.loadObjects[3] == static_cast<void*>(app->AccessorySlot(1)),
                "malformed and disappeared replacements each clear the production slot and re-prompt before the good X loads");
            Check(messages == messagesBefore && missingDialogs == missingBefore &&
                  inputCloses == 1 && !inputOpen,
                "the successful recovery shows no legacy native or project error box and closes its PMM input exactly once");
            checkRecovered(good);
            std::fprintf(stderr,
                "DIAG v2 accessory substitute lang=%u prompts=%zu picks=%zu loads=%zu\n",
                language, dialogs.prompts, dialogs.picks, dialogs.loadPaths.size());
            const auto substituteTarget = projectFolder / (L"sub-" + languageTag + L".pmm");
            Check(!fs::exists(substituteTarget) && substituteTarget.native().size() < 256,
                "the per-language substitute save target is new, fits the path ABI and is never overwritten");
            Check(SaveSceneCopy(app, substituteTarget.c_str()) && fs::exists(substituteTarget),
                "the real V2 writer saves the recovered project with the substitute path");
            Check(Bytes(project) == sourceBytes,
                "the recovered project's source PMM stays byte-identical");
            dialogs.replies.clear();
            dialogs.loadPaths.clear();
            dialogs.loadResults.clear();
            dialogs.loadObjects.clear();
            dialogs.next = 0;
            dialogs.prompts = 0;
            dialogs.picks = 0;
            v2Accessory = &dialogs;
            Normal(substituteTarget, camera, light);
            v2Accessory = nullptr;
            Check(dialogs.prompts == 0 && dialogs.picks == 0 &&
                  dialogs.loadPaths.size() == 2 && dialogs.loadResults.size() == 2 &&
                  dialogs.loadResults[0] && dialogs.loadResults[1] &&
                  samePath(dialogs.loadPaths[0].c_str(), good) &&
                  samePath(dialogs.loadPaths[1].c_str(), sourceB),
                "the real reopen of the saved project loads both accessories through the substitute path without any recovery prompt");
            checkRecovered(good);
        }

        // 2) The picker can be cancelled, and a disappeared replacement can be skipped.
        {
            Check(ResetAppState(app), "reset the replaced scene before the picker-cancel scenario");
            app->EnglishUI() = language;
            V2AccessoryDialogs dialogs;
            dialogs.expectedLabel = L"a.x";
            dialogs.disappearingCandidate = disappearing;
            dialogs.replies = {{1, {}}, {1, disappearing}, {3, {}}};
            v2Accessory = &dialogs;
            const auto messagesBefore = messages;
            Normal(project, camera, light);
            v2Accessory = nullptr;
            Check(dialogs.prompts == 3 && dialogs.picks == 2 &&
                  dialogs.loadPaths == std::vector<std::wstring>{disappearing.native(), sourceB.native()} &&
                  dialogs.loadResults == std::vector<bool>{false, true},
                "a cancelled picker and a disappeared replacement return to recovery, then skip loads only the second accessory");
            Check(messages == messagesBefore,
                "the cancelled picker and explicit skip show no legacy native or project error box");
            checkSkippedFirst();
            std::fprintf(stderr,
                "DIAG v2 accessory cancel-skip lang=%u prompts=%zu picks=%zu loads=%zu\n",
                language, dialogs.prompts, dialogs.picks, dialogs.loadPaths.size());
        }

        // 3) Direct skip: one prompt, no picker at all.
        {
            Check(ResetAppState(app), "reset the replaced scene before the direct-skip scenario");
            app->EnglishUI() = language;
            V2AccessoryDialogs dialogs;
            dialogs.expectedLabel = L"a.x";
            dialogs.replies = {{3, fs::path{}}};
            v2Accessory = &dialogs;
            const auto messagesBefore = messages;
            Normal(project, camera, light);
            v2Accessory = nullptr;
            Check(dialogs.prompts == 1 && dialogs.picks == 0 &&
                  dialogs.loadPaths.size() == 1 &&
                  samePath(dialogs.loadPaths[0].c_str(), sourceB),
                "an explicit skip at the first prompt runs no picker and still loads the second accessory");
            Check(messages == messagesBefore,
                "the direct skip shows no legacy native or project error box");
            checkSkippedFirst();
            std::fprintf(stderr,
                "DIAG v2 accessory skip lang=%u prompts=%zu picks=%zu loads=%zu\n",
                language, dialogs.prompts, dialogs.picks, dialogs.loadPaths.size());
        }

        // 4) Missing and directory selections are known unusable: production
        // re-prompts without calling the low-level object loader. The good file
        // is then accepted.
        {
            Check(ResetAppState(app), "reset the replaced scene before the unusable-selection scenario");
            app->EnglishUI() = language;
            V2AccessoryDialogs dialogs;
            dialogs.expectedLabel = L"a.x";
            dialogs.replies = {{1, absent}, {1, directoryCandidate}, {1, good}};
            v2Accessory = &dialogs;
            const auto messagesBefore = messages;
            Normal(project, camera, light);
            v2Accessory = nullptr;
            Check(dialogs.prompts == 3 && dialogs.picks == 3 &&
                  dialogs.loadPaths.size() == 2 && dialogs.loadResults.size() == 2 &&
                  dialogs.loadResults[0] && dialogs.loadResults[1] &&
                  samePath(dialogs.loadPaths[0].c_str(), good) &&
                  samePath(dialogs.loadPaths[1].c_str(), sourceB),
                "a missing path or directory selection re-prompts without calling the low-level loader, then the real X loads");
            Check(messages == messagesBefore,
                "the known unusable selections show no low-level x-file box");
            checkRecovered(good);
            std::fprintf(stderr,
                "DIAG v2 accessory unusable-selection lang=%u prompts=%zu picks=%zu loads=%zu\n",
                language, dialogs.prompts, dialogs.picks, dialogs.loadPaths.size());
        }

        // 5) Aborting after a disappeared replacement and closing the first prompt both run the normal reset.
        for (const INT_PTR abortAnswer : {static_cast<INT_PTR>(4), static_cast<INT_PTR>(2)}) {
            Check(ResetAppState(app), "reset the replaced scene before the direct-abort scenario");
            app->EnglishUI() = language;
            V2AccessoryDialogs dialogs;
            dialogs.expectedLabel = L"a.x";
            const bool triedReplacement = abortAnswer == 4;
            if (triedReplacement) {
                dialogs.disappearingCandidate = disappearing;
                dialogs.replies.push_back({1, disappearing});
            }
            dialogs.replies.push_back({abortAnswer, {}});
            v2Accessory = &dialogs;
            const auto messagesBefore = messages;
            const auto missingBefore = missingDialogs;
            const auto tailsBefore = tailCalls;
            Arm(project, Fault::None, false);
            const bool loaded = LoadSceneFile(project.c_str());
            FinishV2AccessoryAbort(loaded, project, sourceBytes);
            v2Accessory = nullptr;
            Check(dialogs.prompts == 1 + unsigned(triedReplacement) &&
                  dialogs.picks == unsigned(triedReplacement) &&
                  dialogs.loadPaths == (triedReplacement ? std::vector<std::wstring>{disappearing.native()}
                                                        : std::vector<std::wstring>{}) &&
                  dialogs.loadResults == (triedReplacement ? std::vector<bool>{false} : std::vector<bool>{}) &&
                  messages == messagesBefore &&
                  missingDialogs == missingBefore && tailCalls == tailsBefore,
                "the explicit cancel/close aborts the whole project without a low-level box or success tail");
            Check(dialogs.expectNormalAbort && dialogs.normalAbort &&
                  dialogs.normalAbortResets == 1,
                "the explicit cancel/close answer runs the production ResetAppState exactly once on the normal abort path");
            Check(dialogs.localizeCalls == 1,
                "the shell runs its real LocalizeUI exactly once after the normal accessory abort reset");
#if MMD_ORIGINAL_PROFILE
            const unsigned abortRefreshes =
                dialogs.abortResetRefreshes + dialogs.abortLocalizeRefreshes;
            const unsigned classifiedRefreshes =
                preCloseRefreshCalls + recoveryRefreshCalls + abortRefreshes;
            Check(viewRefreshCalls >= 1 && classifiedRefreshes == viewRefreshCalls,
                "the original profile classifies every normal accessory abort ViewRefresh callback into the exact real ResetAppState/LocalizeUI scope or the unchanged pre-close path");
#endif
            std::fprintf(stderr,
                "DIAG v2 accessory abort lang=%u answer=%lld resets=%u localizeCalls=%u resetRefresh=%u localizeRefresh=%u preClose=%u recovery=%u viewRefresh=%u\n",
                language, static_cast<long long>(abortAnswer),
                dialogs.normalAbortResets, dialogs.localizeCalls,
                dialogs.abortResetRefreshes, dialogs.abortLocalizeRefreshes,
                preCloseRefreshCalls, recoveryRefreshCalls, viewRefreshCalls);
            Normal(healthy, camera, light);
            const auto healthyTarget = root / (L"hs-" + languageTag +
                L"-" + std::to_wstring(abortAnswer) + L".pmm");
            Check(!fs::exists(healthyTarget) && healthyTarget.native().size() < 256,
                "each language/abort healthy save target is new, fits the path ABI and is never overwritten");
            Check(SaveSceneCopy(app, healthyTarget.c_str()) && fs::exists(healthyTarget),
                "the host can still save a valid project after the accessory abort");
            Normal(healthyTarget, camera, light);
            Check(Bytes(project) == sourceBytes,
                "the aborted and subsequent healthy saves leave the fixture source PMM byte-identical");
        }
    }
    std::puts("PASS V2 disappeared replacement: retry, save/reopen, skip and cancel");
    for (const auto action : {V2AbortReset::ThrowUnknown, V2AbortReset::ThrowStandard,
                              V2AbortReset::Refuse}) {
        Check(ResetAppState(app), "reset before exercising an accessory-abort reset failure");
        app->EnglishUI() = kUiChinese;
        V2AccessoryDialogs dialogs;
        dialogs.expectedLabel = L"a.x";
        dialogs.replies = {{4, fs::path{}}};
        dialogs.firstAbortReset = action;
        dialogs.abortLanguage = kUiChinese;
        v2Accessory = &dialogs;
        const auto tailsBefore = tailCalls;
        const auto messagesBefore = messages;
        Arm(project, Fault::None, false);
        bool loaded = false;
        bool escaped = false;
        try {
            loaded = LoadSceneFile(project.c_str());
        } catch (...) {
            escaped = true;
        }
        Check(!escaped, "cancelling V2 accessory recovery must contain exceptions from the reset");
        FinishV2AccessoryAbort(loaded, project, sourceBytes);
        v2Accessory = nullptr;
        Check(dialogs.prompts == 1 && dialogs.picks == 0 && dialogs.firstAbortResetFailures == 1 &&
              dialogs.normalAbortResets == 2 && app->EnglishUI() == kUiChinese &&
              tailCalls == tailsBefore && messages == messagesBefore,
              "the first failure retries once without a format error or an unintended success tail");
        Normal(healthy, camera, light);
        const auto recoveredTarget = root / (L"reset-" + std::to_wstring(static_cast<int>(action)) + L".pmm");
        Check(!fs::exists(recoveredTarget) && SaveSceneCopy(app, recoveredTarget.c_str()),
              "after the abort reset retry, a healthy project can be saved to a new path");
        Normal(recoveredTarget, camera, light);
        Check(Bytes(project) == sourceBytes, "the subsequent save and reopen preserve the rejected PMM");
    }
    const auto storedV1 = projectFolder / L"exact-v1.pmm";
    Check(!fs::exists(storedV1), "the V1 exact-path fixture has a fresh name");
    WriteV1AccessoryMaterialRetry(storedV1, good);
    Normal(storedV1, camera, light);
    checkExactReplacement(app->AccessorySlot(0));
    LoadAccessoryFile(good.c_str());
    checkExactReplacement(app->AccessorySlot(1));
    Check(ResetAppState(app), "leave the isolated host editable after the accessory recovery cases");
    app->EnglishUI() = savedLanguage;
    Check(Bytes(project) == sourceBytes,
        "every accessory recovery load and abort preserved the fixture source PMM bytes");
    Check(SetCurrentDirectoryW(savedCwd) != FALSE,
        "restore the process directory the real accessory loader changed");
    // Keep this run's unique fixture tree and every per-language/per-abort save
    // for independent acceptance. Nothing here is removed or overwritten.
    std::puts("PASS real V2 missing-accessory recovery: substitute, cancel, skip, abort and unusable selections");
}

// ---- Real File > New (WM_COMMAND 204) failure-recovery boundary -------------
void PrepareNewSceneCommand(NewSceneReset action, int reply) {
    newSceneReset = action;
    newSceneConfirmReply = reply;
    newSceneResetCalls = newSceneConfirmDialogs = newSceneRefusalDialogs = 0;
    newSceneEscapes = 0;
    newSceneConfirmAction = nullptr;
    newSceneRefusalAction = nullptr;
    app->EnglishUI() = 2;  // production must preserve this exact caller value
}

LRESULT InvokeNewScene(const char* mode, const char* stage) {
    // Dispatch through the real window procedure entry exactly like a menu
    // command, never around it into CmdResetState or Reset. The direct call also
    // keeps a thrown exception visible: Win32 SendMessage may swallow it.
    bool escaped = false;
    LRESULT result = 0;
    try { result = MainWndProc(app->MainWindow(), WM_COMMAND, MAKEWPARAM(204, 0), 0); }
    catch (...) { escaped = true; }
    if (escaped) ++newSceneEscapes;
    std::fprintf(stderr,
        "DIAG PMM new-scene mode=%s stage=%s escaped=%u resetCalls=%u confirms=%u refusals=%u "
        "language=%d pending=%d frame=%d result=%lld\n",
        mode, stage, unsigned(escaped), newSceneResetCalls, newSceneConfirmDialogs,
        newSceneRefusalDialogs, app->EnglishUI(), int(app->sceneRecoveryPending),
        int(app->CurrentFrame()), static_cast<long long>(result));
    Check(!escaped,
        "the new-scene recovery exception never escapes the real MainWndProc entry");
    Check(app->EnglishUI() == 2,
        "the real new-scene command restores the caller language captured before the command");
    return result;
}

void CheckNewSceneQuarantine(const char* mode, const fs::path& source,
                             const std::vector<unsigned char>& sourceBytes,
                             std::uint64_t generation,
                             unsigned expectedTails, unsigned expectedMessages) {
    const bool stable = app->sceneRecoveryPending &&
        app->CurrentFrame() == kQuarantineResidualFrame &&
        app->SceneGeneration() == generation && !inputOpen && inputCloses == 1 &&
        checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
        tailCalls == expectedTails && messages == expectedMessages &&
        Bytes(source) == sourceBytes && !SceneSaveInProgress() && !SceneSaveBlocksEdits();
    if (!stable) {
        std::fprintf(stderr,
            "DIAG PMM new-scene quarantine mode=%s pending=%d frame=%d generation=%llu expected=%llu "
            "inputOpen=%u closes=%u checkedAtClose=%d checkedBefore=%d checkedNow=%d "
            "tails=%u expectedTails=%u messages=%u expectedMessages=%u\n",
            mode, int(app->sceneRecoveryPending), int(app->CurrentFrame()),
            static_cast<unsigned long long>(app->SceneGeneration()),
            static_cast<unsigned long long>(generation), unsigned(inputOpen), inputCloses,
            checkedAtClose, checkedBefore, pmm_io::checkedDescriptor,
            tailCalls, expectedTails, messages, expectedMessages);
    }
    Check(stable,
        "a failed new-scene command keeps the quarantined scene, playhead, closed input, restored TLS context, source bytes and save gate");
#if !MMD_ORIGINAL_PROFILE
    auto* history = plugins::NativeHistory(app);
    Check(history && history->CheckNativeEdit() == MMS_E_BUSY,
        "a failed new-scene command keeps the real shared journal gate closed on the quarantined scene");
#endif
}

void EnterNewSceneQuarantine(const fs::path& source, const char* mode) {
    newSceneMode = false;
    Arm(source, Fault::ReadResetQuarantine, false);
    const bool loaded = LoadSceneFile(source.c_str());
    Finish(loaded, false, true, false);
    std::fprintf(stderr,
        "DIAG PMM new-scene setup mode=%s loaded=%u refusals=%u pending=%d frame=%d closes=%u checked=%d/%d\n",
        mode, unsigned(loaded), quarantineRefusals, int(app->sceneRecoveryPending),
        int(app->CurrentFrame()), inputCloses, checkedAtClose, pmm_io::checkedDescriptor);
    Check(!loaded && quarantineRefusals == 2 && app->sceneRecoveryPending &&
        app->CurrentFrame() == kQuarantineResidualFrame && !inputOpen && inputCloses == 1 &&
        pmm_io::checkedDescriptor == checkedBefore,
        "the File > New regression starts from the actual V1 short-read scene that stayed quarantined");
}
#include "pmm_read_buffer_host_checks.inc"
#include "pmm_app_lifecycle_checks.inc"
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if ((argc == 5 || argc == 6) && std::wcscmp(argv[1], L"--write-v1-relocation-fixture") == 0) {
        const fs::path project(argv[2]), first(argv[3]), second(argv[4]);
        Check(!fs::exists(project) && !fs::exists(first) && !fs::exists(second),
              "write only new files in the isolated V1 effect recovery fixture");
        for (const auto& modelPath : {first, second}) {
            auto model = geometry_test_reference::Pmd(3, true);
            const auto name = modelPath.stem().string();
            Check(name.size() < 20, "the fixture model name fits the PMD header");
            std::memset(model.bytes.data() + 7, 0, 20);
            std::memcpy(model.bytes.data() + 7, name.data(), name.size());
            // Place the three vertices in view after the fixture's V1 root keys translate them.
            const float left = modelPath == first ? -17.f : -10.f;
            const float vertices[3][3] = {{left, 4.f, 0.f}, {left + 13.f, 4.f, 0.f}, {left + 6.f, 16.f, 0.f}};
            const auto stride = (model.vertexEnd - model.vertexStart) / 3;
            for (int i = 0; i < 3; ++i)
                std::memcpy(model.bytes.data() + model.vertexStart + stride * i, vertices[i], sizeof(vertices[i]));
            Wire wire;
            wire.bytes = std::move(model.bytes);
            wire.Write(modelPath);
        }
        WriteV1(project);
        const auto accessory = argc == 6 ? fs::path(argv[5]) : fs::path{};
        WriteV1ModelProject(project, project, {first, second}, accessory);
        std::puts(accessory.empty() ? "PASS wrote a V1 PMM and two one-bone PMD models" :
            "PASS wrote a V1 PMM with two one-bone PMD models and a VAC track");
        return 0;
    }
    if (AppLifecycleRequested()) return RunAppLifecycle(argc);
    const bool cacheSaveMode = CacheSaveRequested();
    const bool cacheResetMode = CacheResetRequested();
    const bool textureReopenMode = argc == 2 && !std::wcscmp(argv[1], L"--texture-reopen");
    const bool splitRootRelocation = argc == 2 && !std::wcscmp(argv[1], L"--split-root-relocation");
    const bool motionModalMode = argc == 2 && !std::wcscmp(argv[1], L"--vmd-modal-boundary");
    const bool motionSaveMode = argc == 2 && !std::wcscmp(argv[1], L"--vmd-save-boundary");
    const bool preflightRefusalMode = argc == 2 && !std::wcscmp(argv[1], L"--preflight-refusal");
    const bool textureOwnerMode = argc == 3 && !std::wcscmp(argv[1], L"--texture-cache-owner");
    const std::wstring_view ownerStep = textureOwnerMode ? argv[2] : L"";
    if (textureOwnerMode) {
        Check(!cacheSaveMode && !cacheResetMode && (ownerStep == L"prepare" || ownerStep == L"cold" ||
              ownerStep == L"warm" || ownerStep == L"share" || ownerStep == L"verify" ||
              (!build_profile::original && (ownerStep == L"repair" || ownerStep == L"repaired-warm"))),
            "the cross-process test accepts only its fixed isolated stages");
    }
    const bool v1AccessoryRecoveryMode = argc == 2 && !std::wcscmp(argv[1], L"--v1-accessory-recovery");
    const bool recoveryPickerDirectories = argc == 2 && !std::wcscmp(argv[1], L"--recovery-picker-directories");
    const bool vacRelocationV1 = argc == 2 && !std::wcscmp(argv[1], L"--vac-relocation-v1");
    const bool vacRelocationMode = vacRelocationV1 || (argc == 2 && !std::wcscmp(argv[1], L"--vac-relocation"));
    const bool v2ModelCancelThrowMode = argc == 2 && !std::wcscmp(argv[1], L"--v2-model-cancel-throw");
    const bool midResetFailureMode = argc == 2 && !std::wcscmp(argv[1], L"--mid-reset-failure");
    const unsigned v2AccessoryRecoveryMode = V2AccessoryRecoveryMode();
    if (v2AccessoryRecoveryMode == 3)
        Check(MMD_ORIGINAL_PROFILE && argc == 1 && !cacheSaveMode && !cacheResetMode,
            "the V2 partial acquisition mode requires the original profile and no other fixture modes");
    if (v2AccessoryRecoveryMode == 4)
        Check(argc == 1 && !cacheSaveMode && !cacheResetMode,
            "the V2 material-buffer exception mode runs alone");
    wchar_t v1MaterialRetryValue[2]{};
    const bool v1MaterialRetryMode = GetEnvironmentVariableW(L"MMS_TEST_V1_ACCESSORY_MATERIAL_RETRY",
        v1MaterialRetryValue, 2) == 1 && v1MaterialRetryValue[0] == L'1';
    if (v1MaterialRetryMode) {
        Check(MMD_ORIGINAL_PROFILE && argc == 1 && !cacheSaveMode && !cacheResetMode && !v2AccessoryRecoveryMode,
            "the isolated V1 material retry mode requires the original profile and no other fixture modes");
    }
    host_cache_save::DesktopScope cacheSaveDesktop;
    if (cacheSaveMode) {
        std::puts("STAGE cache save host entered wmain; switching desktop before GUI initialization");
        if (!cacheSaveDesktop.Open()) return 1;
    }
    wchar_t executable[32768]{}; const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    Check(count > 0 && count < 32768, "resolve the isolated test executable directory");
    if (cacheResetMode) {
        Check(argc == 1, "real Reset mode accepts no external scene arguments");
        PrepareCacheResetHost(fs::path(executable));
    }
    if (cacheSaveMode || cacheResetMode) PrepareCacheSaveHost(fs::path(executable).parent_path());
#if !MMD_ORIGINAL_PROFILE
    const bool pictureBudgetMode = argc == 3 && !std::wcscmp(argv[1], L"--picture-assets-isolated");
    Check(argc == 1 || pictureBudgetMode || textureReopenMode || splitRootRelocation || textureOwnerMode || v1AccessoryRecoveryMode || recoveryPickerDirectories || vacRelocationMode || v2ModelCancelThrowMode || midResetFailureMode || motionModalMode || motionSaveMode || preflightRefusalMode,
        "known isolated PMM host mode");
    if (pictureBudgetMode) {
        const auto parent = fs::canonical(fs::path(executable).parent_path());
        Check(fs::path(argv[2]).is_absolute() && fs::equivalent(parent, fs::canonical(argv[2])) &&
              system_settings::Path() == parent / L"data" / L"settings.json" && !fs::exists(parent / L"data"),
              "budget settings require the explicit actual EXE directory and a previously absent data directory");
    }
#else
    Check(argc == 1 || textureReopenMode || splitRootRelocation || textureOwnerMode || v1AccessoryRecoveryMode || recoveryPickerDirectories || vacRelocationMode || v2ModelCancelThrowMode || midResetFailureMode || motionModalMode || motionSaveMode || preflightRefusalMode,
        "known isolated original-profile PMM host mode");
#endif
    directory = fs::path(executable).parent_path() / L"data" / L"pmm-input-host";
    fs::create_directories(directory);
    Check(directory.native().size() + 32 < 256, "all real loader fixture paths fit the existing PMM path ABI");
    if (textureOwnerMode) {
        Check(fs::equivalent(fs::current_path(), fs::path(executable).parent_path()),
            "the staged host runs only from its own fixture directory");
        host_cache::SelectRoot(directory / L"texture-owner-store");
        Check(SetEnvironmentVariableW(L"MMS_CACHE_MEMORY_ONLY", ownerStep == L"prepare" ? L"1" : L"0"),
            "use an empty disk cache after preparing the fixture in memory");
    } else {
        IsolateHostCache();
    }
    sentinelPath = directory / L"sentinel.bin";
    SetEnvironmentVariableW(L"MMS_NVIDIA_AA_FIX", L"0");
    SetEnvironmentVariableW(L"MMS_RECENT_PROJECTS_FILE", (directory / L"recent.txt").c_str());
    Check(MH_Initialize() == MH_OK, "initialize the isolated real-host observers");
    Hook(&ShowWindow, &Hidden, reinterpret_cast<void**>(&realShow));
    Hook(&SetWindowPos, &HiddenPosition, reinterpret_cast<void**>(&realSetWindowPos));
#if !MMD_ORIGINAL_PROFILE
    if (cacheResetMode) {
        Hook(&SetForegroundWindow, &host_cache_save::DenyForeground);
        Hook(&SetActiveWindow, &host_cache_save::DenyActivation);
        Hook(&SetWindowPlacement, &host_cache_save::HiddenPlacement,
             reinterpret_cast<void**>(&host_cache_save::realPlacement));
    }
#endif
    Hook(&PanelPaint, &ObservePanelPaint, reinterpret_cast<void**>(&realPanelPaint));
    Hook(&PostQuitMessage, &ObservePostQuit, reinterpret_cast<void**>(&realPostQuit));
    Hook(&HandlePaletteChanged2, &ObserveFloatingPalette, reinterpret_cast<void**>(&realFloatingPalette));
    Hook(&MessageBoxW, &ErrorW); Hook(&MessageBoxA, &ErrorA);
    Hook(&ShowUiMessage, &ErrorMessage, reinterpret_cast<void**>(&realShowUiMessage));
#if !MMD_ORIGINAL_PROFILE
    Hook(&PostMessageW, &ObservePostMessageW, reinterpret_cast<void**>(&realPostMessageW));
#endif
    Hook(&ShowProjectModelDialog, &MissingModel);
    Hook(&_wsopen_s, &ObserveOpen, reinterpret_cast<void**>(&realOpen));
    Hook(&_close, &ObserveClose, reinterpret_cast<void**>(&realClose));
    Hook(&_read, &ObserveRead, reinterpret_cast<void**>(&realRead));
    Hook(&_lseeki64, &ObserveSeek, reinterpret_cast<void**>(&realSeek));
#if !MMD_ORIGINAL_PROFILE
    Hook(&pmm_extension::Read, &ObserveExtensionRead, reinterpret_cast<void**>(&realExtensionRead));
#endif
#if MMD_ORIGINAL_PROFILE
    // The real SuccessTail calls this after Input::Close in both V1 and V2.
    // The replacement forwards the complete readout before injecting a fault.
    Hook(&PostViewRefresh, &ViewRefresh, reinterpret_cast<void**>(&realViewRefresh));
#else
    Hook(&native_mesh::ProjectOpened, &ProjectOpened, reinterpret_cast<void**>(&realProjectOpened));
#endif
    if (!cacheResetMode) Hook(&ResetAppState, &Reset, reinterpret_cast<void**>(&realReset));
    Hook(&LocalizeUI, &ObserveLocalize, reinterpret_cast<void**>(&realLocalize));
    Hook(&accessory_import::LoadObject, &ObserveAccessoryLoad, reinterpret_cast<void**>(&realAccessoryLoad));
    Hook(&GetUiOpenFileNameW, &ObserveUiOpenFileName, reinterpret_cast<void**>(&realUiOpenFileName));
    if (motionSaveMode) Hook(&GetUiSaveFileNameW, &ObserveMotionSavePicker);
#if !MMD_ORIGINAL_PROFILE
    // Armed only around the two FrameDriver calls in the quarantine and reopen
    // stages below; every other real frame boundary forwards the host callback.
    Hook(&plugins::PluginFrameBegin, &ObserveFrameBegin, reinterpret_cast<void**>(&realFrameBegin));
#endif
    Hook(&HandleMouseMove, &ObserveMouseMove, reinterpret_cast<void**>(&realMouseMove));
    Hook(&HandleDropFiles, &ObserveDropFiles, reinterpret_cast<void**>(&realDropFiles));
    Hook(&DragFinish, &ObserveDragFinish, reinterpret_cast<void**>(&realDragFinish));
    const HHOOK hide = SetWindowsHookExW(WH_CBT, NoActivation, nullptr, GetCurrentThreadId());
    Check(hide != nullptr, "prevent the isolated real host from showing or activating top-level windows");
    auto instance = std::make_unique<MMDApp>(); app = instance.get(); g_Block = app;
    std::memset(&app->state, 0, sizeof(app->state)); app->InitDefaults();
    if (cacheSaveMode) InitCacheSaveEmptyHost();
    else Check(InitMainWindowAndD3D(app, GetModuleHandleW(nullptr), SW_HIDE), "initialize the actual hidden production host");
    app->EnglishUI() = 2;
    if (motionModalMode || motionSaveMode || preflightRefusalMode) {
        if (motionModalMode) MotionModalCases();
        else if (motionSaveMode) MotionSaveCases();
        else ProjectPreflightRefusalCases();
        ShutdownCleanup(app);
        instance.reset(); app = nullptr; g_Block = nullptr;
        UnhookWindowsHookEx(hide);
        MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
        return 0;
    }
    if (textureOwnerMode) {
        TextureCacheOwnerCases(ownerStep);
        background_save::Shutdown(app);
        cache_manager::Shutdown();
        Check(DestroyWindow(app->MainWindow()) != FALSE, "destroy only the isolated texture cache host");
        ShutdownCleanup(app);
        instance.reset(); app = nullptr; g_Block = nullptr;
        Check(UnhookWindowsHookEx(hide) != FALSE, "release the hidden-window fixture hook");
        MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
        std::printf("PASS PMM texture cache ownership stage=%ls pid=%lu\n", argv[2], GetCurrentProcessId());
        return 0;
    }
    if (textureReopenMode) {
        TextureReopenCases();
        ModelAndTextureReopenCases();
        background_save::Shutdown(app);
        ShutdownCleanup(app);
        g_Block = nullptr;
        UnhookWindowsHookEx(hide);
        MH_Uninitialize();
        std::puts("PASS real PMM V1/V2 texture reopen");
        return 0;
    }
#if !MMD_ORIGINAL_PROFILE
    if (cacheResetMode) {
        host_cache_save::RealResetSaveCases();
        background_save::Shutdown(app);
        Check(DestroyWindow(app->MainWindow()) != FALSE, "destroy only the isolated hidden Reset host");
        ShutdownCleanup(app);
        host_cache_save::HiddenResetBoundary();
        instance.reset(); app = nullptr; g_Block = nullptr;
        Check(UnhookWindowsHookEx(hide) != FALSE, "remove hidden Reset fixture hook");
        MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
        std::printf("PASS real PMM cache Reset/save host: %u checks; full PMM Load/UI acceptance remains pending\n", checks);
        return 0;
    }
#endif
    if (cacheSaveMode) {
        CacheSaveHostCases();
        Check(!IsWindowVisible(app->MainWindow()), "cache save host never becomes visible");
        background_save::Shutdown(app);
        Check(cacheSaveDesktop.CloseWindows(), "destroy private fixture windows before host cleanup");
        ShutdownCacheSaveEmptyHost();
        Check(cacheSaveDesktop.CloseWindows(), "destroy any remaining fixture helper windows");
        instance.reset(); app = nullptr; g_Block = nullptr;
        Check(UnhookWindowsHookEx(hide) != FALSE, "remove the fixture thread hook before restoring desktop");
        MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
        Check(cacheSaveDesktop.Close(), "restore original desktop and release private fixture desktop");
        std::printf("PASS real PMM cache save host: %u checks; scope=empty-scene+model-save-segments; full Load/Reset/UI acceptance remains pending\n", checks);
        return 0;
    }
    if (v1AccessoryRecoveryMode || recoveryPickerDirectories || vacRelocationMode || v1MaterialRetryMode || v2ModelCancelThrowMode || midResetFailureMode || v2AccessoryRecoveryMode != 0) {
        const char* mode = "V2 missing-accessory recovery";
        if (v1AccessoryRecoveryMode) mode = "V1 missing-accessory recovery";
        if (vacRelocationMode) mode = "VAC relocation";
        if (v2ModelCancelThrowMode) mode = "V2 model cancellation throwing resets";
        if (midResetFailureMode) mode = "V2 model cancellation failing inside reset";
        if (v1MaterialRetryMode) mode = "V1 accessory material retry";
        if (v2AccessoryRecoveryMode == 3) mode = "V2 accessory partial acquisition recovery";
        if (v2AccessoryRecoveryMode == 4) mode = "V2 accessory material-buffer exception";
        if (v2AccessoryRecoveryMode == 5) mode = "V2 accessory cancel with persistent reset refusal";
        if (v2AccessoryRecoveryMode == 6) mode = "V1/V2 accessory cancel with persistently throwing resets";
        if (recoveryPickerDirectories) {
            mode = "recovery picker directories";
            RecoveryPickerDirectoryCases();
        } else if (vacRelocationMode) {
            VacRelocationCases(vacRelocationV1);
        } else if (midResetFailureMode) {
            MidResetFailureCases();
        } else if (v1AccessoryRecoveryMode) {
            V1AccessoryRecoveryCases();
        } else if (v2ModelCancelThrowMode) {
            V2ModelCancelThrowCases();
        } else if (v1MaterialRetryMode) {
            Hook(&DisposeAccessory, &ObserveDisposeAccessory, reinterpret_cast<void**>(&realDisposeAccessory));
            V1AccessoryMaterialRetryCase();
        } else {
            V2AccessoryRecoveryCases(v2AccessoryRecoveryMode);
        }
        Check(!app->sceneRecoveryPending && !app->SceneMutationInProgress() &&
              !IsWindowVisible(app->MainWindow()),
            "the isolated accessory regression ends on an editable hidden host");
        background_save::Shutdown(app);
        Check(DestroyWindow(app->MainWindow()) != FALSE,
            "destroy only the isolated accessory regression host");
        ShutdownCleanup(app);
        instance.reset(); app = nullptr; g_Block = nullptr;
        Check(UnhookWindowsHookEx(hide) != FALSE,
            "remove the hidden accessory regression hook before cleanup");
        MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
        std::printf("PASS real %s: %u checks\n", mode, checks);
        return 0;
    }
#if !MMD_ORIGINAL_PROFILE
    {
        const HWND window = app->MainWindow();
        MSG close{};
        {
            ScopedSceneCloseDeferral transaction(*app);
            {
                ScopedSceneMutation outer(*app);
                SendMessageW(window, WM_CLOSE, 0, 0);
                {
                    ScopedSceneMutation nested(*app);
                    SendMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
                }
                Check(IsWindow(window) && app->SceneMutationInProgress(),
                      "nested scene load defers window close without destroying the host");
                Check(!PeekMessageW(&close, window, kSceneCloseReplayMessage, kSceneCloseReplayMessage, PM_NOREMOVE),
                      "close is not reposted while outer scene mutation remains active");
            }
            Check(!PeekMessageW(&close, window, kSceneCloseReplayMessage, kSceneCloseReplayMessage, PM_NOREMOVE),
                  "close remains deferred until the whole PMM transaction completes");
            failDeferredClosePost = true;
        }
        Check(!failDeferredClosePost && app->deferredSceneMainClose == window &&
              !PeekMessageW(&close, window, kSceneCloseReplayMessage, kSceneCloseReplayMessage, PM_NOREMOVE),
              "failed close post retains the pending request");
        updates::SafePoint(app);
        testCloseCancel = true; closeDialogs = 0; app->SceneModified() = 1;
        SendMessageW(window, WM_CLOSE, 0, 0);
        Check(IsWindow(window) && closeDialogs == 0, "synchronous close is coalesced while replay is queued");
        Check(IsWindow(window) && PeekMessageW(&close, window, kSceneCloseReplayMessage, kSceneCloseReplayMessage, PM_REMOVE),
              "one deferred close is posted after the outer scene mutation");
        Check(!PeekMessageW(&close, window, kSceneCloseReplayMessage, kSceneCloseReplayMessage, PM_NOREMOVE),
              "duplicate close requests are coalesced");
        DispatchMessageW(&close);
        Check(IsWindow(window) && closeDialogs == 1, "close reentry during confirmation is coalesced");
        SendMessageW(window, WM_CLOSE, 0, 0);
        Check(IsWindow(window) && closeDialogs == 2, "cancelling close permits a new request");
        SendMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
        Check(IsWindow(window) && closeDialogs == 3, "system close reaches confirmation exactly once");
        SendMessageW(window, kSceneCloseReplayMessage, 0, 0);
        Check(closeDialogs == 3, "stale private close wake cannot close the window");
        const auto takeReplay = [&] {
            Check(PeekMessageW(&close, window, kSceneCloseReplayMessage, kSceneCloseReplayMessage, PM_REMOVE) != FALSE,
                  "pending close has a queued wake");
        };
        const auto prepareReplay = [&] {
            ScopedSceneCloseDeferral transaction(*app);
            SendMessageW(window, WM_CLOSE, 0, 0);
        };
        for (const auto operation : {recording_state::Operation::Start, recording_state::Operation::Finish}) {
            prepareReplay(); takeReplay();
            const auto before = closeDialogs;
            {
                recording_state::Guard recording(app, operation);
                DispatchMessageW(&close);
                updates::SafePoint(app);
                Check(closeDialogs == before && app->deferredSceneMainClose == window && !app->sceneMainClosePosted,
                      "replay arriving during recording stays pending without queue spin");
            }
            updates::SafePoint(app); takeReplay(); DispatchMessageW(&close);
            Check(closeDialogs == before + 1 && !app->deferredSceneMainClose,
                  "recording completion allows exactly one confirmation");
        }
        prepareReplay(); takeReplay();
        const auto beforeBake = closeDialogs;
        {
            bake::ScopedUiInputGate bakeGate;
            DispatchMessageW(&close);
            Check(closeDialogs == beforeBake && app->deferredSceneMainClose == window && !app->sceneMainClosePosted,
                  "bake gate retains a consumed close wake");
        }
        updates::SafePoint(app); takeReplay(); DispatchMessageW(&close);
        Check(closeDialogs == beforeBake + 1, "close resumes after bake gate releases");
        app->sceneFloatingCloseProcessing = true;
        SendMessageW(window, WM_CLOSE, 0, 0);
        Check(app->deferredSceneMainClose == window && closeDialogs == beforeBake + 1,
              "floating-window close holds a reentrant main-window close");
        app->sceneFloatingCloseProcessing = false;
        updates::SafePoint(app); takeReplay(); DispatchMessageW(&close);
        Check(closeDialogs == beforeBake + 2, "other window completion releases the pending main close");
        testCloseCancel = false; app->SceneModified() = 0;
    }
#endif
    const auto v2 = directory / L"native-v2.pmm", v1 = directory / L"native-v1.pmm";
    app->state.optflag[0] = 1; app->SetSelectedModelSlot(0); app->CurrentFrame() = 12;
    const auto camera = CameraRoot(); const auto light = LightRoot();
    app->CameraKeys()[0] = camera; app->LightKeys()[0] = light;
    app->CameraFov() = 43.f;
    for (unsigned i = 0; i < 3; ++i) { app->LightColor()[i] = light.color[i]; app->LightDirection()[i] = light.direction[i]; }
    wcscpy_s(app->EnvFileName(), 256, v2.c_str()); app->SceneModified() = 1;
    SaveSceneFile(app);
    const auto saved = Bytes(v2);
    Check(!app->SceneModified() && saved.size() > 30 && !std::memcmp(saved.data(), "Polygon Movie maker 0002", 24),
        "the actual native writer supplies a complete ordinary V2 PMM fixture");
    WriteV1(v1);
    if (splitRootRelocation) {
        SplitRootRelocationCases(v1, camera, light);
        ShutdownCleanup(app);
        g_Block = nullptr;
        UnhookWindowsHookEx(hide);
        MH_Uninitialize();
        return 0;
    }
    // Accumulates every new final-recovery-feedback expectation so the unfixed
    // production still runs all new cases in one RED pass; the single Check is
    // emitted after the exit-stage boundary below.
    bool postloadOk = true;
    unsigned postloadForwardPasses = 0;
    bool postloadLegacyV2Persist = false, postloadLegacyV2Forward = false;
    bool postloadLegacyV1Persist = false, postloadLegacyV1Forward = false;
    const auto RecordPostload = [&](bool ok, const char* what) {
        if (!ok) std::fprintf(stderr, "DIAG postload mismatch: %s\n", what);
        postloadOk = postloadOk && ok;
    };
    std::puts("STAGE real V2 writer/parser and genuine V1 mandatory-wire/optional-EOF normal loads");
    Normal(v2, camera, light); Normal(v1, camera, light);
    CacheHostCases(v1, v2, camera, light);
    V1RelocationCases(v1, camera, light);
    V2RelocationCases(v1, v2, camera, light);
    OwnerLifetimeCases(v2, camera, light);
#if !MMD_ORIGINAL_PROFILE
    PictureProjectCases(v1, v2, camera, light, pictureBudgetMode);
#endif
    for (const auto& path : {v2, v1}) for (const auto type : {Fault::Memory, Fault::Runtime})
        for (const bool reuseNumber : {false, true}) {
            std::printf("STAGE real PMM tail version=%u fault=%s reuse=%u\n", path == v1 ? 1u : 2u,
                type == Fault::Memory ? "bad_alloc" : "runtime_error", unsigned(reuseNumber));
            TailFailure(path, type, reuseNumber);
        }
    const auto missing = directory / L"missing-model-v2.pmm"; WriteMissingV2(missing);
    std::puts("STAGE real V2 missing-model Abort with one-shot post-close Reset exception and exact fd reuse");
    const auto oldTails = tailCalls; const auto beforeMissing = Bytes(missing);
    const auto abortThrowNotices = postloadNotices, abortThrowMessages = messages;
    Arm(missing, Fault::AbortReset, true);
    const bool loaded = LoadSceneFile(missing.c_str()); Finish(loaded, false);
    Check(missingDialogs == 1 && resetThrows == 1 && tailCalls == oldTails &&
          messages == abortThrowMessages && Bytes(missing) == beforeMissing,
        "the actual V2 Abort consumes its workspace once before a throwing Reset and shell recovery");
    RecordPostload(postloadNotices == abortThrowNotices && !tailObserved,
        "one-shot V2 abort reset exception recovers through the real reset with no final-failure notice");
    std::puts("STAGE real V2 missing-model Abort with one ignored Reset false and a non-zero playhead sentinel");
    const auto beforeFalseReset = Bytes(missing);
    const auto falseResetDialogs = missingDialogs;
    const auto falseResetMessages = messages;
    const auto ignoredFalseNotices = postloadNotices;
    Arm(missing, Fault::AbortResetFalse, false);
    expectedMissingAbortDialogs = falseResetDialogs + 1;
    const bool refusedLoad = LoadSceneFile(missing.c_str());
    const auto residualFrame = app->CurrentFrame();
    unsigned residualModels = 0, residualAccessories = 0;
    for (unsigned slot = 0; slot < 255; ++slot) {
        if (app->ModelSlot(slot)) ++residualModels;
        if (app->AccessorySlot(slot)) ++residualAccessories;
    }
    std::fprintf(stderr,
        "DIAG PMM ignored Reset false loaded=%u refusals=%u sentinel=%d frame=%d models=%u accessories=%u\n",
        unsigned(refusedLoad), resetFalseRefusals, int(resetFalseResidualFrame), int(residualFrame),
        residualModels, residualAccessories);
    // The real V2 abort path ignores ResetAppState's boolean (pmm_load_v2.cpp
    // AbortV2Load). On the defect the already replaced scene keeps the sentinel
    // playhead and the partial model slot; recovery must run the actual reset.
    Check(resetFalseRefusals == 1 && resetFalseResidualFrame == kResetFalseResidualFrame &&
        missingDialogs == falseResetDialogs + 1 && resetThrows == 1,
        "the one-shot Reset false is taken once by the actual V2 missing-model Abort and plants its non-zero playhead sentinel");
    Check(!refusedLoad && !tailObserved && tailCalls == oldTails && messages == falseResetMessages &&
        Bytes(missing) == beforeFalseReset,
        "the ignored Reset false still fails the load as before, with no success tail, extra modal or source rewrite");
    RecordPostload(postloadNotices == ignoredFalseNotices,
        "the one-shot ignored Abort reset false still recovers through the real reset and shows no final-failure notice");
    Check(residualFrame == 0,
        "the actual ResetAppState clears the replaced scene's sentinel playhead even though the first Reset returned false");
    Check(residualModels == 0 && residualAccessories == 0,
        "the failed Reset leaves no partially loaded native model or accessory in the already replaced scene");
    Finish(refusedLoad, false);
    // Detach the real controls before the load fails. Both ShowWindow and
    // SetWindowPos keep this production window hidden and non-activating.
    Check(!app->FloatingWindow(), "the detached-window regression starts attached");
    std::array<HWND, 22> detachedControls{};
    for (int id = 536; id <= 557; ++id)
        detachedControls[id - 536] = GetDlgItem(app->MainWindow(), id);
    InitFlagSubsystem(app);
    const HWND quarantinedFloating = app->FloatingWindow();
    Check(IsWindow(quarantinedFloating) && !IsWindowVisible(quarantinedFloating),
        "the real detached window exists without becoming visible");
    Check(detachedControls[0] && GetParent(detachedControls[0]) == quarantinedFloating,
        "the actual detached-window entry reparents its controls");
    std::puts("STAGE real V2 missing-model Abort with two refused resets, a quarantined scene and refused real saves");
    const auto beforeQuarantine = Bytes(missing);
    const auto quarantineTails = tailCalls, quarantineDialogs = missingDialogs, quarantineMessages = messages;
    const auto quarantineNotices = postloadNotices;
    Arm(missing, Fault::AbortResetQuarantine, false);
    expectedQuarantineAbortDialogs = quarantineDialogs + 1;
    const bool quarantinedLoad = LoadSceneFile(missing.c_str());
    Finish(quarantinedLoad, false, true);
    Check(!quarantinedLoad && quarantineRefusals == 2 && quarantineResidualFrame == kQuarantineResidualFrame &&
        missingDialogs == expectedQuarantineAbortDialogs && tailCalls == quarantineTails &&
        messages == quarantineMessages && Bytes(missing) == beforeQuarantine,
        "the actual V2 abort's Reset and its bounded recovery retry both refuse, so the load fails with exactly one dialog, no tail, no extra modal and an unchanged source");
    RecordPostload(postloadNotices - quarantineNotices == 1 &&
        tailCalls == quarantineTails && messages == quarantineMessages &&
        missingDialogs == expectedQuarantineAbortDialogs,
        "the double-refused V2 missing-model load ends with exactly one final-recovery notice while every old error and model-selection counter stays unchanged");
    Check(app->FloatingWindow() == quarantinedFloating && IsWindow(quarantinedFloating),
        "the real detached window survives the failed load");
    const auto floatingX = app->MouseX(), floatingY = app->MouseY();
    const auto floatingShift = app->ShiftModifierState(), floatingCtrl = app->CtrlModifierState();
    const auto floatingDistance = app->CameraDistance();
    const HWND floatingCapture = GetCapture();
    SendMessageA(quarantinedFloating, WM_MOUSEMOVE, MK_SHIFT | MK_CONTROL, MAKELPARAM(111, 123));
    SendMessageA(quarantinedFloating, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    SendMessageA(quarantinedFloating, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(111, 123));
    Check(app->MouseX() == floatingX && app->MouseY() == floatingY &&
        app->ShiftModifierState() == floatingShift && app->CtrlModifierState() == floatingCtrl &&
        app->CameraDistance() == floatingDistance && GetCapture() == floatingCapture,
        "quarantined floating input cannot change coordinates, modifiers, camera distance or capture");
    Check(SendMessageA(quarantinedFloating, WM_MOUSEACTIVATE,
        reinterpret_cast<WPARAM>(quarantinedFloating), MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_ACTIVATEANDEAT,
        "quarantined floating activation consumes the initiating click");
    const auto isolatedPalette = floatingPalettePaints;
    SendMessageA(quarantinedFloating, WM_PAINT, 0, 0);
    HDC floatingDC = GetDC(quarantinedFloating);
    Check(floatingDC != nullptr, "obtain the real detached window DC for print-client");
    SendMessageA(quarantinedFloating, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(floatingDC), PRF_CLIENT);
    ReleaseDC(quarantinedFloating, floatingDC);
    Check(floatingPalettePaints == isolatedPalette,
        "quarantined floating paint and print-client never enter the scene palette renderer");
    const auto isolatedPanels = panelPaints;
    // Loading may update the saved attached width while the window is detached.
    const auto attachedSidebar = app->SeparateWindowSidebarWidth();
    SendMessageA(quarantinedFloating, WM_CLOSE, 0, 0);
    std::fprintf(stderr, "DIAG floating close live=%d attached=%d sidebar=%d expected=%d panels=%u before=%u pending=%d frame=%d\n",
        int(IsWindow(quarantinedFloating)), int(app->FloatingWindow() != nullptr),
        int(app->SidebarWidth()), int(attachedSidebar), panelPaints, isolatedPanels,
        int(app->sceneRecoveryPending), int(app->CurrentFrame()));
    Check(!IsWindow(quarantinedFloating) && !app->FloatingWindow() &&
        app->SidebarWidth() == attachedSidebar && panelPaints == isolatedPanels &&
        app->sceneRecoveryPending && app->CurrentFrame() == kQuarantineResidualFrame,
        "quarantined close destroys the real floating window and restores geometry without painting model keys");
    for (HWND control : detachedControls)
        if (control) Check(IsWindow(control) && GetParent(control) == app->MainWindow(),
            "quarantined close returns each detached control to the main window");
    // A real save must refuse the half-loaded scene before the destination
    // path, snapshot buffer, job, status line, or project identity is touched.
    const auto ordinaryTarget = directory / L"quarantine-save-target.pmm";
    // A backup writer never replaces an existing destination, so that route's
    // controlled target starts absent: an existing canary would fail for its
    // own reason and hide a removed quarantine gate. Its byte-level expectation
    // is therefore "still absent" while the ordinary target stays a canary.
    const auto backupTarget = directory / L"quarantine-backup-target.pmm";
    const auto recoveredTarget = directory / L"recovered-quarantine.pmm";
    for (const auto& target : {backupTarget, recoveredTarget})
        if (fs::exists(target)) Check(DeleteFileW(target.c_str()) != FALSE,
            "remove only this stage's own previous save destination before reuse");
    constexpr char ordinaryCanary[] = "controlled non-empty ordinary save destination; these exact bytes must survive";
    {
        std::ofstream output(ordinaryTarget, std::ios::binary | std::ios::trunc);
        output.write(ordinaryCanary, sizeof ordinaryCanary - 1); output.close();
        Check(bool(output) && fs::file_size(ordinaryTarget) == sizeof ordinaryCanary - 1,
            "write a controlled non-empty ordinary save destination inside the isolated installation");
    }
    const auto ordinaryBefore = Bytes(ordinaryTarget);
    unsigned filesBefore = 0;
    for (const auto& entry : fs::directory_iterator(directory)) if (entry.is_regular_file()) ++filesBefore;
    wcscpy_s(app->EnvFileName(), 256, ordinaryTarget.c_str());
    wchar_t titleBefore[256]{}; GetWindowTextW(app->MainWindow(), titleBefore, 256);
    const auto projectDirectoryBefore = std::wstring(app->PathWorkspace().projectDirectory);
    const auto generationBefore = app->SceneGeneration();
    const std::uint8_t dirtyBefore = app->SceneModified();
    const auto statusBefore = std::wstring(background_save::StatusText(app));
    Check(app->sceneRecoveryPending && !SceneSaveInProgress() && !SceneSaveBlocksEdits(),
        "the quarantined scene reaches the real save gate outside every save transaction");
    background_save::Options backupOptions; backupOptions.backup = true;
    Check(!SaveSceneFile(app),
        "the real ordinary save refuses the quarantined scene before its controlled non-empty destination is opened");
    Check(!SaveSceneCopy(app, backupTarget.c_str()),
        "the real backup copy refuses the quarantined scene before its absent destination can be created");
    const bool requestedOrdinary = background_save::Request(app, ordinaryTarget.c_str());
    const bool requestedBackup = background_save::Request(app, backupTarget.c_str(), backupOptions);
    Check(!requestedOrdinary && !requestedBackup,
        "the background save request refuses ordinary and backup options before capturing a snapshot, allocating a job or starting a writer thread");
    Check(Bytes(ordinaryTarget) == ordinaryBefore && !fs::exists(backupTarget),
        "every refused save route leaves the controlled target byte-identical and creates no backup file");
    unsigned filesAfter = 0;
    for (const auto& entry : fs::directory_iterator(directory)) if (entry.is_regular_file()) ++filesAfter;
    Check(filesAfter == filesBefore,
        "no refused save leaves a temporary, lock, sidecar or new file in the isolated directory");
    wchar_t titleAfter[256]{}; GetWindowTextW(app->MainWindow(), titleAfter, 256);
    Check(!std::wcscmp(app->EnvFileName(), ordinaryTarget.c_str()) &&
        std::wstring(app->PathWorkspace().projectDirectory) == projectDirectoryBefore &&
        !std::wcscmp(titleAfter, titleBefore) && app->SceneGeneration() == generationBefore &&
        app->SceneModified() == dirtyBefore && app->CurrentFrame() == kQuarantineResidualFrame &&
        app->ModelSlot(0) != nullptr && app->sceneRecoveryPending && !SceneSaveInProgress() &&
        !SceneSaveBlocksEdits() && std::wstring(background_save::StatusText(app)) == statusBefore,
        "refused saves leave the quarantined scene's save identity, dirty flag, playhead, partial model, gate and status unchanged");
#if !MMD_ORIGINAL_PROFILE
    // The real shared journal is isolated by the same gate: a valid applied
    // reservation is refused before any transaction or handle exists, and
    // its counts and revision do not move. The identical input is retried
    // below once the real reset clears the quarantine.
    auto* quarantineJournal = plugins::NativeHistory(app);
    std::uint64_t isolatedRevision = 0;
    Check(quarantineJournal && quarantineJournal->Revision(isolatedRevision) == MMS_OK,
        "the quarantined scene still exposes its real shared journal");
    const auto isolatedUndo = quarantineJournal->UndoCount();
    const auto isolatedRedo = quarantineJournal->RedoCount();
    constexpr char isolatedEditLabel[] = "quarantined history isolation";
    constexpr std::uint64_t isolatedEditBytes = 4096;
    std::unique_ptr<plugins::SharedHistory::AppliedTransaction> isolatedApplied;
    Check(quarantineJournal->ReserveAppliedNative(isolatedEditLabel, isolatedEditBytes, isolatedApplied) == MMS_E_BUSY &&
        !isolatedApplied && quarantineJournal->UndoCount() == isolatedUndo &&
        quarantineJournal->RedoCount() == isolatedRedo,
        "the real shared journal refuses a valid applied reservation from the quarantined scene without a handle or history change");
    std::uint64_t isolatedRevisionAfter = 0;
    Check(quarantineJournal->Revision(isolatedRevisionAfter) == MMS_OK && isolatedRevisionAfter == isolatedRevision,
        "the refused applied reservation leaves the quarantined scene's shared journal revision unchanged");
#endif
    // The quarantined scene takes no edit from the real main-window input path.
    // These are the messages a menu, accelerator, child control, mouse and a
    // shell drop actually deliver; the observers above count the production
    // entries and forward them, so no OS pointer or file can be touched here.
    const HWND window = app->MainWindow();
    const HWND axisCheckbox = GetDlgItem(window, panel::kCoordAxisCheckbox);
    Check(app->sceneRecoveryPending && IsWindow(window) && !app->SceneMutationInProgress() &&
        GetMenu(window) != nullptr && axisCheckbox != nullptr,
        "the quarantined scene still has its real main window, menu and coordinate-axis child control");
    const auto menuGrid = [&] {
        return (GetMenuState(GetMenu(window), 215, MF_BYCOMMAND) & MF_CHECKED) != 0; };
    const auto boxedGrid = [&] {
        return SendMessageW(axisCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    const auto grid = app->GroundGridEnabled();
    const bool menuBefore = menuGrid(), boxedBefore = boxedGrid();
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(215, 0), 0);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(215, 1), 0);
    Check(app->GroundGridEnabled() == grid && menuGrid() == menuBefore && boxedGrid() == boxedBefore,
        "the quarantined scene drops the coordinate-axis menu and accelerator commands before CommandDispatch");
    const unsigned movesBefore = mouseMoves;
    const auto mouseX = app->MouseX(), mouseY = app->MouseY();
    const auto previousX = app->PreviousMouseX(), previousY = app->PreviousMouseY();
    const auto shiftBefore = app->ShiftModifierState(), ctrlBefore = app->CtrlModifierState();
    const HWND captureBefore = GetCapture();
    SendMessageW(window, WM_MOUSEMOVE, MK_SHIFT | MK_CONTROL | MK_LBUTTON, MAKELPARAM(321, 123));
    Check(mouseMoves == movesBefore && app->MouseX() == mouseX && app->MouseY() == mouseY &&
        app->PreviousMouseX() == previousX && app->PreviousMouseY() == previousY &&
        app->ShiftModifierState() == shiftBefore && app->CtrlModifierState() == ctrlBefore,
        "the quarantined scene drops a modified mouse move before HandleMouseMove and the modifier stores");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, MAKELPARAM(321, 123));
    Check(GetCapture() == captureBefore && app->MouseX() == mouseX && app->MouseY() == mouseY,
        "the quarantined scene drops a press before SetCapture and leaves the pointer state alone");
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(321, 123));
    Check(GetCapture() == captureBefore && app->ShiftModifierState() == shiftBefore &&
        app->CtrlModifierState() == ctrlBefore,
        "the quarantined scene drops the matching release without touching capture or modifier state");
    HGLOBAL drop = DropFileBlock(directory / L"quarantined-drop.txt");
    const unsigned dropsBefore = dropFiles, finishesBefore = dragFinishes;
    SendMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(drop), 0);
    Check(dropFiles == dropsBefore && dragFinishes == finishesBefore + 1 &&
        finishedDrop == static_cast<HDROP>(drop) && app->sceneRecoveryPending &&
        app->CurrentFrame() == kQuarantineResidualFrame,
        "the quarantined scene consumes a valid DROPFILES handle with one DragFinish and never calls HandleDropFiles");
    // File > Exit/New/Open ids delivered by a real child control: the
    // notification's own HWND in lParam must not pass as the menu command. The
    // dirty flag only selects those commands' ordinary confirmation branches if
    // the gate ever fails; it is restored below and does not fake the quarantine.
    const std::uint8_t dirtyBeforeInput = app->SceneModified();
    const auto dialogFlagBefore = app->state.dialogFlags[0];
    const auto enterKeyBefore = app->state.enterKeyState;
    const auto messagesBefore = messages, abortsBefore = missingDialogs;
    app->SceneModified() = 1;
    for (const unsigned id : {200u, 204u, 205u})
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(axisCheckbox));
    Check(IsWindow(window) && app->sceneRecoveryPending && app->SceneModified() == 1 &&
        app->state.dialogFlags[0] == dialogFlagBefore && app->state.enterKeyState == enterKeyBefore &&
        messages == messagesBefore && missingDialogs == abortsBefore &&
        app->CurrentFrame() == kQuarantineResidualFrame &&
        !std::wcscmp(app->EnvFileName(), ordinaryTarget.c_str()),
        "same-id child control notifications cannot execute Exit/New/Open on the quarantined scene");
#if !MMD_ORIGINAL_PROFILE
    // Window close stays live while the scene is quarantined: both the direct
    // WM_CLOSE and the SC_CLOSE system command reach the existing unsaved
    // confirmation and cancel without destroying the window.
    testCloseCancel = true; closeDialogs = 0;
    SendMessageW(window, WM_CLOSE, 0, 0);
    Check(IsWindow(window) && closeDialogs == 1 && messages == messagesBefore &&
        app->sceneRecoveryPending && app->CurrentFrame() == kQuarantineResidualFrame,
        "WM_CLOSE on the quarantined scene reaches the existing cancel path once and keeps the window");
    SendMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
    Check(IsWindow(window) && closeDialogs == 2 && messages == messagesBefore &&
        app->sceneRecoveryPending && app->CurrentFrame() == kQuarantineResidualFrame,
        "SC_CLOSE on the quarantined scene reaches the same cancel path once and keeps the window");
    testCloseCancel = false;
#endif
    app->SceneModified() = dirtyBeforeInput;
    // The pending-recovery gate must refuse while the reset still refuses: a
    // valid candidate fails before the real reset, file read, plugin
    // transaction, success tail, or save identity can run.
    const auto bypassTails = tailCalls, bypassCloses = inputCloses;
    const auto bypassReads = inputReads;
    const auto bypassNotices = recoveryGateNotices;
    const auto bypassNativeNotices = recoveryGateNativeNotices;
    candidate = v2; monitor = true;
    const bool bypassed = LoadSceneFile(v2.c_str());
    monitor = false;
    recoveryGateMonitorNotices[0] = recoveryGateNotices - bypassNotices;
    recoveryGateMonitorNativeNotices[0] = recoveryGateNativeNotices - bypassNativeNotices;
    Check(!bypassed && app->sceneRecoveryPending && quarantineRefusals == 3 &&
        tailCalls == bypassTails && inputCloses == bypassCloses && inputReads == bypassReads &&
        app->CurrentFrame() == kQuarantineResidualFrame && app->ModelSlot(0) != nullptr &&
        !std::wcscmp(app->EnvFileName(), ordinaryTarget.c_str()),
        "the pending-recovery gate refuses a valid project before the real reset, file read, success tail or save identity while the reset still refuses");
#if !MMD_ORIGINAL_PROFILE
    // The same isolation must keep the quarantined scene out of the real frame
    // entry, not only out of saves and inputs. FrameDriver's new
    // sceneRecoveryPending return (frame_driver.cpp:556) sits before FrameScope
    // and plugins::PluginFrameBegin, so the armed observer must stay unused and
    // the playhead, presentation count, modifier stores, scene identity and
    // shared journal must keep the refused double reset's values unchanged. A
    // hit would throw and is caught here, so a leaked frame stops before it can
    // evaluate the half-loaded model.
    Check(!updates::Running() && app->sceneRecoveryPending &&
        app->CurrentFrame() == kQuarantineResidualFrame,
        "the frame gate regression starts outside any frame scope on the quarantined scene");
    const auto quarantineFrame = app->CurrentFrame();
    const auto quarantinePresented = app->frameRate.presentedFrames;
    const auto quarantineShift = app->ShiftModifierState(), quarantineCtrl = app->CtrlModifierState();
    const auto quarantineGeneration = app->SceneGeneration();
    const auto quarantineDirty = app->SceneModified();
    const auto quarantineUndo = quarantineJournal->UndoCount(), quarantineRedo = quarantineJournal->RedoCount();
    bool frameLeaked = false;
    frameBoundaryArmed = true; frameBoundaryHits = 0;
    try { FrameDriver(app); }
    catch (const FrameBoundaryStop&) { frameLeaked = true; }
    frameBoundaryArmed = false;
    Check(!frameLeaked && frameBoundaryHits == 0 && app->sceneRecoveryPending &&
        app->CurrentFrame() == quarantineFrame && app->frameRate.presentedFrames == quarantinePresented &&
        app->ShiftModifierState() == quarantineShift && app->CtrlModifierState() == quarantineCtrl &&
        app->SceneGeneration() == quarantineGeneration && app->SceneModified() == quarantineDirty &&
        app->ModelSlot(0) != nullptr && !app->AccessorySlot(0) &&
        !std::wcscmp(app->EnvFileName(), ordinaryTarget.c_str()) &&
        quarantineJournal->UndoCount() == quarantineUndo && quarantineJournal->RedoCount() == quarantineRedo &&
        quarantineJournal->CheckNativeEdit() == MMS_E_BUSY,
        "the quarantined scene takes no frame at the real FrameDriver entry: the plugin boundary is never hit and playhead, presentation, modifiers, scene identity and journal stay unchanged");
#endif
#if !MMD_ORIGINAL_PROFILE
    // The real WM_PAINT entry must isolate the quarantined scene too. The
    // hidden fixture window keeps no update region and this regression does not
    // observe its BeginPaint/EndPaint pairing (both logged NOT RUN below); the
    // scrollbar fields plus scene identity show the gate returned before the
    // chrome, exposure, frame and scrollbar work.
    struct ScrollbarFields {
        std::int32_t cbSize, fMask, nMin, nMax, nPos, nPage;
    };
    const auto readScrollbar = [&] {
        return ScrollbarFields{app->state.scrollCbSize, app->state.scrollFMask,
            app->state.scrollNMin, app->state.scrollNMax, app->state.scrollNPos,
            app->state.scrollNPage};
    };
    const auto writeScrollbar = [&](const ScrollbarFields& fields) {
        app->state.scrollCbSize = fields.cbSize;
        app->state.scrollFMask = fields.fMask;
        app->state.scrollNMin = fields.nMin;
        app->state.scrollNMax = fields.nMax;
        app->state.scrollNPos = fields.nPos;
        app->state.scrollNPage = fields.nPage;
    };
    const auto scrollbarBefore = readScrollbar();
    constexpr ScrollbarFields scrollbarSentinel{11111, 22222, 33333, 44444, 55555, 66666};
    writeScrollbar(scrollbarSentinel);
    Check(app->sceneRecoveryPending && app->FullscreenMode() == 0 &&
        !app->SceneMutationInProgress() && static_cast<HWND>(app->Hwnd()) == window && IsWindow(window),
        "the quarantined paint starts at the recovery gate, not the fullscreen or scene-mutation early return");
    auto* const quarantinedModel = app->ModelSlot(0);
    HandleWindowPaint(app);
    const auto scrollbarQuarantined = readScrollbar();
    Check(scrollbarQuarantined.cbSize == scrollbarSentinel.cbSize &&
        scrollbarQuarantined.fMask == scrollbarSentinel.fMask &&
        scrollbarQuarantined.nMin == scrollbarSentinel.nMin &&
        scrollbarQuarantined.nMax == scrollbarSentinel.nMax &&
        scrollbarQuarantined.nPos == scrollbarSentinel.nPos &&
        scrollbarQuarantined.nPage == scrollbarSentinel.nPage,
        "the quarantined paint returns before the scrollbar refresh and leaves all six SCROLLINFO fields at their sentinels");
    Check(app->sceneRecoveryPending && app->CurrentFrame() == quarantineFrame &&
        app->frameRate.presentedFrames == quarantinePresented &&
        app->ShiftModifierState() == quarantineShift && app->CtrlModifierState() == quarantineCtrl &&
        app->SceneGeneration() == quarantineGeneration && app->SceneModified() == quarantineDirty &&
        app->ModelSlot(0) == quarantinedModel && !app->AccessorySlot(0) &&
        !std::wcscmp(app->EnvFileName(), ordinaryTarget.c_str()) &&
        quarantineJournal->UndoCount() == quarantineUndo && quarantineJournal->RedoCount() == quarantineRedo &&
        quarantineJournal->CheckNativeEdit() == MMS_E_BUSY,
        "the quarantined paint touches no frame, presentation, modifier, scene identity, model slot or journal state");
    // Do not let the sentinel SCROLLINFO ranges survive into the real reopen;
    // the recovered stage seeds them again before its own paint.
    writeScrollbar(scrollbarBefore);
#else
    std::puts("NOT RUN HandleWindowPaint sceneRecoveryPending isolation: the regression runs in the full configuration only");
#endif
    // Release the injection: the next real open runs the actual reset through
    // the pending-recovery gate, clears the quarantine, and loads normally
    // through the EnvFileName alias frozen before that reset.
    const auto reopenTails = tailCalls;
    Arm(v2, Fault::None, false);
    wcscpy_s(app->EnvFileName(), 256, v2.c_str());
    const bool reopened = LoadSceneFile(app->EnvFileName());
    Check(reopened && !app->sceneRecoveryPending && tailCalls == reopenTails + 1 &&
        !std::wcscmp(app->EnvFileName(), v2.c_str()) && app->CurrentFrame() == 12,
        "the released injection lets the real reset clear the quarantine through the EnvFileName alias and reopen the valid fixture");
    Finish(reopened, true);
    InitFlagSubsystem(app);
    const HWND recoveredFloating = app->FloatingWindow();
    Check(IsWindow(recoveredFloating) && !IsWindowVisible(recoveredFloating),
        "the recovered scene can detach its controls again without showing a window");
    SendMessageA(recoveredFloating, WM_MOUSEMOVE, MK_SHIFT | MK_CONTROL, MAKELPARAM(211, 223));
    Check(app->MouseX() == 211 && app->MouseY() == 223 &&
        app->ShiftModifierState() == 3 && app->CtrlModifierState() == 3,
        "the recovered detached window executes the same mouse-input route");
    const auto recoveredPalette = floatingPalettePaints;
    SendMessageA(recoveredFloating, WM_PAINT, 0, 0);
    Check(floatingPalettePaints > recoveredPalette,
        "the recovered detached window executes the real palette paint route");
    const auto recoveredPanels = panelPaints;
    SendMessageA(recoveredFloating, WM_SYSCOMMAND, SC_CLOSE, 0);
    Check(!IsWindow(recoveredFloating) && !app->FloatingWindow() && panelPaints > recoveredPanels,
        "the recovered floating system-close destroys the window and refreshes the normal panel");
#if !MMD_ORIGINAL_PROFILE
    // The same real entry after the actual reset cleared the quarantine must
    // now run to the scrollbar refresh. Camera display mode keeps the bone-list
    // refresh on the display-object list instead of a selected native model,
    // and the isolation sentinels must be replaced by the normal SCROLLINFO
    // values.
    Check(!app->sceneRecoveryPending && app->FullscreenMode() == 0 &&
        !app->SceneMutationInProgress() && static_cast<HWND>(app->Hwnd()) == window && IsWindow(window),
        "the recovered paint starts on the real normal path and not an early return");
    const auto recoveredDisplayMode = app->state.optflag[0];
    app->state.optflag[0] = 1;
    writeScrollbar(scrollbarSentinel);
    HandleWindowPaint(app);
    RECT recoveredClient{};
    Check(GetClientRect(window, &recoveredClient) != FALSE,
        "the recovered paint keeps the real main window's valid client area");
    const int recoveredPage = (recoveredClient.bottom - 398 -
        panel::TimelinePanelShift(app->SidebarWidth())) / 14 - 1;
    const auto scrollbarRecovered = readScrollbar();
    Check(scrollbarRecovered.cbSize == 28 && scrollbarRecovered.fMask == 7 &&
        scrollbarRecovered.nMin == 0 && scrollbarRecovered.nPage == recoveredPage &&
        scrollbarRecovered.nPos == app->DisplayObjectListScrollPosition() &&
        scrollbarRecovered.nMax == app->DisplayObjectListMatchCount() - 2 + recoveredPage,
        "the recovered HandleWindowPaint writes the normal bone-list SCROLLINFO fields instead of stopping before the selected model");
    app->state.optflag[0] = recoveredDisplayMode;
    // The recovered write-back is proven above; restore the fixture's pre-test
    // scrollbar fields instead of leaving test-owned values behind.
    writeScrollbar(scrollbarBefore);
#else
    std::puts("NOT RUN HandleWindowPaint recovered scrollbar write-back: the regression runs in the full configuration only");
#endif
    // The hidden main window keeps no update region, so neither the non-empty
    // update-region consumption nor a real BeginPaint/EndPaint pairing is
    // observed here; both stay unverified. The post-FrameDriver recheck needs a
    // pump that can quarantine the scene mid-paint, and visual acceptance
    // belongs to Codex.
    std::puts("NOT RUN HandleWindowPaint update-region consumption: the hidden main window keeps no update region");
    std::puts("NOT RUN HandleWindowPaint BeginPaint/EndPaint pairing: not observed by this regression");
    std::puts("NOT RUN HandleWindowPaint post-FrameDriver sceneRecoveryPending recheck: the direct call pumps no quarantining message");
    std::puts("NOT RUN HandleWindowPaint visual acceptance: Codex owns pixel review");
#if !MMD_ORIGINAL_PROFILE
    // The same FrameDriver must reach the boundary after the released injection
    // really reset and reopened the scene. The armed observer records exactly one
    // hit and its dedicated exception stops the frame; only this call site
    // catches that type. Reaching here proves the quarantine no-hit above came
    // from the entry gate, not a dead observer, and that FrameScope's destructor
    // clears the frame guard during the unwind so the host can keep running.
    Check(!updates::Running() && !app->sceneRecoveryPending && app->CurrentFrame() == 12,
        "the recovered frame gate regression starts outside any frame scope on the reopened fixture");
    const auto reopenedPresented = app->frameRate.presentedFrames;
    bool frameStopped = false;
    frameBoundaryArmed = true; frameBoundaryHits = 0;
    try { FrameDriver(app); }
    catch (const FrameBoundaryStop&) { frameStopped = true; }
    frameBoundaryArmed = false;
    Check(frameStopped && frameBoundaryHits == 1 && !updates::Running() &&
        !app->sceneRecoveryPending && app->CurrentFrame() == 12 &&
        app->frameRate.presentedFrames == reopenedPresented,
        "the recovered scene hits the real plugin frame boundary once and the test exception stops the frame, so the quarantine no-hit is the entry gate and the frame scope is released");
#endif
    // Scope: only FrameDriver's entry gate above is covered. The second gate
    // re-checks sceneRecoveryPending after device_lost_wait::Resume
    // (frame_driver.cpp:588); it needs a real device-loss pump to quarantine the
    // scene mid-frame, which this hidden host does not induce, so that check is
    // deliberately not run and stays unverified.
    std::puts("NOT RUN FrameDriver post-device-recovery sceneRecoveryPending gate: no device loss is induced by this hidden host");
#if !MMD_ORIGINAL_PROFILE
    // The real reset cleared the quarantine, so the identical reservation now
    // succeeds. Its handle keeps the journal gate closed until it is released
    // without a commit, after which native editing is admitted again.
    std::uint64_t reopenedRevision = 0;
    std::unique_ptr<plugins::SharedHistory::AppliedTransaction> recoveredApplied;
    Check(quarantineJournal->Revision(reopenedRevision) == MMS_OK &&
        quarantineJournal->ReserveAppliedNative(isolatedEditLabel, isolatedEditBytes, recoveredApplied) == MMS_OK &&
        recoveredApplied && quarantineJournal->CheckNativeEdit() == MMS_E_BUSY,
        "the recovered scene's real shared journal accepts the same applied reservation and holds its gate through the handle");
    recoveredApplied.reset();
    std::uint64_t releasedRevision = 0;
    Check(quarantineJournal->Revision(releasedRevision) == MMS_OK && releasedRevision == reopenedRevision &&
        !quarantineJournal->UndoCount() && !quarantineJournal->RedoCount() &&
        quarantineJournal->CheckNativeEdit() == MMS_OK,
        "releasing the uncommitted applied handle restores native editing without publishing history");
#endif
    Check(SaveSceneCopy(app, recoveredTarget.c_str()),
        "the recovered scene saves again through the real writer once the quarantine is cleared");
    const auto recoveredBytes = Bytes(recoveredTarget);
    Check(recoveredBytes.size() > 30 && !std::memcmp(recoveredBytes.data(), "Polygon Movie maker 0002", 24),
        "the recovered save writes a complete ordinary V2 project");
    // The same main-window messages now reach their real handlers: the
    // coordinate-axis command executes and the modified move and press run.
    Check(IsWindow(window) && !app->sceneRecoveryPending,
        "the recovered scene leaves the main-window input gate");
    const auto recoveredGrid = app->GroundGridEnabled();
    const bool recoveredMenu = menuGrid(), recoveredBoxed = boxedGrid();
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(215, 0), 0);
    Check(app->GroundGridEnabled() != recoveredGrid && menuGrid() != recoveredMenu &&
        boxedGrid() != recoveredBoxed,
        "the recovered scene executes the coordinate-axis menu command dropped during the quarantine");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(215, 1), 0);
    Check(app->GroundGridEnabled() == recoveredGrid && menuGrid() == recoveredMenu &&
        boxedGrid() == recoveredBoxed,
        "the recovered scene executes the same accelerator and restores the coordinate-axis state");
    const unsigned recoveredMoves = mouseMoves;
    SendMessageW(window, WM_MOUSEMOVE, MK_SHIFT | MK_CONTROL, MAKELPARAM(321, 123));
    Check(mouseMoves == recoveredMoves + 1 && app->ShiftModifierState() == 3 &&
        app->CtrlModifierState() == 3,
        "the recovered scene passes the same modified move to HandleMouseMove and stores its modifiers");
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(321, 123));
    Check(GetCapture() == window,
        "the recovered scene passes the same press through the main window and takes capture");
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(321, 123));
    Check(GetCapture() != window, "the recovered scene releases the test press capture on mouse-up");
    // The ordinary loads below must accept the reopened fixture again: the
    // real reset through the gate already produced an unlocked recoverable
    // scene with no quarantine left.
    Normal(v2, camera, light); Normal(v1, camera, light);
    BufferedProjectCases(v2, v1, camera, light);
    // Cover a different V1 failure source here: mandatory EOF after the
    // replacement boundary, independent of missing-model cancellation.
    std::puts("STAGE real V1 mandatory short read with repeatedly refused recovery reset");
    const auto shortV1 = directory / L"quarantined-short-v1.pmm";
    Wire truncatedV1; truncatedV1.Header("0001");
    truncatedV1.Value(std::int32_t(640)); truncatedV1.Write(shortV1);
    const auto shortV1Bytes = Bytes(shortV1);
    const auto shortV2 = directory / L"recovery-throw-v2.pmm";
    Wire truncatedV2; truncatedV2.bytes = Bytes(v2); truncatedV2.bytes.resize(64); truncatedV2.Write(shortV2);
    std::puts("STAGE exceptions inside real V1/V2 short-read recovery, bounded retry and isolation");
    const auto savedBufferMode = pmm_io::bufferedReads;
    for (const auto& path : {shortV1, shortV2}) for (bool buffered : {false, true})
        for (bool unknown : {false, true}) for (bool persistent : {false, true}) {
            pmm_io::bufferedReads = buffered;
            recoveryThrowsUnknown = unknown; recoveryThrowsPersist = persistent;
            const auto before = Bytes(path); const auto tails = tailCalls, errors = messages;
            const auto throwNotices = postloadNotices;
            Arm(path, Fault::ReadResetThrow, false);
            bool loaded = true, escaped = false;
            try { loaded = LoadSceneFile(path.c_str()); }
            catch (...) { escaped = true; }
            Check(!escaped, "an exception from error-recovery reset cannot escape the project load boundary");
            Finish(loaded, false, persistent, false);
            Check(recoveryResetCalls == 2 && tailCalls == tails && messages == errors + 1 &&
                app->EnglishUI() == 2 && Bytes(path) == before &&
                app->sceneRecoveryPending == persistent &&
                app->CurrentFrame() == (persistent ? kQuarantineResidualFrame : 0),
                "reset exceptions preserve the original error and language, retry once, and either clear or isolate the scene");
            RecordPostload(postloadNotices - throwNotices == (persistent ? 1u : 0u) &&
                tailCalls == tails && messages == errors + 1,
                "only a persistently throwing final recovery retry ends with one final-failure notice; a successful retry stays silent and the original error count is unchanged");
            Normal(path == shortV1 ? v1 : v2, camera, light);
            Check(!app->sceneRecoveryPending, "a subsequent real reset and open recovers from throwing cleanup");
        }
    pmm_io::bufferedReads = savedBufferMode;
    const auto v1Tails = tailCalls, v1Messages = messages, v1Dialogs = missingDialogs;
    const auto v1ShortNotices = postloadNotices;
    Arm(shortV1, Fault::ReadResetQuarantine, false);
    const bool v1Failed = LoadSceneFile(shortV1.c_str());
    Finish(v1Failed, false, true, false);
    Check(!v1Failed && quarantineRefusals == 2 && messages == v1Messages + 1 &&
        missingDialogs == v1Dialogs && tailCalls == v1Tails && Bytes(shortV1) == shortV1Bytes,
        "V1 mandatory EOF reports one incomplete-file error and bounded recovery refuses twice without publishing success");
    RecordPostload(postloadNotices - v1ShortNotices == 1,
        "the double-refused V1 short-read load ends with exactly one final-recovery notice");
    const auto v1Save = directory / L"quarantined-v1-must-not-save.pmm";
    Check(!fs::exists(v1Save) && !SaveSceneCopy(app, v1Save.c_str()) && !fs::exists(v1Save),
        "the real save writer refuses the quarantined V1 scene before creating its target");
    const auto v1Notices = recoveryGateNotices;
    const auto v1NativeNotices = recoveryGateNativeNotices;
    monitor = true;
    const bool v1Bypass = LoadSceneFile(v1.c_str());
    monitor = false;
    recoveryGateMonitorNotices[1] = recoveryGateNotices - v1Notices;
    recoveryGateMonitorNativeNotices[1] = recoveryGateNativeNotices - v1NativeNotices;
    Check(!v1Bypass && quarantineRefusals == 3 && app->sceneRecoveryPending &&
        inputCloses == 1 && tailCalls == v1Tails && messages == v1Messages + 1,
        "a normal V1 reopen cannot bypass the still-refused recovery gate");
    Normal(v1, camera, light);
    Check(!app->sceneRecoveryPending && SaveSceneCopy(app, v1Save.c_str()) && fs::exists(v1Save),
        "a real successful reset and V1 reopen clears isolation and restores saving");
    Normal(v2, camera, light);
    // The V1 accessory retry failure: the real first load cannot find the
    // fixture's malformed X source, the picker returns a second malformed X,
    // and the user explicitly aborts at the next recovery prompt. The loader closes its sole stream and the one-shot refused reset raises
    // SceneResetRefused. The shell then cancels the prepared transaction and
    // forwards the real reset, which must clear every residual slot and the
    // planted playhead. Both stream modes are exercised with the same real body.
    std::puts("STAGE real V1 accessory retry failure with one-shot post-close Reset false and real recovery");
    const auto accessoryProject = directory / L"accessory-reset-false-v1.pmm";
    accessorySourcePath = directory / L"malformed-accessory-source.x";
    accessorySubstitutePath = directory / L"malformed-accessory-replacement.x";
    for (const auto& malformed : {accessorySourcePath, accessorySubstitutePath}) {
        std::ofstream stream(malformed, std::ios::binary);
        stream << "not an X mesh";
        Check(bool(stream), "write an isolated malformed X for real parser rejection");
    }
    Check(fs::is_regular_file(accessorySourcePath) && fs::is_regular_file(accessorySubstitutePath),
        "the source and replacement are real files inside the isolated directory");
    WriteV1AccessoryResetFalse(accessoryProject);
    const auto accessoryBytes = Bytes(accessoryProject);
    Check(accessoryBytes.size() == 693 && accessoryBytes[235] == 1,
        "the accessory regression fixture keeps the real V1 mandatory prefix and stops after its source path");
    const auto priorAccessoryLanguage = app->EnglishUI();
    app->EnglishUI() = 1;
    const auto savedAccessoryBuffered = pmm_io::bufferedReads;
    const auto accessoryOneShotNotices = postloadNotices;
    for (const bool buffered : {false, true}) {
        pmm_io::bufferedReads = buffered;
        const auto tails = tailCalls, errors = messages;
        Arm(accessoryProject, Fault::AccessoryResetFalse, false);
        const bool accessoryLoaded = LoadSceneFile(accessoryProject.c_str());
        Check(!accessoryLoaded && tailCalls == tails && messages == errors &&
            accessoryDialogs == 2 && accessoryPicks == 1 && accessoryLoads == 2 &&
            accessoryLoadFailures == 2 && accessoryResetRefusals == 1 &&
            accessoryRecoveryForwards == 1 && !app->sceneRecoveryPending &&
            app->EnglishUI() == 1 && Bytes(accessoryProject) == accessoryBytes,
            "the real V1 accessory retry path shows exactly two recovery prompts, selects once, loads twice, refuses one reset and never reaches the success tail or rewrites its source");
        Check(app->CurrentFrame() == 0 && !app->sceneRecoveryPending,
            "the forwarded real reset clears the planted playhead sentinel and the recovery flag");
        unsigned residualAccessoryModels = 0, residualAccessories = 0;
        for (unsigned slot = 0; slot < 255; ++slot) {
            if (app->ModelSlot(slot)) ++residualAccessoryModels;
            if (app->AccessorySlot(slot)) ++residualAccessories;
        }
        Check(residualAccessoryModels == 0 && residualAccessories == 0,
            "the forwarded real reset releases all 255 model and accessory slots");
        Finish(accessoryLoaded, false);
        Normal(v1, camera, light);
        Check(!app->sceneRecoveryPending && tailCalls == tails + 1 && messages == errors &&
            Bytes(accessoryProject) == accessoryBytes,
            "the same stream mode loads the legal V1 fixture normally after the refused accessory reset");
    }
    pmm_io::bufferedReads = savedAccessoryBuffered;
    RecordPostload(postloadNotices == accessoryOneShotNotices,
        "the one-shot V1 accessory reset false is forwarded to a real reset and shows no final-failure notice in either stream mode");
    app->EnglishUI() = priorAccessoryLanguage;

    // The same real V1 accessory retry with a persistent refusal. The actual
    // loader's reset, the shell's post-replacement retry and the
    // pending-recovery gate before the next open all refuse, so the replaced
    // scene stays quarantined with its real slot-0 accessory. The real save
    // writer and the next open must refuse without touching the half scene, and
    // only a later real reset may release it. Each stream mode owns a different
    // absent save target so the two runs cannot interfere.
    std::puts("STAGE real V1 accessory retry failure with persistently refused resets and real recovery");
    const auto quarantineLanguage = app->EnglishUI();
    const auto quarantineBuffered = pmm_io::bufferedReads;
    app->EnglishUI() = 1;
    for (const bool buffered : {false, true}) {
        pmm_io::bufferedReads = buffered;
        const auto accessoryPersistNotices = postloadNotices;
        const fs::path quarantineTarget = directory / (buffered
            ? L"accessory-quarantine-buffered.pmm" : L"accessory-quarantine-direct.pmm");
        const fs::path recoveredAccessoryTarget = directory / (buffered
            ? L"accessory-recovered-buffered.pmm" : L"accessory-recovered-direct.pmm");
        Check(!fs::exists(quarantineTarget) && !fs::exists(recoveredAccessoryTarget),
            "each stream mode owns genuinely absent accessory save targets");
        const auto tails = tailCalls, errors = messages;
        Arm(accessoryProject, Fault::AccessoryResetFalse, false);
        accessoryResetPersist = true;
        const bool accessoryQuarantinedLoad = LoadSceneFile(accessoryProject.c_str());
        void* const retainedAccessory = static_cast<void*>(app->AccessorySlot(0));
        std::fprintf(stderr, "DIAG PMM persistent accessory refusal loaded=%u refusals=%u forwards=%u frame=%d dialogs=%u loads=%u picks=%u\n",
            unsigned(accessoryQuarantinedLoad), accessoryResetRefusals, accessoryRecoveryForwards,
            int(app->CurrentFrame()), accessoryDialogs, accessoryLoads, accessoryPicks);
        Check(!accessoryQuarantinedLoad && !tailObserved && tailCalls == tails &&
            messages == errors && accessoryDialogs == 2 && accessoryPicks == 1 &&
            accessoryLoads == 2 && accessoryLoadFailures == 2 &&
            accessoryResetRefusals == 2 && accessoryRecoveryForwards == 0 &&
            app->EnglishUI() == 1 && Bytes(accessoryProject) == accessoryBytes &&
            retainedAccessory != nullptr && retainedAccessory == accessoryRetainedSlot &&
            app->sceneRecoveryPending && app->CurrentFrame() == kAccessoryResidualFrame,
            "the actual accessory reset and the shell retry both refuse, leaving exactly two refusals, two recovery prompts, one selection, two failed real loads and the real slot-0 accessory under the 4343 playhead");
        RecordPostload(postloadNotices - accessoryPersistNotices == 1 &&
            accessoryResetRefusals == 2 && messages == errors,
            "the persistently refused V1 accessory load ends with exactly one final-recovery notice while the old dialog, selection and error counts stay unchanged");
        Finish(accessoryQuarantinedLoad, false, true, false, true);
        Check(static_cast<void*>(app->AccessorySlot(0)) == retainedAccessory,
            "the quarantined shell return keeps the exact slot-0 accessory identity");
        Check(!fs::exists(quarantineTarget) && !SaveSceneCopy(app, quarantineTarget.c_str()) &&
            !fs::exists(quarantineTarget),
            "the real save writer refuses the accessory-quarantined scene before creating its absent target");
        Check(static_cast<void*>(app->AccessorySlot(0)) == retainedAccessory &&
            app->sceneRecoveryPending && app->CurrentFrame() == kAccessoryResidualFrame,
            "the refused save leaves the quarantine, the sentinel playhead and the slot-0 identity unchanged");
        const auto reads = inputReads;
        const auto gateNotices = recoveryGateNotices;
        const auto gateNativeNotices = recoveryGateNativeNotices;
        monitor = true;
        const bool gateRefused = LoadSceneFile(v1.c_str());
        monitor = false;
        recoveryGateMonitorNotices[2] = recoveryGateNotices - gateNotices;
        recoveryGateMonitorNativeNotices[2] = recoveryGateNativeNotices - gateNativeNotices;
        Check(!gateRefused && accessoryResetRefusals == 3 && accessoryRecoveryForwards == 0 &&
            app->sceneRecoveryPending && app->CurrentFrame() == kAccessoryResidualFrame &&
            static_cast<void*>(app->AccessorySlot(0)) == retainedAccessory &&
            accessoryLoads == 2 && accessoryPicks == 1 && accessoryDialogs == 2 &&
            inputCloses == 1 && !inputOpen && inputReads == reads && tailCalls == tails &&
            messages == errors,
            "the valid V1 reopen refuses at the pending-recovery gate without reading input, loading an accessory, selecting, showing a repeated original error or model-selection prompt, clearing the half scene or reaching the success tail");
        Check(!app->EnvFileName()[0] && !app->PathWorkspace().projectDirectory[0],
            "the refused reopen never adopts the quarantined scene or a new project identity");
        // Drop the injection: the next load's pending-recovery gate runs the
        // real reset, then parses the legal V1 fixture in the same stream mode.
        accessoryResetPersist = false;
        Normal(v1, camera, light);
        Check(!app->sceneRecoveryPending && app->CurrentFrame() == 12 &&
            tailCalls == tails + 1 && Bytes(accessoryProject) == accessoryBytes,
            "the real reset and V1 open clear the isolation, restore frame 12 and leave the refused accessory source untouched");
        unsigned recoveredModels = 0, recoveredAccessories = 0;
        for (unsigned slot = 0; slot < 255; ++slot) {
            if (app->ModelSlot(slot)) ++recoveredModels;
            if (app->AccessorySlot(slot)) ++recoveredAccessories;
        }
        Check(recoveredModels == 0 && recoveredAccessories == 0,
            "the real reset releases every model and accessory slot after the persistent refusal");
#if !MMD_ORIGINAL_PROFILE
        auto* restoredHistory = plugins::NativeHistory(app);
        Check(restoredHistory && restoredHistory->CheckNativeEdit() == MMS_OK,
            "the real reset restores the shared journal gate after the persistent refusal");
#endif
        Check(!fs::exists(recoveredAccessoryTarget) &&
            SaveSceneCopy(app, recoveredAccessoryTarget.c_str()) &&
            fs::exists(recoveredAccessoryTarget),
            "the recovered scene saves to its own fresh independent target");
        Normal(recoveredAccessoryTarget, camera, light);
        Check(!app->sceneRecoveryPending && Bytes(accessoryProject) == accessoryBytes,
            "the recovered save reopens cleanly while the repeatedly refused accessory source keeps its exact bytes");
    }
    pmm_io::bufferedReads = quarantineBuffered;
    app->EnglishUI() = quarantineLanguage;
    // Real pending-recovery gate before a new open. The defect under regression
    // is LoadSceneFile's sceneRecoveryPending branch: a refused or throwing
    // ResetAppState only logs and returns false, so it neither restores the
    // caller language nor shows the fixed recovery notice. Every language and
    // failure action is accumulated into one bool so the unfixed production
    // still runs all twelve cases in one RED pass; the real Forward control
    // keeps the ordinary strict Normal assertions.
    std::puts("STAGE real pending-recovery gate notice and untouched quarantine before a reopen");
    {
        const auto gateSectionLanguage = app->EnglishUI();
        const auto gateShortBytes = Bytes(shortV1);
        const auto gateV1Bytes = Bytes(v1);
        recoveryGateMode = false;
        recoveryGateReset = RecoveryGateReset::Disabled;
        recoveryGateReopenPath = v1;
        const RecoveryGateReset failureActions[] = {
            RecoveryGateReset::False, RecoveryGateReset::Runtime,
            RecoveryGateReset::Unknown, RecoveryGateReset::StickyTrue};
        const char* const failureNames[] = {"false", "runtime", "unknown", "sticky-true"};
        bool gateOk = true;
        for (unsigned language = 0; language < 3; ++language) {
            for (unsigned actionIndex = 0; actionIndex < 4; ++actionIndex) {
                recoveryGateMode = false;
                recoveryGateReset = RecoveryGateReset::Disabled;
                Normal(v1, camera, light);
                app->EnglishUI() = 2;
                EnterNewSceneQuarantine(shortV1, "pending-gate");
                app->EnglishUI() = static_cast<unsigned char>(language);
                const auto generation = app->SceneGeneration();
                const auto revision = app->SceneRevision();
                const auto frame = app->CurrentFrame();
                // The quarantined short-read scene may fail before its key
                // pools exist, so snapshot the raw pointers first and never
                // dereference a null pool. The pointers themselves are part of
                // the isolation payload check below.
                mdl::CameraKey* const cameraKeys = app->CameraKeys();
                mdl::LightKey* const lightKeys = app->LightKeys();
                std::fprintf(stderr,
                    "DIAG recovery-gate snapshot language=%u action=%s cameraKeys=%p lightKeys=%p\n",
                    language, failureNames[actionIndex],
                    static_cast<void*>(cameraKeys), static_cast<void*>(lightKeys));
                mdl::CameraKey cameraKey{};
                mdl::LightKey lightKey{};
                if (cameraKeys) std::memcpy(&cameraKey, cameraKeys, sizeof(cameraKey));
                if (lightKeys) std::memcpy(&lightKey, lightKeys, sizeof(lightKey));
                const std::uint8_t dirty = app->SceneModified();
                const std::uint8_t layoutReady = app->WindowLayoutReady();
                wchar_t identityFile[256]{};
                wchar_t identityDirectory[256]{};
                std::memcpy(identityFile, app->EnvFileName(), sizeof(identityFile));
                std::memcpy(identityDirectory, app->PathWorkspace().projectDirectory,
                    sizeof(identityDirectory));
                const auto readsBefore = inputReads;
                const auto closesBefore = inputCloses;
                const auto tailsBefore = tailCalls;
                const auto messagesBefore = messages;
                const auto missingBefore = missingDialogs;
                const auto noticesBefore = recoveryGateNotices;
                const auto nativesBefore = recoveryGateNativeNotices;
                wchar_t titleBefore[256]{};
                GetWindowTextW(app->MainWindow(), titleBefore, 256);
                ArmRecoveryGate(failureActions[actionIndex], int(language));
                // Keep the existing CRT observers live exactly like the monitor
                // bypass below: a real candidate open or read is counted, never
                // hidden behind a disabled observer.
                candidate = v1; monitor = true;
                bool loaded = true, escaped = false;
                try { loaded = LoadSceneFile(v1.c_str()); }
                catch (...) { escaped = true; }
                monitor = false;
                DisarmRecoveryGate();
                const fs::path refusedTarget = directory /
                    (std::wstring(L"recovery-gate-refused-") + std::to_wstring(language) +
                     L"-" + std::to_wstring(actionIndex) + L".pmm");
                const bool saveRefused = !fs::exists(refusedTarget) &&
                    !SaveSceneCopy(app, refusedTarget.c_str()) && !fs::exists(refusedTarget);
                const auto resets = recoveryGateResets;
                const auto notices = recoveryGateNotices - noticesBefore;
                const auto natives = recoveryGateNativeNotices - nativesBefore;
                const auto readDelta = inputReads - readsBefore;
                const auto closeDelta = inputCloses - closesBefore;
                const auto tailDelta = tailCalls - tailsBefore;
                wchar_t titleAfter[256]{};
                GetWindowTextW(app->MainWindow(), titleAfter, 256);
                const bool identityIntact =
                    !std::memcmp(app->EnvFileName(), identityFile, sizeof(identityFile)) &&
                    !std::memcmp(app->PathWorkspace().projectDirectory, identityDirectory,
                        sizeof(identityDirectory)) &&
                    app->WindowLayoutReady() == layoutReady &&
                    !std::wcscmp(titleAfter, titleBefore) &&
                    !std::wcscmp(titleAfter, L"MikuMikuDance") &&
                    app->SceneGeneration() == generation &&
                    app->SceneRevision() == revision &&
                    app->SceneModified() == dirty;
                const bool payloadIntact = app->CurrentFrame() == frame &&
                    app->CameraKeys() == cameraKeys && app->LightKeys() == lightKeys &&
                    (!cameraKeys || std::memcmp(cameraKeys, &cameraKey, sizeof(cameraKey)) == 0) &&
                    (!lightKeys || std::memcmp(lightKeys, &lightKey, sizeof(lightKey)) == 0);
                const bool nestedBlocked = recoveryGateNestedRuns == 1 &&
                    recoveryGateNestedBlocked;
                const bool ok = !escaped && !loaded && resets == 1 &&
                    notices == 1 && natives == 1 &&
                    unsigned(app->EnglishUI()) == language &&
                    recoveryGateLanguageBefore == language &&
                    recoveryGateActualApp && app->sceneRecoveryPending &&
                    recoveryGateNoticeActualLanguage == int(language) &&
                    recoveryGateNoticeTextOk && recoveryGateNoticeFlagsOk &&
                    recoveryGateActualOwner && recoveryGateActualPending &&
                    recoveryGateActualInputClosed &&
                    identityIntact && payloadIntact && readDelta == 0 &&
                    closeDelta == 0 && tailDelta == 0 &&
                    messages == messagesBefore && missingDialogs == missingBefore &&
                    Bytes(shortV1) == gateShortBytes && Bytes(v1) == gateV1Bytes &&
                    nestedBlocked && saveRefused;
                std::fprintf(stderr,
                    "DIAG recovery-gate language=%u action=%s escaped=%u loaded=%u resets=%u "
                    "notices=%u nativeNotices=%u languageAfter=%d pending=%d "
                    "identityIntact=%u payloadIntact=%u readDelta=%llu tailDelta=%u nestedBlocked=%u\n",
                    language, failureNames[actionIndex], unsigned(escaped), unsigned(loaded),
                    resets, notices, natives, int(app->EnglishUI()),
                    int(app->sceneRecoveryPending), unsigned(identityIntact),
                    unsigned(payloadIntact), static_cast<unsigned long long>(readDelta),
                    tailDelta, unsigned(nestedBlocked));
                if (!ok) {
                    std::fprintf(stderr,
                        "DIAG recovery-gate detail language=%u action=%s languageBefore=%u "
                        "actualApp=%u noticeLanguage=%d textOk=%u flagsOk=%u ownerOk=%u "
                        "pendingOk=%u inputClosed=%u closeDelta=%u messages=%u/%u "
                        "missing=%u/%u nestedRuns=%u saveRefused=%u\n",
                        language, failureNames[actionIndex], recoveryGateLanguageBefore,
                        unsigned(recoveryGateActualApp), recoveryGateNoticeActualLanguage,
                        unsigned(recoveryGateNoticeTextOk), unsigned(recoveryGateNoticeFlagsOk),
                        unsigned(recoveryGateActualOwner), unsigned(recoveryGateActualPending),
                        unsigned(recoveryGateActualInputClosed), closeDelta, messages,
                        messagesBefore, missingDialogs, missingBefore,
                        recoveryGateNestedRuns, unsigned(saveRefused));
                }
                gateOk = gateOk && ok;
                Normal(v1, camera, light);
            }
        }
        bool forwardOk[3] = {false, false, false};
        for (unsigned language = 0; language < 3; ++language) {
            recoveryGateMode = false;
            recoveryGateReset = RecoveryGateReset::Disabled;
            Normal(v1, camera, light);
            app->EnglishUI() = 2;
            EnterNewSceneQuarantine(shortV1, "pending-forward");
            app->EnglishUI() = static_cast<unsigned char>(language);
            const auto generation = app->SceneGeneration();
            const auto tailsBefore = tailCalls;
            const auto noticesBefore = recoveryGateNotices;
            const auto nativesBefore = recoveryGateNativeNotices;
            ArmRecoveryGate(RecoveryGateReset::Forward, -1);
            Normal(v1, camera, light);
            // Two separate real successful loads follow: the gated v1 open and
            // the private-target reopen. Each owns exactly one success tail.
            const bool resetTail = tailCalls == tailsBefore + 1;
            DisarmRecoveryGate();
            const auto tailsAfterReset = tailCalls;
            const fs::path forwardTarget = directory /
                (std::wstring(L"recovery-gate-forward-") + std::to_wstring(language) + L".pmm");
            const bool saved = !fs::exists(forwardTarget) &&
                SaveSceneCopy(app, forwardTarget.c_str()) && fs::exists(forwardTarget);
            if (saved) Normal(forwardTarget, camera, light);
            const bool reopenedTail = saved && tailCalls == tailsAfterReset + 1;
            const bool ok = recoveryGateResets == 1 &&
                recoveryGateNotices == noticesBefore &&
                recoveryGateNativeNotices == nativesBefore &&
                !app->sceneRecoveryPending && app->CurrentFrame() == 12 &&
                unsigned(app->EnglishUI()) == language &&
                recoveryGateLanguageBefore == language &&
                recoveryGateActualApp &&
                !std::wcscmp(app->EnvFileName(), forwardTarget.c_str()) &&
                app->SceneGeneration() != generation &&
                resetTail && reopenedTail && saved;
            forwardOk[language] = ok;
            std::fprintf(stderr,
                "DIAG recovery-gate forward language=%u resets=%u notices=%u "
                "nativeNotices=%u languageAfter=%d pending=%d frame=%d saved=%u "
                "resetTail=%u reopenedTail=%u\n",
                language, recoveryGateResets, recoveryGateNotices - noticesBefore,
                recoveryGateNativeNotices - nativesBefore, int(app->EnglishUI()),
                int(app->sceneRecoveryPending), int(app->CurrentFrame()), unsigned(saved),
                unsigned(resetTail), unsigned(reopenedTail));
        }
        recoveryGateMode = false;
        recoveryGateReset = RecoveryGateReset::Disabled;
        recoveryGateNativeMessage = false;
        Normal(v1, camera, light);
        app->EnglishUI() = gateSectionLanguage;
        std::fprintf(stderr,
            "DIAG recovery-gate summary failure=%u forward=%u/%u/%u monitorNotices=%u/%u/%u "
            "monitorNativeNotices=%u/%u/%u\n",
            unsigned(gateOk), unsigned(forwardOk[0]), unsigned(forwardOk[1]),
            unsigned(forwardOk[2]), recoveryGateMonitorNotices[0],
            recoveryGateMonitorNotices[1], recoveryGateMonitorNotices[2],
            recoveryGateMonitorNativeNotices[0], recoveryGateMonitorNativeNotices[1],
            recoveryGateMonitorNativeNotices[2]);
        Check(gateOk && forwardOk[0] && forwardOk[1] && forwardOk[2] &&
            recoveryGateMonitorNotices[0] == 1 && recoveryGateMonitorNotices[1] == 1 &&
            recoveryGateMonitorNotices[2] == 1 &&
            recoveryGateMonitorNativeNotices[0] == 1 &&
            recoveryGateMonitorNativeNotices[1] == 1 &&
            recoveryGateMonitorNativeNotices[2] == 1,
            "every language and failure action at the pending-recovery gate fails once through the existing reset boundary, restores the caller language, shows exactly one fixed recovery notice that a nested real load cannot re-enter, and leaves the quarantined identity, payload, source, old errors, read/tail counters and save gate untouched; the real Forward control completes the reset and stays saveable and reopenable, and each refusing monitor reopen shows exactly one recovery notice");
    }
    // Real post-load final-recovery feedback. The helper under regression is
    // RecoverSceneResetRefusal: after the irreversible replacement boundary a
    // refused, throwing or "true but still quarantined" retry only logged, and
    // the old callers treated it as a completed recovery. This mode calls the
    // actual helper directly from a genuine V1 short-read quarantine, observes
    // the production-selected final-failure notice through the real formatter
    // and MessageBoxW, and keeps every target difference in postloadOk so the
    // unfixed production still runs all twelve failure cases plus the three
    // forward controls in one RED pass.
    std::puts("STAGE real post-load final-recovery notice after a genuine V1 short-read quarantine");
    {
        const auto postloadSectionLanguage = app->EnglishUI();
        const auto postloadShortBytes = Bytes(shortV1);
        postloadReopenPath = v1;
        bool postloadForwardOk[3] = {false, false, false};
        const PostloadReset postloadFailures[] = {
            PostloadReset::False, PostloadReset::Runtime,
            PostloadReset::Unknown, PostloadReset::StickyTrue};
        const char* const postloadFailureNames[] = {"false", "runtime", "unknown", "sticky-true"};
        for (unsigned language = 0; language < 3; ++language) {
            for (unsigned actionIndex = 0; actionIndex < 4; ++actionIndex) {
                postloadMode = false;
                postloadReset = PostloadReset::Disabled;
                Normal(v1, camera, light);
                app->EnglishUI() = 2;
                EnterNewSceneQuarantine(shortV1, "postload-feedback");
                app->EnglishUI() = static_cast<unsigned char>(language);
                const auto generation = app->SceneGeneration();
                const auto revision = app->SceneRevision();
                const auto frame = app->CurrentFrame();
                mdl::CameraKey* const cameraKeys = app->CameraKeys();
                mdl::LightKey* const lightKeys = app->LightKeys();
                mdl::CameraKey cameraKey{};
                mdl::LightKey lightKey{};
                if (cameraKeys) std::memcpy(&cameraKey, cameraKeys, sizeof(cameraKey));
                if (lightKeys) std::memcpy(&lightKey, lightKeys, sizeof(lightKey));
                void* const modelSlot = static_cast<void*>(app->ModelSlot(0));
                void* const accessorySlot = static_cast<void*>(app->AccessorySlot(0));
                const std::uint8_t dirty = app->SceneModified();
                const std::uint8_t layout = app->WindowLayoutReady();
                wchar_t identityFile[256]{};
                wchar_t identityDirectory[256]{};
                std::memcpy(identityFile, app->EnvFileName(), sizeof(identityFile));
                std::memcpy(identityDirectory, app->PathWorkspace().projectDirectory,
                    sizeof(identityDirectory));
                const auto readsBefore = inputReads;
                const auto closesBefore = inputCloses;
                const auto tailsBefore = tailCalls;
                const auto messagesBefore = messages;
                const auto missingBefore = missingDialogs;
                const auto noticesBefore = postloadNotices;
                const auto nativesBefore = postloadNativeNotices;
                // The genuine short-read quarantine above already ran with the
                // old monitor fault. Switch it off before the direct helper call
                // so only the independent post-load mode can take the retried
                // reset.
                monitor = false; fault = Fault::None;
                ArmPostload(postloadFailures[actionIndex], int(language));
                bool recovered = true, escaped = false;
                try { recovered = RecoverSceneResetRefusal(app, false, false); }
                catch (...) { escaped = true; }
                DisarmPostload();
                const fs::path refusedTarget = directory /
                    (std::wstring(L"postload-refused-") + std::to_wstring(language) +
                     L"-" + std::to_wstring(actionIndex) + L".pmm");
                const bool saveRefused = !fs::exists(refusedTarget) &&
                    !SaveSceneCopy(app, refusedTarget.c_str()) && !fs::exists(refusedTarget);
                const auto noticeDelta = postloadNotices - noticesBefore;
                const auto nativeDelta = postloadNativeNotices - nativesBefore;
                const bool identityIntact =
                    !std::memcmp(app->EnvFileName(), identityFile, sizeof(identityFile)) &&
                    !std::memcmp(app->PathWorkspace().projectDirectory, identityDirectory,
                        sizeof(identityDirectory)) &&
                    app->WindowLayoutReady() == layout &&
                    app->SceneGeneration() == generation &&
                    app->SceneRevision() == revision &&
                    app->SceneModified() == dirty;
                const bool payloadIntact = app->CurrentFrame() == frame &&
                    app->CameraKeys() == cameraKeys && app->LightKeys() == lightKeys &&
                    (!cameraKeys || std::memcmp(cameraKeys, &cameraKey, sizeof(cameraKey)) == 0) &&
                    (!lightKeys || std::memcmp(lightKeys, &lightKey, sizeof(lightKey)) == 0) &&
                    static_cast<void*>(app->ModelSlot(0)) == modelSlot &&
                    static_cast<void*>(app->AccessorySlot(0)) == accessorySlot;
                const bool guardReleased = !app->SceneMutationInProgress();
                const bool nestedBlocked = postloadNestedRuns == 1 &&
                    !postloadNestedLoaded && !postloadNestedEscaped &&
                    postloadNestedBlocked && postloadNestedSceneIntact;
                const bool ok = !escaped && !recovered && postloadActualApp &&
                    postloadResets == 1 && postloadLanguageBefore == language &&
                    unsigned(app->EnglishUI()) == language &&
                    noticeDelta == 1 && nativeDelta == 1 &&
                    postloadNoticeActualLanguage == int(language) &&
                    postloadNoticeTextOk && postloadNoticeFlagsOk &&
                    postloadNoticeOwnerOk && postloadNoticePending &&
                    postloadNoticeInputClosed && postloadNoticeMutationGuard &&
                    app->sceneRecoveryPending && guardReleased &&
                    identityIntact && payloadIntact &&
                    inputReads == readsBefore && inputCloses == closesBefore &&
                    tailCalls == tailsBefore && messages == messagesBefore &&
                    missingDialogs == missingBefore && Bytes(shortV1) == postloadShortBytes &&
                    nestedBlocked && saveRefused;
                RecordPostload(ok,
                    "each post-load reset failure shows exactly one final-recovery notice for the caller language, keeps the quarantine payload and old counters, and refuses a nested open and a real save");
                std::fprintf(stderr,
                    "DIAG postload failure language=%u action=%s escaped=%u recovered=%u resets=%u "
                    "notices=%u nativeNotices=%u noticeLanguage=%d textOk=%u flagsOk=%u ownerOk=%u "
                    "pending=%u inputClosed=%u guard=%u guardReleased=%u "
                    "languageBefore=%u languageAfter=%d nestedRuns=%u nestedLoaded=%u "
                    "nestedEscaped=%u nestedBlocked=%u nestedIntact=%u nestedPending=%u "
                    "identityIntact=%u payloadIntact=%u readDelta=%llu closeDelta=%u "
                    "tailDelta=%u messages=%u/%u missing=%u/%u saveRefused=%u\n",
                    language, postloadFailureNames[actionIndex], unsigned(escaped),
                    unsigned(recovered), postloadResets, noticeDelta, nativeDelta,
                    postloadNoticeActualLanguage, unsigned(postloadNoticeTextOk),
                    unsigned(postloadNoticeFlagsOk), unsigned(postloadNoticeOwnerOk),
                    unsigned(postloadNoticePending), unsigned(postloadNoticeInputClosed),
                    unsigned(postloadNoticeMutationGuard), unsigned(guardReleased),
                    postloadLanguageBefore, int(app->EnglishUI()), postloadNestedRuns,
                    unsigned(postloadNestedLoaded), unsigned(postloadNestedEscaped),
                    unsigned(postloadNestedBlocked), unsigned(postloadNestedSceneIntact),
                    postloadNestedPendingNotices, unsigned(identityIntact),
                    unsigned(payloadIntact),
                    static_cast<unsigned long long>(inputReads - readsBefore),
                    inputCloses - closesBefore, tailCalls - tailsBefore,
                    messages, messagesBefore, missingDialogs, missingBefore,
                    unsigned(saveRefused));
                Normal(v1, camera, light);
            }
        }
        for (unsigned language = 0; language < 3; ++language) {
            postloadMode = false;
            postloadReset = PostloadReset::Disabled;
            Normal(v1, camera, light);
            app->EnglishUI() = 2;
            EnterNewSceneQuarantine(shortV1, "postload-forward");
            app->EnglishUI() = static_cast<unsigned char>(language);
            const auto generation = app->SceneGeneration();
            const auto tailsBefore = tailCalls;
            const auto noticesBefore = postloadNotices;
            const auto nativesBefore = postloadNativeNotices;
            monitor = false; fault = Fault::None;
            ArmPostload(PostloadReset::Forward, int(language));
            bool recovered = false, escaped = false;
            try { recovered = RecoverSceneResetRefusal(app, false, false); }
            catch (...) { escaped = true; }
            DisarmPostload();
            unsigned residualSlots = 0;
            for (unsigned slot = 0; slot < 255; ++slot)
                if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++residualSlots;
            const bool forwardReset = !escaped && recovered && postloadActualApp &&
                postloadResets == 1 && postloadLanguageBefore == language &&
                unsigned(app->EnglishUI()) == language &&
                postloadNotices == noticesBefore && postloadNativeNotices == nativesBefore &&
                !app->sceneRecoveryPending && app->CurrentFrame() == 0 &&
                app->SceneGeneration() != generation && residualSlots == 0 &&
                !app->SceneMutationInProgress() && tailCalls == tailsBefore;
            // A completed retry is not the previous scene restored. Prove only
            // that the host is usable again through a legal V1 load, a real save
            // copy and a reopen of that saved copy.
            Normal(v1, camera, light);
            const fs::path forwardTarget = directory /
                (std::wstring(L"postload-forward-") + std::to_wstring(language) + L".pmm");
            const bool saved = !fs::exists(forwardTarget) &&
                SaveSceneCopy(app, forwardTarget.c_str()) && fs::exists(forwardTarget);
            if (saved) Normal(forwardTarget, camera, light);
            const bool ok = forwardReset && saved && tailCalls == tailsBefore + 2;
            postloadForwardOk[language] = ok;
            if (ok) ++postloadForwardPasses;
            RecordPostload(ok,
                "the post-load Forward control really resets, clears the quarantine, keeps the caller language and shows no final-failure notice, and the host stays loadable, saveable and reopenable");
            std::fprintf(stderr,
                "DIAG postload forward language=%u escaped=%u recovered=%u resets=%u "
                "notices=%u nativeNotices=%u languageBefore=%u languageAfter=%d "
                "pending=%d frame=%d generation=%llu before=%llu slots=%u "
                "guardReleased=%u saved=%u tailDelta=%u\n",
                language, unsigned(escaped), unsigned(recovered), postloadResets,
                postloadNotices - noticesBefore, postloadNativeNotices - nativesBefore,
                postloadLanguageBefore, int(app->EnglishUI()), int(app->sceneRecoveryPending),
                int(app->CurrentFrame()),
                static_cast<unsigned long long>(app->SceneGeneration()),
                static_cast<unsigned long long>(generation), residualSlots,
                unsigned(!app->SceneMutationInProgress()), unsigned(saved),
                tailCalls - tailsBefore);
        }
        postloadMode = false;
        postloadReset = PostloadReset::Disabled;
        postloadNativeMessage = false;
        app->EnglishUI() = postloadSectionLanguage;
        std::fprintf(stderr,
            "DIAG postload summary ok=%u forward=%u/%u/%u totalNotices=%u totalNativeNotices=%u\n",
            unsigned(postloadOk), unsigned(postloadForwardOk[0]),
            unsigned(postloadForwardOk[1]), unsigned(postloadForwardOk[2]),
            postloadNotices, postloadNativeNotices);
    }
    // Real File > New (WM_COMMAND 204) failure recovery boundary. The defect
    // under regression is CmdResetState calling ResetAppState with no catch
    // (the old code only reported NewSceneRefused for a false return). The
    // regression drives the real MainWndProc entry on a genuinely quarantined
    // scene: a refused reset stays refused, and a throwing reset must not
    // escape the window procedure, must restore the caller language and must
    // keep the scene isolated.
    std::puts("STAGE real File > New failure recovery and exception boundary on a quarantined scene");
    const HWND newSceneWindow = app->MainWindow();
    Check(IsWindow(newSceneWindow) && !IsWindowVisible(newSceneWindow),
        "the File > New regression drives the real hidden main window");
    const auto newSceneLanguage = app->EnglishUI();
    const auto savedNewSceneBuffered = pmm_io::bufferedReads;
    const auto newSceneSourceBytes = Bytes(shortV1);
    for (const bool buffered : {false, true}) {
        const char* const mode = buffered ? "buffer" : "direct";
        pmm_io::bufferedReads = buffered;
        std::printf("STAGE real File > New recovery mode=%s from the V1 quarantine\n", mode);
        EnterNewSceneQuarantine(shortV1, mode);
        const auto generation = app->SceneGeneration();
        const auto tailsBefore = tailCalls;
        const auto messagesBefore = messages;
        const auto refusedTarget = directory / (buffered
            ? L"new-scene-quarantine-buffered.pmm" : L"new-scene-quarantine-direct.pmm");
        Check(!fs::exists(refusedTarget) && !SaveSceneCopy(app, refusedTarget.c_str()) &&
            !fs::exists(refusedTarget),
            "the quarantined scene's real save gate still refuses before the File > New commands");
        newSceneMode = true;

        // Cancel: the confirmation is the only prompt and ResetAppState is
        // never reached.
        PrepareNewSceneCommand(NewSceneReset::False, IDCANCEL);
        InvokeNewScene(mode, "cancel");
        Check(newSceneResetCalls == 0 && newSceneConfirmDialogs == 1 && newSceneRefusalDialogs == 0,
            "cancelling File > New never calls the recovery reset and shows exactly one confirmation");
        CheckNewSceneQuarantine(mode, shortV1, newSceneSourceBytes, generation, tailsBefore, messagesBefore);

        // Confirmed but refused: one attempt, one warning and no cleanup.
        PrepareNewSceneCommand(NewSceneReset::False, IDOK);
        InvokeNewScene(mode, "confirm-false");
        Check(newSceneResetCalls == 1 && newSceneConfirmDialogs == 1 && newSceneRefusalDialogs == 1,
            "a refused new-scene reset is attempted once and reported through one NewSceneRefused prompt");
        CheckNewSceneQuarantine(mode, shortV1, newSceneSourceBytes, generation, tailsBefore, messagesBefore);

        // Standard C++ exception: production recovery must catch it, restore
        // the caller language and keep the scene quarantined.
        PrepareNewSceneCommand(NewSceneReset::Runtime, IDOK);
        InvokeNewScene(mode, "confirm-runtime");
        Check(newSceneResetCalls == 1 && newSceneConfirmDialogs == 1 && newSceneRefusalDialogs == 1,
            "a standard exception from the new-scene reset is reported through one refusal prompt after one attempt");
        CheckNewSceneQuarantine(mode, shortV1, newSceneSourceBytes, generation, tailsBefore, messagesBefore);

        // Non-standard exception: same boundary, same recovery.
        PrepareNewSceneCommand(NewSceneReset::Unknown, IDOK);
        InvokeNewScene(mode, "confirm-nonstandard");
        Check(newSceneResetCalls == 1 && newSceneConfirmDialogs == 1 && newSceneRefusalDialogs == 1,
            "a non-standard exception from the new-scene reset is reported through one refusal prompt after one attempt");
        CheckNewSceneQuarantine(mode, shortV1, newSceneSourceBytes, generation, tailsBefore, messagesBefore);

        // Forward the real reset: the only completed File > New. It is a scene
        // reset, not a project open, so no success tail may run.
        PrepareNewSceneCommand(NewSceneReset::Forward, IDOK);
        InvokeNewScene(mode, "confirm-real-reset");
        Check(newSceneResetCalls == 1 && newSceneConfirmDialogs == 1 && newSceneRefusalDialogs == 0 &&
            !app->sceneRecoveryPending && app->CurrentFrame() == 0 &&
            app->SceneGeneration() != generation && !app->EnvFileName()[0] &&
            !tailObserved && tailCalls == tailsBefore && messages == messagesBefore &&
            !SceneSaveInProgress() && !SceneSaveBlocksEdits(),
            "one forwarded real reset completes File > New, clears the quarantine and frame with no project-open tail or extra prompt");
        unsigned newSceneSlots = 0;
        for (unsigned slot = 0; slot < 255; ++slot)
            if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++newSceneSlots;
        Check(newSceneSlots == 0,
            "the completed new-scene reset releases all 255 model and accessory slots");
#if !MMD_ORIGINAL_PROFILE
        auto* newSceneHistory = plugins::NativeHistory(app);
        Check(newSceneHistory && newSceneHistory->CheckNativeEdit() == MMS_OK,
            "the completed new-scene reset restores the real shared journal gate");
#endif
        newSceneMode = false;
        // A reset is not a project open; the normal load and its copy are
        // checked separately.
        Normal(v1, camera, light);
        Check(!app->sceneRecoveryPending && app->CurrentFrame() == 12 &&
            !std::wcscmp(app->EnvFileName(), v1.c_str()),
            "the real V1 load after the completed new-scene reset restores its own scene identity");
        const auto newSceneSaveTarget = directory / (buffered
            ? L"new-scene-recovered-buffered.pmm" : L"new-scene-recovered-direct.pmm");
        Check(!fs::exists(newSceneSaveTarget) && SaveSceneCopy(app, newSceneSaveTarget.c_str()) &&
            fs::exists(newSceneSaveTarget),
            "the scene reopened after the completed new-scene reset saves to its own independent target");
    }
    pmm_io::bufferedReads = savedNewSceneBuffered;

    // Healthy-scene control. This false refusal is not a quarantined failure:
    // the real reset never runs, so the loaded project identity, camera and
    // playhead must stay untouched. Cancelling never reaches the reset either.
    std::puts("STAGE real File > New refusal and cancel on a healthy V1 scene");
    newSceneMode = false;
    Normal(v1, camera, light);
    newSceneMode = true;
    const auto healthyGeneration = app->SceneGeneration();
    const auto healthyProjectDirectory = std::wstring(app->PathWorkspace().projectDirectory);
    const std::uint8_t healthyDirty = app->SceneModified();
    wchar_t healthyTitle[256]{};
    GetWindowTextW(app->MainWindow(), healthyTitle, 256);
    const auto healthyCamera = app->CameraKeys()[0];
    unsigned healthySlots = 0;
    for (unsigned slot = 0; slot < 255; ++slot)
        if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++healthySlots;
    const auto healthySceneIntact = [&] {
        Check(!app->sceneRecoveryPending && app->CurrentFrame() == 12 &&
            !std::wcscmp(app->EnvFileName(), v1.c_str()) &&
            app->SceneGeneration() == healthyGeneration &&
            std::wstring(app->PathWorkspace().projectDirectory) == healthyProjectDirectory &&
            app->SceneModified() == healthyDirty &&
            std::memcmp(&app->CameraKeys()[0], &healthyCamera, sizeof(healthyCamera)) == 0 &&
            !SceneSaveInProgress() && !SceneSaveBlocksEdits(),
            "a refused or cancelled File > New on a healthy scene keeps its identity, camera, playhead, dirty flag and save gate");
        wchar_t title[256]{};
        GetWindowTextW(app->MainWindow(), title, 256);
        Check(!std::wcscmp(title, healthyTitle),
            "a refused or cancelled File > New keeps the loaded scene window title");
        unsigned slots = 0;
        for (unsigned slot = 0; slot < 255; ++slot)
            if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++slots;
        Check(slots == healthySlots,
            "a refused or cancelled File > New keeps every native object slot");
    };
    PrepareNewSceneCommand(NewSceneReset::False, IDOK);
    InvokeNewScene("healthy", "confirm-false");
    Check(newSceneResetCalls == 1 && newSceneConfirmDialogs == 1 && newSceneRefusalDialogs == 1,
        "the healthy-scene reset false is reported once and is not consumed by any cleanup");
    healthySceneIntact();
    PrepareNewSceneCommand(NewSceneReset::False, IDCANCEL);
    InvokeNewScene("healthy", "cancel");
    Check(newSceneResetCalls == 0 && newSceneConfirmDialogs == 1 && newSceneRefusalDialogs == 0,
        "cancelling File > New on the healthy scene never calls the reset");
    healthySceneIntact();
    // Modal reentrancy and stale-confirmation boundary. Every case first
    // re-establishes the real healthy V1 fixture, then arms exactly one one-shot
    // callback that runs while the File > New confirmation or refusal is still
    // on the stack. The four new expectations are accumulated into one bool so
    // the unfixed production reports every actual difference in a single RED
    // run; setup and guard-release checks stay strict.
    std::puts("STAGE real File > New modal reentrancy and stale confirmation");
    bool newModalCaseOk[4] = {false, false, false, false};
    struct NewModalBaseline {
        std::uint64_t generation, revision;
        std::wstring projectDirectory;
        std::uint8_t dirty;
        mdl::CameraKey cameraKey;
        unsigned slots;
        NewModalBaseline()
            : generation(app->SceneGeneration()), revision(app->SceneRevision()),
              projectDirectory(app->PathWorkspace().projectDirectory),
              dirty(app->SceneModified()), cameraKey(app->CameraKeys()[0]), slots(0) {
            for (unsigned slot = 0; slot < 255; ++slot)
                if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++slots;
        }
    };
    const auto healthyModalIntact = [&](const NewModalBaseline& baseline) {
        unsigned slots = 0;
        for (unsigned slot = 0; slot < 255; ++slot)
            if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++slots;
        return !app->sceneRecoveryPending && app->CurrentFrame() == 12 &&
            !std::wcscmp(app->EnvFileName(), v1.c_str()) &&
            app->SceneGeneration() == baseline.generation &&
            app->SceneRevision() == baseline.revision &&
            std::wstring(app->PathWorkspace().projectDirectory) == baseline.projectDirectory &&
            app->SceneModified() == baseline.dirty && slots == baseline.slots &&
            !SceneSaveInProgress() && !SceneSaveBlocksEdits();
    };
    const auto healthyModalPayload = [&](const NewModalBaseline& baseline) {
        return std::memcmp(&app->CameraKeys()[0], &baseline.cameraKey,
            sizeof(baseline.cameraKey)) == 0;
    };

    // Case 1: the confirmation callback dispatches the real WM_COMMAND 204 entry
    // again. One command, one refused reset attempt and one refusal prompt.
    {
        newSceneMode = false;
        Normal(v1, camera, light);
        newSceneMode = true;
        const NewModalBaseline baseline;
        PrepareNewSceneCommand(NewSceneReset::False, IDOK);
        bool nestedEscaped = false;
        LRESULT nestedResult = 0;
        newSceneConfirmAction = [&] {
            try {
                nestedResult = MainWndProc(app->MainWindow(), WM_COMMAND,
                    MAKEWPARAM(204, 0), 0);
            } catch (...) { nestedEscaped = true; }
        };
        InvokeNewScene("healthy", "reentrant-confirm");
        const bool identity = healthyModalIntact(baseline);
        const bool payload = healthyModalPayload(baseline);
        newModalCaseOk[0] = newSceneConfirmDialogs == 1 && newSceneResetCalls == 1 &&
            newSceneRefusalDialogs == 1 && identity && payload && !nestedEscaped;
        std::fprintf(stderr,
            "DIAG new-modal case=1 reentrant-confirm confirms=%u resetCalls=%u refusals=%u "
            "generation=%llu revision=%llu identityIntact=%u payloadIntact=%u "
            "nestedEscaped=%u nestedResult=%lld\n",
            newSceneConfirmDialogs, newSceneResetCalls, newSceneRefusalDialogs,
            static_cast<unsigned long long>(app->SceneGeneration()),
            static_cast<unsigned long long>(app->SceneRevision()),
            unsigned(identity), unsigned(payload), unsigned(nestedEscaped),
            static_cast<long long>(nestedResult));
    }

    // Case 2: the refusal callback re-enters. The one-shot callback keeps the
    // test layer finite; the production command guard keeps the command single.
    {
        newSceneMode = false;
        Normal(v1, camera, light);
        newSceneMode = true;
        const NewModalBaseline baseline;
        PrepareNewSceneCommand(NewSceneReset::False, IDOK);
        bool nestedEscaped = false;
        LRESULT nestedResult = 0;
        newSceneRefusalAction = [&] {
            try {
                nestedResult = MainWndProc(app->MainWindow(), WM_COMMAND,
                    MAKEWPARAM(204, 0), 0);
            } catch (...) { nestedEscaped = true; }
        };
        InvokeNewScene("healthy", "reentrant-refusal");
        const bool identity = healthyModalIntact(baseline);
        const bool payload = healthyModalPayload(baseline);
        newModalCaseOk[1] = newSceneConfirmDialogs == 1 && newSceneResetCalls == 1 &&
            newSceneRefusalDialogs == 1 && identity && payload && !nestedEscaped;
        std::fprintf(stderr,
            "DIAG new-modal case=2 reentrant-refusal confirms=%u resetCalls=%u refusals=%u "
            "generation=%llu revision=%llu identityIntact=%u payloadIntact=%u "
            "nestedEscaped=%u nestedResult=%lld\n",
            newSceneConfirmDialogs, newSceneResetCalls, newSceneRefusalDialogs,
            static_cast<unsigned long long>(app->SceneGeneration()),
            static_cast<unsigned long long>(app->SceneRevision()),
            unsigned(identity), unsigned(payload), unsigned(nestedEscaped),
            static_cast<long long>(nestedResult));
    }

    // Case 3: the confirmation callback reloads a project through the normal
    // loader (newSceneMode is disabled only inside the callback so the real
    // loader hooks and reset run, then restored with language 2). The outer
    // confirmation is now stale: it must neither run the reset nor refuse, and
    // the loaded identity, frame 12 and camera payload must survive.
    {
        newSceneMode = false;
        Normal(v1, camera, light);
        newSceneMode = true;
        const NewModalBaseline baseline;
        PrepareNewSceneCommand(NewSceneReset::Forward, IDOK);
        std::uint64_t loadedGeneration = baseline.generation;
        std::uint64_t loadedRevision = baseline.revision;
        std::wstring loadedPath;
        std::wstring loadedProjectDirectory;
        std::int32_t loadedFrame = -1;
        mdl::CameraKey loadedCamera{};
        bool loadEscaped = false;
        newSceneConfirmAction = [&] {
            newSceneMode = false;
            try { Normal(v1, camera, light); } catch (...) { loadEscaped = true; }
            newSceneMode = true;
            app->EnglishUI() = 2;
            loadedGeneration = app->SceneGeneration();
            loadedRevision = app->SceneRevision();
            loadedPath = app->EnvFileName();
            loadedProjectDirectory = app->PathWorkspace().projectDirectory;
            loadedFrame = app->CurrentFrame();
            loadedCamera = app->CameraKeys()[0];
        };
        InvokeNewScene("healthy", "confirm-reload");
        const bool noReset = newSceneConfirmDialogs == 1 && newSceneResetCalls == 0 &&
            newSceneRefusalDialogs == 0;
        const bool identity = !app->sceneRecoveryPending &&
            app->SceneGeneration() == loadedGeneration &&
            app->SceneRevision() == loadedRevision &&
            std::wstring(app->EnvFileName()) == loadedPath &&
            std::wstring(app->PathWorkspace().projectDirectory) == loadedProjectDirectory &&
            app->CurrentFrame() == loadedFrame && loadedFrame == 12 &&
            loadedPath == v1.wstring();
        const bool payload = std::memcmp(&app->CameraKeys()[0], &loadedCamera,
            sizeof(loadedCamera)) == 0;
        newModalCaseOk[2] = noReset && identity && payload && !loadEscaped;
        std::fprintf(stderr,
            "DIAG new-modal case=3 confirm-reload confirms=%u resetCalls=%u refusals=%u "
            "generation=%llu revision=%llu identityIntact=%u payloadIntact=%u "
            "loadedGeneration=%llu loadedRevision=%llu loadedFrame=%d loadEscaped=%u\n",
            newSceneConfirmDialogs, newSceneResetCalls, newSceneRefusalDialogs,
            static_cast<unsigned long long>(app->SceneGeneration()),
            static_cast<unsigned long long>(app->SceneRevision()),
            unsigned(identity), unsigned(payload),
            static_cast<unsigned long long>(loadedGeneration),
            static_cast<unsigned long long>(loadedRevision), int(loadedFrame),
            unsigned(loadEscaped));
    }

    // Case 4: the confirmation callback edits the same loaded scene and dirties
    // it. This is a controlled modal-callback edit, not a synthetic mouse
    // gesture. The stale confirmation must not reset; generation stays, the
    // revision advances and the complete edited payload, dirty flag and identity
    // survive.
    {
        newSceneMode = false;
        Normal(v1, camera, light);
        newSceneMode = true;
        const NewModalBaseline baseline;
        PrepareNewSceneCommand(NewSceneReset::Forward, IDOK);
        std::uint64_t editedGeneration = baseline.generation;
        std::uint64_t editedRevision = baseline.revision;
        mdl::CameraKey editedCamera{};
        bool editRan = false;
        newSceneConfirmAction = [&] {
            app->CameraKeys()[0].distance = 54321.0f;
            app->CameraKeys()[0].fov = 777;
            app->SceneModified() = 1;
            editedGeneration = app->SceneGeneration();
            editedRevision = app->SceneRevision();
            editedCamera = app->CameraKeys()[0];
            editRan = true;
        };
        InvokeNewScene("healthy", "confirm-edit");
        const bool noReset = newSceneConfirmDialogs == 1 && newSceneResetCalls == 0 &&
            newSceneRefusalDialogs == 0;
        const bool identity = !app->sceneRecoveryPending &&
            app->SceneGeneration() == baseline.generation &&
            app->SceneGeneration() == editedGeneration &&
            std::wstring(app->EnvFileName()) == v1.wstring() &&
            std::wstring(app->PathWorkspace().projectDirectory) == baseline.projectDirectory &&
            app->CurrentFrame() == 12 && app->SceneModified() == 1;
        const bool payload = std::memcmp(&app->CameraKeys()[0], &editedCamera,
            sizeof(editedCamera)) == 0 && editedCamera.distance == 54321.0f &&
            editedCamera.fov == 777 && baseline.cameraKey.distance != editedCamera.distance &&
            editRan;
        const bool revision = editedRevision > baseline.revision &&
            app->SceneRevision() == editedRevision;
        newModalCaseOk[3] = noReset && identity && payload && revision;
        std::fprintf(stderr,
            "DIAG new-modal case=4 confirm-edit confirms=%u resetCalls=%u refusals=%u "
            "generation=%llu revision=%llu identityIntact=%u payloadIntact=%u "
            "generationBefore=%llu revisionBefore=%llu editedRevision=%llu editRan=%u\n",
            newSceneConfirmDialogs, newSceneResetCalls, newSceneRefusalDialogs,
            static_cast<unsigned long long>(app->SceneGeneration()),
            static_cast<unsigned long long>(app->SceneRevision()),
            unsigned(identity), unsigned(payload),
            static_cast<unsigned long long>(baseline.generation),
            static_cast<unsigned long long>(baseline.revision),
            static_cast<unsigned long long>(editedRevision), unsigned(editRan));
    }

    std::fprintf(stderr,
        "DIAG new-modal summary case1=%u case2=%u case3=%u case4=%u\n",
        unsigned(newModalCaseOk[0]), unsigned(newModalCaseOk[1]),
        unsigned(newModalCaseOk[2]), unsigned(newModalCaseOk[3]));
    Check(newModalCaseOk[0] && newModalCaseOk[1] && newModalCaseOk[2] &&
        newModalCaseOk[3],
        "File > New modal callbacks re-enter or change the scene: one command, and a stale confirmation neither resets nor refuses while the reloaded or edited scene survives");

    // Guard release: one independent command after the modal cases must still
    // complete a real reset, proving the whole-command guard is released.
    std::puts("STAGE real File > New guard release after modal reentrancy");
    newSceneMode = false;
    Normal(v1, camera, light);
    newSceneMode = true;
    const auto releaseGeneration = app->SceneGeneration();
    const auto releaseTails = tailCalls;
    PrepareNewSceneCommand(NewSceneReset::Forward, IDOK);
    InvokeNewScene("healthy", "confirm-real-reset-after-modal");
    Check(newSceneConfirmDialogs == 1 && newSceneResetCalls == 1 &&
        newSceneRefusalDialogs == 0 && !app->sceneRecoveryPending &&
        app->CurrentFrame() == 0 && app->SceneGeneration() != releaseGeneration &&
        !app->EnvFileName()[0] && tailCalls == releaseTails &&
        !SceneSaveInProgress() && !SceneSaveBlocksEdits(),
        "one independent forwarded File > New reset still completes and releases the whole-command guard after the modal reentrancy cases");
    unsigned releaseSlots = 0;
    for (unsigned slot = 0; slot < 255; ++slot)
        if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++releaseSlots;
    Check(releaseSlots == 0,
        "the post-reentrancy File > New reset releases every model and accessory slot");
    newSceneMode = false;
    newSceneReset = NewSceneReset::Disabled;
    app->EnglishUI() = newSceneLanguage;
    // Real legacy void entry contract: LoadSceneV1/LoadSceneV2 own only their
    // input, not the caller-prepared plugin load transaction or project
    // identity. The same final-recovery feedback must appear for a persistent
    // refusal, and the caller cancels its own transaction afterwards. These
    // checks do not reuse Finish: that helper assumes the shell already cleared
    // the project identity.
    std::puts("STAGE real legacy V2 caller persistent final-recovery refusal and forward control");
    {
        const auto legacyLanguage = app->EnglishUI();
        app->EnglishUI() = 1;
        Normal(v1, camera, light);
        const auto legacyBytes = Bytes(missing);
        Check(plugins::PrepareProjectLoad(app, missing.c_str()),
            "prepare the legacy V2 caller's real plugin load transaction");
        wcscpy_s(app->EnvFileName(), 256, missing.c_str());
        Arm(missing, Fault::AbortResetQuarantine, false);
        expectedQuarantineAbortDialogs = missingDialogs + 1;
        ResetPostloadObservation(1);
        postloadLegacyMode = true;
        const auto noticesBefore = postloadNotices;
        const auto nativesBefore = postloadNativeNotices;
        const auto tailsBefore = tailCalls;
        const auto messagesBefore = messages;
        const auto missingBefore = missingDialogs;
        postloadReopenPath = v1;
        int fd = -1;
        Check(_wsopen_s(&fd, missing.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IWRITE) == 0,
            "open the legacy PMM with the shell's read-only flags");
        char header[30]{};
        Check(_read(fd, header, sizeof header) == sizeof header &&
            !std::memcmp(header + 20, "0002", 4), "consume the actual legacy V2 header");
        LoadSceneV2(app, fd);
        postloadLegacyMode = false;
        plugins::CancelProjectLoad(app);
        monitor = false;
        const fs::path legacyTarget = directory / L"legacy-v2-quarantine-target.pmm";
        if (fs::exists(legacyTarget))
            Check(DeleteFileW(legacyTarget.c_str()) != FALSE,
                "remove only this stage's previous legacy V2 save target");
        void* const retainedModel = static_cast<void*>(app->ModelSlot(0));
        const bool ok = postloadNotices - noticesBefore == 1 &&
            postloadNativeNotices - nativesBefore == 1 &&
            postloadNoticeActualLanguage == 1 && postloadNoticeTextOk &&
            postloadNoticeFlagsOk && postloadNoticeOwnerOk && postloadNoticePending &&
            postloadNoticeInputClosed && postloadNoticeMutationGuard &&
            postloadNestedRuns == 1 && !postloadNestedLoaded &&
            !postloadNestedEscaped && postloadNestedBlocked && postloadNestedSceneIntact &&
            app->sceneRecoveryPending && app->CurrentFrame() == kQuarantineResidualFrame &&
            retainedModel != nullptr && !std::wcscmp(app->EnvFileName(), missing.c_str()) &&
            unsigned(app->EnglishUI()) == 1 && quarantineRefusals == 2 &&
            !tailObserved && tailCalls == tailsBefore &&
            inputDescriptor >= 0 && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            messages == messagesBefore && missingDialogs == missingBefore + 1 &&
            Bytes(missing) == legacyBytes && !app->SceneMutationInProgress() &&
            !fs::exists(legacyTarget) && !SaveSceneCopy(app, legacyTarget.c_str()) &&
            !fs::exists(legacyTarget);
        postloadLegacyV2Persist = ok;
        RecordPostload(ok,
            "the real legacy V2 persistent refusal leaves the caller-owned transaction for its caller to cancel, keeps the partial model slot, sentinel playhead and quarantine, closes input once, restores TLS, rewrites nothing and shows exactly one final-recovery notice that blocks a nested open");
        std::fprintf(stderr,
            "DIAG legacy-v2 persistent entry=LoadSceneV2 result=failed notices=%u nativeNotices=%u "
            "noticeLanguage=%d textOk=%u flagsOk=%u ownerOk=%u pending=%u inputClosed=%u guard=%u "
            "language=%u refusals=%u frame=%d retainedModel=%p envMatches=%u inputOpen=%u closes=%u "
            "checked=%d/%d tails=%u/%u messages=%u/%u missing=%u/%u sourceUnchanged=%u "
            "nested=%u/%u/%u/%u/%u saveRefused=%u\n",
            postloadNotices - noticesBefore, postloadNativeNotices - nativesBefore,
            postloadNoticeActualLanguage, unsigned(postloadNoticeTextOk),
            unsigned(postloadNoticeFlagsOk), unsigned(postloadNoticeOwnerOk),
            unsigned(postloadNoticePending), unsigned(postloadNoticeInputClosed),
            unsigned(postloadNoticeMutationGuard), app->EnglishUI(), quarantineRefusals,
            int(app->CurrentFrame()), retainedModel,
            unsigned(!std::wcscmp(app->EnvFileName(), missing.c_str())),
            unsigned(inputOpen), inputCloses, checkedAtClose, checkedBefore,
            tailCalls, tailsBefore, messages, messagesBefore, missingDialogs, missingBefore,
            unsigned(Bytes(missing) == legacyBytes), postloadNestedRuns,
            unsigned(postloadNestedLoaded), unsigned(postloadNestedEscaped),
            unsigned(postloadNestedBlocked), unsigned(postloadNestedSceneIntact),
            unsigned(!fs::exists(legacyTarget)));
        // The actual abort's one-shot refusal is retried once by the helper and
        // forwarded to the real reset. The caller still cancels its own
        // transaction and no final-failure notice may appear.
        expectedMissingAbortDialogs = missingDialogs + 1;
        Normal(v1, camera, light);
        Check(plugins::PrepareProjectLoad(app, missing.c_str()),
            "prepare the legacy V2 forward control's plugin transaction");
        wcscpy_s(app->EnvFileName(), 256, missing.c_str());
        Arm(missing, Fault::AbortResetFalse, false);
        ResetPostloadObservation(1);
        postloadLegacyMode = true;
        const auto forwardGeneration = app->SceneGeneration();
        const auto forwardNotices = postloadNotices;
        const auto forwardNatives = postloadNativeNotices;
        const auto forwardTails = tailCalls;
        fd = -1;
        Check(_wsopen_s(&fd, missing.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IWRITE) == 0,
            "open the legacy forward control PMM with the shell's read-only flags");
        char forwardHeader[30]{};
        Check(_read(fd, forwardHeader, sizeof forwardHeader) == sizeof forwardHeader &&
            !std::memcmp(forwardHeader + 20, "0002", 4),
            "consume the actual legacy V2 header for the forward control");
        LoadSceneV2(app, fd);
        postloadLegacyMode = false;
        plugins::CancelProjectLoad(app);
        monitor = false;
        unsigned forwardSlots = 0;
        for (unsigned slot = 0; slot < 255; ++slot)
            if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++forwardSlots;
        const bool forwardReset = !app->sceneRecoveryPending && app->CurrentFrame() == 0 &&
            app->SceneGeneration() != forwardGeneration && forwardSlots == 0 &&
            postloadNotices == forwardNotices && postloadNativeNotices == forwardNatives &&
            unsigned(app->EnglishUI()) == 1 && tailCalls == forwardTails &&
            inputDescriptor >= 0 && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            !app->SceneMutationInProgress();
        const fs::path forwardTarget = directory / L"legacy-v2-forward-target.pmm";
        if (fs::exists(forwardTarget))
            Check(DeleteFileW(forwardTarget.c_str()) != FALSE,
                "remove only this stage's previous legacy V2 forward target");
        Normal(v1, camera, light);
        const bool saved = !fs::exists(forwardTarget) &&
            SaveSceneCopy(app, forwardTarget.c_str()) && fs::exists(forwardTarget);
        if (saved) Normal(forwardTarget, camera, light);
        postloadLegacyV2Forward = forwardReset && saved && tailCalls == forwardTails + 2;
        RecordPostload(postloadLegacyV2Forward,
            "the real legacy V2 forward control completes the reset, clears the quarantine, keeps the caller language and shows no final-failure notice, and the host stays loadable, saveable and reopenable");
        std::fprintf(stderr,
            "DIAG legacy-v2 forward entry=LoadSceneV2 result=forward resets=1 notices=%u "
            "nativeNotices=%u pending=%d frame=%d generation=%llu before=%llu slots=%u "
            "language=%u tailDelta=%u inputOpen=%u closes=%u checked=%d/%d saved=%u\n",
            postloadNotices - forwardNotices, postloadNativeNotices - forwardNatives,
            int(app->sceneRecoveryPending), int(app->CurrentFrame()),
            static_cast<unsigned long long>(app->SceneGeneration()),
            static_cast<unsigned long long>(forwardGeneration), forwardSlots,
            app->EnglishUI(), tailCalls - forwardTails, unsigned(inputOpen), inputCloses,
            checkedAtClose, checkedBefore, unsigned(saved));
        app->EnglishUI() = legacyLanguage;
    }
    std::puts("STAGE real legacy V1 caller persistent final-recovery refusal and forward control");
    {
        const auto legacyLanguage = app->EnglishUI();
        app->EnglishUI() = 1;
        Normal(v1, camera, light);
        const auto legacyBytes = Bytes(accessoryProject);
        Check(fs::is_regular_file(accessorySourcePath) && fs::is_regular_file(accessorySubstitutePath),
            "both malformed X inputs remain available for the legacy V1 entry");
        Check(plugins::PrepareProjectLoad(app, accessoryProject.c_str()),
            "prepare the legacy V1 caller's real plugin load transaction");
        wcscpy_s(app->EnvFileName(), 256, accessoryProject.c_str());
        Arm(accessoryProject, Fault::AccessoryResetFalse, false);
        accessoryResetPersist = true;
        ResetPostloadObservation(1);
        postloadLegacyMode = true;
        const auto noticesBefore = postloadNotices;
        const auto nativesBefore = postloadNativeNotices;
        const auto tailsBefore = tailCalls;
        const auto messagesBefore = messages;
        const auto missingBefore = missingDialogs;
        postloadReopenPath = v1;
        int fd = -1;
        Check(_wsopen_s(&fd, accessoryProject.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IWRITE) == 0,
            "open the legacy accessory PMM with the shell's read-only flags");
        char header[30]{};
        Check(_read(fd, header, sizeof header) == sizeof header &&
            !std::memcmp(header + 20, "0001", 4), "consume the actual legacy V1 header");
        LoadSceneV1(app, fd);
        postloadLegacyMode = false;
        plugins::CancelProjectLoad(app);
        monitor = false;
        const fs::path legacyTarget = directory / L"legacy-v1-quarantine-target.pmm";
        if (fs::exists(legacyTarget))
            Check(DeleteFileW(legacyTarget.c_str()) != FALSE,
                "remove only this stage's previous legacy V1 save target");
        void* const retainedAccessory = static_cast<void*>(app->AccessorySlot(0));
        const bool saveRefused = !fs::exists(legacyTarget) &&
            !SaveSceneCopy(app, legacyTarget.c_str()) && !fs::exists(legacyTarget);
        const bool ok = postloadNotices - noticesBefore == 1 &&
            postloadNativeNotices - nativesBefore == 1 &&
            postloadNoticeActualLanguage == 1 && postloadNoticeTextOk &&
            postloadNoticeFlagsOk && postloadNoticeOwnerOk && postloadNoticePending &&
            postloadNoticeInputClosed && postloadNoticeMutationGuard &&
            postloadNestedRuns == 1 && !postloadNestedLoaded &&
            !postloadNestedEscaped && postloadNestedBlocked && postloadNestedSceneIntact &&
            app->sceneRecoveryPending && app->CurrentFrame() == kAccessoryResidualFrame &&
            retainedAccessory != nullptr &&
            !std::wcscmp(app->EnvFileName(), accessoryProject.c_str()) &&
            unsigned(app->EnglishUI()) == 1 && accessoryResetRefusals == 2 &&
            accessoryRecoveryForwards == 0 && accessoryLoads == 2 &&
            accessoryLoadFailures == 2 && accessoryPicks == 1 && accessoryDialogs == 2 &&
            !tailObserved && tailCalls == tailsBefore &&
            inputDescriptor >= 0 && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            messages == messagesBefore && missingDialogs == missingBefore &&
            Bytes(accessoryProject) == legacyBytes && !app->SceneMutationInProgress() &&
            saveRefused;
        postloadLegacyV1Persist = ok;
        RecordPostload(ok,
            "the real legacy V1 persistent refusal leaves the caller-owned transaction for its caller to cancel, keeps the real slot-0 accessory, sentinel playhead and quarantine, closes input once, restores TLS, shows exactly one final-recovery notice that blocks a nested open, and never reaches the success tail");
        std::fprintf(stderr,
            "DIAG legacy-v1 persistent entry=LoadSceneV1 result=failed notices=%u nativeNotices=%u "
            "noticeLanguage=%d textOk=%u flagsOk=%u ownerOk=%u pending=%u inputClosed=%u guard=%u "
            "language=%u refusals=%u forwards=%u loads=%u/%u picks=%u dialogs=%u frame=%d "
            "retainedAccessory=%p envMatches=%u inputOpen=%u closes=%u checked=%d/%d "
            "tails=%u/%u messages=%u/%u sourceUnchanged=%u nested=%u/%u/%u/%u/%u saveRefused=%u\n",
            postloadNotices - noticesBefore, postloadNativeNotices - nativesBefore,
            postloadNoticeActualLanguage, unsigned(postloadNoticeTextOk),
            unsigned(postloadNoticeFlagsOk), unsigned(postloadNoticeOwnerOk),
            unsigned(postloadNoticePending), unsigned(postloadNoticeInputClosed),
            unsigned(postloadNoticeMutationGuard), app->EnglishUI(), accessoryResetRefusals,
            accessoryRecoveryForwards, accessoryLoads, accessoryLoadFailures,
            accessoryPicks, accessoryDialogs, int(app->CurrentFrame()), retainedAccessory,
            unsigned(!std::wcscmp(app->EnvFileName(), accessoryProject.c_str())),
            unsigned(inputOpen), inputCloses, checkedAtClose, checkedBefore,
            tailCalls, tailsBefore, messages, messagesBefore,
            unsigned(Bytes(accessoryProject) == legacyBytes), postloadNestedRuns,
            unsigned(postloadNestedLoaded), unsigned(postloadNestedEscaped),
            unsigned(postloadNestedBlocked), unsigned(postloadNestedSceneIntact),
            unsigned(saveRefused));
        // One-shot control: the actual accessory reset refuses once, the helper
        // forwards the real reset, and the caller still cancels its own
        // transaction. No final-failure notice may appear.
        Normal(v1, camera, light);
        Check(plugins::PrepareProjectLoad(app, accessoryProject.c_str()),
            "prepare the legacy V1 forward control's plugin transaction");
        wcscpy_s(app->EnvFileName(), 256, accessoryProject.c_str());
        Arm(accessoryProject, Fault::AccessoryResetFalse, false);
        ResetPostloadObservation(1);
        postloadLegacyMode = true;
        const auto forwardGeneration = app->SceneGeneration();
        const auto forwardNotices = postloadNotices;
        const auto forwardNatives = postloadNativeNotices;
        const auto forwardTails = tailCalls;
        const auto forwardMessages = messages;
        fd = -1;
        Check(_wsopen_s(&fd, accessoryProject.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IWRITE) == 0,
            "open the legacy accessory forward control PMM with the shell's read-only flags");
        char forwardHeader[30]{};
        Check(_read(fd, forwardHeader, sizeof forwardHeader) == sizeof forwardHeader &&
            !std::memcmp(forwardHeader + 20, "0001", 4),
            "consume the actual legacy V1 header for the forward control");
        LoadSceneV1(app, fd);
        postloadLegacyMode = false;
        plugins::CancelProjectLoad(app);
        monitor = false;
        unsigned forwardSlots = 0;
        for (unsigned slot = 0; slot < 255; ++slot)
            if (app->ModelSlot(slot) || app->AccessorySlot(slot)) ++forwardSlots;
        const bool forwardReset = !app->sceneRecoveryPending && app->CurrentFrame() == 0 &&
            app->SceneGeneration() != forwardGeneration && forwardSlots == 0 &&
            postloadNotices == forwardNotices && postloadNativeNotices == forwardNatives &&
            unsigned(app->EnglishUI()) == 1 && tailCalls == forwardTails &&
            accessoryResetRefusals == 1 && accessoryRecoveryForwards == 1 &&
            messages == forwardMessages &&
            inputDescriptor >= 0 && !inputOpen && inputCloses == 1 &&
            checkedAtClose == checkedBefore && pmm_io::checkedDescriptor == checkedBefore &&
            !app->SceneMutationInProgress();
        const fs::path forwardTarget = directory / L"legacy-v1-forward-target.pmm";
        if (fs::exists(forwardTarget))
            Check(DeleteFileW(forwardTarget.c_str()) != FALSE,
                "remove only this stage's previous legacy V1 forward target");
        Normal(v1, camera, light);
        const bool saved = !fs::exists(forwardTarget) &&
            SaveSceneCopy(app, forwardTarget.c_str()) && fs::exists(forwardTarget);
        if (saved) Normal(forwardTarget, camera, light);
        postloadLegacyV1Forward = forwardReset && saved && tailCalls == forwardTails + 2;
        RecordPostload(postloadLegacyV1Forward,
            "the real legacy V1 forward control completes the reset, clears the quarantine, keeps the caller language and shows no final-failure notice, and the host stays loadable, saveable and reopenable");
        std::fprintf(stderr,
            "DIAG legacy-v1 forward entry=LoadSceneV1 result=forward notices=%u nativeNotices=%u "
            "refusals=%u forwards=%u pending=%d frame=%d generation=%llu before=%llu slots=%u "
            "language=%u tailDelta=%u messages=%u/%u inputOpen=%u closes=%u checked=%d/%d saved=%u\n",
            postloadNotices - forwardNotices, postloadNativeNotices - forwardNatives,
            accessoryResetRefusals, accessoryRecoveryForwards,
            int(app->sceneRecoveryPending), int(app->CurrentFrame()),
            static_cast<unsigned long long>(app->SceneGeneration()),
            static_cast<unsigned long long>(forwardGeneration), forwardSlots,
            app->EnglishUI(), tailCalls - forwardTails, messages, forwardMessages,
            unsigned(inputOpen), inputCloses, checkedAtClose, checkedBefore, unsigned(saved));
        app->EnglishUI() = legacyLanguage;
    }
    // Exit the real host while a failed replacement still owns a partial
    // model and detached controls. Do not clear the isolation flag or run a
    // successful reset first: WM_CLOSE, WM_DESTROY and the normal post-quit
    // teardown must all tolerate this state.
    std::puts("STAGE real quarantined main-host exit with partial model and detached controls");
    InitFlagSubsystem(app);
    const HWND exitFloating = app->FloatingWindow();
    const HWND exitMain = app->MainWindow();
    Check(IsWindow(exitFloating) && !IsWindowVisible(exitFloating),
        "the exit fixture owns a hidden production detached window");
    const auto exitAborts = missingDialogs;
    const auto exitNotices = postloadNotices;
    Arm(missing, Fault::AbortResetQuarantine, false);
    expectedQuarantineAbortDialogs = exitAborts + 1;
    const bool exitLoad = LoadSceneFile(missing.c_str());
    Finish(exitLoad, false, true);
    Check(quarantineRefusals == 2 && app->ModelSlot(0) &&
        app->FloatingWindow() == exitFloating && !app->SceneMutationInProgress(),
        "the real exit starts after both refused resets with live partial ownership");
    RecordPostload(postloadNotices - exitNotices == 1 && quarantineRefusals == 2,
        "the quarantined exit setup ends with exactly one final-recovery notice and two refused resets");
    std::fprintf(stderr,
        "DIAG postload summary ok=%u forward=%u legacyV2=%u/%u legacyV1=%u/%u notices=%u nativeNotices=%u oldMessages=%u missingDialogs=%u\n",
        unsigned(postloadOk), postloadForwardPasses,
        unsigned(postloadLegacyV2Persist), unsigned(postloadLegacyV2Forward),
        unsigned(postloadLegacyV1Persist), unsigned(postloadLegacyV1Forward),
        postloadNotices, postloadNativeNotices, messages, missingDialogs);
    Check(postloadOk,
        "the bounded final recovery shows exactly one fixed final-failure notice per genuine failure in every language and entry point, restores the caller language, retains the quarantine payload and old error counters, blocks a nested open and a real save until a real reset, and stays silent on every successful recovery");
    app->EnhancedModelDirty() = 0;
    app->SceneModified() = 1;
    closeDialogs = 0; testCloseAccept = true;
    const auto exitPanels = panelPaints;
    SendMessageW(exitMain, WM_CLOSE, 0, 0);
    testCloseAccept = false;
    Check(closeDialogs == 1 && !IsWindow(exitMain) && !IsWindow(exitFloating) &&
        !app->FloatingWindow() && panelPaints == exitPanels && app->sceneRecoveryPending,
        "accepted quarantined exit destroys both windows without painting partial model keys");
    MSG quit{};
    bool quitReceived = false;
    // WM_QUIT is synthesized after queued work drains; filtering for it alone
    // can leave old paint/wake messages ahead of it in this non-loop fixture.
    for (unsigned drained = 0; drained < 256 &&
        PeekMessageW(&quit, nullptr, 0, 0, PM_REMOVE); ++drained) {
        if (quit.message == WM_QUIT) { quitReceived = true; break; }
        TranslateMessage(&quit);
        DispatchMessageW(&quit);
    }
    std::fprintf(stderr, "DIAG quit posts=%u code=%d received=%d message=%u value=%llu\n",
        quitPosts, quitCode, int(quitReceived), unsigned(quit.message),
        static_cast<unsigned long long>(quit.wParam));
    Check(quitPosts == 1 && quitCode == 0 && quitReceived && quit.wParam == 0,
        "the real WM_DESTROY posts a successful quit before final cleanup");
    ShutdownCleanup(app);
    for (unsigned slot = 0; slot < 255; ++slot)
        Check(!app->ModelSlot(slot) && !app->AccessorySlot(slot),
            "normal post-quit cleanup releases every partial model and accessory slot");
    instance.reset(); app = nullptr; g_Block = nullptr;
    UnhookWindowsHookEx(hide); MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
#if !MMD_ORIGINAL_PROFILE
    if (pictureBudgetMode) {
        const auto settings = system_settings::Path();
        Check(DeleteFileW(settings.c_str()) && DeleteFileW((settings.wstring() + L".lock").c_str()),
              "remove only settings and lock created inside this explicitly isolated installation");
        std::puts("PASS real PMM picture asset budget and settings isolation");
    }
#endif
    std::printf("PASS real PMM input host: %u checks; native tails=%u; errors=%u; missing-model aborts=%u\n",
        checks, tailCalls, messages, missingDialogs);
    return 0;
}
