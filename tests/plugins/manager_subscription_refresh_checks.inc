// Include after manager_refresh_checks.inc and manager_data_refresh_checks.inc.
// The DLL negotiates Manager's actual services. AttachedEdits supplies the real
// common journal/publication dirty observations; this isolated Manager fixture
// has no MMDApp playhead or scene-evaluation adapter.
#include "subscription_refresh_probe.h"
#include <cwchar>

namespace manager_subscription_refresh_checks {
struct Probe {
    SubscriptionProbeBeginFn begin = nullptr;
    SubscriptionProbePublishFn publish = nullptr;
    SubscriptionProbeQueryFn query = nullptr;
    SubscriptionProbeSimpleFn subscribe = nullptr, checkThreads = nullptr, requestOld = nullptr;
    SubscriptionProbeSimpleFn releaseTicket = nullptr, acquireHold = nullptr, releaseHold = nullptr;
    explicit Probe(HMODULE module) {
        CHECK(module != nullptr);
        begin = reinterpret_cast<decltype(begin)>(GetProcAddress(module, "SubscriptionProbeBegin"));
        publish = reinterpret_cast<decltype(publish)>(GetProcAddress(module, "SubscriptionProbePublish"));
        query = reinterpret_cast<decltype(query)>(GetProcAddress(module, "SubscriptionProbeQuery"));
        subscribe = reinterpret_cast<decltype(subscribe)>(GetProcAddress(module, "SubscriptionProbeSubscribe"));
        checkThreads = reinterpret_cast<decltype(checkThreads)>(GetProcAddress(module, "SubscriptionProbeCheckThreads"));
        requestOld = reinterpret_cast<decltype(requestOld)>(GetProcAddress(module, "SubscriptionProbeRequestOld"));
        releaseTicket = reinterpret_cast<decltype(releaseTicket)>(GetProcAddress(module, "SubscriptionProbeReleaseTicket"));
        acquireHold = reinterpret_cast<decltype(acquireHold)>(GetProcAddress(module, "SubscriptionProbeAcquireHold"));
        releaseHold = reinterpret_cast<decltype(releaseHold)>(GetProcAddress(module, "SubscriptionProbeReleaseHold"));
        CHECK(begin && publish && query && subscribe && checkThreads && requestOld && releaseTicket && acquireHold && releaseHold);
    }
    SubscriptionRefreshProbeState Read() const {
        SubscriptionRefreshProbeState value{}; value.size = sizeof(value); CHECK(query(&value) == MMS_OK); return value;
    }
    SubscriptionRefreshProbeState Complete(Manager& manager, uint64_t revision) const {
        const auto until = GetTickCount64() + 2000; auto value = Read();
        const auto complete = [&] {
            return value.subscriptionQuery == MMS_OK && value.subscriptionState.completedRevision == revision &&
                !value.subscriptionState.pending && value.subscriptionState.lastResult == MMS_OK;
        };
        // Manager::Drain services UI tasks. Refreshes use the host's posted
        // wake message, so dispatch this thread's own queue as in other DLL tests.
        while (!complete() && GetTickCount64() < until) { manager.Drain(); manager_refresh_checks::Pump(2); value = Read(); }
        if (!complete()) {
            std::cerr << "SUBSCRIPTION_MANAGER_COMPLETE query=" << value.subscriptionQuery << " desired=" << revision
                << " latest=" << value.subscriptionState.latestRevision << " submitted=" << value.subscriptionState.submittedRevision
                << " completed=" << value.subscriptionState.completedRevision << " pending=" << value.subscriptionState.pending
                << " result=" << value.subscriptionState.lastResult << " prepares=" << value.prepareCount << '\n';
        }
        CHECK(complete()); return value;
    }
};
static HWND Button(HWND toolbar) {
    HWND button = nullptr; unsigned count = 0;
    for (auto child = GetWindow(toolbar, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto id = GetDlgCtrlID(child); if (id < 0xa000 || id >= 0xc000) continue;
        wchar_t label[64]{}; GetWindowTextW(child, label, 64);
        if (std::wcscmp(label, L"Subscription probe")) continue;
        button = child; ++count;
    }
    CHECK(count == 1 && button && IsWindowVisible(button)); return button;
}
static void ReadOnly(const SubscriptionRefreshProbeState& state) {
    CHECK(state.prepareCount && state.prepareWasUi && state.prepareIdentity && state.everyReadOnlyDenied && !state.traceOverflow);
    CHECK(state.editAvailable && state.editQuery == MMS_OK && state.editCalls == state.prepareCount && state.editOutputPreserved);
    CHECK(state.editBegin == MMS_E_BUSY && state.editUndo == MMS_E_BUSY && state.editRedo == MMS_E_BUSY);
    CHECK(state.directMutation == MMS_E_BUSY && state.readOnlySubscribe == MMS_E_BUSY && state.readOnlyUnsubscribe == MMS_E_BUSY);
    CHECK(state.readOnlyPublish == MMS_E_BUSY && state.readOnlyOutputPreserved && state.prepareWrite == MMS_OK);
}
static void Identity(const SubscriptionRefreshProbeState& state, Manager& manager) {
    CHECK(state.button && state.source && state.binding && state.subscription && state.subscription == state.originalSubscription);
    CHECK(state.source != state.binding && state.binding != state.subscription && state.source != state.subscription);
    CHECK(state.target.element == state.button && state.target.instanceGeneration && state.target.projectEpoch);
    CHECK(manager.Ui().elements.at("org.example.subscription-refresh.button").handle == state.button);
    CHECK(state.subscriptionQuery == MMS_OK && state.subscriptionState.subscription == state.subscription &&
        state.subscriptionState.binding == state.binding && state.subscriptionState.projectEpoch == state.target.projectEpoch);
}
static void Run(const fs::path& root, const fs::path& library, bool oldSnapshot) {
    const auto directory = root / (oldSnapshot ? "subscription-manager-old-snapshot" : "subscription-manager-auto");
    const auto package = directory / "PLUGINS" / "fixture";
    fs::create_directories(package); fs::copy_file(library, package / "subscription-refresh-probe.dll");
    Write(package / "plugin.json", ManifestText("org.example.subscription-refresh", "subscription-refresh-probe.dll"));
    std::vector<std::string> logs;
    const auto log = [&](const std::string& value) { logs.push_back(value); };
    const auto count = [&](const char* marker) { return std::count_if(logs.begin(), logs.end(),
        [&](const auto& value) { return value.find(marker) != std::string::npos; }); };
    Manager manager(directory / "user", log); manager_data_refresh_checks::AttachedEdits edits(manager);
    const auto historyRevision = edits.Revision(), historyBytes = edits.host->history.RetainedBytes();
    const auto undo = edits.host->history.UndoCount(), redo = edits.host->history.RedoCount();
    manager.Load(package.parent_path());
    const auto status = manager.Status();
    if (status.size() != 1 || status[0].stage != "started") {
        for (const auto& entry : status) std::cerr << "SUBSCRIPTION_MANAGER_START id=" << entry.id << " stage=" << entry.stage << " error=" << entry.error << '\n';
        for (const auto& entry : logs) std::cerr << "SUBSCRIPTION_MANAGER_LOG " << entry << '\n';
    }
    CHECK(status.size() == 1 && status[0].stage == "started" && count("SUBSCRIPTION_PROBE_START") == 1);
    const auto module = GetModuleHandleW(L"subscription-refresh-probe.dll"); Probe probe(module);
    manager_refresh_checks::Window window;
    auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, log);
    native->Refresh(); CHECK(native->Layout(10, 500, 140, true) > 0); const auto button = Button(native->Toolbar());
    const auto begun = probe.begin(historyRevision, oldSnapshot ? 0u : 1u);
    if (begun) std::cerr << "SUBSCRIPTION_BEGIN result=" << begun << " history=" << historyRevision << '\n';
    CHECK(begun == MMS_OK);
    auto state = probe.Read();
    CHECK(state.capabilities.h.size == sizeof(MmsUiRefreshSubscriptionCapabilitiesV1) && state.capabilities.h.major == 1 &&
        state.capabilities.maxSubscriptionsPerPlugin == 128 && state.capabilities.supportedKinds == 15 && state.capabilities.supportedScopes == MMS_UI_REFRESH_SELF);
    CHECK(!state.capabilities.flags && !state.capabilities.reserved[0] && !state.capabilities.reserved[1]);
    CHECK(state.editAvailable && state.editExpectedRevision == historyRevision && !state.prepareCount && !state.explicitRequests && !state.ticket);
    const auto registryRevision = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision();
    edits.Clean(historyRevision, historyBytes, undo, redo);
    if (!oldSnapshot) {
        Identity(state, manager);
        CHECK(!state.subscriptionState.latestRevision && !state.subscriptionState.pending);
        CHECK(probe.checkThreads() == MMS_OK); state = probe.Read();
        CHECK(state.workerQuery == MMS_OK && state.workerState.subscription == state.subscription && state.workerState.binding == state.binding);
        CHECK(state.workerSubscribe == MMS_E_THREAD && state.workerSdkSubscribe == MMS_E_THREAD &&
            state.workerUnsubscribe == MMS_E_THREAD && state.workerCaps == MMS_E_THREAD && state.workerOutputPreserved);
        CHECK(probe.publish(13, 1) == MMS_OK); const auto published = probe.Read();
        CHECK(published.publishWasWorker && published.sourceRevision == 1 && !published.prepareCount && !published.explicitRequests && !published.ticket);
        CHECK(published.subscriptionState.latestRevision == 1 && published.subscriptionState.pending);
        state = probe.Complete(manager, 1); Identity(state, manager); ReadOnly(state);
        CHECK(state.prepareCount == 1 && state.traceValues[0] == 13 && state.traceRevisions[0] == 1 && state.preparedValue == 26);
        CHECK(state.displayQuery == MMS_OK && state.displayValue == 26 && state.displaySourceRevision == 1 && state.stateValue == 26);
        CHECK(state.stateSequence > state.prepareSequence && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED);
        const auto prepared = state.prepareCount, states = state.stateCount; const auto displayRevision = state.displayRevision;
        CHECK(probe.publish(13, 1) == MMS_OK); manager.Drain(); manager_refresh_checks::Pump(10); state = probe.Read();
        CHECK(state.sourceRevision == 1 && state.prepareCount == prepared && state.stateCount == states &&
            state.displayRevision == displayRevision && !state.subscriptionState.pending && !state.explicitRequests && !state.ticket);
        CHECK(probe.publish(100, 41) == MMS_OK); const auto burst = probe.Read();
        CHECK(burst.sourceValue == 140 && burst.sourceRevision == 42 && burst.prepareCount == prepared && burst.stateCount == states);
        CHECK(burst.subscriptionState.latestRevision == 42 && burst.subscriptionState.submittedRevision == 1 && burst.subscriptionState.pending);
        state = probe.Complete(manager, burst.sourceRevision); Identity(state, manager); ReadOnly(state);
        CHECK(state.prepareCount == prepared + 1 && state.traceValues[1] == 140 && state.traceRevisions[1] == 42 &&
            state.displayValue == 280 && state.displaySourceRevision == 42 && state.stateValue == 280);
        CHECK(state.stateCount == states + 1 && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
        CHECK(!state.explicitRequests && !state.ticket);
    } else {
        // Only this separate positive cohort case submits an explicit ticket.
        // Publication 2 must not change the already accepted revision-1 input.
        CHECK(probe.publish(11, 1) == MMS_OK && probe.requestOld() == MMS_OK);
        const auto accepted = probe.Read(); CHECK(accepted.ticket && accepted.ticketQuery == MMS_OK && accepted.ticketState.status == MMS_UI_REFRESH_PENDING);
        CHECK(probe.subscribe() == MMS_OK && probe.publish(22, 1) == MMS_OK); state = probe.Read();
        CHECK(state.sourceRevision == 2 && !state.prepareCount && state.explicitRequests == 1);
        state = probe.Complete(manager, 2); Identity(state, manager); ReadOnly(state);
        CHECK(state.ticket == accepted.ticket && state.ticketQuery == MMS_OK && state.ticketState.status == MMS_UI_REFRESH_COMPLETED &&
            state.ticketState.result == MMS_OK && state.ticketState.sourceRevision == 1 && state.ticketState.completedKinds == MMS_UI_REFRESH_DATA);
        CHECK(state.prepareCount == 2 && state.traceValues[0] == 11 && state.traceRevisions[0] == 1 &&
            state.traceValues[1] == 22 && state.traceRevisions[1] == 2);
        CHECK(state.previousValue == 22 && state.previousSourceRevision == 1 && state.displayValue == 44 && state.displaySourceRevision == 2);
        CHECK(state.stateValue == 44 && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
        CHECK(probe.releaseTicket() == MMS_OK && !probe.Read().ticket);
    }
    CHECK(manager.Ui().Revision() == registryRevision && manager.Ui().PreferencesRevision() == preferences);
    edits.Clean(historyRevision, historyBytes, undo, redo);
    state = probe.Read(); const auto prepareCount = state.prepareCount, stateCount = state.stateCount;
    const auto heldValue = state.displayValue; const auto heldRevision = state.displaySourceRevision;
    CHECK(probe.acquireHold() == MMS_OK && probe.publish(state.sourceValue + 1, 1) == MMS_OK);
    CHECK(probe.Read().subscriptionState.pending && probe.Read().prepareCount == prepareCount);
    // An immutable DATA display lease, not an extra LoadLibrary reference,
    // retains the DLL while Manager retires its subscriptions and queue work.
    CHECK(!manager.Stop()); state = probe.Read();
    CHECK(manager.Status()[0].stage == "stopping" && GetModuleHandleW(L"subscription-refresh-probe.dll") == module);
    CHECK(state.stopCount == 1 && state.stopHeldDisplay && state.prepareAtStop == prepareCount && state.heldLease);
    CHECK(state.stopQuery == MMS_E_STALE && state.stopUnsubscribe == MMS_E_STALE && state.stopOutputPreserved && state.stopRelease == MMS_OK && !state.subscription);
    CHECK(state.heldValue == heldValue && state.heldSourceRevision == heldRevision && count("SUBSCRIPTION_PROBE_DESTROY") == 0);
    const auto preparedLogs = count("SUBSCRIPTION_PROBE_PREPARE");
    for (unsigned index = 0; index < 4; ++index) { manager.Drain(); manager_refresh_checks::Pump(2); }
    const auto stoppedPublish = probe.publish(301, 1); CHECK(stoppedPublish == MMS_E_NOT_READY || stoppedPublish == MMS_E_STALE);
    manager.Drain(); state = probe.Read();
    CHECK(state.prepareCount == prepareCount && state.stateCount == stateCount && count("SUBSCRIPTION_PROBE_PREPARE") == preparedLogs);
    CHECK(state.heldValue == heldValue && state.heldSourceRevision == heldRevision && !state.subscription && !state.ticket);
    edits.Clean(historyRevision, historyBytes, undo, redo);
    CHECK(probe.releaseHold() == MMS_OK && manager.Stop() && manager.Status()[0].stage == "stopped");
    CHECK(count("SUBSCRIPTION_PROBE_DESTROY") == 1 && GetModuleHandleW(L"subscription-refresh-probe.dll") == nullptr);
    // The host keeps its own roots plus the semantic catalog observation
    // elements; only plugin-owned registrations must be retired here.
    bool pluginElements = false;
    for (const auto& [id, element] : manager.Ui().elements) {
        const auto& owner = element.desc.ownerPlugin;
        if (owner.bytes && std::string(owner.data, owner.bytes) != "mms") pluginElements = true;
    }
    CHECK(manager.Ui().commands.empty() && !pluginElements);
    native.reset(); CHECK(manager.Stop()); edits.Close();
}
} // namespace manager_subscription_refresh_checks
static void ManagerSubscriptionRefreshChecks(const fs::path& root, const fs::path& library) {
    manager_subscription_refresh_checks::Run(root, library, false);
    manager_subscription_refresh_checks::Run(root, library, true);
}
