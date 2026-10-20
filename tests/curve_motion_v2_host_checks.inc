void V2HostRoundTrip(){
    namespace fixture=curve_v2_fixture;
    Fixture f({100,10,20,115});auto channels=f.Read();const auto source=fixture::Source();
    const auto input=fixture::Bytes(source);curve_motion::Motion imported;std::string detail;std::wstring error;
    Check(curve_motion::Decode(input.data(),input.size(),curve_motion::Kind::Camera,imported,detail)&&
          curve_motion::RestoreChannels(imported,channels,detail),"restore independent V2 input into real eleven-channel host source");
    for(auto& channel:channels)if(!channel.nativeControls.empty())curve_host::InstallImportedCameraTiming(channel);
    Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,channels,error),"publish V2 native/Free authoring through the real host writer");
    const auto actual=f.Read();
    const auto verify=[&](const Channels& value){
        Check(value[0].track.keys.size()==3&&value[1].track.keys.size()==2&&value[0].preservedCameraIntervals.size()==2&&
              !value[0].preservedCameraIntervals[0].oneFrameHold&&value[0].preservedCameraIntervals[0].transportTiming,
            "host retains explicit continuous timing and sparse Free author keys");
        Check(value[1].track.keys[0].outSlope==source.tracks[1].track.keys[0].outSlope&&
              value[1].track.keys[1].inWeight==source.tracks[1].track.keys[1].inWeight,"mixed Free handles survive the actual host path");
        for(unsigned tick=0;tick<=104;++tick){const double frame=tick*.125;
            Check(Value(value[0],frame)==fixture::ExpectedX(source,frame),"canonical host matches main V2 fractional playback without analytic replacement");
            Check(Value(value[1],frame)==ac::Evaluate(source.tracks[1].track,frame).value,"canonical host keeps the authored Free lane");}
    };
    verify(actual);const auto native=f.Raw();f.app->CurrentFrame()=7;f.app->CameraPositionX()=123.f;
    const auto pose=Pose(*f.app);Channels prepared;
    Check(curve_host::PrepareMotionChannels(actual,prepared,error,true,0),"transportable V2 timing needs no integer-sampling allocation budget");
    verify(prepared);auto captured=curve_motion::CaptureChannels(curve_motion::Kind::Camera,prepared);std::vector<std::uint8_t> bytes;
    Check(curve_motion::Encode(captured,bytes,detail)&&std::memcmp(bytes.data()+bytes.size()-40,"MMVCURV2",8)==0,
        "real host export preparation preserves V2 author timing instead of baking it away");
    curve_motion::Motion exported;Check(curve_motion::Decode(bytes.data(),bytes.size(),curve_motion::Kind::Camera,exported,detail),"host V2 output reopens");
    for(unsigned tick=0;tick<=104;++tick)Check(curve_motion::Evaluate(exported,"x",tick*.125).value==fixture::ExpectedX(source,tick*.125),
        "actual host import/save/reopen retains fractional native values exactly");
    Check(f.Raw()==native&&Pose(*f.app)==pose&&f.app->CurrentFrame()==7,"save preparation never modifies native keys, dirty pose, or playhead");
    pmm_extension::Document document;curve_host::Capture(*f.app,document);
    const auto* record=pmm_extension::Find(document,"org.mikumikustudio.camera_intervals.v1");
    Check(record&&record->value.size()>12&&record->value[4]==2,"PMM uses explicit interval v2 only when new timing semantics need it");
    curve_host::Reset(f.app.get());curve_host::Load(*f.app,document);verify(f.Read());
    Check(f.Raw()==native&&Pose(*f.app)==pose&&f.app->CurrentFrame()==7,"PMM authoring reload preserves the native pool, current edit pose, and playhead");
#if !MMD_ORIGINAL_PROFILE
    curve_host::TrackAuthoring retained(*f.app,curve_host::Kind::Camera,0);
    Check(retained.RetainedBytes()>=2*sizeof(curve_editor::NativeCameraInterval),"history retention includes the extended timing records");
