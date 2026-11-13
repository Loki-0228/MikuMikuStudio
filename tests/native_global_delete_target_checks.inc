// Include after native_global_key_delete_checks.hpp. These detached tests do
// not implement or exercise a native/negative signed deletion aggregate.
#include "mikumikustudio/ported_funcs.hpp"
#include <cmath>
namespace global_delete_target_checks {
using namespace global_key_delete_checks;
using Fixture=global_key_delete_checks::Fixture;
using Target=plugins::GlobalDeletionTarget;
using Prepared=plugins::PreparedGlobalKeyDeletion;
using Change=plugins::GlobalKeyDeletion;
const std::array<Kind,6> kinds{Kind::Camera,Kind::Light,Kind::Shadow,Kind::Gravity,Kind::Accessory,Kind::Accessory};
const std::array<unsigned,6> slots{0,0,0,0,7,8};
using Authors=std::array<std::vector<curve_editor::Channel>,6>;
Authors AuthorsOf(Fixture& f){Authors result;for(unsigned i=0;i<6;++i)result[i]=Read(f,kinds[i],slots[i]);return result;}
bool AuthorsSame(Fixture& f,const Authors& expected){
    for(unsigned i=0;i<6;++i)if(!Same(Read(f,kinds[i],slots[i]),expected[i]))return false;return true;
}
// Only map public native fields to the documented lane layout. No deletion,
// interpolation, reset defaults or degree conversion is reproduced here.
std::array<double,12> NativePoseOf(Fixture& f,Kind kind,unsigned slot){
    auto& a=*f.app;std::array<double,12> v{};
    switch(kind){
    case Kind::Camera:
        for(unsigned i=0;i<3;++i){v[i]=a.CameraPositionAxis(i);v[3+i]=a.CameraRotation()[i];}
        v[6]=a.CameraDistance();v[7]=a.CameraFov();v[8]=a.CameraPerspective();v[9]=a.CameraParentModel();v[10]=a.CameraParentBone();break;
    case Kind::Light:for(unsigned i=0;i<3;++i){v[i]=a.LightDirection()[i];v[3+i]=a.LightColor()[i];}break;
    case Kind::Shadow:v[0]=a.ShadowMode();v[1]=a.ShadowDistance();break;
    case Kind::Gravity:
        v[0]=a.GravityMagnitude();for(unsigned i=0;i<3;++i)v[1+i]=a.GravityDirection()[i];
        v[4]=a.GravityNoise();v[5]=a.GravityNoiseModeWord();break;
    case Kind::Accessory:{const auto& o=*a.AccessorySlot(slot);
        for(unsigned i=0;i<3;++i){v[i]=o.position[i];v[3+i]=o.rotation[i];}
        v[6]=o.scale;v[7]=o.opacity;v[8]=o.visible;v[9]=o.shadowEnabled;v[10]=o.parentModel;v[11]=o.parentBone;break;}
    }
    return v;
}
using Poses=std::array<std::array<double,12>,6>;
Poses PosesOf(Fixture& f){Poses p;for(unsigned i=0;i<6;++i)p[i]=NativePoseOf(f,kinds[i],slots[i]);return p;}
std::array<std::uint32_t,6> Cursors(Fixture& f){return {f.app->CameraTrackCursor(),f.app->LightTrackCursor(),
    f.app->ShadowTrackCursor(),f.app->GravityTrackCursor(),f.app->AccessoryTrackCursor(7),f.app->AccessoryTrackCursor(8)};}
std::array<unsigned char,6> Active(Fixture& f){return {f.app->CameraTrackActive(),f.app->LightTrackActive(),
    f.app->ShadowTrackActive(),f.app->GravityTrackActive(),f.app->AccessoryTrackActive(7),f.app->AccessoryTrackActive(8)};}
void EvaluateProduction(Fixture& f,int frame){
    f.app->CurrentFrame()=frame;
    ReloadModels(f.app.get());RefreshLightPanel(f.app.get());RefreshSelfShadowPanel(f.app.get());
    ApplyGravityTrack(f.app.get());ApplyAccessoryTrack(f.app.get(),7);ApplyAccessoryTrack(f.app.get(),8);
}
bool Close(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=2e-6*std::max(1.0,std::abs(b));}
void CheckTargets(const Prepared& p,Fixture& f,const Poses& expected){
    const auto& targets=p.ZeroTargets();Check(targets.size()==6,"signed global targets enumerate five families and both accessories");
    std::array<bool,6> seen{};
    for(const auto& target:targets){
        unsigned index=0;while(index<6&&(kinds[index]!=target.kind||slots[index]!=target.slot))++index;
        Check(index<6&&!seen[index],"signed target kind/slot is unique and expected");seen[index]=true;
        Check(target.object==(target.kind==Kind::Accessory?static_cast<const void*>(f.app->AccessorySlot(target.slot)):nullptr),
            "signed target identifies the actual accessory and uses null object for global families");
        for(unsigned lane=0;lane<12;++lane)Check(Close(target.zeroPose[lane],expected[index][lane]),
            "signed zero target agrees with independent ordinary deletion and production zero evaluation");
        if(target.kind==Kind::Gravity){
            if(target.zeroPose[5]!=expected[index][5])
                std::fprintf(stderr,"gravity zero word: target=%.17g oracle=%.17g\n",target.zeroPose[5],expected[index][5]);
            Check(target.zeroPose[5]==expected[index][5],
                "gravity zero target retains the entire native mode word including upper bytes");}
    }
}
void Seed(Fixture& f,bool authored){
    for(unsigned i=0;i<3;++i){const std::uint32_t word=0x00573100u+(i%2);
        std::memcpy(&f.app->GravityKeys()[i].noiseEnabled,&word,sizeof(word));}
    f.app->AccessoryKeys(7)[0].rotation[1]=.375f;f.app->AccessoryKeys(8)[0].rotation[1]=-.625f;
    if(!authored)return;
    for(unsigned i=0;i<6;++i){auto channels=Read(f,kinds[i],slots[i]);
        const unsigned lane=kinds[i]==Kind::Shadow?1:0;auto& keys=channels[lane].track.keys;
        keys.erase(std::remove_if(keys.begin(),keys.end(),[](const auto& key){return key.frame==12;}),keys.end());
        keys.front().value=1e-48;keys.front().tangent=animation_curve::Tangent::Free;keys.front().outSlope=.0013;keys.front().outWeight=.19;
        keys.back().inSlope=-.0017;keys.back().inWeight=.29;keys.back().interpolation=animation_curve::Interpolation::Bezier;
        if(kinds[i]==Kind::Camera){channels[3].track.keys.front().value=23.125;channels[7].track.keys.front().value=45.25;}
        if(kinds[i]==Kind::Accessory)channels[4].track.keys.front().value=i==4?-31.625:17.375;
        std::wstring error;Check(curve_host::Write(*f.app,kinds[i],slots[i],channels,error),"seed independent sparse high precision authored deletion sources");
    }
}
std::shared_ptr<Change> SignedPlan(Fixture& f,bool fallback=false){std::shared_ptr<Change> change;
    Check(plugins::PlanSignedGlobalKeyDeletion(*f.app,change,fallback)==MMS_OK,"typed signed global deletion plans without staging live selection");return change;
}
void Publish(Prepared& p){Check(p.IsCurrent(),"signed target candidate is current before publication");
    failAfter=0;p.Publish();failAfter=-1;Check(!p.IsCurrent(),"signed target candidate is consumed by allocation-free publication");}
void DirectionalTargets(){
    for(bool authored:{false,true})for(unsigned index:{0u,1u}){
        Fixture f,oracle;Seed(f,authored);Seed(oracle,authored);f.ClearMarks();oracle.ClearMarks();f.Mark(index);oracle.Mark(index);
        f.app->CurrentFrame()=-17;oracle.app->CurrentFrame()=0;
        const Keys before(*f.app);const auto pristine=AuthorsOf(f);const auto live=PosesOf(f);const auto pose=Pose(f);
        const auto cursors=Cursors(f);const auto active=Active(f);const auto last=f.app->LastRegisteredFrame();
        const auto dirty=f.app->SceneModified();const auto light=f.app->SceneLight();const auto projection=f.app->cameraProjectionRefreshPending;
        const auto objects=f.accessories;const auto originalDigest=f.FullDigest();
        auto change=SignedPlan(f);auto ordinary=Plan(oracle);
        Check(change&&ordinary&&f.FullDigest()==originalDigest&&AuthorsSame(f,pristine),"signed planning has no native pose cursor selection or pristine author side effects");
        std::shared_ptr<plugins::HistoryChange> rejected;
        Check(plugins::PlanGlobalKeyDeletion(*f.app,rejected)==MMS_E_ARGUMENT&&!rejected,"ordinary global plan still rejects a true negative playhead");
        std::unique_ptr<plugins::PreparedHistoryChange> normal;
        Check(change->Prepare(true,normal)==MMS_E_ARGUMENT&&!normal,"ordinary Prepare on typed signed plan still rejects negative playhead");
        std::unique_ptr<Prepared> candidate;std::unique_ptr<Keys> after;Authors afterAuthors;Poses firstTargets{};
        for(bool forward:{true,false,true}){
            const auto digest=f.FullDigest();const auto authors=AuthorsOf(f);
            Check(change->PrepareSigned(forward,candidate)==MMS_OK&&candidate&&candidate->IsCurrent(),"prepare directional signed targets at actual negative playhead");
            Check(f.FullDigest()==digest&&AuthorsSame(f,authors)&&f.app->CurrentFrame()==-17,"target sampling does not seek or mutate live data during preparation");
            Check(ordinary->Prepare(forward,normal)==MMS_OK&&normal&&normal->IsCurrent(),"independent ordinary zero-frame direction prepares");
            normal->Publish();normal.reset();EvaluateProduction(oracle,0);
            const auto expected=PosesOf(oracle);CheckTargets(*candidate,f,expected);
            // Native-only fixtures retain nonzero padding. Authoring writes
            // rebuild native rows, so those fixtures may legitimately have 0.
            if(!authored)Check(expected[3][5]>255,"native-only oracle exercises nonzero upper gravity mode bytes");
            if(forward){if(!after)firstTargets=expected;else Check(expected==firstTargets,"redo resamples the same forward zero targets");}
            else if(index==0)Check(expected!=firstTargets,"root reverse zero targets differ from reset targets and restore original data");
            Publish(*candidate);candidate.reset();
            Check(Keys(*oracle.app).Same(*f.app)&&AuthorsSame(f,AuthorsOf(oracle)),"signed directional publication matches ordinary native keys and exact authoring");
            Check(PosesOf(f)==live&&Pose(f)==pose&&!std::memcmp(objects.data(),f.accessories.data(),sizeof(objects))&&
                f.app->CurrentFrame()==-17&&!f.history.UndoCount()&&!f.history.RedoCount(),
                "negative publication never writes any live pose object or playhead and detached tests do not append history");
            Check(Cursors(f)==(forward?std::array<std::uint32_t,6>{}:cursors)&&Active(f)==active&&
                f.app->LastRegisteredFrame()==last&&f.app->SceneModified()==dirty&&
                !std::memcmp(&light,&f.app->SceneLight(),sizeof(light))&&f.app->cameraProjectionRefreshPending==projection,
                "negative publication updates only directional cursors and leaves active flags metadata and renderer pose mirrors untouched");
            if(forward&&!after){after=std::make_unique<Keys>(*f.app);afterAuthors=AuthorsOf(f);}
            else if(!forward)Check(before.Same(*f.app)&&AuthorsSame(f,pristine),"reverse restores pristine sparse handles angles fractional FOV and tiny authored zero values");
            else Check(after->Same(*f.app)&&AuthorsSame(f,afterAuthors),"redo reproduces exact forward native and authored target");
            if(authored&&index==1)for(unsigned i=0;i<6;++i){const auto channels=Read(f,kinds[i],slots[i]);const unsigned lane=kinds[i]==Kind::Shadow?1:0;
                // Target reconciliation normalizes auxiliary control arrays
                // even when the sparse author had no key at the deleted frame.
                // Their complete oracle comparison is AuthorsSame above.
                Check(Encoded({channels[lane]})==Encoded({pristine[i][lane]})&&
                    channels[lane].preservedCameraIntervals==pristine[i][lane].preservedCameraIntervals,
                    "positive-only deletion preserves sparse authored keys, precise zero values, Free handles and retained intervals");}
            if(authored){const auto camera=Read(f,Kind::Camera);
                Check(camera[7].track.keys.front().value==45.25,"every deletion direction preserves fractional authored camera FOV");
                if(index==1||!forward)Check(camera[3].track.keys.front().value==23.125,"untouched/restored authored angle remains in original degree units");}
        }
    }
}
void NonnegativeParity(){
    for(bool authored:{false,true})for(int frame:{0,7,31})for(unsigned index:{0u,1u}){
        Fixture f,oracle;Seed(f,authored);Seed(oracle,authored);
        EvaluateProduction(f,frame);EvaluateProduction(oracle,frame);f.ClearMarks();oracle.ClearMarks();f.Mark(index);oracle.Mark(index);
        auto signedChange=SignedPlan(f);auto ordinary=Plan(oracle);Check(signedChange&&ordinary,"nonnegative parity has real selected deletions");
        const auto initial=PosesOf(f);
        for(bool forward:{true,false,true}){
            std::unique_ptr<Prepared> candidate;std::unique_ptr<plugins::PreparedHistoryChange> other;
            Check(signedChange->PrepareSigned(forward,candidate)==MMS_OK&&ordinary->Prepare(forward,other)==MMS_OK&&
                candidate&&other&&candidate->IsCurrent()&&other->IsCurrent(),"signed and ordinary nonnegative candidates prepare together");
            Publish(*candidate);other->Publish();
            Check(Keys(*oracle.app).Same(*f.app)&&AuthorsSame(f,AuthorsOf(oracle))&&PosesOf(f)==PosesOf(oracle)&&Pose(f)==Pose(oracle)&&
                Cursors(f)==Cursors(oracle)&&Active(f)==Active(oracle)&&f.app->CurrentFrame()==frame&&oracle.app->CurrentFrame()==frame,
                "nonnegative signed forward reverse redo preserve normal key author and pose behavior");
            if(frame==0&&index==0&&forward)Check(PosesOf(f)!=initial,"nonnegative signed root deletion actually updates a sampled pose");
        }
    }
}
void Staleness(){
    for(unsigned mutation=0;mutation<10;++mutation){
        Fixture f;Seed(f,true);f.ClearMarks();f.Mark(1);f.app->CurrentFrame()=-19;
        // The curve writer normalizes the selected byte to bool. Use its
        // canonical value so the author-only mutation cannot also alter marks.
        if(mutation==1)f.app->CameraKeys()[1].selected=1;
        mdl::ModelRecord parent{};std::array<mdl::BoneRecord,1> bones{},replacementBones{};
        if(mutation==4){parent.boneCount=1;parent.boneTable=bones.data();f.app->ModelSlot(3)=reinterpret_cast<unsigned char*>(&parent);
            f.app->CameraKeys()[0].parentModel=3;f.app->CameraKeys()[0].parentBone=0;}
        auto change=SignedPlan(f);Check(change!=nullptr,"stale signed source has a complete plan");std::unique_ptr<Prepared> candidate;
        Check(change->PrepareSigned(true,candidate)==MMS_OK&&candidate&&candidate->IsCurrent(),"stale test prepares a current signed candidate");
        auto replacement=f.accessories[1];
        auto* originalPool=f.app->CameraKeys();mdl::CameraKey* replacementPool=nullptr;
        switch(mutation){
        case 0:f.app->LightKeys()[2].color[0]+=.125f;break;
        case 1:{auto channels=Read(f,Kind::Camera);channels[0].track.keys.front().value=2e-48;const Keys native(*f.app);std::wstring error;
            const bool written=curve_host::Write(*f.app,Kind::Camera,0,channels,error);
            if(written&&!native.Same(*f.app))for(unsigned row=0;row<native.camera.size();++row){
                const auto* a=reinterpret_cast<const unsigned char*>(&native.camera[row]);
                const auto* b=reinterpret_cast<const unsigned char*>(&f.app->CameraKeys()[row]);
                for(unsigned byte=0;byte<sizeof(mdl::CameraKey);++byte)if(a[byte]!=b[byte])
                    std::fprintf(stderr,"author source native diff row=%u byte=%u %u->%u\n",row,byte,a[byte],b[byte]);}
            Check(written&&native.Same(*f.app),"author-only source mutation retains identical native bytes");break;}
        case 2:f.app->GravityKeys()[2].selected=17;break;
        case 3:f.app->AccessorySlot(8)=&replacement;break;
        case 4:parent.boneTable=replacementBones.data();break;
        case 5:f.app->CurrentFrame()=-18;break;
        case 6:f.app->CameraPositionX()+=1;break;
        case 7:f.app->AccessoryTrackCursor(8)++;break;
        case 8:replacementPool=mdl::KeyPoolCreate<mdl::CameraKey>(mdl::KeyPoolCapacity(originalPool));
            Check(replacementPool!=nullptr,"allocate same-byte replacement source pool");
            std::memcpy(replacementPool,originalPool,mdl::KeyPoolCapacity(originalPool)*sizeof(*originalPool));
            f.app->CameraKeys()=replacementPool;break;
        case 9:plugins::ForgetSceneObject(&f.accessories[1]);plugins::SceneObjectIdentity(&f.accessories[1]);break;
        }
        const auto digest=f.FullDigest();const Keys keys(*f.app);
        Check(!candidate->IsCurrent(),"native author selection parent/accessory identity playhead pose or cursor mutation stales signed candidate");
        std::unique_ptr<Prepared> next;
        Check(change->PrepareSigned(true,next)==MMS_E_STALE&&!next&&f.FullDigest()==digest&&keys.Same(*f.app)&&
            !f.history.UndoCount()&&!f.history.RedoCount(),"stale signed reprepare refuses without overwriting external changes or creating history");
        if(mutation==1)Check(Read(f,Kind::Camera)[0].track.keys.front().value==2e-48,
            "stale reprepare preserves the external author-only edit despite identical native bytes");
        // Restore borrowed membership before retained guards release; no test
        // here transfers a stack object to the production deletion path.
        f.app->AccessorySlot(8)=&f.accessories[1];parent.boneTable=bones.data();
        candidate.reset();change.reset();
        f.app->CameraKeys()=originalPool;if(replacementPool)mdl::KeyPoolDestroy(replacementPool);
        if(mutation==4){plugins::ForgetSceneObject(&parent);f.app->ModelSlot(3)=nullptr;}
    }
}
void FallbackAndAllocation(){
    Fixture f;Seed(f,true);f.ClearMarks();f.app->CurrentFrame()=-12;
    const auto digest=f.FullDigest();const auto authors=AuthorsOf(f);const Keys keys(*f.app);
    auto empty=SignedPlan(f,true);
    Check(!empty&&f.FullDigest()==digest&&keys.Same(*f.app)&&AuthorsSame(f,authors),"negative current-frame fallback cannot select or delete any native key");
    f.Mark(1);const auto marked=f.FullDigest();const auto markedAuthors=AuthorsOf(f);
    std::shared_ptr<Change> change;unsigned planFailures=0;bool planned=false;
    for(long allowance=0;allowance<4096;++allowance){failAfter=allowance;
        const auto result=plugins::PlanSignedGlobalKeyDeletion(*f.app,change,true);failAfter=-1;
        Check(f.FullDigest()==marked&&AuthorsSame(f,markedAuthors)&&!f.history.UndoCount()&&!f.history.RedoCount(),"signed plan allocation failures preserve keys poses cursors authored sources and history");
        if(result==MMS_OK){planned=bool(change);break;}
        Check(result==MMS_E_MEMORY&&!change,"signed plan OOM exposes no partial plan");++planFailures;
    }
    Check(planned&&planFailures>3,"signed plan allocation sweep reaches successful explicit-mark planning at negative fallback");
    unsigned prepareFailures=0;bool ready=false;
    for(long allowance=0;allowance<8192;++allowance){std::unique_ptr<Prepared> candidate;failAfter=allowance;
        const auto result=change->PrepareSigned(true,candidate);failAfter=-1;
        Check(f.FullDigest()==marked&&AuthorsSame(f,markedAuthors)&&f.app->CurrentFrame()==-12,
            "signed target OOM and cancellation preserve all live state and authored precision");
        if(result==MMS_OK){Check(candidate&&candidate->IsCurrent()&&candidate->ZeroTargets().size()==6,"successful OOM sweep produces all six targets");ready=true;break;}
        Check(result==MMS_E_MEMORY&&!candidate,"signed prepare OOM exposes no partial target vector");++prepareFailures;
    }
    Check(ready&&prepareFailures>3,"signed target allocation sweep eventually succeeds after multiple private stages");
    std::unique_ptr<Prepared> candidate;Check(change->PrepareSigned(true,candidate)==MMS_OK&&candidate,"candidate remains preparable after OOM sweep");
    const auto pose=PosesOf(f);Publish(*candidate);
    Check(f.app->CameraKeys()[1].frame==0&&f.app->GravityKeys()[1].frame==0&&f.app->AccessoryKeys(8)[1].frame==0&&
        PosesOf(f)==pose&&f.app->CurrentFrame()==-12,"explicit native marks still publish at negative playhead even when fallback was requested");
}
}
inline void NativeGlobalDeleteTargetChecks(){
    std::puts("  signed global deletion targets: detached five-family preparation, not a cross-zero DELETE aggregate");
    global_delete_target_checks::DirectionalTargets();global_delete_target_checks::NonnegativeParity();
    global_delete_target_checks::Staleness();global_delete_target_checks::FallbackAndAllocation();
}
