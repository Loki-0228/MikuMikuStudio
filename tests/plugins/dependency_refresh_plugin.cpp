// Test-only real DLL: no manager internals, fabricated clients or private
// queue entry points. Every edge and ticket goes through negotiated services.
#include "dependency_refresh_plugin.h"
#include <mms/cpp/refresh_dependency.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {
using namespace mms::sdk;
struct Plugin;
struct CommandContext {
    Plugin* owner = nullptr;
    uint32_t index = 0;
    uint32_t enabled = 1, checked = 0;
};
struct Plugin {
    MmsHostV1 host{};
    MmsUiApiV1 ui{};
    RefreshServices refresh;
    RefreshDependencyServices dependencies;
    std::array<RefreshDependency, DEPENDENCY_PROBE_EDGES> edges;
    std::array<RefreshTicket, DEPENDENCY_PROBE_TICKETS> tickets;
    std::array<CommandContext, DEPENDENCY_PROBE_BUTTONS> commands;
    std::array<MmsHandle, DEPENDENCY_PROBE_BUTTONS> commandHandles{};
    DependencyRefreshProbeState state{};
    std::string id;
    std::thread::id uiThread;
    bool begun = false;
};
Plugin* current = nullptr;
constexpr const char* kLabels[] = {"Dependency A", "Dependency B", "Dependency C", "Dependency D"};
constexpr uint32_t kFrom[] = {0, 0, 1}, kTo[] = {1, 2, 2};
MmsString S(const std::string& text) { return {text.data(), text.size()}; }
void Log(Plugin& p, const char* text) {
    if (p.host.log) p.host.log(p.host.context, 1, Text(text));
}
bool HasTickets(const Plugin& p) {
    for (const auto& ticket : p.tickets) if (ticket) return true;
    return false;
}
MmsResult MMS_CALL State(void* context, MmsObjectRef, MmsCommandStateV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    auto& command = *static_cast<CommandContext*>(context);
    auto& p = *command.owner;
    ++p.state.calls[command.index];
    p.state.callSequence[command.index] = ++p.state.sequence;
    *out = {Header<MmsCommandStateV1>(), command.enabled, command.checked, 1, 0, {}};
    return MMS_OK;
}
MmsResult MMS_CALL Invoke(void*, MmsObjectRef, MmsSpan, MmsHandle) { return MMS_OK; }
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    if (!host || !info || !out) return MMS_E_ARGUMENT;
    *out = nullptr;
    if (current) return MMS_E_BUSY;
    auto p = std::make_unique<Plugin>();
    p->host = *host;
    p->id.assign(info->pluginId.data, size_t(info->pluginId.bytes));
    p->uiThread = std::this_thread::get_id();
    p->state.size = sizeof(p->state);
    MmsResult result = host->queryService(host->context, Text("mms.ui"), 1, 0, sizeof(p->ui), &p->ui);
    if (result) return result;
    if ((result = RefreshServices::Query(*host, p->refresh))) return result;
    if ((result = RefreshDependencyServices::Query(*host, p->dependencies))) return result;
    if ((result = p->dependencies.Capabilities(p->state.capabilities))) return result;
    current = p.get();
    *out = p.release();
    return MMS_OK;
}
MmsResult MMS_CALL Start(void* context) {
    auto& p = *static_cast<Plugin*>(context);
    for (uint32_t index = 0; index < DEPENDENCY_PROBE_BUTTONS; ++index) {
        auto& command = p.commands[index]; command.owner = &p; command.index = index;
        const auto commandId = p.id + ".command." + std::to_string(index);
        MmsCommandDescV1 desc{};
        desc.info.h = Header<MmsCommandDescV1>(); desc.info.id = S(commandId);
        desc.info.ownerPlugin = S(p.id); desc.info.label = Text(kLabels[index]);
        desc.context = &command; desc.getState = State; desc.invoke = Invoke;
        auto result = p.ui.registerCommand(p.ui.context, &desc, &p.commandHandles[index]);
        if (result) return result;
        const auto elementId = p.id + ".button." + std::to_string(index);
        MmsUiElementV1 element{};
        element.h = Header<MmsUiElementV1>(); element.id = S(elementId); element.ownerPlugin = S(p.id);
        element.parentId = Text("mms.toolbar.main.extensions"); element.label = Text(kLabels[index]);
        element.kind = MMS_UI_BUTTON; element.commandId = S(commandId);
        if ((result = p.ui.registerElement(p.ui.context, &element, &p.state.buttons[index]))) return result;
    }
    Log(p, "DEPENDENCY_PROBE_START");
    return MMS_OK;
}
void MMS_CALL Event(void*, const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void* context) {
    auto& p = *static_cast<Plugin*>(context); ++p.state.stopCount;
    p.state.stopHeldTickets = 0;
    for (const auto& ticket : p.tickets) p.state.stopHeldTickets += bool(ticket);
    if (HasTickets(p)) { Log(p, "DEPENDENCY_PROBE_STOP_HELD"); return MMS_OK; }
    // Manager has retired this client's graph. Release accepts NOT_FOUND while
    // preserving the same SDK handle until its cleanup has actually succeeded.
    for (auto& edge : p.edges) if (const auto result = edge.Release()) return result;
    Log(p, "DEPENDENCY_PROBE_STOP_OK");
    return MMS_OK;
}
void MMS_CALL Destroy(void* context) {
    auto* p = static_cast<Plugin*>(context); Log(*p, "DEPENDENCY_PROBE_DESTROY");
    if (current == p) current = nullptr;
    delete p;
}
}

extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT;
    if (major != 1 || minor) return MMS_E_VERSION;
    *out = {Header<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy};
    return MMS_OK;
}
extern "C" MmsResult MMS_CALL DependencyProbeBegin(void) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p) return MMS_E_NOT_READY;
        if (p->begun || HasTickets(*p)) return MMS_E_BUSY;
        for (uint32_t index = 0; index < DEPENDENCY_PROBE_BUTTONS; ++index) {
            MmsUiRefreshTargetV1 target{};
            if (const auto result = p->refresh.Target(p->state.buttons[index], target)) return result;
            if (!target.ready || !(target.supportedKinds & MMS_UI_REFRESH_STATE)) return MMS_E_NOT_READY;
            p->state.targets[index] = target.target;
        }
        for (uint32_t index = 0; index < DEPENDENCY_PROBE_EDGES; ++index) {
            const auto result = p->dependencies.ConnectElements(p->refresh, p->state.buttons[kFrom[index]],
                p->state.buttons[kTo[index]], p->edges[index]);
            if (result) return result;
            p->state.edges[index] = p->edges[index].get();
        }
        // The SDK and the actual C table both enforce their UI-thread rule.
        // These attempted edges remain absent, including their output handles.
        MmsUiRefreshDependencyDescV1 desc{}; desc.h = Header<MmsUiRefreshDependencyDescV1>();
        desc.upstream = p->state.targets[0]; desc.dependent = p->state.targets[3];
        MmsHandle rejected = 0x123456789abcdef0ull;
        std::thread worker([&] {
            RefreshDependency edge;
            p->state.workerConnectResult = p->dependencies.ConnectElements(p->refresh,
                p->state.buttons[0], p->state.buttons[3], edge);
            p->state.workerRegisterResult = p->dependencies.api().registerDependency(
                p->dependencies.api().context, &desc, &rejected);
        });
        worker.join();
        if (rejected != 0x123456789abcdef0ull) return MMS_E_FAILED;
        p->begun = true; p->state.sequence = 0;
        for (uint32_t index = 0; index < DEPENDENCY_PROBE_BUTTONS; ++index) {
            p->state.calls[index] = 0; p->state.callSequence[index] = 0;
        }
        return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL DependencyProbeSetState(uint32_t index, uint32_t enabled, uint32_t checked) {
    auto* p = current; if (!p) return MMS_E_NOT_READY;
    if (std::this_thread::get_id() != p->uiThread) return MMS_E_THREAD;
    if (index >= DEPENDENCY_PROBE_BUTTONS || enabled > 1 || checked > 1) return MMS_E_ARGUMENT;
    p->commands[index].enabled = enabled; p->commands[index].checked = checked;
    return MMS_OK;
}
extern "C" MmsResult MMS_CALL DependencyProbeRequest(uint32_t count) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY;
        if (!count || count > DEPENDENCY_PROBE_TICKETS) return MMS_E_ARGUMENT;
        if (HasTickets(*p)) return MMS_E_BUSY;
        const auto refresh = p->refresh;
        const auto target = p->state.targets[0];
        MmsResult result = MMS_OK;
        std::thread worker([&] {
            p->state.requestWasWorker = std::this_thread::get_id() != p->uiThread;
            for (uint32_t index = 0; index < count; ++index) {
                result = refresh.RefreshDependents(target, MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT,
                    p->tickets[index], 0x445047u);
                if (result) break;
            }
        });
        worker.join(); p->state.requestCount = count;
        return result;
    });
}
extern "C" MmsResult MMS_CALL DependencyProbeQuery(DependencyRefreshProbeState* out) {
    return detail::Call([&]() -> MmsResult {
        if (!out || out->size != sizeof(*out)) return MMS_E_ARGUMENT;
        auto* p = current; if (!p) return MMS_E_NOT_READY;
        if (std::this_thread::get_id() != p->uiThread) return MMS_E_THREAD;
        auto state = p->state;
        for (uint32_t index = 0; index < DEPENDENCY_PROBE_TICKETS; ++index) {
            state.tickets[index] = p->tickets[index].get();
            state.ticketQuery[index] = p->tickets[index] ? p->tickets[index].Query(state.ticketStates[index]) : MMS_E_NOT_READY;
        }
        *out = state; return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL DependencyProbeRelease(uint32_t index) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p) return MMS_E_NOT_READY;
        if (index >= DEPENDENCY_PROBE_TICKETS) return MMS_E_ARGUMENT;
        MmsResult result = MMS_E_FAILED;
        std::thread worker([&] {
            p->state.releaseWasWorker = std::this_thread::get_id() != p->uiThread;
            result = p->tickets[index].Release();
        });
        worker.join(); return result;
    });
}
extern "C" MmsResult MMS_CALL DependencyProbeRemoveEdge(uint32_t index) {
    auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY;
    if (std::this_thread::get_id() != p->uiThread) return MMS_E_THREAD;
    if (index >= DEPENDENCY_PROBE_EDGES) return MMS_E_ARGUMENT;
    return p->edges[index].Release();
}
extern "C" MmsResult MMS_CALL DependencyProbeCheckEdges(void) {
    auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY;
    if (std::this_thread::get_id() != p->uiThread) return MMS_E_THREAD;
    const auto& api = p->dependencies.api();
    for (uint32_t index = 0; index < DEPENDENCY_PROBE_EDGES; ++index)
        p->state.edgeCleanup[index] = api.unregisterDependency(api.context, p->state.edges[index]);
    return MMS_OK;
}
