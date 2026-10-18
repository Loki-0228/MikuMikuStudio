// Include after native_morph_curve_write_checks.inc and model paste helpers.
namespace {
std::vector<std::uint8_t> MorphVmdCurveBlock(const std::vector<animation_curve::NamedTrack>& tracks){
    std::vector<std::uint8_t> payload,block;std::string error;
    Check(morph_curve::EncodePayload(tracks,payload,error)&&morph_curve::EncodeVmdBlock(payload,block,error),"encode unchanged MMSMCRV1 payload and envelope");return block;
}
struct MorphVmdCurveFixture:MorphCurveWriteFixture{
    MorphVmdCurveFixture(){first.m->physicsMode=second.m->physicsMode=2;}
    plugins::ModelPasteSource Source(const std::vector<animation_curve::NamedTrack>& tracks,unsigned offset=0){
        plugins::ModelPasteSource source;source.frameOffset=offset;source.advanceLastRegisteredFrame=true;
        for(const auto& named:tracks){int morph=-1;std::string name;Check(morph_curve::ParseStableId(named.id,morph,name)&&morph>=0&&morph<3,"resolve test morph VMD standard identity");
            for(const auto& key:named.track.keys){plugins::ModelMorphPasteRecord record;std::memcpy(record.key.name,firstMorphs[unsigned(morph)].name,20);
                record.key.frame=unsigned(key.frame);record.key.value=float(key.value);source.morphs.push_back(record);}}
        source.morphCurveBlock=MorphVmdCurveBlock(tracks);return source;
    }
    void Follow(unsigned morph){morph_curve::AuthoredLayers layers;std::string error;Check(morph_curve::CaptureLayers(first.raw(),layers,error),"capture old morph sampling layers without synchronization");
        const morph_curve::NativeKeyView view{first.m->morphKeys,[](const void* p,std::uint32_t i)noexcept{return static_cast<const mdl::MorphKey*>(p)+i;},mdl::MorphKeyCapacity(first.m->morphKeys)};
        float value=0;Check(morph_curve::SampleLayersTarget(layers,view,int(morph),app->CurrentFrame(),value,error),"sample a following weight without seeking the model");firstMorphs[morph].value=value;
    }
    std::shared_ptr<plugins::HistoryChange> PlanImport(const plugins::ModelPasteSource& source){std::shared_ptr<plugins::HistoryChange> out;
        Check(plugins::PlanModelKeyPaste(*app,0,source,out)==MMS_OK,"prepare optional morph curves with the same private standard pool");return out;}
    std::size_t Oracle(const plugins::ModelPasteSource& source){MorphRegistrationClearMarks(second);ResetMorphKeyCursor(second.raw());
        for(const auto& record:source.morphs)Check(RegisterMorphKeyFromRecord(second.raw(),reinterpret_cast<const unsigned char*>(&record.key),int(source.frameOffset)),"legacy standard morph registrar supplies independent import oracle");
        ResetDisplayKeyCursor(second.raw());return morph_curve::ApplyVmd(second.raw(),source.morphCurveBlock.data(),source.morphCurveBlock.size(),int(source.frameOffset));
    }
    void SameEffective(unsigned morph){animation_curve::Track actual,expected;std::string error;Check(morph_curve::Read(first.raw(),int(morph),actual,error)&&morph_curve::Read(second.raw(),int(morph),expected,error),"read new and legacy effective morph curves");
        std::vector<std::uint8_t> a,b;Check(animation_curve::Encode({{"same",actual}},a,error)&&animation_curve::Encode({{"same",expected}},b,error)&&a==b,"import preserves every key tangent handle and interpolation field of legacy ApplyVmd");
        for(int step=0;step<=560;++step){const double frame=double(step)*.25;Check(animation_curve::Evaluate(actual,frame).value==animation_curve::Evaluate(expected,frame).value,"dense subframe samples match legacy imported authored ease");}
    }
};
void MorphVmdFiniteCurveOutsideFloatHullChecks(){
    MorphVmdCurveFixture f;auto imported=f.Input(1);imported.track.keys.clear();
    for(int frame:{0,10,20}){animation_curve::Key key;key.frame=frame;key.value=frame==0?0:frame==10?.25:.75;
        key.tangent=animation_curve::Tangent::Free;key.inWeight=key.outWeight=1.0/3.0;
        key.interpolation=animation_curve::Interpolation::Linear;imported.track.keys.push_back(key);}
    imported.track.keys[1].outSlope=1.8e38;imported.track.keys[2].inSlope=1.8e38;
    imported.track.keys[2].interpolation=animation_curve::Interpolation::Bezier;
    auto baseline=imported;baseline.track.keys[1].outSlope=baseline.track.keys[2].inSlope=0;
    baseline.track.keys[2].interpolation=animation_curve::Interpolation::Linear;std::string error;
    Check(morph_curve::Write(f.first.raw(),{baseline},error)&&morph_curve::Write(f.second.raw(),{imported},error),
        "old morph Write accepts a finite double author whose control hull exceeds float range");
    morph_curve::Forget(f.first.raw());
    auto source=f.Source({imported});source.morphCurveBlock.clear();
    Check(morph_curve::CaptureVmd(f.second.raw(),0,source.morphCurveBlock,error)&&!source.morphCurveBlock.empty(),
        "real legacy VMD export preserves the interior free handle beyond float control range");
    std::vector<std::uint8_t> payload;std::vector<animation_curve::NamedTrack> exported;
    Check(morph_curve::DecodeVmdBlock(source.morphCurveBlock.data(),source.morphCurveBlock.size(),payload)&&
        morph_curve::DecodePayload(payload.data(),payload.size(),exported,error)&&exported.size()==1&&
        exported[0].track.keys.size()==3&&exported[0].track.keys[1].outSlope==1.8e38&&exported[0].track.keys[2].inSlope==1.8e38,
        "unmodified VMD codec round-trips the original double slopes exactly");
    const auto& track=exported[0].track;const auto& a=track.keys[1];const auto& b=track.keys[2];
    const double upper=a.value+a.outSlope*a.outWeight*10,lower=b.value-b.inSlope*b.inWeight*10;
    Check(!std::isfinite(float(upper))&&!std::isfinite(float(lower))&&
        std::isfinite(float(b.value)-float(a.value)),"counterexample has an overflowing control hull and safe standard native values and delta");
    // With x1=1/3,x2=2/3, time is linear. Ignoring the sub-unit endpoint
    // contribution, y=3*A*t*(1-t)*(1-2*t), A=6e38. Its extrema are
    // +/-sqrt(3)*A/6 = +/-1.732...e38, within native float range.
    for(double t:{(3-std::sqrt(3.0))/6,(3+std::sqrt(3.0))/6}){const double sample=animation_curve::Evaluate(track,10+10*t).value;
        Check(std::isfinite(sample)&&std::isfinite(float(sample))&&std::abs(sample)>1e38,
            "both analytic Bezier extrema remain finite native weights despite overflowing float controls");}
    for(unsigned sample=0;sample<=2000;++sample)Check(std::isfinite(float(animation_curve::Evaluate(track,double(sample)/100).value)),
        "all dense samples of the exported double curve remain representable as native floats");
    f.app->CurrentFrame()=12;f.Follow(1);const auto before=MorphKeysCopy(f.first);const auto digest=f.Digest();
    auto change=f.PlanImport(source);Check(change&&f.Digest()==digest,"legacy-compatible author-only VMD prepares without changing native data");
    Check(f.Oracle(source)==1,"actual old ApplyVmd accepts the old writer's finite-curve VMD block");f.Commit(change);f.SameEffective(1);
    Check(morph_curve::HasCurve(f.first.raw(),1)&&MorphKeysMatch(f.first,MorphKeysCopy(f.second)),
        "controlled import installs the original author instead of silently leaving linear standard motion");
    const auto effective=f.Effective(1);Check(MorphSameFloat(f.firstMorphs[1].value,float(animation_curve::Evaluate(effective.track,12).value))&&
        std::abs(f.firstMorphs[1].value)>1e38,"controlled import updates a following live weight with the finite double sample");
    const auto after=MorphKeysCopy(f.first);source={};f.app->CurrentFrame()=18;
    Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&!morph_curve::HasCurve(f.first.raw(),1)&&
        f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&f.app->CurrentFrame()==18,
        "large finite VMD handles retain exact source-independent undo redo at another frame");f.SameEffective(1);
}
void MorphVmdCurveParityChecks(){
    for(bool authored:{false,true})for(unsigned offset:{0u,100u}){MorphVmdCurveFixture f;std::string error;
        std::printf("    morph VMD parity authored=%u offset=%u\n",unsigned(authored),offset);
        if(authored){auto original=f.Input(1);Check(morph_curve::Write(f.first.raw(),{original},error)&&morph_curve::Write(f.second.raw(),{original},error),"seed preexisting pristine morph layers for import merge");
            const auto slot=f.first.m->morphKeys[1].next;f.first.m->morphKeys[slot].value+=.19f;f.second.m->morphKeys[slot].value+=.19f;}
        auto imported=f.Input(1);imported.track.keys.resize(2);imported.track.keys[0].value=.37;imported.track.keys[1].value=.63;
        auto source=f.Source({imported},offset);f.Follow(1);const auto before=MorphKeysCopy(f.first);const double beforeZero=f.Effective(1).track.keys.front().value;
        const auto peer=f.firstMorphs[0];const auto ring=PoseRingDigest(f.first);const auto digest=f.Digest();
        morph_curve::AuthoredLayers raw;Check(morph_curve::CaptureLayers(f.first.raw(),raw,error),"capture exact raw author state before VMD preparation");auto change=f.PlanImport(source);
        Check(change&&f.Digest()==digest&&morph_curve::LayersUnchanged(f.first.raw(),raw),"standard and author tail preparation leaves all live raw state untouched");
        Check(f.Oracle(source)==1,"legacy ApplyVmd accepts the same marked float-matched track");f.Commit(change);
        Check(MorphKeysMatch(f.first,MorphKeysCopy(f.second))&&PoseRingDigest(f.first)==ring&&!std::memcmp(&f.firstMorphs[0],&peer,sizeof peer),"author tail does not remirror standard keys marks slots or unrelated weights");f.SameEffective(1);
        animation_curve::Track effective;Check(morph_curve::Read(f.first.raw(),1,effective,error)&&MorphSameFloat(f.firstMorphs[1].value,float(animation_curve::Evaluate(effective,f.app->CurrentFrame()).value)),"following live morph weight uses imported exact curve at the unchanged playhead");
        if(offset)Check(effective.keys.front().frame==0&&effective.keys.front().value==beforeZero&&MorphSameFloat(f.first.m->morphKeys[1].value,before[1].value),
            "offset import keeps both the precise authored zero value and the unchanged native float anchor");
        const auto after=MorphKeysCopy(f.first);Check(!f.PlanImport(source),"identical standard and morph author import is a true no-op");source={};f.app->CurrentFrame()=19;
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&morph_curve::LayersUnchanged(f.first.raw(),raw),"undo restores original raw pristine derived mirror and observed state after source destruction");
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after)&&f.app->CurrentFrame()==19,"redo owns exact author and native payload and never moves the playhead");f.SameEffective(1);
    }
}
void MorphVmdCurveMappingAndSkipChecks(){
    for(unsigned reason=0;reason<6;++reason){MorphVmdCurveFixture f;auto imported=f.Input(1);auto source=f.Source({imported});
        if(reason==0)source.morphCurveBlock.back()^=1;
        if(reason==1){imported.track.keys[1].value+=.3;source.morphCurveBlock=MorphVmdCurveBlock({imported});}
        if(reason==2){source.curveTargetsResolved=true;source.morphCurveTargets.clear();}
        if(reason==3){source.morphs.erase(source.morphs.begin()+1);unsigned slot=1;while(f.first.m->morphKeys[slot].frame!=12&&f.first.m->morphKeys[slot].next)slot=f.first.m->morphKeys[slot].next;
            // A previously marked matching key is not proof that this import
            // created it. Use a frame present in the original destination.
            f.first.m->morphKeys[slot].allocated=23;imported.track.keys[1].frame=12;imported.track.keys[1].value=double(f.first.m->morphKeys[slot].value);source.morphCurveBlock=MorphVmdCurveBlock({imported});}
        if(reason==4){std::memset(f.firstMorphs[2].name,'x',20);std::memcpy(f.firstMorphs[2].name,f.firstMorphs[1].name,20);for(auto& record:source.morphs){record.targetResolved=true;record.targetIndex=1;}}
        if(reason==5){std::array<char,20> name;std::memcpy(name.data(),f.firstMorphs[1].name,20);name[19]='x';imported.id=morph_curve::StableId(1,name.data());source.morphCurveBlock=MorphVmdCurveBlock({imported});}
        auto change=f.PlanImport(source);Check(change!=nullptr,"invalid optional curve keeps valid standard morph import");f.Commit(change);
        Check(!morph_curve::AnyCurve(f.first.raw())&&f.history.UndoCount()==1,"CRC value owner ambiguity incomplete import and precise raw-name mismatches skip author tail only");
        Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,"skipped optional tail preserves ordinary standard import undo redo");
    }
    {
        MorphVmdCurveFixture f;auto imported=f.Input(1);std::memset(f.firstMorphs[0].name,'a',20);std::memcpy(f.firstMorphs[1].name,f.firstMorphs[0].name,20);
        imported.id=morph_curve::StableId(7,f.firstMorphs[0].name);plugins::ModelPasteSource source;source.curveTargetsResolved=true;source.morphCurveTargets.assign(8,-1);source.morphCurveTargets[7]=1;
        for(const auto& key:imported.track.keys){plugins::ModelMorphPasteRecord rec;rec.targetResolved=true;rec.targetIndex=1;rec.key.frame=unsigned(key.frame);rec.key.value=float(key.value);source.morphs.push_back(rec);}
        source.morphCurveBlock=MorphVmdCurveBlock({imported});f.Commit(f.PlanImport(source));
        Check(morph_curve::HasCurve(f.first.raw(),1)&&!morph_curve::HasCurve(f.first.raw(),0),"explicit Unicode owner mapping wins even when encoded names collide and owner index differs");
    }
    {
        MorphVmdCurveFixture f;auto valid=f.Input(1),invalid=f.Input(0);auto source=f.Source({valid});source.morphCurveBlock=MorphVmdCurveBlock({invalid,valid});f.Commit(f.PlanImport(source));
        Check(morph_curve::HasCurve(f.first.raw(),1)&&!morph_curve::HasCurve(f.first.raw(),0),"one unmatched track never discards another valid author track in the same block");
    }
}
void MorphVmdCurveMetadataAndHistoryChecks(){
    MorphVmdCurveFixture f;auto first=f.Input(1);auto source=f.Source({first});f.Commit(f.PlanImport(source));const auto native=MorphKeysCopy(f.first);
    auto second=first;second.track.keys[1].inSlope=.117;source.morphCurveBlock=MorphVmdCurveBlock({second});auto changed=f.PlanImport(source);Check(changed!=nullptr,"identical standard native values with different author ease still create an edit");f.Commit(changed);
    Check(MorphKeysMatch(f.first,native)&&f.history.UndoCount()==2,"author-only VMD edit does not remirror or change the original standard pool");
    f.firstMorphs[1].value=.42f;MorphRegistrationCommit(f,MorphRegistrationPlan(f,1,18));auto edited=f.Effective(1);edited.track.keys.back().value+=.11;f.Commit(f.Plan({edited}));
    f.app->CurrentFrame()=23;Check(f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK,"curve editor native registration and author-only import unwind in shared order");
    Check(MorphKeysMatch(f.first,native)&&f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,"all mixed history nodes replay retained author states without original file access");
}
void MorphVmdCurveFailureChecks(){
    {
        MorphVmdCurveFixture f;auto source=f.Source({f.Input(1)});const auto digest=f.Digest();auto change=f.PlanImport(source);std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.Configure(plugins::SharedHistory::RecordingMode::ExplicitOnly,8,1)==MMS_OK&&f.history.PrepareNative("morph VMD",change,transaction)==MMS_E_MEMORY&&!transaction&&f.Digest()==digest,"author layer retention is budgeted before any standard publication");
    }
    {
        MorphVmdCurveFixture f;auto source=f.Source({f.Input(1)});auto change=f.PlanImport(source);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare VMD author and native token privately");f.first.m->morphKeys[1].allocated^=1;
        Check(!candidate->IsCurrent(),"short VMD token freezes selection as well as author and native data");
    }
    unsigned failures=0;
    for(long allocation=0;allocation<2048;++allocation){MorphVmdCurveFixture f;auto source=f.Source({f.Input(1)});std::string error;auto unowned=f.Input(0);
        Check(morph_curve::Write(f.first.raw(),{unowned},error),"seed unowned layers for VMD allocation sweep");morph_curve::AuthoredLayers raw;Check(morph_curve::CaptureLayers(f.first.raw(),raw,error),"capture raw before injected VMD failure");
        const auto digest=f.Digest();std::shared_ptr<plugins::HistoryChange> change;std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;failAfter=allocation;
        auto result=plugins::PlanModelKeyPaste(*f.app,0,source,change);if(result==MMS_OK)result=plugins::Boundary([&]{return f.history.PrepareNative("morph VMD",change,transaction);});failAfter=-1;
        Check(f.Digest()==digest&&morph_curve::LayersUnchanged(f.first.raw(),raw)&&!f.history.UndoCount(),"VMD allocation failures and private successful preparation leave native author and journal unchanged");
        if(result==MMS_OK){Check(transaction&&failures>20,"injected VMD allocation sweep reaches complete prepared transaction");Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,"standard and morph author VMD publish without allocations");return;}
        Check(result==MMS_E_MEMORY&&!transaction,"real VMD allocation failure reports memory rather than silently skipping the author tail");++failures;
    }
    Check(false,"VMD author preparation eventually succeeds under allocation injection");
}
void NativeMorphVmdCurvesChecks(){
    std::puts("  morph VMD curves: legacy finite samples outside the float control hull");MorphVmdFiniteCurveOutsideFloatHullChecks();
    std::puts("  morph VMD curves: legacy private import and dense merge parity");MorphVmdCurveParityChecks();
    std::puts("  morph VMD curves: Unicode mapping per-track skip and exact standard ownership");MorphVmdCurveMappingAndSkipChecks();
    std::puts("  morph VMD curves: author-only shared history and failure atomicity");MorphVmdCurveMetadataAndHistoryChecks();MorphVmdCurveFailureChecks();
}
}
