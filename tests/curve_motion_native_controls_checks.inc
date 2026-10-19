// Included after the independent VMD builders in animation_curve_motion_tests.
// This oracle reproduces the native 12-step operation order, not the scalar
// curve core's high-precision inverse or production timing helpers.
double NativeWireEase(const std::array<double,4>& controls,float time) {
    if(controls[0]==controls[1]&&controls[2]==controls[3])return time;
    const float x1=float(controls[0])/127.f,x2=float(controls[2])/127.f;
    float parameter=.5f,step=.25f;
    for(int i=0;i<12;++i){const float opposite=1-parameter,three=opposite*3;
        const float value=(three*opposite*parameter)*x1+(three*parameter*parameter)*x2+parameter*parameter*parameter;
        if(value==time)break;parameter=value>=time?parameter-step:parameter+step;step*=.5f;}
    const float opposite=1-parameter,three=opposite*3;
    return (three*opposite*parameter)*float(controls[1])/127.f+
        (three*parameter*parameter)*float(controls[3])/127.f+parameter*parameter*parameter;
}
void TestNativeCameraRawRoundTrip(bool legacy,const std::array<double,4>& special) {
    const std::array<std::array<double,4>,6> controls{{special,{0,127,127,0},{127,0,0,127},
        {100,10,20,115},{0,0,127,127},{127,127,0,0}}};
    double lanes[6][4]{};for(unsigned lane=0;lane<6;++lane)for(unsigned axis=0;axis<4;++axis)lanes[lane][axis]=controls[lane][axis];
    VmdFile file;file.legacy=legacy;CameraInput first,last;
    first.frame=3;last.frame=26;first.distance=-45;last.distance=-12;
    first.eye={-2,7,9};last.eye={11,-8,29};first.target={.125,-.25,.375};last.target={-.5,.75,-.875};
    first.fov=30;last.fov=90;last.perspective=1;
    first.block=CameraBlock(lanes);last.block=CameraBlock(lanes);
    file.cameras={last,first}; // Raw record order is deliberately not sorted.
    const auto bytes=Build(file);cm::Motion motion;std::string error;
    Check(cm::Decode(bytes.data(),bytes.size(),cm::Kind::Camera,motion,error),"raw crossed/endpoint camera decodes: "+error);
    Check(bool(motion.nativeCamera),"standard camera import retains an opaque native source");
    std::vector<mikumikustudio::curve_editor::Channel> channels;
    for(const char* id:{"x","y","z","rx","ry","rz","distance","fov","perspective"}){
        mikumikustudio::curve_editor::Channel channel;channel.id=id;channel.nativeControls.push_back({1,2,3,4});
        channel.controlGeometry.push_back({4,3,2,1});channel.preservedCameraIntervals.emplace_back();
        channel.sample=[](const auto&,double){return ac::Sample{-999,0};};channels.push_back(std::move(channel));
    }
    Check(cm::RestoreChannels(motion,channels,error),"raw camera restores detached editor channels: "+error);
    Check(cm::CanCaptureNativeCamera(channels),"all legal crossed and endpoint controls remain native-capturable without sorting X");
    const unsigned mapping[]={0,1,2,3,3,3,4,5};
    for(unsigned channel=0;channel<8;++channel){const auto& restored=channels[channel];
        const auto& track=restored.track;Check(track.keys.size()==2,"native import retains sparse authored keys");if(track.keys.size()!=2)continue;
        const auto& lane=legacy?special:controls[mapping[channel]];
        Check(restored.nativeControls.size()==2&&restored.nativeControls[0]==lane&&restored.nativeControls[1]==lane&&
            restored.preservedCameraIntervals.empty(),"raw controls replace all old channel metadata, including the unused root controls");
        std::string reason;Check(ac::Validate(track,reason),"canvas proxy remains a valid scalar track: "+reason);
        for(double frame:{3.0,3.25,4.0,7.0,14.5,25.0,26.0}){
            const double eased=frame==3?0:frame==26?1:NativeWireEase(lane,float((frame-3)/23));
            const double expected=track.keys.front().value+(track.keys.back().value-track.keys.front().value)*eased;
            const auto actual=cm::Evaluate(motion,restored.id,frame);
            CheckNear(actual.value,expected,0,"ordinary camera Evaluate uses the independent native float inverse and raw Y operation order");
            Check(std::isfinite(actual.derivative),"endpoint/crossed camera speed stays finite");
            Check(restored.sample!=nullptr,"restored native channel has a sampling adapter");
            if(restored.sample)CheckNear(restored.sample(restored,frame).value,expected,0,"window channel timing agrees with ordinary native decode");
        }
    }
    const auto* rotation=Find(motion,"rx");
    if(rotation)CheckNear(rotation->keys.front().value,double(float(first.target[0]))*180.0/3.14159265358979323846,0,
        "camera Euler readout keeps its existing radians-to-degrees convention");
    Check(cm::Evaluate(motion,"fov",3).value==(legacy?45:30)&&cm::Evaluate(motion,"fov",26).value==(legacy?45:90)&&
        cm::Evaluate(motion,"perspective",25.5).value==0&&cm::Evaluate(motion,"perspective",26).value==(legacy?0:1),
        "native FOV and perspective preserve VMD1 defaults and VMD2 discrete switching");
    std::vector<std::uint8_t> encoded;Check(cm::Encode(motion,encoded,error)&&encoded==bytes,
        "ordinary unedited VMD1/2 decode-encode keeps every standard camera byte, version and original record order");
    cm::Motion second;std::vector<std::uint8_t> again;
    Check(cm::Decode(encoded.data(),encoded.size(),cm::Kind::Camera,second,error)&&cm::Encode(second,again,error)&&again==encoded,
        "a second native decode-encode pass remains byte stable without a new wire tail");
    // Existing scalar extension carries only nonstandard binding channels;
    // the standard sparse body remains authoritative for camera timing.
    for(const char* id:{"parentModel","parentBone"}){mikumikustudio::curve_editor::Channel binding;binding.id=id;
        binding.track=MakeTrack({{3,-1},{26,2}},true);channels.push_back(std::move(binding));}
    Check(cm::CanCaptureNativeCamera(channels),"the host's eleven channels keep native eligibility without dropping parent bindings");
    const auto captured=cm::CaptureChannels(cm::Kind::Camera,channels);std::vector<std::uint8_t> bound;
    Check(cm::Encode(captured,bound,error),"native body plus existing binding tail encodes: "+error);
    cm::Motion imported;Check(cm::Decode(bound.data(),bound.size(),cm::Kind::Camera,imported,error)&&imported.nativeCamera&&
        Find(imported,"parentModel")&&Find(imported,"parentBone"),"standard raw timing and nonstandard scalar bindings both survive the existing wire schema");
    BodyCounts counts;std::string parse;
    Check(ParseBody(bound,counts,parse)&&!counts.legacy&&counts.cameras==2&&bound.size()>counts.end&&
        !std::memcmp(bound.data()+bound.size()-40,kFooterMagic,8),"host native export uses a sparse standard VMD2 body and the existing MMVCURV1 footer");
    std::vector<std::uint8_t> boundAgain;Check(cm::Encode(imported,boundAgain,error)&&boundAgain==bound,
        "native body plus scalar binding tail is stable on a second decode-encode pass");
    // Editing any scalar property prevents reuse of stale source camera rows.
    auto edited=motion;for(auto& track:edited.tracks)if(track.id=="x")track.track.keys.back().value+=3;
    std::vector<std::uint8_t> editedBytes;Check(cm::Encode(edited,editedBytes,error)&&editedBytes!=bytes,"edited motion cannot reuse an old raw body");
    cm::Motion editedAgain;Check(cm::Decode(editedBytes.data(),editedBytes.size(),cm::Kind::Camera,editedAgain,error),"edited native motion decodes through its accurately sampled existing extension");
    CheckNear(cm::Evaluate(editedAgain,"x",26).value,cm::Evaluate(edited,"x",26).value,0,"edited native endpoint reaches the exported result");
    CheckNear(cm::Evaluate(editedAgain,"distance",14).value,cm::Evaluate(edited,"distance",14).value,0,
        "editing one lane retains the real native timing of another lane at every baked integer frame");
}
void TestNativeBoneCrossedControls(bool legacy) {
    const double lanes[4][4]={{100,10,20,115},{0,127,127,0},{127,0,0,127},{0,127,127,0}};
    VmdFile file;file.legacy=legacy;BoneInput first,last;first.name=last.name="Accessory";last.frame=23;
    first.position={-2,7,9};last.position={11,-8,29};last.quaternion=QuaternionDegrees(20,30,10);
    first.block=last.block=BoneBlock(lanes);file.bones={last,first};
    const auto bytes=Build(file);cm::Motion motion;std::string error;
    Check(cm::Decode(bytes.data(),bytes.size(),cm::Kind::Accessory,motion,error),"standard bone crossed/endpoint controls import: "+error);
    for(int frame=0;frame<=23;++frame){const double time=double(frame)/23;
        for(unsigned axis=0;axis<3;++axis){const auto* track=Find(motion,axis==0?"x":axis==1?"y":"z");
            const double eased=frame==0?0:frame==23?1:EaseFraction(lanes[axis][0],lanes[axis][1],lanes[axis][2],lanes[axis][3],time);
            if(track)CheckNear(ac::Evaluate(*track,frame).value,first.position[axis]+(last.position[axis]-first.position[axis])*eased,1e-10,
                "bone position samples the original crossed/endpoint easing instead of silently becoming linear");}
        const auto q=EulerDegrees(Slerp(first.quaternion,last.quaternion,frame==0?0:frame==23?1:EaseFraction(0,127,127,0,time)));
        for(unsigned axis=0;axis<3;++axis){const auto* track=Find(motion,axis==0?"rx":axis==1?"ry":"rz");
            if(track)CheckNear(ac::Evaluate(*track,frame).value,q[axis],1e-5,"bone quaternion timing retains legal endpoint control meaning");}
    }
}
void TestNativeIllegalFallback() {
    VmdFile file;CameraInput first,last;last.frame=23;last.eye={23,46,69};first.distance=-45;last.distance=-22;
    const double lanes[6][4]={{200,10,20,115},{100,10,20,115},{0,127,127,0},{20,20,107,107},{20,20,107,107},{20,20,107,107}};
    first.block=last.block=CameraBlock(lanes);file.cameras={first,last};const auto bytes=Build(file);
    cm::Motion motion;std::string error;Check(cm::Decode(bytes.data(),bytes.size(),cm::Kind::Camera,motion,error),"previously accepted out-of-range raw lane keeps its explicit linear fallback");
    CheckNear(cm::Evaluate(motion,"x",7).value,7,1e-12,"illegal raw control above127 does not acquire native nonlinear meaning");
    std::vector<mikumikustudio::curve_editor::Channel> channels;
    for(const auto& track:motion.tracks){mikumikustudio::curve_editor::Channel channel;channel.id=track.id;channels.push_back(std::move(channel));}
    Check(cm::RestoreChannels(motion,channels,error)&&!cm::CanCaptureNativeCamera(channels),"illegal source controls cannot pass native sparse capture eligibility");
    std::vector<std::uint8_t> encoded;Check(cm::Encode(motion,encoded,error)&&encoded!=bytes,"illegal source controls are not written back through the valid-native path");
    cm::Motion restored;Check(cm::Decode(encoded.data(),encoded.size(),cm::Kind::Camera,restored,error),"linear fallback re-export still decodes");
    for(int frame=0;frame<=23;++frame)CheckNear(cm::Evaluate(restored,"x",frame).value,ac::Evaluate(*Find(motion,"x"),frame).value,0,
        "illegal lane retains the exact previous scalar linear export arithmetic");
    // Duplicate source frames keep the last record without changing raw file
    // order during an unmodified standard roundtrip.
    first.block=last.block=CameraBlock(lanes);for(unsigned i=0;i<24;++i)first.block[i]=last.block[i]=20;
    auto duplicate=last;duplicate.eye[0]=17;file.cameras={first,last,duplicate};const auto repeated=Build(file);
    Check(cm::Decode(repeated.data(),repeated.size(),cm::Kind::Camera,motion,error)&&Find(motion,"x")->keys.size()==2&&
        cm::Evaluate(motion,"x",23).value==17&&cm::Encode(motion,encoded,error)&&encoded==repeated,
        "native sorted index resolves duplicate frames by last record and preserves original raw record bytes");
}
void TestUnusedNativeRootBytes(bool legacy) {
    VmdFile file;file.legacy=legacy;CameraInput first,last,ignored;last.frame=23;last.eye[0]=17;
    first.distance=-45;last.distance=-22;
    const double lanes[6][4]={{100,10,20,115},{0,127,127,0},{127,0,0,127},{20,20,107,107},{20,20,107,107},{20,20,107,107}};
    first.block=last.block=CameraBlock(lanes);first.block.fill(250);ignored=last;ignored.block.fill(200);
    file.cameras={first,ignored,last};const auto bytes=Build(file);cm::Motion motion;std::string error;std::vector<std::uint8_t> encoded;
    Check(cm::Decode(bytes.data(),bytes.size(),cm::Kind::Camera,motion,error)&&cm::Encode(motion,encoded,error)&&encoded==bytes,
        "unused root and superseded duplicate control bytes retain their complete 0..255 legacy container without affecting native eligibility");
    std::vector<mikumikustudio::curve_editor::Channel> channels;
    for(const auto& track:motion.tracks){mikumikustudio::curve_editor::Channel channel;channel.id=track.id;channels.push_back(std::move(channel));}
    Check(cm::RestoreChannels(motion,channels,error)&&cm::CanCaptureNativeCamera(channels),"restoring unused root bytes does not downgrade an otherwise legal sparse native motion");
    for(const auto& channel:channels)if(channel.id!="perspective")
        Check(channel.nativeControls[0]==std::array<double,4>{250,250,250,250},"restored native root metadata keeps its unused original byte values");
    wchar_t directory[MAX_PATH]{};Check(GetTempPathW(MAX_PATH,directory)>0,"native camera Unicode file fixture directory");
    const auto path=std::wstring(directory)+L"MMS-原生控制-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".vmd";
    Check(cm::Save(path.c_str(),motion,error),"native sparse motion saves through the real Unicode file entry: "+error);
    cm::Motion loaded;std::vector<std::uint8_t> again;
    Check(cm::Load(path.c_str(),cm::Kind::Camera,loaded,error)&&cm::Encode(loaded,again,error)&&again==bytes,
        "real Unicode Load/Save entry retains native version, duplicate record order and all raw controls");
    Check(DeleteFileW(path.c_str())!=FALSE,"remove only the owned native camera fixture file");
}
void TestNativeRawControlCompatibility() {
    for(bool legacy:{false,true})for(const auto& controls:{std::array<double,4>{100,10,20,115},std::array<double,4>{0,127,127,0}})
        TestNativeCameraRawRoundTrip(legacy,controls);
    TestNativeBoneCrossedControls(false);TestNativeBoneCrossedControls(true);TestNativeIllegalFallback();TestUnusedNativeRootBytes(false);TestUnusedNativeRootBytes(true);
    // Long ordinary imports exercise both the constant/diagonal quick paths
    // and nonlinear handles. Raw lookup and validation must not rescan the
    // whole motion once per key. Timings are evidence, not a machine-speed
    // assertion; all frames/metadata still receive correctness assertions.
    for(bool nonlinear:{false,true}){
        VmdFile file;file.cameras.reserve(10000);
        const double lanes[6][4]={{20,5,110,120},{100,10,20,115},{0,127,127,0},{20,20,107,107},{20,20,107,107},{20,20,107,107}};
        for(unsigned index=0;index<10000;++index){CameraInput row;row.frame=index*2;row.distance=-45;
            row.eye={double(index),7,9};row.fov=45;
            if(nonlinear)row.block=CameraBlock(lanes);
            else for(unsigned lane=0;lane<6;++lane){row.block[lane*4]=20;row.block[lane*4+1]=107;row.block[lane*4+2]=20;row.block[lane*4+3]=107;}
            file.cameras.push_back(row);
        }
        const auto bytes=Build(file);cm::Motion motion;std::string error;const auto start=GetTickCount64();
        Check(cm::Decode(bytes.data(),bytes.size(),cm::Kind::Camera,motion,error),"ten-thousand-key ordinary camera import succeeds");
        std::vector<mikumikustudio::curve_editor::Channel> channels;
        for(const auto& track:motion.tracks){mikumikustudio::curve_editor::Channel channel;channel.id=track.id;channels.push_back(std::move(channel));}
        Check(cm::RestoreChannels(motion,channels,error)&&cm::CanCaptureNativeCamera(channels),"ten-thousand-key metadata restores through one sorted index");
        Check(Find(motion,"x")&&Find(motion,"x")->keys.size()==10000&&cm::Evaluate(motion,"x",19998).value==9999,
            "long native camera retains the complete sparse horizon");
        std::printf("native camera decode+restore 10000 keys (%s): %llu ms\n",nonlinear?"nonlinear":"diagonal/constant",
            static_cast<unsigned long long>(GetTickCount64()-start));
    }
}
