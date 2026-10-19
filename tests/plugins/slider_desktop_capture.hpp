#pragma once
// Test-only, bounded slider-ROI capture during a slider or internal-layout drag. No UI messages
// are sent and no capture, focus, cursor, input or project state is changed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <thread>

namespace slider_desktop_capture {
class Recorder {
    static constexpr std::size_t kBytes=64u*1024u*1024u,kPixels=kBytes/sizeof(std::uint32_t),kFrames=4096;
    static constexpr int kWidth=512,kHeight=128;
    inline static constexpr wchar_t kOwnerProperty[]=L"MMS.Test.SliderDesktopCapture.Owner.v1";
    enum Pause : unsigned {Foreground,NoCapture,ForeignCapture,Hidden,Target,Geometry,Offscreen,
        CoordinateApi,DesktopDc,DesktopCalibration,OcclusionEnumeration,Occluded,ForeignPoint,
        ChangedAfterCopy,CopyFailed,StoppedAfterCopy,RestoreContext,ClockUnavailable,
        LayoutSignalInactive,NoVisibleLayoutTarget,PauseCount};
    enum class End {NotStarted,Stopped,TimeLimit,PixelLimit,FrameLimit,DpiUnavailable,SetupFailed,Exception};
    struct Frame {
        LONGLONG begin=0,end=0;std::uint64_t sequenceBefore=0,sequenceAfter=0;
        RECT rectangle{};HWND window=nullptr;int targetId=0;std::uint32_t offset=0,pixels=0,bright=0;
        HWND captureBefore=nullptr,captureAfter=nullptr;bool layoutMode=false,layoutBefore=false,layoutAfter=false;
    };
    struct CaptureState {HWND window=nullptr;bool layoutSignal=false,layoutMode=false;};
    struct Dpi {
        using Set=HANDLE(WINAPI*)(HANDLE);using Get=HANDLE(WINAPI*)();using Equal=BOOL(WINAPI*)(HANDLE,HANDLE);
        Set set=nullptr;Get get=nullptr;Equal equal=nullptr;HANDLE previous=nullptr;int selected=0;
        std::uint64_t* restoreFailures=nullptr;
        explicit Dpi(std::uint64_t& failures)noexcept:restoreFailures(&failures){
            const auto user=GetModuleHandleW(L"user32.dll");if(!user)return;
            set=reinterpret_cast<Set>(GetProcAddress(user,"SetThreadDpiAwarenessContext"));
            get=reinterpret_cast<Get>(GetProcAddress(user,"GetThreadDpiAwarenessContext"));
            equal=reinterpret_cast<Equal>(GetProcAddress(user,"AreDpiAwarenessContextsEqual"));
            if(!set||!get||!equal)return;
            for(int value:{-4,-3}){
                const auto desired=reinterpret_cast<HANDLE>(static_cast<INT_PTR>(value));
                const auto old=set(desired);if(!old)continue;
                if(equal(get(),desired)){previous=old;selected=value;return;}
                if(!set(old)){++*restoreFailures;return;}
            }
        }
        ~Dpi(){if(previous&&!set(previous))++*restoreFailures;}
        Dpi(const Dpi&)=delete;Dpi& operator=(const Dpi&)=delete;
    };
    struct Surface {
        HDC desktop=nullptr,memory=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;std::uint32_t* pixels=nullptr;
        ~Surface(){if(memory&&previous&&previous!=HGDI_ERROR)SelectObject(memory,previous);
            if(memory)DeleteDC(memory);if(bitmap)DeleteObject(bitmap);if(desktop)ReleaseDC(nullptr,desktop);}
        bool Prepare()noexcept{
            desktop=GetDC(nullptr);if(!desktop)return false;
            memory=CreateCompatibleDC(desktop);if(!memory)return false;
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=kWidth;
            info.bmiHeader.biHeight=-kHeight;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            bitmap=CreateDIBSection(desktop,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
            if(!bitmap||!pixels)return false;previous=SelectObject(memory,bitmap);
            return previous&&previous!=HGDI_ERROR;
        }
        void Clear()noexcept{if(pixels)std::memset(pixels,0,std::size_t(kWidth)*kHeight*sizeof(*pixels));}
    };
    struct Calibration {int width=0,height=0,desktopWidth=0,desktopHeight=0,metricsWidth=0,metricsHeight=0;};
    struct Above {
        HWND main=nullptr;RECT roi{};bool found=false,occluded=false,failed=false;unsigned visited=0;
        static BOOL CALLBACK Visit(HWND window,LPARAM data)noexcept{
            auto& self=*reinterpret_cast<Above*>(data);
            if(window==self.main){self.found=true;return FALSE;}
            if(++self.visited>4096){self.failed=true;return FALSE;}
            if(!IsWindowVisible(window)||IsIconic(window))return TRUE;
            RECT rectangle{},intersection{};
            if(!GetWindowRect(window,&rectangle)){self.failed=true;return FALSE;}
            // Reject even transparent, layered or same-process top-level
            // windows above the host. No window title or pixels are read.
            if(IntersectRect(&intersection,&rectangle,&self.roi)){self.occluded=true;return FALSE;}
            return TRUE;
        }
    };
    HWND main_;DWORD uiThread_,process_=GetCurrentProcessId();const std::array<HWND,13> sliders_;
    std::array<int,13> ids_{};const std::atomic<std::uint64_t>* sequence_;const std::atomic<bool>* layoutDrag_;
    std::filesystem::path directory_;std::unique_ptr<std::uint32_t[]> pixels_;std::unique_ptr<Frame[]> frames_;
    std::thread worker_;HANDLE stopEvent_=nullptr;std::atomic<bool> stop_{false},done_{true};
    bool started_=false,finished_=false,markersReady_=false,startAttempted_=false;std::size_t frameCount_=0,pixelCount_=0;
    std::array<std::uint64_t,PauseCount> pauses_{};End ended_=End::NotStarted;
    LONGLONG frequency_=0,startedQpc_=0,finishedQpc_=0;ULONGLONG startedTick_=0;int dpiMode_=0;DWORD workerError_=0;
    Calibration calibration_{};unsigned nextLayoutTarget_=0;
    static bool Trackbar(HWND window)noexcept{
        wchar_t name[64]{};return GetClassNameW(window,name,64)>0&&!lstrcmpiW(name,L"msctls_trackbar32");
    }
    bool Marked(unsigned index)const noexcept{
        const auto window=sliders_[index];
        return window&&GetPropW(window,kOwnerProperty)==reinterpret_cast<HANDLE>(const_cast<Recorder*>(this))&&
            GetDlgCtrlID(window)==ids_[index]&&Trackbar(window);
    }
    void RemoveMarkers()noexcept{
        for(const auto window:sliders_)if(window&&IsWindow(window)&&
            GetPropW(window,kOwnerProperty)==reinterpret_cast<HANDLE>(this))RemovePropW(window,kOwnerProperty);
        markersReady_=false;
    }
    void MarkWindows()noexcept{
        if(GetCurrentThreadId()!=uiThread_)return;
        for(unsigned i=0;i<sliders_.size();++i){const auto window=sliders_[i];DWORD process=0;
            if(!window||!ids_[i]||GetWindowThreadProcessId(window,&process)!=uiThread_||process!=process_||
                GetAncestor(window,GA_ROOT)!=main_||!Trackbar(window)||GetPropW(window,kOwnerProperty)||
                !SetPropW(window,kOwnerProperty,reinterpret_cast<HANDLE>(this))){RemoveMarkers();return;}}
        markersReady_=true;
    }
    static LONGLONG Qpc()noexcept{LARGE_INTEGER value{};return QueryPerformanceCounter(&value)?value.QuadPart:0;}
    bool TimeExpired()const noexcept{return GetTickCount64()-startedTick_>=30000||Qpc()-startedQpc_>=frequency_*30;}
    bool StopRequested()const noexcept{return stop_.load(std::memory_order_relaxed);}
    void PauseFor(Pause reason)noexcept{++pauses_[reason];}
    static bool Identity(HDC dc)noexcept{
        if(GetObjectType(dc)!=OBJ_DC||GetMapMode(dc)!=MM_TEXT||(GetLayout(dc)&LAYOUT_RTL))return false;
        POINT device{},viewport{},window{};SIZE ve{},we{};
        if(!GetDCOrgEx(dc,&device)||device.x||device.y||!GetViewportOrgEx(dc,&viewport)||viewport.x||viewport.y||
            !GetWindowOrgEx(dc,&window)||window.x||window.y||!GetViewportExtEx(dc,&ve)||!GetWindowExtEx(dc,&we)||
            ve.cx!=we.cx||ve.cy!=we.cy||!ve.cx||!ve.cy)return false;
        const int mode=GetGraphicsMode(dc);if(mode!=GM_COMPATIBLE&&mode!=GM_ADVANCED)return false;
        if(mode==GM_ADVANCED){XFORM x{};if(!GetWorldTransform(dc,&x)||x.eM11!=1||x.eM12||x.eM21||x.eM22!=1||x.eDx||x.eDy)return false;}
        POINT points[]={{0,0},{17,19},{511,127}};
        return LPtoDP(dc,points,3)&&points[0].x==0&&points[0].y==0&&points[1].x==17&&points[1].y==19&&points[2].x==511&&points[2].y==127;
    }
    bool Calibrate(HDC desktop,Calibration& result)noexcept{
        result={};
        if(!desktop||!Identity(desktop)||GetSystemMetrics(SM_CMONITORS)!=1){PauseFor(DesktopCalibration);return false;}
        result.width=GetDeviceCaps(desktop,HORZRES);result.height=GetDeviceCaps(desktop,VERTRES);
        result.desktopWidth=GetDeviceCaps(desktop,DESKTOPHORZRES);result.desktopHeight=GetDeviceCaps(desktop,DESKTOPVERTRES);
        result.metricsWidth=GetSystemMetrics(SM_CXSCREEN);result.metricsHeight=GetSystemMetrics(SM_CYSCREEN);
        if(result.width<=0||result.height<=0||result.width!=result.desktopWidth||result.height!=result.desktopHeight||
            result.width!=result.metricsWidth||result.height!=result.metricsHeight){PauseFor(DesktopCalibration);return false;}
        return true;
    }
    bool VisibleTarget(unsigned index)const noexcept{
        DWORD process=0;const auto window=sliders_[index];
        return Marked(index)&&GetWindowThreadProcessId(window,&process)==uiThread_&&process==process_&&
            GetAncestor(window,GA_ROOT)==main_&&IsWindowVisible(window);
    }
    bool ReadCapture(CaptureState& state)noexcept{
        state={};
        if(GetForegroundWindow()!=main_){PauseFor(Foreground);return false;}
        DWORD process=0;
        if(!IsWindow(main_)||!IsWindowVisible(main_)||IsIconic(main_)||
            GetWindowThreadProcessId(main_,&process)!=uiThread_||process!=process_){PauseFor(Hidden);return false;}
        GUITHREADINFO gui{};gui.cbSize=sizeof(gui);
        if(!GetGUIThreadInfo(uiThread_,&gui)){PauseFor(Target);return false;}
        if(!gui.hwndCapture){PauseFor(NoCapture);return false;}
        state.window=gui.hwndCapture;state.layoutSignal=layoutDrag_&&layoutDrag_->load(std::memory_order_acquire);
        if(state.window==main_){
            if(!state.layoutSignal){PauseFor(LayoutSignalInactive);return false;}
            state.layoutMode=true;return true;
        }
        for(unsigned i=0;i<sliders_.size();++i)if(sliders_[i]&&gui.hwndCapture==sliders_[i]){
            if(!VisibleTarget(i)){PauseFor(Target);return false;}
            return true;
        }
        PauseFor(ForeignCapture);return false;
    }
    int CaptureTarget(CaptureState& state)noexcept{
        if(!ReadCapture(state))return -1;
        if(state.layoutMode){
            // Advance only on initial selection. Rechecks retain this same
            // target and never rotate to another control during a copy.
            for(unsigned offset=0;offset<sliders_.size();++offset){
                const auto index=(nextLayoutTarget_+offset)%unsigned(sliders_.size());
                if(VisibleTarget(index)){nextLayoutTarget_=(index+1)%unsigned(sliders_.size());return int(index);}
            }
            PauseFor(NoVisibleLayoutTarget);return -1;
        }
        for(unsigned i=0;i<sliders_.size();++i)if(state.window==sliders_[i])return int(i);
        PauseFor(Target);return -1;
    }
    bool SameTarget(unsigned target,const CaptureState& before,CaptureState& after)noexcept{
        return ReadCapture(after)&&after.window==before.window&&after.layoutMode==before.layoutMode&&
            after.layoutSignal==before.layoutSignal&&VisibleTarget(target)&&
            (before.layoutMode||after.window==sliders_[target]);
    }
    bool OcclusionFree(const RECT& roi)noexcept{
        Above above;above.main=main_;above.roi=roi;
        const BOOL enumerated=EnumWindows(Above::Visit,reinterpret_cast<LPARAM>(&above));
        if(above.occluded){PauseFor(Occluded);return false;}
        if(above.failed||!above.found||enumerated){PauseFor(OcclusionEnumeration);return false;}
        const LONG xs[]={roi.left,(roi.left+roi.right-1)/2,roi.right-1};
        const LONG ys[]={roi.top,(roi.top+roi.bottom-1)/2,roi.bottom-1};
        for(const auto y:ys)for(const auto x:xs){const auto window=WindowFromPoint(POINT{x,y});DWORD process=0;
            if(!window||GetAncestor(window,GA_ROOT)!=main_||!GetWindowThreadProcessId(window,&process)||process!=process_){
                PauseFor(ForeignPoint);return false;}}
        return true;
    }
    bool Rectangle(HWND target,const Calibration& c,RECT& roi)noexcept{
        if(!GetWindowRect(target,&roi)){PauseFor(Geometry);return false;}
        const auto width=std::int64_t(roi.right)-roi.left,height=std::int64_t(roi.bottom)-roi.top;
        if(width<=0||height<=0||width>kWidth||height>kHeight){PauseFor(Geometry);return false;}
        if(roi.left<0||roi.top<0||roi.right>c.width||roi.bottom>c.height){PauseFor(Offscreen);return false;}
        return true;
    }
    void Sample(Surface& surface)noexcept{
        CaptureState captureBefore{},captureAfter{};const int target=CaptureTarget(captureBefore);if(target<0)return;
        Calibration before{};if(!Calibrate(surface.desktop,before))return;calibration_=before;
        RECT roi{};if(!Rectangle(sliders_[target],before,roi)||!OcclusionFree(roi))return;
        // Recheck after enumeration, immediately before the only screen copy.
        if(!SameTarget(unsigned(target),captureBefore,captureAfter)){PauseFor(ChangedAfterCopy);return;}
        const int width=roi.right-roi.left,height=roi.bottom-roi.top;
        const auto count=std::size_t(width)*height;
        if(count>kPixels-pixelCount_){ended_=End::PixelLimit;stop_.store(true,std::memory_order_relaxed);return;}
        Frame frame;frame.begin=Qpc();frame.sequenceBefore=sequence_?sequence_->load(std::memory_order_relaxed):0;
        if(!frame.begin){PauseFor(ClockUnavailable);return;}
        if(!BitBlt(surface.memory,0,0,width,height,surface.desktop,roi.left,roi.top,SRCCOPY)||!GdiFlush()){
            PauseFor(CopyFailed);surface.Clear();return;}
        Calibration after{};RECT current{};
        // No screenshot enters the retained store until every post-copy gate
        // passes. Rejected staging pixels are cleared and never written out.
        if(!SameTarget(unsigned(target),captureBefore,captureAfter)||!Calibrate(surface.desktop,after)||
            before.width!=after.width||before.height!=after.height||!Rectangle(sliders_[target],after,current)||
            !EqualRect(&roi,&current)||!OcclusionFree(roi)||!SameTarget(unsigned(target),captureBefore,captureAfter)){
            PauseFor(ChangedAfterCopy);surface.Clear();return;}
        frame.end=Qpc();frame.sequenceAfter=sequence_?sequence_->load(std::memory_order_relaxed):0;
        if(!frame.end||frame.end<frame.begin){PauseFor(ClockUnavailable);surface.Clear();return;}
        if(StopRequested()||TimeExpired()){PauseFor(StoppedAfterCopy);surface.Clear();return;}
        frame.window=sliders_[target];frame.targetId=ids_[target];frame.rectangle=roi;
        frame.captureBefore=captureBefore.window;frame.captureAfter=captureAfter.window;frame.layoutMode=captureBefore.layoutMode;
        frame.layoutBefore=captureBefore.layoutSignal;frame.layoutAfter=captureAfter.layoutSignal;
        frame.offset=std::uint32_t(pixelCount_);frame.pixels=std::uint32_t(count);
        for(int y=0;y<height;++y){const auto* row=surface.pixels+std::size_t(y)*kWidth;
            std::memcpy(pixels_.get()+pixelCount_+std::size_t(y)*width,row,std::size_t(width)*sizeof(*row));
            for(int x=0;x<width;++x){const auto pixel=row[x];frame.bright+=(pixel&255)>=240&&((pixel>>8)&255)>=240&&((pixel>>16)&255)>=240;}}
        frames_[frameCount_++]=frame;pixelCount_+=count;
        if(frameCount_==kFrames){ended_=End::FrameLimit;stop_.store(true,std::memory_order_relaxed);}
    }
    void Capture()noexcept{
        Dpi dpi(pauses_[RestoreContext]);dpiMode_=dpi.selected;
        if(!dpi.selected){PauseFor(CoordinateApi);ended_=End::DpiUnavailable;return;}
        Surface surface;
        if(!surface.Prepare()){workerError_=GetLastError();PauseFor(DesktopDc);ended_=End::SetupFailed;return;}
        const LONGLONG period=(frequency_*8/1000)>0?frequency_*8/1000:1;
        LONGLONG next=Qpc();
        while(!StopRequested()&&!TimeExpired()){
            if(frameCount_==kFrames){ended_=End::FrameLimit;break;}
            Sample(surface);
            if(StopRequested()||TimeExpired())break;
            // Target an 8ms start-to-start period, accounting for capture work.
            // Do not spin through a backlog after a slow driver call. Actual
            // QPC timestamps, not the OS timeout granularity, are authoritative.
            next+=period;const auto now=Qpc();if(now>next+period)next=now;
            const auto remaining=next>now?next-now:0;
            const auto timeout=remaining?DWORD((remaining*1000+frequency_-1)/frequency_):DWORD(0);
            WaitForSingleObject(stopEvent_,timeout>8?8:timeout);
        }
        if(ended_==End::NotStarted)ended_=TimeExpired()?End::TimeLimit:End::Stopped;
    }
    static const char* EndName(End value)noexcept{
        switch(value){case End::NotStarted:return "not-started";case End::Stopped:return "stopped";
        case End::TimeLimit:return "30-second-limit";case End::PixelLimit:return "64-MiB-limit";case End::FrameLimit:return "4096-frame-limit";
        case End::DpiUnavailable:return "physical-DPI-API-unavailable";case End::SetupFailed:return "GDI-setup-failed";default:return "worker-exception";}}
    void WriteBitmap(const std::filesystem::path& path,const Frame& frame)const{
        BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);
        file.bfSize=file.bfOffBits+DWORD(frame.pixels*sizeof(std::uint32_t));info.biSize=sizeof(info);
        info.biWidth=frame.rectangle.right-frame.rectangle.left;info.biHeight=-(frame.rectangle.bottom-frame.rectangle.top);
        info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
        std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(&file),sizeof(file));
        f.write(reinterpret_cast<const char*>(&info),sizeof(info));
        f.write(reinterpret_cast<const char*>(pixels_.get()+frame.offset),std::streamsize(frame.pixels*sizeof(std::uint32_t)));
        if(!f)throw std::runtime_error("write bounded slider desktop BMP");
    }
public:
    // Optional signals and counters must outlive Stop/join, as must the HWND
    // identity markers. No MMDApp state is read from the worker thread.
    Recorder(HWND main,DWORD uiThread,std::array<HWND,13> sliders,const std::atomic<std::uint64_t>* sequence=nullptr,
        const std::atomic<bool>* layoutDrag=nullptr):
        main_(main),uiThread_(uiThread),sliders_(sliders),sequence_(sequence),layoutDrag_(layoutDrag){
        for(unsigned i=0;i<sliders_.size();++i)ids_[i]=sliders_[i]?GetDlgCtrlID(sliders_[i]):0;
        MarkWindows();
    }
    Recorder(const Recorder&)=delete;Recorder& operator=(const Recorder&)=delete;
    ~Recorder(){Stop();RemoveMarkers();if(stopEvent_)CloseHandle(stopEvent_);}
    bool Start(const std::filesystem::path& path){
        if(started_||startAttempted_)return false;startAttempted_=true;
        const auto fail=[this]()noexcept{if(stopEvent_){CloseHandle(stopEvent_);stopEvent_=nullptr;}
            pixels_.reset();frames_.reset();RemoveMarkers();done_.store(true,std::memory_order_release);return false;};
        if(GetCurrentThreadId()!=uiThread_||!markersReady_)return fail();
        for(unsigned i=0;i<sliders_.size();++i)if(!Marked(i))return fail();
        try{
            directory_=path;pixels_=std::make_unique<std::uint32_t[]>(kPixels);frames_=std::make_unique<Frame[]>(kFrames);
            stopEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stopEvent_)return fail();
            LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)return fail();
            frequency_=frequency.QuadPart;startedQpc_=Qpc();startedTick_=GetTickCount64();if(!startedQpc_)return fail();
            stop_.store(false,std::memory_order_relaxed);done_.store(false,std::memory_order_relaxed);
            worker_=std::thread([this]()noexcept{
                try{Capture();}catch(...){ended_=End::Exception;workerError_=GetLastError();}
                finishedQpc_=Qpc();done_.store(true,std::memory_order_release);
            });
            started_=true;return true;
        }catch(...){return fail();}
    }
    void Stop()noexcept{
        stop_.store(true,std::memory_order_relaxed);if(stopEvent_)SetEvent(stopEvent_);
        // The worker has no callbacks into Recorder's owner and never invokes
        // Stop/Finish. These lifecycle functions belong to the owning thread.
        if(worker_.joinable()){
            try{worker_.join();}catch(...){std::terminate();}
        }
    }
    bool Done()const noexcept{return done_.load(std::memory_order_acquire);}
    void Finish(){
        Stop();if(!started_)throw std::runtime_error("slider desktop recorder did not start");if(finished_)return;
        std::filesystem::create_directories(directory_);
        std::ofstream csv(directory_/"desktop-frames.csv"),summary(directory_/"desktop-summary.txt");
        if(!csv||!summary)throw std::runtime_error("open slider desktop capture reports");
        csv<<"# bounded registered-slider ROI only; slider capture or main capture plus atomic layout-drag signal; one round-robin ROI per layout sample; requested interval 8ms; QPC timestamps authoritative; bright means every RGB channel >=240; no DWM/flicker verdict\n"
            <<"frame,qpc_begin,qpc_end,target_id,hwnd,left,top,right,bottom,sequence_before,sequence_after,pixels,bright_pixels,bright_ratio,mode,layout_signal_before,layout_signal_after,actual_capture_before,actual_capture_after\n";
        std::size_t layoutFrames=0;
        for(std::size_t i=0;i<frameCount_;++i){const auto& frame=frames_[i];
            csv<<i<<','<<frame.begin<<','<<frame.end<<','<<frame.targetId<<','<<reinterpret_cast<std::uintptr_t>(frame.window)<<','
                <<frame.rectangle.left<<','<<frame.rectangle.top<<','<<frame.rectangle.right<<','<<frame.rectangle.bottom<<','
                <<frame.sequenceBefore<<','<<frame.sequenceAfter<<','<<frame.pixels<<','<<frame.bright<<','<<double(frame.bright)/frame.pixels
                <<','<<(frame.layoutMode?"layout-drag":"slider-drag")<<','<<frame.layoutBefore<<','<<frame.layoutAfter
                <<','<<reinterpret_cast<std::uintptr_t>(frame.captureBefore)<<','<<reinterpret_cast<std::uintptr_t>(frame.captureAfter)<<'\n';
            layoutFrames+=frame.layoutMode;
            char name[80]{};std::snprintf(name,sizeof(name),"frame-%04u-id-%d.bmp",unsigned(i),frame.targetId);WriteBitmap(directory_/name,frame);
        }
        summary<<"status="<<(frameCount_?"observed":"not-observed")<<"\nend="<<EndName(ended_)<<"\nframes="<<frameCount_
            <<"\nlayout_signal_configured="<<(layoutDrag_!=nullptr)<<"\nlayout_drag_frames="<<layoutFrames<<"\nslider_drag_frames="<<frameCount_-layoutFrames
            <<"\npixel_bytes="<<pixelCount_*sizeof(std::uint32_t)<<"\nqpc_frequency="<<frequency_<<"\nqpc_started="<<startedQpc_
            <<"\nqpc_finished="<<finishedQpc_<<"\nworker_error="<<workerError_<<"\nworker_dpi_context="<<dpiMode_
            <<"\ncalibrated_HORZRES="<<calibration_.width<<"\ncalibrated_VERTRES="<<calibration_.height
            <<"\ncalibrated_DESKTOPHORZRES="<<calibration_.desktopWidth<<"\ncalibrated_DESKTOPVERTRES="<<calibration_.desktopHeight
            <<"\ncalibrated_primary_width="<<calibration_.metricsWidth<<"\ncalibrated_primary_height="<<calibration_.metricsHeight<<'\n';
        static constexpr const char* names[]={"foreground-not-main","no-UI-thread-capture","capture-not-listed-slider","host-hidden-or-iconic",
            "invalid-target-or-thread","ROI-geometry-unavailable","ROI-outside-primary","physical-DPI-API-unavailable","desktop-DC-setup-failed",
            "desktop-coordinate-calibration-unavailable","top-level-enumeration-unavailable","upper-window-intersects-ROI","nine-point-not-owned-by-MMS",
            "post-copy-or-pre-copy-state-changed","ROI-copy-failed","stopped-before-retaining-copy","worker-DPI-context-restore-failed","QPC-unavailable",
            "main-capture-without-layout-drag-signal","no-visible-registered-layout-slider"};
        static_assert(sizeof(names)/sizeof(names[0])==PauseCount,"every pause has an output name");
        for(unsigned i=0;i<PauseCount;++i)summary<<"pause."<<names[i]<<'='<<pauses_[i]<<'\n';
        summary<<"No frame means not-observed, never PASS or no-flicker. Captured frames are observations only.\n"
            <<"Only one registered slider ROI is copied per sample. Slider-drag mode requires its own capture; layout-drag mode requires main HWND capture plus the supplied atomic signal and rotates among currently visible registered sliders. The selected target is fixed for all before/after checks.\n"
            <<"Actual capture, layout signal, target identity/visibility, foreground, geometry, nine MMS-owned points and every visible upper top-level intersection are checked before and after each copy. No parent/window-wide ROI is recorded.\n"
            <<"These are point-in-time guards; changes wholly between checks cannot be ruled out. Rejected staging pixels are cleared and are never persisted.\n"
            <<"No file I/O runs in the capture thread. Finish joins before writing. No focus, cursor, key, capture or mutable MMDApp state is modified.\n";
        if(!csv||!summary)throw std::runtime_error("finish slider desktop capture reports");finished_=true;
    }
};
} // namespace slider_desktop_capture
