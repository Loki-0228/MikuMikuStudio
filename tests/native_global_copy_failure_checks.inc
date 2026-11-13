// Include after native_global_paste_checks.inc (and native_key_paste_checks.inc).
// Entry: NativeGlobalCopyFailureChecks(). The parent TU supplies the opt-in route.
// Requires Check, GlobalRegistrationFixture, GlobalPasteKeys, RealFrameCopyBuffers,
// the native history test binding, and the already linked MinHook library.
// Exercises production 420, plus negative-to-negative 421 only. No cross-zero
// paste claim, production test switch, operator-new injector, or other test route.
// Counts come from real managed pools: this does not fabricate impossible pool
// extents to claim dynamic coverage of the size_t multiplication-overflow guard.
#include <MinHook.h>
#include "mikumikustudio/negative_frames.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "../src/plugins/native_pose_scope.hpp"
#include <limits>

namespace {
namespace global_copy_failure_checks {
constexpr std::array<unsigned,5> counts{{2,3,5,7,11}};
constexpr std::array<std::size_t,5> sizes{{
    counts[0]*sizeof(mdl::CameraClipboardRecord), counts[1]*sizeof(mdl::LightClipboardRecord),
    counts[2]*sizeof(mdl::ShadowClipboardRecord), counts[3]*sizeof(mdl::GravityClipboardRecord),
    counts[4]*sizeof(mdl::AccessoryClipboardKey)}};
static_assert(sizes[0]==144 && sizes[1]==84 && sizes[2]==60 && sizes[3]==196 && sizes[4]==572,
    "each global copy allocation has a unique byte size");
static_assert(noexcept(negative_frames::ClearClipboard(static_cast<MMDApp*>(nullptr))),
    "failed native copy must clear the negative clipboard without throwing");

std::array<void*,5> Buffers(MMDApp& app) {
    return {{app.CameraClipboard(),app.LightClipboard(),app.ShadowClipboard(),
        app.GravityClipboard(),app.AccessoryClipboard()}};
}
std::array<unsigned,5> Counts(MMDApp& app) {
    const auto& n=app.ClipboardCounts();return {{n.cameras,n.lights,n.shadows,n.gravity,n.accessories}};
}
template<class T> T* Owned(unsigned count=1) {
    auto* p=static_cast<T*>(std::malloc(sizeof(T)*count));
    Check(p!=nullptr,"allocate malloc-owned fixture clipboard");
    std::memset(p,0,sizeof(T)*count);return p;
}

// Only constant-initialized TLS and fixed storage are touched by the hooks.
// calloc is hooked as well: optimized malloc+memset may become calloc. Suppress
// nested CRT calls so one outer allocation is counted exactly once.
struct Trace {
    struct Block {void* pointer=nullptr;unsigned family=0;bool live=false;};
    DWORD thread=0;
    int failFamily=-1;
    std::array<unsigned,5> hits{},mallocHits{},callocHits{};
    std::array<void*,5> old{};
    std::array<bool,5> oldFreed{};
    std::array<void*,5> model{};
    std::array<Block,32> blocks{};
    unsigned used=0,failures=0;
    bool overflow=false,modelFreed=false,threw=false,rawBatchComplete=false;
    int Family(std::size_t bytes) const noexcept {
        // The immutable receipt copies these buffers afterward. Observe only
        // the first raw allocation batch here; receipt OOM has its own suite.
        if(rawBatchComplete)return -1;
        for(unsigned i=0;i<sizes.size();++i)if(bytes==sizes[i])return int(i);
        return -1;
    }
    bool Fail(int family,bool callocCall) noexcept {
        if(family<0)return false;
        ++hits[family];++(callocCall?callocHits[family]:mallocHits[family]);
        if(family==failFamily && !failures){++failures;return true;}
        return false;
    }
    void Acquire(void* p,int family) noexcept {
        if(!p || family<0)return;
        if(used==blocks.size()){overflow=true;return;}
        blocks[used++]={p,unsigned(family),true};
        if(family==4)rawBatchComplete=true;
    }
    void Release(void* p) noexcept {
        if(!p)return;
        for(auto modelPointer:model)if(p==modelPointer)modelFreed=true;
        for(unsigned i=0;i<old.size();++i)if(p==old[i] && !oldFreed[i]){oldFreed[i]=true;return;}
        for(unsigned i=0;i<used;++i)if(blocks[i].live && blocks[i].pointer==p){blocks[i].live=false;return;}
    }
};
thread_local Trace* activeTrace=nullptr;
thread_local unsigned hookDepth=0;
using MallocFn=void* (__cdecl*)(std::size_t);
using CallocFn=void* (__cdecl*)(std::size_t,std::size_t);
using FreeFn=void (__cdecl*)(void*);
MallocFn originalMalloc=nullptr;
CallocFn originalCalloc=nullptr;
FreeFn originalFree=nullptr;
Trace* Recording() noexcept {
    return activeTrace && !hookDepth && activeTrace->thread==GetCurrentThreadId()?activeTrace:nullptr;
}
void* __cdecl Malloc(std::size_t bytes) {
    Trace* trace=Recording();const int family=trace?trace->Family(bytes):-1;
    if(trace && trace->Fail(family,false))return nullptr;
    ++hookDepth;void* p=originalMalloc(bytes);--hookDepth;
    if(trace)trace->Acquire(p,family);return p;
}
void* __cdecl Calloc(std::size_t count,std::size_t width) {
    Trace* trace=Recording();
    const bool representable=!width || count<=(std::numeric_limits<std::size_t>::max)()/width;
    const int family=trace && representable?trace->Family(count*width):-1;
    if(trace && trace->Fail(family,true))return nullptr;
    ++hookDepth;void* p=originalCalloc(count,width);--hookDepth;
    if(trace)trace->Acquire(p,family);return p;
}
void __cdecl Free(void* p) {
    if(auto* trace=Recording())trace->Release(p);
    ++hookDepth;originalFree(p);--hookDepth;
}
struct Hooks {
    std::array<void*,3> targets{{reinterpret_cast<void*>(&std::malloc),
        reinterpret_cast<void*>(&std::calloc),reinterpret_cast<void*>(&std::free)}};
    std::array<bool,3> created{},enabled{};
    bool ownsInitialization=false,clean=true;
    MH_STATUS setup=MH_OK;
    bool Install() noexcept {
        setup=MH_Initialize();ownsInitialization=setup==MH_OK;
        if(setup!=MH_OK && setup!=MH_ERROR_ALREADY_INITIALIZED)return false;
        void* detours[]={reinterpret_cast<void*>(&Malloc),reinterpret_cast<void*>(&Calloc),reinterpret_cast<void*>(&Free)};
        void** originals[]={reinterpret_cast<void**>(&originalMalloc),reinterpret_cast<void**>(&originalCalloc),
            reinterpret_cast<void**>(&originalFree)};
        for(unsigned i=0;i<targets.size();++i){
            setup=MH_CreateHook(targets[i],detours[i],originals[i]);
            if(setup!=MH_OK)return false;created[i]=true;
        }
        for(unsigned i=0;i<targets.size();++i){
            setup=MH_EnableHook(targets[i]);if(setup!=MH_OK)return false;enabled[i]=true;
        }
        return true;
    }
    bool Remove() noexcept {
        // Never disable/remove another test's hooks, or uninitialize its library.
        activeTrace=nullptr;
        for(unsigned i=0;i<targets.size();++i)if(enabled[i]){
            const auto s=MH_DisableHook(targets[i]);
            if(s!=MH_OK && s!=MH_ERROR_DISABLED)clean=false;else enabled[i]=false;
        }
        for(unsigned i=0;i<targets.size();++i)if(created[i]){
            if(MH_RemoveHook(targets[i])!=MH_OK)clean=false;else created[i]=enabled[i]=false;
        }
        if(ownsInitialization){if(MH_Uninitialize()!=MH_OK)clean=false;else ownsInitialization=false;}
        return clean;
    }
    ~Hooks(){Remove();}
};

// An invisible, nonactivating host with real paste buttons and the real menu ID.
// No ShowWindow, SetFocus, message pump, or user-owned window is used here.
struct Controls {
    HWND host=nullptr,paste=nullptr,reverse=nullptr;
    HMENU menu=nullptr;
    Controls() {
        host=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,L"STATIC",L"copy checks",
            WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(host!=nullptr,"create hidden nonactivating copy control host");
        paste=CreateWindowExW(0,L"BUTTON",L"paste",WS_CHILD,0,0,1,1,host,
            reinterpret_cast<HMENU>(INT_PTR(panel::kPasteButton)),GetModuleHandleW(nullptr),nullptr);
        reverse=CreateWindowExW(0,L"BUTTON",L"reverse",WS_CHILD,0,0,1,1,host,
            reinterpret_cast<HMENU>(INT_PTR(panel::kReversePasteButton)),GetModuleHandleW(nullptr),nullptr);
        menu=CreateMenu();
        Check(paste && reverse && menu && AppendMenuW(menu,MF_STRING,0xFA,L"paste") && SetMenu(host,menu),
            "hidden host owns actual paste controls and menu item");
    }
    ~Controls(){if(host){SetMenu(host,nullptr);DestroyWindow(host);}if(menu)DestroyMenu(menu);}
    void EnableOld(){EnableWindow(paste,TRUE);EnableWindow(reverse,TRUE);EnableMenuItem(menu,0xFA,MF_ENABLED);}
    void Expect(bool canPaste,bool nativeMenu) const {
        Check(!IsWindowVisible(host) && !IsWindowVisible(paste) && !IsWindowVisible(reverse),
            "copy controls stay hidden");
        Check(bool(IsWindowEnabled(paste))==canPaste && !IsWindowEnabled(reverse),
            "actual paste and reverse-paste controls follow the complete copy outcome");
        const auto flags=GetMenuState(menu,0xFA,MF_BYCOMMAND);
        Check(flags!=UINT(-1) && (!(flags&(MF_DISABLED|MF_GRAYED)))==nativeMenu,
            "actual native paste menu follows the positive clipboard outcome");
    }
};

template<class K> void Pool(K*& pool,unsigned selected,unsigned first) {
    mdl::KeyPoolDestroy(pool);pool=mdl::KeyPoolCreate<K>(selected+2);
    Check(pool!=nullptr,"create managed source key pool");
    for(unsigned i=0;i<selected+2;++i){
        pool[i]={};pool[i].frame=i?first+(i-1)*6:0;
        pool[i].previous=i?i-1:0;pool[i].next=i<selected?i+1:0;
        pool[i].selected=i && i<=selected?std::uint8_t(2+i%5):0;
    }
}
struct CopyFixture : GlobalRegistrationFixture {
    RealFrameCopyBuffers modelBuffers{*app};
    plugins::NativePoseTestBinding binding{*app,history};
    Controls controls;
    std::array<mdl::AccessoryRecord,2> accessory{};
    MMDApp* previous=g_Block;
    CopyFixture() {
        g_Block=app.get();negative_frames::Reset(app.get());
        Pool(app->CameraKeys(),counts[0],21);Pool(app->LightKeys(),counts[1],34);
        Pool(app->ShadowKeys(),counts[2],15);Pool(app->GravityKeys(),counts[3],9);
        for(unsigned slot=0;slot<255;++slot)
            for(std::size_t i=0;i<mdl::KeyPoolCapacity(app->AccessoryKeys(slot));++i)app->AccessoryKeys(slot)[i].selected=0;
        Pool(app->AccessoryKeys(7),4,27);Pool(app->AccessoryKeys(254),7,45);
        for(unsigned i=0;i<mdl::KeyPoolCapacity(app->CameraKeys());++i){
            auto& k=app->CameraKeys()[i];k.distance=-31.25f-float(i);k.fov=37+int(i)*9;
            k.perspective=std::uint8_t(i%2);k.parentModel=i%2?-1:17;k.parentBone=23+int(i);
            for(unsigned a=0;a<3;++a){k.eye[a]=float(11+i*7+a)/4;k.target[a]=float(3+i*5+a)/16;}
            for(unsigned a=0;a<4;++a)for(unsigned b=0;b<6;++b)k.interpolation[a][b]=std::uint8_t(3+i*7+a*6+b);
            std::memset(k.reserved1,0xA7,sizeof(k.reserved1));std::memset(k.reserved2,0xB9,sizeof(k.reserved2));
        }
        for(unsigned i=0;i<mdl::KeyPoolCapacity(app->LightKeys());++i){
            auto& k=app->LightKeys()[i];for(unsigned a=0;a<3;++a){k.direction[a]=float(1+i*3+a)/8;k.color[a]=float(1+i+a)/16;}
            std::memset(k.reserved1,0xAD,sizeof(k.reserved1));
        }
        for(unsigned i=0;i<mdl::KeyPoolCapacity(app->ShadowKeys());++i){auto& k=app->ShadowKeys()[i];
            k.mode=std::uint8_t(i%3);k.distance=.0125f*float(1+i);std::memset(k.reserved0,0xBC,sizeof(k.reserved0));}
        for(unsigned i=0;i<mdl::KeyPoolCapacity(app->GravityKeys());++i){auto& k=app->GravityKeys()[i];
            k.acceleration=7.25f+float(i);k.noise=31+int(i)*3;k.noiseEnabled=std::uint8_t(i%2);
            for(unsigned a=0;a<3;++a)k.direction[a]=float(int(i)-int(a)-3)/8;
            std::memset(k.reserved1,0xCD,sizeof(k.reserved1));}
        for(unsigned slot:{7u,254u})for(unsigned i=0;i<mdl::KeyPoolCapacity(app->AccessoryKeys(slot));++i){
            auto& k=app->AccessoryKeys(slot)[i];k.parentModel=i%2?-1:19;k.parentBone=11+int(i);
            k.visible=std::uint8_t(i%2);k.shadowEnabled=std::uint8_t((i/2)%2);
            k.scale=1.125f+float(i)/8;k.opacity=.875f-float(i)/32;
            for(unsigned a=0;a<3;++a){k.position[a]=float(slot+i*3+a)/4;k.rotation[a]=float(i*3+a)/16;}
        }
        app->AccessorySlot(7)=&accessory[0];app->AccessorySlot(254)=&accessory[1];
        app->BoneClipboard()=Owned<mdl::BoneClipboardRecord>(2);
        app->MorphClipboard()=Owned<mdl::MorphClipboardRecord>(3);
        app->DisplayClipboard()=Owned<mdl::DisplayClipboardRecord>();
        std::memset(app->BoneClipboard(),0x35,2*sizeof(mdl::BoneClipboardRecord));
        std::memset(app->MorphClipboard(),0x6A,3*sizeof(mdl::MorphClipboardRecord));
        auto& d=*app->DisplayClipboard();d.frame=73;d.visible=1;d.ikCount=2;d.selectorCount=2;
        d.ikStates=Owned<mdl::IkClipboardState>(2);d.selectorStates=Owned<mdl::SelectorClipboardState>(2);
        std::memset(d.ikStates,0x57,2*sizeof(*d.ikStates));std::memset(d.selectorStates,0x79,2*sizeof(*d.selectorStates));
        auto& n=app->ClipboardCounts();n.bones=2;n.morphs=3;n.displays=1;
        SeedOld();
    }
    ~CopyFixture(){
        history.Reset();history.Collect();negative_frames::Reset(app.get());
        for(void* p:Buffers(*app))std::free(p);
        app->CameraClipboard()=nullptr;app->LightClipboard()=nullptr;app->ShadowClipboard()=nullptr;
        app->GravityClipboard()=nullptr;app->AccessoryClipboard()=nullptr;
        app->AccessorySlot(7)=app->AccessorySlot(254)=nullptr;g_Block=previous;
    }
    void SeedOld(){
        for(void* p:Buffers(*app))std::free(p);
        app->CameraClipboard()=Owned<mdl::CameraClipboardRecord>();
        app->LightClipboard()=Owned<mdl::LightClipboardRecord>();
        app->ShadowClipboard()=Owned<mdl::ShadowClipboardRecord>();
        app->GravityClipboard()=Owned<mdl::GravityClipboardRecord>();
        app->AccessoryClipboard()=Owned<mdl::AccessoryClipboardKey>();
        app->CameraClipboard()->frame=81;app->LightClipboard()->frame=82;app->ShadowClipboard()->frame=83;
        app->GravityClipboard()->frame=84;app->AccessoryClipboard()->frameOffset=85;
        auto& n=app->ClipboardCounts();n.cameras=n.lights=n.shadows=n.gravity=n.accessories=1;
        controls.EnableOld();
    }
    void Select(bool selected){
        const auto mark=[&](auto* pool,unsigned n){for(std::size_t i=0;i<mdl::KeyPoolCapacity(pool);++i)
            pool[i].selected=selected && i && i<=n?std::uint8_t(2+i%5):0;};
        mark(app->CameraKeys(),counts[0]);mark(app->LightKeys(),counts[1]);
        mark(app->ShadowKeys(),counts[2]);mark(app->GravityKeys(),counts[3]);
        for(unsigned slot=0;slot<255;++slot)mark(app->AccessoryKeys(slot),slot==7?4:slot==254?7:0);
    }
    void SeedHistory(){
        app->CurrentFrame()=-70;
        Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Light,-70),"seed negative light and shared undo");
        Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Camera,-40),"seed negative camera and shared undo");
        Check(negative_frames::RegisterGlobal(app.get(),negative_frames::Global::Shadow,-20),"seed disposable negative shadow");
        Check(history.History(false)==MMS_OK && history.UndoCount()==2 && history.RedoCount()==1,
            "copy fixture has meaningful undo and redo branches");
        negative_frames::SelectRange(app.get(),1,2);app->CurrentFrame()=7;
    }
};

