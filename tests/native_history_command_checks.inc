// Include after native_pose_scope_checks.inc. Exercise production routing with
// hidden controls and explicit key snapshots; never activate the user's window.
#include "../src/plugins/native_history_commands.hpp"
namespace mikudancestudio { void DispatchLetterHotkeys(MMDApp*,HWND); }
namespace {
void PressHistoryLetter(PoseNumericFixture& f,int key,HWND focus=nullptr) {
    unsigned char keys[256]{};f.app->LetterHotkeys().Update(keys,true);
    keys[key]=0x80;f.app->LetterHotkeys().Update(keys,true);
    f.app->state.ctrlModifierState=3;DispatchLetterHotkeys(f.app.get(),focus);
}
void BoundHistoryButtonChecks() {
    PoseNumericFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto firstRing=PoseRingDigest(f.first),secondRing=PoseRingDigest(f.second);
    f.Commit(panel::kReadoutPosXEdit,"5.1234");
    f.app->SelectedModelSlot()=1;f.Commit(panel::kReadoutPosYEdit,"9");
    f.app->SelectedModelSlot()=0;f.app->state.currentFrame=27;
    CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
    Check(f.second.bones[0].trans[1]==0&&f.first.bones[0].trans[0]==5.1234f&&
        f.history.UndoCount()==1&&f.history.RedoCount()==1,
        "actual undo button restores latest model independently of current selection");
    Check(f.app->SelectedModelSlot()==0&&f.app->state.currentFrame==27&&f.SelectionUnchanged()&&
        f.app->PhysicsResetPending()==1&&IsWindowEnabled(f.Edit(panel::kUndoButton))&&IsWindowEnabled(f.Edit(panel::kRedoButton)),
        "common undo updates buttons and physics without moving selection or playback head");
    // The global queue is available even when no model is selected.
    f.app->CameraMode()=1;f.app->SelectedModelSlot()=255;
    CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
    Check(f.first.bones[0].trans[0]==0&&f.history.UndoCount()==0&&f.history.RedoCount()==2&&
        !IsWindowEnabled(f.Edit(panel::kUndoButton)),"camera-mode button undoes the earlier model and disables exhausted undo");
    CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
    Check(f.first.bones[0].trans[0]==5.1234f&&f.history.UndoCount()==1,"camera-mode redo restores first common record");
    f.app->CameraMode()=0;
    CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
    Check(f.second.bones[0].trans[1]==9&&f.history.RedoCount()==0&&!IsWindowEnabled(f.Edit(panel::kRedoButton))&&
        f.app->SelectedModelSlot()==255&&f.app->state.currentFrame==27,
        "redo tolerates absent selected model without changing selection or frame");
    Check(PoseRingDigest(f.first)==firstRing&&PoseRingDigest(f.second)==secondRing&&f.SelectionUnchanged(),
        "button history leaves both legacy rings and unrelated tracks untouched");
}
void BoundHistoryKeyboardChecks() {
    PoseNumericFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    f.Commit(panel::kReadoutPosXEdit,"3");
    PressHistoryLetter(f,'Z',f.Edit(panel::kReadoutPosXEdit));
    Check(f.history.UndoCount()==1&&f.first.bones[0].trans[0]==3,"Ctrl+Z in text edit stays with the text field");
    for(int id:{panel::kReadoutScaleXEdit,panel::kReadoutScaleYEdit,panel::kReadoutScaleZEdit}) {
        PressHistoryLetter(f,'Z',f.Edit(id));PressHistoryLetter(f,'X',f.Edit(id));
        Check(f.history.UndoCount()==1&&f.first.bones[0].trans[0]==3,"scale text fields suppress common undo and redo shortcuts");
    }
    f.app->CameraMode()=1;f.app->SelectedModelSlot()=255;
    PressHistoryLetter(f,'Z',f.Edit(panel::kUndoButton));
    Check(f.history.UndoCount()==0&&f.first.bones[0].trans[0]==0,"actual Ctrl+Z ladder uses common history in camera mode");
    PressHistoryLetter(f,'X');
    Check(f.history.UndoCount()==1&&f.first.bones[0].trans[0]==3,"existing Ctrl+X redo shortcut uses common history with cleared focus");
    f.app->CameraMode()=0;f.app->SelectedModelSlot()=1;
    PressHistoryLetter(f,'Z');
    Check(f.first.bones[0].trans[0]==0&&f.app->SelectedModelSlot()==1&&f.SelectionUnchanged(),
        "keyboard common undo targets edited model and preserves selected model");
}
void BoundHistoryRejectionChecks() {
    PoseNumericFixture f;
    Check(!plugins::TryBoundHistoryCommand(f.app.get(),false),"unbound application leaves command routing to legacy history");
    f.first.Entry(1,1,2);f.first.bones[0].trans[0]=7;
    plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto original=f.first.Digest();
    CmdControl400(f.app.get(),f.app->MainWindow(),400,0);PressHistoryLetter(f,'Z');
    Check(f.first.Digest()==original&&f.history.UndoCount()==0&&f.app->SceneModified()==0,
        "empty bound journal never falls through to a populated legacy undo ring");
    f.Commit(panel::kReadoutPosXEdit,"8");
    const auto authored=f.first.Digest();f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;f.app->PhysicsResetPending()=0;
    for(int gate=0;gate<3;++gate) {
        if(gate==0)f.app->PlaybackActive()=1;
        if(gate==1)f.app->FrameStepPlayback()=1;
        if(gate==2)f.app->state.recordPlaybackStartPending=1;
        plugins::NativePoseScope scope;
        Check(scope.Begin(*f.app,f.history,0,"guarded pose")==MMS_E_NOT_READY,
            "pose scope itself rejects playback and recording before capturing model state");
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);PressHistoryLetter(f,'Z');
        f.Commit(panel::kReadoutPosXEdit,"19");
        Check(f.first.Digest()==authored&&f.history.UndoCount()==1&&f.app->SceneModified()==0,
            "playback and recording reject buttons keyboard and direct numeric authoring");
        f.app->PlaybackActive()=0;f.app->FrameStepPlayback()=0;f.app->state.recordPlaybackStartPending=0;
    }
    Check(f.history.Enter()==MMS_OK,"reserve history to test busy command route");
    CmdControl400(f.app.get(),f.app->MainWindow(),400,0);f.history.Leave();
    Check(f.first.Digest()==authored&&f.history.UndoCount()==1,"busy common history does not fall through to legacy ring");
    failAfter=0;CmdControl400(f.app.get(),f.app->MainWindow(),400,0);failAfter=-1;
    Check(f.first.Digest()==authored&&f.history.UndoCount()==1&&f.history.RedoCount()==0&&
        f.app->SceneModified()==0&&f.app->RecordedFrameCountByte1()==0&&f.app->PhysicsResetPending()==0&&
        IsWindowEnabled(f.Edit(panel::kUndoButton))&&!IsWindowEnabled(f.Edit(panel::kRedoButton)),
        "failed undo preparation preserves authored data cursor dirty flags physics and buttons");
    CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
    Check(f.first.bones[0].trans[0]==7&&f.history.RedoCount()==1,"undo remains usable after rejection and allocation failure");
    // Invalid editor selection must not make the post-rejection readout access
    // beyond the bone table or accidentally mutate the common history.
    f.first.m->selectedBone=1000;f.Commit(panel::kReadoutPosXEdit,"15");
    Check(f.first.bones[0].trans[0]==7&&f.history.RedoCount()==1,"invalid selected bone safely rejects numeric editing and readout refresh");
    f.app->SelectedModelSlot()=255;f.Commit(panel::kReadoutPosXEdit,"15");
    Check(f.first.bones[0].trans[0]==7&&f.history.RedoCount()==1,"invalid model slot rejects numeric editing before model lookup");
}
}
void NativeHistoryCommandChecks() {
    BoundHistoryButtonChecks();BoundHistoryKeyboardChecks();BoundHistoryRejectionChecks();
}
