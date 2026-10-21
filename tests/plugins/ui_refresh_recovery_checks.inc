// Include after ui_refresh_dependency_checks.inc. This fixture runs the real
// STATE visibility consumer and queue; only host geometry publication is a
// deterministic fixture. It does not construct the application scene.
namespace ui_refresh_recovery_checks {
struct Fixture : ui_refresh_dependency_checks::Fixture {
    bool armed=false;
    unsigned hides=0;
    MmsResult visibilityDuringHide=MMS_OK;
    MmsUiTargetRefV1 child{},parentRef{};
    static LRESULT CALLBACK Window(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR context){
        auto& self=*reinterpret_cast<Fixture*>(context);
        if(self.armed&&message==WM_SHOWWINDOW&&!wp){
            self.armed=false;++self.hides;
            CHECK(self.registry.InCallback()&&self.queue.ReadOnlyCause(self.client));
            CHECK(self.registry.CheckVisibilityPublication(self.element)==MMS_OK);
            // A native show notification may reenter host layout. Make that
            // real geometry unavailable after the precheck and before the
            // production consumer's PublishVisibility call; do not suspend
            // queue targets or edit its private Target state here.
            self.registry.SuspendGeometry();
            self.visibilityDuringHide=self.registry.CheckVisibilityPublication(self.element);
        }
        return DefSubclassProc(window,message,wp,lp);
    }
    Fixture(){
        auto elementDesc=registry.elements.at("example.refresh.button").desc;
        elementDesc.capabilities|=MMS_UI_LAYOUT;
        CHECK(ui.updateElement(ui.context,element,&elementDesc)==MMS_OK);
        std::map<std::string,UiRegistry::Geometry> geometry;
        for(const auto& entry:registry.elements)if(entry.second.desc.capabilities&MMS_UI_LAYOUT)
            geometry.emplace(entry.first,UiRegistry::Geometry{0,0,100,24,true,false});
        CHECK(registry.PublishGeometry(geometry,registry.Revision()));
        presentations[0].stateVisibility=true;
        queue.SyncTargets(presentations);
        child=Target(element).target;parentRef=Target(group).target;
        CHECK(Target(element).ready&&Target(group).ready&&IsWindowVisible(button));
        CHECK(SetWindowSubclass(button,Window,0x52ec,reinterpret_cast<DWORD_PTR>(this)));
        Edge(group,element);
    }
    ~Fixture(){if(IsWindow(button))RemoveWindowSubclass(button,Window,0x52ec);}
    MmsUiRefreshRequestV1 Combined(uint32_t scope,uint32_t kinds)const{
        MmsUiRefreshRequestV1 request{};request.h=Header<MmsUiRefreshRequestV1>();
        request.target=parentRef;request.scope=scope;request.kinds=kinds;return request;
    }
    MmsHandle QueueVisibilityFailure(){
        CHECK(!hides&&Target(element).ready&&Target(group).ready);
        stateVisible=0;armed=true;
        MmsUiRefreshRequestV1 request{};request.h=Header<MmsUiRefreshRequestV1>();
        request.target=child;request.scope=MMS_UI_REFRESH_SELF;request.kinds=MMS_UI_REFRESH_STATE;
        return Submit(request);
    }
    void Failed(MmsHandle ticket){
        const auto state=State(ticket);
        CHECK(state.status==MMS_UI_REFRESH_FAILED&&state.result==MMS_E_NOT_READY&&!state.completedKinds);
    }
    void OnlyChildSuspended(){
        CHECK(hides==1&&!armed&&visibilityDuringHide==MMS_E_NOT_READY&&!IsWindowVisible(button));
        const auto changed=Target(element),parent=Target(group);
        CHECK(!changed.ready&&parent.ready&&changed.target.instanceGeneration==child.instanceGeneration&&
            parent.target.instanceGeneration==parentRef.instanceGeneration);
        CHECK(Target(second).ready&&Target(nested).ready&&Target(separator).ready&&Target(panelElement).ready);
        CHECK(!invokes&&stateReads==1&&!layouts&&!registry.InCallback()&&!queue.ReadOnlyCause(client));
    }
    void Reject(const MmsUiRefreshRequestV1& request){
        MmsHandle unchanged=0x7ec0;
        CHECK(api.request(api.context,&request,&unchanged)==MMS_E_NOT_READY&&unchanged==0x7ec0);
    }
};
static void ReadyCombinedControl(){
    for(const uint32_t scope:{uint32_t(MMS_UI_REFRESH_SUBTREE),uint32_t(MMS_UI_REFRESH_DEPENDENTS)}){
        Fixture f;const auto revision=f.registry.Revision(),preferences=f.registry.PreferencesRevision();
        const auto request=f.Combined(scope,MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT);
        const auto ticket=f.Submit(request);f.queue.Drain();f.Done(ticket,request.kinds);
        CHECK(f.layouts==1&&!f.stateReads&&!f.hides&&f.Target(f.element).ready&&f.Target(f.group).ready);
        CHECK(f.registry.Revision()==revision&&f.registry.PreferencesRevision()==preferences&&!f.invokes);
    }
}
static void SuspendedMemberRejectsNewCombined(){
    Fixture f;const auto revision=f.registry.Revision(),preferences=f.registry.PreferencesRevision();
    const auto failed=f.QueueVisibilityFailure();f.queue.Drain();f.Failed(failed);f.OnlyChildSuspended();
    for(const uint32_t scope:{uint32_t(MMS_UI_REFRESH_SUBTREE),uint32_t(MMS_UI_REFRESH_DEPENDENTS)})
        for(const uint32_t kinds:{uint32_t(MMS_UI_REFRESH_LAYOUT),uint32_t(MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT)})
            f.Reject(f.Combined(scope,kinds));
    f.OnlyChildSuspended();CHECK(!f.queue.Pending());
    CHECK(f.registry.Revision()==revision&&f.registry.PreferencesRevision()==preferences);
}
static void SuspendedMemberRejectsQueuedCombined(){
    for(const uint32_t scope:{uint32_t(MMS_UI_REFRESH_SUBTREE),uint32_t(MMS_UI_REFRESH_DEPENDENTS)})
        for(const uint32_t kinds:{uint32_t(MMS_UI_REFRESH_LAYOUT),uint32_t(MMS_UI_REFRESH_LAYOUT|MMS_UI_REFRESH_REPAINT)}){
            Fixture f;const auto revision=f.registry.Revision(),preferences=f.registry.PreferencesRevision();
            const auto failed=f.QueueVisibilityFailure();const auto queued=f.Submit(f.Combined(scope,kinds));
            CHECK(f.State(failed).status==MMS_UI_REFRESH_PENDING&&f.State(queued).status==MMS_UI_REFRESH_PENDING&&!f.hides&&!f.layouts);
            // Both contributions enter one real Drain. The earlier STATE
            // suspends only its child after the combined work was accepted.
            f.queue.Drain();f.Failed(failed);f.Failed(queued);f.OnlyChildSuspended();
            CHECK(!f.queue.Pending()&&f.registry.Revision()==revision&&f.registry.PreferencesRevision()==preferences);
        }
}
} // namespace ui_refresh_recovery_checks
static void UiRefreshRecoveryChecks(){
    std::cout<<"UI_REFRESH_RECOVERY ready combined control\n";ui_refresh_recovery_checks::ReadyCombinedControl();
    std::cout<<"UI_REFRESH_RECOVERY suspended child rejects new combined\n";ui_refresh_recovery_checks::SuspendedMemberRejectsNewCombined();
    std::cout<<"UI_REFRESH_RECOVERY suspended child rejects queued combined\n";ui_refresh_recovery_checks::SuspendedMemberRejectsQueuedCombined();
}
