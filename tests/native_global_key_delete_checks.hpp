// Include after native_global_registration_checks.inc in the native history
// regression. Uses the real private plans, journal and bound command entry.
#pragma once
#include "../src/plugins/native_global_key_delete.hpp"
#include "../src/plugins/native_key_commands.hpp"
#include "mikumikustudio/animation_curve_motion.hpp"
#include <stdexcept>
#include <cwchar>
namespace global_key_delete_checks {
using namespace mikumikustudio;
using Kind=curve_host::Kind;
using Mode=plugins::SharedHistory::RecordingMode;
template<class K>void Clear(K* keys){for(std::size_t i=0;i<mdl::KeyPoolCapacity(keys);++i)keys[i].selected=0;}
template<class K>void Chain(K* keys){for(unsigned i=0;i<3;++i){keys[i].frame=i==0?0:i==1?12:31;keys[i].previous=i?i-1:0;keys[i].next=i<2?i+1:0;}}
struct Fixture:GlobalRegistrationFixture{
    std::array<mdl::AccessoryRecord,2> accessories{};
    Fixture(){
        ClearMarks();Chain(app->GravityKeys());
        for(unsigned i=0;i<3;++i){auto& key=app->GravityKeys()[i];key.acceleration=12.f+i;key.noise=13+int(i);
            key.direction[0]=.1f*i;key.direction[1]=-1.f;key.noiseEnabled=std::uint8_t(i%2);key.reserved1[0]=0x31;}
        app->GravityMagnitude()=42.125f;app->GravityDirection()[1]=-.75f;app->GravityNoise()=25;app->GravityNoiseModeWord()=0x12345601;
        for(unsigned i=0;i<2;++i){auto& object=accessories[i];app->AccessorySlot(7+i)=&object;plugins::SceneObjectIdentity(&object);
            object.parentModel=-1;object.scale=1.2345f;object.opacity=.4567f;object.position[0]=99.f+float(i);
            object.rotation[1]=.314159f;object.visible=1;object.additiveBlend=7;object.rowSelected=std::uint8_t(i+1);
            auto* keys=app->AccessoryKeys(7+i);Chain(keys);
            for(unsigned j=0;j<3;++j){auto& key=keys[j];key.parentModel=-1;key.parentBone=0;key.position[0]=float(j+i)*.5f;
                key.rotation[1]=float(j)*.2f;key.scale=1.f+float(j)*.125f;key.opacity=.8f-float(j)*.1f;key.visible=1;key.shadowEnabled=std::uint8_t(j%2);}
        }
    }
    ~Fixture(){history.Reset();history.Collect();for(unsigned i=0;i<2;++i){plugins::ForgetSceneObject(&accessories[i]);app->AccessorySlot(7+i)=nullptr;}}
    void ClearMarks(){Clear(app->CameraKeys());Clear(app->LightKeys());Clear(app->ShadowKeys());Clear(app->GravityKeys());for(unsigned i=0;i<255;++i)Clear(app->AccessoryKeys(i));}
    void Mark(unsigned index){app->CameraKeys()[index].selected=3;app->LightKeys()[index].selected=4;
        app->ShadowKeys()[index].selected=5;app->GravityKeys()[index].selected=6;
        app->AccessoryKeys(7)[index].selected=7;app->AccessoryKeys(8)[index].selected=8;}
    std::uint64_t FullDigest()const{auto value=Digest();for(const auto& a:accessories){const auto* bytes=reinterpret_cast<const unsigned char*>(&a);
        for(std::size_t i=0;i<sizeof(a);++i){value^=bytes[i];value*=1099511628211ull;}}return value;}
};
struct Keys{
    std::vector<mdl::CameraKey> camera;std::vector<mdl::LightKey> light;std::vector<mdl::SelfShadowKey> shadow;
    std::vector<mdl::GravityKey> gravity;std::array<std::vector<mdl::AccessoryKey>,255> accessories;
    explicit Keys(MMDApp& app):camera(GlobalRegistrationKeys(app.CameraKeys())),light(GlobalRegistrationKeys(app.LightKeys())),
        shadow(GlobalRegistrationKeys(app.ShadowKeys())),gravity(GlobalRegistrationKeys(app.GravityKeys())){
        for(unsigned i=0;i<255;++i)accessories[i]=GlobalRegistrationKeys(app.AccessoryKeys(i));}
    bool Same(MMDApp& app)const{if(!GlobalRegistrationSame(app.CameraKeys(),camera)||!GlobalRegistrationSame(app.LightKeys(),light)||
        !GlobalRegistrationSame(app.ShadowKeys(),shadow)||!GlobalRegistrationSame(app.GravityKeys(),gravity))return false;
        for(unsigned i=0;i<255;++i)if(!GlobalRegistrationSame(app.AccessoryKeys(i),accessories[i]))return false;return true;}
};
std::array<double,34> Pose(Fixture& f){std::array<double,34> result{};unsigned n=0;
    for(unsigned i=0;i<3;++i){result[n++]=f.app->CameraPositionAxis(i);result[n++]=f.app->CameraRotation()[i];}
    result[n++]=f.app->CameraDistance();result[n++]=f.app->CameraFov();result[n++]=f.app->CameraParentModel();result[n++]=f.app->CameraParentBone();
    for(unsigned i=0;i<3;++i){result[n++]=f.app->LightDirection()[i];result[n++]=f.app->LightColor()[i];}
    result[n++]=f.app->ShadowDistance();result[n++]=f.app->ShadowMode();result[n++]=f.app->GravityMagnitude();
    for(unsigned i=0;i<3;++i)result[n++]=f.app->GravityDirection()[i];
    result[n++]=f.app->GravityNoise();result[n++]=f.app->GravityNoiseModeWord();
    for(const auto& a:f.accessories){result[n++]=a.position[0];result[n++]=a.rotation[1];result[n++]=a.scale;result[n++]=a.opacity;}
    return result;
}
std::shared_ptr<plugins::HistoryChange> Plan(Fixture& f,bool fallback=false){std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanGlobalKeyDeletion(*f.app,change,fallback)==MMS_OK,"global deletion privately plans all selected families");return change;}
void Commit(Fixture& f,const std::shared_ptr<plugins::HistoryChange>& change){std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("delete all global families",change,token,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,
        "five-family deletion reserves one explicit journal node");
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(result==MMS_OK,"five-family deletion publishes without allocation");}
void AllFamilies(){
    for(const auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(const unsigned index:{0u,1u,2u}){
        Fixture f;Check(f.history.SetMode(mode)==MMS_OK,"configure global deletion recording mode");f.Mark(index);
        const Keys before(*f.app);const auto pose=Pose(f);const auto objects=f.accessories;const auto digest=f.FullDigest();
        auto change=Plan(f);Check(change&&f.FullDigest()==digest,"delete planning preserves pools, objects and unregistered poses");
        Commit(f,change);Check(f.history.UndoCount()==1&&Pose(f)==pose&&!std::memcmp(objects.data(),f.accessories.data(),sizeof(objects)),
            "both modes record all five families and two accessories once while retaining unregistered poses");
        if(index){Check(f.app->CameraKeys()[index].frame==0&&f.app->LightKeys()[index].frame==0&&
            f.app->ShadowKeys()[index].frame==0&&f.app->GravityKeys()[index].frame==0&&
            f.app->AccessoryKeys(7)[index].frame==0&&f.app->AccessoryKeys(8)[index].frame==0,
            "one delete unlinks every selected nonzero key family and accessory");
            Check(f.app->CameraKeys()[0].next==(index==1?2:1)&&f.app->CameraKeys()[index==1?2:1].next==0,
                "nonzero deletion preserves a valid zero-headed native list");
        }else{const auto& c=f.app->CameraKeys()[0];const auto& l=f.app->LightKeys()[0];const auto& g=f.app->GravityKeys()[0];
            Check(c.frame==0&&c.next==1&&c.distance==-45.f&&c.eye[1]==10.f&&c.parentModel==-1&&
                c.fov==before.camera[0].fov&&c.perspective==before.camera[0].perspective&&
                c.interpolation[0][0]==20&&c.interpolation[3][5]==107,
                "zero camera deletion resets native root payload and 24 controls while preserving FOV and perspective");
            Check(l.direction[0]==-.5f&&l.direction[1]==-1.f&&l.direction[2]==.5f&&l.color[0]==.602f&&
                f.app->ShadowKeys()[0].mode==1&&f.app->ShadowKeys()[0].distance==.01125f&&
                g.acceleration==9.8000002f&&g.noise==10&&!g.noiseEnabled&&g.reserved1[0]==before.gravity[0].reserved1[0],
                "zero light, shadow and gravity deletion use exact retained native defaults and reserved bytes");
            for(unsigned slot:{7u,8u}){const auto& a=f.app->AccessoryKeys(slot)[0];Check(a.next==1&&a.parentModel==-1&&a.parentBone==0&&a.scale==1&&
                a.opacity==1&&a.visible==1&&!a.shadowEnabled&&!a.position[0]&&!a.rotation[1],"zero accessory deletion resets each selected root without deleting its head");}
        }
        const Keys after(*f.app);f.app->CurrentFrame()=23;f.app->CameraMode()=0;f.app->SelectedAccessorySlot()=42;
        Check(f.history.History(false)==MMS_OK&&before.Same(*f.app)&&Pose(f)==pose&&f.app->CurrentFrame()==23,
            "global delete undo restores every original key and exact marks after mode, selection and playhead changes");
        Check(f.history.History(true)==MMS_OK&&after.Same(*f.app)&&Pose(f)==pose&&f.app->CurrentFrame()==23,
            "global delete redo restores all deleted payloads without sampling over an unregistered pose");
    }
}
void BoundCommandAndFallback(){
    for(const auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        Fixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);f.Mark(1);
        const Keys before(*f.app);const auto pose=Pose(f);Check(plugins::TryDeleteBoundKeys(f.app.get())&&f.history.UndoCount()==1&&f.app->SceneModified()&&
            f.app->PhysicsResetPending()&&Pose(f)==pose,"actual bound global delete command publishes one explicit node and safe derived refresh");
        Check(f.history.History(false)==MMS_OK&&before.Same(*f.app),"bound command is backed by the common journal");
        f.ClearMarks();f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;const auto digest=f.FullDigest();
        Check(plugins::TryDeleteBoundKeys(f.app.get())&&f.history.RedoCount()==1&&f.FullDigest()==digest,"bound no-mark delete preserves redo and does not enter legacy writes");
        f.app->CurrentFrame()=12;f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Camera);f.accessories[1].rowSelected=0;
        const auto oldLight=f.app->LightKeys()[1];const auto oldSecond=f.app->AccessoryKeys(8)[1];
        Check(plugins::TryDeleteBoundKeys(f.app.get(),true)&&f.app->CameraKeys()[1].frame==0&&f.app->AccessoryKeys(7)[1].frame==0&&
            !std::memcmp(&oldLight,&f.app->LightKeys()[1],sizeof(oldLight))&&!std::memcmp(&oldSecond,&f.app->AccessoryKeys(8)[1],sizeof(oldSecond)),
            "current-frame fallback deletes only selected global/accessory rows with no explicit marks");
        Check(f.history.History(false)==MMS_OK,"current-frame fallback remains a single reversible operation");
        f.ClearMarks();f.app->CurrentFrame()=0;const auto zero=f.FullDigest();
        Check(plugins::TryDeleteBoundKeys(f.app.get(),true)&&f.FullDigest()==zero,"unmarked zero-frame fallback cannot reset every default head");
        f.app->CurrentFrame()=12;f.app->LightKeys()[2].selected=1;const auto camera=f.app->CameraKeys()[1];
        Check(plugins::TryDeleteBoundKeys(f.app.get(),true)&&!f.app->LightKeys()[2].frame&&
            !std::memcmp(&camera,&f.app->CameraKeys()[1],sizeof(camera)),"explicit marks take precedence over current-row fallback without deleting unmarked families");
    }
}
void FailureAndIdentity(){
    Fixture f;f.Mark(1);const auto digest=f.FullDigest();std::shared_ptr<plugins::HistoryChange> change;unsigned failures=0;
    for(long allowance=0;allowance<2048;++allowance){failAfter=allowance;const auto result=plugins::PlanGlobalKeyDeletion(*f.app,change);failAfter=-1;
        Check(f.FullDigest()==digest,"global delete allocation refusal never partially changes a family or unregistered pose");
        if(result==MMS_OK){Check(change!=nullptr,"complete global deletion plan exists after allocation sweep");break;}
        Check(result==MMS_E_MEMORY&&!change,"global delete allocation failure reports memory and no partial plan");++failures;
    }
    Check(change&&failures>10,"global deletion allocation sweep exercises multiple family and author stages");
    bool ready=false;for(long allowance=0;allowance<4096;++allowance){std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=allowance;
        const auto result=change->Prepare(true,candidate);failAfter=-1;Check(f.FullDigest()==digest,"prepared global delete cancellation leaves every family untouched");
        if(result==MMS_OK){Check(candidate&&candidate->IsCurrent(),"all-family prepared candidate is current");ready=true;break;}
        Check(result==MMS_E_MEMORY&&!candidate,"candidate allocation failure cannot publish an earlier family");}
    Check(ready,"five-family candidate preparation eventually succeeds");
    Check(f.history.Configure(Mode::ExplicitOnly,32,1)==MMS_OK,"configure tiny explicit deletion budget");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("over-budget delete",change,token)!=MMS_OK&&f.FullDigest()==digest&&!f.history.UndoCount(),
        "whole global deletion exceeding shared budget changes no key or object");
    Check(f.history.Configure(Mode::AllOperations,32,64ull*1024*1024)==MMS_OK,"restore journal budget");
    auto saved=f.app->LightKeys()[2];f.app->LightKeys()[2].color[0]+=.01f;std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate,"changed unselected dependency rejects the complete planned deletion");f.app->LightKeys()[2]=saved;
    f.app->AccessorySlot(8)=nullptr;Check(change->Prepare(true,candidate)==MMS_E_STALE,"missing second accessory refuses all families before publication");f.app->AccessorySlot(8)=&f.accessories[1];
    Check(change->Prepare(true,candidate)==MMS_OK&&candidate,"restored dependencies permit a detached candidate");
    f.app->GravityKeys()[2].noise++;Check(!candidate->IsCurrent(),"a later family mutation invalidates already-prepared aggregate before any write");f.app->GravityKeys()[2].noise--;
    candidate.reset();const auto identity=mdl::KeyPoolIdentity(f.app->CameraKeys());Check(mdl::KeyPoolEnsure(f.app->CameraKeys(),20)&&
        mdl::KeyPoolIdentity(f.app->CameraKeys())==identity,"same-identity pool growth occurs between plan and commit");
    Commit(f,change);Check(f.history.History(false)==MMS_OK,"global deletion remains reversible across identity-preserving pool growth");
    auto* old=f.app->AccessoryKeys(8);auto* replacement=mdl::KeyPoolCreate<mdl::AccessoryKey>(mdl::KeyPoolCapacity(old));
    std::memcpy(replacement,old,mdl::KeyPoolCapacity(old)*sizeof(*old));f.app->AccessoryKeys(8)=replacement;
    const auto changed=f.FullDigest();Check(f.history.History(true)==MMS_E_STALE&&f.FullDigest()==changed,"same-byte replacement accessory pool cannot redirect retained redo");
    f.app->AccessoryKeys(8)=old;mdl::KeyPoolDestroy(replacement);
}
std::vector<curve_editor::Channel> Read(Fixture& f,Kind kind,int slot=0){std::vector<curve_editor::Channel> channels;std::wstring error;
    Check(curve_host::Read(*f.app,kind,slot,channels,error),"read exact deletion author source");return channels;}
