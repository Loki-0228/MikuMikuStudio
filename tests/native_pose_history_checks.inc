// Include after Fixture, Scale and PublishNoAllocation in native_edit_history_tests.cpp.
// These checks use the production owned-pose path without a window or renderer.
#include "mikudancestudio/undo_storage.hpp"
void NativePoseHistoryChecks() {
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        f.bones[0].trans[0]=1;f.bones[0].trans[1]=2;f.bones[0].trans[2]=3;f.dirty[0]=1;
        Check(capture.Begin(f.raw())==nh::Result::Ready,"component delta capture begins");
        f.bones[0].trans[0]=7;
        Check(capture.Finish(1,edit)==nh::Result::Ready,"single translation component seals");
        // A later seek can change other components on the very same bone.
        f.bones[0].trans[1]=8;f.bones[0].trans[2]=9;f.bones[0].rotQuat[1]=.6f;f.bones[0].rotQuat[3]=.8f;f.dirty[0]=0;
        int frame=25;nh::ModelEdit candidate;
        for(bool forward:{false,true}) {
            Check(edit->Prepare(f.raw(),frame,forward,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame),"single component delta applies");
            Check(f.bones[0].trans[0]==(forward?7:1)&&f.bones[0].trans[1]==8&&f.bones[0].trans[2]==9&&
                  f.bones[0].rotQuat[1]==.6f&&f.bones[0].rotQuat[3]==.8f&&f.dirty[0]==0,"translation delta preserves same-bone untouched position rotation and dirty");
        }
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        Check(capture.Begin(f.raw())==nh::Result::Ready,"rotation delta capture begins");
        f.bones[0].rotQuat[0]=.6f;f.bones[0].rotQuat[3]=.8f;
        Check(capture.Finish(1,edit)==nh::Result::Ready,"quaternion delta seals");
        f.bones[0].trans[0]=7;f.bones[0].trans[1]=8;f.bones[0].trans[2]=9;f.dirty[0]=1;
        // Rotation is an atomic quaternion, including components that happened
        // to be equal before/after this particular rotation edit.
        f.bones[0].rotQuat[1]=.3f;
        int frame=25;nh::ModelEdit candidate;
        for(bool forward:{false,true}) {
            Check(edit->Prepare(f.raw(),frame,forward,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame),"rotation delta applies");
            Check(f.bones[0].rotQuat[0]==(forward?.6f:0)&&f.bones[0].rotQuat[1]==0&&f.bones[0].rotQuat[3]==(forward?.8f:1)&&
                  f.bones[0].trans[0]==7&&f.bones[0].trans[1]==8&&f.bones[0].trans[2]==9&&f.dirty[0]==1,"rotation delta restores whole quaternion and preserves translation dirty");
        }
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        Check(capture.Begin(f.raw())==nh::Result::Ready,"dirty delta capture begins");
        f.dirty[0]=1;
        Check(capture.Finish(1,edit)==nh::Result::Ready,"dirty-only delta seals");
        f.bones[0].trans[0]=7;f.bones[0].rotQuat[0]=.6f;f.bones[0].rotQuat[3]=.8f;
        int frame=25;nh::ModelEdit candidate;
        for(bool forward:{false,true}) {
            Check(edit->Prepare(f.raw(),frame,forward,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame),"dirty-only delta applies");
            Check(f.dirty[0]==(forward?1:0)&&f.bones[0].trans[0]==7&&f.bones[0].rotQuat[0]==.6f&&f.bones[0].rotQuat[3]==.8f,"dirty delta preserves live transform");
        }
    }
    {
        Fixture f;
        std::vector<std::unique_ptr<nh::OwnedPoseEdit>> history;
        bone_scale::SetPose(f.raw(),0,Scale(2));
        bone_scale::SetKey(f.raw(),3,Scale(4));
        bone_scale::SetKeyCurve(f.raw(),3,{12,32,91,115});
        for(unsigned i=0;i<100;++i) {
            nh::PoseEditCapture capture;
            Check(capture.Begin(f.raw())==nh::Result::Ready,"owned pose capture begins before edit");
            const auto slot=(f.m->undoState[0]+1)%30;
            auto& old=f.m->undoRings[0].slots[slot];
            mikudancestudio::undo_storage::Free(old.bonePose);mikudancestudio::undo_storage::Free(old.auxiliaryPose);old={};
            f.Entry(slot,1,float(i));f.m->undoState[0]=f.m->undoState[1]=slot;
            bone_scale::CaptureUndo(f.raw());
            f.bones[0].trans[0]=float(i+1);f.dirty[0]=static_cast<unsigned char>((i+1)%2);
            bone_scale::SetPose(f.raw(),0,Scale(float(i+3)));
            std::unique_ptr<nh::OwnedPoseEdit> edit;
            Check(capture.Finish(1,edit)==nh::Result::Ready&&edit&&edit->RetainedBytes()>sizeof(*edit),"owned pose seals both directions");
            history.push_back(std::move(edit));
        }
        const auto cursor=f.m->undoState[0],head=f.m->undoState[1];
        const auto undoDirty=f.m->undoDirty,redoDirty=f.m->redoDirty;
        // Every historical pool address is now stale; the records still work.
        Check(mdl::BoneKeyPoolEnsure(*f.m,128),"owned pose fixture expands native key pool");
        f.bones[1].trans[1]=42;
        bone_scale::SetPose(f.raw(),1,Scale(7));
        int frame=37;
        for(unsigned i=100;i>0;--i) {
            nh::ModelEdit candidate;
            Check(history[i-1]->Prepare(f.raw(),frame,false,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame),"owned pose undo survives one hundred ring writes");
            Check(f.bones[0].trans[0]==float(i-1)&&f.dirty[0]==(i-1)%2&&bone_scale::Pose(f.raw(),0)==Scale(float(i+1)),"owned undo restores native and scale authoring");
            Check(f.bones[1].trans[1]==42&&bone_scale::Pose(f.raw(),1)==Scale(7)&&frame==37,"owned undo preserves untouched bone scale and playback cursor");
        }
        for(unsigned i=0;i<100;++i) {
            nh::ModelEdit candidate;
            Check(history[i]->Prepare(f.raw(),frame,true,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame),"owned pose redo survives one hundred ring writes");
            Check(f.bones[0].trans[0]==float(i+1)&&f.dirty[0]==(i+1)%2&&bone_scale::Pose(f.raw(),0)==Scale(float(i+3)),"owned redo restores original result");
            Check(bone_scale::Pose(f.raw(),1)==Scale(7),"owned redo preserves a scale outside the edit delta");
        }
        Check(f.m->undoState[0]==cursor&&f.m->undoState[1]==head&&f.m->undoDirty==undoDirty&&f.m->redoDirty==redoDirty,"owned publication never changes legacy ring metadata");
        Check(bone_scale::Key(f.raw(),3)==Scale(4)&&bone_scale::KeyCurve(f.raw(),3)==bone_scale::Curve{12,32,91,115},"owned scale pose exchange preserves key scales and curves");
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        Check(capture.Begin(f.raw())==nh::Result::Ready,"empty capture begins");
        for(int operation:{0,2,3,4,5,6,7,8})
            Check(capture.Finish(operation,edit)==nh::Result::Invalid&&!edit,"unsupported record types are explicitly rejected");
        Check(capture.Finish(1,edit)==nh::Result::Empty&&!edit,"unchanged pose does not create history");
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        Check(capture.Begin(f.raw())==nh::Result::Ready,"physical mode rejection capture begins");
        f.bones[0].physicsDisabled=1;f.bones[0].trans[0]=12;bone_scale::SetPose(f.raw(),0,Scale(3));
        Check(capture.Finish(1,edit)==nh::Result::Invalid&&!edit&&f.bones[0].physicsDisabled==1,"physical mode changes cannot be mislabeled as pose history");
        const auto before=f.Digest();
        failAfter=0;const auto rollback=capture.Rollback();failAfter=-1;
        Check(!rollback&&f.Digest()==before&&bone_scale::Pose(f.raw(),0)==Scale(3),"unsupported physical mutation rejects rollback without a partial restore");
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        Check(capture.Begin(f.raw())==nh::Result::Ready,"scale-only capture begins without side storage");
        bone_scale::SetPose(f.raw(),0,Scale(3));
        Check(capture.Finish(1,edit)==nh::Result::Ready,"scale-only pose creates owned history");
        int frame=4;nh::ModelEdit candidate;
        Check(edit->Prepare(f.raw(),frame,false,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame)&&!bone_scale::HasPose(f.raw()),"owned undo restores absent scale pose");
        Check(edit->Prepare(f.raw(),frame,true,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame)&&bone_scale::Pose(f.raw(),0)==Scale(3),"owned redo reuses retained empty scale storage");
        bone_scale::Forget(f.raw());
        Check(edit->Prepare(f.raw(),frame,true,candidate)==nh::Result::Invalid,"owned history rejects missing nonempty scale storage");
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        bone_scale::SetPose(f.raw(),0,Scale(2));
        const auto before=f.Digest();const auto revision=bone_scale::Revision(f.raw());
        failAfter=0;const auto begin=capture.Begin(f.raw());failAfter=-1;
        Check(begin==nh::Result::Memory&&f.Digest()==before&&bone_scale::Revision(f.raw())==revision,"failed capture begin changes no authored state");
        Check(capture.Begin(f.raw())==nh::Result::Ready,"capture retries after failed allocation");
        f.bones[0].trans[0]=9;f.dirty[0]=1;bone_scale::SetPose(f.raw(),0,Scale(3));
        const auto after=f.Digest();
        failAfter=0;const auto finish=capture.Finish(1,edit);failAfter=-1;
        Check(finish==nh::Result::Memory&&!edit&&f.Digest()==after,"failed seal leaves applied state for rollback");
        failAfter=0;const auto rollback=capture.Rollback();failAfter=-1;
        Check(rollback&&f.bones[0].trans[0]==0&&f.dirty[0]==0&&bone_scale::Pose(f.raw(),0)==Scale(2),"failed seal rolls back pose and scale without allocation");
        Check(!capture.Rollback(),"capture rollback is consumed once");
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        bone_scale::SetPose(f.raw(),0,Scale(2));
        Check(capture.Begin(f.raw())==nh::Result::Ready,"stale fixture begins");
        f.bones[0].trans[0]=9;bone_scale::SetPose(f.raw(),0,Scale(3));
        Check(capture.Finish(1,edit)==nh::Result::Ready,"stale fixture seals");
        int frame=2;nh::ModelEdit candidate;
        const auto after=f.Digest();
        bool prepared=false;unsigned failures=0;
        const auto scaleRevision=bone_scale::Revision(f.raw());
        for(long limit=0;limit<128;++limit) {
            failAfter=limit;const auto prepare=edit->Prepare(f.raw(),frame,false,candidate);failAfter=-1;
            Check(f.Digest()==after&&bone_scale::Revision(f.raw())==scaleRevision,"owned preparation allocation sweep preserves state");
            if(prepare==nh::Result::Ready){prepared=true;break;}
            Check(prepare==nh::Result::Memory,"owned allocation failure reports memory");++failures;
        }
        Check(prepared&&failures>3,"owned preparation allocation sweep reaches all preparation stages");
        Check(edit->Prepare(f.raw(),frame,false,candidate)==nh::Result::Ready,"owned candidate prepares");
        f.bones[0].trans[0]=10;
        Check(!PublishNoAllocation(candidate,frame)&&f.bones[0].trans[0]==10,"owned candidate rejects changed native pose");
        f.bones[0].trans[0]=9;
        Check(edit->Prepare(f.raw(),frame,false,candidate)==nh::Result::Ready,"owned scale candidate prepares");
        bone_scale::SetPose(f.raw(),0,Scale(4));
        Check(!PublishNoAllocation(candidate,frame)&&bone_scale::Pose(f.raw(),0)==Scale(4),"owned candidate rejects changed scale pose");
        bone_scale::SetPose(f.raw(),0,Scale(3));
        Check(edit->Prepare(f.raw(),frame,false,candidate)==nh::Result::Ready,"ring independence candidate prepares");
        f.m->undoState[0]=17;f.m->undoState[1]=22;f.m->undoDirty=0;f.m->redoDirty=1;
        bone_scale::CaptureUndo(f.raw());
        Check(PublishNoAllocation(candidate,frame)&&f.m->undoState[0]==17&&f.m->undoState[1]==22,"ring changes alone do not stale an owned candidate");
        Check(!PublishNoAllocation(candidate,frame),"owned candidate is consumed once");
        Fixture other;
        Check(edit->Prepare(other.raw(),frame,false,candidate)==nh::Result::Invalid,"owned pose rejects a different model");
    }
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> edit;
        Check(capture.Begin(f.raw())==nh::Result::Ready,"adoption rollback capture begins");
        f.bones[0].trans[2]=7;
        Check(capture.Finish(1,edit)==nh::Result::Ready,"adoption rollback seals owned edit");
        failAfter=0;const auto rollback=capture.Rollback();failAfter=-1;
        Check(rollback&&f.bones[0].trans[2]==0,"host can rollback after successful seal if adoption fails");
        int frame=5;nh::ModelEdit candidate;
        Check(edit->Prepare(f.raw(),frame,true,candidate)==nh::Result::Ready&&PublishNoAllocation(candidate,frame)&&f.bones[0].trans[2]==7,"sealed payload is independent of capture rollback storage");
    }
}
