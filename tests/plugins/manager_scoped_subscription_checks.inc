// Real Manager/DLL/NativeUi, with no scene/history or desktop-input claims.
// The accepted-old-cohort concurrency boundary is covered independently by
// the queue tests. This module submits zero explicit refresh.request tickets.
#include "scoped_subscription_probe.h"
namespace manager_scoped_subscription_checks {
struct Probe {
    ScopedProbeBeginFn begin=nullptr;ScopedProbePublishFn publish=nullptr;ScopedProbeGraphFn graph=nullptr;ScopedProbeQueryFn query=nullptr;
    ScopedProbeSimpleFn acquireHold=nullptr,releaseHold=nullptr,removeSeed=nullptr;
    explicit Probe(HMODULE module){CHECK(module);
#define SCOPED_FUNCTION(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(module,name));CHECK(member)
        SCOPED_FUNCTION(begin,"ScopedProbeBegin");SCOPED_FUNCTION(publish,"ScopedProbePublish");SCOPED_FUNCTION(graph,"ScopedProbeGraph");
        SCOPED_FUNCTION(query,"ScopedProbeQuery");SCOPED_FUNCTION(acquireHold,"ScopedProbeAcquireHold");
        SCOPED_FUNCTION(releaseHold,"ScopedProbeReleaseHold");SCOPED_FUNCTION(removeSeed,"ScopedProbeRemoveSeed");
#undef SCOPED_FUNCTION
    }
    ScopedSubscriptionProbeState Read()const{ScopedSubscriptionProbeState value{};value.size=sizeof(value);CHECK(query(&value)==MMS_OK);return value;}
    ScopedSubscriptionProbeState Complete(Manager& manager,uint64_t revision,uint64_t minimumWork=1)const{
        const auto until=GetTickCount64()+3000;auto value=Read();
        const auto done=[&]{const auto& s=value.subscriptionState;return value.query==MMS_OK&&!s.pending&&!s.waitingFor&&s.lastResult==MMS_OK&&
            s.latestRevision==revision&&s.submittedRevision==revision&&s.completedRevision==revision&&s.workGeneration>=minimumWork&&
            s.submittedWorkGeneration==s.workGeneration&&s.completedWorkGeneration==s.workGeneration;};
        while(!done()&&GetTickCount64()<until){manager.Drain();manager_refresh_checks::Pump(2);value=Read();}
        if(!done())std::cerr<<"SCOPED_MANAGER_COMPLETE query="<<value.query<<" desired="<<revision<<" latest="<<value.subscriptionState.latestRevision<<
            " submitted="<<value.subscriptionState.submittedRevision<<" completed="<<value.subscriptionState.completedRevision<<
            " work="<<value.subscriptionState.workGeneration<<" submittedWork="<<value.subscriptionState.submittedWorkGeneration<<
            " completedWork="<<value.subscriptionState.completedWorkGeneration<<" wait="<<value.subscriptionState.waitingFor<<
            " pending="<<value.subscriptionState.pending<<" result="<<value.subscriptionState.lastResult<<'\n';
        CHECK(done());return value;
    }
    ScopedSubscriptionProbeState WaitGraph(Manager& manager)const{
        const auto until=GetTickCount64()+3000;auto value=Read();
        while(value.query==MMS_OK&&!(value.subscriptionState.waitingFor&MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH)&&GetTickCount64()<until){
            manager.Drain();manager_refresh_checks::Pump(2);value=Read();}
        CHECK(value.query==MMS_OK&&(value.subscriptionState.waitingFor&MMS_UI_REFRESH_SUBSCRIPTION_WAIT_GRAPH));return value;
    }
};
static bool Same(const MmsUiTargetRefV1& a,const MmsUiTargetRefV1& b){return a.element==b.element&&a.instanceGeneration==b.instanceGeneration&&a.projectEpoch==b.projectEpoch;}
static unsigned Prepared(const ScopedSubscriptionProbeState& state){unsigned total=0;for(auto count:state.prepared)total+=count;return total;}
static void Negotiation(const ScopedSubscriptionProbeState& state){
    CHECK(state.legacySize==48&&state.legacyMinor==0&&state.newSize==64&&state.newMinor==1);
    CHECK(state.legacyCaps.supportedScopes==MMS_UI_REFRESH_SELF&&
        state.capabilities.supportedScopes==(MMS_UI_REFRESH_SELF|MMS_UI_REFRESH_SUBTREE|MMS_UI_REFRESH_DEPENDENTS));
    CHECK(state.truncatedQuery==MMS_E_ARGUMENT&&state.negotiationPreserved&&state.editQuery==MMS_E_NOT_FOUND&&!state.explicitRequests);
}
static void Identity(Manager& manager,const std::string& id,const ScopedSubscriptionProbeState& state){
    CHECK(state.query==MMS_OK&&state.legacyQuery==MMS_OK&&state.subscriptionState.subscription==state.subscription&&
        state.subscriptionState.binding==state.bindings[0]&&state.subscriptionState.scope==state.scope&&
        state.subscriptionState.kinds==(MMS_UI_REFRESH_DATA|MMS_UI_REFRESH_STATE|MMS_UI_REFRESH_REPAINT));
    CHECK(Same(state.subscriptionState.seed,state.scope==MMS_UI_REFRESH_SELF?state.targets[0]:state.seed));
    CHECK(state.legacyState.subscription==state.subscription&&state.legacyState.completedRevision==state.subscriptionState.completedRevision);
    CHECK(manager.Ui().elements.at(id+".seed").handle==state.seedHandle&&state.seed.element==state.seedHandle);
    for(unsigned i=0;i<4;++i)CHECK(manager.Ui().elements.at(id+".button."+std::to_string(i)).handle==state.buttons[i]&&
        state.targets[i].element==state.buttons[i]&&state.targets[i].instanceGeneration&&state.targets[i].projectEpoch==state.seed.projectEpoch);
    CHECK(state.bindings[0]&&state.bindings[1]&&state.bindings[2]&&state.bindings[3]&&state.sources[0]&&state.sources[1]&&
        state.identity&&state.uiCallbacks&&!state.overflow&&!state.explicitRequests&&!manager.CanInvokeEdit());
}
static void ReadOnly(const ScopedSubscriptionProbeState& state){
    CHECK(state.readOnlyChecks==1&&state.readOnlyDenied&&state.outputsPreserved&&state.readonlyEdge==MMS_E_BUSY&&
        state.readonlyUnedge==MMS_E_BUSY&&state.readonlySubscribe==MMS_E_BUSY&&state.readonlyPublish==MMS_E_BUSY&&
        state.readonlyWorkerPublish==MMS_OK&&state.readonlyWorkerQuery==MMS_OK);
}
static void Phases(const ScopedSubscriptionProbeState& state,unsigned first,uint32_t members){
    CHECK(state.events>first&&state.events<=SCOPED_PROBE_TRACE);bool enteredState=false;uint32_t stateMask=0;
    for(unsigned index=first;index<state.events;++index){const auto& event=state.trace[index];
        CHECK(event.target<4&&(members&(1u<<event.target))&&(event.phase==1||event.phase==2));
        if(event.phase==1)CHECK(!enteredState);else{
            enteredState=true;stateMask|=1u<<event.target;
            CHECK(event.value==state.displayValues[event.target]&&event.revision==state.displayRevisions[event.target]);
        }
    }
    CHECK(enteredState&&stateMask==members);
}
static HWND Button(HWND root,const wchar_t* label){
    HWND found=nullptr;for(auto child=GetWindow(root,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
        if(native_property_input_checks::Caption(child)==label){CHECK(!found);found=child;}
    CHECK(found&&IsWindowVisible(found));return found;
}
static void Run(const fs::path& root,const fs::path& library,uint32_t scope,bool invalidateSeed=false){
    const std::string suffix=invalidateSeed?"seed-retire":scope==MMS_UI_REFRESH_SUBTREE?"subtree":scope==MMS_UI_REFRESH_SELF?"self":"dependents";
    const std::string id="org.example.scoped-"+suffix;const auto directory=root/("scoped-manager-"+suffix),package=directory/"PLUGINS"/"fixture";
    fs::create_directories(package);fs::copy_file(library,package/"scoped-subscription-probe.dll");Write(package/"plugin.json",ManifestText(id,"scoped-subscription-probe.dll"));
    std::vector<std::string> logs;const auto log=[&](const std::string& message){logs.push_back(message);};
    const auto count=[&](const char* marker){return std::count_if(logs.begin(),logs.end(),[&](const auto& message){return message.find(marker)!=std::string::npos;});};
    Manager manager(directory/"user",log);manager.Load(package.parent_path());const auto status=manager.Status();
    if(status.size()!=1||status[0].stage!="started"){
        for(const auto& entry:status)std::cerr<<"SCOPED_MANAGER_START "<<entry.id<<" "<<entry.stage<<" "<<entry.error<<'\n';
        for(const auto& entry:logs)std::cerr<<"SCOPED_MANAGER_LOG "<<entry<<'\n';
    }
    CHECK(status.size()==1&&status[0].stage=="started"&&count("SCOPED_PROBE_START")==1);
    const auto module=GetModuleHandleW(L"scoped-subscription-probe.dll");Probe probe(module);Negotiation(probe.Read());
    manager_refresh_checks::Window window;CHECK(SetWindowPos(window.value,nullptr,0,0,900,620,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
    auto native=std::make_unique<NativeUi>(manager.Ui(),window.value,log);native->SetPropertyArea(520,120,true);
    native->Refresh();CHECK(native->Layout(10,800,160,true)>0);
    const auto aParent=scope==MMS_UI_REFRESH_SUBTREE?HWND(nullptr):native->Toolbar();
    if(aParent){const auto a=Button(aParent,L"Scoped A"),b=Button(native->PropertyArea(),L"Scoped B");CHECK(GetParent(a)!=GetParent(b));}
    const auto registry=manager.Ui().Revision(),preferences=manager.Ui().PreferencesRevision();
    const uint32_t initialEdges=scope==MMS_UI_REFRESH_DEPENDENTS?0u:1u;
    CHECK(probe.begin(scope,initialEdges)==MMS_OK&&probe.publish(1,500,1)==MMS_OK&&probe.publish(0,10,1)==MMS_OK);
    auto state=probe.Read();CHECK(!Prepared(state)&&!state.explicitRequests&&state.sourceRevisions[0]==1&&state.sourceRevisions[1]==1);
    if(scope==MMS_UI_REFRESH_DEPENDENTS){state=probe.WaitGraph(manager);CHECK(!Prepared(state)&&!state.states[0]&&!state.states[1]&&!state.states[2]&&!state.states[3]);
        const auto work=state.subscriptionState.workGeneration;CHECK(probe.graph(3)==MMS_OK);state=probe.Complete(manager,1,work+1);
        CHECK(state.prepared[1]==1&&state.prepared[2]==1&&state.preparedValues[1]==10&&state.preparedValues[2]==500&&
            state.preparedRevisions[1]==1&&state.preparedRevisions[2]==1&&!state.prepared[0]&&!state.prepared[3]);Phases(state,0,(1u<<1)|(1u<<2));
    }else{state=probe.Complete(manager,1);const auto member=scope==MMS_UI_REFRESH_SELF?0u:1u;
        CHECK(state.prepared[member]==1&&state.preparedValues[member]==10&&state.preparedRevisions[member]==1);Phases(state,0,1u<<member);
        for(unsigned i=0;i<4;++i)if(i!=member)CHECK(!state.prepared[i]&&!state.states[i]);
    }
    Identity(manager,id,state);ReadOnly(state);
    if(scope==MMS_UI_REFRESH_DEPENDENTS){
        auto events=state.events;auto work=state.subscriptionState.workGeneration;const auto preparedA=state.prepared[1],preparedB=state.prepared[2];
        CHECK(probe.publish(1,700,1)==MMS_OK&&probe.publish(0,100,41)==MMS_OK);const auto burst=probe.Read();
        CHECK(burst.workerWasWorker&&burst.sourceRevisions[0]==42&&burst.sourceRevisions[1]==2&&burst.prepared[1]==preparedA&&burst.prepared[2]==preparedB);
        state=probe.Complete(manager,42);CHECK(state.prepared[1]==preparedA+1&&state.prepared[2]==preparedB+1&&
            state.displayValues[1]==140&&state.displayValues[2]==700&&state.displayRevisions[1]==42&&state.displayRevisions[2]==2);
        Phases(state,events,(1u<<1)|(1u<<2));
        events=state.events;work=state.subscriptionState.workGeneration;const auto dependency=state.subscriptionState.submittedDependencyRevision;
        CHECK(probe.graph(7)==MMS_OK);state=probe.Complete(manager,42,work+1);
        CHECK(state.prepared[3]==1&&state.displayValues[3]==700&&state.displayRevisions[3]==2&&
            state.subscriptionState.submittedDependencyRevision>dependency);Phases(state,events,(1u<<1)|(1u<<2)|(1u<<3));
        events=state.events;work=state.subscriptionState.workGeneration;const auto excludedData=state.prepared[2],excludedState=state.states[2];
        CHECK(probe.graph(5)==MMS_OK);state=probe.Complete(manager,42,work+1);
        CHECK(state.prepared[2]==excludedData&&state.states[2]==excludedState);Phases(state,events,(1u<<1)|(1u<<3));
        events=state.events;CHECK(probe.publish(1,800,1)==MMS_OK&&probe.publish(0,200,1)==MMS_OK);state=probe.Complete(manager,43);
        CHECK(state.displayValues[1]==200&&state.displayRevisions[1]==43&&state.displayValues[3]==800&&state.displayRevisions[3]==3&&
            state.prepared[2]==excludedData&&state.states[2]==excludedState&&state.displayValues[2]==700);
        Phases(state,events,(1u<<1)|(1u<<3));CHECK(!state.prepared[0]&&!state.states[0]&&state.displayRevisions[0]==0);
        if(invalidateSeed){const auto before=Prepared(state);CHECK(probe.removeSeed()==MMS_OK);state=probe.Read();
            CHECK(state.query==MMS_E_STALE&&state.legacyQuery==MMS_E_STALE);CHECK(probe.publish(0,201,1)==MMS_OK);
            manager.Drain();manager_refresh_checks::Pump(10);CHECK(probe.Read().query==MMS_E_STALE&&Prepared(probe.Read())==before);
        }
    }
    CHECK(manager.Ui().PreferencesRevision()==preferences&&(!invalidateSeed?manager.Ui().Revision()==registry:manager.Ui().Revision()>registry));
    const auto before=Prepared(probe.Read());CHECK(probe.acquireHold()==MMS_OK);
    if(!invalidateSeed)CHECK(probe.publish(0,300,1)==MMS_OK&&probe.Read().subscriptionState.pending);
    CHECK(!manager.Stop()&&manager.Status()[0].stage=="stopping"&&GetModuleHandleW(L"scoped-subscription-probe.dll")==module);
    state=probe.Read();CHECK(state.stops==1&&state.preparesAtStop==before&&state.stopQuery==MMS_E_STALE&&state.stopUnsubscribe==MMS_E_STALE&&
        state.stopOutputPreserved&&!state.subscription&&state.heldSnapshot);
    const auto preparedLogs=count("SCOPED_PROBE_PREPARE");for(unsigned i=0;i<4;++i){manager.Drain();manager_refresh_checks::Pump(2);}
    CHECK(Prepared(probe.Read())==before&&count("SCOPED_PROBE_PREPARE")==preparedLogs&&count("SCOPED_PROBE_DESTROY")==0);
    native.reset();CHECK(probe.releaseHold()==MMS_OK&&manager.Stop()&&GetModuleHandleW(L"scoped-subscription-probe.dll")==nullptr);
    CHECK(count("SCOPED_PROBE_DESTROY")==1&&manager.Ui().commands.empty()&&manager.Ui().elements.size()==6&&manager.Stop());
}
} // namespace manager_scoped_subscription_checks
static void ManagerScopedSubscriptionChecks(const fs::path& root,const fs::path& library){
    manager_scoped_subscription_checks::Run(root,library,MMS_UI_REFRESH_DEPENDENTS);
    manager_scoped_subscription_checks::Run(root,library,MMS_UI_REFRESH_SUBTREE);
    manager_scoped_subscription_checks::Run(root,library,MMS_UI_REFRESH_SELF);
    manager_scoped_subscription_checks::Run(root,library,MMS_UI_REFRESH_DEPENDENTS,true);
}
