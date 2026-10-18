#include "data_refresh_probe.h"

namespace manager_data_refresh_checks {
struct AttachedEdits {
    Manager& manager;
    std::unique_ptr<ManagerEditHost> host;
    unsigned gateCalls=0,allowed=0,published=0;
    bool dirty=false;
    explicit AttachedEdits(Manager& value):manager(value){
        host=std::make_unique<ManagerEditHost>(manager.Storage(),[this]{
            ++gateCalls;
            if(manager.InCallback()&&!manager.CanInvokeEdit())return MMS_E_BUSY;
            if(manager.CanInvokeEdit())++allowed;
            return MMS_OK;
        });
        CHECK(manager.AttachEditHost(*host)==MMS_OK&&host->attached);
        manager.SetEditPublished([this]{++published;dirty=true;});
    }
    ~AttachedEdits(){
        // As in ManagerEditCase, keep the external journal alive if a failing
        // test cannot detach it; Manager still owns the EditStore referencing it.
        if(host&&host->attached){manager.Stop();auto result=manager.DetachEditHost();
            if(result==MMS_E_FAILED)result=manager.DetachEditHost();if(result)host.release();}
        manager.SetEditPublished({});
    }
    uint64_t Revision(){uint64_t result=0;CHECK(host->Revision(result)==MMS_OK);return result;}
    void Clean(uint64_t revision,uint64_t bytes,unsigned undo,unsigned redo){
        CHECK(Revision()==revision&&host->history.RetainedBytes()==bytes&&host->history.UndoCount()==undo&&host->history.RedoCount()==redo);
        CHECK(host->attached&&!host->attached->HasPending()&&!host->attached->InCallback()&&!host->entered&&!host->poisoned);
        CHECK(!published&&!dirty&&!allowed&&!gateCalls&&!manager.InCallback()&&!manager.CanInvokeEdit());
        CHECK(!manager.Storage().Prepared()&&!manager.Storage().InCallback()&&!manager.Storage().Poisoned()&&manager.Storage().Records().empty());
    }
    void Close(){CHECK(manager.DetachEditHost()==MMS_OK&&host->attached==nullptr);manager.SetEditPublished({});}
};
static void EditDenied(const DataRefreshProbeEditState& state,unsigned calls){
    CHECK(state.calls==calls&&state.begin==MMS_E_BUSY&&state.undo==MMS_E_BUSY&&state.redo==MMS_E_BUSY);
    CHECK(state.originalOutput==0x123456789abcdef0ull&&state.beginOutput==state.originalOutput);
}
struct Probe {
    DataRefreshProbeBeginFn begin;
    DataRefreshProbePublishFn publish;
    DataRefreshProbeRequestFn request;
    DataRefreshProbeQueryFn query;
    DataRefreshProbeSimpleFn releaseTicket, acquireHold, releaseHold, removeTarget;
    explicit Probe(HMODULE module) {
        begin = reinterpret_cast<DataRefreshProbeBeginFn>(GetProcAddress(module, "ProbeBegin"));
        publish = reinterpret_cast<DataRefreshProbePublishFn>(GetProcAddress(module, "ProbePublish"));
        request = reinterpret_cast<DataRefreshProbeRequestFn>(GetProcAddress(module, "ProbeRequest"));
        query = reinterpret_cast<DataRefreshProbeQueryFn>(GetProcAddress(module, "ProbeQuery"));
        releaseTicket = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeReleaseTicket"));
        acquireHold = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeAcquireHold"));
        releaseHold = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeReleaseHold"));
        removeTarget = reinterpret_cast<DataRefreshProbeSimpleFn>(GetProcAddress(module, "ProbeRemoveTarget"));
        CHECK(begin && publish && request && query && releaseTicket && acquireHold && releaseHold && removeTarget);
    }
    DataRefreshProbeState Read() const {
        DataRefreshProbeState state{}; state.size = sizeof(state); CHECK(query(&state) == MMS_OK); return state;
    }
    DataRefreshProbeState Complete() const {
        const auto until = GetTickCount64() + 2000;
        auto state = Read();
        while ((state.ticketState.status == MMS_UI_REFRESH_PENDING ||
                state.ticketState.status == MMS_UI_REFRESH_RUNNING) && GetTickCount64() < until) {
            manager_refresh_checks::Pump(5);
            state = Read();
        }
        CHECK(state.ticketState.status != MMS_UI_REFRESH_PENDING &&
            state.ticketState.status != MMS_UI_REFRESH_RUNNING);
        return state;
    }
};
static void Run(const fs::path& root, const fs::path& library, unsigned kind, bool remove = false) {
    const auto directory = root / (remove ? "data-remove" : kind == DATA_PROBE_PANEL ? "data-panel" : "data-button");
    const auto package = directory / "PLUGINS" / "fixture";
    fs::create_directories(package); fs::copy_file(library, package / "data-refresh-probe.dll");
    Write(package / "plugin.json", ManifestText("org.example.data-refresh", "data-refresh-probe.dll"));
    std::vector<std::string> logs;
    const auto log = [&](const std::string& value) { logs.push_back(value); };
    const auto count = [&](const char* marker) { return std::count_if(logs.begin(), logs.end(),
        [&](const auto& value) { return value.find(marker) != std::string::npos; }); };
    Manager manager(directory / "user", log);
    AttachedEdits edits(manager);
    const auto historyRevision=edits.Revision(),historyBytes=edits.host->history.RetainedBytes();
    const auto undo=edits.host->history.UndoCount(),redo=edits.host->history.RedoCount();
    manager.Load(package.parent_path());
    CHECK(manager.Status().size() == 1 && manager.Status()[0].stage == "started");
    const auto module = GetModuleHandleW(L"data-refresh-probe.dll"); CHECK(module != nullptr); Probe probe(module);
    manager_refresh_checks::Window window;
    auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, log);
    native->Refresh(); CHECK(native->Layout(10, 500, 140, true) > 0);
    CHECK(probe.begin(kind) == MMS_OK && probe.publish(13) == MMS_OK);
    const auto before = probe.Read(); CHECK(before.sourceRevision == 1 && before.panelCreated == 1);
    CHECK(before.editQuery==MMS_OK&&before.editAvailable&&before.editExpectedRevision==historyRevision);
    edits.Clean(historyRevision,historyBytes,undo,redo);
    const auto registryRevision = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision();
    const uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_REPAINT |
        (kind == DATA_PROBE_BUTTON ? MMS_UI_REFRESH_STATE : 0u);
    CHECK(probe.request(kinds, DATA_PROBE_ENQUEUE) == MMS_OK);
    CHECK(probe.Read().ticketState.status == MMS_UI_REFRESH_PENDING);
    auto state = probe.Complete();
    CHECK(state.ticketState.status == MMS_UI_REFRESH_COMPLETED && state.ticketState.completedKinds == kinds &&
        state.ticketState.sourceRevision == 1 && state.prepareCount == 1 && state.preparedValue == 26);
    CHECK(state.sourceRead == MMS_OK && state.sourceReadRevision == 1 && state.oldDisplayRead == MMS_E_NOT_READY);
    CHECK(state.directMutation == MMS_E_BUSY && state.recursiveData == MMS_E_BUSY && state.recursiveState == MMS_E_BUSY);
    CHECK(state.enqueueRun == MMS_OK && state.enqueueCancel == MMS_OK && state.selfCancel == MMS_OK);
    EditDenied(state.edit,1);
    if (kind == DATA_PROBE_BUTTON) {
        CHECK(state.stateValue == 26 && state.stateSequence > state.prepareSequence);
    } else {
        const auto panel = reinterpret_cast<HWND>(state.panelWindow); CHECK(IsWindow(panel));
        InvalidateRect(panel, nullptr, FALSE); SendMessageW(panel, WM_PAINT, 0, 0); state = probe.Read();
        CHECK(state.paintValue == 26 && state.paintCount && state.paintSequence > state.prepareSequence && state.prepareCount == 1);
    }
    manager.Drain(); manager.Drain(); state = probe.Read();
    for (unsigned index = 0; index < 4; ++index) {
        CHECK(state.tasks[index].calls == 1 && state.tasks[index].mutate == MMS_E_BUSY &&
            state.tasks[index].dataRequest == MMS_E_BUSY && state.tasks[index].stateRequest == MMS_E_BUSY);
        if (index < 2) CHECK(state.tasks[index].enqueueChild == MMS_OK);
        EditDenied(state.tasks[index].edit,1);
    }
    CHECK(count("DATA_PROBE_TASK_CANCEL") == 1 && count("DATA_PROBE_TASK_RUN") == 3);
    CHECK(manager.Ui().Revision() == registryRevision && manager.Ui().PreferencesRevision() == preferences && !manager.CanInvokeEdit());
    edits.Clean(historyRevision,historyBytes,undo,redo);
    CHECK(probe.releaseTicket() == MMS_OK && probe.publish(13) == MMS_OK && probe.Read().sourceRevision == 1);
    CHECK(probe.request(MMS_UI_REFRESH_DATA, 0) == MMS_OK); state = probe.Complete();
    CHECK(state.prepareCount == 1 && state.ticketState.sourceRevision == 1 && state.ticketState.completedKinds == MMS_UI_REFRESH_DATA);
    CHECK(probe.releaseTicket() == MMS_OK);
    if (!remove) {
        int value = 20;
        for (unsigned flag : {DATA_PROBE_FAIL_PREPARE, DATA_PROBE_NO_WRITE, DATA_PROBE_DOUBLE_WRITE, DATA_PROBE_THROW_MEMORY}) {
            CHECK(probe.publish(value++) == MMS_OK && probe.request(kinds, flag) == MMS_OK);
            const auto states = probe.Read().stateCount; state = probe.Complete();
            const auto expected = flag == DATA_PROBE_THROW_MEMORY ? MMS_E_MEMORY : flag == DATA_PROBE_DOUBLE_WRITE ? MMS_E_CONFLICT : MMS_E_FAILED;
            CHECK(state.ticketState.status == MMS_UI_REFRESH_FAILED && state.ticketState.result == expected &&
                state.ticketState.completedKinds == 0 && state.ticketState.sourceRevision == 0 && state.stateCount == states);
            EditDenied(state.edit,state.prepareCount);edits.Clean(historyRevision,historyBytes,undo,redo);
            CHECK(probe.acquireHold() == MMS_OK && probe.Read().heldValue == 26 && probe.releaseHold() == MMS_OK);
            CHECK(probe.releaseTicket() == MMS_OK);
        }
        CHECK(probe.publish(21) == MMS_OK && probe.request(kinds, 0) == MMS_OK); state = probe.Complete();
        CHECK(state.ticketState.status == MMS_UI_REFRESH_COMPLETED && state.ticketState.sourceRevision == state.sourceRevision);
        EditDenied(state.edit,state.prepareCount);edits.Clean(historyRevision,historyBytes,undo,redo);
        CHECK(probe.releaseTicket() == MMS_OK && probe.acquireHold() == MMS_OK && probe.Read().heldValue == 42);
        CHECK(probe.publish(22) == MMS_OK && probe.request(kinds, 0) == MMS_OK);
        CHECK(!manager.Stop()); state = probe.Read();
        CHECK(state.ticketState.status == MMS_UI_REFRESH_CANCELLED && state.ticketState.result == MMS_E_CANCELLED &&
            state.heldLease && state.heldValue == 42 && state.stopHeldTicket && state.stopHeldDisplay);
        CHECK(GetModuleHandleW(L"data-refresh-probe.dll") == module && count("DATA_PROBE_DESTROY") == 0);
        CHECK(probe.releaseTicket() == MMS_OK && !manager.Stop());
        CHECK(probe.Read().heldValue == 42 && probe.releaseHold() == MMS_OK);
    } else {
        CHECK(probe.publish(30) == MMS_OK && probe.request(kinds, 0) == MMS_OK);
        const auto prepared = probe.Read().prepareCount;
        CHECK(probe.removeTarget() == MMS_OK); state = probe.Complete();
        CHECK(state.ticketState.status == MMS_UI_REFRESH_FAILED && state.ticketState.result == MMS_E_STALE && state.prepareCount == prepared);
        CHECK(probe.releaseTicket() == MMS_OK);
    }
    edits.Clean(historyRevision,historyBytes,undo,redo);
    CHECK(manager.Stop() && manager.Status()[0].stage == "stopped" && count("DATA_PROBE_DESTROY") == 1);
    CHECK(GetModuleHandleW(L"data-refresh-probe.dll") == nullptr && count("DATA_PROBE_VIEW_DESTROY") == 1);
    native.reset(); CHECK(manager.Stop());edits.Close();
}
} // namespace manager_data_refresh_checks
static void ManagerDataRefreshChecks(const fs::path& root, const fs::path& library) {
    manager_data_refresh_checks::Run(root, library, DATA_PROBE_PANEL);
    manager_data_refresh_checks::Run(root, library, DATA_PROBE_BUTTON);
    manager_data_refresh_checks::Run(root, library, DATA_PROBE_PANEL, true);
}
