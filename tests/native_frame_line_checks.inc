#include "../src/plugins/native_frame_lines.hpp"
namespace mikudancestudio {
int ShiftGlobalKeysForFrameLine(MMDApp*,bool,bool);
int ShiftMorphKeysForFrameLine(MMDApp*,bool);
int InsertBoneCameraFrameLine(MMDApp*); int DeleteBoneCameraFrameLine(MMDApp*);
int InsertFacialLightFrameLine(MMDApp*); int DeleteFacialLightFrameLine(MMDApp*);
}
namespace {
MmsResult FrameLinePlan(GlobalRegistrationFixture& f, unsigned kind, int frame, bool insert, std::shared_ptr<plugins::HistoryChange>& out) {
    return kind ? plugins::PlanLightFrameLine(*f.app,frame,insert,out) : plugins::PlanCameraFrameLine(*f.app,frame,insert,out);
}
void FrameLineCommit(plugins::SharedHistory& history,const std::shared_ptr<plugins::HistoryChange>& change) {
    std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
    Check(history.PrepareNative("frame line",change,token)==MMS_OK,"frame line reserves complete explicit transaction");
    failAfter=0;const auto status=history.CommitNative(std::move(token));failAfter=-1;
    Check(status==MMS_OK,"frame line publishes without allocations");
}
void NativeFrameLineChecks() {
    using Mode=plugins::SharedHistory::RecordingMode;
    std::puts("  frame lines: global legacy parity and both modes");std::fflush(stdout);
    for(unsigned kind=0;kind<2;++kind)for(bool insert:{true,false})for(bool merge:{false,true})for(int frame:{0,12,17,31,40})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}) {
        GlobalRegistrationFixture f,oracle;f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        f.app->CameraMode()=oracle.app->CameraMode()=1;
        if(merge){f.app->CameraKeys()[1].frame=oracle.app->CameraKeys()[1].frame=1;f.app->LightKeys()[1].frame=oracle.app->LightKeys()[1].frame=1;}
        const auto cameraX=f.app->CameraPositionX(),lightX=f.app->LightDirection()[0];
        Check(f.history.SetMode(mode)==MMS_OK,"frame line history mode accepted");
        const auto before=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
        Check(FrameLinePlan(f,kind,frame,insert,change)==MMS_OK&&before==f.Digest(),"frame line planning never edits live scene");
        if(!change) {Check(frame==40,"frame line above last key is a true no-op");continue;}
        const auto camera=GlobalRegistrationKeys(f.app->CameraKeys());const auto light=GlobalRegistrationKeys(f.app->LightKeys());
        FrameLineCommit(f.history,change);
        Check(f.app->CameraPositionX()==cameraX&&f.app->LightDirection()[0]==lightX,"frame line commit retains unregistered global author pose");
        ShiftGlobalKeysForFrameLine(oracle.app.get(),kind==0,insert);
        Check(GlobalRegistrationSame(f.app->CameraKeys(),GlobalRegistrationKeys(oracle.app->CameraKeys()))&&
            GlobalRegistrationSame(f.app->LightKeys(),GlobalRegistrationKeys(oracle.app->LightKeys())),"global frame line keys match legacy slot-order surgery");
        Check(f.history.UndoCount()==1,"both history modes record explicit frame line exactly once");
        Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),camera)&&GlobalRegistrationSame(f.app->LightKeys(),light),"global frame line undo restores all original key bytes");
        Check(f.history.History(true)==MMS_OK&&f.app->CameraPositionX()==cameraX&&f.app->LightDirection()[0]==lightX,"global frame line undo redo preserve unregistered pose");
    }
    std::puts("  frame lines: allocation budget and identity growth");std::fflush(stdout);
    for(unsigned kind=0;kind<2;++kind) {
        GlobalRegistrationFixture f;const auto before=f.Digest();std::shared_ptr<plugins::HistoryChange> change;
        bool planned=false;for(long limit=0;limit<128;++limit){failAfter=limit;const auto status=FrameLinePlan(f,kind,12,false,change);failAfter=-1;
            Check(f.Digest()==before,"frame line allocation failure leaves keys and poses intact");if(status==MMS_OK){planned=true;break;}Check(status==MMS_E_MEMORY&&!change,"frame line planning allocation refusal is explicit");}
        Check(planned&&change,"frame line planning eventually succeeds");
        bool prepared=false;for(long limit=0;limit<256;++limit){std::unique_ptr<plugins::PreparedHistoryChange> candidate;failAfter=limit;
            const auto status=change->Prepare(true,candidate);failAfter=-1;Check(f.Digest()==before,"frame line candidate preparation/cancellation never writes live");
            if(status==MMS_OK){Check(candidate&&candidate->IsCurrent(),"prepared frame line candidate is current");prepared=true;break;}}
        Check(prepared,"frame line preparation eventually succeeds");
        Check(f.history.Configure(Mode::ExplicitOnly,32,1)==MMS_OK,"tiny explicit history budget configured");
        std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("frame line",change,token)!=MMS_OK&&f.Digest()==before&&!f.history.UndoCount(),"over-budget explicit frame line never mutates scene");
        Check(f.history.Configure(Mode::AllOperations,32,64*1024*1024)==MMS_OK,"restore frame line budget");
        const auto identity=kind?mdl::KeyPoolIdentity(f.app->LightKeys()):mdl::KeyPoolIdentity(f.app->CameraKeys());
        if(kind)Check(mdl::KeyPoolEnsure(f.app->LightKeys(),100),"grow light pool between planning and prepare");
        else Check(mdl::KeyPoolEnsure(f.app->CameraKeys(),100),"grow camera pool between planning and prepare");
        Check(identity==(kind?mdl::KeyPoolIdentity(f.app->LightKeys()):mdl::KeyPoolIdentity(f.app->CameraKeys())),"frame line pool growth preserves identity");
        FrameLineCommit(f.history,change);Check(f.history.History(false)==MMS_OK,"frame line remains reversible after same-identity growth");
    }
    std::puts("  frame lines: bone scales interpolation and frame zero");std::fflush(stdout);
    for(bool insert:{true,false})for(bool automatic:{false,true})for(bool merge:{false,true})for(int frame:{0,10,20,30,70}) {
        BoneRegistrationFixture f,oracle;SeedRegistrationChain(f.first);SeedRegistrationChain(oracle.first);
        if(merge) { f.first.m->boneKeys[2].frame=1;oracle.first.m->boneKeys[2].frame=1; }
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;
        const auto before=RegistrationKeys(f.first);const auto ring=PoseRingDigest(f.first);const auto pose=f.first.bones[0].trans[0];
        const auto scaleBefore=bone_scale::AuthoringState::Capture(f.first.raw());
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanBoneFrameLine(*f.app,0,frame,insert,automatic,change)==MMS_OK,"bone frame line private plan succeeds");
        Check(SameRegistrationKeys(f.first,before)&&PoseRingDigest(f.first)==ring,"bone frame line plan preserves native keys and legacy rings");
        if(!change){Check(frame==70,"bone frame line above last key is no-op");continue;}
        FrameLineCommit(f.history,change);
        if(insert)ShiftBoneKeysForFrameLineInsert(oracle.app.get(),unsigned(frame));
        else ShiftBoneKeysForFrameLineDelete(oracle.app.get(),unsigned(frame),automatic);
        Check(SameRegistrationKeys(f.first,RegistrationKeys(oracle.first)),"bone frame line matches legacy slot-order auto interpolation and frame-zero merge");
        Check(bone_scale::AuthoringState::Capture(f.first.raw()).Equals(bone_scale::AuthoringState::Capture(oracle.first.raw())),"bone frame line preserves key scale and scale interpolation parity");
        Check(f.first.m->maxFrame==oracle.first.m->maxFrame&&f.first.bones[0].trans[0]==pose&&PoseRingDigest(f.first)==ring,"bone frame line preserves dirty author pose and legacy rings");
        Check(f.history.History(false)==MMS_OK&&SameRegistrationKeys(f.first,before)&&bone_scale::AuthoringState::Capture(f.first.raw()).Equals(scaleBefore),"bone frame line undo restores original keys and author scale without files");
        Check(f.history.History(true)==MMS_OK&&f.first.bones[0].trans[0]==pose,"bone frame line redo preserves dirty author pose");
    }
    std::puts("  frame lines: morph legacy parity");std::fflush(stdout);
    for(bool insert:{true,false})for(int frame:{0,10,20,25,70}) {
        MorphKeyFixture f,oracle;MorphKeySeed(f.first);MorphKeySeed(oracle.first);
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=frame;f.app->CameraMode()=oracle.app->CameraMode()=0;
        const auto before=MorphKeysCopy(f.first);std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanMorphFrameLine(*f.app,0,frame,insert,change)==MMS_OK,"morph frame line private plan succeeds");
        Check(MorphKeysMatch(f.first,before),"morph frame line plan preserves live pool");
        if(!change){Check(frame==70,"morph frame line beyond last key is no-op");continue;}
        FrameLineCommit(f.history,change);
        ShiftMorphKeysForFrameLine(oracle.app.get(),insert);
        Check(MorphKeysMatch(f.first,MorphKeysCopy(oracle.first)),"morph frame line matches legacy mutation");
        Check(f.first.m->maxFrame==oracle.first.m->maxFrame,"morph frame line preserves legacy maximum behavior");
        Check(f.history.History(false)==MMS_OK&&MorphKeysMatch(f.first,before),"morph frame line undo restores complete original keys");
        Check(f.history.History(true)==MMS_OK,"morph frame line redo succeeds");
    }
}
}