std::vector<unsigned char> Bytes(const void* p,std::size_t size) {
    const auto* begin=static_cast<const unsigned char*>(p);return {begin,begin+size};
}
std::vector<unsigned char> NegativeRecord(MMDApp& app,const char* name) {
    pmm_extension::Document document;negative_frames::Capture(app,document);
    const auto* record=pmm_extension::Find(document,name);return record?record->value:std::vector<unsigned char>{};
}
// Compare the complete app-state bytes except the five documented clipboard
// outputs. On x64 their pointers live outside state; on x86 mask the real slots.
std::vector<unsigned char> StateBytes(MMDApp& app) {
    auto bytes=Bytes(&app.state,sizeof(app.state));
    const auto mask=[&](const void* p,std::size_t size){
        const auto base=reinterpret_cast<std::uintptr_t>(&app.state),at=reinterpret_cast<std::uintptr_t>(p);
        if(at>=base && at-base<=bytes.size() && size<=bytes.size()-(at-base))
            std::memset(bytes.data()+(at-base),0,size);
    };
    mask(&app.CameraClipboard(),sizeof(void*));mask(&app.LightClipboard(),sizeof(void*));
    mask(&app.ShadowClipboard(),sizeof(void*));mask(&app.GravityClipboard(),sizeof(void*));mask(&app.AccessoryClipboard(),sizeof(void*));
    auto& n=app.ClipboardCounts();mask(&n.cameras,sizeof(n.cameras));mask(&n.lights,sizeof(n.lights));
    mask(&n.shadows,sizeof(n.shadows));mask(&n.gravity,sizeof(n.gravity));mask(&n.accessories,sizeof(n.accessories));return bytes;
}
struct Snapshot {
    GlobalPasteKeys keys;
    std::vector<unsigned char> state,light,negative,selection,bones,morphs,display,ik,selectors;
    std::array<void*,5> model;
    std::array<const void*,259> pools{};
    std::array<std::size_t,259> capacities{};
    std::size_t undo,redo,retained;
    std::uint64_t revision=0,epoch,sceneRevision,sceneGeneration;
    explicit Snapshot(CopyFixture& f):keys(*f.app),state(StateBytes(*f.app)),light(Bytes(&f.app->SceneLight(),sizeof(D3DLIGHT9))),
        negative(NegativeRecord(*f.app,negative_frames::kPmmKey)),selection(NegativeRecord(*f.app,negative_frames::kPmmSelectionKey)),
        bones(Bytes(f.app->BoneClipboard(),2*sizeof(mdl::BoneClipboardRecord))),morphs(Bytes(f.app->MorphClipboard(),3*sizeof(mdl::MorphClipboardRecord))),
        display(Bytes(f.app->DisplayClipboard(),sizeof(mdl::DisplayClipboardRecord))),
        ik(Bytes(f.app->DisplayClipboard()->ikStates,2*sizeof(mdl::IkClipboardState))),
        selectors(Bytes(f.app->DisplayClipboard()->selectorStates,2*sizeof(mdl::SelectorClipboardState))),
        model{{f.app->BoneClipboard(),f.app->MorphClipboard(),f.app->DisplayClipboard(),
            f.app->DisplayClipboard()->ikStates,f.app->DisplayClipboard()->selectorStates}},
        undo(f.history.UndoCount()),redo(f.history.RedoCount()),retained(f.history.RetainedBytes()),epoch(f.history.NativeEpoch()),
        sceneRevision(f.app->SceneRevision()),sceneGeneration(f.app->SceneGeneration()) {
        Check(f.history.Revision(revision)==MMS_OK,"snapshot history revision before copy");
        unsigned index=0;const auto add=[&](const auto* p){pools[index]=p;capacities[index++]=mdl::KeyPoolCapacity(p);};
        add(f.app->CameraKeys());add(f.app->LightKeys());add(f.app->ShadowKeys());add(f.app->GravityKeys());
        for(unsigned slot=0;slot<255;++slot)add(f.app->AccessoryKeys(slot));
    }
    void Expect(CopyFixture& f) const {
        auto& app=*f.app;std::uint64_t now=0;
        Check(f.history.Revision(now)==MMS_OK && now==revision && f.history.NativeEpoch()==epoch &&
            f.history.UndoCount()==undo && f.history.RedoCount()==redo && f.history.RetainedBytes()==retained,
            "420 preserves undo redo revision epoch and retained history");
        Check(keys.Same(app) && StateBytes(app)==state && Bytes(&app.SceneLight(),sizeof(D3DLIGHT9))==light &&
            app.SceneRevision()==sceneRevision && app.SceneGeneration()==sceneGeneration,
            "420 preserves every native key and selection byte, app state and scene revisions");
        Check(NegativeRecord(app,negative_frames::kPmmKey)==negative && NegativeRecord(app,negative_frames::kPmmSelectionKey)==selection,
            "420 preserves every negative key payload and selection byte");
        unsigned index=0;const auto same=[&](const auto* p){const auto i=index++;return pools[i]==p && capacities[i]==mdl::KeyPoolCapacity(p);};
        bool stable=same(app.CameraKeys());stable=same(app.LightKeys()) && stable;
        stable=same(app.ShadowKeys()) && stable;stable=same(app.GravityKeys()) && stable;
        for(unsigned slot=0;slot<255;++slot)stable=same(app.AccessoryKeys(slot)) && stable;
        Check(stable,"copy never replaces or grows a project key pool");
        Check(app.BoneClipboard()==model[0] && app.MorphClipboard()==model[1] && app.DisplayClipboard()==model[2],
            "global copy leaves all model clipboard pointers intact");
        Check(Bytes(app.BoneClipboard(),bones.size())==bones && Bytes(app.MorphClipboard(),morphs.size())==morphs &&
            Bytes(app.DisplayClipboard(),display.size())==display,"global copy retains all model clipboard records and nested pointers");
        Check(Bytes(app.DisplayClipboard()->ikStates,ik.size())==ik && Bytes(app.DisplayClipboard()->selectorStates,selectors.size())==selectors,
            "global copy retains nested model IK and selector arrays");
    }
};

