// Include after native_numpad_view_checks.inc and shared object-history helpers.
// Only independent unbound apps execute the legacy 535/247 evaluation path.
#include "../src/plugins/native_camera_follow.hpp"
#include "../src/plugins/native_camera_follow_ui.hpp"
#include "../src/plugins/native_object_commands.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
namespace mikudancestudio {void CmdControl500(MMDApp*,HWND,std::uint16_t,std::uint16_t);void CmdFileMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t);}
namespace {
struct FollowState {
    NumpadView view;std::uint8_t follow=0,shadow=0;
    std::array<float,6> light{};D3DLIGHT9 device{};
    float shadowDistance=0;std::array<float,4> gravity{};std::int32_t noise=0;std::uint32_t noiseMode=0;
    std::array<mdl::AccessoryRecord,2> accessories{};
};
struct FollowFixture:NumpadFixture {
    static constexpr std::array<unsigned,2> slots{7,201};
    std::array<mdl::AccessoryRecord,2> objects{};
    HWND floating=nullptr;HMENU menu=nullptr;
    FollowFixture(){app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(4);app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(4);
        app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(4);
        for(unsigned i=0;i<3;++i){const auto frame=i==0?0:i==1?12:31;
            auto& light=app->LightKeys()[i];light.frame=unsigned(frame);light.previous=i?i-1:0;light.next=i<2?i+1:0;
            for(unsigned axis=0;axis<3;++axis){light.direction[axis]=float(i*2+axis+1)*.137918f;light.color[axis]=float(i+axis+1)*.109173f;}
            auto& shadow=app->ShadowKeys()[i];shadow.frame=light.frame;shadow.previous=light.previous;shadow.next=light.next;shadow.mode=std::uint8_t(i);shadow.distance=.02f+float(i)*.03f;
            auto& gravity=app->GravityKeys()[i];gravity.frame=light.frame;gravity.previous=light.previous;gravity.next=light.next;gravity.acceleration=6.13f+float(i)*2.71f;
            gravity.direction[0]=.2f+float(i)*.137f;gravity.direction[1]=-1.1f+float(i)*.271f;gravity.direction[2]=.3f-float(i)*.107f;
            gravity.noise=i==0?11:i==1?-8:17;const std::uint32_t flags=0xc6711200u+i;std::memcpy(&gravity.noiseEnabled,&flags,sizeof flags);
            light.selected=std::uint8_t(i+3);shadow.selected=std::uint8_t(i+5);
        }
        for(unsigned n=0;n<slots.size();++n){auto& object=objects[n];object.visible=1;object.shadowEnabled=1;object.scale=3.75f;object.opacity=.37f;
            object.parentModel=-1;object.parentBone=0;object.additiveBlend=std::uint8_t(11+n);object.rowSelected=std::uint8_t(13+n);
            object.reservedFlags=std::uint8_t(29+n);std::memcpy(object.name,n?"follow second":"follow first",n?14:13);
            for(unsigned axis=0;axis<3;++axis){object.position[axis]=float(n+axis)+99.125f;object.rotation[axis]=-.71f+float(axis)*.3f;}
            app->AccessorySlot(slots[n])=&object;plugins::SceneObjectIdentity(&object);app->AccessoryKeys(slots[n])=mdl::KeyPoolCreate<mdl::AccessoryKey>(4);
            for(unsigned i=0;i<3;++i){auto& key=app->AccessoryKeys(slots[n])[i];key.frame=i==0?0:i==1?12:31;key.previous=i?i-1:0;key.next=i<2?i+1:0;
                key.visible=std::uint8_t(i%2);key.shadowEnabled=std::uint8_t((i+1)%2);key.parentModel=i==1?1:-1;key.parentBone=i==1?1:0;
                key.scale=.73f+float(i+n)*.31713f;key.opacity=.17f+float(i+n)*.10917f;key.selected=std::uint8_t(17+n+i);
                for(unsigned axis=0;axis<3;++axis){key.position[axis]=float(n*3+i+axis)*.9173f;key.rotation[axis]=float(i*2+axis)*.1379f;}}
        }
        app->SelectedAccessorySlot()=std::uint8_t(slots[0]);app->ShadowMode()=2;app->ShadowDistance()=.09137f;
        for(unsigned axis=0;axis<3;++axis){app->LightDirection()[axis]=-3.25f+float(axis);app->LightColor()[axis]=.937f-float(axis)*.117f;}
        std::memset(&app->SceneLight(),0,sizeof(D3DLIGHT9));app->SceneLight().Type=D3DLIGHT_POINT;app->SceneLight().Diffuse={.13f,.27f,.39f,.51f};
        app->SceneLight().Specular={.61f,.73f,.89f,.97f};app->SceneLight().Ambient={.31f,.43f,.59f,.67f};
        app->SceneLight().Direction={7.3f,-2.7f,9.1f};app->SceneLight().Range=123.25f;
        app->GravityMagnitude()=19.73f;app->state.gravityX=1.73f;app->state.gravityY=-.291f;app->state.gravityZ=.817f;app->GravityNoise()=79;app->GravityNoiseModeWord()=0xf7e6d501u;
        app->CameraTrackCursor()=2;app->LightTrackCursor()=1;app->ShadowTrackCursor()=2;app->GravityTrackCursor()=1;
        app->CameraTrackActive()=app->LightTrackActive()=app->ShadowTrackActive()=app->GravityTrackActive()=1;
        for(auto slot:slots){app->AccessoryTrackCursor(slot)=2;app->AccessoryTrackActive(slot)=1;}
        for(auto* f:{&first,&second})for(unsigned bone=0;bone<2;++bone){auto& b=f->bones[bone];b.position[0]=2.5f+bone;b.position[1]=7.25f-bone;b.position[2]=-.713f+bone;
            b.matInit[0]=0;b.matInit[1]=1;b.matInit[4]=-1;b.matInit[5]=0;b.matInit[10]=1;b.matInit[12]=3.17f+bone;b.matInit[13]=-.719f+bone;b.matInit[14]=7.31f-bone;}
        first.m->selectedBone=second.m->selectedBone=1;
        Check(CreateWindowExW(0,L"BUTTON",L"follow",WS_CHILD|BS_CHECKBOX,0,0,80,24,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(panel::kFollowCameraCheckbox)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"follow fixture has a real main checkbox");
        floating=CreateWindowExW(0,L"STATIC",L"follow floating",WS_OVERLAPPED,0,0,300,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(floating&&CreateWindowExW(0,L"BUTTON",L"follow",WS_CHILD|BS_CHECKBOX,0,0,80,24,floating,reinterpret_cast<HMENU>(INT_PTR(panel::kFollowCameraCheckbox)),GetModuleHandleW(nullptr),nullptr),"follow fixture has an independent floating checkbox");
        app->FloatingWindow()=floating;menu=CreateMenu();Check(menu&&AppendMenuW(menu,MF_STRING,247,L"follow")&&SetMenu(app->MainWindow(),menu),"follow fixture has the actual 247 menu entry");
        SetFocus(app->MainWindow());
    }
    ~FollowFixture(){history.Reset();history.Collect();curve_host::Reset(app.get());
        for(unsigned n=0;n<slots.size();++n){plugins::ForgetSceneObject(&objects[n]);app->AccessorySlot(slots[n])=nullptr;mdl::KeyPoolDestroy(app->AccessoryKeys(slots[n]));app->AccessoryKeys(slots[n])=nullptr;}
        mdl::KeyPoolDestroy(app->LightKeys());mdl::KeyPoolDestroy(app->ShadowKeys());mdl::KeyPoolDestroy(app->GravityKeys());app->LightKeys()=nullptr;app->ShadowKeys()=nullptr;app->GravityKeys()=nullptr;
        app->FloatingWindow()=nullptr;DestroyWindow(floating);SetMenu(app->MainWindow(),nullptr);DestroyMenu(menu);
    }
    void Context(bool camera,bool initial,int parent,CameraAttachmentReference reference){app->CameraMode()=camera;app->state.followCameraEnabled=std::uint8_t(initial);
        app->CameraReferenceMode()=reference;app->CameraParentModel()=parent;app->CameraParentBone()=1;app->SelectedModelSlot()=0;
        for(unsigned index=0;index<3;++index){app->CameraKeys()[index].parentModel=parent;app->CameraKeys()[index].parentBone=1;}
        CheckDlgButton(app->MainWindow(),panel::kFollowCameraCheckbox,initial?BST_CHECKED:BST_UNCHECKED);CheckDlgButton(floating,panel::kFollowCameraCheckbox,initial?BST_CHECKED:BST_UNCHECKED);
        CheckMenuItem(menu,247,MF_BYCOMMAND|(initial?MF_CHECKED:MF_UNCHECKED));}
    FollowState State()const {auto& a=*app;FollowState value;value.view=NumpadRead(a);value.follow=a.state.followCameraEnabled;value.shadow=std::uint8_t(a.ShadowMode());
        std::memcpy(value.light.data(),a.LightDirection(),3*sizeof(float));std::memcpy(value.light.data()+3,a.LightColor(),3*sizeof(float));value.device=a.SceneLight();value.shadowDistance=a.ShadowDistance();
        value.gravity={a.GravityMagnitude(),a.state.gravityX,a.state.gravityY,a.state.gravityZ};value.noise=a.GravityNoise();value.noiseMode=a.GravityNoiseModeWord();value.accessories=objects;return value;}
    static bool Same(const FollowState& a,const FollowState& b){return NumpadSame(a.view,b.view)&&a.follow==b.follow&&a.shadow==b.shadow&&
        !std::memcmp(a.light.data(),b.light.data(),sizeof a.light)&&!std::memcmp(&a.device,&b.device,sizeof a.device)&&a.shadowDistance==b.shadowDistance&&
        !std::memcmp(a.gravity.data(),b.gravity.data(),sizeof a.gravity)&&a.noise==b.noise&&a.noiseMode==b.noiseMode&&
        !std::memcmp(a.accessories.data(),b.accessories.data(),sizeof a.accessories);}
    std::uint64_t Digest(){auto value=NumpadFixture::Digest();const auto add=[&](const void* p,std::size_t n){const auto* b=static_cast<const unsigned char*>(p);while(n--){value^=*b++;value*=1099511628211ull;}};
        add(&app->SceneLight(),sizeof(D3DLIGHT9));add(objects.data(),sizeof objects);
        add(app->LightKeys(),mdl::KeyPoolCapacity(app->LightKeys())*sizeof(mdl::LightKey));add(app->ShadowKeys(),mdl::KeyPoolCapacity(app->ShadowKeys())*sizeof(mdl::SelfShadowKey));
        add(app->GravityKeys(),mdl::KeyPoolCapacity(app->GravityKeys())*sizeof(mdl::GravityKey));for(auto slot:slots)add(app->AccessoryKeys(slot),mdl::KeyPoolCapacity(app->AccessoryKeys(slot))*sizeof(mdl::AccessoryKey));return value;}
    std::shared_ptr<plugins::HistoryChange> Plan(bool target){SetFocus(app->MainWindow());std::shared_ptr<plugins::HistoryChange> change;Check(plugins::PlanCameraFollow(*app,target,change)==MMS_OK,"prepare complete camera-follow adjustment privately");return change;}
    void Commit(const std::shared_ptr<plugins::HistoryChange>& change){std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(change&&history.PrepareNative("camera follow",change,token,plugins::SharedHistory::NativeIntent::Adjustment)==MMS_OK,"reserve one complete follow adjustment in the shared journal");
        failAfter=0;const auto result=history.CommitNative(std::move(token));failAfter=-1;Check(result==MMS_OK,"publish view global dirty poses and all accessories without allocation");}
    void Legacy(bool target,unsigned entry){auto* saved=g_Block;g_Block=app.get();
        if(entry==2){Check(bool(app->state.followCameraEnabled)!=target,"menu oracle toggles the actual old state");CmdFileMenu(app.get(),app->MainWindow(),247,0);}
        else{const auto window=entry?floating:app->MainWindow();CheckDlgButton(window,panel::kFollowCameraCheckbox,target?BST_CHECKED:BST_UNCHECKED);CmdControl500(app.get(),window,535,0);}g_Block=saved;}
    void Author(){for(unsigned index=0;index<3;++index){auto& key=app->GravityKeys()[index];key.noise=std::abs(key.noise);key.noiseEnabled=std::uint8_t(index%2);}
        for(auto kind:{curve_host::Kind::Camera,curve_host::Kind::Light,curve_host::Kind::Shadow,curve_host::Kind::Gravity,curve_host::Kind::Accessory}){
        for(unsigned accessory=0;accessory<(kind==curve_host::Kind::Accessory?2u:1u);++accessory){const int slot=kind==curve_host::Kind::Accessory?int(slots[accessory]):0;
            std::vector<curve_editor::Channel> channels;std::wstring error;Check(curve_host::Read(*app,kind,slot,channels,error),"read all following global/accessory channels for a sparse authored fixture");
            const unsigned lane=kind==curve_host::Kind::Shadow?1:0;auto& channel=channels[lane];channel.track.keys.erase(channel.track.keys.begin()+1);
            channel.nativeControls.clear();channel.controlGeometry.clear();channel.sample={};
            channel.track.keys.front().value=kind==curve_host::Kind::Shadow?.023456789012:.123456789012;
            channel.track.keys.back().value=kind==curve_host::Kind::Shadow?.076543210987:7.987654321098;
            for(auto& key:channel.track.keys){key.tangent=animation_curve::Tangent::Free;key.interpolation=animation_curve::Interpolation::Bezier;key.inWeight=.31;key.outWeight=.19;
                key.inSlope=kind==curve_host::Kind::Shadow?.00017:-.017;key.outSlope=kind==curve_host::Kind::Shadow?.00013:.013;}
            if(kind==curve_host::Kind::Camera){auto& rotation=channels[4];rotation.nativeControls.clear();rotation.controlGeometry.clear();rotation.sample={};
                rotation.track.keys.back().value=1080.000012;rotation.track.keys.back().interpolation=animation_curve::Interpolation::Step;}
            Check(curve_host::Write(*app,kind,slot,channels,error),"author sparse exact Free values Step and full Euler revolutions without changing the public format");
        }}
    }
};
struct FollowInvariants {
    std::vector<mdl::CameraKey> camera;std::vector<mdl::LightKey> light;std::vector<mdl::SelfShadowKey> shadow;std::vector<mdl::GravityKey> gravity;
    std::array<std::vector<mdl::AccessoryKey>,2> accessories;std::array<std::uint32_t,6> cursor{};std::array<unsigned char,6> active{};
    std::array<mdl::BoneRecord,2> first,second;std::array<unsigned char,2> dirtyFirst,dirtySecond;std::uint32_t last;int frame;
    explicit FollowInvariants(FollowFixture& f):camera(GlobalRegistrationKeys(f.app->CameraKeys())),light(GlobalRegistrationKeys(f.app->LightKeys())),
        shadow(GlobalRegistrationKeys(f.app->ShadowKeys())),gravity(GlobalRegistrationKeys(f.app->GravityKeys())),first(f.first.bones),second(f.second.bones),dirtyFirst(f.first.dirty),dirtySecond(f.second.dirty),last(f.app->LastRegisteredFrame()),frame(f.app->CurrentFrame()){
        cursor={f.app->CameraTrackCursor(),f.app->LightTrackCursor(),f.app->ShadowTrackCursor(),f.app->GravityTrackCursor(),f.app->AccessoryTrackCursor(FollowFixture::slots[0]),f.app->AccessoryTrackCursor(FollowFixture::slots[1])};
        active={f.app->CameraTrackActive(),f.app->LightTrackActive(),f.app->ShadowTrackActive(),f.app->GravityTrackActive(),f.app->AccessoryTrackActive(FollowFixture::slots[0]),f.app->AccessoryTrackActive(FollowFixture::slots[1])};
        for(unsigned n=0;n<accessories.size();++n)accessories[n]=GlobalRegistrationKeys(f.app->AccessoryKeys(FollowFixture::slots[n]));}
    bool Same(FollowFixture& f)const {FollowInvariants current(f);bool same=GlobalRegistrationSame(f.app->CameraKeys(),camera)&&GlobalRegistrationSame(f.app->LightKeys(),light)&&GlobalRegistrationSame(f.app->ShadowKeys(),shadow)&&GlobalRegistrationSame(f.app->GravityKeys(),gravity)&&
        current.cursor==cursor&&current.active==active&&current.last==last&&current.frame==frame&&!std::memcmp(first.data(),current.first.data(),sizeof first)&&!std::memcmp(second.data(),current.second.data(),sizeof second)&&dirtyFirst==current.dirtyFirst&&dirtySecond==current.dirtySecond;
        for(unsigned n=0;n<accessories.size();++n)same&=GlobalRegistrationSame(f.app->AccessoryKeys(FollowFixture::slots[n]),accessories[n]);return same;}
};
void FollowNativeOracleChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(int frame:{0,1,7,12,13,30,31,44})for(bool target:{false,true})for(unsigned entry=0;entry<3;++entry)for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        FollowFixture f,oracle;for(auto* x:{&f,&oracle}){x->app->CurrentFrame()=frame;x->Context(false,!target,0,CameraAttachmentReference::SelectedBone);}Check(f.history.SetMode(mode)==MMS_OK,"configure follow adjustment recording mode");
        const auto before=f.State();const FollowInvariants invariant(f);const auto digest=f.Digest();oracle.Legacy(target,entry);const auto expected=oracle.State();
        auto change=f.Plan(target);Check(change&&f.Digest()==digest&&invariant.Same(f),"follow planning preserves all dirty source poses keys cursors and model transforms");f.Commit(change);
        if(!FollowFixture::Same(f.State(),expected))std::printf("DIAG follow native frame=%d target=%u entry=%u mode=%u\n",frame,unsigned(target),entry,unsigned(mode));
        Check(FollowFixture::Same(f.State(),expected)&&invariant.Same(f),"full follow result matches actual unbound old main checkbox floating checkbox and menu across native intervals");
        Check(f.history.UndoCount()==(mode==Mode::AllOperations?1:0),"complete follow adjustment records once only in all-operations mode");
        if(mode==Mode::AllOperations){f.app->CurrentFrame()=53;f.app->CameraMode()=1;f.app->CameraReferenceMode()=CameraAttachmentReference::None;f.app->SelectedModelSlot()=1;
            Check(f.history.History(false)==MMS_OK&&FollowFixture::Same(f.State(),before)&&f.app->CurrentFrame()==53,"follow undo restores all overwritten unregistered globals/accessories without reevaluating the new frame or reference");
            Check(f.history.History(true)==MMS_OK&&FollowFixture::Same(f.State(),expected)&&f.app->CurrentFrame()==53,"follow redo replays frozen numeric state independently of mode model and playhead");}
    }
}
void FollowReferenceAndRepeatChecks(){
    for(bool camera:{false,true})for(auto reference:{CameraAttachmentReference::None,CameraAttachmentReference::ModelRoot,CameraAttachmentReference::SelectedBone})
        for(int parent:{-1,0,1})for(bool initial:{false,true})for(bool target:{false,true}){
        FollowFixture f,oracle;for(auto* x:{&f,&oracle})x->Context(camera,initial,parent,reference);const auto before=f.State();oracle.Legacy(target,0);const auto expected=oracle.State();auto change=f.Plan(target);
        const bool changed=!FollowFixture::Same(before,expected);Check(bool(change)==changed,"equal follow target still executes the complete original resample and suppression rules");
        if(change)f.Commit(change);Check(FollowFixture::Same(f.State(),expected),"parent basis transposition and focus carry preserve exact old reference and camera/model mode behavior");
    }
}
void FollowAuthoredOracleChecks(){
    for(int frame:{0,7,12,30,44})for(bool target:{false,true}){FollowFixture f,oracle;for(auto* x:{&f,&oracle}){x->Context(false,!target,1,CameraAttachmentReference::ModelRoot);x->Author();x->app->CurrentFrame()=frame;}
        // Native edits have not been reconciled by a public Read/Apply yet.
        f.app->LightKeys()[1].direction[1]+=.113f;oracle.app->LightKeys()[1].direction[1]+=.113f;
        std::vector<std::unique_ptr<curve_host::TrackAuthoring>> raw;for(auto kind:{curve_host::Kind::Camera,curve_host::Kind::Light,curve_host::Kind::Shadow,curve_host::Kind::Gravity})raw.push_back(std::make_unique<curve_host::TrackAuthoring>(*f.app,kind));
        for(auto slot:FollowFixture::slots)raw.push_back(std::make_unique<curve_host::TrackAuthoring>(*f.app,curve_host::Kind::Accessory,int(slot)));
        const auto before=f.State();const auto digest=f.Digest();const FollowInvariants invariant(f);auto change=f.Plan(target);
        Check(change&&f.Digest()==digest&&std::all_of(raw.begin(),raw.end(),[](const auto& author){return author->CapturedCurrent();}),"free follow sampling reconciles on private author seeds without changing raw authored tables");
        oracle.Legacy(target,0);const auto expected=oracle.State();f.Commit(change);
        Check(FollowFixture::Same(f.State(),expected)&&invariant.Same(f)&&std::all_of(raw.begin(),raw.end(),[](const auto& author){return author->CapturedCurrent();}),"sparse Free Step and Euler-winding follow samples match the real old evaluator and preserve exact author metadata");
        f.app->CurrentFrame()=83;Check(f.history.History(false)==MMS_OK&&FollowFixture::Same(f.State(),before)&&f.history.History(true)==MMS_OK&&FollowFixture::Same(f.State(),expected),"authored follow history owns snapshots and never resamples a later scene");
    }
}
void FollowOwnershipAndOfflineChecks(){
    FollowFixture f;f.Context(false,true,0,CameraAttachmentReference::SelectedBone);const auto before=f.State();auto change=f.Plan(false);f.Commit(change);const auto after=f.State();
    f.app->LightColor()[0]=.812341f;f.app->SceneLight().Diffuse.a=.231f;f.app->SceneLight().Specular.a=.347f;f.app->SceneLight().Range=789.25f;
    f.objects[0].additiveBlend=17;f.objects[0].rowSelected=19;f.app->ShadowDistance()=.1234f;f.app->GravityNoiseModeWord()=0xaabbcc01u;
    auto undo=before,redo=after;for(auto* state:{&undo,&redo}){state->light[3]=f.app->LightColor()[0];state->device=f.app->SceneLight();
        state->accessories=f.objects;state->shadowDistance=f.app->ShadowDistance();state->noiseMode=f.app->GravityNoiseModeWord();}
    // Turning follow off owns neither evaluated global/accessory poses nor
    // their source tracks. Replacing an irrelevant source cannot block undo.
    auto* retired=f.app->LightKeys();f.app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(4);mdl::KeyPoolDestroy(retired);
    f.app->CurrentFrame()=91;f.app->SelectedModelSlot()=1;
    Check(f.history.History(false)==MMS_OK&&FollowFixture::Same(f.State(),undo),"disabled follow history preserves later unrelated global accessory device fields and accepts an unrelated pool replacement");
    Check(f.history.History(true)==MMS_OK&&FollowFixture::Same(f.State(),redo)&&f.app->CurrentFrame()==91,"offline disable redo restores only changed view components and follow state");
    // Repeated enabling resamples even when the flag is already enabled.
    FollowFixture repeat;repeat.Context(true,true,-1,CameraAttachmentReference::None);repeat.Commit(repeat.Plan(true));
    Check(repeat.history.History(false)==MMS_OK&&repeat.history.RedoCount()==1,"seed same-target follow redo branch");
    repeat.Legacy(true,0);const auto stable=repeat.Digest();std::shared_ptr<plugins::HistoryChange> noop;
    Check(plugins::PlanCameraFollow(*repeat.app,true,noop)==MMS_OK&&!noop&&repeat.Digest()==stable&&repeat.history.RedoCount()==1,"a truly unchanged full resample keeps the redo branch and creates no duplicate node");
}
void FollowFailureAndStaleChecks(){
    for(unsigned reason=0;reason<12;++reason){FollowFixture f;f.Context(false,false,0,CameraAttachmentReference::SelectedBone);auto change=f.Plan(true);auto* pool=f.app->LightKeys();
        if(reason==0)++f.app->CurrentFrame();if(reason==1)f.app->CameraYaw()+=1;if(reason==2)f.app->CameraKeys()[1].eye[0]+=.1f;
        if(reason==3)f.app->LightKeys()[1].color[0]+=.1f;if(reason==4)f.app->GravityKeys()[1].noise+=1;if(reason==5)f.app->AccessoryKeys(FollowFixture::slots[1])[1].scale+=.1f;
        if(reason==6)f.first.bones[1].matInit[1]+=.1f;if(reason==7)f.first.bones[1].position[0]+=.1f;
        if(reason==8)f.app->SelectedModelSlot()=1;if(reason==9)f.app->CameraReferenceMode()=CameraAttachmentReference::None;
        if(reason==10)f.app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(4);
        if(reason==11){plugins::ForgetSceneObject(&f.objects[1]);plugins::SceneObjectIdentity(&f.objects[1]);}
        const auto digest=f.Digest();std::unique_ptr<plugins::PreparedHistoryChange> token;
        Check(change->Prepare(true,token)==MMS_E_STALE&&!token&&f.Digest()==digest,"changed follow source key parent/reference bone pool identity or reused accessory refuses all initial publication");
        if(reason==10){mdl::KeyPoolDestroy(f.app->LightKeys());f.app->LightKeys()=pool;}
    }
    {
        FollowFixture f;f.Context(false,false,-1,CameraAttachmentReference::None);const auto digest=f.Digest();auto change=f.Plan(true);std::unique_ptr<plugins::PreparedHistoryChange> token;
        Check(change->Prepare(true,token)==MMS_OK&&token->IsCurrent(),"prepare a fully frozen follow publication candidate");f.objects[1].position[0]+=.5f;
        Check(!token->IsCurrent(),"short follow token freezes every overwritten dirty accessory pose");f.objects[1].position[0]-=.5f;token.reset();
        plugins::SharedHistory budget(f.storage,{},8,1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(budget.PrepareNative("follow budget",change,transaction,plugins::SharedHistory::NativeIntent::Adjustment)==MMS_E_MEMORY&&!transaction&&f.Digest()==digest,"insufficient retained history budget refuses before any follow or pose change");
    }
    FollowFixture f;f.Context(false,false,0,CameraAttachmentReference::SelectedBone);f.Author();const auto digest=f.Digest();unsigned failures=0;bool complete=false;
    for(long allocation=0;allocation<8192;++allocation){std::shared_ptr<plugins::HistoryChange> change;std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;failAfter=allocation;
        auto status=plugins::PlanCameraFollow(*f.app,true,change);if(status==MMS_OK)status=plugins::Boundary([&]{return f.history.PrepareNative("follow allocation",change,transaction,plugins::SharedHistory::NativeIntent::Adjustment);});failAfter=-1;
        Check(f.Digest()==digest&&!f.history.UndoCount(),"follow preparation OOM and cancellation preserve all native poses keys object membership and history");
        if(status==MMS_OK){Check(transaction&&failures>20,"follow OOM sweep reaches complete authored camera global and accessory transaction");
            Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"complete authored follow transaction publishes without allocations");complete=true;break;}
        Check(status==MMS_E_MEMORY&&!transaction,"follow preparation allocation failures are explicit and never publish a partial object set");++failures;
    }
    Check(complete,"authored complete follow allocation sweep terminates");
}
void FollowBoundEntranceAndDispatchChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(unsigned entry=0;entry<3;++entry)for(bool target:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool deferred:{false,true}){
        FollowFixture f,oracle;for(auto* x:{&f,&oracle})x->Context(false,!target,0,CameraAttachmentReference::SelectedBone);
        oracle.Legacy(target,entry);const auto expected=oracle.State();const auto before=f.State();plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"configure the actual follow command's adjustment recording mode");SetFocus(f.app->MainWindow());f.Legacy(target,entry);
        Check(FollowFixture::Same(f.State(),expected)&&f.history.UndoCount()==(mode==Mode::AllOperations?1:0),"actual bound main/floating 535 and 247 route the entire original toggle into one controlled adjustment");
        Check(IsDlgButtonChecked(f.app->MainWindow(),panel::kFollowCameraCheckbox)==(target?BST_CHECKED:BST_UNCHECKED)&&
            IsDlgButtonChecked(f.floating,panel::kFollowCameraCheckbox)==(target?BST_CHECKED:BST_UNCHECKED)&&
            bool(GetMenuState(f.menu,247,MF_BYCOMMAND)&MF_CHECKED)==target,"successful follow command synchronizes both checkboxes and the actual menu flag");
        if(mode==Mode::AllOperations){f.app->CurrentFrame()=57;f.app->CameraMode()=1;f.app->SelectedModelSlot()=1;
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,false,deferred)==MMS_OK&&FollowFixture::Same(f.State(),before)&&f.app->CurrentFrame()==57,
                "real guarded object-history undo restores the complete follow transaction under authorized mutation and UI locks");
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,true,deferred)==MMS_OK&&FollowFixture::Same(f.State(),expected)&&f.app->CurrentFrame()==57,
                "real immediate/deferred follow redo replays numeric state without running the old evaluation chain");
            Check(!plugins::NativeObjectOperationActive(f.app.get())&&!plugins::NativeObjectPublicationAuthorized(f.app.get())&&!f.app->SceneMutationInProgress()&&!bake::UiLocked(),
                "follow history dispatch releases operation publication scene and UI gates");}
    }
    for(unsigned entry=0;entry<3;++entry){FollowFixture f;f.Context(false,false,0,CameraAttachmentReference::SelectedBone);plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto before=f.State();f.app->PlaybackActive()=1;f.Legacy(true,entry);
        Check(FollowFixture::Same(f.State(),before)&&!f.history.UndoCount()&&IsDlgButtonChecked(f.app->MainWindow(),panel::kFollowCameraCheckbox)==BST_UNCHECKED&&
            IsDlgButtonChecked(f.floating,panel::kFollowCameraCheckbox)==BST_UNCHECKED&&!(GetMenuState(f.menu,247,MF_BYCOMMAND)&MF_CHECKED),
            "refused actual follow commands restore checkbox/menu presentation and cannot fall through into old writers");f.app->PlaybackActive()=0;
    }
}
void FollowPendingDraftAndRefusalChecks(){
    for(unsigned entry=0;entry<3;++entry){
        FollowFixture f;f.Context(false,false,0,CameraAttachmentReference::SelectedBone);
        plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto edit=f.Edit(panel::kReadoutPosXEdit);
        const auto floatingEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD,0,30,120,20,f.floating,
            reinterpret_cast<HMENU>(INT_PTR(panel::kFovEdit)),GetModuleHandleW(nullptr),nullptr);
        Check(floatingEdit!=nullptr,"add a real detached pending text field");
        const wchar_t draft[]=L"待提交 12.345";
        for(const auto field:{edit,floatingEdit}){SetWindowTextW(field,draft);SendMessageW(field,EM_SETMODIFY,TRUE,0);SendMessageW(field,EM_SETSEL,2,7);}
        SetFocus(edit);const auto focus=GetFocus();const auto before=f.State();
        failAfter=0;f.Legacy(true,entry);failAfter=-1;
        Check(FollowFixture::Same(f.State(),before)&&!f.history.UndoCount()&&!bake::UiLocked()&&GetFocus()==focus&&
            IsDlgButtonChecked(f.app->MainWindow(),panel::kFollowCameraCheckbox)==BST_UNCHECKED&&
            IsDlgButtonChecked(f.floating,panel::kFollowCameraCheckbox)==BST_UNCHECKED&&!(GetMenuState(f.menu,247,MF_BYCOMMAND)&MF_CHECKED),
            "actual follow entry OOM keeps all live data and restores both checkboxes/menu without allocation or focus loss");
        f.Legacy(true,entry);
        Check(f.app->state.followCameraEnabled&&f.history.UndoCount()==1&&!bake::UiLocked()&&GetFocus()==focus,
            "capturing drafts before commit does not hold the edit gate or disable/focus a window");
        for(const auto field:{edit,floatingEdit}){wchar_t text[64]{};GetWindowTextW(field,text,64);DWORD first=0,last=0;
            SendMessageW(field,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
            Check(!std::wcscmp(text,draft)&&SendMessageW(field,EM_GETMODIFY,0,0)&&first==2&&last==7,
                "successful follow refresh preserves Unicode pending text and selection even after its edit lost focus");}
        f.app->PhysicsResetPending()=0;
        Check(plugins::RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&!f.app->PhysicsResetPending(),
            "follow history changes parameters without scheduling a model-physics settle");
    }
}
HWND FollowPendingEdit(MMDApp& app,int id){
    auto edit=GetDlgItem(app.MainWindow(),id);
    if(!edit)edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD,0,50,140,20,app.MainWindow(),
        reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    Check(edit!=nullptr,"create an independent pending native readout");
    SetWindowTextW(edit,L"待提交 12.345");SendMessageW(edit,EM_SETMODIFY,TRUE,0);SendMessageW(edit,EM_SETSEL,2,7);
    // A deferred host refresh must also retain drafts whose edit lost focus.
    SetFocus(app.MainWindow());return edit;
}
bool FollowHasPendingEdit(HWND edit){
    wchar_t text[64]{};GetWindowTextW(edit,text,64);DWORD first=0,last=0;
    SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
    return !std::wcscmp(text,L"待提交 12.345")&&SendMessageW(edit,EM_GETMODIFY,0,0)&&first==2&&last==7;
}
void FollowOrdinaryReadoutOomChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        {
            ShadowSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            f.Scroll(8765);Check(f.history.History(false)==MMS_OK&&f.history.SetMode(mode)==MMS_OK,"seed common shadow redo before an unrelated pending draft");
            const auto before=f.app->ShadowDistance();const auto marks=AccessoryRegistrationMarks(*f.app);const auto ring=PoseRingDigest(f.first);
            const auto lightDraft=FollowPendingEdit(*f.app,461);const auto poseDraft=FollowPendingEdit(*f.app,panel::kReadoutPosXEdit);
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
            failAfter=0;f.Scroll(4321);failAfter=-1;
            Check(f.app->ShadowDistance()==before&&!f.history.UndoCount()&&f.history.RedoCount()==1&&f.Echo(1,ShadowSliderPosition(f.app.get()))&&
                !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&AccessoryRegistrationMarks(*f.app)==marks&&PoseRingDigest(f.first)==ring&&
                FollowHasPendingEdit(lightDraft)&&FollowHasPendingEdit(poseDraft)&&!bake::UiLocked(),
                "shadow rejection restores the moved thumb under immediate OOM while unrelated light and pose drafts retain text selection and redo");
            const auto sibling=GetDlgItem(f.app->MainWindow(),panel::kReadoutPosYEdit);
            Check(sibling!=nullptr,"pose readout OOM fixture has an independent unmodified sibling");
            SetWindowTextA(sibling,"stale sibling");SendMessageW(sibling,EM_SETMODIFY,FALSE,0);f.app->CameraMode()=1;
            failAfter=0;plugins::RefreshBoundPoseReadout(*f.app);failAfter=-1;
            char actual[64]{};GetWindowTextA(sibling,actual,sizeof(actual));
            Check(FollowHasPendingEdit(poseDraft)&&std::strcmp(actual,"stale sibling")&&FollowHasPendingEdit(lightDraft),
                "allocation-free pose refresh keeps a lost-focus draft while still refreshing its unmodified sibling");
        }
        {
            FovSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            f.Scroll(67);Check(f.history.History(false)==MMS_OK&&f.history.SetMode(mode)==MMS_OK,"seed FOV redo before an unrelated pending draft");
            const auto before=f.app->CameraFov();const auto draft=FollowPendingEdit(*f.app,461);
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
            failAfter=0;f.Scroll(91);failAfter=-1;
            Check(f.app->CameraFov()==before&&!f.history.UndoCount()&&f.history.RedoCount()==1&&f.Echo(before)&&FollowHasPendingEdit(draft)&&
                !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
                "FOV rejection still restores canonical text and thumb without allocation when another panel contains a pending draft");
        }
        for(unsigned field:{0u,3u}){
            LightSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            f.Scroll(field,field<3?123:-53);Check(f.history.History(false)==MMS_OK&&f.history.SetMode(mode)==MMS_OK,"seed light redo before a sibling pending draft");
            const auto before=LightTextPose(*f.app);const auto draft=FollowPendingEdit(*f.app,461+int((field+1)%6));
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
            failAfter=0;f.Scroll(field,field<3?171:-67);failAfter=-1;
            Check(LightTextPose(*f.app)==before&&!f.history.UndoCount()&&f.history.RedoCount()==1&&LightTextMirror(*f.app)&&FollowHasPendingEdit(draft)&&
                SendMessageW(f.Slider(field),TBM_GETPOS,0,0)==f.Canonical(field,before[field])&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
                "RGB and direction rejection restore the moved thumb under OOM without replacing a sibling pending edit");
        }
        {
            MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            f.Scroll(1,75);Check(f.history.History(false)==MMS_OK&&f.history.SetMode(mode)==MMS_OK,"seed morph redo before a different-lane pending draft");
            const auto before=f.firstMorphs;const auto draft=FollowPendingEdit(*f.app,506+2*5);
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
            failAfter=0;f.Scroll(1,53);failAfter=-1;
            Check(!std::memcmp(before.data(),f.firstMorphs.data(),sizeof(before))&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
                f.Thumb(1,before[f.first.m->selectedMorphs[1]].value)&&FollowHasPendingEdit(draft)&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
                "morph rejection keeps another lane draft and redo while restoring the moved thumb without allocation");
        }
    }
}
void FollowProgrammaticReadoutChecks(){
    LightSliderFixture f;
    for(unsigned axis=0;axis<3;++axis){f.app->LightKeys()[0].direction[axis]=.1725f+float(axis)*.2f;
        f.app->LightKeys()[0].color[axis]=.1325f+float(axis)*.15f;}
    RefreshLightPanel(f.app.get());
    for(unsigned field=0;field<6;++field)
        Check(!SendMessageW(f.Field(field),EM_GETMODIFY,0,0),"unbound timeline light output is marked as programmatic before a later history binding");
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    for(unsigned axis=0;axis<3;++axis){f.app->LightDirection()[axis]=-.7135f+float(axis)*.1f;f.app->LightColor()[axis]=.8125f-float(axis)*.2f;}
    f.app->ApplyTimelineLightState();plugins::RefreshBoundGlobalReadout(*f.app);
    for(unsigned field=0;field<6;++field){char expected[64]{},actual[64]{};
        if(field<3)std::snprintf(expected,sizeof(expected),"%3d",int(double(f.app->LightColor()[field])*256.0));
        else std::snprintf(expected,sizeof(expected),"%+.1f",double(f.app->LightDirection()[field-3]));
        GetWindowTextA(f.Field(field),actual,sizeof(actual));
        Check(!std::strcmp(actual,expected)&&!SendMessageW(f.Field(field),EM_GETMODIFY,0,0),
            "binding after actual unbound light refresh permits new live readouts instead of freezing startup output as a draft");}
    const auto draft=FollowPendingEdit(*f.app,462);plugins::RefreshBoundGlobalReadout(*f.app);
    Check(FollowHasPendingEdit(draft)&&!f.history.UndoCount()&&!f.history.RedoCount(),
        "programmatic output classification does not discard a genuine later draft or create history");
}
void FollowDeferredAccessoryDraftChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        AccessorySettingsFixture x;auto& f=x.f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        x.Fill(false);const auto originalName=text_encoding::Display(f.app->AccessorySlot(0)->name);
        const auto list=GetDlgItem(x.dialog,panel::kOrderListBox);
        Check(SendMessageW(list,LB_DELETESTRING,0,0)!=LB_ERR&&
            SendMessageW(list,LB_INSERTSTRING,0,reinterpret_cast<LPARAM>(L"Renamed A"))==0,
            "edit only accessory A metadata in its private settings list");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.SetMode(mode)==MMS_OK&&x.Prepare(transaction)==MMS_OK&&
            CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,
            "publish one explicit accessory A rename before entering an accessory B draft");
        f.app->AccessoryOrderArray()=nullptr;f.app->SelectedAccessorySlot()=1;
        Check(scene_history_ui::RefreshObjectLists(*f.app,scene_history_ui::CaptureSelection(*f.app),true,false)==scene_history_ui::Result::Ok,
            "accessory B remains the selected editor while A owns the history step");
        const auto before=*f.app->AccessorySlot(1);const auto draft=FollowPendingEdit(*f.app,478);
        const auto sibling=GetDlgItem(f.app->MainWindow(),479);SetWindowTextA(sibling,"stale sibling");SendMessageW(sibling,EM_SETMODIFY,FALSE,0);
        for(bool redo:{false,true}){
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,redo,true)==MMS_OK&&plugins::NativeObjectUiPending(f.app.get())&&
                FollowHasPendingEdit(draft),"deferred metadata traversal retains B's text before the queued object-list refresh");
            Check(plugins::RefreshBoundObjectUi(*f.app,f.history)&&!plugins::NativeObjectUiPending(f.app.get()),
                "run the real deferred object-list and accessory readout refresh");
            char text[64]{};GetWindowTextA(sibling,text,sizeof(text));
            Check(text_encoding::Display(f.app->AccessorySlot(0)->name)==(redo?L"Renamed A":originalName)&&
                f.app->SelectedAccessorySlot()==1&&!std::memcmp(&before,f.app->AccessorySlot(1),sizeof(before))&&
                FollowHasPendingEdit(draft)&&std::strtod(text,nullptr)==double(before.position[1])&&!bake::UiLocked(),
                "deferred rename undo and redo refresh A's list label and B's unmodified sibling without replacing B's pending draft");
        }
    }
}
void FollowParentOwnershipChecks(){
    for(bool cameraParent:{false,true}){
        FollowFixture f;f.Context(false,false,cameraParent?0:-1,CameraAttachmentReference::None);
        if(cameraParent){for(unsigned i=0;i<3;++i)f.app->CameraKeys()[i].parentModel=1;}
        else{f.objects[0].parentModel=0;f.objects[0].parentBone=1;for(unsigned i=0;i<3;++i){auto& key=f.app->AccessoryKeys(FollowFixture::slots[0])[i];key.parentModel=1;key.parentBone=1;}}
        auto change=f.Plan(true);Check(change!=nullptr,"prepare follow across two different before/after parent owners");f.Commit(change);
        const auto before=f.State();plugins::ForgetSceneObject(f.first.raw());plugins::SceneObjectIdentity(f.first.raw());
        Check(f.history.History(false)==MMS_E_STALE&&FollowFixture::Same(f.State(),before)&&f.history.UndoCount()==1,
            "old dirty camera/accessory parent stable identity remains protected after sampling installs a different parent");
    }
}
void FollowNativeNoiseBoundaryChecks(){
    for(const auto values:{std::pair<int,int>{INT32_MAX,INT32_MIN},{INT32_MIN,INT32_MAX},{INT32_MAX,0},{0,INT32_MIN}})
        for(int frame:{1,7,11}){
            FollowFixture f,oracle;for(auto* fixture:{&f,&oracle}){
                fixture->Context(false,false,-1,CameraAttachmentReference::None);fixture->app->CurrentFrame()=frame;
                fixture->app->GravityKeys()[0].noise=values.first;fixture->app->GravityKeys()[1].noise=values.second;}
            oracle.Legacy(true,0);const auto expected=oracle.State();auto change=f.Plan(true);f.Commit(change);
            Check(FollowFixture::Same(f.State(),expected),"signed-noise endpoints retain the original 32-bit subtraction/addition and float interpolation");
        }
}
void FollowEnableUnchangedFieldOwnershipChecks(){
    FollowFixture f,oracle;for(auto* fixture:{&f,&oracle})fixture->Context(false,false,-1,CameraAttachmentReference::None);
    oracle.Legacy(true,0);const auto expected=oracle.State();
    f.app->LightColor()[1]=expected.light[4];f.app->SceneLight().Specular.g=expected.device.Specular.g;f.objects[0].scale=expected.accessories[0].scale;
    auto before=f.State();f.Commit(f.Plan(true));f.app->LightColor()[1]=.814123f;f.app->SceneLight().Specular.g=.713123f;f.objects[0].scale=9.713f;
    before.light[4]=f.app->LightColor()[1];before.device.Specular.g=f.app->SceneLight().Specular.g;before.accessories[0].scale=f.objects[0].scale;
    Check(f.history.History(false)==MMS_OK&&FollowFixture::Same(f.State(),before),
        "enabling follow records only fields which changed, preserving later unrelated edits even inside sampled objects");
}
void FollowRetainedOwnerBudgetChecks(){
    FollowFixture f;f.Context(false,false,0,CameraAttachmentReference::SelectedBone);f.app->CurrentFrame()=13;
    for(auto& object:f.objects){object.parentModel=0;object.parentBone=1;}
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    std::shared_ptr<plugins::HistoryChange> seed;
    Check(plugins::PlanCameraRotationReset(*f.app,seed)==MMS_OK&&seed,"prepare an independent small camera redo node");f.Commit(seed);
    Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"keep independent redo while measuring complete follow ownership");
    auto change=f.Plan(true);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change&&change->Prepare(true,candidate)==MMS_OK&&candidate,"prepare the full dynamic retained-owner target");
    plugins::resource_budget::Collector expected;
    for(unsigned slot:{0u,1u})Check(plugins::RetainSceneObject(*f.app,MMS_OBJECT_MODEL,slot)->MeasureBookkeeping(expected)==MMS_OK,
        "measure shared old/new parent owner bookkeeping once");
    for(unsigned slot:FollowFixture::slots)Check(plugins::RetainSceneObject(*f.app,MMS_OBJECT_ACCESSORY,slot)->MeasureBookkeeping(expected)==MMS_OK,
        "measure each accessory's real parked key and authoring storage");
    std::uint64_t target=0;Check(expected.Result().Complete()&&candidate->TargetRetainedBytes(target)&&
        target==change->RetainedBytes()+expected.Result().totalBytes&&expected.Result().totalBytes>=2ull*10000*sizeof(mdl::AccessoryKey),
        "one candidate ledger includes actual parked pools and deduplicates repeated parent holders across the complete follow change");
    failAfter=0;const bool current=candidate->IsCurrent();failAfter=-1;
    Check(current,"prepared follow verifies retained allocation identities and capacities without allocation");candidate.reset();
    const auto reducedBudget=change->RetainedBytes()+expected.Result().totalBytes/2;
    Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations,128,reducedBudget)==MMS_OK&&f.history.RedoCount()==1,
        "set a budget which fits the follow scalars and independent redo but cannot retain the real object holders");
    f.app->SceneModified()=f.app->RecordedFrameCountByte1()=f.app->PhysicsResetPending()=0;
    const auto before=f.State();const auto digest=f.Digest();f.Legacy(true,0);
    Check(FollowFixture::Same(f.State(),before)&&f.Digest()==digest&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
        !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&!f.app->PhysicsResetPending()&&
        IsDlgButtonChecked(f.app->MainWindow(),panel::kFollowCameraCheckbox)==BST_UNCHECKED,
        "real follow refuses a budget below retained owner cost without publishing dirty state poses keys or truncating independent redo");
    Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations,128,target+1024*1024)==MMS_OK,
        "raise the configured limit to fit the exact follow payload");f.Legacy(true,0);
    Check(f.app->state.followCameraEnabled&&f.history.UndoCount()==1&&!f.history.RedoCount()&&f.app->SceneModified(),
        "the same follow operation succeeds after increasing the budget instead of being permanently disabled");
}
void NativeCameraFollowChecks(){
    std::puts("  camera follow: complete unbound legacy entrance parity");FollowNativeOracleChecks();
    std::puts("  camera follow: reference parent repeat and exact authoring");FollowReferenceAndRepeatChecks();FollowAuthoredOracleChecks();
    std::puts("  camera follow: field ownership offline replay source guards and allocation atomicity");FollowOwnershipAndOfflineChecks();FollowFailureAndStaleChecks();
    std::puts("  camera follow: actual bound entrances and authorized immediate/deferred history");FollowBoundEntranceAndDispatchChecks();
    std::puts("  camera follow: pending Unicode drafts OOM presentation and physics isolation");FollowPendingDraftAndRefusalChecks();FollowOrdinaryReadoutOomChecks();FollowProgrammaticReadoutChecks();FollowDeferredAccessoryDraftChecks();
    std::puts("  camera follow: both parent lifetimes native noise limits and unchanged-field ownership");FollowParentOwnershipChecks();FollowNativeNoiseBoundaryChecks();FollowEnableUnchangedFieldOwnershipChecks();
    std::puts("  camera follow: actual retained-owner budget and allocation-free ledger validation");FollowRetainedOwnerBudgetChecks();
}
}
