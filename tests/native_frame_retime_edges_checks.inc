// Include after native_frame_line_curve_checks.inc.
namespace {
void FrameRetimeActiveCameraClipChecks(){
    for(const std::array<std::uint8_t,4> controls:{std::array<std::uint8_t,4>{120,5,8,125},std::array<std::uint8_t,4>{0,32,127,110}}){
        GlobalRegistrationFixture f;auto* pool=f.app->CameraKeys();pool[0].next=2;pool[2].previous=0;pool[1]={};pool[1].parentModel=-1;
        pool[0].eye[0]=.125f;pool[2].eye[0]=4.875f;
        for(unsigned control=0;control<4;++control)pool[2].interpolation[control][0]=controls[control];
        auto channels=FLCRead(f,0);auto& unionLane=channels[1];unionLane.nativeControls.clear();unionLane.controlGeometry.clear();unionLane.sample={};
        std::size_t at=0;std::string error;Check(animation_curve::InsertPreservingShape(unionLane.track,1,at,error)==animation_curve::InsertResult::Inserted,
            "another camera lane creates native frame one while the raw-control lane remains sparse");
        std::wstring writeError;Check(curve_host::Write(*f.app,curve_host::Kind::Camera,0,channels,writeError),"write sparse camera authoring with an active raw crossed or endpoint control interval");
        const auto source=FLCRead(f,0);const auto& lane=source[0];
        Check(lane.sample&&lane.track.keys.size()==2&&lane.track.keys.front().frame==0&&lane.track.keys.back().frame==31&&lane.nativeControls.size()==2&&
            lane.controlGeometry[1]==std::array<double,4>{lane.track.keys[0].outWeight,lane.track.keys[0].outSlope,lane.track.keys[1].inWeight,lane.track.keys[1].inSlope},
            "source camera interval keeps NativeGeometry active rather than falling back to its scalar proxy");
        const double rootSample=lane.sample(lane,1).value;const auto before=FLCEncode(source);const auto native=GlobalRegistrationKeys(f.app->CameraKeys());const auto digest=f.Digest();
        std::shared_ptr<plugins::HistoryChange> change;Check(plugins::PlanCameraFrameLine(*f.app,0,false,change)==MMS_OK&&change&&f.Digest()==digest&&FLCEncode(FLCRead(f,0))==before,
            "active camera clipping plans de Casteljau tail controls without changing the live raw interval");FrameLineCommit(f.history,change);
        const auto target=FLCRead(f,0);const auto& clipped=target[0];const auto after=FLCEncode(target);
        Check(clipped.track.keys.size()==2&&clipped.track.keys[0].frame==0&&clipped.track.keys[1].frame==30&&clipped.track.keys[0].value==rootSample&&
            clipped.track.keys[1].value==lane.track.keys.back().value&&clipped.sample&&clipped.nativeControls.size()==2&&
            clipped.controlGeometry[1]==std::array<double,4>{clipped.track.keys[0].outWeight,clipped.track.keys[0].outSlope,clipped.track.keys[1].inWeight,clipped.track.keys[1].inSlope},
            "clipped camera tail retains the player's exact root sample, endpoint and active raw-control sampler");
        if(controls[0]>controls[2])Check(clipped.nativeControls[1][0]>clipped.nativeControls[1][2],"camera tail clipping retains crossed-X raw controls that scalar weights cannot represent");
        for(double frame:{.125,.5,2.25,7.5,13.125,20.75,29.5}){
            const double expected=lane.sample(lane,frame+1).value;const double actual=clipped.sample(clipped,frame).value;
            Check(std::abs(expected-actual)<.012,"camera tail samples preserve the old interval within the native 12-step inverse's finite precision");
        }
        for(unsigned repeat=0;repeat<4;++repeat){f.app->CurrentFrame()=int(9+repeat);
            Check(f.history.History(false)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),native)&&FLCEncode(FLCRead(f,0))==before,
                "active clipped camera undo restores exact original raw-control authoring and native bytes");
            const auto restored=FLCRead(f,0);Check(restored[0].nativeControls==lane.nativeControls&&restored[0].controlGeometry==lane.controlGeometry,
                "camera clip undo restores all raw controls and proxy geometry exactly");
            Check(f.history.History(true)==MMS_OK&&FLCEncode(FLCRead(f,0))==after&&f.app->CurrentFrame()==int(9+repeat),
                "active camera clip redo repeatedly restores its owned tail without seeking");
        }
    }
}
void FrameRetimeMapValidationChecks(){
    for(unsigned family=0;family<2;++family){GlobalRegistrationFixture f;FLCAuthor(f,family);
        curve_host::TrackAuthoring authoring(*f.app,FLCKind(family));std::vector<int> frames;authoring.RetimingFrames(frames);
        const auto before=f.Digest();const auto source=FLCEncode(FLCRead(f,family));std::vector<std::pair<int,int>> valid;
        for(const auto frame:frames)valid.push_back({frame,frame});
        const auto view=family?curve_host::NativeKeyView{f.app->LightKeys(),mdl::KeyPoolCapacity(f.app->LightKeys()),
            [](const void* p,unsigned i)noexcept->const void*{return static_cast<const mdl::LightKey*>(p)+i;}}:
            curve_host::NativeKeyView{f.app->CameraKeys(),mdl::KeyPoolCapacity(f.app->CameraKeys()),
            [](const void* p,unsigned i)noexcept->const void*{return static_cast<const mdl::CameraKey*>(p)+i;}};
        std::vector<std::vector<std::pair<int,int>>> invalid;
        auto changed=valid;changed.push_back(valid.front());invalid.push_back(changed);
        changed=valid;changed.back().second=changed[1].second;invalid.push_back(changed);
        changed=valid;changed.erase(changed.begin()+1);invalid.push_back(changed);
        changed=valid;changed.front().first=-1;invalid.push_back(changed);
        changed=valid;changed.back().second=frame_limits::maximum+1;invalid.push_back(changed);
        changed=valid;changed.back().first=frame_limits::maximum+1;invalid.push_back(changed);
        changed=valid;changed.front().second=-2;invalid.push_back(changed);
        changed=valid;changed.front().second=-1;invalid.push_back(changed);
        changed=valid;std::swap(changed[1].second,changed.back().second);invalid.push_back(changed);
        changed=valid;++changed.back().second;invalid.push_back(changed);
        for(const auto& map:invalid){bool rejected=false;std::unique_ptr<curve_host::TrackAuthoring::Target> target;
            try{target=authoring.PrepareRetimed(view,map,true);}catch(const std::invalid_argument&){rejected=true;}
            Check(rejected&&!target&&f.Digest()==before&&FLCEncode(FLCRead(f,family))==source,
                "duplicate, incomplete, reversed or out-of-range retiming maps reject before changing any native or authored state");}
        auto target=authoring.PrepareRetimed(view,valid,true);
        Check(target&&target->IsCurrent()&&target->Authored()&&f.Digest()==before&&FLCEncode(FLCRead(f,family))==source,
            "a complete retiming map prepares only detached state and does not consume the pristine author lineage");
        failAfter=0;target.reset();failAfter=-1;
        Check(f.Digest()==before&&FLCEncode(FLCRead(f,family))==source,"cancelling a valid mapped target releases prepared storage without live side effects");
    }
}
void NativeFrameRetimeEdgesChecks(){
    std::puts("  frame retime edges: active crossed-X/endpoint camera clipping and exact retained undo");FrameRetimeActiveCameraClipChecks();
    std::puts("  frame retime edges: complete-map validation and detached preparation");FrameRetimeMapValidationChecks();
}
}
