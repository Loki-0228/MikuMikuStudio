#include <mms/ui.h>
#include <mms/edit.h>
#include <mms/storage.h>
#include <mms/scene.h>
#include <cstring>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace {
template<class T> MmsHeader H() { return {sizeof(T), 1, 0}; }
MmsString S(const char* s) { return {s, std::strlen(s)}; }
MmsString S(const std::string& s) { return {s.data(), s.size()}; }
struct Plugin;
struct Pending { Plugin* owner; int before, after; MmsHandle transaction; bool fail = false, cancelThrows = false, property = false; };
struct Plugin {
    MmsHostV1 host{}; MmsUiApiV1 ui{}; MmsEditApiV1 edit{}; MmsSceneApiV1 scene{};
    std::string id; int value = 0, staged = 0, cancels = 0;
    uint64_t revision = 1, flags = 0; MmsHandle pending = 0; bool explicitRevision = false;
    MmsSerializerV1 serializer{}; MmsHandle provider=0;
    uint64_t providerProbeMask=0,providerProbeFlags=0;
    std::string editableId;
    MmsPropertyDescV1 editable{}; MmsHandle editableHandle=0,propertyOther=0,activeProperty=0;
    std::map<MmsHandle,Pending*> properties;
    uint64_t propertyFlags=0,propertyCalls=0;uint32_t propertyMode=0;bool insideProperty=false;
    MmsResult propertyFailure=MMS_OK;
    uint64_t propertyEvents=0, closingEvents=0, eventServiceReady=0;
};
Plugin* current = nullptr;
uint64_t stoppedFlags = 0; int stoppedCancels = 0;
uint64_t stoppedPropertyFlags=0,stoppedPropertyCalls=0,stoppedPropertyCandidates=0;
uint64_t stoppedPropertyEvents=0,stoppedClosingEvents=0,stoppedEventServiceReady=0;
bool Denied(MmsResult r) { return r == MMS_E_BUSY || r == MMS_E_NOT_READY; }
void ProbeDenied(Plugin& p, uint64_t flag) {
    if (!p.edit.begin) return;
    MmsHandle untouched = 0x123456;
    const auto r = p.edit.begin(p.edit.context, S("forbidden callback"), p.revision, &untouched);
    if (Denied(r) && untouched == 0x123456) p.flags |= flag;
}
void ProbeProvider(Plugin& p,uint64_t phase) {
    if(!(p.providerProbeMask&phase))return;
    const auto removed=p.host.unregisterProvider(p.host.context,p.provider);
    if(removed==MMS_E_BUSY)p.providerProbeFlags|=phase;
    MmsHandle replacement=0x789123;
    const auto added=p.host.registerProvider(p.host.context,S("mms.serializer"),S(p.id),&p.serializer,sizeof p.serializer,&replacement);
    if(added==MMS_E_BUSY&&replacement==0x789123)p.providerProbeFlags|=phase<<8;
    if(p.ui.updateProperty(p.ui.context,p.editableHandle,&p.editable)==MMS_E_BUSY)p.providerProbeFlags|=phase<<16;
    // A failing implementation may have accepted replacement. Retain its
    // actual handle so the diagnostic DLL remains safely unloadable.
    if(!added)p.provider=replacement;
}
MmsResult MMS_CALL Prepare(void* context) {
    auto& t = *static_cast<Pending*>(context); ProbeDenied(*t.owner, 32);
    ProbeProvider(*t.owner,1);
    return t.fail ? MMS_E_FAILED : MMS_OK;
}
void MMS_CALL Commit(void* context) {
    std::unique_ptr<Pending> t(static_cast<Pending*>(context)); ProbeDenied(*t->owner, 64); t->owner->value = t->after;
    ProbeProvider(*t->owner,2);
    if(t->property)t->owner->properties.erase(t->transaction);
    if (t->owner->pending == t->transaction) t->owner->pending = 0;
}
void MMS_CALL Cancel(void* context) {
    std::unique_ptr<Pending> t(static_cast<Pending*>(context)); ++t->owner->cancels; ProbeDenied(*t->owner, 8192);
    ProbeProvider(*t->owner,4);
    if(t->property)t->owner->properties.erase(t->transaction);
    if (t->owner->pending == t->transaction) t->owner->pending = 0;
    if (t->cancelThrows) throw std::runtime_error("cancel contract violation");
}
MmsResult MMS_CALL UndoState(void* context, MmsBytes* before, MmsBytes* after, uint32_t* schema, MmsHandle* lease) {
    auto& t = *static_cast<Pending*>(context); ProbeDenied(*t.owner, 16384);
    ProbeProvider(*t.owner,8);
    *before = {&t.before, sizeof(int)}; *after = {&t.after, sizeof(int)}; *schema = 1; *lease = 1; return MMS_OK;
}
void MMS_CALL ReleaseUndo(void* context, MmsHandle) {
    auto& p=*static_cast<Pending*>(context)->owner;ProbeDenied(p,32768);ProbeProvider(p,16);
}
MmsTransactionParticipantV1 Participant(Plugin& p,Pending* pending) {
    MmsTransactionParticipantV1 t{H<MmsTransactionParticipantV1>(), pending, S(p.id)};
    t.prepare = Prepare; t.commit = Commit; t.cancel = Cancel; t.getUndoState = UndoState; t.releaseUndoState = ReleaseUndo;
    return t;
}
MmsResult Join(Plugin& p, MmsHandle tx, bool fail = false, bool cancelThrows = false) {
    auto pending = std::make_unique<Pending>(Pending{&p, p.value, p.value + 1, tx, fail, cancelThrows});
    auto t=Participant(p,pending.get());
    const auto r = p.edit.join(p.edit.context, tx, &t); if (!r) pending.release(); return r;
}
MmsResult Begin(Plugin& p, bool fail = false, bool cancelThrows = false) {
    if (!p.explicitRevision && p.scene.acquire) {
        MmsSceneQueryV1 query{}; query.h = H<MmsSceneQueryV1>(); query.flags = MMS_SCENE_CURRENT_TIME;
        MmsSnapshotV1 snapshot{}; const auto r = p.scene.acquire(p.scene.context, &query, &snapshot);
        if (r) return r;
        p.revision = snapshot.sceneRevision; p.scene.release(p.scene.context, snapshot.handle);
    }
    auto r = p.edit.begin(p.edit.context, S("Edit probe"), p.revision, &p.pending);
    if (!r) r = Join(p, p.pending, fail, cancelThrows); return r;
}
MmsResult Finish(Plugin& p) {
    auto r = p.edit.prepare(p.edit.context, p.pending);
    if (!r) r = p.edit.commit(p.edit.context, p.pending);
    if (!r) ++p.revision;
    p.pending = 0; return r;
}
MmsResult Once(Plugin& p) { auto r = Begin(p); return r ? r : Finish(p); }
bool PropertyJoinDenied(Plugin& p,MmsHandle tx){
    auto candidate=std::make_unique<Pending>(Pending{&p,p.value,p.value,tx});auto table=Participant(p,candidate.get());
    const auto result=p.edit.join(p.edit.context,tx,&table);if(!result)candidate.release();return result==MMS_E_BUSY;
}
MmsResult BareBegin(Plugin& p,MmsHandle& transaction){
    if(!p.explicitRevision&&p.scene.acquire){MmsSceneQueryV1 query{};query.h=H<MmsSceneQueryV1>();query.flags=MMS_SCENE_CURRENT_TIME;
        MmsSnapshotV1 snapshot{};const auto result=p.scene.acquire(p.scene.context,&query,&snapshot);if(result)return result;
        p.revision=snapshot.sceneRevision;p.scene.release(p.scene.context,snapshot.handle);}
    return p.edit.begin(p.edit.context,S("Property probe"),p.revision,&transaction);
}
MmsResult PropertyValue(Plugin& p,MmsHandle tx,int value){MmsValue v{};v.type=MMS_VALUE_INT;v.value.signedValue=value;
    return p.ui.stageProperty(p.ui.context,tx,S(p.editableId),{},&v);}
