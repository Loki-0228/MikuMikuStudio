namespace light_slider_host_checks {
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()) {
    const auto pose=[&]{return std::array<float,6>{app->LightColor()[0],app->LightColor()[1],app->LightColor()[2],
        app->LightDirection()[0],app->LightDirection()[1],app->LightDirection()[2]};};
    const auto original=pose();const auto light=app->SceneLight();const auto mode=app->CameraMode();const auto frame=app->CurrentFrame();
    auto& model=*mdl::Mdl(app->ModelSlot(0));const auto ring=model.undoState[0];const auto bone=model.boneTable[0].trans[1];
    const std::vector<mdl::LightKey> keys(app->LightKeys(),app->LightKeys()+mdl::KeyPoolCapacity(app->LightKeys()));
    const auto coherent=[&]{
        plugins::Refresh(app);D3DLIGHT9 device{};
        Check(SUCCEEDED(app->Renderer()->device->GetLight(0,&device)),"read device light after live slider publication");
        const auto& cpu=app->SceneLight();
        return cpu.Ambient.r==app->LightColor()[0]&&cpu.Ambient.g==app->LightColor()[1]&&cpu.Ambient.b==app->LightColor()[2]&&
            cpu.Specular.r==app->LightColor()[0]&&cpu.Specular.g==app->LightColor()[1]&&cpu.Specular.b==app->LightColor()[2]&&
            cpu.Direction.x==app->LightDirection()[0]&&cpu.Direction.y==app->LightDirection()[1]&&cpu.Direction.z==app->LightDirection()[2]&&
            !std::memcmp(&device.Ambient,&cpu.Ambient,sizeof(cpu.Ambient))&&!std::memcmp(&device.Specular,&cpu.Specular,sizeof(cpu.Specular))&&
            !std::memcmp(&device.Direction,&cpu.Direction,sizeof(cpu.Direction));
    };
    for(unsigned field=0;field<6;++field) {
        const std::array<float,6> source{.251234f,.351234f,.451234f,.312345f,.412345f,.512345f};
        for(unsigned i=0;i<3;++i){app->LightColor()[i]=source[i];app->LightDirection()[i]=source[i+3];}
        app->ApplyTimelineLightState();plugins::RefreshBoundGlobalReadout(*app);
        const auto slider=GetDlgItem(app->MainWindow(),455+int(field));Check(slider!=nullptr,"actual light slider exists");
        const auto scroll=[&](int tick,unsigned code=TB_THUMBTRACK){SendMessageW(slider,TBM_SETPOS,TRUE,tick);
            SendMessageW(app->MainWindow(),WM_HSCROLL,MAKEWPARAM(code,0),reinterpret_cast<LPARAM>(slider));};
        const int tick=field<3?137:-53,secondTick=field<3?189:71;
        auto target=source;target[field]=field<3?float(double(tick)/256.0):float(double(tick)/100.0);
        auto second=source;second[field]=field<3?float(double(secondTick)/256.0):float(double(secondTick)/100.0);
        Menu(app,L"Edit probe prepared");app->SceneModified()=app->RecordedFrameCountByte1()=0;scroll(tick);
        Check(pose()==source&&!history.UndoCount()&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),
            "pending plugin transaction blocks actual light slider without legacy writes");Menu(app,L"Edit probe cancel");
        Menu(app,L"Edit probe once");SetCapture(slider);Check(GetCapture()==slider,"actual light slider owns capture");scroll(tick);ReleaseCapture();
        Check(value()==1&&pose()==target&&history.UndoCount()==2&&coherent()&&SendMessageW(slider,TBM_GETPOS,0,0)==tick,
            "captured light slider joins plugin history with coherent CPU GPU mirrors and stable tick");
        scroll(secondTick);Check(pose()==second&&history.UndoCount()==3&&coherent(),"next light slider position creates a separate live history node");
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");app->SceneModified()=0;
        scroll(secondTick,TB_ENDTRACK);Check(history.RedoCount()==1&&!app->SceneModified(),"duplicate light end-track retains plugin redo");Menu(app,L"Edit probe redo");
        if(field==5)Check(SaveSceneCopy(app,std::filesystem::absolute(L"light-slider.pmm").c_str()),"save light slider and plugin state in PMM");
        Menu(app,L"Edit probe undo");Menu(app,L"Edit probe undo");Check(value()==1&&pose()==target&&coherent(),"plugin undo reverses latest light slider position");
        Menu(app,L"Edit probe undo");Check(pose()==source&&coherent(),"light slider undo restores exact fractional author values");Menu(app,L"Edit probe undo");
        app->CameraMode()=!mode;app->CurrentFrame()=frame+59;
        for(int i=0;i<4;++i)Menu(app,L"Edit probe redo");
        Check(value()==2&&pose()==second&&coherent()&&app->CurrentFrame()==frame+59&&model.undoState[0]==ring&&model.boneTable[0].trans[1]==bone&&
            !std::memcmp(app->LightKeys(),keys.data(),keys.size()*sizeof(mdl::LightKey)),"light slider redo across editor mode preserves playhead keys bone pose and old ring");
        for(int i=0;i<4;++i)Menu(app,L"Edit probe undo");
        Check(value()==0&&pose()==source&&history.Reset()==MMS_OK,"restore isolated live light slider history");
        app->CameraMode()=mode;app->CurrentFrame()=frame;
    }
    for(unsigned i=0;i<3;++i){app->LightColor()[i]=original[i];app->LightDirection()[i]=original[i+3];}
    app->SceneLight()=light;plugins::RefreshBoundGlobalReadout(*app);
    std::puts("PASS actual six light sliders, own capture, shared history, device light, plugin interleave and PMM save");
}
}
