#include "manager.hpp"
#include "native_ui.hpp"
#include "project_storage.hpp"
#include "history_runtime.hpp"
#include "native_pose_scope.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/animation_curve_editor.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/plugin_storage.hpp"
#include "mikudancestudio/plugin_runtime.hpp"
#include "mikudancestudio/plugin_scene.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/runtime_log.hpp"
#include "mikudancestudio/build_profile.hpp"
#include <ShlObj.h>
#include <commctrl.h>

namespace mikudancestudio::plugins {
namespace {
std::unique_ptr<Manager> manager;
std::unique_ptr<NativeUi> ui;
std::unique_ptr<SharedHistory> history;
#if !MMD_ORIGINAL_PROFILE
class HistoryCommandHost final : public EditHost {
    MMDApp& app_;
    SharedHistory& target_;
public:
    HistoryCommandHost(MMDApp& app, SharedHistory& target) : app_(app), target_(target) {}
    void AttachStore(EditStore* store) override { target_.AttachStore(store); }
    void DetachStore(EditStore* store) noexcept override { target_.DetachStore(store); }
    MmsResult Revision(uint64_t& value) override { return target_.Revision(value); }
    MmsResult Enter() override { return target_.Enter(); }
    void Leave() noexcept override { target_.Leave(); }
    MmsResult Reserve(std::shared_ptr<const EditRecord> record, std::unique_ptr<EditPublication>& publication) override {
        return target_.Reserve(std::move(record), publication);
    }
    MmsResult History(bool redo) override {
        if (!manager || !manager->CanInvokeEdit()) return MMS_E_BUSY;
        return RunBoundObjectHistory(app_, target_, redo, true);
    }
    void Poison() noexcept override { target_.Poison(); }
};
// Manager/EditStore must detach before this forwarding host or its target die.
std::unique_ptr<HistoryCommandHost> historyCommandHost;
std::unique_ptr<NativePoseTestBinding> migrationBinding;
#endif
std::unique_ptr<SharedHistory::ResetTransaction> projectHistoryReset;
MMDApp* owner = nullptr;
int toolbarHeight = 0;
bool refreshing = false, closing = false;
bool historyUiPending = false;
PluginProject currentProject, pendingProject;
bool loadingProject = false;
bool replacingHistory = false, historySceneReplaced = false;
HANDLE loadingMain = INVALID_HANDLE_VALUE;
struct ProjectNotification { uint32_t kind; MmsId id; std::string path; };
std::deque<ProjectNotification> projectEvents;
constexpr UINT kProjectEvent = WM_APP + 0x6b;
constexpr UINT_PTR kProjectRetry = 0x4d4d536b;
constexpr UINT_PTR kHistoryUiRetry = 0x4d4d536c;
MmsResult PrepareHistoryReset(std::unique_ptr<SharedHistory::ResetTransaction>& out) {
    if (!history) return MMS_OK;
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(owner) || NativeObjectUiPending(owner)) return MMS_E_BUSY;
#endif
    // ResetAppState is also used to recover a partially read PMM while the
    // native loader owns ScopedSceneMutation. Only this internal boundary may
    // bypass that gate; serializer and plugin callback checks still apply.
    replacingHistory = true;
    const auto result = history->PrepareReset(out);
    replacingHistory = false;
    return result;
}
void QueueProjectEvent(MMDApp* app, uint32_t kind, const wchar_t* path) {
    if (!manager || owner != app) return;
    Boundary([&] {
        MmsId id{}; unsigned char bytes[16]{};
        if(currentProject.projectId.size()==32){
            const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
            for(size_t i=0;i<16;++i)bytes[i]=static_cast<unsigned char>((digit(currentProject.projectId[2*i])<<4)|digit(currentProject.projectId[2*i+1]));
            std::memcpy(&id,bytes,sizeof(id));
        }
        projectEvents.push_back({kind,id,Utf8(path?path:L"")});
        if(!PostMessageW(app->MainWindow(),kProjectEvent,0,0)){projectEvents.pop_back();return MMS_E_FAILED;}
        return MMS_OK;
    });
}
constexpr UINT kRefresh = WM_APP + 0x6a;
void LogMessage(const std::string& s) { runtime_log::Write("PLUGIN_ABI %s", s.c_str()); }
LRESULT CALLBACK Subclass(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_CLOSE && manager && manager->InCallback()) { PostMessageW(hwnd, WM_CLOSE, wp, lp); return 0; }
    if (message == kRefresh || (message == WM_TIMER && wp == kHistoryUiRetry)) {
        KillTimer(hwnd, kHistoryUiRetry);
        if (!closing && owner) {
            Refresh(owner);
            // A modal plugin callback may consume the wake before it returns.
            // Retry at a safe boundary without spinning its message loop.
            if (historyUiPending) SetTimer(hwnd, kHistoryUiRetry, 16, nullptr);
        }
        return 0;
    }
    if (message == kProjectEvent || (message == WM_TIMER && wp == kProjectRetry)) {
        KillTimer(hwnd,kProjectRetry);
        if(manager && !closing && !manager->InCallback() && owner && owner->WindowLayoutReady() && !owner->SceneMutationInProgress()) {
            auto remaining=projectEvents.size();
            while(remaining-- && !projectEvents.empty()) {
                auto event=std::move(projectEvents.front());projectEvents.pop_front();
                MmsProjectEventV1 payload{Header<MmsProjectEventV1>(),event.id,0,Text(event.path)};
                manager->Event(event.kind,{&payload,sizeof(payload)},1);
            }
        } else if(owner && !closing && !projectEvents.empty()) SetTimer(hwnd,kProjectRetry,16,nullptr);
        return 0;
    }
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(hwnd, Subclass, id); Shutdown(); }
    return DefSubclassProc(hwnd, message, wp, lp);
}
}
static void InitializeRuntime(MMDApp* app, bool migrationTest) {
    if (build_profile::original || !app || manager) return;
    const auto initialized = Boundary([&] {
        owner = app; closing = false;
        wchar_t data[32768]{}; auto dataLength = GetEnvironmentVariableW(L"LOCALAPPDATA", data, 32768);
        if (!dataLength || dataLength >= 32768 || !fs::path(data).is_absolute()) {
            if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, data))) throw std::runtime_error("LOCALAPPDATA unavailable");
        }
        std::wstring module(32768, 0); auto n = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())); if (!n || n >= module.size()) throw std::runtime_error("process path unavailable"); module.resize(n);
        manager = std::make_unique<Manager>(fs::path(data) / L"MMS" / L"plugins", LogMessage, [window = app->MainWindow()] { PostMessageW(window, kRefresh, 0, 0); }, false, CreateSceneStore(app));
        if (manager->Storage().Prepare(currentProject.records) != MMS_OK || !manager->Storage().Commit()) throw std::runtime_error("initialize plugin project data");
        history = std::make_unique<SharedHistory>(manager->Storage(), [app] {
            if (closing || owner != app) return MMS_E_NOT_READY;
            bool objectPublication = false;
#if !MMD_ORIGINAL_PROFILE
            objectPublication = NativeObjectPublicationAuthorized(app);
            if ((NativeObjectOperationActive(app) && !objectPublication) || NativeObjectUiPending(app)) return MMS_E_BUSY;
#endif
            if ((manager->InCallback() && !manager->CanInvokeEdit()) ||
                (app->SceneMutationInProgress() && !replacingHistory && !objectPublication)) return MMS_E_BUSY;
            if (!replacingHistory && (app->PlaybackActive() || app->FrameStepPlayback() ||
                app->state.recordPlaybackStartPending)) return MMS_E_BUSY;
            return MMS_OK;
        });