#endif
    auto opaque=actual;opaque[0].sample=[](const curve_editor::Channel&,double frame)->ac::Sample{return {frame*.125+3,.125};};
    auto sentinel=prepared;
    Check(!curve_host::PrepareMotionChannels(opaque,sentinel,error,true,0)&&sentinel[0].preservedCameraIntervals==prepared[0].preservedCameraIntervals,
        "opaque callbacks still obey the original cumulative bake budget and preserve output on failure");
    auto slices=actual;auto& interval=slices[0].preservedCameraIntervals[1];
    interval.sourceLast=25;interval.sampleLast=25;
    Check(!curve_motion::CanTransportCameraTiming(slices[0]),"different source-domain exact slices are not mislabeled as four-control transport");
    Check(!curve_host::PrepareMotionChannels(slices,sentinel,error,true,0)&&sentinel[0].track.keys.size()==3,
        "nontransportable exact slices retain their existing bounded bake path");
    auto duplicates=actual;auto duplicate=duplicates[0].preservedCameraIntervals.back();duplicate.controls={0,127,127,0};
    duplicates[0].preservedCameraIntervals.push_back(duplicate);
    Check(!curve_motion::CanTransportCameraTiming(duplicates[0]),"ambiguous duplicate source intervals cannot choose a different row during transport");
    Check(curve_host::PrepareMotionChannels(duplicates,sentinel,error)&&!sentinel[0].sample&&sentinel[0].track.keys.size()==14,
        "duplicate source intervals use the established canonical sampling fallback");
    for(int frame=0;frame<=13;++frame)Check(ac::Evaluate(sentinel[0].track,frame).value==Value(duplicates[0],frame),
        "duplicate interval fallback preserves the row actually used by the host sampler");
    auto unsorted=actual;std::reverse(unsorted[0].preservedCameraIntervals.begin(),unsorted[0].preservedCameraIntervals.end());
    Check(!curve_motion::CanTransportCameraTiming(unsorted[0]),"unsorted source metadata cannot bypass indexed transport validation");
    Channels output=actual;allocation_fault::remaining=0;
    const bool preparedOk=curve_host::PrepareMotionChannels(actual,output,error);
    allocation_fault::remaining=-1;
    Check(!preparedOk&&output[0].preservedCameraIntervals==actual[0].preservedCameraIntervals&&f.Raw()==native,
        "V2 export allocation failure preserves both destination metadata and live native keys");
    auto nativeNear=Fixture({20,20,107,107}).Read();
    for(std::size_t end=1;end<nativeNear[0].track.keys.size();++end)nativeNear[0].nativeControls[end]={20,20+5e-13,107,107+5e-13};
    Check(!curve_motion::CanTransportCameraTiming(nativeNear[0]),"ordinary double controls do not acquire MMSN's different near-diagonal rule");
    Check(curve_host::PrepareMotionChannels(nativeNear,output,error)&&!output[0].sample&&output[0].track.keys.size()==81,
        "non-equivalent near-diagonal native controls keep the established bake path");
    const auto nearMotion=curve_motion::CaptureChannels(curve_motion::Kind::Camera,output);
    Check(curve_motion::Encode(nearMotion,bytes,detail)&&curve_motion::Decode(bytes.data(),bytes.size(),curve_motion::Kind::Camera,exported,detail),
        "mixed nontransportable near-diagonal camera still exports a standard-compatible proxy");
    for(int frame=0;frame<=80;++frame)Check(curve_motion::Evaluate(exported,"x",frame).value==Value(nativeNear[0],frame),
        "ordinary strict native diagonal behavior is unchanged at every original proxy sample");
    // Unknown flag bits cannot partially install the new PMM interval state.
    auto damaged=document;auto* bad=const_cast<pmm_extension::Record*>(pmm_extension::Find(damaged,"org.mikumikustudio.camera_intervals.v1"));
    Check(bad&&bad->value.size()>159,"V2 interval fixture contains a complete flag field");bad->value[156]|=4;
    curve_host::Load(*f.app,damaged);const auto refused=f.Read();
    Check(refused[0].preservedCameraIntervals.empty()&&f.Raw()==native,"unknown PMM interval flags reject the record without writing native keys");
}
