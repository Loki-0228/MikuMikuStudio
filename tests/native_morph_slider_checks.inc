// Include after native_morph_text_checks.inc.
namespace {
struct MorphSliderFixture : MorphTextFixture {
    MorphSliderFixture(){first.m->physicsMode=second.m->physicsMode=2;}
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
}
void NativeMorphSliderChecks(){MorphSliderParityChecks();MorphSliderHistoryChecks();MorphSliderFailureChecks();MorphSliderGuardChecks();}
