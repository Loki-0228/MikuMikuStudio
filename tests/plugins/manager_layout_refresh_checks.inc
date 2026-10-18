// Include after manager_data_refresh_checks.inc. Both instances are real DLLs
// with their own negotiated Host contexts, services and attached edit journal.
#include <array>

namespace manager_layout_refresh_checks {
struct Probe : manager_data_refresh_checks::Probe {
    DataRefreshProbeLayoutConfigureFn configure = nullptr;
    DataRefreshProbeLayoutQueryFn queryLayout = nullptr;
    DataRefreshProbeSimpleFn enqueueOnly = nullptr;
    DataRefreshProbeLayoutVisibleFn visible = nullptr;
    explicit Probe(HMODULE module) : manager_data_refresh_checks::Probe(module) {
        configure = reinterpret_cast<DataRefreshProbeLayoutConfigureFn>(GetProcAddress(module, "ProbeLayoutConfigure"));
        queryLayout = reinterpret_cast<DataRefreshProbeLayoutQueryFn>(GetProcAddress(module, "ProbeLayoutQuery"));
        enqueueOnly = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeEnqueueLayoutNormal"));
        visible = reinterpret_cast<DataRefreshProbeLayoutVisibleFn>(GetProcAddress(module, "ProbeLayoutSetVisible"));
        CHECK(configure && queryLayout && enqueueOnly && visible);
    }
    DataRefreshProbeLayoutState Layout() const {
        DataRefreshProbeLayoutState state{}; state.size = sizeof(state); CHECK(queryLayout(&state) == MMS_OK); return state;
    }
};
struct Workspace {
    HWND window = nullptr, tabs = nullptr;
    int shrink = 0;
    explicit Workspace(HWND parent) {
        for (auto child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t name[80]{}; GetClassNameW(child, name, 80);
            if (!std::wcscmp(name, L"MMS.PluginAbi.Workspace.V1")) window = child;
        }
        CHECK(window);
        for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t name[80]{}; GetClassNameW(child, name, 80);
            if (!std::wcscmp(name, WC_TABCONTROLW)) tabs = child;
        }
        CHECK(tabs && TabCtrl_GetItemCount(tabs) == 2);
        CHECK(SetWindowSubclass(window, Constrain, 0x1a71, reinterpret_cast<DWORD_PTR>(this)));
    }
    ~Workspace() { if (IsWindow(window)) RemoveWindowSubclass(window, Constrain, 0x1a71); }
    static LRESULT CALLBACK Constrain(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR p) {
        auto& self = *reinterpret_cast<Workspace*>(p);
        if (message == WM_WINDOWPOSCHANGING && self.shrink) {
            auto& position = *reinterpret_cast<WINDOWPOS*>(lp);
            if (!(position.flags & SWP_NOSIZE)) position.cx = (std::max)(100, position.cx - self.shrink);
        }
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    void Select(unsigned index) {
        TabCtrl_SetCurSel(tabs, index); NMHDR change{tabs, 0, TCN_SELCHANGE};
        SendMessageW(window, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&change));
        CHECK(TabCtrl_GetCurSel(tabs) == int(index));
    }
};
static void Denied(const DataRefreshProbeLayoutTaskState& task) {
    CHECK(task.calls == 1 && task.mutate == MMS_E_BUSY && task.dataRequest == MMS_E_BUSY &&
        task.stateRequest == MMS_E_BUSY && task.layoutRequest == MMS_E_BUSY);
    manager_data_refresh_checks::EditDenied(task.edit, 1);
}
static void Run(const fs::path& root, const fs::path& library, bool foreignProvider, bool resizeFails) {
    const auto directory = root / (resizeFails ? "manager-layout-failed" : foreignProvider ? "manager-layout-provider" : "manager-layout-owner");
    const auto plugins = directory / "PLUGINS", packageA = plugins / "a", packageB = plugins / "b";
    fs::create_directories(packageA); fs::create_directories(packageB);
    fs::copy_file(library, packageA / "layout-owner-a.dll"); fs::copy_file(library, packageB / "layout-owner-b.dll");
    Write(packageA / "plugin.json", ManifestText("org.example.layout-owner-a", "layout-owner-a.dll"));
    Write(packageB / "plugin.json", ManifestText("org.example.layout-owner-b", "layout-owner-b.dll"));
    std::vector<std::string> logs;
    const auto count = [&](const char* owner, const char* marker) {
        return std::count_if(logs.begin(), logs.end(), [&](const auto& value) {
            return value.find(owner) != std::string::npos && value.find(marker) != std::string::npos;
        });
    };
    Manager manager(directory / "user", [&](const auto& value) { logs.push_back(value); });
    manager_data_refresh_checks::AttachedEdits edits(manager);
    const auto historyRevision = edits.Revision(), historyBytes = edits.host->history.RetainedBytes();
    const auto undo = edits.host->history.UndoCount(), redo = edits.host->history.RedoCount();
    manager.Load(plugins); CHECK(manager.Status().size() == 2);
    for (const auto& status : manager.Status()) CHECK(status.stage == "started");
    const auto moduleA = GetModuleHandleW(L"layout-owner-a.dll"), moduleB = GetModuleHandleW(L"layout-owner-b.dll");
    CHECK(moduleA && moduleB && moduleA != moduleB); Probe a(moduleA), b(moduleB);
    manager_refresh_checks::Window window;
    auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, Log{});
    native->Refresh(); CHECK(native->Layout(10, 500, 512, true) > 0); Workspace workspace(window.value);
    if (foreignProvider) workspace.Select(1); // A requests, but B owns the actual surface callback.
    CHECK(a.begin(DATA_PROBE_BUTTON) == MMS_OK && b.begin(DATA_PROBE_BUTTON) == MMS_OK);
    CHECK(a.publish(13) == MMS_OK && b.publish(7) == MMS_OK);
    CHECK(a.visible(2) == MMS_E_ARGUMENT && a.visible(1) == MMS_OK);
    const auto initialA = a.Read(), initialB = b.Read();
    CHECK(initialA.editAvailable && initialB.editAvailable && initialA.editExpectedRevision == historyRevision);
    const auto revision = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision();
    auto& provider = foreignProvider ? b : a; auto& workerOwner = foreignProvider ? a : b;
    CHECK(a.configure(0, nullptr, nullptr) == MMS_OK && b.configure(0, nullptr, nullptr) == MMS_OK);
    struct Hook {
        Manager* manager; Probe* a; Probe* b; DataRefreshProbeSimpleFn enqueue;
        unsigned calls = 0; DWORD uiThread = GetCurrentThreadId(), workerThread = 0;
        MmsResult enqueueResult = MMS_E_NOT_READY; bool readOnly = false, stopDenied = false, modulesHeld = false, tasksDeferred = false;
    } hook{&manager, &a, &b, resizeFails ? nullptr : workerOwner.enqueueOnly};
    CHECK(provider.configure(LAYOUT_PROBE_ACTIVE | LAYOUT_PROBE_ENQUEUE | (resizeFails ? LAYOUT_PROBE_THROW : 0),
        [](void* context) {
            auto& h = *static_cast<Hook*>(context); ++h.calls;
            h.readOnly = h.manager->InCallback() && !h.manager->CanInvokeEdit();
            h.stopDenied = !h.manager->Stop();
            h.modulesHeld = GetModuleHandleW(L"layout-owner-a.dll") && GetModuleHandleW(L"layout-owner-b.dll");
            if (h.enqueue) {
                // LAYOUT has suspended every presentation. Submit only the real
                // task here; its refresh/edit attempts run after publication.
                std::thread worker([&] { h.workerThread = GetCurrentThreadId(); h.enqueueResult = h.enqueue(); }); worker.join();
            }
            manager_refresh_checks::Pump(40); // Nested dispatch must not run either DLL's tasks.
            h.tasksDeferred = !h.a->Layout().tasks[0].calls && !h.b->Layout().tasks[0].calls;
        }, &hook) == MMS_OK);
    workspace.shrink = 30;
    CHECK(a.request(MMS_UI_REFRESH_LAYOUT, 0) == MMS_OK);
    auto ticket = a.Complete();
    const auto until = GetTickCount64() + 2000;
    while ((provider.Layout().tasks[3].calls != 1 || (!resizeFails && workerOwner.Layout().tasks[2].calls != 1)) && GetTickCount64() < until)
        manager_refresh_checks::Pump(5);
    const auto actual = provider.Layout(), worker = workerOwner.Layout();
    std::printf("LAYOUT_DLL provider=%s fail=%u hook=%u resize=%u ticket=%u/%d completed=%u tasks=%u,%u,%u,%u worker=%u,%u,%u,%u\n",
        foreignProvider ? "foreign" : "self", unsigned(resizeFails), hook.calls, actual.resizeCount,
        ticket.ticketState.status, int(ticket.ticketState.result), ticket.ticketState.completedKinds,
        actual.tasks[0].calls, actual.tasks[1].calls, actual.tasks[2].calls, actual.tasks[3].calls,
        worker.tasks[0].calls, worker.tasks[1].calls, worker.tasks[2].calls, worker.tasks[3].calls);
    CHECK(hook.calls == 1 && hook.readOnly && hook.stopDenied && hook.modulesHeld && hook.tasksDeferred);
    if (!resizeFails) CHECK(hook.enqueueResult == MMS_OK && hook.workerThread && hook.workerThread != hook.uiThread);
    CHECK(actual.resizeCount == 1 && actual.directMutation == MMS_E_BUSY && actual.recursiveData == MMS_E_BUSY &&
        actual.recursiveState == MMS_E_BUSY && actual.recursiveLayout == MMS_E_BUSY);
    CHECK(actual.enqueueRun == MMS_OK && actual.enqueueCancel == MMS_OK && actual.selfCancel == MMS_OK);
    manager_data_refresh_checks::EditDenied(actual.edit, 1);
    for (unsigned i = 0; i < 4; ++i) { Denied(actual.tasks[i]); if (i < 2) CHECK(actual.tasks[i].enqueueChild == MMS_OK); }
    CHECK(ticket.ticketState.status == (resizeFails ? MMS_UI_REFRESH_FAILED : MMS_UI_REFRESH_COMPLETED) &&
        ticket.ticketState.result == (resizeFails ? MMS_E_FAILED : MMS_OK) &&
        ticket.ticketState.completedKinds == (resizeFails ? 0u : MMS_UI_REFRESH_LAYOUT) && !ticket.ticketState.sourceRevision);
    if (!resizeFails) {
        for (unsigned i : {0u, 2u}) {
            if (foreignProvider) Denied(worker.tasks[i]);
            else {
                CHECK(worker.tasks[i].calls == 1 && worker.tasks[i].mutate == MMS_OK && worker.tasks[i].dataRequest == MMS_OK &&
                    worker.tasks[i].stateRequest == MMS_OK && worker.tasks[i].layoutRequest == MMS_OK);
                CHECK(worker.tasks[i].edit.calls == 1 && worker.tasks[i].edit.begin == MMS_OK &&
                    worker.tasks[i].edit.beginOutput != worker.tasks[i].edit.originalOutput &&
                    worker.tasks[i].edit.undo == MMS_E_NOT_FOUND && worker.tasks[i].edit.redo == MMS_E_NOT_FOUND);
            }
        }
        CHECK(worker.tasks[0].enqueueChild == MMS_OK && !worker.tasks[1].calls && !worker.tasks[3].calls);
    } else for (const auto& task : worker.tasks) CHECK(!task.calls);
    const auto finalA = a.Read(), finalB = b.Read();
    CHECK(finalA.prepareCount == initialA.prepareCount && finalB.prepareCount == initialB.prepareCount &&
        finalA.stateCount == initialA.stateCount && finalB.stateCount == initialB.stateCount);
    for (unsigned i = 0; i < 4; ++i) CHECK(!finalA.tasks[i].calls && !finalB.tasks[i].calls);
    CHECK(manager.Ui().PreferencesRevision() == preferences &&
        manager.Ui().Revision() == revision + (!foreignProvider && !resizeFails ? 3u : 0u));
    CHECK(edits.Revision() == historyRevision && edits.host->history.RetainedBytes() == historyBytes &&
        edits.host->history.UndoCount() == undo && edits.host->history.RedoCount() == redo &&
        !edits.host->attached->HasPending() && !edits.host->attached->InCallback() && !edits.host->entered && !edits.host->poisoned);
    CHECK(!edits.published && !edits.dirty && !manager.InCallback() && !manager.CanInvokeEdit());
    if (!foreignProvider && !resizeFails) CHECK(edits.allowed && edits.gateCalls >= edits.allowed);
    else CHECK(!edits.allowed && !edits.gateCalls);
    CHECK(!manager.Ui().Refresh().ReadOnlyCause());
    if (resizeFails) {
        UiClient observer{&manager.Ui(), "example.layout-observer"}; auto layout = manager.Ui().LayoutApi(observer);
        MmsUiLayoutSnapshotV1 failed{}; failed.h = Header<MmsUiLayoutSnapshotV1>(); failed.handle = 123;
        CHECK(layout.acquireSnapshot(layout.context, &failed) == MMS_E_NOT_READY && failed.handle == 123);
    }
    CHECK(provider.configure(0, nullptr, nullptr) == MMS_OK); workspace.shrink = 0;
    CHECK(native->Layout(10, 500, 512, true) > 0);
    // The accepted ticket remains a real DLL lease even after completion.
    CHECK(!manager.Stop() && GetModuleHandleW(L"layout-owner-a.dll") == moduleA);
    CHECK(a.Read().stopHeldTicket && count("org.example.layout-owner-a", "DATA_PROBE_DESTROY") == 0);
    CHECK(a.releaseTicket() == MMS_OK && manager.Stop());
    CHECK(GetModuleHandleW(L"layout-owner-a.dll") == nullptr && GetModuleHandleW(L"layout-owner-b.dll") == nullptr);
    CHECK(count("org.example.layout-owner-a", "DATA_PROBE_VIEW_DESTROY") == finalA.panelCreated &&
        count("org.example.layout-owner-b", "DATA_PROBE_VIEW_DESTROY") == finalB.panelCreated);
    CHECK(count("org.example.layout-owner-a", "DATA_PROBE_DESTROY") == 1 && count("org.example.layout-owner-b", "DATA_PROBE_DESTROY") == 1);
    native.reset(); CHECK(manager.Stop()); edits.Close();
    std::cout << "manager LAYOUT " << (resizeFails ? "failed resize" : foreignProvider ? "foreign provider" : "owner and independent worker") << " PASS\n";
}
} // namespace manager_layout_refresh_checks

static void ManagerLayoutRefreshChecks(const fs::path& root, const fs::path& library) {
    manager_layout_refresh_checks::Run(root, library, false, false);
    manager_layout_refresh_checks::Run(root, library, true, false);
    manager_layout_refresh_checks::Run(root, library, false, true);
}
