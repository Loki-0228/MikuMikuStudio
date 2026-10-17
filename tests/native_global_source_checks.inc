// Include after native_global_paste_checks.inc and native accessory fixtures.
namespace {
plugins::GlobalPasteSource GlobalSourceOf(const GlobalPasteFixture& f,unsigned offset) {
    plugins::GlobalPasteSource source;source.frameOffset=offset;
    source.cameras=f.cameras;source.lights=f.lights;source.shadows=f.shadows;
    source.gravity=f.gravity;source.accessories=f.accessories;return source;
}
void GlobalSourceBreakClipboard(GlobalPasteFixture& f) {
    f.app->CameraClipboard()=nullptr;f.app->LightClipboard()=nullptr;f.app->ShadowClipboard()=nullptr;
    f.app->GravityClipboard()=nullptr;f.app->AccessoryClipboard()=nullptr;
    auto& count=f.app->ClipboardCounts();count={};count.cameras=count.lights=count.shadows=count.gravity=count.accessories=UINT32_MAX;
}
bool GlobalSourceClipboardStillBroken(GlobalPasteFixture& f) {
    const auto& n=f.app->ClipboardCounts();
    return !f.app->CameraClipboard()&&!f.app->LightClipboard()&&!f.app->ShadowClipboard()&&!f.app->GravityClipboard()&&!f.app->AccessoryClipboard()&&
        n.cameras==UINT32_MAX&&n.lights==UINT32_MAX&&n.shadows==UINT32_MAX&&n.gravity==UINT32_MAX&&n.accessories==UINT32_MAX;
}
bool GlobalSourcePoseSame(MMDApp& a,MMDApp& b) {
    return MorphSameFloat(a.CameraPositionX(),b.CameraPositionX())&&MorphSameFloat(a.CameraPositionY(),b.CameraPositionY())&&
        MorphSameFloat(a.CameraPositionZ(),b.CameraPositionZ())&&!std::memcmp(a.CameraRotation(),b.CameraRotation(),3*sizeof(float))&&
        MorphSameFloat(a.CameraFov(),b.CameraFov())&&MorphSameFloat(a.CameraDistance(),b.CameraDistance())&&
        a.CameraPerspective()==b.CameraPerspective()&&a.CameraParentModel()==b.CameraParentModel()&&a.CameraParentBone()==b.CameraParentBone()&&
        !std::memcmp(a.LightDirection(),b.LightDirection(),3*sizeof(float))&&!std::memcmp(a.LightColor(),b.LightColor(),3*sizeof(float))&&
        a.ShadowMode()==b.ShadowMode()&&MorphSameFloat(a.ShadowDistance(),b.ShadowDistance())&&
        MorphSameFloat(a.GravityMagnitude(),b.GravityMagnitude())&&MorphSameFloat(a.GravityX(),b.GravityX())&&
        MorphSameFloat(a.GravityY(),b.GravityY())&&MorphSameFloat(a.GravityZ(),b.GravityZ())&&a.GravityNoise()==b.GravityNoise()&&
        a.GravityNoiseModeWord()==b.GravityNoiseModeWord()&&AccessoryRegistrationPoseEqual(*a.AccessorySlot(7),*b.AccessorySlot(7));
}
void NativeGlobalSourceChecks() {
    std::puts("  global source: immutable five-family source versus clipboard overload");
    for(unsigned offset:{7u,40u,90u}) {
        GlobalPasteFixture f,oracle;auto source=GlobalSourceOf(f,offset);
        const auto difference=offset-unsigned(oracle.app->CurrentFrame());
        for(auto& key:oracle.cameras)key.frame+=difference;for(auto& key:oracle.lights)key.frame+=difference;
        for(auto& key:oracle.shadows)key.frame+=difference;for(auto& key:oracle.gravity)key.frame+=difference;
        for(auto& key:oracle.accessories)key.frameOffset+=difference;oracle.Bind(true);
        GlobalSourceBreakClipboard(f);const auto before=f.Digest();const GlobalPasteKeys original(*f.app);
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&change&&f.Digest()==before&&
            f.app->CurrentFrame()==7&&GlobalSourceClipboardStillBroken(f),"explicit source ignores malformed application clipboard and leaves current playhead unchanged");
        source={};f.cameras.clear();f.lights.clear();f.shadows.clear();f.gravity.clear();f.accessories.clear();
        GlobalPasteCommit(f,change);GlobalPasteCommit(oracle,GlobalPastePlan(oracle));
        const GlobalPasteKeys expected(*oracle.app);
        Check(expected.Same(*f.app)&&GlobalSourcePoseSame(*f.app,*oracle.app)&&f.app->LastRegisteredFrame()==oracle.app->LastRegisteredFrame()&&
            f.app->CurrentFrame()==7&&GlobalSourceClipboardStillBroken(f),"all five explicit-source families match the original clipboard overload with offset translated in private records");
        f.app->CurrentFrame()=oracle.app->CurrentFrame()=23;
        Check(f.history.History(false)==MMS_OK&&oracle.history.History(false)==MMS_OK&&original.Same(*f.app)&&GlobalSourcePoseSame(*f.app,*oracle.app),
            "explicit-source undo restores all retained keys and evaluates the real current playhead");
        Check(f.history.History(true)==MMS_OK&&oracle.history.History(true)==MMS_OK&&expected.Same(*f.app)&&GlobalSourcePoseSame(*f.app,*oracle.app)&&
            GlobalSourceClipboardStillBroken(f),"explicit-source redo owns source values after both source and clipboard become unavailable");
    }
    std::puts("  global source: empty no-op offset and count rejection");
    {
        GlobalPasteFixture f;auto source=GlobalSourceOf(f,40);GlobalSourceBreakClipboard(f);
        std::shared_ptr<plugins::HistoryChange> change;
        Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&change,"explicit-source fixture plans");GlobalPasteCommit(f,change);
        auto next=source;next.lights.back().color[0]+=.125f;
        Check(plugins::PlanGlobalKeyPaste(*f.app,next,change)==MMS_OK&&change,"one changed source family creates another atomic step");GlobalPasteCommit(f,change);
        Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"explicit source no-op starts with existing redo");
        const auto before=f.Digest();
        Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_OK&&!change&&f.Digest()==before&&f.history.RedoCount()==1,
            "identical explicit source retains all marks authored pose and existing redo");
        plugins::GlobalPasteSource empty;
        Check(plugins::PlanGlobalKeyPaste(*f.app,empty,change)==MMS_OK&&!change&&f.Digest()==before&&f.history.RedoCount()==1,
            "empty explicit source does not inspect application clipboard");
        empty.frameOffset=std::uint32_t(frame_limits::maximum)+1u;
        Check(plugins::PlanGlobalKeyPaste(*f.app,empty,change)==MMS_E_ARGUMENT&&!change&&f.Digest()==before,
            "explicit source rejects an out-of-range offset before empty-source early return");
        source.frameOffset=unsigned(frame_limits::maximum);
        Check(plugins::PlanGlobalKeyPaste(*f.app,source,change)==MMS_E_ARGUMENT&&!change&&f.Digest()==before&&f.history.RedoCount()==1,
            "overflow in a later family record rejects every source participant without truncating redo");
        // KeyPoolLimit is INT32_MAX on this x64 build. A >limit valid vector
        // would require hundreds of GB; do not forge vector internals. Exercise
        // the same public capture boundary with oversized counts and no data.
        Check(plugins::PlanGlobalKeyPaste(*f.app,change)==MMS_E_ARGUMENT&&!change&&f.Digest()==before,
            "clipboard capture rejects oversized key counts before reading null source storage");
    }
    std::puts("  global source: offset-independent first prepare and allocation failure");
    {
        GlobalPasteFixture f;const auto source=GlobalSourceOf(f,90);GlobalSourceBreakClipboard(f);const auto before=f.Digest();
        std::shared_ptr<plugins::HistoryChange> change;bool planned=false;
        for(long allowance=0;allowance<8192&&!planned;++allowance) {
            failAfter=allowance;const auto result=plugins::PlanGlobalKeyPaste(*f.app,source,change);failAfter=-1;
            Check(f.Digest()==before&&!f.history.UndoCount(),"explicit-source allocation failures preserve all live pools poses and damaged clipboard fields");
            if(result==MMS_OK){Check(change!=nullptr,"explicit-source allocation sweep produces one complete record");planned=true;}
            else Check(result==MMS_E_MEMORY&&!change,"explicit-source allocation fault has no partial record");
        }
        Check(planned,"explicit-source allocation sweep reaches every family planner");
        f.app->CurrentFrame()=90;std::unique_ptr<plugins::SharedHistory::NativeTransaction> token;
        const auto moved=f.Digest();
        Check(f.history.PrepareNative("source playhead guard",change,token)==MMS_E_STALE&&!token&&f.Digest()==moved,
            "first prepare locks original real playhead rather than the independent source offset");
        f.app->CurrentFrame()=7;GlobalPasteCommit(f,change);
        Check(mdl::KeyPoolCapacity(f.app->GravityKeys())>3&&mdl::KeyPoolCapacity(f.app->AccessoryKeys(7))>3&&
            GlobalSourceClipboardStillBroken(f),"explicit-source growth transfers accepted pools without repairing or replacing application clipboard");
    }
}
}
