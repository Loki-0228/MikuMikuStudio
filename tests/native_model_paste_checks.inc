#include "../src/plugins/native_model_paste.hpp"
namespace {
struct ModelPasteFixture : DisplayPasteFixture {
    std::vector<mdl::BoneClipboardRecord> boneClipboard;
    std::vector<mdl::MorphClipboardRecord> morphClipboard;
    explicit ModelPasteFixture(unsigned records=3) {
        for(auto* f:{&first,&second})for(auto& bone:f->bones)bone.type=mdl::BoneType::RotateMove;
        for(unsigned i=0;i<records;++i) {
            mdl::BoneClipboardRecord bone{};std::memcpy(bone.name,first.bones[i%2].name,20);
            bone.frame=i==records-1?0:i*7;bone.position[0]=3+float(i);bone.position[1]=-.5f*float(i);bone.rotation[3]=1;
            for(unsigned lane=0;lane<4;++lane){bone.interpolation[lane]=bone.interpolation[lane+4]=20;bone.interpolation[lane+8]=bone.interpolation[lane+12]=107;}
            boneClipboard.push_back(bone);
            bone_scale::CopyClipboard(app.get(),int(i),bone_scale::Matrix{},false,bone_scale::LinearCurve());
            mdl::MorphClipboardRecord morph{};std::memcpy(morph.name,first.morphs[0].name,20);morph.frame=bone.frame;morph.value=.1f+float(i%8)*.1f;morphClipboard.push_back(morph);
        }
        clipboard[2].frame=0;
        auto& entry=second.m->undoRings[0].slots[second.m->undoState[0]];
        entry.auxiliaryPose=undo_storage::MallocOrThrow(512*sizeof(mdl::BoneKeySnapshot));entry.dirty=0;mdl::BoneKeyVisitClear(*second.m);
        Bind(true);
    }
    ~ModelPasteFixture(){app->BoneClipboard()=nullptr;app->MorphClipboard()=nullptr;app->DisplayClipboard()=nullptr;app->ClipboardCounts()={};}
    void Bind(bool display) {
        app->BoneClipboard()=boneClipboard.data();app->MorphClipboard()=morphClipboard.data();app->DisplayClipboard()=display?clipboard.data():nullptr;
        app->ClipboardCounts()={};auto& n=app->ClipboardCounts();n.bones=unsigned(boneClipboard.size());n.morphs=unsigned(morphClipboard.size());if(display)n.displays=unsigned(clipboard.size());
    }
    void BindMask(unsigned mask) {
        Bind((mask&4u)!=0);
        if(!(mask&1u)){app->BoneClipboard()=nullptr;app->ClipboardCounts().bones=0;}
        if(!(mask&2u)){app->MorphClipboard()=nullptr;app->ClipboardCounts().morphs=0;}
    }
    void ClearSource(){app->BoneClipboard()=nullptr;app->MorphClipboard()=nullptr;app->DisplayClipboard()=nullptr;app->ClipboardCounts()={};boneClipboard.clear();morphClipboard.clear();clipboard={};ik={};parents={};}
    std::shared_ptr<plugins::HistoryChange> Plan(){std::shared_ptr<plugins::HistoryChange> out;Check(plugins::PlanModelKeyPaste(*app,0,out)==MMS_OK,"model clipboard aggregate plans privately");return out;}
    void Oracle() {
        for(std::size_t i=0;i<mdl::BoneKeyCapacity(second.m->boneKeys);++i)second.m->boneKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::MorphKeyCapacity(second.m->morphKeys);++i)second.m->morphKeys[i].allocated=0;
        for(std::size_t i=0;i<mdl::KeyPoolCapacity(second.m->displayKeys);++i)second.m->displayKeys[i].allocated=0;
        ResetBoneKeyCursor(second.raw());
        if(app->ClipboardCounts().bones)for(auto source:boneClipboard){bone_scale::Registration scale(bone_scale::Matrix{},bone_scale::LinearCurve());
            Check(RegisterBoneKey(second.raw(),reinterpret_cast<unsigned char*>(&source),app->CurrentFrame(),0),"legacy mixed bone source registers");}
        ResetMorphKeyCursor(second.raw());if(app->ClipboardCounts().morphs)for(const auto& source:morphClipboard)
            Check(RegisterMorphKeyFromRecord(second.raw(),reinterpret_cast<const unsigned char*>(&source),app->CurrentFrame()),"legacy mixed morph source registers");
        ResetDisplayKeyCursor(second.raw());
        if(app->ClipboardCounts().displays)for(const auto& source:clipboard)
            Check(RegisterDisplayKeyFromRecord(second.raw(),int(source.frame),source.visible,source.ikCount,reinterpret_cast<const unsigned char*>(source.ikStates),
                source.selectorCount,reinterpret_cast<const unsigned char*>(source.selectorStates),app->CurrentFrame()),"legacy mixed deep display source registers");
        DisplayLegacyFrame(second,unsigned(app->CurrentFrame()));
    }
};
struct RealIdentityModelPasteFixture : ModelPasteFixture {
    std::array<mdl::MorphRecord,2> namedMorphs{};
    RealIdentityModelPasteFixture() {
        for(auto& bone:first.bones){std::memset(bone.name,0,sizeof bone.name);std::strcpy(bone.name,"same-bone");}
        for(auto& morph:namedMorphs){morph=first.morphs[0];std::strcpy(morph.name,"same-morph");}
        first.m->morphs=namedMorphs.data();first.m->morphCount=unsigned(namedMorphs.size());first.m->physicsMode=2;
        std::memset(first.m->boneKeys,0,mdl::BoneKeyCapacity(first.m->boneKeys)*sizeof(mdl::BoneKey));
        std::memset(first.m->morphKeys,0,mdl::MorphKeyCapacity(first.m->morphKeys)*sizeof(mdl::MorphKey));
        for(unsigned root=0;root<2;++root){
            first.m->boneKeys[root].rotation[3]=1;first.m->boneKeys[root].next=root+2;
            first.m->morphKeys[root].next=root+2;
        }
        for(unsigned i=0;i<3;++i){
            const unsigned index=i+2;auto& bone=first.m->boneKeys[index];
            bone.previous=i==2?3:i;bone.next=i==1?4:0;bone.frame=i==2?14:10;
            bone.position[0]=3.f*float(index);bone.position[1]=-float(index);bone.rotation[3]=1;
            bone.physicsDisabled=i==1;
            for(unsigned lane=0;lane<16;++lane)bone.interpolation[lane]=static_cast<unsigned char>(20+index+lane);
            auto& morph=first.m->morphKeys[index];morph.previous=bone.previous;morph.next=bone.next;morph.frame=bone.frame;
            const float values[]={.25f,.75f,.5f};morph.value=values[i];
        }
        first.m->maxFrame=30;
    }
    ~RealIdentityModelPasteFixture(){
        history.Reset();history.Collect();
        first.m->morphs=first.morphs.data();first.m->morphCount=unsigned(first.morphs.size());
    }
    void Mark(unsigned families,bool batch=false,unsigned displays=0){
        ClearRealCopyMarks(first);
        for(unsigned index=2;index<=4;++index)if(batch||index==3){
            if(families&1u)first.m->boneKeys[index].allocated=1;
            if(families&2u)first.m->morphKeys[index].allocated=1;
        }
        for(unsigned index=0;index<displays;++index)first.m->displayKeys[index].allocated=1;
    }
};
thread_local HWND missingBoundPasteOwner = nullptr;
thread_local HWND missingBoundPasteWarning = nullptr;

