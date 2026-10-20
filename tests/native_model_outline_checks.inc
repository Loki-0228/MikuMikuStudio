// Include after ObjectCommandFixture and the native plugin composition helpers.
#include "../src/plugins/native_model_outline.hpp"
namespace model_outline_checks {
using namespace plugins;
bool Same(float a,float b){return !std::memcmp(&a,&b,sizeof a);}
std::shared_ptr<HistoryChange> Plan(ObjectCommandFixture& f,unsigned slot,float value){
    NativeModelOutlinePlan plan;std::shared_ptr<HistoryChange> change;
    Check(plan.Begin(*f.app,slot)==MMS_OK&&plan.Set(value)==MMS_OK&&plan.Take(change)==MMS_OK,"prepare one privately owned model outline field");
    return change;
}
void Apply(ObjectCommandFixture& f,unsigned slot,float value){
    auto change=Plan(f,slot,value);Check(bool(change),"outline value creates a substantive candidate");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("model outline",change,token,SharedHistory::NativeIntent::Adjustment)==MMS_OK,"reserve outline before publication");
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(result==MMS_OK,"outline publication is allocation-free");
}
void ScalarsAndModes(){
    for(const auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})for(unsigned slot:{0u,1u}){
        std::printf("outline scalars mode=%u slot=%u\n",unsigned(mode),slot);
        ObjectCommandFixture f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"outline recording mode");
        auto* target=mdl::Mdl(f.app->ModelSlot(slot));auto* other=mdl::Mdl(f.app->ModelSlot(1-slot));target->edgeScale=.625f;
        const auto otherValue=other->edgeScale;const auto physics=f.app->PhysicsResetPending();const auto frame=f.app->CurrentFrame();
        for(float value:{0.f,.01f,.29f,.57f,1.f,2.f,-1.25f,3.5f,(std::numeric_limits<float>::max)()}){
            const auto before=target->edgeScale;Apply(f,slot,value);
            Check(Same(target->edgeScale,value)&&Same(other->edgeScale,otherValue)&&f.app->CurrentFrame()==frame&&
                f.app->PhysicsResetPending()==physics&&!f.history.LastPublicationRequiresPhysicsReset(),"outline scalar preserves other model frame and physics state");
            if(mode==SharedHistory::RecordingMode::AllOperations){f.app->SelectedModelSlot()=std::uint8_t(1-slot);
                Check(f.history.History(false)==MMS_OK&&Same(target->edgeScale,before)&&f.history.History(true)==MMS_OK&&Same(target->edgeScale,value),
                    "outline undo and redo target the stable model independently of current selection");}
            else Check(!f.history.UndoCount(),"explicit-only applies outline adjustments without recording steps");
        }
        NativeModelOutlinePlan noop;std::shared_ptr<HistoryChange> empty;
        Check(noop.Begin(*f.app,slot)==MMS_OK&&noop.Set(target->edgeScale)==MMS_OK&&!noop.Changed()&&noop.Take(empty)==MMS_OK&&!empty,
            "bit-identical model outline produces no candidate");
        for(float invalid:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}){
            NativeModelOutlinePlan bad;Check(bad.Begin(*f.app,slot)==MMS_OK&&bad.Set(invalid)==MMS_E_ARGUMENT&&Same(target->edgeScale,(std::numeric_limits<float>::max)()),
                "non-finite outline values never publish");}
    }
}
void PreviewAndCancel(){
    ObjectCommandFixture f;NativePoseTestBinding binding(*f.app,f.history);auto* target=mdl::Mdl(f.app->ModelSlot(0));target->edgeScale=.625f;
    Apply(f,0,.75f);Check(f.history.History(false)==MMS_OK,"seed outline redo");const auto redo=f.history.RedoCount();
    for(bool restore:{false,true}){
        NativeModelOutlinePlan plan;std::shared_ptr<HistoryChange> change;std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(plan.Begin(*f.app,0)==MMS_OK&&plan.Set(.9f)==MMS_OK&&plan.Take(change)==MMS_OK&&
            f.history.PrepareNative("preview outline",change,token,SharedHistory::NativeIntent::Adjustment)==MMS_OK,"reserve complete outline preview before changing the live field");
        failAfter=0;const auto a=plan.Preview(1.25f),b=plan.Preview(1.75f);failAfter=-1;
        Check(a==MMS_OK&&b==MMS_OK&&Same(target->edgeScale,1.75f),"repeated outline preview updates allocate nothing");
        if(restore){Check(plan.Preview(.625f)==MMS_OK&&!plan.Changed(),"dragging back restores the exact original bits without a node");}
        Check(plan.Cancel()&&Same(target->edgeScale,.625f),"cancel restores only the owned model outline preview");token.reset();
        Check(!f.history.UndoCount()&&f.history.RedoCount()==redo&&!f.app->SceneModified(),"cancelled outline gesture preserves redo and dirty flags");
    }
    {
        NativeModelOutlinePlan plan;std::shared_ptr<HistoryChange> change;std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(plan.Begin(*f.app,0)==MMS_OK&&plan.Set(.9f)==MMS_OK&&plan.Take(change)==MMS_OK&&
            f.history.PrepareNative("last preview wins",change,token,SharedHistory::NativeIntent::Adjustment)==MMS_OK&&
            plan.Preview(1.25f)==MMS_OK&&plan.Preview(1.75f)==MMS_OK,"prepare multi-message outline gesture");
        failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
        Check(result==MMS_OK&&Same(target->edgeScale,1.75f)&&f.history.UndoCount()==1,"one gesture records its last preview rather than the first requested value");
        Check(!plan.Cancel()&&Same(target->edgeScale,1.75f)&&f.history.History(false)==MMS_OK&&Same(target->edgeScale,.625f)&&
            f.history.History(true)==MMS_OK&&Same(target->edgeScale,1.75f),"destroying the plan cannot roll back a published record; undo redo retain final preview");
    }
    {
        NativeModelOutlinePlan plan;std::shared_ptr<HistoryChange> change;std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(plan.Begin(*f.app,0)==MMS_OK&&plan.Set(1.25f)==MMS_OK&&plan.Take(change)==MMS_OK&&
            f.history.PrepareNative("foreign author",change,token,SharedHistory::NativeIntent::Adjustment)==MMS_OK&&plan.Preview(1.25f)==MMS_OK,"start owned outline preview before a foreign author");
        target->edgeScale=1.5f;Check(!plan.Cancel()&&Same(target->edgeScale,1.5f),"outline cancellation cannot overwrite a foreign same-field author");token.reset();
    }
}
void StaleAndBudget(){
    ObjectCommandFixture f;NativePoseTestBinding binding(*f.app,f.history);auto* target=mdl::Mdl(f.app->ModelSlot(0));target->edgeScale=.625f;
    {NativeModelOutlinePlan plan;std::shared_ptr<HistoryChange> change;Check(plan.Begin(*f.app,0)==MMS_OK,"capture outline before source change");
        target->edgeScale=.875f;Check(plan.Set(1.25f)==MMS_E_STALE&&plan.Take(change)==MMS_E_STALE&&!change&&Same(target->edgeScale,.875f),"stale source cannot be silently captured as a new before value");target->edgeScale=.625f;}
    {auto change=Plan(f,0,1.25f);std::unique_ptr<PreparedHistoryChange> token;
        Check(change->Prepare(true,token)==MMS_OK&&token->IsCurrent(),"prepare directly checked outline candidate");
        target->edgeScale=.875f;Check(!token->IsCurrent(),"candidate detects source mutation before publish");target->edgeScale=.625f;}
    {NativeModelOutlinePlan plan;Check(plan.Begin(*f.app,0)==MMS_OK,"capture outline before transient history replacement");
        {NativePoseTestBinding nested(*f.app,f.history);}Check(plan.Set(1.25f)==MMS_E_STALE,"same-address binding replacement invalidates an old outline draft");}
    auto change=Plan(f,0,1.25f);const auto charge=change->RetainedBytes();
    Check(charge>sizeof(float)*2&&f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,charge-1)==MMS_OK,"outline record budgets retained owner bookkeeping");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("small outline budget",change,token,SharedHistory::NativeIntent::Adjustment)==MMS_E_MEMORY&&!token&&Same(target->edgeScale,.625f),"small outline budget refuses before modifying the model");
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,256ull*1024*1024)==MMS_OK,"restore model outline budget");
    bool success=false;unsigned failures=0;
    for(long limit=0;limit<256&&!success;++limit){NativeModelOutlinePlan plan;std::shared_ptr<HistoryChange> candidate;
        failAfter=limit;auto status=plan.Begin(*f.app,0);if(!status)status=plan.Set(1.25f);if(!status)status=plan.Take(candidate);
        if(!status)status=Boundary([&] {return f.history.PrepareNative("outline allocation",candidate,token,SharedHistory::NativeIntent::Adjustment);});failAfter=-1;
        Check(Same(target->edgeScale,.625f)&&!f.history.UndoCount(),"all outline preparation allocation failures preserve live field and journal");
        if(!status){success=true;token.reset();}else{++failures;Check(status==MMS_E_MEMORY&&!token,"outline preparation failures retain memory error and release reservation");}}
    Check(success&&failures>=4,"outline allocation sweep reaches full private success");
}
void DeletionAndLastSlot(){
    ObjectCommandFixture f;NativePoseTestBinding binding(*f.app,f.history);auto* target=mdl::Mdl(f.app->ModelSlot(0));target->edgeScale=.625f;
    const auto id=SceneObjectIdentity(target).id;Apply(f,0,1.25f);
    {ObjectModalProbe probe(f,true);f.Command(true);Check(probe.seen==1&&!f.app->ModelSlot(0),"delete the already-edited outline model through actual native confirmation");}
    Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0)==reinterpret_cast<unsigned char*>(target)&&
        SceneObjectIdentity(target).id.lo==id.lo&&SceneObjectIdentity(target).id.hi==id.hi,"undo deletion restores original stable outline target");
    Check(f.history.History(false)==MMS_OK&&Same(target->edgeScale,.625f)&&f.history.History(true)==MMS_OK&&Same(target->edgeScale,1.25f),"outline history permits a restored target with a new object generation");
    Check(f.history.History(true)==MMS_OK&&!f.app->ModelSlot(0)&&f.history.History(false)==MMS_OK,"redo deletion and restore keep outline holder lifetime valid");
    Check(f.history.Reset()==MMS_OK,"drop records before last-slot object move");f.history.Collect();
    auto* last=f.app->ModelSlot(1);f.app->ModelSlot(254)=last;f.app->ModelSlot(1)=nullptr;
    Apply(f,254,1.75f);f.app->SelectedModelSlot()=0;
    Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK&&Same(mdl::Mdl(last)->edgeScale,1.75f),"last legal model slot supports independent outline replay");
    Check(f.history.Reset()==MMS_OK,"release last-slot outline holders");f.history.Collect();f.app->ModelSlot(1)=last;f.app->ModelSlot(254)=nullptr;
}
}
void NativeModelOutlineChecks(){
    std::puts("outline: scalars");model_outline_checks::ScalarsAndModes();
    std::puts("outline: preview and cancellation");model_outline_checks::PreviewAndCancel();
    std::puts("outline: stale and budget");model_outline_checks::StaleAndBudget();
    std::puts("outline: deletion and last slot");model_outline_checks::DeletionAndLastSlot();
}