Trace Copy(CopyFixture& f,int failFamily=-1) {
    Trace trace;trace.thread=GetCurrentThreadId();trace.failFamily=failFamily;trace.old=Buffers(*f.app);
    trace.model={{f.app->BoneClipboard(),f.app->MorphClipboard(),f.app->DisplayClipboard(),
        f.app->DisplayClipboard()->ikStates,f.app->DisplayClipboard()->selectorStates}};
    Check(failAfter==-1 && !activeTrace,"malloc-specific copy checks do not use the new failure injector");
    Hooks hooks;const bool installed=hooks.Install();
    const auto previousWindow=f.app->MainWindow();
    if(installed){
        f.app->MainWindow()=f.controls.host;
        activeTrace=&trace;
        try {CmdControl400(f.app.get(),f.controls.host,420,0);}catch(...){trace.threw=true;}
        activeTrace=nullptr;
        f.app->MainWindow()=previousWindow;
    }
    // Check exits the process rather than unwinding: no assertion or printing
    // is allowed before this explicit cleanup, including on install failure.
    const bool removed=hooks.Remove();
    if(!installed || !removed)std::fprintf(stderr,"global copy hook setup=%d removed=%d\n",int(hooks.setup),int(removed));
    Check(removed && installed,"real malloc calloc and free hooks install and are completely removed");
    Check(!trace.threw && !trace.overflow && !trace.modelFreed && !hookDepth,
        "copy returns normally within the fixed audit capacity without freeing model buffers");
    for(unsigned i=0;i<trace.old.size();++i)if(trace.old[i])Check(trace.oldFreed[i],"copy frees every previous global allocation");
    return trace;
}
void Empty(CopyFixture& f) {
    Check(Counts(*f.app)==std::array<unsigned,5>{} && Buffers(*f.app)==std::array<void*,5>{},
        "all five global counts and pointers are empty together");
}
void FailureTrace(const Trace& trace,unsigned family) {
    Check(trace.failures==1 && trace.hits[family]==1 && trace.mallocHits[family]+trace.callocHits[family]==1,
        "fault hit the exact requested global malloc or calloc once");
    for(unsigned i=0;i<family;++i)Check(trace.hits[i]==1,"failed batch reached each preceding family allocation exactly once");
    for(unsigned i=0;i<trace.used;++i)Check(!trace.blocks[i].live,"failed copy frees every observed partial global allocation");
}

