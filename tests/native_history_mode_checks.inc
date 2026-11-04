namespace {
void NativeHistoryModeChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    {
        FovSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        for(int tick:{60,61,62,63})f.Scroll(tick);
        const auto bytes=f.history.RetainedBytes();
        Check(f.history.Configure(Mode::ExplicitOnly,2,1024*1024,[]{return MMS_E_FAILED;})==MMS_E_FAILED&&
            f.history.Mode()==Mode::AllOperations&&f.history.UndoCount()==4&&f.history.RetainedBytes()==bytes,
            "failed settings persistence leaves history mode limits and nodes unchanged");
        Check(f.history.Configure(Mode::AllOperations,2,1024*1024)==MMS_OK&&f.history.UndoCount()==2&&f.history.MaxEntries()==2,
            "smaller step limit retires oldest records and keeps newest undo nodes");
        Check(f.history.History(false)==MMS_OK&&f.app->CameraFov()==62&&f.history.History(false)==MMS_OK&&f.app->CameraFov()==61&&
            f.history.History(false)==MMS_E_NOT_FOUND,"trimmed history remains chronologically undoable");
        Check(f.history.Configure(Mode::AllOperations,1,1024*1024)==MMS_OK&&f.history.RedoCount()==1&&
            f.history.History(true)==MMS_OK&&f.app->CameraFov()==62,"shrinking redo removes farthest future and keeps next redo");
        Check(f.history.Configure(Mode::AllOperations,1,1)==MMS_OK&&!f.history.UndoCount()&&!f.history.RetainedBytes(),
            "memory budget retires oversized history without changing authored pose");
        const auto before=f.app->CameraFov();f.Scroll(90);
        Check(f.app->CameraFov()==before&&!f.history.UndoCount(),"over-budget operation is rejected before author mutation");
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"switch tiny budget to explicit-only");f.Scroll(95);
        Check(f.app->CameraFov()==95&&!f.history.UndoCount(),"unrecorded preview does not consume or require history budget");
    }
    {
        FovSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.Scroll(67);Check(f.history.History(false)==MMS_OK,"seed history mode redo");
        std::uint64_t before=0,after=0;Check(f.history.Revision(before)==MMS_OK,"read mode revision");
        f.app->SceneModified()=0;
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK&&!f.app->SceneModified()&&f.history.RedoCount()==1,
            "switching history mode keeps scene dirty flags and existing undo redo");
        f.Scroll(88);
        Check(f.app->CameraFov()==88&&!f.history.UndoCount()&&f.history.RedoCount()==1&&f.history.Revision(after)==MMS_OK&&after>before,
            "explicit-only adjustment publishes pose and revision without journal node or redo truncation");
        Check(f.history.History(true)==MMS_OK&&f.app->CameraFov()==67,"preexisting redo remains usable after an unrecorded adjustment");
        Check(f.history.SetMode(Mode::AllOperations)==MMS_OK,"enable all operations");f.Scroll(91);
        Check(f.history.UndoCount()==2&&f.history.History(false)==MMS_OK&&f.app->CameraFov()==67,
            "all-operations mode records later adjustments without resetting existing history");
    }
    {
        GlobalRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"choose explicit-only global fixture");
        bool changed=false;
        Check(plugins::ApplyBoundLightField(*f.app,0,.812345f,changed)==MMS_OK&&changed&&!f.history.UndoCount(),
            "light adjustment remains live without creating explicit-only history");
        const auto keys=GlobalRegistrationKeys(f.app->LightKeys());
        auto plan=GlobalRegistrationPlan(f,1,20);std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("Register light",plan,transaction)==MMS_OK,"prepare explicit registration in explicit-only mode");
        Check(f.history.SetMode(Mode::AllOperations)==MMS_E_BUSY,"pending native candidate prevents mid-operation mode change");
        Check(f.history.CommitNative(std::move(transaction))==MMS_OK&&f.history.UndoCount()==1,"registration still creates exactly one explicit history node");
        Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->LightKeys(),keys),"explicit registration undo restores original keys");
    }
    {
        MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"choose explicit-only morph fixture");f.Scroll(1,53);
        Check(f.firstMorphs[1].value==.53f&&!f.history.UndoCount(),"explicit-only morph slider previews without history");
        Check(f.history.SetMode(Mode::AllOperations)==MMS_OK,"choose all-operations morph fixture");f.Scroll(1,75);
        Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&f.firstMorphs[1].value==.53f,"all-operations morph edit restores prior preview value");
    }
    {
        PoseNumericFixture f;VpdNames(f);plugins::NativePoseTestBinding binding(*f.app,f.history);
        VpdInput input;input.Write(vpdRecord);const auto before=f.first.Digest();bool changed=false;
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"choose explicit-only VPD fixture");
        Check(plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed)==MMS_OK&&changed&&f.history.UndoCount()==1,
            "VPD import is an explicit commit even though it uses pose capture");
        Check(f.history.History(false)==MMS_OK&&f.first.Digest()==before,"explicit-only VPD import remains exactly undoable");
    }
    {
        PoseNumericFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(Mode::ExplicitOnly)==MMS_OK,"choose explicit-only pose fixture");
        f.Commit(panel::kReadoutPosYEdit,"6");
        Check(f.first.bones[0].trans[1]==6&&!f.history.UndoCount(),"already-applied bone adjustments obey explicit-only recording mode");
        Check(f.history.SetMode(Mode::AllOperations)==MMS_OK,"enable pose operation recording");f.Commit(panel::kReadoutPosYEdit,"7");
        Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&f.first.bones[0].trans[1]==6,"already-applied bone history obeys mode switching");
        Check(f.history.Configure(Mode::ExplicitOnly,1,1)==MMS_OK,"use tiny budget for already-applied preview");
        f.Commit(panel::kReadoutPosYEdit,"8");
        Check(f.first.bones[0].trans[1]==8&&!f.history.UndoCount(),"unrecorded bone preview is independent of retained history budget");
    }
}
}

