// Real DLL; service calls and registrations use only the public C ABI/SDK.
#include "scoped_property_probe.h"
#include <mms/cpp/data_refresh.hpp>
#include <mms/edit.h>
#include <mms/refresh_state.h>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
namespace {
using namespace mms::sdk;
constexpr uint32_t kinds = MMS_UI_REFRESH_DATA | MMS_UI_REFRESH_STATE | MMS_UI_REFRESH_REPAINT;
constexpr MmsHandle sentinel = UINT64_C(0x123456789abcdef0);
struct Plugin;
struct Context { Plugin* owner = nullptr; unsigned index = 0; };
struct Plugin {
    MmsHostV1 host{}; MmsUiApiV1 ui{}; MmsUiRefreshDependencyApiV1 graph{};
    MmsUiPropertyRefreshApiV1 legacy{}; MmsUiPropertyRefreshApiV1_1 properties{};
    MmsUiRefreshStateContentApiV1 content{};
    RefreshServices refresh; DataServices data; DataSource source; std::array<DataBinding, 3> bindings;
    std::array<std::atomic<int64_t>, 3> values{};
    std::array<Context, 3> contexts; ScopedPropertyProbeState state{};
    std::thread::id thread; std::string id; uint32_t flags = 0; bool begun = false, probed = false;
};
Plugin* current = nullptr;
bool Ui(const Plugin& p) { return p.thread == std::this_thread::get_id(); }
void Log(Plugin& p, const char* text) { if (p.host.log) p.host.log(p.host.context, 1, Text(text)); }
unsigned Trigger(const Plugin& p) { return p.state.scope == MMS_UI_REFRESH_SELF ? 1u : 0u; }
MmsUiPropertyRefreshScopedSubscriptionDescV1 Desc(const Plugin& p) {
    MmsUiPropertyRefreshScopedSubscriptionDescV1 d{}; d.h = Header<MmsUiPropertyRefreshScopedSubscriptionDescV1>();
    d.property = p.state.refs[Trigger(p)]; d.seed = p.state.scope == MMS_UI_REFRESH_SELF ? p.state.targetRefs[0] : p.state.seed;
    d.scope = p.state.scope; d.kinds = kinds; d.flags = p.flags; return d;
}
void Trace(Plugin& p, unsigned phase, unsigned target, int64_t value, uint64_t revision = 0) {
    auto& s = p.state; if (!p.begun) return;
    if (s.events < SCOPED_PROPERTY_TRACE) s.trace[s.events++] = {phase, target, value, revision}; else ++s.overflow;
}
int64_t Display(Plugin& p, unsigned i, uint64_t& revision) {
    revision = 0; DataDisplay display;
    if (!p.bindings[i] || p.bindings[i].Acquire(display) != MMS_OK || display.bytes().bytes != sizeof(int64_t)) return INT64_MIN;
    int64_t value = 0; std::memcpy(&value, display.bytes().data, sizeof(value)); revision = display.value().sourceRevision; return value;
}
void ProbeReadOnly(Plugin& p) {
    if (!p.begun || p.probed) return;
    p.probed = true; auto& s = p.state; ++s.readOnlyChecks; const auto desc = Desc(p); MmsHandle output = sentinel;
    s.readNotify = p.properties.notify(p.properties.context, &desc.property);
    s.readSubscribe = p.properties.subscribeScoped(p.properties.context, &desc, &output); s.outputsPreserved &= output == sentinel;
    s.readUnsubscribe = p.properties.unsubscribe(p.properties.context, s.subscription);
    MmsUiRefreshDependencyDescV1 edge{Header<MmsUiRefreshDependencyDescV1>(), s.seed, s.targetRefs[2], 0, 0}; output = sentinel;
    s.readGraph = p.graph.registerDependency(p.graph.context, &edge, &output); s.outputsPreserved &= output == sentinel;
    MmsUiPropertyRefreshScopedSubscriptionStateV1 queried{}; queried.h = Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>();
    const int64_t publishedValue = 777; MmsUiDataSourceStateV1 published{};
    // The established DATA contract permits worker publication during a
    // read-only UI callback. Property notify remains BUSY on that same worker.
    std::thread worker([&] {
        s.readWorkerNotify = p.properties.notify(p.properties.context, &desc.property);
        s.readWorkerQuery = p.properties.queryScoped(p.properties.context, s.subscription, &queried);
        s.readWorkerPublish = p.source.Publish({&publishedValue, sizeof(publishedValue)}, s.sourceRevision, published);
    }); worker.join();
    if (s.readWorkerPublish == MMS_OK) { s.sourceRevision = published.revision; s.sourceValue = publishedValue; }
    s.identity &= s.readWorkerQuery == MMS_OK && queried.subscription == s.subscription;
}
MmsResult MMS_CALL Read(void* context, MmsObjectRef object, MmsValue* out, MmsHandle* lease) {
    return detail::Call([&]() -> MmsResult {
        if (!out || !lease) return MMS_E_ARGUMENT; auto& c = *static_cast<Context*>(context); auto& p = *c.owner; auto& s = p.state;
        ++s.reads[c.index]; ++s.readDepth; if (s.readDepth > s.maxReadDepth) s.maxReadDepth = s.readDepth;
        struct Guard { uint32_t& depth; ~Guard() { --depth; } } guard{s.readDepth};
        s.identity &= !object.id.lo && !object.id.hi && !object.generation; s.callbacksUi &= Ui(p);
        ProbeReadOnly(p); const auto value = p.values[c.index].load(); s.lastRead[c.index] = value;
        if (c.index) Trace(p, 1, c.index - 1, value);
        if (s.failMask & (1u << c.index)) return MMS_E_FAILED;
        *out = {}; out->type = MMS_VALUE_INT; out->value.signedValue = value; *lease = 100 + c.index; ++s.successes[c.index];
        Log(p, "SCOPED_PROPERTY_READ"); return MMS_OK;
    });
}
void MMS_CALL Release(void* context, MmsHandle lease) {
    auto& c = *static_cast<Context*>(context); auto& s = c.owner->state; ++s.releases[c.index];
    s.identity &= lease == 100 + c.index; s.callbacksUi &= Ui(*c.owner);
}
MmsResult MMS_CALL Stage(void* context, MmsHandle, MmsObjectRef, const MmsValue*) {
    ++static_cast<Context*>(context)->owner->state.stages; return MMS_E_UNSUPPORTED;
}
MmsPropertyDescV1 Property(Plugin& p, unsigned i) {
    // Strings are consumed by register/update before their backing locals die.
    // The descriptor's ID is filled by the immediate caller.
    MmsPropertyDescV1 d{}; d.info.h = Header<MmsPropertyDescV1>(); d.info.ownerPlugin = Text(p.id);
    d.info.label = Text(i == 0 ? "Independent trigger" : i == 1 ? "Scoped property A" : "Scoped property B");
    d.info.valueType = MMS_VALUE_INT; d.info.flags = (i == 1 && p.id.find("draft") != std::string::npos) ? MMS_PROPERTY_UNDOABLE : MMS_PROPERTY_READ_ONLY;
    d.context = &p.contexts[i]; d.read = Read; d.releaseValue = Release; d.stage = Stage; return d;
}
MmsResult MMS_CALL Prepare(void* context, const MmsUiDataPrepareV1* input) {
    return detail::Call([&]() -> MmsResult {
        auto& c = *static_cast<Context*>(context); auto& p = *c.owner; auto& s = p.state;
        if (!input || input->sourceBytes.bytes != sizeof(int64_t) || !input->sourceBytes.data) return MMS_E_ARGUMENT;
        int64_t value = 0; std::memcpy(&value, input->sourceBytes.data, sizeof(value)); ++s.prepares[c.index];
        s.callbacksUi &= Ui(p); s.identity &= input->source == s.source && input->target.element == s.targetRefs[c.index].element &&
            input->target.instanceGeneration == s.targetRefs[c.index].instanceGeneration && input->target.projectEpoch == s.targetRefs[c.index].projectEpoch;
        s.prepared[c.index] = value; s.preparedRevisions[c.index] = input->sourceRevision; Trace(p, 1, c.index, value, input->sourceRevision);
        return p.data.WritePrepared(input->builder, input->sourceBytes);
    });
}
void StateTrace(Plugin& p, unsigned i) {
    auto& s = p.state; if (!p.begun) return; ++s.states[i]; s.callbacksUi &= Ui(p);
    uint64_t revision = 0; const auto value = p.bindings[i] ? Display(p, i, revision) : i < 2 ? s.lastRead[i + 1] : INT64_MIN;
    s.stateValues[i] = value; s.stateRevisions[i] = revision; Trace(p, 2, i, value, revision);
}
MmsResult MMS_CALL InputState(void* context, const MmsUiTargetRefV1* target, MmsHandle builder) {
    auto& c = *static_cast<Context*>(context); auto& p = *c.owner;
    p.state.identity &= target && target->element == p.state.targets[c.index]; StateTrace(p, c.index);
    MmsUiStateContentV1 value{}; value.h = Header<MmsUiStateContentV1>(); value.fields = MMS_UI_STATE_DESCRIPTION;
    value.description = Text("Original scoped property INPUT"); return p.content.writePrepared(p.content.context, builder, &value);
}
MmsResult MMS_CALL ButtonState(void* context, MmsObjectRef, MmsCommandStateV1* out) {
    if (!out) return MMS_E_ARGUMENT; auto& p = *static_cast<Plugin*>(context); StateTrace(p, 2);
    *out = {Header<MmsCommandStateV1>(), 1, 0, 1, 0, {}}; return MMS_OK;
}
MmsResult MMS_CALL Invoke(void*, MmsObjectRef, MmsSpan, MmsHandle) { return MMS_OK; }
MmsResult Graph(Plugin& p, uint32_t mask) {
    if (mask & ~7u) return MMS_E_ARGUMENT;
    for (unsigned i = 0; i < 3; ++i) if (p.state.edges[i] && !(mask & (1u << i))) {
        const auto result = p.graph.unregisterDependency(p.graph.context, p.state.edges[i]); if (result) return result; p.state.edges[i] = 0;
    }
    for (unsigned i = 0; i < 3; ++i) if (!p.state.edges[i] && (mask & (1u << i))) {
        const MmsUiRefreshDependencyDescV1 edge{Header<MmsUiRefreshDependencyDescV1>(), i == 2 ? p.state.targetRefs[0] : p.state.seed,
            p.state.targetRefs[i], 0, 0}; const auto result = p.graph.registerDependency(p.graph.context, &edge, &p.state.edges[i]); if (result) return result;
    }
    p.state.edgeMask = mask; return MMS_OK;
}
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    return detail::Call([&]() -> MmsResult {
        if (!host || !info || !out || !host->queryService) return MMS_E_ARGUMENT; if (current) return MMS_E_BUSY;
        auto p = std::make_unique<Plugin>(); p->host = *host; p->thread = std::this_thread::get_id();
        p->id.assign(info->pluginId.data, size_t(info->pluginId.bytes)); auto& s = p->state; s.size = sizeof(s);
        s.identity = s.callbacksUi = s.outputsPreserved = 1; for (unsigned i = 0; i < 3; ++i) { p->contexts[i] = {p.get(), i}; p->values[i] = 10 + i; }
        auto result = host->queryService(host->context, Text("mms.ui"), 1, 0, sizeof(p->ui), &p->ui); if (result) return result;
        if ((result = RefreshServices::Query(*host, p->refresh))) return result;
        if ((result = DataServices::Query(*host, p->data))) return result;
        if ((result = host->queryService(host->context, Text("mms.ui.refresh.dependencies"), 1, 0, sizeof(p->graph), &p->graph))) return result;
        if ((result = host->queryService(host->context, Text("mms.ui.refresh.state"), 1, 0, sizeof(p->content), &p->content))) return result;
        if ((result = host->queryService(host->context, Text("mms.ui.refresh.property"), 1, 0, sizeof(p->legacy), &p->legacy))) return result;
        if ((result = host->queryService(host->context, Text("mms.ui.refresh.property"), 1, 1, sizeof(p->properties), &p->properties))) return result;
        s.oldSize = p->legacy.h.size; s.oldMinor = p->legacy.h.minor; s.newSize = p->properties.h.size; s.newMinor = p->properties.h.minor;
        s.oldCaps.h = Header<MmsUiPropertyRefreshCapabilitiesV1>(); if ((result = p->legacy.getCapabilities(p->legacy.context, &s.oldCaps))) return result;
        s.capabilities.h = Header<MmsUiPropertyRefreshScopedCapabilitiesV1>(); if ((result = p->properties.getScopedCapabilities(p->properties.context, &s.capabilities))) return result;
        MmsUiPropertyRefreshApiV1_1 unchanged{}; std::memset(&unchanged, 0x5a, sizeof(unchanged)); const auto before = unchanged;
        s.shortQuery = host->queryService(host->context, Text("mms.ui.refresh.property"), 1, 1, sizeof(p->legacy), &unchanged);
        s.negotiationPreserved = !std::memcmp(&unchanged, &before, sizeof(unchanged)); MmsEditApiV1 edit{};
        s.editQuery = host->queryService(host->context, Text("mms.edit"), 1, 0, sizeof(edit), &edit);
        current = p.get(); *out = p.release(); return MMS_OK;
    });
}
MmsResult MMS_CALL Start(void* context) {
    return detail::Call([&]() -> MmsResult {
        auto& p = *static_cast<Plugin*>(context); auto& s = p.state;
        for (unsigned i = 0; i < 3; ++i) { const auto id = p.id + ".property." + std::to_string(i); auto d = Property(p, i); d.info.id = Text(id);
            const auto result = p.ui.registerProperty(p.ui.context, &d, &s.properties[i]); if (result) return result; }
        const auto seedId = p.id + ".seed"; MmsUiElementV1 seed{}; seed.h = Header<MmsUiElementV1>(); seed.id = Text(seedId);
        seed.ownerPlugin = Text(p.id); seed.parentId = Text("mms.toolbar.main.extensions"); seed.kind = MMS_UI_GROUP; seed.capabilities = MMS_UI_CONTAINER | MMS_UI_LAYOUT;
        auto result = p.ui.registerElement(p.ui.context, &seed, &s.seedHandle); if (result) return result;
        for (unsigned i = 0; i < 2; ++i) { const auto id = p.id + ".input." + std::to_string(i), property = p.id + ".property." + std::to_string(i + 1);
            MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = Text(id); e.ownerPlugin = Text(p.id);
            e.parentId = Text(i ? "mms.properties.main.extensions" : "mms.toolbar.main.extensions");
            if (!i && p.id.find("subtree") != std::string::npos) e.parentId = Text(seedId);
            e.label = Text(i ? "Scoped property B" : "Scoped property A"); e.propertyId = Text(property); e.kind = MMS_UI_INPUT;
            e.capabilities = MMS_UI_LAYOUT | MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_RESIZE;
            if ((result = p.ui.registerElement(p.ui.context, &e, &s.targets[i]))) return result;
        }
        const auto commandId = p.id + ".command", buttonId = p.id + ".button";
        MmsCommandDescV1 command{}; command.info.h = Header<MmsCommandDescV1>(); command.info.id = Text(commandId); command.info.ownerPlugin = Text(p.id);
        command.info.label = Text("Scoped data C"); command.info.flags = MMS_COMMAND_INVOKE_ONLY; command.context = &p; command.getState = ButtonState; command.invoke = Invoke;
        if ((result = p.ui.registerCommand(p.ui.context, &command, &s.command))) return result;
        MmsUiElementV1 e{}; e.h = Header<MmsUiElementV1>(); e.id = Text(buttonId); e.ownerPlugin = Text(p.id);
        e.parentId = Text("mms.toolbar.main.extensions"); e.label = command.info.label; e.commandId = Text(commandId); e.kind = MMS_UI_BUTTON; e.capabilities = MMS_UI_LAYOUT;
        if ((result = p.ui.registerElement(p.ui.context, &e, &s.targets[2]))) return result;
        Log(p, "SCOPED_PROPERTY_START"); return MMS_OK;
    });
}
void MMS_CALL Event(void*, const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void* context) {
    auto& p = *static_cast<Plugin*>(context); auto& s = p.state; ++s.stops; s.readsAtStop = s.reads[0] + s.reads[1] + s.reads[2];
    MmsUiPropertyRefreshScopedSubscriptionStateV1 value{}; value.h = Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>(); value.workGeneration = sentinel; const auto before = value;
    s.stopQuery = p.properties.queryScoped(p.properties.context, s.subscription, &value); s.stopOutputPreserved = !std::memcmp(&before, &value, sizeof(value));
    s.stopNotify = p.properties.notify(p.properties.context, &s.refs[Trigger(p)]); s.stopUnsubscribe = p.properties.unsubscribe(p.properties.context, s.subscription);
    if (s.stopUnsubscribe == MMS_OK || s.stopUnsubscribe == MMS_E_STALE) s.subscription = 0; Log(p, "SCOPED_PROPERTY_STOP"); return MMS_OK;
}
void MMS_CALL Destroy(void* context) { auto* p = static_cast<Plugin*>(context); Log(*p, "SCOPED_PROPERTY_DESTROY"); if (current == p) current = nullptr; delete p; }
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out)) return MMS_E_ARGUMENT; if (major != 1 || minor) return MMS_E_VERSION;
    *out = {Header<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy}; return MMS_OK;
}
extern "C" MmsResult MMS_CALL ScopedPropertyBegin(uint32_t scope, uint32_t mask, uint32_t flags) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; if (p->begun) return MMS_E_BUSY;
        if (scope != MMS_UI_REFRESH_SELF && scope != MMS_UI_REFRESH_SUBTREE && scope != MMS_UI_REFRESH_DEPENDENTS) return MMS_E_ARGUMENT;
        auto& s = p->state; s.scope = scope; p->flags = flags;
        for (unsigned i = 0; i < 3; ++i) { s.refs[i].h = Header<MmsUiPropertyRefV1>(); const auto r = p->properties.getPropertyRef(p->properties.context, s.properties[i], &s.refs[i]); if (r) return r;
            MmsUiRefreshTargetV1 target{}; const auto result = p->refresh.Target(s.targets[i], target); if (result) return result; if (!target.ready) return MMS_E_NOT_READY; s.targetRefs[i] = target.target; }
        MmsUiRefreshTargetV1 seed{}; auto result = p->refresh.Target(s.seedHandle, seed); if (result) return result; if (!seed.ready) return MMS_E_NOT_READY; s.seed = seed.target;
        for (unsigned i = 0; i < 2; ++i) { MmsUiStateContentDescV1 d{}; d.h = Header<MmsUiStateContentDescV1>(); d.target = s.targetRefs[i];
            d.context = &p->contexts[i]; d.prepare = InputState; d.fields = MMS_UI_STATE_DESCRIPTION;
            if ((result = p->content.registerProvider(p->content.context, &d, &s.providers[i]))) return result; }
        if ((result = p->data.RegisterSource(Text(p->id + ".source"), sizeof(int64_t), p->source))) return result; s.source = p->source.get();
        const int64_t value = 10; MmsUiDataSourceStateV1 published{};
        if ((result = p->source.Publish({&value, sizeof(value)}, 0, published))) return result; s.sourceRevision = published.revision; s.sourceValue = value;
        if ((result = p->data.Bind(s.targetRefs[2], p->source, sizeof(value), p->bindings[2], Prepare, &p->contexts[2]))) return result; s.bindings[2] = p->bindings[2].get();
        if ((result = Graph(*p, mask))) return result; const auto d = Desc(*p);
        s.legacySelf = scope == MMS_UI_REFRESH_SELF && p->id.find("legacy") != std::string::npos;
        if (s.legacySelf) { const MmsUiPropertyRefreshSubscriptionDescV1 old{Header<MmsUiPropertyRefreshSubscriptionDescV1>(), d.seed, d.property, d.kinds, d.flags, 0};
            result = p->legacy.subscribe(p->legacy.context, &old, &s.subscription); }
        else result = p->properties.subscribeScoped(p->properties.context, &d, &s.subscription);
        if (result) return result;
        s.events = s.overflow = s.readDepth = s.maxReadDepth = 0;
        for (unsigned i = 0; i < 3; ++i) s.reads[i] = s.successes[i] = s.releases[i] = s.prepares[i] = s.states[i] = 0;
        p->begun = true; return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL ScopedPropertySet(uint32_t index, int64_t value) {
    auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; if (index >= 3) return MMS_E_ARGUMENT;
    p->values[index].store(value); return MMS_OK;
}
extern "C" MmsResult MMS_CALL ScopedPropertyNotify(uint32_t count, uint32_t worker) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; if (!count || count > 128 || worker > 1) return MMS_E_ARGUMENT;
        MmsResult result = MMS_OK; const auto run = [&] { p->state.workerWasWorker = !Ui(*p); for (unsigned i = 0; i < count; ++i) {
            result = p->properties.notify(p->properties.context, &p->state.refs[Trigger(*p)]); if (result) break; } };
        if (worker) { std::thread thread(run); thread.join(); } else run(); return result;
    });
}
extern "C" MmsResult MMS_CALL ScopedPropertyBurst(int64_t first, uint32_t count) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD;
        if (!count || count > 64 || first > INT64_MAX - 1000 - int64_t(count)) return MMS_E_ARGUMENT; MmsResult result = MMS_OK;
        std::thread worker([&] { p->state.workerWasWorker = !Ui(*p); for (unsigned i = 0; i < count; ++i) {
            p->values[1].store(first + i); p->values[2].store(first + 1000 + i);
            result = p->properties.notify(p->properties.context, &p->state.refs[Trigger(*p)]); if (result) break; } }); worker.join(); return result;
    });
}
extern "C" MmsResult MMS_CALL ScopedPropertyGraph(uint32_t mask) {
    auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; return Graph(*p, mask);
}
extern "C" MmsResult MMS_CALL ScopedPropertyBindInput(uint32_t enabled) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; if (enabled > 1) return MMS_E_ARGUMENT;
        MmsResult result = MMS_OK; if (enabled) { if (p->bindings[0]) return MMS_E_CONFLICT;
            result = p->data.Bind(p->state.targetRefs[0], p->source, sizeof(int64_t), p->bindings[0], Prepare, &p->contexts[0]); }
        else result = p->bindings[0].Release(); if (!result) p->state.bindings[0] = p->bindings[0].get(); return result;
    });
}
extern "C" MmsResult MMS_CALL ScopedPropertyFailReads(uint32_t mask) {
    auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; if (mask & ~7u) return MMS_E_ARGUMENT; p->state.failMask = mask; return MMS_OK;
}
extern "C" MmsResult MMS_CALL ScopedPropertyCheckThreads(void) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; auto& s = p->state; const auto d = Desc(*p);
        MmsUiPropertyRefreshScopedCapabilitiesV1 caps{}; caps.h = Header<MmsUiPropertyRefreshScopedCapabilitiesV1>(); caps.supportedScopes = 991; const auto beforeCaps = caps;
        auto ref = s.refs[Trigger(*p)]; const auto beforeRef = ref; MmsHandle output = sentinel;
        std::thread worker([&] { MmsUiPropertyRefreshScopedSubscriptionStateV1 q{}; q.h = Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>();
            s.workerQuery = p->properties.queryScoped(p->properties.context, s.subscription, &q);
            s.workerCaps = p->properties.getScopedCapabilities(p->properties.context, &caps);
            s.workerRef = p->properties.getPropertyRef(p->properties.context, s.properties[Trigger(*p)], &ref);
            s.workerSubscribe = p->properties.subscribeScoped(p->properties.context, &d, &output);
            s.workerUnsubscribe = p->properties.unsubscribe(p->properties.context, s.subscription); }); worker.join();
        s.outputsPreserved &= output == sentinel && !std::memcmp(&caps, &beforeCaps, sizeof(caps)) && !std::memcmp(&ref, &beforeRef, sizeof(ref)); return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL ScopedPropertyReplaceTrigger(void) {
    auto* p = current; if (!p || !p->begun) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD;
    const unsigned index = Trigger(*p); const auto id = p->id + ".property." + std::to_string(index); auto d = Property(*p, index); d.info.id = Text(id);
    return p->ui.updateProperty(p->ui.context, p->state.properties[index], &d);
}
extern "C" MmsResult MMS_CALL ScopedPropertyRemoveSeed(void) {
    auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; return p->ui.unregisterItem(p->ui.context, p->state.seedHandle);
}
extern "C" MmsResult MMS_CALL ScopedPropertyQuery(ScopedPropertyProbeState* out) {
    return detail::Call([&]() -> MmsResult {
        auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; if (!out || out->size != sizeof(*out)) return MMS_E_ARGUMENT;
        auto s = p->state; for (unsigned i = 0; i < 3; ++i) s.values[i] = p->values[i].load();
        s.subscriptionState = {}; s.subscriptionState.h = Header<MmsUiPropertyRefreshScopedSubscriptionStateV1>(); s.subscriptionState.workGeneration = sentinel; const auto before = s.subscriptionState;
        s.query = p->properties.queryScoped(p->properties.context, s.subscription, &s.subscriptionState); s.queryOutputPreserved = !std::memcmp(&before, &s.subscriptionState, sizeof(before));
        s.oldState = {}; s.oldState.h = Header<MmsUiPropertyRefreshSubscriptionStateV1>(); s.oldQuery = p->legacy.query(p->legacy.context, s.subscription, &s.oldState);
        for (unsigned i = 0; i < 3; ++i) s.display[i] = Display(*p, i, s.displayRevisions[i]); *out = s; return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL ScopedPropertyAcquireHold(void) {
    auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD; if (p->state.heldSnapshot) return MMS_E_BUSY;
    MmsUiSnapshotV1 value{}; value.h = Header<MmsUiSnapshotV1>(); const auto result = p->ui.acquireSnapshot(p->ui.context, 0, &value);
    if (!result) p->state.heldSnapshot = value.handle; return result;
}
extern "C" MmsResult MMS_CALL ScopedPropertyReleaseHold(void) {
    auto* p = current; if (!p) return MMS_E_NOT_READY; if (!Ui(*p)) return MMS_E_THREAD;
    if (p->state.heldSnapshot) { p->ui.releaseSnapshot(p->ui.context, p->state.heldSnapshot); p->state.heldSnapshot = 0; } return MMS_OK;
}
