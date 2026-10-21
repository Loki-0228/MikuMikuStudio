// Actual DLL: public negotiated services only, no Manager/queue test hooks.
#include "scoped_subscription_probe.h"
#include <mms/cpp/data_refresh.hpp>
#include <mms/edit.h>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
namespace {
using namespace mms::sdk;
constexpr uint32_t kinds=MMS_UI_REFRESH_DATA|MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_REPAINT;
constexpr MmsHandle sentinel=UINT64_C(0x123456789abcdef0);
struct Plugin;
struct Context{Plugin* owner=nullptr;uint32_t index=0;};
struct Plugin {
    MmsHostV1 host{};MmsUiApiV1 ui{};MmsUiRefreshDependencyApiV1 dependencies{};
    MmsUiRefreshSubscriptionApiV1 legacy{};MmsUiRefreshSubscriptionApiV1_1 subscriptions{};
    RefreshServices refresh;DataServices data;std::array<DataSource,2> sources;std::array<DataBinding,4> bindings;
    std::array<Context,4> contexts;ScopedSubscriptionProbeState state{};std::string id;
    std::thread::id thread;bool begun=false,probing=false;
};
Plugin* current=nullptr;
constexpr const char* labels[]{"Scoped trigger","Scoped A","Scoped B","Scoped C"};
bool Ui(const Plugin& p){return p.thread==std::this_thread::get_id();}
void Log(Plugin& p,const char* text){if(p.host.log)p.host.log(p.host.context,1,Text(text));}
MmsUiRefreshScopedSubscriptionDescV1 Desc(const Plugin& p){
    MmsUiRefreshScopedSubscriptionDescV1 desc{};desc.h=Header<MmsUiRefreshScopedSubscriptionDescV1>();
    desc.binding=p.bindings[0].get();desc.seed=p.state.scope==MMS_UI_REFRESH_SELF?p.state.targets[0]:p.state.seed;
    desc.scope=p.state.scope;desc.kinds=kinds;return desc;
}
MmsUiRefreshDependencyDescV1 EdgeDesc(const Plugin& p,unsigned index){
    return {Header<MmsUiRefreshDependencyDescV1>(),index==2?p.state.targets[1]:p.state.seed,p.state.targets[index+1],0,0};
}
MmsResult Graph(Plugin& p,uint32_t mask){
    if(mask&~7u)return MMS_E_ARGUMENT;
    for(unsigned i=0;i<3;++i)if(p.state.edges[i]&&!(mask&(1u<<i))){
        const auto result=p.dependencies.unregisterDependency(p.dependencies.context,p.state.edges[i]);if(result)return result;p.state.edges[i]=0;
    }
    for(unsigned i=0;i<3;++i)if(!p.state.edges[i]&&(mask&(1u<<i))){
        const auto desc=EdgeDesc(p,i);const auto result=p.dependencies.registerDependency(p.dependencies.context,&desc,&p.state.edges[i]);if(result)return result;
    }
    p.state.edgeMask=mask;return MMS_OK;
}
int32_t Display(Plugin& p,unsigned index,uint64_t& revision){
    DataDisplay display;revision=0;if(p.bindings[index].Acquire(display)!=MMS_OK)return INT32_MIN;
    const auto bytes=display.bytes();if(bytes.bytes!=sizeof(int32_t)||!bytes.data)return INT32_MIN;
    int32_t value=0;std::memcpy(&value,bytes.data,sizeof(value));revision=display.value().sourceRevision;return value;
}
void Trace(Plugin& p,uint32_t phase,uint32_t index,int32_t value,uint64_t revision){
    auto& s=p.state;if(s.events<SCOPED_PROBE_TRACE)s.trace[s.events++]={phase,index,value,0,revision};else ++s.overflow;
}
MmsResult MMS_CALL State(void* context,MmsObjectRef,MmsCommandStateV1* out){
    if(!out)return MMS_E_ARGUMENT;auto& c=*static_cast<Context*>(context);auto& p=*c.owner;auto& s=p.state;
    ++s.states[c.index];s.uiCallbacks=uint32_t(s.uiCallbacks&&Ui(p));uint64_t revision=0;const auto value=Display(p,c.index,revision);
    s.stateValues[c.index]=value;s.stateRevisions[c.index]=revision;Trace(p,2,c.index,value,revision);
    *out={Header<MmsCommandStateV1>(),1,0,1,0,{}};return MMS_OK;
}
MmsResult MMS_CALL Invoke(void*,MmsObjectRef,MmsSpan,MmsHandle){return MMS_OK;}
MmsResult MMS_CALL Prepare(void* context,const MmsUiDataPrepareV1* input){
    return detail::Call([&]()->MmsResult{
        auto& c=*static_cast<Context*>(context);auto& p=*c.owner;auto& s=p.state;
        if(!input||input->sourceBytes.bytes!=sizeof(int32_t)||!input->sourceBytes.data)return MMS_E_ARGUMENT;
        const unsigned source=c.index<2?0:1;int32_t value=0;std::memcpy(&value,input->sourceBytes.data,sizeof(value));
        ++s.prepared[c.index];s.uiCallbacks=uint32_t(s.uiCallbacks&&Ui(p));s.identity&=input->source==p.sources[source].get()&&
            input->target.element==s.targets[c.index].element&&input->target.instanceGeneration==s.targets[c.index].instanceGeneration&&
            input->target.projectEpoch==s.targets[c.index].projectEpoch;
        s.preparedValues[c.index]=value;s.preparedRevisions[c.index]=input->sourceRevision;Trace(p,1,c.index,value,input->sourceRevision);
        if(!p.probing){
            p.probing=true;++s.readOnlyChecks;const auto desc=Desc(p);MmsHandle output=sentinel;
            const MmsUiRefreshDependencyDescV1 edge{Header<MmsUiRefreshDependencyDescV1>(),s.seed,s.targets[0],0,0};
            s.readonlyEdge=p.dependencies.registerDependency(p.dependencies.context,&edge,&output);s.outputsPreserved&=output==sentinel;
            s.readonlyUnedge=p.dependencies.unregisterDependency(p.dependencies.context,s.edges[0]);
            output=sentinel;s.readonlySubscribe=p.subscriptions.subscribeScoped(p.subscriptions.context,&desc,&output);s.outputsPreserved&=output==sentinel;
            int32_t rejected=319;MmsUiDataSourceStateV1 state{};state.revision=sentinel;
            s.readonlyPublish=p.sources[0].Publish({&rejected,sizeof(rejected)},s.sourceRevisions[0],state);s.outputsPreserved&=state.revision==sentinel;
            MmsUiRefreshScopedSubscriptionStateV1 queried{};queried.h=Header<MmsUiRefreshScopedSubscriptionStateV1>();
            // Existing DATA publication remains worker-safe during a UI
            // prepare. This idempotent publication must retain its revision;
            // changed-version acceptance is tested by the queue cohort probe.
            std::thread worker([&]{MmsUiDataSourceStateV1 noChange{};
                s.readonlyWorkerPublish=p.sources[0].Publish({&s.sourceValues[0],sizeof(int32_t)},s.sourceRevisions[0],noChange);
                s.outputsPreserved&=noChange.revision==s.sourceRevisions[0];
                s.readonlyWorkerQuery=p.subscriptions.queryScoped(p.subscriptions.context,s.subscription,&queried);});worker.join();
            s.readOnlyDenied&=s.readonlyEdge==MMS_E_BUSY&&s.readonlyUnedge==MMS_E_BUSY&&s.readonlySubscribe==MMS_E_BUSY&&
                s.readonlyPublish==MMS_E_BUSY&&s.readonlyWorkerPublish==MMS_OK&&s.readonlyWorkerQuery==MMS_OK&&queried.subscription==s.subscription;
        }
        Log(p,"SCOPED_PROBE_PREPARE");return p.data.WritePrepared(input->builder,{&value,sizeof(value)});
    });
}
MmsResult MMS_CALL Create(const MmsHostV1* host,const MmsCreateInfoV1* info,void** out){
    return detail::Call([&]()->MmsResult{
        if(!host||!info||!out||!host->queryService)return MMS_E_ARGUMENT;if(current)return MMS_E_BUSY;
        auto p=std::make_unique<Plugin>();p->host=*host;p->thread=std::this_thread::get_id();p->id.assign(info->pluginId.data,size_t(info->pluginId.bytes));
        auto& s=p->state;s.size=sizeof(s);s.identity=s.uiCallbacks=s.readOnlyDenied=s.outputsPreserved=1;
        auto result=host->queryService(host->context,Text("mms.ui"),1,0,sizeof(p->ui),&p->ui);if(result)return result;
        if((result=RefreshServices::Query(*host,p->refresh)))return result;if((result=DataServices::Query(*host,p->data)))return result;
        if((result=host->queryService(host->context,Text("mms.ui.refresh.dependencies"),1,0,sizeof(p->dependencies),&p->dependencies)))return result;
        if((result=host->queryService(host->context,Text("mms.ui.refresh.subscribe"),1,0,sizeof(p->legacy),&p->legacy)))return result;
        if((result=host->queryService(host->context,Text("mms.ui.refresh.subscribe"),1,1,sizeof(p->subscriptions),&p->subscriptions)))return result;
        s.legacySize=p->legacy.h.size;s.legacyMinor=p->legacy.h.minor;s.newSize=p->subscriptions.h.size;s.newMinor=p->subscriptions.h.minor;
        s.legacyCaps.h=Header<MmsUiRefreshSubscriptionCapabilitiesV1>();s.capabilities.h=Header<MmsUiRefreshSubscriptionCapabilitiesV1>();
        if((result=p->legacy.getCapabilities(p->legacy.context,&s.legacyCaps)))return result;
        if((result=p->subscriptions.getCapabilities(p->subscriptions.context,&s.capabilities)))return result;
        MmsUiRefreshSubscriptionApiV1_1 unchanged{};std::memset(&unchanged,0x5a,sizeof(unchanged));const auto before=unchanged;
        s.truncatedQuery=host->queryService(host->context,Text("mms.ui.refresh.subscribe"),1,1,sizeof(p->legacy),&unchanged);
        s.negotiationPreserved=!std::memcmp(&before,&unchanged,sizeof(unchanged));
        MmsEditApiV1 edit{};s.editQuery=host->queryService(host->context,Text("mms.edit"),1,0,sizeof(edit),&edit);
        for(unsigned i=0;i<4;++i)p->contexts[i]={p.get(),i};current=p.get();*out=p.release();return MMS_OK;
    });
}
MmsResult MMS_CALL Start(void* context){
    return detail::Call([&]()->MmsResult{
        auto& p=*static_cast<Plugin*>(context);const auto seedId=p.id+".seed";
        MmsUiElementV1 seed{};seed.h=Header<MmsUiElementV1>();seed.id=Text(seedId);seed.ownerPlugin=Text(p.id);
        seed.parentId=Text("mms.toolbar.main.extensions");seed.kind=MMS_UI_GROUP;seed.capabilities=MMS_UI_CONTAINER|MMS_UI_LAYOUT;
        auto result=p.ui.registerElement(p.ui.context,&seed,&p.state.seedHandle);if(result)return result;
        for(unsigned i=0;i<4;++i){const auto commandId=p.id+".command."+std::to_string(i),buttonId=p.id+".button."+std::to_string(i);
            MmsCommandDescV1 command{};command.info.h=Header<MmsCommandDescV1>();command.info.id=Text(commandId);command.info.ownerPlugin=Text(p.id);
            command.info.label=Text(labels[i]);command.context=&p.contexts[i];command.getState=State;command.invoke=Invoke;MmsHandle registered=0;
            if((result=p.ui.registerCommand(p.ui.context,&command,&registered)))return result;
            MmsUiElementV1 button{};button.h=Header<MmsUiElementV1>();button.id=Text(buttonId);button.ownerPlugin=Text(p.id);button.label=command.info.label;
            button.parentId=Text(i>=2?"mms.properties.main.extensions":"mms.toolbar.main.extensions");
            if(i==1&&p.id.find("subtree")!=std::string::npos)button.parentId=Text(seedId);
            button.kind=MMS_UI_BUTTON;button.commandId=Text(commandId);button.capabilities=MMS_UI_LAYOUT|MMS_UI_HIDE|MMS_UI_MOVE;
            if((result=p.ui.registerElement(p.ui.context,&button,&p.state.buttons[i])))return result;
        }
        Log(p,"SCOPED_PROBE_START");return MMS_OK;
    });
}
void MMS_CALL Event(void*,const MmsEventV1*){}
MmsResult MMS_CALL Stop(void* context){
    auto& p=*static_cast<Plugin*>(context);auto& s=p.state;++s.stops;s.preparesAtStop=0;for(auto count:s.prepared)s.preparesAtStop+=count;
    MmsUiRefreshScopedSubscriptionStateV1 value{};value.h=Header<MmsUiRefreshScopedSubscriptionStateV1>();value.workGeneration=sentinel;const auto before=value;
    s.stopQuery=p.subscriptions.queryScoped(p.subscriptions.context,s.subscription,&value);s.stopOutputPreserved=!std::memcmp(&before,&value,sizeof(value));
    s.stopUnsubscribe=p.subscriptions.unsubscribe(p.subscriptions.context,s.subscription);
    if(s.stopUnsubscribe==MMS_OK||s.stopUnsubscribe==MMS_E_STALE)s.subscription=0;
    Log(p,"SCOPED_PROBE_STOP");return MMS_OK;
}
void MMS_CALL Destroy(void* context){auto* p=static_cast<Plugin*>(context);Log(*p,"SCOPED_PROBE_DESTROY");if(current==p)current=nullptr;delete p;}
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major,uint32_t minor,uint32_t bytes,MmsPluginApiV1* out){
    if(!out||bytes<sizeof(*out))return MMS_E_ARGUMENT;if(major!=1||minor)return MMS_E_VERSION;*out={Header<MmsPluginApiV1>(),Create,Start,Event,Stop,Destroy};return MMS_OK;
}
extern "C" MmsResult MMS_CALL ScopedProbeBegin(uint32_t scope,uint32_t edgeMask){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;if(p->begun)return MMS_E_BUSY;
        if(scope!=MMS_UI_REFRESH_SELF&&scope!=MMS_UI_REFRESH_SUBTREE&&scope!=MMS_UI_REFRESH_DEPENDENTS)return MMS_E_ARGUMENT;p->state.scope=scope;
        MmsUiRefreshTargetV1 target{};auto result=p->refresh.Target(p->state.seedHandle,target);if(result)return result;
        if(!target.ready)return MMS_E_NOT_READY;p->state.seed=target.target;
        for(unsigned i=0;i<4;++i){if((result=p->refresh.Target(p->state.buttons[i],target)))return result;if(!target.ready)return MMS_E_NOT_READY;p->state.targets[i]=target.target;}
        for(unsigned i=0;i<2;++i){const auto sourceId=p->id+".source."+std::to_string(i);if((result=p->data.RegisterSource(Text(sourceId),sizeof(int32_t),p->sources[i])))return result;p->state.sources[i]=p->sources[i].get();}
        for(unsigned i=0;i<4;++i){if((result=p->data.Bind(p->state.targets[i],p->sources[i<2?0:1],sizeof(int32_t),p->bindings[i],Prepare,&p->contexts[i])))return result;p->state.bindings[i]=p->bindings[i].get();}
        if((result=Graph(*p,edgeMask)))return result;const auto desc=Desc(*p);
        if((result=p->subscriptions.subscribeScoped(p->subscriptions.context,&desc,&p->state.subscription)))return result;
        p->state.events=0;for(unsigned i=0;i<4;++i){p->state.states[i]=0;p->state.prepared[i]=0;}p->begun=true;return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL ScopedProbePublish(uint32_t source,int32_t first,uint32_t count){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p||!p->begun)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;
        if(source>=2||!count||count>64||int64_t(first)+count-1>INT32_MAX)return MMS_E_ARGUMENT;MmsResult result=MMS_OK;
        std::thread worker([&]{p->state.workerWasWorker=!Ui(*p);for(uint32_t i=0;i<count;++i){int32_t value=first+int32_t(i);MmsUiDataSourceStateV1 published{};
            result=p->sources[source].Publish({&value,sizeof(value)},p->state.sourceRevisions[source],published);if(result)break;
            p->state.sourceRevisions[source]=published.revision;p->state.sourceValues[source]=value;}});worker.join();return result;
    });
}
extern "C" MmsResult MMS_CALL ScopedProbeGraph(uint32_t mask){auto* p=current;if(!p||!p->begun)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;return Graph(*p,mask);}
extern "C" MmsResult MMS_CALL ScopedProbeQuery(ScopedSubscriptionProbeState* out){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;if(!out||out->size!=sizeof(*out))return MMS_E_ARGUMENT;
        auto state=p->state;state.subscriptionState={};state.subscriptionState.h=Header<MmsUiRefreshScopedSubscriptionStateV1>();
        state.query=p->subscriptions.queryScoped(p->subscriptions.context,state.subscription,&state.subscriptionState);
        state.legacyState={};state.legacyState.h=Header<MmsUiRefreshSubscriptionStateV1>();state.legacyQuery=p->legacy.query(p->legacy.context,state.subscription,&state.legacyState);
        for(unsigned i=0;i<4;++i)state.displayValues[i]=Display(*p,i,state.displayRevisions[i]);*out=state;return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL ScopedProbeAcquireHold(void){
    auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;if(p->state.heldSnapshot)return MMS_E_BUSY;
    MmsUiSnapshotV1 snapshot{};snapshot.h=Header<MmsUiSnapshotV1>();const auto result=p->ui.acquireSnapshot(p->ui.context,0,&snapshot);
    if(!result)p->state.heldSnapshot=snapshot.handle;return result;
}
extern "C" MmsResult MMS_CALL ScopedProbeReleaseHold(void){auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;
    if(p->state.heldSnapshot){p->ui.releaseSnapshot(p->ui.context,p->state.heldSnapshot);p->state.heldSnapshot=0;}return MMS_OK;}
extern "C" MmsResult MMS_CALL ScopedProbeRemoveSeed(void){auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;
    return p->ui.unregisterItem(p->ui.context,p->state.seedHandle);}
