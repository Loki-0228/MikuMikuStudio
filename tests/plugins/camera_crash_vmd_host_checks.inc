// Explicit local user fixture; never modify or bundle the original motion.
namespace camera_crash_vmd_host_checks {
struct CameraPose {
    std::array<float,8> values;
    std::int32_t parentModel,parentBone;
    std::uint32_t cursor;
    std::uint8_t perspective,active;
    explicit CameraPose(MMDApp& app):values{app.CameraPositionX(),app.CameraPositionY(),app.CameraPositionZ(),
        app.CameraPitch(),app.CameraYaw(),app.CameraRoll(),app.CameraDistance(),app.CameraFov()},
        parentModel(app.CameraParentModel()),parentBone(app.CameraParentBone()),cursor(app.CameraTrackCursor()),
        perspective(app.CameraPerspective()),active(app.CameraTrackActive()){}
    void Restore(MMDApp& app)const {
        for(unsigned axis=0;axis<3;++axis){app.CameraPositionAxis(axis)=values[axis];app.CameraRotation()[axis]=values[axis+3];}
        app.CameraDistance()=values[6];app.CameraFov()=values[7];app.CameraParentModel()=parentModel;
        app.CameraParentBone()=parentBone;app.CameraTrackCursor()=cursor;app.CameraPerspective()=perspective;app.CameraTrackActive()=active;
    }
};
struct FixtureCopy {
    std::wstring path;
    FixtureCopy(const wchar_t* source,unsigned attempt):path(std::filesystem::absolute(
        L"camera-crash-shared-history-"+std::to_wstring(attempt)+L".vmd").native()) {
        const auto original=std::filesystem::absolute(source).lexically_normal().native();
        Check(_wcsicmp(original.c_str(),path.c_str())!=0,"camera crash fixture copy cannot address its original");
        Check(CopyFileW(source,path.c_str(),TRUE)!=FALSE,"copy explicitly supplied camera crash fixture into isolated test directory");
    }
    void Remove(){Check(DeleteFileW(path.c_str())!=FALSE,"remove only the owned camera crash fixture copy before undo and redo");path.clear();}
    ~FixtureCopy(){if(!path.empty())DeleteFileW(path.c_str());}
};
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    using edit_history_host_checks::Menu;
    const auto* source=_wgetenv(L"MMS_CAMERA_CRASH_VMD");
    if(!source||!*source){std::puts("SKIP external camera crash VMD fixture: MMS_CAMERA_CRASH_VMD is not set");return;}
    Check(plugins::BoundNativePoseHistory(app)==&history&&!history.UndoCount(),"camera crash regression uses the actual controlled loader and an empty journal");
    const auto mode=history.Mode();const auto frame=app->CurrentFrame();const auto camera=app->CameraMode();
    const auto slot=app->SelectedModelSlot();const auto last=app->LastRegisteredFrame();const auto modified=app->SceneModified();
    const auto initial=value();const CameraPose original(*app);unsigned attempt=0;
    for(auto recording:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        FixtureCopy copy(source,attempt++);
        Check(history.SetMode(recording)==MMS_OK,"configure camera crash fixture history mode");
        app->CameraMode()=1;app->CurrentFrame()=0;app->SelectedModelSlot()=0;
        const global_paste_host_checks::Keys before(*app);const auto beforeLast=app->LastRegisteredFrame();
        Menu(app,L"Edit probe once");LoadVmdFile(copy.path.c_str());
        Check(history.UndoCount()==2&&value()==initial+1&&app->CurrentFrame()==0,
            "external camera motion is one explicit common-history node in both history modes");
        const global_paste_host_checks::Keys after(*app);const CameraPose imported(*app);
        const auto keyAt=[&](unsigned frame)->const mdl::CameraKey&{
            const auto* keys=app->CameraKeys();const auto capacity=mdl::KeyPoolCapacity(keys);unsigned at=0;
            for(std::size_t visits=0;visits<capacity;++visits){
                Check(at<capacity,"external camera key chain remains within its owned pool");
                if(keys[at].frame==frame)return keys[at];
                Check(keys[at].next!=0&&keys[at].frame<frame,"external camera motion retains the requested key");at=keys[at].next;
            }
            Check(false,"external camera key chain terminates");return keys[0];
        };
        Check(keyAt(44).eye[0]==0.f&&keyAt(92).frame==92&&keyAt(93).frame==93,
            "controlled import retains exact frame-44 zero and adjacent camera cut keys");
        app->CurrentFrame()=44;ReloadModels(app);
        Check(app->CameraPositionX()==0.f,"seeking to camera frame 44 returns exact zero without approximate interpolation");
        app->CurrentFrame()=92;ReloadModels(app);const CameraPose at92(*app);
        app->CurrentFrame()=93;ReloadModels(app);const CameraPose at93(*app);
        Check(at92.values[0]==keyAt(92).eye[0]&&at93.values[0]==keyAt(93).eye[0]&&at92.values!=at93.values,
            "native camera evaluation retains frame 92 until the distinct frame-93 cut");
        std::vector<curve_editor::Channel> channels;std::wstring error;
        Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error)&&channels.size()>=8,
            "curve view reads the imported external camera motion including crossed-X controls");
        for(const double sampleFrame:{92.,92.25,92.5,92.75}){
            const auto& channel=channels[0];const auto result=channel.sample?channel.sample(channel,sampleFrame):animation_curve::Evaluate(channel.track,sampleFrame);
            Check(result.value==double(keyAt(92).eye[0]),"fractional camera curve samples hold frame 92 before the one-frame cut");
        }
        Check(history.UndoCount()==2&&app->CurrentFrame()==93,"camera sampling creates no history records");
        imported.Restore(*app);app->CurrentFrame()=0;
        copy.Remove();Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");
        Check(before.Equals(*app)&&app->LastRegisteredFrame()==beforeLast&&value()==initial+1,
            "external camera import undo restores every key selection and last-frame metadata after source deletion");
        app->CameraMode()=0;app->SelectedModelSlot()=1;app->CurrentFrame()=37;Menu(app,L"Edit probe redo");
        Check(after.Equals(*app)&&app->CurrentFrame()==37,"external camera redo uses retained data across model mode and playhead changes");
        Menu(app,L"Edit probe redo");for(unsigned i=0;i<3;++i)Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"external camera fixture restores plugin value and clears common history");history.Collect();
    }
    original.Restore(*app);app->CurrentFrame()=frame;app->CameraMode()=camera;app->SelectedModelSlot()=slot;
    app->LastRegisteredFrame()=last;app->SceneModified()=modified;
    Check(history.SetMode(mode)==MMS_OK,"restore history mode after external camera regression");
    ApplyFrameCameraProjection(app);
    std::puts("PASS external camera crash VMD through controlled import, exact key evaluation, one-frame cut, deleted-source undo/redo and plugin interleaving");
}
}
