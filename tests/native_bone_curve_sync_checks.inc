// Include after native_model_vmd_checks.inc and native_bone_registration_checks.inc.
#include "../src/plugins/native_model_vmd_source.hpp"
#include <filesystem>
namespace {
using BSyncBytes=std::vector<std::uint8_t>;
bone_curve::Tracks BSyncTracks(){
    bone_curve::Tracks tracks;
    for(unsigned lane=0;lane<9;++lane){
        for(int frame:{0,20,40}){animation_curve::Key key;key.frame=frame;
            key.value=lane>=6?1+frame*.025:lane==0?frame*.5:lane==4?frame*27.0+.000012:0;
            key.tangent=animation_curve::Tangent::Free;key.inWeight=.21;key.outWeight=.18;
            key.inSlope=key.outSlope=lane>=6?.01:lane==0?.23:0;
            key.interpolation=lane==1?animation_curve::Interpolation::Step:animation_curve::Interpolation::Bezier;
            tracks[lane].keys.push_back(key);
        }
    }
    animation_curve::Key middle;middle.frame=10;middle.value=5;middle.tangent=animation_curve::Tangent::Free;
    tracks[0].keys.insert(tracks[0].keys.begin()+1,middle);
    return tracks;
}
void BSyncAuthor(Fixture& f){
    for(auto& bone:f.bones){bone.type=mdl::BoneType::RotateMove;bone.slotIndex=-1;}
    std::string error;
    Check(bone_curve::Write(f.raw(),0,BSyncTracks(),error)&&bone_curve::Write(f.raw(),1,RegistrationFreeTracks(),error),"seed sparse Euler-winding free curves and a separate authored bone");
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.m->boneKeys);++i)f.m->boneKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.m->morphKeys);++i)f.m->morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.m->displayKeys);++i)f.m->displayKeys[i].allocated=0;
    f.selected[0]=1;f.selected[1]=0;f.dirty[0]=f.dirty[1]=1;
}
unsigned BSyncAt(const Fixture& f,unsigned frame,unsigned bone=0){
    auto slot=bone;for(std::size_t hops=0;hops<mdl::BoneKeyCapacity(f.m->boneKeys);++hops){
        const auto& key=f.m->boneKeys[slot];if(key.frame==frame)return slot;if(!key.next)break;slot=key.next;}
    Check(false,"expected synchronized bone native key exists");return 0;
}
bone_curve::Tracks BSyncRead(Fixture& f,unsigned bone=0){bone_curve::Tracks out;
    Check(bone_curve::Read(f.raw(),int(bone),out),"controlled edit retains readable free bone authoring");return out;}
