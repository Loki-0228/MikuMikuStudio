#include "../src/plugins/native_morph_curve_write.hpp"
namespace {
struct MorphCurveWriteFixture:MorphKeyFixture {
    std::array<std::uint32_t,3> firstCursors{{7,6,2}},secondCursors{{7,6,2}};
    std::array<unsigned char,3> firstActive{{1,1,0}},secondActive{{1,1,0}};
    MorphCurveWriteFixture(){MorphKeySeed(first);MorphKeySeed(second);first.m->morphKeyCursors=firstCursors.data();second.m->morphKeyCursors=secondCursors.data();
        first.m->morphTrackActive=firstActive.data();second.m->morphTrackActive=secondActive.data();app->CurrentFrame()=7;app->LastRegisteredFrame()=9;}
    ~MorphCurveWriteFixture(){history.Reset();history.Collect();first.m->morphKeyCursors=second.m->morphKeyCursors=nullptr;first.m->morphTrackActive=second.m->morphTrackActive=nullptr;}
    std::uint64_t Digest(){std::uint64_t hash=first.Digest();const auto add=[&](const void* data,std::size_t bytes){const auto* p=static_cast<const unsigned char*>(data);while(bytes--){hash^=*p++;hash*=1099511628211ull;}};
        add(&app->state,sizeof app->state);add(firstMorphs.data(),sizeof firstMorphs);add(firstCursors.data(),sizeof firstCursors);add(firstActive.data(),sizeof firstActive);
        add(first.m->morphKeys,mdl::MorphKeyCapacity(first.m->morphKeys)*sizeof(mdl::MorphKey));return hash;}
    animation_curve::NamedTrack Input(unsigned morph=0){return MorphAuthoredTrack(firstMorphs[morph],int(morph));}
    animation_curve::NamedTrack Native(unsigned morph=0){animation_curve::NamedTrack named;std::string error;named.id=morph_curve::StableId(int(morph),firstMorphs[morph].name);
        Check(morph_curve::ReadNative(first.raw(),int(morph),named.track,error),"read native morph curve source without authored synchronization");return named;}
    animation_curve::NamedTrack Effective(unsigned morph=0){std::vector<animation_curve::Track> tracks;std::wstring error;
        Check(morph_curve::ReadManyDetached(*app,{int(morph)},tracks,error)&&tracks.size()==1,"read detached canonical morph curve");
        return {morph_curve::StableId(int(morph),firstMorphs[morph].name),std::move(tracks.front())};}
    std::shared_ptr<plugins::HistoryChange> Plan(const std::vector<animation_curve::NamedTrack>& tracks){std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanMorphCurveWrite(*app,0,tracks,change)==MMS_OK,"prepare detached morph authored replacement");return change;}
    void Commit(const std::shared_ptr<plugins::HistoryChange>& change){std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(history.PrepareNative("morph curves",change,transaction,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"reserve morph authored replacement in shared history");
        Check(CommitRegistrationNoAllocation(history,std::move(transaction))==MMS_OK,"publish morph pool layers local pose and cursors without allocations");}
};
void MorphCurveWriteParity(){
    for(bool multiple:{false,true})for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        MorphCurveWriteFixture f;Check(f.history.SetMode(mode)==MMS_OK,"morph curve explicit commit works in each recording mode");
        f.first.m->maxFrame=f.second.m->maxFrame=7;f.first.m->morphKeys[0].allocated=f.second.m->morphKeys[0].allocated=7;
        const auto unchanged=f.Native();const auto before=MorphKeysCopy(f.first);const auto source=f.Digest();
        Check(!f.Plan({unchanged})&&f.Digest()==source,"unchanged effective morph curve is a no-op without changing marks or history");
        const auto ring=PoseRingDigest(f.first);const auto bones=RegistrationKeys(f.first);const auto other=f.firstMorphs;
        std::vector<animation_curve::NamedTrack> input={f.Input()};if(multiple)input.push_back(f.Input(1));auto change=f.Plan(input);
        Check(change&&f.Digest()==source&&change->RetainedBytes()>before.size()*sizeof(mdl::MorphKey),"morph plan accounts private native and full authored payload without publishing");
        std::string error;Check(morph_curve::Write(f.second.raw(),input,error),"legacy pure morph Write supplies exact mirror oracle");f.Commit(change);
        Check(MorphKeysMatch(f.first,MorphKeysCopy(f.second))&&f.first.m->searchCursor==f.second.m->searchCursor&&f.first.m->maxFrame==f.second.m->maxFrame&&f.app->LastRegisteredFrame()==50,
            "first morph curve publication exactly matches legacy slots bytes export marks and scalar bookkeeping");
        for(unsigned morph=0;morph<3;++morph)Check(MorphSameFloat(f.firstMorphs[morph].value,morph<unsigned(input.size())?float(animation_curve::Evaluate(input[morph].track,7).value):other[morph].value),
            "first curve write samples only named morph weights and preserves unrelated temporary values");
        Check(SameRegistrationKeys(f.first,bones)&&PoseRingDigest(f.first)==ring,"morph curve write does not evaluate bone pose or write old native rings");
        std::vector<std::uint8_t> actualVmd,oracleVmd;Check(morph_curve::CaptureVmd(f.first.raw(),0,actualVmd,error)&&morph_curve::CaptureVmd(f.second.raw(),0,oracleVmd,error)&&actualVmd==oracleVmd,
            "MMSMCRV1 bytes and standard-section export selection remain unchanged");
        const auto after=MorphKeysCopy(f.first);f.app->CurrentFrame()=23;f.app->SelectedModelSlot()=1;const auto foreign=f.secondMorphs;
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&!morph_curve::AnyCurve(f.first.raw())&&f.app->CurrentFrame()==23,
            "undo restores exact original morph pool and author absence at the current playhead");
        Check(!std::memcmp(f.secondMorphs.data(),foreign.data(),sizeof foreign)&&f.firstMorphs[2].value==other[2].value,
            "morph curve undo preserves a different selected model and unowned morph weights");
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&f.history.UndoCount()==1,
            "morph curve redo restores the installed mirror and exact authoring in the common journal");
    }
}
void MorphCurveWriteRawLayers(){
    MorphCurveWriteFixture f;std::string error;auto seed=f.Input();Check(morph_curve::Write(f.first.raw(),{seed},error),"seed pristine morph authoring before a native edit");
    morph_curve::AuthoredLayers raw;Check(morph_curve::CaptureLayers(f.first.raw(),raw,error),"capture full raw pristine effective mirror and observed layers");
    unsigned slot=f.first.m->morphKeys[0].next;f.first.m->morphKeys[slot].value+=.4f;
    const auto source=f.Digest();const auto before=MorphKeysCopy(f.first);auto effective=f.Effective();
    Check(morph_curve::LayersUnchanged(f.first.raw(),raw)&&f.Digest()==source,"bound detached read derives a native edit without updating live author maps");
    const auto expected=float(animation_curve::Evaluate(effective.track,19).value);effective.track.keys.back().value+=.2;
    auto change=f.Plan({effective});std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK&&f.Digest()==source&&morph_curve::LayersUnchanged(f.first.raw(),raw),"private morph candidate retains the exact raw preexisting author layer");
    candidate.reset();Check(f.Digest()==source&&morph_curve::LayersUnchanged(f.first.raw(),raw),"cancelled morph replacement releases private maps without modifying raw cached layers");
    f.Commit(change);f.app->CurrentFrame()=19;
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&morph_curve::LayersUnchanged(f.first.raw(),raw)&&MorphSameFloat(f.firstMorphs[0].value,expected),
        "morph undo publishes original raw layers while sampling a private derived target at the current time");
    Check(f.history.History(true)==MMS_OK,"morph redo remains valid after restoring stale raw mirror and observed layers");
}
void MorphCurveWriteGrowthAndSelection(){
    {
        MorphCurveWriteFixture f;auto track=f.Input();track.track.keys.clear();for(int frame=0;frame<128;++frame){animation_curve::Key key;key.frame=frame;key.value=-2+.03*frame;key.interpolation=animation_curve::Interpolation::Linear;track.track.keys.push_back(key);}
        const auto before=MorphKeysCopy(f.first);const auto identity=mdl::MorphKeyPoolIdentity(f.first.m->morphKeys);std::string error;
        Check(morph_curve::Write(f.second.raw(),{track},error),"legacy morph mirror grows an independent oracle pool");f.Commit(f.Plan({track}));const auto after=MorphKeysCopy(f.first);
        Check(MorphKeysMatch(f.first,MorphKeysCopy(f.second))&&mdl::MorphKeyCapacity(f.first.m->morphKeys)==mdl::MorphKeyCapacity(f.second.m->morphKeys)&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==identity,
            "private morph growth matches legacy capacity and bytes while retaining stable pool identity");
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==identity&&f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after),
            "morph growth undo and redo resolve the new allocation by stable identity");
    }
    {
        MorphCurveWriteFixture f;f.Commit(f.Plan({f.Input(1),f.Input(0)}));
        const auto set=[&](unsigned morph,unsigned frame,unsigned char value){unsigned index=morph;for(;;){auto& key=f.first.m->morphKeys[index];if(key.frame==frame)key.allocated=value;if(!key.next)break;index=key.next;}};
        const auto mark=[&](unsigned morph,unsigned frame){unsigned index=morph;for(;;){const auto& key=f.first.m->morphKeys[index];if(key.frame==frame)return unsigned(key.allocated);if(!key.next)break;index=key.next;}return 0u;};
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)f.first.m->morphKeys[i].allocated=0;
        set(0,0,9);set(1,0,13);set(0,10,91);set(1,10,73);
        Check(f.history.History(false)==MMS_OK&&mark(0,0)==9&&mark(1,0)==13&&mark(0,10)==91&&mark(1,12)==0,
            "morph undo maps root and multi-selection by morph plus frame across cross-morph slot reuse");
        set(1,12,55);Check(f.history.History(true)==MMS_OK&&mark(0,10)==91&&mark(1,10)==0&&mark(1,25)==0,
            "morph redo never moves another morph's equal-frame selection into a restored slot");
        auto track=f.Effective();track.track.keys.back().value+=.2;auto change=f.Plan({track});std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare full-byte frozen morph publication token");f.first.m->morphKeys[0].allocated^=1;
        Check(!candidate->IsCurrent(),"prepared morph publication still refuses selection changes before commit");
    }
    {
        MorphCurveWriteFixture f;f.Commit(f.Plan({f.Input(0)}));f.first.m->morphKeys[1].allocated=23;
        Check(f.history.History(false)==MMS_OK&&!f.first.m->morphKeys[0].allocated&&f.first.m->morphKeys[1].allocated==23,
            "another morph's new selection does not change this record's exact selection restoration policy");
    }
}
void MorphCurveWriteMixedHistory(){
    MorphCurveWriteFixture f;
    // Morph zero is an editable PMX morph only in mode 2; legacy PMD reserves
    // it as the base morph and deliberately rejects native registration.
    f.first.m->physicsMode=f.second.m->physicsMode=2;
    f.Commit(f.Plan({f.Input()}));const auto first=MorphKeysCopy(f.first);f.firstMorphs[0].value=.42f;
    MorphRegistrationCommit(f,MorphRegistrationPlan(f,0,18));const auto registered=MorphKeysCopy(f.first);auto input=f.Effective();input.track.keys.back().value+=.25;f.Commit(f.Plan({input}));
    f.app->CurrentFrame()=19;morph_curve::Evaluate(f.first.raw(),19);
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,registered),"authored morph curve undo restores an intervening native registration");
    f.app->CurrentFrame()=21;morph_curve::Evaluate(f.first.raw(),21);Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,first),"native registration undo derives against the restored pristine author");
    f.app->CurrentFrame()=3;morph_curve::Evaluate(f.first.raw(),3);Check(f.history.History(false)==MMS_OK&&!morph_curve::AnyCurve(f.first.raw()),"earlier morph replacement remains valid after later derived-cache reads");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,"morph curve native registration curve redo preserves common history ordering");
    // An independent morph may acquire authoring after capture. Undoing this
    // record must merge current unowned maps rather than replacing the table.
    MorphCurveWriteFixture independent;auto a=independent.Plan({independent.Input(0)});independent.Commit(a);
    auto single=independent.Input(1);single.track.keys.resize(1);
    auto b=independent.Plan({single});std::unique_ptr<plugins::PreparedHistoryChange> privateB;Check(b->Prepare(true,privateB)==MMS_OK,"prepare independent author on another morph without reusing this record's slots");privateB->Publish();
    auto other=independent.Effective(1);Check(independent.history.History(false)==MMS_OK&&morph_curve::HasCurve(independent.first.raw(),1)&&!morph_curve::HasCurve(independent.first.raw(),0),
        "undo merges only owned author maps and preserves a later unowned morph's authoring");
    Check(animation_curve::Evaluate(independent.Effective(1).track,13).value==animation_curve::Evaluate(other.track,13).value,"unowned morph's exact authored sampling survives another curve undo");
}
void MorphCurveWriteRefusals(){
    {
        MorphCurveWriteFixture f;const auto input=f.Input();const auto before=f.Digest();auto change=f.Plan({input});
        Check(f.history.Configure(plugins::SharedHistory::RecordingMode::ExplicitOnly,8,1)==MMS_OK,"set tiny morph curve byte budget");std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("morph curves",change,transaction)==MMS_E_MEMORY&&!transaction&&f.Digest()==before,"morph byte-budget refusal leaves native pool author pose and journal intact");
    }
    for(unsigned reason=0;reason<5;++reason){MorphCurveWriteFixture f;const auto input=f.Input();std::shared_ptr<plugins::HistoryChange> change;
        if(reason==0)f.first.m->morphKeyCursors=reinterpret_cast<std::uint32_t*>(&f.firstMorphs[1].value);
        if(reason==1)f.first.m->morphTrackActive=reinterpret_cast<unsigned char*>(f.firstCursors.data());
        if(reason==2)f.second.m->morphs=f.first.m->morphs;
        if(reason==3)f.first.m->morphs=reinterpret_cast<mdl::MorphRecord*>(f.first.m->morphKeys);
        if(reason==4)f.first.m->boneTable=reinterpret_cast<mdl::BoneRecord*>(f.first.m->morphKeys);
        const auto before=f.Digest();const auto result=plugins::PlanMorphCurveWrite(*f.app,0,{input},change);const auto after=f.Digest();
        f.first.m->morphKeyCursors=f.firstCursors.data();f.first.m->morphTrackActive=f.firstActive.data();f.first.m->morphs=f.firstMorphs.data();f.second.m->morphs=f.secondMorphs.data();f.first.m->boneTable=f.first.bones.data();
        Check(result==MMS_E_NOT_READY&&!change&&before==after,"aliased morph pool pose tables cursors and other models are rejected without partial writes");
    }
    for(unsigned reason=0;reason<4;++reason){MorphCurveWriteFixture f;const auto input=f.Input();std::shared_ptr<plugins::HistoryChange> change;
        auto& model=*(reason%2?f.second.m:f.first.m);const auto oldChains=model.ikChains;const auto oldChainCount=model.ikChainCount;
        const auto oldOrder=model.boneOrderTable;const auto oldOrderCount=model.boneOrderCount;mdl::IkChain chain{};
        if(reason<2){chain.links=reinterpret_cast<std::uint16_t*>(reason%2?static_cast<void*>(&f.firstMorphs[1].value):static_cast<void*>(&f.first.m->morphKeys[0].value));
            chain.linkCount=1;model.ikChains=&chain;model.ikChainCount=1;}
        else{model.boneOrderTable=reinterpret_cast<mdl::BoneOrderEntry*>(reason%2?static_cast<void*>(f.firstCursors.data()):static_cast<void*>(&f.first.m->morphKeys[0].value));model.boneOrderCount=1;}
        const auto before=f.Digest();const auto result=plugins::PlanMorphCurveWrite(*f.app,0,{input},change);const auto after=f.Digest();
        model.ikChains=oldChains;model.ikChainCount=oldChainCount;model.boneOrderTable=oldOrder;model.boneOrderCount=oldOrderCount;
        Check(result==MMS_E_NOT_READY&&!change&&before==after,"own and peer IK links and bone evaluation tables cannot alias the morph pool weights or cursors");
    }
    for(unsigned reason=0;reason<3;++reason){MorphCurveWriteFixture f;auto input=f.Input();
        if(reason==0)input.track.keys[0].outSlope=1.e39;
        if(reason==1){input.track.keys[0].value=-3.e38;input.track.keys[1].value=3.e38;}
        if(reason==2){input.track.keys[0].value=1.e100;}
        f.app->CurrentFrame()=0;const auto before=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanMorphCurveWrite(*f.app,0,{input},change)==MMS_E_ARGUMENT&&!change&&f.Digest()==before,
            "morph controls future native delta and float storage overflow refuse even when the current frame is finite");
    }
    {
        MorphCurveWriteFixture f;auto input=f.Input();input.track.keys[0].value=-2;input.track.keys[1].value=3;
        f.Commit(f.Plan({input}));Check(f.history.UndoCount()==1,"safe negative and above-one morph weights remain supported");
    }
}
void MorphCurveWriteLifetimeAndStale(){
    {
        MorphCurveWriteFixture f;const auto before=MorphKeysCopy(f.first);f.Commit(f.Plan({f.Input()}));auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
        const auto identity=plugins::SceneObjectIdentity(f.first.raw());std::unique_ptr<plugins::SharedHistory::NativeTransaction> removal;
        Check(f.history.PrepareNative("remove model",owner->CaptureRemoval(4096),removal)==MMS_OK&&f.history.CommitNative(std::move(removal))==MMS_OK&&!f.app->ModelSlot(0),
            "remove a retained model after its morph curve edit");
        Check(f.history.History(false)==MMS_OK&&plugins::SceneObjectIdentity(f.first.raw()).generation!=identity.generation&&
            f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before),"morph curve history follows the restored stable model ID with a new generation");
        Check(f.history.History(true)==MMS_OK,"morph curve redo resolves its restored model and stable key pool");
    }
    for(unsigned reason=0;reason<3;++reason){MorphCurveWriteFixture f;auto change=f.Plan({f.Input()});
        if(reason==0)f.first.m->morphKeys[15].reserved[1]^=1;
        if(reason==1)f.firstMorphs[0].name[0]^=1;
        if(reason==2)f.app->CurrentFrame()=8;
        const auto before=f.Digest();std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&f.Digest()==before,
            "first morph source freezes unused pool bytes table identity and playhead before any publication");
    }
}
void MorphCurveWriteFailures(){
    unsigned failed=0;
    for(long allocation=0;allocation<2048;++allocation){MorphCurveWriteFixture f;const auto input=f.Input();std::string error;
        auto seed=f.Input(1);Check(morph_curve::Write(f.first.raw(),{seed},error),"seed unowned author data for morph failure sweep");morph_curve::AuthoredLayers raw;
        Check(morph_curve::CaptureLayers(f.first.raw(),raw,error),"capture complete layers before allocation injection");const auto before=f.Digest();
        std::shared_ptr<plugins::HistoryChange> change;std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;const std::vector<animation_curve::NamedTrack> payload{input};
        failAfter=allocation;auto result=plugins::PlanMorphCurveWrite(*f.app,0,payload,change);
        if(result==MMS_OK)result=plugins::Boundary([&]{return f.history.PrepareNative("morph curves",change,transaction);});failAfter=-1;
        Check(f.Digest()==before&&morph_curve::LayersUnchanged(f.first.raw(),raw)&&f.history.UndoCount()==0,"every morph allocation failure leaves full raw layers native pool current weights and history unchanged");
        if(result==MMS_OK){Check(transaction&&failed>20,"morph failure injection reaches a fully prepared author replacement");return;}
        Check(result==MMS_E_MEMORY&&!transaction,"morph preparation reports allocation failure without a publication token");++failed;
    }Check(false,"morph curve failure sweep terminates");
}
void NativeMorphCurveWriteChecks(){
    std::puts("  morph curve write: legacy mirror export and common history parity");MorphCurveWriteParity();
    std::puts("  morph curve write: raw layer restoration and private effective sampling");MorphCurveWriteRawLayers();
    std::puts("  morph curve write: pool growth root and multi-morph selection");MorphCurveWriteGrowthAndSelection();
    std::puts("  morph curve write: mixed native history and unowned author preservation");MorphCurveWriteMixedHistory();
    std::puts("  morph curve write: budgets aliasing finite evaluation and lifetime");MorphCurveWriteRefusals();MorphCurveWriteLifetimeAndStale();
    std::puts("  morph curve write: full private preparation allocation sweep");MorphCurveWriteFailures();
}
}
