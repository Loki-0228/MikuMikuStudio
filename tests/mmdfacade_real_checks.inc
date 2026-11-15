namespace mmdfacade_real_checks {
using namespace mikumikustudio;
unsigned dialogs = 0;
LRESULT CALLBACK RejectDialog(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE && GetDlgItem(reinterpret_cast<HWND>(wp),IDOK)) {
        ++dialogs;
        wchar_t message[1024]{}; GetDlgItemTextW(reinterpret_cast<HWND>(wp),0xffff,message,1024);
        std::printf("Unexpected plugin dialog: %ls\n",message);
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
std::vector<uint32_t> Pixels(MMDApp* app, const wchar_t* output) {
    auto* device = app->Renderer()->device;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> source, resolved, cpu;
    Check(SUCCEEDED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&source)), "Effekseer backbuffer");
    D3DSURFACE_DESC desc{}; source->GetDesc(&desc);
    Check(SUCCEEDED(device->CreateRenderTarget(desc.Width,desc.Height,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr)), "Effekseer resolve target");
    Check(SUCCEEDED(device->StretchRect(source.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE)), "Effekseer resolve image");
    Check(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,nullptr)), "Effekseer CPU surface");
    Check(SUCCEEDED(device->GetRenderTargetData(resolved.Get(),cpu.Get())), "Effekseer read pixels");
    D3DLOCKED_RECT lock{}; Check(SUCCEEDED(cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY)), "Effekseer lock pixels");
    std::vector<uint32_t> result(size_t(desc.Width)*desc.Height);
    for (UINT y=0;y<desc.Height;++y)
        std::memcpy(result.data()+size_t(y)*desc.Width,static_cast<const char*>(lock.pBits)+y*lock.Pitch,desc.Width*4);
    cpu->UnlockRect();
    std::ofstream image(std::filesystem::path(output),std::ios::binary);
    image << "P6\n" << desc.Width << ' ' << desc.Height << "\n255\n";
    for (auto pixel:result) { const char rgb[]{char(pixel>>16),char(pixel>>8),char(pixel)}; image.write(rgb,3); }
    return result;
}
size_t Difference(const std::vector<uint32_t>& a,const std::vector<uint32_t>& b) {
    Check(a.size()==b.size(),"consistent render dimensions"); size_t count=0;
    for(size_t i=0;i<a.size();++i) if((a[i]&0xffffff)!=(b[i]&0xffffff)) ++count;
    return count;
}
void Run(MMDApp* app,const wchar_t* effect) {
    const auto dialogHook=SetWindowsHookExW(WH_CBT,RejectDialog,nullptr,GetCurrentThreadId());
    Check(GetModuleHandleW(L"MMSFirstPersonCamera.dll") && GetModuleHandleW(L"EffekseerForMMD.dll"),"both real plugins started");
    Check(plugins::LegacyMmeStatus().loaded || GetEnvironmentVariableW(L"MMDFACADE_TEST_NO_MME",nullptr,0),"MME coexistence control");
    if(GetEnvironmentVariableW(L"MMDFACADE_TEST_MME_OFF",nullptr,0))
        SendMessageW(app->MainWindow(),WM_COMMAND,40006,0);
    app->CameraMode()=1; app->CameraPitch()=app->CameraYaw()=app->CameraRoll()=0;
    app->CameraPositionX()=app->CameraPositionY()=app->CameraPositionZ()=0;
    SetFocus(app->MainWindow());
    SendMessageW(app->MainWindow(),WM_KEYDOWN,'W',1);
    Check(std::abs(app->CameraPositionZ()-1)<.0001f,"real window routes camera W");
    SendMessageW(app->MainWindow(),WM_KEYUP,'W',LPARAM(1)<<31);
    Check(std::abs(app->CameraPositionZ()-1)<.0001f,"real window ignores key release");
    { FrameSceneOutputScope viewport(app); Check(viewport.Ready(),"hidden camera viewport scope");
    const auto rect=app->ViewportRect(); const POINT p{(rect.left+rect.right)/2,(rect.top+rect.bottom)/2};
    SendMessageW(app->MainWindow(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(p.x,p.y));
    std::printf("camera viewport=%ld,%ld,%ld,%ld capture=%d\n",rect.left,rect.top,rect.right,rect.bottom,GetCapture()==app->MainWindow());
    SendMessageW(app->MainWindow(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(p.x+50,p.y+25));
    std::printf("camera mouse yaw=%g pitch=%g capture=%d\n",app->CameraYaw(),app->CameraPitch(),GetCapture()==app->MainWindow());
    Check(std::abs(app->CameraYaw()+.1f)<.0001f && std::abs(app->CameraPitch()+.05f)<.0001f,"real window routes camera mouse look");
    SendMessageW(app->MainWindow(),WM_CANCELMODE,0,0);
    Check(!GetCapture(),"real window releases camera capture");
    }
    std::puts("PASS real window camera keyboard and mouse routing");
    LoadModelFile(app,effect,false);
    auto* raw=static_cast<unsigned char*>(app->ModelSlot(0));
    Check(raw!=nullptr,"explicit .efk model import");
    Check(std::wcscmp(mdl::Mdl(raw)->path,effect)==0,"saved model path remains the real .efk file");
    int autoplay=-1, loop=-1;
    for(unsigned i=0;i<mdl::Mdl(raw)->morphCount;++i)
        if(text_encoding::MorphName(mdl::Morphs(raw)[i],false)==L"オート再生") autoplay=int(i);
        else if(text_encoding::MorphName(mdl::Morphs(raw)[i],false)==L"ループ") loop=int(i);
    Check(autoplay>=0 && loop>=0,"real template auto-play and loop morphs");
    app->CameraMode()=1; app->CameraPitch()=app->CameraYaw()=app->CameraRoll()=0;
    app->CameraPositionX()=app->CameraPositionY()=app->CameraPositionZ()=0;
    app->CameraDistance()=-400; app->GroundGridEnabled()=0; app->GroundShadowEnabled()=0;
    app->state.blackBackgroundEnabled=1;
    const auto render=[&](int frame) {
        app->CurrentFrame()=frame; mdl::Morphs(raw)[autoplay].value=mdl::Morphs(raw)[loop].value=1;
        FrameSceneOutputScope output(app); Check(output.Ready(),"Effekseer output scope");
        app->DeltaTime()=1.f/30.f; app->state.messageSeen=10; FrameDriver(app);
        mdl::Morphs(raw)[autoplay].value=mdl::Morphs(raw)[loop].value=1;
        RenderFrameScene(app);

    };
    for(int frame=0;frame<=20;++frame) render(frame);
    Check(dialogs==0,"unexpected plugin error dialog");
    const auto enabled=Pixels(app,L"effekseer-enabled.ppm");
    if (!plugins::LegacyMmeStatus().loaded) {
        D3DPRESENT_PARAMETERS invalid{}; invalid.Windowed=TRUE;
        Check(FAILED(legacy_mmd_plugin::ResetDevice(app->Renderer()->device,&invalid)),"real invalid D3D9 reset rejected");
        std::puts("PASS failed native Reset before recovery");
    }
    Check(PostDeviceReset(app),"real D3D9 reset with Effekseer resources");
    render(20);
    const auto reset=Pixels(app,L"effekseer-reset.ppm");
    legacy_mmd_plugin::Stop();
    Check(!GetModuleHandleW(L"EffekseerForMMD.dll") && !GetModuleHandleW(L"MMSFirstPersonCamera.dll"),"real plugins unloaded");
    render(20);
    const auto disabled=Pixels(app,L"effekseer-disabled.ppm");
    std::printf("Effekseer rendered difference enabled=%zu reset=%zu\n",Difference(enabled,disabled),Difference(reset,disabled));
    Check(Difference(enabled,disabled)>100 && Difference(reset,disabled)>100,"real effect draws before and after D3D9 reset");
    legacy_mmd_plugin::Start(app);
    for(int frame=0;frame<=20;++frame) render(frame);
    const auto restarted=Pixels(app,L"effekseer-restarted.ppm");
    Check(Difference(restarted,disabled)>100,"real effect draws after restart");
    std::printf("Effekseer restarted difference=%zu\n",Difference(restarted,disabled));
    ResetAppState(app);
    Check(app->ModelSlot(0)==nullptr,"effect model deletion");
    { FrameSceneOutputScope output(app); Check(output.Ready(),"deleted effect output"); RenderFrameScene(app); }
    std::puts("PASS real Effekseer import render reset stop restart and model deletion");
    UnhookWindowsHookEx(dialogHook);
}
}
