#include "../src/plugins/native_morph_commands.hpp"
#include <commctrl.h>
namespace {
struct MorphTextFixture : MorphKeyFixture {
    MorphTextFixture(){
        for(auto* f:{&first,&second}){for(int lane=0;lane<4;++lane)f->m->selectedMorphs[lane]=lane%3;
            for(unsigned i=0;i<3;++i)f->m->morphs[i].value=.12345678f+float(i)*.1f;}
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES};InitCommonControlsEx(&common);
        for(int lane=0;lane<4;++lane){const int id=506+lane*5;
            Check(CreateWindowExA(0,"EDIT","0",WS_CHILD,0,0,80,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"morph numeric control exists");
            const auto slider=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD,0,0,80,20,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id-1)),GetModuleHandleW(nullptr),nullptr);
            Check(slider!=nullptr,"morph slider exists");SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(0,100));}
    }
    HWND Field(unsigned lane){return GetDlgItem(app->MainWindow(),506+int(lane)*5);}
    void TypeField(unsigned lane,const char* text){SetWindowTextA(Field(lane),text);SendMessageA(Field(lane),EM_SETMODIFY,TRUE,0);}
    void CommitField(unsigned lane,const char* text){TypeField(lane,text);CommitEditControl(app.get(),Field(lane));}
    void LoseField(unsigned lane){EditSubclassProc(Field(lane),WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);}
};
void MorphTextRoutingChecks(){
    for(unsigned lane=0;lane<4;++lane)for(unsigned slot=0;slot<2;++slot){
        MorphTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.app->SelectedModelSlot()=std::uint8_t(slot);
        auto& target=slot?f.second:f.first;auto& other=slot?f.firstMorphs:f.secondMorphs;const auto otherBefore=other;
        const auto index=target.m->selectedMorphs[lane];const auto before=target.m->morphs[index].value;const auto keys=MorphKeysCopy(target);const auto ring=PoseRingDigest(target);
        f.TypeField(lane,".6543219");f.LoseField(lane);
        Check(target.m->morphs[index].value==.654321f&&f.history.UndoCount()==1&&f.SelectionUnchanged()&&
            SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==65,"four morph text lanes keep seven-character float conversion and update paired slider without selecting tracks");
        f.LoseField(lane);CommitEditControl(f.app.get(),f.Field(lane));
        Check(target.m->morphs[index].value==.654321f&&f.history.UndoCount()==1,"morph rounded echo does not overwrite precise authoring");
        f.app->SelectedModelSlot()=std::uint8_t(1-slot);target.m->selectedMorphs[lane]=(index+1)%3;f.app->CurrentFrame()=37;f.app->CameraMode()=1;
        Check(f.history.History(false)==MMS_OK&&target.m->morphs[index].value==before&&f.app->CurrentFrame()==37,"morph text undo resolves original model and morph after selection mode and playhead changes");
        f.app->CameraMode()=0;f.app->SelectedModelSlot()=std::uint8_t(slot);target.m->selectedMorphs[lane]=index;
        for(const char* invalid:{"NaN","inf","","3 junk","1e999","1e100"}){f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.CommitField(lane,invalid);
            Check(target.m->morphs[index].value==before&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"invalid or float-overflow morph text preserves weight dirty flags and redo");}
        const std::string oversized(270,'1');f.CommitField(lane,oversized.c_str());Check(target.m->morphs[index].value==before&&f.history.RedoCount()==1,"oversized morph text rejected");
        Check(f.history.History(true)==MMS_OK&&target.m->morphs[index].value==.654321f,"morph redo survives rejected text");
        f.app->SceneModified()=0;f.CommitField(lane,".6543219");Check(f.history.UndoCount()==1&&!f.app->SceneModified(),"same morph weight is no-op");
        f.CommitField(lane,"0.53");Check(target.m->morphs[index].value==.53f&&SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==53,
            "initial morph slider keeps parsed-double percent rather than narrowed-float off-by-one");
        plugins::RefreshBoundMorphReadout(*f.app);Check(SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==52,
            "later morph readout follows exact stored float using native row conversion");
        f.app->SceneModified()=0;f.CommitField(lane,"0.53");Check(f.history.UndoCount()==2&&!f.app->SceneModified()&&SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==53,
            "same-weight morph text preserves initial slider convention without another history step");
        f.CommitField(lane,"2.5");Check(target.m->morphs[index].value==2.5f&&SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==100,"morph authoring above one is retained while slider clamps display");
        f.CommitField(lane,"-1.25");Check(target.m->morphs[index].value==-1.25f&&SendDlgItemMessageW(f.app->MainWindow(),505+int(lane)*5,TBM_GETPOS,0,0)==0,"negative morph authoring remains supported");
        Check(MorphKeysMatch(target,keys)&&PoseRingDigest(target)==ring&&!std::memcmp(other.data(),otherBefore.data(),sizeof(otherBefore)),"morph scalar edits preserve keys model undo ring and other model morphs");
    }
}
void MorphTextFailureChecks(){
    for(unsigned lane=0;lane<4;++lane){MorphTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto index=f.first.m->selectedMorphs[lane];
        f.CommitField(lane,".75");Check(f.history.History(false)==MMS_OK,"seed morph text redo");const auto before=f.firstMorphs;const auto keys=MorphKeysCopy(f.first);const auto ring=PoseRingDigest(f.first);
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;unsigned failures=0;bool succeeded=false;
        for(long limit=0;limit<128;++limit){f.TypeField(lane,".6543219");failAfter=limit;CommitEditControl(f.app.get(),f.Field(lane));failAfter=-1;
            if(f.history.UndoCount()){succeeded=true;break;}++failures;
            Check(!std::memcmp(before.data(),f.firstMorphs.data(),sizeof(before))&&MorphKeysMatch(f.first,keys)&&PoseRingDigest(f.first)==ring&&f.history.RedoCount()==1&&
                !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.SelectionUnchanged(),"morph preparation failure retains all weights keys ring dirty flags selection and redo");
            Check(f.history.Enter()==MMS_OK,"morph failure releases journal gate");f.history.Leave();}
        Check(succeeded&&failures>=5&&f.firstMorphs[index].value==.654321f,"morph allocation failure sweep reaches atomic commit");}
}
void MorphTextIdentityChecks(){
    for(int change=0;change<7;++change){MorphTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.CommitField(1,".75");
        const auto table=f.first.m->morphs;const auto count=f.first.m->morphCount;
        if(change==0)f.app->ModelSlot(0)=f.second.raw();
        if(change==1)f.first.m->morphs=f.secondMorphs.data();
        if(change==2)--f.first.m->morphCount;
        if(change==3)f.firstMorphs[1].name[0]='x';
        if(change==4)f.firstMorphs[1].panel=mdl::MorphPanel::mouth;
        if(change==5)++f.firstMorphs[1].type;
        if(change==6){plugins::ForgetSceneObject(f.first.raw());plugins::SceneObjectIdentity(f.first.raw());}
        Check(f.history.History(false)==MMS_E_STALE&&f.firstMorphs[1].value==.75f&&f.history.UndoCount()==1,"replaced model table count morph identity or object ID rejects old scalar undo");
        f.app->ModelSlot(0)=f.first.raw();f.first.m->morphs=table;f.first.m->morphCount=count;}
    MorphTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.CommitField(1,".75");
    Check(mdl::MorphKeyPoolEnsure(*f.first.m,17)&&f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,"morph scalar record survives identity-preserving key pool growth");
    mdl::MorphKeyPoolDestroy(f.first.m->morphKeys);f.first.m->morphKeys=mdl::MorphKeyPoolCreate(16);
    Check(f.history.History(false)==MMS_E_STALE&&f.firstMorphs[1].value==.75f&&f.history.UndoCount()==1,"new morph key pool rejects previous scene scalar history");
}
void MorphTextPendingAndGuardChecks(){
    MorphTextFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto original=f.firstMorphs;
    f.TypeField(1,".87654");plugins::RefreshBoundMorphReadout(*f.app);char text[64]{};GetWindowTextA(f.Field(1),text,sizeof(text));
    Check(!std::strcmp(text,".87654")&&SendMessageA(f.Field(1),EM_GETMODIFY,0,0),"readout refresh preserves another lane's pending user input");
    f.CommitField(0,".25");GetWindowTextA(f.Field(1),text,sizeof(text));
    Check(!std::strcmp(text,".87654")&&f.firstMorphs[1].value==original[1].value,"committing one morph does not discard another lane's uncommitted text");
    f.LoseField(1);Check(f.history.UndoCount()==2&&f.firstMorphs[1].value==.87654f,"pending second morph text commits separately");
    Check(f.history.Reset()==MMS_OK,"clear morph pending text case");
    for(int guard=0;guard<4;++guard){const auto before=f.firstMorphs;
        if(guard==0)f.app->PlaybackActive()=1;if(guard==1)f.app->CameraMode()=1;if(guard==2)f.app->state.recordPlaybackStartPending=true;if(guard==3)f.first.m->selectedMorphs[0]=999;
        f.CommitField(0,".5");Check(!std::memcmp(before.data(),f.firstMorphs.data(),sizeof(before))&&!f.history.UndoCount(),"playback camera mode pending recording or invalid morph selector cannot reach legacy writer");
        f.app->PlaybackActive()=f.app->CameraMode()=0;f.app->state.recordPlaybackStartPending=false;f.first.m->selectedMorphs[0]=0;}
    Check(f.history.Enter()==MMS_OK,"hold journal for morph busy check");f.CommitField(0,".5");f.history.Leave();Check(!f.history.UndoCount(),"busy journal refuses morph scalar edit");
}
}
void NativeMorphTextChecks(){MorphTextRoutingChecks();MorphTextFailureChecks();MorphTextIdentityChecks();MorphTextPendingAndGuardChecks();}
