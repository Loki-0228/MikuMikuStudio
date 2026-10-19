// Include after native_morph_text_checks.inc. Uses the real menu-230 route;
// the unbound menu command is an independent oracle for weights and controls.
#include "../src/plugins/native_morph_reset.hpp"
#include "../src/plugins/scene_object_history.hpp"
#include "../src/plugins/native_object_commands.hpp"
#include "mikudancestudio/bake_ui_lock.hpp"
#include "mikudancestudio/autosave.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include "mikudancestudio/scene_ownership.hpp"
namespace mikudancestudio { void CmdFileMenu(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
namespace {
struct MorphResetFixture:MorphTextFixture {
    std::array<std::array<mdl::MorphRecord,7>,2> morphs{};
    MorphResetFixture(){
        for(unsigned slot=0;slot<2;++slot){auto& target=slot?second:first;
            target.m->morphs=morphs[slot].data();target.m->morphCount=unsigned(morphs[slot].size());
            for(unsigned i=0;i<morphs[slot].size();++i){auto& m=morphs[slot][i];
                std::snprintf(m.name,sizeof(m.name),"reset-%u-%u",slot,i);m.type=std::uint8_t(i%9);
                m.panel=static_cast<mdl::MorphPanel>(i%5);m.value=(slot?-.25f:.12345678f)+float(i)*.137f;}
        }
        app->CurrentFrame()=41;app->PhysicsResetPending()=0;
    }
    ~MorphResetFixture(){history.Reset();history.Collect();
        first.m->morphs=firstMorphs.data();first.m->morphCount=unsigned(firstMorphs.size());
        second.m->morphs=secondMorphs.data();second.m->morphCount=unsigned(secondMorphs.size());}
    std::array<std::uint32_t,7> Bits(unsigned slot=0)const{
        std::array<std::uint32_t,7> out{};
        for(unsigned i=0;i<out.size();++i)std::memcpy(&out[i],&morphs[slot][i].value,4);return out;
    }
    void SetBits(unsigned i,std::uint32_t value,unsigned slot=0){std::memcpy(&morphs[slot][i].value,&value,4);}
    void Command(){CmdFileMenu(app.get(),app->MainWindow(),230,0);}
    bool Zero(unsigned slot=0)const{for(auto b:Bits(slot))if(b)return false;return true;}
};
void MorphResetParityAndHistory(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned slot=0;slot<2;++slot){
        MorphResetFixture f;f.app->SelectedModelSlot()=std::uint8_t(slot);auto& model=slot?f.second:f.first;
        // Preserve NaN payloads by bits, without evaluating or normalizing them.
        f.SetBits(3,0x80000000u,slot);f.SetBits(4,0x7fc12345u,slot);f.SetBits(5,0x7f800000u,slot);f.SetBits(6,0xff800000u,slot);
        const auto before=f.Bits(slot),other=f.Bits(1-slot);const auto keys=MorphKeysCopy(model);const auto ring=PoseRingDigest(model);
        const auto bones=model.bones;const auto flags=model.dirty;const auto selected=model.selected;
        std::array<std::uint32_t,7> oracle{};std::array<std::array<char,32>,4> captions{};
        {
            const auto old=g_Block;MorphResetFixture native;native.app->SelectedModelSlot()=std::uint8_t(slot);
            for(unsigned i=0;i<before.size();++i)native.SetBits(i,before[i],slot);
            native.Command();oracle=native.Bits(slot);
            for(unsigned lane=0;lane<4;++lane)GetWindowTextA(native.Field(lane),captions[lane].data(),int(captions[lane].size()));
            Check(!native.app->PhysicsResetPending()&&!native.app->SceneModified(),"original all-morph reset changes no scene dirty or physics-reset flags");
            g_Block=old;
        }
        g_Block=f.app.get();plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"configure all-morph reset recording mode");
        f.Type(544,"bone draft");for(unsigned lane=0;lane<4;++lane)f.TypeField(lane,"morph draft");
        f.Command();Check(f.Bits(slot)==oracle&&f.Zero(slot)&&f.Bits(1-slot)==other&&
            f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"one production reset clears every visible and hidden morph in one policy-controlled Adjustment");
        for(unsigned lane=0;lane<4;++lane){char text[32]{};GetWindowTextA(f.Field(lane),text,sizeof(text));
            Check(!std::strcmp(text,captions[lane].data())&&!SendMessageA(f.Field(lane),EM_GETMODIFY,0,0)&&
                SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==0,
                "successful reset reproduces the original four exact captions and zero thumbs and discards their morph drafts");}
        char draft[32]{};GetWindowTextA(f.Edit(544),draft,sizeof(draft));
        Check(!std::strcmp(draft,"bone draft")&&SendMessageA(f.Edit(544),EM_GETMODIFY,0,0)&&
            f.app->SceneModified()&&f.app->RecordedFrameCountByte1()&&!f.app->PhysicsResetPending()&&f.app->CurrentFrame()==41&&
            MorphKeysMatch(model,keys)&&PoseRingDigest(model)==ring&&!std::memcmp(bones.data(),model.bones.data(),sizeof(bones))&&
            flags==model.dirty&&selected==model.selected&&f.SelectionUnchanged(),
            "reset publication changes only morph weights and common dirty hints while preserving native keys bones physics flags selection and other drafts");
        const auto revision=[&]{std::uint64_t r=0;Check(f.history.Revision(r)==MMS_OK,"read reset revision");return r;}();
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Command();std::uint64_t afterRevision=0;f.history.Revision(afterRevision);
        Check(afterRevision==revision&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
            f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"all +0 reset is a true no-op without another revision dirty hint or history entry");
        if(mode==Mode::AllOperations){
            f.app->SelectedModelSlot()=std::uint8_t(1-slot);f.app->CameraMode()=1;f.app->CurrentFrame()=93;
            Check(f.history.History(false)==MMS_OK&&f.Bits(slot)==before&&f.Bits(1-slot)==other&&f.app->CurrentFrame()==93,
                "reset undo restores exact finite signed-zero NaN and infinity bits on the original model across model mode and frame changes");
            Check(f.history.History(true)==MMS_OK&&f.Zero(slot)&&f.Bits(1-slot)==other&&MorphKeysMatch(model,keys),
                "reset redo clears only original model weights without resampling or editing original VMD fields");
        }
    }
}
std::shared_ptr<plugins::HistoryChange> MorphResetPlan(MorphResetFixture& f){
    std::shared_ptr<plugins::HistoryChange> out;Check(plugins::PlanMorphReset(*f.app,0,out)==MMS_OK&&out,"prepare complete private all-morph reset");return out;
}
void MorphResetCancellationAndOwnership(){
    MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.SetBits(3,0);
    const auto before=f.Bits();const auto keys=MorphKeysCopy(f.first);auto change=MorphResetPlan(f);
    {std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("reset cancel",change,tx,plugins::SharedHistory::NativeIntent::Adjustment)==MMS_OK&&f.Bits()==before,
            "prepared reset owns a private candidate without live preview writes");tx.reset();}
    Check(f.Bits()==before&&!f.history.UndoCount()&&!f.history.RedoCount()&&MorphKeysMatch(f.first,keys),
        "cancelling reset discards the whole candidate without author or key changes");
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("reset atomic",change,tx,plugins::SharedHistory::NativeIntent::Adjustment)==MMS_OK,"prepare atomic reset publication");
    failAfter=0;const auto committed=f.history.CommitNative(std::move(tx));failAfter=-1;
    Check(committed==MMS_OK&&f.Zero()&&f.history.UndoCount()==1,"complete prepared reset publishes without allocation in one journal node");
    plugins::resource_budget::Collector bookkeeping;auto retained=plugins::RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
    Check(retained->MeasureBookkeeping(bookkeeping)==MMS_OK&&bookkeeping.Result().Complete()&&
        f.history.RetainedBytes()>=change->RetainedBytes()+bookkeeping.Result().totalBytes,
        "reset history charges the complete captured weight metadata vector and actual retained-owner bookkeeping");
    f.morphs[0][3].value=.9876543f;f.morphs[0][3].name[0]^=1;f.first.bones[0].trans[2]=77;f.first.dirty[0]=1;
    f.first.m->morphKeys[15].reserved[1]^=1;const auto foreignKeys=MorphKeysCopy(f.first);const auto foreign=f.Bits()[3];
    Check(f.history.History(false)==MMS_OK,"reset undo accepts independent changes to fields it never changed");
    auto expected=before;expected[3]=foreign;
    Check(f.Bits()==expected&&f.first.bones[0].trans[2]==77&&f.first.dirty[0]==1&&MorphKeysMatch(f.first,foreignKeys),
        "undo preserves unowned weights their metadata unregistered bone pose physics flags and independent native key edits");
    Check(f.history.History(true)==MMS_OK&&f.Bits()[3]==foreign,"redo also preserves foreign edits of originally unchanged +0 weights");
    for(unsigned i=0;i<7;++i)if(i!=3)Check(f.Bits()[i]==0,"redo restores every originally owned weight to exact +0");
}
void MorphResetStaleAndGuardChecks(){
    for(unsigned why=0;why<10;++why){MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);auto change=MorphResetPlan(f);
        const auto table=f.first.m->morphs;const auto count=f.first.m->morphCount;auto* keys=f.first.m->morphKeys;mdl::MorphKey* replacement=nullptr;
        if(why==0)f.app->ModelSlot(0)=f.second.raw();if(why==1)f.first.m->morphs=f.morphs[1].data();if(why==2)--f.first.m->morphCount;
        if(why==3)f.morphs[0][6].name[0]^=1;if(why==4)f.morphs[0][6].panel=mdl::MorphPanel::other;if(why==5)++f.morphs[0][6].type;
        if(why==6){replacement=mdl::MorphKeyPoolCreate(16);f.first.m->morphKeys=replacement;}
        if(why==7)f.app->CurrentFrame()=42;if(why==8)f.morphs[0][6].value=.555f;
        if(why==9){plugins::ForgetSceneObject(f.first.raw());plugins::SceneObjectIdentity(f.first.raw());}
        const auto foreign=f.Bits();std::unique_ptr<plugins::PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&f.Bits()==foreign&&!f.history.UndoCount(),
            "initial reset rejects replaced model table count hidden-morph identity key pool frame changed source or stable ID without touching foreign state");
        f.app->ModelSlot(0)=f.first.raw();f.first.m->morphs=table;f.first.m->morphCount=count;f.first.m->morphKeys=keys;
        if(replacement)mdl::MorphKeyPoolDestroy(replacement);
    }
    {
        MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);auto change=MorphResetPlan(f);const auto before=f.Bits();
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
        Check(f.history.PrepareNative("stale reset candidate",change,tx,plugins::SharedHistory::NativeIntent::Adjustment)==MMS_OK,
            "reserve initial reset before a conflicting external write");f.morphs[0][6].value=.777f;const auto foreign=f.Bits();
        Check(f.history.CommitNative(std::move(tx))==MMS_E_STALE&&f.Bits()==foreign&&!f.history.UndoCount(),
            "conflicting value between reset preparation and commit rejects publication without rolling over the foreign write");
        Check(foreign!=before,"reset candidate conflict is a real source change");
    }
    {
        MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);auto change=MorphResetPlan(f);const auto before=f.Bits();
        {plugins::NativePoseTestBinding rebound(*f.app,f.history);std::unique_ptr<plugins::PreparedHistoryChange> candidate;
            Check(change->Prepare(true,candidate)==MMS_E_STALE&&!candidate&&f.Bits()==before,
                "rebinding the same app and journal invalidates an unpublished reset source");}
    }
    {
        MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Bits();f.Command();
        Check(mdl::MorphKeyPoolEnsure(*f.first.m,17)&&f.history.History(false)==MMS_OK&&f.Bits()==before&&
            f.history.History(true)==MMS_OK&&f.Zero(),"reset replay survives identity-preserving growth of the original native morph key pool");
    }
    {
        MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.Command();bool changed=false;
        Check(plugins::ApplyBoundMorphValue(*f.app,0,1,.75f,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK&&f.Zero(),
            "seed a real redo entry while all current morph weights are +0");
        std::uint64_t before=0,after=0;f.history.Revision(before);f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Command();f.history.Revision(after);
        Check(before==after&&f.history.RedoCount()==1&&f.history.UndoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
            "a true zero reset keeps existing redo and does not publish even a nonretained history change");
    }
    for(unsigned why=0;why<8;++why){MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Bits();f.TypeField(0,"pending");
        std::unique_ptr<ScopedSceneMutation> mutation;
        if(why==0)f.app->CameraMode()=1;if(why==1)f.app->PlaybackActive()=1;if(why==2)f.app->FrameStepPlayback()=1;
        if(why==3)f.app->state.recordPlaybackStartPending=1;if(why==4)mutation=std::make_unique<ScopedSceneMutation>(*f.app);
        if(why==5)f.app->state.frameCopyDialog=reinterpret_cast<HWND>(INT_PTR(1));
        if(why==6)Check(f.history.Enter()==MMS_OK,"hold journal before reset rejection");if(why==7)SetCapture(f.Field(0));
        f.Command();char text[32]{};GetWindowTextA(f.Field(0),text,sizeof(text));
        Check(f.Bits()==before&&!f.history.UndoCount()&&!f.app->SceneModified()&&!std::strcmp(text,"pending")&&SendMessageA(f.Field(0),EM_GETMODIFY,0,0),
            "bound reset consumes blocked commands without legacy fallback or draft changes");
        if(why==6)f.history.Leave();if(why==7)ReleaseCapture();
        f.app->CameraMode()=f.app->PlaybackActive()=f.app->FrameStepPlayback()=f.app->state.recordPlaybackStartPending=0;
        f.app->state.frameCopyDialog=nullptr;
    }
}
void MorphResetFailureAndBudgetChecks(){
    MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);bool changed=false;
    Check(plugins::ApplyBoundMorphValue(*f.app,0,1,.75f,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,"seed reset failure redo");
    const auto before=f.Bits();const auto keys=MorphKeysCopy(f.first);const auto ring=PoseRingDigest(f.first);unsigned failures=0;bool success=false;
    for(long limit=0;limit<256;++limit){f.TypeField(2,"pending draft");f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        failAfter=limit;f.Command();failAfter=-1;
        if(f.history.UndoCount()){success=true;break;}++failures;char text[32]{};GetWindowTextA(f.Field(2),text,sizeof(text));
        Check(f.Bits()==before&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
            !f.app->PhysicsResetPending()&&MorphKeysMatch(f.first,keys)&&PoseRingDigest(f.first)==ring&&!std::strcmp(text,"pending draft")&&SendMessageA(f.Field(2),EM_GETMODIFY,0,0),
            "each real reset allocation refusal preserves all weights keys old ring redo dirty flags and pending text");
        Check(f.history.CheckNativeEdit()==MMS_OK,"failed reset releases journal admission");
    }
    Check(success&&failures>=5&&f.Zero()&&f.history.UndoCount()==1&&f.history.RedoCount()==0,"allocation sweep reaches a single atomic reset and discards redo only on success");
    Check(f.history.History(false)==MMS_OK&&f.Bits()==before,"successful reset remains exactly undoable after allocation failures");
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        MorphResetFixture tiny;plugins::NativePoseTestBinding inner(*tiny.app,tiny.history);const auto original=tiny.Bits();
        Check(tiny.history.Configure(mode,128,1)==MMS_OK,"configure one-byte reset history budget");tiny.Command();
        Check(!tiny.history.UndoCount()&&!tiny.history.RedoCount()&&!tiny.history.RetainedBytes()&&
            (mode==plugins::SharedHistory::RecordingMode::AllOperations?tiny.Bits()==original:tiny.Zero()),
            "retained reset rejects insufficient budget while explicit-only adjustment applies without retaining a hidden history node");
    }
}
void MorphResetObjectChronology(){
    MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Bits();
    auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);const auto id=plugins::SceneObjectIdentity(f.first.raw());
    std::shared_ptr<plugins::HistoryChange> insertion;std::unique_ptr<plugins::PreparedHistoryChange> stage;
    Check(owner->PrepareInsertion(4096,insertion,stage)==MMS_OK&&stage->IsCurrent(),"prepare real retained-object insertion protocol for reset chronology");stage->Publish();stage.reset();
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("insert reset model",insertion,tx)==MMS_OK&&f.history.CommitNative(std::move(tx))==MMS_OK,
        "publish retained model insertion before its reset");
    f.Command();Check(f.Zero()&&f.history.UndoCount()==2,"reset follows model insertion in the same journal");
    Check(f.history.PrepareNative("delete reset model",owner->CaptureRemoval(4096),tx)==MMS_OK&&f.history.CommitNative(std::move(tx))==MMS_OK&&!f.app->ModelSlot(0),
        "model deletion follows reset in the same journal");
    Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0)==f.first.raw()&&plugins::SceneObjectIdentity(f.first.raw()).generation!=id.generation,
        "undo deletion restores original stable model identity with a new resource generation");
    f.app->SelectedModelSlot()=1;f.app->CurrentFrame()=83;
    Check(f.history.History(false)==MMS_OK&&f.Bits()==before&&f.history.History(false)==MMS_OK&&!f.app->ModelSlot(0),
        "undo reset then insertion works across selected models and changed frames without source reload");
    Check(f.history.History(true)==MMS_OK&&f.app->ModelSlot(0)==f.first.raw()&&f.history.History(true)==MMS_OK&&f.Zero()&&
        f.history.History(true)==MMS_OK&&!f.app->ModelSlot(0),"redo insertion reset deletion preserves exact chronological resource ownership");
    Check(f.history.History(false)==MMS_OK&&owner->Present(),"restore fixture model membership before caller-owned resources are released");
}
struct MorphResetReentry {
    inline static thread_local MorphResetReentry* active=nullptr;
    MorphResetFixture& fixture;HWND main,undo,edit;WNDPROC mainProc,undoProc,editProc;
    bool enableArmed=false,textArmed=false,gate=false,applyChanged=true,resetConsumed=false,unchanged=false;
    unsigned calls=0,mainCommands=0,nestedCommands=0;
    MmsResult undoResult=MMS_OK,applyResult=MMS_OK;
    explicit MorphResetReentry(MorphResetFixture& f):fixture(f),main(f.app->MainWindow()),
        undo(GetDlgItem(main,panel::kUndoButton)),edit(f.Field(0)),
        mainProc(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(main,GWLP_WNDPROC))),
        undoProc(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(undo,GWLP_WNDPROC))),
        editProc(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(edit,GWLP_WNDPROC))){
        Check(!active&&mainProc&&undoProc&&editProc,"install one same-thread native reset reentry observer");active=this;
        Check(SetWindowLongPtrA(main,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Main))!=0&&
            SetWindowLongPtrA(undo,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Undo))!=0&&
            SetWindowLongPtrA(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Edit))!=0,
            "observe actual native button enable and ANSI edit text callbacks without synthesizing notifications");
    }
    ~MorphResetReentry(){SetWindowLongPtrA(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(editProc));
        SetWindowLongPtrA(undo,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(undoProc));
        SetWindowLongPtrA(main,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(mainProc));active=nullptr;}
    static LRESULT CALLBACK Main(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto& self=*active;
        // Only command dispatch uses the production main procedure; all other
        // fixture messages keep the original STATIC native implementation.
        if(message==WM_COMMAND){++self.mainCommands;return MainWndProc(window,message,wp,lp);}
        return CallWindowProcA(self.mainProc,window,message,wp,lp);
    }
    static LRESULT CALLBACK Undo(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto& self=*active;if(message==WM_ENABLE&&wp&&self.enableArmed){self.enableArmed=false;self.Attack();}
        return CallWindowProcA(self.undoProc,window,message,wp,lp);
    }
    static LRESULT CALLBACK Edit(HWND window,UINT message,WPARAM wp,LPARAM lp){
        auto& self=*active;if(message==WM_SETTEXT&&self.textArmed){self.textArmed=false;self.Attack();}
        return CallWindowProcA(self.editProc,window,message,wp,lp);
    }
    void Attack(){
        ++calls;gate=bake::UiLocked();const auto before=fixture.Bits();
        const auto undoBefore=fixture.history.UndoCount(),redoBefore=fixture.history.RedoCount();const auto bytesBefore=fixture.history.RetainedBytes();
        std::uint64_t revisionBefore=0,revisionAfter=0;fixture.history.Revision(revisionBefore);
        const auto commandsBefore=mainCommands;
        SendMessageA(main,WM_COMMAND,MAKEWPARAM(panel::kUndoButton,BN_CLICKED),reinterpret_cast<LPARAM>(undo));
        undoResult=plugins::RunBoundObjectHistory(*fixture.app,fixture.history,false);
        applyResult=plugins::ApplyBoundMorphValue(*fixture.app,0,1,.75f,applyChanged);
        resetConsumed=plugins::TryResetBoundMorphs(fixture.app.get());
        SendMessageA(main,WM_COMMAND,MAKEWPARAM(230,0),0);
        nestedCommands=mainCommands-commandsBefore;fixture.history.Revision(revisionAfter);
        unchanged=fixture.Bits()==before&&fixture.history.UndoCount()==undoBefore&&fixture.history.RedoCount()==redoBefore&&
            fixture.history.RetainedBytes()==bytesBefore&&revisionBefore==revisionAfter;
    }
};
void MorphResetReentryChecks(){
    for(bool noChange:{false,true}){
        MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto original=f.Bits();
        if(noChange){for(unsigned i=0;i<7;++i)f.SetBits(i,0);bool changed=false;
            Check(plugins::ApplyBoundMorphValue(*f.app,0,1,.75f,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,
                "seed redo before a no-change reset echo callback");}
        EnableWindow(GetDlgItem(f.app->MainWindow(),panel::kUndoButton),FALSE);
        f.Type(544,"external bone draft");f.TypeField(0,"reset this draft");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        MorphResetReentry observer(f);observer.enableArmed=!noChange;observer.textArmed=noChange;f.Command();
        Check(observer.calls==1&&observer.gate&&observer.nestedCommands==2&&observer.undoResult==MMS_E_BUSY&&
            observer.applyResult!=MMS_OK&&!observer.applyChanged&&observer.resetConsumed&&observer.unchanged,
            "real undo-button WM_ENABLE and no-op edit echoes refuse nested main undo reset and direct authoring before the first publication hint");
        Check(!bake::UiLocked()&&f.Zero()&&f.history.UndoCount()==(noChange?0u:1u)&&f.history.RedoCount()==(noChange?1u:0u),
            "reset finishes with zero authoring and one changed-operation history entry and releases the temporary input gate");
        for(unsigned lane=0;lane<4;++lane){char text[32]{};GetWindowTextA(f.Field(lane),text,sizeof(text));
            Check(!std::strcmp(text,"0.00000")&&!SendMessageA(f.Field(lane),EM_GETMODIFY,0,0)&&
                SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==0,
                "rejected synchronous edits leave every reset caption thumb and native weight consistent");}
        char draft[32]{};GetWindowTextA(f.Edit(544),draft,sizeof(draft));
        Check(!std::strcmp(draft,"external bone draft")&&SendMessageA(f.Edit(544),EM_GETMODIFY,0,0),
            "publication reentry protection preserves another numeric field's uncommitted draft");
        if(noChange)Check(f.history.History(true)==MMS_OK,"no-op echo gate releases normally before later redo");
        const auto bitsBeforeUndo=f.Bits();auto owner=plugins::RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
        std::uint64_t revisionBeforeUndo=0,revisionAfterUndo=0;f.history.Revision(revisionBeforeUndo);
        const auto undoResult=plugins::RunBoundObjectHistory(*f.app,f.history,false);
        const auto actual=f.Bits();std::array<std::uint32_t,7> expected{};if(!noChange)expected=original;
        f.history.Revision(revisionAfterUndo);
        const bool restored=undoResult==MMS_OK&&actual==expected;
        if(!restored){
            const auto identity=plugins::SceneObjectIdentity(f.first.raw());auto* live=f.app->ModelSlot(0);
            const auto* model=live?mdl::Mdl(live):nullptr;
            std::printf("MORPH_RESET_UNDO_DIAGNOSTIC no_change=%u result=%d frame=%d selected=%u camera=%u undo=%llu redo=%llu revision=%llu/%llu retained=%llu model=%p expected_model=%p table=%p expected_table=%p count=%u pool=%llu identity_generation=%llu owner_present=%u ui_locked=%u mutation=%u playing=%u frame_step=%u record_pending=%u save=%u capture=%p frame_dialog=%p copy_dialog=%p recording=%p gravity=%p shadow=%p accessory_order=%p accessory_edit=%u edge_dialog=%p object_operation=%u object_ui_pending=%u layout=%u\n",
                unsigned(noChange),int(undoResult),f.app->CurrentFrame(),unsigned(f.app->SelectedModelSlot()),unsigned(f.app->CameraMode()),
                static_cast<unsigned long long>(f.history.UndoCount()),static_cast<unsigned long long>(f.history.RedoCount()),
                static_cast<unsigned long long>(revisionBeforeUndo),static_cast<unsigned long long>(revisionAfterUndo),
                static_cast<unsigned long long>(f.history.RetainedBytes()),static_cast<void*>(live),static_cast<void*>(f.first.raw()),
                model?static_cast<void*>(model->morphs):nullptr,static_cast<void*>(f.morphs[0].data()),model?model->morphCount:0,
                static_cast<unsigned long long>(model?mdl::MorphKeyPoolIdentity(model->morphKeys):0),static_cast<unsigned long long>(identity.generation),
                unsigned(owner->Present()),unsigned(bake::UiLocked()),unsigned(f.app->SceneMutationInProgress()),unsigned(f.app->PlaybackActive()),
                unsigned(f.app->FrameStepPlayback()),unsigned(f.app->state.recordPlaybackStartPending),unsigned(SceneSaveInProgress()),
                static_cast<void*>(GetCapture()),static_cast<void*>(f.app->FrameRangeDialog()),static_cast<void*>(f.app->state.frameCopyDialog),
                static_cast<void*>(f.app->RecordingWindow()),static_cast<void*>(f.app->GravitySettingDialog()),static_cast<void*>(f.app->GroundShadowColorDialog()),
                f.app->AccessoryOrderArray(),unsigned(f.app->state.accessoryEditDialogOpen),static_cast<void*>(f.app->state.edgeThicknessDialog),
                unsigned(plugins::NativeObjectOperationActive(f.app.get())),unsigned(plugins::NativeObjectUiPending(f.app.get())),unsigned(f.app->WindowLayoutReady()));
            const auto print=[](const char* label,const auto& bits){std::printf("%s",label);for(auto value:bits)std::printf(" %08X",unsigned(value));std::printf("\n");};
            print("  before",bitsBeforeUndo);print("  actual",actual);print("  expected",expected);
            std::printf("  identity");
            const auto* idBytes=reinterpret_cast<const unsigned char*>(&identity.id);for(unsigned i=0;i<sizeof(MmsId);++i)std::printf(" %02X",unsigned(idBytes[i]));
            std::printf("\n");std::fflush(stdout);
        }
        Check(restored,
            "normal production undo succeeds after the reset echo gate exits");
        if(!noChange){
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&f.Zero()&&f.history.UndoCount()==1&&f.history.RedoCount()==0,
                "normal production redo also publishes reset inside the authorized host's own UI and scene locks");
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&f.Bits()==original,
                "repeated production undo remains available after protected reset redo and readout callbacks");
        }
    }
}
void MorphResetReplayLockChecks(){
    for(unsigned locks:{1u,2u,3u}){
        MorphResetFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto original=f.Bits();f.Command();
        Check(f.Zero()&&f.history.UndoCount()==1,"seed a published reset before nonauthorized replay locks");
        std::uint64_t before=0,after=0;f.history.Revision(before);const auto bytes=f.history.RetainedBytes();
        {
            std::unique_ptr<bake::ScopedUiInputGate> input;std::unique_ptr<ScopedSceneMutation> mutation;
            if(locks&1)input=std::make_unique<bake::ScopedUiInputGate>();
            if(locks&2)mutation=std::make_unique<ScopedSceneMutation>(*f.app);
            Check(!plugins::NativeObjectPublicationAuthorized(f.app.get())&&f.history.History(false)==MMS_E_BUSY,
                "an ordinary input gate scene-mutation lock or their combination cannot authorize published reset replay");
            bool changed=true;Check(plugins::ApplyBoundMorphValue(*f.app,0,1,.75f,changed)!=MMS_OK&&!changed,
                "nonauthorized replay locks still reject new scalar authoring");
            Check(plugins::TryResetBoundMorphs(f.app.get())&&f.Zero(),"nonauthorized replay locks consume a new reset without fallback");
            f.history.Revision(after);
            Check(before==after&&f.history.RetainedBytes()==bytes&&f.history.UndoCount()==1&&!f.history.RedoCount()&&f.Zero(),
                "rejected locked replay and new adjustments preserve complete reset history and current weights");
        }
        Check(plugins::RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&f.Bits()==original,
            "the real history command can undo after unrelated locks have been released");
        Check(plugins::RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&f.Zero(),
            "the real history command can redo inside its explicitly authorized protection scopes");
    }
}
}
void NativeMorphResetChecks(){MorphResetParityAndHistory();MorphResetCancellationAndOwnership();MorphResetStaleAndGuardChecks();MorphResetFailureAndBudgetChecks();MorphResetObjectChronology();MorphResetReentryChecks();MorphResetReplayLockChecks();}
