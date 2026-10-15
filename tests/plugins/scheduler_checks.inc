namespace scheduler_checks {
bool available=true;
bool rejectRender=false;
decltype(&RenderFrameSceneForOutput) realOutputRender=nullptr;
void OutputRender(MMDApp* app) { if(!rejectRender) realOutputRender(app); }
decltype(&IsWindowVisible) realVisible=nullptr;
BOOL WINAPI Visible(HWND window) {
    if(g_Block && (window==g_Block->MainWindow() || window==g_Block->FloatingWindow())) return available;
    return realVisible(window);
}
uint64_t Count(ui_metrics::Counter counter) { return ui_metrics::counts[counter].load(); }
void Tick(MMDApp* app) { Sleep(3); updates::SafePoint(app); }
void Paint(MMDApp* app,const RECT& area) {
    HDC dc=GetDC(app->MainWindow());previewDC=dc;forcedPaintRect=&area;
    SendMessageW(app->MainWindow(),WM_PAINT,0,0);
    previewDC=nullptr;forcedPaintRect=nullptr;ReleaseDC(app->MainWindow(),dc);
}
void Run(MMDApp* app) {
    Hook(&IsWindowVisible,&Visible,&realVisible);
    Hook(&RenderFrameSceneForOutput,&OutputRender,&realOutputRender);
    app->FpsLimit()=1000;app->FrameVolumeControlEnabled()=1;
    Check(updates::Initialize(app),"initialize production update coordinator");
    // Drain startup notifications and the existing two-pass pose gate.
    for(unsigned n=0;n<20;++n) {
        MSG msg{};for(unsigned i=0;i<64 && PeekMessageW(&msg,nullptr,0,0,PM_REMOVE);++i) {TranslateMessage(&msg);DispatchMessageW(&msg);}
        Tick(app);
    }
    Check(app->MessageSeen()==0,"stopped scene reaches a clean render gate");
    auto* world=app->Physics()->world;
    Check(world!=nullptr,"production physics world exists");
    btSphereShape shape(.1f);btVector3 inertia;shape.calculateLocalInertia(1,inertia);
    btRigidBody::btRigidBodyConstructionInfo bodyInfo(1,nullptr,&shape,inertia);
    btRigidBody body(bodyInfo);auto transform=btTransform::getIdentity();transform.setOrigin(btVector3(0,100,0));
    body.setWorldTransform(transform);body.setLinearVelocity(btVector3(1,0,0));world->addRigidBody(&body);
    const auto scene=Count(ui_metrics::Scene), effect=Count(ui_metrics::EffectBegin), present=Count(ui_metrics::Present), reset=Count(ui_metrics::Reset);
    const RECT ui{0,50,app->SidebarWidth()-10,90};
    for(unsigned i=0;i<8;++i) Paint(app,ui);
    NMCUSTOMDRAW draw{};draw.hdr.hwndFrom=GetDlgItem(app->MainWindow(),panel::kPlayButton);
    draw.hdr.idFrom=panel::kPlayButton;draw.hdr.code=NM_CUSTOMDRAW;draw.dwDrawStage=CDDS_PREPAINT;
    draw.hdc=GetDC(draw.hdr.hwndFrom);GetClientRect(draw.hdr.hwndFrom,&draw.rc);
    SendMessageW(app->MainWindow(),WM_NOTIFY,draw.hdr.idFrom,reinterpret_cast<LPARAM>(&draw));ReleaseDC(draw.hdr.hwndFrom,draw.hdc);
    Check(Count(ui_metrics::Scene)==scene,"WM_PAINT never renders synchronously");
    for(unsigned i=0;i<4;++i) Tick(app);
    Check(Count(ui_metrics::Scene)==scene && Count(ui_metrics::EffectBegin)==effect && Count(ui_metrics::Present)==present && Count(ui_metrics::Reset)==reset,
        "pure UI paint adds zero scene, effect entry, present and reset calls");
    const RECT viewport=FramePresentationRect(app);
    const auto physics=Count(ui_metrics::Physics);
    const auto frame=app->CurrentFrame();
    const auto cursor=app->PlaybackCursorSeconds();
    for(unsigned i=0;i<8;++i) Paint(app,viewport);
    Check(Count(ui_metrics::Scene)==scene,"viewport exposure only queues recovery");
    Tick(app);
    Check(Count(ui_metrics::Scene)==scene+1 && Count(ui_metrics::Present)==present+1,"coalesced exposure renders and presents one valid replacement frame");
    for(unsigned i=0;i<4;++i) Tick(app);
    Check(Count(ui_metrics::Scene)==scene+1,"exposure recovery does not restart the two-pass edit gate");
    Check(Count(ui_metrics::Physics)==physics && app->CurrentFrame()==frame && app->PlaybackCursorSeconds()==cursor,
        "exposure recovery does not enter physics or advance animation time");
    Check(body.getWorldTransform()==transform && body.getLinearVelocity()==btVector3(1,0,0),"exposure preserves a live dynamic rigid body's transform and velocity");
    const auto failedPresent=Count(ui_metrics::Present);
    rejectRender=true;updates::RequestViewport(app);Tick(app);
    Check(Count(ui_metrics::Present)==failedPresent,"failed exposure never presents an invalid discarded backbuffer");
    rejectRender=false;Tick(app);
    Check(Count(ui_metrics::Present)==failedPresent+1,"failed exposure retains its render request for the next safe point");
    auto before=Count(ui_metrics::Scene);
    EnableWindow(app->MainWindow(),FALSE);Paint(app,viewport);Tick(app);
    Check(Count(ui_metrics::Scene)==before,"disabled modal owner retains requests without rendering");
    EnableWindow(app->MainWindow(),TRUE);Tick(app);
    Check(Count(ui_metrics::Scene)==before+1,"modal owner resume consumes retained recovery");
    before=Count(ui_metrics::Scene);available=false;
    updates::RequestViewport(app,true);Tick(app);
    Check(Count(ui_metrics::Scene)==before && updates::WaitMilliseconds(app)>0,"hidden viewport retains work without a busy wait");
    available=true;Tick(app);
    Check(Count(ui_metrics::Scene)==before+1,"restored viewport processes latest geometry");
    before=Count(ui_metrics::Scene);updates::RequestViewport(app);
    { updates::MessageScope dispatch;Tick(app);Check(updates::WaitMilliseconds(app)>0,"nested modal dispatch backs off its timer"); }
    Check(Count(ui_metrics::Scene)==before,"ordinary nested dispatch cannot run a frame");
    Tick(app);Check(Count(ui_metrics::Scene)==before+1,"outer safe point drains deferred work");
    before=Count(ui_metrics::Scene);updates::RequestViewport(app);
    { updates::FrameScope frame;Tick(app);FrameDriver(app);Check(updates::WaitMilliseconds(app)>0,"active frame has a bounded wait"); }
    Check(Count(ui_metrics::Scene)==before,"both scheduler and frame driver reject recursive execution");
    Tick(app);Check(Count(ui_metrics::Scene)==before+1,"reentrant request survives until next safe point");
    before=Count(ui_metrics::Scene);updates::RequestViewport(app);
    { updates::MessageScope loop; updates::ObserveMessage(app,WM_ENTERSIZEMOVE,0,0);Tick(app);updates::ObserveMessage(app,WM_EXITSIZEMOVE,0,0); }
    Check(Count(ui_metrics::Scene)==before+1,"native move-size loop uses the same bounded safe point");
    Tick(app);
    Check(Count(ui_metrics::Reset)==reset,"UI and exposure cases do not reset the device");
    before=Count(ui_metrics::Physics);
    RECT bounds{};GetWindowRect(app->MainWindow(),&bounds);
    SetWindowPos(app->MainWindow(),nullptr,0,0,bounds.right-bounds.left-8,bounds.bottom-bounds.top,
        SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    Tick(app);
    Check(Count(ui_metrics::Physics)==before && app->CurrentFrame()==frame && app->PlaybackCursorSeconds()==cursor,
        "stopped window resize redraws without physics or animation advance");
    Check(body.getWorldTransform()==transform && body.getLinearVelocity()==btVector3(1,0,0),"window resize preserves the live rigid body");
    before=Count(ui_metrics::Scene);
    { ScopedSceneMutation mutation(*app);updates::RequestViewport(app);Tick(app);
      Check(Count(ui_metrics::Scene)==before && updates::WaitMilliseconds(app)>0,"scene mutation retains requests with a bounded wait"); }
    for(unsigned i=0;i<4;++i) Tick(app);
    before=Count(ui_metrics::Scene);
    { bake::ScopedUiLock lock(nullptr);updates::RequestViewport(app);Tick(app);
      Check(Count(ui_metrics::Scene)==before && updates::WaitMilliseconds(app)>0,"bake lock retains requests with a bounded wait"); }
    Tick(app);Check(Count(ui_metrics::Scene)>before,"unlock resumes pending viewport work");
    SetWindowTextW(GetDlgItem(app->MainWindow(),panel::kPlayStartFrameEdit),L"0");
    SetWindowTextW(GetDlgItem(app->MainWindow(),panel::kPlayStopFrameEdit),L"120");
    app->PlaybackLoopEnabled()=0;app->PlaybackStartsAtCurrentFrame()=0;app->MilliToSec()=.001f;
    CommandDispatch(GetDlgItem(app->MainWindow(),panel::kPlayButton),MAKEWPARAM(408,BN_CLICKED));
    Check(app->PlaybackActive()!=0,"production play command starts playback");
    before=Count(ui_metrics::Scene);
    for(unsigned i=0;i<8;++i){Sleep(20);Tick(app);}
    Check(app->PlaybackCursorSeconds()>0 && Count(ui_metrics::Scene)>before,"unified clock advances playback and renders while no window messages arrive");
    Check(!(body.getWorldTransform()==transform),"playback still advances the dynamic rigid body");
    app->PlaybackActive()=0;StopPlayback(app);world->removeRigidBody(&body);
    std::printf("METRICS paint=%llu scene=%llu effectBegin=%llu reset=%llu present=%llu\n",
        Count(ui_metrics::Paint),Count(ui_metrics::Scene),Count(ui_metrics::EffectBegin),Count(ui_metrics::Reset),Count(ui_metrics::Present));
    updates::Shutdown(app);available=false;
}
}
