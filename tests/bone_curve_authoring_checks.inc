// Include after Fixture/Check/failAfter; call BoneCurveAuthoringChecks().
#include "../src/plugins/scene_resource_budget.hpp"
#include <stdexcept>
namespace {
using CurveAuthoring=bone_curve::AuthoringState;
using CurveRestore=bone_curve::AuthoringRestore;
bone_curve::Tracks OwnedCurveTracks(double value) {
    bone_curve::Tracks tracks;
    for(int channel=0;channel<9;++channel) {
        animation_curve::Key first,last;last.frame=20;
        first.tangent=last.tangent=animation_curve::Tangent::Free;
        first.value=last.value=channel>=6?1:0;
        if(channel==0){last.value=value;first.outSlope=.2;last.inSlope=.1;first.outWeight=.2;last.inWeight=.4;}
        tracks[channel].keys={first,last};
    }
    return tracks;
}
void WriteOwnedCurve(Fixture& f,int bone,double value) {
    std::string error;Check(bone_curve::Write(f.raw(),bone,OwnedCurveTracks(value),error),"production free curve created for authoring snapshot");
}
std::uint64_t CurveStorageBytes(const unsigned char* model) {
    plugins::resource_budget::Collector resources;bone_curve::MeasureResources(model,resources);return resources.Result().cpuBytes;
}
bool PublishCurveNoAllocation(CurveRestore& restore) {
    failAfter=0;const bool result=restore.Publish();failAfter=-1;return result;
}
void BoneCurveAuthoringIsolationChecks() {
    Fixture f,other;WriteOwnedCurve(f,0,10);WriteOwnedCurve(f,1,30);WriteOwnedCurve(other,0,70);
    for(auto& bone:f.bones)for(int axis=0;axis<4;++axis)bone.matInit[axis*5]=1;
    bone_scale::SetPose(f.raw(),0,Scale(2));bone_scale::ApplyTransforms(f.raw());
    auto full=CurveAuthoring::Capture(f.raw()),otherBefore=CurveAuthoring::Capture(other.raw());
    const auto bytes=full->RetainedBytes();auto withoutFirst=full->Clone();
    Check(withoutFirst->Equals(*full)&&bytes>9*2*sizeof(animation_curve::Key),"authoring clone retains tracks and charges actual key storage");
    Check(withoutFirst->Invalidate(0)&&!withoutFirst->Invalidate(0)&&!withoutFirst->Invalidate(-1)&&!withoutFirst->Invalidate(2),
        "private invalidation removes only a specified existing bone");
    Check(!withoutFirst->Equals(*full)&&withoutFirst->RetainedBytes()<bytes&&CurveAuthoring::Capture(f.raw())->Equals(*full),
        "private invalidation reduces retained payload without touching live authoring");
    const auto native=f.Digest(),otherNative=other.Digest();
    auto candidate=CurveRestore::Prepare(f.raw(),*withoutFirst);
    for(unsigned ring=0;ring<30;++ring)bone_curve::CaptureUndo(f.raw(),ring);
    Check(candidate->IsCurrent()&&CurveAuthoring::Capture(f.raw())->Equals(*full)&&CurveAuthoring::Capture(f.raw())->RetainedBytes()==bytes,
        "legacy ring writes do not change authoring snapshots retained bytes or candidate freshness");
    Check(PublishCurveNoAllocation(*candidate)&&!PublishCurveNoAllocation(*candidate),"curve authoring publication allocates nothing and consumes once");
    Check(f.Digest()==native&&other.Digest()==otherNative&&CurveAuthoring::Capture(other.raw())->Equals(*otherBefore)&&
        bone_scale::Pose(f.raw(),0)==Scale(2)&&f.bones[0].matInit[0]==2,
        "side-table publication preserves native keys scale pose render cache and other model authoring");
    bone_curve::Tracks read;
    Check(!bone_curve::Read(f.raw(),0,read)&&bone_curve::Read(f.raw(),1,read)&&read[0].keys.back().value==30,
        "published invalidation preserves the other bone's real free curve");
    bone_curve::Undo(f.raw(),7);
    Check(CurveAuthoring::Capture(f.raw())->Equals(*full)&&bone_curve::Read(f.raw(),0,read),"old undo ring remains usable after independent authoring publication");
    bone_curve::Redo(f.raw(),7);Check(CurveAuthoring::Capture(f.raw())->Equals(*withoutFirst),"old redo retains its independently captured authoring");
    auto restore=CurveRestore::Prepare(f.raw(),*full);Check(PublishCurveNoAllocation(*restore),"full owned curve authoring restores without allocation");
    bone_curve::Redo(f.raw(),7);Check(CurveAuthoring::Capture(f.raw())->Equals(*withoutFirst),"owned restore does not overwrite legacy redo payload");
    auto stale=CurveRestore::Prepare(f.raw(),*full);WriteOwnedCurve(f,1,45);
    const auto changed=f.Digest();auto changedAuthor=CurveAuthoring::Capture(f.raw());
    Check(!stale->IsCurrent()&&!PublishCurveNoAllocation(*stale)&&f.Digest()==changed&&CurveAuthoring::Capture(f.raw())->Equals(*changedAuthor),
        "changed side-table tracks reject stale publication without partial mutation");
}
void BoneCurveAuthoringMissingChecks() {
    Fixture f;WriteOwnedCurve(f,0,10);auto full=CurveAuthoring::Capture(f.raw());
    bone_curve::Forget(f.raw());auto empty=CurveAuthoring::Capture(f.raw());const auto native=f.Digest();
    Check(CurveStorageBytes(f.raw())==0,"missing curve state has no physical storage");
    {
        auto cancelled=CurveRestore::Prepare(f.raw(),*full);
        Check(cancelled->IsCurrent()&&CurveStorageBytes(f.raw())==0,"missing-state prepare keeps all allocation private");
    }
    Check(CurveStorageBytes(f.raw())==0&&CurveAuthoring::Capture(f.raw())->Equals(*empty)&&f.Digest()==native,
        "cancelling prepared insertion leaves missing storage and native model untouched");
    auto insert=CurveRestore::Prepare(f.raw(),*full);
    Check(PublishCurveNoAllocation(*insert)&&CurveAuthoring::Capture(f.raw())->Equals(*full),"prepared map node restores previously missing curves without allocation");
    bone_curve::CaptureUndo(f.raw(),3);
    auto clear=CurveRestore::Prepare(f.raw(),*empty);Check(PublishCurveNoAllocation(*clear),"restore missing logical authoring clears only current curves");
    Check(CurveStorageBytes(f.raw())>0&&CurveAuthoring::Capture(f.raw())->Equals(*empty),"empty authoring equals missing state while preserving existing ring storage");
    bone_curve::Undo(f.raw(),3);Check(CurveAuthoring::Capture(f.raw())->Equals(*full),"legacy ring survives restoring a previously missing authoring state");
    bone_curve::Forget(f.raw());auto noOp=CurveRestore::Prepare(f.raw(),*empty);
    Check(PublishCurveNoAllocation(*noOp)&&CurveStorageBytes(f.raw())==0,"empty-to-missing publication does not create storage");
    // Storage is independent of logical authoring. A legacy undo capture may
    // create empty storage after prepare; publishing must preserve that ring.
    auto populated=CurveRestore::Prepare(f.raw(),*full);bone_curve::CaptureUndo(f.raw(),4);
    Check(populated->IsCurrent()&&PublishCurveNoAllocation(*populated),"new empty ring storage does not invalidate prepared authoring insertion");
    bone_curve::Undo(f.raw(),4);Check(CurveAuthoring::Capture(f.raw())->Equals(*empty),"ring created during preparation remains intact");
    auto removed=CurveRestore::Prepare(f.raw(),*full);bone_curve::Forget(f.raw());
    Check(removed->IsCurrent()&&PublishCurveNoAllocation(*removed),"prebuilt node also handles empty storage removed before publication");
    Check(f.Digest()==native,"all missing-storage transitions leave native key pool and ring bytes unchanged");
}
void BoneCurveAuthoringIdentityChecks() {
    Fixture f,other;WriteOwnedCurve(f,0,10);auto original=CurveAuthoring::Capture(f.raw());
    auto invalidated=original->Clone();invalidated->Invalidate(0);
    auto restore=CurveRestore::Prepare(f.raw(),*invalidated);
    // Key edits deliberately precede side-table publication in the host. No
    // Read/Has call is permitted here: those APIs would prune the stale hash.
    f.m->boneKeys[0].allocated=0;f.m->boneKeys[0].position[0]=42;
    // Write rebuilds position and allocation flags from the tracks, but keeps
    // the keyed physics flag. That flag gives the later equal-track rewrite
    // a genuinely different native hash (allocated is excluded from Hash).
    f.m->boneKeys[0].physicsDisabled=1;
    const auto native=f.Digest();auto staleHash=CurveAuthoring::Capture(f.raw());
    Check(staleHash->Equals(*original)&&restore->IsCurrent(),"capture and freshness do not prune or recompute stored native hashes after key edits");
    Check(PublishCurveNoAllocation(*restore)&&f.Digest()==native&&CurveAuthoring::Capture(f.raw())->Equals(*invalidated),
        "host can publish native keys before its curve invalidation participant");
    auto bringBack=CurveRestore::Prepare(f.raw(),*original);Check(PublishCurveNoAllocation(*bringBack),"stored hash can be restored independently of current native keys");
    WriteOwnedCurve(f,0,10);auto newHash=CurveAuthoring::Capture(f.raw());
    Check(!newHash->Equals(*original),"equal tracks with different stored native hashes are different authoring states");
    auto pending=CurveRestore::Prepare(f.raw(),*invalidated);
    auto* bones=f.m->boneTable;f.m->boneTable=other.bones.data();
    Check(!pending->IsCurrent()&&!PublishCurveNoAllocation(*pending),"changed bone table identity rejects prepared curve publication");
    const auto retained=CurveStorageBytes(f.raw());auto wrongOwner=CurveAuthoring::Capture(f.raw());
    Check(CurveStorageBytes(f.raw())==retained,"capture with stale stored identity does not erase live storage");
    f.m->boneTable=bones;
    Check(!wrongOwner->Equals(*newHash)&&pending->IsCurrent(),"snapshot owns bone table metadata as well as curve data");
    --f.m->boneCount;Check(!pending->IsCurrent(),"changed bone count rejects prepared curve publication");++f.m->boneCount;
    bool refused=false;try{CurveRestore::Prepare(other.raw(),*original);}catch(const std::invalid_argument&){refused=true;}
    Check(refused,"curve authoring snapshot cannot restore into a different model");
    refused=false;try{CurveAuthoring::Capture(nullptr);}catch(const std::invalid_argument&){refused=true;}
    Check(refused,"null authoring model is rejected before capture");
    // Pool identity and layout validation belongs to the parent transaction;
    // a side-table-only candidate must not become stale because capacity grew.
    Check(mdl::BoneKeyPoolEnsure(*f.m,64)&&pending->IsCurrent(),"curve candidate does not pin native pool storage or recompute native hashes");
}
void BoneCurveAuthoringFailureChecks() {
    Fixture f;WriteOwnedCurve(f,0,10);WriteOwnedCurve(f,1,20);
    auto original=CurveAuthoring::Capture(f.raw());const auto native=f.Digest();
    for(bool cloning:{false,true}) {
        bool succeeded=false;unsigned failures=0;
        for(long limit=0;limit<512;++limit) {
            std::unique_ptr<CurveAuthoring> captured;bool memory=false;
            failAfter=limit;try{captured=cloning?original->Clone():CurveAuthoring::Capture(f.raw());}catch(const std::bad_alloc&){memory=true;}failAfter=-1;
            Check(f.Digest()==native&&CurveAuthoring::Capture(f.raw())->Equals(*original),"capture or clone allocation failure leaves authored curves and native model intact");
            if(captured){Check(captured->Equals(*original),"successful owned curve capture is exact");succeeded=true;break;}
            ++failures;Check(memory,"capture and clone report allocation failure");
        }
        Check(succeeded&&failures>9,"capture and clone allocation sweeps reach individual track vectors");
    }
    bone_curve::Forget(f.raw());auto empty=CurveAuthoring::Capture(f.raw());bool succeeded=false;unsigned failures=0;
    for(long limit=0;limit<512;++limit) {
        std::unique_ptr<CurveRestore> candidate;bool memory=false;
        failAfter=limit;try{candidate=CurveRestore::Prepare(f.raw(),*original);}catch(const std::bad_alloc&){memory=true;}failAfter=-1;
        Check(CurveStorageBytes(f.raw())==0&&CurveAuthoring::Capture(f.raw())->Equals(*empty)&&f.Digest()==native,
            "restore preparation allocation failure never creates live storage or mutates native data");
        if(candidate){Check(PublishCurveNoAllocation(*candidate)&&CurveAuthoring::Capture(f.raw())->Equals(*original),"first fully prepared candidate restores under allocation failure");succeeded=true;break;}
        ++failures;Check(memory,"restore reports private allocation failure");
    }
    Check(succeeded&&failures>20,"restore allocation sweep reaches private side data and map node preparation");
}
}
void BoneCurveAuthoringChecks() {
    BoneCurveAuthoringIsolationChecks();BoneCurveAuthoringMissingChecks();
    BoneCurveAuthoringIdentityChecks();BoneCurveAuthoringFailureChecks();
}
