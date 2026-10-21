// Include after manager_refresh_checks.inc. This fixture loads the real test
// DLL through Manager; no queue/client construction substitutes for negotiation.
#include "dependency_refresh_plugin.h"
#include <array>
#include <cwchar>

namespace manager_dependency_refresh_checks {
struct Probe {
    DependencyRefreshProbeSimpleFn begin = nullptr, checkEdges = nullptr;
    DependencyRefreshProbeRequestFn request = nullptr;
    DependencyRefreshProbeQueryFn query = nullptr;
    DependencyRefreshProbeIndexFn release = nullptr, removeEdge = nullptr;
    DependencyRefreshProbeSetStateFn setState = nullptr;
    explicit Probe(HMODULE module) {
        CHECK(module != nullptr);
        begin = reinterpret_cast<decltype(begin)>(GetProcAddress(module, "DependencyProbeBegin"));
        checkEdges = reinterpret_cast<decltype(checkEdges)>(GetProcAddress(module, "DependencyProbeCheckEdges"));
        request = reinterpret_cast<decltype(request)>(GetProcAddress(module, "DependencyProbeRequest"));
        query = reinterpret_cast<decltype(query)>(GetProcAddress(module, "DependencyProbeQuery"));
        release = reinterpret_cast<decltype(release)>(GetProcAddress(module, "DependencyProbeRelease"));
        removeEdge = reinterpret_cast<decltype(removeEdge)>(GetProcAddress(module, "DependencyProbeRemoveEdge"));
        setState = reinterpret_cast<decltype(setState)>(GetProcAddress(module, "DependencyProbeSetState"));
        CHECK(begin && checkEdges && request && query && release && removeEdge && setState);
    }
    DependencyRefreshProbeState Read() const {
        DependencyRefreshProbeState state{}; state.size = sizeof(state);
        CHECK(query(&state) == MMS_OK);
        return state;
    }
    DependencyRefreshProbeState Complete(uint32_t count) const {
        const auto until = GetTickCount64() + 2000;
        auto state = Read();
        const auto pending = [&] {
            for (uint32_t index = 0; index < count; ++index)
                if (state.ticketStates[index].status == MMS_UI_REFRESH_PENDING ||
                    state.ticketStates[index].status == MMS_UI_REFRESH_RUNNING) return true;
            return false;
        };
        while (pending() && GetTickCount64() < until) { manager_refresh_checks::Pump(5); state = Read(); }
        bool complete = !pending();
        for (uint32_t index = 0; index < count; ++index)
            complete = complete && state.ticketQuery[index] == MMS_OK &&
                state.ticketStates[index].status == MMS_UI_REFRESH_COMPLETED &&
                state.ticketStates[index].result == MMS_OK &&
                state.ticketStates[index].completedKinds == (MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT);
        if (!complete) {
            for (uint32_t index = 0; index < count; ++index)
                std::cerr << "DEPENDENCY_MANAGER_COMPLETE ticket=" << state.tickets[index]
                    << " query=" << state.ticketQuery[index] << " status=" << state.ticketStates[index].status
                    << " result=" << state.ticketStates[index].result << " kinds=" << state.ticketStates[index].completedKinds << '\n';
            std::cerr << "DEPENDENCY_MANAGER_CALLS " << state.calls[0] << ',' << state.calls[1] << ','
                << state.calls[2] << ',' << state.calls[3] << " sequence=" << state.sequence << '\n';
        }
        CHECK(!pending());
        for (uint32_t index = 0; index < count; ++index)
            CHECK(state.ticketQuery[index] == MMS_OK && state.ticketStates[index].status == MMS_UI_REFRESH_COMPLETED &&
                state.ticketStates[index].result == MMS_OK &&
                state.ticketStates[index].completedKinds == (MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT) &&
                state.ticketStates[index].sourceRevision == 0);
        return state;
    }
};
static std::array<HWND, DEPENDENCY_PROBE_BUTTONS> Buttons(HWND toolbar) {
    std::array<HWND, DEPENDENCY_PROBE_BUTTONS> result{};
    unsigned count = 0;
    for (auto child = GetWindow(toolbar, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const int id = GetDlgCtrlID(child);
        if (id < 0xa000 || id >= 0xc000) continue;
        wchar_t text[64]{}; GetWindowTextW(child, text, 64);
        for (uint32_t index = 0; index < DEPENDENCY_PROBE_BUTTONS; ++index) {
            const wchar_t expected[] = {L'D', L'e', L'p', L'e', L'n', L'd', L'e', L'n', L'c', L'y', L' ', wchar_t(L'A' + index), 0};
            if (std::wcscmp(text, expected)) continue;
            CHECK(result[index] == nullptr); result[index] = child; ++count;
        }
    }
    CHECK(count == DEPENDENCY_PROBE_BUTTONS);
    for (const auto button : result) CHECK(button != nullptr && IsWindowVisible(button) && IsWindowEnabled(button));
    return result;
}
static void Run(const fs::path& root, const fs::path& library, bool pendingStop, bool changeGraph) {
    const auto directory = root / (changeGraph ? "dependency-manager-graph-change" :
        pendingStop ? "dependency-manager-pending" : "dependency-manager-completed");
    const auto package = directory / "PLUGINS" / "fixture";
    fs::create_directories(package);
    fs::copy_file(library, package / "dependency-refresh-probe.dll");
    Write(package / "plugin.json", ManifestText("org.example.dependency-refresh", "dependency-refresh-probe.dll"));
    std::vector<std::string> logs;
    const auto log = [&](const std::string& value) { logs.push_back(value); };
    const auto count = [&](const char* marker) { return std::count_if(logs.begin(), logs.end(),
        [&](const auto& entry) { return entry.find(marker) != std::string::npos; }); };
    bool allow = false; unsigned guardCalls = 0;
    Manager manager(directory / "user", log);
    manager.SetRefreshGuard([&] { ++guardCalls; return allow; });
    manager.Load(package.parent_path());
    const auto statuses = manager.Status();
    if (statuses.size() != 1 || statuses[0].stage != "started") {
        for (const auto& status : statuses)
            std::cerr << "DEPENDENCY_MANAGER_START id=" << status.id << " stage=" << status.stage << " error=" << status.error << '\n';
        for (const auto& entry : logs) std::cerr << "DEPENDENCY_MANAGER_LOG " << entry << '\n';
    }
    CHECK(statuses.size() == 1 && statuses[0].stage == "started" && count("DEPENDENCY_PROBE_START") == 1);
    const auto module = GetModuleHandleW(L"dependency-refresh-probe.dll");
    Probe probe(module);
    manager_refresh_checks::Window window;
    auto native = std::make_unique<NativeUi>(manager.Ui(), window.value, log);
    native->Refresh(); CHECK(native->Layout(10, 620, 140, true) > 0);
    const auto buttons = Buttons(native->Toolbar());
    CHECK(probe.begin() == MMS_OK);
    auto state = probe.Read();
    CHECK(state.capabilities.h.size == sizeof(MmsUiRefreshDependencyCapabilitiesV1) && state.capabilities.h.major == 1 &&
        state.capabilities.maxDependenciesPerPlugin == 512 && state.capabilities.maxDependentsPerRequest == 256 &&
        state.capabilities.flags == 0 && state.capabilities.reserved == 0);
    CHECK(state.workerConnectResult == MMS_E_THREAD && state.workerRegisterResult == MMS_E_THREAD);
    for (uint32_t index = 0; index < DEPENDENCY_PROBE_BUTTONS; ++index) {
        CHECK(state.buttons[index] && state.targets[index].element == state.buttons[index] &&
            state.targets[index].instanceGeneration && state.targets[index].projectEpoch);
        CHECK(state.calls[index] == 0 && state.callSequence[index] == 0);
        // All providers change. DEPENDENTS must still leave A and unrelated D
        // at their previously displayed state instead of running Refresh().
        CHECK(probe.setState(index, 0, 1) == MMS_OK);
    }
    for (uint32_t index = 0; index < DEPENDENCY_PROBE_EDGES; ++index) {
        CHECK(state.edges[index] != 0);
        for (uint32_t previous = 0; previous < index; ++previous) CHECK(state.edges[index] != state.edges[previous]);
    }
    const auto revision = manager.Ui().Revision(), preferences = manager.Ui().PreferencesRevision();
    CHECK(probe.request(2) == MMS_OK);
    state = probe.Read();
    CHECK(state.requestWasWorker && state.requestCount == 2 && state.tickets[0] && state.tickets[1] && state.tickets[0] != state.tickets[1]);
    const auto firstTicket = state.tickets[0], secondTicket = state.tickets[1];
    for (uint32_t index = 0; index < 2; ++index)
        CHECK(state.ticketQuery[index] == MMS_OK && state.ticketStates[index].status == MMS_UI_REFRESH_PENDING);
    manager_refresh_checks::Pump(40); state = probe.Read();
    CHECK(guardCalls != 0 && state.ticketStates[0].status == MMS_UI_REFRESH_PENDING && state.ticketStates[1].status == MMS_UI_REFRESH_PENDING);
    for (uint32_t index = 0; index < DEPENDENCY_PROBE_BUTTONS; ++index) CHECK(!state.calls[index] && IsWindowEnabled(buttons[index]));
    if (changeGraph) {
        // Removing A->B retires both captured graph versions. Releasing an edge
        // must use the same SDK handle and leave no old request partly applied.
        CHECK(probe.removeEdge(0) == MMS_OK); state = probe.Read();
        for (uint32_t index = 0; index < 2; ++index)
            CHECK(state.ticketStates[index].status == MMS_UI_REFRESH_FAILED &&
                state.ticketStates[index].result == MMS_E_STALE && state.ticketStates[index].completedKinds == 0);
        CHECK(probe.release(0) == MMS_OK && probe.release(1) == MMS_OK);
        CHECK(probe.request(2) == MMS_OK); allow = true; state = probe.Complete(2);
        CHECK(state.calls[0] == 0 && state.calls[1] == 0 && state.calls[2] == 1 && state.calls[3] == 0 && state.sequence == 1);
        CHECK(IsWindowEnabled(buttons[0]) && IsWindowEnabled(buttons[1]) && !IsWindowEnabled(buttons[2]) && IsWindowEnabled(buttons[3]));
    } else if (!pendingStop) {
        allow = true; state = probe.Complete(2);
        CHECK(state.tickets[0] == firstTicket && state.tickets[1] == secondTicket);
        CHECK(state.calls[0] == 0 && state.calls[1] == 1 && state.calls[2] == 1 && state.calls[3] == 0);
        CHECK(state.sequence == 2 && state.callSequence[1] == 1 && state.callSequence[2] == 2);
        CHECK(IsWindowEnabled(buttons[0]) && !IsWindowEnabled(buttons[1]) && !IsWindowEnabled(buttons[2]) && IsWindowEnabled(buttons[3]));
        CHECK(SendMessageW(buttons[1], BM_GETCHECK, 0, 0) == BST_CHECKED && SendMessageW(buttons[2], BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    CHECK(manager.Ui().Revision() == revision && manager.Ui().PreferencesRevision() == preferences && !manager.CanInvokeEdit());
    CHECK(!manager.Storage().Prepared() && manager.Storage().Records().empty() && !manager.Storage().Poisoned());
    CHECK(!manager.Stop());
    state = probe.Read();
    CHECK(state.stopCount == 1 && state.stopHeldTickets == 2 && count("DEPENDENCY_PROBE_STOP_HELD") == 1);
    CHECK(GetModuleHandleW(L"dependency-refresh-probe.dll") == module && manager.Status()[0].stage == "stopping" && count("DEPENDENCY_PROBE_DESTROY") == 0);
    for (uint32_t index = 0; index < 2; ++index)
        CHECK(state.ticketQuery[index] == MMS_OK && state.ticketStates[index].status ==
            (pendingStop ? MMS_UI_REFRESH_CANCELLED : MMS_UI_REFRESH_COMPLETED) &&
            state.ticketStates[index].result == (pendingStop ? MMS_E_CANCELLED : MMS_OK));
    if (pendingStop) for (uint32_t index = 0; index < DEPENDENCY_PROBE_BUTTONS; ++index) CHECK(!state.calls[index]);
    // Query the retained original service while the ticket holds the DLL in
    // stopping. NOT_FOUND for every original edge proves host-side retirement.
    CHECK(probe.checkEdges() == MMS_OK); state = probe.Read();
    for (uint32_t index = 0; index < DEPENDENCY_PROBE_EDGES; ++index) CHECK(state.edgeCleanup[index] == MMS_E_NOT_FOUND);
    CHECK(probe.release(0) == MMS_OK); state = probe.Read();
    CHECK(state.releaseWasWorker && !state.tickets[0] && state.tickets[1] && !manager.Stop());
    CHECK(GetModuleHandleW(L"dependency-refresh-probe.dll") == module && count("DEPENDENCY_PROBE_DESTROY") == 0);
    CHECK(probe.release(1) == MMS_OK);
    CHECK(manager.Stop() && manager.Status()[0].stage == "stopped" && count("DEPENDENCY_PROBE_DESTROY") == 1);
    CHECK(GetModuleHandleW(L"dependency-refresh-probe.dll") == nullptr);
    CHECK(manager.Ui().commands.empty() && manager.Ui().elements.size() == 6);
    native.reset(); CHECK(manager.Stop());
}
} // namespace manager_dependency_refresh_checks

static void ManagerDependencyRefreshChecks(const fs::path& root, const fs::path& library) {
    manager_dependency_refresh_checks::Run(root, library, false, false);
    manager_dependency_refresh_checks::Run(root, library, true, false);
    manager_dependency_refresh_checks::Run(root, library, false, true);
}
