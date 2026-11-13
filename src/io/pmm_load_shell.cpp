#include "mikumikustudio/background_save.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "project_open.hpp"
// ===========================================================================
// VA 0x00408870 - PathToProjectDir  (original: sub_408870, 0xEF bytes)
// VA 0x00458F80 - LoadSceneFile    (original: sub_458F80, 0x589B bytes,
//                                    PHASE A: shell + version dispatch)
// ===========================================================================
// sub_408870 (thiscall, ecx = Destination wchar_t[0x100]): copies the scene
// path to a local, then truncates at the first L"UserFile" occurrence; if
// the result is empty or does not end in L'\\' the destination is cleared.
// Called from the PMM load shell (dst app+0x9F338, src app+0xA0900) and
// from FontSubInit 0x408E70.
//
// sub_458F80 shell (PHASE A - this file):
//   0x458FE6  _wsopen_s(&fd, app+0xA0900, 0x8000, 0x40, 0x80); on error
//             sprintf_s + MessageBoxA (JP 0x52CDD4/0x52DB80, EN
//             "Cannot open file:%d"/"open file").
//   0x459055  app+0xA442C = 0; PathToProjectDir(app+0x9F338, path).
//   0x459070  _read(fd, hdr, 0x1E); the magic "Polygon Movie maker" is matched
//             by the port via memcmp at header offset 0 (19 bytes). An
//             earlier note claimed the original used strstr; that was
//             never checked against the binary.  Defect B3-30.
//             (0x45908A: jnz continue - Ghidra prints the polarity
//             backwards); on miss MessageBoxA "This isn't the data for
//             Polygon Movie Maker!" / JP 0x52DF78 + _close + return.
//   0x4590D3  version field at hdr+0x14: "0001" -> inline v1 loader body
//             (0x45916D..0x45E7F7, fully ported as LoadSceneV1 in
//             pmm_load_v1.cpp; that body owns the fd and the _close on
//             every path.  Verified 2026-08-28: all three stock v1 scenes
//             load with bit-identical kinematic pose blocks (K hashes on
//             every slot), and v1-load -> save-as products are equivalent
//             through the orig round-trip (only orig's own stack-residue
//             nondeterminism remains, same class as the v2 verdict);
//             "0002" -> sub_450000(app, fd) which owns the rest of the
//             stream AND the _close + success tail (its epilogue at
//             0x45E7F7 only restores SEH state); anything else ->
//             "Cannot read this version of pmm!" box + _close.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <io.h>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <utility>

#include "mikumikustudio/mmd_app.hpp"
#include "pmm_io_common.hpp"
#include "pmm_input.hpp"
#include "project_relocation_context.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/effect_api.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/scene_ownership.hpp"
#include "mikumikustudio/texture_lookup.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "mikumikustudio/ui_language.hpp"

namespace mikumikustudio {

namespace project_relocation {
namespace {
std::vector<std::wstring> UnresolvedEffects(const Ticket& ticket, const wchar_t* project) {
    auto paths = MissingProjectEffects(project);
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const auto& path) {
        const auto mapped = ticket.Infer(path);
        std::error_code error;
        return mapped.status == Status::Candidate && std::filesystem::is_regular_file(mapped.path, error);
    }), paths.end());
    return paths;
}

bool ConfirmReplacement(MMDApp& app, HWND owner, const Ticket& ticket,
    const std::wstring& identity, const std::wstring& original, const std::wstring& selected) {
    // Both the picker and the compiler can dispatch work. Re-read the sidecar
    // from the frozen project path and preserve a reappearing original.
    const auto stillMissing = [&] {
        if (!ticket.Current(app) || identity != app.EnvFileName()) return false;
        const auto remaining = UnresolvedEffects(ticket, LoadScope::ProjectPath(app));
        return std::find(remaining.begin(), remaining.end(), original) != remaining.end();
    };
    if (!stillMissing()) return false;
    RootMappings selection;
    const auto* renderer = app.Renderer();
    if (selection.AddConfirmed(original, selected) &&
        CompileLegacyMmeRecoveryEffect(renderer ? renderer->device : nullptr, selected.c_str())) {
        if (!stillMissing()) return false;
        if (ticket.ConfirmEffect(app, original, selected)) return true;
    }
    if (ticket.Current(app) && identity == app.EnvFileName())
        ShowUiMessage(&app, owner, UiMessage::ProjectEffectRecoveryFailed, MB_OK | MB_ICONWARNING, selected.c_str());
    return false;
}
}