BSyncBytes BSyncEncode(const bone_curve::Tracks& tracks){
    std::vector<animation_curve::NamedTrack> named;for(unsigned lane=0;lane<tracks.size();++lane)named.push_back({std::to_string(lane),tracks[lane]});
    BSyncBytes out;std::string error;Check(animation_curve::Encode(named,out,error),"encode all free bone values and handles exactly");return out;
}
const animation_curve::Key* BSyncKey(const animation_curve::Track& track,int frame){
    const auto at=std::find_if(track.keys.begin(),track.keys.end(),[&](const auto& key){return key.frame==frame;});return at==track.keys.end()?nullptr:&*at;
}
void BSyncPoseAt(Fixture& f,unsigned frame){
    const auto index=BSyncAt(f,frame);const auto& key=f.m->boneKeys[index];
    std::memcpy(f.bones[0].trans,key.position,sizeof(key.position));std::memcpy(f.bones[0].rotQuat,key.rotation,sizeof(key.rotation));f.dirty[0]=0;
    Check(bone_scale::SetPose(f.raw(),0,bone_scale::Key(f.raw(),int(index))),"set the exact same authored pose and scale as its native mirror");
}
plugins::ModelPasteSource BSyncSource(Fixture& f,unsigned frame,unsigned offset=5){
    const auto index=BSyncAt(f,frame);const auto& key=f.m->boneKeys[index];plugins::ModelPasteSource source;source.frameOffset=offset;
    plugins::ModelBonePasteRecord record;record.targetResolved=true;record.targetIndex=0;record.key.frame=frame-offset;
    std::memcpy(record.key.name,f.bones[0].name,sizeof(f.bones[0].name));std::memcpy(record.key.position,key.position,sizeof(key.position));
    std::memcpy(record.key.rotation,key.rotation,sizeof(key.rotation));std::memcpy(record.key.interpolation,key.interpolation,sizeof(key.interpolation));
    record.key.physicsDisabled=key.physicsDisabled;record.scale=bone_scale::Key(f.raw(),int(index));record.scaleCurve=bone_scale::KeyCurve(f.raw(),int(index));
    source.bones.push_back(record);return source;
}
void BSyncCommit(BoneRegistrationFixture& f,const std::shared_ptr<plugins::HistoryChange>& change){
    Check(change!=nullptr,"bone curve synchronization owns a changed native or authored transaction");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("synchronize free bone curves",change,transaction)==MMS_OK,"prepare complete native keys, scale and reconciled free authoring");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"publish native and reconciled authoring together without allocation");
}
void BSyncExplicitRoundTrip(){
    for(bool paste:{false,true})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        BoneRegistrationFixture f;BSyncAuthor(f.first);BSyncPoseAt(f.first,10);f.app->CurrentFrame()=7;
        Check(f.history.SetMode(mode)==MMS_OK,"free bone explicit history mode configured");
        const auto before=BSyncEncode(BSyncRead(f.first));const auto other=BSyncEncode(BSyncRead(f.first,1));
        const auto original=RegistrationKeys(f.first);const auto ring=PoseRingDigest(f.first);const auto digest=f.first.Digest();
        const auto scale=bone_scale::AuthoringState::Capture(f.first.raw());auto source=BSyncSource(f.first,10);std::shared_ptr<plugins::HistoryChange> change;
        if(paste){plugins::ModelPasteFamilyResult family;
            Check(plugins::PlanBoneKeyPaste(*f.app,0,false,source,change,&family)==MMS_OK&&change&&family.dataChanged,
                "same-valued bone paste reports missing sparse lane keys as a family data change");
            Check(plugins::PlanBoneKeyPaste(*f.app,0,false,source,change)==MMS_OK,"same-valued bone paste privately plans explicit sparse registration");
        }else{plugins::BoneRegistrationOptions options;options.frame=10;
            Check(plugins::PlanBoneRegistration(*f.app,0,options,change)==MMS_OK,"same-valued bone registration privately plans sparse lane completion");}
        Check(change&&f.first.Digest()==digest&&BSyncEncode(BSyncRead(f.first))==before,"free-curve planning preserves all native pose, ring, scale and live authored data");
        source={};BSyncCommit(f,change);const auto afterTracks=BSyncRead(f.first);const auto after=BSyncEncode(afterTracks);
        Check(std::all_of(afterTracks.begin(),afterTracks.end(),[](const auto& track){return BSyncKey(track,10)!=nullptr;})&&
            BSyncKey(afterTracks[4],20)->value==540.000012&&BSyncKey(afterTracks[4],40)->value==1080.000012&&BSyncEncode(BSyncRead(f.first,1))==other,
            "explicit sparse completion preserves untouched Euler revolutions, high precision and another bone's free handles");
        Check(PoseRingDigest(f.first)==ring&&f.history.UndoCount()==1&&f.app->CurrentFrame()==7,"explicit synchronized authoring records once in both modes without legacy ring or playhead writes");
        f.app->CurrentFrame()=27;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,original)&&BSyncEncode(BSyncRead(f.first))==before&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale),"undo restores exact sparse tracks, scale, marks and native keys across selected model and mode changes");
        Check(f.history.History(true)==MMS_OK&&BSyncEncode(BSyncRead(f.first))==after&&f.app->CurrentFrame()==27&&PoseRingDigest(f.first)==ring,
            "redo restores the owned reconciled tracks after source destruction without seeking");
        f.app->SelectedModelSlot()=0;f.app->CameraMode()=0;BSyncPoseAt(f.first,10);
        if(paste){const auto repeated=BSyncSource(f.first,10);Check(plugins::PlanBoneKeyPaste(*f.app,0,false,repeated,change)==MMS_OK&&!change,
            "repeated identical bone paste is a true no-op after sparse lane completion");}
        else{plugins::BoneRegistrationOptions options;options.frame=10;Check(plugins::PlanBoneRegistration(*f.app,0,options,change)==MMS_OK&&!change,
            "repeated identical bone registration is a true no-op after sparse lane completion");}
    }
}
void BSyncRetimeAndDeletion(){
    for(unsigned action=0;action<5;++action){
        BoneRegistrationFixture f;BSyncAuthor(f.first);std::shared_ptr<plugins::HistoryChange> change;
        if(action<2){plugins::BoneRangeScaleOptions options;options.start=action?0:10;options.end=40;options.scale=action?.01:2;
            const auto old=BSyncAt(f.first,40);const bone_scale::Curve curve{11,29,93,121};bone_scale::SetKeyCurve(f.first.raw(),int(old),curve);
            // Reconcile that prior, independent scale-control edit before capture.
            BSyncRead(f.first);Check(plugins::PlanBoneRangeScale(*f.app,0,options,change)==MMS_OK,"range retime privately keeps explicit original-to-final frame identities");
        }else if(action<4)Check(plugins::PlanBoneFrameLine(*f.app,0,20,action==2,false,change)==MMS_OK,"bone frame-line edit privately retimes its retained free keys");
        else{f.first.m->boneKeys[BSyncAt(f.first,20)].allocated=1;
            Check(plugins::PlanBoneKeyDeletion(*f.app,0,false,change)==MMS_OK,"native deletion privately rebuilds surviving free bone lanes");}
        // Native scale-control setup above is part of the operation's source.
        const auto pristine=BSyncEncode(BSyncRead(f.first));const auto scaleSource=bone_scale::AuthoringState::Capture(f.first.raw());const auto nativeSource=RegistrationKeys(f.first);
        BSyncCommit(f,change);const auto tracks=BSyncRead(f.first);const auto after=BSyncEncode(tracks);
        const int end=action==0?70:action==1?0:action==2?41:action==3?39:40;
        Check(BSyncKey(tracks[4],end)&&BSyncKey(tracks[4],end)->value==1080.000012,"retiming/deletion preserves the surviving authored Euler winding even when native slots are recycled or the root is overwritten");
        if(action==0)Check(!BSyncKey(tracks[4],10)&&BSyncKey(tracks[4],30)&&BSyncKey(tracks[4],30)->value==540.000012&&
            tracks[0].keys.front().outWeight==.18,"range retime preserves sparse lanes and normalized free handles instead of matching recycled slots");
        if(action==1)Check(tracks[4].keys.size()==1&&BSyncKey(tracks[6],0)->value==2,
            "root collision retains the last actual source slot's authored Euler and scale values");
        if(action==4)Check(!BSyncKey(tracks[4],20),"deleting one native key removes only its authored frame");
        if(action<2)Check(bone_scale::KeyCurve(f.first.raw(),int(BSyncAt(f.first,unsigned(end))))==bone_scale::Curve({11,29,93,121}),
            "retime carries the winning source scale interpolation curve to its new native slot");
        f.app->CurrentFrame()=17;
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,nativeSource)&&BSyncEncode(BSyncRead(f.first))==pristine&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleSource),"retime/deletion undo restores exact source free handles, scale curves and native payload");
        Check(f.history.History(true)==MMS_OK&&BSyncEncode(BSyncRead(f.first))==after&&f.app->CurrentFrame()==17,"retime/deletion redo restores frozen reconciled authoring at another playhead");
    }
}
void BSyncVmdImport(){
    for(int version:{1,2})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        ModelPasteFixture f;BSyncAuthor(f.first);Check(f.history.SetMode(mode)==MMS_OK,"configure free-curve model VMD recording mode");
        const auto record=BSyncSource(f.first,10).bones.front();VmdDecodeBytes bytes;
        VmdDecodeText(bytes,version==1?"Vocaloid Motion Data file":"Vocaloid Motion Data 0002",30);
        VmdDecodeText(bytes,"bone-curve",version==1?10:20);VmdDecodePut(bytes,1u);
        VmdDecodeText(bytes,record.key.name,15);VmdDecodePut(bytes,record.key.frame);
        for(float value:record.key.position)VmdDecodePut(bytes,value);for(float value:record.key.rotation)VmdDecodePut(bytes,value);
        const auto controls=bytes.size();bytes.resize(controls+64,0);
        for(unsigned channel=0;channel<4;++channel)for(unsigned control=0;control<4;++control)
            bytes[controls+channel*16+control*4]=record.key.interpolation[channel+4*control];
        for(unsigned section=0;section<5;++section)VmdDecodePut(bytes,0u);
        bone_scale_io::VmdExtension extension;bone_scale_io::VmdExtension::Entry scale;
        std::memcpy(scale.name.data(),record.key.name,15);scale.frame=record.key.frame;scale.scale=record.scale;scale.curve=record.scaleCurve;extension.entries.push_back(scale);
        VmdDecodeFile file;file.Write(ModelVmdWithScale(std::move(bytes),extension));plugins::ModelVmdSource source;
        Check(plugins::DecodeModelVmdSource(nullptr,file.path.c_str(),5,source)==MMS_OK&&source.source.bones.size()==1,
            "actual standard VMD and scale tail decode a same-valued sparse bone registration");
        const ModelKeyState before(f.first);const auto beforeCurves=BSyncEncode(BSyncRead(f.first));const auto ring=PoseRingDigest(f.first);
        const auto beforeScale=bone_scale::AuthoringState::Capture(f.first.raw());const auto digest=f.first.Digest();
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanModelVmdImport(*f.app,0,source,change)==MMS_OK&&change&&f.first.Digest()==digest&&BSyncEncode(BSyncRead(f.first))==beforeCurves,
            "same-valued immutable VMD plans all sparse lane additions without live mutation");
        source={};Check(DeleteFileW(file.path.c_str()),"remove sparse bone VMD source before publication");ModelPasteCommit(f,change);
        const auto tracks=BSyncRead(f.first);const auto afterCurves=BSyncEncode(tracks);const ModelKeyState after(f.first);
        const auto afterScale=bone_scale::AuthoringState::Capture(f.first.raw());
        Check(std::all_of(tracks.begin(),tracks.end(),[](const auto& track){return BSyncKey(track,10)!=nullptr;})&&
            BSyncKey(tracks[4],20)->value==540.000012&&BSyncKey(tracks[4],40)->value==1080.000012&&
            f.history.UndoCount()==1&&f.app->CurrentFrame()==5&&PoseRingDigest(f.first)==ring,
            "VMD1/2 explicitly registers every lane while retaining distant winding and one history step in both modes");
        f.app->CurrentFrame()=29;f.app->CameraMode()=1;f.app->SelectedModelSlot()=1;
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&BSyncEncode(BSyncRead(f.first))==beforeCurves&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(beforeScale),"VMD undo restores exact sparse authoring, native keys and scale after source deletion");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&BSyncEncode(BSyncRead(f.first))==afterCurves&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(afterScale)&&f.app->CurrentFrame()==29&&PoseRingDigest(f.first)==ring,
            "VMD redo owns all authored data across frame and model selection changes");
    }
}
void BSyncFailures(){
    BoneRegistrationFixture f;BSyncAuthor(f.first);BSyncPoseAt(f.first,10);plugins::BoneRegistrationOptions options;options.frame=10;
    const auto before=f.first.Digest();const auto curves=bone_curve::AuthoringState::Capture(f.first.raw());const auto scale=bone_scale::AuthoringState::Capture(f.first.raw());
    std::shared_ptr<plugins::HistoryChange> change;bool completed=false;
    for(long allowance=0;allowance<2048;++allowance){failAfter=allowance;const auto result=plugins::PlanBoneRegistration(*f.app,0,options,change);failAfter=-1;
        Check(f.first.Digest()==before&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*curves)&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale),
            "failed sparse bone planning leaves native data, pose, scale, free authoring and ring unchanged");
        if(result==MMS_OK){completed=bool(change);break;}Check(result==MMS_E_MEMORY&&!change,"sparse bone planning reports OOM without partial authoring");}
    Check(completed,"bone curve synchronization planning OOM sweep completes");completed=false;
    for(long allowance=0;allowance<2048;++allowance){std::unique_ptr<plugins::PreparedHistoryChange> prepared;
        failAfter=allowance;const auto result=change->Prepare(true,prepared);failAfter=-1;
        Check(f.first.Digest()==before&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*curves),"failed native/curve preparation publishes no participant");
        if(result==MMS_OK){completed=prepared&&prepared->IsCurrent();break;}Check(result==MMS_E_MEMORY&&!prepared,"bone curve preparation reports detached-copy allocation failure");}
    Check(completed,"bone curve synchronization candidate OOM sweep completes");
    plugins::SharedHistory budget(f.storage,{},32,change->RetainedBytes()-1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(budget.PrepareNative("bone curve budget",change,transaction)==MMS_E_MEMORY&&!transaction&&f.first.Digest()==before,"both curve snapshots and frozen native dependencies are charged before publication");
    auto& untouched=f.first.m->boneKeys[BSyncAt(f.first,40)];untouched.position[0]+=1;
    std::unique_ptr<plugins::PreparedHistoryChange> prepared;
    Check(change->Prepare(true,prepared)==MMS_E_STALE&&!prepared,"first preparation rejects an untouched native dependency changed since curve planning");untouched.position[0]-=1;
    Check(f.history.PrepareNative("stale bone curve",change,transaction)==MMS_OK,"prepare bone synchronization before another author's free-curve edit");
    auto external=BSyncRead(f.first);external[0].keys.front().outWeight=.23;std::string error;
    Check(bone_curve::Write(f.first.raw(),0,external,error),"external authored handle edit replaces the prepared curve source");const auto stale=f.first.Digest();
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_E_STALE&&f.first.Digest()==stale&&!f.history.UndoCount(),"stale curve source cannot publish native keys or overwrite the other author's handles");
}
void NativeBoneCurveSyncChecks(){
    {
        BoneRegistrationFixture f;BSyncAuthor(f.first);f.app->CurrentFrame()=20;BSyncPoseAt(f.first,20);
        f.first.m->boneKeys[BSyncAt(f.first,20)].allocated=1;
        const auto before=f.first.Digest();std::shared_ptr<plugins::HistoryChange> change;
        plugins::BoneRegistrationOptions options;options.frame=20;
        Check(plugins::PlanBoneRegistration(*f.app,0,options,change)==MMS_OK&&!change&&f.first.Digest()==before,
            "fully authored same-value bone registration ignores stale selection bytes in its native baseline");
    }
    std::puts("  controlled bone curves: explicit sparse registration and paste");BSyncExplicitRoundTrip();
    std::puts("  controlled bone curves: deletion, retime, recycled slots and root collision");BSyncRetimeAndDeletion();
    std::puts("  controlled bone curves: actual VMD1/2 decode, scale extension and exact sparse undo");BSyncVmdImport();
    std::puts("  controlled bone curves: preparation failures, budgets and frozen sources");BSyncFailures();
}
}
