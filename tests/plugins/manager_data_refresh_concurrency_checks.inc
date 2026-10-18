#include "data_refresh_probe.h"

namespace manager_data_refresh_concurrency_checks {
static void StateProviderOwner() {
    state_refresh_checks::Fixture x;
    struct StateContext { state_refresh_checks::Fixture* fixture; MmsUiTargetRefV1 providerTarget{}; bool requester = false, provider = false;
        MmsResult requesterRequest = MMS_OK, providerRequest = MMS_OK; } state{&x};
    auto foreign = x.registry.Api(x.foreign);
    MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>();
    command.info.id = S("example.foreign.state"); command.info.ownerPlugin = S("example.foreign");
    command.info.label = S("Foreign state"); command.context = &state;
    command.getState = [](void* context, MmsObjectRef, MmsCommandStateV1* out) -> MmsResult {
        auto& s = *static_cast<StateContext*>(context); auto& f = *s.fixture;
        s.requester = f.queue.ReadOnlyCause(f.client); s.provider = f.queue.ReadOnlyCause(f.other);
        auto request = f.RequestState(); MmsHandle unchanged = 777;
        s.requesterRequest = f.api.request(f.api.context, &request, &unchanged);
        request.target = s.providerTarget;
        s.providerRequest = f.otherApi.request(f.otherApi.context, &request, &unchanged);
        if (unchanged != 777) return MMS_E_FAILED;
        *out = {Header<MmsCommandStateV1>(), 1, 0, 1, 0, {}}; return MMS_OK;
    };
    command.invoke = [](void*, MmsObjectRef, MmsSpan, MmsHandle) -> MmsResult { return MMS_OK; };
    MmsHandle registered = 0; CHECK(foreign.registerCommand(foreign.context, &command, &registered) == MMS_OK);
    struct Window { HWND value = nullptr; ~Window() { if (value) DestroyWindow(value); } } providerWindow;
    providerWindow.value = CreateWindowExW(0, L"BUTTON", L"provider", WS_CHILD | WS_VISIBLE, 360, 180, 180, 24,
        x.parent, nullptr, GetModuleHandleW(nullptr), nullptr); CHECK(providerWindow.value != nullptr);
    MmsUiElementV1 providerElement{}; providerElement.h = Header<MmsUiElementV1>();
    providerElement.id = S("example.foreign.button"); providerElement.ownerPlugin = S("example.foreign");
    providerElement.parentId = S("mms.toolbar.main.extensions"); providerElement.commandId = command.info.id; providerElement.kind = MMS_UI_BUTTON;
    MmsHandle providerHandle = 0; CHECK(foreign.registerElement(foreign.context, &providerElement, &providerHandle) == MMS_OK);
    auto element = x.Button("example.refresh.button"); element.commandId = command.info.id;
    CHECK(x.ui.updateElement(x.ui.context, x.element, &element) == MMS_OK);
    x.presentations.push_back({providerHandle, x.foreign.id, 14, providerWindow.value, 96, 360, 180, 180, 24, true, true});
    x.queue.SyncTargets(x.presentations);
    MmsUiRefreshTargetV1 providerTarget{}; providerTarget.h = Header<MmsUiRefreshTargetV1>();
    CHECK(x.otherApi.getTarget(x.otherApi.context, providerHandle, &providerTarget) == MMS_OK); state.providerTarget = providerTarget.target;
    const auto ticket = x.Submit(x.RequestState()); x.queue.Drain(); x.StateDone(ticket);
    CHECK(state.requester && state.provider && state.requesterRequest == MMS_E_BUSY && state.providerRequest == MMS_E_BUSY);
    CHECK(!x.queue.ReadOnlyCause() && !x.queue.ReadOnlyCause(x.client) && !x.queue.ReadOnlyCause(x.other));
}
struct Probe {
    DataRefreshProbeBeginFn begin = nullptr;
    DataRefreshProbePublishFn publish = nullptr;
    DataRefreshProbeRequestFn request = nullptr;
    DataRefreshProbeQueryFn query = nullptr;
    DataRefreshProbeSimpleFn releaseTicket = nullptr, enqueueNormal = nullptr;
    DataRefreshProbeSetPrepareHookFn setHook = nullptr;
    explicit Probe(HMODULE module) {
        begin = reinterpret_cast<DataRefreshProbeBeginFn>(GetProcAddress(module, "ProbeBegin"));
        publish = reinterpret_cast<DataRefreshProbePublishFn>(GetProcAddress(module, "ProbePublish"));
        request = reinterpret_cast<DataRefreshProbeRequestFn>(GetProcAddress(module, "ProbeRequest"));
        query = reinterpret_cast<DataRefreshProbeQueryFn>(GetProcAddress(module, "ProbeQuery"));
        releaseTicket = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeReleaseTicket"));
        enqueueNormal = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeEnqueueNormal"));
        setHook = reinterpret_cast<DataRefreshProbeSetPrepareHookFn>(GetProcAddress(module, "ProbeSetPrepareHook"));
        CHECK(begin && publish && request && query && releaseTicket && enqueueNormal && setHook);
    }
    DataRefreshProbeState Read() const {
        DataRefreshProbeState state{}; state.size = sizeof(state); CHECK(query(&state) == MMS_OK); return state;
    }
};
static void IndependentOwners(const fs::path& root, const fs::path& library) {
    const auto directory = root / "data-independent-owners", plugins = directory / "PLUGINS";
    const auto packageA = plugins / "a", packageB = plugins / "b";
    fs::create_directories(packageA); fs::create_directories(packageB);
    fs::copy_file(library, packageA / "data-owner-a.dll"); fs::copy_file(library, packageB / "data-owner-b.dll");
    Write(packageA / "plugin.json", ManifestText("org.example.data-owner-a", "data-owner-a.dll"));
    Write(packageB / "plugin.json", ManifestText("org.example.data-owner-b", "data-owner-b.dll"));
    std::vector<std::string> logs;
    const auto log = [&](const std::string& value) { logs.push_back(value); };
    const auto count = [&](const char* owner, const char* marker) { return std::count_if(logs.begin(), logs.end(),
        [&](const auto& value) { return value.find(owner) != std::string::npos && value.find(marker) != std::string::npos; }); };
    Manager manager(directory / "user", log); manager.Load(plugins);
    CHECK(manager.Status().size() == 2);
    for (const auto& status : manager.Status()) CHECK(status.stage == "started");
    const auto moduleA = GetModuleHandleW(L"data-owner-a.dll"), moduleB = GetModuleHandleW(L"data-owner-b.dll");
    CHECK(moduleA && moduleB && moduleA != moduleB); Probe a(moduleA), b(moduleB);
    manager_refresh_checks::Window window;
    auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, log);
    native->Refresh(); CHECK(native->Layout(10, 500, 180, true) > 0);
    CHECK(a.begin(DATA_PROBE_BUTTON) == MMS_OK && b.begin(DATA_PROBE_BUTTON) == MMS_OK);
    CHECK(a.publish(13) == MMS_OK && b.publish(7) == MMS_OK);
    struct Hook {
        DataRefreshProbeSimpleFn enqueue; DWORD uiThread, workerThread = 0;
        unsigned calls = 0; MmsResult result = MMS_E_FAILED;
    } hook{b.enqueueNormal, GetCurrentThreadId()};
    CHECK(a.setHook([](void* context) {
        auto& h = *static_cast<Hook*>(context); ++h.calls;
        // B is a different loaded DLL with its own real Host and service tables.
        // Its worker enters the host while A's DATA prepare is still active.
        std::thread worker([&] { h.workerThread = GetCurrentThreadId(); h.result = h.enqueue(); }); worker.join();
    }, &hook) == MMS_OK);
    const auto revision = manager.Ui().Revision();
    CHECK(a.request(MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT, DATA_PROBE_ENQUEUE) == MMS_OK);
    DataRefreshProbeState first{}, second{}; const auto until = GetTickCount64() + 3000;
    do {
        // Dispatch the actual Manager refresh/task wake messages. No direct
        // access to a forged queue/client replaces the manager safety boundary.
        manager_refresh_checks::Pump(10); first = a.Read(); second = b.Read();
    } while ((first.tasks[3].calls != 1 || second.tasks[2].calls != 1) && GetTickCount64() < until);
    CHECK(hook.calls == 1 && hook.result == MMS_OK && hook.workerThread && hook.workerThread != hook.uiThread);
    CHECK(first.ticketState.status == MMS_UI_REFRESH_COMPLETED && first.ticketState.result == MMS_OK && first.prepareCount == 1);
    CHECK(first.directMutation == MMS_E_BUSY && first.recursiveData == MMS_E_BUSY && first.recursiveState == MMS_E_BUSY);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(first.tasks[i].calls == 1 && first.tasks[i].mutate == MMS_E_BUSY &&
            first.tasks[i].dataRequest == MMS_E_BUSY && first.tasks[i].stateRequest == MMS_E_BUSY);
        if (i < 2) CHECK(first.tasks[i].enqueueChild == MMS_OK);
    }
    for (unsigned i : {0u, 2u}) {
        CHECK(second.tasks[i].calls == 1 && second.tasks[i].mutate == MMS_OK &&
            second.tasks[i].dataRequest == MMS_OK && second.tasks[i].stateRequest == MMS_OK);
    }
    CHECK(second.tasks[0].enqueueChild == MMS_OK && !second.tasks[1].calls && !second.tasks[3].calls);
    CHECK(!second.menu && second.prepareCount == 0 && second.sourceRevision == 1 && manager.Ui().Revision() >= revision + 3);
    CHECK(count("org.example.data-owner-a", "DATA_PROBE_TASK_CANCEL") == 1 && count("org.example.data-owner-a", "DATA_PROBE_TASK_RUN") == 3);
    CHECK(count("org.example.data-owner-b", "DATA_PROBE_TASK_CANCEL") == 0 && count("org.example.data-owner-b", "DATA_PROBE_TASK_RUN") == 2);
    CHECK(a.setHook(nullptr, nullptr) == MMS_OK && a.releaseTicket() == MMS_OK);
    first = a.Read(); second = b.Read();
    const auto views = [&](const char* owner, const DataRefreshProbeState& state) {
        const auto destroyed = count(owner, "DATA_PROBE_VIEW_DESTROY");
        std::printf("DATA_OWNER_VIEWS owner=%s created=%u destroyed=%u logged=%lld window=%p\n", owner,
            state.panelCreated, state.panelDestroyed, static_cast<long long>(destroyed), reinterpret_cast<void*>(state.panelWindow));
        CHECK(state.panelCreated >= state.panelDestroyed && destroyed == state.panelDestroyed);
        CHECK(state.panelCreated == state.panelDestroyed ? !state.panelWindow : IsWindow(reinterpret_cast<HWND>(state.panelWindow)) != FALSE);
    };
    views("org.example.data-owner-a", first); views("org.example.data-owner-b", second);
    CHECK(manager.Stop());
    CHECK(GetModuleHandleW(L"data-owner-a.dll") == nullptr && GetModuleHandleW(L"data-owner-b.dll") == nullptr);
    // Only visible panels are mounted. An inactive owner's panel may never
    // exist; compare the real creation count, including any earlier rebuild.
    CHECK(count("org.example.data-owner-a", "DATA_PROBE_VIEW_DESTROY") == first.panelCreated &&
        count("org.example.data-owner-b", "DATA_PROBE_VIEW_DESTROY") == second.panelCreated);
    CHECK(count("org.example.data-owner-a", "DATA_PROBE_DESTROY") == 1 && count("org.example.data-owner-b", "DATA_PROBE_DESTROY") == 1);
    native.reset(); CHECK(manager.Stop());
}
} // namespace manager_data_refresh_concurrency_checks

// Include after refresh/state fixtures, ManifestText/Write and manager_refresh_checks.
static void ManagerDataRefreshConcurrencyChecks(const fs::path& root, const fs::path& library) {
    manager_data_refresh_concurrency_checks::StateProviderOwner();
    manager_data_refresh_concurrency_checks::IndependentOwners(root, library);
}
