#include "mikumikustudio/local_data_path.hpp"
#include "manager.hpp"
#include "native_ui.hpp"
#include "ui_property_refresh.hpp"
#include "project_storage.hpp"
#include "history_runtime.hpp"
#include "native_pose_scope.hpp"
#include "native_viewport_history.hpp"
#include "native_history_commands.hpp"
#include "native_object_commands.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/animation_curve_editor.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/plugin_storage.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "mikumikustudio/system_settings_ui.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/runtime_log.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/ui_message.hpp"
#include "../window/ui_control_layout.hpp"
#include <ShlObj.h>
#include <d3d9.h>
#include <commctrl.h>
#include <atomic>
#include <cwchar>

namespace mikumikustudio { void LayoutViewportPanels(MMDApp*); }

namespace mikumikustudio::plugins {
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
std::uint64_t runtimeBinding = 0;
int toolbarHeight = 0, propertyAreaHeight = 0, leftAreaWidth = 0, rightAreaWidth = 0;
bool refreshing = false, closing = false;
bool historyUiPending = false;
// Accumulated publication masks waiting for the next safe-point UI refresh.
// A successful history with a None mask still updates the history controls, so
// this cursor is separate from historyUiPending. Requests are only ORed here;
// they never mark the project dirty or authorize a history publication.
HistoryEffects pendingEffects = HistoryEffects::None;
// Bumped by every enqueue so consumption can tell whether a new request arrived
// while the previous batch was being applied.
std::uint64_t historyUiGeneration = 0;
PluginProject currentProject, pendingProject;
bool loadingProject = false;
bool replacingHistory = false, historySceneReplaced = false;
HANDLE loadingMain = INVALID_HANDLE_VALUE;
struct ProjectNotification { uint32_t kind; MmsId id; std::string path; };
std::deque<ProjectNotification> projectEvents;
struct BusinessNotification {
    uint32_t kind = 0, version = 0;
    MmsObjectRef object{};
    std::vector<unsigned char> bytes;
};
std::deque<BusinessNotification> businessEvents;
// Probe-only property ids must outlive the deferred delivery payload.
std::deque<std::string> propertyProbeIds;
uint64_t deviceGeneration = 1;
bool deviceLost = false;
constexpr UINT kProjectEvent = WM_APP + 0x6b;
constexpr UINT_PTR kProjectRetry = 0x4d4d536b;
constexpr UINT_PTR kHistoryUiRetry = 0x4d4d536c;
MmsResult PrepareHistoryReset(std::unique_ptr<SharedHistory::ResetTransaction>& out) {
    if (!history) return MMS_OK;
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(owner) || NativeObjectUiPending(owner)) return MMS_E_BUSY;
    if(owner)CancelViewportCameraGesture(*owner);
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
void QueueBusinessEvent(MMDApp* app, uint32_t kind, uint32_t version,
                        const void* bytes, std::size_t size,
                        MmsObjectRef object = {}) {
    if (!manager || owner != app) return;
    Boundary([&] {
        BusinessNotification event{kind, version, object, {}};
        if (bytes && size) {
            const auto* first = static_cast<const unsigned char*>(bytes);
            event.bytes.assign(first, first + size);
        }
        businessEvents.push_back(std::move(event));
        if (!PostMessageW(app->MainWindow(), kProjectEvent, 0, 0)) {
            businessEvents.pop_back();
            return MMS_E_FAILED;
        }
        return MMS_OK;
    });
}
const wchar_t* ProjectClosedPath(MMDApp* app) {
    return app && app->EnvFileName()[0] ? app->EnvFileName() : L"";
}
void NotifyProjectClosedEvent(MMDApp* app) {
    if (!manager || owner != app) return;
    Boundary([&] {
        MmsId id{}; unsigned char bytes[16]{};
        if (currentProject.projectId.size() == 32) {
            const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            for (size_t i = 0; i < 16; ++i)
                bytes[i] = static_cast<unsigned char>((digit(currentProject.projectId[2 * i]) << 4) |
                    digit(currentProject.projectId[2 * i + 1]));
            std::memcpy(&id, bytes, sizeof(id));
        }
        const auto path = ProjectClosedPath(app);
        MmsProjectEventV1 payload{Header<MmsProjectEventV1>(), id, 0, Text(Utf8(path))};
        businessEvents.push_back({MMS_EVENT_PROJECT_CLOSED, 1, {},
            std::vector<unsigned char>(reinterpret_cast<const unsigned char*>(&payload),
                                       reinterpret_cast<const unsigned char*>(&payload) + sizeof(payload))});
        if (!PostMessageW(app->MainWindow(), kProjectEvent, 0, 0)) {
            businessEvents.pop_back();
            return MMS_E_FAILED;
        }
        return MMS_OK;
    });
}
void NotifyLanguageChangedEvent(MMDApp* app) {
    if (!manager || owner != app) return;
    MmsCodeEventV1 payload{Header<MmsCodeEventV1>(), app->EnglishUI(), 0};
    QueueBusinessEvent(app, MMS_EVENT_LANGUAGE_CHANGED, 1, &payload, sizeof(payload));
}
bool FlushDeviceLoss(MMDApp* app) {
    if (!manager || owner != app) return true;
    if (closing || manager->InCallback()) return false;
    return manager->Render().FlushDeviceLost();
}
bool DeviceWorkPending() {
    return manager && (manager->Render().DeviceLossPending() || manager->DeviceEventsPending());
}
void WakeDeviceWork(MMDApp* app) {
    if (!manager || owner != app || closing || !DeviceWorkPending()) return;
    if (!PostMessageW(app->MainWindow(), kProjectEvent, 0, 0))
        SetTimer(app->MainWindow(), kProjectRetry, 16, nullptr);
}
bool FlushDeviceWork(MMDApp* app) {
    if (!FlushDeviceLoss(app)) return false;
    return !manager || owner != app || manager->FlushDeviceEvents();
}
void NotifyDeviceLostEvent(MMDApp* app) {
    if (!manager || owner != app || closing) return;
    if (!deviceLost) {
        deviceLost = true;
        manager->Render().MarkDeviceLost(deviceGeneration);
        manager->RecordDeviceLost(deviceGeneration);
    }
    FlushDeviceLoss(app);
    WakeDeviceWork(app);
}
void NotifyDeviceRestoredEvent(MMDApp* app) {
    if (!manager || owner != app || closing || !deviceLost || manager->InCallback() ||
        manager->Render().DeviceLossPending()) return;
    ++deviceGeneration;
    manager->Render().DeviceRestored(deviceGeneration);
    deviceLost = false;
    manager->RecordDeviceRestored(deviceGeneration);
    WakeDeviceWork(app);
}
void NotifyObjectRemovedEvent(MMDApp* app, const void* native) {
    if (!manager || owner != app || !native) return;
    MmsObjectRef object{};
    if (!FindSceneObjectIdentity(native, object) || (!object.id.lo && !object.id.hi)) return;
    QueueBusinessEvent(app, MMS_EVENT_OBJECT_REMOVED, 0, nullptr, 0, object);
}
void NotifyDpiChangedEvent(MMDApp* app, unsigned dpi) {
    if (!manager || owner != app || !dpi) return;
    MmsCodeEventV1 payload{Header<MmsCodeEventV1>(), dpi, 0};
    QueueBusinessEvent(app, MMS_EVENT_DPI_CHANGED, 1, &payload, sizeof(payload));
}
constexpr UINT kRefresh = WM_APP + 0x6a;
void LogMessage(const std::string& s) { runtime_log::Write("PLUGIN_ABI %s", s.c_str()); }
LRESULT CALLBACK Subclass(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_DPICHANGED) {
        const unsigned dpi = HIWORD(wp);
        if (owner && dpi) NotifyDpiChangedEvent(owner, dpi);
    }
    if (message == WM_CLOSE && ui && ui->DeferClose(wp, lp)) return 0;
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
        if(manager && !closing && !manager->InCallback() && owner && owner->WindowLayoutReady() && !owner->SceneMutationInProgress() && FlushDeviceWork(owner)) {
            auto remaining=projectEvents.size();
            while(remaining-- && !projectEvents.empty()) {
                auto event=std::move(projectEvents.front());projectEvents.pop_front();
                MmsProjectEventV1 payload{Header<MmsProjectEventV1>(),event.id,0,Text(event.path)};
                manager->Event(event.kind,{&payload,sizeof(payload)},1);
            }
            remaining=businessEvents.size();
            while(remaining-- && !businessEvents.empty()) {
                if (!FlushDeviceWork(owner)) break;
                auto event=std::move(businessEvents.front());businessEvents.pop_front();
                manager->Event(event.kind,
                    {event.bytes.empty()?nullptr:static_cast<void*>(event.bytes.data()),event.bytes.size()},
                    event.version, event.object);
            }
        } else if(owner && !closing && (!projectEvents.empty() || !businessEvents.empty() ||
            DeviceWorkPending()))
            SetTimer(hwnd,kProjectRetry,16,nullptr);
        return 0;
    }
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(hwnd, Subclass, id); Shutdown(); }
    return DefSubclassProc(hwnd, message, wp, lp);
}
}
void PluginNotifyProjectClosed(MMDApp* app) { NotifyProjectClosedEvent(app); }
void PluginNotifyLanguageChanged(MMDApp* app) { NotifyLanguageChangedEvent(app); }
void PluginNotifyDeviceLost(MMDApp* app) { NotifyDeviceLostEvent(app); }
void PluginNotifyDeviceRestored(MMDApp* app) { NotifyDeviceRestoredEvent(app); }
bool PluginPrepareDeviceReset(MMDApp* app) {
    NotifyDeviceLostEvent(app);
    return FlushDeviceLoss(app);
}
void PluginNotifyObjectRemoved(MMDApp* app, const void* native) { NotifyObjectRemovedEvent(app, native); }
void PluginNotifyDpiChanged(MMDApp* app, unsigned dpi) { NotifyDpiChangedEvent(app, dpi); }
void PluginFrameBegin(MMDApp* app) {
    if (!manager || !app || owner != app || manager->InCallback()) return;
    if (!FlushDeviceWork(app)) { WakeDeviceWork(app); return; }
    manager->Render().FrameBegin();
}
void PluginFrameEnd(MMDApp* app) {
    if (!manager || !app || owner != app || manager->InCallback()) return;
    manager->Render().FrameEnd();
}
void PluginFramePresented(MMDApp* app) {
    if (!manager || !app || owner != app || manager->InCallback()) return;
    auto* renderer = app->Renderer();
    if (!renderer || !renderer->device) return;
    IDirect3DSurface9* backbuffer = nullptr;
    if (FAILED(renderer->device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer) return;
    D3DSURFACE_DESC description{};
    const bool described = SUCCEEDED(backbuffer->GetDesc(&description));
    backbuffer->Release();
    manager->Render().FramePresented(described ? description.Width : 0,
        described ? description.Height : 0, described ? unsigned(description.Format) : 0);
}
void PluginNotifyPropertyChanged(MMDApp* app, const std::string& propertyId, uint64_t revision) {
    if (!manager || !app || propertyId.empty() || !revision) return;
    propertyProbeIds.push_back(propertyId);
    const std::string& stored = propertyProbeIds.back();
    MmsPropertyEventV1 payload{Header<MmsPropertyEventV1>(),
        {stored.data(), stored.size()}, {}, revision};
    QueueBusinessEvent(app, MMS_EVENT_PROPERTY_CHANGED, 1, &payload, sizeof(payload));
}

static void InitializeRuntime(MMDApp* app, bool migrationTest) {
    if (build_profile::original || !app || manager) return;
    const auto initialized = Boundary([&] {
        if (runtimeBinding == UINT64_MAX) throw std::runtime_error("plugin runtime binding exhausted");
        ++runtimeBinding;
        owner = app; closing = false;
        deviceGeneration = 1; deviceLost = false;
        businessEvents.clear(); propertyProbeIds.clear();
        historyUiPending = false; pendingEffects = HistoryEffects::None; historyUiGeneration = 0;
        const auto data = local_data::File(L"plugins");
        if (data.empty()) throw std::runtime_error("plugin data directory unavailable");
        std::wstring module(32768, 0); auto n = GetModuleFileNameW(nullptr, module.data(), DWORD(module.size())); if (!n || n >= module.size()) throw std::runtime_error("process path unavailable"); module.resize(n);
        manager = std::make_unique<Manager>(data, LogMessage, [window = app->MainWindow()] { PostMessageW(window, kRefresh, 0, 0); }, false, CreateSceneStore(app), true);
        manager->SetRefreshGuard([app] { return !closing && app->WindowLayoutReady() && !app->SceneMutationInProgress(); });
        if (manager->Storage().Prepare(currentProject.records) != MMS_OK || !manager->Storage().Commit()) throw std::runtime_error("initialize plugin project data");
        history = std::make_unique<SharedHistory>(manager->Storage(), [app] {
            if (closing || owner != app) return MMS_E_NOT_READY;
            if (SceneSaveBlocksEdits()) return MMS_E_BUSY;
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
        // This trusted signal only records a revision and wakes the existing
        // dispatcher. Property callbacks run later at its normal safe point.
        if (history->SetPublicationSignal({manager.get(), [](void* context, uint64_t revision) noexcept {
            static_cast<Manager*>(context)->NotifyHistoryPublication(revision);
        }}) != MMS_OK) throw std::runtime_error("initialize property history refresh signal");
        manager->Ui().Refresh().PropertyRefresh().SetHistoryAvailable(true);
#if !MMD_ORIGINAL_PROFILE
        const auto historyPreference=system_settings_ui::HistoryPreference();
        if (history->Configure(historyPreference.recordAdjustments ? SharedHistory::RecordingMode::AllOperations :
            SharedHistory::RecordingMode::ExplicitOnly,historyPreference.steps,std::uint64_t(historyPreference.memoryMiB)*1024*1024) != MMS_OK)
            throw std::runtime_error("initialize history recording mode and limits");
        if (migrationTest) {
            historyCommandHost = std::make_unique<HistoryCommandHost>(*app, *history);
            if (manager->AttachEditHost(*historyCommandHost) != MMS_OK) throw std::runtime_error("attach common edit history");
            migrationBinding = std::make_unique<NativePoseTestBinding>(*app, *history);
            manager->SetEditPublished([app] {
                if (closing || owner != app || !history) return;
                // Exactly this publication's frozen mask decides the project
                // scalars. A renderer-only traversal must leave a saved scene
                // clean; physics invalidation stays an independent fact.
                const auto effects = history->LastPublicationEffects();
                if (HasHistoryEffect(effects, HistoryEffects::ProjectData)) {
                    app->SceneModified() = 1; app->RecordedFrameCountByte1() = 1;
                }
                if (history->LastPublicationRequiresPhysicsReset()) app->PhysicsResetPending() = 1;
                // The deferred controls and readouts consume the accumulated mask
                // at the host's normal safe point.
                ++historyUiGeneration;
                pendingEffects = pendingEffects | effects;
                historyUiPending = true;
                PostMessageW(app->MainWindow(), kRefresh, 0, 0);
            });
        }
#endif
        ui = std::make_unique<NativeUi>(manager->Ui(), app->MainWindow(), LogMessage);
        ui->SetCommandStateChanged([app](uint64_t revision) {
            MmsRevisionEventV1 payload{Header<MmsRevisionEventV1>(), revision};
            QueueBusinessEvent(app, MMS_EVENT_COMMAND_STATE_CHANGED, 1, &payload, sizeof(payload));
        });
        ui->SetLayoutPublished([app](const LayoutReservations& areas) -> MmsResult {
            if (closing || owner != app || !app->WindowLayoutReady() || app->SceneMutationInProgress()) return MMS_E_NOT_READY;
            toolbarHeight = areas.toolbarHeight;
            propertyAreaHeight = areas.propertyHeight;
            leftAreaWidth = areas.leftWidth;
            rightAreaWidth = areas.rightWidth;
            // Only the geometry that depends on the plugin strip changes.
            // Do not enter HandleWindowSize, FrameDriver or scene evaluation.
            return Boundary([&] {
                mikumikustudio::ui::LayoutBatch batch;
                RefreshMainWindowViewport(app);
                LayoutViewportPanels(app);
                const bool applied = batch.Commit();
                RECT damage{}; GetClientRect(app->MainWindow(), &damage);
                damage.left = app->SidebarWidth() + 9; damage.bottom = std::max(0L, damage.bottom - 157);
                mikumikustudio::ui::RepaintLayout(app->MainWindow(), batch.Changes(), damage);
                return applied ? MMS_OK : MMS_E_FAILED;
            });
        });
        if (!SetWindowSubclass(app->MainWindow(), Subclass, 0x4d4d5301, 0)) throw std::runtime_error("plugin lifetime hook failed");
        // The focused host test supplies an explicit package tree; production
        // always resolves PLUGINS next to the executable.
        const auto pluginRoot = [] {
            wchar_t value[32768]{};
            const DWORD length = GetEnvironmentVariableW(L"MMS_PLUGIN_TEST_DIR", value, _countof(value));
            return length && length < _countof(value) ? fs::path(value)
                : fs::path();
        }();
        manager->Load(pluginRoot.empty() ? fs::path(module).parent_path() / L"PLUGINS" : pluginRoot);
        Refresh(app); return MMS_OK;
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
    // Native painting/layout and popup loops can pump messages without plugin
    // callback permissions. Retain their adapter until the outermost return;
    // STATE remains usable and the pending close is posted at that boundary.
    if (ui && ui->DeferClose(0, 0)) return;
    if (manager && manager->ExecutingCallback()) return;
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(owner)) return;
#endif
    if (closing) return; closing = true;
#if !MMD_ORIGINAL_PROFILE
    if(owner)CancelViewportCameraGesture(*owner);
#endif
    if (owner && owner->MainWindow()) {
        KillTimer(owner->MainWindow(), kHistoryUiRetry);
        KillTimer(owner->MainWindow(), kProjectRetry);
    }
    CancelProjectLoad(owner);
    if (manager) manager->Ui().Refresh().PropertyRefresh().SetHistoryAvailable(false);
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
    if (history) history->SetPublicationSignal({});
    history.reset(); manager.reset(); projectEvents.clear();
    businessEvents.clear(); propertyProbeIds.clear(); deviceLost = false; deviceGeneration = 1;
    toolbarHeight = propertyAreaHeight = leftAreaWidth = rightAreaWidth = 0;
    owner = nullptr; historyUiPending = false;
    pendingEffects = HistoryEffects::None; historyUiGeneration = 0;
}
SharedHistory* NativeHistory(MMDApp* app) noexcept { return app == owner && !closing ? history.get() : nullptr; }
void RequestBoundHistoryUiRefresh(MMDApp* app, HistoryEffects effects) noexcept {
    if (!app || app != owner || closing || !app->MainWindow()) return;
    // The mask is accumulated, never applied here: this only schedules the
    // existing message-boundary consumer and cannot mark the project dirty.
    ++historyUiGeneration;
    pendingEffects = pendingEffects | effects;
    historyUiPending = true;
    PostMessageW(app->MainWindow(), kRefresh, 0, 0);
}
void Layout(MMDApp* app) {
    if (!ui || app != owner) return;
    Boundary([&] {
        RECT rect{}; GetClientRect(app->MainWindow(), &rect);
        int left = app->SidebarWidth() + 9;
        const bool shown = !app->FullscreenMode() && !app->FloatingWindow();
        const int maximum = std::max(0L, (rect.bottom - 217) / 3);
        const int availableWidth = std::max(0L, rect.right - left);
        ui->SetSideAreas({left, 25, rect.right, std::max(25L, rect.bottom - 192)}, availableWidth / 3, shown);
        ui->SetPropertyArea(std::max(0L, rect.bottom - 160), maximum, shown);
        toolbarHeight = ui->Layout(left, availableWidth, maximum, shown);
        ui->DetectCommandStateChanges();
        propertyAreaHeight = ui->PropertyAreaHeight();
        leftAreaWidth = ui->LeftAreaWidth();
        rightAreaWidth = ui->RightAreaWidth();
        return MMS_OK;
    });
}
int ToolbarHeight(MMDApp* app) { return app && app == owner && !app->FullscreenMode() && !app->FloatingWindow() ? toolbarHeight : 0; }
int PropertyAreaHeight(MMDApp* app) { return app && app == owner && !app->FullscreenMode() && !app->FloatingWindow() ? propertyAreaHeight : 0; }
int LeftAreaWidth(MMDApp* app) { return app && app == owner && !app->FullscreenMode() && !app->FloatingWindow() ? leftAreaWidth : 0; }
int RightAreaWidth(MMDApp* app) { return app && app == owner && !app->FullscreenMode() && !app->FloatingWindow() ? rightAreaWidth : 0; }
void Refresh(MMDApp* app) {
    if (!ui || app != owner || refreshing || closing) return;
    if (manager && manager->InCallback()) { SetTimer(app->MainWindow(), kHistoryUiRetry, 16, nullptr); return; }
    refreshing = true;
    const auto old = toolbarHeight, oldProperties = propertyAreaHeight, oldLeft = leftAreaWidth, oldRight = rightAreaWidth;
    Boundary([&] { ui->Refresh(); Layout(app);
        if (toolbarHeight != old || propertyAreaHeight != oldProperties || leftAreaWidth != oldLeft || rightAreaWidth != oldRight)
            HandleWindowSize(app);
#if !MMD_ORIGINAL_PROFILE
        if ((historyUiPending || pendingEffects != HistoryEffects::None) && history && manager &&
            !manager->InCallback() && !app->SceneMutationInProgress()) {
            // Take this batch out before consuming it. A request enqueued by the
            // consumer (or by a nested publication) must survive to the next safe
            // point instead of being cleared by a tail assignment.
            const auto batch = pendingEffects;
            const auto generation = historyUiGeneration;
            pendingEffects = HistoryEffects::None;
            bool refreshed = false;
            try {
                refreshed = RefreshBoundHistoryUi(*app, *history, batch);
            } catch (...) {
                // Keep this batch and anything requested while it ran.
                pendingEffects = pendingEffects | batch;
                throw;
            }
            if (refreshed) {
                // A new request during consumption re-armed the flag itself.
                historyUiPending = historyUiGeneration != generation;
            } else {
                // Refused or failed control writes stay retryable.
                pendingEffects = pendingEffects | batch;
                historyUiPending = true;
            }
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

struct ProjectSave::Impl {
    enum class Stage { Captured, Publishing, Published, Failed, Completed };
    MMDApp* app = nullptr;
    MMDApp* runtimeOwner = nullptr;
    Manager* capturedManager = nullptr;
    DWORD thread = GetCurrentThreadId();
    std::uint64_t binding = 0;
    PluginProject data;
    StoredRecords storeRecords;
    std::wstring destination;
    bool copy = false;
    std::atomic<Stage> stage{Stage::Captured};
};
ProjectSave::ProjectSave(MMDApp* app) : impl_(std::make_unique<Impl>()) {
    if (!app || (app->MainWindow() && GetWindowThreadProcessId(app->MainWindow(), nullptr) != impl_->thread))
        throw std::runtime_error("project save capture requires the UI thread");
    if(app->SceneMutationInProgress() || (manager && owner==app && manager->InCallback()))throw std::runtime_error("project save during callback or scene mutation");
    auto* journal = owner == app ? history.get() : nullptr;
#if !MMD_ORIGINAL_PROFILE
    if (!journal) journal = BoundNativePoseHistory(app);
    if (NativeObjectOperationActive(app) || NativeObjectUiPending(app)) throw std::runtime_error("object UI refresh pending");
#endif
    // Capture may run under the save edit gate, so use the read-only admission
    // probe. A pending applied native edit can already have temporary live
    // values which are still cancellable and must never reach a saved pair.
    if (journal && journal->CheckNativeNavigation() != MMS_OK)
        throw std::runtime_error("project save during a pending history transaction");
    if(currentProject.projectId.empty())currentProject.projectId=StorageId();
    impl_->app = app; impl_->data = currentProject;
    impl_->runtimeOwner = owner; impl_->capturedManager = manager.get(); impl_->binding = runtimeBinding;
    if (manager && owner == app) {
        const auto r = manager->Storage().Capture(impl_->data.records);
        if (r) throw std::runtime_error("plugin serializer save returned " + std::to_string(r));
        impl_->storeRecords=impl_->data.records;
    }
}
ProjectSave::~ProjectSave() = default;
bool ProjectSave::PublishDisk(const wchar_t* staged, const wchar_t* destination, bool copy, bool keepPrevious, bool numberedManualVersions) noexcept {
    if (!staged || !*staged || !destination || !*destination) return false;
    auto expected = Impl::Stage::Captured;
    if (!impl_->stage.compare_exchange_strong(expected, Impl::Stage::Publishing)) return false;
    try {
        impl_->destination = destination; impl_->copy = copy;
        // An incremental save publishes a fresh unique path; it must neither
        // replace an existing project nor shift the .pmm1 history chain.
        PublishProjectPair(fs::absolute(destination),fs::absolute(staged),impl_->data,
            copy || numberedManualVersions,{},keepPrevious && !numberedManualVersions);
        impl_->stage.store(Impl::Stage::Published, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        runtime_log::Write("PLUGIN_ABI project save failed: %s", e.what());
        try { RecoverProjectPair(fs::absolute(destination)); }
        catch (const std::exception& recovery) { runtime_log::Write("PLUGIN_ABI project recovery pending: %s", recovery.what()); }
        catch (...) { runtime_log::Write("PLUGIN_ABI project recovery pending after an unknown exception"); }
    } catch (...) {
        runtime_log::Write("PLUGIN_ABI project save failed with an unknown exception");
    }
    impl_->stage.store(Impl::Stage::Failed, std::memory_order_release);
    return false;
}
bool ProjectSave::Complete(const wchar_t* destination, bool copy) noexcept {
    if (GetCurrentThreadId() != impl_->thread || !destination) return false;
    const auto stage = impl_->stage.load(std::memory_order_acquire);
    if (stage != Impl::Stage::Published && stage != Impl::Stage::Completed) return false;
    if (copy != impl_->copy || _wcsicmp(destination, impl_->destination.c_str())) return false;
    if (stage == Impl::Stage::Completed) return true;
    // Compare the runtime token before following any captured native pointer.
    // In particular, allocator address reuse after unload/reload is not enough
    // to admit an old snapshot into the new manager's last-saved baseline.
    if (runtimeBinding != impl_->binding || owner != impl_->runtimeOwner || manager.get() != impl_->capturedManager ||
        (manager && (owner != impl_->app || closing || manager->InCallback() || manager->Storage().Prepared()))) return false;
    if (!copy) {
        // Saved is a last-saved byte baseline, not prepareLoad/commitLoad.
        // Newer live edits and the common journal remain untouched.
        if (manager) manager->Storage().Saved(impl_->storeRecords);
        currentProject = std::move(impl_->data);
    }
    impl_->stage.store(Impl::Stage::Completed, std::memory_order_release);
    return true;
}
bool ProjectSave::Publish(const wchar_t* staged, const wchar_t* destination, bool copy) {
    if (GetCurrentThreadId() != impl_->thread || !PublishDisk(staged, destination, copy)) return false;
    Complete(destination, copy);
    return true;
}
bool PrepareProjectLoad(MMDApp* app, const wchar_t* path) {
    if (build_profile::original) return true;
#if !MMD_ORIGINAL_PROFILE
    if (NativeObjectOperationActive(app) || NativeObjectUiPending(app)) return false;
#endif
    if (loadingProject || (manager && manager->InCallback())) return false;
    bool mainOpenFailed = false;
    DWORD mainOpenError = ERROR_SUCCESS;
    try {
        auto data=ReadPluginProject(fs::absolute(path));
        loadingMain=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(loadingMain==INVALID_HANDLE_VALUE){
            mainOpenError=GetLastError();
            mainOpenFailed=true;
            throw std::runtime_error("cannot lock PMM for loading");
        }
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
        if(mainOpenFailed){
            const wchar_t* formats[]={L"%ls\nエラーコード: %lu",L"%ls\nError code: %lu",L"%ls\n错误码：%lu"};
            wchar_t detail[512]{};
            _snwprintf_s(detail,_countof(detail),_TRUNCATE,formats[app->EnglishUI()<=2?app->EnglishUI():1],path,static_cast<unsigned long>(mainOpenError));
            if(app->MainWindow())ShowUiMessage(app,app->MainWindow(),UiMessage::OpenProjectError,MB_OK|MB_ICONERROR,detail);
            return false;
        }
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
    if (manager) manager->AdvanceRefreshEpoch();
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
#if !MMD_ORIGINAL_PROFILE
    if(app)CancelViewportCameraGesture(*app);
#endif
    CancelProjectLoad(app);
    std::unique_ptr<SharedHistory::ResetTransaction> reset;
    if(manager && owner==app){
        if (PrepareHistoryReset(reset) != MMS_OK) { LogMessage("history project reset refused"); return false; }
        const auto r=manager->Storage().Prepare({});
        if(r){LogMessage("plugin project reset refused");return false;}
        if (reset && reset->Commit()!=MMS_OK) { manager->Storage().Cancel(); LogMessage("history project reset refused"); return false; }
        manager->AdvanceRefreshEpoch();
        if(!manager->Storage().Commit()){LogMessage("plugin project reset refused");return false;}
    }
    if(!currentProject.projectId.empty()) NotifyProjectClosedEvent(app);
    currentProject={};QueueProjectEvent(app,MMS_EVENT_SCENE_RESET,L"");return true;
}
void PluginProjectSaved(MMDApp* app,const wchar_t* path){QueueProjectEvent(app,MMS_EVENT_PROJECT_SAVED,path);}
bool DeletePluginBackup(const wchar_t* path) {
    if (!path || !*path) return false;
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
