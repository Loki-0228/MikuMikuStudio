// Include after native_numpad_view_checks.inc and native_camera_hud_history_checks.inc.
// Uses owned native models, the original unbound math as oracle, and actual
// hidden Win32 EDIT/MicWndProc messages. No D3D device is created.
#include "../src/plugins/native_model_hud_history.hpp"
#include "mikumikustudio/bone_scale_gizmo.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include "mikumikustudio/frame_presentation.hpp"
#include "mikumikustudio/viewport_tool_layout.hpp"
#include "mikumikustudio/viewport_corner_settings.hpp"
namespace mikumikustudio {int ViewportToolAtPointForTest(MMDApp*);}
namespace {
constexpr int modelHudOperations[]={3,4,5,6,7,8,9,10,11,12,13,14,22,23,24,25,26,27,28,29};
int ModelHudMode(int operation){
    // Independent copy of the native operation table, not the new helper.
    constexpr unsigned char modes[]={0,0,0,1,2,3,7,8,9,4,5,6,10,11,12,13,14,15,16,17,18,0,19,20,21,22,23,24,25,26};
    return modes[operation];
}
struct ModelHudPose{
    std::array<std::array<float,7>,2> bones{};
    std::array<unsigned char,2> dirty{};
    bone_scale::Values scale;
};
ModelHudPose ModelHudRead(Fixture& f){ModelHudPose result;result.dirty=f.dirty;result.scale=bone_scale::PoseValues(f.raw());
    for(unsigned b=0;b<2;++b){std::memcpy(result.bones[b].data(),f.bones[b].trans,3*sizeof(float));
        std::memcpy(result.bones[b].data()+3,f.bones[b].rotQuat,4*sizeof(float));}return result;}
bool ModelHudSame(const ModelHudPose& a,const ModelHudPose& b){return !std::memcmp(a.bones.data(),b.bones.data(),sizeof(a.bones))&&a.dirty==b.dirty&&a.scale==b.scale;}
void ModelHudWrite(Fixture& f,const ModelHudPose& value){for(unsigned b=0;b<2;++b){std::memcpy(f.bones[b].trans,value.bones[b].data(),3*sizeof(float));
    std::memcpy(f.bones[b].rotQuat,value.bones[b].data()+3,4*sizeof(float));}f.dirty=value.dirty;auto scale=value.scale;Check(bone_scale::ExchangePoseValues(f.raw(),scale),"restore test model HUD pose in its existing storage");}
struct ModelHudFixture:CameraHudFixture{
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    ModelHudFixture(){Branch(2,CameraAttachmentReference::SelectedBone);app->CameraParentModel()=-1;app->EditMode()=ViewportEditMode::Bone;
        app->Renderer()=renderer.get();renderer->screenWidth=800;renderer->screenHeight=600;renderer->viewScale=1.25f;
        app->RenderWidth()=1280;app->RenderHeight()=720;app->state.selectedClipW=3.75f;app->CameraPerspective()=1;
        first.selected={1,1};first.m->physicsMode=2;
        for(unsigned i=0;i<2;++i){auto& bone=first.bones[i];bone.type=i?mdl::BoneType::Ik:mdl::BoneType::Move;bone.parent=-1;bone.tailBone=1-int(i);
            bone.physicsDisabled=std::uint8_t(i);bone.hasRigidBody=std::uint8_t(i);bone.flags|=mdl::kBoneFlagVisible;
            for(unsigned n=0;n<16;++n)bone.matInit[n]=bone.matWorld[n]=(n%5==0)?1.f:0.f;
            bone.position[0]=float(i)*3;bone.position[1]=float(i)+1;bone.position[2]=float(i)*-2;
            bone.trans[0]=1.125f+float(i);bone.trans[1]=-.625f-float(i);bone.trans[2]=3.3125f+float(i);
            bone.rotQuat[0]=0;bone.rotQuat[1]=.6f;bone.rotQuat[2]=0;bone.rotQuat[3]=.8f;
            std::memcpy(bone.name,i?"root-two":"root-one",9);
            bone.localAxes[0]=.8f;bone.localAxes[1]=.6f;bone.localAxes[2]=0;bone.localAxes[3]=0;bone.localAxes[4]=0;bone.localAxes[5]=1;
        }
        app->ViewportToolDragOriginX()=70;app->ViewportToolDragOriginY()=60;
        bone_scale::gizmo::ScreenGeometry geometry{};geometry.valid=true;geometry.originX=60;geometry.originY=60;
        geometry.planeX=90;geometry.planeY=90;geometry.half=6;geometry.rod=3;
        const float x[]={160.f,90.f,130.f},y[]={90.f,160.f,130.f};
        for(unsigned axis=0;axis<3;++axis){geometry.tipX[axis]=geometry.endX[axis]=x[axis];geometry.tipY[axis]=geometry.endY[axis]=y[axis];geometry.axisAvailable[axis]=true;}
        bone_scale::gizmo::Publish(app.get(),geometry);
    }
    ~ModelHudFixture(){plugins::CancelModelHudGesture(*app);history.Reset();history.Collect();bone_scale::gizmo::Forget(app.get());app->Renderer()=nullptr;}
};
void ModelHudPointer(MMDApp& app,int operation,int dx,int dy,unsigned modifier=0){app.InteractionDragMode()=static_cast<ViewportDragMode>(ModelHudMode(operation));
    if(operation>=22)app.EditMode()=ViewportEditMode::Scale;
    app.PreviousMouseX()=100;app.PreviousMouseY()=100;app.MouseX()=100+dx;app.MouseY()=100+dy;
    app.ShiftModifierState()=(modifier&1)?3:0;app.CtrlModifierState()=(modifier&2)?3:0;}
void ModelHudNativeSample(MMDApp& app,int operation,int dx,int dy,unsigned modifier=0){ModelHudPointer(app,operation,dx,dy,modifier);
    if(operation<22)BoneEditModes(&app);else if(operation<=25)bone_scale::Drag(&app,operation-23);
    else if(operation<=28)bone_scale::DragScreen(&app,operation-26);else bone_scale::DragScreenPlane(&app);}
MmsResult ModelHudBoundSample(MMDApp& app,int operation,int dx,int dy,unsigned modifier=0){ModelHudPointer(app,operation,dx,dy,modifier);
    return plugins::ApplyModelHudSample(app,operation);}
struct ModelHudInvariant{
    std::vector<mdl::BoneKey> keys;std::uint64_t pool,ring,other;NumpadView camera;
    std::array<mdl::BoneRecord,2> metadata;std::array<std::uint8_t,4> ik{};
    bone_scale::AuthoringState scale;std::unique_ptr<bone_curve::AuthoringState> curves;
    explicit ModelHudInvariant(ModelHudFixture& f):keys(f.first.m->boneKeys,f.first.m->boneKeys+mdl::BoneKeyCapacity(*f.first.m)),
        pool(mdl::BoneKeyPoolIdentity(f.first.m->boneKeys)),ring(PoseRingDigest(f.first)),other(f.second.Digest()),camera(NumpadRead(*f.app)),metadata(f.first.bones),
        scale(bone_scale::AuthoringState::Capture(f.first.raw())),curves(bone_curve::AuthoringState::Capture(f.first.raw())){
        for(unsigned i=0;i<4;++i)ik[i]=f.first.m->displayKeys[i].ikStates[0];}
    void Unchanged(ModelHudFixture& f)const{auto actual=f.first.bones;
        for(unsigned i=0;i<2;++i){std::memcpy(actual[i].trans,metadata[i].trans,sizeof(actual[i].trans));std::memcpy(actual[i].rotQuat,metadata[i].rotQuat,sizeof(actual[i].rotQuat));}
        bool sameIk=true;for(unsigned i=0;i<4;++i)sameIk&=ik[i]==f.first.m->displayKeys[i].ikStates[0];
        Check(mdl::BoneKeyPoolIdentity(f.first.m->boneKeys)==pool&&keys.size()==mdl::BoneKeyCapacity(*f.first.m)&&
            !std::memcmp(keys.data(),f.first.m->boneKeys,keys.size()*sizeof(mdl::BoneKey))&&PoseRingDigest(f.first)==ring&&
            !std::memcmp(actual.data(),metadata.data(),sizeof(metadata))&&sameIk&&f.first.m->ikChainCount==1&&
            f.first.m->boneOrderCount==1&&scale.Equals(bone_scale::AuthoringState::Capture(f.first.raw()))&&
            curves->Equals(*bone_curve::AuthoringState::Capture(f.first.raw()))&&f.second.Digest()==other&&NumpadSame(camera,NumpadRead(*f.app)),
            "model HUD writes only bone pose/scale/dirty, preserving exact native keys, free authors, IK, metadata, other model, camera and old rings");}
};
void ModelHudMathAndHistory(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(int operation:modelHudOperations)for(int coordinate:{0,1,2})for(unsigned modifier:{0u,1u,2u,3u})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        if(coordinate==0&&modifier==0&&mode==Mode::AllOperations){std::printf("TRACE model HUD math operation=%d\n",operation);std::fflush(stdout);}
        const bool trace=operation==3&&coordinate==0&&modifier==0;
        ModelHudFixture f,oracle;f.app->state.coordinateSystem=oracle.app->state.coordinateSystem=coordinate;
        // Ordinary HUD operations must also work when no scale side entry has
        // ever existed. Scale operations exercise nonidentity pose separately.
        if(operation>=22){bone_scale::SetPose(f.first.raw(),0,Scale(1.125f));bone_scale::SetPose(oracle.first.raw(),0,Scale(1.125f));
            f.app->EditMode()=oracle.app->EditMode()=ViewportEditMode::Scale;}
        const auto before=ModelHudRead(f.first);const ModelHudInvariant invariant(f);
        plugins::NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"configure model HUD history policy");
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,operation)==MMS_OK,"reserve complete model/bone HUD before first native pose write");
        if(trace){std::printf("TRACE model HUD first begin mode=%u\n",unsigned(mode));std::fflush(stdout);}
        Check(f.history.SetMode(mode)==MMS_E_BUSY,"model HUD owns one common history reservation throughout its held samples");
        for(unsigned sample=0;sample<3;++sample){const int dx=sample==1?-5:9,dy=sample==1?7:-11;
            ModelHudNativeSample(*oracle.app,operation,dx,dy,modifier);
            Check(ModelHudBoundSample(*f.app,operation,dx,dy,modifier)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),ModelHudRead(oracle.first))&&!f.history.UndoCount(),
                "isolated model HUD samples match original float/quaternion/selected-root/scale math at all coordinate and modifier settings without partial history");}
        const auto after=ModelHudRead(f.first);Check(!ModelHudSame(before,after),"each admitted model HUD family changes actual selected native pose or scale");
        if(trace){std::puts("TRACE model HUD first samples complete");std::fflush(stdout);}
        bool changed=false;failAfter=0;const auto result=plugins::EndModelHudGesture(*f.app,changed);failAfter=-1;
        Check(result==MMS_OK&&changed&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"model HUD publishes one adjustment, or one explicit-only preview, without allocation");
        if(trace){std::puts("TRACE model HUD first End complete");std::fflush(stdout);}
        invariant.Unchanged(f);
        if(operation<22)Check(!bone_scale::HasPoseStorage(f.first.raw()),"ordinary bone HUD never creates or swaps absent scale storage");
        if(mode==Mode::AllOperations){f.app->CurrentFrame()=79;f.app->SelectedModelSlot()=1;f.app->CameraMode()=1;f.app->state.coordinateSystem=2;
            Check(f.history.History(false)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),before)&&f.app->CurrentFrame()==79&&f.app->SelectedModelSlot()==1,
                "model HUD undo owns exact original unregistered quaternion and scale across frame/model/editor switches without seeking");
            if(trace){std::puts("TRACE model HUD first Undo complete");std::fflush(stdout);}
            Check(f.history.History(true)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),after),"model HUD redo replays captured actual pose without native keys or source files");invariant.Unchanged(f);}
        if(trace){std::puts("TRACE model HUD first case complete");std::fflush(stdout);}
    }
}
void ModelHudForeignAndCancellation(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation:{9,12,23})for(UINT message:{UINT(WM_CANCELMODE),UINT(WM_KILLFOCUS),UINT(WM_CAPTURECHANGED),UINT(WM_ACTIVATEAPP),UINT(WM_KEYDOWN),UINT(WM_CLOSE),UINT(WM_DESTROY),UINT(WM_NCDESTROY)}){
        ModelHudFixture f;if(operation>=22)f.app->EditMode()=ViewportEditMode::Scale;plugins::NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);
        const auto before=ModelHudRead(f.first);const ModelHudInvariant invariant(f);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,operation)==MMS_OK&&ModelHudBoundSample(*f.app,operation,9,-11)==MMS_OK,"begin model HUD before lifecycle cancellation");
        failAfter=0;plugins::HandleBoundViewportMessage(*f.app,nullptr,message,message==WM_KEYDOWN?VK_ESCAPE:0,message==WM_CAPTURECHANGED?LPARAM(1):0);failAfter=-1;
        Check(!plugins::ModelHudGestureActive(*f.app)&&ModelHudSame(ModelHudRead(f.first),before)&&!f.history.UndoCount(),"all model HUD lifecycle cancellations restore owned pose and scale without allocations or a history node");invariant.Unchanged(f);
    }
    for(bool sameAxis:{false,true}){ModelHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ModelHudRead(f.first);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12)==MMS_OK&&ModelHudBoundSample(*f.app,12,9,-11)==MMS_OK,"begin axis HUD before foreign per-component pose edits");
        const unsigned axis=sameAxis?0:1;f.first.bones[0].trans[axis]+=8;const auto foreign=f.first.bones[0].trans[axis];f.first.bones[0].rotQuat[2]=.125f;
        plugins::CancelModelHudGesture(*f.app);auto expected=before;expected.bones[0][axis]=foreign;expected.bones[0][5]=.125f;
        // A foreign geometry write prevents restoration of the shared dirty bit.
        expected.dirty[0]=1;
        Check(ModelHudSame(ModelHudRead(f.first),expected)&&!f.history.UndoCount(),"cancellation restores still-owned individual translation components while preserving external same-axis writes, atomic foreign quaternion and its dirty flag");}
    {ModelHudFixture f;f.first.selected={0,0};f.app->EditMode()=ViewportEditMode::Scale;plugins::NativePoseTestBinding binding(*f.app,f.history);
        const auto before=ModelHudRead(f.first);Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,22)==MMS_OK&&f.first.selected[0]==1,"scale HUD reserves before synchronizing an unmarked selected bone");
        Check(ModelHudBoundSample(*f.app,22,0,-9)==MMS_OK,"auto-selected scale HUD changes an actual pose matrix");failAfter=0;plugins::CancelModelHudGesture(*f.app);failAfter=-1;
        Check(f.first.selected==std::array<unsigned char,2>{0,0}&&ModelHudSame(ModelHudRead(f.first),before),"scale cancellation restores only its owned auto-selection bit and exact original pose");}
    {ModelHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ModelHudRead(f.first);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12)==MMS_OK&&ModelHudBoundSample(*f.app,12,0,-9)==MMS_OK,"prepare selective model pose history");bool changed=false;
        Check(plugins::EndModelHudGesture(*f.app,changed)==MMS_OK&&changed,"commit selective bone pose before an independent write");const auto after=ModelHudRead(f.first);
        f.first.bones[0].trans[1]+=6;f.first.bones[0].rotQuat[2]=.2f;bone_scale::SetPose(f.first.raw(),1,Scale(2.375f));
        auto expectedBefore=before,expectedAfter=after;for(auto* value:{&expectedBefore,&expectedAfter}){value->bones[0][1]=f.first.bones[0].trans[1];value->bones[0][5]=.2f;value->scale=bone_scale::PoseValues(f.first.raw());}
        Check(f.history.History(false)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),expectedBefore),"undo patches only its recorded translation axes and preserves unrelated unregistered quaternion, scale and axis");
        Check(f.history.History(true)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),expectedAfter),"redo keeps the same independent author fields");}
}
void ModelHudSelectionAxesAndAuthors(){
    for(unsigned variant=0;variant<7;++variant)for(int operation:{3,9,12,23,26}){
        ModelHudFixture f,oracle;
        for(auto* fixture:{&f,&oracle}){
            if(operation>=22)fixture->app->EditMode()=ViewportEditMode::Scale;
            if(variant==0)fixture->first.selected[1]=0;
            if(variant==1)fixture->first.bones[1].parent=0;
            if(variant==2){fixture->first.bones[0].type=mdl::BoneType::FixedAxis;fixture->first.bones[0].flags|=mdl::kBoneFlagFixedAxis;
                fixture->first.bones[0].axis[0]=.4f;fixture->first.bones[0].axis[1]=.7f;fixture->first.bones[0].axis[2]=.2f;}
            if(variant==3){fixture->first.bones[0].type=mdl::BoneType::UnderIk;fixture->first.bones[0].flags|=mdl::kBoneFlagFixedAxis;fixture->first.m->physicsMode=0;}
            if(variant==4){fixture->app->state.physicsEditorJointPage=1;fixture->app->state.coordinateSystem=0;}
            if(variant==5){bone_scale::SetPose(fixture->first.raw(),0,Scale(1.875f));bone_scale::ApplyTransforms(fixture->first.raw());}
            if(variant==6){WriteOwnedCurve(fixture->first,0,12.25);WriteOwnedCurve(fixture->first,1,27.5);
                // Preserve a deliberately stale stored native hash too. A pose
                // draft may not call the source curve synchronizer to prune it.
                fixture->first.m->boneKeys[0].position[0]+=1.25f;
                bone_scale::SetKey(fixture->first.raw(),1,Scale(1.375f));bone_scale::SetKeyCurve(fixture->first.raw(),1,{3,97,99,113});
                bone_scale::CaptureUndo(fixture->first.raw());
            }
        }
        const auto before=ModelHudRead(f.first);const auto keys=RegistrationKeys(f.first);const auto ring=PoseRingDigest(f.first);
        const auto author=bone_curve::AuthoringState::Capture(f.first.raw());const auto scaleAuthor=bone_scale::AuthoringState::Capture(f.first.raw());
        plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,operation)==MMS_OK,"begin model HUD with selected-child/fixed-axis/direct-axis/applied-scale/free-author source");
        ModelHudNativeSample(*oracle.app,operation,9,-11);Check(ModelHudBoundSample(*f.app,operation,9,-11)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),ModelHudRead(oracle.first)),
            "model HUD preserves actual selected-root filtering, authored fixed axes, direct mode and once-applied scale cache math");
        bool changed=false;Check(plugins::EndModelHudGesture(*f.app,changed)==MMS_OK,"complete actual selected-root or fixed-axis model HUD");
        Check(SameRegistrationKeys(f.first,keys)&&PoseRingDigest(f.first)==ring&&author->Equals(*bone_curve::AuthoringState::Capture(f.first.raw()))&&
            scaleAuthor.Equals(bone_scale::AuthoringState::Capture(f.first.raw())),"pose draft and publication never synchronize stale free author hashes, change native keys/scale curves or write the old ring");
        if(changed)Check(f.history.History(false)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),before),"root/child and fixed-axis history owns exact original author pose including applied nonidentity scale");
    }
}
void ModelHudIdentityFailureAndRedo(){
    for(unsigned reason=0;reason<11;++reason){ModelHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12)==MMS_OK&&ModelHudBoundSample(*f.app,12,0,-9)==MMS_OK,"begin model HUD before source identity or UI invalidation");
        const auto before=ModelHudRead(f.first);auto* pool=f.first.m->boneKeys;mdl::BoneKey* replacement=nullptr;
        switch(reason){case 0:++f.app->CurrentFrame();break;case 1:f.first.m->selectedBone=1;break;case 2:f.app->SelectedModelSlot()=1;break;
        case 3:f.app->state.coordinateSystem=2;break;case 4:f.first.selected[1]=0;break;case 5:f.app->CameraMode()=1;break;
        case 6:plugins::ForgetSceneObject(f.first.raw());plugins::SceneObjectIdentity(f.first.raw());break;
        case 7:f.first.m->boneTable=f.second.bones.data();break;case 8:--f.first.m->boneCount;break;
        case 9:replacement=mdl::BoneKeyPoolCreate(32);std::memcpy(replacement,pool,32*sizeof(mdl::BoneKey));f.first.m->boneKeys=replacement;break;
        case 10:f.app->ModelSlot(0)=f.second.raw();break;}
        Check(!plugins::AllowModelHudPreview(*f.app,12)&&!plugins::ModelHudGestureActive(*f.app)&&!f.history.UndoCount(),"changed model domain cancels its reserved HUD and cannot commit against a recycled object, key pool or selection");
        if(reason==0||reason>=6)Check(ModelHudSame(ModelHudRead(f.first),before),"changed evaluation frame or resource identity never writes the previous pose into a different domain");
        f.first.m->boneTable=f.first.bones.data();f.first.m->boneCount=2;f.app->ModelSlot(0)=f.first.raw();
        if(replacement){f.first.m->boneKeys=pool;mdl::BoneKeyPoolDestroy(replacement);}
    }
    {ModelHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ModelHudRead(f.first);bool succeeded=false;unsigned failures=0;
        for(long allowance=0;allowance<200;++allowance){failAfter=allowance;const auto result=plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12);failAfter=-1;
            Check(ModelHudSame(ModelHudRead(f.first),before)&&!f.history.UndoCount(),"every model HUD preparation failure leaves all authored pose and history intact");
            if(result==MMS_OK){succeeded=true;plugins::CancelModelHudGesture(*f.app);break;}Check(result==MMS_E_MEMORY&&!plugins::ModelHudGestureActive(*f.app),"failed model HUD draft/reservation is never exposed");++failures;}
        Check(succeeded&&failures>8,"model HUD allocation sweep reaches retained owner, preallocated pose/rollback and complete history reservation");
        plugins::StorageStore storage;plugins::SharedHistory tiny(storage,{},1,1);
        Check(plugins::BeginModelHudGesture(*f.app,tiny,nullptr,12)==MMS_E_MEMORY&&ModelHudSame(ModelHudRead(f.first),before),"retained budget rejects a model HUD before any author write");
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12)==MMS_OK&&ModelHudBoundSample(*f.app,12,0,-9)==MMS_OK,"seed model HUD redo");bool changed=false;
        Check(plugins::EndModelHudGesture(*f.app,changed)==MMS_OK&&changed&&f.history.History(false)==MMS_OK,"undo complete model HUD to seed redo");
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12)==MMS_OK&&ModelHudBoundSample(*f.app,12,0,0)==MMS_OK&&plugins::EndModelHudGesture(*f.app,changed)==MMS_OK&&!changed&&
            f.history.RedoCount()==1&&ModelHudSame(ModelHudRead(f.first),before),"native zero-delta dirty-only sample is an exact no-op and preserves common redo");}
    for(int operation:{22,26,29}){unsigned failures=0;bool succeeded=false;
        for(long allowance=0;allowance<1024;++allowance){ModelHudFixture f;f.app->EditMode()=ViewportEditMode::Scale;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ModelHudRead(f.first);
            Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,operation)==MMS_OK&&ModelHudBoundSample(*f.app,operation,9,-11)==MMS_OK,"accept first scale HUD preview before private next-sample OOM sweep");
            ModelHudPointer(*f.app,operation,-5,7);failAfter=allowance;const auto result=plugins::ApplyModelHudSample(*f.app,operation);failAfter=-1;
            if(result==MMS_OK){succeeded=true;plugins::CancelModelHudGesture(*f.app);Check(ModelHudSame(ModelHudRead(f.first),before),"successful scale sample remains cancellable");break;}
            Check(result==MMS_E_MEMORY&&!plugins::ModelHudGestureActive(*f.app)&&ModelHudSame(ModelHudRead(f.first),before)&&!f.history.UndoCount(),
                "failure anywhere in scale matrices/selected-root draft restores prior previews atomically without partial bones, scale or journal");++failures;}
        Check(succeeded&&failures>3,"scale HUD failure sweep reaches all private math allocations before live publication");}
}
void ModelHudNetZeroAndLease(){
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        ModelHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);
        for(auto& bone:f.first.bones)bone.trans[0]=0;
        f.app->state.followCameraEnabled=1;f.app->CameraParentModel()=0;f.app->state.viewDirty=0;f.app->WindowLayoutReady()=1;
        const auto before=ModelHudRead(f.first);Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12)==MMS_OK,"begin exactly cancelling native translation samples");
        ModelHudPointer(*f.app,12,0,-1);ModePhysicsBody(f.app.get(),0);ModelHudPointer(*f.app,12,0,1);ModePhysicsBody(f.app.get(),0);
        Check(f.first.bones[0].trans[0]==0&&f.first.dirty[0]==1,"opposite unit-axis native samples cancel geometrically while still marking physical dirty");
        bool changed=true;failAfter=0;const auto result=plugins::EndModelHudGesture(*f.app,changed);failAfter=-1;
        Check(result==MMS_OK&&!changed&&ModelHudSame(ModelHudRead(f.first),before)&&!f.history.UndoCount()&&!f.app->state.viewDirty&&f.app->WindowLayoutReady()==1,
            "zero-net HUD restores owned physics dirty and transient refresh hints without allocation or invisible history");
    }
    {ModelHudFixture f;Check(!bone_scale::HasPoseStorage(f.first.raw()),"lease accounting starts from absent scale runtime");
        {auto fresh=bone_scale::PoseStorageLease::Prepare(f.first.raw());auto shared=bone_scale::PoseStorageLease::Prepare(f.first.raw());
            const auto emptyNodes=61ull*(2*(sizeof(bone_scale::Values::value_type)+4*sizeof(void*))+sizeof(bone_scale::Curves::value_type)+4*sizeof(void*));
            Check(fresh->RetainedBytes()>emptyNodes&&shared->RetainedBytes()<fresh->RetainedBytes()-emptyNodes,
                "fresh pose lease charges its new State/hash node, all 183 Windows empty-map sentinels and reserved pose nodes; shared existing state is not charged again");}
        Check(!bone_scale::HasPoseStorage(f.first.raw()),"cancelling unused fresh scale leases removes only their own empty runtime entry");}
    {ModelHudFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ModelHudRead(f.first);bool changed=false;
        Check(!bone_scale::HasPoseStorage(f.first.raw())&&plugins::BeginModelHudGesture(*f.app,f.history,nullptr,9)==MMS_OK&&ModelHudBoundSample(*f.app,9,0,-7)==MMS_OK&&
            plugins::EndModelHudGesture(*f.app,changed)==MMS_OK&&changed&&!bone_scale::HasPoseStorage(f.first.raw()),"ordinary quaternion HUD completes without acquiring scale storage");
        const auto after=ModelHudRead(f.first);Check(f.history.History(false)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),before)&&!bone_scale::HasPoseStorage(f.first.raw())&&
            f.history.History(true)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),after)&&!bone_scale::HasPoseStorage(f.first.raw()),
            "ordinary HUD undo/redo never tries to publish a scale table against an absent runtime entry");}
}
void ModelHudTailReferenceAdmission(){
    const char* names[]={"\x89\x45\x8E\xE8\x8E\xF1","\x89\x45\x82\xD0\x82\xB6","\x89\x45\x98\x72",
        "\x8D\xB6\x8E\xE8\x8E\xF1","\x8D\xB6\x82\xD0\x82\xB6","\x8D\xB6\x98\x72","prefix\x8E\x77" "suffix"};
    for(const char* name:names)for(int tail:{-2,-1,1,97})for(int operation:{9,10,11,12,13,14,23,24,25}){
        ModelHudFixture f;f.first.m->boneCount=1;f.first.m->physicsMode=0;f.app->state.coordinateSystem=0;
        std::memset(f.first.bones[0].localAxes,0,sizeof(f.first.bones[0].localAxes));std::memset(f.first.bones[0].name,0,sizeof(f.first.bones[0].name));
        std::memcpy(f.first.bones[0].name,name,std::strlen(name));f.first.bones[0].tailBone=tail;
        if(operation>=22)f.app->EditMode()=ViewportEditMode::Scale;const auto before=ModelHudRead(f.first);plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,operation)==MMS_E_NOT_READY&&!plugins::ModelHudGestureActive(*f.app)&&
            ModelHudSame(ModelHudRead(f.first),before)&&!f.history.UndoCount(),"actual SJIS arm/wrist/elbow/finger local-axis tail dereference is rejected before private math when outside the real one-bone table");
        f.first.m->boneCount=2;
    }
    for(unsigned branch=0;branch<7;++branch){ModelHudFixture f;f.first.m->boneCount=1;f.app->state.coordinateSystem=0;f.first.bones[0].tailBone=-1;
        std::memset(f.first.bones[0].name,0,sizeof(f.first.bones[0].name));std::memcpy(f.first.bones[0].name,names[0],std::strlen(names[0]));
        std::memset(f.first.bones[0].localAxes,0,sizeof(f.first.bones[0].localAxes));f.first.m->physicsMode=0;int operation=12;
        if(branch==0)operation=3;else if(branch==1)operation=6;else if(branch==2)f.app->state.coordinateSystem=1;
        else if(branch==3){operation=23;f.app->state.coordinateSystem=1;}
        else if(branch==4)operation=26;
        else if(branch==5){f.first.m->physicsMode=2;f.first.bones[0].localAxes[0]=1;f.first.bones[0].localAxes[5]=1;}
        else{operation=9;f.app->state.physicsEditorJointPage=1;}
        if(operation>=22)f.app->EditMode()=ViewportEditMode::Scale;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,operation)==MMS_OK&&ModelHudBoundSample(*f.app,operation,9,-11)==MMS_OK,
            "named terminal bone remains usable for HUD math that bypasses the tail-derived local axes, including explicit PMX axes, direct axes and view-only scale gizmos");
        plugins::CancelModelHudGesture(*f.app);f.first.m->boneCount=2;
    }
    {ModelHudFixture f;f.first.m->boneCount=1;f.first.bones[0].tailBone=-1;f.app->state.coordinateSystem=0;f.first.m->physicsMode=0;
        std::memset(f.first.bones[0].name,0,sizeof(f.first.bones[0].name));std::memcpy(f.first.bones[0].name,"ordinary terminal",18);
        plugins::NativePoseTestBinding binding(*f.app,f.history);Check(plugins::BeginModelHudGesture(*f.app,f.history,nullptr,12)==MMS_OK&&ModelHudBoundSample(*f.app,12,0,-7)==MMS_OK,
            "ordinary terminal bones with tail=-1 retain local-axis HUD support");plugins::CancelModelHudGesture(*f.app);f.first.m->boneCount=2;}
}
struct ModelHudReleaseWindow{
    inline static thread_local ModelHudReleaseWindow* active=nullptr;
    ModelHudFixture& fixture;HWND owner,edit,floating;WNDPROC ownerProc,editProc;RECT originalView{};POINT cursor{};bool cursorValid,armed=false,observed=false;
    ModelHudPose original{},replacement{};std::shared_ptr<const plugins::HistoryChange> oldSource,newSource;
    explicit ModelHudReleaseWindow(ModelHudFixture& f):fixture(f),owner(f.app->MainWindow()),edit(GetDlgItem(owner,panel::kReadoutPosXEdit)),floating(f.app->FloatingWindow()),
        ownerProc(nullptr),editProc(nullptr),originalView(f.app->ViewportRect()),cursorValid(GetCursorPos(&cursor)!=FALSE){Check(!active&&edit,"model HUD reentry fixture uses a real native EDIT");active=this;
        ownerProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
        editProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(EditProc)));
        Check(ownerProc&&editProc,"model HUD fixture forwards actual MicWndProc and native EDIT procedures");f.app->FloatingWindow()=owner;HudFixtureGeometry(*f.app);}
    ~ModelHudReleaseWindow(){armed=false;plugins::CancelModelHudGesture(*fixture.app);if(GetCapture()==owner)ReleaseCapture();
        SetWindowLongPtrW(edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(editProc));SetWindowLongPtrW(owner,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ownerProc));
        fixture.app->FloatingWindow()=floating;fixture.app->ViewportRect()=originalView;active=nullptr;if(cursorValid)SetCursorPos(cursor.x,cursor.y);}
    void Cursor(int x,int y){HudFixtureCursor(*fixture.app,owner,{x,y},
        "place actual OS model HUD cursor independently from stale message lParam",
        "real model HUD cursor round-trips through actual screen and client coordinates");}
    static LRESULT CALLBACK EditProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){auto* self=active;const auto proc=self->editProc;
        if(message==WM_SETTEXT&&self->armed){self->armed=false;self->observed=true;auto& f=self->fixture;
            Check(!ModelHudSame(ModelHudRead(f.first),self->original),"model release readout callback sees a real accepted native pose sample");
            SendMessageW(self->owner,WM_CANCELMODE,0,0);Check(ModelHudSame(ModelHudRead(f.first),self->original)&&!plugins::ModelHudGestureActive(*f.app),"synchronous readout cancellation restores the old accepted pose before replacement");
            Check(plugins::BeginModelHudGesture(*f.app,f.history,self->owner,13)==MMS_OK,"readout callback reserves a different same-owner model HUD gesture");SetCapture(self->owner);
            Check(ModelHudBoundSample(*f.app,13,0,-3)==MMS_OK,"replacement model HUD authors its independent real translation axis");
            self->replacement=ModelHudRead(f.first);self->newSource=plugins::ModelHudPreviewSource(*f.app);
            f.app->MouseX()=333;f.app->MouseY()=444;f.app->PreviousMouseX()=300;f.app->PreviousMouseY()=400;
            f.app->ViewToolDragOperation()=static_cast<ViewportToolAction>(13);f.app->InteractionDragMode()=static_cast<ViewportDragMode>(11);
            f.app->LeftMouseButtonState()=3;f.app->state.a06B5=1;
        }return CallWindowProcA(proc,hwnd,message,wp,lp);}
};
void ModelHudRealPress(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation:{3,4,5,6,7,8,9,12,22,23,26,27,28,29}){
        ModelHudFixture f;const HWND owner=f.app->MainWindow();RECT previous{};GetWindowRect(owner,&previous);
        const auto corner=viewport_corner_settings::Current();viewport_corner_settings::ResetCacheForTest(viewport_tool_layout::Corner::BottomRight);
        SetWindowPos(owner,nullptr,0,0,800,600,SWP_NOZORDER|SWP_NOACTIVATE);
        {ModelHudReleaseWindow window(f);plugins::NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);
            f.app->ViewportRect()=FrameOverlayRect(f.app.get());f.app->ViewportToolCenterX()=350;f.app->ViewportToolCenterY()=220;
            f.app->EditMode()=operation<=5?ViewportEditMode::Camera:operation<=8?ViewportEditMode::Light:operation>=22?ViewportEditMode::Scale:ViewportEditMode::Bone;
            const auto editMode=f.app->EditMode();const auto before=ModelHudRead(f.first);const auto ring=PoseRingDigest(f.first);
            POINT point{};const auto hud=viewport_tool_layout::Make(FrameOverlayRect(f.app.get()),FrameOverlayScale(f.app.get()),viewport_corner_settings::Current());
            if(operation<=8){point={350,220};if(operation==3)point.x+=20;else if(operation==4)point.y+=20;else if(operation==5)point.x+=40;
                else if(operation==6)point.x+=50;else if(operation==7)point.y-=50;}
            else if(operation<22){point.x=LONG(hud.x-110.f*hud.scale);point.y=LONG(hud.y-(operation==9?105.f:65.f)*hud.scale);}
            else if(operation<=25){point.x=LONG(hud.ScaleCenterX(operation-22));point.y=LONG(hud.y-25.f*hud.scale);}
            else{const auto& geometry=bone_scale::gizmo::Published(f.app.get());if(operation==29){point.x=75;point.y=75;}
                else{point.x=LONG(geometry.tipX[operation-26]);point.y=LONG(geometry.tipY[operation-26]);}}
            f.app->MouseX()=point.x;f.app->MouseY()=point.y;Check(ViewportToolAtPointForTest(f.app.get())==operation,"real model HUD press hits its displayed row, ring, move handle or published scale gizmo");
            window.Cursor(point.x,point.y);SendMessageW(owner,WM_LBUTTONDOWN,0,MAKELPARAM(point.x,point.y));
            Check(plugins::ModelHudGestureActive(*f.app)&&GetCapture()==owner&&int(f.app->InteractionDragMode())==ModelHudMode(operation)&&
                !f.history.UndoCount()&&PoseRingDigest(f.first)==ring,"actual MicWndProc press enters owned model HUD and original drag mode without old PushUndo/ring writes");
            // A press-only click at the centred warp location must never apply
            // the distance from the visible button to the centre as a delta.
            f.app->PreviousMouseX()=f.app->MouseX();f.app->PreviousMouseY()=f.app->MouseY();window.Cursor(f.app->MouseX(),f.app->MouseY());
            SendMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(1,1));
            Check(!plugins::ModelHudGestureActive(*f.app)&&!f.history.UndoCount()&&ModelHudSame(ModelHudRead(f.first),before)&&
                f.app->EditMode()==editMode&&f.app->InteractionDragMode()==ViewportDragMode::None&&PoseRingDigest(f.first)==ring&&GetCapture()!=owner,
                "real press/release centre warp is an exact no-op, restores the original edit cursor and does not consume history or capture");
        }
        SetWindowPos(owner,nullptr,previous.left,previous.top,previous.right-previous.left,previous.bottom-previous.top,SWP_NOZORDER|SWP_NOACTIVATE);
        viewport_corner_settings::ResetCacheForTest(corner);
    }
}
void ModelHudRealReleaseAndReentry(){
    using Mode=plugins::SharedHistory::RecordingMode;
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool directToolRelease:{false,true}){ModelHudFixture f;ModelHudReleaseWindow window(f);plugins::NativePoseTestBinding binding(*f.app,f.history);f.history.SetMode(mode);
        window.original=ModelHudRead(f.first);const ModelHudInvariant invariant(f);PostViewRefresh(f.app.get());
        Check(plugins::BeginModelHudGesture(*f.app,f.history,window.owner,12)==MMS_OK,"reserve real MicWndProc model release");SetCapture(window.owner);
        f.app->InteractionDragMode()=static_cast<ViewportDragMode>(10);f.app->PreviousMouseX()=f.app->MouseX()=100;f.app->PreviousMouseY()=f.app->MouseY()=100;
        window.oldSource=plugins::ModelHudPreviewSource(*f.app);window.Cursor(100,107);window.armed=true;
        // A WM_SETTEXT callback cancels and rebuilds under the same owner. An
        // old release must neither End the new token nor clear its capture.
        if(directToolRelease)HandleViewportToolRelease(f.app.get());else SendMessageW(window.owner,WM_LBUTTONUP,0,MAKELPARAM(1,1));
        Check(window.observed&&window.oldSource!=window.newSource&&plugins::ModelHudPreviewSource(*f.app)==window.newSource&&GetCapture()==window.owner&&
            ModelHudSame(ModelHudRead(f.first),window.replacement)&&!f.history.UndoCount()&&f.app->MouseX()==333&&f.app->MouseY()==444&&
            f.app->PreviousMouseX()==300&&f.app->PreviousMouseY()==400&&f.app->LeftMouseButtonState()==3&&f.app->state.a06B5==1&&
            f.app->ViewToolDragOperation()==static_cast<ViewportToolAction>(13)&&f.history.SetMode(mode)==MMS_E_BUSY,
            "old model mouse-up preserves replacement source/token/pose/mouse snapshots and same-owner capture after synchronous EDIT reentry");
        bool changed=false;Check(plugins::EndModelHudGesture(*f.app,changed)==MMS_OK&&changed&&f.history.UndoCount()==(mode==Mode::AllOperations?1u:0u),"replacement model HUD independently commits once");ReleaseCapture();
        if(mode==Mode::AllOperations)Check(f.history.History(false)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),window.original),"replacement model undo proves the old release never consumed its history token");invariant.Unchanged(f);
    }
    {ModelHudFixture f;ModelHudReleaseWindow window(f);plugins::NativePoseTestBinding binding(*f.app,f.history);const auto before=ModelHudRead(f.first);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,window.owner,12)==MMS_OK,"begin final coalesced model HUD release sample");SetCapture(window.owner);
        ModelHudPointer(*f.app,12,0,0);window.Cursor(100,107);SendMessageW(window.owner,WM_LBUTTONUP,0,MAKELPARAM(1,1));
        const float expected=float(double(before.bones[0][0])+double(-7)*0.05000000074505806);
        Check(!plugins::ModelHudGestureActive(*f.app)&&f.history.UndoCount()==1&&f.first.bones[0].trans[0]==expected&&GetCapture()!=window.owner,
            "real release samples OS client cursor rather than queued stale lParam, commits once and releases only its own capture");
        Check(f.history.History(false)==MMS_OK&&ModelHudSame(ModelHudRead(f.first),before),"one common undo removes the actual final native release pose");
        const HWND stealer=CreateWindowExW(0,L"STATIC",L"model HUD capture replacement",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(stealer!=nullptr,"create real model HUD capture replacement");SetCapture(window.owner);
        Check(plugins::BeginModelHudGesture(*f.app,f.history,window.owner,12)==MMS_OK&&ModelHudBoundSample(*f.app,12,0,-7)==MMS_OK,"accept model HUD preview before native capture transfer");
        Check(PostMessageW(window.owner,WM_LBUTTONUP,0,MAKELPARAM(1,1))!=FALSE,"queue stale model release before transfer");SetCapture(stealer);
        MSG message{};Check(PeekMessageW(&message,window.owner,WM_LBUTTONUP,WM_LBUTTONUP,PM_REMOVE)!=FALSE,"retrieve queued model HUD release after capture transfer");DispatchMessageW(&message);
        Check(!plugins::ModelHudGestureActive(*f.app)&&ModelHudSame(ModelHudRead(f.first),before)&&GetCapture()==stealer,"native capture cancellation rolls back model preview and old queued mouse-up leaves the new owner capture intact");
        ReleaseCapture();DestroyWindow(stealer);
    }
}
}
void NativeModelHudHistoryChecks(){ModelHudMathAndHistory();ModelHudForeignAndCancellation();ModelHudSelectionAxesAndAuthors();ModelHudIdentityFailureAndRedo();ModelHudNetZeroAndLease();ModelHudTailReferenceAdmission();ModelHudRealPress();ModelHudRealReleaseAndReentry();}
