#include "../src/plugins/native_morph_keys.hpp"
namespace mikudancestudio {
bool RegisterMorphKeyFromRecord(unsigned char*,const unsigned char*,int);
int ResetMorphKeyCursor(unsigned char*);
void ResetDisplayKeyCursor(unsigned char*);
}
namespace {
struct MorphPasteFixture : MorphKeyFixture {
    std::array<mdl::MorphClipboardRecord,5> clipboard{};
    MorphPasteFixture() {
        MorphKeySeed(first);MorphKeySeed(second);
        for(unsigned i=0;i<clipboard.size();++i) {
            auto& rec=clipboard[i];std::memcpy(rec.name,first.m->morphs[i%2].name,20);
            rec.frame=i==4?0:i*7;rec.value=.13f+float(i)*.11f;
        }
        app->CurrentFrame()=0;app->MorphClipboard()=clipboard.data();app->ClipboardCounts()={};app->ClipboardCounts().morphs=unsigned(clipboard.size());
        first.m->searchCursor=second.m->searchCursor=7;
        first.m->boneKeys[0].allocated=second.m->boneKeys[0].allocated=3;
        first.m->displayKeys[0].allocated=second.m->displayKeys[0].allocated=5;
    }
    ~MorphPasteFixture(){app->MorphClipboard()=nullptr;app->ClipboardCounts()={};}
    std::shared_ptr<plugins::HistoryChange> Plan(){std::shared_ptr<plugins::HistoryChange> out;
        Check(plugins::PlanMorphKeyPaste(*app,0,out)==MMS_OK&&out,"morph-only clipboard produces one prepared record");return out;}
    void Oracle(){
        for(std::size_t i=0;i<mdl::BoneKeyCapacity(second.m->boneKeys);++i)second.m->boneKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(second.m->morphKeys);++i)second.m->morphKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(second.m->displayKeys);++i)second.m->displayKeys[i].allocated=0;
        ResetMorphKeyCursor(second.raw());
        for(unsigned i=0;i<app->ClipboardCounts().morphs;++i)Check(RegisterMorphKeyFromRecord(second.raw(),reinterpret_cast<const unsigned char*>(&clipboard[i]),app->CurrentFrame()),"actual old morph registrar accepts clipboard record");
        ResetDisplayKeyCursor(second.raw());
    }
};
void NativeMorphPasteChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}) {
        MorphPasteFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        std::memcpy(f.firstMorphs[2].name,f.firstMorphs[1].name,20);
        for(auto& source:f.clipboard)std::memcpy(source.name,f.firstMorphs[0].name,20);
        Check(f.history.SetMode(mode)==MMS_OK,"configure duplicate-helper morph paste mode");
        const auto before=MorphKeysCopy(f.first);CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
        Check(f.history.UndoCount()==1,"actual morph paste accepts duplicates outside copied destinations");
        const auto after=MorphKeysCopy(f.first);f.clipboard={};f.app->ClipboardCounts()={};
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&
            f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,after),
            "duplicate-helper morph paste owns undo redo after original clipboard is destroyed");
    }
    for(bool afterPreparation:{false,true}) {
        MorphPasteFixture f;
        for(auto& source:f.clipboard)std::memcpy(source.name,f.firstMorphs[0].name,20);
        auto change=f.Plan();std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        if(afterPreparation)Check(f.history.PrepareNative("morph name collision",change,transaction)==MMS_OK,
            "prepare morph transaction before helper name changes");
        std::memcpy(f.firstMorphs[2].name,f.firstMorphs[0].name,20);const auto before=f.first.Digest();
        const auto result=afterPreparation?f.history.CommitNative(std::move(transaction)):
            f.history.PrepareNative("morph name collision",change,transaction);
        Check(result==MMS_E_STALE&&f.first.Digest()==before&&!f.history.UndoCount(),
            "a helper becoming a copied morph destination invalidates both prepare and commit");
    }
    std::puts("  morph paste: legacy registrar parity and explicit modes");std::fflush(stdout);
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int frame:{0,10,25,70}) {
        MorphPasteFixture f;f.app->CurrentFrame()=frame;Check(f.history.SetMode(mode)==MMS_OK,"morph paste history mode set");
        const auto original=MorphKeysCopy(f.first);const auto digest=f.first.Digest();const auto ring=PoseRingDigest(f.first);
        const auto beforeValue=f.first.m->morphs[0].value;auto change=f.Plan();
        Check(f.first.Digest()==digest,"morph paste planning leaves native data unchanged");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("morph paste",change,token)==MMS_OK,"morph paste private reservation succeeds");
        Check(CommitRegistrationNoAllocation(f.history,std::move(token))==MMS_OK,"morph paste publishes without allocations");f.Oracle();
        Check(MorphKeysMatch(f.first,MorphKeysCopy(f.second))&&f.first.m->maxFrame==f.second.m->maxFrame&&f.first.m->searchCursor==f.second.m->searchCursor,"morph paste matches old slot order overwrite append frame zero max cursor");
        Check(f.history.UndoCount()==1&&PoseRingDigest(f.first)==ring&&f.first.m->morphs[0].value==beforeValue,"morph paste records once and preserves dirty weights and old rings");
        f.app->MorphClipboard()=nullptr;f.app->ClipboardCounts()={};
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,original),"morph paste undo restores all old keys without clipboard");
        Check(f.history.History(true)==MMS_OK&&MorphKeysMatch(f.first,MorphKeysCopy(f.second)),"morph paste redo uses retained copied values");
    }
    {
        MorphPasteFixture f;auto change=f.Plan();std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("ABA",change,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK,"commit morph paste ABA fixture");
        std::unique_ptr<plugins::PreparedHistoryChange> oldCandidate;
        Check(change->Prepare(false,oldCandidate)==MMS_OK&&oldCandidate&&oldCandidate->IsCurrent(),"prepare private morph paste undo candidate");
        Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK&&!oldCandidate->IsCurrent(),"morph paste candidate rejects history undo redo ABA despite identical key bytes");
    }
    {
        MorphPasteFixture f;auto change=f.Plan();std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("seed",change,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK&&f.history.History(false)==MMS_OK,"seed redo before unmatched morph paste");
        for(auto& rec:f.clipboard)std::memcpy(rec.name,"unmatched",10);
        const auto digest=f.first.Digest();std::shared_ptr<plugins::HistoryChange> absent;
        Check(plugins::PlanMorphKeyPaste(*f.app,0,absent)==MMS_OK&&!absent&&f.first.Digest()==digest&&f.history.RedoCount()==1,"unmatched morph clipboard preserves marks cursor and redo");
    }
    {
        MorphPasteFixture f;auto change=f.Plan();std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("seed",change,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK&&f.history.History(false)==MMS_OK,"seed redo before equivalent morph paste");
        f.app->ClipboardCounts().morphs=1;f.clipboard[0].frame=0;f.clipboard[0].value=f.first.m->morphKeys[0].value;
        const auto digest=f.first.Digest();std::shared_ptr<plugins::HistoryChange> absent;
        Check(plugins::PlanMorphKeyPaste(*f.app,0,absent)==MMS_OK&&!absent&&f.first.Digest()==digest&&f.history.RedoCount()==1,"equivalent morph paste preserves redo marks and nondefault cursor");
    }
    {
        MorphPasteFixture f;for(auto& rec:f.clipboard)std::memcpy(rec.name,f.first.m->morphs[1].name,20);
        auto change=f.Plan();f.first.m->morphs[0].value=std::numeric_limits<float>::quiet_NaN();
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate&&candidate->IsCurrent(),"unrelated base morph weight change does not stale targeted paste");
    }
    {
        MorphPasteFixture f;std::memcpy(f.first.m->morphs[2].name,f.first.m->morphs[1].name,20);
        std::shared_ptr<plugins::HistoryChange> absent;const auto digest=f.first.Digest();
        Check(plugins::PlanMorphKeyPaste(*f.app,0,absent)==MMS_E_ARGUMENT&&!absent&&f.first.Digest()==digest,"ambiguous target morph names reject before any paste mutation");
    }
    {
        MorphPasteFixture f;unsigned tail=2;
        for(unsigned i=3;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)if(!f.first.m->morphKeys[i].frame){
            auto& key=f.first.m->morphKeys[i];f.first.m->morphKeys[tail].next=i;key.previous=tail;key.frame=100+i;key.value=.2f;tail=i;
        }
        f.app->CurrentFrame()=317;const auto before=MorphKeysCopy(f.first);const auto capacity=mdl::MorphKeyCapacity(f.first.m->morphKeys);
        auto change=f.Plan();std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate&&candidate->IsCurrent()&&mdl::MorphKeyCapacity(f.first.m->morphKeys)==capacity&&MorphKeysMatch(f.first,before),
            "morph paste private growth preparation does not publish live pool");candidate.reset();
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("growth",change,token)==MMS_OK&&CommitRegistrationNoAllocation(f.history,std::move(token))==MMS_OK&&mdl::MorphKeyCapacity(f.first.m->morphKeys)>capacity,
            "morph paste commits privately grown pool once without allocations");
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before)&&f.history.History(true)==MMS_OK,"grown morph paste keys remain reversible");
    }
    {
        MorphPasteFixture f;std::string error;const auto track=MorphAuthoredTrack(f.firstMorphs[1],1);
        Check(morph_curve::Write(f.first.raw(),{track},error),"morph paste installs pristine custom authored tangent fixture");
        f.app->CurrentFrame()=14;f.app->ClipboardCounts().morphs=1;std::memcpy(f.clipboard[0].name,f.firstMorphs[1].name,20);
        f.clipboard[0].frame=0;f.clipboard[0].value=.812345f;
        const auto before=MorphKeysCopy(f.first);auto change=f.Plan();std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("authored",change,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK,"morph paste commits under authored layer");
        animation_curve::Track derived;Check(morph_curve::Read(f.first.raw(),1,derived,error),"normal reader reconciles pasted native changes with pristine authoring");
        f.app->CurrentFrame()=6;Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before),"undo authored morph paste restores original native keys after reconciliation");
        animation_curve::Track restored;Check(morph_curve::Read(f.first.raw(),1,restored,error)&&restored.keys[1].inWeight==.2&&restored.keys[0].outSlope==-.02,
            "morph paste undo preserves pristine custom tangent metadata");
    }
    {
        MorphPasteFixture f;const auto digest=f.first.Digest();std::shared_ptr<plugins::HistoryChange> change;bool success=false;
        for(long limit=0;limit<256;++limit){failAfter=limit;const auto status=plugins::PlanMorphKeyPaste(*f.app,0,change);failAfter=-1;
            Check(f.first.Digest()==digest,"morph paste planning OOM never changes live scene");if(status==MMS_OK){success=bool(change);break;}Check(status==MMS_E_MEMORY&&!change,"morph paste reports planning allocation failure");}
        Check(success,"morph paste planning failure sweep reaches success");success=false;
        for(long limit=0;limit<512;++limit){std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=limit;const auto status=change->Prepare(true,candidate);failAfter=-1;
            Check(f.first.Digest()==digest,"morph paste candidate failure and cancellation leave scene unchanged");if(status==MMS_OK){Check(candidate&&candidate->IsCurrent(),"morph paste candidate is privately current");success=true;break;}Check(status==MMS_E_MEMORY&&!candidate,"morph paste candidate reports allocation failure");}
        Check(success,"morph paste candidate failure sweep reaches success");
        plugins::SharedHistory tiny(f.storage,{},32,1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(tiny.PrepareNative("budget",change,token)==MMS_E_MEMORY&&!token&&f.first.Digest()==digest,"morph paste budget refusal is atomic");
        Check(f.history.PrepareNative("paste",change,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK,"morph paste commits before identity growth");
        const auto identity=mdl::MorphKeyPoolIdentity(f.first.m->morphKeys);
        Check(mdl::MorphKeyPoolEnsure(*f.first.m,mdl::MorphKeyCapacity(f.first.m->morphKeys)+20)&&identity==mdl::MorphKeyPoolIdentity(f.first.m->morphKeys),"morph paste pool grows with persistent identity");
        Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,"morph paste remains undoable after identity growth");
    }
}
}
