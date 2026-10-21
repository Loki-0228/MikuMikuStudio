// Include after the existing Manager/NativeUi/property helpers. This is a
// real DLL/service/posted-dispatch test, without MMDApp or scene/physics claims.
#include "property_refresh_probe.h"
#include "ui_property_refresh.hpp"
#include "shared_history.hpp"

namespace manager_property_refresh_checks {
constexpr const char* id="org.example.property-refresh";
struct Probe {
    PropertyRefreshProbeBeginFn begin=nullptr;PropertyRefreshProbeBurstFn burst=nullptr;
    PropertyRefreshProbeValueFn set=nullptr;PropertyRefreshProbeGetValueFn get=nullptr;PropertyRefreshProbeQueryFn query=nullptr;
    PropertyRefreshProbeSimpleFn notify=nullptr,invoke=nullptr,checkThreads=nullptr,checkOutputs=nullptr,acquireHold=nullptr,releaseHold=nullptr;
    explicit Probe(HMODULE module){
        CHECK(module);
#define PROPERTY_PROBE(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(module,name));CHECK(member)
        PROPERTY_PROBE(begin,"PropertyRefreshProbeBegin");PROPERTY_PROBE(burst,"PropertyRefreshProbeBurst");
        PROPERTY_PROBE(set,"PropertyRefreshProbeSetValue");PROPERTY_PROBE(get,"PropertyRefreshProbeGetValue");PROPERTY_PROBE(query,"PropertyRefreshProbeQuery");
        PROPERTY_PROBE(notify,"PropertyRefreshProbeNotify");PROPERTY_PROBE(invoke,"PropertyRefreshProbeInvoke");
        PROPERTY_PROBE(checkThreads,"PropertyRefreshProbeCheckThreads");PROPERTY_PROBE(checkOutputs,"PropertyRefreshProbeCheckOutputs");
        PROPERTY_PROBE(acquireHold,"PropertyRefreshProbeAcquireHold");PROPERTY_PROBE(releaseHold,"PropertyRefreshProbeReleaseHold");
#undef PROPERTY_PROBE
    }
    PropertyRefreshProbeState Read()const{PropertyRefreshProbeState value{};value.size=sizeof(value);CHECK(query(&value)==MMS_OK);return value;}
    PropertyRefreshProbeState Complete(Manager& manager,uint64_t sequence)const{
        const auto until=GetTickCount64()+3000;auto value=Read();
        const auto completed=[&]{return value.subscriptionQuery==MMS_OK&&value.subscriptionState.latestSequence==sequence&&
            value.subscriptionState.submittedSequence==sequence&&value.subscriptionState.completedSequence==sequence&&
            !value.subscriptionState.pending&&!value.subscriptionState.waitingFor&&value.subscriptionState.lastResult==MMS_OK;};
        // Drain alone handles plugin tasks. The actual subscription consumer is
        // reached by dispatching Manager's posted refresh message on this UI thread.
        while(!completed()&&GetTickCount64()<until){manager.Drain();manager_refresh_checks::Pump(2);value=Read();}
        if(!completed())std::cerr<<"PROPERTY_MANAGER_COMPLETE wanted="<<sequence<<" query="<<value.subscriptionQuery<<
            " latest="<<value.subscriptionState.latestSequence<<" submitted="<<value.subscriptionState.submittedSequence<<
            " completed="<<value.subscriptionState.completedSequence<<" pending="<<value.subscriptionState.pending<<
            " wait="<<value.subscriptionState.waitingFor<<" result="<<value.subscriptionState.lastResult<<
            " reads="<<value.reads<<" readonly_ui="<<value.readNotify<<" readonly_worker="<<value.readWorkerNotify<<'\n';
        CHECK(completed());return value;
    }
};
static bool Same(const MmsUiTargetRefV1& a,const MmsUiTargetRefV1& b){return a.element==b.element&&a.instanceGeneration==b.instanceGeneration&&a.projectEpoch==b.projectEpoch;}
static bool Same(const MmsUiPropertyRefV1& a,const MmsUiPropertyRefV1& b){return a.property==b.property&&a.registrationGeneration==b.registrationGeneration&&a.projectEpoch==b.projectEpoch;}
static void Identity(Manager& manager,const PropertyRefreshProbeState& state){
    CHECK(state.property&&state.input&&state.subscription&&state.subscription==state.originalSubscription);
    CHECK(state.target.element==state.input&&state.target.instanceGeneration&&state.target.projectEpoch);
    CHECK(state.propertyRef.property==state.property&&state.propertyRef.registrationGeneration&&state.propertyRef.projectEpoch==state.target.projectEpoch);
    const auto property=manager.Ui().properties.at(std::string(id)+".value");
    CHECK(manager.Ui().properties.size()==1&&property->handle==state.property&&property->registrationGeneration==state.propertyRef.registrationGeneration&&
        property->desc.info.valueType==MMS_VALUE_INT&&(property->desc.info.flags&MMS_PROPERTY_READ_ONLY));
    CHECK(manager.Ui().elements.at(std::string(id)+".input").handle==state.input&&manager.Ui().elements.at(std::string(id)+".button").handle==state.button);
    CHECK(state.subscriptionQuery==MMS_OK&&state.subscriptionState.subscription==state.subscription&&
        Same(state.subscriptionState.target,state.target)&&Same(state.subscriptionState.property,state.propertyRef));
    CHECK(state.editQuery==MMS_E_NOT_FOUND&&!manager.CanInvokeEdit());
}
static void ReadOnly(const PropertyRefreshProbeState& state){
    CHECK(state.readOnlyChecks==1&&state.readOnlyDenied&&state.readOnlyOutputPreserved&&state.readWasUi&&state.readIdentity&&
        state.maxReadDepth==1&&!state.readDepth&&!state.traceOverflow&&state.releases==state.reads);
    CHECK(state.readNotify==MMS_E_BUSY&&state.readWorkerNotify==MMS_E_BUSY&&state.readWorkerQuery==MMS_OK&&
        state.readSubscribe==MMS_E_BUSY&&state.readUnsubscribe==MMS_E_BUSY&&state.readMutation==MMS_E_BUSY);
}
struct Journal {
    SharedHistory* history=nullptr;uint64_t revision=0,bytes=0;size_t undo=0,redo=0;
    explicit Journal(SharedHistory* value):history(value){if(history){CHECK(history->Revision(revision)==MMS_OK);bytes=history->RetainedBytes();undo=history->UndoCount();redo=history->RedoCount();}}
    void Same()const{if(history){uint64_t current=0;CHECK(history->Revision(current)==MMS_OK&&current==revision&&
        history->RetainedBytes()==bytes&&history->UndoCount()==undo&&history->RedoCount()==redo);}}
};
struct ValueChange final : HistoryChange {
    PropertyRefreshProbeGetValueFn get;PropertyRefreshProbeValueFn set;int64_t before,after;
    ValueChange(const Probe& probe,int64_t a,int64_t b):get(probe.get),set(probe.set),before(a),after(b){}
    struct Candidate final : PreparedHistoryChange {
        ValueChange& change;int64_t expected,target;
        Candidate(ValueChange& value,bool forward):change(value),expected(forward?value.before:value.after),target(forward?value.after:value.before){}
        bool IsCurrent()const noexcept override{int64_t actual=0;return change.get(&actual)==MMS_OK&&actual==expected;}
        void Publish()noexcept override{if(change.set(target)!=MMS_OK)std::terminate();}
    };
    MmsResult Prepare(bool forward,std::unique_ptr<PreparedHistoryChange>& out)override{out=std::make_unique<Candidate>(*this,forward);return MMS_OK;}
    uint64_t RetainedBytes()const noexcept override{return sizeof(*this);}
    bool RequiresPhysicsReset()const noexcept override{return false;}
};
static void HistoryRoundTrip(Manager& manager,SharedHistory& history,const Probe& probe,HWND field){
    auto state=probe.Read();const auto baseline=state.value;const auto sequence=state.subscriptionState.completedSequence;
    const auto manualNotifies=state.notifies,commands=state.commands;
    const auto reads=state.reads;const auto revision=manager.Ui().Revision(),preferences=manager.Ui().PreferencesRevision();
    auto change=std::make_shared<ValueChange>(probe,baseline,700);std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    CHECK(history.PrepareNative("property DLL native value",change,transaction)==MMS_OK&&probe.Read().reads==reads&&
        native_property_input_checks::Caption(field)==std::to_wstring(baseline));
    CHECK(history.CommitNative(std::move(transaction))==MMS_OK&&probe.Read().value==700&&probe.Read().reads==reads);
    state=probe.Complete(manager,sequence+1);CHECK(state.lastRead==700&&state.reads==reads+1&&native_property_input_checks::Caption(field)==L"700");
    CHECK(history.UndoCount()==1&&!history.RedoCount());
    CHECK(history.History(false)==MMS_OK);state=probe.Complete(manager,sequence+2);
    CHECK(state.value==baseline&&state.lastRead==baseline&&native_property_input_checks::Caption(field)==std::to_wstring(baseline)&&
        !history.UndoCount()&&history.RedoCount()==1);
    CHECK(history.History(true)==MMS_OK);state=probe.Complete(manager,sequence+3);
    CHECK(state.value==700&&native_property_input_checks::Caption(field)==L"700"&&history.UndoCount()==1&&!history.RedoCount());
    CHECK(history.SetMode(SharedHistory::RecordingMode::ExplicitOnly)==MMS_OK);
    auto adjustment=std::make_shared<ValueChange>(probe,700,701);
    CHECK(history.PrepareNative("unrecorded property DLL value",adjustment,transaction,SharedHistory::NativeIntent::Adjustment)==MMS_OK&&
        history.CommitNative(std::move(transaction))==MMS_OK);state=probe.Complete(manager,sequence+4);
    CHECK(state.value==701&&native_property_input_checks::Caption(field)==L"701"&&history.UndoCount()==1&&!history.RedoCount());
    CHECK(manager.Ui().Revision()==revision&&manager.Ui().PreferencesRevision()==preferences&&state.notifies==manualNotifies&&state.commands==commands);
    Identity(manager,state);ReadOnly(state);
}
static void Run(const fs::path& root,const fs::path& library,bool followHistory){
    const auto directory=root/(followHistory?"property-manager-history":"property-manager-notify");const auto package=directory/"PLUGINS"/"fixture";
    fs::create_directories(package);fs::copy_file(library,package/"property-refresh-probe.dll");
    Write(package/"plugin.json",ManifestText(id,"property-refresh-probe.dll"));
    std::vector<std::string> logs;const auto log=[&](const std::string& message){logs.push_back(message);};
    const auto count=[&](const char* marker){return std::count_if(logs.begin(),logs.end(),[&](const auto& text){return text.find(marker)!=std::string::npos;});};
    Manager manager(directory/"user",log);std::unique_ptr<SharedHistory> history;
    if(followHistory){
        history=std::make_unique<SharedHistory>(manager.Storage());
        CHECK(history->SetPublicationSignal({&manager,[](void* context,uint64_t revision)noexcept{
            static_cast<Manager*>(context)->NotifyHistoryPublication(revision);}})==MMS_OK);
        manager.Ui().Refresh().PropertyRefresh().SetHistoryAvailable(true);
    }
    // Deliberately never AttachEditHost: the same production edit gate stays
    // closed even while trusted native history publications can drive the UI.
    manager.Load(package.parent_path());const auto statuses=manager.Status();
    if(statuses.size()!=1||statuses[0].stage!="started"){
        for(const auto& status:statuses)std::cerr<<"PROPERTY_MANAGER_START "<<status.id<<" "<<status.stage<<" "<<status.error<<'\n';
        for(const auto& entry:logs)std::cerr<<"PROPERTY_MANAGER_LOG "<<entry<<'\n';
    }
    CHECK(statuses.size()==1&&statuses[0].stage=="started"&&count("PROPERTY_REFRESH_PROBE_START")==1);
    const auto module=GetModuleHandleW(L"property-refresh-probe.dll");Probe probe(module);auto state=probe.Read();
    CHECK(!state.subscription&&!state.readOnlyChecks&&state.editQuery==MMS_E_NOT_FOUND&&
        bool(state.capabilities.supportedFlags&MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY)==followHistory);
    manager_refresh_checks::Window window;auto native=std::make_unique<NativeUi>(manager.Ui(),window.value,log);
    native->Refresh();CHECK(native->Layout(10,620,160,true)>0);const auto field=native_property_input_checks::FindField(native->Toolbar());
    CHECK(IsWindowVisible(field)&&(GetWindowLongPtrW(field,GWL_STYLE)&ES_READONLY)&&native_property_input_checks::Caption(field)==L"2");
    const auto initialReads=probe.Read().reads;CHECK(initialReads&&!probe.Read().subscription);
    const auto registry=manager.Ui().Revision(),preferences=manager.Ui().PreferencesRevision();Journal journal(history.get());
    CHECK(probe.begin(followHistory?MMS_UI_PROPERTY_REFRESH_FOLLOW_HISTORY:0)==MMS_OK);state=probe.Read();
    CHECK(state.subscription&&state.subscriptionState.latestSequence&&state.subscriptionState.pending&&state.reads==initialReads);
    const auto initialSequence=state.subscriptionState.latestSequence;state=probe.Complete(manager,initialSequence);
    CHECK(state.reads==initialReads+1&&state.value==2&&state.lastRead==2);Identity(manager,state);ReadOnly(state);journal.Same();
    CHECK(probe.checkThreads()==MMS_OK&&probe.checkOutputs()==MMS_OK);state=probe.Read();
    CHECK(state.workerQuery==MMS_OK&&Same(state.workerState.target,state.target)&&Same(state.workerState.property,state.propertyRef)&&
        state.workerCaps==MMS_E_THREAD&&state.workerRef==MMS_E_THREAD&&state.workerSubscribe==MMS_E_THREAD&&
        state.workerSdkSubscribe==MMS_E_THREAD&&state.workerUnsubscribe==MMS_E_THREAD&&state.workerOutputPreserved);
    CHECK(state.badRef==MMS_E_STALE&&state.badSubscribe==MMS_E_STALE&&state.badQuery==MMS_E_STALE&&state.badOutputPreserved);
    auto reads=state.reads;auto sequence=state.subscriptionState.completedSequence;
    CHECK(probe.burst(12,1)==MMS_OK);state=probe.Read();
    CHECK(state.workerWasWorker&&state.value==12&&state.reads==reads&&state.subscriptionState.latestSequence==sequence+1&&
        state.subscriptionState.completedSequence==sequence&&native_property_input_checks::Caption(field)==L"2");
    state=probe.Complete(manager,++sequence);CHECK(state.reads==++reads&&native_property_input_checks::Caption(field)==L"12");journal.Same();
    CHECK(probe.burst(100,41)==MMS_OK);state=probe.Read();
    CHECK(state.value==140&&state.reads==reads&&state.subscriptionState.latestSequence==sequence+41&&state.subscriptionState.completedSequence==sequence);
    sequence+=41;state=probe.Complete(manager,sequence);CHECK(state.reads==++reads&&state.lastRead==140&&native_property_input_checks::Caption(field)==L"140");
    CHECK(state.traceValues[state.reads-1]==140);journal.Same();
    // This is the plugin's real public ui.invoke command, without the native
    // button adapter's ordinary full Refresh masking the notification result.
    CHECK(probe.invoke()==MMS_OK);state=probe.Read();CHECK(state.commands==1&&state.commandNotify==MMS_OK&&state.value==141&&state.reads==reads);
    state=probe.Complete(manager,++sequence);CHECK(state.reads==++reads&&native_property_input_checks::Caption(field)==L"141");journal.Same();
    CHECK(native_property_input_checks::FindField(native->Toolbar())==field&&manager.Ui().Revision()==registry&&manager.Ui().PreferencesRevision()==preferences);
    Identity(manager,state);ReadOnly(state);
    if(history){HistoryRoundTrip(manager,*history,probe,field);
        // All candidates retaining DLL get/set functions are reclaimed while
        // its code and context are still alive. Reset is not a value signal.
        CHECK(history->Reset()==MMS_OK);history->Collect();CHECK(history->SetPublicationSignal({})==MMS_OK);
        manager.Ui().Refresh().PropertyRefresh().SetHistoryAvailable(false);
    }
    state=probe.Read();const auto stoppedReads=state.reads;Journal stoppedJournal(history.get());
    CHECK(probe.acquireHold()==MMS_OK&&probe.burst(999,1)==MMS_OK&&probe.Read().subscriptionState.pending);
    CHECK(!manager.Stop()&&manager.Status()[0].stage=="stopping"&&GetModuleHandleW(L"property-refresh-probe.dll")==module);
    state=probe.Read();CHECK(state.stops==1&&state.readsAtStop==stoppedReads&&!state.subscription&&state.heldSnapshot&&
        state.stopQuery==MMS_E_STALE&&state.stopNotify==MMS_E_STALE&&state.stopUnsubscribe==MMS_E_STALE&&state.stopRelease==MMS_OK&&state.stopOutputPreserved);
    const auto readLogs=count("PROPERTY_REFRESH_PROBE_READ");
    for(unsigned i=0;i<4;++i){manager.Drain();manager_refresh_checks::Pump(2);}
    CHECK(probe.notify()==MMS_E_STALE&&probe.Read().reads==stoppedReads&&count("PROPERTY_REFRESH_PROBE_READ")==readLogs&&
        count("PROPERTY_REFRESH_PROBE_DESTROY")==0);
    stoppedJournal.Same();
    native.reset();CHECK(probe.releaseHold()==MMS_OK&&manager.Stop()&&manager.Status()[0].stage=="stopped");
    CHECK(count("PROPERTY_REFRESH_PROBE_DESTROY")==1&&GetModuleHandleW(L"property-refresh-probe.dll")==nullptr&&
        manager.Ui().commands.empty()&&manager.Ui().properties.empty()&&manager.Ui().elements.size()==6);
    CHECK(manager.Stop());
}
} // namespace manager_property_refresh_checks
static void ManagerPropertyRefreshChecks(const fs::path& root,const fs::path& library){
    manager_property_refresh_checks::Run(root,library,false);
    manager_property_refresh_checks::Run(root,library,true);
}