#if !MMD_ORIGINAL_PROFILE
        if (migrationTest) {
            historyCommandHost = std::make_unique<HistoryCommandHost>(*app, *history);
            if (manager->AttachEditHost(*historyCommandHost) != MMS_OK) throw std::runtime_error("attach common edit history");
            migrationBinding = std::make_unique<NativePoseTestBinding>(*app, *history);
            manager->SetEditPublished([app] {
                if (closing || owner != app) return;
                app->SceneModified() = 1; app->RecordedFrameCountByte1() = 1;
                app->PhysicsResetPending() = 1; historyUiPending = true;
                PostMessageW(app->MainWindow(), kRefresh, 0, 0);
            });
        }
#endif
        ui = std::make_unique<NativeUi>(manager->Ui(), app->MainWindow(), LogMessage);
        if (!SetWindowSubclass(app->MainWindow(), Subclass, 0x4d4d5301, 0)) throw std::runtime_error("plugin lifetime hook failed");
        manager->Load(fs::path(module).parent_path() / L"PLUGINS"); Refresh(app); return MMS_OK;
    });
    if (initialized != MMS_OK) {
        runtime_log::Write("PLUGIN_ABI initialization failed: %d", initialized);
        Shutdown();
    }
}
void Initialize(MMDApp* app) { InitializeRuntime(app, false); }
bool InitializeHistoryTestRuntime(MMDApp* app) noexcept {
    if (build_profile::original || !app || !app->MainWindow() || manager) return false;
    InitializeRuntime(app, true);
    return manager && owner == app && history;
}
void Shutdown() {
    if (manager && manager->ExecutingCallback()) return;
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(owner)) return;
#endif
    if (closing) return; closing = true;
    if (owner && owner->MainWindow()) KillTimer(owner->MainWindow(), kHistoryUiRetry);
    CancelProjectLoad(owner);
    if (manager) manager->Stop();
    ui.reset();
