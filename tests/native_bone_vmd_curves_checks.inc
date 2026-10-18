// Include after native_bone_registration_checks.inc and model-paste helpers.
// The legacy ApplyVmd/registrar remain independent native-byte oracles.
#include "mikudancestudio/extended_names.hpp"
namespace {
using BoneVmdBytes=std::vector<std::uint8_t>;
std::string BoneVmdId(const char* name,unsigned owner,unsigned lane){
    std::string id="bone/"+std::to_string(owner)+"/"+std::to_string(lane)+"/";
    constexpr char hex[]="0123456789abcdef";for(unsigned i=0;i<20;++i){const auto value=static_cast<unsigned char>(name[i]);id+=hex[value>>4];id+=hex[value&15];}return id;
}
bone_curve::Tracks BoneVmdTracks(double value=12){
    auto tracks=RegistrationFreeTracks();
    tracks[0].keys.back().value=value;tracks[4].keys.back().value=1080.000012;
    animation_curve::Key middle;middle.frame=10;middle.value=value*.37;middle.inSlope=.17;middle.outSlope=.43;
    middle.inWeight=.19;middle.outWeight=.31;middle.tangent=animation_curve::Tangent::Free;
    tracks[0].keys.insert(tracks[0].keys.begin()+1,middle);
    tracks[1].keys.back().interpolation=animation_curve::Interpolation::Step;
    return tracks;
}
BoneVmdBytes BoneVmdBlock(const std::vector<animation_curve::NamedTrack>& tracks){
    BoneVmdBytes body;std::string error;Check(animation_curve::Encode(tracks,body,error),"encode original bone curve wire payload");
    BoneVmdBytes block{'M','D','S','B','C','R','V','1'};
    for(unsigned byte=0;byte<4;++byte)block.push_back(std::uint8_t(body.size()>>(8*byte)));
    block.insert(block.end(),body.begin(),body.end());return block;
}
void BoneVmdAppend(std::vector<animation_curve::NamedTrack>& named,const char* name,unsigned owner,const bone_curve::Tracks& tracks){
    for(unsigned lane=0;lane<9;++lane)named.push_back({BoneVmdId(name,owner,lane),tracks[lane]});
}
BoneVmdBytes BoneVmdEncoded(const bone_curve::Tracks& tracks){
    std::vector<animation_curve::NamedTrack> named;for(unsigned lane=0;lane<9;++lane)named.push_back({std::to_string(lane),tracks[lane]});
    BoneVmdBytes out;std::string error;Check(animation_curve::Encode(named,out,error),"encode exact nine-lane author state");return out;
}
struct BoneVmdFixture:PoseNumericFixture{
    std::array<std::uint32_t,2> firstCursors{0,1},secondCursors{0,1};
    std::array<unsigned char,2> firstActive{3,5},secondActive{3,5};
    BoneVmdFixture(){for(auto* f:{&first,&second}){
        f->m->ikChainCount=f->m->boneOrderCount=0;f->m->maxFrame=0;f->m->searchCursor=5;
        for(std::size_t slot=0;slot<mdl::BoneKeyCapacity(f->m->boneKeys);++slot)f->m->boneKeys[slot]={};
        for(unsigned bone=0;bone<2;++bone){f->bones[bone].type=mdl::BoneType::RotateMove;f->bones[bone].slotIndex=-1;
            f->m->boneKeys[bone].rotation[3]=1;std::memcpy(f->bones[bone].name,bone?"bone B":"bone A",7);}
        f->m->boneKeys[0].allocated=7;f->m->boneKeys[1].allocated=9;
    }
        first.m->boneKeyCursors=firstCursors.data();second.m->boneKeyCursors=secondCursors.data();
        first.m->boneTrackActive=firstActive.data();second.m->boneTrackActive=secondActive.data();
        auto& entry=second.m->undoRings[0].slots[second.m->undoState[0]];
        entry.auxiliaryPose=undo_storage::MallocOrThrow(512*sizeof(mdl::BoneKeySnapshot));entry.dirty=0;
        mdl::BoneKeyVisitClear(*second.m);app->CurrentFrame()=5;app->LastRegisteredFrame()=3;
    }
    ~BoneVmdFixture(){history.Reset();history.Collect();bone_curve::Forget(first.raw());bone_curve::Forget(second.raw());
        first.m->boneKeyCursors=second.m->boneKeyCursors=nullptr;first.m->boneTrackActive=second.m->boneTrackActive=nullptr;}
    plugins::ModelPasteSource Source(const std::vector<std::pair<unsigned,bone_curve::Tracks>>& imports,unsigned offset=0,bool standard=true){
        plugins::ModelPasteSource source;source.frameOffset=offset;source.advanceLastRegisteredFrame=true;
        std::vector<animation_curve::NamedTrack> named;
        for(const auto& imported:imports){const auto bone=imported.first;BoneVmdAppend(named,first.bones[bone].name,bone,imported.second);
            if(standard){std::set<int> frames;for(const auto& track:imported.second)for(const auto& key:track.keys)frames.insert(key.frame);
                for(int frame:frames){plugins::ModelBonePasteRecord record;record.targetResolved=true;record.targetIndex=int(bone);
                    std::memcpy(record.key.name,first.bones[bone].name,20);record.key.frame=unsigned(frame);record.key.rotation[3]=1;
                    record.key.position[0]=float(frame)*.25f;record.key.physicsDisabled=std::uint8_t(frame%3);
                    for(unsigned lane=0;lane<4;++lane)for(unsigned c=0;c<4;++c)record.key.interpolation[lane+4*c]=c<2?20:107;
                    record.scale.v={1,.3f,0,.2f,1,0,0,.1f,1};record.scaleCurve={11,28,92,119};source.bones.push_back(record);}}
        }
        source.boneCurveBlock=BoneVmdBlock(named);return source;
    }
    std::shared_ptr<plugins::HistoryChange> Plan(const plugins::ModelPasteSource& source,plugins::ModelPasteReport* report=nullptr){
        std::shared_ptr<plugins::HistoryChange> out;Check(plugins::PlanModelKeyPaste(*app,0,source,out,report)==MMS_OK,"plan standard bone keys scale and author tail atomically");return out;
    }
    void Commit(const std::shared_ptr<plugins::HistoryChange>& change){
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(change&&history.PrepareNative("bone VMD curves",change,transaction,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"prepare one explicit standard and bone author history node");
        Check(CommitRegistrationNoAllocation(history,std::move(transaction))==MMS_OK,"publish standard bone scale author and metadata without allocation");
    }
    void Oracle(const plugins::ModelPasteSource& source){
        for(std::size_t index=0;index<mdl::BoneKeyCapacity(second.m->boneKeys);++index)second.m->boneKeys[index].allocated=0;
        for(std::size_t index=0;index<mdl::MorphKeyCapacity(second.m->morphKeys);++index)second.m->morphKeys[index].allocated=0;
        for(std::size_t index=0;index<mdl::KeyPoolCapacity(second.m->displayKeys);++index)second.m->displayKeys[index].allocated=0;
        ResetBoneKeyCursor(second.raw());
        for(auto record:source.bones){bone_scale::Registration scale(record.scale,record.scaleCurve);
            Check(RegisterBoneKey(second.raw(),reinterpret_cast<unsigned char*>(&record.key),int(source.frameOffset),0),"legacy standard registrar sees imported scale basis before author mirror");}
        ResetDisplayKeyCursor(second.raw());std::string error;
        extended_names::MotionScope names(false,source.curveTargetsResolved?&source.boneCurveTargets:nullptr,nullptr);
        Check(bone_curve::ApplyVmd(second.raw(),source.boneCurveBlock.data(),source.boneCurveBlock.size(),int(source.frameOffset),error),"old ApplyVmd accepts independent bone author oracle");
    }
    void Same(unsigned bone){bone_curve::Tracks a,b;
        Check(bone_curve::ReadDetached(first.raw(),int(bone),a)&&bone_curve::Read(second.raw(),int(bone),b)&&BoneVmdEncoded(a)==BoneVmdEncoded(b),"import retains sparse nine-lane frames full Euler winding handles and step intervals");
        for(unsigned lane=0;lane<9;++lane)for(int sample=0;sample<=160;++sample)
            Check(animation_curve::Evaluate(a[lane],sample*.25).value==animation_curve::Evaluate(b[lane],sample*.25).value,"dense subframe bone samples match original ApplyVmd");
    }
};
void BoneVmdCurveParity(){
    for(unsigned offset:{0u,7u})for(bool multiple:{false,true}){BoneVmdFixture f;std::vector<std::pair<unsigned,bone_curve::Tracks>> imports{{0,BoneVmdTracks()}};
        if(multiple)imports.push_back({1,BoneVmdTracks(-8)});auto source=f.Source(imports,offset);const auto digest=f.first.Digest(),ring=PoseRingDigest(f.first);
        const auto before=RegistrationKeys(f.first);const auto scale=bone_scale::AuthoringState::Capture(f.first.raw());auto author=bone_curve::AuthoringState::Capture(f.first.raw());
        auto change=f.Plan(source);Check(change&&f.first.Digest()==digest&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale)&&
            bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author),"bone author decoding planning and growth leave live state untouched");f.Oracle(source);f.Commit(change);
        Check(SameRegistrationKeys(f.first,RegistrationKeys(f.second))&&f.first.m->searchCursor==f.second.m->searchCursor&&f.first.m->maxFrame==f.second.m->maxFrame,
            "bone VMD first publication matches legacy native bytes selection maximum and search cursor");
        for(unsigned slot=0;slot<32;++slot)Check(bone_scale::Key(f.first.raw(),int(slot))==bone_scale::Key(f.second.raw(),int(slot))&&
            bone_scale::KeyCurve(f.first.raw(),int(slot))==bone_scale::KeyCurve(f.second.raw(),int(slot)),"bone VMD mirror samples imported sheared scale basis and replaces controls exactly");
        for(const auto& imported:imports){f.Same(imported.first);Check(f.firstCursors[imported.first]==imported.first&&f.firstActive[imported.first]==1,"first imported free bone resets its native playback cursor and active marker");}
        Check(f.app->LastRegisteredFrame()==20+offset&&f.history.UndoCount()==1&&PoseRingDigest(f.first)==ring,"standard extent records once and native legacy ring remains unmodified");
        const auto after=RegistrationKeys(f.first);source={};f.app->CurrentFrame()=16;f.app->SelectedModelSlot()=1;
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,before)&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale)&&
            bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author)&&f.app->LastRegisteredFrame()==3,"undo restores raw author scale and keys at another frame without original file");
        Check(f.history.History(true)==MMS_OK&&SameRegistrationKeys(f.first,after)&&f.app->CurrentFrame()==16&&PoseRingDigest(f.first)==ring,"redo restores retained bone tail without moving playhead or native ring");
        for(const auto& imported:imports)f.Same(imported.first);
    }
}
void BoneVmdCurveMapping(){
    {
        BoneVmdFixture f;std::memcpy(f.first.bones[1].name,f.first.bones[0].name,20);std::memcpy(f.second.bones[1].name,f.second.bones[0].name,20);
        auto source=f.Source({{0,BoneVmdTracks()}},0,false);f.Oracle(source);f.Commit(f.Plan(source));f.Same(0);f.Same(1);
        Check(f.first.m->maxFrame==20&&f.app->LastRegisteredFrame()==3,"tail-only duplicate legacy names mirror both bones without extending pre-tail registration marker");
    }
    {
        BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()}});source.curveTargetsResolved=true;source.boneCurveTargets={1};
        for(auto& record:source.bones){record.targetIndex=1;std::memcpy(record.key.name,f.first.bones[1].name,20);}f.Oracle(source);f.Commit(f.Plan(source));
        Check(!bone_curve::Has(f.first.raw(),0)&&bone_curve::Has(f.first.raw(),1),"explicit source dictionary owner mapping overrides encoded raw bone name");f.Same(1);
    }
    for(bool empty:{false,true}){BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()}});source.curveTargetsResolved=true;
        if(!empty)source.boneCurveTargets={-1};plugins::ModelPasteReport report;auto change=f.Plan(source,&report);f.Commit(change);
        Check(!report.boneCurvesSkipped&&!bone_curve::Has(f.first.raw(),0),"explicit empty or skipped Unicode target list never falls back to raw names");
    }
    {
        BoneVmdFixture f;auto tracks=BoneVmdTracks();std::vector<animation_curve::NamedTrack> named;BoneVmdAppend(named,f.first.bones[0].name,7,tracks);
        auto other=tracks;other[0].keys.back().value=99;BoneVmdAppend(named,f.first.bones[0].name,8,other);
        auto source=f.Source({{0,tracks}});source.boneCurveBlock=BoneVmdBlock(named);f.Oracle(source);f.Commit(f.Plan(source));f.Same(0);
    }
}
void BoneVmdCurveSkipAndRollback(){
    for(unsigned reason=0;reason<4;++reason){BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()},{1,BoneVmdTracks(-7)}});
        if(reason==0)source.boneCurveBlock[12]^=0x7f;
        if(reason==1){auto extra=source.bones.back();extra.key.frame=17;source.bones.push_back(extra);}
        if(reason==2){std::vector<animation_curve::NamedTrack> named;BoneVmdAppend(named,f.first.bones[0].name,0,BoneVmdTracks());auto invalid=BoneVmdTracks();invalid[6].keys.back().value=1001;
            BoneVmdAppend(named,f.first.bones[1].name,1,invalid);source.boneCurveBlock=BoneVmdBlock(named);}
        if(reason==3){source.frameOffset=unsigned(frame_limits::maximum)-10;for(auto& key:source.bones)key.key.frame=0;}
        auto standard=source;standard.boneCurveBlock.clear();auto clean=f.Plan(standard);f.Commit(clean);const auto expected=RegistrationKeys(f.first);
        const auto scale=bone_scale::AuthoringState::Capture(f.first.raw());Check(f.history.History(false)==MMS_OK,"restore pre-standard state for optional-tail failure comparison");
        plugins::ModelPasteReport report;f.Commit(f.Plan(source,&report));
        Check(report.boneCurvesSkipped&&SameRegistrationKeys(f.first,expected)&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale)&&
            !bone_curve::Has(f.first.raw(),0)&&!bone_curve::Has(f.first.raw(),1),"invalid payload missing native frame scale hull or tail frame overflow skips entire author block while preserving standard+scale");
        Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,"skipped whole bone block retains atomic ordinary standard undo redo");
    }
    {
        BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()}});std::vector<animation_curve::NamedTrack> named;auto tracks=BoneVmdTracks();
        for(unsigned lane=0;lane<8;++lane)named.push_back({BoneVmdId(f.first.bones[0].name,0,lane),tracks[lane]});source.boneCurveBlock=BoneVmdBlock(named);
        plugins::ModelPasteReport report;f.Commit(f.Plan(source,&report));Check(!report.boneCurvesSkipped&&!bone_curve::Has(f.first.raw(),0),"incomplete nine-lane owner is silently ignored like old ApplyVmd");
    }
}
void BoneVmdCurveGrowthAndCandidates(){
    {
        BoneVmdFixture f;auto tracks=BoneVmdTracks();for(unsigned lane=0;lane<9;++lane){tracks[lane].keys.clear();for(int frame=0;frame<=48;++frame){animation_curve::Key key;key.frame=frame;key.value=lane>=6?1:lane==0?frame*.25:0;key.interpolation=animation_curve::Interpolation::Linear;tracks[lane].keys.push_back(key);}}
        auto source=f.Source({{0,tracks}},0,false);const auto identity=mdl::BoneKeyPoolIdentity(f.first.m->boneKeys);const auto before=RegistrationKeys(f.first);auto change=f.Plan(source);
        f.Oracle(source);f.Commit(change);Check(mdl::BoneKeyCapacity(f.first.m->boneKeys)>32&&mdl::BoneKeyPoolIdentity(f.first.m->boneKeys)==identity,"bone tail-only import grows managed storage while keeping stable pool identity");
        f.Same(0);Check(SameRegistrationKeys(f.first,RegistrationKeys(f.second)),"grown private tail mirror matches original Write slot allocation and full native bytes");
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,before)&&f.history.History(true)==MMS_OK,"grown tail owns exact undo redo payload across pool replacement");
    }
    {
        BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()}});auto change=f.Plan(source);const auto digest=f.first.Digest();std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent()&&f.first.Digest()==digest,"prepare complete bone-tail short token without changing live keys or authoring");
        ++f.firstCursors[0];Check(!candidate->IsCurrent(),"short bone tail token freezes the actual playback cursor table");--f.firstCursors[0];candidate.reset();
        ++f.firstActive[0];Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate,"first bone tail prepare refuses an active flag changed since planning");--f.firstActive[0];
        Check(f.history.Configure(plugins::SharedHistory::RecordingMode::ExplicitOnly,8,1)==MMS_OK,"configure rejecting whole bone author history retention budget");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;Check(f.history.PrepareNative("bone tail budget",change,transaction)==MMS_E_MEMORY&&!transaction&&f.first.Digest()==digest,"budget failure cannot leave standard keys or imported authoring behind");
    }
    {
        BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()}});f.first.m->boneKeyCursors=reinterpret_cast<std::uint32_t*>(f.first.m->boneKeys);
        const auto digest=f.first.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanBoneKeyPaste(*f.app,0,false,source,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==digest,"standalone family rejects tail cursor writes aliased into its native key pool");f.first.m->boneKeyCursors=f.firstCursors.data();
    }
    for(unsigned reason=0;reason<4;++reason){
        BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()}});std::array<std::uint32_t,2> indices{13,17};
        auto& target=*(reason>=2?f.second.m:f.first.m);const auto savedBone=target.boneKeyIndices,savedMorph=target.morphKeyIndices;
        if(reason%2)target.morphKeyIndices=indices.data();else target.boneKeyIndices=indices.data();f.first.m->boneKeyCursors=indices.data();
        const auto digest=f.first.Digest();std::shared_ptr<plugins::HistoryChange> change;
        const auto result=plugins::PlanBoneKeyPaste(*f.app,0,false,source,change);
        Check(result==MMS_E_ARGUMENT&&!change&&f.first.Digest()==digest&&indices==std::array<std::uint32_t,2>{13,17},
            "standalone bone tail rejects cursors aliased into own or peer bone and morph key-index tables without writes");
        f.first.m->boneKeyCursors=f.firstCursors.data();target.boneKeyIndices=savedBone;target.morphKeyIndices=savedMorph;
    }
}
void BoneVmdCurveLateAliasChecks(){
    for(unsigned reason=0;reason<4;++reason)for(unsigned stage=0;stage<3;++stage){
        BoneVmdFixture f;auto source=f.Source({{0,BoneVmdTracks()}});std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanBoneKeyPaste(*f.app,0,false,source,change)==MMS_OK&&change,"standalone tail prepares before any index alias exists");
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        if(stage==0)Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare standalone tail candidate before protected-table pointer change");
        else if(stage==1)Check(f.history.PrepareNative("bone late alias",change,transaction)==MMS_OK,"reserve standalone tail journal before protected-table pointer change");
        else f.Commit(change);
        auto& target=*(reason>=2?f.second.m:f.first.m);const auto savedBone=target.boneKeyIndices,savedMorph=target.morphKeyIndices;
        if(reason%2)target.morphKeyIndices=f.firstCursors.data();else target.boneKeyIndices=f.firstCursors.data();
        const auto digest=f.first.Digest();const auto cursors=f.firstCursors;const auto active=f.firstActive;
        if(stage==0){failAfter=0;const bool current=candidate->IsCurrent();failAfter=-1;Check(!current,"short standalone token rejects a newly aliased own or peer index table without allocating");}
        else if(stage==1)Check(f.history.CommitNative(std::move(transaction))==MMS_E_STALE&&!f.history.UndoCount(),"journal commit refuses a late index alias without publishing or advancing history");
        else Check(change->Prepare(false,candidate)==MMS_E_STALE&&!candidate&&f.history.UndoCount()==1,"published standalone record rejects late index alias before preparing undo");
        Check(f.first.Digest()==digest&&f.firstCursors==cursors&&f.firstActive==active,"late-alias refusal preserves complete live native state and cursor payload");
        target.boneKeyIndices=savedBone;target.morphKeyIndices=savedMorph;
    }
}
void BoneVmdCurveAllocationFailures(){
    BoneVmdFixture f;const auto source=f.Source({{0,BoneVmdTracks()},{1,BoneVmdTracks(-4)}});const auto digest=f.first.Digest();
    auto author=bone_curve::AuthoringState::Capture(f.first.raw());const auto scale=bone_scale::AuthoringState::Capture(f.first.raw());unsigned failures=0;
    for(long allocation=0;allocation<4096;++allocation){std::shared_ptr<plugins::HistoryChange> change;std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;failAfter=allocation;
        auto status=plugins::PlanModelKeyPaste(*f.app,0,source,change);if(status==MMS_OK)status=plugins::Boundary([&]{return f.history.PrepareNative("bone VMD allocation",change,transaction);});failAfter=-1;
        Check(f.first.Digest()==digest&&bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*author)&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scale)&&!f.history.UndoCount(),"failed or successful private preparation cannot publish standard data or a partial first-bone author");
        if(status==MMS_OK){Check(transaction&&failures>30,"allocation sweep reaches complete two-bone standard+author transaction");Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"complete prepared two-bone import publishes with allocation disabled");return;}
        Check(status==MMS_E_MEMORY&&!transaction,"real allocation failure is reported as memory and never downgraded to optional format skip");++failures;
    }
    Check(false,"two-bone VMD allocation sweep terminates");
}
void NativeBoneVmdCurvesChecks(){
    BoneVmdCurveParity();BoneVmdCurveMapping();BoneVmdCurveSkipAndRollback();BoneVmdCurveGrowthAndCandidates();BoneVmdCurveLateAliasChecks();BoneVmdCurveAllocationFailures();
}
}
