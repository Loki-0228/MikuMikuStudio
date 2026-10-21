// Original test DLL. Only negotiated public MMS services are used here.
#include <mms/ui.h>
#include <mms/layout.h>
#include <mms/refresh.h>
#include <Windows.h>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
template<class T> constexpr MmsHeader H() { return {sizeof(T), 1, 0}; }
MmsString S(const char* s) { return {s, std::strlen(s)}; }
MmsString S(const std::string& s) { return {s.data(), s.size()}; }
template<class F> MmsResult Guard(F&& f) noexcept {
    try { return f(); } catch (const std::bad_alloc&) { return MMS_E_MEMORY; } catch (...) { return MMS_E_FAILED; }
}
// Shared test protocol, duplicated in side_mount_host_checks.inc. No HWNDs.
struct ProbeState {
    uint32_t size, submitted, completed, mask;
    MmsResult result;
    uint32_t reads, invokes, taskRuns, taskCancels, removed, reserved[2];
};
static_assert(sizeof(ProbeState) == 48, "side mount test protocol");
constexpr const char* anchors[] = {"mms.sidebar.left.extensions", "mms.sidebar.right.extensions", "mms.properties.main.extensions"};
constexpr const char* names[] = {"left", "right", "property"};
constexpr const char* labels[] = {"Side fixture left button", "Side fixture right button", "Side fixture property button"};
constexpr const char* inputLabels[] = {"Side fixture left value", "Side fixture right value", "Side fixture property value"};
struct Plugin {
    MmsHostV1 host{}; MmsUiApiV1 ui{}; MmsUiLayoutApiV1 layout{}; MmsUiRefreshApiV1 refresh{};
    std::string id;
    std::array<std::array<std::string,3>,3> ids;
    std::array<std::array<MmsHandle,3>,3> elements{};
    std::vector<MmsHandle> registrations;
    MmsHandle changeCommand = 0;
    DWORD thread = GetCurrentThreadId();
    uint32_t requestedOperation = 0, requestedMask = 0, pendingTasks = 0;
    ProbeState state{sizeof(ProbeState),0,0,7,MMS_OK,0,0,0,0,0,{0,0}};
};
Plugin* current = nullptr;
bool Retired(MmsResult r) { return r == MMS_E_STALE || r == MMS_E_NOT_FOUND || r == MMS_E_NOT_READY; }
MmsResult MMS_CALL State(void*, MmsObjectRef, MmsCommandStateV1* out) {
    if (!out) return MMS_E_ARGUMENT;
    *out = {H<MmsCommandStateV1>(),1,0,1,0,{}}; return MMS_OK;
}
MmsResult MMS_CALL Action(void* p, MmsObjectRef, MmsSpan, MmsHandle) { ++static_cast<Plugin*>(p)->state.invokes; return MMS_OK; }
MmsResult MMS_CALL Read(void* p, MmsObjectRef, MmsValue* out, MmsHandle* lease) {
    if (!out || !lease) return MMS_E_ARGUMENT;
    ++static_cast<Plugin*>(p)->state.reads;
    MmsValue value{}; value.type = MMS_VALUE_NUMBER; value.value.number = 2;
    *out = value; *lease = 0; return MMS_OK;
}
void MMS_CALL ReleaseValue(void*, MmsHandle) {}
MmsResult Remove(Plugin& p) {
    for (auto it = p.registrations.rbegin(); it != p.registrations.rend(); ++it) {
        if (!*it) continue;
        const auto r = p.ui.unregisterItem(p.ui.context,*it);
        if (r && !Retired(r)) return r;
        *it = 0;
    }
    for (auto& root : p.elements) root.fill(0);
    p.state.mask = 0; p.state.removed = 1; return MMS_OK;
}
MmsResult SetVisible(Plugin& p, uint32_t mask) {
    MmsUiSnapshotV1 snapshot{}; snapshot.h = H<MmsUiSnapshotV1>();
    auto r = p.ui.acquireSnapshot(p.ui.context,0,&snapshot); if (r) return r;
    const auto revision = snapshot.revision; std::array<uint64_t,3> generations{};
    for(uint64_t index=0;index<snapshot.elements.count;++index) {
        const auto* element=reinterpret_cast<const MmsUiElementV1*>(static_cast<const unsigned char*>(snapshot.elements.data)+size_t(index*snapshot.elements.stride));
        for(unsigned root=0;root!=3;++root) {
            const auto& id=p.ids[root][0];
            if(element->id.bytes==id.size()&&!std::memcmp(element->id.data,id.data(),id.size()))generations[root]=element->generation;
        }
    }
    p.ui.releaseSnapshot(p.ui.context,snapshot.handle);
    for(const auto generation:generations)if(!generation)return MMS_E_STALE;
    MmsHandle transaction = 0; r = p.ui.beginLayout(p.ui.context,revision,&transaction); if (r) return r;
    for (unsigned root = 0; root != 3; ++root) {
        MmsLayoutOpV1 operation{}; operation.h = H<MmsLayoutOpV1>();
        operation.operation = MMS_LAYOUT_VISIBLE; operation.elementId = S(p.ids[root][0]);
        operation.expectedGeneration = generations[root]; operation.value.visible = (mask >> root) & 1;
        r = p.ui.applyLayout(p.ui.context,transaction,&operation);
        if (r) { p.ui.cancelLayout(p.ui.context,transaction); return r; }
    }
    r = p.ui.commitLayout(p.ui.context,transaction);
    if (r) p.ui.cancelLayout(p.ui.context,transaction);
    else p.state.mask = mask;
    return r;
}
struct Task { Plugin* owner; uint32_t operation, mask, serial; };
void Finish(void* context, bool cancelled) noexcept {
    std::unique_ptr<Task> task(static_cast<Task*>(context)); auto& p = *task->owner;
    --p.pendingTasks;
    if (cancelled) { ++p.state.taskCancels; p.state.result = MMS_E_CANCELLED; }
    else {
        ++p.state.taskRuns;
        p.state.result = Guard([&] { return task->operation == 1 ? SetVisible(p,task->mask) : Remove(p); });
    }
    p.state.completed = task->serial;
}
void MMS_CALL Run(void* context) { Finish(context,false); }
void MMS_CALL Cancel(void* context) { Finish(context,true); }
MmsResult MMS_CALL Change(void* context, MmsObjectRef, MmsSpan, MmsHandle) {
    return Guard([&]() -> MmsResult {
        auto& p = *static_cast<Plugin*>(context);
        if (p.pendingTasks || p.state.removed) return MMS_E_BUSY;
        const auto serial = p.state.submitted + 1;
        auto task = std::make_unique<Task>(Task{&p,p.requestedOperation,p.requestedMask,serial});
        MmsHandle ticket = 0;
        const auto r = p.host.enqueueUi(p.host.context,Run,Cancel,task.get(),&ticket);
        if (r) return r;
        task.release(); ++p.pendingTasks; p.state.submitted = serial; return MMS_OK;
    });
}
MmsResult RegisterCommand(Plugin& p, const std::string& id, const char* label,
        MmsResult (MMS_CALL *invoke)(void*,MmsObjectRef,MmsSpan,MmsHandle), MmsHandle* handle = nullptr) {
    MmsCommandDescV1 desc{}; desc.info.h = H<MmsCommandDescV1>();
    desc.info.id = S(id); desc.info.ownerPlugin = S(p.id); desc.info.label = S(label);
    desc.info.flags = MMS_COMMAND_INVOKE_ONLY; desc.context = &p; desc.getState = State; desc.invoke = invoke;
    MmsHandle result = 0; const auto r = p.ui.registerCommand(p.ui.context,&desc,&result);
    if (!r) { p.registrations.push_back(result); if (handle) *handle = result; }
    return r;
}
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    if (!host || !info || !out || !host->queryService || !host->enqueueUi || current) return MMS_E_ARGUMENT;
    return Guard([&]() -> MmsResult {
        auto p = std::make_unique<Plugin>(); p->host = *host;
        p->id.assign(info->pluginId.data,size_t(info->pluginId.bytes));
        p->registrations.reserve(16);
        for (unsigned i=0;i!=3;++i) for (unsigned j=0;j!=3;++j)
            p->ids[i][j] = p->id + "." + names[i] + (j==0?".group":j==1?".button":".input");
        auto r = host->queryService(host->context,S("mms.ui"),1,0,sizeof(p->ui),&p->ui); if (r) return r;
        r = host->queryService(host->context,S("mms.ui.layout"),1,0,sizeof(p->layout),&p->layout); if (r) return r;
        r = host->queryService(host->context,S("mms.ui.refresh"),1,0,sizeof(p->refresh),&p->refresh); if (r) return r;
        *out = p.get(); current = p.release(); return MMS_OK;
    });
}
MmsResult MMS_CALL Start(void* context) {
    return Guard([&]() -> MmsResult {
        auto& p = *static_cast<Plugin*>(context);
        auto r = RegisterCommand(p,p.id+".change","Side fixture change",Change,&p.changeCommand); if (r) return r;
        const auto propertyId = p.id + ".value";
        MmsPropertyDescV1 property{}; property.info.h = H<MmsPropertyDescV1>();
        property.info.id = S(propertyId); property.info.ownerPlugin = S(p.id); property.info.label = S("Side fixture value");
        property.info.valueType = MMS_VALUE_NUMBER; property.info.flags = MMS_PROPERTY_READ_ONLY;
        property.context = &p; property.read = Read; property.releaseValue = ReleaseValue;
        MmsHandle handle = 0; r = p.ui.registerProperty(p.ui.context,&property,&handle); if (r) return r;
        p.registrations.push_back(handle);
        for (unsigned root=0;root!=3;++root) {
            const auto command = p.id + "." + names[root] + ".action";
            r = RegisterCommand(p,command,labels[root],Action); if (r) return r;
            for (unsigned item=0;item!=3;++item) {
                MmsUiElementV1 element{}; element.h = H<MmsUiElementV1>();
                element.id = S(p.ids[root][item]); element.ownerPlugin = S(p.id);
                element.parentId = item ? S(p.ids[root][0]) : S(anchors[root]);
                element.label = S(item==1?labels[root]:item==2?inputLabels[root]:names[root]);
                element.kind = item==0?MMS_UI_GROUP:item==1?MMS_UI_BUTTON:MMS_UI_INPUT;
                element.capabilities = MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE|(item==0?MMS_UI_CONTAINER:0);
                element.generation = 1;
                if (item==1) element.commandId=S(command);
                if (item==2) element.propertyId=S(propertyId);
                r = p.ui.registerElement(p.ui.context,&element,&p.elements[root][item]); if (r) return r;
                p.registrations.push_back(p.elements[root][item]);
                MmsUiLayoutHintsV1 hints{}; hints.h=H<MmsUiLayoutHintsV1>();
                hints.flow=item==0?MMS_FLOW_COLUMN:MMS_FLOW_AUTO;
                hints.minimum[0]=150; hints.preferred[0]=180; hints.maximum[0]=220;
                hints.minimum[1]=item==0?56:24; hints.preferred[1]=item==0?60:26; hints.maximum[1]=item==0?1000:30;
                r=p.layout.setDefaultHints(p.layout.context,S(p.ids[root][item]),&hints); if(r)return r;
            }
        }
        return MMS_OK;
    });
}
void MMS_CALL Event(void*,const MmsEventV1*) {}
MmsResult MMS_CALL Stop(void* context) { auto& p=*static_cast<Plugin*>(context); return p.pendingTasks?MMS_E_BUSY:Guard([&]{return Remove(p);}); }
void MMS_CALL Destroy(void* context) { auto* p=static_cast<Plugin*>(context); if(current==p)current=nullptr; delete p; }
}

extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major,uint32_t minor,uint32_t bytes,MmsPluginApiV1* out) {
    if(!out||bytes<sizeof(*out))return MMS_E_ARGUMENT;
    if(major!=1||minor)return MMS_E_VERSION;
    *out={H<MmsPluginApiV1>(),Create,Start,Event,Stop,Destroy};return MMS_OK;
}
extern "C" MmsResult MMS_CALL SideMountProbeQuery(ProbeState* out) {
    if(!current)return MMS_E_NOT_READY;
    if(GetCurrentThreadId()!=current->thread)return MMS_E_THREAD;
    if(!out||out->size!=sizeof(*out))return MMS_E_ARGUMENT;
    *out=current->state;return MMS_OK;
}
extern "C" MmsResult MMS_CALL SideMountProbeHandle(uint32_t root,uint32_t item,MmsHandle* out) {
    if(!current)return MMS_E_NOT_READY;
    if(GetCurrentThreadId()!=current->thread)return MMS_E_THREAD;
    if(root>=3||item>=3||!out)return MMS_E_ARGUMENT;
    const auto handle=current->elements[root][item];if(!handle)return MMS_E_STALE;
    *out=handle;return MMS_OK;
}
extern "C" MmsResult MMS_CALL SideMountProbeChange(uint32_t operation,uint32_t mask) {
    if(!current)return MMS_E_NOT_READY;
    if(GetCurrentThreadId()!=current->thread)return MMS_E_THREAD;
    if((operation!=1&&operation!=2)||(mask&~7u))return MMS_E_ARGUMENT;
    current->requestedOperation=operation;current->requestedMask=mask;
    return Guard([&]{return current->ui.invoke(current->ui.context,S(current->id+".change"),{},{});});
}
extern "C" MmsResult MMS_CALL SideMountProbeTarget(uint32_t root,uint32_t item,MmsUiRefreshTargetV1* out,float* rect) {
    if(!current)return MMS_E_NOT_READY;
    if(GetCurrentThreadId()!=current->thread)return MMS_E_THREAD;
    if(root>=3||item>=3||!out||out->h.size<sizeof(*out)||!rect)return MMS_E_ARGUMENT;
    return Guard([&]() -> MmsResult {
        auto& p=*current;if(!p.elements[root][item])return MMS_E_STALE;
        MmsUiRefreshTargetV1 target{};target.h=H<MmsUiRefreshTargetV1>();
        auto r=p.refresh.getTarget(p.refresh.context,p.elements[root][item],&target);if(r)return r;
        MmsUiLayoutSnapshotV1 snapshot{};snapshot.h=H<MmsUiLayoutSnapshotV1>();
        r=p.layout.acquireSnapshot(p.layout.context,&snapshot);if(r)return r;
        struct Lease { MmsUiLayoutApiV1& api;MmsHandle handle;~Lease(){api.releaseSnapshot(api.context,handle);} } lease{p.layout,snapshot.handle};
        const auto& id=p.ids[root][item];
        for(uint64_t i=0;i<snapshot.items.count;++i){
            const auto* value=reinterpret_cast<const MmsUiLayoutItemV1*>(static_cast<const unsigned char*>(snapshot.items.data)+size_t(i*snapshot.items.stride));
            if(value->elementId.bytes!=id.size()||std::memcmp(value->elementId.data,id.data(),id.size()))continue;
            *out=target;std::memcpy(rect,value->rect,sizeof(value->rect));return MMS_OK;
        }
        return MMS_E_NOT_READY;
    });
}
