// Actual DLL, negotiated public services only. No Manager/registry/test hook.
#include "property_refresh_probe.h"
#include <mms/cpp/property_refresh.hpp>
#include <mms/edit.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
namespace {
using namespace mms::sdk;
constexpr uint32_t kinds=MMS_UI_REFRESH_DATA|MMS_UI_REFRESH_REPAINT;
constexpr MmsHandle sentinel=UINT64_C(0x123456789abcdef0);
struct Plugin {
    MmsHostV1 host{};MmsUiApiV1 ui{};RefreshServices refresh;PropertyRefreshServices properties;
    PropertyRefreshSubscription subscription;PropertyRefreshProbeState state{};
    std::atomic<int64_t> value{2};std::thread::id thread;std::string id,propertyId,inputId,commandId,buttonId;
    uint32_t flags=0;bool readProbe=false;
};
Plugin* current=nullptr;
bool Ui(const Plugin& p){return p.thread==std::this_thread::get_id();}
void Log(Plugin& p,const char* text){if(p.host.log)p.host.log(p.host.context,1,Text(text));}
MmsUiPropertyRefreshSubscriptionDescV1 Desc(const Plugin& p){
    return {Header<MmsUiPropertyRefreshSubscriptionDescV1>(),p.state.target,p.state.propertyRef,kinds,p.flags,0};
}
MmsResult MMS_CALL Read(void* context,MmsObjectRef object,MmsValue* out,MmsHandle* lease){
    return detail::Call([&]()->MmsResult{
        if(!out||!lease)return MMS_E_ARGUMENT;auto& p=*static_cast<Plugin*>(context);auto& s=p.state;
        ++s.reads;++s.readDepth;if(s.readDepth>s.maxReadDepth)s.maxReadDepth=s.readDepth;
        struct Depth{uint32_t& value;~Depth(){--value;}} depth{s.readDepth};
        s.readWasUi&=Ui(p);s.readIdentity&=!object.id.lo&&!object.id.hi&&!object.generation;
        if(p.readProbe&&p.subscription){
            p.readProbe=false;++s.readOnlyChecks;const auto desc=Desc(p);MmsHandle unchanged=sentinel;
            s.readNotify=p.properties.Notify(s.propertyRef);
            MmsUiPropertyRefreshSubscriptionStateV1 worker{};
            std::thread background([&]{s.readWorkerNotify=p.properties.Notify(s.propertyRef);s.readWorkerQuery=p.subscription.State(worker);});background.join();
            s.readSubscribe=p.properties.api().subscribe(p.properties.api().context,&desc,&unchanged);
            s.readOnlyOutputPreserved&=unchanged==sentinel;
            s.readUnsubscribe=p.properties.api().unsubscribe(p.properties.api().context,p.subscription.get());
            s.readMutation=p.ui.unregisterItem(p.ui.context,s.input);
            s.readOnlyDenied&=s.readNotify==MMS_E_BUSY&&s.readWorkerNotify==MMS_E_BUSY&&s.readWorkerQuery==MMS_OK&&
                s.readSubscribe==MMS_E_BUSY&&s.readUnsubscribe==MMS_E_BUSY&&s.readMutation==MMS_E_BUSY&&
                worker.subscription==p.subscription.get();
        }
        const auto value=p.value.load();s.lastRead=value;
        if(s.reads<=PROPERTY_REFRESH_PROBE_TRACE)s.traceValues[s.reads-1]=value;else ++s.traceOverflow;
        *out={};out->type=MMS_VALUE_INT;out->value.signedValue=value;*lease=77;
        Log(p,"PROPERTY_REFRESH_PROBE_READ");return MMS_OK;
    });
}
void MMS_CALL Release(void* context,MmsHandle lease){
    auto& p=*static_cast<Plugin*>(context);++p.state.releases;p.state.readIdentity&=lease==77;p.state.readWasUi&=Ui(p);
}
MmsResult MMS_CALL State(void*,MmsObjectRef,MmsCommandStateV1* out){
    if(!out)return MMS_E_ARGUMENT;*out={Header<MmsCommandStateV1>(),1,0,1,0,{}};return MMS_OK;
}
MmsResult MMS_CALL Invoke(void* context,MmsObjectRef,MmsSpan,MmsHandle){
    auto& p=*static_cast<Plugin*>(context);if(!p.subscription)return MMS_E_NOT_READY;
    ++p.state.commands;p.value.fetch_add(1);p.state.commandNotify=p.properties.Notify(p.state.propertyRef);return p.state.commandNotify;
}
MmsResult MMS_CALL Create(const MmsHostV1* host,const MmsCreateInfoV1* info,void** out){
    return detail::Call([&]()->MmsResult{
        if(!host||!info||!out||!host->queryService)return MMS_E_ARGUMENT;if(current)return MMS_E_BUSY;
        auto p=std::make_unique<Plugin>();p->host=*host;p->thread=std::this_thread::get_id();
        p->id.assign(info->pluginId.data,size_t(info->pluginId.bytes));p->propertyId=p->id+".value";p->inputId=p->id+".input";
        p->commandId=p->id+".command";p->buttonId=p->id+".button";p->state.size=sizeof(p->state);
        p->state.readWasUi=p->state.readIdentity=p->state.readOnlyDenied=p->state.readOnlyOutputPreserved=1;
        auto result=host->queryService(host->context,Text("mms.ui"),1,0,sizeof(p->ui),&p->ui);if(result)return result;
        if((result=RefreshServices::Query(*host,p->refresh)))return result;
        if((result=PropertyRefreshServices::Query(*host,p->properties)))return result;
        if((result=p->properties.Capabilities(p->state.capabilities)))return result;
        MmsEditApiV1 edit{};p->state.editQuery=host->queryService(host->context,Text("mms.edit"),1,0,sizeof(edit),&edit);
        current=p.get();*out=p.release();return MMS_OK;
    });
}
MmsResult MMS_CALL Start(void* context){
    return detail::Call([&]()->MmsResult{
        auto& p=*static_cast<Plugin*>(context);auto& s=p.state;
        MmsPropertyDescV1 property{};property.info.h=Header<MmsPropertyDescV1>();property.info.id=Text(p.propertyId);
        property.info.ownerPlugin=Text(p.id);property.info.label=Text("Property notification value");property.info.valueType=MMS_VALUE_INT;
        property.info.flags=MMS_PROPERTY_READ_ONLY;property.context=&p;property.read=Read;property.releaseValue=Release;
        auto result=p.ui.registerProperty(p.ui.context,&property,&s.property);if(result)return result;
        MmsUiElementV1 input{};input.h=Header<MmsUiElementV1>();input.id=Text(p.inputId);input.ownerPlugin=Text(p.id);
        input.parentId=Text("mms.toolbar.main.extensions");input.label=Text("Property notification value");input.propertyId=Text(p.propertyId);
        input.kind=MMS_UI_INPUT;input.capabilities=MMS_UI_LAYOUT|MMS_UI_MOVE|MMS_UI_HIDE|MMS_UI_RESIZE;
        if((result=p.ui.registerElement(p.ui.context,&input,&s.input)))return result;
        MmsCommandDescV1 command{};command.info.h=Header<MmsCommandDescV1>();command.info.id=Text(p.commandId);
        command.info.ownerPlugin=Text(p.id);command.info.label=Text("Property notification command");command.info.flags=MMS_COMMAND_INVOKE_ONLY;
        command.context=&p;command.getState=State;command.invoke=Invoke;
        if((result=p.ui.registerCommand(p.ui.context,&command,&s.command)))return result;
        auto button=input;button.id=Text(p.buttonId);button.label=command.info.label;button.kind=MMS_UI_BUTTON;button.propertyId={};button.commandId=Text(p.commandId);
        if((result=p.ui.registerElement(p.ui.context,&button,&s.button)))return result;
        // No subscriptions until Begin, after NativeUi has mounted this INPUT.
        Log(p,"PROPERTY_REFRESH_PROBE_START");return MMS_OK;
    });
}
void MMS_CALL Event(void*,const MmsEventV1*){}
MmsResult MMS_CALL Stop(void* context){
    return detail::Call([&]()->MmsResult{
        auto& p=*static_cast<Plugin*>(context);auto& s=p.state;++s.stops;s.readsAtStop=s.reads;
        MmsUiPropertyRefreshSubscriptionStateV1 old{};old.h=Header<MmsUiPropertyRefreshSubscriptionStateV1>();old.latestSequence=sentinel;const auto before=old;
        s.stopQuery=p.properties.api().query(p.properties.api().context,s.originalSubscription,&old);
        s.stopOutputPreserved=!std::memcmp(&before,&old,sizeof(old));s.stopNotify=p.properties.Notify(s.propertyRef);
        s.stopUnsubscribe=p.properties.api().unsubscribe(p.properties.api().context,s.originalSubscription);
        s.stopRelease=p.subscription.Release();if(s.stopRelease)return s.stopRelease;
        Log(p,"PROPERTY_REFRESH_PROBE_STOP");return MMS_OK;
    });
}
void MMS_CALL Destroy(void* context){auto* p=static_cast<Plugin*>(context);Log(*p,"PROPERTY_REFRESH_PROBE_DESTROY");if(current==p)current=nullptr;delete p;}
}
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major,uint32_t minor,uint32_t bytes,MmsPluginApiV1* out){
    if(!out||bytes<sizeof(*out))return MMS_E_ARGUMENT;if(major!=1||minor)return MMS_E_VERSION;
    *out={Header<MmsPluginApiV1>(),Create,Start,Event,Stop,Destroy};return MMS_OK;
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeBegin(uint32_t flags){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;if(p->subscription)return MMS_E_BUSY;
        if(flags&~MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY)return MMS_E_ARGUMENT;
        MmsUiRefreshTargetV1 target{};auto result=p->refresh.Target(p->state.input,target);if(result)return result;
        if(!target.ready||(target.supportedKinds&kinds)!=kinds)return MMS_E_NOT_READY;
        if((result=p->properties.GetPropertyRef(p->state.property,p->state.propertyRef)))return result;p->state.target=target.target;p->flags=flags;
        result=p->properties.SubscribeInput(p->refresh,p->state.input,p->state.property,p->subscription,kinds,flags);
        if(!result){p->state.originalSubscription=p->subscription.get();p->readProbe=true;}return result;
    });
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeBurst(int64_t first,uint32_t count){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p||!p->subscription)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;
        if(!count||count>128||first>(std::numeric_limits<int64_t>::max)()-int64_t(count-1))return MMS_E_ARGUMENT;
        MmsResult result=MMS_OK;uint32_t notified=0;
        std::thread background([&]{p->state.workerWasWorker=!Ui(*p);for(uint32_t index=0;index<count;++index){
            p->value.store(first+index);result=p->properties.Notify(p->state.propertyRef);if(result)break;++notified;}});background.join();
        p->state.notifies+=notified;return result;
    });
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeSetValue(int64_t value){
    auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;p->value.store(value);return MMS_OK;
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeGetValue(int64_t* out){
    auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;if(!out)return MMS_E_ARGUMENT;*out=p->value.load();return MMS_OK;
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeNotify(void){return current?current->properties.Notify(current->state.propertyRef):MMS_E_NOT_READY;}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeInvoke(void){
    return current?current->ui.invoke(current->ui.context,Text(current->commandId),{},{}):MMS_E_NOT_READY;
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeCheckThreads(void){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p||!p->subscription)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;
        auto& s=p->state;const auto desc=Desc(*p);MmsHandle output=sentinel;
        MmsUiPropertyRefreshCapabilitiesV1 caps{};caps.h=Header<MmsUiPropertyRefreshCapabilitiesV1>();caps.reserved=sentinel;const auto oldCaps=caps;
        MmsUiPropertyRefV1 ref{};ref.h=Header<MmsUiPropertyRefV1>();ref.property=sentinel;const auto oldRef=ref;
        std::thread background([&]{s.workerQuery=p->subscription.State(s.workerState);
            s.workerCaps=p->properties.api().getCapabilities(p->properties.api().context,&caps);
            s.workerRef=p->properties.api().getPropertyRef(p->properties.api().context,s.property,&ref);
            s.workerSubscribe=p->properties.api().subscribe(p->properties.api().context,&desc,&output);
            PropertyRefreshSubscription rejected;s.workerSdkSubscribe=p->properties.SubscribeInput(p->refresh,s.input,s.property,rejected,kinds,p->flags);
            s.workerUnsubscribe=p->properties.api().unsubscribe(p->properties.api().context,p->subscription.get());});background.join();
        s.workerOutputPreserved=output==sentinel&&!std::memcmp(&caps,&oldCaps,sizeof(caps))&&!std::memcmp(&ref,&oldRef,sizeof(ref));return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeCheckOutputs(void){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p||!p->subscription)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;auto& s=p->state;
        MmsUiPropertyRefV1 ref{};ref.h=Header<MmsUiPropertyRefV1>();ref.property=sentinel;const auto oldRef=ref;
        s.badRef=p->properties.api().getPropertyRef(p->properties.api().context,sentinel,&ref);
        auto desc=Desc(*p);++desc.property.registrationGeneration;MmsHandle output=sentinel;
        s.badSubscribe=p->properties.api().subscribe(p->properties.api().context,&desc,&output);
        MmsUiPropertyRefreshSubscriptionStateV1 state{};state.h=Header<MmsUiPropertyRefreshSubscriptionStateV1>();state.latestSequence=sentinel;const auto old=state;
        s.badQuery=p->properties.api().query(p->properties.api().context,sentinel,&state);
        s.badOutputPreserved=output==sentinel&&!std::memcmp(&ref,&oldRef,sizeof(ref))&&!std::memcmp(&state,&old,sizeof(state));return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeQuery(PropertyRefreshProbeState* out){
    return detail::Call([&]()->MmsResult{
        auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;if(!out||out->size!=sizeof(*out))return MMS_E_ARGUMENT;
        auto state=p->state;state.value=p->value.load();state.subscription=p->subscription.get();
        state.subscriptionQuery=p->subscription?p->subscription.State(state.subscriptionState):MMS_E_NOT_READY;*out=state;return MMS_OK;
    });
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeAcquireHold(void){
    auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;if(p->state.heldSnapshot)return MMS_E_BUSY;
    MmsUiSnapshotV1 snapshot{};snapshot.h=Header<MmsUiSnapshotV1>();const auto result=p->ui.acquireSnapshot(p->ui.context,0,&snapshot);
    if(!result)p->state.heldSnapshot=snapshot.handle;return result;
}
extern "C" MmsResult MMS_CALL PropertyRefreshProbeReleaseHold(void){
    auto* p=current;if(!p)return MMS_E_NOT_READY;if(!Ui(*p))return MMS_E_THREAD;
    if(p->state.heldSnapshot){p->ui.releaseSnapshot(p->ui.context,p->state.heldSnapshot);p->state.heldSnapshot=0;}return MMS_OK;
}