MmsResult PropertyOnce(Plugin& p){
    auto result=BareBegin(p,p.pending);if(!result)result=PropertyValue(p,p.pending,7);if(!result)result=PropertyValue(p,p.pending,9);
    if(!result)return Finish(p);p.pending=0;return result;
}
MmsResult MMS_CALL PropertyStage(void* context,MmsHandle tx,MmsObjectRef,const MmsValue* value){
    auto& p=*static_cast<Plugin*>(context);if(p.insideProperty)return MMS_E_FAILED;
    ++p.propertyCalls;p.insideProperty=true;p.activeProperty=tx;
    struct Clear{Plugin& owner;~Clear(){owner.insideProperty=false;owner.activeProperty=0;}}clear{p};
    MmsHandle untouched=0x654321;
    if(p.edit.begin(p.edit.context,S("stage forbidden"),p.revision,&untouched)==MMS_E_BUSY&&untouched==0x654321)p.propertyFlags|=1;
    if(p.edit.prepare(p.edit.context,tx)==MMS_E_BUSY)p.propertyFlags|=2;
    if(p.edit.commit(p.edit.context,tx)==MMS_E_BUSY)p.propertyFlags|=4;
    const auto cancels=p.cancels;p.edit.cancel(p.edit.context,tx);if(p.cancels==cancels)p.propertyFlags|=8;
    if(p.edit.undo(p.edit.context)==MMS_E_BUSY)p.propertyFlags|=16;
    if(p.edit.redo(p.edit.context)==MMS_E_BUSY)p.propertyFlags|=32;
    if(PropertyJoinDenied(p,tx+0x100000))p.propertyFlags|=64;
    MmsValue read{};MmsHandle lease=0;const auto readonly=p.id+".value";
    const auto readResult=p.ui.readProperty(p.ui.context,S(readonly),{},&read,&lease);if(lease)p.ui.releaseValue(p.ui.context,lease);
    if(!readResult)p.propertyFlags|=65536;
    MmsCommandStateV1 state{};state.h=H<MmsCommandStateV1>();lease=0;const auto command=p.id+".once";
    const auto stateResult=p.ui.getCommandState(p.ui.context,S(command),{},&state,&lease);if(lease)p.ui.releaseValue(p.ui.context,lease);
    if(!stateResult)p.propertyFlags|=131072;
    if(p.ui.stageProperty(p.ui.context,tx,S(p.editableId),{},value)==MMS_E_BUSY)p.propertyFlags|=512;
    untouched=0x654321;
    if(p.ui.registerProperty(p.ui.context,&p.editable,&untouched)==MMS_E_BUSY&&untouched==0x654321&&
        p.ui.updateProperty(p.ui.context,p.editableHandle,&p.editable)==MMS_E_BUSY)p.propertyFlags|=1024;
    if(p.host.unregisterProvider(p.host.context,p.provider)==MMS_E_BUSY)p.propertyFlags|=2048;
    untouched=0x654321;
    if(p.host.registerProvider(p.host.context,S("mms.serializer"),S(p.id),&p.serializer,sizeof p.serializer,&untouched)==MMS_E_BUSY&&untouched==0x654321)p.propertyFlags|=4096;
    if(p.propertyMode==1)return MMS_E_FAILED;
    if(p.propertyMode==4)return MMS_OK;
    auto found=p.properties.find(tx);
    if(found==p.properties.end()){
        auto candidate=std::make_unique<Pending>(Pending{&p,p.value,int(value->value.signedValue),tx,false,p.propertyMode==5,true});
        auto table=Participant(p,candidate.get());auto inserted=p.properties.emplace(tx,candidate.get());
        const auto result=p.edit.join(p.edit.context,tx,&table);
        if(result){p.properties.erase(inserted.first);return result;}
        candidate.release();p.propertyFlags|=8192;
    }else found->second->after=int(value->value.signedValue);
    if(p.propertyMode==2)return MMS_E_ARGUMENT;
    if(p.propertyMode==3)throw std::runtime_error("property stage failure after join");
    return MMS_OK;
}
struct Task { Plugin* owner; bool fail, explicitRevision; uint64_t revision;bool property=false; };
void MMS_CALL Run(void* context) {
    std::unique_ptr<Task> task(static_cast<Task*>(context));
    task->owner->explicitRevision = task->explicitRevision; task->owner->revision = task->revision;
    if(task->property){if(!PropertyOnce(*task->owner))task->owner->propertyFlags|=16384;return;}
    if (task->fail) { if (Begin(*task->owner)) throw std::runtime_error("begin queued edit"); throw std::runtime_error("queued edit failure"); }
    if (!Once(*task->owner)) task->owner->flags |= 512;
}
void MMS_CALL CancelTask(void* context) {
    std::unique_ptr<Task> task(static_cast<Task*>(context)); ProbeDenied(*task->owner, 1024);
}
MmsResult Queue(Plugin& p, bool fail) {
    auto task = std::make_unique<Task>(Task{&p, fail, p.explicitRevision, p.revision}); MmsHandle handle = 0;
    auto r = p.host.enqueueUi(p.host.context, Run, CancelTask, task.get(), &handle);
    if (!r) task.release(); return r;
}
MmsResult MMS_CALL State(void* context, MmsObjectRef, MmsCommandStateV1* out) {
    auto& p=*static_cast<Plugin*>(context);ProbeDenied(p, 8);
    if(p.insideProperty&&PropertyJoinDenied(p,p.activeProperty))p.propertyFlags|=256;
    *out = {H<MmsCommandStateV1>(), 1, 0, 1, 0, {}}; return MMS_OK;
}
MmsResult MMS_CALL Read(void* context, MmsObjectRef, MmsValue* out, MmsHandle* lease) {
    auto& p = *static_cast<Plugin*>(context); ProbeDenied(p, 16);
    if(p.insideProperty&&PropertyJoinDenied(p,p.activeProperty))p.propertyFlags|=128;
    *out = {}; out->type = MMS_VALUE_INT; out->value.signedValue = p.value; *lease = 0; return MMS_OK;
}
void MMS_CALL ReleaseRead(void*, MmsHandle) {}
struct Command { Plugin* owner; const char* name; };
// Descriptors retain these contexts until the plugin has retired all UI items.
struct OwnedPlugin : Plugin { std::unique_ptr<Command[]> commands; };
MmsResult MMS_CALL Invoke(void* context, MmsObjectRef, MmsSpan args, MmsHandle) {
    const auto& command = *static_cast<Command*>(context); auto& p = *command.owner;
    uint64_t foreign = 0; p.explicitRevision = false;
    for (uint64_t n = 0; n < args.count; ++n) {
        const auto& a = *reinterpret_cast<const MmsNamedValue*>(static_cast<const unsigned char*>(args.data) + n * args.stride);
        const std::string name(a.name.data, size_t(a.name.bytes));
        if (name == "revision" && a.value.value.signedValue > 0) { p.revision = uint64_t(a.value.value.signedValue); p.explicitRevision = true; }
        if (name == "transaction") foreign = uint64_t(a.value.value.signedValue);
    }
    if (!p.edit.begin) return MMS_E_NOT_READY;
    const std::string name = command.name;
    if(name=="replace-provider"){
        const auto removed=p.host.unregisterProvider(p.host.context,p.provider);if(removed)return removed;
        return p.host.registerProvider(p.host.context,S("mms.serializer"),S(p.id),&p.serializer,sizeof p.serializer,&p.provider);
    }
    if(name=="commit-pending"){
        const auto result=p.edit.commit(p.edit.context,p.pending);p.pending=0;if(!result)++p.revision;return result;
    }
    if(name=="property-once")return PropertyOnce(p);
    if(name=="property-empty")return BareBegin(p,p.pending);
    if(name=="property-pending"){auto result=BareBegin(p,p.pending);return result?result:PropertyValue(p,p.pending,7);}
    if(name=="property-stage")return PropertyValue(p,foreign?foreign:p.pending,9);
    if(name=="property-try"){p.propertyFailure=PropertyValue(p,foreign?foreign:p.pending,9);return MMS_OK;}
    if(name=="prepare-pending")return p.edit.prepare(p.edit.context,foreign?foreign:p.pending);
    if(name=="property-cancel"){const auto tx=foreign?foreign:p.pending;p.edit.cancel(p.edit.context,tx);if(tx==p.pending)p.pending=0;return MMS_OK;}
    if(name=="property-queue"){
        auto task=std::make_unique<Task>(Task{&p,false,p.explicitRevision,p.revision,true});MmsHandle handle=0;
        MmsResult result=MMS_E_FAILED;
        std::thread background([&]{result=p.host.enqueueUi(p.host.context,Run,CancelTask,task.get(),&handle);});background.join();
        if(!result)task.release();return result;
    }
    if(name=="property-fail-keep"){
        const auto mode=p.propertyMode;p.propertyMode=0;
        auto result=BareBegin(p,p.pending);if(!result)result=PropertyValue(p,p.pending,7);
        if(!result)result=BareBegin(p,p.propertyOther);if(!result)result=PropertyValue(p,p.propertyOther,7);
        const auto cancels=p.cancels;p.propertyMode=2;
        if(!result)result=PropertyValue(p,p.propertyOther,9);
        p.propertyMode=mode;p.propertyFailure=result;
        if(result==MMS_E_ARGUMENT&&p.cancels==cancels+1&&p.properties.count(p.pending)&&!p.properties.count(p.propertyOther))p.propertyFlags|=32768;
        return MMS_OK; // Keep the unrelated first transaction after the accepted stage fails.
    }
    if (name == "once") return Once(p);
    if (name == "pump-once") {
        const auto result = Once(p); if (result) return result;
        // Snapshot a bounded batch before dispatch: messages posted by these
        // handlers wait for the outer host loop instead of spinning here.
        MSG messages[64]{}; size_t count = 0;
        while (count < 64 && PeekMessageW(&messages[count], nullptr, 0, 0, PM_REMOVE)) {
            if (messages[count].message == WM_QUIT) { PostQuitMessage(int(messages[count].wParam)); break; }
            ++count;
        }
        for (size_t index = 0; index < count; ++index) { TranslateMessage(&messages[index]); DispatchMessageW(&messages[index]); }
        return MMS_OK;
    }
    if (name == "twice") { auto r = Once(p); return r ? r : Once(p); }
    if (name == "pending") return Begin(p);
    if (name == "cancel-throw-pending") return Begin(p, false, true);
    if (name == "prepared") { auto r = Begin(p); return r ? r : p.edit.prepare(p.edit.context, p.pending); }
    if (name == "prepare-fail") { auto r = Begin(p, true); return r ? r : Finish(p); }
    if (name == "throw") { if (Begin(p)) return MMS_E_FAILED; throw std::runtime_error("invoke edit failure"); }
    if (name == "join-fail") { const auto r = Join(p, foreign); return r ? r : MMS_E_FAILED; }
    if (name == "finish") return Finish(p);
    if (name == "cancel") { p.edit.cancel(p.edit.context, p.pending); p.pending = 0; return MMS_OK; }
    if (name == "queue") return Queue(p, false);
    if (name == "queue-throw") return Queue(p, true);
    if (name == "undo" || name == "redo") {
        const auto r = name == "undo" ? p.edit.undo(p.edit.context) : p.edit.redo(p.edit.context);
        if (!r) ++p.revision; return r;
    }
    if (name == "nested") {
        const auto id = p.id + ".value"; MmsValue value{}; MmsHandle lease = 0;
        const auto r = p.ui.readProperty(p.ui.context, S(id), {}, &value, &lease);
        if (lease) p.ui.releaseValue(p.ui.context, lease);
        const auto nested = p.id + ".once";
        if (Denied(p.ui.invoke(p.ui.context, S(nested), {}, {}))) p.flags |= 2048;
        return r;
    }
    return MMS_E_ARGUMENT;
}
MmsResult MMS_CALL CommandState(void* context, MmsObjectRef target, MmsCommandStateV1* out) {
    return State(static_cast<Command*>(context)->owner, target, out);
}
MmsResult MMS_CALL Create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) {
    auto p = std::make_unique<OwnedPlugin>(); p->host = *host; p->id.assign(info->pluginId.data, size_t(info->pluginId.bytes));
    auto r = host->queryService(host->context, S("mms.ui"), 1, 0, sizeof(p->ui), &p->ui); if (r) return r;
    r = host->queryService(host->context, S("mms.edit"), 1, 0, sizeof(p->edit), &p->edit);
    if (!r) p->flags |= 1; else if (r == MMS_E_NOT_FOUND) p->flags |= 2; else return r;
    r = host->queryService(host->context, S("mms.scene"), 1, 0, sizeof(p->scene), &p->scene);
    if (r && r != MMS_E_NOT_FOUND) return r;
    p->editableId=p->id+".editable";
    ProbeDenied(*p, 4); current = p.get(); stoppedFlags = 0; stoppedCancels = 0;
    stoppedPropertyFlags=stoppedPropertyCalls=stoppedPropertyCandidates=0;*out = p.release(); return MMS_OK;
}
MmsResult MMS_CALL Start(void* context) {
    auto& p = *static_cast<OwnedPlugin*>(context); ProbeDenied(p, 4);
    MmsSerializerV1 serializer{H<MmsSerializerV1>(), &p};
    serializer.prepareLoad = [](void* c, MmsBytes bytes, uint32_t schema, MmsHandle* out) -> MmsResult {
        auto& p = *static_cast<Plugin*>(c); ProbeDenied(p, 128);
        if (schema && (schema != 1 || bytes.bytes != sizeof(int))) return MMS_E_VERSION;
        p.staged = 0; if (schema) std::memcpy(&p.staged, bytes.data, sizeof(int)); *out = 1; return MMS_OK;
    };
    serializer.commitLoad = [](void* c, MmsHandle) { auto& p = *static_cast<Plugin*>(c); ProbeDenied(p, 128); p.value = p.staged; };
    serializer.cancelLoad = [](void* c, MmsHandle) { ProbeDenied(*static_cast<Plugin*>(c), 128); };
    serializer.save = [](void* c, MmsBytes* bytes, uint32_t* schema, MmsHandle* lease) -> MmsResult {
        auto& p = *static_cast<Plugin*>(c); ProbeDenied(p, 128); *bytes = {&p.value, sizeof(int)}; *schema = 1; *lease = 1; return MMS_OK;
    };
    serializer.releaseBytes = [](void* c, MmsHandle) { ProbeDenied(*static_cast<Plugin*>(c), 128); };
    p.serializer=serializer;
    MmsHandle handle = 0;
    auto r = p.host.registerProvider(p.host.context, S("mms.serializer"), S(p.id), &serializer, sizeof(serializer), &handle); if (r) return r;
    p.provider=handle;
    static const char* names[] = {"once","pump-once","twice","prepare-fail","pending","prepared","throw","join-fail","finish","cancel","queue","queue-throw","undo","redo","nested","cancel-throw-pending","replace-provider","commit-pending",
        "property-once","property-empty","property-pending","property-stage","property-try","prepare-pending","property-cancel","property-queue","property-fail-keep"};
    p.commands = std::make_unique<Command[]>(sizeof(names) / sizeof(names[0]));
    MmsArgumentV1 arguments[2]{};
    for (auto& a : arguments) { a.h = H<MmsArgumentV1>(); a.valueType = MMS_VALUE_INT; a.minimum.type = a.maximum.type = a.defaultValue.type = MMS_VALUE_INT; a.maximum.value.signedValue = INT64_MAX; }
    arguments[0].name = S("revision"); arguments[1].name = S("transaction");
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
        p.commands[n] = {&p, names[n]}; const auto id = p.id + "." + names[n];
        const auto label = std::string("Edit probe ") + names[n];
        MmsCommandDescV1 c{}; c.info.h = H<MmsCommandDescV1>(); c.info.id = S(id); c.info.ownerPlugin = S(p.id); c.info.label = S(label);
        c.info.arguments = {arguments, 2, sizeof(MmsArgumentV1), 1}; c.context = &p.commands[n]; c.getState = CommandState; c.invoke = Invoke;
        if ((r = p.ui.registerCommand(p.ui.context, &c, &handle))) return r;
        const auto menuId = id + ".menu";
        MmsUiElementV1 e{}; e.h = H<MmsUiElementV1>(); e.id = S(menuId); e.ownerPlugin = S(p.id); e.label = S(label);
        e.kind = MMS_UI_MENU_ITEM; e.parentId = S("mms.menu.plugins"); e.commandId = S(id);
        if ((r = p.ui.registerElement(p.ui.context, &e, &handle))) return r;
    }
    const auto propertyId = p.id + ".value";
    MmsPropertyDescV1 property{}; property.info.h = H<MmsPropertyDescV1>(); property.info.id = S(propertyId);
    property.info.ownerPlugin = S(p.id); property.info.label = S("Edit probe value"); property.info.flags = MMS_PROPERTY_READ_ONLY;
    property.info.valueType = MMS_VALUE_INT; property.context = &p; property.read = Read; property.releaseValue = ReleaseRead;
    r = p.ui.registerProperty(p.ui.context, &property, &handle);
    if(r)return r;
    p.editable=property;p.editable.info.id=S(p.editableId);p.editable.info.flags=MMS_PROPERTY_UNDOABLE;
    p.editable.info.minimum.type=p.editable.info.maximum.type=p.editable.info.defaultValue.type=MMS_VALUE_INT;
    p.editable.info.maximum.value.signedValue=100;p.editable.stage=PropertyStage;
    r=p.ui.registerProperty(p.ui.context,&p.editable,&p.editableHandle);
    return !r && p.id.find(".failstart") != std::string::npos ? MMS_E_FAILED : r;
}
void MMS_CALL Event(void* context, const MmsEventV1* event) {
    auto& p=*static_cast<Plugin*>(context); ProbeDenied(p,4096);
    if (event && event->kind==MMS_EVENT_PROPERTY_CHANGED) {
        ++p.propertyEvents;MmsEditApiV1 api{};
        if (!p.closingEvents && p.host.queryService(p.host.context,S("mms.edit"),1,0,sizeof(api),&api)==MMS_OK) ++p.eventServiceReady;
    }
    if (event && event->kind==MMS_EVENT_UI_CLOSING) ++p.closingEvents;
}
MmsResult MMS_CALL Stop(void* context) { ProbeDenied(*static_cast<Plugin*>(context), 256); return MMS_OK; }
void MMS_CALL Destroy(void* context) {
    auto* p = static_cast<OwnedPlugin*>(context); stoppedFlags = p->flags; stoppedCancels = p->cancels;
    stoppedPropertyFlags=p->propertyFlags;stoppedPropertyCalls=p->propertyCalls;stoppedPropertyCandidates=p->properties.size();
    stoppedPropertyEvents=p->propertyEvents;stoppedClosingEvents=p->closingEvents;stoppedEventServiceReady=p->eventServiceReady;
    if (current == p) current = nullptr; delete p;
}
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor, uint32_t bytes, MmsPluginApiV1* out) {
    if (!out || bytes < sizeof(*out) || major != 1 || minor) return MMS_E_VERSION;
    *out = {H<MmsPluginApiV1>(), Create, Start, Event, Stop, Destroy}; return MMS_OK;
}
extern "C" int GetProbeValue() { return current ? current->value : -1; }
extern "C" uint64_t GetProbeFlags() { return current ? current->flags : stoppedFlags; }
extern "C" int GetProbeCancels() { return current ? current->cancels : stoppedCancels; }
extern "C" uint64_t GetProbePending() { return current ? current->pending : 0; }
extern "C" uint64_t GetProviderProbeFlags() { return current ? current->providerProbeFlags : 0; }
extern "C" void SetProviderProbeMask(uint64_t mask) { if(current){current->providerProbeMask=mask;current->providerProbeFlags=0;} }
extern "C" void SetPropertyStageMode(uint32_t mode){if(current){current->propertyMode=mode;current->propertyFlags=current->propertyCalls=0;}}
extern "C" uint64_t GetPropertyStageFlags(){return current?current->propertyFlags:stoppedPropertyFlags;}
extern "C" uint64_t GetPropertyStageCalls(){return current?current->propertyCalls:stoppedPropertyCalls;}
extern "C" uint64_t GetPropertyCandidateCount(){return current?current->properties.size():stoppedPropertyCandidates;}
extern "C" uint64_t GetPropertyOther(){return current?current->propertyOther:0;}
extern "C" MmsResult GetPropertyStageFailure(){return current?current->propertyFailure:MMS_E_NOT_READY;}
extern "C" __declspec(dllexport) uint64_t GetPropertyEventCount(){return current?current->propertyEvents:stoppedPropertyEvents;}
extern "C" __declspec(dllexport) uint64_t GetClosingEventCount(){return current?current->closingEvents:stoppedClosingEvents;}
extern "C" __declspec(dllexport) uint64_t GetPropertyEventServiceReady(){return current?current->eventServiceReady:stoppedEventServiceReady;}