template<class Record,class Key,class Convert>
void Payload(Record* actual,unsigned count,Key* pool,unsigned origin,Convert convert) {
    unsigned out=0;
    for(std::size_t i=0;i<mdl::KeyPoolCapacity(pool);++i)if(pool[i].selected){
        Check(actual && out<count,"successful clipboard has storage for each selected source");
        Record expected{};convert(expected,pool[i]);expected.frame=pool[i].frame-origin;
        Check(!std::memcmp(actual+out,&expected,sizeof(expected)),"all clipboard fields and initialized reserved bytes match the selected source");++out;
    }
    Check(out==count,"copy contains selected keys only, in pool order");
}
void FullPayload(CopyFixture& f) {
    auto& app=*f.app;const auto n=Counts(app);unsigned origin=UINT32_MAX;
    const auto minimum=[&](const auto* pool){for(std::size_t i=0;i<mdl::KeyPoolCapacity(pool);++i)
        if(pool[i].selected)origin=(std::min)(origin,pool[i].frame);};
    minimum(app.CameraKeys());minimum(app.LightKeys());minimum(app.ShadowKeys());minimum(app.GravityKeys());
    for(unsigned slot=0;slot<255;++slot)minimum(app.AccessoryKeys(slot));
    Check(origin==9,"the global minimum comes from gravity, after earlier families");
    Payload(app.CameraClipboard(),n[0],app.CameraKeys(),origin,[](auto& r,const auto& k){
        std::memcpy(r.eye,k.eye,sizeof(r.eye));std::memcpy(r.target,k.target,sizeof(r.target));
        std::memcpy(r.interpolation,k.interpolation,sizeof(r.interpolation));r.fov=k.fov;r.perspective=k.perspective;
        r.distance=k.distance;r.parentModel=k.parentModel;r.parentBone=k.parentBone;});
    Payload(app.LightClipboard(),n[1],app.LightKeys(),origin,[](auto& r,const auto& k){
        std::memcpy(r.direction,k.direction,sizeof(r.direction));std::memcpy(r.color,k.color,sizeof(r.color));});
    Payload(app.ShadowClipboard(),n[2],app.ShadowKeys(),origin,[](auto& r,const auto& k){r.mode=k.mode;r.distance=k.distance;});
    Payload(app.GravityClipboard(),n[3],app.GravityKeys(),origin,[](auto& r,const auto& k){
        r.acceleration=k.acceleration;std::memcpy(r.direction,k.direction,sizeof(r.direction));r.noise=k.noise;r.noiseEnabled=k.noiseEnabled;});
    unsigned out=0;
    for(unsigned slot=0;slot<255;++slot)for(std::size_t i=0;i<mdl::KeyPoolCapacity(app.AccessoryKeys(slot));++i){
        const auto& k=app.AccessoryKeys(slot)[i];if(!k.selected)continue;
        Check(app.AccessoryClipboard() && out<n[4],"accessory clipboard covers selected keys from both source slots");
        mdl::AccessoryClipboardKey r{};r.frameOffset=k.frame-origin;r.slot=std::uint8_t(slot);
        r.visible=k.visible;r.shadowEnabled=k.shadowEnabled;r.parentModel=k.parentModel;r.parentBone=k.parentBone;
        std::memcpy(r.position,k.position,sizeof(r.position));std::memcpy(r.rotation,k.rotation,sizeof(r.rotation));r.scale=k.scale;r.opacity=k.opacity;
        Check(!std::memcmp(app.AccessoryClipboard()+out,&r,sizeof(r)),"every accessory field uses the same global origin and retains its slot");++out;
    }
    Check(out==n[4],"accessory count spans both slots without unselected entries");
    Check(app.CameraClipboard()[0].frame==12 && app.LightClipboard()[0].frame==25 &&
        app.ShadowClipboard()[0].frame==6 && app.GravityClipboard()[0].frame==0 && app.AccessoryClipboard()[0].frameOffset==18,
        "families keep distinct first offsets relative to one shared minimum");
}

