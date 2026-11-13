// Include after native_global_delete_target_checks.inc. The parent test TU
// calls NativeGlobalDeleteBudgetChecks(); no coordinator or entry gate here.
namespace global_delete_budget_checks {
using namespace global_delete_target_checks;
using Fixture=global_delete_target_checks::Fixture;

Authors ReadAuthors(Fixture& f){
    // Match the ABI host's detached read path. An unbound legacy Read may
    // reconcile the live author and invalidate a captured deletion source.
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    return AuthorsOf(f);
}
bool MatchAuthors(Fixture& f,const Authors& expected){
    const auto actual=ReadAuthors(f);
    for(unsigned i=0;i<actual.size();++i)if(!Same(actual[i],expected[i]))return false;
    return true;
}
struct Snapshot {
    std::uint64_t digest;Keys keys;Authors authors;Poses poses;
    std::array<std::uint32_t,6> cursors;
    std::array<unsigned char,6> active;
    explicit Snapshot(Fixture& f):digest(f.FullDigest()),keys(*f.app),authors(ReadAuthors(f)),
        poses(PosesOf(f)),cursors(Cursors(f)),active(Active(f)){}
    void Unchanged(Fixture& f)const{
        Check(f.FullDigest()==digest&&keys.Same(*f.app)&&MatchAuthors(f,authors)&&
            PosesOf(f)==poses&&Cursors(f)==cursors&&Active(f)==active&&
            !f.history.UndoCount()&&!f.history.RedoCount(),
            "budgeted preparation/cancellation leaves native source, authors, pose, selection and journal intact");
    }
};

std::uint64_t Charge(Change& change,bool forward){
    std::uint64_t remaining=UINT64_MAX;std::unique_ptr<Prepared> candidate;
    const auto result=change.PrepareSignedBudgeted(forward,remaining,candidate);
    if(result!=MMS_OK)std::fprintf(stderr,"budgeted global result=%d direction=%d remainder=%llu\n",int(result),int(forward),static_cast<unsigned long long>(remaining));
    Check(result==MMS_OK&&candidate&&candidate->IsCurrent(),
        "production signed candidate reports its measured preparation debit");
    const auto charge=UINT64_MAX-remaining;
    Check(charge>0&&charge<UINT64_MAX,"preparation charge is finite, positive and independent of granted limit");
    return charge;
}

void SameTargets(const Prepared& a,const Prepared& b){
    Check(a.OwnerApp()==b.OwnerApp()&&a.ZeroTargets().size()==b.ZeroTargets().size(),
        "bounded preparation retains ordinary signed owner and target count");
    Check(a.RetainedObjects()==b.RetainedObjects(),"bounded and ordinary candidates expose the same complete retained holders");
    std::size_t visited=0;failAfter=0;
    a.VisitRetainedOwners(&visited,[](void* context,const std::shared_ptr<plugins::RetainedSceneObject>&){++*static_cast<std::size_t*>(context);});
    failAfter=-1;Check(visited==a.RetainedObjects().size(),"prepared owner visitor enumerates the ledger dependencies without allocating");
    for(std::size_t i=0;i<a.ZeroTargets().size();++i){const auto& x=a.ZeroTargets()[i];const auto& y=b.ZeroTargets()[i];
        Check(x.kind==y.kind&&x.slot==y.slot&&x.object==y.object&&x.zeroPose==y.zeroPose,
            "bounded candidate targets match the existing production PrepareSigned API");
    }
}

void RetimingMeasurement(){
    for(bool authored:{false,true}) {
        Fixture f;Seed(f,authored);const Snapshot before(f);
        curve_host::TrackAuthoring source(*f.app,Kind::Camera);
        std::vector<int> frames;source.RetimingFrames(frames);
        const auto* pool=f.app->CameraKeys();
        for(unsigned slot=0;;slot=pool[slot].next){frames.push_back(int(pool[slot].frame));if(!pool[slot].next)break;}
        std::sort(frames.begin(),frames.end());frames.erase(std::unique(frames.begin(),frames.end()),frames.end());
        std::vector<std::pair<int,int>> mapping;
        for(int frame:frames)mapping.emplace_back(frame,frame);
        curve_host::NativeKeyView view{pool,mdl::KeyPoolCapacity(pool),
            [](const void* context,unsigned slot)noexcept->const void*{return static_cast<const mdl::CameraKey*>(context)+slot;}};
        for(bool forward:{false,true}) {
            std::uint64_t bytes=0;failAfter=0;
            const auto result=curve_host::MeasureGlobalDeletionRetimingPreparation(source,view,mapping,forward,1,bytes);
            failAfter=-1;
            Check(result==MMS_OK&&bytes>0,"retiming measurement allocates nothing even with authored camera metadata");
            before.Unchanged(f);
        }
        auto invalid=mapping;invalid.front().second=-1;std::uint64_t bytes=1234;
        Check(curve_host::MeasureGlobalDeletionRetimingPreparation(source,view,invalid,true,1,bytes)==MMS_E_ARGUMENT&&bytes==1234,
            "deletion sizing rejects a missing native root without changing its output");
        auto missing=view;missing.key=nullptr;failAfter=0;
        const auto rejected=curve_host::MeasureGlobalDeletionRetimingPreparation(source,missing,mapping,true,1,bytes);
        failAfter=-1;
        Check(rejected==MMS_E_ARGUMENT&&bytes==1234,"invalid native view is rejected without allocating an exception message");
        before.Unchanged(f);
    }
}
void ThresholdsAndDirections(){
    for(bool authored:{false,true})for(unsigned deleted:{0u,1u})for(int frame:{-17,7}){
        Fixture f;Seed(f,authored);f.ClearMarks();f.Mark(deleted);f.app->CurrentFrame()=frame;
        auto change=SignedPlan(f);Check(bool(change),"threshold fixture has a real native deletion plan");
        Check(change->RetainedObjects().size()==2,"record exposes both selected accessory holders for resident resource accounting");
        for(bool forward:{true,false,true}){
            std::printf("    authored=%d deleted=%u frame=%d forward=%d\n",int(authored),deleted,frame,int(forward));
            const Snapshot source(f);const auto retained=change->RetainedBytes();
            const auto charge=Charge(*change,forward);source.Unchanged(f);
            for(const auto granted:{std::uint64_t(0),charge-1}){
                std::unique_ptr<Prepared> candidate;
                Check(change->PrepareSigned(forward,candidate)==MMS_OK&&candidate,
                    "seed nonnull output before budget rejection");
                auto remaining=granted;
                Check(change->PrepareSignedBudgeted(forward,remaining,candidate)==MMS_E_MEMORY&&!candidate&&remaining==granted,
                    "zero and one-byte-short budgets reject with null output and unchanged caller remainder");
                source.Unchanged(f);
            }
            for(const auto spare:{std::uint64_t(0),std::uint64_t(1)}){
                auto remaining=charge+spare;std::unique_ptr<Prepared> candidate,ordinary;
                Check(change->PrepareSignedBudgeted(forward,remaining,candidate)==MMS_OK&&candidate&&
                    candidate->IsCurrent()&&remaining==spare,"exact measured threshold and threshold plus one succeed");
                Check(change->PrepareSigned(forward,ordinary)==MMS_OK&&ordinary,"legacy signed preparation stays available without a budget");
                SameTargets(*candidate,*ordinary);candidate.reset();ordinary.reset();source.Unchanged(f);
            }
            Check(change->RetainedBytes()==retained,"candidate preparation does not change retained record charge");
            auto remaining=charge;std::unique_ptr<Prepared> candidate;
            Check(change->PrepareSignedBudgeted(forward,remaining,candidate)==MMS_OK&&candidate,
                "bounded forward/reverse/redo candidate can be prepared after cancellations");
            Publish(*candidate);
        }
    }
}

void SequentialBudgets(){
    Fixture first,second;Seed(first,true);Seed(second,true);first.ClearMarks();second.ClearMarks();
    first.app->LightKeys()[1].selected=1;second.app->AccessoryKeys(8)[1].selected=1;
    first.app->CurrentFrame()=second.app->CurrentFrame()=-13;
    auto a=SignedPlan(first),b=SignedPlan(second);Check(a&&b,"sequential fixtures are real global and accessory participants");
    const Snapshot beforeA(first),beforeB(second);const auto costA=Charge(*a,true),costB=Charge(*b,true);
    Check(costA<UINT64_MAX-costB,"sum of measured participant charges fits");
    auto remaining=costA+costB-1;std::unique_ptr<Prepared> pa,pb;
    Check(a->PrepareSignedBudgeted(true,remaining,pa)==MMS_OK&&pa&&remaining==costB-1,
        "first participant consumes only its own measured share");
    const auto afterFirst=remaining;
    Check(b->PrepareSignedBudgeted(true,remaining,pb)==MMS_E_MEMORY&&!pb&&remaining==afterFirst&&pa->IsCurrent(),
        "second participant cannot reuse original cap or refund the successful first participant");
    pa.reset();beforeA.Unchanged(first);beforeB.Unchanged(second);
    remaining=costA+costB;
    Check(a->PrepareSignedBudgeted(true,remaining,pa)==MMS_OK&&b->PrepareSignedBudgeted(true,remaining,pb)==MMS_OK&&
        pa&&pb&&pa->IsCurrent()&&pb->IsCurrent()&&!remaining,
        "two coexisting production candidates share exactly one additive budget");
    pa.reset();pb.reset();beforeA.Unchanged(first);beforeB.Unchanged(second);

    Fixture all;Seed(all,true);all.ClearMarks();all.Mark(1);all.app->CurrentFrame()=-13;
    auto joined=SignedPlan(all);Check(bool(joined),"joined fixture selects all six native tracks");
    const auto allCost=Charge(*joined,true);Check(allCost>costA&&allCost>costB,"aggregate debit includes every selected family");
    remaining=std::max(costA,costB);const auto granted=remaining;
    Check(joined->PrepareSignedBudgeted(true,remaining,pa)==MMS_E_MEMORY&&!pa&&remaining==granted,
        "families inside one aggregate also cannot each receive the entire caller cap");
}

void PoolGrowth(){
    Fixture f;Seed(f,true);f.ClearMarks();f.Mark(1);f.app->CurrentFrame()=-9;
    auto change=SignedPlan(f);Check(bool(change),"growth fixture plans deletion");const auto before=Charge(*change,true);
    const auto camera=mdl::KeyPoolCapacity(f.app->CameraKeys()),accessory=mdl::KeyPoolCapacity(f.app->AccessoryKeys(8));
    Check(mdl::KeyPoolEnsure(f.app->CameraKeys(),camera+137)&&mdl::KeyPoolEnsure(f.app->AccessoryKeys(8),accessory+91),
        "grow actual native pool capacities without changing identities or linked key counts");
    const auto after=Charge(*change,true);
    const auto minimumGrowth=(mdl::KeyPoolCapacity(f.app->CameraKeys())-camera)*(2*sizeof(mdl::CameraKey)+2)+
        (mdl::KeyPoolCapacity(f.app->AccessoryKeys(8))-accessory)*(2*sizeof(mdl::AccessoryKey)+2);
    Check(after>=before+minimumGrowth,"budget follows both full pool copies and both validation arrays after real growth");
    const Snapshot source(f);auto remaining=before;std::unique_ptr<Prepared> candidate;
    Check(change->PrepareSignedBudgeted(true,remaining,candidate)==MMS_E_MEMORY&&!candidate&&remaining==before,
        "old preparation allowance rejects a grown native pool without changing the source");source.Unchanged(f);
}

void AllocationFailures(){
    for(bool authored:{false,true}){
        Fixture f;Seed(f,authored);f.ClearMarks();f.Mark(0);f.app->CurrentFrame()=-23;
        auto change=SignedPlan(f);Check(bool(change),"OOM fixture includes all globals and both accessories");
        for(bool forward:{true,false,true}){
            const auto charge=Charge(*change,forward);const Snapshot source(f);bool ready=false;unsigned failures=0;
            for(long allowance=0;allowance<8192;++allowance){
                auto remaining=charge;std::unique_ptr<Prepared> candidate;failAfter=allowance;
                const auto result=change->PrepareSignedBudgeted(forward,remaining,candidate);failAfter=-1;
                if(result==MMS_OK){Check(candidate&&candidate->IsCurrent()&&!remaining,
                    "allocation sweep eventually creates a complete bounded production candidate");candidate.reset();ready=true;source.Unchanged(f);break;}
                Check(result==MMS_E_MEMORY&&!candidate&&remaining==charge,
                    "OOM at any preparation stage cancels all partial families and restores caller budget");
                ++failures;source.Unchanged(f);
            }
            Check(ready&&failures>3,"bounded OOM sweep covers multiple native and authoring allocation stages");
            auto remaining=charge;std::unique_ptr<Prepared> candidate;
            Check(change->PrepareSignedBudgeted(forward,remaining,candidate)==MMS_OK&&candidate,
                "the same source remains usable after all allocation cancellations");Publish(*candidate);
        }
    }
}

void ParentsAndStaleSource(){
    Fixture f;auto storage=std::make_unique<std::array<mdl::ModelRecord,2>>();auto& models=*storage;
    std::array<std::array<mdl::BoneRecord,4>,2> bones{};
    for(unsigned i=0;i<2;++i){models[i].boneCount=4;models[i].boneTable=bones[i].data();
        f.app->ModelSlot(3+i)=reinterpret_cast<unsigned char*>(&models[i]);}
    plugins::SceneObjectIdentity(&models[0]);
    f.app->CameraKeys()[0].parentModel=3;f.app->CameraKeys()[0].parentBone=2;
    f.app->AccessoryKeys(8)[0].parentModel=3;f.app->AccessoryKeys(8)[0].parentBone=3;
    Seed(f,true);f.ClearMarks();f.Mark(1);f.app->CurrentFrame()=-11;
    auto change=SignedPlan(f);Check(bool(change),"authored camera/accessory parents participate in the real deletion");
    Check(change->RetainedObjects().size()==3,"record deduplicates the shared camera/accessory parent alongside both accessories");
    auto remaining=Charge(*change,true);std::unique_ptr<Prepared> candidate;
    Check(change->PrepareSignedBudgeted(true,remaining,candidate)==MMS_OK&&candidate,"existing retained parents admit bounded preparation");
    Publish(*candidate);candidate.reset();
    // A new unregistered pose parent after publication exercises acquisition,
    // rather than just copying the holders already captured by the record.
    f.app->CameraParentModel()=4;f.app->CameraParentBone()=3;
    f.app->AccessorySlot(8)->parentModel=4;f.app->AccessorySlot(8)->parentBone=2;
    const Snapshot unregistered(f);MmsObjectRef missing{};
    for(unsigned retry=0;retry<2;++retry){
        remaining=UINT64_MAX;
        Check(change->PrepareSignedBudgeted(false,remaining,candidate)==MMS_E_NOT_READY&&!candidate&&remaining==UINT64_MAX&&
            !plugins::FindSceneObjectIdentity(&models[1],missing),
            "unregistered pose parent is rejected repeatedly before capture without publishing identity or consuming budget");
        unregistered.Unchanged(f);
    }
    plugins::SceneObjectIdentity(&models[1]);
    const Snapshot source(f);const auto charge=Charge(*change,false);remaining=charge-1;
    Check(change->PrepareSignedBudgeted(false,remaining,candidate)==MMS_E_MEMORY&&!candidate&&remaining==charge-1,
        "new pose parent acquisition is admitted within the same measured preparation budget");source.Unchanged(f);
    remaining=charge;
    Check(change->PrepareSignedBudgeted(false,remaining,candidate)==MMS_OK&&candidate&&!remaining,
        "multiple-bone parents are measured rather than skipped");
    Check(candidate->RetainedObjects().size()==4&&change->RetainedObjects().size()==3,
        "candidate exposes newly acquired parent for coordinator accounting without modifying the retained record");
    candidate.reset();bool ready=false;unsigned failures=0;
    for(long allowance=0;allowance<8192;++allowance){remaining=charge;failAfter=allowance;
        const auto result=change->PrepareSignedBudgeted(false,remaining,candidate);failAfter=-1;
        if(result==MMS_OK){Check(candidate&&candidate->IsCurrent()&&!remaining,"new-parent OOM sweep reaches a complete candidate");ready=true;break;}
        Check(result==MMS_E_MEMORY&&!candidate&&remaining==charge,"parent acquisition OOM restores the full caller allowance");
        ++failures;source.Unchanged(f);
    }
    Check(ready&&failures>3,"new parent capture participates in transactional allocation cancellation");
    ++f.app->LightKeys()[2].color[0];const Snapshot mutated(f);remaining=charge;
    Check(change->PrepareSignedBudgeted(false,remaining,candidate)==MMS_E_STALE&&!candidate&&remaining==charge,
        "stale source clears preexisting output without debiting caller budget");mutated.Unchanged(f);
    change.reset();
    for(unsigned i=0;i<2;++i){plugins::ForgetSceneObject(&models[i]);f.app->ModelSlot(3+i)=nullptr;}
}
}

inline void NativeGlobalDeleteBudgetChecks(){
    std::puts("  global budget: retiming measurement");
    global_delete_budget_checks::RetimingMeasurement();
    std::puts("  global budget: direction thresholds");
    global_delete_budget_checks::ThresholdsAndDirections();
    std::puts("  global budget: sequential shares");
    global_delete_budget_checks::SequentialBudgets();
    std::puts("  global budget: pool growth");
    global_delete_budget_checks::PoolGrowth();
    std::puts("  global budget: allocation failures");
    global_delete_budget_checks::AllocationFailures();
    std::puts("  global budget: parents and stale source");
    global_delete_budget_checks::ParentsAndStaleSource();
}
