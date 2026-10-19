// Include after light_slider_geometry_checks.inc and the slider trace host helpers.
// Diagnostic only: no pixel is drawn, no window is moved, and no SWP refresh is forced.
#include "../slider_paint_trace.hpp"
#include "mikumikustudio/build_profile.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/key_pool.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#if !MMD_ORIGINAL_PROFILE
namespace light_slider_clip_checks {
namespace trace = slider_paint_trace;
namespace prior = slider_paint_trace_host_checks;
namespace geometry = light_slider_geometry_checks::detail;
using mikumikustudio::MMDApp;
constexpr std::array<int, 6> kIds{455,456,457,458,459,460};

template<class Key> struct KeyCopy {
    Key* const pointer;
    const std::uint64_t identity;
    const std::vector<Key> bytes;
    explicit KeyCopy(Key* value) : pointer(value), identity(mikumikustudio::mdl::KeyPoolIdentity(value)),
        bytes(value,value+mikumikustudio::mdl::KeyPoolCapacity(value)) {}
    void Check(Key* value) const {
        using namespace mikumikustudio::mdl;
        trace::Require(value==pointer && KeyPoolIdentity(value)==identity && KeyPoolCapacity(value)==bytes.size() &&
            !std::memcmp(value,bytes.data(),bytes.size()*sizeof(Key)),
            "clip-only style comparison preserves original VMD pool identity, capacity and every byte");
    }
};
struct Invariant {
    struct Window { HWND hwnd=nullptr,previous=nullptr,next=nullptr; LONG_PTR style=0; RECT rectangle{}; UINT ui=0; LRESULT tick=0; };
    MMDApp& app;
    const prior::PumpInvariant history;
    const int modified,physics,recorded;
    const HWND focus=GetFocus(),capture=GetCapture();
    std::array<Window,6> windows{};
    KeyCopy<mikumikustudio::mdl::CameraKey> camera;
    KeyCopy<mikumikustudio::mdl::LightKey> light;
    KeyCopy<mikumikustudio::mdl::SelfShadowKey> shadow;
    KeyCopy<mikumikustudio::mdl::GravityKey> gravity;
    explicit Invariant(MMDApp& value) : app(value),history(value),modified(value.SceneModified()),
        physics(value.PhysicsResetPending()),recorded(value.RecordedFrameCountByte1()),
        camera(value.CameraKeys()),light(value.LightKeys()),shadow(value.ShadowKeys()),gravity(value.GravityKeys()) {
        for(unsigned i=0;i<kIds.size();++i){auto& item=windows[i];item.hwnd=GetDlgItem(value.MainWindow(),kIds[i]);
            trace::Require(item.hwnd && IsWindow(item.hwnd),"capture actual six lighting HWND identities");
            item.previous=GetWindow(item.hwnd,GW_HWNDPREV);item.next=GetWindow(item.hwnd,GW_HWNDNEXT);
            item.style=GetWindowLongPtrW(item.hwnd,GWL_STYLE);
            trace::Require(GetWindowRect(item.hwnd,&item.rectangle)!=FALSE,"capture original lighting screen rectangle");
            item.ui=UINT(SendMessageW(item.hwnd,WM_QUERYUISTATE,0,0));item.tick=SendMessageW(item.hwnd,TBM_GETPOS,0,0);
        }
    }
    void Check(bool styles=true) const {
        history.Check();camera.Check(app.CameraKeys());light.Check(app.LightKeys());shadow.Check(app.ShadowKeys());gravity.Check(app.GravityKeys());
        trace::Require(app.SceneModified()==modified && app.PhysicsResetPending()==physics && app.RecordedFrameCountByte1()==recorded &&
            GetFocus()==focus && GetCapture()==capture,"clip-only comparison preserves focus, capture, dirty, physics and recording state");
        for(const auto& item:windows){RECT rectangle{};
            trace::Require(IsWindow(item.hwnd) && GetDlgItem(app.MainWindow(),GetDlgCtrlID(item.hwnd))==item.hwnd &&
                GetWindow(item.hwnd,GW_HWNDPREV)==item.previous && GetWindow(item.hwnd,GW_HWNDNEXT)==item.next &&
                GetWindowRect(item.hwnd,&rectangle) && EqualRect(&rectangle,&item.rectangle) &&
                SendMessageW(item.hwnd,WM_QUERYUISTATE,0,0)==item.ui && SendMessageW(item.hwnd,TBM_GETPOS,0,0)==item.tick &&
                (!styles || GetWindowLongPtrW(item.hwnd,GWL_STYLE)==item.style),
                "clip-only comparison restores full styles and preserves dimensions, Z order, UI state and tick of all six HWNDs");
        }
    }
};
static void Guard(ULONGLONG started){
    trace::Require(GetTickCount64()-started<5000,"INCOMPLETE clip comparison exceeded its five-second cooperative budget");
    trace::Require(!GetCapture(),"INCOMPLETE clip comparison never changes an active capture");
    for(int key=1;key<=255;++key)trace::Require((GetAsyncKeyState(key)&0x8000)==0,
        "INCOMPLETE real held key/button prevents isolated clip comparison");
}
static bool Above(HWND candidate,HWND target){
    unsigned count=0;
    for(HWND window=GetWindow(target,GW_HWNDPREV);window && count++<512;window=GetWindow(window,GW_HWNDPREV))
        if(window==candidate)return true;
    return false;
}
struct Observation {
    bool entered=false,contextRestored=false,physical=false,dc=false,region=false,coordinates=false,centerVisible=false;
    int sysResult=0,regionKind=ERROR,clipKind=ERROR,mapMode=0;
    RECT target{},peer{},client{},clip{},regionBounds{},overlap{};
    POINT origin{},viewport{},windowOrigin{},mapped[2]{},clientScreen[2]{};
    unsigned tested=0,inRegion=0,ownedByPeer=0,ownedByTarget=0,foreign=0;
};
static Observation Read(const geometry::Api& api,HWND target,HWND peer){
    Observation result;
    geometry::PhysicalContext physical(api);
    result.entered=physical.entered;result.physical=physical.qualified;
    if(physical.qualified){
        trace::Require(GetWindowRect(target,&result.target) && GetWindowRect(peer,&result.peer) &&
            GetClientRect(target,&result.client),"read verified physical target/peer/client geometry");
        IntersectRect(&result.overlap,&result.target,&result.peer);
        result.clientScreen[0]={0,0};result.clientScreen[1]={result.client.right,result.client.bottom};
        trace::Require(ClientToScreen(target,&result.clientScreen[0]) && ClientToScreen(target,&result.clientScreen[1]),
            "record physical caller client mapping without rounding correction");
        const HDC dc=GetDC(target);
        if(dc){
            struct Release{HWND window;HDC dc;~Release(){ReleaseDC(window,dc);}}release{target,dc};
            result.dc=GetObjectType(dc)==OBJ_DC;result.clipKind=GetClipBox(dc,&result.clip);result.mapMode=GetMapMode(dc);
            const bool mapped=GetDCOrgEx(dc,&result.origin) && GetViewportOrgEx(dc,&result.viewport) && GetWindowOrgEx(dc,&result.windowOrigin);
            result.mapped[0]={0,0};result.mapped[1]={result.client.right,result.client.bottom};
            const bool converted=LPtoDP(dc,result.mapped,2)!=FALSE;
            const HRGN region=CreateRectRgn(0,0,0,0);
            if(region){
                struct Delete{HRGN region;~Delete(){DeleteObject(region);}}deleteRegion{region};
                // SYSRGN=4 is the actual ordinary GetDC client-visible region,
                // expressed in screen coordinates on Windows NT. No extra
                // DCX_CLIPSIBLINGS flag is supplied to manufacture this result.
                result.sysResult=GetRandomRgn(dc,region,4);
                result.regionKind=GetRgnBox(region,&result.regionBounds);result.region=result.sysResult==1;
                const POINT center{(result.target.left+result.target.right)/2,(result.target.top+result.target.bottom)/2};
                result.centerVisible=PtInRegion(region,center.x,center.y)!=FALSE;
                // Reject virtualized/empty/foreign coordinate regions. Do not
                // rescale or move the region until the expected answer appears.
                result.coordinates=result.dc && mapped && converted && result.region && result.regionKind!=ERROR &&
                    result.regionKind!=NULLREGION && result.regionBounds.left>=result.target.left &&
                    result.regionBounds.top>=result.target.top && result.regionBounds.right<=result.target.right &&
                    result.regionBounds.bottom<=result.target.bottom && result.centerVisible;
                const auto width=std::int64_t(result.overlap.right)-result.overlap.left;
                const auto height=std::int64_t(result.overlap.bottom)-result.overlap.top;
                if(result.coordinates && width>0 && height>0 && width*height<=4096){
                    for(LONG y=result.overlap.top;y<result.overlap.bottom;++y)for(LONG x=result.overlap.left;x<result.overlap.right;++x){
                        ++result.tested;result.inRegion+=PtInRegion(region,x,y)!=FALSE;
                        const auto hit=WindowFromPoint(POINT{x,y});
                        result.ownedByPeer+=hit==peer;result.ownedByTarget+=hit==target;
                        result.foreign+=hit!=peer && hit!=target;
                    }
                }
            }
        }
    }
    physical.Restore();result.contextRestored=!result.entered || physical.restored;
    return result;
}
static bool SameObservation(const Observation& a,const Observation& b){
    return a.physical==b.physical && a.coordinates==b.coordinates && a.dc==b.dc && a.region==b.region &&
        a.regionKind==b.regionKind && EqualRect(&a.target,&b.target) && EqualRect(&a.peer,&b.peer) &&
        EqualRect(&a.client,&b.client) && EqualRect(&a.clip,&b.clip) && EqualRect(&a.regionBounds,&b.regionBounds) &&
        EqualRect(&a.overlap,&b.overlap) && a.tested==b.tested && a.inRegion==b.inRegion &&
        a.ownedByPeer==b.ownedByPeer && a.ownedByTarget==b.ownedByTarget && a.foreign==b.foreign;
}
static void Row(std::ofstream& report,const char* label,const Observation& value){
    report<<label<<" physical="<<value.physical<<" context_restored="<<value.contextRestored<<" actual_dc="<<value.dc
        <<" native_client="<<value.client.right<<'x'<<value.client.bottom<<" client_screen="<<value.clientScreen[0].x<<','<<value.clientScreen[0].y
        <<','<<value.clientScreen[1].x<<','<<value.clientScreen[1].y<<" dc_org="<<value.origin.x<<','<<value.origin.y
        <<" lp_to_dp="<<value.mapped[0].x<<','<<value.mapped[0].y<<','<<value.mapped[1].x<<','<<value.mapped[1].y
        <<" viewport="<<value.viewport.x<<','<<value.viewport.y<<" window_org="<<value.windowOrigin.x<<','<<value.windowOrigin.y
        <<" map_mode="<<value.mapMode<<" clip_kind="<<value.clipKind<<" sys_result="<<value.sysResult<<" region_kind="<<value.regionKind
        <<" region_bounds="<<value.regionBounds.left<<','<<value.regionBounds.top<<','<<value.regionBounds.right<<','<<value.regionBounds.bottom
        <<" verified_screen_coordinates="<<value.coordinates<<" physical_intersection="<<value.overlap.left<<','<<value.overlap.top<<','
        <<value.overlap.right<<','<<value.overlap.bottom<<" tested="<<value.tested<<" client_visible="<<value.inRegion
        <<" owner_peer="<<value.ownedByPeer<<" owner_target="<<value.ownedByTarget<<" owner_foreign="<<value.foreign<<'\n';report.flush();
}
struct RestoreStyle {
    HWND window=nullptr;LONG_PTR original=0;bool pending=false;
    void Restore() noexcept {if(pending && IsWindow(window)){
        SetWindowLongPtrW(window,GWL_STYLE,original);pending=GetWindowLongPtrW(window,GWL_STYLE)!=original;}}
    ~RestoreStyle(){Restore();}
};
static void Run(MMDApp* app,const std::filesystem::path& outputDirectory){
    trace::Require(app && app==mikumikustudio::g_Block && app->MainWindow() && app->CameraKeys() && app->LightKeys() &&
        app->ShadowKeys() && app->GravityKeys() && !app->PlaybackActive() && !app->FrameStepPlayback(),
        "clip comparison requires a disposable idle actual MMS host");
    std::filesystem::create_directories(outputDirectory);
    std::ofstream report(outputDirectory/"light-slider-clip-report.txt");trace::Require(bool(report),"open actual light clip comparison report");
    report<<"Ordinary actual GetDC/SYSRGN client-visible-region comparison on one identical HWND: production style, raw style without WS_CLIPSIBLINGS, exact restored style. "
        "No DCX clipping override, SWP_FRAMECHANGED, Z-order/geometry change, native paint, screen write, focus/capture or authored operation is requested. "
        "Screen ownership must name the actual higher-Z sibling for every intersection pixel. No natural-paint, hardware, DWM or all-six flicker claim. "
        "Unavailable region/physical API, no overlap, unverified coordinates, external occlusion or unchanged raw clipping cannot pass.\n";
    const auto started=GetTickCount64();Guard(started);
    const Invariant invariant(*app);const geometry::Api api;
    HWND target=nullptr,peer=nullptr;
    try{
        // Discover actual physical overlap, never enlarge or reposition a test
        // HWND in order to create a clipping counterexample.
        {
            geometry::PhysicalContext physical(api);
            trace::Require(physical.qualified,"INCOMPLETE verified physical coordinate APIs unavailable for clip comparison");
            for(unsigned i=0;i<kIds.size() && !target;++i)for(unsigned j=i+1;j<kIds.size();++j){
                const auto a=GetDlgItem(app->MainWindow(),kIds[i]),b=GetDlgItem(app->MainWindow(),kIds[j]);RECT ar{},br{},overlap{};
                if(!IsWindowVisible(a) || !IsWindowVisible(b) || !GetWindowRect(a,&ar) || !GetWindowRect(b,&br) || !IntersectRect(&overlap,&ar,&br))continue;
                if(Above(b,a)){target=a;peer=b;}else if(Above(a,b)){target=b;peer=a;}
                if(target)break;
            }
            physical.Restore();trace::Require(physical.restored,"restore exact caller DPI context after physical pair discovery");
        }
        trace::Require(target && peer,"INCOMPLETE no actual higher-Z lighting sibling intersection exists at this DPI/layout");
        const auto original=GetWindowLongPtrW(target,GWL_STYLE);
        report<<"PAIR lower_z_id="<<GetDlgCtrlID(target)<<" higher_z_id="<<GetDlgCtrlID(peer)<<" original_full_style="<<original<<'\n';
        trace::Require((original & WS_CLIPSIBLINGS)!=0,"INCOMPLETE production candidate must already provide WS_CLIPSIBLINGS");
        Guard(started);const auto baseline=Read(api,target,peer);Row(report,"PRODUCTION",baseline);invariant.Check();
        trace::Require(baseline.contextRestored && baseline.physical && baseline.coordinates && baseline.tested &&
            baseline.ownedByPeer==baseline.tested && !baseline.foreign,
            "INCOMPLETE overlap must map to a calibrated actual client DC region and higher-Z sibling screen owner");
        RestoreStyle restore{target,original,true};
        SetLastError(0);const auto previous=SetWindowLongPtrW(target,GWL_STYLE,original & ~LONG_PTR(WS_CLIPSIBLINGS));const auto styleError=GetLastError();
        report<<"RAW_STYLE returned_previous="<<previous<<" last_error="<<styleError<<" actual="<<GetWindowLongPtrW(target,GWL_STYLE)<<'\n';
        trace::Require((previous || !styleError) && previous==original && GetWindowLongPtrW(target,GWL_STYLE)==(original & ~LONG_PTR(WS_CLIPSIBLINGS)),
            "remove only the clipping bit on the exact same live HWND");
        Guard(started);const auto raw=Read(api,target,peer);Row(report,"RAW_SAME_HWND",raw);invariant.Check(false);
        restore.Restore();trace::Require(!restore.pending,"restore complete original slider style without any SWP fallback");
        Guard(started);const auto restored=Read(api,target,peer);Row(report,"RESTORED",restored);invariant.Check();
        trace::Require(raw.contextRestored && restored.contextRestored,"all clip snapshots restore the exact prior DPI context");
        trace::Require(raw.coordinates && raw.tested==baseline.tested && raw.ownedByPeer==raw.tested && !raw.foreign &&
            EqualRect(&raw.target,&baseline.target) && EqualRect(&raw.peer,&baseline.peer) && EqualRect(&raw.overlap,&baseline.overlap),
            "INCOMPLETE same physical client row and sibling owner must remain qualified in the raw counterexample");
        trace::Require(SameObservation(baseline,restored),"restored style must reproduce the entire original actual clip observation");
        trace::Require(!baseline.inRegion && !restored.inRegion && raw.inRegion==raw.tested,
            "INCOMPLETE raw GetDC must admit the peer-owned row while production and exact restore both exclude it; unchanged cached clipping is not proof");
        report<<"PASS exact_same_HWND_raw_negative_control=1 peer_owned_physical_client_pixels="<<raw.tested
            <<" full_state_restored=1 elapsed_ms="<<GetTickCount64()-started<<'\n';report.flush();
    }catch(const std::exception& error){
        // A live RestoreStyle is destroyed before this handler, including any
        // failed raw query. Never hide a restoration failure behind INCOMPLETE.
        try{invariant.Check();}catch(const std::exception& restoreError){report<<"FAILED restoration="<<restoreError.what()<<'\n';report.flush();throw;}
        report<<"INCOMPLETE reason="<<error.what()<<" full_state_restored=1 elapsed_ms="<<GetTickCount64()-started<<'\n';report.flush();throw;
    }
}
} // namespace light_slider_clip_checks
#endif