#include "mikumikustudio/negative_frames.hpp"
#include "../src/animation/negative_history.hpp"
namespace {
struct NegativeGlobalFixture:LightSliderFixture {
    NegativeGlobalFixture() {
        app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(4);
        app->CameraKeys()[0].fov=45;app->CameraKeys()[0].parentModel=-1;app->CameraKeys()[0].perspective=1;
        app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(4);app->ShadowKeys()[0].distance=.02f;
        app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(3);app->GravityKeys()[0].acceleration=9.8f;
        app->GravityKeys()[0].direction[1]=-1;
    }
    ~NegativeGlobalFixture(){
        history.Reset();history.Collect();
        mdl::KeyPoolDestroy(app->CameraKeys());app->CameraKeys()=nullptr;
        mdl::KeyPoolDestroy(app->ShadowKeys());app->ShadowKeys()=nullptr;
        mdl::KeyPoolDestroy(app->GravityKeys());app->GravityKeys()=nullptr;
    }
};
void PrepareNegativeDisplayFixture(DisplayFixture& f) {
    auto& model=*f.first.m;
    model.ikChainCount=1;model.boneOrderCount=2;
    f.firstIk[0].boneIndex=0;f.firstIk[0].enabled=1;
    f.firstOrder[0].boneIndex=-1;f.firstOrder[0].linkedModel=-1;f.firstOrder[0].linkedBone=0;
    f.firstOrder[1].boneIndex=0;f.firstOrder[1].linkedModel=-1;f.firstOrder[1].linkedBone=0;
    for(unsigned slot=0;slot<4;++slot) {
        auto& key=model.displayKeys[slot];
        delete[] key.selectorStates;
        key.selectorStates=new mdl::BoneReference[2]{{-1,0},{-1,0}};
    }
    model.displayKeys[0].visible=1;model.displayKeys[0].ikStates[0]=1;
    model.loadComplete=1;
}
void NegativeBoundCommandHistoryChecks();
void NegativeBoundCandidateStaleChecks();
void NegativeBoundTimelineScopeChecks();
void NegativeBoundFailureSweepChecks();
void NegativeBoundCandidateScopeChecks();
void NegativeRegistrationHistoryChecks() {
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.app->CameraFov()=45;f.app->CurrentFrame()=-10;
        Check(f.history.SetMode(mode)==MMS_OK,"negative registration chooses common recording mode");
        Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10),"negative camera registration publishes");
        Check(f.history.UndoCount()==1&&negative_frames::Count(f.app.get())==1,"negative registration owns one common-history entry");
        negative_frames::SelectCell(f.app.get(),0,-999,false);
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(negative_frames::Count(f.app.get())==0&&f.app->CurrentFrame()==-10,
            "changed UI selection does not stale negative registration undo or move cursor");
        CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
        Check(negative_frames::Count(f.app.get())==1,"common redo restores negative keys");
        Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10)&&f.history.UndoCount()==1,
            "equal negative registration adds no journal entry");
        f.app->CurrentFrame()=20;
        std::vector<mdl::LightKey> lightBefore(f.app->LightKeys(),f.app->LightKeys()+4);
        std::shared_ptr<plugins::HistoryChange> positive;
        Check(plugins::PlanLightRegistration(*f.app,20,positive)==MMS_OK&&positive,"plan positive registration between negative entries");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("positive light",positive,token)==MMS_OK&&f.history.CommitNative(std::move(token))==MMS_OK,
            "positive key publication joins the same journal");
        f.app->CurrentFrame()=-5;
        f.app->LightDirection()[0]=.5f;
        Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Light,-15)&&f.history.UndoCount()==3,
            "another negative family joins the same ordered journal");
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(negative_frames::Count(f.app.get())==1&&f.history.UndoCount()==2,"first undo restores latest negative family");
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(GlobalRegistrationSame(f.app->LightKeys(),lightBefore)&&negative_frames::Count(f.app.get())==1&&f.history.UndoCount()==1,
            "negative playback cursor still undoes the intervening positive registration first");
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(negative_frames::Count(f.app.get())==0&&!f.history.UndoCount()&&f.app->CurrentFrame()==-5,
            "third undo restores earliest negative registration without seeking");
        for(int i=0;i<3;++i)CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
        Check(f.history.UndoCount()==3&&negative_frames::Count(f.app.get())==2,"real command redo traverses negative and positive entries");
        Check(f.history.Reset()==MMS_OK,"release negative history records before scene cleanup");f.history.Collect();
        negative_frames::Reset(f.app.get());
    }
    {
        NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.app->CameraFov()=45;f.app->SceneModified()=0;
        unsigned failures=0;
        for(long allocation=0;allocation<128;++allocation) {
            failAfter=allocation;
            const bool success=negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10);
            failAfter=-1;
            if(success){Check(failures>3,"negative registration failure sweep reaches multiple private allocations");break;}
            ++failures;
            Check(negative_frames::Count(f.app.get())==0&&!f.history.UndoCount()&&!f.app->SceneModified(),
                "failed negative registration leaves keys dirty state and journal untouched");
        }
        Check(f.history.UndoCount()==1,"negative registration eventually succeeds after allocation failures");
        Check(f.history.Reset()==MMS_OK,"release failure-sweep history");negative_frames::Reset(f.app.get());
        Check(f.history.Configure(plugins::SharedHistory::RecordingMode::AllOperations,1,1)==MMS_OK,"configure tiny negative history budget");
        f.app->SceneModified()=0;
        Check(!negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10)&&
            negative_frames::Count(f.app.get())==0&&!f.history.UndoCount()&&!f.app->SceneModified(),
            "negative key payload cannot bypass the configured journal memory budget");
        negative_frames::Reset(f.app.get());
    }
    {
        BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();
        f.app->SelectedModelSlot()=0;f.app->CurrentFrame()=-10;
        f.first.selected[0]=1;f.first.selected[1]=0;f.first.dirty[0]=1;
        Check(negative_frames::RegisterSelectedBones(f.first.raw(),-10)&&f.first.dirty[0]==0&&f.history.UndoCount()==1,
            "selected negative bone registration publishes its physics flag in the same record");
        Check(f.history.History(false)==MMS_OK&&negative_frames::Count(f.app.get())==0&&f.first.dirty[0]==1,
            "negative registration undo restores the original physics flag");
        Check(f.history.History(true)==MMS_OK&&f.first.dirty[0]==0,"negative registration redo restores physics flag publication");
        Check(f.history.Reset()==MMS_OK,"release negative model history");negative_frames::Reset(f.app.get());g_Block=previous;
    }

    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();
        for(auto& bone:f.first.bones)bone.slotIndex=-1;
        f.first.dirty[0]=0;f.first.dirty[1]=0;f.app->CurrentFrame()=-10;
        f.first.bones[1].trans[0]=99;Check(bone_scale::SetPose(f.first.raw(),1,Scale(3)),"prepare unrelated bone scale draft");
        Check(f.history.SetMode(mode)==MMS_OK,"bone negative pose selects common history mode");
        f.first.bones[0].trans[0]=4;Check(bone_scale::SetPose(f.first.raw(),0,Scale(2)),"prepare first negative bone scale");
        Check(negative_frames::RegisterBone(f.first.raw(),0,-10),"register initial negative bone pose");
        f.first.bones[0].trans[0]=8;f.first.bones[0].rotQuat[0]=.6f;f.first.bones[0].rotQuat[3]=.8f;
        Check(bone_scale::SetPose(f.first.raw(),0,Scale(4)),"prepare changed negative bone scale");
        Check(negative_frames::RegisterBone(f.first.raw(),0,-10),"overwrite negative bone pose");
        Check(f.history.History(false)==MMS_OK && f.first.bones[0].trans[0]==4 &&
            f.first.bones[0].rotQuat[0]==0 && f.first.bones[0].rotQuat[3]==1 && bone_scale::Pose(f.first.raw(),0)==Scale(2),
            "negative bone undo restores previous position rotation and scale");
        Check(f.history.History(false)==MMS_OK && f.first.bones[0].trans[0]==0 &&
            f.first.bones[0].rotQuat[0]==0 && f.first.bones[0].rotQuat[3]==1 && bone_scale::Pose(f.first.raw(),0)==Scale(1),
            "last negative bone undo restores independent native frame-zero channels");
        Check(f.history.History(true)==MMS_OK && f.first.bones[0].trans[0]==4 && bone_scale::Pose(f.first.raw(),0)==Scale(2),
            "negative bone redo restores first position and scale");
        Check(f.history.History(true)==MMS_OK && f.first.bones[0].trans[0]==8 &&
            f.first.bones[0].rotQuat[0]==.6f && f.first.bones[0].rotQuat[3]==.8f && bone_scale::Pose(f.first.raw(),0)==Scale(4),
            "negative bone redo restores overwritten position rotation and scale");
        Check(f.first.bones[1].trans[0]==99 && bone_scale::Pose(f.first.raw(),1)==Scale(3) &&
            f.first.dirty[0]==0 && f.app->CurrentFrame()==-10,
            "bone history keeps unrelated draft physics flag and signed playhead");
        Check(f.history.Reset()==MMS_OK,"release negative bone pose history");negative_frames::Reset(f.app.get());g_Block=previous;
    }
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();f.app->CurrentFrame()=-10;
        f.firstMorphs[1].value=.875f;
        Check(f.history.SetMode(mode)==MMS_OK,"morph negative pose selects common history mode");
        f.firstMorphs[0].value=4;
        Check(negative_frames::RegisterMorph(f.first.raw(),0,-10,4),"register initial negative morph pose");
        f.firstMorphs[0].value=8;
        Check(negative_frames::RegisterMorph(f.first.raw(),0,-10,8),"overwrite negative morph pose");
        Check(f.history.History(false)==MMS_OK && f.firstMorphs[0].value==4,"negative morph undo restores earlier weight");
        Check(f.history.History(false)==MMS_OK && f.firstMorphs[0].value==0,"last negative morph undo restores native frame-zero weight");
        Check(f.history.History(true)==MMS_OK && f.firstMorphs[0].value==4,"negative morph redo restores first weight");
        Check(f.history.History(true)==MMS_OK && f.firstMorphs[0].value==8 && f.firstMorphs[1].value==.875f &&
            f.app->CurrentFrame()==-10,"negative morph redo preserves unrelated draft and signed playhead");
        Check(f.history.Reset()==MMS_OK,"release negative morph pose history");negative_frames::Reset(f.app.get());g_Block=previous;
    }
    {
        BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();for(auto& bone:f.first.bones)bone.slotIndex=-1;
        f.first.dirty[0]=0;f.app->CurrentFrame()=-20;f.first.bones[0].trans[0]=4;
        Check(negative_frames::RegisterBone(f.first.raw(),0,-20),"register earlier negative bone sample");
        f.app->CurrentFrame()=-10;f.first.bones[0].trans[0]=8;
        Check(negative_frames::RegisterBone(f.first.raw(),0,-10),"register later negative bone sample");
        f.app->CurrentFrame()=-15;
        Check(f.history.History(false)==MMS_OK && f.first.bones[0].trans[0]==3,
            "negative bone undo interpolates earlier key to native zero at the current frame");
        f.app->CurrentFrame()=-10;
        Check(f.history.History(true)==MMS_OK && f.first.bones[0].trans[0]==8,
            "negative bone interpolation redo restores exact later key");
        Check(f.history.History(false)==MMS_OK && f.first.bones[0].trans[0]==2,
            "negative bone undo samples halfway from earlier key to native zero");
        Check(f.history.Reset()==MMS_OK,"release negative bone interpolation history");negative_frames::Reset(f.app.get());
        f.app->CurrentFrame()=-10;f.first.bones[0].trans[0]=4;
        Check(negative_frames::RegisterBone(f.first.raw(),0,-10),"register bone before unregistered draft test");
        f.first.bones[0].trans[0]=8;Check(negative_frames::RegisterBone(f.first.raw(),0,-10),"overwrite bone before draft test");
        f.first.bones[0].trans[0]=13;f.first.dirty[0]=1;
        Check(f.history.History(false)==MMS_OK && f.first.bones[0].trans[0]==13 && f.first.dirty[0]==1,
            "negative bone undo preserves the active unregistered physics draft");
        Check(f.history.Reset()==MMS_OK,"release draft history");negative_frames::Reset(f.app.get());
        f.first.dirty[0]=0;f.first.m->boneKeys[0].frame=1;f.app->CurrentFrame()=-5;f.app->SceneModified()=0;
        Check(!negative_frames::RegisterBone(f.first.raw(),0,-10) && !f.history.UndoCount() &&
            !negative_frames::Count(f.app.get()) && !f.app->SceneModified() && f.first.dirty[0]==0,
            "malformed bone native frame-zero head refuses registration without a partial edit");
        f.first.m->boneKeys[0].frame=0;negative_frames::Reset(f.app.get());g_Block=previous;
    }
    {
        MorphSliderFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();f.app->CurrentFrame()=-20;f.firstMorphs[0].value=4;
        Check(negative_frames::RegisterMorph(f.first.raw(),0,-20,4),"register earlier negative morph sample");
        f.app->CurrentFrame()=-10;f.firstMorphs[0].value=8;
        Check(negative_frames::RegisterMorph(f.first.raw(),0,-10,8),"register later negative morph sample");
        f.app->CurrentFrame()=-15;
        Check(f.history.History(false)==MMS_OK && f.firstMorphs[0].value==3,
            "negative morph undo interpolates earlier key to native zero at the current frame");
        f.app->CurrentFrame()=-10;
        Check(f.history.History(true)==MMS_OK && f.firstMorphs[0].value==8 &&
            f.history.History(false)==MMS_OK && f.firstMorphs[0].value==2,
            "negative morph redo and undo sample exact and halfway values");
        Check(f.history.Reset()==MMS_OK,"release negative morph interpolation history");negative_frames::Reset(f.app.get());
        f.first.m->morphKeys[0].frame=1;f.app->CurrentFrame()=-5;f.app->SceneModified()=0;
        Check(!negative_frames::RegisterMorph(f.first.raw(),0,-10,4) && !f.history.UndoCount() &&
            !negative_frames::Count(f.app.get()) && !f.app->SceneModified(),
            "malformed morph native frame-zero head refuses registration without a partial edit");
        f.first.m->morphKeys[0].frame=0;negative_frames::Reset(f.app.get());g_Block=previous;
    }
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        for(auto type:{mdl::BoneType::InertTip,mdl::BoneType::CoRotate}) {
            DisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();PrepareNegativeDisplayFixture(f);
            f.first.bones[0].type=type;f.first.bones[0].slotIndex=-1;f.first.dirty[0]=0;
            f.app->CurrentFrame()=-10;f.first.bones[0].trans[0]=4;
            Check(bone_scale::SetPose(f.first.raw(),0,Scale(2)),"prepare derived negative bone scale");
            Check(f.history.SetMode(mode)==MMS_OK && negative_frames::RegisterBone(f.first.raw(),0,-10),
                "register negative derived bone using a valid display baseline");
            Check(f.history.History(false)==MMS_OK && f.first.bones[0].trans[0]==0 &&
                f.first.bones[0].rotQuat[0]==0 && f.first.bones[0].rotQuat[3]==1 &&
                bone_scale::Pose(f.first.raw(),0)==Scale(1),
                "last derived negative bone undo restores kinematic frame-zero channels");
            Check(f.history.History(true)==MMS_OK && f.first.bones[0].trans[0]==4 &&
                bone_scale::Pose(f.first.raw(),0)==Scale(2),"derived negative bone redo restores registered channels");
            Check(f.history.Reset()==MMS_OK,"release derived bone history");negative_frames::Reset(f.app.get());g_Block=previous;
        }
        {
            DisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();PrepareNegativeDisplayFixture(f);
            f.first.bones[0].slotIndex=1;f.first.dirty[0]=0;f.app->CurrentFrame()=-10;
            f.first.bones[0].trans[0]=4;
            Check(f.history.SetMode(mode)==MMS_OK && negative_frames::RegisterBone(f.first.raw(),0,-10),
                "register selector-dependent negative bone with valid native selector mapping");
            Check(f.history.History(false)==MMS_OK && f.first.bones[0].trans[0]==0,
                "selector-dependent negative bone undo samples its frame-zero local channels");
            Check(f.history.History(true)==MMS_OK && f.first.bones[0].trans[0]==4,
                "selector-dependent negative bone redo restores its registered local channels");
            Check(f.history.Reset()==MMS_OK,"release selector bone history");negative_frames::Reset(f.app.get());g_Block=previous;
        }
        {
            DisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();PrepareNegativeDisplayFixture(f);
            f.first.bones[0].slotIndex=1;f.app->CurrentFrame()=-10;
            f.first.m->loadComplete=0;f.firstIk[0].enabled=0;
            f.firstOrder[1].linkedModel=1;f.firstOrder[1].linkedBone=0;
            Check(f.history.SetMode(mode)==MMS_OK && negative_frames::RegisterDisplay(f.first.raw(),-10),
                "register negative visibility IK and parent selection");
            f.first.m->loadComplete=1;f.firstIk[0].enabled=1;
            f.firstOrder[1].linkedModel=-1;f.firstOrder[1].linkedBone=0;
            Check(negative_frames::RegisterDisplay(f.first.raw(),-10),"overwrite negative display channels");
            Check(f.history.History(false)==MMS_OK && f.first.m->loadComplete==0 && f.firstIk[0].enabled==0 &&
                f.firstOrder[1].linkedModel==1 && f.firstOrder[1].linkedBone==0,
                "negative display undo restores prior visibility IK and external parent");
            Check(f.history.History(false)==MMS_OK && f.first.m->loadComplete==1 && f.firstIk[0].enabled==1 &&
                f.firstOrder[1].linkedModel==-1 && f.firstOrder[1].linkedBone==0,
                "last negative display undo restores native frame-zero channels");
            Check(f.history.History(true)==MMS_OK && f.first.m->loadComplete==0 && f.firstIk[0].enabled==0 &&
                f.firstOrder[1].linkedModel==1 && f.firstOrder[1].linkedBone==0,
                "negative display redo restores the first registered channels");
            Check(f.history.History(true)==MMS_OK && f.first.m->loadComplete==1 && f.firstIk[0].enabled==1 &&
                f.firstOrder[1].linkedModel==-1 && f.firstOrder[1].linkedBone==0,
                "negative display redo restores overwritten channels");
            Check(f.history.Reset()==MMS_OK,"release negative display history");negative_frames::Reset(f.app.get());g_Block=previous;
        }
    }
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();for(auto& bone:f.first.bones)bone.slotIndex=-1;
        f.first.dirty[0]=0;f.app->CurrentFrame()=-10;f.first.bones[0].trans[0]=4;f.app->SceneModified()=0;
        Check(f.history.SetMode(mode)==MMS_OK,"allocation sweep chooses negative bone history mode");
        unsigned failures=0;bool accepted=false;
        for(long allocation=0;allocation<512;++allocation) {
            failAfter=allocation;
            const bool success=negative_frames::RegisterBone(f.first.raw(),0,-10);
            failAfter=-1;
            if(success){accepted=true;break;}
            ++failures;
            Check(!negative_frames::Count(f.app.get()) && !f.history.UndoCount() && !f.history.RedoCount() &&
                !f.app->SceneModified() && f.first.bones[0].trans[0]==4 && f.first.dirty[0]==0,
                "negative bone preparation allocation failure preserves keys journal pose and dirty flags");
        }
        Check(accepted && failures>3 && f.history.UndoCount()==1,"negative bone allocation sweep reaches eventual publication");
        Check(f.history.Reset()==MMS_OK,"release bone allocation sweep history");f.history.Collect();negative_frames::Reset(f.app.get());
        Check(f.history.Configure(mode,1,1)==MMS_OK,"configure tiny negative bone history budget");
        f.app->SceneModified()=0;
        Check(!negative_frames::RegisterBone(f.first.raw(),0,-10) && !negative_frames::Count(f.app.get()) &&
            !f.history.UndoCount() && !f.app->SceneModified() && f.first.bones[0].trans[0]==4 && f.first.dirty[0]==0,
            "tiny bone history budget rejects registration without a partial edit");
        negative_frames::Reset(f.app.get());g_Block=previous;
    }
    {
        BoneRegistrationFixture f;
        bone_scale::Forget(f.first.raw());
        auto old=bone_scale::PoseStorageLease::Prepare(f.first.raw());
        Check(old && old->IsCurrent(),"new bone pose storage lease identifies its live storage");
        bone_scale::Forget(f.first.raw());
        Check(!old->IsCurrent(),"bone pose lease detects storage removal");
        auto replacement=bone_scale::PoseStorageLease::Prepare(f.first.raw());
        Check(replacement && replacement->IsCurrent() && !old->IsCurrent(),
            "a recreated pose store at the same model address does not revive the stale lease");
        old.reset();
        Check(replacement->IsCurrent(),"destroying the stale lease preserves replacement storage");
        replacement.reset();
    }
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();for(auto& bone:f.first.bones)bone.slotIndex=-1;
        f.first.dirty[0]=0;f.app->CurrentFrame()=-5;f.first.bones[0].trans[0]=4;f.app->SceneModified()=0;
        Check(f.history.SetMode(mode)==MMS_OK,"zero-sample failure sweep chooses history mode");
        unsigned failures=0;bool accepted=false;
        for(long allocation=0;allocation<2048;++allocation) {
            failAfter=allocation;
            const bool success=negative_frames::RegisterBone(f.first.raw(),0,-10);
            failAfter=-1;
            if(success){accepted=true;break;}
            ++failures;
            Check(!negative_frames::Count(f.app.get()) && !f.history.UndoCount() && !f.history.RedoCount() &&
                !f.app->SceneModified() && f.first.bones[0].trans[0]==4 && f.first.dirty[0]==0 && f.app->CurrentFrame()==-5,
                "failed negative bone zero-sample preparation leaves pose keys dirty state and history unchanged");
        }
        Check(accepted && failures>3 && f.history.UndoCount()==1 && negative_frames::Count(f.app.get())==1 &&
            f.first.bones[0].trans[0]==2 && f.app->CurrentFrame()==-5,
            "successful negative bone registration publishes interpolation against independent frame zero");
        Check(f.history.Reset()==MMS_OK,"release zero-sample failure history");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
    }
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        DisplayFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();PrepareNegativeDisplayFixture(f);
        f.first.bones[0].slotIndex=1;f.app->CurrentFrame()=-10;
        f.first.m->loadComplete=0;f.firstIk[0].enabled=0;
        Check(f.history.SetMode(mode)==MMS_OK && negative_frames::RegisterDisplay(f.first.raw(),-10),
            "register existing negative display step");
        auto* pool=f.first.m->displayKeys;f.first.m->displayKeys=nullptr;
        f.app->CurrentFrame()=-5;f.first.m->loadComplete=1;f.firstIk[0].enabled=1;
        Check(negative_frames::RegisterDisplay(f.first.raw(),-20) && f.first.m->loadComplete==0 && f.firstIk[0].enabled==0 &&
            f.history.UndoCount()==2,
            "display registration keeps the later negative step without a native zero pool");
        Check(f.history.History(false)==MMS_OK && f.first.m->loadComplete==0 && f.firstIk[0].enabled==0 &&
            f.history.UndoCount()==1,
            "display undo with a remaining negative step needs no native zero pool");
        const auto keys=negative_frames::Count(f.app.get()), undo=f.history.UndoCount(), redo=f.history.RedoCount();
        const auto dirty=f.app->SceneModified();
        Check(f.history.History(false)!=MMS_OK && negative_frames::Count(f.app.get())==keys &&
            f.history.UndoCount()==undo && f.history.RedoCount()==redo && f.app->SceneModified()==dirty &&
            f.first.m->loadComplete==0 && f.firstIk[0].enabled==0,
            "removing the final display step without a native zero pool fails atomically");
        f.first.m->displayKeys=pool;
        Check(f.history.History(false)==MMS_OK && f.first.m->loadComplete==1 && f.firstIk[0].enabled==1 &&
            negative_frames::Count(f.app.get())==0,
            "restoring the native display pool permits final undo to frame-zero channels");
        Check(f.history.Reset()==MMS_OK,"release conditional display baseline history");negative_frames::Reset(f.app.get());g_Block=previous;
    }
    for(int kind:{2,3,4,5}) {
        NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.app->CameraFov()=45;f.app->CurrentFrame()=-10;
        f.app->GravityMagnitude()=9.8f;f.app->GravityNoise()=0;f.app->GravityNoiseEnabled()=0;
        const auto read=[&](){return kind==2?f.app->CameraPositionX():kind==3?f.app->LightDirection()[0]:kind==4?f.app->ShadowDistance():f.app->GravityMagnitude();};
        const auto write=[&](float value){if(kind==2)f.app->CameraPositionX()=value;else if(kind==3)f.app->LightDirection()[0]=value;
            else if(kind==4)f.app->ShadowDistance()=value;else f.app->GravityMagnitude()=value;};
        write(kind==4?.03f:.4f);
        Check(negative_frames::RegisterGlobal(f.app.get(),static_cast<negative_frames::Global>(kind),-10),"register initial negative global scalar");
        const auto before=read();write(kind==4?.06f:.8f);
        Check(negative_frames::RegisterGlobal(f.app.get(),static_cast<negative_frames::Global>(kind),-10),"overwrite negative global scalar");
        const auto after=read();
        Check(f.history.History(false)==MMS_OK&&read()==before,"direct common undo restores current negative global pose without a UI repaint");
        Check(f.history.History(true)==MMS_OK&&read()==after,"direct common redo restores current negative global pose");
        if(kind==3)Check(LightTextMirror(*f.app),"negative history light publication updates renderer-facing fields");
        Check(f.history.Reset()==MMS_OK,"retire global sampling history");negative_frames::Reset(f.app.get());
    }

    {
        NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        f.app->LightKeys()[0].direction[0]=0;f.app->CurrentFrame()=-20;f.app->LightDirection()[0]=4;
        Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Light,-20),"register interpolation start");
        f.app->CurrentFrame()=-10;f.app->LightDirection()[0]=8;
        Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Light,-10),"register exact interpolation target");
        f.app->CameraPositionX()=999;
        Check(f.history.History(false)==MMS_OK&&f.app->LightDirection()[0]==2&&f.app->CameraPositionX()==999,
            "negative undo interpolates toward independent frame zero without touching unrelated drafts");
        Check(f.history.History(false)==MMS_OK&&f.app->LightDirection()[0]==0&&negative_frames::Count(f.app.get())==0,
            "removing the final negative key restores its native frame-zero value");
        Check(f.history.Reset()==MMS_OK,"release interpolation history");negative_frames::Reset(f.app.get());
        auto* pool=f.app->LightKeys();f.app->LightKeys()=nullptr;
        Check(!negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Light,-10)&&!f.history.UndoCount(),
            "missing independent frame-zero data rejects history preparation");
        f.app->LightKeys()=pool;negative_frames::Reset(f.app.get());
    }
    {
        NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        mdl::AccessoryRecord object{};object.scale=object.opacity=1;object.parentModel=-1;
        f.app->AccessorySlot(0)=&object;f.app->AccessoryKeys(0)=mdl::KeyPoolCreate<mdl::AccessoryKey>(3);
        auto& zero=f.app->AccessoryKeys(0)[0];zero.scale=zero.opacity=1;zero.parentModel=-1;
        f.app->CurrentFrame()=-10;object.position[0]=4;
        Check(negative_frames::RegisterAccessory(f.app.get(),0,-10),"register negative accessory pose");
        object.position[0]=8;Check(negative_frames::RegisterAccessory(f.app.get(),0,-10),"overwrite negative accessory pose");
        Check(f.history.History(false)==MMS_OK&&object.position[0]==4,"common undo restores current accessory pose");
        Check(f.history.History(false)==MMS_OK&&object.position[0]==0,"last accessory key undo restores native baseline");
        Check(f.history.Reset()==MMS_OK,"release accessory pose history");negative_frames::Reset(f.app.get());
        mdl::KeyPoolDestroy(f.app->AccessoryKeys(0));f.app->AccessoryKeys(0)=nullptr;f.app->AccessorySlot(0)=nullptr;
    }

    NegativeBoundCommandHistoryChecks();
    NegativeBoundCandidateStaleChecks();
    NegativeBoundTimelineScopeChecks();
    NegativeBoundFailureSweepChecks();
    NegativeBoundCandidateScopeChecks();
}
std::uint32_t NegativeTestU32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) | (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24);
}
std::vector<std::uint8_t> NegativeTestRecord(MMDApp& app,const char* key) {
    pmm_extension::Document document;negative_frames::Capture(app,document);
    const auto* record=pmm_extension::Find(document,key);
    return record ? record->value : std::vector<std::uint8_t>{};
}
bool NegativeTestGlobal(MMDApp& app,unsigned kind,int frame,double& value) {
    const auto bytes=NegativeTestRecord(app,negative_frames::kPmmKey);
    if(bytes.size()<8 || NegativeTestU32(bytes.data())!=1)return false;
    const auto count=NegativeTestU32(bytes.data()+4);
    if(std::uint64_t(count)*164!=bytes.size()-8)return false;
    for(unsigned i=0;i<count;++i) {
        const auto* row=bytes.data()+8+std::size_t(i)*164;
        if(NegativeTestU32(row+4)==2 && NegativeTestU32(row+8)==kind &&
            static_cast<std::int32_t>(NegativeTestU32(row+12))==frame) {
            std::memcpy(&value,row+36,sizeof(value));return true;
        }
    }
    return false;
}
bool NegativeTestBone(MMDApp& app,unsigned slot,unsigned bone,int frame,double& value) {
    const auto bytes=NegativeTestRecord(app,negative_frames::kPmmKey);
    if(bytes.size()<8 || NegativeTestU32(bytes.data())!=1)return false;
    const auto count=NegativeTestU32(bytes.data()+4);
    if(std::uint64_t(count)*164!=bytes.size()-8)return false;
    for(unsigned i=0;i<count;++i) {
        const auto* row=bytes.data()+8+std::size_t(i)*164;
        if(NegativeTestU32(row)==slot && NegativeTestU32(row+4)==0 && NegativeTestU32(row+8)==bone &&
            static_cast<std::int32_t>(NegativeTestU32(row+12))==frame) {
            std::memcpy(&value,row+36,sizeof(value));return true;
        }
    }
    return false;
}
void NegativeBoundCommandHistoryChecks() {
    std::puts("RUN NegativeBoundCommandHistoryChecks");std::fflush(stdout);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        {
            std::printf("  bound negative delete mode=%d\n",int(mode));std::fflush(stdout);
            NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();f.app->CameraFov()=45;
            f.app->LightKeys()[0].direction[0]=0;
            Check(f.history.SetMode(mode)==MMS_OK,"negative delete chooses common history mode");
            f.app->CurrentFrame()=-20;f.app->CameraPositionX()=9;
            Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-20),"register surviving negative camera");
            f.app->CurrentFrame()=-10;f.app->CameraPositionX()=4;f.app->LightDirection()[0]=6;
            Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10) &&
                negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Light,-10),"register negative camera and light for deletion");
            Check(f.history.Reset()==MMS_OK,"clear setup entries before negative delete");f.history.Collect();
            Check(negative_frames::SelectCell(f.app.get(),-10,0,false) && negative_frames::SelectCell(f.app.get(),-10,1,true),
                "select only camera and light negative keys for deletion");
            CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
            double value=0;
            Check(f.history.UndoCount()==1 && !NegativeTestGlobal(*f.app,2,-10,value) && !NegativeTestGlobal(*f.app,3,-10,value) &&
                NegativeTestGlobal(*f.app,2,-20,value) && value==9 && f.app->CameraPositionX()==4.5f && f.app->LightDirection()[0]==0,
                "one bound delete removes both selected keys and evaluates remaining negative/native tracks");
            Check(negative_frames::SelectCell(f.app.get(),-20,0,false),"select surviving camera after deletion");
            CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
            Check(f.history.UndoCount()==0 && f.history.RedoCount()==1 && NegativeTestGlobal(*f.app,2,-10,value) && value==4 &&
                NegativeTestGlobal(*f.app,3,-10,value) && value==6 && f.app->CameraPositionX()==4 && f.app->LightDirection()[0]==6 &&
                negative_frames::CellSelected(f.app.get(),-20,0) && negative_frames::CellSelected(f.app.get(),-10,0) &&
                negative_frames::CellSelected(f.app.get(),-10,1),
                "one bound undo restores deleted values poses and recorded selection while preserving surviving live selection");
            CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
            Check(f.history.UndoCount()==1 && !NegativeTestGlobal(*f.app,2,-10,value) && !NegativeTestGlobal(*f.app,3,-10,value) &&
                negative_frames::CellSelected(f.app.get(),-20,0) && f.app->CurrentFrame()==-10,
                "one bound redo removes the same keys without replacing surviving live selection or moving the cursor");
            Check(f.history.Reset()==MMS_OK,"release negative delete history");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
        }
        {
            std::printf("  bound negative paste mode=%d\n",int(mode));std::fflush(stdout);
            NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();f.app->CameraFov()=45;
            Check(f.history.SetMode(mode)==MMS_OK,"negative paste chooses common history mode");
            f.app->CurrentFrame()=-20;f.app->CameraPositionX()=4;
            Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-20),"register negative clipboard source");
            f.app->CurrentFrame()=-10;f.app->CameraPositionX()=8;
            Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10),"register negative overwrite destination");
            Check(negative_frames::SelectCell(f.app.get(),-20,0,false) && negative_frames::Command(f.app.get(),420),
                "copy only the source negative key");
            Check(f.history.Reset()==MMS_OK,"clear setup entries before negative paste");f.history.Collect();
            CmdControl400(f.app.get(),f.app->MainWindow(),421,0);double value=0;
            Check(f.history.UndoCount()==1 && NegativeTestGlobal(*f.app,2,-10,value) && value==4 && f.app->CameraPositionX()==4,
                "bound paste overwrites the negative key as one common-history operation");
            CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
            Check(NegativeTestGlobal(*f.app,2,-10,value) && value==8 && f.app->CameraPositionX()==8,
                "bound paste undo restores overwritten payload and current pose");
            CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
            Check(NegativeTestGlobal(*f.app,2,-10,value) && value==4 && f.app->CameraPositionX()==4,
                "bound paste redo restores overwritten payload and current pose");
            f.app->CurrentFrame()=-5;
            const auto prior=f.history.UndoCount();CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
            Check(f.history.UndoCount()==prior+1 && NegativeTestGlobal(*f.app,2,-5,value) && value==4,
                "bound paste inserts a new negative key in one history record");
            Check(negative_frames::SelectCell(f.app.get(),-20,0,false),"change a surviving selection after insertion");
            CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
            Check(!NegativeTestGlobal(*f.app,2,-5,value) && negative_frames::CellSelected(f.app.get(),-20,0) &&
                f.app->CurrentFrame()==-5,"undo removes only the inserted key while preserving selection and playhead");
            CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
            Check(NegativeTestGlobal(*f.app,2,-5,value) && value==4 && negative_frames::CellSelected(f.app.get(),-20,0),
                "redo restores the inserted value while preserving surviving selection");
            Check(f.history.Reset()==MMS_OK,"release negative paste history");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
        }
        {
            std::printf("  bound negative rejection mode=%d\n",int(mode));std::fflush(stdout);
            NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();f.app->CameraFov()=45;
            Check(f.history.SetMode(mode)==MMS_OK,"negative rejection checks choose common history mode");
            f.app->CurrentFrame()=-20;f.app->CameraPositionX()=4;
            Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-20),"register rejection source key");
            f.app->CurrentFrame()=-10;f.app->CameraPositionX()=8;
            Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10),"register rejection second key");
            Check(negative_frames::SelectCell(f.app.get(),-20,0,false) && negative_frames::Command(f.app.get(),420),
                "copy a single negative key before memory failure");
            Check(f.history.Reset()==MMS_OK,"clear rejection setup history");f.history.Collect();
            auto before=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
            auto selected=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
            f.app->SceneModified()=0;const auto pose=f.app->CameraPositionX();
            bool threw=false;failAfter=0;
            try {CmdControl400(f.app.get(),f.app->MainWindow(),421,0);} catch(...) {threw=true;}
            failAfter=-1;
            Check(!threw && !f.history.UndoCount() && !f.history.RedoCount() && !f.app->SceneModified() &&
                f.app->CameraPositionX()==pose && NegativeTestRecord(*f.app,negative_frames::kPmmKey)==before &&
                NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selected,
                "allocation failure rejects bound negative paste without throwing or changing values selection dirty state or history");
            Check(negative_frames::SelectCell(f.app.get(),-20,0,false) && negative_frames::SelectCell(f.app.get(),-10,0,true) &&
                negative_frames::Command(f.app.get(),420),"copy a two-frame negative range across zero when pasted");
            f.app->CurrentFrame()=-5;f.app->SceneModified()=0;
            before=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
            selected=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
            const auto cameraCapacity=mdl::KeyPoolCapacity(f.app->CameraKeys());
            const std::vector<mdl::CameraKey> nativeBefore(f.app->CameraKeys(),f.app->CameraKeys()+cameraCapacity);
            CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
            Check(!f.history.UndoCount() && !f.app->SceneModified() && f.app->CurrentFrame()==-5 &&
                NegativeTestRecord(*f.app,negative_frames::kPmmKey)==before && NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selected &&
                GlobalRegistrationSame(f.app->CameraKeys(),nativeBefore),
                "cross-zero paste is currently rejected as a whole without negative or native writes");
            auto* camera=f.app->CameraKeys();camera[1]=camera[0];camera[0].next=1;camera[1].frame=5;camera[1].previous=0;camera[1].next=0;camera[1].selected=1;
            Check(negative_frames::SelectCell(f.app.get(),-10,0,false),"select a negative key beside a selected positive key");
            const std::vector<mdl::CameraKey> mixedBefore(camera,camera+cameraCapacity);
            before=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
            selected=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);f.app->SceneModified()=0;
            CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
            Check(!f.history.UndoCount() && !f.app->SceneModified() && GlobalRegistrationSame(camera,mixedBefore) &&
                NegativeTestRecord(*f.app,negative_frames::kPmmKey)==before && NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selected,
                "mixed positive and negative delete is currently rejected without partial writes or legacy history");
            camera[1].frame=frame_limits::maximum;
            const std::vector<mdl::CameraKey> maximumBefore(camera,camera+cameraCapacity);
            CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
            Check(!f.history.UndoCount() && !f.app->SceneModified() && GlobalRegistrationSame(camera,maximumBefore) &&
                NegativeTestRecord(*f.app,negative_frames::kPmmKey)==before && NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selected,
                "selected maximum-frame native key still rejects mixed deletion without partial writes");
            Check(f.history.Reset()==MMS_OK,"release negative rejection history");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
        }
        {
            NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();f.app->CurrentFrame()=-10;
            f.app->LightKeys()[0].direction[0]=0;
            Check(f.history.SetMode(mode)==MMS_OK && negative_frames::Count(f.app.get())==0,
                "native light clipboard import starts without a negative clipboard cache");
            mdl::LightClipboardRecord clipboard{};clipboard.frame=0;clipboard.direction[0]=.75f;
            clipboard.color[0]=.25f;clipboard.color[1]=.35f;clipboard.color[2]=.45f;
            struct RestoreClipboard {
                MMDApp& app;mdl::LightClipboardRecord* original;mdl::ClipboardSelectionCounts counts;
                void Restore() noexcept {app.LightClipboard()=original;app.ClipboardCounts()=counts;}
                ~RestoreClipboard(){Restore();}
            } restore{*f.app,f.app->LightClipboard(),f.app->ClipboardCounts()};
            f.app->LightClipboard()=&clipboard;f.app->ClipboardCounts()={};f.app->ClipboardCounts().lights=1;
            CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
            restore.Restore();
            double value=0;
            Check(f.history.UndoCount()==1 && NegativeTestGlobal(*f.app,3,-10,value) && value==.75 &&
                f.app->LightDirection()[0]==.75f && f.app->CurrentFrame()==-10,
                "native light clipboard imports a real negative key and pose in one transaction");
            CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
            Check(!NegativeTestGlobal(*f.app,3,-10,value) && f.app->LightDirection()[0]==0 && f.history.RedoCount()==1,
                "native clipboard import undo removes the negative key and restores the native zero pose");
            CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
            Check(NegativeTestGlobal(*f.app,3,-10,value) && value==.75 && f.app->LightDirection()[0]==.75f,
                "native clipboard import redo restores the negative payload and pose");
            Check(f.history.Reset()==MMS_OK,"release native clipboard import history");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
        }
    }
}
void NegativeBoundCandidateStaleChecks() {
    std::puts("RUN NegativeBoundCandidateStaleChecks");std::fflush(stdout);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();f.app->CameraFov()=45;f.app->CurrentFrame()=-10;
        f.app->CameraPositionX()=4;f.app->LightDirection()[0]=6;
        Check(f.history.SetMode(mode)==MMS_OK && negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,-10) &&
            negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Light,-10),"prepare negative candidate keys");
        Check(f.history.Reset()==MMS_OK,"clear candidate setup history");f.history.Collect();
        Check(negative_frames::SelectCell(f.app.get(),-10,0,false),"select camera for prepared deletion");
        std::shared_ptr<plugins::HistoryChange> change;
        Check(negative_frames::PlanSelectedDeletion(*f.app,change)==MMS_OK && change,"plan selected negative camera deletion");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("negative candidate selection",change,transaction)==MMS_OK && transaction,
            "prepare negative deletion candidate before selection changes");
        const auto keys=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
        f.app->SceneModified()=0;const auto camera=f.app->CameraPositionX(),light=f.app->LightDirection()[0];
        Check(negative_frames::SelectCell(f.app.get(),-10,1,false),"change selection after candidate preparation");
        const auto selection=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
        Check(f.history.CommitNative(std::move(transaction))==MMS_E_STALE &&
            NegativeTestRecord(*f.app,negative_frames::kPmmKey)==keys &&
            NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selection &&
            !f.history.UndoCount() && !f.history.RedoCount() && !f.app->SceneModified() &&
            f.app->CameraPositionX()==camera && f.app->LightDirection()[0]==light && f.app->CurrentFrame()==-10 &&
            negative_frames::CellSelected(f.app.get(),-10,1) && !negative_frames::CellSelected(f.app.get(),-10,0),
            "changed selection rejects the candidate without overwriting external selection keys poses or history");
        Check(negative_frames::SelectCell(f.app.get(),-10,0,false),"restore camera selection for fresh deletion plan");
        change.reset();Check(negative_frames::PlanSelectedDeletion(*f.app,change)==MMS_OK && change &&
            f.history.PrepareNative("fresh negative candidate",change,transaction)==MMS_OK &&
            f.history.CommitNative(std::move(transaction))==MMS_OK,
            "a fresh candidate after selection settles can publish");
        double value=0;
        Check(!NegativeTestGlobal(*f.app,2,-10,value) && NegativeTestGlobal(*f.app,3,-10,value) && value==6 &&
            f.history.UndoCount()==1,"fresh candidate deletes only the newly selected camera key");
        Check(f.history.Reset()==MMS_OK,"release selection candidate history");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
    }
    {
        BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();
        std::array<std::uint32_t,2> rows{1,2};f.first.m->boneKeyIndices=rows.data();
        for(auto& bone:f.first.bones)bone.slotIndex=-1;
        f.first.dirty[0]=0;f.app->CurrentFrame()=-20;f.first.bones[0].trans[0]=4;
        Check(bone_scale::SetPose(f.first.raw(),0,Scale(2)) && negative_frames::RegisterBone(f.first.raw(),0,-20),
            "register earlier nonidentity-scale negative bone key");
        f.app->CurrentFrame()=-10;f.first.bones[0].trans[0]=8;
        Check(bone_scale::SetPose(f.first.raw(),0,Scale(3)) && negative_frames::RegisterBone(f.first.raw(),0,-10),
            "register later nonidentity-scale negative bone key");
        Check(f.history.Reset()==MMS_OK,"clear bone candidate setup history");f.history.Collect();
        Check(negative_frames::SelectCell(f.app.get(),-10,1,false),"select only the later negative bone key");
        std::shared_ptr<plugins::HistoryChange> change;
        Check(negative_frames::PlanSelectedDeletion(*f.app,change)==MMS_OK && change,"plan negative bone deletion");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
        Check(f.history.PrepareNative("negative bone candidate",change,transaction)==MMS_OK && transaction,
            "prepare deletion while earlier scaled negative key survives");
        Check(negative_frames::Count(f.app.get())==2,"both negative bone keys exist before external invalidation");
        // Production Forget also removes negative keys and free curves; this is
        // deliberate external invalidation, not a claim of isolated lease ABA.
        bone_scale::Forget(f.first.raw());
        const auto keysAfterForget=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
        const auto selectionAfterForget=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
        const auto countAfterForget=negative_frames::Count(f.app.get());
        const auto poseAfterForget=f.first.bones[0].trans[0];f.app->SceneModified()=0;
        Check(f.history.CommitNative(std::move(transaction))==MMS_E_STALE &&
            negative_frames::Count(f.app.get())==countAfterForget &&
            NegativeTestRecord(*f.app,negative_frames::kPmmKey)==keysAfterForget &&
            NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selectionAfterForget &&
            f.first.bones[0].trans[0]==poseAfterForget && !f.first.dirty[0] && !f.app->SceneModified() &&
            !f.history.UndoCount() && !f.history.RedoCount(),
            "bone store invalidation rejects prepared deletion without additional key pose or history writes");
        Check(f.history.Reset()==MMS_OK,"release invalidated bone candidate");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
    }
}
void NegativeBoundTimelineScopeChecks() {
    std::puts("RUN NegativeBoundTimelineScopeChecks");std::fflush(stdout);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
        auto* previous=g_Block;g_Block=f.app.get();
        std::array<std::uint32_t,2> firstRows{1,2},secondRows{1,2};
        f.first.m->boneKeyIndices=firstRows.data();f.second.m->boneKeyIndices=secondRows.data();
        for(auto* model:{&f.first,&f.second}) {
            for(auto& bone:model->bones)bone.slotIndex=-1;
            std::strcpy(model->bones[0].name,"scope-bone");
            for(std::size_t i=0;i<mdl::BoneKeyCapacity(*model->m);++i)model->m->boneKeys[i].allocated=0;
            for(std::size_t i=0;i<mdl::MorphKeyCapacity(*model->m);++i)model->m->morphKeys[i].allocated=0;
            for(std::size_t i=0;i<mdl::KeyPoolCapacity(model->m->displayKeys);++i)model->m->displayKeys[i].allocated=0;
        }
        f.app->CameraMode()=0;f.app->CurrentFrame()=-10;
        Check(f.history.SetMode(mode)==MMS_OK,"model timeline scope chooses common history mode");
        f.app->SelectedModelSlot()=0;f.first.bones[0].trans[0]=4;
        Check(negative_frames::RegisterBone(f.first.raw(),0,-10),"register model A negative bone");
        f.app->SelectedModelSlot()=1;f.second.bones[0].trans[0]=8;
        Check(negative_frames::RegisterBone(f.second.raw(),0,-10),"register model B negative bone");
        Check(f.history.Reset()==MMS_OK,"clear model scope setup history");f.history.Collect();
        f.app->SelectedModelSlot()=0;
        Check(negative_frames::CellSelected(f.app.get(),-10,1),"model A registration retains its selected negative key");
        f.app->SelectedModelSlot()=1;
        Check(negative_frames::CellSelected(f.app.get(),-10,1),"model B registration retains its selected negative key");
        double value=0;
        CmdControl400(f.app.get(),f.app->MainWindow(),423,0);
        Check(f.history.UndoCount()==1 && NegativeTestBone(*f.app,0,0,-10,value) && value==4 &&
            !NegativeTestBone(*f.app,1,0,-10,value) && f.first.bones[0].trans[0]==4 && f.second.bones[0].trans[0]==0,
            "model B deletion preserves model A key and pose while removing only model B");
        CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
        Check(NegativeTestBone(*f.app,1,0,-10,value) && value==8 && f.second.bones[0].trans[0]==8,
            "model B undo restores its own negative key and pose");
        f.app->SelectedModelSlot()=0;
        Check(negative_frames::CellSelected(f.app.get(),-10,1) && f.first.bones[0].trans[0]==4,
            "model B undo keeps model A's previously selected key and pose");
        f.app->CameraMode()=1;
        std::shared_ptr<plugins::HistoryChange> change;
        Check(negative_frames::PlanSelectedDeletion(*f.app,change)==MMS_OK && !change &&
            NegativeTestBone(*f.app,0,0,-10,value) && value==4,
            "camera timeline deletion plan ignores retained selected model keys");
        f.app->CameraMode()=0;f.app->SelectedModelSlot()=1;
        Check(negative_frames::CellSelected(f.app.get(),-10,1) && negative_frames::Command(f.app.get(),420),
            "copy selected keys from model B while model A remains selected");
        f.app->CurrentFrame()=-5;
        CmdControl400(f.app.get(),f.app->MainWindow(),421,0);
        Check(NegativeTestBone(*f.app,1,0,-5,value) && value==8 &&
            NegativeTestBone(*f.app,0,0,-10,value) && value==4 && !NegativeTestBone(*f.app,0,0,-5,value) &&
            f.first.bones[0].trans[0]==4,
            "model B paste uses only model B clipboard data and leaves model A unchanged");
        f.app->SelectedModelSlot()=0;
        Check(negative_frames::CellSelected(f.app.get(),-10,1),"model B paste preserves model A's inactive timeline selection");
        f.app->SelectedModelSlot()=1;
        Check(f.history.Reset()==MMS_OK,"release model scope history");f.history.Collect();negative_frames::Reset(f.app.get());
        f.first.m->boneKeyIndices=nullptr;f.second.m->boneKeyIndices=nullptr;g_Block=previous;
    }
}
void NegativeBoundFailureSweepChecks() {
    std::puts("RUN NegativeBoundFailureSweepChecks");std::fflush(stdout);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        for(bool paste:{false,true}) {
            std::printf("  negative allocation sweep operation=%s mode=%d\n",paste?"paste":"delete",int(mode));std::fflush(stdout);
            NegativeGlobalFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();f.app->CameraFov()=45;
            f.app->CameraKeys()[0].eye[0]=0;f.app->LightKeys()[0].direction[0]=0;
            Check(f.history.SetMode(mode)==MMS_OK,"negative command failure sweep chooses history mode");
            for(const auto entry:{std::pair<int,float>{-20,4},std::pair<int,float>{-10,8}}) {
                f.app->CurrentFrame()=entry.first;f.app->CameraPositionX()=entry.second;
                f.app->LightDirection()[0]=entry.second/4;
                Check(negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Camera,entry.first) &&
                    negative_frames::RegisterGlobal(f.app.get(),negative_frames::Global::Light,entry.first),
                    "seed multi-frame camera and light negative keys");
            }
            Check(negative_frames::SelectCell(f.app.get(),-20,0,false) && negative_frames::SelectCell(f.app.get(),-10,0,true) &&
                negative_frames::SelectCell(f.app.get(),-20,1,true) && negative_frames::SelectCell(f.app.get(),-10,1,true),
                "select both negative frames in both global tracks");
            if(paste)Check(negative_frames::Command(f.app.get(),420),"copy both selected negative global tracks");
            Check(f.history.Reset()==MMS_OK,"clear allocation sweep setup history");f.history.Collect();
            f.app->CurrentFrame()=-40;f.app->SceneModified()=0;
            const auto beforeKeys=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
            const auto beforeSelection=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
            const auto beforeCamera=f.app->CameraPositionX(),beforeLight=f.app->LightDirection()[0];
            const std::vector<mdl::CameraKey> cameras(f.app->CameraKeys(),f.app->CameraKeys()+mdl::KeyPoolCapacity(f.app->CameraKeys()));
            const std::vector<mdl::LightKey> lights(f.app->LightKeys(),f.app->LightKeys()+mdl::KeyPoolCapacity(f.app->LightKeys()));
            Check(!GetCapture(),"negative command failure sweep begins without mouse capture");
            unsigned failures=0;bool accepted=false;
            for(long allocation=0;allocation<2048;++allocation) {
                bool threw=false;failAfter=allocation;
                try {CmdControl400(f.app.get(),f.app->MainWindow(),paste?421:423,0);} catch(...) {threw=true;}
                failAfter=-1;
                Check(!threw,"negative command handles allocation failures without escaping");
                if(f.history.UndoCount()==1){accepted=true;break;}
                ++failures;
                Check(!f.history.UndoCount() && !f.history.RedoCount() && !f.app->SceneModified() &&
                    f.app->CurrentFrame()==-40 && f.app->CameraPositionX()==beforeCamera && f.app->LightDirection()[0]==beforeLight &&
                    NegativeTestRecord(*f.app,negative_frames::kPmmKey)==beforeKeys &&
                    NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==beforeSelection &&
                    GlobalRegistrationSame(f.app->CameraKeys(),cameras) && GlobalRegistrationSame(f.app->LightKeys(),lights) && !GetCapture(),
                    "failed negative command preserves data selection pose native buffers dirty state and mouse capture");
            }
            Check(accepted && failures>3 && !GetCapture(),"negative command eventually commits one complete record after allocation failures");
            double value=0;
            if(paste) {
                Check(NegativeTestGlobal(*f.app,2,-40,value) && value==4 && NegativeTestGlobal(*f.app,2,-30,value) && value==8 &&
                    NegativeTestGlobal(*f.app,3,-40,value) && value==1 && NegativeTestGlobal(*f.app,3,-30,value) && value==2,
                    "successful negative paste publishes both destination frames and both global payloads");
                CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
                Check(!NegativeTestGlobal(*f.app,2,-40,value) && !NegativeTestGlobal(*f.app,2,-30,value) &&
                    !NegativeTestGlobal(*f.app,3,-40,value) && !NegativeTestGlobal(*f.app,3,-30,value) &&
                    NegativeTestGlobal(*f.app,2,-20,value) && value==4 && f.history.RedoCount()==1,
                    "one undo removes the entire multi-track paste and preserves source keys");
                CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
                Check(NegativeTestGlobal(*f.app,2,-40,value) && value==4 && NegativeTestGlobal(*f.app,2,-30,value) && value==8 &&
                    NegativeTestGlobal(*f.app,3,-40,value) && value==1 && NegativeTestGlobal(*f.app,3,-30,value) && value==2,
                    "one redo restores every pasted negative payload");
            } else {
                Check(!NegativeTestGlobal(*f.app,2,-20,value) && !NegativeTestGlobal(*f.app,2,-10,value) &&
                    !NegativeTestGlobal(*f.app,3,-20,value) && !NegativeTestGlobal(*f.app,3,-10,value) &&
                    f.app->CameraPositionX()==0 && f.app->LightDirection()[0]==0,
                    "successful negative delete removes all selected frames and restores frame-zero poses");
                CmdControl400(f.app.get(),f.app->MainWindow(),400,0);
                Check(NegativeTestGlobal(*f.app,2,-20,value) && value==4 && NegativeTestGlobal(*f.app,2,-10,value) && value==8 &&
                    NegativeTestGlobal(*f.app,3,-20,value) && value==1 && NegativeTestGlobal(*f.app,3,-10,value) && value==2 &&
                    f.history.RedoCount()==1,"one undo restores every deleted negative payload");
                CmdControl400(f.app.get(),f.app->MainWindow(),401,0);
                Check(!NegativeTestGlobal(*f.app,2,-20,value) && !NegativeTestGlobal(*f.app,2,-10,value) &&
                    !NegativeTestGlobal(*f.app,3,-20,value) && !NegativeTestGlobal(*f.app,3,-10,value),
                    "one redo deletes the complete multi-track selection");
            }
            Check(f.app->CurrentFrame()==-40 && !GetCapture(),"negative command traversal preserves playhead and releases mouse capture");
            Check(f.history.Reset()==MMS_OK,"release negative failure sweep history");f.history.Collect();negative_frames::Reset(f.app.get());g_Block=previous;
        }
    }
}
void NegativeBoundCandidateScopeChecks() {
    std::puts("RUN NegativeBoundCandidateScopeChecks");std::fflush(stdout);
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}) {
        for(bool switchCamera:{false,true})for(bool switchBeforePrepare:{false,true}) {
            BoneRegistrationFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
            auto* previous=g_Block;g_Block=f.app.get();
            std::array<std::uint32_t,2> firstRows{1,2},secondRows{1,2};
            f.first.m->boneKeyIndices=firstRows.data();f.second.m->boneKeyIndices=secondRows.data();
            for(auto* model:{&f.first,&f.second}) {
                for(auto& bone:model->bones)bone.slotIndex=-1;
                for(std::size_t i=0;i<mdl::BoneKeyCapacity(*model->m);++i)model->m->boneKeys[i].allocated=0;
                for(std::size_t i=0;i<mdl::MorphKeyCapacity(*model->m);++i)model->m->morphKeys[i].allocated=0;
                for(std::size_t i=0;i<mdl::KeyPoolCapacity(model->m->displayKeys);++i)model->m->displayKeys[i].allocated=0;
            }
            f.app->CameraMode()=0;f.app->CurrentFrame()=-10;
            Check(f.history.SetMode(mode)==MMS_OK,"scope candidate chooses common history mode");
            f.app->SelectedModelSlot()=0;f.first.bones[0].trans[0]=4;
            Check(negative_frames::RegisterBone(f.first.raw(),0,-10),"register model A scope candidate key");
            f.app->SelectedModelSlot()=1;f.second.bones[0].trans[0]=8;
            Check(negative_frames::RegisterBone(f.second.raw(),0,-10),"register model B scope candidate key");
            Check(f.history.Reset()==MMS_OK,"clear scope candidate setup history");f.history.Collect();
            f.app->SelectedModelSlot()=0;f.app->SceneModified()=0;
            const auto keys=NegativeTestRecord(*f.app,negative_frames::kPmmKey);
            const auto selection=NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey);
            const auto a=f.first.bones[0].trans[0],b=f.second.bones[0].trans[0];
            std::shared_ptr<plugins::HistoryChange> change;
            Check(negative_frames::PlanSelectedDeletion(*f.app,change)==MMS_OK && change,
                "plan selected deletion on model A");
            std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
            const auto switchScope=[&] {
                if(switchCamera)f.app->CameraMode()=1;else f.app->SelectedModelSlot()=1;
            };
            if(switchBeforePrepare) {
                switchScope();
                Check(f.history.PrepareNative("stale scope plan",change,transaction)==MMS_E_STALE && !transaction,
                    "switching timeline after planning rejects initial preparation");
            } else {
                Check(f.history.PrepareNative("prepared scope plan",change,transaction)==MMS_OK && transaction,
                    "prepare deletion while original timeline is active");
                switchScope();
                Check(f.history.CommitNative(std::move(transaction))==MMS_E_STALE,
                    "switching timeline after preparation rejects initial publication");
            }
            Check(NegativeTestRecord(*f.app,negative_frames::kPmmKey)==keys &&
                NegativeTestRecord(*f.app,negative_frames::kPmmSelectionKey)==selection &&
                f.first.bones[0].trans[0]==a && f.second.bones[0].trans[0]==b &&
                !f.app->SceneModified() && !f.history.UndoCount() && !f.history.RedoCount() && f.app->CurrentFrame()==-10,
                "stale timeline candidate leaves both models keys selection poses and history unchanged");
            f.app->CameraMode()=0;f.app->SelectedModelSlot()=0;change.reset();
            Check(negative_frames::PlanSelectedDeletion(*f.app,change)==MMS_OK && change &&
                f.history.PrepareNative("fresh model A deletion",change,transaction)==MMS_OK &&
                f.history.CommitNative(std::move(transaction))==MMS_OK,
                "fresh candidate publishes while model A is active");
            double value=0;
            Check(!NegativeTestBone(*f.app,0,0,-10,value) && NegativeTestBone(*f.app,1,0,-10,value) && value==8,
                "fresh candidate deletes only model A's negative key");
            f.app->SelectedModelSlot()=1;
            Check(f.history.History(false)==MMS_OK && NegativeTestBone(*f.app,0,0,-10,value) && value==4 &&
                NegativeTestBone(*f.app,1,0,-10,value) && value==8 && f.first.bones[0].trans[0]==4 &&
                f.second.bones[0].trans[0]==8,
                "undo after switching to model B restores model A without an initial-scope restriction");
            Check(f.history.Reset()==MMS_OK,"release scope candidate history");f.history.Collect();negative_frames::Reset(f.app.get());
            f.first.m->boneKeyIndices=nullptr;f.second.m->boneKeyIndices=nullptr;g_Block=previous;
        }
    }
}
}
