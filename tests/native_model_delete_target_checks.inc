// Include immediately after native_model_key_checks.inc.
// Positive-key participants only, not the complete positive/negative DELETE command.
#include "../src/plugins/native_model_keys.hpp"
namespace model_delete_target_checks {
using Change=plugins::ModelKeyDeletion;
using Prepared=plugins::PreparedModelKeyDeletion;
enum class Families { All, Display, Morph, Bone };

struct Authors {
    bone_scale::AuthoringState scale;
    std::vector<std::uint8_t> bones,morphs;
    explicit Authors(ModelKeyFixture& f):scale(bone_scale::AuthoringState::Capture(f.first.raw())) {
        std::string error;std::vector<animation_curve::NamedTrack> tracks;
        const auto state=bone_curve::AuthoringState::Capture(f.first.raw());
        for(int bone=0;bone<2;++bone) {
            bone_curve::Tracks lanes;
            if(state->Read(bone,lanes))for(unsigned lane=0;lane<lanes.size();++lane)
                tracks.push_back({std::to_string(bone)+":"+std::to_string(lane),lanes[lane]});
        }
        Check(animation_curve::Encode(tracks,bones,error),"encode complete model bone authoring without owner identities");
        morph_curve::AuthoredLayers layers;tracks.clear();
        Check(morph_curve::CaptureLayers(f.first.raw(),layers,error)&&morph_curve::LayersTracks(layers,tracks,error)&&
              animation_curve::Encode(tracks,morphs,error),"encode complete model morph authoring");
    }
    bool Matches(ModelKeyFixture& f) const {
        const Authors current(f);
        return scale.Equals(current.scale)&&bones==current.bones&&morphs==current.morphs;
    }
};
struct Live {
    std::array<mdl::BoneRecord,2> bones;
    std::array<unsigned char,2> dirty,selected;
    ModelKeyDerivedState derived;
    bone_scale::Values scales;
    unsigned char visible;
    explicit Live(ModelKeyFixture& f):bones(f.first.bones),dirty(f.first.dirty),selected(f.first.selected),
        derived(f),scales(bone_scale::PoseValues(f.first.raw())),visible(f.first.m->loadComplete){}
    bool Matches(ModelKeyFixture& f) const {
        return !std::memcmp(bones.data(),f.first.bones.data(),sizeof(bones))&&dirty==f.first.dirty&&selected==f.first.selected&&
            derived.Matches(f)&&bone_scale::PoseValuesEqual(f.first.raw(),scales)&&visible==f.first.m->loadComplete;
    }
};
plugins::ModelKeySelection Seed(ModelKeyFixture& f,Families families,bool privateSelection) {
    std::string error;
    auto tracks=RegistrationFreeTracks();
    for(auto& lane:tracks)lane.keys.back().frame=30;
    tracks[0].keys.front().value=4.25;tracks[6].keys.front().value=1.25;
    if(families!=Families::Bone)
        Check(bone_curve::Write(f.first.raw(),0,tracks,error),"seed independent aggregate bone authoring");
    else {
        // Keep this window regression native: a pristine Free channel could
        // override native window clipping and hide the wrong-selector bug.
        f.first.m->boneKeys[0].position[0]=4.25f;
        auto scale=bone_scale::Matrix{};scale.v[0]=1.25f;bone_scale::SetKey(f.first.raw(),0,scale);
    }
    const auto morph=MorphAuthoredTrack(f.first.morphs[0],0);
    Check(morph_curve::Write(f.first.raw(),{morph},error),"seed independent aggregate morph authoring");
    // Bone 1 has no selected key, but its frame-zero evaluation depends on display.
    f.first.bones[1].slotIndex=0;f.first.m->boneKeys[1].position[0]=9;
    f.firstOrder[0]={1,7,23,1,0};
    f.first.m->displayKeys[0].selectorStates[0]={1,0};
    f.first.m->displayKeys[0].visible=0;f.first.m->displayKeys[0].ikStates[0]=0;
    if(families==Families::Bone) {
        f.first.bones[0].slotIndex=0;f.first.bones[1].slotIndex=-1;
        f.firstOrder[0]={0,30,0,-1,0};
    }
    f.first.m->loadComplete=1;f.firstIk[0].enabled=1;
    f.first.bones[0].trans[0]=765;f.first.bones[1].trans[0]=-543;f.first.dirty={1,0};
    f.first.morphs[0].value=4.75f;
    auto draft=bone_scale::Matrix{};draft.v[0]=2.75f;
    Check(bone_scale::SetPose(f.first.raw(),0,draft),"seed dirty scale draft independently of registered zero");
    // These are fixture inputs, never temporary changes to bypass a family gate.
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.first.m->boneKeys);++i)f.first.m->boneKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)f.first.m->morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.first.m->displayKeys);++i)f.first.m->displayKeys[i].allocated=0;
    plugins::ModelKeySelection selection;
    if(families==Families::All||families==Families::Bone)selection.bones={0,f.first.m->boneKeys[0].next};
    if(families==Families::All||families==Families::Morph)selection.morphs={0,f.first.m->morphKeys[0].next};
    if(families==Families::All||families==Families::Display)selection.displays={0,1};
    if(!selection.bones.empty())Check(selection.bones[1]>=2,"aggregate seed has a real positive bone key");
    if(!selection.morphs.empty())Check(selection.morphs[1]>=1,"aggregate seed has a real positive morph key");
    if(!privateSelection) {
        for(auto slot:selection.bones)f.first.m->boneKeys[slot].allocated=3;
        for(auto slot:selection.morphs)f.first.m->morphKeys[slot].allocated=3;
        for(auto slot:selection.displays)f.first.m->displayKeys[slot].allocated=3;
    }
    return selection;
}
std::shared_ptr<Change> Plan(ModelKeyFixture& f,const plugins::ModelKeySelection* selection=nullptr,bool automatic=false) {
    std::shared_ptr<Change> change;
    Check(plugins::PlanSignedModelKeyDeletion(*f.app,0,automatic,change,false,selection)==MMS_OK&&change,
          "typed model deletion plans without changing shared marks");
    return change;
}
void Publish(plugins::PreparedHistoryChange& candidate) {
    Check(candidate.IsCurrent(),"complete aggregate is current before publication");
    failAfter=0;candidate.Publish();failAfter=-1;
    Check(!candidate.IsCurrent(),"aggregate publication is allocation-free and one-shot");
}
void PayloadsMatch(ModelKeyFixture& f,ModelKeyFixture& oracle) {
    Check(ModelKeyState(oracle.first).Matches(f.first)&&Authors(oracle).Matches(f),
          "complete native key pools deep display payloads and authored tracks match ordinary production candidate");
    Check(mdl::BoneKeyCapacity(f.first.m->boneKeys)==mdl::BoneKeyCapacity(oracle.first.m->boneKeys)&&
          mdl::MorphKeyCapacity(f.first.m->morphKeys)==mdl::MorphKeyCapacity(oracle.first.m->morphKeys)&&
          mdl::KeyPoolCapacity(f.first.m->displayKeys)==mdl::KeyPoolCapacity(oracle.first.m->displayKeys),
          "complete ordinary and typed key pool extents match");
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.first.m->displayKeys);++i) {
        auto a=f.first.m->displayKeys[i],b=oracle.first.m->displayKeys[i];
        a.ikStates=b.ikStates=nullptr;a.selectorStates=b.selectorStates=nullptr;
        Check(!std::memcmp(&a,&b,sizeof(a)),"display record bytes including reserved payload match independently of owned pointers");
    }
    Check(f.first.m->searchCursor==oracle.first.m->searchCursor&&f.first.m->maxFrame==oracle.first.m->maxFrame&&
          f.app->state.lastRegisteredFrame==oracle.app->state.lastRegisteredFrame,
          "aggregate registration metadata matches the ordinary oracle");
}
void TargetsMatch(const Prepared& candidate,ModelKeyFixture& oracle,Families families) {
    std::array<bool,2> seen{};
    const auto& bones=candidate.BoneZeroTargets();
    Check(bones.size()==(families==Families::All?2u:families==Families::Morph?0u:1u),
          "zero bone targets include display-dependent unmarked bones and exclude unrelated tracks");
    for(const auto& target:bones) {
        Check(target.bone<2&&!seen[target.bone],"aggregate bone targets have unique valid identities");seen[target.bone]=true;
        const auto& bone=oracle.first.bones[target.bone];
        Check(target.zeroPose.writePose&&
              !std::memcmp(target.zeroPose.position.data(),bone.trans,sizeof(bone.trans))&&
              !std::memcmp(target.zeroPose.rotation.data(),bone.rotQuat,sizeof(bone.rotQuat))&&
              target.zeroPose.scale==bone_scale::Pose(oracle.first.raw(),int(target.bone)),
              "aggregate bone zero sample agrees with independent ordinary publication and real frame-zero seek");
    }
    if(families==Families::All||families==Families::Display)Check(seen[1],"display zero target reaches the unmarked externally parented bone");
    if(families==Families::Bone)Check(seen[0],"bone-only zero target belongs to the externally parented edited bone");
    const auto& morphs=candidate.MorphZeroTargets();
    Check(morphs.size()==(families==Families::All||families==Families::Morph?1u:0u),"aggregate morph target presence matches participating family");
    if(!morphs.empty())Check(morphs[0].morph==0&&morphs[0].zeroWeight==oracle.first.morphs[0].value,
                            "aggregate morph zero sample uses registered target keys rather than live draft");
    const auto* display=candidate.DisplayZeroTarget();
    Check(bool(display)==(families==Families::All||families==Families::Display),"aggregate display target is null when display keys are not edited");
    if(display)Check(display->visible==oracle.first.m->loadComplete&&display->ik.size()==1&&
        display->ik[0]==oracle.firstIk[0].enabled&&display->selectors.size()==1&&
        !std::memcmp(display->selectors.data(),oracle.firstOrder.data(),sizeof(oracle.firstOrder)),
        "aggregate registered display zero ignores first-publication private draft");
}
void Directions() {
    // Bound the interpolation axis to both supported policies; retain every
    // existing family/selection case and the same independent ordinary oracle.
    for(bool automatic:{false,true})
    for(auto families:{Families::All,Families::Display,Families::Morph})for(bool privateSelection:{false,true}) {
        ModelKeyFixture f,oracle;
        f.Automatic(automatic);oracle.Automatic(automatic);
        const auto selection=Seed(f,families,privateSelection),otherSelection=Seed(oracle,families,privateSelection);
        f.app->CurrentFrame()=-17;oracle.app->CurrentFrame()=0;
        PayloadsMatch(f,oracle);
        const ModelKeyState before(f.first);const Authors beforeAuthors(f);const Live live(f);
        const auto ring=PoseRingDigest(f.first),digest=f.first.Digest();
        const auto peer=f.second.Digest();
        auto change=Plan(f,privateSelection?&selection:nullptr,automatic);
        std::shared_ptr<plugins::HistoryChange> ordinary;
        Check(plugins::PlanModelKeyDeletion(*oracle.app,0,automatic,ordinary,false,privateSelection?&otherSelection:nullptr)==MMS_OK&&ordinary,
              "independent app plans an ordinary aggregate from identical seed and selection");
        Check(f.first.Digest()==digest&&beforeAuthors.Matches(f),"planning preserves target source and authoring");
        for(bool forward:{true,false,true}) {
            const ModelKeyState source(f.first);const Authors sourceAuthors(f);const auto sourceDigest=f.first.Digest();
            std::unique_ptr<Prepared> candidate;std::unique_ptr<plugins::PreparedHistoryChange> expected;
            Check(change->PrepareSigned(forward,candidate)==MMS_OK&&candidate&&candidate->IsCurrent(),
                  "all signed model participants prepare at the real negative playhead");
            Check(source.Matches(f.first)&&sourceAuthors.Matches(f)&&f.first.Digest()==sourceDigest&&live.Matches(f)&&
                  f.app->CurrentFrame()==-17,"preparation never seeks target app or contaminates live drafts");
            Check(ordinary->Prepare(forward,expected)==MMS_OK&&expected,"ordinary independent candidate prepares same direction");
            Publish(*expected);
            // Seek only the independent oracle; dirty flags and target frame are never rewritten.
            SeekModelFrame(oracle.first.raw(),0,0);
            TargetsMatch(*candidate,oracle,families);
            for(const auto& target:candidate->BoneZeroTargets())
                Check(target.zeroPose.position[0]!=live.bones[target.bone].trans[0],"registered bone zero is distinguishable from dirty or selector drafts");
            for(const auto& target:candidate->MorphZeroTargets())
                Check(target.zeroWeight!=live.derived.morphs[target.morph].value,"registered morph zero is distinguishable from live draft");
            Publish(*candidate);PayloadsMatch(f,oracle);
            Check(live.Matches(f)&&f.second.Digest()==peer&&f.app->CurrentFrame()==-17&&f.SelectionUnchanged()&&
                  PoseRingDigest(f.first)==ring&&!f.app->SceneModified()&&!f.history.UndoCount()&&!f.history.RedoCount(),
                  "negative aggregate changes keys and authoring only, preserving complete live pose scale dirty morph display and selectors");
            if(!forward)Check(before.Matches(f.first)&&beforeAuthors.Matches(f),"reverse restores complete original native and authored state");
        }
    }
}
void Nonnegative() {
    for(bool automatic:{false,true})for(int frame:{0,19})for(bool privateSelection:{false,true}) {
        ModelKeyFixture f,oracle;
        f.Automatic(automatic);oracle.Automatic(automatic);
        const auto selection=Seed(f,Families::All,privateSelection),other=Seed(oracle,Families::All,privateSelection);
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        auto change=Plan(f,privateSelection?&selection:nullptr,automatic);std::shared_ptr<plugins::HistoryChange> ordinary;
        Check(plugins::PlanModelKeyDeletion(*oracle.app,0,automatic,ordinary,false,privateSelection?&other:nullptr)==MMS_OK&&ordinary,
              "nonnegative ordinary aggregate plans from identical draft");
        for(bool forward:{true,false,true}) {
            std::unique_ptr<Prepared> candidate;std::unique_ptr<plugins::PreparedHistoryChange> expected;
            Check(change->PrepareSigned(forward,candidate)==MMS_OK&&candidate&&ordinary->Prepare(forward,expected)==MMS_OK&&expected,
                  "typed and ordinary aggregate prepare at a nonnegative playhead");
            Publish(*candidate);Publish(*expected);PayloadsMatch(f,oracle);
            Check(Live(oracle).Matches(f)&&f.app->CurrentFrame()==frame,
                  "nonnegative typed aggregate preserves ordinary live-pose and private-selection policy");
        }
    }
}
void BoneOnlyZeroWindow() {
    ModelKeyFixture f,ordinary,zero;
    Seed(f,Families::Bone,false);Seed(ordinary,Families::Bone,false);Seed(zero,Families::Bone,false);
    f.app->CurrentFrame()=ordinary.app->CurrentFrame()=35;zero.app->CurrentFrame()=0;
    const DisplayTestState displayBefore(f.first);
    Check(f.firstOrder[0].windowStart==30&&f.firstOrder[0].windowEnd==0&&
          f.first.m->displayKeys[0].next==1&&f.first.m->displayKeys[1].frame==10,
          "bone-only source uses live [30,0) while its registered zero display segment ends at 10");
    auto change=Plan(f);std::shared_ptr<plugins::HistoryChange> normal,zeroNormal;
    Check(plugins::PlanModelKeyDeletion(*ordinary.app,0,false,normal)==MMS_OK&&normal&&
          plugins::PlanModelKeyDeletion(*zero.app,0,false,zeroNormal)==MMS_OK&&zeroNormal,
          "bone-only normal and zero oracles independently plan the identical source");
    for(bool forward:{true,false,true}) {
        const Live live(f);std::unique_ptr<Prepared> candidate;
        std::unique_ptr<plugins::PreparedHistoryChange> expected,zeroExpected;
        Check(change->PrepareSigned(forward,candidate)==MMS_OK&&candidate&&candidate->IsCurrent()&&live.Matches(f)&&
              f.app->CurrentFrame()==35,"bone-only aggregate reads zero selectors without changing the real playhead or live window");
        Check(normal->Prepare(forward,expected)==MMS_OK&&expected&&zeroNormal->Prepare(forward,zeroExpected)==MMS_OK&&zeroExpected,
              "independent ordinary candidates prepare both live-time and zero-time references");
        Publish(*expected);Publish(*zeroExpected);SeekModelFrame(zero.first.raw(),0,0);
        Check(zero.firstOrder[0].windowStart==0&&zero.firstOrder[0].windowEnd==10,
              "real zero seek confirms registered [0,10) selectors on the independent oracle");
        if(!forward)Check(zero.first.bones[0].trans[0]!=0,
                          "reverse native zero pose is nonzero so a stale terminal-window reset cannot pass vacuously");
        TargetsMatch(*candidate,zero,Families::Bone);
        Publish(*candidate);PayloadsMatch(f,ordinary);
        Check(Live(ordinary).Matches(f)&&displayBefore.Matches(f.first)&&f.firstOrder[0].windowStart==30&&
              f.firstOrder[0].windowEnd==0&&f.app->CurrentFrame()==35,
              "source-zero dependency preserves ordinary frame-35 behavior and never publishes display keys or zero windows");
    }
}
struct Adapter final:plugins::HistoryChange {
    std::shared_ptr<Change> change;
    explicit Adapter(std::shared_ptr<Change> value):change(std::move(value)){}
    MmsResult Prepare(bool forward,std::unique_ptr<plugins::PreparedHistoryChange>& out) override {
        std::unique_ptr<Prepared> candidate;const auto result=change->PrepareSigned(forward,candidate);
        if(result==MMS_OK)out=std::move(candidate);return result;
    }
    std::uint64_t RetainedBytes() const noexcept override{return change->RetainedBytes();}
};
void HistoryAndFailures() {
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        ModelKeyFixture f;const auto selection=Seed(f,Families::All,true);f.app->CurrentFrame()=-17;
        Check(f.history.SetMode(mode)==MMS_OK,"select common-history recording policy");
        auto adapter=std::make_shared<Adapter>(Plan(f,&selection));
        const ModelKeyState before(f.first);const Authors authors(f);const Live live(f);const auto digest=f.first.Digest();
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        unsigned failures=0;bool ready=false;
        for(long limit=0;limit<4096;++limit) {
            failAfter=mode==plugins::SharedHistory::RecordingMode::AllOperations?limit:-1;
            const auto result=f.history.PrepareNative("model targets",adapter,transaction);failAfter=-1;
            Check(before.Matches(f.first)&&authors.Matches(f)&&live.Matches(f)&&f.first.Digest()==digest&&
                  !f.history.UndoCount()&&!f.history.RedoCount(),"OOM sweep never publishes a partial family or journal entry");
            if(result==MMS_OK){ready=bool(transaction);break;}
            Check(result==MMS_E_MEMORY&&!transaction,"failed aggregate preparation leaves no partial transaction");++failures;
        }
        Check(ready&&(mode!=plugins::SharedHistory::RecordingMode::AllOperations||failures>8),
              "allocation sweep reaches a complete aggregate and both modes reserve one journal entry");
        Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK&&f.history.UndoCount()==1&&live.Matches(f),
              "typed-to-base adapter commits all positive-key participants as one explicit common-history entry");
        const ModelKeyState after(f.first);const Authors afterAuthors(f);
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&authors.Matches(f)&&live.Matches(f)&&
              !f.history.UndoCount()&&f.history.RedoCount()==1,"one common undo restores the complete aggregate");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&afterAuthors.Matches(f)&&live.Matches(f)&&
              f.history.UndoCount()==1&&!f.history.RedoCount()&&f.app->CurrentFrame()==-17,"one common redo republishes the complete aggregate");
    }
}
void StaleParticipants() {
    for(unsigned mutation=0;mutation<6;++mutation) {
        ModelKeyFixture f;const auto selection=Seed(f,mutation==5?Families::Bone:Families::All,true);
        f.app->CurrentFrame()=mutation==5?35:-17;
        auto adapter=std::make_shared<Adapter>(Plan(f,&selection));
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("signed model stale",adapter,transaction)==MMS_OK&&transaction,
              "all aggregate participants are prepared before external mutation");
        const ModelKeyState preparedKeys(f.first);
        const auto parent=f.first.m->displayKeys[0].selectorStates[0];
        auto* peer=f.second.m->displayKeys[3].selectorStates;
        morph_curve::AuthoredLayers replacement;std::string error;
        if(mutation==0||mutation==5)f.first.m->displayKeys[0].selectorStates[0].boneIndex=1;
        if(mutation==1) {
            morph_curve::AuthoredLayers original;std::vector<animation_curve::NamedTrack> tracks;
            Check(morph_curve::CaptureLayers(f.first.raw(),original,error)&&morph_curve::LayersTracks(original,tracks,error)&&!tracks.empty(),
                  "read real authored source for an author-only stale mutation");
            tracks[0].track.keys[0].outSlope+=.125;
            Check(morph_curve::RebuildLayers(f.first.raw(),tracks,replacement,error),"prepare changed pristine tangent without native key writes");
            morph_curve::SwapLayers(f.first.raw(),replacement);
            Check(preparedKeys.Matches(f.first),"author-only tangent mutation leaves every native key byte unchanged");
        }
        if(mutation==2)f.app->CurrentFrame()=-16;
        if(mutation==3)f.app->ModelSlot(0)=nullptr;
        if(mutation==4)f.second.m->displayKeys[3].selectorStates=f.first.m->displayKeys[0].selectorStates;
        const ModelKeyState external(f.first);const Authors externalAuthors(f);const Live externalLive(f);
        const auto first=f.first.Digest(),second=f.second.Digest();
        const int externalFrame=f.app->CurrentFrame();const auto externalModel=f.app->ModelSlot(0);
        const auto result=CommitRegistrationNoAllocation(f.history,std::move(transaction));
        const bool unchanged=external.Matches(f.first)&&externalAuthors.Matches(f)&&externalLive.Matches(f)&&
            f.first.Digest()==first&&f.second.Digest()==second&&f.app->CurrentFrame()==externalFrame&&
            f.app->ModelSlot(0)==externalModel&&!f.history.UndoCount()&&!f.history.RedoCount();
        // Restore borrowed pointers before any assertion can unwind the fixture.
        f.first.m->displayKeys[0].selectorStates[0]=parent;
        if(mutation==1)morph_curve::SwapLayers(f.first.raw(),replacement);
        f.app->CurrentFrame()=-17;f.app->ModelSlot(0)=f.first.raw();f.second.m->displayKeys[3].selectorStates=peer;
        Check(result==MMS_E_STALE&&!transaction&&unchanged,
              "deep display including an unedited source-zero dependency, author, frame, model and peer-alias changes reject the whole aggregate");
    }
}
}
void NativeModelDeleteTargetChecks() {
    model_delete_target_checks::Directions();
    model_delete_target_checks::Nonnegative();
    model_delete_target_checks::BoneOnlyZeroWindow();
    model_delete_target_checks::HistoryAndFailures();
    model_delete_target_checks::StaleParticipants();
}
