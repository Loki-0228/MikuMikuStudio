// Include after native_model_paste_checks.inc.
#include "../src/plugins/native_model_paste.hpp"
namespace {
void ModelSourceBreakClipboard(ModelPasteFixture& f) {
    f.app->BoneClipboard()=nullptr;f.app->MorphClipboard()=nullptr;f.app->DisplayClipboard()=nullptr;
    f.app->ClipboardCounts()={};auto& counts=f.app->ClipboardCounts();counts.bones=counts.morphs=counts.displays=UINT32_MAX;
}
bool ModelSourceClipboardBroken(ModelPasteFixture& f) {
    const auto& counts=f.app->ClipboardCounts();
    return !f.app->BoneClipboard()&&!f.app->MorphClipboard()&&!f.app->DisplayClipboard()&&
        counts.bones==UINT32_MAX&&counts.morphs==UINT32_MAX&&counts.displays==UINT32_MAX;
}
plugins::ModelPasteSource ModelSourceCapture(ModelPasteFixture& f) {
    plugins::ModelPasteSource source;
    Check(plugins::CaptureModelPasteSource(*f.app,source)==MMS_OK,"model source captures bone scale curves and deep display payloads");return source;
}
std::shared_ptr<plugins::HistoryChange> ModelSourcePlan(ModelPasteFixture& f,const plugins::ModelPasteSource& source) {
    std::shared_ptr<plugins::HistoryChange> result;
    Check(plugins::PlanModelKeyPaste(*f.app,0,source,result)==MMS_OK,"immutable model source plans privately");return result;
}
const mdl::BoneKey& ModelSourceBoneAt(const Fixture& f,unsigned bone,unsigned frame) {
    unsigned slot=bone;
    for(std::size_t hops=0;f.m->boneKeys[slot].frame<frame&&f.m->boneKeys[slot].next;++hops) {
        Check(hops<mdl::BoneKeyCapacity(f.m->boneKeys),"immutable-source bone chain remains bounded");slot=f.m->boneKeys[slot].next;
    }
    Check(f.m->boneKeys[slot].frame==frame,"immutable-source bone key exists");return f.m->boneKeys[slot];
}
void NativeModelSourceChecks() {
    std::puts("  model source: offset independent deep source and explicit scale curve");
    for(unsigned offset:{5u,40u,150u}) {
        ModelPasteFixture f,oracle;
        const bone_scale::Curve curve{12,33,94,115};
        for(unsigned i=0;i<f.boneClipboard.size();++i) {
            auto scale=Scale(2.5f+float(i));scale.v[1]=.125f;
            bone_scale::CopyClipboard(f.app.get(),int(i),scale,false,curve);
            bone_scale::CopyClipboard(oracle.app.get(),int(i),scale,false,curve);
        }
        auto source=ModelSourceCapture(f);source.frameOffset=offset;
        const auto distance=offset-unsigned(oracle.app->CurrentFrame());
        for(auto& key:oracle.boneClipboard)key.frame+=distance;for(auto& key:oracle.morphClipboard)key.frame+=distance;
        for(auto& key:oracle.clipboard)key.frame+=distance;oracle.Bind(true);
        for(unsigned i=0;i<f.boneClipboard.size();++i)bone_scale::CopyClipboard(f.app.get(),int(i),Scale(99),false);
        ModelSourceBreakClipboard(f);const auto before=f.first.Digest();const ModelKeyState original(f.first);
        auto change=ModelSourcePlan(f,source);
        Check(change&&f.first.Digest()==before&&f.app->CurrentFrame()==5&&ModelSourceClipboardBroken(f),
            "explicit model source ignores damaged clipboard and scale side-table while preserving real playhead");
        if(offset!=5) {
            f.app->CurrentFrame()=int(offset);std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
            Check(f.history.PrepareNative("immutable source playhead",change,tx)==MMS_E_STALE&&!tx,
                "first model source preparation locks original playhead rather than insertion offset");f.app->CurrentFrame()=5;
        }
        source={};f.boneClipboard.clear();f.morphClipboard.clear();f.clipboard={};f.ik={};f.parents={};
        ModelPasteCommit(f,change);ModelPasteCommit(oracle,oracle.Plan());
        Check(ModelKeyState(oracle.first).Matches(f.first)&&f.first.m->maxFrame==oracle.first.m->maxFrame&&
            f.first.m->searchCursor==oracle.first.m->searchCursor&&DisplayFrameEqual(f.first,oracle.first)&&ModelSourceClipboardBroken(f),
            "immutable three-family source matches clipboard clone with equivalent private frame translation");
        for(unsigned i=0;i<mdl::BoneKeyCapacity(oracle.first.m->boneKeys);++i)
            Check(bone_scale::Key(f.first.raw(),int(i))==bone_scale::Key(oracle.first.raw(),int(i))&&
                bone_scale::KeyCurve(f.first.raw(),int(i))==bone_scale::KeyCurve(oracle.first.raw(),int(i)),
                "immutable bone source owns its explicit matrix and scale interpolation curve");
        const ModelKeyState after(f.first);f.app->CurrentFrame()=83;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&original.Matches(f.first),"immutable model undo restores all families after owned source destruction");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&f.app->CurrentFrame()==83&&ModelSourceClipboardBroken(f),
            "immutable model redo does not re-open source or read application clipboard");
    }
    std::puts("  model source: precise targets independent of encoded aliases");
    {
        ModelPasteFixture f;auto source=ModelSourceCapture(f);
        std::memcpy(f.first.bones[1].name,f.first.bones[0].name,sizeof(f.first.bones[0].name));
        for(unsigned i=0;i<source.bones.size();++i) {
            auto& key=source.bones[i];key.targetResolved=true;key.targetIndex=int(i%2);
            std::memset(key.key.name,'x',sizeof(key.key.name));
        }
        for(auto& key:source.morphs){key.targetResolved=true;key.targetIndex=0;std::memset(key.key.name,'x',sizeof(key.key.name));}
        for(auto& key:source.displays){key.ikTargetsResolved=true;key.ikTargets={-1,1,0};key.selectors.clear();}
        ModelSourceBreakClipboard(f);auto change=ModelSourcePlan(f,source);Check(change!=nullptr,"precise model source permits encoded destination aliases");
        ModelPasteCommit(f,change);
        Check(ModelSourceBoneAt(f.first,1,12).position[0]==source.bones[1].key.position[0]&&
            f.firstIk[1].enabled==source.displays.back().ik[1].enabled&&f.firstIk[0].enabled==source.displays.back().ik[2].enabled,
            "explicit bone and IK destination indices bypass duplicate encoded names without thread-local import state");
        auto malformed=source;malformed.displays.back().ikTargets.pop_back();const auto before=f.first.Digest();
        Check(plugins::PlanModelKeyPaste(*f.app,0,malformed,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==before,
            "resolved IK target count mismatch rejects entire source");
        malformed=source;malformed.bones.back().targetIndex=222;
        Check(plugins::PlanModelKeyPaste(*f.app,0,malformed,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==before,
            "out-of-range precise bone target rejects without other-family edits");
    }
    std::puts("  model source: no-op late malformed nested source and identity");
    {
        ModelPasteFixture f;auto source=ModelSourceCapture(f);ModelSourceBreakClipboard(f);
        ModelPasteCommit(f,ModelSourcePlan(f,source));auto next=source;next.morphs.back().key.value+=.125f;
        ModelPasteCommit(f,ModelSourcePlan(f,next));Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"model source no-op starts with a redo branch");
        const auto before=f.first.Digest();auto noop=ModelSourcePlan(f,source);
        Check(!noop&&f.first.Digest()==before&&f.history.RedoCount()==1,"same immutable source preserves redo and selection");
        plugins::ModelPasteSource empty;noop=ModelSourcePlan(f,empty);
        Check(!noop&&f.first.Digest()==before,"empty source never consults invalid application clipboard");
        auto bad=source;bad.displays.back().selectors[0].modelIndex=255;
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanModelKeyPaste(*f.app,0,bad,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==before&&f.history.RedoCount()==1,
            "invalid late owned external-parent payload rejects whole model source and preserves redo");
        bad=source;bad.bones.back().scaleCurve={128,20,107,107};
        Check(plugins::PlanModelKeyPaste(*f.app,0,bad,change)==MMS_E_ARGUMENT&&!change&&f.first.Digest()==before,
            "invalid late explicit scale curve does not leave any display or morph changes");
        bad=source;bad.frameOffset=std::uint32_t(frame_limits::maximum)+1u;
        Check(plugins::PlanModelKeyPaste(*f.app,0,bad,change)==MMS_E_ARGUMENT&&!change,"immutable model source rejects offset overflow");
        next.morphs.back().key.value+=.1f;change=ModelSourcePlan(f,next);auto* original=f.app->ModelSlot(0);f.app->ModelSlot(0)=f.second.raw();
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("source target identity",change,tx)==MMS_E_STALE&&!tx,"immutable source record rejects model-slot replacement");
        f.app->ModelSlot(0)=original;
    }
    std::puts("  model source: all-family allocation failure and growth");
    {
        ModelPasteFixture f(42);const auto source=ModelSourceCapture(f);ModelSourceBreakClipboard(f);const auto before=f.first.Digest();
        bool success=false;std::shared_ptr<plugins::HistoryChange> change;
        for(long allowance=0;allowance<8192&&!success;++allowance) {
            failAfter=allowance;const auto result=plugins::PlanModelKeyPaste(*f.app,0,source,change);failAfter=-1;
            Check(f.first.Digest()==before&&!f.history.UndoCount(),"immutable source planning OOM never publishes partial pools pose or marks");
            if(result==MMS_OK){Check(change!=nullptr,"immutable source allocation sweep obtains complete aggregate");success=true;}
            else Check(result==MMS_E_MEMORY&&!change,"immutable source allocation failure returns no partial record");
        }
        Check(success,"immutable source allocation sweep reaches all three family plans");ModelPasteCommit(f,change);
        Check(mdl::BoneKeyCapacity(f.first.m->boneKeys)>32&&mdl::MorphKeyCapacity(f.first.m->morphKeys)>16&&
            mdl::KeyPoolCapacity(f.first.m->displayKeys)>4&&ModelSourceClipboardBroken(f),"immutable source grows three owned pools without touching application clipboard");
    }
}
}
