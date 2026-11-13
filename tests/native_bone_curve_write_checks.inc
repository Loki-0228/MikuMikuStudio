// Include after native_bone_registration_checks.inc and evaluation fixtures.
#include "../src/plugins/native_bone_curve_write.hpp"
#include <limits>
namespace {
struct BoneCurveWriteFixture:PoseNumericFixture{
    BoneCurveWriteFixture(){for(auto* f:{&first,&second}){
        f->m->ikChainCount=0;f->m->boneOrderCount=0;f->m->boneOrderTable=nullptr;
        f->m->maxFrame=30;f->m->searchCursor=5;
        f->m->boneKeys[0].next=2;
        for(unsigned bone=0;bone<2;++bone){f->bones[bone].slotIndex=-1;f->bones[bone].type=mdl::BoneType::RotateMove;f->dirty[bone]=0;
            f->m->boneKeys[bone].rotation[3]=1;}
        for(unsigned index=2;index<4;++index){auto& key=f->m->boneKeys[index];key.frame=index==2?10:30;
            key.previous=index==2?0:2;key.next=index==2?3:0;key.position[0]=float(index*3);key.rotation[3]=1;
            for(unsigned lane=0;lane<4;++lane){key.interpolation[lane]=20;key.interpolation[lane+4]=20;key.interpolation[lane+8]=107;key.interpolation[lane+12]=107;}}
        f->m->boneKeys[2].allocated=7;f->m->boneKeys[3].reserved[1]=29;
        bone_scale::SetKey(f->raw(),2,Scale(2));
    }app->CurrentFrame()=5;app->LastRegisteredFrame()=30;}
    ~BoneCurveWriteFixture(){history.Reset();history.Collect();bone_curve::Forget(first.raw());bone_curve::Forget(second.raw());}
    std::shared_ptr<plugins::HistoryChange> Plan(const std::vector<plugins::BoneCurveEdit>& input,const plugins::DisplayCurveRows* display=nullptr){
        std::shared_ptr<plugins::HistoryChange> result;
        Check(plugins::PlanBoneCurveWrite(*app,0,input,display,result)==MMS_OK,"prepare private multi-bone curve write");return result;
    }
    void Commit(const std::shared_ptr<plugins::HistoryChange>& change){
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(change&&history.PrepareNative("bone curves",change,token,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"reserve one explicit bone curve transaction");
        failAfter=0;const auto status=history.CommitNative(std::move(token));failAfter=-1;
        Check(status==MMS_OK,"bone curve keys scale authoring and local pose publish without allocation");
    }
};
plugins::BoneCurveEdit FreeBoneCurve(unsigned bone=0){plugins::BoneCurveEdit edit;edit.bone=bone;edit.freeCurves=true;
    for(unsigned c=0;c<9;++c){animation_curve::Key first,last;first.value=last.value=c>=6?1:0;last.frame=20;
        first.interpolation=last.interpolation=animation_curve::Interpolation::Linear;
        first.tangent=last.tangent=animation_curve::Tangent::Free;
        if(c==0)last.value=12;if(c==3)last.value=25;if(c==6)last.value=3;edit.tracks[c].keys={first,last};}
    return edit;}
plugins::BoneCurveEdit NativeBoneCurve(const Fixture& fixture,unsigned bone=0){plugins::BoneCurveEdit edit;edit.bone=bone;
    unsigned index=bone;for(;;){const auto& key=fixture.m->boneKeys[index];plugins::NativeBoneCurveControls row;row.frame=int(key.frame);
        for(unsigned lane=0;lane<4;++lane)for(unsigned c=0;c<4;++c)row.controls[lane][c]=key.interpolation[lane+c*4];
        row.controls[4]=bone_scale::KeyCurve(fixture.m->boneTable?reinterpret_cast<const unsigned char*>(fixture.m.get()):nullptr,int(index));
        edit.nativeControls.push_back(row);if(!key.next)break;index=key.next;}return edit;}
bool SameTracksForBoneTest(const bone_curve::Tracks&,const bone_curve::Tracks&);
void BoneCurveWriteOracle(){
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        BoneCurveWriteFixture f;Check(f.history.SetMode(mode)==MMS_OK,"bone curve explicit recording mode");
        const auto edit=FreeBoneCurve();const auto before=RegistrationKeys(f.first);const auto source=f.first.Digest(),ring=PoseRingDigest(f.first);
        const auto scale=bone_scale::AuthoringState::Capture(f.first.raw());const auto author=bone_curve::AuthoringState::Capture(f.first.raw());
        auto change=f.Plan({edit});Check(change&&f.first.Digest()==source,"free curve plan leaves native keys pose metadata and undo ring unchanged");
        f.Commit(change);std::string error;Check(bone_curve::Write(f.second.raw(),0,edit.tracks,error),"old free curve writer supplies independent native oracle");
        Check(SameRegistrationKeys(f.first,RegistrationKeys(f.second))&&f.first.m->searchCursor==f.second.m->searchCursor&&f.first.m->maxFrame==f.second.m->maxFrame,
            "first free publication matches old Write native bytes and metadata");
        for(unsigned index=0;index<32;++index)Check(bone_scale::Key(f.first.raw(),int(index))==bone_scale::Key(f.second.raw(),int(index))&&
            bone_scale::KeyCurve(f.first.raw(),int(index))==bone_scale::KeyCurve(f.second.raw(),int(index)),"free publication matches old scale key and interpolation results");
        bone_curve::Tracks tracks;Check(bone_curve::ReadDetached(f.first.raw(),0,tracks)&&SameTracksForBoneTest(tracks,edit.tracks),"free author is available through detached canonical read");
        const auto committed=RegistrationKeys(f.first);f.app->CurrentFrame()=13;
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,before)&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale)&&
            bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author),"free curve undo restores raw native scale and author snapshots at another frame");
        Check(f.history.History(true)==MMS_OK&&SameRegistrationKeys(f.first,committed)&&PoseRingDigest(f.first)==ring&&f.history.UndoCount()==1,
            "free curve redo uses common history without touching the thirty-slot ring");
    }
}
bool SameTracksForBoneTest(const bone_curve::Tracks& a,const bone_curve::Tracks& b){
    std::vector<animation_curve::NamedTrack> left,right;for(unsigned c=0;c<9;++c){left.push_back({std::to_string(c),a[c]});right.push_back({std::to_string(c),b[c]});}
    std::vector<std::uint8_t> x,y;std::string error;return animation_curve::Encode(left,x,error)&&animation_curve::Encode(right,y,error)&&x==y;
}
void BoneCurveWriteLocalEvaluation(){
    BoneCurveWriteFixture f;f.first.bones[1].trans[0]=91;f.first.dirty[1]=3;bone_scale::SetPose(f.first.raw(),1,Scale(8));
    const auto peer=f.first.bones[1];const auto peerScale=bone_scale::Pose(f.first.raw(),1);f.first.bones[0].trans[0]=-45;
    auto edit=FreeBoneCurve();f.Commit(f.Plan({edit}));
    Check(std::abs(f.first.bones[0].trans[0]-3.f)<1e-6f&&f.first.dirty[0]==0,"clean edited bone responds locally to a committed free curve");
    Check(!std::memcmp(&peer,&f.first.bones[1],sizeof peer)&&bone_scale::Pose(f.first.raw(),1)==peerScale,"curve publication preserves peer dirty pose and scale");
    f.first.dirty[0]=1;f.first.bones[0].trans[0]=123;bone_scale::SetPose(f.first.raw(),0,Scale(7));f.app->CurrentFrame()=9;
    Check(f.history.History(false)==MMS_OK&&f.first.bones[0].trans[0]==123&&f.first.dirty[0]==1&&bone_scale::Pose(f.first.raw(),0)==Scale(7),
        "undo at another frame retains even the edited bone's unregistered dirty pose");
    f.first.dirty[0]=0;f.app->CurrentFrame()=15;Check(f.history.History(true)==MMS_OK&&std::abs(f.first.bones[0].trans[0]-9.f)<1e-6f,
        "redo evaluates the new curve at the current frame once the bone is clean");
    edit.tracks[0].keys[1].value=18;auto second=FreeBoneCurve(1);f.Commit(f.Plan({edit,second}));
    Check(f.history.UndoCount()==2&&bone_curve::Has(f.first.raw(),0)&&bone_curve::Has(f.first.raw(),1),"two edited bones publish as one shared history action");
    Check(f.history.History(false)==MMS_OK&&!bone_curve::Has(f.first.raw(),1),"one undo removes the complete multi-bone authoring transaction");
}
void BoneCurveWriteNativeAndGuards(){
    BoneCurveWriteFixture f;auto edit=NativeBoneCurve(f.first);const auto source=f.first.Digest();
    Check(!f.Plan({edit})&&f.first.Digest()==source,"unchanged native controls are a no-op without local or common history");
    edit.nativeControls[1].controls[0]={11,35,93,116};edit.nativeControls[1].controls[3]={15,44,98,111};edit.nativeControls[1].controls[4]={18,30,90,120};
    f.Commit(f.Plan({edit}));const auto& key=f.first.m->boneKeys[2];
    Check(key.interpolation[0]==11&&key.interpolation[4]==35&&key.interpolation[8]==93&&key.interpolation[12]==116&&
        bone_scale::KeyCurve(f.first.raw(),2)==edit.nativeControls[1].controls[4],"native position rotation and scale handles are written with the original byte layout");
    const auto before=RegistrationKeys(f.first);auto invalid=edit;invalid.nativeControls[1].frame=11;std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanBoneCurveWrite(*f.app,0,{invalid},nullptr,change)==MMS_E_ARGUMENT&&!change&&SameRegistrationKeys(f.first,before),"native curve rejects a stale source frame without partial publication");
    {
        BoneCurveWriteFixture timing;
        auto move=NativeBoneCurve(timing.first);
        auto* keys=timing.first.m->boneKeys;
        const auto original=keys[2];
        const auto scale=bone_scale::Key(timing.first.raw(),2);
        move.nativeControls[1].targetFrame=30;
        std::shared_ptr<plugins::HistoryChange> rejected;
        Check(plugins::PlanBoneCurveWrite(*timing.app,0,{move},nullptr,rejected)==MMS_E_ARGUMENT&&
            !rejected&&keys[2].frame==10,"native curve retiming refuses an occupied frame");
        move.nativeControls[1].targetFrame=17;
        timing.Commit(timing.Plan({move}));
        auto expected=original;expected.frame=17;
        Check(!std::memcmp(&keys[2],&expected,sizeof(expected))&&
            bone_scale::Key(timing.first.raw(),2)==scale,"shared history retimes the original key slot and preserves native payload");
        Check(timing.history.History(false)==MMS_OK&&keys[2].frame==10&&
            timing.history.History(true)==MMS_OK&&keys[2].frame==17,"shared undo and redo replay the native frame move");
    }
    auto next=FreeBoneCurve();change=f.Plan({next});std::unique_ptr<plugins::PreparedHistoryChange> token;
    Check(change->Prepare(true,token)==MMS_OK&&token->IsCurrent(),"prepare short bone curve publication token");
    f.first.m->boneKeys[1].allocated^=1;Check(!token->IsCurrent(),"short bone curve token freezes peer selection bytes too");token.reset();f.first.m->boneKeys[1].allocated^=1;
    const auto digest=f.first.Digest();Check(f.history.Configure(plugins::SharedHistory::RecordingMode::ExplicitOnly,8,1)==MMS_OK,"configure a rejecting bone curve memory budget");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("bone curves",change,transaction)==MMS_E_MEMORY&&!transaction&&f.first.Digest()==digest,"budget refusal retains native pose keys authoring and selection");
}
void BoneCurveWriteSelectionAndInterleave(){
    BoneCurveWriteFixture f;auto edit=FreeBoneCurve();f.Commit(f.Plan({edit}));
    unsigned index=0;for(;;){auto& key=f.first.m->boneKeys[index];key.allocated=key.frame==20?17:0;if(!key.next)break;index=key.next;}
    Check(f.history.History(false)==MMS_OK,"bone curve undo tolerates legal later selection changes");
    Check(f.first.m->boneKeys[0].allocated==0&&f.first.m->boneKeys[2].allocated==0&&f.first.m->boneKeys[3].allocated==0,
        "selection projection uses bone and frame identities and leaves restored missing frames unselected");
    f.first.m->boneKeys[0].allocated=9;Check(f.history.History(true)==MMS_OK&&f.first.m->boneKeys[0].allocated==9,"bone curve redo retains an independently selected frame-zero head");
    f.first.selected[0]=1;f.first.bones[0].trans[0]=6;f.first.dirty[0]=1;plugins::BoneRegistrationOptions options;options.frame=12;
    std::shared_ptr<plugins::HistoryChange> registration;Check(plugins::PlanBoneRegistration(*f.app,0,options,registration)==MMS_OK&&registration,"prepare native registration after a free curve");f.Commit(registration);
    bone_curve::Tracks canonical;Check(bone_curve::ReadDetached(f.first.raw(),0,canonical),"detached read sees registered free curve keys");
    edit.tracks=canonical;edit.tracks[0].keys.back().value+=2;f.Commit(f.Plan({edit}));
    Check(f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK,
        "curve registration curve sequence undoes across raw author snapshots");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,
        "curve registration curve sequence redoes in shared journal order");
}
void BoneCurveWriteDisplay(){
    BoneCurveWriteFixture f;std::array<mdl::IkChain,1> ik{};ik[0].enabled=1;
    f.first.m->ikChains=ik.data();f.first.m->ikChainCount=1;
    auto& first=f.first.m->displayKeys[0];auto& last=f.first.m->displayKeys[1];first.next=1;first.visible=1;
    last.frame=20;last.previous=0;last.visible=1;last.ikStates[0]=1;f.first.m->loadComplete=1;
    plugins::DisplayCurveRows rows{{0,0,{0}},{20,1,{1}}};const auto root=first,tail=last;
    f.first.bones[0].trans[0]=42;const auto pose=f.first.bones[0];
    f.Commit(f.Plan({},&rows));
    Check(first.visible==0&&first.ikStates[0]==0&&last.visible==1&&f.first.m->loadComplete==0&&ik[0].enabled==0,
        "display-only curve values update existing frames and current visibility IK without installing free bone authors");
    Check(first.frame==root.frame&&first.next==root.next&&first.selectorStates==root.selectorStates&&last.frame==tail.frame&&last.previous==tail.previous&&
        !bone_curve::Has(f.first.raw(),0)&&!std::memcmp(&pose,&f.first.bones[0],sizeof pose),"display curve edit preserves links parents and bone author pose; normal draw consumes the new IK flag");
    f.app->CurrentFrame()=25;Check(f.history.History(false)==MMS_OK&&first.visible==1&&first.ikStates[0]==1&&f.first.m->loadComplete==1&&ik[0].enabled==1,
        "display undo restores values and samples state at the current frame");
    auto edit=FreeBoneCurve();rows[1].visible=0;f.Commit(f.Plan({edit},&rows));
    Check(f.history.UndoCount()==1&&bone_curve::Has(f.first.raw(),0)&&last.visible==0,"bone and display values publish as one aggregate action");
    rows[1].frame=21;const auto source=f.first.Digest();std::shared_ptr<plugins::HistoryChange> rejected;
    Check(plugins::PlanBoneCurveWrite(*f.app,0,{},&rows,rejected)==MMS_E_ARGUMENT&&!rejected&&f.first.Digest()==source,"display retiming rejects the complete aggregate");
    f.first.m->ikChains=nullptr;f.first.m->ikChainCount=0;
}
void BoneCurveWriteGrowthAndDetached(){
    BoneCurveWriteFixture f;bone_scale::Forget(f.first.raw());mdl::BoneKeyPoolDestroy(f.first.m->boneKeys);
    f.first.m->boneKeys=mdl::BoneKeyPoolCreate(2);for(unsigned i=0;i<2;++i)f.first.m->boneKeys[i].rotation[3]=1;
    const auto before=RegistrationKeys(f.first);const auto identity=mdl::BoneKeyPoolIdentity(f.first.m->boneKeys);
    auto edit=FreeBoneCurve();f.Commit(f.Plan({edit}));const auto capacity=mdl::BoneKeyCapacity(f.first.m->boneKeys);
    Check(capacity>2&&mdl::BoneKeyPoolIdentity(f.first.m->boneKeys)==identity,"bone curve mirror grows privately with a stable native pool identity");
    Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,before)&&mdl::BoneKeyCapacity(f.first.m->boneKeys)==capacity,
        "bone curve growth undo retains allocation while restoring original native heads");
    Check(f.history.History(true)==MMS_OK,"bone curve growth redo resolves current allocation from its stable identity");
    const auto author=bone_curve::AuthoringState::Capture(f.first.raw());const auto index=f.first.m->boneKeys[0].next;
    f.first.m->boneKeys[index].position[0]+=2;bone_curve::Tracks canonical;
    Check(bone_curve::ReadDetached(f.first.raw(),0,canonical)&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author)&&
        canonical[0].keys.back().value==14,"detached canonical bone read reconciles native changes without modifying the captured raw author");
    f.first.m->boneKeys[index].position[0]-=2;
    Check(f.history.History(false)==MMS_OK,"restored native source remains traversable after detached read");
}
void BoneCurveWriteAliases(){
    for(unsigned variant=0;variant<3;++variant){BoneCurveWriteFixture f;
        auto* savedBones=f.second.m->boneTable;auto* savedDirty=f.second.m->bonePhysicsState;auto* savedDisplay=f.second.m->displayKeys;
        if(variant==0)f.second.m->boneTable=f.first.m->boneTable;
        else if(variant==1)f.second.m->bonePhysicsState=f.first.m->bonePhysicsState;
        else f.second.m->displayKeys=f.first.m->displayKeys;
        const auto first=f.first.Digest(),second=f.second.Digest();std::shared_ptr<plugins::HistoryChange> change;
        const plugins::DisplayCurveRows display{{0,0,{}}};const std::vector<plugins::BoneCurveEdit> edits{FreeBoneCurve()};
        const auto status=plugins::PlanBoneCurveWrite(*f.app,0,edits,variant==2?&display:nullptr,change);
        const bool unchanged=f.first.Digest()==first&&f.second.Digest()==second;
        f.second.m->boneTable=savedBones;f.second.m->bonePhysicsState=savedDirty;f.second.m->displayKeys=savedDisplay;
        Check(status==MMS_E_ARGUMENT&&!change&&unchanged,"cross-model bone table dirty state and display aliases reject without editing either model");
    }
    BoneCurveWriteFixture f;std::array<mdl::IkChain,1> firstIk{},secondIk{};
    f.first.m->ikChainCount=f.second.m->ikChainCount=1;f.first.m->ikChains=firstIk.data();f.second.m->ikChains=secondIk.data();
    auto* original=f.second.m->displayKeys[0].ikStates;f.second.m->displayKeys[0].ikStates=f.first.m->displayKeys[0].ikStates;
    const auto first=f.first.Digest(),second=f.second.Digest();plugins::DisplayCurveRows rows{{0,0,{0}}};std::shared_ptr<plugins::HistoryChange> change;
    const auto status=plugins::PlanBoneCurveWrite(*f.app,0,{},&rows,change);const bool unchanged=f.first.Digest()==first&&f.second.Digest()==second;
    f.second.m->displayKeys[0].ikStates=original;f.first.m->ikChains=f.second.m->ikChains=nullptr;f.first.m->ikChainCount=f.second.m->ikChainCount=0;
    Check(status==MMS_E_ARGUMENT&&!change&&unchanged,"aliased peer display IK payload rejects before any current state or key write");
    for(unsigned variant=0;variant<3;++variant){BoneCurveWriteFixture shared;
        auto* savedMorphs=shared.first.m->morphs;auto* savedCursors=shared.first.m->boneKeyCursors;auto* savedActive=shared.first.m->boneTrackActive;
        if(variant==0)shared.first.m->morphs=reinterpret_cast<mdl::MorphRecord*>(shared.first.m->boneTable);
        else if(variant==1)shared.first.m->boneKeyCursors=reinterpret_cast<std::uint32_t*>(&shared.app->state);
        else{shared.first.m->morphTrackActive=shared.first.dirty.data();shared.first.m->boneTrackActive=shared.first.dirty.data();}
        const auto source=shared.first.Digest();std::shared_ptr<plugins::HistoryChange> rejected;
        const auto result=plugins::PlanBoneCurveWrite(*shared.app,0,{FreeBoneCurve()},nullptr,rejected);const bool clean=shared.first.Digest()==source;
        shared.first.m->morphs=savedMorphs;shared.first.m->boneKeyCursors=savedCursors;shared.first.m->boneTrackActive=savedActive;shared.first.m->morphTrackActive=nullptr;
        Check(result==MMS_E_ARGUMENT&&!rejected&&clean,"bone curve authoring refuses morph-value cursor and app-state aliases before touching any participant");
    }
}
void BoneCurveWriteNumericGuards(){
    for(unsigned variant=0;variant<4;++variant){BoneCurveWriteFixture f;auto edit=FreeBoneCurve();
        if(variant==0)edit.tracks[0].keys.back().inSlope=std::numeric_limits<double>::infinity();
        else if(variant==1){edit.tracks[6].keys.back().interpolation=animation_curve::Interpolation::Bezier;edit.tracks[6].keys.back().inSlope=1000;}
        else if(variant==2){f.first.m->boneKeys[0].position[0]=-(std::numeric_limits<float>::max)();f.first.m->boneKeys[2].position[0]=(std::numeric_limits<float>::max)();}
        else{f.first.m->boneKeys[0].rotation[0]=1e30f;f.first.m->boneKeys[2].rotation[0]=1e30f;}
        const auto source=f.first.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanBoneCurveWrite(*f.app,0,{edit},nullptr,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==source,
            "future handle scale hull native float difference and quaternion-dot overflow reject before history publication");
    }
}
void BoneCurveWriteFailures(){
    unsigned failures=0;bool reached=false;
    for(long allocation=0;allocation<512;++allocation){BoneCurveWriteFixture f;const std::vector<plugins::BoneCurveEdit> inputs{FreeBoneCurve()};const auto source=f.first.Digest();
        std::shared_ptr<plugins::HistoryChange> change;std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        failAfter=allocation;auto status=plugins::PlanBoneCurveWrite(*f.app,0,inputs,nullptr,change);
        if(status==MMS_OK)status=f.history.PrepareNative("bone curves",change,token);failAfter=-1;
        Check(f.first.Digest()==source&&f.history.UndoCount()==0,"every bone curve allocation failure preserves live model and history");
        if(status==MMS_OK){Check(token&&failures>5,"allocation sweep reaches a complete bone curve candidate");reached=true;break;}
        Check(status==MMS_E_MEMORY&&!token,"bone curve preparation failures report memory without a publication token");++failures;
    }
    Check(reached,"bone curve allocation sweep terminates");
}
void NativeBoneCurveWriteChecks(){
    BoneCurveWriteOracle();BoneCurveWriteLocalEvaluation();BoneCurveWriteNativeAndGuards();BoneCurveWriteSelectionAndInterleave();BoneCurveWriteDisplay();BoneCurveWriteGrowthAndDetached();BoneCurveWriteAliases();BoneCurveWriteNumericGuards();BoneCurveWriteFailures();
}
}
