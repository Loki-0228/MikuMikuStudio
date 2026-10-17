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
