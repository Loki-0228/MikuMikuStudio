// Include after native_pose_scope_checks.inc; call NativePoseCommandChecks().
// Reuses its hidden windows and invokes the production command-family entry.
namespace mikudancestudio { void CmdControl450(MMDApp*,HWND,std::uint16_t,std::uint16_t); }
namespace {
struct PoseCommandFixture : PoseNumericFixture {
    PoseCommandFixture() {
        for(auto* f:{&first,&second}) {
            strcpy_s(f->bones[0].name,"\x8d\xb6" "arm");
            strcpy_s(f->bones[1].name,"\x89\x45" "arm");
            f->bones[1].parent=0;
        }
    }
    ~PoseCommandFixture() {
        std::free(app->BoneCopyRecords());app->BoneCopyRecords()=nullptr;app->state.copiedBoneCount=0;
        // The existing clipboard table is keyed by app address. Clear values
        // before a later fixture can reuse that address.
        bone_scale::CopyClipboard(app.get(),0,{},true);
    }
    void Command(unsigned id){CmdControl450(app.get(),app->MainWindow(),std::uint16_t(id),0);}
    void Copy(bool both=false) {
        app->SelectedModelSlot()=0;first.selected[0]=1;first.selected[1]=both?1:0;Command(496);
        Check(app->state.copiedBoneCount==(both?2:1)&&app->BoneCopyRecords(),"production bone copy fills clipboard before pose command");
    }
};
bone_scale::Matrix CommandScale(float diagonal=2) {
    bone_scale::Matrix result;result.v={diagonal,.25f,.5f,.25f,3,.75f,.5f,.75f,4};return result;
}
void SeedCommandSource(PoseCommandFixture& f) {
    for(int i=0;i<2;++i) {
        f.first.bones[i].trans[0]=float(2+i);f.first.bones[i].trans[1]=float(4+i);f.first.bones[i].trans[2]=float(6+i);
        f.first.bones[i].rotQuat[0]=.1f;f.first.bones[i].rotQuat[1]=.2f;f.first.bones[i].rotQuat[2]=.3f;f.first.bones[i].rotQuat[3]=.9f;
        bone_scale::SetPose(f.first.raw(),i,CommandScale(float(2+i)));
    }
}
void PoseResetCommandChecks() {
    PoseCommandFixture f;SeedCommandSource(f);f.first.selected[0]=f.first.selected[1]=1;f.first.m->selectedBone=1;
    const auto root=f.first.bones[0],child=f.first.bones[1];const auto ring=PoseRingDigest(f.first);
    plugins::NativePoseTestBinding binding(*f.app,f.history);f.Command(495);
    Check(f.history.UndoCount()==1&&f.first.bones[0].trans[0]==root.trans[0]&&f.first.bones[0].trans[1]==0&&
        f.first.bones[0].trans[2]==root.trans[2]&&f.first.bones[0].rotQuat[3]==1,
        "bound reset preserves native root X/Z and resets root Y and rotation");
    Check(f.first.bones[1].trans[0]==0&&f.first.bones[1].trans[1]==0&&f.first.bones[1].trans[2]==0&&
        f.first.bones[1].rotQuat[0]==0&&f.first.bones[1].rotQuat[1]==0&&f.first.bones[1].rotQuat[2]==0&&f.first.bones[1].rotQuat[3]==1,
        "bound reset clears every selected parented bone pose component");
    Check(bone_scale::Identity(bone_scale::Pose(f.first.raw(),0))&&bone_scale::Identity(bone_scale::Pose(f.first.raw(),1))&&
        f.first.dirty[0]==1&&f.first.dirty[1]==1,"bound reset includes selected scale poses and unregistered flags");
    Check(f.first.selected[0]&&f.first.selected[1]&&f.first.m->selectedBone==1&&f.app->PhysicsResetPending()==0&&
        f.SelectionUnchanged()&&PoseRingDigest(f.first)==ring,"reset leaves selection physics flag and legacy ring intact");
    f.Command(495);Check(f.history.UndoCount()==1,"already reset pose adds no history node");
    Check(f.history.History(false)==MMS_OK&&f.first.bones[0].trans[1]==root.trans[1]&&f.first.bones[1].trans[0]==child.trans[0]&&
        bone_scale::Pose(f.first.raw(),0)==CommandScale()&&bone_scale::Pose(f.first.raw(),1)==CommandScale(3),
        "one undo restores all selected reset poses and nonuniform scales");
    f.first.selected[0]=f.first.selected[1]=0;f.Command(495);
    Check(f.history.UndoCount()==0&&f.history.RedoCount()==1,"reset with empty selection preserves redo");
    Check(f.history.History(true)==MMS_OK&&f.first.bones[1].trans[0]==0,"reset redo is independent of current selection");
    // Parentless reset uses the root's final seed and performs all D3DX work
    // before the scope. The test does not require a renderer/device.
    if(d3dx::Get().Load()) {
        f.first.selected[0]=f.first.selected[1]=1;f.first.bones[1].parent=-1;
        f.first.bones[0].trans[1]=8;f.first.bones[1].trans[0]=20;
        f.first.bones[1].position[0]=2;f.first.bones[1].position[1]=3;
        f.first.bones[1].rotQuat[2]=.6f;f.first.bones[1].rotQuat[3]=.8f;
        d3dx::D3DXMATRIXF matrix{};d3dx::Get().matrixRotationQuaternion(&matrix,f.first.bones[1].rotQuat);
        float expected[3]{};
        for(int axis=0;axis<3;++axis) {
            double value=double(f.first.bones[1].position[1])*matrix.m[1][axis];
            value+=double(f.first.bones[1].position[0])*matrix.m[0][axis];value+=double(f.first.bones[1].position[2])*matrix.m[2][axis];
            value+=matrix.m[3][axis];value-=f.first.bones[1].position[axis];value+=axis==1?0:f.first.bones[0].trans[axis];
            expected[axis]=float(value);
        }
        f.Command(495);
        Check(f.first.bones[1].trans[0]==expected[0]&&f.first.bones[1].trans[1]==expected[1]&&f.first.bones[1].trans[2]==expected[2]&&
            f.first.bones[1].rotQuat[3]==1,"parentless reset matches native root-relative bake");
    }
}
void PosePasteCommandChecks() {
    PoseCommandFixture f;SeedCommandSource(f);f.Copy();
    const auto source=f.first.bones[0];const auto scale=bone_scale::Pose(f.first.raw(),0);
    const auto firstRing=PoseRingDigest(f.first),secondRing=PoseRingDigest(f.second);
    f.app->SelectedModelSlot()=1;f.second.selected[0]=0;f.second.selected[1]=1;f.second.m->selectedBone=1;
    plugins::NativePoseTestBinding binding(*f.app,f.history);f.Command(497);
    Check(f.history.UndoCount()==1&&!std::memcmp(f.second.bones[0].trans,source.trans,sizeof(source.trans))&&
        !std::memcmp(f.second.bones[0].rotQuat,source.rotQuat,sizeof(source.rotQuat))&&bone_scale::Pose(f.second.raw(),0)==scale,
        "real bound paste matches names across models and transfers pose plus scale");
    Check(f.second.selected[0]==1&&f.second.selected[1]==0&&f.second.m->selectedBone==0&&f.second.dirty[0]==1&&
        f.app->PhysicsResetPending()==1&&f.app->SceneModified()==1&&f.SelectionUnchanged(),
        "successful paste publishes native bone selection physics request and dirty UI only after commit");
    f.app->SelectedModelSlot()=0;f.Command(495);
    Check(f.history.UndoCount()==2&&f.first.bones[0].trans[1]==0,"different model reset joins paste history");
    Check(f.history.History(false)==MMS_OK&&f.first.bones[0].trans[1]==source.trans[1]&&f.second.bones[0].trans[0]==source.trans[0],
        "last model reset undoes before previous model paste");
    Check(f.history.History(false)==MMS_OK&&f.second.bones[0].trans[0]==0&&bone_scale::Identity(bone_scale::Pose(f.second.raw(),0)),
        "earlier model paste restores its own before pose and scale");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK,"cross-model paste and reset redo in order");
    f.app->SelectedModelSlot()=1;f.app->PhysicsResetPending()=0;f.Command(498);
    auto mirrored=scale;for(int i:{1,2,3,6})mirrored.v[i]=-mirrored.v[i];
    Check(f.history.UndoCount()==3&&f.second.bones[1].trans[0]==-source.trans[0]&&f.second.bones[1].trans[1]==source.trans[1]&&
        f.second.bones[1].rotQuat[0]==source.rotQuat[0]&&f.second.bones[1].rotQuat[1]==-source.rotQuat[1]&&
        f.second.bones[1].rotQuat[2]==-source.rotQuat[2]&&f.second.bones[1].rotQuat[3]==source.rotQuat[3]&&
        bone_scale::Pose(f.second.raw(),1)==mirrored,"mirror paste resolves opposite SJIS name and mirrors pose and scale shear terms");
    Check(f.second.selected[0]==0&&f.second.selected[1]==1&&f.second.m->selectedBone==1&&f.app->PhysicsResetPending()==1,
        "mirror paste publishes the matched opposite-side selection");
    Check(f.history.History(false)==MMS_OK&&f.second.bones[1].trans[0]==0&&bone_scale::Identity(bone_scale::Pose(f.second.raw(),1)),
        "mirror paste undo restores opposite bone without altering source bone");
    f.app->PhysicsResetPending()=0;f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;
    const auto unchanged=f.second.Digest();f.Command(497);
    Check(f.second.Digest()==unchanged&&f.history.UndoCount()==2&&f.history.RedoCount()==1&&
        f.app->PhysicsResetPending()==0&&f.app->SceneModified()==0&&f.app->RecordedFrameCountByte1()==0,
        "already matching paste preserves selection dirty flags physics request and redo");
    strcpy_s(f.first.bones[0].name,"missing");f.Copy();f.app->SelectedModelSlot()=1;
    for(unsigned command:{497u,498u})f.Command(command);
    Check(f.second.Digest()==unchanged&&f.history.UndoCount()==2&&f.history.RedoCount()==1&&f.app->PhysicsResetPending()==0,
        "no-match normal and mirror paste preserve scene and redo");
    f.app->state.copiedBoneCount=0;f.Command(497);f.Command(498);
    Check(f.history.RedoCount()==1&&PoseRingDigest(f.first)==firstRing&&PoseRingDigest(f.second)==secondRing&&f.SelectionUnchanged(),
        "empty clipboard and all bound commands preserve both legacy rings and global selection");
}
void PoseCommandFailureChecks(unsigned command) {
    PoseCommandFixture f;SeedCommandSource(f);
    if(command==495){f.first.selected[0]=f.first.selected[1]=1;f.first.m->selectedBone=1;}
    else {f.Copy(true);f.app->SelectedModelSlot()=1;f.second.selected[0]=0;f.second.selected[1]=1;f.second.m->selectedBone=1;}
    auto& target=command==495?f.first:f.second;
    const auto digest=target.Digest(),ring=PoseRingDigest(target);const auto scale=bone_scale::PoseValues(target.raw());
    const auto selection=target.selected;const auto selected=target.m->selectedBone;
    plugins::NativePoseTestBinding binding(*f.app,f.history);unsigned failures=0;bool succeeded=false;
    for(long limit=0;limit<256;++limit) {
        failAfter=limit;f.Command(command);failAfter=-1;
        if(f.history.UndoCount()){succeeded=true;break;}
        ++failures;
        Check(target.Digest()==digest&&bone_scale::PoseValues(target.raw())==scale&&target.selected==selection&&target.m->selectedBone==selected&&
            f.app->PhysicsResetPending()==0&&f.app->SceneModified()==0&&f.app->RecordedFrameCountByte1()==0&&
            !IsWindowEnabled(f.Edit(panel::kUndoButton))&&!IsWindowEnabled(f.Edit(panel::kRedoButton))&&f.SelectionUnchanged(),
            "failed pose command restores authoring selection physics dirty UI and native ring");
        Check(f.history.Enter()==MMS_OK,"failed pose command releases journal after rollback");f.history.Leave();
    }
    Check(succeeded&&failures>8&&PoseRingDigest(target)==ring,"command allocation sweep reaches precompute capture writes and journal preparation before success");
    Check(f.history.History(false)==MMS_OK&&bone_scale::PoseValues(target.raw())==scale,"first successful command remains undoable after all allocation failures");
    // Leave a redo branch and fail a new command before reservation: no empty
    // operation is allowed to prune that existing branch.
    f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;f.app->PhysicsResetPending()=0;
    const auto afterUndo=target.Digest();failAfter=0;f.Command(command);failAfter=-1;
    Check(f.history.UndoCount()==0&&f.history.RedoCount()==1&&target.Digest()==afterUndo&&f.app->SceneModified()==0&&f.app->PhysicsResetPending()==0,
        "failed command after undo preserves redo and current native state");
}
void PoseCommandLegacyChecks() {
    PoseCommandFixture f;SeedCommandSource(f);const auto cursor=f.first.m->undoState[0];f.Command(495);
    Check(f.first.m->undoState[0]==(cursor+1)%30&&f.history.UndoCount()==0&&f.first.bones[0].trans[1]==0,
        "unbound reset retains native legacy ring behavior");
    f.Copy();f.app->SelectedModelSlot()=1;
    strcpy_s(f.second.bones[0].name,"other0");strcpy_s(f.second.bones[1].name,"other1");
    for(unsigned command:{497u,498u}) {
        const auto before=f.second.m->undoState[0];f.Command(command);
        Check(f.second.m->undoState[0]==(before+1)%30&&f.history.UndoCount()==0&&f.app->PhysicsResetPending()==1,
            "unbound paste commands retain legacy unmatched-clipboard behavior");
    }
}
}
void NativePoseCommandChecks() {
    PoseResetCommandChecks();PosePasteCommandChecks();
    for(unsigned command:{495u,497u,498u})PoseCommandFailureChecks(command);
    PoseCommandLegacyChecks();
}
