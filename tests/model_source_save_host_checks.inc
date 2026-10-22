// Included by the real PMM host after its Check/Load helpers. The file picker
// is controlled; model serialization, common history and PMM reload are real.
#if !MMD_ORIGINAL_PROFILE
#include "../src/plugins/native_model_toon.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include "../src/plugins/native_object_commands.hpp"
#include "../src/plugins/native_history_commands.hpp"
#include "../src/plugins/history_runtime.hpp"
#include "../src/window/enhanced_model_save.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include "mikumikustudio/bake_ui_lock.hpp"
#include <commdlg.h>

namespace model_source_save_host {
using namespace plugins;
using Path = std::array<wchar_t, 256>;
Path Source(MMDApp& app) {
    Path path{}; std::memcpy(path.data(), mdl::Mdl(app.ModelSlot(0))->path, sizeof(path)); return path;
}
struct Hooks {
    MMDApp& app;
    unsigned char* const object;
    std::filesystem::path output;
    unsigned dialogs = 0, writes = 0;
    bool ownMinhook = false;
    static inline Hooks* active = nullptr;
    static inline decltype(&WriteEnhancedModelPmd) writer = nullptr;
    static BOOL WINAPI Choose(OPENFILENAMEW* dialog) {
        auto& self = *active;
        DWORD process = 0;
        Check(dialog && dialog->hwndOwner == self.app.MainWindow() &&
            GetWindowThreadProcessId(dialog->hwndOwner, &process) == GetCurrentThreadId() && process == GetCurrentProcessId(),
            "model source picker belongs to this real host window and thread");
        Check(NativeObjectOperationActive(&self.app) && !NativeObjectPublicationAuthorized(&self.app) && bake::UiLocked() &&
            self.app.SelectedModel() == self.object && (dialog->Flags & OFN_NOCHANGEDIR),
            "real 263 protects its captured model throughout the controlled file picker");
        Check(dialog->nMaxFile > self.output.wstring().size(), "owned model destination fits the original source field");
        wcscpy_s(dialog->lpstrFile, dialog->nMaxFile, self.output.c_str());
        ++self.dialogs; return TRUE;
    }
    static EnhancedPmdWriteResult Write(MMDApp* app, unsigned char* object, const wchar_t* path, const EnhancedPmdPublishCheck& check) {
        auto& self = *active;
        Check(app == &self.app && object == self.object && std::filesystem::path(path) == self.output &&
            NativeObjectOperationActive(app) && !NativeObjectPublicationAuthorized(app) && check.current && check.deferErrors,
            "actual writer receives the captured object and trusted final check outside publication authorization");
        ++self.writes;
        return writer(app, object, path, check);
    }
    explicit Hooks(MMDApp& value) : app(value), object(value.ModelSlot(0)) {
        Check(!active, "one source-save host hook owner"); active = this;
        const auto status = MH_Initialize(); ownMinhook = status == MH_OK;
        Check(ownMinhook || status == MH_ERROR_ALREADY_INITIALIZED, "initialize only this host process's source-save hooks");
        Check(MH_CreateHook(reinterpret_cast<void*>(&GetUiSaveFileNameW), reinterpret_cast<void*>(&Choose), nullptr) == MH_OK &&
            MH_CreateHook(reinterpret_cast<void*>(&WriteEnhancedModelPmd), reinterpret_cast<void*>(&Write), reinterpret_cast<void**>(&writer)) == MH_OK,
            "control only the file picker and forward the real PMD serializer");
        Check(MH_EnableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW)) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(&WriteEnhancedModelPmd)) == MH_OK, "enable source-save host hooks");
    }
    ~Hooks() {
        MH_DisableHook(reinterpret_cast<void*>(&WriteEnhancedModelPmd)); MH_RemoveHook(reinterpret_cast<void*>(&WriteEnhancedModelPmd));
        MH_DisableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW)); MH_RemoveHook(reinterpret_cast<void*>(&GetUiSaveFileNameW));
        writer = nullptr; active = nullptr; if (ownMinhook) MH_Uninitialize();
    }
    void Menu(const std::filesystem::path& path, unsigned notification) {
        output = path; const auto oldDialogs = dialogs, oldWrites = writes;
        SendMessageW(app.MainWindow(), WM_COMMAND, MAKEWPARAM(263, notification), 0);
        Check(dialogs == oldDialogs + 1 && writes == oldWrites + 1 && std::filesystem::is_regular_file(path),
            "real main-window command 263 reaches its picker and standard writer exactly once");
    }
};
void Run(MMDApp& app, const std::filesystem::path& directory, int mode) {
    auto* history = NativeHistory(&app);
    Check(history && !BoundNativePoseHistory(&app) && app.ModelSlot(0), "real ordinary host starts with its existing private journal");
    const Path original = Source(app);
    const auto exported = directory / L"enhanced-source.pmd";
    const auto second = directory / L"other-source.pmd";
    const auto project = directory / L"enhanced-source.pmm";
    const unsigned char expectedToon[] = "toon09.bmp";
    const auto editToon = [&] {
        NativeModelToonPlan plan; ModelToonDraft draft; std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(app, 0) == MMS_OK && plan.Read(draft) == MMS_OK, "prepare enhanced Toon data through the production participant");
        draft[0] = L"toon09.bmp";
        Check(plan.Set(draft) == MMS_OK && plan.Take(change) == MMS_OK && change, "freeze a real enhanced-model edit before saving its source");
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(history->PrepareNative("Source save host Toon edit", change, token, SharedHistory::NativeIntent::ExplicitCommit) == MMS_OK &&
            history->CommitNative(std::move(token)) == MMS_OK, "enhanced edit uses the same common journal as source saving");
        PublishBoundHistoryEffects(app, *history, history->LastPublicationEffects(), false);
    };
    if (mode < 0) {
        // Ordinary startup intentionally does not expose mms.edit. Configure
        // source-only model data here to verify that PMM really reloads the new
        // PMD; this cohort does not claim a public plugin editing transaction.
        std::memcpy(mdl::Mdl(app.ModelSlot(0))->pmdToonFileNames[0], expectedToon, sizeof(expectedToon));
        const auto undo = history->UndoCount(), redo = history->RedoCount();
        Hooks hooks(app); hooks.Menu(exported, 0);
        Check(history->UndoCount() == undo && history->RedoCount() == redo && !BoundNativePoseHistory(&app) && app.SceneModified(),
            "ordinary real runtime authorizes safe source publication without exposing or recording migration edits");
    } else {
        NativePoseTestBinding binding(app, *history);
        Check(history->SetMode(mode ? SharedHistory::RecordingMode::ExplicitOnly : SharedHistory::RecordingMode::AllOperations) == MMS_OK,
            "real host accepts the selected history mode");
        editToon(); const auto edited = history->UndoCount();
        Hooks hooks(app);
        app.MessageSeen() = mode ? 1 : 0; app.PhysicsResetPending() = 7;
        const auto enhancedDirty = app.EnhancedModelDirty();
        const auto frame = app.CurrentFrame();
        hooks.Menu(exported, unsigned(mode));
        Check(history->UndoCount() == edited + 1 && history->LastPublicationEffects() == HistoryEffects::ProjectData &&
            !history->LastPublicationRequiresPhysicsReset() && app.PhysicsResetPending() == 7 &&
            app.EnhancedModelDirty() == enhancedDirty && app.CurrentFrame() == frame && app.MessageSeen() == unsigned(mode),
            "real source save is one explicit project-only edit and preserves existing input, pose, physics and enhancement state");
        const Path saved = Source(app);
        hooks.Menu(second, unsigned(mode));
        Check(history->UndoCount() == edited + 2 && RunBoundObjectHistory(app, *history, false) == MMS_OK && Source(app) == saved && history->RedoCount() == 1,
            "a later source save can be undone independently");
        std::uint64_t revision = 0; Check(history->Revision(revision) == MMS_OK, "read revision before same-source output");
        const auto sceneRevision = app.SceneRevision(); hooks.Menu(exported, unsigned(mode));
        std::uint64_t after = 0; Check(history->Revision(after) == MMS_OK && after == revision && app.SceneRevision() == sceneRevision &&
            history->UndoCount() == edited + 1 && history->RedoCount() == 1 && Source(app) == saved,
            "same-source file output preserves every path byte, redo and both revision counters");
        const auto writes = hooks.writes;
        Check(RunBoundObjectHistory(app, *history, false) == MMS_OK && Source(app) == original, "real source undo restores the complete original model reference");
        const auto offline = directory / L"enhanced-source.offline";
        std::filesystem::rename(exported, offline);
        Check(RunBoundObjectHistory(app, *history, true) == MMS_OK && Source(app) == saved && hooks.writes == writes,
            "real source redo uses its retained path while the PMD is offline, without writing a file");
        std::filesystem::rename(offline, exported);
        Check(history->Reset() == MMS_OK, "retire source and enhancement records before the real scene replacement"); history->Collect();
    }
    Check(std::filesystem::equivalent(mdl::Mdl(app.ModelSlot(0))->path, exported), "successful source selection still points PMM at the new model");
    Check(SaveSceneCopy(&app, project.c_str()), "actual PMM writer saves the enhanced model's new source");
    // Replace live source-only data so it cannot accidentally satisfy reload.
    std::memcpy(mdl::Mdl(app.ModelSlot(0))->pmdToonFileNames[0], "toon01.bmp", 11);
    Load(app, project);
    Check(app.ModelSlot(0) && std::filesystem::equivalent(mdl::Mdl(app.ModelSlot(0))->path, exported) &&
        !std::memcmp(mdl::Mdl(app.ModelSlot(0))->pmdToonFileNames[0], expectedToon, sizeof(expectedToon)),
        "actual PMM reopen loads the exported PMD and restores enhanced Toon data absent from the PMM extension");
    std::printf("PASS real model source save and PMM reopen mode=%d\n", mode);
}
}
#endif