void SuccessAndEmpty(plugins::SharedHistory::RecordingMode mode) {
    CopyFixture f;Check(f.history.SetMode(mode)==MMS_OK,"configure copy history mode");f.SeedHistory();
    for(unsigned repeat=0;repeat<2;++repeat){
        const Snapshot before(f);const auto trace=Copy(f);before.Expect(f);
        Check(Counts(*f.app)==counts && !trace.failures,"complete copy publishes exactly the five selected counts");
        const auto buffers=Buffers(*f.app);
        for(unsigned family=0;family<5;++family){
            Check(trace.hits[family]==1,"successful copy reaches each unique malloc/calloc size exactly once");
            unsigned live=0;for(unsigned i=0;i<trace.used;++i)if(trace.blocks[i].family==family && trace.blocks[i].live){
                ++live;Check(trace.blocks[i].pointer==buffers[family],"published buffer is the observed real CRT allocation");}
            Check(live==1,"one owned allocation remains per successful family");
        }
        FullPayload(f);f.controls.Expect(true,true);
    }
    // Change both payload and selected counts: a repeat must not retain a tail
    // from the previous copy, even when the allocator reuses its old address.
    f.app->CameraKeys()[2].selected=0;f.app->LightKeys()[3].selected=0;f.app->CameraKeys()[1].distance=-123.5f;
    {const Snapshot before(f);Copy(f);before.Expect(f);FullPayload(f);
        Check(f.app->ClipboardCounts().cameras==1 && f.app->ClipboardCounts().lights==2,"repeat copy replaces smaller selections and edited payloads");}
    f.Select(false);
    for(unsigned repeat=0;repeat<2;++repeat){
        f.controls.EnableOld();const Snapshot before(f);const auto trace=Copy(f);before.Expect(f);Empty(f);f.controls.Expect(false,false);
        Check(trace.hits==std::array<unsigned,5>{} && !trace.failures && !trace.used,"empty copy requests none of the five global buffers");
    }
    f.Select(true);{const Snapshot before(f);Copy(f);before.Expect(f);FullPayload(f);f.controls.Expect(true,true);}
}

