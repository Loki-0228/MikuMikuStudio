#include "../src/plugins/native_model_paste.hpp"
namespace {
bool MorphPastePartData(const Fixture& fixture,const std::vector<mdl::MorphKey>& expected){
    if(mdl::MorphKeyCapacity(fixture.m->morphKeys)<expected.size())return false;
    for(std::size_t i=0;i<expected.size();++i){auto a=fixture.m->morphKeys[i],b=expected[i];a.allocated=b.allocated=0;if(std::memcmp(&a,&b,sizeof(a)))return false;}return true;
}
void NativeMorphPastePartChecks(){
    std::puts("  morph paste part: mixed source ownership and metadata isolation");std::fflush(stdout);
    {
        MorphPasteFixture f;f.app->ClipboardCounts().bones=f.app->ClipboardCounts().displays=1;
        const auto before=MorphKeysCopy(f.first);const auto digest=f.first.Digest();plugins::ModelPasteFamilyResult result;std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanMorphKeyPaste(*f.app,0,change,&result)==MMS_OK&&change&&result.change==change&&result.hasKeys&&result.dataChanged&&!result.markedSlots.empty(),
            "morph participant accepts mixed clipboard counts and reports source targets");
        Check(f.first.Digest()==digest,"aggregate morph planning does not change scene or sibling marks");
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"mixed morph participant privately prepares");
        Check(mdl::BoneKeyPoolEnsure(*f.first.m,mdl::BoneKeyCapacity(f.first.m->boneKeys)+20),"simulate earlier bone participant pool growth");
        f.first.m->boneKeys[0].allocated=29;f.first.m->displayKeys[0].allocated=31;
        f.first.m->maxFrame=876;f.first.m->searchCursor=42;f.app->LastRegisteredFrame()=765;
        Check(candidate->IsCurrent(),"sibling growth marks and metadata do not invalidate independently owned morph candidate");
        failAfter=0;candidate->Publish();failAfter=-1;
        Check(f.first.m->maxFrame==876&&f.first.m->searchCursor==42&&f.app->LastRegisteredFrame()==765&&f.first.m->boneKeys[0].allocated==29&&f.first.m->displayKeys[0].allocated==31,
            "morph aggregate publication leaves sibling pools and shared metadata to owner");
        f.Oracle();Check(MorphPastePartData(f.first,MorphKeysCopy(f.second)),"aggregate morph payload matches old name registrar while marks are deferred");
        for(auto slot:result.markedSlots)Check(f.first.m->morphKeys[slot].allocated==1,"reported final source targets agree with pasted slots");
        std::unique_ptr<plugins::PreparedHistoryChange> undo;Check(change->Prepare(false,undo)==MMS_OK&&undo->IsCurrent(),"aggregate morph undo prepares after shared metadata changes");
        failAfter=0;undo->Publish();failAfter=-1;
        Check(MorphPastePartData(f.first,before)&&f.first.m->maxFrame==876&&f.first.m->searchCursor==42,"aggregate morph undo restores own keys and leaves shared metadata alone");
    }
    {
        MorphPasteFixture f;f.app->ClipboardCounts().morphs=1;f.clipboard[0].frame=0;f.clipboard[0].value=f.first.m->morphKeys[0].value;
        plugins::ModelPasteFamilyResult result;std::shared_ptr<plugins::HistoryChange> change;const auto before=f.first.Digest();
        Check(plugins::PlanMorphKeyPaste(*f.app,0,change,&result)==MMS_OK&&change&&result.hasKeys&&!result.dataChanged&&result.markedSlots==std::vector<unsigned>{0},
            "equal morph data still retains aggregate participant and source selection target");
        Check(f.first.Digest()==before,"equal aggregate plan remains private");
        std::unique_ptr<plugins::PreparedHistoryChange> candidate;Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"equal morph part freezes dependencies before aggregate publication");
        f.first.m->morphKeys[0].value+=.01f;Check(!candidate->IsCurrent(),"equal morph part rejects a changed own key dependency");
    }
    {
        MorphPasteFixture f;for(auto& source:f.clipboard)std::memcpy(source.name,"unknown",8);
        plugins::ModelPasteFamilyResult result;std::shared_ptr<plugins::HistoryChange> change;const auto before=f.first.Digest();
        Check(plugins::PlanMorphKeyPaste(*f.app,0,change,&result)==MMS_OK&&!change&&!result.change&&!result.hasKeys&&result.markedSlots.empty()&&f.first.Digest()==before,
            "unmatched morph family contributes no part or selected slots to mixed paste");
    }
    std::puts("  morph paste part: private growth and failure cancellation");std::fflush(stdout);
    {
        MorphPasteFixture f;unsigned tail=2;
        for(unsigned i=3;i<mdl::MorphKeyCapacity(f.first.m->morphKeys);++i)if(!f.first.m->morphKeys[i].frame){auto& key=f.first.m->morphKeys[i];f.first.m->morphKeys[tail].next=i;key.previous=tail;key.frame=100+i;key.value=.2f;tail=i;}
        f.app->CurrentFrame()=317;const auto capacity=mdl::MorphKeyCapacity(f.first.m->morphKeys);const auto before=f.first.Digest();
        plugins::ModelPasteFamilyResult result;std::shared_ptr<plugins::HistoryChange> change;bool success=false;
        for(long limit=0;limit<256;++limit){failAfter=limit;const auto status=plugins::PlanMorphKeyPaste(*f.app,0,change,&result);failAfter=-1;
            Check(f.first.Digest()==before,"mixed morph plan allocation refusal leaves scene intact");if(status==MMS_OK){success=bool(change);break;}Check(status==MMS_E_MEMORY&&!change&&!result.change,"failed mixed morph plan returns no partial participant");}
        Check(success,"mixed morph plan failure sweep completes");success=false;
        for(long limit=0;limit<512;++limit){std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=limit;const auto status=change->Prepare(true,candidate);failAfter=-1;
            Check(f.first.Digest()==before&&mdl::MorphKeyCapacity(f.first.m->morphKeys)==capacity,"mixed morph candidate OOM cancellation leaves growing pool private");
            if(status==MMS_OK){Check(candidate&&candidate->IsCurrent(),"mixed morph growth candidate eventually current");success=true;break;}Check(status==MMS_E_MEMORY&&!candidate,"mixed morph candidate reports allocation refusal");}
        Check(success,"mixed morph private-growth preparation completes");
    }
}
}