LRESULT CALLBACK ObserveMissingBoundPaste(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE && missingBoundPasteOwner) {
        const auto window = reinterpret_cast<HWND>(wp);
        if (GetWindow(window, GW_OWNER) == missingBoundPasteOwner) {
            if (window == missingBoundPasteWarning) return CallNextHookEx(nullptr, code, wp, lp);
            wchar_t title[64]{};
            GetWindowTextW(window, title, std::size(title));
            Check(!missingBoundPasteWarning && std::wcscmp(title, L"Paste keyframes") == 0 &&
                  GetDlgItem(window, IDOK), "bound missing target opens one localized warning");
            missingBoundPasteWarning = window;
            Check(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE, "dismiss the bound paste warning");
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void RealSameModelIdentityPasteChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    // Single-family routes must consume the same private resolved-source plan
    // as the aggregate; the last case includes both duplicate tracks and time offsets.
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned scenario:{1u,2u,3u,7u}){
        RealIdentityModelPasteFixture f;RealFrameCopyBuffers buffers(*f.app);plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"configure real same-model copy common-history mode");
        const unsigned families=scenario&3u;const bool batch=(scenario&4u)!=0;f.Mark(families,batch);
        CmdControl400(f.app.get(),f.app->MainWindow(),420,0);
        const unsigned count=batch?3:1;
        Check(f.app->ClipboardCounts().bones==((families&1u)?count:0)&&
            f.app->ClipboardCounts().morphs==((families&2u)?count:0)&&!f.app->ClipboardCounts().displays,
            "actual 420 copies only requested bone and morph families");
        if(families&1u)Check(frame_clipboard::BoneTarget(f.app.get(),f.first.raw(),0)==(batch?0:1),"actual copy records the exact duplicated bone track");
        if(families&2u)Check(frame_clipboard::MorphTarget(f.app.get(),f.first.raw(),0)==(batch?0:1),"actual copy records the exact duplicated morph track");
        const ModelKeyState before(f.first);const auto ring=PoseRingDigest(f.first);const auto selection=f.first.selected;
        f.app->CurrentFrame()=40;CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
        Check(f.history.UndoCount()==1&&!f.history.RedoCount()&&PoseRingDigest(f.first)==ring&&f.first.selected==selection,
            "actual same-model identity paste publishes one common-history step and preserves selection and legacy rings");
        const auto boneCapacity=mdl::BoneKeyCapacity(f.first.m->boneKeys),morphCapacity=mdl::MorphKeyCapacity(f.first.m->morphKeys);
        for(unsigned i=0;i<3;++i)if(batch||i==1){
            const unsigned track=i==0?0:1,frame=i==2?44:40;
            if(families&1u){const int key=FindRealCopyKey(f.first.m->boneKeys,boneCapacity,track,frame);const auto& expected=before.bones[i+2];
                Check(key>=0&&std::memcmp(f.first.m->boneKeys[key].position,expected.position,sizeof expected.position)==0&&
                    std::memcmp(f.first.m->boneKeys[key].rotation,expected.rotation,sizeof expected.rotation)==0&&
                    std::memcmp(f.first.m->boneKeys[key].interpolation,expected.interpolation,sizeof expected.interpolation)==0&&
                    f.first.m->boneKeys[key].physicsDisabled==expected.physicsDisabled&&f.first.m->boneKeys[key].allocated,
                    "bound identity paste preserves each bone pose interpolation physics mark and relative frame");}
            if(families&2u){const int key=FindRealCopyKey(f.first.m->morphKeys,morphCapacity,track,frame);
                Check(key>=0&&f.first.m->morphKeys[key].value==before.morphs[i+2].value&&f.first.m->morphKeys[key].allocated,
                    "bound identity paste preserves each morph value selection and relative frame");}
        }
        if(!batch)Check(FindRealCopyKey(f.first.m->boneKeys,boneCapacity,0,40)<0&&
            FindRealCopyKey(f.first.m->morphKeys,morphCapacity,0,40)<0,
            "copying only second duplicate tracks never writes the first name match");
        const ModelKeyState after(f.first);
        // Replace the heap clipboard by copying the other model. Keep this
        // model's live key marks unchanged so replay checks only clipboard
        // independence rather than unrelated later selection ownership.
        ClearRealCopyMarks(f.second);f.second.m->boneKeys[0].allocated=1;f.second.m->morphKeys[0].allocated=1;
        f.app->SelectedModelSlot()=1;
        CmdControl400(f.app.get(),f.app->MainWindow(),420,0);
        Check(frame_clipboard::BoneTarget(f.app.get(),f.second.raw(),0)==0&&frame_clipboard::MorphTarget(f.app.get(),f.second.raw(),0)==0&&
            frame_clipboard::BoneTarget(f.app.get(),f.first.raw(),0)<0&&frame_clipboard::MorphTarget(f.app.get(),f.first.raw(),0)<0,
            "real second copy replaces both clipboard identities before replay");
        f.app->SelectedModelSlot()=0;
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(!f.history.UndoCount()&&f.history.RedoCount()==1&&before.Matches(f.first)&&f.first.selected==selection&&PoseRingDigest(f.first)==ring,
            "actual bound undo restores original mixed keys independently of replaced clipboard");
        buffers.Clear();
        CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
        Check(f.history.UndoCount()==1&&!f.history.RedoCount()&&after.Matches(f.first)&&f.first.selected==selection&&PoseRingDigest(f.first)==ring,
            "actual bound redo restores all pasted families after heap clipboard destruction");
    }
    // Exercise the actual command route, including a destination that cannot
    // represent the source's second occurrence. A missing target must never
    // silently fall back to the first same-named track.
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool targetExists:{true,false}){
        RealIdentityModelPasteFixture f;RealFrameCopyBuffers buffers(*f.app);plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"configure real cross-model copy common-history mode");
        for(auto& bone:f.second.bones){std::memset(bone.name,0,sizeof bone.name);std::strcpy(bone.name,"same-bone");}
        if(!targetExists)std::strcpy(f.second.bones[1].name,"other-bone");
        f.app->SelectedModelSlot()=0;f.Mark(1,false);
        const auto sourceKey=f.first.m->boneKeys[3];
        CmdControl400(f.app.get(),f.app->MainWindow(),420,0);
        Check(f.app->ClipboardCounts().bones==1 && f.app->ClipboardCounts().morphs==0,
            "actual cross-model copy contains only the source second bone track");
        f.app->SelectedModelSlot()=1;
        Check(frame_clipboard::BoneTarget(f.app.get(),f.second.raw(),0)==
            (targetExists?1:frame_clipboard::kMissingTarget),
            "cross-model identity resolves the same-name occurrence or reports it missing");
        const ModelKeyState before(f.second);const auto ring=PoseRingDigest(f.second);const auto selection=f.second.selected;
        HHOOK warningHook = nullptr;
        if (!targetExists) {
            f.app->EnglishUI() = 1;
            missingBoundPasteOwner = f.app->MainWindow();
            missingBoundPasteWarning = nullptr;
            warningHook = SetWindowsHookExW(WH_CBT, ObserveMissingBoundPaste, nullptr, GetCurrentThreadId());
            Check(warningHook && missingBoundPasteOwner, "observe the bound paste command on its fixture window");
        }
        f.app->CurrentFrame()=40;CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
        if (warningHook) {
            Check(UnhookWindowsHookEx(warningHook) && missingBoundPasteWarning &&
                  !IsWindow(missingBoundPasteWarning), "bound warning closes before command completion");
            missingBoundPasteOwner = missingBoundPasteWarning = nullptr;
        }
        const auto capacity=mdl::BoneKeyCapacity(f.second.m->boneKeys);
        Check(FindRealCopyKey(f.second.m->boneKeys,capacity,0,40)<0,
            "cross-model paste never writes the first duplicate bone track");
        if(!targetExists){
            Check(!f.history.UndoCount()&&!f.history.RedoCount()&&before.Matches(f.second)&&
                f.second.selected==selection&&PoseRingDigest(f.second)==ring,
                "missing duplicate target refuses the entire paste without a history entry");
            continue;
        }
        const int key=FindRealCopyKey(f.second.m->boneKeys,capacity,1,40);
        Check(key>=0&&std::memcmp(f.second.m->boneKeys[key].position,sourceKey.position,sizeof sourceKey.position)==0&&
            std::memcmp(f.second.m->boneKeys[key].rotation,sourceKey.rotation,sizeof sourceKey.rotation)==0&&
            std::memcmp(f.second.m->boneKeys[key].interpolation,sourceKey.interpolation,sizeof sourceKey.interpolation)==0&&
            f.second.m->boneKeys[key].physicsDisabled==sourceKey.physicsDisabled&&
            f.history.UndoCount()==1&&!f.history.RedoCount()&&f.second.selected==selection&&PoseRingDigest(f.second)==ring,
            "cross-model paste writes the second occurrence as one common-history entry");
        const ModelKeyState after(f.second);
        buffers.Clear();
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(!f.history.UndoCount()&&f.history.RedoCount()==1&&before.Matches(f.second)&&
            f.second.selected==selection&&PoseRingDigest(f.second)==ring,
            "cross-model undo restores the target after clipboard destruction");
        CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
        Check(f.history.UndoCount()==1&&!f.history.RedoCount()&&after.Matches(f.second)&&
            f.second.selected==selection&&PoseRingDigest(f.second)==ring,
            "cross-model redo restores the paste without the clipboard");
    }
    std::puts("  real duplicate-track copy/paste uses common history in both recording modes");
}
void RealFrameCopyFailureChecks() {
    RealIdentityModelPasteFixture f;RealFrameCopyBuffers buffers(*f.app);plugins::NativePoseTestBinding binding(*f.app,f.history);
    unsigned failures=0;bool completed=false;
    for(long allowance=0;allowance<128;++allowance){
        // A smaller old display clipboard exercises cleanup when new counts
        // already exceed the number of old allocated nested records.
        f.Mark(3,false,1);CmdControl400(f.app.get(),f.app->MainWindow(),420,0);
        Check(f.app->ClipboardCounts().bones==1&&f.app->ClipboardCounts().morphs==1&&f.app->ClipboardCounts().displays==1&&
            frame_clipboard::BoneTarget(f.app.get(),f.first.raw(),0)==1,"seed smaller real clipboard before copy failure");
        f.Mark(3,true,3);const ModelKeyState before(f.first);const auto ring=PoseRingDigest(f.first);
        failAfter=allowance;
        try{CmdControl400(f.app.get(),f.app->MainWindow(),420,0);}catch(const std::bad_alloc&){}
        failAfter=-1;
        const auto& counts=f.app->ClipboardCounts();
        if(counts.bones==3&&counts.morphs==3&&counts.displays==3){
            Check(f.app->BoneClipboard()&&f.app->MorphClipboard()&&f.app->DisplayClipboard()&&
                frame_clipboard::BoneTarget(f.app.get(),f.first.raw(),0)==0&&frame_clipboard::MorphTarget(f.app.get(),f.first.raw(),0)==0,
                "successful real copy publishes complete replacement buffers and matching identities");completed=true;break;
        }
        ++failures;
        Check(!counts.bones&&!counts.morphs&&!counts.displays&&!f.app->BoneClipboard()&&!f.app->MorphClipboard()&&!f.app->DisplayClipboard()&&
            frame_clipboard::BoneTarget(f.app.get(),f.first.raw(),0)<0&&frame_clipboard::MorphTarget(f.app.get(),f.first.raw(),0)<0,
            "failed actual copy clears counts buffers and old identities together");
        Check(before.Matches(f.first)&&PoseRingDigest(f.first)==ring&&!f.history.UndoCount()&&!f.history.RedoCount(),
            "failed copy never edits source keys marks or either history");
        CmdControl400(f.app.get(),f.app->MainWindow(),420,0);
        Check(f.app->ClipboardCounts().bones==3&&f.app->ClipboardCounts().morphs==3&&f.app->ClipboardCounts().displays==3&&
            f.app->BoneClipboard()&&f.app->MorphClipboard()&&f.app->DisplayClipboard()&&
            frame_clipboard::BoneTarget(f.app.get(),f.first.raw(),0)==0&&frame_clipboard::MorphTarget(f.app.get(),f.first.raw(),0)==0,
            "next real copy recovers fresh complete identities after allocation failure");
    }
    Check(completed&&failures>=2,"bounded real copy allocation sweep reaches failure and complete publication");
    std::printf("  real clipboard allocation recovery: %u rejected attempts before success\n",failures);
}
void ModelPasteCommit(ModelPasteFixture& f,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(change&&f.history.PrepareNative("model mixed paste",change,token,plugins::SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,"all model paste candidates prepare together");
    Check(CommitRegistrationNoAllocation(f.history,std::move(token))==MMS_OK,"mixed model publication allocates nothing");
}
void ModelPasteParityChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    for(unsigned mask:{3u,5u,6u,7u})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned physics:{0u,2u}) {
        std::printf("  model mixed parity families=%u mode=%u physics=%u\n",mask,unsigned(mode),physics);std::fflush(stdout);
        const bool display=(mask&4u)!=0;
        ModelPasteFixture f;f.BindMask(mask);f.first.m->physicsMode=f.second.m->physicsMode=static_cast<unsigned char>(physics);
        Check(f.history.SetMode(mode)==MMS_OK,"configure mixed model recording mode");
        const ModelKeyState before(f.first);const auto digest=f.first.Digest(),ring=PoseRingDigest(f.first);const auto last=f.app->LastRegisteredFrame();
        const auto dirty=f.first.bones[1];const auto dirtyScale=bone_scale::Pose(f.first.raw(),1);
        auto change=f.Plan();Check(change&&f.first.Digest()==digest&&before.Matches(f.first),"mixed model planning preserves all native storage");
        ModelPasteCommit(f,change);f.Oracle();
        Check(ModelKeyState(f.second).Matches(f.first)&&f.first.m->maxFrame==f.second.m->maxFrame&&f.first.m->searchCursor==f.second.m->searchCursor,
            "mixed model final keys all marks maximum and final display cursor match sequential legacy registrars");
        if(display)Check(DisplayFrameEqual(f.first,f.second),"mixed model deep IK and external-parent state matches target display frame");
        Check(PoseRingDigest(f.first)==ring&&f.app->LastRegisteredFrame()==last&&f.history.UndoCount()==1,
            "mixed model paste records one explicit step without changing old rings or registration marker");
        Check(!std::memcmp(&f.first.bones[1],&dirty,sizeof(dirty))&&bone_scale::Pose(f.first.raw(),1)==dirtyScale&&f.first.dirty[1],"mixed display and bone paste preserve dirty author pose and scale");
        if(display)Check(f.firstOrder[0].linkedModel==1&&f.firstOrder[0].linkedBone==1&&f.first.bones[0].trans[0]!=123,
            "clean bone and model-wide external parent are evaluated from the same final mixed target");
        const ModelKeyState after(f.first);const auto maxAfter=f.first.m->maxFrame;const auto cursorAfter=f.first.m->searchCursor;f.ClearSource();
        f.app->CurrentFrame()=83;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&before.Matches(f.first)&&f.first.m->searchCursor==5,"mixed model undo restores deep payloads marks and metadata after source destruction");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first)&&f.first.m->maxFrame==maxAfter&&f.first.m->searchCursor==cursorAfter&&f.app->CurrentFrame()==83,
            "mixed model redo owns every source byte across model and playhead changes");
    }
}
void ModelPasteGrowthAndNoopChecks() {
    std::puts("  model mixed growth and no-op");std::fflush(stdout);
    ModelPasteFixture f(42);const ModelKeyState before(f.first);
    const auto boneId=mdl::BoneKeyPoolIdentity(f.first.m->boneKeys),morphId=mdl::MorphKeyPoolIdentity(f.first.m->morphKeys),displayId=mdl::KeyPoolIdentity(f.first.m->displayKeys);
    ModelPasteCommit(f,f.Plan());
    Check(mdl::BoneKeyCapacity(f.first.m->boneKeys)>32&&mdl::MorphKeyCapacity(f.first.m->morphKeys)>16&&mdl::KeyPoolCapacity(f.first.m->displayKeys)>4,
        "mixed paste privately grows all three full pools");
    Check(mdl::BoneKeyPoolIdentity(f.first.m->boneKeys)==boneId&&mdl::MorphKeyPoolIdentity(f.first.m->morphKeys)==morphId&&mdl::KeyPoolIdentity(f.first.m->displayKeys)==displayId,"three-family growth preserves all pool identities");
    const auto unchanged=f.first.Digest();auto same=f.Plan();Check(!same&&f.first.Digest()==unchanged&&f.history.UndoCount()==1,"identical mixed paste creates no mark-only history");
    Check(f.history.History(false)==MMS_OK&&before.Matches(f.first),"mixed grown pools undo original prefix and selection");
    f.Bind(false);for(auto& source:f.boneClipboard){std::memset(source.name,0,sizeof(source.name));std::memcpy(source.name,"unmatched",10);}
    for(auto& source:f.morphClipboard){std::memset(source.name,0,sizeof(source.name));std::memcpy(source.name,"unmatched",10);}
    const auto noMatch=f.first.Digest();same=f.Plan();Check(!same&&f.history.RedoCount()==1&&f.first.Digest()==noMatch,"all-unmatched two-family paste preserves redo cursor and selection");
}
void ModelPasteFailureChecks() {
    std::puts("  model mixed cancellation failures alias and staleness");std::fflush(stdout);
    {
        ModelPasteFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        ModelPasteCommit(f,f.Plan());const ModelKeyState after(f.first);
        Check(f.history.History(false)==MMS_OK,"seed mixed-family redo before destination name ambiguity");
        auto* oldMorphs=f.first.m->morphs;const auto oldCount=f.first.m->morphCount;
        std::array<mdl::MorphRecord,3> morphs{};morphs[0]=oldMorphs[0];morphs[1]=morphs[0];
        std::strcpy(morphs[2].name,"helper");
        f.first.m->morphs=morphs.data();f.first.m->morphCount=unsigned(morphs.size());
        const auto digest=f.first.Digest();const ModelKeyState before(f.first);
        CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
        Check(f.first.Digest()==digest&&before.Matches(f.first)&&!f.history.UndoCount()&&f.history.RedoCount()==1,
            "late ambiguous morph target rejects the entire real mixed paste before bone or display publication");
        f.first.m->morphs=oldMorphs;f.first.m->morphCount=oldCount;
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),
            "failed mixed destination validation preserves existing complete redo payload");
    }
    ModelPasteFixture f;const auto before=f.first.Digest();const ModelKeyState keys(f.first);std::shared_ptr<plugins::HistoryChange> change;
    f.clipboard[2].ikCount=1;f.clipboard[2].ikStates=nullptr;
    Check(plugins::PlanModelKeyPaste(*f.app,0,change)!=MMS_OK&&!change&&f.first.Digest()==before&&keys.Matches(f.first),"malformed late deep participant cancels bone and morph plans");
    f.clipboard[2].ikCount=3;f.clipboard[2].ikStates=f.ik[2].data();
    auto* original=f.first.m->displayKeys[2].ikStates;f.first.m->displayKeys[2].ikStates=reinterpret_cast<unsigned char*>(f.first.m->morphKeys);
    Check(plugins::PlanModelKeyPaste(*f.app,0,change)!=MMS_OK&&!change,"cross-family nested payload alias rejects before any write");f.first.m->displayKeys[2].ikStates=original;
    bool success=false;unsigned failed=0;
    for(long allowance=0;allowance<4096;++allowance){failAfter=allowance;const auto result=plugins::PlanModelKeyPaste(*f.app,0,change);failAfter=-1;
        if(result==MMS_OK){success=bool(change);break;}++failed;Check(!change&&f.first.Digest()==before&&keys.Matches(f.first),"mixed plan allocation failure leaves every family intact");}
    Check(success&&failed>10,"mixed plan failure sweep reaches a complete aggregate");
    plugins::SharedHistory limited(f.storage,{},32,1);std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(limited.PrepareNative("budget",change,token)==MMS_E_MEMORY&&!token&&f.first.Digest()==before,"mixed deep payload budget refuses before publication");
    success=false;failed=0;
    for(long allowance=0;allowance<4096;++allowance){std::string label="mixed candidate";failAfter=allowance;
        const auto result=f.history.PrepareNative(std::move(label),change,token);failAfter=-1;
        if(result==MMS_OK){token.reset();success=true;break;}++failed;Check(!token&&f.first.Digest()==before&&keys.Matches(f.first),"late mixed candidate failure publishes no earlier family");}
    Check(success&&failed>10,"mixed candidate sweep reaches all participants and deep growth");
    ModelPasteCommit(f,change);std::unique_ptr<plugins::PreparedHistoryChange> old;
    Check(change->Prepare(false,old)==MMS_OK&&old&&old->IsCurrent(),"prepare mixed candidate before ABA");
    Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK&&!old->IsCurrent(),"mixed candidate rejects undo redo ABA");
    old.reset();Check(f.history.History(false)==MMS_OK,"seed mixed redo after failure checks");
    const auto stable=f.first.Digest();failAfter=0;const auto result=f.history.History(true);failAfter=-1;
    Check(result==MMS_E_MEMORY&&f.history.RedoCount()==1&&f.first.Digest()==stable,"mixed redo allocation failure preserves all data and journal cursor");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> stale;
    Check(f.history.PrepareNative("identity",change,stale)==MMS_OK,"prepare mixed aggregate before replacing a participant pool");
    auto* oldPool=f.first.m->morphKeys;auto* replacement=mdl::MorphKeyPoolCreate(mdl::MorphKeyCapacity(oldPool));
    std::memcpy(replacement,oldPool,mdl::MorphKeyCapacity(oldPool)*sizeof(*oldPool));f.first.m->morphKeys=replacement;
    const auto changedIdentity=f.first.Digest();
    Check(f.history.CommitNative(std::move(stale))==MMS_E_STALE&&f.first.Digest()==changedIdentity&&f.history.RedoCount()==1,
        "replaced morph pool identity cancels whole mixed publication without consuming redo");
    f.first.m->morphKeys=oldPool;mdl::MorphKeyPoolDestroy(replacement);
}
void ModelPasteZeroBoneChecks() {
    std::puts("  model mixed zero-bone destination");std::fflush(stdout);
    for(unsigned mask:{3u,5u,7u}) {
        ModelPasteFixture f;f.BindMask(mask);
        auto& model=*f.first.m;
        // Retain the registered empty bone allocation so Fixture::Digest can
        // inspect its known 32-slot storage. Remove all bone/IK/parent tables
        // from the model view, then restore them only after the journal dies.
        const auto count=model.boneCount,ikCount=model.ikChainCount,parentCount=model.boneOrderCount;
        auto* bones=model.boneTable;auto* ik=model.ikChains;auto* order=model.boneOrderTable;
        const auto boneKeys=RegistrationKeys(f.first);
        model.boneCount=0;model.boneTable=nullptr;model.ikChainCount=0;model.ikChains=nullptr;
        model.boneOrderCount=0;model.boneOrderTable=nullptr;
        std::memset(model.boneKeys,0,boneKeys.size()*sizeof(mdl::BoneKey));
        for(auto& source:f.clipboard){source.ikCount=source.selectorCount=0;source.ikStates=nullptr;source.selectorStates=nullptr;}
        for(auto& source:f.boneClipboard){std::memset(source.name,0,sizeof(source.name));std::memcpy(source.name,"unmatched",10);}
        const ModelKeyState before(f.first);const auto digest=f.first.Digest();
        auto change=f.Plan();Check(change&&f.first.Digest()==digest,"zero-bone mixed destination privately accepts unmatched bone source and remaining editable families");
        ModelPasteCommit(f,change);const ModelKeyState after(f.first);
        Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&before.Matches(f.first),"zero-bone mixed paste commits and undoes without a bone participant");
        Check(f.history.History(true)==MMS_OK&&after.Matches(f.first),"zero-bone mixed paste redoes morph and display payloads");
        Check(f.history.History(false)==MMS_OK,"seed zero-bone mixed redo before invalid source");
        f.boneClipboard.back().frame=std::uint32_t(frame_limits::maximum);
        const auto stable=f.first.Digest();std::shared_ptr<plugins::HistoryChange> invalid;
        Check(plugins::PlanModelKeyPaste(*f.app,0,invalid)==MMS_E_ARGUMENT&&!invalid&&f.first.Digest()==stable&&before.Matches(f.first)&&f.history.RedoCount()==1,
            "unmatched zero-bone source still validates every relative frame before publishing other families");
        f.boneClipboard.back().frame=0;f.app->BoneClipboard()=nullptr;
        Check(plugins::PlanModelKeyPaste(*f.app,0,invalid)==MMS_E_ARGUMENT&&!invalid&&f.first.Digest()==stable&&before.Matches(f.first)&&f.history.RedoCount()==1,
            "zero-bone destination rejects a missing counted bone source without modifying model pools");
        f.app->BoneClipboard()=f.boneClipboard.data();change.reset();
        Check(f.history.Reset()==MMS_OK,"release zero-bone history before restoring fixture tables");f.history.Collect();
        model.boneCount=count;model.boneTable=bones;model.ikChainCount=ikCount;model.ikChains=ik;
        model.boneOrderCount=parentCount;model.boneOrderTable=order;
        std::memcpy(model.boneKeys,boneKeys.data(),boneKeys.size()*sizeof(mdl::BoneKey));
    }
}
void ModelPasteLaterGrowthChecks() {
    std::puts("  model mixed later growth selection ownership");std::fflush(stdout);
    ModelPasteFixture f(42);ModelPasteCommit(f,f.Plan());
    const auto boneSize=mdl::BoneKeyCapacity(f.first.m->boneKeys),morphSize=mdl::MorphKeyCapacity(f.first.m->morphKeys),displaySize=mdl::KeyPoolCapacity(f.first.m->displayKeys);
    const auto boneTail=boneSize+3,morphTail=morphSize+3,displayTail=displaySize+3;
    Check(mdl::BoneKeyPoolEnsure(*f.first.m,boneTail+1)&&mdl::MorphKeyPoolEnsure(*f.first.m,morphTail+1)&&mdl::DisplayKeyPoolEnsure(*f.first.m,displayTail+1),
        "later same-identity growth creates unrelated selection tails in every model pool");
    f.first.m->boneKeys[boneTail].allocated=21;f.first.m->morphKeys[morphTail].allocated=22;f.first.m->displayKeys[displayTail].allocated=23;
    for(bool forward:{false,true}) {
        Check(f.history.History(forward)==MMS_OK,"older mixed paste survives later same-identity growth");
        Check(f.first.m->boneKeys[boneTail].allocated==21&&f.first.m->morphKeys[morphTail].allocated==22&&f.first.m->displayKeys[displayTail].allocated==23,
            "older mixed paste leaves marks outside captured and pasted extents unchanged");
    }
}
void NativeModelPasteChecks(){RealSameModelIdentityPasteChecks();RealFrameCopyFailureChecks();ModelPasteParityChecks();ModelPasteGrowthAndNoopChecks();ModelPasteFailureChecks();ModelPasteZeroBoneChecks();ModelPasteLaterGrowthChecks();}
}
