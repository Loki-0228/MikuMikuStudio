// Opt-in product-DPI layout observation. Not CTest and not a no-flicker test.
// Include after slider_user_capture_checks and layout_slider_repaint_checks.
#include "mikudancestudio/update_coordinator.hpp"
#include "mikudancestudio/animation_curve_host.hpp"
#include <iterator>
#include <thread>

#if !MMD_ORIGINAL_PROFILE
namespace layout_product_dpi_checks {
namespace base=layout_slider_repaint_checks;
namespace trace=slider_paint_trace;
using mikudancestudio::MMDApp;
inline constexpr std::array<int,19> kIcons{453,454,469,470,566,567,488,489,502,503,528,529,530,
    mikudancestudio::curve_editor::kInterpolationCurve,mikudancestudio::curve_editor::kAccessoryCurve,
    mikudancestudio::curve_host::kMorphCurveLane[0],mikudancestudio::curve_host::kMorphCurveLane[1],
    mikudancestudio::curve_host::kMorphCurveLane[2],mikudancestudio::curve_host::kMorphCurveLane[3]};
struct Incomplete:std::runtime_error{using std::runtime_error::runtime_error;};
inline LONGLONG Now(){LARGE_INTEGER value{};QueryPerformanceCounter(&value);return value.QuadPart;}
inline void Guard(MMDApp& app,bool gesture){
    const auto capture=GetCapture();
    if(capture&&(!gesture||capture!=app.MainWindow()))throw Incomplete("foreign or unexpected capture");
    BYTE state[256]{};
    if(!GetKeyboardState(state))throw Incomplete("keyboard state unavailable");
    for(int key=1;key<256;++key)if((state[key]&0x80)||(GetAsyncKeyState(key)&0x8000))throw Incomplete("real held key or mouse button");
    MSG message{};
    // Capture can enqueue a synthetic OS WM_MOUSEMOVE without a held button.
    // Leave movement queued and unconsumed; button/wheel/keyboard input aborts.
    for(auto range:{std::pair<UINT,UINT>{WM_KEYFIRST,WM_KEYLAST},{WM_LBUTTONDOWN,WM_MOUSELAST}})
        if(PeekMessageW(&message,nullptr,range.first,range.second,PM_NOREMOVE))
            throw Incomplete(message.message==WM_QUIT?"queued quit left untouched":"queued real input left untouched");
    if(PeekMessageW(&message,app.MainWindow(),WM_CLOSE,WM_CLOSE,PM_NOREMOVE))throw Incomplete("queued close left untouched");
}
struct PresentEvent{LONGLONG begin=0,end=0;unsigned phase=0;RECT source{},target{};bool hasSource=false,hasTarget=false;HWND window=nullptr;HRESULT result=E_FAIL;};
class PresentTrace{
    using Function=HRESULT(WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
    inline static PresentTrace* active=nullptr;
    Function original=nullptr;void* address=nullptr;bool created=false,enabled=false;
public:
    IDirect3DDevice9* device;unsigned phase=0;std::array<PresentEvent,128> events{};unsigned count=0,overflow=0;
    explicit PresentTrace(IDirect3DDevice9* value):device(value){
        trace::Require(device&&!active,"one real D3D presentation observer");address=(*reinterpret_cast<void***>(device))[17];
        trace::Require(MH_CreateHook(address,reinterpret_cast<void*>(Call),reinterpret_cast<void**>(&original))==MH_OK,"create presentation observer");
        created=true;active=this;
        if(MH_EnableHook(address)!=MH_OK){Close();throw std::runtime_error("enable presentation observer");}enabled=true;
    }
    ~PresentTrace(){Close();}
    void Close()noexcept{if(enabled)MH_DisableHook(address);if(created)MH_RemoveHook(address);enabled=created=false;if(active==this)active=nullptr;}
    static HRESULT WINAPI Call(IDirect3DDevice9* device,const RECT* source,const RECT* target,HWND window,const RGNDATA* dirty){
        auto& self=*active;PresentEvent* event=nullptr;
        if(device==self.device){if(self.count<self.events.size()){event=&self.events[self.count++];event->begin=Now();event->phase=self.phase;
            event->hasSource=source!=nullptr;event->hasTarget=target!=nullptr;if(source)event->source=*source;if(target)event->target=*target;event->window=window;}else ++self.overflow;}
        const auto result=self.original(device,source,target,window,dirty);
        if(event){event->end=Now();event->result=result;}return result;
    }
};
struct VisualState{
    LONG_PTR style=0,extended=0;LRESULT ui=0,button=0;bool enabled=false,focused=false,isButton=false;
    bool Same(const VisualState& other)const noexcept{
        return style==other.style&&extended==other.extended&&ui==other.ui&&button==other.button&&
            enabled==other.enabled&&focused==other.focused&&isButton==other.isButton;
    }
};
inline VisualState Visual(HWND window){
    VisualState state;state.style=GetWindowLongPtrW(window,GWL_STYLE);state.extended=GetWindowLongPtrW(window,GWL_EXSTYLE);
    state.enabled=IsWindowEnabled(window)!=FALSE;state.focused=GetFocus()==window;state.ui=SendMessageW(window,WM_QUERYUISTATE,0,0);
    wchar_t name[32]{};state.isButton=GetClassNameW(window,name,32)&&!_wcsicmp(name,L"BUTTON");
    if(state.isButton)state.button=SendMessageW(window,BM_GETSTATE,0,0);return state;
}
inline std::size_t White(const trace::Snapshot& image)noexcept{
    std::size_t count=0;for(auto pixel:image.pixels)count+=(pixel&0x00ffffffu)==0x00ffffffu;return count;
}
inline bool Slider(int id)noexcept{return std::find(base::kIds.begin(),base::kIds.end(),id)!=base::kIds.end();}
inline bool CurveLauncher(int id)noexcept{
    return id==int(mikudancestudio::curve_editor::kInterpolationCurve)||id==int(mikudancestudio::curve_editor::kAccessoryCurve)||
        std::find(std::begin(mikudancestudio::curve_host::kMorphCurveLane),std::end(mikudancestudio::curve_host::kMorphCurveLane),unsigned(id))!=std::end(mikudancestudio::curve_host::kMorphCurveLane);
}
struct Capture{unsigned phase=0;int id=0;std::string kind,status;RECT bounds{};LONGLONG begin=0,end=0;trace::Snapshot image;unsigned nativePaints=0,nativeErases=0;LRESULT ui=0;VisualState visual;};
class DirectDrawTrace{
    struct DcPixels{
        std::uintptr_t dc=0,owner=0;DWORD type=0;int map=0,clipKind=ERROR;RECT clip{},roi{};
        POINT origin{},viewport{},windowOrigin{};bool originKnown=false,viewportKnown=false,windowOriginKnown=false;
        const char* status="not_sampled";trace::Snapshot image;bool baselineKnown=false,baselineCompared=false,baselineFull=false;
        std::size_t newWhite=0,lostWhite=0,changed=0;
    };
    struct Event{
        unsigned sequence=0,parent=0,phase=0,depth=0;HWND window=nullptr;int id=0;UINT message=0;
        LONGLONG begin=0,beforeCopied=0,returned=0,afterCopied=0;LRESULT result=0;DWORD error=0;
        UINT ctlType=0,ctlId=0,itemAction=0,itemState=0;RECT item{};bool relayout=false,dragging=false;
        bool positionKnown=false;WINDOWPOS position{};unsigned sizeType=0;int sizeWidth=0,sizeHeight=0;
        const char* source="supplied-message-dc";DcPixels before,after;
    };
    struct Baseline{HWND window=nullptr;trace::Snapshot image;VisualState visual;};
    static constexpr unsigned kCapacity=1024;
    MMDApp& app;std::vector<HWND> windows;std::unique_ptr<Event[]> events=std::make_unique<Event[]>(kCapacity);
    unsigned active=0,depth=0;bool sampling=false,closed=false;std::vector<Baseline> baselines;
    DcPixels Pixels(HWND window,HDC dc,const RECT& requested){
        DcPixels value;value.dc=reinterpret_cast<std::uintptr_t>(dc);value.type=dc?GetObjectType(dc):0;
        value.owner=reinterpret_cast<std::uintptr_t>(dc?WindowFromDC(dc):nullptr);
        value.map=dc?GetMapMode(dc):0;value.clipKind=dc?GetClipBox(dc,&value.clip):ERROR;
        value.originKnown=dc&&GetDCOrgEx(dc,&value.origin);value.viewportKnown=dc&&GetViewportOrgEx(dc,&value.viewport);
        value.windowOriginKnown=dc&&GetWindowOrgEx(dc,&value.windowOrigin);
        value.status="not_display_dc";if(value.type!=OBJ_DC)return value;
        value.status="source_window_mismatch";if(reinterpret_cast<HWND>(value.owner)!=window)return value;
        value.status="non_identity_mapping";if(value.map!=MM_TEXT||!value.viewportKnown||!value.windowOriginKnown||
            value.viewport.x||value.viewport.y||value.windowOrigin.x||value.windowOrigin.y)return value;
        RECT client{},own{};value.status="unbounded_or_complex_clip";
        if(!IsWindowVisible(window)||!GetClientRect(window,&client)||value.clipKind!=SIMPLEREGION||
            !IntersectRect(&own,&client,&requested)||!IntersectRect(&value.roi,&own,&value.clip))return value;
        const int width=value.roi.right-value.roi.left,height=value.roi.bottom-value.roi.top;
        if(width<=0||height<=0||width>512||height>128)return value;
        trace::Dib dib(width,height,dc);value.status="source_copy_failed";
        if(dib&&BitBlt(dib.dc,0,0,width,height,dc,value.roi.left,value.roi.top,SRCCOPY)&&GdiFlush()){
            auto& image=value.image;image.window=window;image.id=GetDlgCtrlID(window);image.width=width;image.height=height;image.visible=true;
            image.pixels.assign(dib.pixels,dib.pixels+std::size_t(width)*height);image.copied=true;value.status="observed";
            const auto baseline=std::find_if(baselines.begin(),baselines.end(),[window](const auto& b){return b.window==window;});
            value.baselineKnown=baseline!=baselines.end();
            if(value.baselineKnown&&baseline->image.copied&&baseline->image.Varied()&&baseline->image.width==client.right&&baseline->image.height==client.bottom&&baseline->visual.Same(Visual(window))){
                value.baselineCompared=true;
                value.baselineFull=value.roi.left==0&&value.roi.top==0&&value.roi.right==client.right&&value.roi.bottom==client.bottom;
                for(int y=0;y<height;++y)for(int x=0;x<width;++x){
                    const auto before=baseline->image.pixels[std::size_t(y+value.roi.top)*baseline->image.width+x+value.roi.left]&0xffffffu;
                    const auto after=image.pixels[std::size_t(y)*width+x]&0xffffffu;
                    value.newWhite+=after==0xffffffu&&before!=0xffffffu;value.lostWhite+=before==0xffffffu&&after!=0xffffffu;value.changed+=before!=after;
                }
            }
        }return value;
    }
    DcPixels ClientPixels(HWND window){
        HDC dc=GetDC(window);if(!dc){DcPixels value;value.status="own_client_dc_unavailable";return value;}
        struct Release{HWND window;HDC dc;~Release(){ReleaseDC(window,dc);}}release{window,dc};
        RECT client{};if(!GetClientRect(window,&client)){DcPixels value;value.status="own_client_rect_unavailable";return value;}
        return Pixels(window,dc,client);
    }
    bool Watched(HWND window)const noexcept{return std::find(windows.begin(),windows.end(),window)!=windows.end();}
    static LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR context){
        auto& self=*reinterpret_cast<DirectDrawTrace*>(context);const DWORD entryError=GetLastError();
        HWND target=window;HDC dc=nullptr;RECT roi{};DRAWITEMSTRUCT draw{};
        const bool geometry=message==WM_WINDOWPOSCHANGING||message==WM_WINDOWPOSCHANGED||message==WM_SIZE||message==WM_SYNCPAINT;
        bool interesting=geometry&&self.Watched(window);
        if(!interesting)interesting=(message==WM_ERASEBKGND||message==WM_PAINT)&&window!=self.app.MainWindow()&&self.Watched(window);
        if(interesting&&!geometry){dc=reinterpret_cast<HDC>(wp);GetClientRect(window,&roi);}
        else if(message==WM_DRAWITEM&&window==self.app.MainWindow()&&lp){
            // The source is this isolated host's synchronous owner-draw message.
            draw=*reinterpret_cast<const DRAWITEMSTRUCT*>(lp);target=draw.hwndItem;
            interesting=draw.CtlType==ODT_BUTTON&&self.Watched(target)&&
                std::find(kIcons.begin(),kIcons.end(),int(draw.CtlID))!=kIcons.end();
            dc=draw.hDC;roi=draw.rcItem;
        }
        Event* event=nullptr;const unsigned parent=self.active;
        if(interesting&&!self.sampling){
            if(self.count<kCapacity){event=&self.events[self.count++];event->sequence=self.count;event->parent=parent;event->phase=self.phase;
                event->depth=self.depth;event->window=target;event->id=GetDlgCtrlID(target);event->message=message;event->begin=Now();
                event->ctlType=draw.CtlType;event->ctlId=draw.CtlID;event->itemAction=draw.itemAction;event->itemState=draw.itemState;event->item=draw.rcItem;
                if((message==WM_WINDOWPOSCHANGING||message==WM_WINDOWPOSCHANGED)&&lp){event->position=*reinterpret_cast<const WINDOWPOS*>(lp);event->positionKnown=true;}
                if(message==WM_SIZE){event->sizeType=unsigned(wp);event->sizeWidth=LOWORD(lp);event->sizeHeight=HIWORD(lp);}
                if(geometry)event->source=window==self.app.MainWindow()?"parent_metadata_only":"own-client-dc-readback";
                event->relayout=self.app.bottomPanelSizing.relayout;event->dragging=self.app.SidebarResizeDragging()||self.app.bottomPanelSizing.dragging>=0;
                self.active=event->sequence;++self.depth;self.sampling=true;
                try{event->before=geometry&&window!=self.app.MainWindow()?self.ClientPixels(target):self.Pixels(target,dc,roi);}catch(...){self.copyFailure=true;event->before.status="sampling_exception";}
                self.sampling=false;event->beforeCopied=Now();
            }else ++self.overflow;
        }
        SetLastError(entryError);const auto result=DefSubclassProc(window,message,wp,lp);const DWORD resultError=GetLastError();
        if(event){event->returned=Now();event->result=result;event->error=resultError;self.sampling=true;
            try{event->after=geometry&&window!=self.app.MainWindow()?self.ClientPixels(target):self.Pixels(target,dc,roi);}catch(...){self.copyFailure=true;event->after.status="sampling_exception";}
            self.sampling=false;event->afterCopied=Now();--self.depth;self.active=parent;}
        SetLastError(resultError);return result;
    }
public:
    unsigned count=0,overflow=0,phase=0;bool copyFailure=false;
    explicit DirectDrawTrace(MMDApp& a):app(a){
        windows.reserve(1+base::kIds.size()+kIcons.size());baselines.reserve(base::kIds.size()+kIcons.size());
        try{auto attach=[&](HWND window){if(window&&std::find(windows.begin(),windows.end(),window)==windows.end()){
            trace::Require(SetWindowSubclass(window,Proc,reinterpret_cast<UINT_PTR>(this),reinterpret_cast<DWORD_PTR>(this))!=FALSE,"attach supplied-DC observer");windows.push_back(window);}};
            attach(app.MainWindow());for(int id:base::kIds)attach(GetDlgItem(app.MainWindow(),id));
            for(int id:kIcons){const auto window=GetDlgItem(app.MainWindow(),id);if(window&&IsWindowVisible(window))attach(window);}
        }catch(...){Close();throw;}
    }
    ~DirectDrawTrace(){Close();}
    void Close()noexcept{if(closed)return;closed=true;for(auto window:windows)if(IsWindow(window))RemoveWindowSubclass(window,Proc,reinterpret_cast<UINT_PTR>(this));}
    void SetBaseline(HWND window,const trace::Snapshot& image,const VisualState& visual){baselines.push_back({window,image,visual});}
    void Write(const std::filesystem::path& output)const{
        std::ofstream f(output/"layout-product-direct-draw.csv"),report(output/"layout-product-direct-draw.txt");
        trace::Require(f&&report,"write supplied-display-DC observations");
        report<<"Before/after WM_ERASEBKGND and parent WM_DRAWITEM use the original supplied HDC. "
            "WINDOWPOSCHANGING/CHANGED, SIZE and SYNCPAINT use a separate bounded GetDC readback of the same own control; main-window geometry is metadata only. "
            "The source column distinguishes these routes. Readback never requests paint, validates the update region, changes its clip or replaces a paint HDC. "
            "Control WM_PAINT is recorded too; a zero paint wParam has no supplied HDC and yields no pixel sample. "
            "Exactly one DefSubclassProc call forwards each message. Readback can perturb timing; display-DC pixels alone do not authenticate final desktop exposure. "
            "Complex clips, memory DCs and a WindowFromDC mismatch are recorded without pixel evidence. No pixel defines a visual PASS.\n"
            <<"events="<<count<<" overflow="<<overflow<<" sampling_exception="<<copyFailure<<'\n';
        f<<"sequence,parent,phase,depth,id,message,qpc_begin,qpc_before_copied,qpc_returned,qpc_after_copied,result,last_error,ctl_type,ctl_id,item_action,item_state,item_l,item_t,item_r,item_b,relayout,dragging,source,position_known,position_x,position_y,position_width,position_height,position_flags,size_type,size_width,size_height,when,status,hdc,dc_type,window_from_dc,map_mode,origin_known,origin_x,origin_y,viewport_known,viewport_x,viewport_y,window_origin_known,window_origin_x,window_origin_y,clip_kind,clip_l,clip_t,clip_r,clip_b,roi_l,roi_t,roi_r,roi_b,width,height,hash,white_pixels,baseline_known,baseline_compared,baseline_full,new_white_pixels,lost_white_pixels,changed_pixels,file\n";
        struct First{int id=0;LONGLONG qpc=0;unsigned index=0;bool after=false;};std::vector<First> firstWhite,firstBlank;
        auto first=[&](std::vector<First>& list,const Event& event,unsigned index,bool after){const LONGLONG time=after?event.afterCopied:event.beforeCopied;
            const auto found=std::find_if(list.begin(),list.end(),[&](const First& entry){return entry.id==event.id;});
            if(found==list.end())list.push_back({event.id,time,index,after});else if(time<found->qpc)*found={event.id,time,index,after};};
        for(unsigned i=0;i<count;++i){const auto& e=events[i];for(int when=0;when<2;++when){const auto& p=when?e.after:e.before;
            std::string file;std::size_t white=0;if(p.image.copied){file="direct-"+std::to_string(e.sequence)+(when?"-after-":"-before-")+std::to_string(e.id)+".bmp";
                base::Bitmap(output/file,p.image);for(auto pixel:p.image.pixels)white+=(pixel&0x00ffffffu)==0x00ffffffu;}
            f<<e.sequence<<','<<e.parent<<','<<e.phase<<','<<e.depth<<','<<e.id<<','<<e.message<<','<<e.begin<<','<<e.beforeCopied<<','<<e.returned<<','<<e.afterCopied<<','
                <<e.result<<','<<e.error<<','<<e.ctlType<<','<<e.ctlId<<','<<e.itemAction<<','<<e.itemState<<','<<e.item.left<<','<<e.item.top<<','<<e.item.right<<','<<e.item.bottom<<','
                <<e.relayout<<','<<e.dragging<<','<<e.source<<','<<e.positionKnown<<','<<e.position.x<<','<<e.position.y<<','<<e.position.cx<<','<<e.position.cy<<','<<e.position.flags<<','
                <<e.sizeType<<','<<e.sizeWidth<<','<<e.sizeHeight<<','<<(when?"after":"before")<<','<<p.status<<','<<p.dc<<','<<p.type<<','<<p.owner<<','<<p.map<<','
                <<p.originKnown<<','<<p.origin.x<<','<<p.origin.y<<','<<p.viewportKnown<<','<<p.viewport.x<<','<<p.viewport.y<<','<<p.windowOriginKnown<<','<<p.windowOrigin.x<<','<<p.windowOrigin.y<<','
                <<p.clipKind<<','<<p.clip.left<<','<<p.clip.top<<','<<p.clip.right<<','<<p.clip.bottom<<','<<p.roi.left<<','<<p.roi.top<<','<<p.roi.right<<','<<p.roi.bottom<<','
                <<p.image.width<<','<<p.image.height<<','<<p.image.Hash()<<','<<white<<','<<p.baselineKnown<<','<<p.baselineCompared<<','<<p.baselineFull<<','<<p.newWhite<<','<<p.lostWhite<<','<<p.changed<<','<<file<<'\n';
            if(p.baselineCompared&&p.newWhite)first(firstWhite,e,i,when!=0);
            if(p.baselineCompared&&p.baselineFull&&CurveLauncher(e.id)&&p.image.copied&&!p.image.Varied())first(firstBlank,e,i,when!=0);
        }}
        for(const auto* list:{&firstWhite,&firstBlank})for(const auto& entry:*list){const auto& event=events[entry.index];const auto& pixels=entry.after?event.after:event.before;
            report<<(list==&firstWhite?"first_new_white":"first_blank_curve")<<" id="<<entry.id<<" sequence="<<event.sequence<<" phase="<<event.phase
                <<" message="<<event.message<<" source="<<event.source<<" when="<<(entry.after?"after":"before")<<" qpc="<<entry.qpc
                <<" new_white="<<pixels.newWhite<<" changed="<<pixels.changed<<'\n';}
        trace::Require(f&&report,"finish supplied-display-DC observations");
    }
};
inline trace::Snapshot NativePixels(HWND window){
    trace::Snapshot result;result.window=window;result.id=GetDlgCtrlID(window);result.visible=IsWindowVisible(window)!=FALSE;
    RECT client{};if(!result.visible||!GetClientRect(window,&client)||client.right<=0||client.bottom<=0||client.right>2048||client.bottom>512)return result;
    result.width=client.right;result.height=client.bottom;
    HDC dc=GetDC(window);if(!dc)return result;
    struct Release{HWND window;HDC dc;~Release(){ReleaseDC(window,dc);}}release{window,dc};
    result.dcType=GetObjectType(dc);result.mapMode=GetMapMode(dc);result.clipKind=GetClipBox(dc,&result.clip);
    if(result.dcType!=OBJ_DC||result.mapMode!=MM_TEXT||result.clipKind!=SIMPLEREGION||result.clip.left>0||result.clip.top>0||
        result.clip.right<client.right||result.clip.bottom<client.bottom)return result;
    trace::Dib dib(client.right,client.bottom,dc);
    if(dib&&BitBlt(dib.dc,0,0,client.right,client.bottom,dc,0,0,SRCCOPY)&&GdiFlush()){
        result.pixels.assign(dib.pixels,dib.pixels+std::size_t(client.right)*client.bottom);result.copied=true;}
    return result;
}
struct PhysicalScope{
    using Set=HANDLE(WINAPI*)(HANDLE);using Get=HANDLE(WINAPI*)();using Equal=BOOL(WINAPI*)(HANDLE,HANDLE);
    Set set=nullptr;Get get=nullptr;Equal equal=nullptr;HANDLE previous=nullptr;
    PhysicalScope(){const auto user=GetModuleHandleW(L"user32.dll");set=reinterpret_cast<Set>(GetProcAddress(user,"SetThreadDpiAwarenessContext"));
        get=reinterpret_cast<Get>(GetProcAddress(user,"GetThreadDpiAwarenessContext"));equal=reinterpret_cast<Equal>(GetProcAddress(user,"AreDpiAwarenessContextsEqual"));
        if(!set||!get||!equal)return;for(int context:{-4,-3}){const auto wanted=reinterpret_cast<HANDLE>(INT_PTR(context));const auto old=set(wanted);
            if(old){if(equal(get(),wanted)){previous=old;return;}if(!set(old))throw std::runtime_error("restore desktop sampler DPI");}}}
    ~PhysicalScope(){if(previous)set(previous);}
};
inline void Describe(MMDApp& app,const std::filesystem::path& output){
    using WindowContext=HANDLE(WINAPI*)(HWND);using Equal=BOOL(WINAPI*)(HANDLE,HANDLE);using Dpi=UINT(WINAPI*)(HWND);
    const auto user=GetModuleHandleW(L"user32.dll");
    const auto context=reinterpret_cast<WindowContext>(GetProcAddress(user,"GetWindowDpiAwarenessContext"));
    const auto equal=reinterpret_cast<Equal>(GetProcAddress(user,"AreDpiAwarenessContextsEqual"));
    const auto dpi=reinterpret_cast<Dpi>(GetProcAddress(user,"GetDpiForWindow"));
    struct Window{int id=0;HWND hwnd=nullptr;RECT caller{},physical{};bool callerKnown=false,physicalKnown=false;};
    std::vector<Window> windows{{0,app.MainWindow()}};
    for(int id:base::kIds)windows.push_back({id,GetDlgItem(app.MainWindow(),id)});
    for(int id:kIcons)windows.push_back({id,GetDlgItem(app.MainWindow(),id)});
    for(auto& window:windows)if(window.hwnd)window.callerKnown=GetWindowRect(window.hwnd,&window.caller)!=FALSE;
    bool physicalContext=false;int physicalWidth=0,physicalHeight=0;std::exception_ptr failure;
    std::thread worker([&]{try{PhysicalScope scope;physicalContext=scope.previous!=nullptr;if(!physicalContext)return;
        physicalWidth=GetSystemMetrics(SM_CXSCREEN);physicalHeight=GetSystemMetrics(SM_CYSCREEN);
        for(auto& window:windows)if(window.hwnd)window.physicalKnown=GetWindowRect(window.hwnd,&window.physical)!=FALSE;
    }catch(...){failure=std::current_exception();}});worker.join();if(failure)std::rethrow_exception(failure);
    std::ofstream f(output/"host.txt");trace::Require(bool(f),"write product-DPI host metadata");
    f<<"Synthetic SendMessage divider-handler diagnostic; no physical input, foreground request or cue-bit changes.\n"
        <<"Empty isolated scene; real full-profile update coordinator and real D3D Present.\n"
        <<"Window DPI 96 does not establish monitor scale for a DPI-unaware window. Physical/caller rectangle ratios are observed geometry only.\n"
        <<"Boundary snapshots and instrumentation do not establish no-flicker; unavailable desktop ROIs are not observations.\n";
    RECT client{},work{};GetClientRect(app.MainWindow(),&client);SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    f<<"client="<<client.right<<','<<client.bottom<<" sidebar_width="<<app.SidebarWidth()<<" bottom_band_top="<<client.bottom-157
        <<" camera="<<int(app.CameraMode())<<" caller_workarea="<<work.left<<','<<work.top<<','<<work.right<<','<<work.bottom
        <<" physical_context="<<physicalContext<<" physical_screen="<<physicalWidth<<','<<physicalHeight<<'\n';
    for(const auto& panel:mikudancestudio::BottomPanelSpans(&app))f<<"bottom_separator panel="<<panel.id
        <<" x="<<mikudancestudio::BottomPanelX(&app,panel.right)<<" width="<<panel.right-panel.left<<" extra="<<panel.extra<<'\n';
    for(const auto& window:windows){const auto awareness=window.hwnd&&context?context(window.hwnd):nullptr;
        f<<"id="<<window.id<<" hwnd="<<reinterpret_cast<std::uintptr_t>(window.hwnd)
            <<" visible="<<(window.hwnd&&IsWindowVisible(window.hwnd))<<" dpi="<<(window.hwnd&&dpi?dpi(window.hwnd):0)
            <<" unaware="<<(awareness&&equal?equal(awareness,reinterpret_cast<HANDLE>(INT_PTR(-1))):-1)
            <<" unaware_gdi_scaled="<<(awareness&&equal?equal(awareness,reinterpret_cast<HANDLE>(INT_PTR(-5))):-1)
            <<" caller_known="<<window.callerKnown<<" caller_rect="<<window.caller.left<<','<<window.caller.top<<','<<window.caller.right<<','<<window.caller.bottom
            <<" physical_known="<<window.physicalKnown<<" physical_rect="<<window.physical.left<<','<<window.physical.top<<','<<window.physical.right<<','<<window.physical.bottom;
        if(window.callerKnown&&window.physicalKnown&&window.caller.right>window.caller.left&&window.caller.bottom>window.caller.top)
            f<<" physical_caller_ratio="<<double(window.physical.right-window.physical.left)/double(window.caller.right-window.caller.left)<<','
                <<double(window.physical.bottom-window.physical.top)/double(window.caller.bottom-window.caller.top);
        f<<'\n';}
    trace::Require(bool(f),"finish product-DPI host metadata");
}
struct Above{
    HWND main;RECT roi;bool found=false,blocked=false,failed=false;unsigned count=0;
    static BOOL CALLBACK Visit(HWND window,LPARAM context){auto& self=*reinterpret_cast<Above*>(context);
        if(window==self.main){self.found=true;return FALSE;}if(++self.count>4096){self.failed=true;return FALSE;}
        if(!IsWindowVisible(window)||IsIconic(window))return TRUE;RECT bounds{},hit{};
        if(!GetWindowRect(window,&bounds)){self.failed=true;return FALSE;}if(IntersectRect(&hit,&bounds,&self.roi)){self.blocked=true;return FALSE;}return TRUE;}
};
inline bool OwnDesktop(HWND main,const RECT& roi){
    Above above{main,roi};EnumWindows(Above::Visit,reinterpret_cast<LPARAM>(&above));if(!above.found||above.blocked||above.failed)return false;
    for(LONG y:{roi.top,(roi.top+roi.bottom-1)/2,roi.bottom-1})for(LONG x:{roi.left,(roi.left+roi.right-1)/2,roi.right-1}){
        const auto window=WindowFromPoint(POINT{x,y});DWORD process=0;GetWindowThreadProcessId(window,&process);
        if(!window||GetAncestor(window,GA_ROOT)!=main||process!=GetCurrentProcessId())return false;}
    return true;
}
inline void DesktopPixels(HWND main,const std::vector<HWND>& targets,unsigned phase,std::vector<Capture>& output){
    // Only this worker uses physical coordinates; the product UI thread keeps
    // its unchanged DPI context. It reads no MMDApp fields and sends no messages.
    PhysicalScope dpi;
    for(auto window:targets){Capture sample;sample.phase=phase;sample.id=GetDlgCtrlID(window);sample.kind="desktop";sample.begin=Now();
        sample.status="dpi_api_unavailable";
        if(dpi.previous){sample.status="geometry_unavailable";
            if(IsWindowVisible(window)&&GetWindowRect(window,&sample.bounds)){
                const auto& r=sample.bounds;const int width=r.right-r.left,height=r.bottom-r.top;
                sample.status="offscreen_or_unbounded";
                if(width>0&&height>0&&width<=512&&height<=128&&r.left>=0&&r.top>=0&&r.right<=GetSystemMetrics(SM_CXSCREEN)&&r.bottom<=GetSystemMetrics(SM_CYSCREEN)){
                    sample.status="occluded_or_foreign";
                    if(OwnDesktop(main,r)){
                        HDC desktop=GetDC(nullptr);sample.status="desktop_dc_unavailable";
                        if(desktop){POINT origin{},view{},win{};SIZE ve{},we{};
                            const bool calibrated=GetSystemMetrics(SM_CMONITORS)==1&&GetMapMode(desktop)==MM_TEXT&&
                                GetDCOrgEx(desktop,&origin)&&!origin.x&&!origin.y&&GetViewportOrgEx(desktop,&view)&&!view.x&&!view.y&&
                                GetWindowOrgEx(desktop,&win)&&!win.x&&!win.y&&GetViewportExtEx(desktop,&ve)&&GetWindowExtEx(desktop,&we)&&ve.cx==we.cx&&ve.cy==we.cy&&
                                GetDeviceCaps(desktop,HORZRES)==GetDeviceCaps(desktop,DESKTOPHORZRES)&&GetDeviceCaps(desktop,VERTRES)==GetDeviceCaps(desktop,DESKTOPVERTRES)&&
                                GetDeviceCaps(desktop,HORZRES)==GetSystemMetrics(SM_CXSCREEN)&&GetDeviceCaps(desktop,VERTRES)==GetSystemMetrics(SM_CYSCREEN);
                            sample.status="desktop_coordinate_uncalibrated";
                            if(calibrated){trace::Dib dib(width,height,desktop);sample.status="desktop_copy_failed";
                                RECT after{};
                                if(dib&&BitBlt(dib.dc,0,0,width,height,desktop,r.left,r.top,SRCCOPY)&&GdiFlush()&&
                                    GetWindowRect(window,&after)&&EqualRect(&r,&after)&&OwnDesktop(main,r)){
                                    auto& image=sample.image;image.window=window;image.id=sample.id;image.width=width;image.height=height;image.visible=true;
                                    image.pixels.assign(dib.pixels,dib.pixels+std::size_t(width)*height);image.copied=true;sample.status="observed";}
                            }ReleaseDC(nullptr,desktop);
                        }
                    }
                }
            }
        }sample.end=Now();output.push_back(std::move(sample));
    }
}
struct Driver{
    MMDApp& app;base::Probe& native;slider_real_input_metadata::Trace& messages;PresentTrace& presents;DirectDrawTrace& direct;
    std::vector<HWND> targets;std::vector<Capture> captures;unsigned phase=0;bool gesture=false;
    std::vector<std::pair<unsigned,std::string>> phases;unsigned safePoints=0;
    explicit Driver(MMDApp& a,base::Probe& p,slider_real_input_metadata::Trace& m,PresentTrace& d,DirectDrawTrace& e):app(a),native(p),messages(m),presents(d),direct(e){
        captures.reserve(512);for(auto window:p.windows)if(IsWindowVisible(window))targets.push_back(window);
        for(int id:kIcons){const auto window=GetDlgItem(a.MainWindow(),id);if(window&&IsWindowVisible(window))targets.push_back(window);}}
    ~Driver(){Cancel();}
    void Cancel()noexcept{if(gesture&&IsWindow(app.MainWindow())){if(GetCapture()==app.MainWindow())SendMessageW(app.MainWindow(),WM_CANCELMODE,0,0);
        if(GetCapture()==app.MainWindow())ReleaseCapture();}gesture=false;}
    void Phase(const std::string& name){++phase;phases.push_back({phase,name});messages.SetPhase(phase);presents.phase=phase;native.step=phase;direct.phase=phase;}
    void CaptureNow(const char* name){
        Phase(name);Guard(app,gesture);GdiFlush();
        for(auto window:targets){Capture sample;sample.phase=phase;sample.id=GetDlgCtrlID(window);sample.kind="native";sample.begin=Now();
            GetWindowRect(window,&sample.bounds);MapWindowPoints(nullptr,app.MainWindow(),reinterpret_cast<POINT*>(&sample.bounds),2);
            sample.visual=Visual(window);sample.ui=sample.visual.ui;const int index=native.Index(window);
            if(index>=0){sample.nativePaints=native.counts[unsigned(index)].paint;sample.nativeErases=native.counts[unsigned(index)].erase;}
            sample.image=NativePixels(window);sample.status=sample.image.copied?"observed":"native_dc_not_fully_readable";sample.end=Now();
            if(!std::strcmp(name,"initial"))direct.SetBaseline(window,sample.image,sample.visual);
            captures.push_back(std::move(sample));}
        std::vector<Capture> desktop;desktop.reserve(targets.size());std::exception_ptr failure;
        std::thread worker([&]{try{DesktopPixels(app.MainWindow(),targets,phase,desktop);}catch(...){failure=std::current_exception();}});worker.join();
        if(failure)std::rethrow_exception(failure);for(auto& sample:desktop)captures.push_back(std::move(sample));Guard(app,gesture);
    }
    bool Dispatch(HWND window,UINT message){
        MSG msg{};if(!PeekMessageW(&msg,window,message,message,PM_NOREMOVE))return false;
        if(msg.message==WM_QUIT)throw Incomplete("queued quit left untouched");
        if(msg.hwnd!=window||msg.message!=message)return false;
        Guard(app,gesture);if(!PeekMessageW(&msg,window,message,message,PM_REMOVE))return false;
        TranslateMessage(&msg);DispatchMessageW(&msg);return true;
    }
    void SafeAndPresent(const char* name,const base::Invariant& invariant){
        Phase(name);const unsigned before=presents.count;const auto begin=GetTickCount64();
        HWND wake=nullptr;for(HWND candidate=nullptr;(candidate=FindWindowExW(HWND_MESSAGE,candidate,L"MMS.UpdateCoordinator.V1",nullptr));)
            if(GetWindowThreadProcessId(candidate,nullptr)==GetCurrentThreadId()){wake=candidate;break;}
        mikudancestudio::updates::RequestViewport(&app,false);
        do{Guard(app,gesture);++safePoints;mikudancestudio::updates::SafePoint(&app);invariant.Check();
            Dispatch(app.MainWindow(),WM_APP+0x6a);if(wake){Dispatch(wake,WM_APP+0x73);Dispatch(wake,WM_TIMER);}
            Dispatch(app.MainWindow(),WM_PAINT);for(auto window:targets)Dispatch(window,WM_PAINT);
            if(presents.count>before)break;Sleep(1);
        }while(GetTickCount64()-begin<180);
        invariant.Check();if(presents.count==before)throw Incomplete("no actual product Present observed after SafePoint");
    }
    void Send(UINT message,int x,int y){Guard(app,gesture);SendMessageW(app.MainWindow(),message,message==WM_LBUTTONUP?0:MK_LBUTTON,MAKELPARAM(x,y));}
};
inline void Fit(MMDApp& app){
    RECT work{};trace::Require(SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)!=FALSE,"read product-DPI work area");
    const int width=std::min(1380,int(work.right-work.left)-36),height=std::min(820,int(work.bottom-work.top)-70);
    if(width<700||height<400)throw Incomplete("work area too small for actual legacy bottom panels");
    small_viewport_host_checks::ClientSize(app,width,height);
    SetWindowPos(app.MainWindow(),nullptr,work.left+8,work.top+8,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    SetWindowTextW(app.MainWindow(),L"MMS 产品 DPI 布局诊断（合成分隔线操作，无消闪结论）");realShow(app.MainWindow(),SW_SHOWNOACTIVATE);
    RECT outer{};GetWindowRect(app.MainWindow(),&outer);
    if(outer.right>work.right||outer.bottom>work.bottom)
        SetWindowPos(app.MainWindow(),nullptr,work.left+4,work.top+4,std::min(int(outer.right-outer.left),int(work.right-work.left)-8),
            std::min(int(outer.bottom-outer.top),int(work.bottom-work.top)-8),SWP_NOACTIVATE|SWP_NOZORDER);
    HandleWindowSize(&app);RefreshMainWindowViewport(&app);RedrawWindow(app.MainWindow(),nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    GetWindowRect(app.MainWindow(),&outer);
    if(outer.left<work.left||outer.top<work.top||outer.right>work.right||outer.bottom>work.bottom)throw Incomplete("diagnostic host not contained in actual work area");
}
struct Regression{unsigned comparisons=0,failures=0,incomplete=0,geometrySkipped=0;};
inline Regression Evaluate(const std::filesystem::path& output,const Driver& driver,bool interrupted,bool traceIncomplete){
    std::ofstream report(output/"layout-product-regression.txt"),csv(output/"layout-product-regression.csv");
    trace::Require(report&&csv,"write stable-boundary layout regression");Regression result;
    unsigned initialPhase=0;bool released=false;
    for(const auto& phase:driver.phases){if(phase.second=="initial")initialPhase=phase.first;if(phase.second=="bottom-released")released=true;}
    std::vector<const Capture*> initial;
    for(const auto& sample:driver.captures)if(sample.phase==initialPhase&&sample.kind=="native"&&(Slider(sample.id)||CurveLauncher(sample.id)))initial.push_back(&sample);
    if(interrupted||traceIncomplete||!released||initial.empty())++result.incomplete;
    unsigned initialSliders=0,initialLaunchers=0;
    for(const auto* baseline:initial){initialSliders+=Slider(baseline->id);initialLaunchers+=CurveLauncher(baseline->id);
        if(!baseline->image.copied||!baseline->image.Varied())++result.incomplete;}
    if(!initialSliders||!initialLaunchers)++result.incomplete;
    csv<<"phase,id,boundary,result,baseline_width,baseline_height,current_width,current_height,baseline_white,current_white,new_white,lost_white,changed_pixels,baseline_varied,current_varied,state_equal\n";
    for(const auto& phase:driver.phases){
        const bool release=phase.second=="sidebar-released"||phase.second=="bottom-released";
        if(!release&&phase.second!="sidebar-after-present"&&phase.second!="bottom-after-present")continue;
        for(const auto* baseline:initial){const auto found=std::find_if(driver.captures.begin(),driver.captures.end(),[&](const Capture& c){return c.phase==phase.first&&c.id==baseline->id&&c.kind=="native";});
            const Capture* sample=found==driver.captures.end()?nullptr:&*found;const char* verdict="INCOMPLETE";
            std::size_t newWhite=0,lostWhite=0,changed=0;bool same=false;
            if(!sample||!baseline->image.copied||!baseline->image.Varied()||!sample->image.copied)++result.incomplete;
            else if(baseline->image.width!=sample->image.width||baseline->image.height!=sample->image.height){
                if(release){++result.incomplete;verdict="INCOMPLETE_RELEASE_GEOMETRY";}
                else{++result.geometrySkipped;verdict="EXPECTED_RESIZE_SKIPPED";}
            }else if(!(same=baseline->visual.Same(sample->visual))){++result.incomplete;verdict="INCOMPLETE_VISUAL_STATE_CHANGED";}
            else{++result.comparisons;
                for(std::size_t i=0;i<baseline->image.pixels.size();++i){const auto before=baseline->image.pixels[i]&0xffffffu,after=sample->image.pixels[i]&0xffffffu;
                    newWhite+=after==0xffffffu&&before!=0xffffffu;lostWhite+=before==0xffffffu&&after!=0xffffffu;changed+=before!=after;}
                if(Slider(baseline->id)&&newWhite){++result.failures;verdict="FAIL_NEW_SLIDER_WHITE";}
                else if(CurveLauncher(baseline->id)&&!sample->image.Varied()){++result.failures;verdict="FAIL_BLANK_CURVE_LAUNCHER";}
                else verdict="CHECKED";
            }
            csv<<phase.first<<','<<baseline->id<<','<<phase.second<<','<<verdict<<','<<baseline->image.width<<','<<baseline->image.height<<','
                <<(sample?sample->image.width:0)<<','<<(sample?sample->image.height:0)<<','<<White(baseline->image)<<','<<(sample?White(sample->image):0)<<','
                <<newWhite<<','<<lostWhite<<','<<changed<<','<<baseline->image.Varied()<<','<<(sample&&sample->image.Varied())<<','<<same<<'\n';
        }
    }
    const char* status=result.failures?"FAIL":result.incomplete||!result.comparisons?"INCOMPLETE":"PASS_STABLE_BOUNDARIES";
    report<<"status="<<status<<" comparisons="<<result.comparisons<<" failures="<<result.failures<<" incomplete="<<result.incomplete
        <<" expected_resizes_skipped="<<result.geometrySkipped<<" initial_sliders="<<initialSliders<<" initial_curve_launchers="<<initialLaunchers<<'\n'
        <<"Only native readback after a real product Present and splitter release is checked against the initial isolated host. "
        "Comparable client size, styles, UI cues, enabled/focus and native button state are required. A slider may not acquire pure-white pixels absent at the same baseline coordinates; "
        "a visible curve launcher with an initially varied glyph may not become a single-color blank. Changed RGB pixels are reported without an exact-color assertion. "
        "Expected resizing skips intermediate geometry comparisons, but released original geometry must be comparable. "
        "Unreadable images, blank initial controls, changed visual state, missing completion, input interruption or loss in the direct/native/Present observers prevent PASS. "
        "Intermediate display-DC new-white observations are evidence of pending surface contents, not assertions of final desktop exposure. "
        "PASS_STABLE_BOUNDARIES never claims continuous no-flicker, complete glyph fidelity, hardware input, or an unperturbed frame rate. "
        "Ordinary layout-product writes these findings without failing; layout-product-regression makes a FAIL or INCOMPLETE process failure.\n";
    trace::Require(report&&csv,"finish stable-boundary layout regression");return result;
}
inline void Write(const std::filesystem::path& output,const Driver& driver,const PresentTrace& present,const std::string& status){
    std::ofstream report(output/"layout-product-report.txt"),csv(output/"layout-product-samples.csv"),events(output/"layout-product-present.csv");
    trace::Require(report&&csv&&events,"open product-DPI diagnostic outputs");
    report<<"status="<<status<<"\nSynthetic SendMessage splitter down/move/release invokes the real product handlers. No physical input is generated. "
        "ProductDpi is selected before HWND creation. No focus-cue bits or display scale are changed, and foreground is never requested. "
        "Existing trace text describes its observer only; input messages in this run include this synthetic driver. "
        "Snapshots are sequential boundary observations, not continuous/native hardware timing and never a no-flicker PASS. "
        "Unavailable desktop/native snapshots remain unavailable rather than successful images. Only own bounded control regions can be copied.\n"
        <<"safe_points="<<driver.safePoints<<" actual_presents="<<present.count<<" present_overflow="<<present.overflow<<'\n';
    for(const auto& phase:driver.phases)report<<"phase="<<phase.first<<" name="<<phase.second<<'\n';
    csv<<"phase,id,kind,status,qpc_begin,qpc_end,left,top,right,bottom,width,height,ui_state,native_paints,native_erases,style,extended_style,enabled,focused,is_button,button_state,hash,white_pixels,file\n";
    unsigned index=0;for(const auto& sample:driver.captures){std::string filename;std::size_t white=0;
        if(sample.image.copied){filename="roi-"+std::to_string(++index)+"-phase-"+std::to_string(sample.phase)+"-"+sample.kind+"-"+std::to_string(sample.id)+".bmp";
            base::Bitmap(output/filename,sample.image);for(auto pixel:sample.image.pixels)white+=(pixel&0x00ffffffu)==0x00ffffffu;}
        csv<<sample.phase<<','<<sample.id<<','<<sample.kind<<','<<sample.status<<','<<sample.begin<<','<<sample.end<<','
            <<sample.bounds.left<<','<<sample.bounds.top<<','<<sample.bounds.right<<','<<sample.bounds.bottom<<','<<sample.image.width<<','<<sample.image.height<<','
            <<sample.ui<<','<<sample.nativePaints<<','<<sample.nativeErases<<','<<sample.visual.style<<','<<sample.visual.extended<<','<<sample.visual.enabled<<','
            <<sample.visual.focused<<','<<sample.visual.isButton<<','<<sample.visual.button<<','<<sample.image.Hash()<<','<<white<<','<<filename<<'\n';}
    events<<"phase,qpc_begin,qpc_end,source_known,source_l,source_t,source_r,source_b,target_known,target_l,target_t,target_r,target_b,hwnd,result\n";
    for(unsigned i=0;i<present.count;++i){const auto& e=present.events[i];events<<e.phase<<','<<e.begin<<','<<e.end<<','<<e.hasSource<<','
        <<e.source.left<<','<<e.source.top<<','<<e.source.right<<','<<e.source.bottom<<','<<e.hasTarget<<','<<e.target.left<<','<<e.target.top<<','
        <<e.target.right<<','<<e.target.bottom<<','<<reinterpret_cast<std::uintptr_t>(e.window)<<','<<e.result<<'\n';}
    trace::Require(report&&csv&&events,"finish product-DPI diagnostic outputs");
}
inline void Run(MMDApp& app,const std::filesystem::path& output,bool regression=false){
    std::filesystem::create_directories(output);std::string status="observed";
    app.CameraMode()=1;PostLanguageSweep2(&app);PostViewRefresh(&app);Fit(app);Describe(app,output);
    trace::Require(mikudancestudio::updates::Initialize(&app),"initialize product update coordinator");
    struct Coordinator{MMDApp& app;~Coordinator(){mikudancestudio::updates::Shutdown(&app);}}coordinator{app};app.MilliToSec()=.001f;
    slider_real_input_metadata::Trace messages(app);trace::Require(messages.Attach(),"attach actual fixed-ring message observation");
    base::Probe native(app);PresentTrace present(app.Renderer()->device);DirectDrawTrace direct(app);Driver driver(app,native,messages,present,direct);
    try{
        Guard(app,false);base::Invariant invariant(app);driver.SafeAndPresent("initial-product-present",invariant);driver.CaptureNow("initial");
        RECT client{};GetClientRect(app.MainWindow(),&client);const int side=app.SidebarWidth(),y=std::min(200,int(client.bottom)-170);
        driver.Phase("sidebar-down");driver.gesture=true;driver.Send(WM_LBUTTONDOWN,side+2,y);
        if(!app.SidebarResizeDragging()||GetCapture()!=app.MainWindow())throw Incomplete("actual sidebar handler did not acquire gesture");
        const int sidebarDelta=std::min(600,int(client.right)-5-side);
        if(sidebarDelta<20)throw Incomplete("actual sidebar has insufficient resizing range");
        for(int delta:{sidebarDelta/2,sidebarDelta,0}){driver.Phase("sidebar-move-"+std::to_string(delta));driver.Send(WM_MOUSEMOVE,side+delta,y);invariant.Check();driver.CaptureNow("sidebar-after-handler");
            driver.SafeAndPresent("sidebar-safepoint-present",invariant);driver.CaptureNow("sidebar-after-present");}
        driver.Send(WM_LBUTTONUP,side,y);driver.gesture=false;invariant.Check();driver.CaptureNow("sidebar-released");
        auto spans=mikudancestudio::BottomPanelSpans(&app);int used=0;for(const auto& p:spans)used+=p.extra;
        const int spare=client.right-3-spans.back().right-used;
        if(spare<4)throw Incomplete("actual work-area client lacks four bottom-panel expansion pixels");
        const int bottomDelta=std::min(80,spare);
        const auto edge=std::find_if(spans.begin(),spans.end(),[](const auto& p){return p.id==2&&p.right-p.left>20;});
        if(edge==spans.end())throw Incomplete("camera bottom panel is collapsed");
        const int x=mikudancestudio::BottomPanelX(&app,edge->right),bottomY=client.bottom-150;
        driver.Phase("bottom-down");driver.gesture=true;driver.Send(WM_LBUTTONDOWN,x,bottomY);
        if(app.bottomPanelSizing.dragging!=2||GetCapture()!=app.MainWindow())throw Incomplete("actual bottom handler did not acquire intended edge");
        for(int delta:{bottomDelta/2,bottomDelta,0}){driver.Phase("bottom-move-"+std::to_string(delta));driver.Send(WM_MOUSEMOVE,x+delta,bottomY);invariant.Check();driver.CaptureNow("bottom-after-handler");
            driver.SafeAndPresent("bottom-safepoint-present",invariant);driver.CaptureNow("bottom-after-present");}
        driver.Send(WM_LBUTTONUP,x,bottomY);driver.gesture=false;invariant.Check();driver.CaptureNow("bottom-released");
    }catch(const Incomplete& reason){status="INCOMPLETE: "+std::string(reason.what());driver.Cancel();}
    catch(const std::exception& reason){status="FAILED: "+std::string(reason.what());driver.Cancel();}
    driver.Cancel();direct.Close();messages.SetPhase(0);messages.Detach();messages.Write(output);direct.Write(output);base::WriteEvents(output,native);
    const auto verdict=Evaluate(output,driver,status!="observed",native.overflow||present.overflow||direct.overflow||direct.copyFailure);
    if(regression&&status=="observed"){
        if(verdict.failures)status="FAILED: stable-boundary white-pixel or blank-launcher regression";
        else if(verdict.incomplete||!verdict.comparisons)status="INCOMPLETE: stable-boundary evidence unavailable";
    }
    Write(output,driver,present,status);
    realShow(app.MainWindow(),SW_HIDE);
    std::printf("LAYOUT_PRODUCT %s actual_presents=%u captures=%zu; observation only, no visual success assertion\n",status.c_str(),present.count,driver.captures.size());
    if(status!="observed")throw std::runtime_error(status);
}
}
#endif
