// Include after native_morph_text_checks.inc.
#include "slider_readout_spy.hpp"
#include "../src/plugins/native_object_commands.hpp"
namespace {
struct MorphSliderFixture : MorphTextFixture {
    MorphSliderFixture(){
        first.m->physicsMode=second.m->physicsMode=2;
        // The base captured the native ANSI EDIT cookie while the four fields
        // were still plain class windows. Install the actual production handler
        // on every lane so the fixture matches the running UI.
        const auto nativeEditProc=app->OriginalEditProc();
        for(unsigned lane=0;lane<4;++lane){
            const int id=506+int(lane)*5;const auto field=Field(lane);
            InstallControlSubclass(app.get(),field,id);
            Check(!IsWindowUnicode(field),"morph slider fixture installs one ANSI EDIT per lane");
            Check(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(field,GWLP_WNDPROC))==EditSubclassProc,
                "morph slider fixture installs the actual production EDIT subclass");
            Check(app->OriginalEditProc()==nativeEditProc,"installing the production EDIT subclass preserves the base ANSI cookie");
        }
    }
    HWND Slider(unsigned lane){return GetDlgItem(app->MainWindow(),505+int(lane)*5);}
    void Scroll(unsigned lane,int tick,unsigned code=TB_THUMBTRACK){
        g_Block=app.get();SendMessageW(Slider(lane),TBM_SETPOS,TRUE,tick);
        HandleHScroll(reinterpret_cast<LPARAM>(Slider(lane)),MAKEWPARAM(code,0));
    }
    bool Thumb(unsigned lane,float weight){return SendMessageW(Slider(lane),TBM_GETPOS,0,0)==
        std::clamp(int(double(weight)*100.0),0,100);}
};
void MorphSliderParityChecks(){
    for(unsigned lane=0;lane<4;++lane)for(unsigned slot=0;slot<2;++slot)for(int tick:{-1,0,1,53,75,100,101}){
        MorphSliderFixture f;f.app->SelectedModelSlot()=std::uint8_t(slot);
        auto& target=slot?f.second:f.first;const auto index=target.m->selectedMorphs[lane];
        const float before=target.m->morphs[index].value;
        const auto keys=MorphKeysCopy(target);const auto ring=PoseRingDigest(target);const auto other=slot?f.firstMorphs:f.secondMorphs;
        plugins::NativePoseTestBinding binding(*f.app,f.history);f.Scroll(lane,tick);
        const int effective=std::clamp(tick,0,100);const float expected=float(double(effective)/100.0);
        Check(MorphSameFloat(target.m->morphs[index].value,expected)&&f.history.UndoCount()==1&&
            SendMessageW(f.Slider(lane),TBM_GETPOS,0,0)==effective,"four bound morph lanes keep native clamping double percent and submitted tick");
        for(unsigned code:{unsigned(TB_LINEUP),unsigned(TB_THUMBPOSITION),unsigned(TB_ENDTRACK)}){
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Scroll(lane,tick,code);
            Check(f.history.UndoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"duplicate morph slider messages do not append history or mark dirty");
        }
        f.app->SelectedModelSlot()=std::uint8_t(1-slot);f.app->CameraMode()=1;f.app->CurrentFrame()=41;
        target.m->selectedMorphs[lane]=(index+1)%3;
        Check(f.history.History(false)==MMS_OK&&MorphSameFloat(target.m->morphs[index].value,before),"morph slider undo restores exact original float across selected models channels and modes");
        Check(f.history.History(true)==MMS_OK&&MorphSameFloat(target.m->morphs[index].value,expected)&&f.app->CurrentFrame()==41,
            "morph slider redo retains authored value without moving playhead");
        Check(MorphKeysMatch(target,keys)&&PoseRingDigest(target)==ring&&
            !std::memcmp((slot?f.firstMorphs:f.secondMorphs).data(),other.data(),sizeof(other)),"morph slider history preserves unrelated weights keys and legacy pose ring");
    }
}
void MorphSliderHistoryChecks(){
    MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto initial=f.firstMorphs[1].value;
    f.Scroll(1,53);f.app->SelectedModelSlot()=1;f.Scroll(2,69);
    Check(f.history.UndoCount()==2&&f.history.History(false)==MMS_OK&&f.history.History(false)==MMS_OK&&f.firstMorphs[1].value==initial,
        "cross-model morph slider positions share chronological common history");
    f.app->SelectedModelSlot()=0;f.Scroll(1,int(double(initial)*100.0));
    // The actual tick is a new authored value; return to the precisely seeded state.
    Check(f.history.History(false)==MMS_OK&&f.history.Reset()==MMS_OK,"reset cross-model morph slider case");
    f.Scroll(1,53);f.CommitField(1,".75");Check(f.history.History(false)==MMS_OK,"seed later morph text redo");
    f.app->SceneModified()=0;f.Scroll(1,53,TB_ENDTRACK);
    Check(f.history.RedoCount()==1&&!f.app->SceneModified()&&SendMessageW(f.Slider(1),TBM_GETPOS,0,0)==53,
        "same morph position preserves later text redo and float-boundary tick");
    plugins::RefreshBoundMorphReadout(*f.app);g_Block=f.app.get();
    HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(1)),TB_ENDTRACK);
    Check(f.history.RedoCount()==1&&!f.app->SceneModified()&&f.firstMorphs[1].value==.53f&&SendMessageW(f.Slider(1),TBM_GETPOS,0,0)==53,
        "safe refresh followed by ENDTRACK cannot drift a matching morph tick or truncate later redo");
    Check(f.history.Enter()==MMS_OK,"hold history while a moved morph tick is rejected");
    f.Scroll(1,90);f.history.Leave();
    g_Block=f.app.get();HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(1)),TB_ENDTRACK);
    Check(f.history.RedoCount()==1&&!f.app->SceneModified()&&f.firstMorphs[1].value==.53f&&SendMessageW(f.Slider(1),TBM_GETPOS,0,0)==53,
        "rejected moved tick followed by ENDTRACK preserves original hundredth weight and redo");
    Check(mdl::MorphKeyPoolEnsure(*f.first.m,17)&&f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,
        "morph slider history survives identity-preserving key pool growth");
    mdl::MorphKeyPoolDestroy(f.first.m->morphKeys);f.first.m->morphKeys=mdl::MorphKeyPoolCreate(32);
    Check(f.history.History(false)==MMS_E_STALE&&f.firstMorphs[1].value==.53f&&f.history.UndoCount()==1,
        "replacement morph pool refuses slider history without cursor movement");
}
void MorphSliderFailureChecks(){
    for(unsigned lane=0;lane<4;++lane){
        MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.Scroll(lane,75);
        Check(f.history.History(false)==MMS_OK,"seed morph slider allocation redo");
        const auto weights=f.firstMorphs;const auto keys=MorphKeysCopy(f.first);const auto ring=PoseRingDigest(f.first);
        const auto index=f.first.m->selectedMorphs[lane];f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;
        bool success=false;unsigned failures=0;
        for(long limit=0;limit<128;++limit){SendMessageW(f.Slider(lane),TBM_SETPOS,TRUE,53);g_Block=f.app.get();failAfter=limit;
            HandleHScroll(reinterpret_cast<LPARAM>(f.Slider(lane)),TB_THUMBTRACK);failAfter=-1;
            if(f.history.UndoCount()){success=true;break;}++failures;
            Check(!std::memcmp(weights.data(),f.firstMorphs.data(),sizeof(weights))&&f.history.RedoCount()==1&&f.Thumb(lane,weights[index].value)&&
                !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&MorphKeysMatch(f.first,keys)&&PoseRingDigest(f.first)==ring&&f.SelectionUnchanged(),
                "morph slider allocation failure restores thumb and keeps author pose keys redo flags and selection");
            Check(f.history.Enter()==MMS_OK,"failed morph slider releases journal gate");f.history.Leave();
        }
        Check(success&&failures>=5&&f.firstMorphs[index].value==.53f,"morph slider allocation sweep reaches atomic success");
        Check(f.history.History(false)==MMS_OK&&MorphSameFloat(f.firstMorphs[index].value,weights[index].value),"morph slider remains exactly undoable after failed preparations");
    }
}
void MorphSliderGuardChecks(){
    MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=f.firstMorphs;
    for(int gate=0;gate<9;++gate){
        if(gate==0)f.app->PlaybackActive()=1;if(gate==1)f.app->FrameStepPlayback()=1;if(gate==2)f.app->CameraMode()=1;
        if(gate==3)f.app->state.recordPlaybackStartPending=true;if(gate==4)f.app->FrameRangeDialog()=reinterpret_cast<HWND>(INT_PTR(1));
        if(gate==5)f.app->state.accessoryEditDialogOpen=true;if(gate==6){Check(f.history.Enter()==MMS_OK,"hold morph journal");}
        if(gate==7){SetCapture(f.Field(1));Check(GetCapture()==f.Field(1),"unrelated morph edit capture");}
        if(gate==8){f.first.m->selectedMorphs[1]=0;f.first.m->physicsMode=0;}
        f.TypeField(1,"pending");f.Scroll(1,90);
        char text[32]{};GetWindowTextA(f.Field(1),text,sizeof(text));
        Check(!std::memcmp(before.data(),f.firstMorphs.data(),sizeof(before))&&!f.history.UndoCount()&&
            f.Thumb(1,before[f.first.m->selectedMorphs[1]].value)&&!std::strcmp(text,"pending"),
            "guarded morph slider rejects legacy fallback restores thumb and preserves pending edit text");
        f.app->PlaybackActive()=f.app->FrameStepPlayback()=f.app->CameraMode()=0;f.app->state.recordPlaybackStartPending=false;
        f.app->FrameRangeDialog()=nullptr;f.app->state.accessoryEditDialogOpen=false;
        if(gate==6)f.history.Leave();if(gate==7)ReleaseCapture();f.first.m->selectedMorphs[1]=1;f.first.m->physicsMode=2;
    }
    SendMessageW(f.Field(1),EM_SETMODIFY,FALSE,0);SetCapture(f.Slider(1));Check(GetCapture()==f.Slider(1),"own morph slider capture");f.Scroll(1,53);
    Check(f.history.UndoCount()==1&&f.firstMorphs[1].value==.53f,"own morph slider capture permits author edit");
    bool changed=true;Check(plugins::ApplyBoundMorphValue(*f.app,0,1,.9f,changed)!=MMS_OK&&!changed,"ordinary morph API cannot borrow captured slider privilege");
    f.Scroll(2,90);Check(f.history.UndoCount()==1&&f.firstMorphs[2].value==before[2].value,"other morph lane cannot borrow captured slider privilege");ReleaseCapture();
    Check(plugins::TryCommitBoundMorphSlider(f.app.get(),f.Field(1))&&f.history.UndoCount()==1,"wrong morph source consumed without edits");
    const auto fake=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD,0,0,80,20,f.app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(505)),GetModuleHandleW(nullptr),nullptr);
    Check(fake&&plugins::TryCommitBoundMorphSlider(f.app.get(),fake)&&f.history.UndoCount()==1,"duplicate control ID is not a registered morph source");DestroyWindow(fake);
}
struct MorphEnterControl {
    MorphSliderFixture& fixture;HWND edit,focus;WNDPROC previous,saved;
    inline static thread_local MorphEnterControl* active=nullptr;
    unsigned lostFocus=0;HWND nextFocus=nullptr;
    LRESULT modifyBeforeEnter=-1,modifyAtLoss=-1,modifyAfterLoss=-1,modifyAfterEnter=-1;
    char textBeforeEnter[64]{};
    static LRESULT CALLBACK ObserveFocus(HWND window,UINT message,WPARAM wp,LPARAM lp){
        Check(active&&active->edit==window,"ANSI morph focus observer belongs to this thread and this original EDIT");
        auto& self=*active;
        if(message==WM_KILLFOCUS){++self.lostFocus;self.nextFocus=reinterpret_cast<HWND>(wp);
            self.modifyAtLoss=SendMessageA(window,EM_GETMODIFY,0,0);}
        const auto result=CallWindowProcA(self.previous,window,message,wp,lp);
        if(message==WM_KILLFOCUS)self.modifyAfterLoss=SendMessageA(window,EM_GETMODIFY,0,0);
        return result;
    }
    explicit MorphEnterControl(MorphSliderFixture& f,unsigned lane):fixture(f),edit(f.Field(lane)),focus(GetFocus()),
        previous(reinterpret_cast<WNDPROC>(GetWindowLongPtrA(edit,GWLP_WNDPROC))),saved(f.app->OriginalEditProc()){
        Check(!active&&!IsWindowUnicode(edit),"morph Enter starts with one original ANSI EDIT on the current thread");
        Check(previous==EditSubclassProc&&saved!=nullptr,"morph Enter fixture retains the installed production EDIT subclass above the native ANSI cookie");
        active=this;
        Check(SetWindowLongPtrA(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ObserveFocus))==reinterpret_cast<LONG_PTR>(previous),
            "install ANSI focus observer forwarding every message once through the actual production EDIT handler");
        Check(!IsWindowUnicode(edit)&&fixture.app->OriginalEditProc()==saved,
            "ANSI focus observer preserves EDIT encoding and the original native ANSI procedure");
    }
    ~MorphEnterControl(){Check(!IsWindowUnicode(edit)&&fixture.app->OriginalEditProc()==saved,
            "morph Enter leaves the original ANSI encoding and native procedure intact");
        SetWindowLongPtrA(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previous));active=nullptr;
        Check(!IsWindowUnicode(edit)&&reinterpret_cast<WNDPROC>(GetWindowLongPtrA(edit,GWLP_WNDPROC))==previous&&
            fixture.app->OriginalEditProc()==saved,
            "removing the focus observer restores exactly the production ANSI EDIT subclass above the native cookie");
        if(focus&&IsWindow(focus))SetFocus(focus);else SetFocus(nullptr);}
    void Focus(){
        // Use only this thread's hidden fixture. Never show or activate a
        // window, or change the user's foreground window, to obtain focus.
        SetFocus(edit);Check(GetFocus()==edit,"real morph EDIT owns focus before entering text");
    }
    void PressEnter(){
        Check(GetFocus()==edit,"real morph EDIT retains focus immediately before Enter");
        Check(!IsWindowUnicode(edit)&&fixture.app->OriginalEditProc()==saved,"Enter starts with the unchanged native ANSI EDIT chain");
        lostFocus=0;nextFocus=nullptr;
        modifyBeforeEnter=SendMessageA(edit,EM_GETMODIFY,0,0);GetWindowTextA(edit,textBeforeEnter,sizeof(textBeforeEnter));
        SendMessageA(edit,WM_KEYDOWN,VK_RETURN,0);
        modifyAfterEnter=SendMessageA(edit,EM_GETMODIFY,0,0);
        Check(!IsWindowUnicode(edit)&&fixture.app->OriginalEditProc()==saved,"Enter preserves the native ANSI EDIT chain");
        Check(GetFocus()==fixture.app->MainWindow(),"production Enter transfers native focus to the main window");
        Check(lostFocus==1&&nextFocus==fixture.app->MainWindow(),
            "production Enter causes exactly one real WM_KILLFOCUS to commit the morph edit");
    }
    void Enter(unsigned lane,const char* text){Focus();fixture.TypeField(lane,text);PressEnter();}
};
void MorphTextPercentThumbChecks(){
    using Mode=plugins::SharedHistory::RecordingMode;
    struct Input {const char* text;int tick;float authored;};
    const Input input[]={{"0.29",29,.29f},{"0.57",57,.57f},{"0.58",58,.58f},{"0.53",53,.53f},{"0.59",59,.59f},{".6543219",65,.654321f}};
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned lane=0;lane<4;++lane)for(unsigned slot=0;slot<2;++slot)for(const auto& value:input){
        MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->SelectedModelSlot()=std::uint8_t(slot);
        Check(f.history.SetMode(mode)==MMS_OK,"configure actual morph Enter adjustment recording policy");auto& target=slot?f.second:f.first;
        const int index=target.m->selectedMorphs[lane];const float before=target.m->morphs[index].value;
        const auto keys=MorphKeysCopy(target);const auto other=slot?f.firstMorphs:f.secondMorphs;const auto ring=PoseRingDigest(target);
        MorphEnterControl control(f,lane);slider_readout_probe::Spy slider;Check(slider.Watch(f.Slider(lane)),"observe every real native thumb-position write during EDIT Enter");
        SendMessageW(f.Slider(lane),TBM_SETPOS,FALSE,0);slider.Clear();control.Enter(lane,value.text);
        const auto actualTick=SendMessageW(f.Slider(lane),TBM_GETPOS,0,0);
        const bool accepted=MorphSameFloat(target.m->morphs[index].value,value.authored)&&actualTick==value.tick&&
            slider.counts.positions==1&&slider.counts.lastPosition==value.tick&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u);
        if(!accepted){
            std::uint32_t actualBits=0,expectedBits=0;std::memcpy(&actualBits,&target.m->morphs[index].value,sizeof(actualBits));
            std::memcpy(&expectedBits,&value.authored,sizeof(expectedBits));char textAfter[64]{};GetWindowTextA(control.edit,textAfter,sizeof(textAfter));
            std::printf("MORPH_ENTER_DIAGNOSTIC mode=%s lane=%u slot=%u input=%s before_text=%s after_text=%s actual=%.9g expected=%.9g bits=%08X/%08X tick=%lld expected_tick=%d positions=%u last=%lld undo=%llu redo=%llu focus=%p main=%p loss=%u next=%p modify=%lld/%lld/%lld/%lld original=%p previous=%p installed=%p edit_unicode=%d dirty=%u recorded=%u\n",
                mode==Mode::AllOperations?"all":"explicit",lane,slot,value.text,control.textBeforeEnter,textAfter,
                double(target.m->morphs[index].value),double(value.authored),actualBits,expectedBits,static_cast<long long>(actualTick),value.tick,
                slider.counts.positions,static_cast<long long>(slider.counts.lastPosition),static_cast<unsigned long long>(f.history.UndoCount()),
                static_cast<unsigned long long>(f.history.RedoCount()),static_cast<void*>(GetFocus()),static_cast<void*>(f.app->MainWindow()),
                control.lostFocus,static_cast<void*>(control.nextFocus),static_cast<long long>(control.modifyBeforeEnter),
                static_cast<long long>(control.modifyAtLoss),static_cast<long long>(control.modifyAfterLoss),static_cast<long long>(control.modifyAfterEnter),
                reinterpret_cast<void*>(f.app->OriginalEditProc()),reinterpret_cast<void*>(control.previous),
                reinterpret_cast<void*>(GetWindowLongPtrA(control.edit,GWLP_WNDPROC)),int(IsWindowUnicode(control.edit)),
                unsigned(f.app->SceneModified()),unsigned(f.app->RecordedFrameCountByte1()));std::fflush(stdout);
        }
        Check(accepted,
            "real EDIT Enter authors the original seven-character double-to-float value and writes its one canonical displayed tick; .29/.57/.58 never visit the adjacent lower tick");
        slider.Clear();for(unsigned repeat=0;repeat<3;++repeat)plugins::RefreshBoundMorphReadout(*f.app);
        Check(slider.counts.positions==0&&SendMessageW(f.Slider(lane),TBM_GETPOS,0,0)==value.tick&&MorphSameFloat(target.m->morphs[index].value,value.authored),
            "repeated native morph readout preserves the accepted thumb and precise author without redundant position redraws");
        slider.Clear();f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;control.Enter(lane,value.text);
        Check(slider.counts.positions==0&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u)&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
            MorphSameFloat(target.m->morphs[index].value,value.authored),"equal real EDIT Enter adds no history and never forces an extra thumb redraw or narrowed precision write");
        if(mode==Mode::AllOperations){
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&MorphSameFloat(target.m->morphs[index].value,before),"actual guarded morph undo restores original precise author after Enter");
            slider.Clear();Check(plugins::RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&MorphSameFloat(target.m->morphs[index].value,value.authored)&&
                SendMessageW(f.Slider(lane),TBM_GETPOS,0,0)==value.tick&&slider.counts.positions==1&&slider.counts.lastPosition==value.tick,
                "actual guarded morph redo uses the same canonical thumb tick without the old parsed-double second write");
        }
        Check(MorphKeysMatch(target,keys)&&PoseRingDigest(target)==ring&&!std::memcmp((slot?f.firstMorphs:f.secondMorphs).data(),other.data(),sizeof(other)),
            "morph display-only correction leaves all native keys, legacy rings and the other model authoring intact");
    }
    {MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const unsigned lane=1;MorphEnterControl control(f,lane);
        f.CommitField(lane,".75");Check(f.history.History(false)==MMS_OK,"seed morph Enter failure redo");plugins::RefreshBoundMorphReadout(*f.app);
        const auto before=f.firstMorphs;const auto tick=SendMessageW(f.Slider(lane),TBM_GETPOS,0,0);f.TypeField(2,"pending draft");
        slider_readout_probe::Spy slider;Check(slider.Watch(f.Slider(lane)),"observe native slider while Enter preparation is refused");
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;control.Focus();f.TypeField(lane,"0.29");slider.Clear();failAfter=0;
        control.PressEnter();failAfter=-1;char draft[64]{};GetWindowTextA(f.Field(2),draft,sizeof(draft));
        Check(!std::memcmp(before.data(),f.firstMorphs.data(),sizeof(before))&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
            SendMessageW(f.Slider(lane),TBM_GETPOS,0,0)==tick&&!slider.counts.positions&&!std::strcmp(draft,"pending draft")&&
            SendMessageW(f.Field(2),EM_GETMODIFY,0,0)&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),
            "failed real EDIT Enter preserves precise author, original thumb, redo and another lane's uncommitted draft without publishing a transient requested tick");
    }
}
}
void NativeMorphSliderChecks(){MorphSliderParityChecks();MorphSliderHistoryChecks();MorphSliderFailureChecks();MorphSliderGuardChecks();MorphTextPercentThumbChecks();}
