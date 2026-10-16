// Include after native_bone_registration_checks.inc. Call NativeMorphKeyChecks().
// The actual legacy deletion and seek functions supply independent oracles.
#include "../src/plugins/native_morph_keys.hpp"
#include <limits>
namespace {
struct MorphKeyFixture : BoneRegistrationFixture {
    std::array<mdl::MorphRecord,3> firstMorphs{},secondMorphs{};
    MorphKeyFixture() {
        first.m->morphs=firstMorphs.data();second.m->morphs=secondMorphs.data();
        first.m->morphCount=second.m->morphCount=3;
        for(unsigned index=0;index<3;++index)for(auto* records:{&firstMorphs,&secondMorphs}) {
            std::snprintf((*records)[index].name,20,"morph%u",index);
            (*records)[index].panel=mdl::MorphPanel::eye;(*records)[index].type=1;
        }
        for(auto* fixture:{&first,&second}) {
            fixture->m->ikChainCount=fixture->m->boneOrderCount=0;
            for(auto& bone:fixture->bones)bone.slotIndex=-1;
        }
    }
    ~MorphKeyFixture() {
        history.Reset();history.Collect();morph_curve::Forget(first.raw());morph_curve::Forget(second.raw());
        first.m->morphs=first.morphs.data();first.m->morphCount=1;
        second.m->morphs=second.morphs.data();second.m->morphCount=1;
    }
};
void MorphKeySeed(Fixture& f) {
    std::memset(f.m->morphKeys,0,mdl::MorphKeyCapacity(f.m->morphKeys)*sizeof(mdl::MorphKey));
    for(unsigned morph=0;morph<3;++morph) {
        f.m->morphKeys[morph].value=.11f*float(morph+1);
        f.m->morphs[morph].value=float(morph+9);
    }
    const unsigned slots[2][3]={{7,4,8},{6,3,9}};
    const unsigned frames[2][3]={{10,25,50},{12,30,60}};
    const float values[2][3]={{.7f,-.2f,1.3f},{.77f,.39f,.94f}};
    for(unsigned morph=0;morph<2;++morph) {
        unsigned previous=morph;
        for(unsigned i=0;i<3;++i) {
            const auto slot=slots[morph][i];auto& key=f.m->morphKeys[slot];
            f.m->morphKeys[previous].next=slot;key.previous=previous;previous=slot;
            key.frame=frames[morph][i];key.value=values[morph][i];key.reserved[1]=static_cast<unsigned char>(slot+23);
        }
    }
}
std::vector<mdl::MorphKey> MorphKeysCopy(const Fixture& f) {
    return {f.m->morphKeys,f.m->morphKeys+mdl::MorphKeyCapacity(f.m->morphKeys)};
}
bool MorphKeysMatch(const Fixture& f,const std::vector<mdl::MorphKey>& keys) {
    return mdl::MorphKeyCapacity(f.m->morphKeys)>=keys.size()&&
        !std::memcmp(f.m->morphKeys,keys.data(),keys.size()*sizeof(mdl::MorphKey));
}
bool MorphSameFloat(float a,float b){return !std::memcmp(&a,&b,sizeof(a));}
std::shared_ptr<plugins::HistoryChange> MorphDeletePlan(MorphKeyFixture& f,unsigned slot=0,bool mixed=false) {
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanMorphKeyDeletion(*f.app,slot,change,mixed)==MMS_OK&&change,"morph deletion produces an owned record");return change;
}
void MorphDeleteCommit(MorphKeyFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("morph deletion",change,transaction)==MMS_OK,"morph journal prepares complete keys and local weights");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"morph keys and sampled weights publish without allocation");
}
void MorphDeletionParity() {
    const std::vector<std::vector<unsigned>> cases={{0},{1},{2},{7},{4},{8},{9},{4,7},{0,1,3,4,6,7,8,9}};
    for(const auto& marks:cases) {
        MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);f.app->CurrentFrame()=19;
        std::array<bool,3> affected{};
        for(auto slot:marks) {
            f.first.m->morphKeys[slot].allocated=f.second.m->morphKeys[slot].allocated=3;
            affected[slot==2?2:slot==1||slot==3||slot==6||slot==9?1:0]=true;
        }
        const auto before=MorphKeysCopy(f.first);const auto digest=f.first.Digest(),ring=PoseRingDigest(f.first);
        const auto morphs=f.firstMorphs;const auto boneKeys=RegistrationKeys(f.first);
        auto change=MorphDeletePlan(f);
        Check(f.first.Digest()==digest&&!std::memcmp(f.firstMorphs.data(),morphs.data(),sizeof(morphs))&&
            change->RetainedBytes()>=2*sizeof(mdl::MorphKey),"morph planning retains private payload without changing native scene or weight");
        MorphDeleteCommit(f,change);DeleteMarkedModelKeys(f.second.raw(),19);SeekModelFrame(f.second.raw(),19,0);
        Check(!std::memcmp(f.first.m->morphKeys,f.second.m->morphKeys,16*sizeof(mdl::MorphKey)),
            "morph root middle tail adjacent and non-time slot deletion exactly match legacy keys including sentinels and reserved bytes");
        for(unsigned morph=0;morph<3;++morph)Check(MorphSameFloat(f.firstMorphs[morph].value,
            affected[morph]?f.secondMorphs[morph].value:morphs[morph].value),
            "only affected morph weights are reevaluated with native seek float rounding");
        Check(PoseRingDigest(f.first)==ring&&SameRegistrationKeys(f.first,boneKeys)&&f.first.m->maxFrame==99&&
            f.first.m->searchCursor==5&&f.SelectionUnchanged(),"morph publication leaves bone keys old history metadata and selection untouched");
        const auto after=MorphKeysCopy(f.first);
        f.app->CurrentFrame()=37;f.app->SelectedModelSlot()=1;
        std::memcpy(f.second.m->morphKeys,before.data(),before.size()*sizeof(mdl::MorphKey));SeekModelFrame(f.second.raw(),37,0);
        const auto other=f.secondMorphs;
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&
            !std::memcmp(f.secondMorphs.data(),other.data(),sizeof(other)),"morph undo restores marked native keys without touching the selected other model");
        for(unsigned morph=0;morph<3;++morph)if(affected[morph])Check(MorphSameFloat(f.firstMorphs[morph].value,f.secondMorphs[morph].value),
            "morph undo reevaluates at the current playhead instead of restoring the commit-frame weight");
        f.app->CurrentFrame()=5;std::memcpy(f.second.m->morphKeys,after.data(),after.size()*sizeof(mdl::MorphKey));SeekModelFrame(f.second.raw(),5,0);
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after),"morph redo restores sparse target native keys");
        for(unsigned morph=0;morph<3;++morph)if(affected[morph])Check(MorphSameFloat(f.firstMorphs[morph].value,f.secondMorphs[morph].value),
            "morph redo samples the new playhead and preserves unrelated current weights");
    }
}
animation_curve::NamedTrack MorphAuthoredTrack(const mdl::MorphRecord& morph,int index) {
    animation_curve::NamedTrack result;result.id=morph_curve::StableId(index,morph.name);
    for(int frame:{0,10,25,50}) {
        animation_curve::Key key;key.frame=frame;key.value=frame==0?.1:frame==10?.8:frame==25?.25:.7;
        key.interpolation=animation_curve::Interpolation::Bezier;key.tangent=animation_curve::Tangent::Free;
        key.inSlope=.03;key.outSlope=-.02;key.inWeight=.2;key.outWeight=.4;result.track.keys.push_back(key);
    }
    return result;
}
void MorphDeletionAuthoring() {
    MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);std::string error;
    const auto track=MorphAuthoredTrack(f.firstMorphs[0],0);
    Check(morph_curve::Write(f.first.raw(),{track},error)&&morph_curve::Write(f.second.raw(),{track},error),"pristine weighted morph authoring seeded in both independent models");
    for(auto* fixture:{&f.first,&f.second}) {
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(fixture->m->morphKeys);++i)fixture->m->morphKeys[i].allocated=0;
        fixture->m->morphKeys[fixture->m->morphKeys[0].next].allocated=1;
    }
    f.app->CurrentFrame()=17;const auto before=MorphKeysCopy(f.first);
    morph_curve::AuthoredLayers pristine;Check(morph_curve::CaptureLayers(f.first.raw(),pristine,error),"capture pristine morph layers without synchronization");
    auto change=MorphDeletePlan(f);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent()&&morph_curve::LayersUnchanged(f.first.raw(),pristine),
        "target morph derivation prepares without changing live authored effective mirror or observed layers");
    auto changedTrack=track;changedTrack.track.keys[1].inSlope=.12;
    morph_curve::AuthoredLayers replacement;
    Check(morph_curve::RebuildLayers(f.first.raw(),{changedTrack},replacement,error),"prepare a tangent-only authoring change with identical native keys");
    morph_curve::SwapLayers(f.first.raw(),replacement);
    Check(!candidate->IsCurrent()&&MorphKeysMatch(f.first,before),"tangent-only authoring changes invalidate morph candidate without any key mutation");
    morph_curve::SwapLayers(f.first.raw(),replacement);Check(candidate->IsCurrent(),"restoring exact authored layers restores candidate freshness");
    bool prepared=false;unsigned failures=0;
    for(long limit=0;limit<512;++limit) {
        std::unique_ptr<plugins::PreparedHistoryChange> attempt;
        failAfter=limit;const auto status=change->Prepare(true,attempt);failAfter=-1;
        Check(MorphKeysMatch(f.first,before)&&morph_curve::LayersUnchanged(f.first.raw(),pristine),
            "authored derivation allocation failures preserve live native keys and all author layers");
        if(status==MMS_OK){prepared=true;break;}
        Check(status==MMS_E_MEMORY&&!attempt,"authored morph candidate allocation failure reports memory");++failures;
    }
    Check(prepared&&failures>15,"authored morph failure sweep reaches copied tracks mirrors and private effective derivation");
    failAfter=0;candidate->Publish();candidate.reset();failAfter=-1;
    DeleteMarkedModelKeys(f.second.raw(),17);SeekModelFrame(f.second.raw(),17,0);
    Check(MorphSameFloat(f.firstMorphs[0].value,f.secondMorphs[0].value)&&morph_curve::LayersUnchanged(f.first.raw(),pristine),
        "native deletion derives the affected authored curve without erasing or rewriting pristine layers");
    animation_curve::Track bent;
    Check(morph_curve::Read(f.first.raw(),0,bent,error)&&bent.keys.size()==3,"ordinary reader derives the same deleted authored frame after publication");
    f.app->CurrentFrame()=6;Check(change->Prepare(false,candidate)==MMS_OK&&candidate->IsCurrent(),"morph inverse prepares after live derived cache synchronization");
    failAfter=0;candidate->Publish();candidate.reset();failAfter=-1;
    Check(MorphKeysMatch(f.first,before)&&MorphSameFloat(f.firstMorphs[0].value,float(animation_curve::Evaluate(track.track,6).value)),
        "restoring native mirror revives exact weighted authoring at the current frame");
    animation_curve::Track restored;Check(morph_curve::Read(f.first.raw(),0,restored,error)&&restored.keys.size()==4&&
        restored.keys[1].inWeight==.2&&restored.keys[0].outSlope==-.02,"undo restores pristine custom tangent and handle values automatically");
    // A private target view must not borrow model.morphKeys or Sync the cache.
    morph_curve::AuthoredLayers layers;Check(morph_curve::CaptureLayers(f.first.raw(),layers,error),"capture target-view sample layers");
    auto privateKeys=before;const auto removed=privateKeys[0].next,next=privateKeys[removed].next;
    privateKeys[0].next=next;privateKeys[next].previous=0;
    morph_curve::NativeKeyView view;view.context=&privateKeys;view.capacity=privateKeys.size();
    view.key=[](const void* data,std::uint32_t slot) noexcept -> const mdl::MorphKey* {
        const auto& keys=*static_cast<const std::vector<mdl::MorphKey>*>(data);return slot<keys.size()?&keys[slot]:nullptr;
    };
    const auto* pool=f.first.m->morphKeys;float value=-99;
    Check(morph_curve::SampleLayersTarget(layers,view,0,17,value,error)&&MorphSameFloat(value,f.secondMorphs[0].value)&&
        f.first.m->morphKeys==pool&&MorphKeysMatch(f.first,before)&&morph_curve::LayersUnchanged(f.first.raw(),layers),
        "private target sampling matches legacy-derived evaluation and leaves live pointers keys and layers untouched");
    privateKeys[next].next=next;value=-99;
    Check(!morph_curve::SampleLayersTarget(layers,view,0,17,value,error)&&value==-99,"malformed private morph target does not publish a partial sampled value");
}
void MorphDeletionLifetimeAndGrowth() {
    MorphKeyFixture f;MorphKeySeed(f.first);MorphKeySeed(f.second);
    f.first.m->morphKeys[7].allocated=f.second.m->morphKeys[6].allocated=1;
    const auto firstBefore=MorphKeysCopy(f.first),secondBefore=MorphKeysCopy(f.second);
    auto firstChange=MorphDeletePlan(f);MorphDeleteCommit(f,firstChange);MorphDeleteCommit(f,MorphDeletePlan(f,1));
    const auto firstAfter=MorphKeysCopy(f.first),secondAfter=MorphKeysCopy(f.second);
    for(unsigned n=0;n<100;++n)PushBoneEditUndo(f.app.get());const auto ring=PoseRingDigest(f.first);
    const auto identity=mdl::MorphKeyPoolIdentity(f.first.m->morphKeys);const auto* old=f.first.m->morphKeys;
    Check(mdl::MorphKeyPoolEnsure(*f.first.m,17)&&f.first.m->morphKeys!=old&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==identity,
        "morph pool growth retains its registered identity");
    f.first.m->morphKeys[16].value=83;f.first.m->morphKeys[16].reserved[2]=91;
    f.app->CurrentFrame()=22;
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.second,secondBefore)&&MorphKeysMatch(f.first,firstAfter),"common morph undo resolves the last edited model first");
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,firstBefore)&&PoseRingDigest(f.first)==ring&&
        f.first.m->morphKeys[16].value==83,"morph history survives pool relocation and one hundred overwritten native ring slots");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,firstAfter)&&
        MorphKeysMatch(f.second,secondAfter)&&f.first.m->morphKeys[16].reserved[2]==91,"sparse morph redo preserves newly allocated pool tail contents");
    f.history.Reset();f.history.Collect();std::unique_ptr<plugins::PreparedHistoryChange> inverse;
    Check(firstChange->Prepare(false,inverse)==MMS_OK&&inverse->IsCurrent(),"independent morph record prepares without a journal node");
    firstChange.reset();failAfter=0;inverse->Publish();inverse.reset();failAfter=-1;
    Check(MorphKeysMatch(f.first,firstBefore),"morph candidate keeps both directions alive after the caller releases the record");
}
void MorphDeletionGuards() {
    MorphKeyFixture f;MorphKeySeed(f.first);f.first.m->morphKeys[7].allocated=1;
    auto change=MorphDeletePlan(f);MorphDeleteCommit(f,change);Check(f.history.History(false)==MMS_OK,"seed morph redo for guard tests");
    for(std::size_t i=0;i<16;++i)f.first.m->morphKeys[i].allocated=0;
    const auto redo=f.history.RedoCount();const auto digest=f.first.Digest();std::shared_ptr<plugins::HistoryChange> rejected;
    Check(plugins::PlanMorphKeyDeletion(*f.app,0,rejected)==MMS_OK&&!rejected&&f.history.RedoCount()==redo&&f.first.Digest()==digest,
        "empty morph deletion preserves redo and every scene byte");
    f.first.m->morphKeys[7].allocated=1;auto pending=MorphDeletePlan(f);const auto pendingKeys=MorphKeysCopy(f.first);
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;std::string label="morph reservation failure";
    failAfter=0;const auto failed=f.history.PrepareNative(std::move(label),pending,transaction);failAfter=-1;
    Check(failed==MMS_E_MEMORY&&!transaction&&f.history.RedoCount()==redo&&MorphKeysMatch(f.first,pendingKeys),
        "failed morph journal reservation leaves existing redo and keys intact");
    Check(f.history.PrepareNative("stale morph",pending,transaction)==MMS_OK,"prepare a morph operation while redo exists");
    const auto weight=f.firstMorphs[0].value;f.firstMorphs[0].value+=1;
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_E_STALE&&f.history.RedoCount()==redo&&
        MorphKeysMatch(f.first,pendingKeys)&&f.firstMorphs[0].value==weight+1,
        "stale morph commit preserves both redo and the intervening unregistered weight");
    f.firstMorphs[0].value=weight;f.first.m->boneKeys[0].allocated=4;f.first.m->displayKeys[0].allocated=5;
    Check(plugins::PlanMorphKeyDeletion(*f.app,0,rejected)==MMS_E_NOT_READY&&!rejected&&f.history.RedoCount()==redo,"standalone morph deletion rejects other-family marks");
    auto mixed=MorphDeletePlan(f,0,true);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(mixed->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"aggregate morph participant permits independent other-family work");
    f.first.m->boneKeys[0].allocated=0;f.first.m->displayKeys[0].allocated=0;
    Check(candidate->IsCurrent(),"other participants may publish bone and display marks before morph publication");
    failAfter=0;candidate->Publish();failAfter=-1;
    Check(f.first.m->boneKeys[0].allocated==0&&f.first.m->displayKeys[0].allocated==0,"morph participant never restores other-family marks");
    f.history.Reset();f.history.Collect();MorphKeySeed(f.first);f.first.m->morphKeys[7].allocated=1;
    const auto original=MorphKeysCopy(f.first);
    for(int kind=0;kind<6;++kind) {
        std::memcpy(f.first.m->morphKeys,original.data(),original.size()*sizeof(mdl::MorphKey));
        if(kind==0)f.first.m->morphKeys[7].next=99;
        if(kind==1)f.first.m->morphKeys[7].next=7;
        if(kind==2)f.first.m->morphKeys[4].frame=5;
        if(kind==3)f.first.m->morphKeys[1].next=7;
        if(kind==4){f.first.m->morphKeys[10].frame=40;f.first.m->morphKeys[10].allocated=1;}
        if(kind==5)f.first.m->morphKeys[7].value=(std::numeric_limits<float>::quiet_NaN)();
        const auto malformed=MorphKeysCopy(f.first);
        Check(plugins::PlanMorphKeyDeletion(*f.app,0,rejected)==MMS_E_ARGUMENT&&!rejected&&MorphKeysMatch(f.first,malformed),
            "invalid out-of-bounds cycle reversed shared orphan and nonfinite morph tracks reject atomically");
    }
}
void MorphDeletionStaleAndFailures() {
    MorphKeyFixture f;MorphKeySeed(f.first);f.first.m->morphKeys[7].allocated=1;auto change=MorphDeletePlan(f);
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"morph stale fixture prepares");
    ++f.app->CurrentFrame();Check(!candidate->IsCurrent(),"morph current-frame dependency rejects a moved playhead");--f.app->CurrentFrame();
    f.firstMorphs[0].value+=1;Check(!candidate->IsCurrent(),"changed affected unregistered weight invalidates prepared evaluation");f.firstMorphs[0].value-=1;
    f.firstMorphs[2].value=72;Check(candidate->IsCurrent(),"unrelated morph current values are outside the write scope");
    const auto neighbour=f.first.m->morphKeys[8].value;f.first.m->morphKeys[8].value+=1;
    Check(!candidate->IsCurrent(),"unchanged source interval neighbour remains a sampling dependency");f.first.m->morphKeys[8].value=neighbour;
    f.firstMorphs[0].name[0]='X';Check(!candidate->IsCurrent(),"morph identity name change rejects prepared history");f.firstMorphs[0].name[0]='m';
    const auto keys=MorphKeysCopy(f.first);const auto native=f.first.Digest();const auto morphs=f.firstMorphs;
    bool planned=false;unsigned planFailures=0;
    for(long limit=0;limit<256;++limit) {
        std::shared_ptr<plugins::HistoryChange> attempt;failAfter=limit;const auto status=plugins::PlanMorphKeyDeletion(*f.app,0,attempt);failAfter=-1;
        Check(f.first.Digest()==native&&MorphKeysMatch(f.first,keys)&&!std::memcmp(f.firstMorphs.data(),morphs.data(),sizeof(morphs)),"morph planner allocation failure preserves all native bytes and weights");
        if(status==MMS_OK){planned=true;break;}Check(status==MMS_E_MEMORY&&!attempt,"morph plan allocation failure reports memory");++planFailures;
    }
    Check(planned&&planFailures>5,"morph planning allocation sweep reaches ownership and sparse patch storage");
    bool prepared=false;unsigned prepareFailures=0;
    for(long limit=0;limit<256;++limit) {
        std::unique_ptr<plugins::PreparedHistoryChange> attempt;failAfter=limit;const auto status=change->Prepare(true,attempt);failAfter=-1;
        Check(f.first.Digest()==native&&MorphKeysMatch(f.first,keys)&&!std::memcmp(f.firstMorphs.data(),morphs.data(),sizeof(morphs)),"morph candidate allocation failure preserves keys and local weights");
        if(status==MMS_OK){prepared=true;failAfter=0;attempt->Publish();attempt.reset();failAfter=-1;break;}
        Check(status==MMS_E_MEMORY&&!attempt,"morph candidate allocation failure reports memory");++prepareFailures;
    }
    Check(prepared&&prepareFailures>5&&f.firstMorphs[2].value==72,"morph prepare allocation sweep ends in no-allocation publication preserving unrelated weight");
    Check(change->Prepare(false,candidate)==MMS_OK,"morph inverse prepares before relocation");
    Check(mdl::MorphKeyPoolEnsure(*f.first.m,17)&&!candidate->IsCurrent(),"pool relocation invalidates a pending morph candidate");
    Check(change->Prepare(false,candidate)==MMS_OK,"owned record resolves the new allocation with the same pool identity");
    failAfter=0;candidate->Publish();candidate.reset();failAfter=-1;
    mdl::MorphKeyPoolDestroy(f.first.m->morphKeys);f.first.m->morphKeys=mdl::MorphKeyPoolCreate(16);MorphKeySeed(f.first);
    Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate,"different pool ownership cannot resurrect a morph history record at a recycled address");
}
}
void NativeMorphKeyChecks() {
    std::puts("  morph deletion: legacy parity");MorphDeletionParity();
    std::puts("  morph deletion: pristine curves and target sampling");MorphDeletionAuthoring();
    std::puts("  morph deletion: lifetime and pool growth");MorphDeletionLifetimeAndGrowth();
    std::puts("  morph deletion: no-op mixed marks and malformed tracks");MorphDeletionGuards();
    std::puts("  morph deletion: stale candidates and allocation failures");MorphDeletionStaleAndFailures();
}
