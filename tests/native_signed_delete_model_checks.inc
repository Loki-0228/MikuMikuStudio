// Production signed participant through the actual SharedHistory protocol.
#include "../src/plugins/native_signed_key_delete.hpp"
namespace signed_delete_model_checks {
namespace n=negative_model_delete_target_checks;
namespace m=model_delete_target_checks;
using Fixture=n::Fixture;
void Mark(Fixture& f,const plugins::ModelKeySelection& selection){
    for(auto slot:selection.bones)f.first.m->boneKeys[slot].allocated=3;
    for(auto slot:selection.morphs)f.first.m->morphKeys[slot].allocated=3;
    for(auto slot:selection.displays)f.first.m->displayKeys[slot].allocated=3;
}
std::shared_ptr<plugins::HistoryChange> Plan(Fixture& f,bool automatic=false){
    std::shared_ptr<plugins::HistoryChange> change;
    const auto result=plugins::PlanSignedKeyDeletion(*f.app,f.history,automatic,false,change);
    if(result)std::fprintf(stderr,"signed model plan result=%d\n",int(result));
    Check(result==MMS_OK&&change,"production signed model coordinator plans one native participant");
    return change;
}
void Frame(Fixture& f,int frame){n::Owner owner(f.app.get());f.app->CurrentFrame()=frame;SeekModelFrame(f.first.raw(),frame,0);}
void Directions(){
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly})
    for(bool display:{false,true})for(bool automatic:{false,true})for(int initial:{-5,0,11}){
        std::printf("  signed model mode=%d display=%d automatic=%d initial=%d\n",int(mode),int(display),int(automatic),initial);
        Fixture f,oracle;const auto selected=n::Seed(f,display),other=n::Seed(oracle,display);
        Mark(f,selected);Mark(oracle,other);f.Automatic(automatic);oracle.Automatic(automatic);
        Frame(f,initial);Frame(oracle,initial);
        std::shared_ptr<plugins::ModelKeyDeletion> positive;
        std::shared_ptr<negative_frames::SelectedDeletion> negative;
        Check(plugins::PlanSignedModelKeyDeletion(*oracle.app,0,automatic,positive)==MMS_OK&&positive&&
            negative_frames::PlanSignedSelectedDeletion(*oracle.app,negative,true)==MMS_OK&&negative,
            "independent oracle uses previously verified typed positive and negative participants");
        n::Owner owner(f.app.get());plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"signed model coordinator uses actual journal recording mode");
        auto change=Plan(f,automatic);
        for(unsigned step=0;step<3;++step){
            std::printf("    direction step=%u oracle prepare\n",step);
            const bool forward=step!=1;
            const int frame=step==0?initial:step==1?(initial<0?17:-7):(initial<0?-3:9);
            if(step){Frame(f,frame);Frame(oracle,frame);}
            std::unique_ptr<plugins::PreparedModelKeyDeletion> p;
            std::unique_ptr<plugins::PreparedHistoryChange> q;
            Check(positive->PrepareSigned(forward,p)==MMS_OK&&p,"oracle positive direction prepares at the real signed playhead");
            negative_frames::DeletionTargets targets{nullptr,p.get(),0};auto remaining=n::kPreparationBytes;
            Check(negative->PrepareSigned(forward,step==0,targets,remaining,q)==MMS_OK&&q&&p->IsCurrent()&&q->IsCurrent(),
                "oracle negative direction consumes real target zero values");
            std::puts("    oracle publish");p->Publish();q->Publish();q.reset();p.reset();
            if(!step){
                const n::Snapshot before(f);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
                const auto result=f.history.PrepareNative("Delete selected keys",change,token,plugins::SharedHistory::NativeIntent::ExplicitCommit);
                if(result)std::fprintf(stderr,"signed model initial result=%d mode=%d display=%d automatic=%d frame=%d\n",int(result),int(mode),int(display),int(automatic),frame);
                Check(result==MMS_OK&&token&&before.Matches(f),"actual journal prepares both signed halves without writes");
                std::puts("    actual commit");
                failAfter=0;const auto committed=f.history.CommitNative(std::move(token));failAfter=-1;
                Check(committed==MMS_OK,"both signed halves commit without allocation through one journal node");
            }else {std::puts("    actual replay");Check(f.history.History(forward)==MMS_OK,"one actual journal undo/redo replays both signed halves");}
            Check(f.history.UndoCount()==(forward?1u:0u)&&f.history.RedoCount()==(forward?0u:1u),
                "signed model edit has exactly one node in either recording mode");
            m::PayloadsMatch(f,oracle);
            Check(m::Live(oracle).Matches(f)&&f.app->CurrentFrame()==frame&&
                NegativeTestRecord(*f.app,negative_frames::kPmmKey)==NegativeTestRecord(*oracle.app,negative_frames::kPmmKey)&&
                NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==NegativeTestRecord(*oracle.app,negative_frames::kPmmSelectionKey),
                "production coordinator matches independent key author pose parent-window and selection targets in each direction");
        }
    }
}
void CancellationAndBudget(){
    std::puts("  signed model cancellation and total budget");
    Fixture f;Mark(f,n::Seed(f,true));
    n::Owner owner(f.app.get());plugins::NativePoseTestBinding binding(*f.app,f.history);
    auto change=Plan(f);
    const auto tryLimit=[&](std::uint64_t limit){
        Check(f.history.Configure(f.history.Mode(),128,limit)==MMS_OK,"set isolated journal byte limit before preparation");
        const n::Snapshot before(f);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        const auto result=f.history.PrepareNative("Delete selected keys",change,token);
        if(result!=MMS_OK)Check(result==MMS_E_MEMORY&&!token,"shared total allowance rejects without retaining a partial transaction");
        token.reset();Check(before.Matches(f),"whole coordinator budget refusal or cancellation leaves both sides and journal intact");
        return result==MMS_OK;
    };
    std::uint64_t low=1,high=64ull*1024*1024;
    Check(!tryLimit(low)&&tryLimit(high),"real signed coordinator has a finite enforceable total admission threshold");
    while(low+1<high){const auto middle=low+(high-low)/2;if(tryLimit(middle))high=middle;else low=middle;}
    Check(!tryLimit(high-1)&&tryLimit(high),"one-byte-short and exact total allowance exercise complete retained and preparation accounting");
    Check(f.history.Configure(f.history.Mode(),128,64ull*1024*1024)==MMS_OK,"restore isolated fixture allowance for fault sweep");
    const n::Snapshot before(f);bool complete=false;unsigned failures=0;
    for(long allowed=0;allowed<8192;++allowed){
        std::string label="Delete selected keys";
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;failAfter=allowed;
        const auto result=f.history.PrepareNative(std::move(label),change,token);failAfter=-1;
        if(result==MMS_OK){Check(bool(token),"complete coordinator reserves a real journal publication");token.reset();complete=true;Check(before.Matches(f),"successful staged coordinator cancels cleanly");break;}
        Check(result==MMS_E_MEMORY&&!token&&before.Matches(f),"OOM anywhere in whole preparation has no partial positive or negative result");++failures;
    }
    Check(complete&&failures>10,"allocation sweep spans journal, retained ledger and both signed participants");
}
}
void NativeSignedDeleteModelChecks(){
    signed_delete_model_checks::Directions();
    signed_delete_model_checks::CancellationAndBudget();
}
