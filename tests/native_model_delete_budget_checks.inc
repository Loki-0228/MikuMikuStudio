// Include after native_model_delete_target_checks.inc; parent registers the
// entry point. These are real owned native pools, not budget-only fake children.
#include "../src/plugins/scene_object_history.hpp"
#include "mikumikustudio/frame_limits.hpp"
namespace model_delete_budget_checks {
using namespace model_delete_target_checks;
constexpr std::uint64_t fixtureAllowance=256ull*1024;

struct Source {
    ModelKeyState keys;
    Authors authors;
    Live live;
    std::uint64_t digest,peer,ring;
    bool storage;
    int frame;
    explicit Source(ModelKeyFixture& f):keys(f.first),authors(f),live(f),digest(f.first.Digest()),
        peer(f.second.Digest()),ring(PoseRingDigest(f.first)),storage(bone_scale::HasPoseStorage(f.first.raw())),
        frame(f.app->CurrentFrame()){}
    void CheckUnchanged(ModelKeyFixture& f) const {
        Check(keys.Matches(f.first)&&authors.Matches(f)&&live.Matches(f)&&digest==f.first.Digest()&&
            peer==f.second.Digest()&&ring==PoseRingDigest(f.first)&&frame==f.app->CurrentFrame()&&
            storage==bone_scale::HasPoseStorage(f.first.raw())&&!f.history.UndoCount()&&!f.history.RedoCount(),
            "budget failure/cancellation preserves every source including absence of scale storage");
    }
};
std::uint64_t Measure(ModelKeyFixture& f,const std::shared_ptr<Change>& change,bool forward) {
    const Source before(f);std::uint64_t remaining=fixtureAllowance;
    std::unique_ptr<Prepared> prepared;
    Check(change->PrepareSignedBudgeted(forward,remaining,prepared)==MMS_OK&&prepared&&prepared->IsCurrent(),
        "real native candidate fits a small fixture allowance");
    Check(remaining<fixtureAllowance&&remaining>0,"positive preparation reports a nonzero bounded charge");
    const auto charge=fixtureAllowance-remaining;
    prepared.reset();before.CheckUnchanged(f);return charge;
}
void Refuse(ModelKeyFixture& f,const std::shared_ptr<Change>& change,bool forward,std::uint64_t limit) {
    const Source before(f);auto remaining=limit;std::unique_ptr<Prepared> prepared;
    Check(change->PrepareSignedBudgeted(forward,remaining,prepared)==MMS_E_MEMORY&&!prepared&&remaining==limit,
        "insufficient shared allowance returns null and restores the caller's exact balance");
    before.CheckUnchanged(f);
}
void DirectionsAndBounds() {
    for(bool automatic:{false,true})for(bool privateSelection:{false,true})
    for(auto family:{Families::All,Families::Bone,Families::Display,Families::Morph})
    for(int frame:{frame_limits::minimum,-17,0,35,frame_limits::maximum}) {
        ModelKeyFixture f,oracle;f.Automatic(automatic);oracle.Automatic(automatic);
        const auto selection=Seed(f,family,privateSelection),other=Seed(oracle,family,privateSelection);
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        auto change=Plan(f,privateSelection?&selection:nullptr,automatic);
        auto expectedChange=Plan(oracle,privateSelection?&other:nullptr,automatic);
        for(bool forward:{true,false,true}) {
            const auto charge=Measure(f,change,forward);
            for(auto limit:{std::uint64_t(0),std::uint64_t(1),charge/2,charge-1})Refuse(f,change,forward,limit);
            std::uint64_t remaining=charge+37;std::unique_ptr<Prepared> prepared,expected;
            Check(change->PrepareSignedBudgeted(forward,remaining,prepared)==MMS_OK&&prepared&&remaining==37,
                "successful positive half leaves exactly the reserved negative-half balance");
            Check(expectedChange->PrepareSigned(forward,expected)==MMS_OK&&expected,
                "unbudgeted production target provides an independent preparation oracle");
            Check(prepared->BoneZeroTargets().size()==expected->BoneZeroTargets().size()&&
                prepared->MorphZeroTargets().size()==expected->MorphZeroTargets().size(),
                "budgeting preserves every zero-target participant");
            for(std::size_t i=0;i<prepared->BoneZeroTargets().size();++i) {
                const auto& a=prepared->BoneZeroTargets()[i];const auto& b=expected->BoneZeroTargets()[i];
                Check(a.bone==b.bone&&a.zeroPose.position==b.zeroPose.position&&a.zeroPose.rotation==b.zeroPose.rotation&&
                    a.zeroPose.scale==b.zeroPose.scale&&a.zeroPose.writePose==b.zeroPose.writePose,
                    "budgeted zero sampling keeps source/target selector and authored-scale semantics");
            }
            for(std::size_t i=0;i<prepared->MorphZeroTargets().size();++i) {
                const auto& a=prepared->MorphZeroTargets()[i];const auto& b=expected->MorphZeroTargets()[i];
                Check(a.morph==b.morph&&a.zeroWeight==b.zeroWeight,"budgeted authored morph zero matches production");
            }
            const auto* a=prepared->DisplayZeroTarget();const auto* b=expected->DisplayZeroTarget();
            Check(bool(a)==bool(b),"budgeting preserves display-target presence");
            if(a)Check(a->visible==b->visible&&a->ik==b->ik&&a->selectors.size()==b->selectors.size()&&
                (a->selectors.empty()||!std::memcmp(a->selectors.data(),b->selectors.data(),a->selectors.size()*sizeof(mdl::BoneOrderEntry))),
                "budgeted registered zero display and parent windows match production");
            Publish(*prepared);Publish(*expected);PayloadsMatch(f,oracle);
            Check(Live(oracle).Matches(f),"budgeting preserves supported direction and real-playhead pose behavior");
        }
    }
}
void CumulativeChildren() {
    ModelKeyFixture f;Seed(f,Families::All,false);f.app->CurrentFrame()=-17;
    auto aggregate=Plan(f);const Source before(f);
    std::shared_ptr<plugins::DisplayKeyDeletion> display;
    std::shared_ptr<plugins::BoneKeyDeletion> bone;
    std::shared_ptr<plugins::MorphKeyDeletion> morph;
    Check(plugins::PlanSignedDisplayKeyDeletion(*f.app,0,display,true,false)==MMS_OK&&display&&
        plugins::PlanSignedBoneKeyDeletion(*f.app,0,false,bone,false,true,display)==MMS_OK&&bone&&
        plugins::PlanSignedMorphKeyDeletion(*f.app,0,morph,true)==MMS_OK&&morph,
        "cumulative check uses the aggregate's actual three native families");
    std::unique_ptr<plugins::PreparedDisplayKeyDeletion> d;
    std::unique_ptr<plugins::PreparedBoneKeyDeletion> b;
    std::unique_ptr<plugins::PreparedMorphKeyDeletion> m;
    std::uint64_t remaining=fixtureAllowance;
    Check(display->PrepareSignedBudgeted(true,remaining,d)==MMS_OK&&d,"display consumes shared balance first");
    const auto displayBytes=fixtureAllowance-remaining,afterDisplay=remaining;
    Check(bone->PrepareSignedBudgeted(true,&d->ZeroTarget().selectors,remaining,b)==MMS_OK&&b,
        "bone consumes the reduced balance and actual display zero target");
    const auto boneBytes=afterDisplay-remaining,afterBone=remaining;
    Check(morph->PrepareSignedBudgeted(true,remaining,m)==MMS_OK&&m,"morph consumes the remaining balance");
    const auto morphBytes=afterBone-remaining,total=displayBytes+boneBytes+morphBytes;
    const auto largest=(std::max)({displayBytes,boneBytes,morphBytes});
    Check(total==fixtureAllowance-remaining&&total>largest,"child charges accumulate rather than reuse a full allowance");
    m.reset();b.reset();d.reset();before.CheckUnchanged(f);
    const auto aggregateBytes=Measure(f,aggregate,true);
    Check(aggregateBytes>total,"aggregate also charges Layout, peer alias ledger and its own candidate");
    Refuse(f,aggregate,true,largest);Refuse(f,aggregate,true,total);
    remaining=aggregateBytes;std::unique_ptr<Prepared> exact;
    Check(aggregate->PrepareSignedBudgeted(true,remaining,exact)==MMS_OK&&exact&&remaining==0,
        "exact aggregate bound admits all children together");
    exact.reset();before.CheckUnchanged(f);
}
void EmptyScaleAndOom() {
    for(bool emptyScale:{false,true}) {
        ModelKeyFixture f;
        plugins::ModelKeySelection selection;
        if(emptyScale) {
            f.Marks(false,false,false);selection.bones={2};selection.morphs={1};selection.displays={1};
            Check(!bone_scale::HasPoseStorage(f.first.raw()),"no-scale fixture begins with absent side storage");
        } else selection=Seed(f,Families::All,true);
        f.app->CurrentFrame()=-17;auto change=Plan(f,&selection);const Source before(f);
        const auto charge=Measure(f,change,true);
        // Metadata is prepared last, after the bone's reversible lease exists.
        Refuse(f,change,true,charge-1);
        unsigned failures=0;bool completed=false;
        for(long allocation=0;allocation<4096;++allocation) {
            std::uint64_t remaining=charge;std::unique_ptr<Prepared> prepared;
            failAfter=allocation;
            const auto result=change->PrepareSignedBudgeted(true,remaining,prepared);
            failAfter=-1;
            if(result==MMS_OK) {
                Check(prepared&&remaining==0,"OOM sweep reaches exact-bound preparation");
                if(emptyScale)Check(bone_scale::HasPoseStorage(f.first.raw()),"negative candidate holds reversible empty pose lease");
                prepared.reset();before.CheckUnchanged(f);completed=true;break;
            }
            Check(result==MMS_E_MEMORY&&!prepared&&remaining==charge,
                "every injected allocation failure restores output and entire shared allowance");
            before.CheckUnchanged(f);++failures;
        }
        Check(completed&&failures>8,"failure sweep covers multiple native families and authoring/lease stages");
        std::uint64_t remaining=charge;std::unique_ptr<Prepared> prepared;
        Check(change->PrepareSignedBudgeted(true,remaining,prepared)==MMS_OK&&prepared,"prepare nonempty output for failure contract");
        remaining=0;
        Check(change->PrepareSignedBudgeted(true,remaining,prepared)==MMS_E_MEMORY&&!prepared&&!remaining,
            "budget refusal clears a previously populated output");
        before.CheckUnchanged(f);
    }
}
void RetainedOwnersAndReadDependency() {
    for(auto family:{Families::All,Families::Bone}) {
        ModelKeyFixture f;const auto selection=Seed(f,family,true);f.app->CurrentFrame()=35;
        auto change=Plan(f,&selection);const auto charge=Measure(f,change,true);
        std::uint64_t remaining=charge;std::unique_ptr<Prepared> prepared;
        Check(change->PrepareSignedBudgeted(true,remaining,prepared)==MMS_OK&&prepared,"prepare selector/metadata owner visitor");
        struct Owners {std::array<std::shared_ptr<plugins::RetainedSceneObject>,8> values;std::size_t size=0;} owners;
        prepared->VisitRetainedModels(&owners,[](void* context,const std::shared_ptr<plugins::RetainedSceneObject>& owner) {
            auto& state=*static_cast<Owners*>(context);
            if(std::none_of(state.values.begin(),state.values.begin()+state.size,[&](const auto& p){return p==owner;})) {
                Check(state.size<state.values.size(),"bounded owner visitor fixture");state.values[state.size++]=owner;
            }
        });
        Check(owners.size==2,"visitor covers the edited model and registered external parent including unchangedDisplay");
        plugins::resource_budget::Collector collector;
        for(std::size_t i=0;i<owners.size;++i)
            Check(owners.values[i]->MeasureBookkeeping(collector)==MMS_OK,"one collector measures only participating holders");
        for(std::size_t i=0;i<owners.size;++i)
            Check(owners.values[i]->CoversBookkeeping(collector),"shared bookkeeping ledger covers each visited owner");
        auto& pair=f.first.m->displayKeys[0].selectorStates[0];const auto old=pair;pair.boneIndex=1;
        Check(!prepared->IsCurrent(),"unedited source-zero display dependency remains guarded after budget admission");
        pair=old;
        auto* oldPeer=f.second.m->displayKeys[3].selectorStates;
        f.second.m->displayKeys[3].selectorStates=f.first.m->displayKeys[0].selectorStates;
        const bool stale=!prepared->IsCurrent();
        f.second.m->displayKeys[3].selectorStates=oldPeer;
        Check(stale,"budgeted Layout still guards peer aliases in unused payload slots");
    }
}
}
void NativeModelDeleteBudgetChecks() {
    model_delete_budget_checks::DirectionsAndBounds();
    model_delete_budget_checks::CumulativeChildren();
    model_delete_budget_checks::EmptyScaleAndOom();
    model_delete_budget_checks::RetainedOwnersAndReadDependency();
}
