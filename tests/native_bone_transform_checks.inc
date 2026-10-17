// Include after native_bone_registration_checks.inc; call NativeBoneTransformChecks().
// Deletion uses the real old writer. The range oracle follows ScaleBoneKeys in
// src/window/frame_range_apply.cpp: copy by slot, delete, open type 4, reset the
// cursor, then RegisterBoneKey with one matrix payload per saved record. It
// deliberately omits DeleteMarkedKeyframes/PanelPaint/SeekModelFrame: this hidden
// CPU fixture has no renderer. Neither oracle calls the new planner.
#include "mikudancestudio/frame_limits.hpp"
#include "../src/plugins/native_key_commands.hpp"
#include <limits>
namespace mikudancestudio { void ApplyFrameRangeScale(HWND); }
namespace {
void TransformProgress(const char* name) {
    std::printf("  bone transform: %s\n",name);std::fflush(stdout);
}
void TransformSeed(Fixture& f) {
    const auto capacity=mdl::BoneKeyCapacity(f.m->boneKeys);
    std::memset(f.m->boneKeys,0,capacity*sizeof(mdl::BoneKey));
    f.m->maxFrame=40;f.m->searchCursor=17;f.m->selectedBone=1;
    for(unsigned bone=0;bone<2;++bone) {
        f.m->boneKeys[bone].rotation[3]=1;f.m->boneKeys[bone].position[0]=float(bone+2);
        f.bones[bone].type=mdl::BoneType::RotateMove;f.bones[bone].trans[0]=float(bone+8);
        f.bones[bone].slotIndex=-1; // This fixture has no external-parent selector table.
        f.selected[bone]=1;f.dirty[bone]=1;
        std::snprintf(f.bones[bone].name,sizeof(f.bones[bone].name),"transform%u",bone);
    }
    const unsigned slots[2][4]={{6,3,8,5},{7,2,9,0}};
    for(unsigned bone=0;bone<2;++bone) {
        const unsigned count=bone?3:4;unsigned previous=bone;
        for(unsigned i=0;i<count;++i) {
            const auto slot=slots[bone][i];auto& key=f.m->boneKeys[slot];
            f.m->boneKeys[previous].next=slot;key.previous=previous;previous=slot;
            key.frame=(i+1)*10+bone*2;key.position[0]=float(key.frame+100*bone);
            key.position[1]=i==2?-3.f:float(i*4);key.position[2]=float(2-int(i));
            key.rotation[3]=1;key.physicsDisabled=static_cast<unsigned char>(i%2);
            key.reserved[0]=static_cast<unsigned char>(slot+20);
            for(unsigned lane=0;lane<4;++lane) {
                key.interpolation[lane]=static_cast<unsigned char>(12+slot);
                key.interpolation[lane+4]=static_cast<unsigned char>(15+lane);
                key.interpolation[lane+8]=static_cast<unsigned char>(100-slot);
                key.interpolation[lane+12]=static_cast<unsigned char>(110-lane);
            }
            bone_scale::SetKey(f.raw(),int(slot),Scale(float(slot+2)));
            bone_scale::SetKeyCurve(f.raw(),int(slot),bone_scale::Curve{12,24,90,110});
        }
        bone_scale::SetPose(f.raw(),int(bone),Scale(float(bone+3)));
    }
    for(std::size_t i=0;i<mdl::MorphKeyCapacity(f.m->morphKeys);++i)f.m->morphKeys[i].allocated=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(f.m->displayKeys);++i)f.m->displayKeys[i].allocated=0;
}
std::shared_ptr<plugins::HistoryChange> TransformPlan(BoneRegistrationFixture& f,bool range,
    const plugins::BoneRangeScaleOptions& options,unsigned slot=0,bool selected=false) {
    std::shared_ptr<plugins::HistoryChange> result;
    const auto status=range?plugins::PlanBoneRangeScale(*f.app,slot,options,result):
        plugins::PlanBoneKeyDeletion(*f.app,slot,options.automaticInterpolation,result,selected);
    Check(status==MMS_OK,"bone transform planning succeeds");return result;
}
void TransformCommit(BoneRegistrationFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    Check(bool(change),"bone transform owns a nonempty record");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("bone transform",change,transaction)==MMS_OK,"bone transform journal prepares");
    Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,
        "bone transform journal publishes without allocation");
}
void TransformCandidatePublish(const std::shared_ptr<plugins::HistoryChange>& change,bool forward) {
    std::unique_ptr<plugins::PreparedHistoryChange> candidate;
    Check(change->Prepare(forward,candidate)==MMS_OK&&candidate&&candidate->IsCurrent(),
        "bone transform direction prepares a current candidate");
    failAfter=0;candidate->Publish();candidate.reset();failAfter=-1;
}
void TransformCheckScales(Fixture& actual,Fixture& expected) {
    for(unsigned slot=0;slot<32;++slot)
        Check(bone_scale::Key(actual.raw(),int(slot))==bone_scale::Key(expected.raw(),int(slot))&&
            bone_scale::KeyCurve(actual.raw(),int(slot))==bone_scale::KeyCurve(expected.raw(),int(slot)),
            "bone transform matrix and scale curve match the old writer by slot");
}
void TransformRangeOracle(BoneRegistrationFixture& f,const plugins::BoneRangeScaleOptions& o) {
    auto& fixture=f.second;auto& model=*fixture.m;auto* keys=model.boneKeys;
    struct Copy { unsigned owner; std::array<unsigned char,84> data{}; bone_scale::Matrix scale; bone_scale::Curve curve; };
    std::vector<Copy> copies;const auto capacity=mdl::BoneKeyCapacity(keys);
    for(std::size_t slot=0;slot<capacity;++slot) {
        auto& key=keys[slot];key.allocated=0;
        if((key.frame||slot<model.boneCount)&&key.frame>=o.start)key.allocated=1;
    }
    for(std::size_t slot=0;slot<capacity;++slot)if(keys[slot].allocated) {
        Copy copy;copy.owner=unsigned(slot);
        while(copy.owner>=model.boneCount)copy.owner=keys[copy.owner].previous;
        const auto& key=keys[slot];
        const auto relative=key.frame<=o.end?std::uint32_t(std::int64_t(double(key.frame-o.start)*o.scale)):
            key.frame+std::uint32_t(std::int64_t(double(o.end-o.start)*o.scale))-o.end;
        std::memcpy(copy.data.data()+32,&relative,sizeof(relative));
        std::memcpy(copy.data.data()+36,key.rotation,sizeof(key.rotation));
        std::memcpy(copy.data.data()+52,key.position,sizeof(key.position));
        copy.data[64]=key.physicsDisabled;
        std::memcpy(copy.data.data()+65,key.interpolation,sizeof(key.interpolation));
        copy.scale=bone_scale::Key(fixture.raw(),int(slot));copy.curve=bone_scale::KeyCurve(fixture.raw(),int(slot));copies.push_back(copy);
    }
    if(copies.empty())return;
    for(std::size_t slot=0;slot<mdl::MorphKeyCapacity(model.morphKeys);++slot)model.morphKeys[slot].allocated=0;
    for(std::size_t slot=0;slot<mdl::KeyPoolCapacity(model.displayKeys);++slot)model.displayKeys[slot].allocated=0;
    DeleteMarkedModelKeys(fixture.raw(),f.app->CurrentFrame());
    BeginRangeScaleBoneUndo(fixture.raw(),f.app->CurrentFrame(),int(copies.size()));
    ResetBoneKeyCursor(fixture.raw());const auto selected=model.selectedBone;
    for(auto& copy:copies) {
        model.selectedBone=int(copy.owner);bone_scale::Registration scale(copy.scale,copy.curve);
        Check(RegisterBoneKey(fixture.raw(),copy.data.data(),int(o.start),1),"legacy range oracle reinserts saved bone key");
    }
    model.selectedBone=selected;
}
void TransformDeletionParity() {
    const std::vector<std::vector<unsigned>> marks={{0},{1},{6},{3},{5},{9},{3,8},{0,1,2,3,5,6,7,8,9}};
    for(bool automatic:{false,true})for(const auto& marked:marks) {
        BoneRegistrationFixture f;TransformSeed(f.first);TransformSeed(f.second);f.Automatic(automatic);
        for(auto slot:marked)for(auto* fixture:{&f.first,&f.second})fixture->m->boneKeys[slot].allocated=1;
        // Root reset has a key physics byte; it does not switch the live solver.
        for(auto* fixture:{&f.first,&f.second}) {
            fixture->bones[0].hasRigidBody=1;fixture->m->boneKeys[0].physicsDisabled=1;
        }
        const auto before=f.first.Digest(),ring=PoseRingDigest(f.first);const auto original=RegistrationKeys(f.first);
        const auto scales=bone_scale::AuthoringState::Capture(f.first.raw());
        plugins::BoneRangeScaleOptions options;options.automaticInterpolation=automatic;
        auto change=TransformPlan(f,false,options);
        Check(change&&f.first.Digest()==before&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scales),
            "deletion planning leaves root links neighbours scale and ring untouched");
        TransformCommit(f,change);DeleteMarkedModelKeys(f.second.raw(),f.app->CurrentFrame());
        Check(!std::memcmp(f.first.m->boneKeys,f.second.m->boneKeys,32*sizeof(mdl::BoneKey)),
            "owned root middle tail and adjacent deletion match every old key byte and interpolation neighbour");
        TransformCheckScales(f.first,f.second);
        Check(f.first.m->maxFrame==f.second.m->maxFrame&&f.first.m->searchCursor==f.second.m->searchCursor&&
            f.first.dirty==f.second.dirty&&PoseRingDigest(f.first)==ring&&f.history.UndoCount()==1,
            "deletion preserves native maxima cursor dirty flags and old ring while recording once");
        const auto deleted=RegistrationKeys(f.first);
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,original)&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scales),"deletion undo restores marked source and scales");
        Check(f.history.History(true)==MMS_OK&&SameRegistrationKeys(f.first,deleted),"deletion redo restores every neighbour byte");
    }
}
void TransformRangeParity() {
    const std::array<plugins::BoneRangeScaleOptions,5> cases={{{10,30,2,false},{10,40,.01,false},
        {0,40,.01,false},{15,30,.5,false},{1,10,3,false}}};
    for(bool automatic:{false,true})for(auto options:cases) {
        BoneRegistrationFixture f;TransformSeed(f.first);TransformSeed(f.second);f.Automatic(automatic);
        options.automaticInterpolation=automatic;f.app->state.lastRegisteredFrame=40;
        for(auto* fixture:{&f.first,&f.second}) {
            fixture->m->boneKeys[0].allocated=1;fixture->m->boneKeys[8].allocated=1;
            fixture->m->morphKeys[0].allocated=3;fixture->m->displayKeys[0].allocated=5;
        }
        const auto original=RegistrationKeys(f.first);const auto ring=PoseRingDigest(f.first),before=f.first.Digest();
        const auto scaleBefore=bone_scale::AuthoringState::Capture(f.first.raw());
        auto change=TransformPlan(f,true,options);
        Check(change&&f.first.Digest()==before&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleBefore),
            "range planning leaves all live key families scale and ring untouched");
        TransformCommit(f,change);TransformRangeOracle(f,options);
        Check(!std::memcmp(f.first.m->boneKeys,f.second.m->boneKeys,32*sizeof(mdl::BoneKey)),
            "range transform matches old slot-order reinsertion including root merge tail links and automatic deletion curves");
        TransformCheckScales(f.first,f.second);
        Check(f.first.m->maxFrame==f.second.m->maxFrame&&f.first.m->searchCursor==f.second.m->searchCursor&&
            f.first.m->selectedBone==f.second.m->selectedBone&&f.first.m->morphKeys[0].allocated==0&&
            f.first.m->displayKeys[0].allocated==0&&PoseRingDigest(f.first)==ring&&f.history.UndoCount()==1,
            "range transforms include selection clearing and native metadata in one record without writing type 4 ring pairs");
        if(options.start==10&&options.scale==.01) {
            const auto slot=f.first.m->boneKeys[0].next;
            Check(f.first.m->boneKeys[slot].frame==10&&f.first.m->boneKeys[slot].position[0]==30&&
                bone_scale::Key(f.first.raw(),int(slot))==Scale(10),
                "compressed collision is won by the last source slot rather than the latest source frame");
        }
        const auto transformed=RegistrationKeys(f.first);
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,original)&&
            f.first.m->morphKeys[0].allocated==3&&f.first.m->displayKeys[0].allocated==5&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleBefore),
            "one range undo restores deletion insertion matrix curves and other family selection");
        Check(f.history.History(true)==MMS_OK&&SameRegistrationKeys(f.first,transformed)&&
            f.first.m->morphKeys[0].allocated==0&&f.first.m->displayKeys[0].allocated==0,
            "one range redo reapplies the entire compound change");
    }
}
void TransformFreeCurveAndRing() {
    for(bool range:{false,true}) {
        BoneRegistrationFixture f;std::string error;auto tracks=RegistrationFreeTracks();
        // Unlike TransformSeed, this path starts with clean pose bytes and
        // therefore samples its keys. A bone without an external parent uses
        // -1 in production; the generic zeroed fixture has no selector table.
        for(auto& bone:f.first.bones)bone.slotIndex=-1;
        tracks[6].keys.back().value=2;
        Check(bone_curve::Write(f.first.raw(),0,tracks,error)&&bone_curve::Write(f.first.raw(),1,RegistrationFreeTracks(),error),
            "two authored free-curve bones seeded for native transform");
        for(std::size_t slot=0;slot<mdl::BoneKeyCapacity(f.first.m->boneKeys);++slot)f.first.m->boneKeys[slot].allocated=0;
        const auto target=f.first.m->boneKeys[0].next;f.first.m->boneKeys[target].allocated=1;
        const auto original=RegistrationKeys(f.first);const auto originalMax=f.first.m->maxFrame;
        const auto originalCursor=f.first.m->searchCursor;const auto originalLast=f.app->state.lastRegisteredFrame;
        const auto scaleBefore=bone_scale::AuthoringState::Capture(f.first.raw());
        const auto curves=bone_curve::AuthoringState::Capture(f.first.raw());
        plugins::BoneRangeScaleOptions options;options.start=10;options.end=20;options.scale=2;
        auto change=TransformPlan(f,range,options);TransformCommit(f,change);
        bone_curve::Tracks changedCurve,otherCurve;
        Check(bone_curve::Read(f.first.raw(),0,changedCurve)&&bone_curve::Read(f.first.raw(),1,otherCurve)&&
            changedCurve[0].keys.back().frame==(range?30:0)&&otherCurve[0].keys.back().frame==(range?30:20)&&
            (!range||(changedCurve[0].keys.front().outWeight==.2&&changedCurve[0].keys.back().value==12&&changedCurve[6].keys.back().value==2)),
            "native deletion/retime keeps authored curves and surviving endpoints, including retimed free handles and scale values");
        const auto transformedCurves=bone_curve::AuthoringState::Capture(f.first.raw());
        const auto transformed=RegistrationKeys(f.first);
        for(unsigned n=0;n<100;++n)PushBoneEditUndo(f.app.get());
        const auto overwritten=PoseRingDigest(f.first);
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,original)&&
            f.first.m->maxFrame==originalMax&&f.first.m->searchCursor==originalCursor&&
            f.app->state.lastRegisteredFrame==originalLast&&
            bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleBefore)&&
            bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*curves),
            "transform undo restores keys metadata scale and exact free authoring after one hundred legacy ring captures");
        bone_curve::Tracks read;
        Check(bone_curve::Read(f.first.raw(),0,read)&&read[0].keys.front().outWeight==.2&&read[6].keys.back().value==2,
            "restored free-curve hash accepts restored native keys and scale");
        Check(f.history.History(true)==MMS_OK&&SameRegistrationKeys(f.first,transformed)&&
            PoseRingDigest(f.first)==overwritten&&bone_curve::Has(f.first.raw(),0)&&
            bone_curve::AuthoringState::Capture(f.first.raw())->Equals(*transformedCurves),
            "compound redo restores reconciled authoring independently of the overwritten legacy ring");
    }
}
void TransformRangeBoneTypes() {
    const bool fixedAvailable=d3dx::Get().Load();
    for(int kind=0;kind<(fixedAvailable?2:1);++kind) {
        BoneRegistrationFixture f;TransformSeed(f.first);TransformSeed(f.second);
        for(auto* fixture:{&f.first,&f.second}) {
            fixture->bones[0].type=mdl::BoneType::RotateGrant;
            if(kind) {
                fixture->m->physicsMode=2;fixture->bones[1].type=mdl::BoneType::FixedAxis;
                fixture->bones[1].axis[0]=.6f;fixture->bones[1].axis[1]=.8f;
                for(unsigned slot:{2u,7u,9u}) {
                    fixture->m->boneKeys[slot].rotation[0]=.3f;
                    fixture->m->boneKeys[slot].rotation[1]=.4f;
                    fixture->m->boneKeys[slot].rotation[3]=.8660254f;
                }
            }
        }
        plugins::BoneRangeScaleOptions options{10,30,1.5,false};
        TransformCommit(f,TransformPlan(f,true,options));TransformRangeOracle(f,options);
        Check(!std::memcmp(f.first.m->boneKeys,f.second.m->boneKeys,32*sizeof(mdl::BoneKey)),
            "range registrar matches legacy translation restrictions and optional fixed-axis quaternion correction");
    }
    if(!fixedAvailable)std::puts("SKIP range fixed-axis parity: D3DX math library unavailable");
}
void TransformDirectionalAndCrossModel() {
    for(bool range:{false,true}) {
        BoneRegistrationFixture f;TransformSeed(f.first);f.first.m->boneKeys[3].allocated=1;
        plugins::BoneRangeScaleOptions options;options.start=10;options.end=30;options.scale=2;options.automaticInterpolation=true;
        auto change=TransformPlan(f,range,options);const auto original=RegistrationKeys(f.first);
        TransformCandidatePublish(change,true);const auto changed=RegistrationKeys(f.first);
        TransformCandidatePublish(change,false);
        Check(SameRegistrationKeys(f.first,original),"inverse candidate publishes exact authoring with allocation disabled");
        TransformCandidatePublish(change,true);
        Check(SameRegistrationKeys(f.first,changed),"redo candidate publishes exact authoring with allocation disabled");
    }
    BoneRegistrationFixture f;TransformSeed(f.first);TransformSeed(f.second);
    f.first.m->boneKeys[3].allocated=1;const auto firstBefore=RegistrationKeys(f.first),secondBefore=RegistrationKeys(f.second);
    const auto firstRing=PoseRingDigest(f.first),secondRing=PoseRingDigest(f.second);
    plugins::BoneRangeScaleOptions options;options.start=10;options.end=30;options.scale=2;
    TransformCommit(f,TransformPlan(f,false,options,0));const auto firstAfter=RegistrationKeys(f.first);
    f.app->SelectedModelSlot()=1;TransformCommit(f,TransformPlan(f,true,options,1));const auto secondAfter=RegistrationKeys(f.second);
    f.app->SelectedModelSlot()=0;
    Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.second,secondBefore)&&SameRegistrationKeys(f.first,firstAfter),
        "common undo first restores the other model's range transform");
    Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,firstBefore),"next common undo restores earlier deletion");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&
        SameRegistrationKeys(f.first,firstAfter)&&SameRegistrationKeys(f.second,secondAfter)&&
        f.app->SelectedModelSlot()==0&&f.SelectionUnchanged()&&
        PoseRingDigest(f.first)==firstRing&&PoseRingDigest(f.second)==secondRing,
        "mixed cross-model transforms redo in order without changing selection or native rings");
}
void TransformNoOpAndRejections() {
    BoneRegistrationFixture f;TransformSeed(f.first);plugins::NativePoseTestBinding binding(*f.app,f.history);
    f.Commit(panel::kReadoutPosXEdit,"31");Check(f.history.History(false)==MMS_OK,"seed redo before transform no-op checks");
    f.first.m->boneKeys[3].allocated=1;f.first.m->morphKeys[0].allocated=2;f.first.m->displayKeys[0].allocated=3;
    const auto before=f.first.Digest();const auto redo=f.history.RedoCount();
    for(auto options:std::array<plugins::BoneRangeScaleOptions,3>{{{10,30,1,false},{41,50,2,false},{10,40,1.00001,false}}}) {
        auto change=TransformPlan(f,true,options);
        Check(!change&&f.first.Digest()==before&&f.history.RedoCount()==redo,
            "unit empty and integer-unchanged ranges preserve selection redo and every model byte");
    }
    std::shared_ptr<plugins::HistoryChange> change;
    Check(plugins::PlanBoneKeyDeletion(*f.app,0,false,change)==MMS_E_NOT_READY&&!change&&f.first.Digest()==before,
        "mixed-family marked deletion rejects before touching any selected key");
    f.first.m->morphKeys[0].allocated=f.first.m->displayKeys[0].allocated=0;f.first.m->boneKeys[3].allocated=0;
    const auto empty=f.first.Digest();
    Check(plugins::PlanBoneKeyDeletion(*f.app,0,false,change)==MMS_OK&&!change&&f.first.Digest()==empty,
        "empty explicit deletion is a true no-op");
    f.app->CurrentFrame()=0;
    Check(plugins::PlanBoneKeyDeletion(*f.app,0,false,change,true)==MMS_OK&&!change&&f.first.Digest()==empty,
        "keyboard fallback never implicitly deletes frame zero roots");
    f.app->CurrentFrame()=20;
    Check(plugins::PlanBoneKeyDeletion(*f.app,0,false,change,true)==MMS_OK&&change&&f.first.Digest()==empty,
        "keyboard fallback selects current-frame keys privately without publishing mark bytes");
    change.reset();
    const std::array<plugins::BoneRangeScaleOptions,8> invalid={{{30,10,2,false},{10,10,2,false},
        {0,40,0,false},{0,40,-1,false},{0,40,(std::numeric_limits<double>::infinity)(),false},
        {0,40,(std::numeric_limits<double>::quiet_NaN)(),false},{0,40,1e20,false},
        {0,std::uint32_t(frame_limits::maximum)+1,2,false}}};
    for(const auto& options:invalid)
        Check(plugins::PlanBoneRangeScale(*f.app,0,options,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==empty,
            "invalid spans rates and resulting frame overflow reject the complete range before mutation");
    const auto original=RegistrationKeys(f.first);
    for(int kind=0;kind<5;++kind) {
        std::memcpy(f.first.m->boneKeys,original.data(),original.size()*sizeof(mdl::BoneKey));
        f.first.m->boneKeys[3].allocated=1;
        if(kind==0)f.first.m->boneKeys[0].next=999;
        if(kind==1)f.first.m->boneKeys[3].next=3;
        if(kind==2)f.first.m->boneKeys[3].frame=9;
        if(kind==3)f.first.m->boneKeys[1].next=6;
        if(kind==4){f.first.m->boneKeys[10].frame=25;f.first.m->boneKeys[10].allocated=1;}
        const auto malformed=f.first.Digest();plugins::BoneRangeScaleOptions options{10,30,2,false};
        Check(plugins::PlanBoneKeyDeletion(*f.app,0,true,change)==MMS_E_ARGUMENT&&!change&&
            plugins::PlanBoneRangeScale(*f.app,0,options,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==malformed,
            "out-of-bounds cycles backwards shared and orphan chains are rejected without partial mutation");
    }
}
void TransformFailureAndStale() {
    for(bool range:{false,true}) {
        BoneRegistrationFixture f;TransformSeed(f.first);f.first.m->boneKeys[3].allocated=1;
        plugins::BoneRangeScaleOptions options{10,30,2,true};
        const auto before=f.first.Digest();const auto original=RegistrationKeys(f.first);
        const auto scales=bone_scale::AuthoringState::Capture(f.first.raw());
        std::shared_ptr<plugins::HistoryChange> change;unsigned planFailures=0;bool planned=false;
        for(long allowance=0;allowance<512&&!planned;++allowance) {
            failAfter=allowance;
            const auto result=range?plugins::PlanBoneRangeScale(*f.app,0,options,change):
                plugins::PlanBoneKeyDeletion(*f.app,0,true,change);
            failAfter=-1;
            Check(f.first.Digest()==before&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scales)&&
                f.history.UndoCount()==0,"every transform planning allocation failure preserves keys scale ring and journal");
            if(result==MMS_OK){Check(bool(change),"fault sweep eventually returns an owned transform");planned=true;}
            else{++planFailures;Check(result==MMS_E_MEMORY&&!change,"failed transform plan has no partial output");}
        }
        Check(planned&&planFailures>8,"transform planning fault sweep reaches independent payload participants");
        unsigned prepareFailures=0;bool committed=false;
        for(long allowance=0;allowance<512&&!committed;++allowance) {
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;std::string label="bone transform allocation";
            failAfter=allowance;const auto result=f.history.PrepareNative(std::move(label),change,transaction);failAfter=-1;
            Check(f.first.Digest()==before&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scales),
                "every transform prepare failure preserves the complete live before state");
            if(result==MMS_OK){Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,
                "fault sweep completes with one allocation-free transform commit");committed=true;}
            else{++prepareFailures;Check(result==MMS_E_MEMORY&&!transaction&&f.history.UndoCount()==0,
                "failed transform journal preparation keeps history empty");}
        }
        Check(committed&&prepareFailures>5,"journal fault sweep reaches pool and side-table candidates");
        const auto changed=RegistrationKeys(f.first);const auto after=f.first.Digest();
        failAfter=0;const auto failedUndo=f.history.History(false);failAfter=-1;
        Check(failedUndo==MMS_E_MEMORY&&f.first.Digest()==after&&f.history.UndoCount()==1&&f.history.RedoCount()==0,
            "failed inverse preparation cannot partially undo transform participants");
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,original),"transform remains undoable after inverse allocation failure");
        const auto undone=f.first.Digest();failAfter=0;const auto failedRedo=f.history.History(true);failAfter=-1;
        Check(failedRedo==MMS_E_MEMORY&&f.first.Digest()==undone&&f.history.RedoCount()==1,
            "failed redo preparation preserves the restored authoring and redo node");
        Check(f.history.History(true)==MMS_OK&&SameRegistrationKeys(f.first,changed),"transform remains redoable after allocation failure");
    }
    for(bool range:{false,true})for(int mutation=0;mutation<4;++mutation) {
        BoneRegistrationFixture f;TransformSeed(f.first);f.first.m->boneKeys[3].allocated=1;
        plugins::BoneRangeScaleOptions options{10,30,2,true};auto change=TransformPlan(f,range,options);
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("stale bone transform",change,transaction)==MMS_OK,"prepare stale transform fixture");
        if(mutation==0)f.first.m->boneKeys[3].position[0]=999;
        if(mutation==1)f.first.m->maxFrame=123;
        if(mutation==2)bone_scale::SetKey(f.first.raw(),3,Scale(42));
        if(mutation==3)f.first.m->keyVisitMap[3]=1;
        const auto stale=f.first.Digest();
        Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_E_STALE&&
            f.first.Digest()==stale&&f.history.UndoCount()==0,
            "stale key metadata scale and visit guard reject before any transform participant publishes");
    }
}
struct TransformRangeDialog {
    HWND window=nullptr;
    explicit TransformRangeDialog(BoneRegistrationFixture& f) {
        window=CreateWindowExA(0,"STATIC","bone range test",WS_CHILD,0,0,200,100,
            f.app->MainWindow(),nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(window!=nullptr,"hidden bone range dialog created");
        for(int id:{panel::kScaleFromEdit,panel::kScaleToEdit,panel::kScaleRateEdit})
            Check(CreateWindowExA(0,"EDIT","",WS_CHILD,0,0,40,20,window,
                reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,
                "hidden bone range edit created");
        for(int id:{panel::kScaleBoneCheckbox,panel::kScaleMorphCheckbox,panel::kScaleDispIkCheckbox})
            Check(CreateWindowExA(0,"BUTTON","family",WS_CHILD|BS_CHECKBOX,0,0,40,20,window,
                reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,
                "hidden bone range family checkbox created");
        Text("10","30","2");CheckDlgButton(window,panel::kScaleBoneCheckbox,BST_CHECKED);
    }
    ~TransformRangeDialog(){DestroyWindow(window);}
    void Text(const char* start,const char* end,const char* scale) {
        SetDlgItemTextA(window,panel::kScaleFromEdit,start);SetDlgItemTextA(window,panel::kScaleToEdit,end);
        SetDlgItemTextA(window,panel::kScaleRateEdit,scale);
    }
};
void TransformCommandRoutes() {
    {
        BoneRegistrationFixture f;TransformSeed(f.first);TransformRangeDialog dialog(f);
        const auto before=f.first.Digest();
        Check(!plugins::TryDeleteBoundKeys(f.app.get(),true)&&
            !plugins::TryScaleBoundBoneRange(f.app.get(),dialog.window)&&f.first.Digest()==before,
            "unbound transform helpers preserve legacy command ownership without invoking renderer paths");
    }
    {
        BoneRegistrationFixture f;TransformSeed(f.first);plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.first.m->boneKeys[3].allocated=1;const auto ring=PoseRingDigest(f.first);
        CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
        Check(f.first.m->boneKeys[3].frame==0&&f.history.UndoCount()==1&&f.app->SceneModified()==1&&
            f.app->RecordedFrameCountByte1()==1&&PoseRingDigest(f.first)==ring&&f.SelectionUnchanged(),
            "actual delete button publishes one bound history node before dirty UI metadata");
        Check(f.history.History(false)==MMS_OK,"undo button-route deletion before private keyboard selection");
        f.first.m->boneKeys[3].allocated=0;f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;
        const auto noMarks=f.first.Digest();CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
        Check(f.first.Digest()==noMarks&&f.history.UndoCount()==0&&f.history.RedoCount()==1&&f.app->SceneModified()==0,
            "delete button without explicit marks preserves redo and scene dirty flags");
        f.app->CurrentFrame()=20;const auto keyboardBefore=RegistrationKeys(f.first);
        Check(plugins::TryDeleteBoundKeys(f.app.get(),true)&&f.first.m->boneKeys[3].frame==0&&
            f.history.UndoCount()==1&&f.history.RedoCount()==0,
            "bound keyboard route deletes selected-bone current-frame keys in one transaction");
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,keyboardBefore)&&
            f.first.m->boneKeys[3].frame==20&&f.first.m->boneKeys[3].allocated==0,
            "keyboard deletion undo restores the real unmarked before state instead of synthetic marks");
    }
    {
        BoneRegistrationFixture f;TransformSeed(f.first);TransformRangeDialog dialog(f);
        plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.first.m->morphKeys[0].allocated=2;f.first.m->displayKeys[0].allocated=3;
        const auto ring=PoseRingDigest(f.first);ApplyFrameRangeScale(dialog.window);
        Check(f.history.UndoCount()==1&&PoseRingDigest(f.first)==ring&&f.app->FrameRangeStartFrame()==10&&
            f.app->SceneModified()==1&&f.first.m->morphKeys[0].allocated==0&&f.SelectionUnchanged(),
            "actual range dialog publishes one compound bound record and refreshes range metadata without a renderer");
        Check(f.history.History(false)==MMS_OK,"undo range dialog before no-op and invalid input checks");
        f.app->FrameRangeStartFrame()=123;f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;
        const auto before=f.first.Digest();
        const std::array<std::array<const char*,3>,11> inputs={{{"10","30","1"},{"10","30","nan"},
            {"10","30","inf"},{"10","30","2junk"},{"10","30","0"},{"10","30","-2"},
            {"10","30","1e300"},{"10.5","30","2"},{"30","10","2"},{"","30","2"},
            {"10","30","22222222222222222222222222222222222222222222222222222222222222222"}}};
        for(const auto& values:inputs) {
            dialog.Text(values[0],values[1],values[2]);ApplyFrameRangeScale(dialog.window);
            Check(f.first.Digest()==before&&f.history.UndoCount()==0&&f.history.RedoCount()==1&&
                f.app->FrameRangeStartFrame()==123&&f.app->SceneModified()==0&&f.app->RecordedFrameCountByte1()==0,
                "range parsing no-op and overflow rejection preserve redo authoring and UI metadata");
        }
        dialog.Text("10","30","1");
        for(int family:{panel::kScaleMorphCheckbox,panel::kScaleDispIkCheckbox}) {
            CheckDlgButton(dialog.window,family,BST_CHECKED);ApplyFrameRangeScale(dialog.window);
            CheckDlgButton(dialog.window,family,BST_UNCHECKED);
            Check(f.first.Digest()==before&&f.history.RedoCount()==1&&f.app->SceneModified()==0,
                "mixed-family identity range preserves redo and all selection marks");
        }
        dialog.Text("10","30","2");
        Check(f.history.Enter()==MMS_OK,"reserve journal before bound range busy check");
        ApplyFrameRangeScale(dialog.window);f.history.Leave();
        Check(f.first.Digest()==before&&f.history.RedoCount()==1,"busy range route never falls through to legacy mutation");
        for(int gate=0;gate<5;++gate) {
            if(gate==0)f.app->CameraMode()=1;if(gate==1)f.app->PlaybackActive()=1;
            if(gate==2)f.app->FrameStepPlayback()=1;if(gate==3)f.app->state.recordPlaybackStartPending=1;
            if(gate==4)f.app->SelectedModelSlot()=255;
            ApplyFrameRangeScale(dialog.window);CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
            f.app->CameraMode()=f.app->PlaybackActive()=f.app->FrameStepPlayback()=0;
            f.app->state.recordPlaybackStartPending=0;f.app->SelectedModelSlot()=0;
            Check(f.first.Digest()==before&&f.history.RedoCount()==1&&f.app->SceneModified()==0,
                "bound deletion and range gates preserve live state redo and scene dirty flags");
        }
    }
}
void TransformCommandFailure() {
    for(bool range:{false,true}) {
        BoneRegistrationFixture f;TransformSeed(f.first);TransformRangeDialog dialog(f);
        plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.Commit(panel::kReadoutPosXEdit,"31");Check(f.history.History(false)==MMS_OK,"seed redo for real transform fault sweep");
        f.first.m->boneKeys[3].allocated=1;f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;
        f.app->FrameRangeStartFrame()=123;const auto before=f.first.Digest();
        unsigned failures=0;bool committed=false;
        for(long allowance=0;allowance<512&&!committed;++allowance) {
            failAfter=allowance;
            if(range)ApplyFrameRangeScale(dialog.window);else CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
            failAfter=-1;
            if(f.history.UndoCount()){committed=true;break;}
            ++failures;Check(f.first.Digest()==before&&f.history.RedoCount()==1&&f.app->SceneModified()==0&&
                f.app->RecordedFrameCountByte1()==0&&f.app->FrameRangeStartFrame()==123&&f.SelectionUnchanged(),
                "real transform allocation failure preserves authoring redo range metadata dirty flags and selection");
        }
        Check(committed&&failures>10&&f.history.UndoCount()==1&&f.history.RedoCount()==0&&f.app->SceneModified()==1,
            "real transform fault sweep branches common redo exactly once on success");
    }
}
}
void NativeBoneTransformChecks() {
    TransformProgress("deletion legacy parity");TransformDeletionParity();
    TransformProgress("range legacy parity");TransformRangeParity();
    TransformProgress("range bone type rules");TransformRangeBoneTypes();
    TransformProgress("free curves and overwritten ring");TransformFreeCurveAndRing();
    TransformProgress("directions and cross-model history");TransformDirectionalAndCrossModel();
    TransformProgress("no-op malformed chains and overflow");TransformNoOpAndRejections();
    TransformProgress("allocation failures and stale candidates");TransformFailureAndStale();
    TransformProgress("actual command routes");TransformCommandRoutes();
    TransformProgress("actual command failures");TransformCommandFailure();
}
