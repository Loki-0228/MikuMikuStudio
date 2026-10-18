// Include after numpad_view_host_checks.inc inside edit_history_host_checks.
// The aggregate needs animation_curve_motion.hpp and channel_bake.hpp globally.
// Uses the real curve editor's Write motion button and controlled camera bake.
namespace native_camera_sampling_host_checks {
namespace ac=animation_curve;
std::wstring destination;
unsigned pickerCalls=0;
BOOL SavePicker(LPOPENFILENAMEW dialog){
    Check(dialog&&dialog->lpstrDefExt&&!std::wcscmp(dialog->lpstrDefExt,L"vmd"),"actual camera motion export requests a VMD destination");
    Check(destination.size()<dialog->nMaxFile,"real camera export fixture fits the native dialog buffer");
    wcscpy_s(dialog->lpstrFile,dialog->nMaxFile,destination.c_str());++pickerCalls;return TRUE;
}
std::uint32_t U32(const std::vector<std::uint8_t>& bytes,std::size_t offset){
    Check(offset+4<=bytes.size(),"actual camera export contains the standard field");
    return std::uint32_t(bytes[offset])|std::uint32_t(bytes[offset+1])<<8|std::uint32_t(bytes[offset+2])<<16|std::uint32_t(bytes[offset+3])<<24;
}
double Sample(const curve_editor::Channel& channel,double frame){return channel.sample?channel.sample(channel,frame).value:ac::Evaluate(channel.track,frame).value;}
std::vector<curve_editor::Channel> Read(MMDApp& app){
    std::vector<curve_editor::Channel> result;std::wstring error;
    Check(curve_host::Read(app,curve_host::Kind::Camera,0,result,error),"read real host camera source with its production sampler");return result;
}
HWND Editor(MMDApp& app){
    struct Find{HWND owner=nullptr,result=nullptr;}find{app.MainWindow()};
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM context)->BOOL{
        auto& find=*reinterpret_cast<Find*>(context);wchar_t name[96]{};GetClassNameW(window,name,96);
        if(GetWindow(window,GW_OWNER)==find.owner&&!std::wcscmp(name,L"MikuDanceStudioCurveEditorWindow"))find.result=window;
        return TRUE;
    },reinterpret_cast<LPARAM>(&find));return find.result;
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*pluginValue)()){
    Check(app&&!history.UndoCount()&&!history.RedoCount(),"real camera sampling fixture starts with an empty common history");
    const auto initialPlugin=pluginValue();
    D3DMATRIX savedProjection{};auto* device=app->Renderer()?app->Renderer()->device:nullptr;
    const bool haveProjection=device&&SUCCEEDED(device->GetTransform(D3DTS_PROJECTION,&savedProjection));
    {
        numpad_view_host_checks::Original original(*app,history);
        numpad_view_host_checks::Build(*app,0,false);
        // Ordinary native data, with no prior Free edit or shape-preserving
        // insertion. Preserve non-default root bytes as well as later lanes.
        for(unsigned row=0;row<3;++row)for(unsigned lane=0;lane<6;++lane){auto& key=app->CameraKeys()[row];
            key.interpolation[0][lane]=100;key.interpolation[1][lane]=10;
            key.interpolation[2][lane]=20;key.interpolation[3][lane]=115;
            key.target[0]=row==0?0.f:row==1?12.566371f:-6.2831855f;
        }
        const auto source=Read(*app);
        Check(!curve_host::Has(app,curve_host::Kind::Camera),"actual export begins with ordinary native camera timing");
        Check(curve_motion::CanCaptureNativeCamera(source),"ordinary real-host fixture has complete native-ready camera channels before opening the editor");
        const auto raw=std::vector<mdl::CameraKey>(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
        const auto frame=app->CurrentFrame();const auto dirty=app->SceneModified();
        const auto output=std::filesystem::temp_directory_path()/(L"MMS-native-camera-host-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".vmd");
        destination=output.native();pickerCalls=0;
        Hook(&GetUiSaveFileNameW,&SavePicker);
        curve_host::Open(app,false);const auto editor=Editor(*app);
        Check(editor&&GetDlgItem(editor,1004),"real host opens the scene curve editor's Write motion button");
        // OpenScene can initially choose the selected accessory. This test
        // explicitly chooses the camera source, just as the user would.
        const auto sources=GetDlgItem(editor,1002);
        Check(sources&&SendMessageW(sources,CB_SETCURSEL,0,0)==0,"choose the actual camera source before exporting its native motion");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1002,CBN_SELCHANGE),reinterpret_cast<LPARAM>(sources));
        Check(SendMessageW(sources,CB_GETCURSEL,0,0)==0,"actual native camera export remains on the explicitly selected camera source");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1004,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(editor,1004)));
        MH_DisableHook(reinterpret_cast<void*>(&GetUiSaveFileNameW));MH_RemoveHook(reinterpret_cast<void*>(&GetUiSaveFileNameW));
        curve_editor::CloseOwned(app->MainWindow());
        Check(pickerCalls==1&&std::filesystem::exists(output),"actual editor callback writes one VMD through the production atomic saver");
        std::ifstream file(output,std::ios::binary);const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
        Check(U32(bytes,50)==0&&U32(bytes,54)==0&&U32(bytes,58)==3,"actual native camera export keeps three sparse standard records");
        for(unsigned row=0;row<3;++row)for(unsigned lane=0;lane<6;++lane)for(unsigned component=0;component<4;++component){
            const unsigned order[]={0,2,1,3};
            Check(bytes[62+61*row+32+4*lane+component]==raw[row].interpolation[order[component]][lane],
                "actual curve-editor export preserves every standard timing byte including root controls");
        }
        Check(!std::memcmp(raw.data(),app->CameraKeys(),raw.size()*sizeof(raw[0]))&&
            app->CurrentFrame()==frame&&app->SceneModified()==dirty&&!history.UndoCount()&&pluginValue()==initialPlugin,
            "real native export leaves camera pools current frame dirty flag and plugin/common history unchanged");

        channel_bake::Request request;request.scope=channel_bake::Scope::Scene;
        request.firstFrame=7;request.lastFrame=19;request.step=2;
        auto& choice=request.selection.Touch(channel_bake::Kind::Camera,0,11);choice.fields.assign(11,false);choice.fields[0]=true;
        std::wstring error;channel_bake::Plan plan;
        Check(channel_bake::Prepare(*app,request,plan,error)==channel_bake::Error::None,
            "real initialized host prepares a native partial bake without an editor proxy");
        Check(!std::memcmp(raw.data(),app->CameraKeys(),raw.size()*sizeof(raw[0]))&&!history.UndoCount(),
            "real host bake preparation keeps native camera bytes and history private");
        Check(channel_bake::Commit(*app,plan,error)==channel_bake::Error::None&&history.UndoCount()==1,
            "real host camera bake writes through the controlled curve bridge as one explicit record");
        const auto baked=Read(*app);
        for(int tick=0;tick<=124;++tick){const double at=tick*.25;
            if(at<7||at>19)Check(Sample(baked[0],at)==Sample(source[0],at),"real partial bake preserves native timing outside its range");
            for(unsigned lane=1;lane<8;++lane)Check(Sample(baked[lane],at)==Sample(source[lane],at),
                "real partial bake keeps all unselected camera peers despite native shared frame rows");
        }
        for(int at=7;at<=19;at+=2)Check(Sample(baked[0],at)==Sample(source[0],at),"real host bake key matches the canonical native sample");
        Menu(app,L"Edit probe undo");const auto undone=Read(*app);
        Check(history.UndoCount()==0&&history.RedoCount()==1&&app->CurrentFrame()==frame,
            "actual plugin undo restores the camera bake without moving the main playhead");
        for(int at=0;at<=31;++at)Check(Sample(undone[0],at)==Sample(source[0],at),"actual plugin undo restores the original native camera timing");
        Menu(app,L"Edit probe redo");const auto redone=Read(*app);
        for(int at=0;at<=31;++at)Check(Sample(redone[0],at)==Sample(baked[0],at),"actual plugin redo restores the sampled and exact-outside camera data");
        Check(pluginValue()==initialPlugin,"native camera export and bake do not modify the plugin participant");
    }
    if(haveProjection)Check(SUCCEEDED(device->SetTransform(D3DTS_PROJECTION,&savedProjection)),"restore actual device projection after the isolated camera fixture");
    plugins::RefreshBoundPoseReadout(*app);plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual host native camera export, partial baking and shared undo/redo");
}
}