bool CanRecoverProjectEffect(MMDApp& app) noexcept {
    try {
        const auto ticket = LoadScope::Accepted(app);
        return &app == g_Block && ticket.Current(app) && !UnresolvedEffects(ticket, LoadScope::ProjectPath(app)).empty();
    } catch (...) {
        return false;
    }
}

bool RecoverProjectEffect(MMDApp& app, HWND owner) noexcept {
    try {
        const auto ticket = LoadScope::Accepted(app);
        if (&app != g_Block || !ticket.Current(app)) return false;
        const std::wstring identity(app.EnvFileName());
        const auto missing = UnresolvedEffects(ticket, LoadScope::ProjectPath(app));
        std::wstring original, selected;
        if (SelectProjectEffectReplacement(app, owner, missing, false, original, selected) !=
            ProjectEffectAction::Replace) return false;
        return ConfirmReplacement(app, owner, ticket, identity, original, selected);
    } catch (...) {
        // A failed optional recovery leaves model selection available.
    }
    return false;
}

bool ConfirmRemainingProjectEffects(MMDApp& app, HWND owner) noexcept {
    try {
        if (&app != g_Block || !LegacyMmeLoaded()) return true;
        const auto ticket = LoadScope::Accepted(app);
        if (!ticket.Current(app)) return true;
        const std::wstring identity(app.EnvFileName());
        while (ticket.Current(app) && identity == app.EnvFileName()) {
            const auto missing = UnresolvedEffects(ticket, LoadScope::ProjectPath(app));
            if (missing.empty()) return true;
            std::wstring original, selected;
            const auto action = SelectProjectEffectReplacement(app, owner, missing, true, original, selected);
            if (action == ProjectEffectAction::Cancel) return false;
            if (action == ProjectEffectAction::Skip) return true;
            if (action == ProjectEffectAction::Replace)
                ConfirmReplacement(app, owner, ticket, identity, original, selected);
        }
        return false;
    } catch (...) {
        // Optional effect recovery cannot veto otherwise valid PMM data.
        return true;
    }
}
}

namespace {
thread_local bool projectOpenUiActive = false;

bool CanStartProjectOpen(MMDApp* app) {
    return app && g_Block == app && !app->SceneMutationInProgress()
#if !MMD_ORIGINAL_PROFILE
        && !app->sceneMutationFinishing && !app->sceneCloseHold
#endif
        && !plugins::InCallback();
}
}

ProjectOpenUiScope::ProjectOpenUiScope(MMDApp* app) noexcept
    : app_(projectOpenUiActive ? nullptr : app) {
    if (app_) projectOpenUiActive = true;
}

ProjectOpenUiScope::~ProjectOpenUiScope() {
    if (app_) projectOpenUiActive = false;
}

bool ProjectOpenUiScope::Prepare() {
    if (!CanStartProjectOpen(app_)) return false;
    background_save::Wait(app_);
    if (!CanStartProjectOpen(app_) || SceneSaveInProgress()) return false;
    generation_ = app_->SceneGeneration();
    revision_ = app_->SceneRevision();
    recoveryPending_ = app_->sceneRecoveryPending;
    return true;
}

bool ProjectOpenUiScope::ConfirmDiscard(bool topmost) const {
    if (app_->SceneModified() != 0 &&
        ShowUiMessage(app_, app_->MainWindow(), UiMessage::OpenUnsaved,
                      MB_OKCANCEL | (topmost ? MB_TOPMOST : 0)) != IDOK)
        return false;
    return CanContinue();
}

