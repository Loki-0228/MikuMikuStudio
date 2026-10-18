// Requires the global ConsumeNumpadViewPresets declaration at the host entry.
namespace numpad_view_host_checks {
struct View {
    std::array<float,26> f{};std::uint8_t suppressed=0,perspective=0;int parent=-1,bone=0;
};
View Read(MMDApp& app){View value;
    for(unsigned axis=0;axis<3;++axis){value.f[axis]=app.CameraPositionAxis(axis);value.f[axis+3]=app.CameraRotation()[axis];}
    value.f[6]=app.CameraDistance();value.f[7]=app.CameraFov();value.f[8]=app.ViewOffsetX();value.f[9]=app.ViewOffsetY();
    std::memcpy(value.f.data()+10,&app.CameraAttachmentBasis(),sizeof(float)*16);
    value.suppressed=app.CameraAttachmentTransformSuppressed();value.perspective=app.CameraPerspective();value.parent=app.CameraParentModel();value.bone=app.CameraParentBone();return value;}
bool Same(const View& a,const View& b){return !std::memcmp(a.f.data(),b.f.data(),sizeof(a.f))&&a.suppressed==b.suppressed&&a.perspective==b.perspective&&a.parent==b.parent&&a.bone==b.bone;}
void Write(MMDApp& app,const View& value){for(unsigned axis=0;axis<3;++axis){app.CameraPositionAxis(axis)=value.f[axis];app.CameraRotation()[axis]=value.f[axis+3];}
    app.CameraDistance()=value.f[6];app.CameraFov()=value.f[7];app.ViewOffsetX()=value.f[8];app.ViewOffsetY()=value.f[9];
    std::memcpy(&app.CameraAttachmentBasis(),value.f.data()+10,sizeof(float)*16);app.CameraAttachmentTransformSuppressed()=value.suppressed;app.CameraPerspective()=value.perspective;
    app.CameraParentModel()=value.parent;app.CameraParentBone()=value.bone;}
float Bits(std::uint32_t bits){float value;std::memcpy(&value,&bits,sizeof(value));return value;}
void Keys(MMDApp& app,unsigned mask,unsigned state=1){std::memset(app.state.numpadKeyState,0,sizeof(app.state.numpadKeyState));
    for(unsigned key=0;key<10;++key)if(mask&(1u<<key))app.state.numpadKeyState[key]=state;}
struct Original {
    MMDApp& app;plugins::SharedHistory& history;View view;mdl::CameraKey* pool;const std::uint64_t identity;
    curve_host::AuthoringTable authoring,verification;std::vector<std::uint8_t> keys;HWND focus,floating;
    const int frame,last,cursor,active,modified,recorded,messageSeen,physicsReset;
    const std::uint8_t camera,slot,follow,reference,input,playback,step,pending,mutation;
    const bool projection;const plugins::SharedHistory::RecordingMode mode;const std::size_t maxEntries;const std::uint64_t maxBytes;
    Original(MMDApp& a,plugins::SharedHistory& h):app(a),history(h),view(Read(a)),pool(a.CameraKeys()),identity(mdl::KeyPoolIdentity(pool)),focus(GetFocus()),floating(a.FloatingWindow()),
        frame(a.CurrentFrame()),last(a.LastRegisteredFrame()),cursor(a.CameraTrackCursor()),active(a.CameraTrackActive()),modified(a.SceneModified()),recorded(a.RecordedFrameCountByte1()),
        messageSeen(a.MessageSeen()),physicsReset(a.PhysicsResetPending()),camera(a.CameraMode()),slot(a.SelectedModelSlot()),follow(a.state.followCameraEnabled),
        reference(static_cast<std::uint8_t>(a.CameraReferenceMode())),input(a.ViewportInputActive()),playback(a.PlaybackActive()),step(a.FrameStepPlayback()),
        pending(a.state.recordPlaybackStartPending),mutation(a.SceneMutationInProgress()),projection(a.cameraProjectionRefreshPending),mode(h.Mode()),maxEntries(h.MaxEntries()),maxBytes(h.MaxBytes()){
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(a.state.numpadKeyState);keys.assign(bytes,bytes+sizeof(a.state.numpadKeyState));
        std::wstring error;Check(curve_host::CaptureAuthoring(app,verification,error),"capture real authoring before isolated numpad view fixture");curve_host::SwapAuthoring(app,authoring);
        app.CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(8);app.FloatingWindow()=nullptr;app.PlaybackActive()=app.FrameStepPlayback()=0;app.state.recordPlaybackStartPending=0;
    }
    ~Original(){Check(history.Reset()==MMS_OK,"release real preset history before restoring original camera pool");history.Collect();
        curve_host::AuthoringTable discarded;curve_host::SwapAuthoring(app,discarded);auto* temporary=app.CameraKeys();app.CameraKeys()=pool;mdl::KeyPoolDestroy(temporary);curve_host::SwapAuthoring(app,authoring);
        Write(app,view);app.CurrentFrame()=frame;app.LastRegisteredFrame()=last;app.CameraTrackCursor()=cursor;app.CameraTrackActive()=active;app.SceneModified()=modified;
        app.RecordedFrameCountByte1()=recorded;app.MessageSeen()=messageSeen;app.PhysicsResetPending()=physicsReset;
        app.CameraMode()=camera;app.SelectedModelSlot()=slot;app.state.followCameraEnabled=follow;app.CameraReferenceMode()=static_cast<CameraAttachmentReference>(reference);
        app.ViewportInputActive()=input;app.PlaybackActive()=playback;app.FrameStepPlayback()=step;app.state.recordPlaybackStartPending=pending;
        app.cameraProjectionRefreshPending=projection;app.FloatingWindow()=floating;std::memcpy(app.state.numpadKeyState,keys.data(),keys.size());
        if(focus&&IsWindow(focus))SetFocus(focus);else SetFocus(nullptr);
        Check(history.Configure(mode,maxEntries,maxBytes)==MMS_OK&&mdl::KeyPoolIdentity(pool)==identity&&curve_host::AuthoringUnchanged(app,verification)&&app.SceneMutationInProgress()==mutation,
            "real numpad fixture restores complete view, original pool, authoring, input states and history configuration");
    }
};
void Branch(MMDApp& app,unsigned branch){app.CameraMode()=branch==0;app.state.followCameraEnabled=branch==1;app.SelectedModelSlot()=0;
    app.CameraParentModel()=branch?0:-1;app.CameraParentBone()=0;app.CameraReferenceMode()=CameraAttachmentReference::SelectedBone;app.ViewportInputActive()=1;SetFocus(app.MainWindow());}
void Build(MMDApp& app,unsigned branch,bool authored){curve_host::AuthoringTable old;curve_host::SwapAuthoring(app,old);
    std::memset(app.CameraKeys(),0,mdl::KeyPoolCapacity(app.CameraKeys())*sizeof(mdl::CameraKey));
    for(unsigned index=0;index<3;++index){auto& key=app.CameraKeys()[index];key.frame=index==0?0:index==1?12:31;key.previous=index?index-1:0;key.next=index<2?index+1:0;
        key.parentModel=-1;key.distance=-33.f-float(index)*7;key.fov=37+int(index)*13;key.perspective=std::uint8_t(index%2);
        for(unsigned axis=0;axis<3;++axis){key.eye[axis]=float(index*3+axis)*.317f;key.target[axis]=float(index+axis)*.1234567f;}
        std::memset(key.interpolation,20,12);std::memset(&key.interpolation[2][0],107,12);key.interpolation[0][0]=3;key.interpolation[1][0]=81;key.interpolation[2][0]=91;key.interpolation[3][0]=119;}
    if(authored){std::vector<curve_editor::Channel> channels;std::wstring error;Check(curve_host::Read(app,curve_host::Kind::Camera,0,channels,error),"read real camera source for model-zero authoring");
        auto& lane=channels[0];lane.track.keys.erase(lane.track.keys.begin()+1);lane.nativeControls.clear();lane.controlGeometry.clear();lane.sample={};
        lane.track.keys.front().value=.123456789012;lane.track.keys.back().value=7.987654321098;
        for(auto& key:lane.track.keys){key.tangent=animation_curve::Tangent::Free;key.interpolation=animation_curve::Interpolation::Bezier;key.inWeight=.31;key.outWeight=.19;key.inSlope=-.017;key.outSlope=.013;}
        channels[1].track.keys.back().interpolation=animation_curve::Interpolation::Step;channels[1].nativeControls.clear();channels[1].controlGeometry.clear();channels[1].sample={};
        Check(curve_host::Write(app,curve_host::Kind::Camera,0,channels,error),"author real sparse Free/Step camera source for numpad zero");}
    Branch(app,branch);app.CurrentFrame()=7;app.CameraPositionX()=3.125f;app.CameraPositionY()=-2.25f;app.CameraPositionZ()=6.5f;
    app.CameraPitch()=.23f;app.CameraYaw()=-.57f;app.CameraRoll()=.13f;app.CameraDistance()=-71.25f;app.CameraFov()=45;app.ViewOffsetX()=2.75f;app.ViewOffsetY()=-3.5f;
    app.CameraAttachmentTransformSuppressed()=1;for(unsigned index=0;index<16;++index)app.CameraAttachmentBasis().m[index/4][index%4]=float(index+1)*.125f;
    app.SceneModified()=app.RecordedFrameCountByte1()=0;app.cameraProjectionRefreshPending=false;Keys(app,0);
}
View Expected(MMDApp& app,unsigned mask){auto view=Read(app);const bool cameraLike=app.CameraMode()!=0||app.state.followCameraEnabled!=0;
    const float half=Bits(0x3fc90ff9),pi=Bits(0x40490fd8);struct Preset{unsigned key;float pitch,yaw;bool target;};
    const Preset presets[]={{2,0,0,true},{4,0,half,true},{6,0,-half,true},{8,0,pi,true},{5,-half,0,false},{0,half,0,false},{1,half,0,false}};
    std::unique_ptr<MMDApp> oracle;
    if(!cameraLike&&(mask&1u)){oracle=std::make_unique<MMDApp>();::new(static_cast<void*>(&oracle->state))MMDAppState{};
        oracle->CurrentFrame()=app.CurrentFrame();oracle->CameraMode()=0;oracle->state.followCameraEnabled=0;
        for(unsigned slot=0;slot<kModelSlotCount;++slot)oracle->ModelSlot(slot)=app.ModelSlot(slot);
        oracle->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(mdl::KeyPoolCapacity(app.CameraKeys()));
        std::memcpy(oracle->CameraKeys(),app.CameraKeys(),mdl::KeyPoolCapacity(app.CameraKeys())*sizeof(mdl::CameraKey));
        pmm_extension::Document authoring;curve_host::Capture(app,authoring);curve_host::Load(*oracle,authoring);}
    for(const auto& preset:presets)if(mask&(1u<<preset.key)){
        if(!cameraLike&&preset.key==0){Write(*oracle,view);oracle->ViewOffsetX()=oracle->ViewOffsetY()=0;oracle->CameraAttachmentTransformSuppressed()=0;
            ReloadModels(oracle.get());PostModelReload(oracle.get());view=Read(*oracle);continue;}
        view.f[3]=preset.pitch;view.f[4]=preset.yaw;view.f[5]=0;view.f[6]=!cameraLike&&preset.key==1?-30.f:-45.f;
        if(!cameraLike){view.f[8]=0;view.f[9]=preset.target&&app.CameraReferenceMode()!=CameraAttachmentReference::SelectedBone?10.f:0.f;
            for(unsigned i=0;i<16;++i)view.f[10+i]=i%5==0?1.f:0.f;}
    }
    if(oracle){curve_host::Reset(oracle.get());mdl::KeyPoolDestroy(oracle->CameraKeys());for(unsigned slot=0;slot<kModelSlotCount;++slot)oracle->ModelSlot(slot)=nullptr;}
    return view;
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;using Mode=plugins::SharedHistory::RecordingMode;
    Check(app&&app->ModelSlot(0)&&!history.UndoCount()&&!history.RedoCount(),"real numpad fixture starts with a loaded model and empty common history");
    Original original(*app,history);const auto initial=value();
    for(unsigned branch=0;branch<3;++branch)for(unsigned mask:{1u<<4,1u,1u|(1u<<1)})for(bool authored:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        if(authored&&(branch!=2||!(mask&1u)))continue;Build(*app,branch,authored);
        // The bound curve writer records the authored preparation itself.
        // Keep that data while isolating the numpad command's journal nodes.
        Check(history.Reset()==MMS_OK,"clear only completed numpad camera authoring setup history");history.Collect();
        Check(!history.UndoCount()&&!history.RedoCount(),"real numpad command fixture begins after setup with an empty common journal");
        Check(history.SetMode(mode)==MMS_OK,"configure real numpad adjustment history mode");
        const auto before=Read(*app),expected=Expected(*app,mask);const auto native=std::vector<mdl::CameraKey>(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
        const auto& model=*mdl::Mdl(app->ModelSlot(0));const auto bone=model.boneTable[0];const std::array<std::uint32_t,2> rings{model.undoState[0],model.undoState[1]};
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe prepared");Keys(*app,mask);ConsumeNumpadViewPresets(app);
        Check(Same(Read(*app),before)&&history.UndoCount()==1,"real pending plugin transaction rejects the numpad pump without legacy fallback");
        Menu(app,L"Edit probe cancel");ConsumeNumpadViewPresets(app);const auto after=Read(*app);
        Check(Same(after,expected)&&app->CurrentFrame()==7&&history.UndoCount()==(mode==Mode::AllOperations?2:1)&&value()==initial+1&&
            !std::memcmp(app->CameraKeys(),native.data(),native.size()*sizeof(native[0]))&&!std::memcmp(&model.boneTable[0],&bone,sizeof(bone))&&
            model.undoState[0]==rings[0]&&model.undoState[1]==rings[1]&&app->cameraProjectionRefreshPending,
            "actual camera/follow/model numpad pump matches original constants or old ReloadModels and updates only view state as one adjustment");
        Keys(*app,mask,3);ConsumeNumpadViewPresets(app);Check(Same(Read(*app),after)&&history.UndoCount()==(mode==Mode::AllOperations?2:1),"real held numpad key does not repeat");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Check(value()==initial+1&&Same(Read(*app),after),"later plugin edit is separately undoable after the preset");
        app->CurrentFrame()=53;app->CameraMode()=branch==0?0:1;app->state.followCameraEnabled=0;
        if(mode==Mode::AllOperations){Menu(app,L"Edit probe undo");Check(Same(Read(*app),before)&&app->CurrentFrame()==53&&value()==initial+1,"real preset undo crosses mode and frame while preserving the preceding plugin edit");}
        Menu(app,L"Edit probe undo");Check(value()==initial,"earlier plugin edit remains independently undoable");
        Menu(app,L"Edit probe redo");if(mode==Mode::AllOperations)Menu(app,L"Edit probe redo");
        Check(Same(Read(*app),after)&&app->CurrentFrame()==53&&value()==initial+1,"real preset redo restores frozen native/free values after preceding plugin redo");
        Menu(app,L"Edit probe redo");Check(value()==initial+2,"later plugin edit remains independently redoable");
        const unsigned nodes=mode==Mode::AllOperations?3:2;for(unsigned i=0;i<nodes;++i)Menu(app,L"Edit probe undo");
        Branch(*app,branch);app->CurrentFrame()=7;const auto noop=Expected(*app,1u<<4);Write(*app,noop);app->SceneModified()=app->RecordedFrameCountByte1()=0;app->cameraProjectionRefreshPending=false;
        Keys(*app,1u<<4);ConsumeNumpadViewPresets(app);
        Check(Same(Read(*app),noop)&&!history.UndoCount()&&history.RedoCount()==nodes&&!app->SceneModified()&&!app->RecordedFrameCountByte1()&&!app->cameraProjectionRefreshPending,
            "real no-op preset preserves redo, scene flags and deferred projection state");
        app->ViewportInputActive()=0;Keys(*app,1u<<6);ConsumeNumpadViewPresets(app);app->ViewportInputActive()=1;
        app->PlaybackActive()=1;ConsumeNumpadViewPresets(app);app->PlaybackActive()=0;
        HWND edit=GetDlgItem(app->MainWindow(),0x22a);Check(edit!=nullptr,"real numpad edit-focus control exists");SetFocus(edit);ConsumeNumpadViewPresets(app);SetFocus(app->MainWindow());
        Check(Same(Read(*app),noop)&&history.RedoCount()==nodes,"real inactive, playback and panel text focus suppress numpad consumption");
        Check(history.Reset()==MMS_OK&&value()==initial,"real numpad case restores the plugin baseline");history.Collect();
    }
    std::puts("PASS actual numpad pump, camera/follow/model constants, model-zero native/free reset, input gates and plugin-interleaved adjustment history");
}
}
