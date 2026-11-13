// Real shared-history publication compared bit-for-bit with a subsequent
// ordinary SeekModelFrame on the same registered state and playhead.
namespace {
void NativeNegativeSeekParityChecks() {
    namespace n=negative_model_delete_target_checks;
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly})
    for(bool display:{false,true}) {
        n::Fixture f;const auto selection=n::Seed(f,display);
        n::Owner owner(f.app.get());plugins::NativePoseTestBinding binding(*f.app,f.history);
        Check(f.history.SetMode(mode)==MMS_OK,"choose negative seek parity history mode");
        auto change=n::Plan(f,selection);
        const int frames[]={-5,-9,-3,-19,-1,-20,-37,-11,-7};
        for(unsigned step=0;step<sizeof(frames)/sizeof(frames[0]);++step) {
            const int frame=frames[step];f.app->CurrentFrame()=frame;
            if(!step) {
                std::unique_ptr<plugins::SharedHistory::NativeTransaction> transaction;
                Check(f.history.PrepareNative("negative seek parity",change,transaction)==MMS_OK&&transaction,
                    "prepare real joint deletion at negative frame");
                Check(CommitRegistrationNoAllocation(f.history,std::move(transaction))==MMS_OK,
                    "publish real joint deletion without allocation");
            }else Check(f.history.History(step%2==0)==MMS_OK,"replay real shared negative history at changed playhead");
            const auto bones=f.first.bones;const float morph=f.first.morphs[0].value;
            std::array<bone_scale::Matrix,2> scales{bone_scale::Pose(f.first.raw(),0),bone_scale::Pose(f.first.raw(),1)};
            SeekModelFrame(f.first.raw(),frame,0);
            for(unsigned bone=0;bone<2;++bone) {
                Check(!std::memcmp(bones[bone].trans,f.first.bones[bone].trans,sizeof bones[bone].trans),
                    "shared negative position is bit-identical to ordinary seek");
                Check(!std::memcmp(bones[bone].rotQuat,f.first.bones[bone].rotQuat,sizeof bones[bone].rotQuat),
                    "shared negative quaternion preserves ordinary seek rounding exactly");
                const auto scale=bone_scale::Pose(f.first.raw(),int(bone));
                Check(!std::memcmp(scales[bone].v.data(),scale.v.data(),sizeof scale.v),
                    "shared negative scale is bit-identical to ordinary seek");
            }
            Check(!std::memcmp(&morph,&f.first.morphs[0].value,sizeof morph),
                "shared negative morph is bit-identical to ordinary seek");
        }
    }
}
}