bool ProjectOpenUiScope::CanContinue() const {
    return CanStartProjectOpen(app_) && !SceneSaveInProgress() &&
        app_->SceneGeneration() == generation_ && app_->SceneRevision() == revision_ &&
        app_->sceneRecoveryPending == recoveryPending_;
}

// VA 0x00408870 - wide path -> project directory (truncate at "UserFile").
void PathToProjectDir(wchar_t* destination, const wchar_t* source) {
    wchar_t local[0x100];
    wcscpy_s(local, 0x100, source);
    if (local[0] == L'\0') {
        destination[0] = L'\0';
        return;
    }
    wchar_t* hit = wcsstr(local, L"UserFile");
    if (hit != nullptr) {
        *hit = L'\0';
        wcscpy_s(destination, 0x100, local);
        return;
    }
    const size_t len = wcslen(local);
    if (local[len - 1] == L'\\') {
        wcscpy_s(destination, 0x100, local);
        return;
    }
    destination[0] = L'\0';
}

// VA 0x00408E70.  The original `this` is the inline path context at
// app+0x9F338, not the MMDApp base.  Keep the public port signature while
// restoring the two 256-wchar directory fields exactly:
//   +0   project/UserFile base, +512 executable directory.
void FontSubInit(MMDApp* app, const wchar_t* directory) {
    if (app == nullptr || directory == nullptr)
        return;
    PathResolutionWorkspace& paths = app->PathWorkspace();
    wcscpy_s(paths.executableDirectory, 0x100, directory);
    PathToProjectDir(paths.projectDirectory, paths.executableDirectory);
}

// Error handlers cannot rely on sibling catch clauses to catch a second
// exception from cleanup. Treat throwing resets like refusals and quarantine
// before reporting the original error, which may pump window messages.
bool ResetAfterLoadFailure(MMDApp* app) noexcept {
    const auto language = app->EnglishUI();
    try {
        if (ResetAppState(app)) return true;
    } catch (const std::exception& error) {
        runtime_log::Write("PMM recovery: error-handler reset threw: %s", error.what());
    } catch (...) {
        runtime_log::Write("PMM recovery: error-handler reset threw an unknown exception");
    }
    app->sceneRecoveryPending = true;
    app->EnglishUI() = language;
    return false;
}

bool ReportInvalidProjectLoad(MMDApp* app, pmm_io::Input& input, bool replaced,
                              const wchar_t* path) {
    const auto language = app->EnglishUI();
    input.Close();
    const bool reset = !replaced || ResetAfterLoadFailure(app);
    app->EnglishUI() = language;
    ShowUiMessage(app, app->MainWindow(), UiMessage::InvalidProjectFile, MB_OK | MB_ICONERROR, path);
    return reset;
}

// VA 0x00458F80 - PMM load shell (phase A).
// Returns false only when the post-boundary reset was refused; the caller then
// retries that reset once after its own teardown.
static bool KeyPoolLoadFailed(MMDApp* app, pmm_io::Input& input, bool replaced) {
    const unsigned language = app->EnglishUI();
    input.Close();
    const bool reset = !replaced || ResetAfterLoadFailure(app);
    app->EnglishUI() = language;
    const wchar_t* text[] = {
        L"キーフレームを読み込むためのメモリが不足しています。",
        L"Not enough memory to load all project keyframes.",
        L"内存不足，无法完整加载项目关键帧。"};
    MessageBoxW(app->MainWindow(), text[language <= 2 ? language : 1],
                L"PMM", MB_OK | MB_ICONERROR);
    return reset;
}

