// Include after ObjectCommandFixture and the native/plugin composition helpers.
#include "../src/plugins/native_model_outline_color.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include "mikumikustudio/skinning_backend.hpp"
#include <MinHook.h>
#include <wrl/client.h>
#include <commdlg.h>
#include <type_traits>
namespace mikumikustudio {
bool UpdateModelVertexBuffersCpu(MMDApp*, unsigned char*, const float[16]);
float ModelOutlineDistance(MMDApp*, unsigned char*, const float[16]);
}
namespace model_outline_color_checks {
using namespace plugins;
static_assert(!std::is_copy_constructible_v<NativeModelOutlineColorPlan>);
static_assert(!std::is_copy_assignable_v<NativeModelOutlineColorPlan>);
using Microsoft::WRL::ComPtr;
constexpr unsigned count = 4, side = 64;
constexpr std::array<DWORD, count> originalColors{0xff102030, 0xff204060, 0xff306090, 0xff4080c0};
std::array<float, 16> Identity() { std::array<float, 16> m{}; m[0]=m[5]=m[10]=m[15]=1; return m; }
struct Fixture : ObjectCommandFixture {
    ComPtr<IDirect3D9> d3d;
    ComPtr<IDirect3DDevice9> device;
    ComPtr<IDirect3DSurface9> pixels;
    skinning::Preference preference = skinning::GetPreference();
    const std::array<float,16> world = Identity();
    explicit Fixture(bool secondPmx = true) {
        d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION)); Check(bool(d3d), "outline color creates real D3D9 runtime");
        D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
        pp.BackBufferWidth=pp.BackBufferHeight=side; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
        Check(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,app->MainWindow(),
            D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&device)), "outline color hidden real D3D9 device");
        Check(SUCCEEDED(device->CreateOffscreenPlainSurface(side,side,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&pixels,nullptr)), "outline color pixel inspection surface");
        std::memcpy(&app->ViewRotationTransform(),world.data(),sizeof(world));
        app->CameraReferenceMode()=CameraAttachmentReference::None; app->CameraDistance()=-20; app->CameraFov()=30;
        skinning::SetPreference({skinning::Mode::Cpu});
        for(unsigned slot=0;slot<2;++slot) {
            auto& m=*mdl::Mdl(app->ModelSlot(slot)); m.vertexCount=count; m.loadComplete=1; m.edgeScale=1;
            m.rawVertices=static_cast<mdl::PmdVertex*>(::operator new(sizeof(mdl::PmdVertex)*count));
            std::memset(m.rawVertices,0,sizeof(mdl::PmdVertex)*count);
            m.boneCount=2; m.boneTable=static_cast<mdl::BoneRecord*>(::operator new(sizeof(mdl::BoneRecord)*2));
            std::memset(m.boneTable,0,sizeof(mdl::BoneRecord)*2);
            m.materialCount=1; m.materials=::operator new(sizeof(mdl::ModelMaterialRecord)); std::memset(m.materials,0,sizeof(mdl::ModelMaterialRecord));
            for(unsigned b=0;b<2;++b) std::memcpy(m.boneTable[b].matInit,world.data(),sizeof(world));
            for(unsigned i=0;i<count;++i) {
                auto& v=m.rawVertices[i]; v.position[0]=-.75f+.5f*i; v.position[2]=.5f;
                v.normal[1]=1; v.bone[1]=1; v.weightPercent=70; v.edgeDisabled=BYTE(i&1);
            }
            IDirect3DVertexBuffer9 *body=nullptr,*edge=nullptr;
            Check(SUCCEEDED(device->CreateVertexBuffer(count*sizeof(mdl::SkinnedVertexBase),0,
                D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1,D3DPOOL_MANAGED,&body,nullptr)) &&
                SUCCEEDED(device->CreateVertexBuffer(count*sizeof(mdl::EdgeVertex),D3DUSAGE_WRITEONLY,
                D3DFVF_XYZ|D3DFVF_DIFFUSE,D3DPOOL_MANAGED,&edge,nullptr)), "outline color creates readable body and WRITEONLY edge resources");
            m.vertexBuffer=body; m.vertexBuffer2=edge;
            void* bytes=nullptr; Check(SUCCEEDED(edge->Lock(0,0,&bytes,0))&&bytes, "initialize exact per-vertex original colors");
            auto* output=static_cast<mdl::EdgeVertex*>(bytes);
            for(unsigned i=0;i<count;++i) { std::memcpy(output[i].position,m.rawVertices[i].position,12); output[i].diffuse=originalColors[i]; }
            Check(SUCCEEDED(edge->Unlock()), "finish original edge upload");
            Check(UpdateModelVertexBuffersCpu(app.get(),app->ModelSlot(slot),world.data()), "produce real CPU skinning main and edge buffers");
            if(slot&&secondPmx) m.physicsMode=2; // menu 286 must never touch this resource or material
        }
    }
    ~Fixture() { if(device)device->SetStreamSource(0,nullptr,0,0);skinning::SetPreference(std::move(preference)); }
    std::vector<mdl::SkinnedVertexBase> Body(unsigned slot=0) {
        auto* buffer=static_cast<IDirect3DVertexBuffer9*>(mdl::Mdl(app->ModelSlot(slot))->vertexBuffer);
        void* bytes=nullptr; Check(SUCCEEDED(buffer->Lock(0,0,&bytes,D3DLOCK_READONLY))&&bytes, "inspect legally readable production body");
        std::vector<mdl::SkinnedVertexBase> out(count); std::memcpy(out.data(),bytes,out.size()*sizeof(out[0]));
        Check(SUCCEEDED(buffer->Unlock()), "unlock production body inspection"); return out;
    }
    std::vector<mdl::EdgeVertex> Expected(unsigned slot=0) {
        auto out=std::vector<mdl::EdgeVertex>(count); const auto body=Body(slot); const auto* m=mdl::Mdl(app->ModelSlot(slot));
        const float distance=ModelOutlineDistance(app.get(),app->ModelSlot(slot),world.data());
        for(unsigned i=0;i<count;++i) for(unsigned c=0;c<3;++c)
            out[i].position[c]=body[i].position[c]+body[i].normal[c]*(m->rawVertices[i].edgeDisabled?-.004999999888241291f:distance);
        return out;
    }
    void Visible(const std::array<DWORD,count>& colors,unsigned slot=0) {
        const auto expected=Expected(slot);
        const auto matrix=*reinterpret_cast<const D3DMATRIX*>(world.data());
        for(auto transform:{D3DTS_WORLD,D3DTS_VIEW,D3DTS_PROJECTION}) Check(SUCCEEDED(device->SetTransform(transform,&matrix)), "set isolated outline pixel transform");
        device->SetVertexShader(nullptr); device->SetPixelShader(nullptr); device->SetTexture(0,nullptr);
        device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
        device->SetRenderState(D3DRS_POINTSIZE,0x40400000); // three pixels
        device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
        device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
        Check(SUCCEEDED(device->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE)) && SUCCEEDED(device->SetStreamSource(0,
            static_cast<IDirect3DVertexBuffer9*>(mdl::Mdl(app->ModelSlot(slot))->vertexBuffer2),0,sizeof(mdl::EdgeVertex))), "bind actual active outline resource");
        Check(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0)) && SUCCEEDED(device->BeginScene()) &&
            SUCCEEDED(device->DrawPrimitive(D3DPT_POINTLIST,0,count)) && SUCCEEDED(device->EndScene()), "draw actual PMD/PMX outline colors without reading WRITEONLY memory");
        ComPtr<IDirect3DSurface9> target;Check(SUCCEEDED(device->GetRenderTarget(0,&target))&&SUCCEEDED(device->GetRenderTargetData(target.Get(),pixels.Get())), "read rendered outline pixels");
        D3DLOCKED_RECT lock{};Check(SUCCEEDED(pixels->LockRect(&lock,nullptr,D3DLOCK_READONLY)), "lock rendered outline pixels");
        for(unsigned i=0;i<count;++i) {
            const int x=int((expected[i].position[0]+1)*side/2),y=int((1-expected[i].position[1])*side/2);bool found=false;
            for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)if(x+dx>=0&&x+dx<int(side)&&y+dy>=0&&y+dy<int(side)){
                const auto value=*reinterpret_cast<const DWORD*>(static_cast<const unsigned char*>(lock.pBits)+(y+dy)*lock.Pitch+(x+dx)*4);
                found|=(value&0xffffff)==(colors[i]&0xffffff);
            }
            Check(found,"actual outline resource renders each expected color at its current skinned position");
        }
        pixels->UnlockRect();
    }
    void Pose(float x) {
        auto* m=mdl::Mdl(app->ModelSlot(0));m->boneTable[0].matInit[12]=m->boneTable[1].matInit[12]=x;
        Check(UpdateModelVertexBuffersCpu(app.get(),app->ModelSlot(0),world.data()), "unregistered pose changes through real CPU skinning");
    }
};
struct Picker {
    static thread_local Picker* current;
    Fixture& fixture;
    bool accept=true,stale=false,close=false;
    COLORREF color=RGB(91,123,157);
    unsigned calls=0;
    bool initialized=false;
    static BOOL WINAPI Choose(LPCHOOSECOLORA cc) {
        auto& self=*current;++self.calls;
        Check(cc&&cc->lpCustColors&&cc->lpCustColors!=reinterpret_cast<COLORREF*>(self.fixture.app->state.customColorTable), "real286 uses a complete private sixteen-color picker table");
        std::array<COLORREF,16> before{};
        std::memcpy(before.data(),self.fixture.app->state.customColorTable,sizeof(self.fixture.app->state.customColorTable));
        std::memcpy(reinterpret_cast<unsigned char*>(before.data())+sizeof(self.fixture.app->state.customColorTable),&self.fixture.app->state.pad209,sizeof(self.fixture.app->state.pad209));
        for(unsigned i=0;i<16;++i)Check(before[i]==cc->lpCustColors[i],"actual286 initializes both twelve-item and four-item palette storage");
        for(unsigned i=0;i<16;++i)cc->lpCustColors[i]=RGB(i+1,i+2,i+3);
        if(self.stale)self.fixture.app->state.modelOutlineColorRed=17;
        if(self.close)SendMessageW(self.fixture.app->MainWindow(),WM_CLOSE,0,0);
        cc->rgbResult=self.color;return self.accept;
    }
    explicit Picker(Fixture& f):fixture(f) {
        Check(!current,"no nested outline test picker");current=this;
        auto status=MH_Initialize(); initialized=status==MH_OK;Check(initialized||status==MH_ERROR_ALREADY_INITIALIZED,"initialize real outline picker interception");
        Check(MH_CreateHook(reinterpret_cast<void*>(&ChooseColorA),reinterpret_cast<void*>(&Choose),nullptr)==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&ChooseColorA))==MH_OK,"intercept production color picker boundary");
    }
    ~Picker(){MH_DisableHook(reinterpret_cast<void*>(&ChooseColorA));MH_RemoveHook(reinterpret_cast<void*>(&ChooseColorA));if(initialized)MH_Uninitialize();current=nullptr;}
    void Run(){CmdViewMenu(fixture.app.get(),fixture.app->MainWindow(),286,0);}
};
thread_local Picker* Picker::current=nullptr;
std::shared_ptr<HistoryChange> Plan(Fixture& f,int red=91,int green=123,int blue=157) {
    NativeModelOutlineColorPlan plan;std::shared_ptr<HistoryChange> out;
    Check(plan.Begin(*f.app)==MMS_OK&&plan.Set(red,green,blue)==MMS_OK&&plan.Take(out)==MMS_OK&&out,"stage private outline color and retained resources");return out;
}
void MenuAndReplay() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"outline color history mode");
        Picker picker(f);auto* model=mdl::Mdl(f.app->ModelSlot(0));auto* pmx=mdl::Mdl(f.app->ModelSlot(1));
        const auto id=SceneObjectIdentity(model);const auto before=model->vertexBuffer2,pmxBuffer=pmx->vertexBuffer2;
        const auto pose=f.Body();f.Visible(originalColors);f.Visible(originalColors,1);
        const auto frame=f.app->CurrentFrame();const auto selected=f.app->SelectedModelSlot();const auto physics=f.app->PhysicsResetPending();
        picker.Run();const auto after=model->vertexBuffer2;
        Check(picker.calls==1&&after!=before&&pmx->vertexBuffer2==pmxBuffer&&f.history.UndoCount()==1&&f.app->SceneModified(), "actual286 commits one explicit batch in both recording modes");
        Check(f.app->state.modelOutlineColorRed==91&&f.app->state.modelOutlineColorGreen==123&&f.app->state.modelOutlineColorBlue==157&&
            !std::memcmp(pose.data(),f.Body().data(),pose.size()*sizeof(pose[0])), "color publication changes RGB and actual edge resource while preserving completed main geometry");
        f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});f.Visible(originalColors,1);
        f.Pose(.15f);const auto currentPose=f.Body();
        Check(f.history.History(false)==MMS_OK&&model->vertexBuffer2==before&&f.history.RedoCount()==1, "undo switches back to the exact retained original resource");
        f.Visible(originalColors);Check(model->boneTable[0].matInit[12]==.15f&&!std::memcmp(currentPose.data(),f.Body().data(),currentPose.size()*sizeof(currentPose[0])), "undo reconstructs original-resource geometry from the current unregistered pose");
        Check(UpdateModelVertexBuffersCpu(f.app.get(),f.app->ModelSlot(0),f.world.data()),"natural CPU skinning updates the restored original outline resource");f.Visible(originalColors);
        Check(f.history.History(true)==MMS_OK&&model->vertexBuffer2==after&&SceneObjectIdentity(model).id.lo==id.id.lo&&SceneObjectIdentity(model).id.hi==id.id.hi, "redo reuses the retained colored resource without reopening a model file");
        f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});f.Visible(originalColors,1);
        Check(UpdateModelVertexBuffersCpu(f.app.get(),f.app->ModelSlot(0),f.world.data()),"natural CPU skinning updates the redone colored outline resource");f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});
        Check(f.app->CurrentFrame()==frame&&f.app->SelectedModelSlot()==selected&&f.app->PhysicsResetPending()==physics, "color traversal does not seek, select, reset physics, or replace object identity");
        Check(f.history.History(false)==MMS_OK,"create outline redo before cancellation");
        const auto redo=f.history.RedoCount();picker.accept=false;picker.Run();
        Check(model->vertexBuffer2==before&&f.history.RedoCount()==redo&&!f.history.UndoCount(), "cancelled actual286 picker preserves current resources and redo");
        picker.accept=true;picker.color=RGB(0,0,0);picker.Run();
        Check(model->vertexBuffer2==before&&f.history.RedoCount()==redo&&!f.history.UndoCount(), "same RGB confirmation is a no-op even with nonuniform original vertex colors");
    }
}
void FailureAndSource() {
    Fixture f(false);NativePoseTestBinding binding(*f.app,f.history);auto* m=mdl::Mdl(f.app->ModelSlot(0));
    const auto before=m->vertexBuffer2;auto change=Plan(f);const auto charge=change->RetainedBytes();
    Check(charge>count*sizeof(mdl::EdgeVertex)*4&&f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,charge-1)==MMS_OK,"budget includes both actual managed outline resources and retained owners");
    std::unique_ptr<SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("outline color budget",change,tx)==MMS_E_MEMORY&&!tx&&m->vertexBuffer2==before&&!f.history.UndoCount(), "budget refusal leaves all live model resources and RGB unchanged");f.Visible(originalColors);f.Visible(originalColors,1);
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,256ull*1024*1024)==MMS_OK,"restore outline color history budget");
    bool success=false;unsigned failures=0;
    for(long limit=0;limit<512&&!success;++limit) {
        std::shared_ptr<HistoryChange> candidate;NativeModelOutlineColorPlan plan;
        failAfter=limit;auto result=plan.Begin(*f.app);if(!result)result=plan.Set(19,29,39);if(!result)result=plan.Take(candidate);
        if(!result)result=Boundary([&]{return f.history.PrepareNative("outline color allocation",candidate,tx);});failAfter=-1;
        Check(m->vertexBuffer2==before&&!f.history.UndoCount()&&!f.app->SceneModified(),"every allocation failure preserves actual scene resources, RGB and journal");
        if(!result){success=true;tx.reset();}else{++failures;Check(result==MMS_E_MEMORY&&!tx,"outline resource preparation reports exact memory refusal");}
    }
    Check(success&&failures>8,"outline color private allocation sweep reaches a complete reservation");
    {auto candidate=Plan(f);std::unique_ptr<PreparedHistoryChange> prepared;Check(candidate->Prepare(true,prepared)==MMS_OK&&prepared->IsCurrent(),"prepare actual outline resources before a foreign pose write");
        f.Pose(.1f);Check(!prepared->IsCurrent()&&m->vertexBuffer2==before,"last source check rejects changed CPU/GPU body bytes without publishing any partial model");}
    {NativeModelOutlineColorPlan plan;Check(plan.Begin(*f.app)==MMS_OK,"capture color dialog before registration replacement");{NativePoseTestBinding temporary(*f.app,f.history);}
        Check(plan.Set(11,22,33)==MMS_E_STALE,"nested binding replacement invalidates old modal capture even at the same pointers");}
    {Picker picker(f);picker.stale=true;picker.Run();Check(picker.calls==1&&m->vertexBuffer2==before&&!f.history.UndoCount()&&f.app->state.modelOutlineColorRed==17,"modal source change cannot be silently recaptured as the before color");f.app->state.modelOutlineColorRed=0;}
    {NativeModelOutlineColorPlan plan;Check(plan.Begin(*f.app)==MMS_OK&&plan.Set(-1,2,3)==MMS_E_ARGUMENT&&plan.Set(1,256,3)==MMS_E_ARGUMENT,"out-of-range outline channels never enter a transaction");}
    for(DWORD usage:{DWORD(D3DUSAGE_WRITEONLY),DWORD(0)}) {
        IDirect3DVertexBuffer9* replacement=nullptr;
        Check(SUCCEEDED(f.device->CreateVertexBuffer(count*sizeof(mdl::SkinnedVertexBase),usage,usage?D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1:0,D3DPOOL_MANAGED,&replacement,nullptr)),"create deliberately unsupported body descriptor");
        const auto original=m->vertexBuffer;m->vertexBuffer=replacement;
        NativeModelOutlineColorPlan plan;Check(plan.Begin(*f.app)==MMS_E_NOT_READY&&m->vertexBuffer2==before,"WRITEONLY body and unknown stride/FVF refuse without any unsafe read or live change");
        m->vertexBuffer=original;replacement->Release();
    }
    {auto* other=mdl::Mdl(f.app->ModelSlot(1));const auto original=other->vertexBuffer2;other->vertexBuffer2=before;
        NativeModelOutlineColorPlan plan;Check(plan.Begin(*f.app)==MMS_E_STALE,"a shared active outline resource has no unambiguous private color target");other->vertexBuffer2=original;}
    {Picker picker(f);picker.close=true;picker.Run();Check(IsWindow(f.app->MainWindow())&&m->vertexBuffer2==before&&!f.history.UndoCount(),"reentrant owner close cancels before publication and is deferred until candidate cleanup");
        MSG message{};while(PeekMessageW(&message,f.app->MainWindow(),WM_CLOSE,WM_CLOSE,PM_REMOVE)){} }
}
void PaletteAndLegacy() {
    for(bool bound:{false,true})for(bool accept:{false,true}) {
        Fixture f;std::unique_ptr<NativePoseTestBinding> binding;if(bound)binding=std::make_unique<NativePoseTestBinding>(*f.app,f.history);
        std::array<COLORREF,16> before{};for(unsigned i=0;i<16;++i)before[i]=RGB(20+i,40+i,60+i);
        std::memcpy(f.app->state.customColorTable,before.data(),sizeof(f.app->state.customColorTable));
        std::memcpy(&f.app->state.pad209,reinterpret_cast<const unsigned char*>(before.data())+sizeof(f.app->state.customColorTable),sizeof(f.app->state.pad209));
        f.app->WireframeRenderingEnabled()=37;Picker picker(f);picker.accept=accept;picker.Run();
        std::array<COLORREF,16> actual{};std::memcpy(actual.data(),f.app->state.customColorTable,sizeof(f.app->state.customColorTable));
        std::memcpy(reinterpret_cast<unsigned char*>(actual.data())+sizeof(f.app->state.customColorTable),&f.app->state.pad209,sizeof(f.app->state.pad209));
        for(unsigned i=0;i<16;++i)Check(actual[i]==(accept?RGB(i+1,i+2,i+3):before[i]),"all sixteen legacy custom-color slots are preserved on OK and untouched on Cancel");
        Check(f.app->WireframeRenderingEnabled()==37,"picker table access never writes the following live wireframe field");
        if(!bound)Check(!f.history.UndoCount(),"unbound normal profile keeps its existing immediate color behavior");
    }
}
struct DriverFault {
    using LockFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*,UINT,UINT,void**,DWORD);
    using UnlockFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*);
    using CreateFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,UINT,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer9**,HANDLE*);
    static thread_local DriverFault* current;
    static LockFn lockOriginal;static UnlockFn unlockOriginal;static CreateFn createOriginal;
    Fixture& f;std::array<void*,3> targets{};std::array<IDirect3DVertexBuffer9*,4> created{};
    IDirect3DVertexBuffer9* failBody=nullptr;unsigned failCreation=0,creates=0;bool failUnlock=false,initialized=false;
    unsigned readonlyOutline=0,originalWrites=0,inactiveWrites=0;
    static HRESULT STDMETHODCALLTYPE Lock(IDirect3DVertexBuffer9* self,UINT offset,UINT bytes,void** out,DWORD flags) {
        auto& fault=*current;
        bool active=false;
        for(unsigned slot=0;slot<2;++slot)if(self==mdl::Mdl(fault.f.app->ModelSlot(slot))->vertexBuffer2){
            active=true;
            if(flags&D3DLOCK_READONLY)++fault.readonlyOutline;else ++fault.originalWrites;
        }
        if(!active&&!(flags&D3DLOCK_READONLY))++fault.inactiveWrites;
        if(self==fault.failBody&&(flags&D3DLOCK_READONLY))return E_FAIL;
        return lockOriginal(self,offset,bytes,out,flags);
    }
    static HRESULT STDMETHODCALLTYPE Unlock(IDirect3DVertexBuffer9* self) {
        const auto result=unlockOriginal(self);
        if(current->failUnlock&&std::find(current->created.begin(),current->created.end(),self)!=current->created.end())return E_FAIL;
        return result;
    }
    static HRESULT STDMETHODCALLTYPE Create(IDirect3DDevice9* self,UINT size,DWORD usage,DWORD fvf,D3DPOOL pool,IDirect3DVertexBuffer9** out,HANDLE* shared) {
        auto& fault=*current;++fault.creates;
        if(fault.failCreation==fault.creates){*out=nullptr;return D3DERR_OUTOFVIDEOMEMORY;}
        const auto result=createOriginal(self,size,usage,fvf,pool,out,shared);
        if(SUCCEEDED(result)&&fault.creates<=fault.created.size())fault.created[fault.creates-1]=*out;
        return result;
    }
    explicit DriverFault(Fixture& value):f(value) {
        Check(!current,"no nested outline driver fault scope");current=this;
        const auto init=MH_Initialize();initialized=init==MH_OK;Check(initialized||init==MH_ERROR_ALREADY_INITIALIZED,"initialize outline driver boundary interception");
        auto** vb=*reinterpret_cast<void***>(mdl::Mdl(f.app->ModelSlot(0))->vertexBuffer);
        auto** dev=*reinterpret_cast<void***>(f.device.Get());targets={vb[11],vb[12],dev[26]};
        const std::array<void*,3> replacements{reinterpret_cast<void*>(&Lock),reinterpret_cast<void*>(&Unlock),reinterpret_cast<void*>(&Create)};
        const std::array<void**,3> originals{reinterpret_cast<void**>(&lockOriginal),reinterpret_cast<void**>(&unlockOriginal),reinterpret_cast<void**>(&createOriginal)};
        for(unsigned i=0;i<targets.size();++i)Check(MH_CreateHook(targets[i],replacements[i],originals[i])==MH_OK&&MH_EnableHook(targets[i])==MH_OK,"intercept exact real D3D9 resource boundary");
    }
    ~DriverFault(){for(auto target:targets){MH_DisableHook(target);MH_RemoveHook(target);}if(initialized)MH_Uninitialize();current=nullptr;}
};
thread_local DriverFault* DriverFault::current=nullptr;
DriverFault::LockFn DriverFault::lockOriginal=nullptr;
DriverFault::UnlockFn DriverFault::unlockOriginal=nullptr;
DriverFault::CreateFn DriverFault::createOriginal=nullptr;
void DriverAndPublication() {
    Fixture f(false);NativePoseTestBinding binding(*f.app,f.history);
    const auto before0=mdl::Mdl(f.app->ModelSlot(0))->vertexBuffer2,before1=mdl::Mdl(f.app->ModelSlot(1))->vertexBuffer2;
    for(unsigned failure:{0u,1u,2u}) {
        auto change=Plan(f);std::unique_ptr<SharedHistory::NativeTransaction> tx;MmsResult result=MMS_OK;
        {DriverFault fault(f);
            if(failure==0)fault.failBody=static_cast<IDirect3DVertexBuffer9*>(mdl::Mdl(f.app->ModelSlot(1))->vertexBuffer);
            if(failure==1)fault.failCreation=2;
            if(failure==2)fault.failUnlock=true;
            result=f.history.PrepareNative("outline driver refusal",change,tx);
            Check(!fault.readonlyOutline&&!fault.originalWrites,"forward preparation never locks or reads any live WRITEONLY outline buffer");}
        Check(result==(failure==1?MMS_E_MEMORY:MMS_E_FAILED)&&!tx&&mdl::Mdl(f.app->ModelSlot(0))->vertexBuffer2==before0&&
            mdl::Mdl(f.app->ModelSlot(1))->vertexBuffer2==before1&&!f.history.UndoCount()&&!f.app->state.modelOutlineColorRed,
            "real D3D lock, second-resource allocation and upload refusal do not partially publish a multi-model color change");
        f.Visible(originalColors);f.Visible(originalColors,1);
    }
    auto change=Plan(f);std::unique_ptr<SharedHistory::NativeTransaction> tx;
    Check(f.history.PrepareNative("outline no-fail publication",change,tx)==MMS_OK,"reserve complete outline color batch");
    failAfter=0;const auto result=f.history.CommitNative(std::move(tx));failAfter=-1;
    Check(result==MMS_OK&&f.history.UndoCount()==1,"outline resource and RGB publication performs no C++ allocation");
    f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d},1);
}
void PreparationEpoch() {
    for(bool failSecond:{false,true}) {
        Fixture f(false);NativePoseTestBinding binding(*f.app,f.history);
        auto* first=mdl::Mdl(f.app->ModelSlot(0));auto* second=mdl::Mdl(f.app->ModelSlot(1));
        const auto before0=first->vertexBuffer2,before1=second->vertexBuffer2;
        const auto scale=first->edgeScale;auto change=Plan(f);
        std::unique_ptr<SharedHistory::NativeTransaction> older,newer;
        Check(f.history.PrepareNative("outline older candidate",change,older)==MMS_OK&&older,
            "prepare the first real publication for a retained outline change");
        first->edgeScale=scale*2;
        if(failSecond) {
            DriverFault fault(f);fault.failBody=static_cast<IDirect3DVertexBuffer9*>(second->vertexBuffer);
            Check(f.history.PrepareNative("outline failed replacement candidate",change,newer)==MMS_E_FAILED&&!newer&&
                fault.inactiveWrites==1&&!fault.originalWrites&&!fault.readonlyOutline,
                "a later real preparation fails only after rewriting the first shared inactive target");
        } else {
            Check(f.history.PrepareNative("outline replacement candidate",change,newer)==MMS_OK&&newer,
                "prepare the same retained outline change again with different expansion geometry");
        }
        first->edgeScale=scale;
        Check(f.history.CommitNative(std::move(older))==MMS_E_STALE&&first->vertexBuffer2==before0&&
            second->vertexBuffer2==before1&&!f.history.UndoCount()&&!f.history.RedoCount()&&
            !f.app->state.modelOutlineColorRed&&!f.app->SceneModified(),
            "restoring every source parameter cannot revive a candidate whose shared target was prepared again");
        f.Visible(originalColors);f.Visible(originalColors,1);
        if(failSecond) {
            Check(f.history.PrepareNative("outline retry after replacement failure",change,newer)==MMS_OK&&newer,
                "failed replacement preparation permits a fresh complete candidate");
        } else first->edgeScale=scale*2;
        Check(f.history.CommitNative(std::move(newer))==MMS_OK&&f.history.UndoCount()==1&&
            first->vertexBuffer2!=before0&&second->vertexBuffer2!=before1,
            "only the newest complete outline candidate can publish one history node");
        f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});
        f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d},1);
        Check(f.history.History(false)==MMS_OK&&first->vertexBuffer2==before0&&second->vertexBuffer2==before1,
            "preparation epochs preserve the separate publication serial and normal undo");
        f.Visible(originalColors);f.Visible(originalColors,1);
    }
}
void DeletionAndGpu() {
    {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);Picker picker(f);picker.Run();
        auto* original=f.app->ModelSlot(0);const auto id=SceneObjectIdentity(original).id;
        {ObjectModalProbe confirmation(f,true);f.Command(true);Check(confirmation.seen==1&&!f.app->ModelSlot(0),"delete a colored model through real437 confirmation");}
        Check(f.history.History(false)==MMS_OK&&f.app->ModelSlot(0)==original&&SceneObjectPresent(original,id),"undo deletion restores the same retained color target and resources");
        Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,"color traversal accepts the restored stable ID with a new generation");
    }
    {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);Picker picker(f);picker.Run();
        const auto& adapters=skinning::AvailableAdapters();
        if(adapters.empty()){std::puts("SKIP outline GPU skinning: no supported compute adapter; real CPU/D3D9 checks remain mandatory");return;}
        skinning::Preference preference;preference.mode=skinning::Mode::Adapter;preference.adapterId=adapters.front().id;
        skinning::SetPreference(preference);auto* m=mdl::Mdl(f.app->ModelSlot(0));
        m->boneTable[0].matInit[12]=m->boneTable[1].matInit[12]=.1f;
        if(!skinning::UpdateModel(f.app.get(),f.app->ModelSlot(0),f.world.data(),UpdateModelVertexBuffersCpu)){
            std::puts("SKIP outline GPU skinning: compute execution unavailable; no GPU pass claimed");return;
        }
        f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});
        Check(f.history.History(false)==MMS_OK&&m->boneTable[0].matInit[12]==.1f,"undo preserves current actual GPU-skinned pose and original diffuse colors");f.Visible(originalColors);
        Check(skinning::UpdateModel(f.app.get(),f.app->ModelSlot(0),f.world.data(),UpdateModelVertexBuffersCpu),"real GPU path updates the restored original outline resource");f.Visible(originalColors);
        Check(f.history.History(true)==MMS_OK,"redo after actual GPU update reuses its retained colored resource");
        Check(skinning::UpdateModel(f.app.get(),f.app->ModelSlot(0),f.world.data(),UpdateModelVertexBuffersCpu),"real GPU path updates the redone outline resource");f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});
        std::puts("PASS actual GPU skinning preserves PMD color across retained-buffer undo and redo");
    }
}
void MixedPluginAndLastSlot() {
    {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);auto change=Plan(f);
        camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.model-outline-color";f.storage.Api(storage);
        auto serializer=plugin.Serializer();MmsHandle provider=0;
        Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register real color/plugin history serializer");
        EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);
        MmsHandle transaction=0;std::uint64_t revision=0;
        Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("outline color and plugin"),revision,&transaction)==MMS_OK,"begin one real mixed native/plugin transaction");
        NativeModelOutlineColorPlan busy;Check(busy.Begin(*f.app)==MMS_E_BUSY,"pending plugin edit excludes a competing native color dialog");
        auto participant=plugin.Participant(Text(client.id));
        Check(api.join(api.context,transaction,&participant)==MMS_OK&&edits.JoinNative(client,transaction,change)==MMS_OK&&
            api.prepare(api.context,transaction)==MMS_OK&&api.commit(api.context,transaction)==MMS_OK&&plugin.value==42&&f.history.UndoCount()==1,
            "one shared node publishes actual outline resources and plugin-owned data");
        f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});
        Check(f.history.History(false)==MMS_OK&&plugin.value==0,"common undo restores both real plugin and original vertex color resources");f.Visible(originalColors);
        Check(f.history.History(true)==MMS_OK&&plugin.value==42,"common redo restores both real plugin and colored vertex resources");f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d});
        Check(f.history.Reset()==MMS_OK,"retire mixed color records before unregistering serializer");f.history.Collect();
        Check(f.storage.Unregister(storage,provider)==MMS_OK,"release mixed color serializer after history cleanup");
    }
    {
        Fixture f(false);auto* last=f.app->ModelSlot(1);f.app->ModelSlot(254)=last;f.app->ModelSlot(1)=nullptr;
        NativePoseTestBinding binding(*f.app,f.history);Picker picker(f);picker.Run();
        Check(f.history.UndoCount()==1&&f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,"all 255 model slots participate in the same explicit color batch");
        f.Visible({0xff5b7b9d,0xff5b7b9d,0xff5b7b9d,0xff5b7b9d},254);
        Check(f.history.Reset()==MMS_OK,"release last-slot color owners before fixture restores slots");f.history.Collect();
        f.app->ModelSlot(1)=last;f.app->ModelSlot(254)=nullptr;
    }
}
}
void NativeModelOutlineColorChecks() {
    model_outline_color_checks::MenuAndReplay();
    model_outline_color_checks::FailureAndSource();
    model_outline_color_checks::PaletteAndLegacy();
    model_outline_color_checks::DriverAndPublication();
    model_outline_color_checks::PreparationEpoch();
    model_outline_color_checks::DeletionAndGpu();
    model_outline_color_checks::MixedPluginAndLastSlot();
}
