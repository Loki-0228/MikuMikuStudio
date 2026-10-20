void TestNativeTimingV2Transport(){
    namespace fixture=curve_v2_fixture;
    const auto source=fixture::Source();const auto input=fixture::Bytes(source);std::string error;cm::Motion imported;
    Check(cm::Decode(input.data(),input.size(),cm::Kind::Camera,imported,error),"independent committed MMSN/V2 bytes decode: "+error);
    Check(!imported.nativeCamera&&imported.nativeTiming.size()==2&&SameTracks(imported,source),
        "V2 author keys replace the dense proxy without claiming its baked rows as an original native source");
    for(unsigned tick=0;tick<=104;++tick){const double frame=tick*.125;
        CheckNear(cm::Evaluate(imported,"x",frame).value,fixture::ExpectedX(source,frame),0,"V2 fractional value matches the independent main sampler");
        CheckNear(cm::Evaluate(imported,"y",frame).value,ac::Evaluate(source.tracks[1].track,frame).value,0,"mixed Free lane retains its own handles");}
    CheckNear(cm::Evaluate(imported,"x",.5).value,5,0,"V2's explicit continuous one-frame interval is not a native cut");
    auto held=source;held.nativeTiming[0].oneFrameHold=true;
    CheckNear(cm::Evaluate(held,"x",.5).value,0,0,"V2 retains an explicitly requested one-frame native hold");
    std::vector<mikumikustudio::curve_editor::Channel> channels(2);channels[0].id="x";channels[1].id="y";
    Check(cm::RestoreChannels(imported,channels,error),"V2 restores detached channels transactionally: "+error);
    Check(cm::CanTransportCameraTiming(channels[0])&&!cm::CanTransportCameraTiming(channels[1])&&
        channels[0].preservedCameraIntervals.size()==2&&!channels[0].preservedCameraIntervals[0].oneFrameHold,
        "canonical timing and continuous semantics have explicit retained metadata");
    for(unsigned tick=0;tick<=104;++tick){const double frame=tick*.125;
        CheckNear(channels[0].sample(channels[0],frame).value,fixture::ExpectedX(source,frame),0,"restored channel sampler retains V2 fractional timing");}
    const auto captured=cm::CaptureChannels(cm::Kind::Camera,channels);std::vector<std::uint8_t> output;
    Check(cm::Encode(captured,output,error)&&output.size()>40&&std::memcmp(output.data()+output.size()-40,"MMVCURV2",8)==0,
        "captured partial native/Free camera exports the V2 author tail");
    cm::Motion restored;Check(cm::Decode(output.data(),output.size(),cm::Kind::Camera,restored,error)&&SameTracks(restored,source),
        "V2 export preserves sparse scalar keys and Free handles");
    Check(restored.nativeTiming.size()==source.nativeTiming.size(),"all exact timing intervals survive capture and save");
    for(std::size_t i=0;i<restored.nativeTiming.size();++i)Check(restored.nativeTiming[i].id==source.nativeTiming[i].id&&
        restored.nativeTiming[i].endFrame==source.nativeTiming[i].endFrame&&restored.nativeTiming[i].controls==source.nativeTiming[i].controls&&
        restored.nativeTiming[i].oneFrameHold==source.nativeTiming[i].oneFrameHold,"control doubles and hold flag survive V2 without rounding to native bytes");
    cm::Motion compatible;const auto body=WithoutTail(output);
    Check(cm::Decode(body.data(),body.size(),cm::Kind::Camera,compatible,error),"V2 leaves a complete standard VMD body");
    for(int frame=0;frame<=13;++frame)CheckNear(cm::Evaluate(compatible,"x",frame).value,float(fixture::ExpectedX(source,frame)),0,
        "extension-free standard proxy retains original integer-frame compatibility");
    const auto refused=[&](std::vector<std::uint8_t> bad,const char* why,bool crc=true){
        if(crc)fixture::PayloadCrc(bad);cm::Motion untouched;untouched.kind=cm::Kind::Morph;untouched.tracks={{"sentinel",MakeTrack({{0,7}})}};
        const auto before=untouched;Check(!cm::Decode(bad.data(),bad.size(),cm::Kind::Camera,untouched,error)&&SameTracks(untouched,before),why);};
    const auto prefix=TailPrefix(input),countAt=prefix+12+std::size_t(fixture::U64(input,prefix+4)),entry=countAt+4;
    auto bad=input;bad[prefix+1]='D';refused(bad,"MDSN envelope is rejected even with a valid V2 checksum");
    bad=input;bad[prefix+13]='D';refused(bad,"MDSC nested scalar payload is rejected even with a valid checksum");
    bad=input;fixture::Set32(bad,entry+9,2);refused(bad,"invalid V2 hold flags are rejected transactionally");
    bad=input;fixture::Set32(bad,entry+5,0x7fffffffu);refused(bad,"out-of-domain V2 frame does not publish");
    bad=input;bad[entry+4]='q';refused(bad,"timing for a missing source track is rejected even if the recipient omits it");
    bad=input;fixture::Set32(bad,countAt,0xffffffffu);refused(bad,"impossible V2 timing count is rejected before allocating records");
    bad=input;fixture::Set32(bad,entry+13,0);fixture::Set32(bad,entry+17,0x7ff80000u);refused(bad,"nonfinite V2 control is rejected");
    bad=input;fixture::Set32(bad,entry+45+5,1);refused(bad,"duplicate channel/end-frame metadata is rejected");
    bad=input;bad.pop_back();refused(bad,"truncated V2 footer cannot silently fall back to its proxy",false);
    bad=input;bad.resize(bad.size()-40);refused(bad,"MMSN without its footer is rejected",false);
    bad=input;bad.push_back(0);refused(bad,"misplaced V2 footer is rejected",false);
    bad=input;bad[prefix+14]^=1;refused(bad,"invalid V2 CRC is rejected",false);
    for(const auto value:{-1.0,128.0,(std::numeric_limits<double>::infinity)(),(std::numeric_limits<double>::quiet_NaN)()}){
        auto invalid=source;invalid.nativeTiming[0].controls[0]=value;
        auto old=channels;const auto saved=old[0].preservedCameraIntervals;std::vector<std::uint8_t> destination{1,2,3};
        Check(!cm::Encode(invalid,destination,error)&&destination==std::vector<std::uint8_t>({1,2,3}),"invalid canonical timing cannot partially replace an output file buffer");
        Check(!cm::RestoreChannels(invalid,old,error)&&old[0].preservedCameraIntervals==saved&&old[0].track.keys.size()==3,
            "invalid timing preserves recipient author keys and retained metadata");
    }
    auto invalid=source;invalid.nativeTiming.push_back({"missing",1,{20,20,107,107},true});
    std::vector<mikumikustudio::curve_editor::Channel> omitted(1);omitted[0].id="y";
    Check(!cm::RestoreChannels(invalid,omitted,error)&&omitted[0].track.keys.empty(),"unmatched recipient tracks do not bypass source metadata validation");
    invalid=source;invalid.kind=cm::Kind::Light;Check(!cm::RestoreChannels(invalid,omitted,error),"non-camera timing transport is rejected");
    invalid=source;invalid.tracks[0].id=std::string("x\0q",3);
    for(auto& timing:invalid.nativeTiming)timing.id=invalid.tracks[0].id;
    std::vector<std::uint8_t> destination{7};
    Check(!cm::Encode(invalid,destination,error)&&destination==std::vector<std::uint8_t>{7},"embedded-NUL timing ids cannot create an unreadable V2 output");
    Check(!cm::RestoreChannels(invalid,omitted,error)&&omitted[0].track.keys.empty(),"embedded-NUL timing ids reject without partial restore");
    // A realistic long lane exercises the indexed validation, channel capture,
    // and proxy writer. Metadata order is intentionally unrelated to key order.
    // Do not gate on wall time: the implementation must validate each track
    // once and locate intervals by index, independently of machine speed.
    cm::Motion large;large.kind=cm::Kind::Camera;ac::Track many;
    constexpr int count=4096;
    for(int frame=0;frame<count;++frame){ac::Key key;key.frame=frame;key.value=(frame%37)*.25;
        key.tangent=ac::Tangent::Free;key.interpolation=ac::Interpolation::Bezier;many.keys.push_back(key);
        if(frame)large.nativeTiming.push_back({"x",frame,{100.25,10.5,20.75,115.125},frame%3==0});}
    large.tracks={{"x",many}};std::reverse(large.nativeTiming.begin(),large.nativeTiming.end());
    std::vector<std::uint8_t> manyBytes;cm::Motion manyRead;
    Check(cm::Encode(large,manyBytes,error)&&cm::Decode(manyBytes.data(),manyBytes.size(),cm::Kind::Camera,manyRead,error),
        "thousands of unordered timing records encode and decode as one sparse author lane");
    std::vector<mikumikustudio::curve_editor::Channel> manyChannels(1);manyChannels[0].id="x";
    Check(cm::RestoreChannels(manyRead,manyChannels,error)&&cm::CanTransportCameraTiming(manyChannels[0]),
        "large restored timing remains eligible for canonical transport");
    const auto manyCaptured=cm::CaptureChannels(cm::Kind::Camera,manyChannels);
    Check(manyCaptured.tracks.size()==1&&manyCaptured.tracks[0].track.keys.size()==count&&manyCaptured.nativeTiming.size()==count-1,
        "large channel capture keeps every authored key and interval without dense-author substitution");
    Check(cm::Encode(manyCaptured,manyBytes,error)&&cm::Decode(manyBytes.data(),manyBytes.size(),cm::Kind::Camera,manyRead,error),
        "large captured canonical source saves and reopens");
    for(const int first:{0,1,29,1023,2047,4094})for(const double fraction:{0.0,.125,.5,.875}){
        const auto& a=many.keys[first];const auto& b=many.keys[first+1];const bool hold=(first+1)%3==0;
        const double expected=hold?a.value:a.value+(b.value-a.value)*fixture::Ease({100.25,10.5,20.75,115.125},float(fraction));
        const double frame=first+fraction;
        CheckNear(cm::Evaluate(manyRead,"x",frame).value,fraction==0?a.value:expected,0,"large V2 transport preserves both continuous intervals and native cuts");
    }
}
