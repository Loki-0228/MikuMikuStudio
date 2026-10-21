// Test-only DLL. All registrations, publications, snapshots and subscriptions
// use negotiated public services; this file has no Manager/registry headers.
#include "subscription_refresh_probe.h"
#include <mms/cpp/refresh_subscription.hpp>
#include <mms/edit.h>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace {
using namespace mms::sdk;
constexpr uint32_t kKinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT;
constexpr MmsHandle kSentinel = UINT64_C(0x123456789abcdef0);
struct Plugin {
    MmsHostV1 host{}; MmsUiApiV1 ui{}; MmsEditApiV1 edit{};
    RefreshServices refresh; DataServices data; RefreshSubscriptionServices subscriptions;
    DataSource source; DataBinding binding; RefreshSubscription subscription;
    RefreshTicket ticket; DataDisplay held;
    MmsHandle command = 0, button = 0;
    std::string id; std::thread::id uiThread;
    SubscriptionRefreshProbeState state{};
};
Plugin* current = nullptr;
MmsString S(const std::string& value) { return {value.data(), value.size()}; }
void Log(Plugin& p, const char* value) { if (p.host.log) p.host.log(p.host.context, 1, Text(value)); }
bool Ui(const Plugin& p) { return std::this_thread::get_id() == p.uiThread; }
int32_t DisplayValue(const DataDisplay& value) {
    int32_t result = -1234567; const auto bytes = value.bytes();
    if (bytes.data && bytes.bytes == sizeof(result)) std::memcpy(&result, bytes.data, sizeof(result));
    return result;
}
MmsUiRefreshSubscriptionDescV1 Desc(const Plugin& p) {
    MmsUiRefreshSubscriptionDescV1 desc{}; desc.h = Header<MmsUiRefreshSubscriptionDescV1>();
    desc.binding = p.binding.get(); desc.kinds = kKinds; return desc;
}
MmsResult Subscribe(Plugin& p) {
    const auto result = p.subscriptions.Subscribe(p.binding, p.subscription, kKinds);
    if (!result) p.state.originalSubscription = p.subscription.get(); return result;
}
MmsResult MMS_CALL Prepare(void* context, const MmsUiDataPrepareV1* input) {
    return detail::Call([&]() -> MmsResult {
        auto& p = *static_cast<Plugin*>(context); auto& s = p.state;
        if (!input || input->sourceBytes.bytes != sizeof(int32_t) || !input->sourceBytes.data) return MMS_E_ARGUMENT;
        const auto index = s.prepareCount++; s.prepareSequence = ++s.sequence;
        s.prepareWasUi &= Ui(p); s.preparedRevision = input->sourceRevision;
        s.prepareIdentity &= input->source == p.source.get() && input->target.element == s.target.element &&
            input->target.instanceGeneration == s.target.instanceGeneration && input->target.projectEpoch == s.target.projectEpoch;
        int32_t value = 0; std::memcpy(&value, input->sourceBytes.data, sizeof(value));
        if (index >= SUBSCRIPTION_PROBE_TRACE) { ++s.traceOverflow; return MMS_E_FAILED; }
        s.traceValues[index] = value; s.traceRevisions[index] = input->sourceRevision;
        const auto doubled = int64_t(value) * 2;
        if (doubled < (std::numeric_limits<int32_t>::min)() || doubled > (std::numeric_limits<int32_t>::max)()) return MMS_E_ARGUMENT;
        s.preparedValue = int32_t(doubled);
        DataDisplay old;
        if (p.binding.Acquire(old) == MMS_OK) {
            s.previousValue = DisplayValue(old); s.previousSourceRevision = old.value().sourceRevision;
        }
        ++s.editCalls; MmsHandle editOutput = kSentinel;
        s.editBegin = p.edit.begin(p.edit.context, Text("subscription read-only prepare"), s.editExpectedRevision, &editOutput);
        s.editOutputPreserved &= editOutput == kSentinel;
        if (!s.editBegin) p.edit.cancel(p.edit.context, editOutput);
        s.editUndo = p.edit.undo(p.edit.context); s.editRedo = p.edit.redo(p.edit.context);
        s.directMutation = p.ui.unregisterItem(p.ui.context, p.button);
        const auto desc = Desc(p); MmsHandle output = kSentinel;
        s.readOnlySubscribe = p.subscriptions.api().subscribe(p.subscriptions.api().context, &desc, &output);
        s.readOnlyOutputPreserved &= output == kSentinel;
        if (!s.readOnlySubscribe) p.subscriptions.api().unsubscribe(p.subscriptions.api().context, output);
        s.readOnlyUnsubscribe = p.subscriptions.api().unsubscribe(p.subscriptions.api().context, p.subscription.get());
        MmsUiDataSourceStateV1 ignored{}; ignored.revision = UINT64_C(0x5566778899);
        s.readOnlyPublish = p.source.Publish({&value, sizeof(value)}, s.sourceRevision, ignored);
        s.readOnlyOutputPreserved &= ignored.revision == UINT64_C(0x5566778899);
        s.prepareWrite = p.data.WritePrepared(input->builder, {&s.preparedValue, sizeof(s.preparedValue)});
        s.everyReadOnlyDenied &= s.editBegin == MMS_E_BUSY && s.editUndo == MMS_E_BUSY && s.editRedo == MMS_E_BUSY &&
            s.directMutation == MMS_E_BUSY && s.readOnlySubscribe == MMS_E_BUSY && s.readOnlyUnsubscribe == MMS_E_BUSY && s.readOnlyPublish == MMS_E_BUSY;
        Log(p, "SUBSCRIPTION_PROBE_PREPARE"); return s.prepareWrite;
    });
}
MmsResult MMS_CALL State(void* context, MmsObjectRef, MmsCommandStateV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    auto& p = *static_cast<Plugin*>(context); auto& s = p.state;
    ++s.stateCount; s.stateSequence = ++s.sequence;
    *out = {Header<MmsCommandStateV1>(), 1, 0, 1, 0, {}};
    if (p.binding) {
        DataDisplay display;
        if (p.binding.Acquire(display) == MMS_OK) {
            s.stateValue = DisplayValue(display); out->enabled = s.stateValue >= 0;
            out->checked = (static_cast<uint32_t>(s.stateValue / 2) & 1u);
        }
    }
    return MMS_OK;
}
MmsResult MMS_CALL Invoke(void*, MmsObjectRef, MmsSpan, MmsHandle) { return MMS_OK; }
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    return detail::Call([&]() -> MmsResult {
        if (!host || !info || !out) return MMS_E_ARGUMENT; *out = nullptr;
        if (current) return MMS_E_BUSY;
        auto p = std::make_unique<Plugin>(); p->host = *host; p->uiThread = std::this_thread::get_id();
        p->id.assign(info->pluginId.data, size_t(info->pluginId.bytes)); p->state.size = sizeof(p->state);
        p->state.prepareIdentity = p->state.prepareWasUi = p->state.editOutputPreserved = p->state.readOnlyOutputPreserved = p->state.everyReadOnlyDenied = 1;
        p->state.displayValue = p->state.stateValue = p->state.heldValue = p->state.previousValue = -1234567;
        auto result = host->queryService(host->context, Text("mms.ui"), 1, 0, sizeof(p->ui), &p->ui); if (result) return result;
        if ((result = RefreshServices::Query(*host, p->refresh))) return result;
        if ((result = DataServices::Query(*host, p->data))) return result;
        if ((result = RefreshSubscriptionServices::Query(*host, p->subscriptions))) return result;
        if ((result = p->subscriptions.Capabilities(p->state.capabilities))) return result;
        p->state.editQuery = host->queryService(host->context, Text("mms.edit"), 1, 0, sizeof(p->edit), &p->edit);
        if (p->state.editQuery) return p->state.editQuery;
        if (p->edit.h.size < sizeof(p->edit) || p->edit.h.major != 1 || !p->edit.begin || !p->edit.cancel || !p->edit.undo || !p->edit.redo) return MMS_E_VERSION;
        p->state.editAvailable = 1; current = p.get(); *out = p.release(); return MMS_OK;
    });
}
MmsResult MMS_CALL Start(void* context) {
    return detail::Call([&]() -> MmsResult {
        auto& p = *static_cast<Plugin*>(context); const auto commandId = p.id + ".command", elementId = p.id + ".button";
        MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>(); command.info.id = S(commandId);
        command.info.ownerPlugin = S(p.id); command.info.label = Text("Subscription probe");
        command.context = &p; command.getState = State; command.invoke = Invoke;
        auto result = p.ui.registerCommand(p.ui.context, &command, &p.command); if (result) return result;
        MmsUiElementV1 element{}; element.h = Header<MmsUiElementV1>(); element.id = S(elementId); element.ownerPlugin = S(p.id);
        element.parentId = Text("mms.toolbar.main.extensions"); element.label = Text("Subscription probe");
        element.kind = MMS_UI_BUTTON; element.commandId = S(commandId);
        if ((result = p.ui.registerElement(p.ui.context, &element, &p.button))) return result;
        if ((result = p.data.RegisterSource(S(p.id + ".data"), sizeof(int32_t), p.source))) return result;
        Log(p, "SUBSCRIPTION_PROBE_START"); return MMS_OK;
    });
}
void MMS_CALL Event(void*, const MmsEventV1*) {}
bool Retired(MmsResult value) { return value == MMS_E_STALE || value == MMS_E_NOT_FOUND || value == MMS_E_NOT_READY; }
MmsResult MMS_CALL Stop(void* context) {
    return detail::Call([&]() -> MmsResult {
        auto& p = *static_cast<Plugin*>(context); auto& s = p.state; ++s.stopCount; s.prepareAtStop = s.prepareCount;
        MmsUiRefreshSubscriptionStateV1 state{}; state.h = Header<MmsUiRefreshSubscriptionStateV1>(); state.latestRevision = 878;
        const auto before = state;
        s.stopQuery = p.subscriptions.api().query(p.subscriptions.api().context, s.originalSubscription, &state);
        s.stopOutputPreserved = !std::memcmp(&before, &state, sizeof(state));
        s.stopUnsubscribe = p.subscriptions.api().unsubscribe(p.subscriptions.api().context, s.originalSubscription);
        s.stopRelease = p.subscription.Release(); if (s.stopRelease) return s.stopRelease;
        s.stopHeldDisplay = bool(p.held);
        if (p.held || p.ticket) { Log(p, "SUBSCRIPTION_PROBE_STOP_HELD"); return MMS_OK; }
        if (const auto result = p.binding.Release()) if (!Retired(result)) return result;
        if (const auto result = p.source.Release()) if (!Retired(result)) return result;
        if (p.button) { const auto result = p.ui.unregisterItem(p.ui.context, p.button); if (result && !Retired(result)) return result; p.button = 0; }
        if (p.command) { const auto result = p.ui.unregisterItem(p.ui.context, p.command); if (result && !Retired(result)) return result; p.command = 0; }
        Log(p, "SUBSCRIPTION_PROBE_STOP_OK"); return MMS_OK;
    });
}
void MMS_CALL Destroy(void* context) {
    auto* p = static_cast<Plugin*>(context); Log(*p, "SUBSCRIPTION_PROBE_DESTROY");
    if (current == p) current = nullptr; delete p;
}
}

extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT; if (major != 1 || minor) return MMS_E_VERSION;
    *out = {Header<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy}; return MMS_OK;
}
extern "C" MmsResult MMS_CALL SubscriptionProbeBegin(uint64_t historyRevision, uint32_t subscribeNow) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD;
        if (!historyRevision || subscribeNow > 1) return MMS_E_ARGUMENT; if (p->binding || p->subscription) return MMS_E_BUSY;
        MmsUiRefreshTargetV1 target{}; auto result = p->refresh.Target(p->button, target); if (result) return result;
        // DATA becomes available through this very binding. Before binding,
        // query only the native presentation's STATE and REPAINT capabilities.
        constexpr auto presentationKinds = MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT;
        if (!target.ready || (target.supportedKinds & presentationKinds) != presentationKinds) return MMS_E_NOT_READY;
        p->state.editExpectedRevision = historyRevision; p->state.target = target.target;
        if ((result = p->data.Bind(target.target, p->source, sizeof(int32_t), p->binding, Prepare, p))) return result;
        return subscribeNow ? Subscribe(*p) : MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL SubscriptionProbeSubscribe(void) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->binding) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; return Subscribe(*p);
    });
}
extern "C" MmsResult MMS_CALL SubscriptionProbePublish(int32_t first, uint32_t count) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->source) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD;
        if (!count || count > 512 || int64_t(first) + count - 1 > (std::numeric_limits<int32_t>::max)()) return MMS_E_ARGUMENT;
        MmsResult result = MMS_OK; auto revision = p->state.sourceRevision; int32_t last = first; uint32_t published = 0;
        std::thread worker([&] {
            p->state.publishWasWorker = !Ui(*p);
            for (uint32_t index = 0; index < count; ++index) {
                int32_t bytes = int32_t(int64_t(first) + index); MmsUiDataSourceStateV1 state{};
                result = p->source.Publish({&bytes, sizeof(bytes)}, revision, state);
                bytes = (std::numeric_limits<int32_t>::min)(); if (result) break;
                revision = state.revision; last = int32_t(int64_t(first) + index); ++published;
            }
        }); worker.join();
        p->state.publishResult = result; p->state.publishCount += published;
        if (published) { p->state.sourceRevision = revision; p->state.sourceValue = last; } return result;
    });
}
extern "C" MmsResult MMS_CALL SubscriptionProbeCheckThreads(void) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->subscription) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD;
        auto& s = p->state; const auto desc = Desc(*p); MmsHandle output = kSentinel;
        MmsUiRefreshSubscriptionCapabilitiesV1 caps{}; caps.h = Header<MmsUiRefreshSubscriptionCapabilitiesV1>(); caps.flags = 917;
        std::thread worker([&] {
            s.workerQuery = p->subscription.State(s.workerState);
            s.workerSubscribe = p->subscriptions.api().subscribe(p->subscriptions.api().context, &desc, &output);
            RefreshSubscription rejected; s.workerSdkSubscribe = p->subscriptions.Subscribe(p->binding, rejected, kKinds);
            s.workerUnsubscribe = p->subscriptions.api().unsubscribe(p->subscriptions.api().context, p->subscription.get());
            s.workerCaps = p->subscriptions.api().getCapabilities(p->subscriptions.api().context, &caps);
        }); worker.join(); s.workerOutputPreserved = output == kSentinel && caps.flags == 917; return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL SubscriptionProbeQuery(SubscriptionRefreshProbeState* out) {
    return detail::Call([&]() -> MmsResult {
        if (!out || out->size != sizeof(*out)) return MMS_E_ARGUMENT; auto* p = current; if (!p) return MMS_E_NOT_READY;
        if (!Ui(*p)) return MMS_E_THREAD; auto state = p->state;
        state.button = p->button; state.source = p->source.get(); state.binding = p->binding.get(); state.subscription = p->subscription.get();
        state.subscriptionQuery = p->subscription ? p->subscription.State(state.subscriptionState) : MMS_E_NOT_READY;
        state.ticket = p->ticket.get(); state.ticketQuery = p->ticket ? p->ticket.Query(state.ticketState) : MMS_E_NOT_READY;
        DataDisplay display; state.displayQuery = p->binding.Acquire(display);
        if (!state.displayQuery) { state.displayValue = DisplayValue(display); state.displaySourceRevision = display.value().sourceRevision; state.displayRevision = display.value().displayRevision; }
        if (p->held) { state.heldLease = p->held.value().lease; state.heldValue = DisplayValue(p->held); state.heldSourceRevision = p->held.value().sourceRevision; }
        *out = state; return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL SubscriptionProbeRequestOld(void) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->binding || !p->state.sourceRevision) return MMS_E_NOT_READY;
        if (!Ui(*p)) return MMS_E_THREAD; if (p->ticket) return MMS_E_BUSY; ++p->state.explicitRequests;
        return p->binding.Refresh(p->refresh, p->state.target, p->state.sourceRevision, p->ticket, MMS_UI_REFRESH_DATA);
    });
}
extern "C" MmsResult MMS_CALL SubscriptionProbeReleaseTicket(void) { return current ? current->ticket.Release() : MMS_E_NOT_READY; }
extern "C" MmsResult MMS_CALL SubscriptionProbeAcquireHold(void) {
    return current ? current->binding.Acquire(current->held) : MMS_E_NOT_READY;
}
extern "C" MmsResult MMS_CALL SubscriptionProbeReleaseHold(void) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p) return MMS_E_NOT_READY; MmsResult result = MMS_E_FAILED;
        std::thread worker([&] { result = p->held.Release(); }); worker.join(); p->state.heldRelease = result; return result;
    });
}
