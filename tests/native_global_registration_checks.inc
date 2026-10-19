// Private global registration, included by native_edit_history_tests.cpp.
#include "../src/plugins/native_global_registration.hpp"
namespace mikumikustudio {
void RegisterCameraState(MMDApp*,int);
void RegisterLightState(MMDApp*,int);
void RegisterSelfShadowState(MMDApp*,int);
void ClearGlobalFrameSelection(MMDApp*);
void ReloadModels(MMDApp*);
void RefreshLightPanel(MMDApp*);
void RefreshSelfShadowPanel(MMDApp*);
}
namespace {
struct GlobalRegistrationFixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    plugins::StorageStore storage;plugins::SharedHistory history{storage};
    GlobalRegistrationFixture(){
        ::new(static_cast<void*>(&app->state)) MMDAppState{};app->EnglishUI()=2;app->CameraMode()=1;
        app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(4);
        app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(4);
        app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(4);
        app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(3);
        for(unsigned slot=0;slot<255;++slot){app->AccessoryKeys(slot)=mdl::KeyPoolCreate<mdl::AccessoryKey>(3);app->AccessoryKeys(slot)[slot%3].selected=std::uint8_t(slot%7+1);}
        for(unsigned i=0;i<3;++i){
            auto& camera=app->CameraKeys()[i];camera.frame=i==0?0:i==1?12:31;camera.previous=i?i-1:0;camera.next=i<2?i+1:0;
            camera.distance=-float(31+i*7);camera.fov=45+int(i)*9;camera.parentModel=-1;camera.perspective=std::uint8_t(i%2);
            std::memset(camera.interpolation,20,12);std::memset(reinterpret_cast<unsigned char*>(camera.interpolation)+12,107,12);
            camera.interpolation[0][0]=3;camera.interpolation[1][0]=81;camera.interpolation[2][0]=91;camera.interpolation[3][0]=119;
            for(unsigned axis=0;axis<3;++axis){camera.eye[axis]=float(i*7+axis)/3.f;camera.target[axis]=float(i+axis+1)*.1234567f;}
            auto& light=app->LightKeys()[i];light.frame=camera.frame;light.previous=camera.previous;light.next=camera.next;
            for(unsigned axis=0;axis<3;++axis){light.direction[axis]=float(i+axis+1)*.137918f;light.color[axis]=float(i+axis+1)*.109173f;}
            auto& shadow=app->ShadowKeys()[i];shadow.frame=camera.frame;shadow.previous=camera.previous;shadow.next=camera.next;
            shadow.mode=std::uint8_t(i);shadow.distance=.02f+float(i)*.03f;
        }
        app->CameraKeys()[3].parentModel=-1;app->CameraKeys()[3].reserved2[1]=17;
        app->LightKeys()[3].reserved1[2]=23;app->ShadowKeys()[3].reserved0[1]=31;
        app->CameraKeys()[2].selected=3;app->LightKeys()[1].selected=4;app->ShadowKeys()[0].selected=5;app->GravityKeys()[2].selected=6;
        app->CameraParentModel()=-1;app->CameraParentBone()=0;app->CameraPerspective()=1;app->CameraFov()=67.75f;app->CameraDistance()=-72.5f;
        for(unsigned i=0;i<3;++i){app->CameraPositionAxis(i)=4.125f+float(i);app->CameraRotation()[i]=.24367f+float(i)*.1f;
            app->LightDirection()[i]=-.21737f+float(i)*.2119f;app->LightColor()[i]=.71379f-float(i)*.125f;}
        app->ShadowMode()=2;app->ShadowDistance()=.075f;app->CurrentFrame()=7;app->LastRegisteredFrame()=9;
        app->CameraTrackCursor()=2;app->LightTrackCursor()=1;app->ShadowTrackCursor()=2;
        app->CameraTrackActive()=app->LightTrackActive()=app->ShadowTrackActive()=1;
    }
    ~GlobalRegistrationFixture(){
        history.Reset();history.Collect();curve_host::Reset(app.get());
        mdl::KeyPoolDestroy(app->CameraKeys());mdl::KeyPoolDestroy(app->LightKeys());mdl::KeyPoolDestroy(app->ShadowKeys());mdl::KeyPoolDestroy(app->GravityKeys());
        for(unsigned i=0;i<255;++i)mdl::KeyPoolDestroy(app->AccessoryKeys(i));
    }
    std::uint64_t Digest() const {
        std::uint64_t value=14695981039346656037ull;
        const auto add=[&](const void* p,std::size_t size){const auto* bytes=static_cast<const unsigned char*>(p);while(size--){value^=*bytes++;value*=1099511628211ull;}};
        add(&app->state,sizeof(app->state));add(&app->SceneLight(),sizeof(D3DLIGHT9));
        const auto keys=[&](const auto* p){add(p,mdl::KeyPoolCapacity(p)*sizeof(*p));};
        keys(app->CameraKeys());keys(app->LightKeys());keys(app->ShadowKeys());keys(app->GravityKeys());
        for(unsigned i=0;i<255;++i)keys(app->AccessoryKeys(i));return value;
    }
};
std::shared_ptr<plugins::HistoryChange> GlobalRegistrationPlan(GlobalRegistrationFixture& f,unsigned kind,int frame){
    std::shared_ptr<plugins::HistoryChange> result;
    const auto status=kind==0?plugins::PlanCameraRegistration(*f.app,frame,false,result):kind==1?plugins::PlanLightRegistration(*f.app,frame,result):
        plugins::PlanShadowRegistration(*f.app,frame,f.app->ShadowDistance(),result);
    Check(status==MMS_OK&&result,"global registration private plan exists");return result;
}
void GlobalRegistrationCommit(GlobalRegistrationFixture& f,const std::shared_ptr<plugins::HistoryChange>& change){
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("global registration",change,transaction)==MMS_OK,"global journal prepares");
    failAfter=0;const auto result=f.history.CommitNative(std::move(transaction));failAfter=-1;
    Check(result==MMS_OK,"global key pose curve selection publication performs no allocation");
}
template<class K> std::vector<K> GlobalRegistrationKeys(K* keys){return {keys,keys+mdl::KeyPoolCapacity(keys)};}
template<class K> bool GlobalRegistrationSame(K* keys,const std::vector<K>& expected){return mdl::KeyPoolCapacity(keys)>=expected.size()&&
    !std::memcmp(keys,expected.data(),expected.size()*sizeof(K));}
