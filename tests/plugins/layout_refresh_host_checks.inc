// The actual MMS integration and independently loaded C-ABI fixture. No
// replacement NativeUi, layout hook, geometry publication or scene driver.
#include "data_refresh_probe.h"
#include "../../src/plugins/native_pose_scope.hpp"
#include <iterator>
namespace layout_refresh_host_checks {
static DataRefreshProbeState Read(DataRefreshProbeQueryFn query) {
    DataRefreshProbeState state{}; state.size = sizeof(state);
    Check(query(&state) == MMS_OK, "read actual layout plugin state"); return state;
}
static void Pump() {
    MSG message{}; unsigned count = 0;
    while (count++ < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Check(message.message != WM_QUIT, "layout refresh does not close the real host");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
struct SettingsBytes {
    bool exists = false; std::vector<unsigned char> bytes;
    bool operator==(const SettingsBytes& other) const { return exists == other.exists && bytes == other.bytes; }
};
static SettingsBytes Settings() {
    wchar_t path[32768]{}; Check(GetModuleFileNameW(nullptr, path, 32768) != 0, "locate isolated executable settings");
    const auto target = std::filesystem::path(path).parent_path() / "data" / "settings.json";
    std::error_code error; SettingsBytes result; result.exists = std::filesystem::exists(target,error);
    Check(!error,"read isolated settings file existence");
    if(result.exists) {
        std::ifstream file(target,std::ios::binary); Check(bool(file),"open existing isolated settings file");
        result.bytes = std::vector<unsigned char>(std::istreambuf_iterator<char>(file), {});
        Check(!file.bad(),"read isolated settings bytes without an I/O error");
    }
    return result;
}
static RECT Control(MMDApp* app, int id) {
    RECT rect{}; Check(GetWindowRect(GetDlgItem(app->MainWindow(),id),&rect),"read actual dependent viewport control geometry"); return rect;
}
static void Run(MMDApp* app, bool bound) {
    Check(app && app == g_Block && !app->PlaybackActive() && !app->FrameStepPlayback() && !GetCapture(),
        "actual layout host is stopped without input capture");
    Check(*ui_metrics::Output() != 0, "actual scene and physics counters are enabled before startup");
    wchar_t modelPath[32768]{};
    Check(GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL", modelPath, 32768) > 0, "layout integration has a real PMX source");
    LoadModelFile(app, modelPath, false);
    Check(app->ModelSlot(0) && mdl::Mdl(app->ModelSlot(0))->boneCount, "layout integration loads an actual model");
    if (bound) {
        plugins::Shutdown(); Check(plugins::InitializeHistoryTestRuntime(app), "bind actual journal for layout integration");
        Check(plugins::NativeHistory(app)->SetMode(plugins::SharedHistory::RecordingMode::AllOperations) == MMS_OK,
            "layout integration exercises all-operation history without recording layout");
    }
    auto* history = plugins::NativeHistory(app); Check(history && bool(plugins::BoundNativePoseHistory(app)) == bound, "actual edit binding matches layout test mode");
    const auto module = GetModuleHandleW(L"data-refresh-probe.dll"); Check(module != nullptr, "actual startup loads the C refresh plugin");
    const auto begin = reinterpret_cast<DataRefreshProbeBeginFn>(GetProcAddress(module, "ProbeBegin"));
    const auto request = reinterpret_cast<DataRefreshProbeRequestFn>(GetProcAddress(module, "ProbeRequest"));
    const auto query = reinterpret_cast<DataRefreshProbeQueryFn>(GetProcAddress(module, "ProbeQuery"));
    const auto release = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeReleaseTicket"));
    const auto visible = reinterpret_cast<MmsResult(MMS_CALL*)(uint32_t)>(GetProcAddress(module, "ProbeLayoutSetVisible"));
    Check(begin && request && query && release && visible, "layout plugin exports only private test controls over public host services");
    for (unsigned i = 0; i < 3; ++i) { Pump(); plugins::Refresh(app); }
    Check(begin(DATA_PROBE_BUTTON) == MMS_OK, "bind original mounted button for actual layout requests");
    const auto original = Read(query); Check(!original.editAvailable == !bound, "ordinary edit ABI remains closed while controlled binding is negotiated");
    const auto initialHeight = plugins::ToolbarHeight(app); const RECT initialView = app->ViewportRect();
    const RECT initialControl = Control(app,551);
    Check(initialHeight > 0, "actual plugin strip reserves viewport space");
    const auto file = Settings();
    for (uint32_t show : {0u, 1u}) {
        app->SceneModified()=0; app->PhysicsResetPending()=0;
        const auto frame = app->CurrentFrame(); const auto modified = app->SceneModified();
        const auto physics = app->PhysicsResetPending();
        const std::array<float,8> camera{app->CameraPositionX(),app->CameraPositionY(),app->CameraPositionZ(),
            app->CameraPitch(),app->CameraYaw(),app->CameraRoll(),app->CameraDistance(),app->CameraFov()};
        const auto rawModel = app->ModelSlot(0); const auto* model = mdl::Mdl(rawModel);
        const auto boneTable=model->boneTable; const auto boneCount=model->boneCount;
        const auto cameraPool=app->CameraKeys(); const auto lightPool=app->LightKeys();
        const std::vector<mdl::BoneRecord> bones(model->boneTable,model->boneTable+model->boneCount);
        const std::vector<mdl::CameraKey> cameraKeys(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
        const std::vector<mdl::LightKey> lightKeys(app->LightKeys(),app->LightKeys()+mdl::KeyPoolCapacity(app->LightKeys()));
        std::array<uint64_t,ui_metrics::Count> counters{};
        for (unsigned i=0;i<counters.size();++i) counters[i]=ui_metrics::counts[i].load();
        uint64_t revision=0; if(history) Check(history->Revision(revision)==MMS_OK,"capture common journal before layout");
        const auto undo=history?history->UndoCount():0,redo=history?history->RedoCount():0;
        const auto bytes=history?history->RetainedBytes():0;
        Check(visible(show)==MMS_OK && request(MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT,0)==MMS_OK,
            "real background request combines state layout repaint on the same target");
        const auto until=GetTickCount64()+5000; auto completed=Read(query);
        while ((completed.ticketState.status==MMS_UI_REFRESH_PENDING||completed.ticketState.status==MMS_UI_REFRESH_RUNNING)&&GetTickCount64()<until) {
            Pump(); completed=Read(query);
        }
        Check(completed.ticketState.status==MMS_UI_REFRESH_COMPLETED && completed.ticketState.result==MMS_OK && completed.ticketState.completedKinds==13,
            "actual Manager dispatch completes all requested layout stages");
        Check(completed.target.element==original.target.element && completed.target.instanceGeneration==original.target.instanceGeneration &&
            completed.target.projectEpoch==original.target.projectEpoch,"collapsed button restores through its original ABI identity");
        Check(show?plugins::ToolbarHeight(app)==initialHeight:plugins::ToolbarHeight(app)<initialHeight,
            "actual viewport reservation follows collapse and restoration without a full window resize");
        const auto currentView=app->ViewportRect();
        Check(show?EqualRect(&initialView,&currentView):!EqualRect(&initialView,&currentView),
            "actual scene display geometry responds to plugin strip height");
        const auto control=Control(app,551); Check(show?EqualRect(&initialControl,&control):!EqualRect(&initialControl,&control),
            "dependent native viewport control is actually moved and restored");
        D3DVIEWPORT9 viewport{};
        Check(SUCCEEDED(app->Renderer()->device->GetViewport(&viewport)) && viewport.X==DWORD(currentView.left) && viewport.Y==DWORD(currentView.top) &&
            viewport.Width==DWORD(currentView.right-currentView.left) && viewport.Height==DWORD(currentView.bottom-currentView.top),
            "actual D3D viewport agrees with the published display rectangle");
        Check(ui_metrics::counts[ui_metrics::FrameDriver].load()==counters[ui_metrics::FrameDriver] &&
            ui_metrics::counts[ui_metrics::Scene].load()==counters[ui_metrics::Scene] &&
            ui_metrics::counts[ui_metrics::Physics].load()==counters[ui_metrics::Physics] &&
            ui_metrics::counts[ui_metrics::Reset].load()==counters[ui_metrics::Reset] &&
            ui_metrics::counts[ui_metrics::Layout].load()==counters[ui_metrics::Layout],
            "refresh changes dependent geometry without full-layout driver scene physics or device reset work");
        const std::array<float,8> after{app->CameraPositionX(),app->CameraPositionY(),app->CameraPositionZ(),
            app->CameraPitch(),app->CameraYaw(),app->CameraRoll(),app->CameraDistance(),app->CameraFov()};
        Check(app->ModelSlot(0)==rawModel && model->boneTable==boneTable && model->boneCount==boneCount &&
            app->CameraKeys()==cameraPool && mdl::KeyPoolCapacity(cameraPool)==cameraKeys.size() &&
            app->LightKeys()==lightPool && mdl::KeyPoolCapacity(lightPool)==lightKeys.size(),"layout retains actual scene objects and key resources");
        Check(app->CurrentFrame()==frame && app->SceneModified()==modified && app->PhysicsResetPending()==physics && camera==after &&
            !std::memcmp(bones.data(),model->boneTable,bones.size()*sizeof(mdl::BoneRecord)) &&
            !std::memcmp(cameraKeys.data(),app->CameraKeys(),cameraKeys.size()*sizeof(mdl::CameraKey)) &&
            !std::memcmp(lightKeys.data(),app->LightKeys(),lightKeys.size()*sizeof(mdl::LightKey)) && Settings()==file,
            "layout refresh preserves live pose playhead project keys dirty physics state and settings bytes");
        if(history){uint64_t current=0;Check(history->Revision(current)==MMS_OK&&current==revision&&history->UndoCount()==undo&&history->RedoCount()==redo&&history->RetainedBytes()==bytes,
            "actual layout request does not create a native or plugin history record");}
        Check(release()==MMS_OK,"release original completed layout ticket before next request");
    }
    std::printf("PASS actual MMS LAYOUT integration mode=%s\n",bound?"bound":"ordinary");
}
}