std::vector<std::uint8_t> Encoded(const std::vector<curve_editor::Channel>& channels){std::vector<animation_curve::NamedTrack> tracks;
    for(const auto& c:channels)tracks.push_back({c.id,c.track});std::vector<std::uint8_t> bytes;std::string error;
    Check(animation_curve::Encode(tracks,bytes,error),"encode original author keys and Free handles");return bytes;}
bool Has(const curve_editor::Channel& channel,int frame){return std::any_of(channel.track.keys.begin(),channel.track.keys.end(),[&](const auto& key){return key.frame==frame;});}
bool Same(const std::vector<curve_editor::Channel>& a,const std::vector<curve_editor::Channel>& b){if(Encoded(a)!=Encoded(b)||a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(a[i].nativeControls!=b[i].nativeControls||a[i].controlGeometry!=b[i].controlGeometry||
        a[i].preservedCameraIntervals!=b[i].preservedCameraIntervals)return false;return true;}
void DiagnoseChannel(const curve_editor::Channel& actual,const curve_editor::Channel& expected,unsigned lane){
    if(Same({actual},{expected}))return;
    std::printf("DIAG root-reset lane=%u track=%u nativeControls=%u controlGeometry=%u preserved=%u keys=%zu/%zu controls=%zu/%zu geometry=%zu/%zu intervals=%zu/%zu\n",
        lane,unsigned(Encoded({actual})==Encoded({expected})),unsigned(actual.nativeControls==expected.nativeControls),
        unsigned(actual.controlGeometry==expected.controlGeometry),unsigned(actual.preservedCameraIntervals==expected.preservedCameraIntervals),
        actual.track.keys.size(),expected.track.keys.size(),actual.nativeControls.size(),expected.nativeControls.size(),
        actual.controlGeometry.size(),expected.controlGeometry.size(),actual.preservedCameraIntervals.size(),expected.preservedCameraIntervals.size());
    if(!actual.nativeControls.empty()&&!expected.nativeControls.empty()){
        const auto& a=actual.nativeControls.front();const auto& b=expected.nativeControls.front();
        std::printf("DIAG root-reset lane=%u actual-root-controls=(%.17g,%.17g,%.17g,%.17g) expected=(%.17g,%.17g,%.17g,%.17g)\n",
            lane,a[0],a[1],a[2],a[3],b[0],b[1],b[2],b[3]);
    }
    std::fflush(stdout);
}
void SparseAndRootAuthoring(){
    for(const auto kind:{Kind::Camera,Kind::Light,Kind::Shadow,Kind::Gravity,Kind::Accessory}){
        Fixture f;const int slot=kind==Kind::Accessory?7:0;auto channels=Read(f,kind,slot);const unsigned lane=kind==Kind::Shadow?1:0;
        auto& keys=channels[lane].track.keys;keys.erase(std::remove_if(keys.begin(),keys.end(),[](const auto& key){return key.frame==12;}),keys.end());
        keys.front().outSlope=.0013;keys.front().outWeight=.19;keys.front().tangent=animation_curve::Tangent::Free;
        keys.back().inSlope=-.0017;keys.back().inWeight=.29;keys.back().tangent=animation_curve::Tangent::Free;keys.back().interpolation=animation_curve::Interpolation::Bezier;
        std::wstring error;Check(curve_host::Write(*f.app,kind,slot,channels,error),"author sparse deletion lane and exact Free handles");
        const auto before=Read(f,kind,slot);const Keys native(*f.app);
        switch(kind){case Kind::Camera:f.app->CameraKeys()[1].selected=1;break;case Kind::Light:f.app->LightKeys()[1].selected=1;break;
            case Kind::Shadow:f.app->ShadowKeys()[1].selected=1;break;case Kind::Gravity:f.app->GravityKeys()[1].selected=1;break;
            default:f.app->AccessoryKeys(7)[1].selected=1;break;}
        auto change=Plan(f);Check(change!=nullptr,"sparse native-union deletion plans");Commit(f,change);
        const auto after=Read(f,kind,slot);Check(!Has(after[lane],12)&&after[lane].track.keys.size()==before[lane].track.keys.size()&&
            after[lane].track.keys.front().outSlope==before[lane].track.keys.front().outSlope&&
            after[lane].track.keys.back().inSlope==before[lane].track.keys.back().inSlope,
            "deleting native-union frame keeps already-sparse Free lane keys and handles");
        Check(f.history.History(false)==MMS_OK&&Same(Read(f,kind,slot),before),"sparse delete undo restores complete author data and interval metadata");
        Check(f.history.History(true)==MMS_OK&&Same(Read(f,kind,slot),after),"sparse delete redo reproduces author structure without resampling unrelated lanes");
        (void)native;
    }
    {
        Fixture f;auto& root=f.app->CameraKeys()[0];root.distance=-45;root.eye[0]=root.eye[2]=0;root.eye[1]=10;root.target[0]=root.target[1]=root.target[2]=0;
        root.parentModel=-1;root.parentBone=0;std::memset(root.interpolation,20,12);std::memset(reinterpret_cast<unsigned char*>(root.interpolation)+12,107,12);
        auto channels=Read(f,Kind::Camera);channels[0].track.keys.front().value=1e-48;channels[0].track.keys.front().tangent=animation_curve::Tangent::Free;
        channels[7].track.keys.front().value=45.25;
        std::size_t inserted=0;std::wstring error;Check(curve_host::InsertPreservingShape(channels,1,20,inserted,error),"prepare independent preserved native camera tail interval");
        Check(curve_host::InsertPreservingShape(channels,7,6,inserted,error),"prepare exact FOV root interval owned by an independent high precision author");
        // These root bytes are unused for sampling, but native zero deletion
        // deliberately resets all six control tuples, including FOV. Give
        // both independent lanes nondefault tuples so the expectation does
        // not depend on an insertion helper's choice of root placeholder.
        channels[1].nativeControls.front()={7,19,93,111};
        channels[7].nativeControls.front()={9,21,95,113};
        Check(curve_host::Write(*f.app,Kind::Camera,0,channels,error)&&f.app->CameraKeys()[0].eye[0]==0.f,
            "high precision author root differs while native root is already exact default");
        const auto before=Read(f,Kind::Camera);Check(before[0].track.keys.front().value==1e-48,"independent high precision root survives ordinary read");
        Check(before[7].track.keys.front().value==45.25&&f.app->CameraKeys()[0].fov==45&&
            !before[7].preservedCameraIntervals.empty(),"FOV retains its fractional authored root and exact interval despite rounded native payload");
        f.app->CameraKeys()[0].selected=1;auto change=Plan(f);Check(change!=nullptr,"explicit zero delete notices author-only root reset despite identical native value");Commit(f,change);
        const auto after=Read(f,Kind::Camera);Check(after[0].track.keys.front().value==0&&after[1].track.keys.size()==before[1].track.keys.size(),
            "zero delete resets independent authored root without dropping unrelated author-only keys");
        auto expectedY=before[1],expectedFov=before[7];
        expectedY.nativeControls.front()=expectedFov.nativeControls.front()={20,20,107,107};
        Check(before[1].nativeControls.front()!=expectedY.nativeControls.front()&&before[7].nativeControls.front()!=expectedFov.nativeControls.front(),
            "root deletion fixture includes real nondefault unused Y and FOV control bytes that native reset must replace");
        DiagnoseChannel(after[1],expectedY,1);DiagnoseChannel(after[7],expectedFov,7);
        Check(Same({after[1]},{expectedY}),"root reset updates its native root tuple while retaining exact independent handles, other controls and preserved tail fragments");
        Check(Same({after[7]},{expectedFov})&&Same({after[8]},{before[8]}),
            "zero camera delete updates unused FOV root controls while preserving fractional FOV, exact root fragments, handles and perspective authoring");
        Check(f.history.History(false)==MMS_OK&&Same(Read(f,Kind::Camera),before),"zero delete undo restores tiny author root and exact preserved interval metadata");
        Check(f.history.History(true)==MMS_OK&&Same(Read(f,Kind::Camera),after),"zero delete redo applies same explicit author root reset");
    }
}
void PreservedInsertionDeletion(){
    Fixture f;
    auto channels=Read(f,Kind::Camera);channels[7].track.keys.front().value=45.25;
    std::size_t inserted=0;std::wstring error;
    Check(curve_host::InsertPreservingShape(channels,0,6,inserted,error),"prepare exact X subdivision at the native union deletion frame");
    Check(curve_host::InsertPreservingShape(channels,1,20,inserted,error),"prepare independent exact Y subdivision outside the deleted interval");
    // This CPU fixture has no native HWND. Initialize its source through the
    // production unbound writer before attaching common history: a bound curve
    // edit correctly requires the real host window and UI publication gate.
    const bool written=curve_host::Write(*f.app,Kind::Camera,0,channels,error);
    if(!written)std::fwprintf(stderr,L"DIAG camera union deletion source write: %ls\n",error.c_str());
    Check(written,"publish actual native union keys after independent exact camera subdivisions");
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto before=Read(f,Kind::Camera);f.ClearMarks();
    Check(!before[7].controlGeometry.empty()&&before[7].controlGeometry.front()==std::array<double,4>{0,0,0,0},
        "untouched FOV retains its legitimate native root geometry rather than a subdivision placeholder");
    unsigned removed=0;
    for(unsigned index=0;;index=f.app->CameraKeys()[index].next){
        if(f.app->CameraKeys()[index].frame==6){removed=index;break;}
        if(!f.app->CameraKeys()[index].next)break;
    }
    Check(removed&&Has(before[0],6)&&!Has(before[1],6)&&
        !before[0].preservedCameraIntervals.empty()&&!before[1].preservedCameraIntervals.empty(),
        "find the real selected native union row and independent preserved peer lane");
    f.app->CameraKeys()[removed].selected=1;const Keys nativeBefore(*f.app);const auto pose=Pose(f);
    // The deletion-only policy must not weaken ordinary strict retiming.
    {
        curve_host::TrackAuthoring author(*f.app,Kind::Camera);std::vector<int> frames;author.RetimingFrames(frames);
        std::vector<std::pair<int,int>> map;for(int frame:frames)map.push_back({frame,frame==6?-1:frame});
        auto target=GlobalRegistrationKeys(f.app->CameraKeys());auto& key=target[removed];
        target[key.previous].next=key.next;target[key.next].previous=key.previous;key.frame=key.previous=key.next=0;
        curve_host::NativeKeyView view{&target,target.size(),[](const void* value,unsigned index)noexcept->const void*{
            const auto& keys=*static_cast<const std::vector<mdl::CameraKey>*>(value);return index<keys.size()?&keys[index]:nullptr;}};
        bool refused=false;try{(void)author.PrepareRetimed(view,map);}catch(const std::invalid_argument&){refused=true;}
        Check(refused&&nativeBefore.Same(*f.app),"ordinary retiming still refuses a join across exact native slice boundaries without publication");
    }
    Check(plugins::TryDeleteBoundKeys(f.app.get())&&f.history.UndoCount()==1&&
        f.app->CameraKeys()[removed].frame==0&&Pose(f)==pose,
        "actual bound deletion removes a shape-preserving native union row in one undoable operation");
    const auto after=Read(f,Kind::Camera);const Keys nativeAfter(*f.app);
    Check(!Has(after[0],6)&&after[0].track.keys.size()+1==before[0].track.keys.size(),
        "deletion removes the authored key as well as its native proxy without rebuilding the whole track");
    std::vector<curve_editor::NativeCameraInterval> unaffected;
    for(const auto& interval:before[0].preservedCameraIntervals)if(interval.first>=12)unaffected.push_back(interval);
    Check(!unaffected.empty()&&after[0].preservedCameraIntervals==unaffected,
        "only the joined native slices are released and the independent native tail stays bit exact");
    DiagnoseChannel(after[1],before[1],1);DiagnoseChannel(after[7],before[7],7);
    Check(Same({after[1]},{before[1]})&&Same({after[7]},{before[7]}),
        "deleting another lane's union row preserves peer slices, sparse keys, handles and fractional FOV exactly");
    Check(f.history.History(false)==MMS_OK&&nativeBefore.Same(*f.app)&&Same(Read(f,Kind::Camera),before)&&Pose(f)==pose,
        "one undo restores the removed native row and every original exact native slice and author value");
    Check(f.history.History(true)==MMS_OK&&nativeAfter.Same(*f.app)&&Same(Read(f,Kind::Camera),after)&&Pose(f)==pose,
        "redo repeats the same intentional deletion without losing unrelated authoring or an unregistered pose");
}
void LaterSelectionAndNoop(){
    Fixture f;f.Mark(1);auto change=Plan(f);Commit(f,change);f.app->CameraKeys()[2].selected=11;f.app->LightKeys()[2].selected=12;
    Check(f.history.History(false)==MMS_OK&&f.app->CameraKeys()[2].selected==11&&f.app->LightKeys()[2].selected==12&&
        !f.app->CameraKeys()[1].selected&&!f.app->LightKeys()[1].selected,"undo preserves later independent selection by frame instead of resurrecting old selected marks");
    Check(f.history.History(true)==MMS_OK&&f.app->CameraKeys()[2].selected==11&&f.app->LightKeys()[2].selected==12,
        "redo keeps later independent selection on surviving keys");
    Check(f.history.History(false)==MMS_OK,"prepare global delete redo for no-op check");f.ClearMarks();const auto digest=f.FullDigest();
    Check(!Plan(f)&&f.history.RedoCount()==1&&f.FullDigest()==digest,"planning unmarked deletion is a true no-op preserving existing redo");
    {
        Fixture root;root.app->CameraKeys()[0].selected=1;auto reset=Plan(root);Commit(root,reset);
        root.app->LightKeys()[1].selected=1;auto another=Plan(root);Commit(root,another);
        Check(root.history.History(false)==MMS_OK,"seed redo while camera root already holds its native defaults");
        root.ClearMarks();root.app->CameraKeys()[0].selected=1;const auto unchanged=root.FullDigest();
        Check(!Plan(root)&&root.FullDigest()==unchanged&&root.history.RedoCount()==1,
            "repeating an already-default marked root reset is a semantic no-op and preserves redo and selection");
    }
    {
        Fixture authored;auto channels=Read(authored,Kind::Camera);channels[0].track.keys.front().value=1e-48;std::wstring error;
        Check(curve_host::Write(*authored.app,Kind::Camera,0,channels,error),"create author lineage for source revision rejection");
        authored.app->CameraKeys()[1].selected=1;auto planned=Plan(authored);const auto raw=GlobalRegistrationKeys(authored.app->CameraKeys());
        channels=Read(authored,Kind::Camera);channels[0].track.keys.front().value=2e-48;
        Check(curve_host::Write(*authored.app,Kind::Camera,0,channels,error)&&GlobalRegistrationSame(authored.app->CameraKeys(),raw),
            "edit only high precision source authoring without changing native key bytes");
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;Check(planned->Prepare(true,candidate)==MMS_E_STALE&&!candidate,
            "same-lineage author source revision change rejects the prepared delete before any write");
    }
}
}
inline void NativeGlobalKeyDeleteChecks(){
    std::puts("  global deletion: five families, root defaults and one explicit common journal");global_key_delete_checks::AllFamilies();
    global_key_delete_checks::BoundCommandAndFallback();global_key_delete_checks::FailureAndIdentity();
    global_key_delete_checks::SparseAndRootAuthoring();global_key_delete_checks::PreservedInsertionDeletion();global_key_delete_checks::LaterSelectionAndNoop();
}