template<class K> void GlobalRegistrationFillTail(K* keys){keys[2].next=3;keys[3]=keys[2];keys[3].frame=52;keys[3].previous=2;keys[3].next=0;}
void GlobalRegistrationMarkOracle(GlobalRegistrationFixture& f){ClearGlobalFrameSelection(f.app.get());}
void GlobalRegistrationOracle(GlobalRegistrationFixture& f,unsigned kind,int frame){
    GlobalRegistrationMarkOracle(f);
    if(kind==0)RegisterCameraState(f.app.get(),frame);else if(kind==1)RegisterLightState(f.app.get(),frame);else RegisterSelfShadowState(f.app.get(),frame);
}
void GlobalRegistrationParity(){
    for(unsigned kind=0;kind<3;++kind)for(int frame:{12,20,80}){
        GlobalRegistrationFixture f,oracle;
        if(frame==80){GlobalRegistrationFillTail(f.app->CameraKeys());GlobalRegistrationFillTail(oracle.app->CameraKeys());
            GlobalRegistrationFillTail(f.app->LightKeys());GlobalRegistrationFillTail(oracle.app->LightKeys());
            GlobalRegistrationFillTail(f.app->ShadowKeys());GlobalRegistrationFillTail(oracle.app->ShadowKeys());}
        const auto camera=GlobalRegistrationKeys(f.app->CameraKeys());const auto light=GlobalRegistrationKeys(f.app->LightKeys());
        const auto shadow=GlobalRegistrationKeys(f.app->ShadowKeys());
        const auto identity=kind==0?mdl::KeyPoolIdentity(f.app->CameraKeys()):kind==1?mdl::KeyPoolIdentity(f.app->LightKeys()):mdl::KeyPoolIdentity(f.app->ShadowKeys());
        const auto before=f.Digest();auto change=GlobalRegistrationPlan(f,kind,frame);
        Check(f.Digest()==before,"global planning preserves live keys poses selection and scalars");
        GlobalRegistrationCommit(f,change);GlobalRegistrationOracle(oracle,kind,frame);
        Check(GlobalRegistrationSame(f.app->CameraKeys(),GlobalRegistrationKeys(oracle.app->CameraKeys()))&&
            GlobalRegistrationSame(f.app->LightKeys(),GlobalRegistrationKeys(oracle.app->LightKeys()))&&
            GlobalRegistrationSame(f.app->ShadowKeys(),GlobalRegistrationKeys(oracle.app->ShadowKeys())),"global insert overwrite append growth match real native registrars");
        Check(f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame()&&f.app->CurrentFrame()==7&&
            f.app->CameraPositionX()==oracle.app->CameraPositionX()&&f.app->CameraFov()==oracle.app->CameraFov()&&
            f.app->LightColor()[0]==oracle.app->LightColor()[0]&&f.app->ShadowDistance()==oracle.app->ShadowDistance(),"first global publication never seeks or changes current authored poses");
        Check(identity==(kind==0?mdl::KeyPoolIdentity(f.app->CameraKeys()):kind==1?mdl::KeyPoolIdentity(f.app->LightKeys()):mdl::KeyPoolIdentity(f.app->ShadowKeys())),"global growth retains pool identity");
        for(unsigned i=0;i<255;++i)for(unsigned j=0;j<3;++j)Check(!f.app->AccessoryKeys(i)[j].selected,"global registration clears every accessory selection");
        const auto afterCamera=GlobalRegistrationKeys(f.app->CameraKeys());const auto afterLight=GlobalRegistrationKeys(f.app->LightKeys());
        const auto afterShadow=GlobalRegistrationKeys(f.app->ShadowKeys());
        for(int playhead:{0,5,12,20,31,90}){
            f.app->CurrentFrame()=oracle.app->CurrentFrame()=playhead;
            f.app->CameraMode()=0;f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Gravity);
            f.app->AccessoryKeys(254)[2].selected=11;f.app->GravityKeys()[1].selected=13;
            Check(f.history.History(false)==MMS_OK,"global registration undo at another playhead");
            Check(GlobalRegistrationSame(f.app->CameraKeys(),camera)&&GlobalRegistrationSame(f.app->LightKeys(),light)&&GlobalRegistrationSame(f.app->ShadowKeys(),shadow),"undo restores original native keys and every global mark");
            for(unsigned i=0;i<255;++i)for(unsigned j=0;j<3;++j)Check(f.app->AccessoryKeys(i)[j].selected==(j==i%3?i%7+1:0),"undo restores complete accessory marks after new user selection");
            std::memcpy(oracle.app->CameraKeys(),camera.data(),camera.size()*sizeof(mdl::CameraKey));
            std::memcpy(oracle.app->LightKeys(),light.data(),light.size()*sizeof(mdl::LightKey));
            std::memcpy(oracle.app->ShadowKeys(),shadow.data(),shadow.size()*sizeof(mdl::SelfShadowKey));
            if(kind==0)ReloadModels(oracle.app.get());else if(kind==1)RefreshLightPanel(oracle.app.get());else RefreshSelfShadowPanel(oracle.app.get());
            if(kind==0)Check(!std::memcmp(f.app->CameraRotation(),oracle.app->CameraRotation(),3*sizeof(float))&&f.app->CameraPositionX()==oracle.app->CameraPositionX()&&f.app->CameraFov()==oracle.app->CameraFov(),"native camera undo agrees with real seek including exact frame and Bezier");
            if(kind==1)Check(!std::memcmp(f.app->LightDirection(),oracle.app->LightDirection(),3*sizeof(float))&&!std::memcmp(f.app->LightColor(),oracle.app->LightColor(),3*sizeof(float))&&
                f.app->SceneLight().Direction.x==f.app->LightDirection()[0]&&f.app->SceneLight().Specular.r==f.app->LightColor()[0],"light undo matches actual refresh arithmetic and CPU device-light mirror");
            if(kind==2)Check(f.app->ShadowMode()==oracle.app->ShadowMode()&&f.app->ShadowDistance()==oracle.app->ShadowDistance(),"shadow undo matches actual step evaluation");
            f.app->AccessoryKeys(254)[1].selected=17;
            Check(f.history.History(true)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),afterCamera)&&GlobalRegistrationSame(f.app->LightKeys(),afterLight)&&GlobalRegistrationSame(f.app->ShadowKeys(),afterShadow),"redo restores registered global track after accepted growth");
            Check(!f.app->AccessoryKeys(254)[1].selected&&f.app->CurrentFrame()==playhead&&!f.app->CameraMode()&&f.app->GlobalTrackSelected(GlobalTimelineTrack::Gravity),"global redo clears key marks but preserves playhead and current view selection");
        }
    }
}
void GlobalRegistrationRootAndNoop(){
    for(unsigned kind=0;kind<3;++kind){GlobalRegistrationFixture f;
        auto change=GlobalRegistrationPlan(f,kind,0);GlobalRegistrationCommit(f,change);
        if(kind==0)Check(f.app->CameraKeys()[0].frame==0&&f.app->CameraKeys()[0].next==1&&f.app->CameraKeys()[1].previous==0&&f.app->CameraKeys()[3].frame==0&&f.app->CameraKeys()[0].fov==67,"camera frame-zero overwrites root instead of legacy non-head-zero insertion bug");
        if(kind==1)Check(f.app->LightKeys()[0].next==1&&f.app->LightKeys()[1].previous==0&&f.app->LightKeys()[3].frame==0&&f.app->LightKeys()[0].color[0]==f.app->LightColor()[0],"light frame-zero overwrites root while preserving successor");
        Check(f.app->LastRegisteredFrame()==9,"root overwrite preserves native last-frame metadata");
        Check(f.history.History(false)==MMS_OK,"undo root registration");
        f.app->CurrentFrame()=0;
        if(kind==0)ReloadModels(f.app.get());else if(kind==1)RefreshLightPanel(f.app.get());else RefreshSelfShadowPanel(f.app.get());
        f.app->CameraKeys()[0].selected=23;f.app->AccessoryKeys(254)[1].selected=25;
        const auto before=f.Digest();const auto redo=f.history.RedoCount();std::shared_ptr<plugins::HistoryChange> empty;
        const auto status=kind==0?plugins::PlanCameraRegistration(*f.app,0,false,empty):kind==1?plugins::PlanLightRegistration(*f.app,0,empty):plugins::PlanShadowRegistration(*f.app,0,f.app->ShadowDistance(),empty);
        Check(status==MMS_OK&&!empty&&f.Digest()==before&&f.history.RedoCount()==redo,"same global author data preserves redo and selections");
    }
    GlobalRegistrationFixture f;
    std::shared_ptr<plugins::HistoryChange> reset;
    Check(plugins::PlanCameraRegistration(*f.app,12,true,reset)==MMS_OK&&reset,"camera reset interpolation is a pure input");
    GlobalRegistrationCommit(f,reset);for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<6;++lane)
        Check(f.app->CameraKeys()[1].interpolation[row][lane]==(row<2?20:107),"reset interpolation replaces controls at exact key");
    GlobalRegistrationFixture shadow;
    const auto before=shadow.app->ShadowDistance();std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanShadowRegistration(*shadow.app,18,.04251f,change)==MMS_OK&&shadow.app->ShadowDistance()==before,"shadow slider input is staged without changing live distance");
    GlobalRegistrationCommit(shadow,change);Check(shadow.app->ShadowDistance()==.04251f&&shadow.app->ShadowKeys()[3].distance==.04251f,"shadow input pose and key publish together");
}
void GlobalRegistrationCurves(){
    for(unsigned kind=0;kind<3;++kind){GlobalRegistrationFixture f,oracle;
        const auto track=kind==0?curve_host::Kind::Camera:kind==1?curve_host::Kind::Light:curve_host::Kind::Shadow;
        std::vector<curve_editor::Channel> channels;std::wstring error;
        Check(curve_host::Read(*f.app,track,0,channels,error),"read global native channels for authored fixture");
        const unsigned lane=kind==2?1:0;auto& keys=channels[lane].track.keys;
        keys[0].outWeight=.19;keys[0].outSlope=kind==2?.001:.173;
        keys[1].inWeight=.31;keys[1].inSlope=kind==2?-.001:-.113;keys[1].interpolation=animation_curve::Interpolation::Bezier;
        Check(curve_host::Write(*f.app,track,0,channels,error)&&curve_host::Write(*oracle.app,track,0,channels,error),"author nonnative global tangent fixture");
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=7;
        const auto initial=f.Digest();auto change=GlobalRegistrationPlan(f,kind,20);
        Check(initial==f.Digest(),"global authoring capture never synchronizes or mutates live state");
        GlobalRegistrationCommit(f,change);GlobalRegistrationOracle(oracle,kind,20);
        std::vector<curve_editor::Channel> after;
        Check(curve_host::Read(*f.app,track,0,after,error),"ordinary read reconciles the registered native track");
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=5;
        Check(f.history.History(false)==MMS_OK,"undo survives ordinary authoring reconciliation");
        std::vector<curve_editor::Channel> restored;Check(curve_host::Read(*f.app,track,0,restored,error),"read restored pristine global authoring");
        Check(restored[lane].track.keys[0].outWeight==.19&&restored[lane].track.keys[0].outSlope==keys[0].outSlope&&
            restored[lane].track.keys[1].inWeight==.31,"undo restores original custom global handles rather than reconciled replacements");
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=23;
        Check(f.history.History(true)==MMS_OK,"redo derives custom authoring against target registered keys");
        if(kind==0)ReloadModels(oracle.app.get());else if(kind==1)RefreshLightPanel(oracle.app.get());else RefreshSelfShadowPanel(oracle.app.get());
        Check(kind==0?std::fabs(f.app->CameraPositionX()-oracle.app->CameraPositionX())<1.e-5f:
              kind==1?std::fabs(f.app->LightDirection()[0]-oracle.app->LightDirection()[0])<1.e-6f:
                      std::fabs(f.app->ShadowDistance()-oracle.app->ShadowDistance())<1.e-7f,"target authored sampling agrees with native registration plus existing curve application");
    }
}
void GlobalRegistrationFailures(){
    for(unsigned kind=0;kind<3;++kind){GlobalRegistrationFixture f;
        GlobalRegistrationFillTail(f.app->CameraKeys());GlobalRegistrationFillTail(f.app->LightKeys());GlobalRegistrationFillTail(f.app->ShadowKeys());
        const auto before=f.Digest();std::shared_ptr<plugins::HistoryChange> change;bool success=false;
        for(long limit=0;limit<1024;++limit){failAfter=limit;
            const auto status=kind==0?plugins::PlanCameraRegistration(*f.app,80,false,change):kind==1?plugins::PlanLightRegistration(*f.app,80,change):plugins::PlanShadowRegistration(*f.app,80,.04f,change);
            failAfter=-1;Check(f.Digest()==before,"global planning OOM preserves all scene data");
            if(status==MMS_OK){success=bool(change);break;}Check(status==MMS_E_MEMORY&&!change,"global planning reports allocation failure");}
        Check(success,"global planning failure sweep reaches success");success=false;
        for(long limit=0;limit<2048;++limit){std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=limit;
            const auto status=change->Prepare(true,candidate);failAfter=-1;Check(f.Digest()==before,"global candidate OOM and cancellation leave live scene untouched");
            if(status==MMS_OK){Check(candidate&&candidate->IsCurrent(),"complete private global candidate is current");failAfter=0;candidate.reset();failAfter=-1;success=true;break;}
            Check(status==MMS_E_MEMORY&&!candidate,"global candidate reports allocation failure");}
        Check(success,"global candidate failure sweep reaches selection authoring and private growth");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        plugins::SharedHistory tiny(f.storage,{},16,1);
        Check(tiny.PrepareNative("global budget",change,tx)==MMS_E_MEMORY&&!tx&&f.Digest()==before,"global retained payload cannot bypass history budget");
        GlobalRegistrationCommit(f,change);Check(f.history.History(false)==MMS_OK,"global fault fixture retains redo");
        const auto redo=f.history.RedoCount(),undo=f.history.UndoCount();const auto stable=f.Digest();
        failAfter=0;const auto status=f.history.History(true);failAfter=-1;
        Check(status==MMS_E_MEMORY&&f.Digest()==stable&&f.history.RedoCount()==redo&&f.history.UndoCount()==undo,"redo allocation failure preserves cursor and complete scene");
    }
    for(unsigned mutation=0;mutation<5;++mutation){GlobalRegistrationFixture f;auto change=GlobalRegistrationPlan(f,0,20);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> candidate;
        Check(f.history.PrepareNative("stale global",change,candidate)==MMS_OK,"prepare stale global fixture");
        switch(mutation){case 0:f.app->CameraPositionX()+=1;break;case 1:f.app->AccessoryKeys(254)[2].selected=99;break;
        case 2:++f.app->LastRegisteredFrame();break;case 3:++f.app->CurrentFrame();break;default:f.app->CameraKeys()[1].eye[1]+=1;break;}
        const auto before=f.Digest();Check(f.history.CommitNative(std::move(candidate))==MMS_E_STALE&&f.Digest()==before&&!f.history.UndoCount(),"stale global pose marks scalar playhead or native dependency reject atomically");
    }
    GlobalRegistrationFixture f;std::shared_ptr<plugins::HistoryChange> out;
    f.app->CameraKeys()[1].next=1;
    Check(plugins::PlanCameraRegistration(*f.app,20,false,out)==MMS_E_ARGUMENT&&!out,"malformed global key chain rejects");
    Check(plugins::PlanLightRegistration(*f.app,-1,out)==MMS_E_ARGUMENT&&!out,"negative global registration frame rejects");
    Check(plugins::PlanShadowRegistration(*f.app,1,std::numeric_limits<float>::infinity(),out)==MMS_E_ARGUMENT&&!out,"nonfinite shadow input rejects");
}
void GlobalRegistrationLaterGrowthAndParents(){
    for(unsigned kind=0;kind<3;++kind){
        GlobalRegistrationFixture f;
        GlobalRegistrationFillTail(f.app->CameraKeys());GlobalRegistrationFillTail(f.app->LightKeys());GlobalRegistrationFillTail(f.app->ShadowKeys());
        auto change=GlobalRegistrationPlan(f,kind,80);GlobalRegistrationCommit(f,change);
        Check(mdl::KeyPoolEnsure(f.app->CameraKeys(),16)&&mdl::KeyPoolEnsure(f.app->LightKeys(),16)&&
            mdl::KeyPoolEnsure(f.app->ShadowKeys(),16)&&mdl::KeyPoolEnsure(f.app->GravityKeys(),16)&&
            mdl::KeyPoolEnsure(f.app->AccessoryKeys(254),16),"later global and accessory pool growth preserves strong identity");
        f.app->CameraKeys()[15].selected=21;f.app->LightKeys()[15].selected=22;f.app->ShadowKeys()[15].selected=23;
        f.app->GravityKeys()[15].selected=24;f.app->AccessoryKeys(254)[15].selected=25;
        for(bool forward:{false,true}){
            Check(f.history.History(forward)==MMS_OK,"older global history survives later pool growth");
            Check(f.app->CameraKeys()[15].selected==21&&f.app->LightKeys()[15].selected==22&&f.app->ShadowKeys()[15].selected==23&&
                f.app->GravityKeys()[15].selected==24&&f.app->AccessoryKeys(254)[15].selected==25,"older global selection does not write later-grown unrelated tails");
            const auto mark=kind==0?f.app->CameraKeys()[4].selected:kind==1?f.app->LightKeys()[4].selected:f.app->ShadowKeys()[4].selected;
            Check(mark==unsigned(forward),"own newly grown registration slot clears on undo and selects on redo");
        }
    }
    {
        Fixture parent,replacement;GlobalRegistrationFixture f;f.app->ModelSlot(2)=parent.raw();
        f.app->CameraParentModel()=2;f.app->CameraParentBone()=1;
        auto change=GlobalRegistrationPlan(f,0,20);const auto originalId=plugins::SceneObjectIdentity(parent.raw()).id;
        f.app->ModelSlot(2)=replacement.raw();std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        const auto before=f.Digest();Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&f.Digest()==before,
            "planned camera parent cannot silently bind a replacement model in the same slot");
        f.app->ModelSlot(2)=parent.raw();GlobalRegistrationCommit(f,change);f.app->CurrentFrame()=20;
        Check(f.history.History(false)==MMS_OK&&f.app->CameraParentModel()==-1,"camera undo samples original unbound target at current playhead");
        Check(f.history.History(true)==MMS_OK&&f.app->CameraParentModel()==2&&f.app->CameraParentBone()==1&&
            plugins::SceneObjectPresent(parent.raw(),originalId),"camera redo restores target parent bone while retaining model identity");
        f.app->ModelSlot(2)=replacement.raw();const auto changed=f.Digest();const auto undo=f.history.UndoCount();
        Check(f.history.History(false)==MMS_E_STALE&&f.Digest()==changed&&f.history.UndoCount()==undo,
            "committed camera record rejects replacement parent without consuming history");
        f.app->ModelSlot(2)=parent.raw();change.reset();f.history.Reset();f.history.Collect();
        plugins::ForgetSceneObject(parent.raw());plugins::ForgetSceneObject(replacement.raw());f.app->ModelSlot(2)=nullptr;
    }
    {
        Fixture parent;GlobalRegistrationFixture f;f.app->ModelSlot(1)=parent.raw();
        f.app->CameraKeys()[1].parentModel=1;f.app->CameraKeys()[1].parentBone=0;
        auto change=GlobalRegistrationPlan(f,0,20);
        plugins::ForgetSceneObject(parent.raw());plugins::SceneObjectIdentity(parent.raw());
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate,"camera original chain parent rejects recycled object address identity");
        change.reset();plugins::ForgetSceneObject(parent.raw());f.app->ModelSlot(1)=nullptr;
    }
}
void GlobalRegistrationFiniteSampling(){
    for(bool color:{false,true}){
        GlobalRegistrationFixture f;
        auto* keys=f.app->LightKeys();const float extreme=(std::numeric_limits<float>::max)();
        (color?keys[0].color:keys[0].direction)[0]=-extreme;
        (color?keys[1].color:keys[1].direction)[0]=extreme;
        auto change=GlobalRegistrationPlan(f,1,20);GlobalRegistrationCommit(f,change);
        // Every stored endpoint is finite. The native float subtraction at a
        // middle frame nevertheless overflows before the double multiply.
        f.app->CurrentFrame()=6;
        const auto before=f.Digest();const auto undo=f.history.UndoCount(),redo=f.history.RedoCount();
        Check(f.history.History(false)==MMS_E_ARGUMENT,
            "nonfinite sampled light direction or color rejects undo before publication");
        Check(f.Digest()==before&&f.history.UndoCount()==undo&&f.history.RedoCount()==redo,
            "failed light sampling preserves native keys selections CPU light and history position");
        f.app->CurrentFrame()=0;
        Check(f.history.History(false)==MMS_OK,
            "light undo remains available at a finite exact-key sample after rejected interpolation");
        Check((color?f.app->LightColor():f.app->LightDirection())[0]==-extreme,
            "finite native exact-key light value remains representable");
        Check(f.history.History(true)==MMS_OK,
            "light redo remains available after a safe exact-key undo");
    }
    for(unsigned kind=0;kind<3;++kind){
        GlobalRegistrationFixture f;auto change=GlobalRegistrationPlan(f,kind,20);
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate,
            "prepare global candidate for allocation-free invalid-pose guard");
        const float invalid=std::numeric_limits<float>::quiet_NaN();
        if(kind==0)f.app->CameraPositionX()=invalid;
        else if(kind==1)f.app->LightDirection()[0]=invalid;
        else f.app->ShadowDistance()=invalid;
        failAfter=1;const bool current=candidate->IsCurrent();const long remaining=failAfter;failAfter=-1;
        Check(!current&&remaining==1,
            "global stale guard rejects nonfinite current pose without allocating an exception");
        std::shared_ptr<plugins::HistoryChange> rejected;
        const auto status=kind==0?plugins::PlanCameraRegistration(*f.app,20,false,rejected):
            kind==1?plugins::PlanLightRegistration(*f.app,20,rejected):
                    plugins::PlanShadowRegistration(*f.app,20,.05f,rejected);
        Check(status==MMS_E_ARGUMENT&&!rejected,
            "global planning still rejects nonfinite current pose after separating pure pose reads");
    }
}
void NativeGlobalRegistrationChecks(){
    std::puts("  global registration: native registrar and seek parity");GlobalRegistrationParity();
    std::puts("  global registration: root overwrite and no-op");GlobalRegistrationRootAndNoop();
    std::puts("  global registration: authored curves and target sampling");GlobalRegistrationCurves();
    std::puts("  global registration: private allocation failures and stale guards");GlobalRegistrationFailures();
    std::puts("  global registration: growth selection and persistent camera parents");GlobalRegistrationLaterGrowthAndParents();
    std::puts("  global registration: finite target sampling and allocation-free guards");GlobalRegistrationFiniteSampling();
}
}