// Shared by LoadSceneFile and the two legacy void entry points. The caller has
// closed the PMM stream and released the native loader workspace. Returns true
// only once the retried reset completes and clears the quarantine.
bool RecoverSceneResetRefusal(MMDApp* app, bool ownsLoadTransaction,
                              bool restoreWindowSize) {
    if (!app) return false;
    // The replacement boundary is irreversible and the first reset already
    // refused. Record the scene as quarantined before any plugin cancellation
    // callback or the retry can observe it: only a reset that runs to
    // completion clears the flag (see its own tail), so a second refusal or an
    // exception below keeps this scene out of PMM snapshots.
    app->sceneRecoveryPending = true;
    // Cancellation callbacks may adopt a stored UI language. Freeze the language
    // the user reads now and restore it before any notice below.
    const unsigned language = app->EnglishUI();
    if (ownsLoadTransaction) plugins::CancelProjectLoad(app);
    bool reset = false;
    try {
        reset = ResetAppState(app);
    } catch (const std::exception& error) {
        runtime_log::Write("PMM recovery: post-replacement reset retry threw: %s", error.what());
    } catch (...) {
        runtime_log::Write("PMM recovery: post-replacement reset retry threw an unknown exception");
    }
    app->EnglishUI() = language;
    // A model abort defers this restore until the reset is known to succeed, so
    // a refusal never reshapes the half-replaced scene and the recovered scene
    // is adjusted exactly once.
    if (reset && !app->sceneRecoveryPending) {
        if (restoreWindowSize) HandleWindowSize(app);
        return true;
    }
    // The load already crossed the replacement boundary, so this is the final
    // failure: say the load stopped, never that the previous scene was restored.
    // Keep the quarantine explicit before the notice pumps messages. The guard
    // is local because the two legacy void entry points have no outer scene
    // mutation, and it must stay in force across the modal call.
    app->sceneRecoveryPending = true;
    runtime_log::Write("PMM recovery: post-replacement reset did not complete (reset %d); "
                       "the load fails and the replaced scene stays quarantined", reset ? 1 : 0);
    {
        ScopedSceneMutation mutation(*app);
        ShowUiMessage(app, app->MainWindow(), UiMessage::ProjectRecoveryFailed, MB_OK | MB_ICONWARNING);
    }
    return false;
}

void AcceptProjectSceneReplacement(MMDApp* app) {
    app->AdvanceSceneGeneration();
    project_relocation::LoadScope::Activate(*app);
    // The new project's legacy notification is deferred until commit.
    // Retire the previous project's relative EMM aliases during parsing.
    PrepareEffectProjectPaths(nullptr);
    // The old scene's GPU pool is keyed by filename alone. The new scene
    // must load again so the persistent cache can check the source bytes.
    if (auto* renderer = app->Renderer()) ReleaseTexturePool(*renderer);
    app->SceneModified() = 0;
    app->WindowLayoutReady() = 0;
    PathToProjectDir(app->PathWorkspace().projectDirectory, app->EnvFileName());
}

