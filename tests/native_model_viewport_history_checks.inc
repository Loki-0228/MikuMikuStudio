// Include after native_numpad_dispatch_checks.inc for NumpadFixture and its
// independent full view/ring snapshots. No renderer or source assets required.
#include "../src/plugins/native_history_commands.hpp"
namespace {
void ModelViewportBranch(NumpadFixture& f,unsigned branch){
    f.Branch(2,CameraAttachmentReference::SelectedBone);
    f.app->state.followCameraEnabled=branch!=0;
    f.app->CameraParentModel()=branch==1?1:branch==2?-1:0;
}
NumpadView ModelViewportOracle(NumpadView value,UINT button,unsigned modifiers,int dx,int dy){
    const bool shift=(modifiers&1)!=0,ctrl=(modifiers&2)!=0;
    if(button==WM_RBUTTONDOWN){
        if(shift){value.values[8]=value.values[8]+float(dy)*.1f;value.values[7]=value.values[7]-float(dx)*.1f;}
        else if(ctrl)value.values[6]-=float(dy)*.1f;
        else{value.values[4]=value.values[4]-float(dx)*.01f;value.values[3]=value.values[3]-float(dy)*.01f;}
    }else{
        const float scale=shift?.5f:ctrl?.005f:.05f;
        value.values[8]=value.values[8]+float(dy)*scale;value.values[7]=value.values[7]-float(dx)*scale;
    }return value;
}
void ModelViewportSample(NumpadFixture& f,UINT button,unsigned modifiers,int dx,int dy){
    f.app->PreviousMouseX()=0;f.app->PreviousMouseY()=0;f.app->MouseX()=dx;f.app->MouseY()=dy;
    f.app->ShiftModifierState()=(modifiers&1)?3:0;f.app->CtrlModifierState()=(modifiers&2)?3:0;
    if(button==WM_RBUTTONDOWN){f.app->state.rightMouseButtonState=3;ConsumeRightButtonDrag(f.app.get());}
    else{f.app->state.middleMouseButtonState=3;ConsumeMiddleButtonPan(f.app.get());}
}
void ModelViewportMathAndHistory(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(unsigned branch=0;branch<3;++branch)for(UINT button:{UINT(WM_RBUTTONDOWN),UINT(WM_MBUTTONDOWN)})
    for(unsigned modifier=0;modifier<4;++modifier)for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        NumpadFixture f;ModelViewportBranch(f,branch);plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"set model viewport adjustment recording policy");
        const auto before=NumpadRead(*f.app);auto expected=before;
        const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());const auto first=f.first.Digest(),second=f.second.Digest();
        const auto ring=PoseRingDigest(f.first);
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,button)==MMS_OK,"reserve model gesture before its first sample");
        Check(f.history.SetMode(mode)==MMS_E_BUSY,"a pending model gesture owns one journal reservation");
        for(unsigned sample=0;sample<4;++sample){
            const int dx=sample%2? -7:13,dy=sample%2?11:-5;
            expected=ModelViewportOracle(expected,button,modifier,dx,dy);
            if(sample)failAfter=0;
            const bool allowed=plugins::AllowViewportModelPreview(*f.app,button);
            ModelViewportSample(f,button,modifier,dx,dy);
            const bool accepted=plugins::AcceptViewportModelPreview(*f.app);failAfter=-1;
            Check(allowed&&accepted&&NumpadSame(NumpadRead(*f.app),expected),
                "free and nonmatching-follow model previews use the original float math and modifier precedence");
            Check(!f.history.UndoCount(),"intermediate model previews do not publish partial history");
        }
        bool changed=false;failAfter=0;const auto result=plugins::EndViewportModelGesture(*f.app,changed);failAfter=-1;
        Check(result==MMS_OK&&changed&&NumpadSame(NumpadRead(*f.app),expected),"model gesture release publishes the prepared final view without allocation");
        Check(GlobalRegistrationSame(f.app->CameraKeys(),keys)&&f.first.Digest()==first&&f.second.Digest()==second&&PoseRingDigest(f.first)==ring,
            "model navigation never edits native keys, model poses, physics or legacy undo rings");
        Check(f.history.UndoCount()==(mode==Mode::AllOperations?1:0),"one model gesture obeys the shared adjustment recording mode");
        if(mode==Mode::AllOperations){
            auto undo=before,redo=expected;
            f.app->CameraPositionX()+=7;f.app->CameraFov()+=3;f.app->CameraRoll()+=.7f;f.app->CameraAttachmentBasis().m[0][3]+=5;
            undo.values[0]=redo.values[0]=f.app->CameraPositionX();undo.values[9]=redo.values[9]=f.app->CameraFov();
            undo.values[5]=redo.values[5]=f.app->CameraRoll();undo.basis.m[0][3]=redo.basis.m[0][3]=f.app->CameraAttachmentBasis().m[0][3];
            f.app->CameraMode()=1;f.app->CurrentFrame()=91;f.app->SelectedModelSlot()=1;f.app->state.followCameraEnabled=0;
            Check(f.history.History(false)==MMS_OK&&NumpadSame(NumpadRead(*f.app),undo)&&f.app->CurrentFrame()==91,
                "model gesture undo crosses mode frame and selected model without restoring untouched fields");
            Check(f.history.History(true)==MMS_OK&&NumpadSame(NumpadRead(*f.app),redo),"model gesture redo owns its captured numeric view without reevaluation");
        }
    }
}
void ModelViewportCancellationAndContexts(){
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        NumpadFixture f;ModelViewportBranch(f,1);plugins::NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);
        const auto before=NumpadRead(*f.app);
        for(UINT message:{UINT(WM_CANCELMODE),UINT(WM_KILLFOCUS),UINT(WM_CAPTURECHANGED),UINT(WM_KEYDOWN),UINT(WM_ACTIVATEAPP),UINT(WM_NCDESTROY)}){
            Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"start model preview for lifecycle cancellation");
            f.app->ViewOffsetX()+=3;Check(plugins::AcceptViewportModelPreview(*f.app),"accept owned model cancellation sample");
            failAfter=0;plugins::HandleBoundViewportMessage(*f.app,nullptr,message,message==WM_KEYDOWN?VK_ESCAPE:0,message==WM_CAPTURECHANGED?LPARAM(1):0);failAfter=-1;
            Check(NumpadSame(NumpadRead(*f.app),before)&&!f.history.UndoCount(),"model focus capture escape deactivate and destroy cancellation restores owned preview without allocation");
        }
        for(unsigned stale=0;stale<10;++stale){
            Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"begin model context stale fixture");
            f.app->ViewOffsetY()+=2;Check(plugins::AcceptViewportModelPreview(*f.app),"accept model preview before external context change");
            const auto frame=f.app->CurrentFrame();const auto reference=f.app->CameraReferenceMode();const auto bone=f.first.m->selectedBone;
            const auto pool=f.app->CameraKeys();mdl::CameraKey* replacement=nullptr;
            const auto bones=f.first.m->boneTable;const auto parentCount=f.second.m->boneCount;
            switch(stale){
            case 0:++f.app->CurrentFrame();break;case 1:f.app->CameraMode()=1;break;
            case 2:f.app->CameraReferenceMode()=CameraAttachmentReference::None;break;
            case 3:f.first.m->selectedBone=bone==0?1:0;break;
            case 4:f.app->ViewOffsetX()+=9;break;case 5:f.app->CameraParentBone()=1;break;
            case 6:replacement=mdl::KeyPoolCreate<mdl::CameraKey>(4);f.app->CameraKeys()=replacement;break;
            case 7:f.app->SelectedModelSlot()=1;break;
            case 8:f.first.m->boneTable=bones+1;break;
            case 9:--f.second.m->boneCount;break;
            }
            const auto foreign=NumpadRead(*f.app);
            Check(!plugins::AllowViewportModelPreview(*f.app,WM_RBUTTONDOWN)&&NumpadSame(NumpadRead(*f.app),foreign),
                "stale frame mode reference selected bone parent pool model or foreign view cancels without overwriting the new source");
            f.app->CameraKeys()=pool;if(replacement)mdl::KeyPoolDestroy(replacement);f.app->CurrentFrame()=frame;
            f.app->CameraMode()=0;f.app->CameraReferenceMode()=reference;f.first.m->selectedBone=bone;f.app->SelectedModelSlot()=0;
            f.first.m->boneTable=bones;f.second.m->boneCount=parentCount;
            NumpadWrite(*f.app,before);
        }
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"begin nonfinite model preview");
        f.app->CameraDistance()=std::numeric_limits<float>::infinity();
        Check(!plugins::AcceptViewportModelPreview(*f.app)&&NumpadSame(NumpadRead(*f.app),before),"nonfinite owned model math restores the last valid sample then the gesture source");
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin model preview before journal reset");
        f.app->ViewOffsetY()+=5;plugins::AcceptViewportModelPreview(*f.app);Check(f.history.Reset()==MMS_OK,"journal reset invalidates reserved model publication");
        bool changed=true;Check(plugins::EndViewportModelGesture(*f.app,changed)==MMS_E_STALE&&!changed&&NumpadSame(NumpadRead(*f.app),before),
            "a stale model reservation rolls back only its still-owned preview");
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"begin model preview before temporary binding");
        f.app->ViewOffsetX()+=2;plugins::AcceptViewportModelPreview(*f.app);
        {plugins::NativePoseTestBinding nested(*f.app,f.history);Check(NumpadSame(NumpadRead(*f.app),before),"existing binding hooks cancel model previews through the camera lifecycle bridge");}
    }
}
void ModelViewportWheelAndNoops(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(unsigned branch=0;branch<3;++branch)for(bool floating:{false,true})for(int delta:{-32768,-120,1,120,32767})
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        NumpadFixture f;ModelViewportBranch(f,branch);f.history.SetMode(mode);const auto before=NumpadRead(*f.app);auto expected=before;
        if(floating)expected.values[6]=float(double(std::int16_t(delta))*0.05000000074505806+double(before.values[6]));
        else expected.values[6]=float(std::int16_t(delta))*.05f+before.values[6];
        const auto keys=GlobalRegistrationKeys(f.app->CameraKeys());const auto first=f.first.Digest(),second=f.second.Digest();bool changed=false;
        Check(plugins::AdjustViewportModelWheel(*f.app,f.history,std::int16_t(delta),floating,changed)==MMS_OK&&changed&&NumpadSame(NumpadRead(*f.app),expected),
            "main and detached model wheel retain their distinct original float expressions");
        Check(GlobalRegistrationSame(f.app->CameraKeys(),keys)&&f.first.Digest()==first&&f.second.Digest()==second,
            "model wheel never edits keys or model data");
        Check(f.history.UndoCount()==(mode==Mode::AllOperations?1:0),"model wheel is one shared-history adjustment");
        if(mode==Mode::AllOperations){f.app->CameraMode()=1;f.app->CurrentFrame()=57;
            Check(f.history.History(false)==MMS_OK&&NumpadSame(NumpadRead(*f.app),before),"model wheel undo crosses mode and frame");
            Check(f.history.History(true)==MMS_OK&&NumpadSame(NumpadRead(*f.app),expected),"model wheel redo replays captured values");}
    }
    NumpadFixture f;ModelViewportBranch(f,0);bool changed=false;
    Check(plugins::AdjustViewportModelWheel(*f.app,f.history,120,false,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,"create a model wheel redo branch");
    failAfter=0;const auto zero=plugins::AdjustViewportModelWheel(*f.app,f.history,0,false,changed);failAfter=-1;
    Check(zero==MMS_OK&&!changed&&f.history.RedoCount()==1,"zero model wheel allocates nothing and preserves redo");
    f.app->CameraDistance()=1e30f;failAfter=0;const auto tiny=plugins::AdjustViewportModelWheel(*f.app,f.history,1,true,changed);failAfter=-1;
    Check(tiny==MMS_OK&&!changed&&f.history.RedoCount()==1,"rounded model wheel no-op also allocates nothing and preserves redo");
    Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"begin model no-op gesture with redo pending");
    failAfter=0;const auto noMove=plugins::EndViewportModelGesture(*f.app,changed);failAfter=-1;
    Check(noMove==MMS_OK&&!changed&&f.history.RedoCount()==1,"empty model gesture cancels its reservation without trimming redo");
}
void ModelViewportGatesAndFailures(){
    NumpadFixture f;ModelViewportBranch(f,0);const auto before=NumpadRead(*f.app);bool completed=false;unsigned refusals=0;
    for(long allowance=0;allowance<256;++allowance){
        failAfter=allowance;const auto result=plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN);failAfter=-1;
        Check(NumpadSame(NumpadRead(*f.app),before)&&!f.history.UndoCount(),"every model gesture preflight allocation failure leaves live state and history untouched");
        if(result==MMS_OK){completed=true;plugins::CancelViewportModelGesture(*f.app);break;}
        Check(result==MMS_E_MEMORY,"model gesture preflight reports allocation refusal explicitly");++refusals;
    }
    Check(completed&&refusals>5,"model gesture OOM sweep reaches a fully reserved publication");
    plugins::StorageStore storage;plugins::SharedHistory budget(storage,{},8,1);
    Check(plugins::BeginViewportModelGesture(*f.app,budget,nullptr,WM_RBUTTONDOWN)==MMS_E_MEMORY&&NumpadSame(NumpadRead(*f.app),before),"model gesture budget fails before the first preview");
    bool changed=true;Check(plugins::AdjustViewportModelWheel(*f.app,budget,120,false,changed)==MMS_E_MEMORY&&!changed&&NumpadSame(NumpadRead(*f.app),before),"model wheel budget failure is atomic");
    completed=false;refusals=0;
    for(long allowance=0;allowance<256;++allowance){
        failAfter=allowance;const auto result=plugins::AdjustViewportModelWheel(*f.app,f.history,120,false,changed);failAfter=-1;
        if(result==MMS_OK){completed=true;break;}
        Check(result==MMS_E_MEMORY&&!changed&&NumpadSame(NumpadRead(*f.app),before)&&!f.history.UndoCount(),"every model wheel allocation refusal leaves source state and redo intact");++refusals;
    }
    Check(completed&&refusals>3&&f.history.History(false)==MMS_OK,"model wheel preparation covers detached state owner and publication allocations");
    {
        ScopedSceneMutation guard(*f.app);
        Check(f.history.History(true)==MMS_E_BUSY&&plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_E_BUSY,
            "unrelated scene mutation cannot impersonate a model-view initial edit or history publication");
    }
    {
        bake::ScopedUiLock guard(f.app->MainWindow());
        Check(f.history.History(true)==MMS_E_BUSY&&plugins::AdjustViewportModelWheel(*f.app,f.history,120,false,changed)==MMS_E_BUSY,
            "unrelated UI locking remains strict for model-view wheel and history");
    }
    for(bool defer:{false,true}){
        Check(plugins::RunBoundObjectHistory(*f.app,f.history,true,defer)==MMS_OK&&plugins::RunBoundObjectHistory(*f.app,f.history,false,defer)==MMS_OK,
            "actual object-history wrapper may replay model view under its authorized scene and UI guards");
    }
    f.app->state.followCameraEnabled=1;f.app->CameraParentModel()=0;f.app->SelectedModelSlot()=0;
    for(UINT button:{UINT(WM_RBUTTONDOWN),UINT(WM_MBUTTONDOWN)})
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,button)==MMS_E_NOT_READY,"matched-follow legacy hint/release gestures stay explicitly closed");
    Check(plugins::AdjustViewportModelWheel(*f.app,f.history,120,false,changed)==MMS_E_NOT_READY&&!changed,"matched-follow main wheel cannot fall through to the unmigrated hint path");
    Check(plugins::AdjustViewportModelWheel(*f.app,f.history,120,true,changed)==MMS_OK&&changed,"detached model wheel has no hint path and supports the same followed parent");
    f.app->state.followCameraEnabled=0;
    Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"begin model gesture for camera mutual exclusion");
    f.app->CameraMode()=1;
    Check(plugins::BeginViewportCameraGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_E_BUSY,"a camera gesture cannot coexist with a model gesture");
    Check(!plugins::AllowViewportCameraPreview(*f.app,WM_RBUTTONDOWN),"old pump bridge cancels a model gesture immediately after a mode switch");
    f.app->CameraMode()=0;
    Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,"mode-switch cancellation releases the model reservation");
    plugins::CancelViewportCameraGesture(*f.app);
}
void ModelViewportWheelSourceGates(){
    for(unsigned stage:{2u,3u})for(unsigned kind=0;kind<10;++kind){
        NumpadFixture f;ModelViewportBranch(f,0);const auto before=NumpadRead(*f.app);auto foreign=before;
        const auto pool=f.app->CameraKeys();auto* replacement=mdl::KeyPoolCreate<mdl::CameraKey>(4);
        const auto bones=f.first.m->boneTable;const auto selectedBone=f.first.m->selectedBone;
        unsigned calls=0;plugins::StorageStore storage;
        plugins::SharedHistory history(storage,[&]{
            if(++calls==stage){switch(kind){
                case 0:++f.app->CurrentFrame();break;case 1:f.app->CameraMode()=1;break;
                case 2:f.app->state.followCameraEnabled=1;break;
                case 3:f.app->CameraReferenceMode()=CameraAttachmentReference::None;break;
                case 4:f.first.m->selectedBone=selectedBone==0?1:0;break;
                case 5:f.app->ViewOffsetX()+=11;break;case 6:f.app->CameraParentBone()=1;break;
                case 7:f.app->CameraKeys()=replacement;break;case 8:f.first.m->boneTable=bones+1;break;
                case 9:f.app->PlaybackActive()=1;break;
            }foreign=NumpadRead(*f.app);}return MMS_OK;
        });
        bool changed=true;const auto result=plugins::AdjustViewportModelWheel(*f.app,history,120,false,changed);
        Check(result==MMS_E_STALE&&!changed&&NumpadSame(NumpadRead(*f.app),foreign)&&!history.UndoCount()&&!history.RetainedBytes(),
            "model wheel revalidates source context identity and readiness at both reservation and publication without overwriting foreign changes");
        f.app->CameraKeys()=pool;mdl::KeyPoolDestroy(replacement);f.first.m->boneTable=bones;f.first.m->selectedBone=selectedBone;
        f.app->PlaybackActive()=0;
    }
}
void ModelViewportReadoutGates(){
    for(unsigned kind=0;kind<5;++kind){
        NumpadFixture f;ModelViewportBranch(f,0);f.app->CameraParentModel()=-1;
        f.app->CameraReferenceMode()=CameraAttachmentReference::None;
        const auto table=f.first.m->boneTable;const auto count=f.first.m->boneCount;const auto selected=f.first.m->selectedBone;
        switch(kind){
        case 0:f.app->SelectedModelSlot()=2;break;
        case 1:f.first.m->boneTable=nullptr;break;
        case 2:f.first.m->boneCount=0;break;
        case 3:f.first.m->selectedBone=int(count);break;
        case 4:f.first.m->selectedBone=std::numeric_limits<int>::max();break;
        }
        const auto before=NumpadRead(*f.app);const auto capture=GetCapture();bool changed=true;
        failAfter=0;
        const auto right=plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN);
        const auto middle=plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN);
        const auto wheel=plugins::AdjustViewportModelWheel(*f.app,f.history,120,false,changed);failAfter=-1;
        Check(right==MMS_E_NOT_READY&&middle==MMS_E_NOT_READY&&wheel==MMS_E_NOT_READY&&!changed&&
            NumpadSame(NumpadRead(*f.app),before)&&GetCapture()==capture&&!f.history.UndoCount()&&!f.history.RetainedBytes(),
            "missing model or invalid model readout is refused before view history capture or allocation changes");
        f.app->SelectedModelSlot()=0;f.first.m->boneTable=table;f.first.m->boneCount=count;f.first.m->selectedBone=selected;
    }
    for(bool noBones:{false,true}){
        NumpadFixture f;ModelViewportBranch(f,0);f.app->CameraParentModel()=-1;f.first.m->selectedBone=-1;
        const auto table=f.first.m->boneTable;const auto count=f.first.m->boneCount;
        if(noBones){f.first.m->boneCount=0;f.first.m->boneTable=nullptr;}
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK&&
            plugins::AllowViewportModelPreview(*f.app,WM_MBUTTONDOWN),"a negative selected bone safely uses the original empty readout with or without bones");
        f.app->ViewOffsetX()+=1;Check(plugins::AcceptViewportModelPreview(*f.app),"empty bone readout still accepts an owned model pan");
        bool changed=false;Check(plugins::EndViewportModelGesture(*f.app,changed)==MMS_OK&&changed,"empty bone readout can publish a complete model-view gesture");
        Check(f.history.History(false)==MMS_OK,"empty bone readout history restores without evaluating a bone");
        f.first.m->boneTable=table;f.first.m->boneCount=count;
    }
    for(bool accept:{false,true}){
        NumpadFixture f;ModelViewportBranch(f,0);f.app->CameraReferenceMode()=CameraAttachmentReference::None;
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"prepare a model preview before its readout source becomes invalid");
        f.app->ViewOffsetY()+=1;Check(plugins::AcceptViewportModelPreview(*f.app),"accept the last valid model readout sample");
        const auto selected=f.first.m->selectedBone;f.first.m->selectedBone=int(f.first.m->boneCount);
        const auto foreign=NumpadRead(*f.app);
        const bool allowed=accept?plugins::AcceptViewportModelPreview(*f.app):plugins::AllowViewportModelPreview(*f.app,WM_RBUTTONDOWN);
        Check(!allowed&&NumpadSame(NumpadRead(*f.app),foreign)&&!f.history.UndoCount()&&!f.history.RetainedBytes(),
            "both pre-math and post-math checks cancel an invalid readout without overwriting its foreign source");
        f.first.m->selectedBone=selected;
    }
    for(unsigned stage:{2u,3u}){
        NumpadFixture f;ModelViewportBranch(f,0);f.app->CameraReferenceMode()=CameraAttachmentReference::None;
        const auto before=NumpadRead(*f.app);const auto selected=f.first.m->selectedBone;
        plugins::StorageStore storage;unsigned calls=0;
        plugins::SharedHistory history(storage,[&]{if(++calls==stage)f.first.m->selectedBone=int(f.first.m->boneCount);return MMS_OK;});
        bool changed=true;
        Check(plugins::AdjustViewportModelWheel(*f.app,history,120,false,changed)==MMS_E_STALE&&!changed&&
            NumpadSame(NumpadRead(*f.app),before)&&!history.UndoCount()&&!history.RetainedBytes(),
            "model wheel reservation and initial publication revalidate readout even without a selected-bone camera reference");
        f.first.m->selectedBone=selected;
    }
}
void ModelViewportScaleReadoutIsolation(){
    const auto scaleSame=[](const bone_scale::Snapshot& a,const bone_scale::Snapshot& b){
        return a.pose==b.pose&&a.keys==b.keys&&a.curves==b.curves;
    };
    {
        NumpadFixture f;ModelViewportBranch(f,0);bone_scale::SetPose(f.first.raw(),0,Scale(2));
        bone_scale::SetKey(f.first.raw(),3,Scale(3));bone_scale::SetKeyCurve(f.first.raw(),3,{12,32,91,115});
        const auto scale=bone_scale::Capture(f.first.raw());const auto revision=bone_scale::Revision(f.first.raw());
        const auto model=f.first.Digest();const auto before=NumpadRead(*f.app);const auto capture=GetCapture();
        bool changed=true;failAfter=0;
        const auto right=plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN);
        const auto middle=plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN);
        const auto wheel=plugins::AdjustViewportModelWheel(*f.app,f.history,120,false,changed);
        const auto detached=plugins::AdjustViewportModelWheel(*f.app,f.history,120,true,changed);failAfter=-1;
        Check(right==MMS_E_NOT_READY&&middle==MMS_E_NOT_READY&&wheel==MMS_E_NOT_READY&&detached==MMS_E_NOT_READY&&!changed&&
            !bone_scale::TransformsApplied(f.first.raw())&&f.first.Digest()==model&&NumpadSame(NumpadRead(*f.app),before)&&
            bone_scale::Revision(f.first.raw())==revision&&scaleSame(bone_scale::Capture(f.first.raw()),scale)&&
            GetCapture()==capture&&!f.history.UndoCount()&&!f.history.RetainedBytes(),
            "pending nonidentity scale readout is refused without applying matrices allocating or changing pose authoring view or history");
    }
    {
        NumpadFixture f;ModelViewportBranch(f,0);plugins::NativePoseTestBinding binding(*f.app,f.history);
        bone_scale::SetPose(f.first.raw(),0,Scale(2));bone_scale::ApplyTransforms(f.first.raw());
        const auto scale=bone_scale::Capture(f.first.raw());const auto revision=bone_scale::Revision(f.first.raw());const auto model=f.first.Digest();
        auto expected=NumpadRead(*f.app);
        Check(bone_scale::TransformsApplied(f.first.raw())&&plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_MBUTTONDOWN)==MMS_OK,
            "an already applied nonidentity scale retains model navigation support");
        for(unsigned sample=0;sample<2;++sample){
            expected=ModelViewportOracle(expected,WM_MBUTTONDOWN,0,11,-5);if(sample)failAfter=0;
            ModelViewportSample(f,WM_MBUTTONDOWN,0,11,-5);const auto accepted=plugins::AcceptViewportModelPreview(*f.app);failAfter=-1;
            Check(accepted&&NumpadSame(NumpadRead(*f.app),expected)&&f.first.Digest()==model&&bone_scale::TransformsApplied(f.first.raw())&&
                bone_scale::Revision(f.first.raw())==revision&&scaleSame(bone_scale::Capture(f.first.raw()),scale),
                "the real model drag pump reads an applied scale without allocating or changing bone matrices or scale state");
        }
        plugins::CancelViewportModelGesture(*f.app);
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"prepare model preview before an external scale-cache invalidation");
        f.app->ViewOffsetY()+=1;Check(plugins::AcceptViewportModelPreview(*f.app),"accept model preview with an applied scale");
        bone_scale::RestoreTransforms(f.first.raw());const auto invalidated=f.first.Digest();const auto foreign=NumpadRead(*f.app);
        failAfter=0;const auto allowed=plugins::AllowViewportModelPreview(*f.app,WM_RBUTTONDOWN);failAfter=-1;
        Check(!allowed&&!bone_scale::TransformsApplied(f.first.raw())&&f.first.Digest()==invalidated&&NumpadSame(NumpadRead(*f.app),foreign)&&
            !f.history.UndoCount()&&!f.history.RetainedBytes(),
            "cache invalidation cancels before original model drag math and cannot silently apply matrices or overwrite the foreign source");
    }
    for(unsigned stage:{2u,3u}){
        NumpadFixture f;ModelViewportBranch(f,0);bone_scale::SetPose(f.first.raw(),0,Scale(2));bone_scale::ApplyTransforms(f.first.raw());
        const auto before=NumpadRead(*f.app);unsigned calls=0;std::uint64_t invalidated=0;plugins::StorageStore storage;
        plugins::SharedHistory history(storage,[&]{if(++calls==stage){bone_scale::RestoreTransforms(f.first.raw());invalidated=f.first.Digest();}return MMS_OK;});
        bool changed=true;
        Check(plugins::AdjustViewportModelWheel(*f.app,history,120,false,changed)==MMS_E_STALE&&!changed&&invalidated&&
            f.first.Digest()==invalidated&&!bone_scale::TransformsApplied(f.first.raw())&&NumpadSame(NumpadRead(*f.app),before)&&
            !history.UndoCount()&&!history.RetainedBytes(),"wheel revalidates scale-readout readiness before reservation and first publication");
    }
}
void ModelViewportScaleMessageAndHistoryTails(){
    const auto scaleSame=[](const bone_scale::Snapshot& a,const bone_scale::Snapshot& b){
        return a.pose==b.pose&&a.keys==b.keys&&a.curves==b.curves;
    };
    // Run each path both normally and with allocation refused. A normal run
    // is essential: swallowing ApplyTransforms' bad_alloc would hide a write.
    for(bool refuseAllocation:{false,true})for(UINT message:{UINT(WM_MOUSEMOVE),UINT(WM_CANCELMODE),UINT(WM_CAPTURECHANGED)}){
        NumpadFixture f;ModelViewportBranch(f,0);plugins::NativePoseTestBinding binding(*f.app,f.history);
        bone_scale::SetPose(f.first.raw(),0,Scale(2));bone_scale::ApplyTransforms(f.first.raw());
        Check(plugins::BeginViewportModelGesture(*f.app,f.history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"reserve a model gesture before scale message-tail invalidation");
        f.app->ViewOffsetX()+=2;Check(plugins::AcceptViewportModelPreview(*f.app),"accept the last model preview before a real cancellation message");
        bone_scale::RestoreTransforms(f.first.raw());const auto model=f.first.Digest();const auto peer=f.second.Digest();
        const auto scale=bone_scale::Capture(f.first.raw());const auto revision=bone_scale::Revision(f.first.raw());const auto view=NumpadRead(*f.app);
        if(refuseAllocation)failAfter=0;
        plugins::HandleBoundViewportMessage(*f.app,nullptr,message,MK_RBUTTON,
            message==WM_CAPTURECHANGED?LPARAM(1):MAKELPARAM(13,7));failAfter=-1;
        Check(!bone_scale::TransformsApplied(f.first.raw())&&f.first.Digest()==model&&f.second.Digest()==peer&&
            bone_scale::Revision(f.first.raw())==revision&&scaleSame(bone_scale::Capture(f.first.raw()),scale)&&
            NumpadSame(NumpadRead(*f.app),view)&&!f.history.UndoCount()&&!f.history.RetainedBytes(),
            "real move cancel and capture-loss tails cannot apply pending scale transforms allocate or change the foreign view");
    }
    for(bool refuseAllocation:{false,true}){
        NumpadFixture f;ModelViewportBranch(f,0);bone_scale::SetPose(f.first.raw(),0,Scale(2));bone_scale::ApplyTransforms(f.first.raw());
        plugins::StorageStore storage;bool invalidate=false;std::uint64_t model=0;
        plugins::SharedHistory history(storage,[&]{if(invalidate){invalidate=false;bone_scale::RestoreTransforms(f.first.raw());model=f.first.Digest();}return MMS_OK;});
        plugins::NativePoseTestBinding binding(*f.app,history);
        Check(plugins::BeginViewportModelGesture(*f.app,history,nullptr,WM_RBUTTONDOWN)==MMS_OK,"reserve model release before a late publication refusal");
        f.app->ViewOffsetX()+=2;Check(plugins::AcceptViewportModelPreview(*f.app),"accept model release preview before its publication gate");
        const auto scale=bone_scale::Capture(f.first.raw());const auto revision=bone_scale::Revision(f.first.raw());const auto view=NumpadRead(*f.app);
        invalidate=true;if(refuseAllocation)failAfter=0;
        plugins::HandleBoundViewportMessage(*f.app,nullptr,WM_RBUTTONUP,0,0);failAfter=-1;
        Check(model&&!bone_scale::TransformsApplied(f.first.raw())&&f.first.Digest()==model&&
            bone_scale::Revision(f.first.raw())==revision&&scaleSame(bone_scale::Capture(f.first.raw()),scale)&&
            NumpadSame(NumpadRead(*f.app),view)&&!history.UndoCount()&&!history.RetainedBytes(),
            "failed model release publication skips its pending-scale readout tail without clobbering the view or bone matrices");
    }
    for(bool defer:{false,true}){
        NumpadFixture f;ModelViewportBranch(f,0);plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto before=NumpadRead(*f.app);bool changed=false;
        Check(plugins::AdjustViewportModelWheel(*f.app,f.history,120,false,changed)==MMS_OK&&changed,"publish a view node before changing the currently selected model");
        auto after=NumpadRead(*f.app);f.app->SelectedModelSlot()=1;f.second.m->selectedBone=0;
        bone_scale::SetPose(f.second.raw(),0,Scale(2));bone_scale::SetKey(f.second.raw(),3,Scale(3));
        bone_scale::SetKeyCurve(f.second.raw(),3,{12,32,91,115});
        // This record only owns distance. Preserve later unrelated view edits
        // while the current model's scale readout is intentionally unavailable.
        f.app->CameraPositionX()+=9;f.app->ViewOffsetY()+=4;f.app->CameraRoll()+=.25f;
        before.values[0]=after.values[0]=f.app->CameraPositionX();before.values[8]=after.values[8]=f.app->ViewOffsetY();
        before.values[5]=after.values[5]=f.app->CameraRoll();
        const auto model=f.first.Digest();const auto peer=f.second.Digest();const auto scale=bone_scale::Capture(f.second.raw());
        const auto revision=bone_scale::Revision(f.second.raw());
        for(unsigned cycle=0;cycle<2;++cycle)for(bool redo:{false,true}){
            Check(plugins::RunBoundObjectHistory(*f.app,f.history,redo,defer)==MMS_OK,"actual guarded history replays a view node despite a pending current-model scale readout");
            if(defer)plugins::RefreshBoundPoseReadout(*f.app);
            Check(NumpadSame(NumpadRead(*f.app),redo?after:before)&&!bone_scale::TransformsApplied(f.second.raw())&&
                f.first.Digest()==model&&f.second.Digest()==peer&&bone_scale::Revision(f.second.raw())==revision&&
                scaleSame(bone_scale::Capture(f.second.raw()),scale),
                "common history updates only its view mask and neither immediate nor deferred readout applies the selected model scale");
        }
    }
    for(unsigned kind=0;kind<4;++kind){
        NumpadFixture f;ModelViewportBranch(f,0);bone_scale::SetPose(f.first.raw(),kind==2?1:0,Scale(2));
        if(kind==1)bone_scale::ApplyTransforms(f.first.raw());if(kind==3)f.first.m->selectedBone=-1;
        const auto model=f.first.Digest();const auto scale=bone_scale::Capture(f.first.raw());const auto revision=bone_scale::Revision(f.first.raw());
        const bool applied=bone_scale::TransformsApplied(f.first.raw());
        SetWindowTextA(f.Edit(panel::kReadoutPosXEdit),"unchanged readout");
        SetWindowTextA(f.Edit(panel::kReadoutScaleXEdit),"unchanged scale");
        plugins::RefreshBoundPoseReadout(*f.app);
        char position[64]{},factor[64]{};GetWindowTextA(f.Edit(panel::kReadoutPosXEdit),position,64);GetWindowTextA(f.Edit(panel::kReadoutScaleXEdit),factor,64);
        Check(f.first.Digest()==model&&bone_scale::TransformsApplied(f.first.raw())==applied&&
            bone_scale::Revision(f.first.raw())==revision&&scaleSame(bone_scale::Capture(f.first.raw()),scale),
            "direct common readout is purely observational for pending applied identity and negative-bone cases");
        Check(kind==0?(!std::strcmp(position,"unchanged readout")&&!std::strcmp(factor,"unchanged scale")):
            kind==3?(!std::strcmp(position,"-----")&&!std::strcmp(factor,"-----")):
            (std::strcmp(position,"unchanged readout")&&std::strcmp(factor,"unchanged scale")),
            "common readout skips pending selected scale while retaining applied identity and empty-selection UI updates");
    }
}
void NativeModelViewportHistoryChecks(){
    std::puts("  model viewport: original free/follow math, one adjustment and independent cross-mode history");ModelViewportMathAndHistory();
    std::puts("  model viewport: lifecycle cancellation, foreign authors and source identity guards");ModelViewportCancellationAndContexts();
    std::puts("  model viewport: distinct wheel math, two recording policies and no-op redo preservation");ModelViewportWheelAndNoops();
    std::puts("  model viewport: exhaustive preparation refusal, real guarded history and explicit hint-path boundary");ModelViewportGatesAndFailures();
    std::puts("  model viewport: late reservation/publication source and identity changes");ModelViewportWheelSourceGates();
    std::puts("  model viewport: valid model readout before preview and publication");ModelViewportReadoutGates();
    std::puts("  model viewport: readout cannot implicitly apply pending scale transforms");ModelViewportScaleReadoutIsolation();
    std::puts("  model viewport: message and actual history readout tails preserve pending scale state");ModelViewportScaleMessageAndHistoryTails();
}
}