#if !MMD_ORIGINAL_PROFILE
    migrationBinding.reset();
    ForgetBoundObjectUi(owner);
#endif
    if (manager) {
        auto detached = manager->DetachEditHost();
        // A failed cancel callback clears pending entries and poisons history.
        // Retry only that completed cancellation; BUSY/THREAD remain attached.
        if (detached == MMS_E_FAILED) detached = manager->DetachEditHost();
        if (detached != MMS_OK) {
            runtime_log::Write("PLUGIN_ABI edit detach failed: %d; retaining host until process exit", detached);
            // The retained manager still references this exact host/storage.
            // Closing rejects later calls before accessing the application.
            history.release(); manager.release();
#if !MMD_ORIGINAL_PROFILE
            historyCommandHost.release();
#endif
        }
    }
#if !MMD_ORIGINAL_PROFILE
    historyCommandHost.reset();
#endif
    history.reset(); manager.reset(); projectEvents.clear(); toolbarHeight = 0; owner = nullptr; historyUiPending = false;
}
SharedHistory* NativeHistory(MMDApp* app) noexcept { return app == owner && !closing ? history.get() : nullptr; }
void RequestBoundHistoryUiRefresh(MMDApp* app) noexcept {
    if (!app || app != owner || closing) return;
    historyUiPending = true;
    PostMessageW(app->MainWindow(), kRefresh, 0, 0);
}
void Layout(MMDApp* app) {
    if (!ui || app != owner) return;
    Boundary([&] {
        RECT rect{}; GetClientRect(app->MainWindow(), &rect);
        int left = app->SidebarWidth() + 9;
        toolbarHeight = ui->Layout(left, std::max(0L, rect.right - left), std::max(0L, (rect.bottom - 217) / 3), !app->FullscreenMode() && !app->FloatingWindow());
        return MMS_OK;
    });
}
int ToolbarHeight(MMDApp* app) { return app == owner && !app->FullscreenMode() && !app->FloatingWindow() ? toolbarHeight : 0; }
void Refresh(MMDApp* app) {
    if (!ui || app != owner || refreshing || closing) return;
    refreshing = true; auto old = toolbarHeight;
    Boundary([&] { ui->Refresh(); Layout(app); if (toolbarHeight != old) HandleWindowSize(app);
#if !MMD_ORIGINAL_PROFILE
        if (historyUiPending && history && manager && !manager->InCallback() && !app->SceneMutationInProgress()) {
            historyUiPending = !RefreshBoundObjectUi(*app, *history);
        }
#endif
        if (manager && !manager->InCallback()) manager->SaveUiPreferences();
        if (history && manager && !manager->InCallback() && !app->SceneMutationInProgress()) history->Collect(); return MMS_OK; });
    refreshing = false;
}
bool HandleCommand(unsigned id) { bool handled = false; Boundary([&] { if (ui) handled = ui->Command(id); return MMS_OK; }); return handled; }
bool HandleShortcut(unsigned key, bool viewportFocused) {
    unsigned modifiers = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
    bool handled = false; Boundary([&] { if (ui) handled = ui->Shortcut(key, modifiers, viewportFocused); return MMS_OK; }); return handled;
}
bool HandleDialogMessage(MSG* message) { return ui && !closing && ui->DialogMessage(message); }
bool InCallback() { return manager && manager->InCallback(); }

