// Read-only geometry diagnostics. No focus, capture, style, layout, paint or
// authored-data message is sent to the application.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "mikumikustudio/mmd_app.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace light_slider_geometry_checks {
enum class PhysicalStatus { Captured, Unavailable, Failed };
struct Result {
    PhysicalStatus physical=PhysicalStatus::Unavailable;
    unsigned physicalRectangles=0,overlapPairs=0,ownVisibleOverlapPairs=0;
    bool contextRestored=false;
};
namespace detail {
using Context=HANDLE;
using GetThread=Context(WINAPI*)();
using GetWindow=Context(WINAPI*)(HWND);
using SetThread=Context(WINAPI*)(Context);
using Equal=BOOL(WINAPI*)(Context,Context);
using Awareness=int(WINAPI*)(Context);
using GetDpi=UINT(WINAPI*)(HWND);
struct Api {
    GetThread thread=nullptr;GetWindow window=nullptr;SetThread set=nullptr;
    Equal equal=nullptr;Awareness awareness=nullptr;GetDpi dpi=nullptr;
    Api(){
        const auto module=GetModuleHandleW(L"user32.dll");
        if(!module)return;
        thread=reinterpret_cast<GetThread>(GetProcAddress(module,"GetThreadDpiAwarenessContext"));
        window=reinterpret_cast<GetWindow>(GetProcAddress(module,"GetWindowDpiAwarenessContext"));
        set=reinterpret_cast<SetThread>(GetProcAddress(module,"SetThreadDpiAwarenessContext"));
        equal=reinterpret_cast<Equal>(GetProcAddress(module,"AreDpiAwarenessContextsEqual"));
        awareness=reinterpret_cast<Awareness>(GetProcAddress(module,"GetAwarenessFromDpiAwarenessContext"));
        dpi=reinterpret_cast<GetDpi>(GetProcAddress(module,"GetDpiForWindow"));
    }
};
inline std::uintptr_t Bits(HANDLE value){return reinterpret_cast<std::uintptr_t>(value);}
struct PhysicalContext {
    const Api& api;Context before=nullptr,previous=nullptr,active=nullptr,after=nullptr;
    DWORD v2Error=0,v1Error=0,restoreError=0;
    int selected=0;
    bool entered=false,qualified=false,restored=false;
    explicit PhysicalContext(const Api& value):api(value){
        if(!api.thread||!api.window||!api.set||!api.equal||!api.awareness||!api.dpi)return;
        before=api.thread();
        if(!before)return;
        const auto v2=reinterpret_cast<Context>(INT_PTR(-4));
        const auto v1=reinterpret_cast<Context>(INT_PTR(-3));
        SetLastError(0);previous=api.set(v2);v2Error=GetLastError();
        if(previous)selected=2;
        else{
            SetLastError(0);previous=api.set(v1);v1Error=GetLastError();
            if(previous)selected=1;
        }
        if(!previous)return;
        entered=true;active=api.thread();
        qualified=active&&api.equal(before,previous)&&api.awareness(active)==2&&
            api.equal(active,selected==2?v2:v1);
    }
    void Restore() noexcept {
        if(!entered)return;
        // Pass the exact opaque context returned by SetThread, not a guessed
        // equivalent awareness constant. The report retains all raw handles.
        SetLastError(0);const auto result=api.set(previous);restoreError=GetLastError();
        after=api.thread();
        restored=result&&after&&api.equal(after,previous)&&api.equal(after,before);
        // Keep the pending restoration for one final destructor attempt if
        // the explicit call failed. The caller still receives FAILED.
        entered=!restored;
    }
    ~PhysicalContext(){Restore();}
};
struct Geometry {
    int id=0;HWND window=nullptr,parent=nullptr;DWORD style=0,extended=0;
    RECT client{},screen{};bool clientOk=false,screenOk=false,ownVisible=false,visible=false;
    Context context=nullptr;int awareness=-1;UINT dpi=0;DWORD thread=0;
};
inline Geometry Read(const Api& api,HWND window,int id){
    Geometry g;g.id=id;g.window=window;
    if(!window||!IsWindow(window))return g;
    g.parent=GetParent(window);g.style=DWORD(GetWindowLongPtrW(window,GWL_STYLE));
    g.extended=DWORD(GetWindowLongPtrW(window,GWL_EXSTYLE));
    g.ownVisible=(g.style&WS_VISIBLE)!=0;g.visible=IsWindowVisible(window)!=FALSE;
    g.clientOk=GetClientRect(window,&g.client)!=FALSE;
    g.screenOk=GetWindowRect(window,&g.screen)!=FALSE;
    g.context=api.window?api.window(window):nullptr;
    g.awareness=g.context&&api.awareness?api.awareness(g.context):-1;
    g.dpi=api.dpi?api.dpi(window):0;g.thread=GetWindowThreadProcessId(window,nullptr);
    return g;
}
inline void Row(std::ostream& out,const char* pass,const Geometry& g,Context caller,int callerAwareness){
    out<<pass<<','<<g.id<<','<<Bits(g.window)<<','<<Bits(g.parent)<<','<<g.thread<<','
        <<Bits(caller)<<','<<callerAwareness<<','<<Bits(g.context)<<','<<g.awareness<<','<<g.dpi<<','
        <<g.style<<','<<g.extended<<','<<g.ownVisible<<','<<g.visible<<','<<g.clientOk<<','
        <<g.client.left<<','<<g.client.top<<','<<g.client.right<<','<<g.client.bottom<<','<<g.screenOk<<','
        <<g.screen.left<<','<<g.screen.top<<','<<g.screen.right<<','<<g.screen.bottom<<'\n';
}
using Group=std::array<Geometry,15>;
inline Group ReadGroup(const Api& api,HWND main){
    Group group{};group[0]=Read(api,main,0);
    for(int i=0;i<12;++i)group[std::size_t(i)+1]=Read(api,GetDlgItem(main,455+i),455+i);
    group[13]=Read(api,GetDlgItem(main,447),447);group[14]=Read(api,GetDlgItem(main,560),560);
    return group;
}
inline const Geometry* Find(const Group& group,int id){
    for(const auto& g:group)if(g.id==id)return &g;
    return nullptr;
}
inline bool Complete(const Group& group){
    return std::all_of(group.begin(),group.end(),[](const auto& g){return g.window&&g.clientOk&&g.screenOk;});
}
inline void Pair(std::ostream& out,const char* kind,const Geometry& a,const Geometry& b,Result& result){
    if(!a.screenOk||!b.screenOk)return;
    const auto left=std::max(a.screen.left,b.screen.left),top=std::max(a.screen.top,b.screen.top);
    const auto right=std::min(a.screen.right,b.screen.right),bottom=std::min(a.screen.bottom,b.screen.bottom);
    const std::int64_t width=std::max<std::int64_t>(0,std::int64_t(right)-left);
    const std::int64_t height=std::max<std::int64_t>(0,std::int64_t(bottom)-top);
    const auto area=std::uint64_t(width)*std::uint64_t(height);
    const bool ownVisible=a.ownVisible&&b.ownVisible;
    result.overlapPairs+=area!=0;result.ownVisibleOverlapPairs+=area!=0&&ownVisible;
    out<<kind<<','<<a.id<<','<<b.id<<','<<ownVisible<<','<<width<<','<<height<<','<<area<<','
        <<std::int64_t(b.screen.left)-a.screen.right<<','<<std::int64_t(b.screen.top)-a.screen.bottom<<'\n';
}
struct DcInfo {
    int id=0,type=0,mapMode=0,clipKind=ERROR;HWND window=nullptr;std::uintptr_t handle=0;
    POINT origin{},screenZero{},deviceZero{};RECT clip{};
    bool opened=false,originOk=false,screenZeroOk=false,zeroMapped=false,originMatches=false,visible=false;
};
struct RawDc {
    HDC dc=nullptr;DcInfo info;
    RawDc()=default;
    RawDc(const RawDc&)=delete;RawDc& operator=(const RawDc&)=delete;
    void Open(const Geometry& g){
        info.id=g.id;info.window=g.window;info.visible=g.visible;
        if(!g.window||!IsWindow(g.window))return;
        dc=GetDC(g.window);info.opened=dc!=nullptr;
        if(!dc)return;
        info.handle=reinterpret_cast<std::uintptr_t>(dc);info.type=GetObjectType(dc);
        info.mapMode=GetMapMode(dc);info.originOk=GetDCOrgEx(dc,&info.origin)!=FALSE;
        info.screenZeroOk=ClientToScreen(g.window,&info.screenZero)!=FALSE;
        info.zeroMapped=LPtoDP(dc,&info.deviceZero,1)!=FALSE;
        info.clipKind=GetClipBox(dc,&info.clip);
        info.originMatches=info.originOk&&info.screenZeroOk&&info.zeroMapped&&
            std::int64_t(info.origin.x)+info.deviceZero.x==info.screenZero.x&&
            std::int64_t(info.origin.y)+info.deviceZero.y==info.screenZero.y;
    }
    void Close() noexcept {if(dc){ReleaseDC(info.window,dc);dc=nullptr;}}
    ~RawDc(){Close();}
};
struct ClipPoint {
    int first=0,second=0,source=0;POINT physical{},logical{},device{},roundTrip{};
    bool deviceFits=false,dpToLp=false,lpToDp=false,exact=false,qualified=false;
    int visible=-1;
};
inline ClipPoint Probe(RawDc& dc,int first,int second,POINT physical){
    ClipPoint p;p.first=first;p.second=second;p.source=dc.info.id;p.physical=physical;
    const auto x=std::int64_t(physical.x)-dc.info.origin.x,y=std::int64_t(physical.y)-dc.info.origin.y;
    p.deviceFits=x>=(std::numeric_limits<LONG>::min)()&&x<=(std::numeric_limits<LONG>::max)()&&
        y>=(std::numeric_limits<LONG>::min)()&&y<=(std::numeric_limits<LONG>::max)();
    if(!dc.dc||!dc.info.originOk||!p.deviceFits)return p;
    p.device={LONG(x),LONG(y)};p.logical=p.device;p.dpToLp=DPtoLP(dc.dc,&p.logical,1)!=FALSE;
    if(p.dpToLp){p.roundTrip=p.logical;p.lpToDp=LPtoDP(dc.dc,&p.roundTrip,1)!=FALSE;}
    p.exact=p.dpToLp&&p.lpToDp&&p.roundTrip.x==p.device.x&&p.roundTrip.y==p.device.y;
    p.qualified=p.exact&&dc.info.type==OBJ_DC&&dc.info.mapMode!=0&&dc.info.originMatches&&
        dc.info.clipKind!=ERROR&&dc.info.visible;
    if(p.qualified)p.visible=PtVisible(dc.dc,p.logical.x,p.logical.y)?1:0;
    return p;
}
struct Overlap {
    int first=0,second=0,above=0;RECT rect{};std::uint64_t area=0;
    bool allPoints=false;unsigned firstMapped=0,secondMapped=0,firstVisible=0,secondVisible=0;
};
inline int Above(const Geometry& a,const Geometry& b){
    if(a.parent!=b.parent)return 0;
    unsigned count=0;
    for(HWND w=::GetWindow(a.window,GW_HWNDPREV);w&&count<4096;w=::GetWindow(w,GW_HWNDPREV),++count)
        if(w==b.window)return b.id;
    count=0;
    for(HWND w=::GetWindow(b.window,GW_HWNDPREV);w&&count<4096;w=::GetWindow(w,GW_HWNDPREV),++count)
        if(w==a.window)return a.id;
    return 0;
}
struct ClipData {
    std::array<DcInfo,15> dc{};
    std::vector<Overlap> overlaps;
    std::vector<ClipPoint> points;
    ClipData(){overlaps.reserve(76);points.reserve(32768);}
    void Capture(const Group& group){
        std::array<RawDc,15> native{};
        for(unsigned i=0;i<group.size();++i){native[i].Open(group[i]);dc[i]=native[i].info;}
        const auto observe=[&](int first,int second){
            const auto* a=Find(group,first);const auto* b=Find(group,second);
            if(!a||!b||!a->screenOk||!b->screenOk)return;
            Overlap overlap;overlap.first=first;overlap.second=second;
            overlap.rect={std::max(a->screen.left,b->screen.left),std::max(a->screen.top,b->screen.top),
                std::min(a->screen.right,b->screen.right),std::min(a->screen.bottom,b->screen.bottom)};
            if(IsRectEmpty(&overlap.rect))return;
            overlap.area=std::uint64_t(std::int64_t(overlap.rect.right)-overlap.rect.left)*
                std::uint64_t(std::int64_t(overlap.rect.bottom)-overlap.rect.top);
            overlap.above=Above(*a,*b);
            // Never turn a large/unmappable rectangle into a passing clip
            // claim. Every pixel of a small shared strip is checked twice.
            overlap.allPoints=overlap.area<=(points.capacity()-points.size())/2;
            if(overlap.allPoints){
                auto& ad=native[std::size_t(a-group.data())];auto& bd=native[std::size_t(b-group.data())];
                for(LONG y=overlap.rect.top;y<overlap.rect.bottom;++y)for(LONG x=overlap.rect.left;x<overlap.rect.right;++x){
                    const auto ap=Probe(ad,first,second,{x,y}),bp=Probe(bd,first,second,{x,y});
                    overlap.firstMapped+=ap.qualified;overlap.secondMapped+=bp.qualified;
                    overlap.firstVisible+=ap.visible==1;overlap.secondVisible+=bp.visible==1;
                    points.push_back(ap);points.push_back(bp);
                }
            }
            overlaps.push_back(overlap);
        };
        constexpr int sliders[]{447,455,456,457,458,459,460,560};
        for(unsigned i=0;i<std::size(sliders);++i)for(unsigned j=i+1;j<std::size(sliders);++j)observe(sliders[i],sliders[j]);
        for(int slider:sliders)for(int edit=461;edit<=466;++edit)observe(slider,edit);
        for(auto& dc:native)dc.Close();
    }
    void Write(std::ostream& dcs,std::ostream& owners,std::ostream& samples)const{
        dcs<<"id,hwnd,hdc,opened,dc_type,map_mode,actual_visible,origin_ok,origin_x,origin_y,client0_screen_ok,client0_screen_x,client0_screen_y,"
            <<"lp0_to_dp_ok,lp0_device_x,lp0_device_y,origin_matches,clip_kind,clip_l,clip_t,clip_r,clip_b\n";
        for(const auto& i:dc)dcs<<i.id<<','<<Bits(i.window)<<','<<i.handle<<','<<i.opened<<','<<i.type<<','<<i.mapMode<<','<<i.visible<<','
            <<i.originOk<<','<<i.origin.x<<','<<i.origin.y<<','<<i.screenZeroOk<<','<<i.screenZero.x<<','<<i.screenZero.y<<','
            <<i.zeroMapped<<','<<i.deviceZero.x<<','<<i.deviceZero.y<<','<<i.originMatches<<','<<i.clipKind<<','
            <<i.clip.left<<','<<i.clip.top<<','<<i.clip.right<<','<<i.clip.bottom<<'\n';
        owners<<"first_id,second_id,higher_z_id,shared_l,shared_t,shared_r,shared_b,area,all_pixels_probed,"
            <<"first_mapped,second_mapped,first_ptvisible,second_ptvisible,status\n";
        for(const auto& o:overlaps)owners<<o.first<<','<<o.second<<','<<o.above<<','<<o.rect.left<<','<<o.rect.top<<','<<o.rect.right<<','
            <<o.rect.bottom<<','<<o.area<<','<<o.allPoints<<','<<o.firstMapped<<','<<o.secondMapped<<','<<o.firstVisible<<','<<o.secondVisible<<','
            <<(o.above&&o.allPoints&&o.firstMapped==o.area&&o.secondMapped==o.area?"CAPTURED":"UNAVAILABLE")<<'\n';
        samples<<"first_id,second_id,source_id,physical_x,physical_y,device_fits,device_x,device_y,dp_to_lp_ok,logical_x,logical_y,"
            <<"lp_to_dp_ok,roundtrip_device_x,roundtrip_device_y,exact_roundtrip,qualified,ptvisible\n";
        for(const auto& p:points)samples<<p.first<<','<<p.second<<','<<p.source<<','<<p.physical.x<<','<<p.physical.y<<','<<p.deviceFits<<','
            <<p.device.x<<','<<p.device.y<<','<<p.dpToLp<<','<<p.logical.x<<','<<p.logical.y<<','<<p.lpToDp<<','
            <<p.roundTrip.x<<','<<p.roundTrip.y<<','<<p.exact<<','<<p.qualified<<','<<p.visible<<'\n';
    }
};
}
inline Result Run(mikumikustudio::MMDApp& app,const std::filesystem::path& outputDirectory){
    namespace fs=std::filesystem;using namespace detail;
    fs::create_directories(outputDirectory);
    std::ofstream report(outputDirectory/L"light-slider-geometry.txt");
    std::ofstream rectangles(outputDirectory/L"light-slider-geometry.csv");
    std::ofstream pairs(outputDirectory/L"light-slider-intersections.csv");
    std::ofstream dcs(outputDirectory/L"light-slider-native-dc.csv");
    std::ofstream owners(outputDirectory/L"light-slider-overlap-owners.csv");
    std::ofstream samples(outputDirectory/L"light-slider-clip-points.csv");
    if(!report||!rectangles||!pairs||!dcs||!owners||!samples)throw std::runtime_error("open light slider geometry diagnostic output");
    const Api api;Result result;const auto main=app.MainWindow();
    const auto before=api.thread?api.thread():nullptr;
    const int awareness=before&&api.awareness?api.awareness(before):-1;
    report<<"Read-only actual HWND geometry; no layout/style/focus/capture/paint or scene writes.\n"
        <<"thread_id="<<GetCurrentThreadId()<<" caller_context="<<Bits(before)<<" caller_awareness="<<awareness<<'\n'
        <<"api_thread="<<bool(api.thread)<<" api_window="<<bool(api.window)<<" api_set="<<bool(api.set)
        <<" api_equal="<<bool(api.equal)<<" api_awareness="<<bool(api.awareness)<<" api_dpi="<<bool(api.dpi)<<'\n'
        <<"DPI/window API absence is UNAVAILABLE, never a passing physical-overlap check. "
        <<"GetClientRect is recorded in each caller context without claiming it is a physical screen rectangle. "
        <<"Only GetWindowRect under a verified per-monitor-aware thread is used for physical intersections.\n"
        <<"Parent-hidden fixtures can have WS_VISIBLE children: both own visibility and IsWindowVisible are recorded. "
        <<"Overlap counts include hidden controls; own-visible counts are separate. No overlap alone does not prove flicker absent.\n"
        <<"Native DC probes are read-only: GetDC/GetDCOrgEx/GetMapMode/LPtoDP/DPtoLP/GetClipBox/PtVisible only, no marker or pixel writes. "
        <<"A shared physical point qualifies only when the client origin agrees with ClientToScreen, DP/LP roundtrip is exact, "
        <<"and the source is actually visible. Unmappable/hidden/budget-limited strips are UNAVAILABLE, not inferred ownership. "
        <<"GetWindowRect intersection does not itself prove both client DCs can draw it.\n";
    rectangles<<"pass,id,hwnd,parent,owner_thread,caller_context,caller_awareness,window_context,window_awareness,window_dpi,"
        <<"style,extended,own_visible,is_window_visible,client_ok,client_l,client_t,client_r,client_b,"
        <<"window_ok,screen_l,screen_t,screen_r,screen_b\n";
    pairs<<"kind,first_id,second_id,both_own_visible,intersection_width,intersection_height,intersection_area,"
        <<"second_left_minus_first_right,second_top_minus_first_bottom\n";
    const auto caller=ReadGroup(api,main);
    for(const auto& g:caller)Row(rectangles,"caller",g,before,awareness);
    ClipData clips;
    PhysicalContext physical(api);
    Group measured{};
    if(physical.qualified){measured=ReadGroup(api,main);clips.Capture(measured);}
    const bool entered=physical.entered;
    physical.Restore();
    const auto after=api.thread?api.thread():nullptr;
    // Formatting, file I/O and intersection analysis run after restoring the
    // caller. The per-monitor scope contains only the actual window queries.
    report<<"pm_v2_error="<<physical.v2Error<<" pm_v1_error="<<physical.v1Error
        <<" selected_pm_version="<<physical.selected<<" active_context="<<Bits(physical.active)
        <<" exact_previous_handle="<<Bits(physical.previous)<<" physical_context_qualified="<<physical.qualified<<'\n';
    if(physical.qualified){
        const int currentAwareness=api.awareness(physical.active);
        for(const auto& g:measured){Row(rectangles,"physical",g,physical.active,currentAwareness);result.physicalRectangles+=g.screenOk;}
        if(Complete(caller)&&Complete(measured)){
            result.physical=PhysicalStatus::Captured;
            constexpr int sliders[]{447,455,456,457,458,459,460,560};
            for(unsigned i=0;i<std::size(sliders);++i)for(unsigned j=i+1;j<std::size(sliders);++j)
                Pair(pairs,"slider_peer",*Find(measured,sliders[i]),*Find(measured,sliders[j]),result);
            for(int slider:sliders)for(int edit=461;edit<=466;++edit)
                Pair(pairs,"slider_edit",*Find(measured,slider),*Find(measured,edit),result);
        }else result.physical=PhysicalStatus::Failed;
    }else if(physical.previous||!Complete(caller))result.physical=PhysicalStatus::Failed;
    result.contextRestored=entered?physical.restored:api.equal&&before&&after&&api.equal(before,after);
    if(entered&&!result.contextRestored)result.physical=PhysicalStatus::Failed;
    report<<"after_context="<<Bits(after)<<" exact_old_context_restored="<<result.contextRestored
        <<" restore_error="<<physical.restoreError<<'\n'
        <<"STATUS "<<(result.physical==PhysicalStatus::Captured?"CAPTURED":result.physical==PhysicalStatus::Failed?"FAILED":"UNAVAILABLE")
        <<" physical_rectangles="<<result.physicalRectangles<<" overlap_pairs="<<result.overlapPairs
        <<" own_visible_overlap_pairs="<<result.ownVisibleOverlapPairs<<'\n';
    clips.Write(dcs,owners,samples);
    report.flush();rectangles.flush();pairs.flush();dcs.flush();owners.flush();samples.flush();
    if(!report||!rectangles||!pairs||!dcs||!owners||!samples)throw std::runtime_error("write light slider geometry diagnostic output");
    return result;
}
} // namespace light_slider_geometry_checks
