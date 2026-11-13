// Include after all native history fixtures; call NativeBoneCopyAuthoringChecks().
// Uses Fixture, Check, failAfter and the parent's allocation injector. Production
// 420 / ModelCopySnapshot integration is exercised separately by the parent.
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/native_edit_history.hpp"
#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
namespace bone_copy_authoring_checks {
namespace bc = bone_curve;
namespace ac = animation_curve;
using Copies = std::vector<bc::CopyAuthoring>;

struct Model : Fixture {
    Model() {
        bc::Forget(raw());
        for(auto& bone:bones) {
            bone.type=mdl::BoneType::RotateMove;bone.slotIndex=-1;
            for(int i=0;i<16;++i)bone.matInit[i]=i%5==0?1.f:0.f;
        }
        Entry(3,2,9);
    }
    ~Model() { bc::Forget(raw()); }
    unsigned Slot(int bone,unsigned frame) const {
        unsigned slot=unsigned(bone);
        for(std::size_t hop=0;hop<mdl::BoneKeyCapacity(m->boneKeys);++hop) {
            if(m->boneKeys[slot].frame==frame)return slot;
            if(!m->boneKeys[slot].next)break;
            slot=m->boneKeys[slot].next;
        }
        Check(false,"copy author fixture native frame exists");return 0;
    }
};
bc::Tracks Tracks(double offset=0) {
    bc::Tracks tracks;
    for(unsigned lane=0;lane<9;++lane)for(int index=0;index<3;++index) {
        ac::Key key;key.frame=index==0?0:index==1?10+int(lane%3)*5:60;
        key.value=lane>=6?1+lane*.1+index*.25:offset+lane*.1234567890123+index*(lane==4?540.000012:2.125);
        key.inSlope=.001*(lane+1);key.outSlope=-.002*(index+1);
        key.inWeight=.13+.01*lane;key.outWeight=.21+.01*index;
        key.interpolation=static_cast<ac::Interpolation>((lane+index)%3);
        key.tangent=static_cast<ac::Tangent>((lane+index)%5);
        tracks[lane].keys.push_back(key);
    }
    return tracks;
}
bool Same(const bc::Tracks& a,const bc::Tracks& b) {
    for(unsigned lane=0;lane<9;++lane) {
        if(a[lane].discrete!=b[lane].discrete||a[lane].keys.size()!=b[lane].keys.size())return false;
        for(std::size_t i=0;i<a[lane].keys.size();++i) {
            const auto& x=a[lane].keys[i];const auto& y=b[lane].keys[i];
            if(x.frame!=y.frame||x.value!=y.value||x.inSlope!=y.inSlope||x.outSlope!=y.outSlope||
                x.inWeight!=y.inWeight||x.outWeight!=y.outWeight||x.interpolation!=y.interpolation||x.tangent!=y.tangent)return false;
        }
    }
    return true;
}
bool Same(const Copies& a,const Copies& b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(a[i].bone!=b[i].bone||!Same(a[i].tracks,b[i].tracks))return false;
    return true;
}
void Write(Model& f,int bone,double offset=0) {
    std::string error;Check(bc::Write(f.raw(),bone,Tracks(offset),error),"seed complete sparse nine-lane author");
}
Copies Capture(Model& f,const std::vector<int>& indices) {
    Copies out;std::string error="stale error";
    Check(bc::CaptureCopyAuthoring(f.raw(),indices,out,error)&&error.empty(),"capture owned selected bone authors");return out;
}
// An empty-step history observer compares the complete stored author map,
// baselines, registrations, undo/redo maps and valid bits without publishing.
auto Observe(Model& f) { return bc::PrepareHistory(f.raw(),{}); }
struct Native {
    std::unique_ptr<mdl::ModelRecord> record;
    std::array<mdl::BoneRecord,2> bones;
    std::array<unsigned char,2> selected,dirty;
    std::vector<mdl::BoneKey> keys;
    std::array<mdl::BonePoseSnapshot,2> undoPose;
    mdl::BoneKeySnapshot undoKey;
    bone_scale::Snapshot scale;
    bool applied;
    std::unique_ptr<native_history::SideState> scaleHistory;
    explicit Native(Model& f):record(std::make_unique<mdl::ModelRecord>(*f.m)),bones(f.bones),
        selected(f.selected),dirty(f.dirty),keys(f.m->boneKeys,f.m->boneKeys+mdl::BoneKeyCapacity(f.m->boneKeys)),
        scale(bone_scale::Capture(f.raw())),applied(bone_scale::TransformsApplied(f.raw())),
        scaleHistory(bone_scale::PrepareHistory(f.raw(),{})) {
        std::memcpy(undoPose.data(),f.m->undoRings[0].slots[3].bonePose,sizeof(undoPose));
        std::memcpy(&undoKey,f.m->undoRings[0].slots[3].auxiliaryPose,sizeof(undoKey));
    }
    void Expect(Model& f,bool sameScaleRevision=true) const {
        const auto now=bone_scale::Capture(f.raw());
        Check(!std::memcmp(record.get(),f.m.get(),sizeof(*record))&&
            !std::memcmp(bones.data(),f.bones.data(),sizeof(bones))&&selected==f.selected&&dirty==f.dirty,
            "copy author read preserves native metadata flags poses matrices selection and ring headers");
        Check(keys.size()==mdl::BoneKeyCapacity(f.m->boneKeys)&&
            !std::memcmp(keys.data(),f.m->boneKeys,keys.size()*sizeof(mdl::BoneKey))&&
            !std::memcmp(undoPose.data(),f.m->undoRings[0].slots[3].bonePose,sizeof(undoPose))&&
            !std::memcmp(&undoKey,f.m->undoRings[0].slots[3].auxiliaryPose,sizeof(undoKey)),
            "copy author read preserves the complete native pool and allocated undo payloads");
        Check(scale.pose==now.pose&&scale.keys==now.keys&&scale.curves==now.curves&&
            applied==bone_scale::TransformsApplied(f.raw()),"copy author read preserves scale and applied-transform cache flag");
        Check(!sameScaleRevision||scaleHistory->IsCurrent(),"copy author read preserves scale revision caches and complete scale undo rings");
    }
};
void Refused(Model& f,const std::vector<int>& indices) {
    Copies out{{1,Tracks(91)}};std::string error;
    Check(!bc::CaptureCopyAuthoring(f.raw(),indices,out,error)&&out.empty()&&!error.empty(),
        "invalid copy author source refuses the entire result and clears old output");
}

void FieldsOrderAndHistory() {
    Model f;Write(f,0,-7);Write(f,1,-17);
    const auto older=bc::AuthoringState::Capture(f.raw());bc::CaptureUndo(f.raw(),7);
    Write(f,0,7);Write(f,1,17);
    bc::Undo(f.raw(),7);bc::Redo(f.raw(),7); // Populate both side-table directions.
    f.bones[0].trans[0]=81;f.selected={0,1};f.dirty={7,3};
    Check(bone_scale::SetPose(f.raw(),0,Scale(2)),"seed independent dirty pose scale");
    bone_scale::ApplyTransforms(f.raw());
    for(std::size_t i=0;i<mdl::BoneKeyCapacity(f.m->boneKeys);++i)f.m->boneKeys[i].allocated=0;
    f.m->boneKeys[f.Slot(1,15)].allocated=1;
    const Native native(f);const auto author=bc::AuthoringState::Capture(f.raw());auto observer=Observe(f);
    const auto out=Capture(f,{1,0,1,0});
    Check(out.size()==2&&out[0].bone==1&&out[1].bone==0&&Same(out[0].tracks,Tracks(17))&&Same(out[1].tracks,Tracks(7)),
        "first occurrence ordering deduplicates indices and preserves every field of all nine lanes");
    Check(out[0].tracks[0].keys.front().frame==0&&out[0].tracks[0].keys.back().frame==60&&
        out[0].tracks[0].keys[1].frame==10&&out[0].tracks[1].keys[1].frame==15,
        "one selected native frame does not crop retime normalize or complete sparse authored lanes");
    native.Expect(f);Check(observer->IsCurrent()&&bc::AuthoringState::Capture(f.raw())->Equals(*author),
        "successful capture preserves original author baselines and both old rings");
    bc::Undo(f.raw(),7);Check(bc::AuthoringState::Capture(f.raw())->Equals(*older),"original author undo still restores its exact snapshot");
    bc::Redo(f.raw(),7);Check(bc::AuthoringState::Capture(f.raw())->Equals(*author),"original author redo still restores its exact snapshot");
    native.Expect(f);
}
void TemporaryNativeEdit() {
    Model f;Write(f,0);Write(f,1,17);auto observer=Observe(f);
    const auto original=Capture(f,{0,1});const Native native(f);
    const auto slot=f.Slot(0,60);const auto key=f.m->boneKeys[slot];
    const auto scale=bone_scale::Key(f.raw(),int(slot));const auto controls=bone_scale::KeyCurve(f.raw(),int(slot));
    f.m->boneKeys[slot].position[0]=123;
    auto changedScale=scale;for(unsigned c=0;c<3;++c)changedScale.v[c]*=1.25f;
    bone_scale::SetKey(f.raw(),int(slot),changedScale);
    bone_scale::SetKeyCurve(f.raw(),int(slot),{11,29,93,121});
    const Native changed(f);const auto out=Capture(f,{0,1});bc::Tracks canonical;
    Check(bc::ReadDetached(f.raw(),0,canonical)&&Same(out[0].tracks,canonical)&&out[0].tracks[0].keys.back().value==123&&
        !Same(out[0].tracks,original[0].tracks)&&Same(out[1].tracks,original[1].tracks),
        "private reconciliation includes pending native position and scale edits only for their author");
    changed.Expect(f);Check(observer->IsCurrent(),"temporary native edits do not become the live author baseline or ring contents");
    f.m->boneKeys[slot]=key;bone_scale::SetKey(f.raw(),int(slot),scale);bone_scale::SetKeyCurve(f.raw(),int(slot),controls);
    Check(Same(Capture(f,{0,1}),original)&&observer->IsCurrent(),"restoring temporary native bytes recovers exact original authored handles");
    // The fixture's SetKey/SetKeyCurve writes advance scale's revision even
    // when restoring the original values; each capture above is checked alone.
    native.Expect(f,false);
}
void EmptyMissingAndIndices() {
    Model f;const Native native(f);
    const std::vector<int> empty,selected{1,0,1};
    auto* pool=f.m->boneKeys;
    for(bool storedEmpty:{false,true}) {
        if(storedEmpty)bc::CaptureUndo(f.raw(),5);
        auto absent=Observe(f);f.m->boneKeys=reinterpret_cast<mdl::BoneKey*>(std::uintptr_t(1));
        for(const auto* indices:{&empty,&selected}) {
            Copies out{{0,Tracks()}};std::string error="previous";bool success=false,threw=false;
            failAfter=0;
            try {success=bc::CaptureCopyAuthoring(f.raw(),*indices,out,error);}catch(...){threw=true;}
            const auto remaining=failAfter;failAfter=-1;
            Check(success&&!threw&&out.empty()&&error.empty()&&remaining==0&&absent->IsCurrent(),
                "empty and missing-author reads allocate nothing create no registry and never read even a poisoned native pool");
        }
        f.m->boneKeys=pool;native.Expect(f);
    }
    Refused(f,{-1});Refused(f,{0,2});Refused(f,{(std::numeric_limits<int>::max)()});
    Copies out{{0,Tracks()}};std::string error;
    Check(!bc::CaptureCopyAuthoring(nullptr,{},out,error)&&out.empty()&&!error.empty(),"null owner refuses and clears output");
    f.m->boneCount=0;Check(Capture(f,{}).empty(),"zero-bone model accepts empty selection");Refused(f,{0});f.m->boneCount=2;
    Write(f,0);auto author=Observe(f);
    f.m->boneKeys[1].next=999;const Native badMissing(f);
    Check(Capture(f,{1,1}).empty(),"selected bone without an author does not inspect its invalid native chain");
    Check(Capture(f,{1,0,1}).size()==1&&author->IsCurrent(),"missing author is omitted without synthesizing native defaults");badMissing.Expect(f);
    Refused(f,{0,1,-1});Check(author->IsCurrent(),"late invalid index cannot publish an earlier valid author");
}
void IdentityAndDeferred() {
    Model f,other;Write(f,0);auto observer=Observe(f);const auto original=Capture(f,{0});const Native native(f);
    auto* bones=f.m->boneTable;auto* pool=f.m->boneKeys;
    for(auto* stale:std::array<mdl::BoneRecord*,2>{other.bones.data(),nullptr}) {
        f.m->boneTable=stale;f.m->boneKeys=reinterpret_cast<mdl::BoneKey*>(std::uintptr_t(1));
        Check(Capture(f,{0,1}).empty()&&observer->IsCurrent(),"stale author identity is omitted without pruning or reading native keys");
    }
    f.m->boneTable=bones;f.m->boneKeys=pool;
    Check(Same(Capture(f,{0}),original)&&observer->IsCurrent(),"restored identity finds the original unpruned author and rings");native.Expect(f);
    {
        bc::RangeRetime deferred(f.raw(),0,60,1);Check(deferred.Ready(),"open real deferred range owner");
        for(bool stale:{false,true}) {
            f.m->boneTable=stale?other.bones.data():bones;
            const Native pending(f);
            Refused(f,{0});Refused(f,{1});Refused(f,{});
            Check(observer->IsCurrent(),"deferred refusal preserves author and rings even with stale identity or empty selection");pending.Expect(f);
        }
        f.m->boneTable=bones;Check(deferred.Complete(),"rejected capture leaves deferred edit completable");
    }
    Check(Same(Capture(f,{0}),original)&&observer->IsCurrent(),"completed deferred edit permits complete capture again");native.Expect(f);
}
void BadChainsAndTracks() {
    for(unsigned kind=0;kind<8;++kind) {
        Model f;Write(f,0);Write(f,1,17);auto observer=Observe(f);
        auto& root=f.m->boneKeys[1];auto& end=f.m->boneKeys[f.Slot(1,60)];
        if(kind==0)root.next=unsigned(mdl::BoneKeyCapacity(f.m->boneKeys));
        if(kind==1)end.next=root.next;
        if(kind==2)end.previous=1;
        if(kind==3)root.frame=1;
        if(kind==4)std::fill(std::begin(end.rotation),std::end(end.rotation),0.f);
        if(kind==5)end.position[0]=std::numeric_limits<float>::quiet_NaN();
        if(kind==6)end.interpolation[0]=255;
        if(kind==7)bone_scale::SetKeyCurve(f.raw(),int(f.Slot(1,60)),{255,20,107,107});
        const Native bad(f);
        const auto unrelated=Capture(f,{0});Check(unrelated.size()==1&&Same(unrelated[0].tracks,Tracks()),
            "unselected malformed authored native chain does not affect selected good bone");
        Refused(f,{0,1});bad.Expect(f);Check(observer->IsCurrent(),"selected chain rejection publishes no partial result or live repair");
    }
    for(bool scaleFailure:{false,true}) {
        Model f;Write(f,0);auto observer=Observe(f);
        const auto slot=f.Slot(0,60);
        if(scaleFailure)bone_scale::SetKey(f.raw(),int(slot),Scale(2000));
        else f.m->boneKeys[slot].position[0]=1e13f;
        const Native bad(f);Refused(f,{0});bad.Expect(f);
        Check(observer->IsCurrent(),"invalid reconciled track limits fail without replacing original live author");
        if(!scaleFailure) {
            bc::Tracks invalid;std::string error;
            Check(bc::Read(f.raw(),0,invalid)&&!bc::ValidateFreeTracks(invalid,error),
                "legacy reconciliation can expose a stored out-of-policy author for validation coverage");
            auto invalidObserver=Observe(f);Refused(f,{0});
            Check(invalidObserver->IsCurrent(),"invalid stored tracks fail even when native hash already matches");bad.Expect(f);
        }
    }
}
void ReconciliationFailure() {
    Model f;auto tracks=Tracks();auto& lane=tracks[0];
    for(auto& key:lane.keys){key.interpolation=ac::Interpolation::Bezier;key.tangent=ac::Tangent::Free;key.inSlope=-2.2;key.outSlope=3.7;}
    lane.keys[0].outWeight=.0001;lane.keys[1].inWeight=.1;
    auto split=lane;std::size_t at=0;std::string error;
    Check(ac::InsertPreservingShape(split,9,at,error)==ac::InsertResult::Rejected,
        "constrained weighted handle fixture cannot represent a native insertion at frame nine");
    Check(bc::Write(f.raw(),0,tracks,error),"seed valid constrained free bone author");auto observer=Observe(f);
    const unsigned next=f.m->boneKeys[0].next;auto inserted=f.m->boneKeys[0];
    inserted.frame=9;inserted.previous=0;inserted.next=next;inserted.position[0]=23;
    f.m->boneKeys[31]=inserted;f.m->boneKeys[0].next=31;f.m->boneKeys[next].previous=31;
    const Native native(f);Refused(f,{0});native.Expect(f);
    Check(observer->IsCurrent(),"failed private shape-preserving reconciliation never repairs the live author");
}
void AllocationFailures(bool reconcile) {
    Model f;Write(f,0,-7);Write(f,1,-17);bc::CaptureUndo(f.raw(),7);Write(f,0,7);Write(f,1,17);
    bc::Undo(f.raw(),7);bc::Redo(f.raw(),7);auto observer=Observe(f);
    if(reconcile)f.m->boneKeys[f.Slot(0,60)].position[0]=123;
    const Native native(f);const auto author=bc::AuthoringState::Capture(f.raw());
    const std::vector<int> indices{1,0,1,0};const auto expected=Capture(f,indices);
    Copies measured;std::string error;constexpr long allowance=1000000;
    failAfter=allowance;const bool success=bc::CaptureCopyAuthoring(f.raw(),indices,measured,error);
    const auto spent=allowance-failAfter;failAfter=-1;
    Check(success&&spent>0&&spent<allowance&&Same(measured,expected),"measure complete successful copy-author allocation path");
    for(long limit=0;limit<spent;++limit) {
        Copies out{{0,Tracks(91)}};std::string detail;bool memory=false,other=false,returned=false;
        failAfter=limit;
        try {returned=bc::CaptureCopyAuthoring(f.raw(),indices,out,detail);}
        catch(const std::bad_alloc&){memory=true;}catch(...){other=true;}
        const auto remaining=failAfter;failAfter=-1;
        Check(memory&&!other&&!returned&&remaining==0&&out.empty(),"every measured OOM propagates bad_alloc and clears prepopulated output");
        native.Expect(f);Check(observer->IsCurrent()&&bc::AuthoringState::Capture(f.raw())->Equals(*author),
            "every failed allocation preserves author baseline registrations and both undo directions");
    }
    Check(Same(Capture(f,indices),expected)&&observer->IsCurrent(),"capture recovers after every allocation point has failed");native.Expect(f);
}
void IndependentLifetime() {
    Copies held,saved;
    {
        Model f;Write(f,0);Write(f,1,17);held=Capture(f,{1,0});saved=held;
        Write(f,1,999);f.m->boneKeys[0].position[0]=-81;bc::Forget(f.raw());
        Check(Same(held,saved),"held author vectors do not alias live writes native keys or registry storage");
    }
    Check(Same(held,saved),"all nine author lanes survive complete source model destruction");
    held[0].tracks[0].keys[0].value+=1;
    Check(!Same(held,saved)&&Same(saved[0].tracks,Tracks(17)),"independent result copies own separate key storage");
}
}
void NativeBoneCopyAuthoringChecks() {
    using namespace bone_copy_authoring_checks;
    FieldsOrderAndHistory();TemporaryNativeEdit();EmptyMissingAndIndices();IdentityAndDeferred();
    BadChainsAndTracks();ReconciliationFailure();AllocationFailures(false);AllocationFailures(true);IndependentLifetime();
}
}