struct ProjectSave::Impl { MMDApp* app; PluginProject data; StoredRecords storeRecords; };
ProjectSave::ProjectSave(MMDApp* app) : impl_(std::make_unique<Impl>()) {
    if(app->SceneMutationInProgress() || (manager && owner==app && manager->InCallback()))throw std::runtime_error("project save during callback or scene mutation");
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(app) || NativeObjectUiPending(app)) throw std::runtime_error("object UI refresh pending");
#endif
    if(currentProject.projectId.empty())currentProject.projectId=StorageId();
    impl_->app = app; impl_->data = currentProject;
    if (manager && owner == app) {
        const auto r = manager->Storage().Capture(impl_->data.records);
        if (r) throw std::runtime_error("plugin serializer save returned " + std::to_string(r));
        impl_->storeRecords=impl_->data.records;
    }
}
ProjectSave::~ProjectSave() = default;
bool ProjectSave::Publish(const wchar_t* staged, const wchar_t* destination, bool copy) {
    try {
        PublishProjectPair(fs::absolute(destination),fs::absolute(staged),impl_->data,copy);
        if (!copy) {
            if (manager && owner == impl_->app) manager->Storage().Saved(impl_->storeRecords);
            currentProject=std::move(impl_->data);
        }
        return true;
    } catch (const std::exception& e) {
        LogMessage(std::string("project save failed: ")+e.what());
        try { RecoverProjectPair(fs::absolute(destination)); }
        catch (const std::exception& recovery) { LogMessage(std::string("project recovery pending: ")+recovery.what()); }
        return false;
    }
}
bool PrepareProjectLoad(MMDApp* app, const wchar_t* path) {
    if (build_profile::original) return true;
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(app) || NativeObjectUiPending(app)) return false;
#endif
    if (loadingProject || (manager && manager->InCallback())) return false;
    try {
        auto data=ReadPluginProject(fs::absolute(path));
        loadingMain=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(loadingMain==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot lock PMM for loading");
        if(!data.mainHash.empty() && FileHash(path)!=data.mainHash)throw std::runtime_error("PMM changed before loading");
        if (manager && owner == app) {
            if (auto r=PrepareHistoryReset(projectHistoryReset))
                throw std::runtime_error("history project preparation returned "+std::to_string(r));
            auto r=manager->Storage().Prepare(data.records);
            if(r)throw std::runtime_error("plugin serializer prepareLoad returned "+std::to_string(r));
        }
        pendingProject=std::move(data);loadingProject=true;historySceneReplaced=false;return true;
    } catch(const std::exception& e) {
        projectHistoryReset.reset();
        if(loadingMain!=INVALID_HANDLE_VALUE){CloseHandle(loadingMain);loadingMain=INVALID_HANDLE_VALUE;}
        LogMessage(std::string("project load refused: ")+e.what());
        const wchar_t* messages[]={L"プラグインのプロジェクトデータを読み込めません。PMM と付随ファイルの対応、およびプラグインのバージョンを確認してください。",L"Cannot load plugin project data. Check the PMM/sidecar pair and plugin versions.",L"无法读取插件工程数据。请检查 PMM 与扩展文件是否配套，以及插件版本是否匹配。"};
        if(app->MainWindow())MessageBoxW(app->MainWindow(),messages[app->EnglishUI()<=2?app->EnglishUI():1],L"MMS",MB_OK|MB_ICONERROR);
        return false;
    }
}
bool BeginProjectSceneReplacement(MMDApp* app) {
    if (build_profile::original || !history || owner != app) return true;
    if (!loadingProject || !projectHistoryReset || historySceneReplaced) return false;
    const auto result = projectHistoryReset->Commit();
    if (result != MMS_OK) { LogMessage("history scene replacement refused: "+std::to_string(result)); return false; }
    historySceneReplaced = true;
    return true;
}
void CommitProjectLoad(MMDApp* app) {
    if (!loadingProject) return;
    if (manager && owner == app && !manager->Storage().Commit()) { LogMessage("serializer commitLoad violated its no-fail contract");return; }
    currentProject=std::move(pendingProject);loadingProject=false;historySceneReplaced=false;
    projectHistoryReset.reset();
    if(loadingMain!=INVALID_HANDLE_VALUE){CloseHandle(loadingMain);loadingMain=INVALID_HANDLE_VALUE;}
    QueueProjectEvent(app,MMS_EVENT_PROJECT_OPENED,app->EnvFileName());
}
void CancelProjectLoad(MMDApp* app) {
    if(!loadingProject)return;
    if(manager && owner==app)manager->Storage().Cancel();
    pendingProject={};loadingProject=false;historySceneReplaced=false;
    projectHistoryReset.reset();
    if(loadingMain!=INVALID_HANDLE_VALUE){CloseHandle(loadingMain);loadingMain=INVALID_HANDLE_VALUE;}
}
bool ResetPluginProject(MMDApp* app) {
    if(build_profile::original)return true;
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(app) || NativeObjectUiPending(app)) return false;
#endif
    if(manager && owner==app && manager->Storage().InCallback())return false;
    if(manager && owner==app && manager->InCallback() && !loadingProject)return false;
    CancelProjectLoad(app);
    std::unique_ptr<SharedHistory::ResetTransaction> reset;
    if(manager && owner==app){
        if (PrepareHistoryReset(reset) != MMS_OK) { LogMessage("history project reset refused"); return false; }
        const auto r=manager->Storage().Prepare({});
        if(r){LogMessage("plugin project reset refused");return false;}
        if (reset && reset->Commit()!=MMS_OK) { manager->Storage().Cancel(); LogMessage("history project reset refused"); return false; }
        if(!manager->Storage().Commit()){LogMessage("plugin project reset refused");return false;}
    }
    currentProject={};QueueProjectEvent(app,MMS_EVENT_SCENE_RESET,L"");return true;
}
void PluginProjectSaved(MMDApp* app,const wchar_t* path){QueueProjectEvent(app,MMS_EVENT_PROJECT_SAVED,path);}
bool DeletePluginBackup(const wchar_t* path) {
    if(build_profile::original)return DeleteFileW(path)!=FALSE;
    // Caller has already checked the native PMM ownership marker. Only remove
    // an accompanying extension if its hash proves it belongs to this PMM.
    return Boundary([&]() -> MmsResult {
        const fs::path target(path); const auto side=PluginSidecar(target);
        const bool hasSide=fs::exists(side);
        if(hasSide) ReadPluginProject(target);
        if(!DeleteFileW(path))return MMS_E_FAILED;
        if(hasSide && !DeleteFileW(side.c_str()))LogMessage("obsolete autosave sidecar could not be removed");
        return MMS_OK;
    }) == MMS_OK;
}
}