bool LoadSceneFile(const wchar_t* candidate) {
    MMDApp* app = g_Block;
    if (!app || !candidate) return false;
    const auto length = wcsnlen_s(candidate, 0x100);
    if (!length || length >= 0x100) return false;
    static thread_local bool opening = false;
    if (opening || app->SceneMutationInProgress() || plugins::InCallback()) return false;
    struct OpenScope {
        bool& active;
        explicit OpenScope(bool& value) : active(value) { active = true; }
        ~OpenScope() { active = false; }
    } openScope(opening);
#if !MMD_ORIGINAL_PROFILE
    // Outlives plugin preparation, cancellation, and native mutation scopes.
    ScopedSceneCloseDeferral closeDeferral(*app);
#endif
    // The input can alias EnvFileName. Freeze it before assigning read context.
    wchar_t path[0x100]{};
    std::wmemcpy(path, candidate, length + 1);
    background_save::Wait(app);
    if (SceneSaveInProgress()) return false;
    // A previous PMM replacement whose recovery reset was refused has left a
    // half-loaded scene (sceneRecoveryPending). The candidate path above is
    // already frozen, so an alias of EnvFileName cannot be disturbed by the
    // reset. Inside the open-reentrancy guard and after the save boundary has
    // drained, one normal reset is the only way out: a refusal or an exception
    // keeps the quarantine and fails this load before the plugin transaction,
    // the file open, or any project identity is touched. Only a completed
    // reset continues into the ordinary load path below.
    if (app->sceneRecoveryPending) {
        // ResetAppState may adopt a stored UI language. The gate below explains
        // itself after the attempt, so freeze the language the user reads now
        // and restore it on both the success and the refusal path.
        const unsigned language = app->EnglishUI();
        bool recovered = false;
        try {
            recovered = ResetAppState(app);
        } catch (const std::exception& error) {
            runtime_log::Write("PMM recovery: reset before a new load threw: %s", error.what());
        } catch (...) {
            runtime_log::Write("PMM recovery: reset before a new load threw an unknown exception");
        }
        app->EnglishUI() = language;
        if (!recovered || app->sceneRecoveryPending) {
            // A refused or thrown reset leaves the scene quarantined. State that
            // explicitly so nothing downstream treats it as recovered, then tell
            // the user the selected project was never touched.
            app->sceneRecoveryPending = true;
            runtime_log::Write("PMM recovery: reset did not complete before a new load; the quarantined scene is retained and the load fails");
            ShowUiMessage(app, app->MainWindow(), UiMessage::ProjectRecoveryPending, MB_OK | MB_ICONWARNING);
            return false;
        }
    }
    struct ConsumeOpenEnter {
        MMDApp* app;
        void Consume() const { if(app->MainWindow())app->state.enterKeyState=3; }
        ~ConsumeOpenEnter(){Consume();}
    } openEnter{app};
    openEnter.Consume();

    // Pair recovery, the PMM read lock and serializer candidates stay inside
    // the existing plugin transaction. No current project identity is changed.
    if (!plugins::PrepareProjectLoad(app, path)) return false;
    struct PluginLoadScope {
        MMDApp* app;
        void Cancel() {
            if (auto* owner = std::exchange(app, nullptr)) plugins::CancelProjectLoad(owner);
        }
        ~PluginLoadScope() { Cancel(); }
    } pluginLoadScope{app};
    runtime_log::WritePath("LOAD_SCENE", path);

    auto cachePath = project_relocation::LoadScope::FreezePath(path);
    int fd = -1;
    const errno_t err = _wsopen_s(&fd, path, 0x8000, 0x40, 0x80);
    pmm_io::Input input(fd);
    const auto refuseBeforeReplacement = [&](UiMessage reason, const wchar_t* detail) {
        input.Close();
        pluginLoadScope.Cancel();
        ShowUiMessage(app, app->MainWindow(), reason, MB_OK, detail);
        return false;
    };
    if (err != 0) {
        const wchar_t* format[] = {L"%ls\nエラーコード: %d", L"%ls\nError code: %d", L"%ls\n错误码：%d"};
        wchar_t detail[512]{};
        swprintf_s(detail, format[app->EnglishUI() <= kUiChinese ? app->EnglishUI() : kUiEnglish], path, err);
        return refuseBeforeReplacement(UiMessage::OpenProjectError, detail);
    }
    char hdr[0x1E]{};
    const int read = _read(fd, hdr, sizeof hdr);
    if (read != sizeof hdr || std::memcmp(hdr, "Polygon Movie maker", 19) != 0) {
        return refuseBeforeReplacement(UiMessage::InvalidProjectFile, path);
    }
    const bool version1 = strncmp(hdr + 0x14, "0001", 5) == 0;
    const bool supported = version1 || strncmp(hdr + 0x14, "0002", 5) == 0
#if !MMD_ORIGINAL_PROFILE
        || strncmp(hdr + 0x14, "0003", 5) == 0
#endif
        ;
    if (!supported) {
        return refuseBeforeReplacement(UiMessage::UnsupportedProjectVersion, path);
    }

    ScopedSceneMutation mutation(*app);
    bool replaced = false;
    struct RestoreIdentity {
        MMDApp* app;
        const bool& replaced;
        wchar_t file[0x100];
        bool pending = true;
        RestoreIdentity(MMDApp* value, const bool& changed) : app(value), replaced(changed) {
            std::memcpy(file, app->EnvFileName(), sizeof file);
        }
        void RestoreBeforeReplacement() {
            if (!pending || replaced) return;
            std::memcpy(app->EnvFileName(), file, sizeof file);
            pending = false;
        }
        ~RestoreIdentity() {
            if (!pending) return;
            if (!replaced) {
                RestoreBeforeReplacement();
                return;
            }
            // A failed replacement owns neither the original identity nor
            // the candidate. Clear it before the editor redraws.
            app->EnvFileName()[0] = L'\0';
            app->PathWorkspace().projectDirectory[0] = L'\0';
            if (app->MainWindow()) SetWindowTextW(app->MainWindow(), L"MikuMikuDance");
        }
    } restore{app, replaced};
    // Native asset resolution and success-tail notifications need this read
    // context. The scope restores it if the history boundary refuses entry.
    std::wmemcpy(app->EnvFileName(), path, length + 1);
    bool loaded = false;
    bool resetRefused = false, restoreWindowSize = false;
    project_relocation::LoadScope relocation(*app, std::move(cachePath));
    try {
        loaded = relocation.Run([&] {
            if (version1) return TryLoadSceneV1(app, input, replaced);
            input.EnableCheckedReads();
            return TryLoadSceneV2(app, input, replaced);
        });
    } catch (const SceneResetRefused& refused) {
        input.Close();
        resetRefused = true;
        restoreWindowSize = refused.restoreWindowSize;
    } catch (const pmm_io::UnsupportedText&) {
        restore.RestoreBeforeReplacement();
        const auto language = app->EnglishUI();
        input.Close();
        if (replaced && !ResetAfterLoadFailure(app)) resetRefused = true;
        app->EnglishUI() = language;
        original_import::Refuse(app->MainWindow(), language);
    } catch (const std::bad_alloc&) {
        restore.RestoreBeforeReplacement();
        if (!KeyPoolLoadFailed(app, input, replaced)) resetRefused = true;
    } catch (const pmm_io::ShortRead& error) {
        restore.RestoreBeforeReplacement();
        const auto language = app->EnglishUI();
        const wchar_t* formats[] = {L"%ls\n位置：%lld、必要なバイト数：%u、読み取ったバイト数：%d",
            L"%ls\nByte offset: %lld; expected bytes: %u; received bytes: %d",
            L"%ls\n文件偏移：%lld；应读取：%u 字节；实际读取：%d 字节"};
        wchar_t detail[512]{};
        swprintf_s(detail, formats[language <= 2 ? language : 1], path, error.offset, error.expected, error.received);
        input.Close();
        if (replaced && !ResetAfterLoadFailure(app)) resetRefused = true;
        app->EnglishUI() = language;
        ShowUiMessage(app, app->MainWindow(), UiMessage::IncompleteProjectFile, MB_OK | MB_ICONERROR, detail);
    } catch (const std::runtime_error&) {
        restore.RestoreBeforeReplacement();
        if (!ReportInvalidProjectLoad(app, input, replaced, path)) resetRefused = true;
    }
    if (resetRefused) {
        // The replacement is irreversible, so a refused failure reset cannot be
        // ignored. This entry owns the prepared plugin transaction; recovered or
        // not, the load itself stays failed.
        (void)RecoverSceneResetRefusal(app, true, restoreWindowSize);
    }
    if (replaced) LocalizeUI(app);
    if (loaded) restore.pending = false;
    if (loaded) relocation.Complete();
    return loaded;
}

void LoadSceneFile() {
    if (g_Block) (void)LoadSceneFile(g_Block->EnvFileName());
}

}  // namespace mikumikustudio
