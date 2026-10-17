// MAINTENANCE: regression-keep; dense curve synchronization and raw native edits.
#define NOMINMAX
#include "mikudancestudio/animation_curve_host.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/morph_curve.hpp"
#include "mikudancestudio/global_key_layout.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/clipboard_layout.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace mikudancestudio;
namespace ac = animation_curve;
namespace ce = curve_editor;
namespace mc = morph_curve;
namespace mikudancestudio {
void PumpEnterRegisterFrame(MMDApp*, HWND, bool);
void PumpDeleteRebuild(MMDApp*, HWND, bool);
void RegisterCameraState(MMDApp*, int);
int RegisterLightKey(MMDApp*, const void*);
int RegisterSelfShadowKey(MMDApp*, const void*);
int RegisterGravityKey(MMDApp*, const void*);
void ReloadModels(MMDApp*);
}
namespace {
void Check(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
template<class F> double Measure(int count, F run) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i) run();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count()/count;
}
bool Same(const ac::Track& a, const ac::Track& b) {
    if (a.discrete != b.discrete || a.keys.size() != b.keys.size()) return false;
    for (std::size_t i=0;i<a.keys.size();++i) {
        const auto& x=a.keys[i];const auto& y=b.keys[i];
        if(x.frame!=y.frame||x.value!=y.value||x.inSlope!=y.inSlope||x.outSlope!=y.outSlope||
           x.inWeight!=y.inWeight||x.outWeight!=y.outWeight||x.tangent!=y.tangent||x.interpolation!=y.interpolation)return false;
    }
    return true;
}
void MorphSync() {
    constexpr int count=64, length=501;
    auto app=std::make_unique<MMDApp>();
    std::memset(&app->state,0,sizeof app->state);app->EnglishUI()=2;
    auto record=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::MorphRecord> morphs(count);
    record->morphCount=count;record->morphs=morphs.data();
    record->morphKeys=mdl::MorphKeyPoolCreate(count*length+32);
    Check(record->morphKeys!=nullptr,"allocate morph fixture");
    auto* model=reinterpret_cast<unsigned char*>(record.get());app->ModelSlot(0)=model;
    std::vector<ac::NamedTrack> authored;
    for(int m=0;m<count;++m) {
        std::snprintf(morphs[m].name,sizeof morphs[m].name,"morph%d",m);
        morphs[m].type=1;morphs[m].panel=static_cast<mdl::MorphPanel>(1+m%4);
        record->morphKeys[m].allocated=1;
        ac::Track track;
        for(int f=0;f<length;++f) {
            ac::Key key;key.frame=f;key.value=(f%7)/8.0;
            key.tangent=ac::Tangent::Free;key.interpolation=ac::Interpolation::Bezier;
            key.inSlope=.1;key.outSlope=-.05;track.keys.push_back(key);
        }
        authored.push_back({mc::StableId(m,morphs[m].name),std::move(track)});
    }
    std::string error;Check(mc::Write(model,authored,error),error.c_str());
    std::vector<ce::Channel> channels;std::wstring reason;
    const auto read=[&]{Check(curve_host::ReadMorphChannels(*app,channels,reason),"read all morph channels");};
    read();
    const double readMs=Measure(8,read);
    const double evalMs=Measure(80,[&]{mc::Evaluate(model,250.5);});
    for(int m=0;m<count;++m) {
        Check(Same(channels[m].track,authored[m].track),"bulk read retains every authored tangent");
        Check(std::abs(morphs[m].value-ac::Evaluate(authored[m].track,250.5).value)<1e-6,"fractional evaluation unchanged");
    }
    // External/native writes have no notification. Both lanes must update in
    // the same read, including after their bytes are restored by native undo.
    const auto first=record->morphKeys[0],last=record->morphKeys[count-1];
    record->morphKeys[0].value=.875f;record->morphKeys[count-1].value=.25f;
    read();Check(channels.front().track.keys[0].value==.875&&channels.back().track.keys[0].value==.25,"native changes synchronize together");
    record->morphKeys[0]=first;record->morphKeys[count-1]=last;read();
    Check(Same(channels.front().track,authored.front().track)&&Same(channels.back().track,authored.back().track),"native undo restores exact tangents");
    for(int m=0;m<count;++m)record->morphKeys[m].allocated=0;
    read();Check(Same(channels[0].track,authored[0].track),"selection changes preserve the curve");
    record->selectedMorphs[0]=0;
    auto display=std::make_unique<mdl::DisplayKey[]>(mdl::kDisplayKeyCapacity);
    display[0].visible=1;record->displayKeys=display.get();
    auto owner=CreateWindowW(L"STATIC",L"morph sync",WS_OVERLAPPED,0,0,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(owner!=nullptr,"create enabled hidden morph owner");app->Hwnd()=owner;
    curve_host::OpenMorph(app.get(),0,false);
    HWND editor=nullptr;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM parameter)->BOOL{
        wchar_t name[80]{};GetClassNameW(window,name,80);
        if(wcscmp(name,L"MikuDanceStudioCurveEditorWindow")==0)*reinterpret_cast<HWND*>(parameter)=window;
        return TRUE;
    },reinterpret_cast<LPARAM>(&editor));
    Check(editor!=nullptr,"real morph editor opens hidden");
    for(int frame : {1,250}) {
        auto point=SendMessageW(editor,WM_APP+300,frame,0);
        auto canvas=GetDlgItem(editor,1020);
        SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(canvas,WM_LBUTTONUP,0,point);
        unsigned slot=0,marks=0;
        do {if(record->morphKeys[slot].allocated){++marks;Check(record->morphKeys[slot].frame==unsigned(frame),"editor writes the selected native frame");}slot=record->morphKeys[slot].next;} while(slot);
        Check(marks==1,"native selection clears old marks across the dense chain");
    }
    auto interpolation=GetDlgItem(editor,1045);
    SendMessageW(interpolation,CB_SETCURSEL,1,0);
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1045,CBN_SELCHANGE),reinterpret_cast<LPARAM>(interpolation));
    Check(app->SceneModified()!=0,"morph interpolation edit reaches the real host write");
    unsigned marks=0,slot=0;
    do{if(record->morphKeys[slot].allocated){++marks;Check(record->morphKeys[slot].frame==250,"changing interpolation keeps the selected morph frame");}slot=record->morphKeys[slot].next;}while(slot);
    Check(marks==1,"changing morph interpolation does not select the entire native track");
    SendMessageW(editor,WM_COMMAND,MAKEWPARAM(1059,BN_CLICKED),0);
    ce::CloseOwned(owner);DestroyWindow(owner);app->Hwnd()=nullptr;
    // The canonical fast path must fall back for a reordered but readable chain.
    auto* keys=record->morphKeys;const unsigned a=keys[0].next,b=keys[a].next;
    std::swap(keys[a].frame,keys[b].frame);read();
    Check(channels[0].track.keys[1].value==keys[b].value,"out-of-order native frames are normalized");
    std::swap(keys[a].frame,keys[b].frame);read();
    Check(Same(channels[0].track,authored[0].track),"restoring frame order restores authored shape");
    keys[b].frame=keys[a].frame;read();
    Check(channels[0].track.keys.size()==length-1&&channels[0].track.keys[1].value==keys[b].value,"duplicate native frames retain the last value");
    keys[b].frame=2;read();
    Check(Same(channels[0].track,authored[0].track),"undoing duplicate frames restores authored shape");
    std::vector<ac::Track> batch;
    Check(mc::ReadMany(*app,{count-1,0,count-1},batch,reason)&&batch.size()==3&&
          Same(batch[0],authored.back().track)&&Same(batch[1],authored.front().track)&&Same(batch[2],batch[0]),"batch follows requested order including duplicate indices");
    Check(!mc::ReadMany(*app,{0,-1},batch,reason)&&batch.empty()&&!reason.empty(),"failed batch exposes no partial output");
    Check(mc::ReadMany(*app,{},batch,reason)&&batch.empty()&&reason.empty(),"empty batch clears output and stale error");
    const auto next=keys[b].next;keys[b].next=b;
    Check(!mc::ReadMany(*app,{0},batch,reason)&&batch.empty(),"cyclic native chain is rejected instead of reusing stale data");
    keys[b].next=next;
    std::printf("morph_sync channels=%d keys_per_channel=%d read_ms=%.6f evaluate_ms=%.6f\n",count,length,readMs,evalMs);
    mc::Forget(model);app->ModelSlot(0)=nullptr;mdl::MorphKeyPoolDestroy(record->morphKeys);
}
void SelectionSync() {
    constexpr int count=8000;
    std::vector<ce::Channel> data(1);data[0].id="dense";data[0].label=L"Dense";
    std::vector<int> frames;
    for(int i=0;i<count;++i) {ac::Key key;key.frame=i;key.value=i%2;data[0].track.keys.push_back(key);frames.push_back(i);}
    int reads=0,writes=0;ce::Session session;session.id="sync-test";session.currentFrame=[]{return 0.0;};
    ce::Source source;source.id="dense";
    source.read=[&](auto& out,auto&){++reads;out=data;return true;};
    source.apply=[&](const auto& next,auto&){++writes;data=next;return true;};
    source.readKeySelection=[&](const auto&){return frames;};
    session.sources.push_back(std::move(source));
    auto window=static_cast<HWND>(ce::Open(nullptr,std::move(session),false));Check(window!=nullptr,"hidden editor opens");
    // Exercise the visible timer branch without mapping a window to the desktop.
    const auto style=GetWindowLongPtrW(window,GWL_STYLE);SetWindowLongPtrW(window,GWL_STYLE,style|WS_VISIBLE);
    const auto tick=[&]{SendMessageW(window,WM_TIMER,1,0);};tick();
    const int beforeReads=reads;
    const double unchangedMs=Measure(60,tick);
    int alternating=0;
    const double changedMs=Measure(30,[&]{frames.back()=(++alternating%2)?count+1:count-1;tick();});
    Check(reads==beforeReads&&writes==0,"selection polling never reloads or writes curves");
    frames={20,4,4,-1,count+100};tick();
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(1057,BN_CLICKED),0);
    Check(data[0].track.keys.size()==count-2,"unsorted duplicate selection maps to exactly two real keys");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(1059,BN_CLICKED),0);
    Check(data[0].track.keys.size()==count,"undo retains dense keys");
    frames.clear();tick();const int beforeWrites=writes;
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(1057,BN_CLICKED),0);
    Check(writes==beforeWrites,"empty native selection clears the editor selection");
    const int beforeNotify=reads;
    for(int i=0;i<100;++i)ce::NotifySourceChanged(nullptr,"dense");
    Check(reads==beforeNotify,"notifications do not synchronously reload each source");
    MSG message{};
    while(PeekMessageW(&message,window,WM_APP+401,WM_APP+401,PM_REMOVE))DispatchMessageW(&message);
    Check(reads==beforeNotify+1,"a notification burst performs only one source read");
    SetWindowLongPtrW(window,GWL_STYLE,style);ce::CloseOwned(nullptr);
    std::printf("selection_sync keys=%d unchanged_tick_ms=%.6f changed_tick_ms=%.6f\n",count,unchangedMs,changedMs);
}
int mainRegistrations = 0;
LRESULT CALLBACK InputHost(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_COMMAND && LOWORD(wp) == 500) { ++mainRegistrations; return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
void CameraEditorCommands() {
    WNDCLASSW type{};type.lpfnWndProc=InputHost;type.hInstance=GetModuleHandleW(nullptr);
    type.lpszClassName=L"MmsCurveSyncInputTest";RegisterClassW(&type);
    HWND owner=CreateWindowW(type.lpszClassName,L"",WS_OVERLAPPED,0,0,400,300,nullptr,nullptr,type.hInstance,nullptr);
    Check(owner!=nullptr,"create hidden camera input owner");
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);app->Hwnd()=owner;
    app->state.enterKeyState=1;app->ViewportInputActive()=1;
    std::vector<ce::Channel> data(1);data[0].id="x";
    data[0].track.keys={{0,0},{30,1}};
    ce::Session session;session.id="camera-input";
    ce::Source source;source.id="camera";source.read=[&](auto& out,auto&){out=data;return true;};
    source.apply=[&](const auto& next,auto&){data=next;return true;};session.sources.push_back(std::move(source));
    auto editor=static_cast<HWND>(ce::Open(owner,std::move(session),false));
    Check(editor!=nullptr,"create hidden camera curve editor");
    Check(ce::OwnsWindow(owner,editor)&&ce::OwnsWindow(owner,GetDlgItem(editor,1020))&&!ce::OwnsWindow(owner,owner),
          "curve keyboard ownership includes its root and children, and excludes the main window");
    PumpEnterRegisterFrame(app.get(),GetDlgItem(editor,1020),true);
    Check(mainRegistrations==0,"Enter in the curve canvas never registers in the main window");
    PumpEnterRegisterFrame(app.get(),GetDlgItem(editor,1041),true);
    Check(mainRegistrations==0,"Enter in the curve frame field never registers in the main window");
    app->state.deleteKeyState=1;
    PumpDeleteRebuild(app.get(),GetDlgItem(editor,1020),true);
    Check(data[0].track.keys.size()==2,"Delete in the curve canvas never enters the main deletion backend");
    PumpEnterRegisterFrame(app.get(),owner,true);
    Check(mainRegistrations==1,"Enter in the main window still registers once");
    ce::CloseOwned(owner);DestroyWindow(owner);app->Hwnd()=nullptr;
}
void CameraSelectionAfterCurveWrite() {
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);app->EnglishUI()=2;
    auto* keys=mdl::KeyPoolCreate<mdl::CameraKey>();app->CameraKeys()=keys;
    const int frames[]={0,60,218,220};
    for(int i=0;i<4;++i){
        auto& key=keys[i];key.frame=frames[i];key.previous=i?i-1:0;key.next=i<3?i+1:0;
        key.eye[1]=10.f+float(i);key.distance=33;key.fov=30;key.parentModel=-1;
        std::memset(key.interpolation,20,12);std::memset(&key.interpolation[2][0],107,12);
    }
    keys[2].selected=1;
    std::vector<ce::Channel> channels;std::wstring error;
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"read camera before curve edit");
    channels[1].track.keys[2].value=18.79;
    Check(curve_host::Write(*app,curve_host::Kind::Camera,0,channels,error),"write one camera curve key");
    keys=app->CameraKeys();
    Check(!keys[0].selected&&!keys[1].selected&&keys[2].selected&&!keys[3].selected,
          "editing one camera curve keeps the native selection instead of selecting the whole track");
    app->CurrentFrame()=219;ReloadModels(app.get());RegisterCameraState(app.get(),219);
    std::vector<int> actual;
    for(unsigned i=0;;i=keys[i].next){actual.push_back(int(keys[i].frame));if(!keys[i].next)break;}
    Check(actual==std::vector<int>({0,60,218,219,220}),"registering the next camera frame retains every earlier key");
    for(int frame:{220,219,218,191,60,0}){
        app->CurrentFrame()=frame;ReloadModels(app.get());
        Check(std::isfinite(app->CameraPositionAxis(1))&&std::fabs(app->CameraPositionAxis(1))<100,
              "backward camera seeks remain finite after a curve edit and native registration");
    }
    curve_host::Reset(app.get());mdl::KeyPoolDestroy(app->CameraKeys());
}
void CameraFrameZeroRegistration() {
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);
    auto* keys=mdl::KeyPoolCreate<mdl::CameraKey>();app->CameraKeys()=keys;
    const int frames[]={0,169,193,210,220};
    for(int i=0;i<5;++i){
        auto& key=keys[i];key.frame=frames[i];key.previous=i?i-1:0;key.next=i<4?i+1:0;
        key.eye[1]=i==4?18.92f:10.f;key.distance=33;key.fov=30;key.parentModel=-1;
        std::memset(key.interpolation,20,12);std::memset(&key.interpolation[2][0],107,12);
    }
    app->CurrentFrame()=0;app->CameraPositionAxis(1)=10;RegisterCameraState(app.get(),0);
    app->CurrentFrame()=222;app->CameraPositionAxis(1)=19;RegisterCameraState(app.get(),222);
    std::vector<int> actual;
    for(unsigned i=0,hops=0;hops<10;++hops){actual.push_back(int(keys[i].frame));if(!keys[i].next)break;i=keys[i].next;}
    app->CurrentFrame()=191;ReloadModels(app.get());
    std::printf("camera_zero_register reachable=%zu y_at_191=%.9g\n",actual.size(),app->CameraPositionAxis(1));
    Check(actual==std::vector<int>({0,169,193,210,220,222}),
          "re-registering frame zero then the next camera key preserves the entire chain");
    Check(std::isfinite(app->CameraPositionAxis(1))&&std::fabs(app->CameraPositionAxis(1))<100,
          "camera interpolation never underflows after updating the frame-zero anchor");
    mdl::KeyPoolDestroy(app->CameraKeys());
}
template<class Key,class Record,class Register>
void GlobalFrameZeroRegistration(MMDApp& app,Key*& pool,Record record,Register add) {
    pool=mdl::KeyPoolCreate<Key>();pool[0].next=1;pool[1].frame=30;
    app.CurrentFrame()=0;record.frame=0;Check(add(&app,&record)!=0,"update global frame-zero anchor");
    Check(pool[0].next==1&&pool[1].previous==0,"updating frame zero does not allocate a second anchor");
    record.frame=60;Check(add(&app,&record)!=0,"append global frame after updating zero");
    Check(pool[0].frame==0&&pool[0].next==1&&pool[1].frame==30&&pool[1].next==2&&
          pool[2].frame==60&&pool[2].previous==1&&pool[2].next==0,"global registration keeps both neighbours and all earlier keys");
    mdl::KeyPoolDestroy(pool);
}
void GlobalFrameZeroRegistrations() {
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);
    GlobalFrameZeroRegistration(*app,app->LightKeys(),mdl::LightClipboardRecord{},RegisterLightKey);
    GlobalFrameZeroRegistration(*app,app->ShadowKeys(),mdl::ShadowClipboardRecord{},RegisterSelfShadowKey);
    GlobalFrameZeroRegistration(*app,app->GravityKeys(),mdl::GravityClipboardRecord{},RegisterGravityKey);
}
void CameraSparseRegistration() {
    auto app=std::make_unique<MMDApp>();std::memset(&app->state,0,sizeof app->state);
    app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>();
    app->CameraKeys()[0].fov=45;app->CameraKeys()[0].parentModel=-1;
    std::vector<ce::Channel> channels;std::wstring error;
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"read sparse camera fixture");
    for(auto& channel:channels){auto key=channel.track.keys[0];key.frame=20;channel.track.keys.push_back(key);}
    Check(curve_host::Write(*app,curve_host::Kind::Camera,0,channels,error),"create shared camera frame");
    channels[0].track.keys.pop_back();
    Check(curve_host::Write(*app,curve_host::Kind::Camera,0,channels,error),"remove one authored camera lane key");
    app->CurrentFrame()=20;ReloadModels(app.get());RegisterCameraState(app.get(),20);
    Check(curve_host::Read(*app,curve_host::Kind::Camera,0,channels,error),"read camera after explicit native registration");
    Check(channels[0].track.keys.size()==2&&channels[0].track.keys.back().frame==20,
          "native camera registration creates a key in every lane even when the native row payload is unchanged");
    curve_host::Reset(app.get());mdl::KeyPoolDestroy(app->CameraKeys());
}
}
int main() {CameraSparseRegistration();CameraEditorCommands();CameraSelectionAfterCurveWrite();CameraFrameZeroRegistration();GlobalFrameZeroRegistrations();MorphSync();SelectionSync();std::puts("PASS: curve synchronization");}
