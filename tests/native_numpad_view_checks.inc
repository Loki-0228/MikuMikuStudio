#include "../src/plugins/native_viewport_history.hpp"
namespace mikudancestudio {void ConsumeNumpadViewPresets(MMDApp*);void ReloadModels(MMDApp*);void PostModelReload(MMDApp*);}
namespace {
struct NumpadView {
    std::array<float,10> values{};D3DMATRIX basis{};std::uint8_t suppressed=0,perspective=0;int parentModel=-1,parentBone=0;
};
NumpadView NumpadRead(MMDApp& app){return {{app.CameraPositionX(),app.CameraPositionY(),app.CameraPositionZ(),app.CameraPitch(),app.CameraYaw(),app.CameraRoll(),
    app.CameraDistance(),app.ViewOffsetX(),app.ViewOffsetY(),app.CameraFov()},app.CameraAttachmentBasis(),app.CameraAttachmentTransformSuppressed(),app.CameraPerspective(),app.CameraParentModel(),app.CameraParentBone()};}
bool NumpadSame(const NumpadView& a,const NumpadView& b){return !std::memcmp(a.values.data(),b.values.data(),sizeof(a.values))&&
    !std::memcmp(&a.basis,&b.basis,sizeof(a.basis))&&a.suppressed==b.suppressed&&a.perspective==b.perspective&&a.parentModel==b.parentModel&&a.parentBone==b.parentBone;}
void NumpadWrite(MMDApp& app,const NumpadView& view){for(unsigned axis=0;axis<3;++axis){app.CameraPositionAxis(axis)=view.values[axis];app.CameraRotation()[axis]=view.values[axis+3];}
    app.CameraDistance()=view.values[6];app.ViewOffsetX()=view.values[7];app.ViewOffsetY()=view.values[8];app.CameraFov()=view.values[9];app.CameraAttachmentBasis()=view.basis;
    app.CameraAttachmentTransformSuppressed()=view.suppressed;app.CameraPerspective()=view.perspective;app.CameraParentModel()=view.parentModel;app.CameraParentBone()=view.parentBone;}
float NumpadBits(std::uint32_t bits){float value;std::memcpy(&value,&bits,sizeof(value));return value;}
NumpadView NumpadOracle(NumpadView view,unsigned mask,bool cameraLike,CameraAttachmentReference reference){
    // Independent transcription of the original image's preset table. The
    // image's half-pi and pi are deliberately not rounded standard constants.
    const float half=NumpadBits(0x3fc90ff9),pi=NumpadBits(0x40490fd8);
    struct Preset{unsigned key;float pitch,yaw;bool targetY;};
    const Preset original[]={{2,0,0,true},{4,0,half,true},{6,0,-half,true},{8,0,pi,true},{5,-half,0,false},{0,half,0,false},{1,half,0,false}};
    for(const auto& preset:original)if(mask&(1u<<preset.key)){
        view.values[3]=preset.pitch;view.values[4]=preset.yaw;view.values[5]=0;
        view.values[6]=!cameraLike&&preset.key==1?-30.f:-45.f;
        if(!cameraLike){view.values[7]=0;view.values[8]=preset.targetY&&reference!=CameraAttachmentReference::SelectedBone?10.f:0.f;
            for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)view.basis.m[row][col]=row==col?1.f:0.f;}
    }return view;
}
struct NumpadFixture:PoseNumericFixture {
    HWND previousFocus=GetFocus();
    NumpadFixture(){app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(4);
        for(unsigned index=0;index<3;++index){auto& key=app->CameraKeys()[index];key.frame=index==0?0:index==1?12:31;key.previous=index?index-1:0;key.next=index<2?index+1:0;
            key.parentModel=-1;key.distance=-33.f-float(index)*7;key.fov=37+int(index)*13;key.perspective=std::uint8_t(index%2);
            for(unsigned axis=0;axis<3;++axis){key.eye[axis]=float(index*3+axis)*.317f;key.target[axis]=float(index+axis)*.1234567f;}
            std::memset(key.interpolation,20,12);std::memset(&key.interpolation[2][0],107,12);key.interpolation[0][0]=3;key.interpolation[1][0]=81;key.interpolation[2][0]=91;key.interpolation[3][0]=119;}
        app->ViewportInputActive()=1;app->CurrentFrame()=7;app->CameraMode()=1;app->CameraParentModel()=-1;app->CameraFov()=45;
        app->CameraPitch()=.23f;app->CameraYaw()=-.57f;app->CameraRoll()=.13f;app->CameraDistance()=-71.25f;
        app->CameraPositionX()=3.125f;app->CameraPositionY()=-2.25f;app->CameraPositionZ()=6.5f;app->ViewOffsetX()=2.75f;app->ViewOffsetY()=-3.5f;
        app->CameraAttachmentTransformSuppressed()=1;
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)app->CameraAttachmentBasis().m[row][col]=float(row*4+col+1)*.125f;
        app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);SetFocus(app->MainWindow());}
    ~NumpadFixture(){history.Reset();history.Collect();curve_host::Reset(app.get());mdl::KeyPoolDestroy(app->CameraKeys());if(previousFocus&&IsWindow(previousFocus))SetFocus(previousFocus);else SetFocus(nullptr);}
    void Branch(unsigned branch,CameraAttachmentReference reference=CameraAttachmentReference::None){
        app->CameraMode()=branch==0;app->state.followCameraEnabled=branch==1;app->CameraParentModel()=branch==0?-1:0;app->CameraParentBone()=0;
        app->CameraReferenceMode()=reference;app->SelectedModelSlot()=0;first.m->selectedBone=0;
    }
    void Keys(unsigned mask,unsigned state=1){std::memset(app->state.numpadKeyState,0,sizeof(app->state.numpadKeyState));
        for(unsigned key=0;key<10;++key)if(mask&(1u<<key))app->state.numpadKeyState[key]=state;}
    std::uint64_t Digest(){std::uint64_t value=14695981039346656037ull;
        const auto add=[&](const void* p,std::size_t n){const auto* bytes=static_cast<const unsigned char*>(p);while(n--){value^=*bytes++;value*=1099511628211ull;}};
        add(&app->state,sizeof(app->state));add(app->CameraKeys(),mdl::KeyPoolCapacity(app->CameraKeys())*sizeof(mdl::CameraKey));
        const auto a=first.Digest(),b=second.Digest();add(&a,sizeof(a));add(&b,sizeof(b));return value;}
};
NumpadView NumpadExpected(NumpadFixture& source,unsigned mask,bool cameraLike,CameraAttachmentReference reference){
    auto view=NumpadRead(*source.app);if(cameraLike||!(mask&1u))return NumpadOracle(view,mask,cameraLike,reference);
    // ReloadModels is the existing independent seek implementation. Run its
    // complete native/free-camera evaluation only on a separate unbound app.
    NumpadFixture oracle;oracle.Branch(2,reference);oracle.app->CurrentFrame()=source.app->CurrentFrame();
    const auto capacity=mdl::KeyPoolCapacity(source.app->CameraKeys());Check(mdl::KeyPoolEnsure(oracle.app->CameraKeys(),capacity),"size isolated legacy numpad-zero oracle pool");
    std::memcpy(oracle.app->CameraKeys(),source.app->CameraKeys(),capacity*sizeof(mdl::CameraKey));
    pmm_extension::Document authoring;curve_host::Capture(*source.app,authoring);curve_host::Load(*oracle.app,authoring);
    for(unsigned key:{2u,4u,6u,8u,5u,0u,1u})if(mask&(1u<<key)){
        if(key){view=NumpadOracle(view,1u<<key,false,reference);continue;}
        NumpadWrite(*oracle.app,view);oracle.app->ViewOffsetX()=oracle.app->ViewOffsetY()=0;oracle.app->CameraAttachmentTransformSuppressed()=0;
        ReloadModels(oracle.app.get());PostModelReload(oracle.app.get());view=NumpadRead(*oracle.app);
    }return view;
}
void NumpadPlanCommit(NumpadFixture& f,const std::shared_ptr<plugins::HistoryChange>& change){
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(change&&f.history.PrepareNative("numpad preset",change,token,plugins::SharedHistory::NativeIntent::Adjustment)==MMS_OK,"reserve one complete numpad adjustment");
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(result==MMS_OK,"publish numpad view and its common history without allocation");
}
void NumpadConstantsAndModes(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(unsigned branch=0;branch<3;++branch)for(auto reference:{CameraAttachmentReference::None,CameraAttachmentReference::ModelRoot,CameraAttachmentReference::SelectedBone})
        for(unsigned key:{2u,4u,6u,8u,5u,0u,1u})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        NumpadFixture f;f.Branch(branch,reference);plugins::NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"configure numpad adjustment recording mode");
        const auto before=NumpadRead(*f.app),expected=NumpadExpected(f,1u<<key,branch!=2,reference);const auto digest=f.Digest();
        const auto ring=PoseRingDigest(f.first);const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());std::shared_ptr<plugins::HistoryChange> change;
        const auto result=plugins::PlanViewportNumpadPresets(*f.app,std::uint16_t(1u<<key),change);
        Check(result==MMS_OK&&change&&f.Digest()==digest,"plan presets privately against exact original view constants and source context");NumpadPlanCommit(f,change);
        Check(NumpadSame(NumpadRead(*f.app),expected)&&GlobalRegistrationSame(f.app->CameraKeys(),keys)&&PoseRingDigest(f.first)==ring&&f.app->CurrentFrame()==7,
            "camera/follow/model presets match independent old constants, offsets, basis, distance and selected-bone reference rules");
        Check(f.history.UndoCount()==(mode==Mode::AllOperations?1:0),"presets are one adjustment in all-operations mode and preview-only in explicit mode");
        if(mode==Mode::AllOperations){
            const bool reset=branch==2&&key==0;auto undo=before,redo=expected;
            if(!reset){f.app->CameraPositionX()+=17;f.app->CameraFov()=53.125f;f.app->CameraAttachmentTransformSuppressed()^=1;
                undo.values[0]=redo.values[0]=f.app->CameraPositionX();undo.values[9]=redo.values[9]=f.app->CameraFov();undo.suppressed=redo.suppressed=f.app->CameraAttachmentTransformSuppressed();}
            if(reset){f.app->CameraAttachmentBasis().m[0][3]+=4;undo.basis.m[0][3]=redo.basis.m[0][3]=f.app->CameraAttachmentBasis().m[0][3];}
            if(branch!=2){f.app->ViewOffsetX()+=2;f.app->CameraAttachmentBasis().m[0][3]+=4;
                undo.values[7]=redo.values[7]=f.app->ViewOffsetX();undo.basis.m[0][3]=redo.basis.m[0][3]=f.app->CameraAttachmentBasis().m[0][3];}
            f.app->CurrentFrame()=29;f.app->CameraMode()=branch==2;f.app->state.followCameraEnabled=0;f.app->SelectedModelSlot()=1;f.second.bones[0].trans[1]=9.25f;
            Check(f.history.History(false)==MMS_OK&&NumpadSame(NumpadRead(*f.app),undo)&&f.second.bones[0].trans[1]==9.25f,
                "numpad undo crosses frame, mode and selected model while preserving unrelated view and model edits");
            Check(f.history.History(true)==MMS_OK&&NumpadSame(NumpadRead(*f.app),redo)&&f.app->CurrentFrame()==29&&PoseRingDigest(f.first)==ring,
                "numpad redo owns the exact preset result without moving the playhead or rewriting legacy rings");
        }
    }
}
void NumpadBatchAndNoop(){
    for(unsigned branch=0;branch<3;++branch){NumpadFixture f;f.Branch(branch);plugins::NativePoseTestBinding binding(*f.app,f.history);
        const unsigned mask=(1u<<2)|(1u<<4)|(1u<<6)|(1u<<8)|(1u<<5)|(1u<<1)|1u;
        const auto expected=NumpadExpected(f,mask,branch!=2,CameraAttachmentReference::None);bool changed=false;
        f.Keys(mask);ConsumeNumpadViewPresets(f.app.get());
        Check(NumpadSame(NumpadRead(*f.app),expected)&&f.history.UndoCount()==1,"one pump consumes simultaneous numpad edges in the original 2/4/6/8/5/0/1 order as one adjustment");
        f.Keys(mask,3);ConsumeNumpadViewPresets(f.app.get());Check(f.history.UndoCount()==1,"held numpad keys do not repeat a preset");
        f.Keys(mask,2);ConsumeNumpadViewPresets(f.app.get());f.Keys(0);ConsumeNumpadViewPresets(f.app.get());Check(f.history.UndoCount()==1,"release and idle pump states do not manufacture an adjustment");
        Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"seed numpad no-op redo branch");
        auto noop=NumpadOracle(NumpadRead(*f.app),1u<<2,branch!=2,CameraAttachmentReference::None);NumpadWrite(*f.app,noop);
        const auto before=f.Digest();f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;const auto view=NumpadRead(*f.app);
        Check(plugins::AdjustViewportNumpadPresets(*f.app,f.history,1u<<2,changed)==MMS_OK&&!changed&&NumpadSame(NumpadRead(*f.app),view)&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
            "an already-applied preset leaves redo and scene flags intact without retaining another adjustment");
        (void)before;
        std::shared_ptr<plugins::HistoryChange> empty;Check(plugins::PlanViewportNumpadPresets(*f.app,0,empty)==MMS_OK&&!empty,"an empty preset mask is a true no-op");
    }
}
void NumpadPumpGates(){
    for(unsigned branch=0;branch<3;++branch)for(unsigned gate=0;gate<7;++gate){NumpadFixture f;f.Branch(branch);plugins::NativePoseTestBinding binding(*f.app,f.history);f.Keys(1u<<4);
        HWND edit=nullptr;
        if(gate==0)f.app->ViewportInputActive()=0;
        if(gate==1)f.app->PlaybackActive()=1;
        if(gate==2){edit=CreateWindowExW(0,L"EDIT",L"typing",WS_CHILD,0,0,80,20,f.app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(0x22a)),GetModuleHandleW(nullptr),nullptr);
            Check(edit&&SetFocus(edit)!=reinterpret_cast<HWND>(-1)&&GetFocus()==edit,"hidden panel edit owns focus for the numpad input gate");}
        if(gate==3)f.app->FrameStepPlayback()=1;
        if(gate==4)f.app->state.recordPlaybackStartPending=1;
        std::unique_ptr<ScopedSceneMutation> mutation;
        if(gate==5)mutation=std::make_unique<ScopedSceneMutation>(*f.app);
        if(gate==6)SetCapture(f.app->MainWindow());
        const auto view=NumpadRead(*f.app);ConsumeNumpadViewPresets(f.app.get());
        Check(NumpadSame(NumpadRead(*f.app),view)&&!f.history.UndoCount(),"actual numpad pump refuses inactive, playback, text-edit, pending, mutation and captured-pointer input without old fallback");
        if(gate==6)ReleaseCapture();if(edit){SetFocus(f.app->MainWindow());DestroyWindow(edit);}
    }
}
void NumpadFailuresAndStale(){
    for(unsigned branch=0;branch<3;++branch)for(unsigned mask:{1u<<4,1u}){NumpadFixture f;f.Branch(branch);plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto before=f.Digest();std::shared_ptr<plugins::HistoryChange> change;bool completed=false;
        for(long allowance=0;allowance<512;++allowance){failAfter=allowance;const auto result=plugins::PlanViewportNumpadPresets(*f.app,std::uint16_t(mask),change);failAfter=-1;
            Check(f.Digest()==before,"numpad planning OOM leaves every view, model and native key unchanged");
            if(result==MMS_OK){completed=bool(change);break;}Check(result==MMS_E_MEMORY&&!change,"numpad planner reports allocation failure atomically");}
        Check(completed,"numpad planning OOM sweep reaches success");completed=false;
        for(long allowance=0;allowance<512;++allowance){std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=allowance;const auto result=change->Prepare(true,candidate);failAfter=-1;
            Check(f.Digest()==before,"numpad candidate OOM and cancellation never publish a partial view");
            if(result==MMS_OK){completed=candidate&&candidate->IsCurrent();break;}Check(result==MMS_E_MEMORY&&!candidate,"numpad candidate reports private allocation failure");}
        Check(completed,"numpad candidate OOM sweep reaches success");
        plugins::SharedHistory budget(f.storage,{},8,1);bool changed=false;const auto view=NumpadRead(*f.app);
        Check(plugins::AdjustViewportNumpadPresets(*f.app,budget,std::uint16_t(mask),changed)==MMS_E_MEMORY&&!changed&&NumpadSame(NumpadRead(*f.app),view),"numpad budget rejection precedes all live mutations");
        Check(budget.SetMode(plugins::SharedHistory::RecordingMode::ExplicitOnly)==MMS_OK&&plugins::AdjustViewportNumpadPresets(*f.app,budget,std::uint16_t(mask),changed)==MMS_OK&&changed&&!budget.RetainedBytes(),
            "explicit-only numpad preview needs no retained history-node budget");
    }
    for(unsigned reason=0;reason<9;++reason){NumpadFixture f;f.Branch(2,CameraAttachmentReference::SelectedBone);plugins::NativePoseTestBinding binding(*f.app,f.history);
        std::shared_ptr<plugins::HistoryChange> change;Check(plugins::PlanViewportNumpadPresets(*f.app,1u<<4,change)==MMS_OK&&change,"capture privately frozen numpad source");
        auto* pool=f.app->CameraKeys();
        if(reason==0)f.app->CameraYaw()+=1;
        if(reason==1)f.app->CurrentFrame()+=1;
        if(reason==2)f.app->CameraParentModel()=1;
        if(reason==3)f.app->CameraReferenceMode()=CameraAttachmentReference::ModelRoot;
        if(reason==4)f.app->SelectedModelSlot()=1;
        if(reason==5)f.first.m->selectedBone=1;
        if(reason==6)f.app->CameraAttachmentBasis().m[0][0]+=1;
        if(reason==7){plugins::NativePoseTestBinding nested(*f.app,f.history);}
        if(reason==8)f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(4);
        const auto source=NumpadRead(*f.app);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&NumpadSame(NumpadRead(*f.app),source),"changed numpad pose, frame, reference, model/bone, basis, binding generation or pool identity rejects the frozen source");
        if(reason==8){mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=pool;}
    }
}
void NumpadResetAuthor(NumpadFixture& f){
    std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,channels,error),"read native camera for zero-preset sparse authoring");
    auto& lane=channels[0];lane.track.keys.erase(lane.track.keys.begin()+1);lane.nativeControls.clear();lane.controlGeometry.clear();lane.sample={};
    lane.track.keys.front().value=.123456789012;lane.track.keys.back().value=7.987654321098;
    for(auto& key:lane.track.keys){key.tangent=animation_curve::Tangent::Free;key.interpolation=animation_curve::Interpolation::Bezier;
        key.inWeight=.31;key.outWeight=.19;key.inSlope=-.017;key.outSlope=.013;}
    channels[1].track.keys.back().interpolation=animation_curve::Interpolation::Step;channels[1].nativeControls.clear();channels[1].controlGeometry.clear();channels[1].sample={};
    Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,channels,error),"author exact sparse Free and Step camera lanes for model zero reset");
}
void NumpadResetLegacyParity(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(bool authored:{false,true})for(int frame:{0,1,7,12,30,31,44})for(int parent:{-1,0})for(unsigned mask:{1u,1u|(1u<<1),1u|(1u<<4)})
        for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        NumpadFixture f;f.Branch(2,CameraAttachmentReference::SelectedBone);f.app->CurrentFrame()=frame;
        for(unsigned index=0;index<3;++index){f.app->CameraKeys()[index].parentModel=parent;f.app->CameraKeys()[index].parentBone=parent<0?0:1;}
        if(authored)NumpadResetAuthor(f);
        const auto expected=NumpadExpected(f,mask,false,CameraAttachmentReference::SelectedBone);const auto before=NumpadRead(*f.app);
        const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());const auto first=f.first.Digest(),second=f.second.Digest();
        Check(f.history.SetMode(mode)==MMS_OK,"configure model zero native/free camera reset recording mode");plugins::NativePoseTestBinding binding(*f.app,f.history);
        bool changed=false;Check(plugins::AdjustViewportNumpadPresets(*f.app,f.history,std::uint16_t(mask),changed)==MMS_OK&&changed&&NumpadSame(NumpadRead(*f.app),expected),
            "model zero and zero-then-one batches exactly match old ReloadModels native interpolation, parents, projection flags and sparse free evaluation");
        Check(GlobalRegistrationSame(f.app->CameraKeys(),keys)&&f.first.Digest()==first&&f.second.Digest()==second&&f.app->CurrentFrame()==frame&&
            f.history.UndoCount()==(mode==Mode::AllOperations?1:0),"zero preset writes only captured view fields and records one adjustment without key, pose or playhead edits");
        if(mode==Mode::AllOperations){f.app->CurrentFrame()=89;f.app->CameraMode()=1;f.app->SelectedModelSlot()=1;
            for(unsigned repeat=0;repeat<3;++repeat)Check(f.history.History(false)==MMS_OK&&NumpadSame(NumpadRead(*f.app),before)&&
                f.history.History(true)==MMS_OK&&NumpadSame(NumpadRead(*f.app),expected)&&f.app->CurrentFrame()==89,
                "reset undo redo restore frozen native/free values after crossing frame, mode and selected model");}
    }
    for(unsigned reason=0;reason<5;++reason){NumpadFixture f;f.Branch(2);NumpadResetAuthor(f);plugins::NativePoseTestBinding binding(*f.app,f.history);
        std::shared_ptr<plugins::HistoryChange> change;Check(plugins::PlanViewportNumpadPresets(*f.app,1u,change)==MMS_OK&&change,"freeze model zero source before independent changes");
        std::unique_ptr<plugins::PreparedHistoryChange> prepared;Check(change->Prepare(true,prepared)==MMS_OK&&prepared&&prepared->IsCurrent(),"prepare a read-only frozen model zero reset");
        if(reason==0)f.app->CameraKeys()[1].eye[2]+=1;
        if(reason==1)f.app->CameraKeys()[3].frame=13;
        if(reason==2){plugins::ForgetSceneObject(f.first.raw());plugins::SceneObjectIdentity(f.first.raw());}
        if(reason==3){std::vector<curve_editor::Channel> channels;std::wstring error;Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,channels,error),"read source for independent reset metadata edit");
            channels[0].collections.push_back({"other",L"other author metadata"});Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,channels,error),"independent camera metadata author invalidates a pending reset");}
        if(reason==4){
            std::vector<curve_editor::Channel> channels;std::wstring error;Check(curve_host::Read(*f.app,curve_host::Kind::Camera,0,channels,error),"read PMM author-only reset source");
            channels[0].track.keys.back().frame=55;std::vector<animation_curve::NamedTrack> named;
            for(const auto& channel:channels)named.push_back({"0/0/"+channel.id,channel.track});std::vector<std::uint8_t> bytes;std::string encodeError;
            Check(animation_curve::Encode(named,bytes,encodeError),"encode PMM author-only camera source");pmm_extension::Document document;
            pmm_extension::Set(document,"org.mikudancestudio.animation_curves.v1",std::move(bytes));curve_host::Load(*f.app,document);
            // Capture a new pristine source after installing the authored-only
            // frame, then change it through the same-author retime participant.
            Check(plugins::PlanViewportNumpadPresets(*f.app,1u,change)==MMS_OK&&change&&change->Prepare(true,prepared)==MMS_OK,"freeze reset over PMM author-only frame source");
            const auto native=GlobalRegistrationKeys(f.app->CameraKeys());std::shared_ptr<plugins::HistoryChange> retime;
            Check(plugins::PlanCameraFrameLine(*f.app,40,true,retime)==MMS_OK&&retime,"prepare an independent same-author frame-line with no native patches");FrameLineCommit(f.history,retime);
            Check(GlobalRegistrationSame(f.app->CameraKeys(),native),"independent author-only retime changes no native camera bytes");
        }
        const auto before=NumpadRead(*f.app);Check(!prepared->IsCurrent(),"reset candidate detects native, owner or authored source invalidation before publication");
        std::unique_ptr<plugins::PreparedHistoryChange> fresh;Check(change->Prepare(true,fresh)==MMS_E_STALE&&!fresh&&NumpadSame(NumpadRead(*f.app),before),
            "reset cannot silently re-evaluate a changed native or same-author sparse source");
    }
}
void NativeNumpadViewChecks(){
    std::puts("  numpad views: original constants, camera/follow/model branches and adjustment modes");NumpadConstantsAndModes();
    std::puts("  numpad views: actual pump order, held/no-op redo and input gates");NumpadBatchAndNoop();NumpadPumpGates();
    std::puts("  numpad views: private OOM, budget and source/binding invalidation");NumpadFailuresAndStale();
    std::puts("  numpad views: native/free model-zero ReloadModels oracle and captured source lifetime");NumpadResetLegacyParity();
}
}