void Failures(plugins::SharedHistory::RecordingMode mode,bool negativeSource) {
    for(unsigned family=0;family<5;++family){
        CopyFixture f;Check(f.history.SetMode(mode)==MMS_OK,"configure failed-copy history mode");f.SeedHistory();
        if(negativeSource){
            // Old negative clipboard is LIGHT; current selection is CAMERA.
            // A failed 420 must neither retain the old one nor rebuild the new.
            f.Select(false);negative_frames::SelectRange(f.app.get(),-70,-70);
            Copy(f);Empty(f);f.controls.Expect(true,false);
            negative_frames::SelectRange(f.app.get(),-40,-40);f.Select(true);f.SeedOld();
        }
        const Snapshot before(f);const auto trace=Copy(f,int(family));
        FailureTrace(trace,family);Empty(f);before.Expect(f);f.controls.Expect(false,false);
        if(negativeSource){
            // All possible destinations stay negative, so a no-op cannot be
            // explained by the still-unimplemented cross-zero paste path.
            f.app->CurrentFrame()=-500;const Snapshot noPaste(f);
            CmdControl400(f.app.get(),nullptr,421,0);noPaste.Expect(f);Empty(f);
            Check(negative_frames::Count(f.app.get())==2,"failed copy leaves neither stale light nor rebuilt camera clipboard to paste");
            // RefreshControls would re-enable paste if a side clipboard survived.
            f.app->MainWindow()=f.controls.host;negative_frames::RefreshControls(f.app.get());f.app->MainWindow()=nullptr;
            f.controls.Expect(false,false);
        }
        std::printf("  global copy allocation failure: family=%u negative=%u malloc=%u calloc=%u\n",
            family,unsigned(negativeSource),trace.mallocHits[family],trace.callocHits[family]);
    }
}

