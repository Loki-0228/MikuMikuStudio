// Include after native_global_registration_checks.inc and existing pose fixture.
#include "../src/plugins/native_accessory_registration.hpp"
namespace mikumikustudio { void ApplyAccessoryTrack(MMDApp*,int); void ClearGlobalFrameSelection(MMDApp*);
void CmdControl450(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
namespace {
struct AccessoryRegistrationFixture : PoseNumericFixture {
    mdl::AccessoryRecord other{};
    AccessoryRegistrationFixture() {
        app->SelectedAccessorySlot()=7;app->AccessorySlot(8)=&other;
        plugins::SceneObjectIdentity(&accessory);plugins::SceneObjectIdentity(&other);
        app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(8);
        app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(8);
        app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(8);
        app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(8);
        for(int slot:{7,8}) {
            app->AccessoryKeys(slot)=mdl::KeyPoolCreate<mdl::AccessoryKey>(8);
            auto& a=*app->AccessorySlot(slot);a.parentModel=-1;a.scale=a.opacity=1;a.visible=1;a.shadowEnabled=1;
            auto* keys=app->AccessoryKeys(slot);keys[0].parentModel=-1;keys[0].scale=keys[0].opacity=1;keys[0].visible=1;
            keys[0].next=5;keys[5].frame=10;keys[5].next=2;keys[5].previous=0;
            keys[2].frame=30;keys[2].previous=5;
            for(unsigned i:{2u,5u}) {keys[i].parentModel=-1;keys[i].position[0]=float(i);keys[i].rotation[1]=float(i)*.137f;
                keys[i].scale=float(i)*.3f;keys[i].opacity=float(i)*.1f;keys[i].visible=1;keys[i].shadowEnabled=1;
                keys[i].reserved[0]=static_cast<unsigned char>(i+20);keys[i].reservedSelection[1]=static_cast<unsigned char>(i+30);}
        }
        accessory.position[0]=9;accessory.rotation[0]=.312f;accessory.scale=2;accessory.opacity=.37f;
        app->LastRegisteredFrame()=15;
        app->CameraKeys()[1].selected=2;app->LightKeys()[2].selected=3;
        app->ShadowKeys()[3].selected=4;app->GravityKeys()[4].selected=5;
        app->AccessoryKeys(7)[5].selected=6;app->AccessoryKeys(8)[2].selected=7;
        for(int id=478;id<=484;++id)Check(CreateWindowExA(0,"EDIT","0",WS_CHILD,0,0,80,20,app->MainWindow(),
            reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"accessory registration numeric control exists");
    }
    ~AccessoryRegistrationFixture() {
        history.Reset();history.Collect();curve_host::Reset(app.get());
        plugins::ForgetSceneObject(&accessory);plugins::ForgetSceneObject(&other);
        for(int slot:{7,8})mdl::KeyPoolDestroy(app->AccessoryKeys(slot));
        app->AccessorySlot(8)=nullptr;
        mdl::KeyPoolDestroy(app->CameraKeys());mdl::KeyPoolDestroy(app->LightKeys());
        mdl::KeyPoolDestroy(app->ShadowKeys());mdl::KeyPoolDestroy(app->GravityKeys());
    }
};
plugins::AccessoryRegistrationInput AccessoryRegistrationInputOf(const mdl::AccessoryRecord& a) {
    plugins::AccessoryRegistrationInput input;std::memcpy(input.position,a.position,sizeof(input.position));
    std::memcpy(input.rotation,a.rotation,sizeof(input.rotation));input.scale=a.scale;return input;
}
std::vector<mdl::AccessoryKey> AccessoryRegistrationKeys(MMDApp& app,int slot=7) {
    const auto* keys=app.AccessoryKeys(slot);return {keys,keys+mdl::KeyPoolCapacity(keys)};
}
bool AccessoryRegistrationKeysEqual(MMDApp& app,const std::vector<mdl::AccessoryKey>& keys,int slot=7) {
    return mdl::KeyPoolCapacity(app.AccessoryKeys(slot))>=keys.size()&&
        !std::memcmp(app.AccessoryKeys(slot),keys.data(),keys.size()*sizeof(mdl::AccessoryKey));
}
bool AccessoryRegistrationPoseEqual(const mdl::AccessoryRecord& a,const mdl::AccessoryRecord& b) {
    return !std::memcmp(a.position,b.position,sizeof(a.position))&&!std::memcmp(a.rotation,b.rotation,sizeof(a.rotation))&&
        MorphSameFloat(a.scale,b.scale)&&MorphSameFloat(a.opacity,b.opacity)&&a.visible==b.visible&&a.shadowEnabled==b.shadowEnabled&&
        a.parentModel==b.parentModel&&a.parentBone==b.parentBone;
}
std::vector<unsigned char> AccessoryRegistrationMarks(MMDApp& app) {
    std::vector<unsigned char> values;
    const auto add=[&](const auto* keys){for(std::size_t i=0,n=mdl::KeyPoolCapacity(keys);i<n;++i)values.push_back(keys[i].selected);};
    add(app.CameraKeys());add(app.LightKeys());add(app.ShadowKeys());add(app.GravityKeys());
    for(int slot=0;slot<255;++slot)add(app.AccessoryKeys(slot));return values;
}
std::shared_ptr<plugins::HistoryChange> AccessoryRegistrationPlan(AccessoryRegistrationFixture& f,
    const plugins::AccessoryRegistrationInput& input,unsigned slot=7) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanAccessoryRegistration(*f.app,slot,input,change)==MMS_OK&&change,"accessory command prepares a complete private record");return change;
}
void AccessoryRegistrationCommit(AccessoryRegistrationFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("accessory registration",change,tx)==MMS_OK,"accessory registration journal prepares");
    Check(CommitRegistrationNoAllocation(f.history,std::move(tx))==MMS_OK,"accessory pose keys marks and curves publish without allocation");
}
void AccessoryRegistrationLegacyParity() {
    const char* text[]={"12.34567","-2.875","3.125","17.12345","-32.875","91.2500","1.375"};
    for(bool bound:{false,true})for(int frame:{0,10,20,60}) {
        AccessoryRegistrationFixture f,oracle;
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        if(bound)for(auto* fixture:{&f,&oracle}) {fixture->accessory.parentModel=1;fixture->accessory.parentBone=1;}
        plugins::AccessoryRegistrationInput input;
        for(int i=0;i<7;++i) {
            SetWindowTextA(GetDlgItem(oracle.app->MainWindow(),478+i),text[i]);
            char truncated[8]{};std::memcpy(truncated,text[i],(std::min)(std::size_t(7),std::strlen(text[i])));const double value=std::atof(truncated);
            if(i<3)input.position[i]=float(value);else if(i<6)input.rotation[i-3]=float(value/180.0*3.141592025756836);else input.scale=float(value);
        }
        const auto before=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);
        const auto other=f.other;const auto model=f.first.Digest();const auto originalPose=f.accessory;
        auto change=AccessoryRegistrationPlan(f,input);
        Check(AccessoryRegistrationKeysEqual(*f.app,before)&&AccessoryRegistrationMarks(*f.app)==marks&&
            AccessoryRegistrationPoseEqual(f.accessory,originalPose)&&f.app->LastRegisteredFrame()==15,
            "preparing accessory input does not commit the text pose or selection early");
        AccessoryRegistrationCommit(f,change);
        for(int i=478;i<=484;++i)CommitEditControl(oracle.app.get(),GetDlgItem(oracle.app->MainWindow(),i));
        ClearGlobalFrameSelection(oracle.app.get());RegisterAccessoryKey(oracle.app.get(),frame,7);
        Check(AccessoryRegistrationKeysEqual(*f.app,AccessoryRegistrationKeys(*oracle.app))&&
            AccessoryRegistrationPoseEqual(f.accessory,oracle.accessory)&&f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame()&&
            AccessoryRegistrationMarks(*f.app)==AccessoryRegistrationMarks(*oracle.app),
            "private accessory input plus registrar matches seven native EDIT commits including seven-byte truncation and degree conversion");
        Check(f.first.Digest()==model&&!std::memcmp(&f.other,&other,sizeof(other))&&f.SelectionUnchanged(),
            "first accessory publication preserves other model object and view selection");
        const auto after=AccessoryRegistrationKeys(*f.app);const auto last=f.app->LastRegisteredFrame();
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=17;
        std::memcpy(oracle.app->AccessoryKeys(7),before.data(),before.size()*sizeof(mdl::AccessoryKey));ApplyAccessoryTrack(oracle.app.get(),7);
        Check(f.history.History(false)==MMS_OK&&AccessoryRegistrationKeysEqual(*f.app,before)&&
            AccessoryRegistrationPoseEqual(f.accessory,oracle.accessory)&&AccessoryRegistrationMarks(*f.app)==marks&&f.app->LastRegisteredFrame()==15,
            "accessory undo restores keys and marks while sampling exact native step and float interpolation at current frame");
        f.app->CameraKeys()[6].selected=12;f.app->AccessoryKeys(8)[7].selected=13;
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=25;std::memcpy(oracle.app->AccessoryKeys(7),after.data(),after.size()*sizeof(mdl::AccessoryKey));
        ApplyAccessoryTrack(oracle.app.get(),7);
        Check(f.history.History(true)==MMS_OK&&AccessoryRegistrationKeysEqual(*f.app,after)&&
            AccessoryRegistrationPoseEqual(f.accessory,oracle.accessory)&&f.app->LastRegisteredFrame()==last&&
            f.app->CameraKeys()[6].selected==0&&f.app->AccessoryKeys(8)[7].selected==0,
            "accessory redo clears intervening global selections and samples only its own current pose");
    }
}
void AccessoryRegistrationCurves() {
    AccessoryRegistrationFixture f,oracle;std::wstring error;
    for(auto* fixture:{&f,&oracle}) {
        std::vector<curve_editor::Channel> channels;
        Check(curve_host::Read(*fixture->app,curve_host::Kind::Accessory,7,channels,error),"read accessory authoring fixture");
        auto& track=channels[0].track;track.keys[1].interpolation=animation_curve::Interpolation::Bezier;
        track.keys[0].outWeight=.2;track.keys[0].outSlope=1.2;track.keys[1].inWeight=.3;track.keys[1].inSlope=-.1;
        Check(curve_host::Write(*fixture->app,curve_host::Kind::Accessory,7,channels,error),"write custom accessory handles");
    }
    f.app->CurrentFrame()=oracle.app->CurrentFrame()=20;
    const auto before=AccessoryRegistrationKeys(*f.app);auto input=AccessoryRegistrationInputOf(f.accessory);input.position[0]=7.5f;
    auto change=AccessoryRegistrationPlan(f,input);AccessoryRegistrationCommit(f,change);
    oracle.accessory.position[0]=7.5f;ClearGlobalFrameSelection(oracle.app.get());RegisterAccessoryKey(oracle.app.get(),20,7);
    std::vector<curve_editor::Channel> synchronized;Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,synchronized,error),
        "ordinary curve reader reconciles post-registration effective authoring");
    f.app->CurrentFrame()=oracle.app->CurrentFrame()=5;
    Check(f.history.History(false)==MMS_OK&&AccessoryRegistrationKeysEqual(*f.app,before),"accessory undo retains original authored native mirror");
    std::vector<curve_editor::Channel> restored;Check(curve_host::Read(*f.app,curve_host::Kind::Accessory,7,restored,error)&&
        restored[0].track.keys[0].outWeight==.2&&restored[0].track.keys[0].outSlope==1.2,
        "accessory undo restores pristine weighted handles after ordinary live reconciliation");
    f.app->CurrentFrame()=oracle.app->CurrentFrame()=25;ApplyAccessoryTrack(oracle.app.get(),7);
    Check(f.history.History(true)==MMS_OK&&AccessoryRegistrationPoseEqual(f.accessory,oracle.accessory),
        "accessory redo derives the original authored curve and matches the legacy reader at a new frame");
}
void AccessoryRegistrationGrowthFailures() {
    AccessoryRegistrationFixture f;auto* keys=f.app->AccessoryKeys(7);std::memset(keys,0,8*sizeof(*keys));
    for(unsigned slot=0;slot<8;++slot) {keys[slot].frame=slot*10;keys[slot].parentModel=-1;keys[slot].scale=keys[slot].opacity=1;
        keys[slot].previous=slot?slot-1:0;keys[slot].next=slot<7?slot+1:0;}
    f.app->CurrentFrame()=90;const auto identity=mdl::KeyPoolIdentity(keys);const auto before=AccessoryRegistrationKeys(*f.app);
    const auto marks=AccessoryRegistrationMarks(*f.app);const auto pose=f.accessory;auto input=AccessoryRegistrationInputOf(f.accessory);input.position[2]=13;
    bool ready=false;unsigned failed=0;std::shared_ptr<plugins::HistoryChange> change;
    for(long limit=0;limit<1024;++limit) {
        failAfter=limit;const auto status=plugins::PlanAccessoryRegistration(*f.app,7,input,change);failAfter=-1;
        Check(f.app->AccessoryKeys(7)==keys&&AccessoryRegistrationKeysEqual(*f.app,before)&&AccessoryRegistrationMarks(*f.app)==marks&&
            AccessoryRegistrationPoseEqual(f.accessory,pose),"accessory planning allocation failure preserves input pose pool and all global marks");
        if(status==MMS_OK){ready=true;break;}Check(status==MMS_E_MEMORY&&!change,"accessory plan allocation failure reports memory");++failed;
    }
    Check(ready&&change&&failed>5,"accessory plan allocation sweep reaches retained owners and source curves");
    ready=false;failed=0;
    for(long limit=0;limit<1024;++limit) {
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=limit;const auto status=change->Prepare(true,candidate);failAfter=-1;
        Check(f.app->AccessoryKeys(7)==keys&&mdl::KeyPoolCapacity(keys)==8&&mdl::KeyPoolIdentity(keys)==identity&&
            AccessoryRegistrationKeysEqual(*f.app,before)&&AccessoryRegistrationMarks(*f.app)==marks&&AccessoryRegistrationPoseEqual(f.accessory,pose),
            "accessory candidate failure and cancelled growth preserve pointer capacity keys pose and selection");
        if(status==MMS_OK){ready=true;candidate.reset();break;}
        Check(status==MMS_E_MEMORY&&!candidate,"accessory candidate allocation failure reports memory");++failed;
    }
    Check(ready&&failed>10,"accessory prepare allocation sweep reaches target sampling and global selection capture");
    AccessoryRegistrationCommit(f,change);const auto grown=mdl::KeyPoolCapacity(f.app->AccessoryKeys(7));
    Check(grown>8&&f.app->AccessoryKeys(7)!=keys&&mdl::KeyPoolIdentity(f.app->AccessoryKeys(7))==identity&&
        f.app->AccessoryKeys(7)[8].frame==90&&f.accessory.position[2]==13,"full accessory pool grows privately with stable identity");
    Check(f.history.History(false)==MMS_OK&&AccessoryRegistrationKeysEqual(*f.app,before)&&
        mdl::KeyPoolCapacity(f.app->AccessoryKeys(7))==grown&&f.app->AccessoryKeys(7)[8].frame==0,
        "accessory undo restores author data without shrinking accepted allocation");
    const auto redo=f.history.RedoCount();f.app->CurrentFrame()=100;input.position[2]=17;auto next=AccessoryRegistrationPlan(f,input);
    std::string label="accessory reservation failure";std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    failAfter=0;const auto status=f.history.PrepareNative(std::move(label),next,tx);failAfter=-1;
    Check(status==MMS_E_MEMORY&&!tx&&f.history.RedoCount()==redo&&AccessoryRegistrationKeysEqual(*f.app,before),
        "accessory journal allocation failure does not truncate redo");
    plugins::SharedHistory limited(f.storage,{},128,1);
    Check(limited.PrepareNative("accessory budget",next,tx)==MMS_E_MEMORY&&!tx&&f.history.RedoCount()==redo,
        "actual accessory record payload must fit the journal budget before publication");
}
void AccessoryRegistrationGuards() {
    for(int kind=0;kind<6;++kind) {
        AccessoryRegistrationFixture f;f.app->CurrentFrame()=20;f.accessory.parentModel=1;f.accessory.parentBone=1;
        auto input=AccessoryRegistrationInputOf(f.accessory);input.scale=3;auto change=AccessoryRegistrationPlan(f,input);
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;Check(change->Prepare(true,candidate)==MMS_OK,"prepare accessory stale fixture");
        switch(kind) {
        case 0:f.accessory.position[2]=4;break;
        case 1:f.app->LightKeys()[7].selected=8;break;
        case 2:++f.app->CurrentFrame();break;
        case 3:++f.app->LastRegisteredFrame();break;
        case 4:f.app->AccessoryKeys(7)[5].position[1]=6;break;
        default:f.second.m->boneCount=1;break;
        }
        Check(!candidate->IsCurrent(),"accessory candidate detects pose global selection frame keys and binding structure changes");candidate.reset();
    }
    AccessoryRegistrationFixture f;auto input=AccessoryRegistrationInputOf(f.accessory);f.app->CurrentFrame()=0;
    auto* root=f.app->AccessoryKeys(7);root->visible=f.accessory.visible;root->shadowEnabled=f.accessory.shadowEnabled;
    std::memcpy(root->position,f.accessory.position,sizeof(root->position));std::memcpy(root->rotation,f.accessory.rotation,sizeof(root->rotation));
    root->scale=f.accessory.scale;root->opacity=f.accessory.opacity;
    const auto before=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);std::shared_ptr<plugins::HistoryChange> empty;
    Check(plugins::PlanAccessoryRegistration(*f.app,7,input,empty)==MMS_OK&&!empty&&AccessoryRegistrationKeysEqual(*f.app,before)&&
        AccessoryRegistrationMarks(*f.app)==marks,"same accessory pose and key do not turn selection clearing into a history operation");
    f.app->CurrentFrame()=20;root[5].next=5;
    Check(plugins::PlanAccessoryRegistration(*f.app,7,input,empty)==MMS_E_ARGUMENT&&!empty,"accessory malformed track rejected without registration");
    root[5].next=2;input.scale=std::numeric_limits<float>::infinity();
    Check(plugins::PlanAccessoryRegistration(*f.app,7,input,empty)==MMS_E_ARGUMENT&&!empty,"nonfinite accessory input rejected before pose writes");
}
void AccessoryRegistrationCrossObject() {
    AccessoryRegistrationFixture f;const auto first=AccessoryRegistrationKeys(*f.app,7);
    const auto originalMarks=AccessoryRegistrationMarks(*f.app);
    auto firstInput=AccessoryRegistrationInputOf(f.accessory);firstInput.position[0]=17;
    f.app->CurrentFrame()=20;AccessoryRegistrationCommit(f,AccessoryRegistrationPlan(f,firstInput));
    const auto firstAfter=AccessoryRegistrationKeys(*f.app,7);
    // Registering the first object already clears the second object's marks.
    // Undo of the second command restores this immediate predecessor state.
    const auto secondBefore=AccessoryRegistrationKeys(*f.app,8);
    const auto firstMarks=AccessoryRegistrationMarks(*f.app);
    auto secondInput=AccessoryRegistrationInputOf(f.other);secondInput.position[2]=23;
    f.app->CurrentFrame()=40;AccessoryRegistrationCommit(f,AccessoryRegistrationPlan(f,secondInput,8));
    const auto secondAfter=AccessoryRegistrationKeys(*f.app,8);const auto firstPose=f.accessory;
    const auto secondMarks=AccessoryRegistrationMarks(*f.app);
    f.app->CurrentFrame()=15;
    Check(f.history.History(false)==MMS_OK,"undo of later accessory registration succeeds");
    Check(AccessoryRegistrationKeysEqual(*f.app,secondBefore,8),"later accessory undo restores its immediate predecessor keys and marks");
    Check(AccessoryRegistrationMarks(*f.app)==firstMarks,"later accessory undo restores selection captured after the first command");
    Check(AccessoryRegistrationPoseEqual(f.accessory,firstPose),"undo of later accessory registration leaves earlier accessory pose untouched");
    const auto secondPose=f.other;
    Check(f.history.History(false)==MMS_OK,"undo of earlier accessory registration succeeds");
    Check(AccessoryRegistrationKeysEqual(*f.app,first,7),"earlier accessory undo restores its original keys and marks");
    Check(AccessoryRegistrationMarks(*f.app)==originalMarks,"earlier accessory undo restores original global marks");
    Check(AccessoryRegistrationPoseEqual(f.other,secondPose),"cross-accessory undo preserves the other local evaluated pose");
    Check(f.history.History(true)==MMS_OK,"redo of earlier accessory registration succeeds");
    Check(AccessoryRegistrationMarks(*f.app)==firstMarks,"earlier accessory redo restores its selected key and clears original other marks");
    Check(f.history.History(true)==MMS_OK,"redo of later accessory registration succeeds");
    Check(AccessoryRegistrationMarks(*f.app)==secondMarks,"later accessory redo restores final global marks");
    // The second command clears the first command's key selection, so compare
    // authored payload separately from the globally coordinated selection.
    auto actual=AccessoryRegistrationKeys(*f.app,7);auto expected=firstAfter;
    for(auto& key:actual)key.selected=0;for(auto& key:expected)key.selected=0;
    Check(!std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(mdl::AccessoryKey))&&
        AccessoryRegistrationKeysEqual(*f.app,secondAfter,8),"cross-accessory redo preserves both authored tracks");
}
void AccessoryRegistrationCommandChecks() {
    AccessoryRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->CurrentFrame()=20;
    const char* inputs[]={"12.3456","2.25","-4.75","17.125","31.5","-9.25","1.625"};
    for(int i=0;i<7;++i)SetWindowTextA(GetDlgItem(f.app->MainWindow(),478+i),inputs[i]);
    f.app->state.enterKeyState=1;f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;
    const auto before=AccessoryRegistrationKeys(*f.app);const auto marks=AccessoryRegistrationMarks(*f.app);const auto pose=f.accessory;
    failAfter=0;CmdControl450(f.app.get(),f.app->MainWindow(),487,0);failAfter=-1;
    Check(AccessoryRegistrationKeysEqual(*f.app,before)&&AccessoryRegistrationMarks(*f.app)==marks&&
        AccessoryRegistrationPoseEqual(f.accessory,pose)&&f.app->SceneModified()==0&&f.app->RecordedFrameCountByte1()==0&&
        f.app->state.enterKeyState==1&&f.history.UndoCount()==0&&f.SelectionUnchanged(),
        "real bound 487 allocation failure preserves input pose key marks dirty key-consumption and view selection");
    for(int i=0;i<7;++i){char text[32]{};GetWindowTextA(GetDlgItem(f.app->MainWindow(),478+i),text,sizeof(text));
        Check(!std::strcmp(text,inputs[i]),"failed bound accessory registration leaves numeric text exactly unchanged");}
    CmdControl450(f.app.get(),f.app->MainWindow(),487,0);
    Check(f.history.UndoCount()==1&&f.app->SceneModified()==1&&f.app->state.enterKeyState==3&&
        f.accessory.position[0]==12.3456f&&f.accessory.scale==1.625f&&f.accessory.rowSelected==1,
        "real bound 487 publishes parsed numeric fields and journal before UI feedback");
}
}
void NativeAccessoryRegistrationChecks() {
    std::puts("  accessory registration: numeric input and native key parity");AccessoryRegistrationLegacyParity();
    std::puts("  accessory registration: authored curves and local seek");AccessoryRegistrationCurves();
    std::puts("  accessory registration: private growth failures and budget");AccessoryRegistrationGrowthFailures();
    std::puts("  accessory registration: no-op stale and invalid input");AccessoryRegistrationGuards();
    std::puts("  accessory registration: shared cross-object ordering");AccessoryRegistrationCrossObject();
    std::puts("  accessory registration: real command failure and success");AccessoryRegistrationCommandChecks();
}
