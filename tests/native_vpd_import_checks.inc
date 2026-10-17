#include "../src/plugins/native_vpd_import.hpp"
#include "mikudancestudio/vpd_scale_io.hpp"
#include <filesystem>
#include <share.h>
#include <io.h>
#include <fcntl.h>
namespace {
struct VpdInput {
    std::wstring path;
    VpdInput() {static unsigned serial=0;path=(std::filesystem::temp_directory_path()/
        (L"mms-vpd-\u6d4b\u8bd5-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial)+L".vpd")).native();}
    ~VpdInput(){DeleteFileW(path.c_str());}
    void Write(const char* records,int count=1){FILE* file=nullptr;Check(!_wfopen_s(&file,path.c_str(),L"wt")&&file,"open VPD fixture");
        std::fprintf(file,"Vocaloid Pose Data file\n\nmodel.osm;\n%d;\n\n%s",count,records);Check(!std::fclose(file),"close VPD fixture");}
    void Tail(){vpd_scale_io::Entry e;e.bone=0;std::memcpy(e.name.data(),"bone",5);e.scale=Scale(2);
        Check(vpd_scale_io::AppendTail(path,{e})==vpd_scale_io::TailSave::Written,"append valid native scale tail");}
    bool Exclusive(){int fd=-1;const auto result=_wsopen_s(&fd,path.c_str(),_O_RDONLY|_O_BINARY,_SH_DENYRW,0);if(fd>=0)_close(fd);return result==0;}
};
const char* vpdRecord="Bone0{bone\n  3,4,5;\n  0,0,0.6,0.8;\n}\n\n";
void VpdNames(PoseNumericFixture& f){std::memcpy(f.first.bones[0].name,"bone",5);std::memcpy(f.first.bones[1].name,"other",6);}
void VpdHistoryChecks(){
    PoseNumericFixture f;VpdNames(f);plugins::NativePoseTestBinding binding(*f.app,f.history);VpdInput input;input.Write(vpdRecord);input.Tail();
    const auto before=f.first.Digest(),ring=PoseRingDigest(f.first);bool changed=false;
    plugins::VpdImportSession session;Check(session.Begin(*f.app,f.history)==MMS_OK,"capture VPD import session");
    Check(f.history.Enter()==MMS_OK,"file picker preparation does not reserve journal");f.history.Leave();
    Check(session.Apply(input.path.c_str(),changed)==MMS_OK&&changed&&f.history.UndoCount()==1,"pose and scale import creates one common step");
    Check(f.first.bones[0].trans[0]==3&&f.first.bones[0].rotQuat[2]==.6f&&bone_scale::Pose(f.first.raw(),0)==Scale(2)&&
        f.first.dirty[0]==1&&!f.first.dirty[1]&&PoseRingDigest(f.first)==ring&&f.SelectionUnchanged(),"import retains untouched data selection and old ring");
    Check(session.Apply(input.path.c_str(),changed)==MMS_E_NOT_READY,"import session is consumed once");
    const auto afterBones=f.first.bones;const auto afterDirty=f.first.dirty;f.Commit(panel::kReadoutPosYEdit,"9");Check(f.history.History(false)==MMS_OK,"seed redo after VPD import");
    f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;const auto selected=f.first.selected;
    Check(plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed)==MMS_OK&&!changed&&f.history.RedoCount()==1&&
        !f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.first.selected==selected,"identical import preserves redo dirty and selection");
    Check(DeleteFileW(input.path.c_str())!=FALSE,"source removed before undo and redo");f.app->SelectedModelSlot()=1;f.app->CurrentFrame()=31;
    Check(f.history.History(false)==MMS_OK&&f.first.Digest()==before&&bone_scale::Identity(bone_scale::Pose(f.first.raw(),0)),"undo restores exact pre-import pose without source");
    // Scale readout installs derived matInit transforms; history owns author values.
    Check(f.history.History(true)==MMS_OK&&
        !std::memcmp(f.first.bones[0].trans,afterBones[0].trans,12)&&
        !std::memcmp(f.first.bones[0].rotQuat,afterBones[0].rotQuat,16)&&f.first.dirty==afterDirty&&
        PoseRingDigest(f.first)==ring&&bone_scale::Pose(f.first.raw(),0)==Scale(2)&&f.app->CurrentFrame()==31,
        "redo restores owned pose and scale independently of file frame or model selection");
}
void VpdInvalidChecks(){
    const char* invalid[]={"Bone0{bone\n1,2,3;\n0,0,0,1;\n}\n\nBone1{other\n1,2;\n0,0,0,1;\n}\n\n",
        "Bone0{bone\nnan,2,3;\n0,0,0,1;\n}\n\n","Bone0{bone\n1,2,3;\n0,0,inf,1;\n}\n\n",
        "Bone0{bone\n1,2,3\n0,0,0,1;\n}\n\n","Bone0{bone\n1,2,3;\n0,0,0,1;\n"};
    PoseNumericFixture f;VpdNames(f);plugins::NativePoseTestBinding binding(*f.app,f.history);f.Commit(panel::kReadoutPosYEdit,"3");Check(f.history.History(false)==MMS_OK,"seed invalid VPD redo");
    f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;const auto before=f.first.Digest();const auto selection=f.first.selected;VpdInput input;
    for(unsigned i=0;i<5;++i){input.Write(invalid[i],i==0?2:1);bool changed=true;
        Check(plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed)==MMS_E_ARGUMENT&&!changed&&f.first.Digest()==before&&f.first.selected==selection&&
            f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&input.Exclusive(),"malformed or nonfinite VPD refuses atomically and closes handles");}
    input.Write(vpdRecord,33);bool changed=true;Check(plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed)==MMS_E_ARGUMENT&&!changed&&f.first.Digest()==before,"record count is bounded by native key capacity");
    input.Write("Bone0{missing\n1,2,3;\n0,0,0,1;\n}\n\n");Check(plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed)==MMS_OK&&!changed&&f.history.RedoCount()==1,"unmatched file is a clean no-op");
}
void VpdStaleChecks(){
    for(int change=0;change<10;++change){PoseNumericFixture f;VpdNames(f);plugins::NativePoseTestBinding binding(*f.app,f.history);VpdInput input;input.Write(vpdRecord);
        plugins::VpdImportSession session;Check(session.Begin(*f.app,f.history)==MMS_OK,"capture VPD stale boundary");
        if(change==0)f.app->SelectedModelSlot()=1;if(change==1)f.app->CurrentFrame()=9;
        if(change==2)f.first.bones[0].trans[0]=8;if(change==3)f.first.bones[0].axis[0]=1;
        if(change==4)f.first.bones[0].name[0]='x';if(change==5)f.first.m->physicsMode=2;
        if(change==6)bone_scale::SetPose(f.first.raw(),0,Scale(2));if(change==7)f.history.Reset();
        if(change==8){plugins::ForgetSceneObject(f.first.raw());plugins::SceneObjectIdentity(f.first.raw());}
        if(change==9)f.first.m->boneTable=nullptr;
        const auto before=f.first.Digest();bool changed=true;Check(session.Apply(input.path.c_str(),changed)==MMS_E_STALE&&!changed&&before==f.first.Digest()&&!f.history.UndoCount(),"modal change rejects stale VPD without author writes");
        f.first.m->boneTable=f.first.bones.data();
    }
}
void VpdFailureChecks(){
    for(bool tail:{false,true}){PoseNumericFixture f;VpdNames(f);plugins::NativePoseTestBinding binding(*f.app,f.history);VpdInput input;input.Write(vpdRecord);if(tail)input.Tail();
        f.Commit(panel::kReadoutPosYEdit,"3");Check(f.history.History(false)==MMS_OK,"seed VPD failure redo");const auto before=f.first.Digest();const auto selection=f.first.selected;
        f.app->SceneModified()=f.app->RecordedFrameCountByte1()=f.app->PhysicsResetPending()=0;unsigned failures=0;bool success=false;PoseAxisMessageProbe messages(f);
        for(long limit=0;limit<256;++limit){bool changed=false;failAfter=limit;const auto result=plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed);failAfter=-1;
            if(result==MMS_OK){Check(changed,"successful VPD allocation sweep publishes edit");success=true;break;}++failures;
            Check(!changed&&before==f.first.Digest()&&bone_scale::Identity(bone_scale::Pose(f.first.raw(),0))&&selection==f.first.selected&&f.history.RedoCount()==1&&
                !f.history.UndoCount()&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&!f.app->PhysicsResetPending()&&input.Exclusive(),"VPD allocation refusal restores all data redo and file handles");
            Check(f.history.Enter()==MMS_OK,"failed VPD releases journal gate");f.history.Leave();}
        Check(success&&failures>8&&!messages.observedBusy,"VPD allocation sweep reaches publish and notifies outside reservation");
        Check(f.history.History(false)==MMS_OK&&before==f.first.Digest(),"successful VPD after allocation failures remains exactly undoable");
    }
}
void VpdTailAndGateChecks(){
    for(int tail=0;tail<3;++tail){PoseNumericFixture f;VpdNames(f);plugins::NativePoseTestBinding binding(*f.app,f.history);VpdInput input;input.Write(vpdRecord);if(tail)input.Tail();
        if(tail==2){FILE* file=nullptr;Check(!_wfopen_s(&file,input.path.c_str(),L"r+b")&&file,"open optional tail corruption fixture");
            Check(!std::fseek(file,-17,SEEK_END)&&std::fputc(0,file)!=EOF&&!std::fclose(file),"corrupt payload CRC while preserving standard pose");}
        bone_scale::SetPose(f.first.raw(),0,Scale(3));bool changed=false;
        Check(plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed)==MMS_OK&&changed&&f.first.bones[0].trans[0]==3&&
            bone_scale::Pose(f.first.raw(),0)==(tail==1?Scale(2):bone_scale::Matrix{}),"valid scale tail applies; absent or malformed tail clears touched residual scale");
        Check(f.history.History(false)==MMS_OK&&bone_scale::Pose(f.first.raw(),0)==Scale(3),"all optional tail outcomes restore original nonidentity scale on undo");
    }
    for(int gate=0;gate<6;++gate){PoseNumericFixture f;VpdNames(f);plugins::NativePoseTestBinding binding(*f.app,f.history);VpdInput input;input.Write(vpdRecord);
        if(gate==0)f.app->PlaybackActive()=1;if(gate==1)f.app->FrameStepPlayback()=1;if(gate==2)f.app->state.recordPlaybackStartPending=true;
        if(gate==3)f.app->CameraMode()=1;if(gate==4)f.app->state.accessoryEditDialogOpen=true;if(gate==5)Check(f.history.Enter()==MMS_OK,"reserve competing VPD transaction");
        const auto before=f.first.Digest();bool changed=true;Check(plugins::ImportBoundVpd(*f.app,input.path.c_str(),changed)!=MMS_OK&&!changed&&before==f.first.Digest()&&!f.history.UndoCount(),"playback recording camera modal and busy gates refuse VPD without mutation");
        if(gate==5)f.history.Leave();
    }
}
void VpdLegacyParityChecks(){
    VpdInput input;input.Write("Bone0{bone\n3,4,5;\n0,0,-0.6,0.8;\n}\n\nBone1{bone\n6,7,8;\n0,0,0.6,0.8;\n}\n\n",2);
    for(int mode=0;mode<2;++mode)for(int kind=0;kind<4;++kind){PoseNumericFixture controlled,oracle;
        for(auto* f:{&controlled,&oracle}){VpdNames(*f);std::memcpy(f->first.bones[1].name,"bone",5);f->first.m->physicsMode=mode?2:0;
            auto& b=f->first.bones[0];b.type=kind==0?mdl::BoneType{}:(kind==2?mdl::BoneType::UnderIk:mdl::BoneType::FixedAxis);
            b.flags=kind==2?mdl::kBoneFlagFixedAxis:0;b.tailBone=kind==3?-1:1;b.axis[2]=-1;f->first.bones[1].position[2]=-3;
            bone_scale::SetPose(f->first.raw(),0,Scale(3));}
        g_Block=oracle.app.get();LoadVpdFile(input.path.c_str());g_Block=controlled.app.get();plugins::NativePoseTestBinding binding(*controlled.app,controlled.history);
        bool changed=false;Check(plugins::ImportBoundVpd(*controlled.app,input.path.c_str(),changed)==MMS_OK&&changed,"valid legacy pose imports in common history");
        for(unsigned i=0;i<2;++i)Check(!std::memcmp(controlled.first.bones[i].trans,oracle.first.bones[i].trans,12)&&
            !std::memcmp(controlled.first.bones[i].rotQuat,oracle.first.bones[i].rotQuat,16)&&controlled.first.dirty[i]==oracle.first.dirty[i]&&
            controlled.first.selected[i]==oracle.first.selected[i]&&bone_scale::Pose(controlled.first.raw(),int(i))==bone_scale::Pose(oracle.first.raw(),int(i)),
            "PMD PMX fixed-axis UnderIK invalid-tail duplicate-name and duplicate-record results match actual legacy loader");
    }
}
}
void NativeVpdImportChecks(){VpdLegacyParityChecks();VpdHistoryChecks();VpdInvalidChecks();VpdStaleChecks();VpdFailureChecks();VpdTailAndGateChecks();}