std::uint32_t Read32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) | (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24);
}
std::vector<unsigned char> NegativeRow(CopyFixture& f,int target,int frame) {
    const auto bytes=NegativeRecord(*f.app,negative_frames::kPmmKey);constexpr unsigned stride=36+16*8;
    Check(bytes.size()>=8 && bytes.size()==8+std::size_t(Read32(bytes.data()+4))*stride,"negative source has complete serialized key rows");
    for(std::size_t at=8;at<bytes.size();at+=stride)
        if(Read32(bytes.data()+at+8)==unsigned(target) && static_cast<std::int32_t>(Read32(bytes.data()+at+12))==frame)
            return {bytes.begin()+at,bytes.begin()+at+stride};
    return {};
}
void NegativeOnly(plugins::SharedHistory::RecordingMode mode) {
    CopyFixture f;Check(f.history.SetMode(mode)==MMS_OK,"configure negative-only copy history mode");f.SeedHistory();
    f.Select(false);negative_frames::SelectRange(f.app.get(),-70,-40);
    const auto light=NegativeRow(f,3,-70),camera=NegativeRow(f,2,-40);
    Check(!light.empty() && !camera.empty(),"negative-only copy has two distinct selected families");
    for(unsigned repeat=0;repeat<2;++repeat){
        const Snapshot before(f);const auto trace=Copy(f);before.Expect(f);Empty(f);f.controls.Expect(true,false);
        Check(trace.hits==std::array<unsigned,5>{} && !trace.failures,"empty positive selection succeeds without a native allocation");
    }
    const auto native=GlobalPasteKeys(*f.app);const auto undo=f.history.UndoCount();
    f.app->CurrentFrame()=-300;const auto beforeKeys=NegativeRecord(*f.app,negative_frames::kPmmKey);
    const auto beforeSelection=NegativeRecord(*f.app,negative_frames::kPmmSelectionKey);
    CmdControl400(f.app.get(),nullptr,421,0);
    Check(native.Same(*f.app) && negative_frames::Count(f.app.get())==4 &&
        f.history.UndoCount()==undo+1 && !f.history.RedoCount(),"negative-only real 421 publishes one shared step without touching native pools");
    auto expectedLight=light,expectedCamera=camera;
    const auto frame=[](std::vector<unsigned char>& row,int value){for(unsigned i=0;i<4;++i)row[12+i]=std::uint8_t(std::uint32_t(value)>>(8*i));};
    frame(expectedLight,-300);frame(expectedCamera,-270);
    Check(NegativeRow(f,3,-300)==expectedLight && NegativeRow(f,2,-270)==expectedCamera,
        "negative-only copy retains complete source values and shared offsets zero and thirty");
    const auto pastedKeys=NegativeRecord(*f.app,negative_frames::kPmmKey),pastedSelection=NegativeRecord(*f.app,negative_frames::kPmmSelectionKey);
    // History replay preserves current selection on surviving source keys.
    // Pasting deselects those keys; undo removes the new rows without reselecting
    // the originals. The selection header/CRC still describes beforeKeys.
    auto undoneSelection=beforeSelection;
    Check(undoneSelection.size()==14,"two original negative keys have two selection flags");
    undoneSelection[12]=undoneSelection[13]=0;
    Check(f.history.History(false)==MMS_OK && NegativeRecord(*f.app,negative_frames::kPmmKey)==beforeKeys &&
        NegativeRecord(*f.app,negative_frames::kPmmSelectionKey)==undoneSelection,"negative-only paste undo restores original keys and preserves surviving selection");
    Check(f.history.History(true)==MMS_OK && NegativeRecord(*f.app,negative_frames::kPmmKey)==pastedKeys &&
        NegativeRecord(*f.app,negative_frames::kPmmSelectionKey)==pastedSelection,"negative-only paste redo restores exact pasted keys and selections");
    // Empty BOTH selections must also discard a previously populated side clipboard.
    negative_frames::SelectRange(f.app.get(),1,2);f.controls.EnableOld();
    {const Snapshot before(f);Copy(f);before.Expect(f);Empty(f);f.controls.Expect(false,false);}
    f.app->CurrentFrame()=-600;{const Snapshot before(f);CmdControl400(f.app.get(),nullptr,421,0);before.Expect(f);}
}
} // namespace global_copy_failure_checks

void NativeGlobalCopyFailureChecks() {
    using namespace global_copy_failure_checks;
    for(auto mode:{plugins::SharedHistory::RecordingMode::AllOperations,plugins::SharedHistory::RecordingMode::ExplicitOnly}){
        SuccessAndEmpty(mode);Failures(mode,false);Failures(mode,true);NegativeOnly(mode);
    }
}
} // namespace
