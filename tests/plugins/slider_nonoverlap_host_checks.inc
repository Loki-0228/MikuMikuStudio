// Include in the actual disposable scene host. Run after all seven parameter
// sliders are visible/on-screen. Observation changes no authored slider value.
#include "../slider_paint_trace.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/globals.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/build_profile.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "../../src/plugins/native_pose_scope.hpp"
#endif
#include <fstream>
#include <cstdio>
#include <cstring>

namespace slider_nonoverlap_host_checks {
namespace trace=slider_paint_trace;
constexpr std::array<int,7> kIds{447,455,456,457,458,459,460};
static RECT ScreenRect(HWND window){RECT rect{};trace::Require(GetWindowRect(window,&rect)!=FALSE,"read actual slider screen bounds");return rect;}
static bool Contains(const RECT& outer,const RECT& inner){
    return inner.left>=outer.left&&inner.top>=outer.top&&inner.right<=outer.right&&inner.bottom<=outer.bottom&&
        inner.left<inner.right&&inner.top<inner.bottom;
}
static void Run(mikumikustudio::MMDApp* app,const std::filesystem::path& output){
    using namespace mikumikustudio;
    trace::Require(app&&app==g_Block&&IsWindow(app->MainWindow())&&!GetCapture()&&!app->PlaybackActive()&&!app->FrameStepPlayback(),
        "slider nonoverlap regression requires the actual idle disposable UI host");
    std::array<HWND,7> windows{};std::array<RECT,7> bounds{};
    std::array<int,7> positions{};
    for(unsigned i=0;i<windows.size();++i){
        windows[i]=GetDlgItem(app->MainWindow(),kIds[i]);
        trace::Require(windows[i]&&IsWindowVisible(windows[i])&&IsWindowEnabled(windows[i]),"mount all seven native parameter sliders");
        bounds[i]=ScreenRect(windows[i]);positions[i]=int(SendMessageW(windows[i],TBM_GETPOS,0,0));
        for(unsigned j=0;j<i;++j){RECT overlap{};trace::Require(!IntersectRect(&overlap,&bounds[i],&bounds[j]),"parameter slider sibling rectangles are pairwise disjoint");}
    }
    const auto rgb=ScreenRect(windows[3]);
    const auto loadButton=ScreenRect(GetDlgItem(app->MainWindow(),467));
    const auto registerButton=ScreenRect(GetDlgItem(app->MainWindow(),468));
    trace::Require(bounds[4].bottom==bounds[5].top&&bounds[5].bottom==bounds[6].top&&
        rgb.bottom<=bounds[4].top&&bounds[6].bottom<=loadButton.top&&bounds[6].bottom<=registerButton.top,
        "complete direction-slider rows also fit between the RGB controls and the load/register buttons");
    const int frame=app->CurrentFrame();const auto modified=app->SceneModified();
    const auto recorded=app->RecordedFrameCountByte1();const auto physics=app->PhysicsResetPending();
    trace::Require(app->CameraKeys()&&app->LightKeys(),"native geometry observer has managed camera/light key pools");
    const std::vector<mdl::CameraKey> camera(app->CameraKeys(),app->CameraKeys()+mdl::KeyPoolCapacity(app->CameraKeys()));
    const std::vector<mdl::LightKey> light(app->LightKeys(),app->LightKeys()+mdl::KeyPoolCapacity(app->LightKeys()));
    std::array<float,7> values{app->CameraFov(),app->LightColor()[0],app->LightColor()[1],app->LightColor()[2],
        app->LightDirection()[0],app->LightDirection()[1],app->LightDirection()[2]};
#if !MMD_ORIGINAL_PROFILE
    auto* history=plugins::BoundNativePoseHistory(app);std::uint64_t revision=0;
    if(history)trace::Require(history->Revision(revision)==MMS_OK,"capture the actual common-history revision");
    const auto undo=history?history->UndoCount():0,redo=history?history->RedoCount():0;
    const auto retained=history?history->RetainedBytes():0;
#endif
    std::filesystem::create_directories(output);std::ofstream report(output/"nonoverlap-report.txt");
    trace::Require(bool(report),"create native slider geometry/pixel report");
    trace::Trace observer(app->MainWindow(),windows);
    const auto flush=[&]{UpdateWindow(app->MainWindow());for(auto w:windows)UpdateWindow(w);GdiFlush();};
    const auto redraw=[&](HWND window){trace::Require(RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW)!=FALSE,"run the installed native erase and paint procedures");GdiFlush();};
    try{
        observer.Phase("disjoint-native-warm");for(auto w:windows)redraw(w);flush();
        for(unsigned i=0;i<windows.size();++i){
            RECT client{},thumb{},channel{};trace::Require(GetClientRect(windows[i],&client)!=FALSE,"read native slider client extent");
            SendMessageW(windows[i],TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
            SendMessageW(windows[i],TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
            const auto pixels=observer.Read(windows[i]);
            trace::Require(pixels.Live()&&pixels.Varied()&&Contains(client,thumb)&&Contains(client,channel),
                "native channel and complete thumb fit the unchanged slider client and actual screen pixels exist");
            if(i>=4)trace::Require(client.right==132&&client.bottom==20,"direction sliders keep their original complete 132 by 20 theme/tick area");
            report<<"GEOMETRY id="<<kIds[i]<<" window="<<bounds[i].left<<','<<bounds[i].top<<','<<bounds[i].right<<','<<bounds[i].bottom
                <<" client="<<client.right<<'x'<<client.bottom<<" thumb="<<thumb.left<<','<<thumb.top<<','<<thumb.right<<','<<thumb.bottom
                <<" channel="<<channel.left<<','<<channel.top<<','<<channel.right<<','<<channel.bottom<<'\n';
        }
        // Native erase/paint of one real HWND must not modify another slider.
        // This uses displayed pixels, not the controlled WM_PRINTCLIENT DIB.
        unsigned peerSamples=0;
        for(unsigned source=0;source<windows.size();++source){
            observer.Phase(std::to_string(kIds[source])+"-disjoint-native-redraw");
            std::array<trace::Snapshot,7> before;
            for(unsigned peer=0;peer<windows.size();++peer)before[peer]=observer.Read(windows[peer]);
            redraw(windows[source]);
            for(unsigned peer=0;peer<windows.size();++peer)if(peer!=source){
                const auto after=observer.Read(windows[peer]);
                trace::Require(before[peer].Live()&&after.Live()&&before[peer].tick==after.tick&&
                    before[peer].paints==after.paints&&before[peer].erases==after.erases&&trace::Changed(before[peer],after)==0,
                    "an isolated native redraw leaves every idle peer slider's displayed pixels and paint counters untouched");++peerSamples;
            }
        }
        // Restore the former geometry temporarily to prove this pixel oracle
        // detects the exact two-row defect. This is a labelled counterexample,
        // not an observation of a production flash after the fix.
        struct Restore {
            std::array<HWND,3> windows{};std::array<RECT,3> parentBounds{};
            bool armed=true;
            ~Restore(){if(armed)for(unsigned i=0;i<windows.size();++i)SetWindowPos(windows[i],nullptr,parentBounds[i].left,parentBounds[i].top,
                parentBounds[i].right-parentBounds[i].left,parentBounds[i].bottom-parentBounds[i].top,SWP_NOZORDER|SWP_NOACTIVATE);}
        } restore;
        for(unsigned i=0;i<3;++i){restore.windows[i]=windows[i+4];restore.parentBounds[i]=bounds[i+4];
            MapWindowPoints(nullptr,app->MainWindow(),reinterpret_cast<POINT*>(&restore.parentBounds[i]),2);}
        trace::Require(SetWindowPos(windows[4],nullptr,restore.parentBounds[0].left,restore.parentBounds[0].top+2,132,20,SWP_NOZORDER|SWP_NOACTIVATE)&&
            SetWindowPos(windows[6],nullptr,restore.parentBounds[2].left,restore.parentBounds[2].top-2,132,20,SWP_NOZORDER|SWP_NOACTIVATE),
            "recreate only the historical two-row slider overlap on owned test windows");
        observer.Phase("counterexample-old-overlap-warm");for(unsigned i=4;i<7;++i)redraw(windows[i]);flush();
        const auto before=observer.Read(windows[5]);observer.Phase("counterexample-old-overlap-repaint-X");redraw(windows[4]);
        const auto after=observer.Read(windows[5]);const auto changed=trace::Changed(before,after);
        trace::Require(before.Live()&&after.Live()&&changed>0&&before.tick==after.tick&&before.paints==after.paints&&before.erases==after.erases,
            "old-overlap counterexample changes real peer pixels without a peer paint or authored tick change");
        report<<"COUNTEREXAMPLE old_overlap_peer_pixels="<<changed<<" peer_tick="<<before.tick<<" peer_paints="<<before.paints
            <<" (temporary old geometry, no visible-flicker duration claim)\n";
        for(unsigned i=0;i<3;++i)trace::Require(SetWindowPos(restore.windows[i],nullptr,restore.parentBounds[i].left,restore.parentBounds[i].top,
            restore.parentBounds[i].right-restore.parentBounds[i].left,restore.parentBounds[i].bottom-restore.parentBounds[i].top,SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,
            "restore the exact production slider geometry after the overlap counterexample");
        restore.armed=false;
        observer.Phase("restored-disjoint-native-warm");for(auto w:windows)redraw(w);flush();
        for(unsigned i=0;i<windows.size();++i){const auto rect=ScreenRect(windows[i]);trace::Require(EqualRect(&rect,&bounds[i])&&
            int(SendMessageW(windows[i],TBM_GETPOS,0,0))==positions[i],"regression restores every slider's actual geometry and retains its authored tick");}
        trace::Require(observer.Complete(),"complete native nonoverlap observation without quota truncation");
        report<<"SUMMARY disjoint_controls=7 unchanged_idle_peer_samples="<<peerSamples<<" native_direction_size=132x20\n";
    }catch(...){observer.Write(output);report<<"FAILED: evidence retained.\n";report.flush();throw;}
    observer.Write(output);report.flush();trace::Require(bool(report),"finish native nonoverlap evidence report");
    const std::array<float,7> afterValues{app->CameraFov(),app->LightColor()[0],app->LightColor()[1],app->LightColor()[2],
        app->LightDirection()[0],app->LightDirection()[1],app->LightDirection()[2]};
    trace::Require(!std::memcmp(values.data(),afterValues.data(),sizeof(values))&&app->CurrentFrame()==frame&&app->SceneModified()==modified&&
        app->RecordedFrameCountByte1()==recorded&&app->PhysicsResetPending()==physics&&camera.size()==mdl::KeyPoolCapacity(app->CameraKeys())&&
        light.size()==mdl::KeyPoolCapacity(app->LightKeys())&&!std::memcmp(camera.data(),app->CameraKeys(),camera.size()*sizeof(mdl::CameraKey))&&
        !std::memcmp(light.data(),app->LightKeys(),light.size()*sizeof(mdl::LightKey)),"geometry/pixel regression changes no live authored parameter playhead dirty flag physics flag or VMD key");
#if !MMD_ORIGINAL_PROFILE
    if(history){std::uint64_t current=0;trace::Require(history->Revision(current)==MMS_OK&&current==revision&&history->UndoCount()==undo&&
        history->RedoCount()==redo&&history->RetainedBytes()==retained,"native painting and the old-geometry counterexample do not record common history");}
#endif
    std::puts("SLIDER_NONOVERLAP controls=7 idle_peer_samples=42 native_direction_size=132x20 counterexample=observed (no full-flicker claim)");
}
}
