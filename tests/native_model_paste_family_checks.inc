// Include after native_display_paste_checks.inc and native_key_paste_checks.inc.
#include "../src/plugins/native_model_paste.hpp"
namespace {
struct ModelPasteFamilyFixture : DisplayPasteFixture {
    std::array<mdl::BoneClipboardRecord,2> bones{};
    ModelPasteFamilyFixture() {
        for(unsigned i=0;i<bones.size();++i) {
            auto& source=bones[i];std::memcpy(source.name,first.bones[0].name,sizeof(first.bones[0].name));
            source.frame=i*10;source.position[0]=7+float(i);source.position[1]=2;source.rotation[3]=1;
            for(int lane=0;lane<4;++lane){source.interpolation[lane]=source.interpolation[lane+4]=20;
                source.interpolation[lane+8]=source.interpolation[lane+12]=107;}
            bone_scale::CopyClipboard(app.get(),int(i),Scale(2.5f+float(i)),false);
        }
        app->BoneClipboard()=bones.data();app->ClipboardCounts().bones=unsigned(bones.size());app->CurrentFrame()=150;
    }
    ~ModelPasteFamilyFixture(){app->BoneClipboard()=nullptr;app->ClipboardCounts().bones=0;}
};
struct ModelPasteFamilyPlans {
    plugins::ModelPasteFamilyResult bone,display;
    std::shared_ptr<plugins::HistoryChange> bones,displays;
    explicit ModelPasteFamilyPlans(ModelPasteFamilyFixture& f) {
        Check(plugins::PlanDisplayKeyPaste(*f.app,0,displays,&display)==MMS_OK&&displays&&display.hasKeys,
            "model paste display family privately returns an independent aggregate participant");
        auto target=std::dynamic_pointer_cast<plugins::DisplayKeyDeletion>(displays);
        Check(target!=nullptr,"display paste exposes target selectors to the bone family");
        Check(plugins::PlanBoneKeyPaste(*f.app,0,false,bones,&bone,std::move(target))==MMS_OK&&bones,
            "model paste bone family owns source keys and target display evaluation");
    }
};
void ModelPasteFamilyPrepare(ModelPasteFamilyPlans& plan,bool forward,
    std::unique_ptr<plugins::PreparedHistoryChange>& bone,std::unique_ptr<plugins::PreparedHistoryChange>& display) {
    Check(plan.bones->Prepare(forward,bone)==MMS_OK&&plan.displays->Prepare(forward,display)==MMS_OK&&
        bone->IsCurrent()&&display->IsCurrent(),"all model paste family candidates validate before any publication");
}
void ModelPasteFamilyPublish(std::unique_ptr<plugins::PreparedHistoryChange>& bone,
    std::unique_ptr<plugins::PreparedHistoryChange>& display) {
    failAfter=0;bone->Publish();display->Publish();failAfter=-1;
}
void NativeModelPasteFamilyChecks() {
    std::puts("  model paste families: metadata selection and private growth ownership");
    {
        ModelPasteFamilyFixture f;const auto before=f.first.Digest();const auto originalKeys=RegistrationKeys(f.first);
        const DisplayTestState originalDisplay(f.first);const auto initialPose=f.first.bones[1];const auto dirty=f.first.dirty[1];
        const auto max=f.first.m->maxFrame;const auto cursor=f.first.m->searchCursor;const auto last=f.app->LastRegisteredFrame();
        const auto identity=mdl::KeyPoolIdentity(f.first.m->displayKeys);auto* pool=f.first.m->displayKeys;
        ModelPasteFamilyPlans plan(f);
        Check(f.first.Digest()==before&&plan.bone.hasKeys&&plan.bone.dataChanged&&plan.display.dataChanged&&
            plan.bone.markedSlots.size()==2&&plan.display.markedSlots.size()==3&&plan.bone.maxFrame==160&&plan.display.maxFrame==170,
            "family planning returns pasted destinations and native maxima without shared metadata writes");
        std::unique_ptr<plugins::PreparedHistoryChange> bone,display;
        ModelPasteFamilyPrepare(plan,true,bone,display);
        Check(f.first.Digest()==before&&f.first.m->displayKeys==pool,"aggregate display family keeps deep growth off-scene during preparation");
        ModelPasteFamilyPublish(bone,display);
        Check(f.first.m->maxFrame==max&&f.first.m->searchCursor==cursor&&f.app->LastRegisteredFrame()==last&&
            f.first.m->morphKeys[0].allocated==4&&f.first.m->boneKeys[0].allocated==3,
            "family publication leaves metadata and marks for the root aggregate");
        Check(mdl::KeyPoolIdentity(f.first.m->displayKeys)==identity&&mdl::KeyPoolCapacity(f.first.m->displayKeys)>4,
            "aggregate display retains private growth even though registration metadata is disabled");
        Check(!std::memcmp(&f.first.bones[1],&initialPose,sizeof(initialPose))&&f.first.dirty[1]==dirty,
            "combined target selector evaluation preserves another dirty authored bone");
        for(auto& source:f.bones)source.position[0]=777;f.clipboard={};f.app->ClipboardCounts()={};
        bone.reset();display.reset();ModelPasteFamilyPrepare(plan,false,bone,display);ModelPasteFamilyPublish(bone,display);
        Check(SameRegistrationKeys(f.first,originalKeys)&&originalDisplay.Matches(f.first)&&f.first.m->maxFrame==max&&
            f.first.m->searchCursor==cursor&&f.app->LastRegisteredFrame()==last,"independent family undo owns both clipboard directions and preserves shared metadata");
        bone.reset();display.reset();ModelPasteFamilyPrepare(plan,true,bone,display);ModelPasteFamilyPublish(bone,display);
    }
    std::puts("  model paste families: unmatched bones still evaluate display targets");
    for(bool noSource:{false,true}) {
        ModelPasteFamilyFixture f;
        if(noSource)f.app->ClipboardCounts().bones=0;
        else for(auto& source:f.bones)std::memcpy(source.name,"unmatched bone",15);
        const auto original=RegistrationKeys(f.first);const auto pose=f.first.bones[0];
        ModelPasteFamilyPlans plan(f);
        Check(!plan.bone.hasKeys&&!plan.bone.dataChanged&&plan.bone.markedSlots.empty()&&plan.bone.change,
            "empty or unmatched bone clipboard retains a separate display evaluator without claiming pasted keys");
        std::unique_ptr<plugins::PreparedHistoryChange> bone,display;ModelPasteFamilyPrepare(plan,true,bone,display);ModelPasteFamilyPublish(bone,display);
        Check(SameRegistrationKeys(f.first,original)&&f.firstOrder[0].linkedModel==1&&f.firstOrder[0].linkedBone==1&&
            f.first.dirty[0]==0&&f.first.bones[0].trans[0]!=pose.trans[0],
            "display-only aggregate updates selectors and clean bone pose even when bone source has no destination");
    }
    std::puts("  model paste families: same source still returns selected targets");
    {
        ModelPasteFamilyFixture f;ModelPasteFamilyPlans plan(f);
        std::unique_ptr<plugins::PreparedHistoryChange> bone,display;ModelPasteFamilyPrepare(plan,true,bone,display);ModelPasteFamilyPublish(bone,display);
        ModelPasteFamilyPlans identical(f);
        Check(identical.bone.hasKeys&&!identical.bone.dataChanged&&identical.bones&&identical.bone.markedSlots.size()==2&&
            identical.display.hasKeys&&!identical.display.dataChanged&&identical.displays&&identical.display.markedSlots.size()==3,
            "same bone and display source families keep participants and destination selections for other-family edits");
    }
    std::puts("  model paste families: allocation cancellation and target identity");
    {
        ModelPasteFamilyFixture f;const auto before=f.first.Digest();ModelPasteFamilyPlans plan(f);bool ready=false;
        for(long allowance=0;allowance<2048&&!ready;++allowance) {
            std::unique_ptr<plugins::PreparedHistoryChange> bone,display;failAfter=allowance;
            auto result=plan.bones->Prepare(true,bone);
            if(result==MMS_OK)result=plan.displays->Prepare(true,display);failAfter=-1;
            Check(f.first.Digest()==before&&!f.history.UndoCount(),"partial model family preparation failures leave every live pool pose and mark untouched");
            if(result==MMS_OK){ready=true;bone.reset();display.reset();}
            else Check(result==MMS_E_MEMORY,"model paste family preparation reports allocation failures");
        }
        Check(ready,"model family injection reaches both candidates and nested display growth");
        std::unique_ptr<plugins::PreparedHistoryChange> bone,display;ModelPasteFamilyPrepare(plan,true,bone,display);
        auto* target=f.app->ModelSlot(1);f.app->ModelSlot(1)=nullptr;
        Check(!display->IsCurrent(),"display participant vetoes the whole transaction when target external-parent identity disappears");
        f.app->ModelSlot(1)=target;
    }
}
}
