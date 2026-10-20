// Include after ImportTextureFixture, ObjectCommandFixture and the names tests.
#include "../src/plugins/native_model_toon.hpp"
#include "../src/render/model_toon.hpp"
#include "mikumikustudio/shared_toon.hpp"
#include <MinHook.h>
#include <type_traits>
#include <wrl/client.h>
namespace model_toon_checks {
using namespace plugins;
static_assert(!std::is_copy_constructible_v<NativeModelToonPlan>);
static_assert(!std::is_copy_assignable_v<NativeModelToonPlan>);
using Raw = std::array<std::array<char,100>,10>;
thread_local bool requireFiles = false;
HRESULT FileLoader(IDirect3DDevice9* device,const wchar_t* path,IDirect3DTexture9** out) {
    if(requireFiles&&GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES) {
        ++importTextureTestState->calls;*out=nullptr;return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    return ImportTestTextureLoader(device,path,out);
}
struct Fixture : ObjectCommandFixture {
    Microsoft::WRL::ComPtr<IDirect3D9> uiD3d;
    Microsoft::WRL::ComPtr<IDirect3DDevice9> uiDevice;
    ImportTextureFixture textures;
    explicit Fixture(bool nativeUi=false) {
        if(nativeUi) {
            // Real object/menu publication refreshes the light through D3D.
            // The texture-only fixture's device==1 is an opaque identity, not
            // a device on which production UI may call SetLight/projection.
            uiD3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));Check(bool(uiD3d),"create D3D9 runtime for real Toon UI publication");
            D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
            pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
            Check(SUCCEEDED(uiD3d->CreateDevice(0,D3DDEVTYPE_HAL,app->MainWindow(),
                D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&uiDevice)),
                "real Toon command fixture owns a callable D3D9 device for its entire history lifetime");
            textures.renderer->device=uiDevice.Get();
        }
        app->HInstance()=GetModuleHandleW(nullptr);app->Renderer()=textures.renderer.get();
        for(unsigned i=0;i<11;++i)app->ToonTexture(i)=new ImportTestTexture(textures.state);
        for(unsigned i=0;i<30;++i)app->state.toonEdgeTable[i]=.01f*float(i+1);
        for(unsigned slot=0;slot<2;++slot) {
            auto& m=*mdl::Mdl(app->ModelSlot(slot));std::wcscpy(m.modelDirectory,L"D:\\toon-plan\\");
            for(unsigned i=0;i<10;++i)std::sprintf(m.pmdToonFileNames[i],"toon%02u.bmp",i+1);
            m.materialCount=1;m.materials=::operator new(sizeof(mdl::ModelMaterialRecord));
            std::memset(m.materials,0,sizeof(mdl::ModelMaterialRecord));mdl::Materials(app->ModelSlot(slot))[0].toonReference=0;
        }
    }
    ~Fixture() {
        Check(history.Reset()==MMS_OK,"release all Toon histories before renderer/COM owners");history.Collect();
        app->Renderer()=nullptr;
        for(unsigned i=0;i<11;++i)if(auto* p=app->ToonTexture(i)){app->ToonTexture(i)=nullptr;p->Release();}
    }
    Raw Bytes(unsigned slot=0) const {
        Raw out;std::memcpy(out.data(),mdl::Mdl(app->ModelSlot(slot))->pmdToonFileNames,sizeof(out));return out;
    }
    ModelToonDraft Read(unsigned slot=0) {
        NativeModelToonPlan plan;ModelToonDraft value;
        Check(plan.Begin(*app,slot,FileLoader)==MMS_OK&&plan.Read(value)==MMS_OK,"read exact native Toon table through a fixed identity");return value;
    }
    std::shared_ptr<HistoryChange> Change(const ModelToonDraft& value,unsigned slot=0) {
        NativeModelToonPlan plan;std::shared_ptr<HistoryChange> out;
        Check(plan.Begin(*app,slot,FileLoader)==MMS_OK&&plan.Set(value)==MMS_OK&&plan.Take(out)==MMS_OK,
            "prepare before and after Toon names and textures privately");return out;
    }
    IDirect3DTexture9* Resolved(unsigned slot=0) {
        const auto* object=app->ModelSlot(slot);
        return ResolveModelToonTexture(app.get(),textures.renderer.get(),*mdl::Mdl(object),mdl::Materials(app->ModelSlot(slot))[0]);
    }
    void Append(const wchar_t* path,IUnknown* texture) {
        for(auto& entry:textures.renderer->resourcePool)if(!entry.heapBuffer) {
            const auto bytes=(std::wcslen(path)+1)*sizeof(wchar_t);
            entry.heapBuffer=std::malloc(bytes);Check(entry.heapBuffer!=nullptr,"allocate distinct existing Toon cache name");
            std::memcpy(entry.heapBuffer,path,bytes);entry.comObject=texture;InvalidateTextureLookup();return;
        }
        Check(false,"fixture texture cache has room");
    }
};
void Commit(Fixture& f,const ModelToonDraft& value,unsigned slot=0) {
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("Toon filename edit",f.Change(value,slot),token,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,
        "admit complete Toon resources before the first cache or filename write");
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(result==MMS_OK,"Toon cache and 1000-byte publication allocate nothing");
}
void RoundTrip() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})
    for(unsigned physics:{0u,2u}) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);
        auto& m=*mdl::Mdl(f.app->ModelSlot(0));m.physicsMode=static_cast<unsigned char>(physics);
        Check(f.history.SetMode(mode)==MMS_OK,"Toon history accepts both recording modes");
        std::strcpy(m.pmdToonFileNames[0],"before.bmp");m.pmdToonFileNames[0][40]=char(0xa5);
        auto* beforeTexture=new ImportTestTexture(f.textures.state);f.Append(L"D:\\toon-plan\\before.bmp",beforeTexture);
        Check(f.Resolved()==beforeTexture,"capture the actual before renderer resource before opening a Toon plan");
        m.pmdToonFileNames[3][80]=char(0xf1);const auto raw=f.Bytes();const auto before=f.Read();
        auto after=before;after[0]=L"after.bmp";after[3]=L"toon02.bmp";after[4]=L"";
        auto* object=f.app->ModelSlot(0);const auto id=SceneObjectIdentity(object).id;
        NativeModelToonPlan plan;
        const auto loads=f.textures.state.calls;
        Check(plan.Begin(*f.app,0,FileLoader)==MMS_OK&&plan.Set(after)==MMS_OK&&f.textures.state.calls==loads&&
            f.textures.renderer->resourcePool[0].comObject==beforeTexture&&!f.textures.renderer->resourcePool[1].heapBuffer&&
            f.Bytes()==raw,"Begin retains the actual before resource and Set performs no I/O/shared writes");
        std::shared_ptr<HistoryChange> change;Check(plan.Take(change)==MMS_OK&&change&&f.textures.state.calls==loads+1&&
            f.Resolved()==beforeTexture&&!f.textures.renderer->resourcePool[1].heapBuffer,"Take prepares after in the same unpublished cache batch");
        std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("Toon full roundtrip",change,token)==MMS_OK,"complete Toon candidate admitted");
        failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
        Check(result==MMS_OK&&f.history.UndoCount()==1&&f.Read()==after&&f.Unmoved()&&f.app->EnhancedModelDirty(),
            "one explicit Toon node changes real PMD/PMX cache selection in either mode");
        auto* afterTexture=f.Resolved();Check(afterTexture&&afterTexture!=f.app->ToonTexture(1),"native renderer sees the prepared after custom texture immediately");
        m.lightDir[2]=.375f;
        const auto completedLoads=f.textures.state.calls;
        Check(f.history.History(false)==MMS_OK&&f.Bytes()==raw&&f.Resolved()==beforeTexture&&f.Resolved()!=afterTexture&&
            f.textures.state.calls==completedLoads&&m.lightDir[2]==.375f,"undo restores all raw filename bytes and the before texture without reload or pose overwrite");
        {auto noop=f.Change(before);Check(!noop&&f.history.RedoCount()==1,"same-value confirmation preserves raw padding and the redo node");}
        Check(f.history.History(true)==MMS_OK&&f.Read()==after&&f.Resolved()==afterTexture,
            "redo retains the exact after resource without model source access");
        Check(f.app->ModelSlot(0)==object&&SceneObjectPresent(object,id)&&m.lightDir[2]==.375f&&!f.history.LastPublicationRequiresPhysicsReset(),
            "Toon history preserves model identity, unregistered pose and physics state");
        auto shared=after;shared[0]=L"toon07.bmp";Commit(f,shared);
        Check(f.Resolved()==f.app->ToonTexture(7),"custom-to-default and cross-slot shared names use the actual shared texture");
        auto empty=shared;empty[0].clear();Commit(f,empty);Check(f.Resolved()==nullptr,"empty custom name displays no custom texture and never loads the directory");
    }
}
struct TempFiles {
    std::wstring directory;
    TempFiles() {
        wchar_t root[MAX_PATH]{},path[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH,root)&&GetTempFileNameW(root,L"mmt",0,path),"choose isolated Toon source files");
        Check(DeleteFileW(path)&&CreateDirectoryW(path,nullptr),"create isolated Toon source directory");directory=std::wstring(path)+L"\\";
        for(const auto* name:{L"before.bmp",L"after.bmp"}) {
            const auto full=directory+name;const auto file=CreateFileW(full.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            Check(file!=INVALID_HANDLE_VALUE,"create real source image-presence fixture");CloseHandle(file);
        }
    }
    void Offline() { for(const auto* name:{L"before.bmp",L"after.bmp"})Check(DeleteFileW((directory+name).c_str()),"remove original Toon source before undo/redo"); }
    ~TempFiles(){for(const auto* name:{L"before.bmp",L"after.bmp"})DeleteFileW((directory+name).c_str());auto root=directory;root.pop_back();RemoveDirectoryW(root.c_str());}
};
void OfflineAndOwnership() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);TempFiles files;
    auto& m=*mdl::Mdl(f.app->ModelSlot(0));Check(files.directory.size()<std::size(m.modelDirectory),"temporary Toon directory fits the native field");
    std::wcscpy(m.modelDirectory,files.directory.c_str());std::strcpy(m.pmdToonFileNames[0],"before.bmp");
    auto* beforeTexture=new ImportTestTexture(f.textures.state);f.Append((files.directory+L"before.bmp").c_str(),beforeTexture);
    Check(f.Resolved()==beforeTexture,"the offline test starts with a real currently resolved before resource");
    struct Require {bool previous=requireFiles;Require(){requireFiles=true;}~Require(){requireFiles=previous;}} require;
    const auto before=f.Read();auto after=before;after[0]=L"after.bmp";
    Commit(f,after);auto* afterTexture=f.Resolved();const auto calls=f.textures.state.calls;
    files.Offline();
    Check(f.history.History(false)==MMS_OK&&f.Resolved()==beforeTexture&&f.Resolved()!=afterTexture&&f.history.History(true)==MMS_OK&&
        f.Resolved()==afterTexture&&f.textures.state.calls==calls,"both file paths are absent while actual native texture undo/redo makes zero loader calls");
    auto* shared=static_cast<ImportTestTexture*>(f.app->ToonTexture(1));
    Check(shared->ReferenceCount()>=2,"history holds independent COM references to the original shared Toon slots");
    Check(f.history.Reset()==MMS_OK,"release Toon retained COM resources");f.history.Collect();
    Check(shared->ReferenceCount()==1,"dropping Toon history releases its shared texture ownership exactly once");
}
void BeforeCacheMiss() {
    for(bool ownPmx:{false,true}) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);TempFiles files;
        auto other=f.Read(1);other[0]=L"toon08.bmp";Commit(f,other,1);
        Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"keep an independent model redo before refusing an unresolved before texture");
        auto& m=*mdl::Mdl(f.app->ModelSlot(0));
        Check(files.directory.size()<std::size(m.modelDirectory),"cache-miss source directory fits the native field");
        std::wcscpy(m.modelDirectory,files.directory.c_str());
        if(ownPmx) {
            m.physicsMode=2;auto& material=mdl::Materials(f.app->ModelSlot(0))[0];material.toonReference=0xfe;
            const auto path=files.directory+L"before.bmp";
            Check(path.size()<std::size(material.toonPath),"PMX own path fits its native field");std::wcscpy(material.toonPath,path.c_str());
        } else std::strcpy(m.pmdToonFileNames[0],"before.bmp");
        Check(GetFileAttributesW((files.directory+L"before.bmp").c_str())!=INVALID_FILE_ATTRIBUTES&&f.Resolved()==nullptr,
            "an on-disk image can exist while the current native renderer has no matching before resource");
        const auto bytes=f.Bytes();const auto calls=f.textures.state.calls;
        const std::uint8_t sceneDirty=f.app->SceneModified(),modelDirty=f.app->EnhancedModelDirty();
        std::uint64_t revision=0,current=0;Check(f.history.Revision(revision)==MMS_OK,"capture revision before unresolved before resource");
        NativeModelToonPlan plan;
        Check(plan.Begin(*f.app,0,FileLoader)==MMS_E_NOT_READY&&f.textures.state.calls==calls&&f.Resolved()==nullptr&&
            f.Bytes()==bytes&&!f.textures.renderer->resourcePool[0].heapBuffer&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
            f.history.Revision(current)==MMS_OK&&current==revision&&f.app->SceneModified()==sceneDirty&&f.app->EnhancedModelDirty()==modelDirty,
            "missing table or PMX-own before cache resource refuses without loader calls, substitute pixels, model/cache writes or redo loss");
        Check(f.history.History(true)==MMS_OK&&f.Read(1)==other,"an unrelated existing redo remains usable after before resource refusal");
    }
}
void ValidationAndResources() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);auto before=f.Read();const auto raw=f.Bytes();
    for(unsigned kind=0;kind<5;++kind) {
        auto draft=before;NativeModelToonPlan plan;Check(plan.Begin(*f.app,0,FileLoader)==MMS_OK,"capture Toon boundary case");
        if(kind==0)draft[0]=std::wstring(100,L'x');
        if(kind==1)draft[0]=std::wstring(1,wchar_t(0xd800));
        if(kind==2)draft[0]=L"emoji-\U0001f600.bmp";
        if(kind==3)draft[0]=std::wstring(L"a\0b",3);
        if(kind==4)draft[0]=std::wstring(50,L'陰');
        std::shared_ptr<HistoryChange> change;
        Check(plan.Set(draft)==MMS_E_ARGUMENT&&plan.Take(change)==MMS_E_NOT_READY&&!change&&f.Bytes()==raw,
            "Toon filenames reject 100 CP932 bytes, malformed UTF16, NUL and lossy encoding without staging");
    }
    auto longName=before;longName[0]=std::wstring(99,L'a');Commit(f,longName);
    Check(std::strlen(mdl::Mdl(f.app->ModelSlot(0))->pmdToonFileNames[0])==99,"exactly 99 bytes plus NUL fit the standard Toon field");
    Check(f.history.History(false)==MMS_OK,"restore default before texture failures");
    for(const auto* name:{L"missing.bmp",L"null-success.bmp",L"bad-query.bmp",L"bad-format.bmp"}) {
        NativeModelToonPlan plan;auto value=before;value[0]=name;std::shared_ptr<HistoryChange> change;
        Check(plan.Begin(*f.app,0,FileLoader)==MMS_OK&&plan.Set(value)==MMS_OK&&plan.Take(change)!=MMS_OK&&!change&&
            f.Bytes()==raw&&f.history.RedoCount()==1,"missing, null or unmeasurable required textures cannot partially publish or erase redo");
    }
    {
        Fixture cached;NativePoseTestBinding current(*cached.app,cached.history);
        std::strcpy(mdl::Mdl(cached.app->ModelSlot(0))->pmdToonFileNames[0],"null.bmp");cached.Append(L"D:\\toon-plan\\null.bmp",nullptr);
        const auto calls=cached.textures.state.calls;NativeModelToonPlan plan;
        Check(plan.Begin(*cached.app,0,FileLoader)==MMS_E_NOT_READY&&cached.textures.state.calls==calls&&!cached.history.UndoCount(),
            "an existing cache slot with a null texture cannot load a substitute for the old rendered resource");
    }
    {
        Fixture source;NativePoseTestBinding current(*source.app,source.history);auto& m=*mdl::Mdl(source.app->ModelSlot(0));
        std::memset(m.pmdToonFileNames[0],'x',100);NativeModelToonPlan plan;
        Check(plan.Begin(*source.app,0,FileLoader)==MMS_E_NOT_READY,"unterminated original filename refuses without pretending to restore unknown semantics");
        std::memset(m.pmdToonFileNames[0],0,100);m.pmdToonFileNames[0][0]=char(0x82);
        Check(plan.Begin(*source.app,0,FileLoader)==MMS_E_NOT_READY,"truncated original CP932 filename cannot enter a restorable plan");
    }
    {
        Fixture cancelled;NativePoseTestBinding current(*cancelled.app,cancelled.history);
        auto& m=*mdl::Mdl(cancelled.app->ModelSlot(0));std::strcpy(m.pmdToonFileNames[0],"before.bmp");
        auto* beforeTexture=new ImportTestTexture(cancelled.textures.state);cancelled.Append(L"D:\\toon-plan\\before.bmp",beforeTexture);
        const auto bytes=cancelled.Bytes();const auto live=cancelled.textures.state.live;
        {
            NativeModelToonPlan privatePlan;ModelToonDraft value;
            Check(privatePlan.Begin(*cancelled.app,0,FileLoader)==MMS_OK&&privatePlan.Read(value)==MMS_OK,"capture a custom before texture for cancellation");
            value[0]=L"never-loaded.bmp";Check(privatePlan.Set(value)==MMS_OK&&cancelled.textures.state.live==live&&beforeTexture->ReferenceCount()==2&&
                !cancelled.textures.renderer->resourcePool[1].heapBuffer,"cancelled draft retains the existing before texture without loading a replacement");
        }
        Check(cancelled.textures.state.live==live&&beforeTexture->ReferenceCount()==1&&cancelled.Bytes()==bytes&&!cancelled.history.UndoCount()&&!cancelled.app->EnhancedModelDirty(),
            "discarding a custom private draft releases its resources without touching cache, model, reminder or history");
    }
    {
        Fixture pmx;NativePoseTestBinding current(*pmx.app,pmx.history);auto& m=*mdl::Mdl(pmx.app->ModelSlot(0));m.physicsMode=2;
        auto& material=mdl::Materials(pmx.app->ModelSlot(0))[0];material.toonReference=0xfe;std::wcscpy(material.toonPath,L"D:\\toon-plan\\pmx-own.bmp");
        auto* originalOwn=new ImportTestTexture(pmx.textures.state);pmx.Append(material.toonPath,originalOwn);
        auto value=pmx.Read();value[2]=L"toon09.bmp";Commit(pmx,value);auto* own=pmx.Resolved();
        Check(own==originalOwn&&std::wcscmp(material.toonPath,L"D:\\toon-plan\\pmx-own.bmp")==0&&material.toonReference==0xfe&&
            pmx.history.History(false)==MMS_OK&&pmx.Resolved()==own,"independent PMX material Toon paths/references are retained and never rewritten by table history");
    }
    {
        Fixture unused;NativePoseTestBinding current(*unused.app,unused.history);auto* old=unused.app->ToonTexture(0);unused.app->ToonTexture(0)=nullptr;old->Release();
        NativeModelToonPlan plan;Check(plan.Begin(*unused.app,0,FileLoader)==MMS_OK,"an unused neutral shared slot may be null");
        old=unused.app->ToonTexture(1);unused.app->ToonTexture(1)=nullptr;old->Release();
        NativeModelToonPlan missing;Check(missing.Begin(*unused.app,0,FileLoader)==MMS_E_NOT_READY,"a referenced shared filename requires a live shared texture");
    }
}
void StaleAndBudget() {
    for(unsigned kind=0;kind<6;++kind) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);auto before=f.Read(),after=before;after[0]=L"custom.bmp";
        auto change=f.Change(after);std::unique_ptr<SharedHistory::NativeTransaction> token;
        Check(f.history.PrepareNative("Toon stale candidate",change,token)==MMS_OK,"prepare complete Toon candidate before metadata replacement");
        auto& m=*mdl::Mdl(f.app->ModelSlot(0));auto& material=mdl::Materials(f.app->ModelSlot(0))[0];
        auto* device=f.textures.renderer->device;
        if(kind==0)m.modelDirectory[200]=L'x';
        if(kind==1)material.toonReference=1;
        if(kind==2)material.toonPath[300]=L'x';
        if(kind==3)f.app->state.toonEdgeTable[29]=.99f;
        if(kind==4){auto* old=f.app->ToonTexture(10);f.app->ToonTexture(10)=new ImportTestTexture(f.textures.state);old->Release();}
        if(kind==5)f.textures.renderer->device=reinterpret_cast<IDirect3DDevice9*>(std::uintptr_t(2));
        Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&!f.history.UndoCount()&&!f.textures.renderer->resourcePool[0].heapBuffer,
            "changed directory, material references/path, shared table/slot or renderer rejects before cache publication");
        if(kind==5)f.textures.renderer->device=device;
    }
    {
        Fixture reset;NativePoseTestBinding current(*reset.app,reset.history);auto value=reset.Read();value[0]=L"retained.bmp";Commit(reset,value);
        auto* held=static_cast<ImportTestTexture*>(reset.Resolved());const auto calls=reset.textures.state.calls;
        reset.textures.Clear();
        Check(held->ReferenceCount()>=1&&reset.history.History(false)==MMS_E_STALE&&reset.history.UndoCount()==1&&
            reset.textures.state.calls==calls,"renderer cache replacement retains COM lifetime but refuses stale history without reopening files");
    }
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Read();auto after=before;after[0]=L"budget.bmp";
    auto change=f.Change(after);std::unique_ptr<PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK,"measure Toon candidate real resource payload");std::uint64_t bytes=0;
    Check(candidate->TargetRetainedBytes(bytes)&&bytes>=change->RetainedBytes()&&bytes>11*80,"Toon charge includes all real shared mip textures, cache, strings and retained owner");candidate.reset();
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,128,bytes)==MMS_OK,"set Toon budget below complete journal admission");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("Toon budget failure",change,token)==MMS_E_MEMORY&&!token&&!f.history.UndoCount()&&f.Read()==before&&
        !f.textures.renderer->resourcePool[0].heapBuffer,"budget refusal is atomic for model fields, cache and common history");
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,256ull*1024*1024)==MMS_OK,"restore Toon resource budget");
    bool success=false;unsigned failures=0;
    for(long limit=0;limit<2048&&!success;++limit) {
        NativeModelToonPlan plan;std::shared_ptr<HistoryChange> next;
        failAfter=limit;
        const auto result=Boundary([&]() -> MmsResult {
            auto r=plan.Begin(*f.app,0,FileLoader);if(!r)r=plan.Set(after);if(!r)r=plan.Take(next);
            return r?r:f.history.PrepareNative("Toon allocation failure",next,token);
        });
        failAfter=-1;
        Check(f.Read()==before&&!f.history.UndoCount()&&!f.textures.renderer->resourcePool[0].heapBuffer&&!f.app->EnhancedModelDirty(),
            "every allocation fault keeps the original model, unpublished textures, history and dirty reminder intact");
        if(!result){token.reset();success=true;}else{++failures;Check(result==MMS_E_MEMORY&&!token,"allocation sweep reports a memory refusal");}
    }
    Check(success&&failures>16,"Toon allocation sweep reaches resource capture, schema, cache preparation, bookkeeping and journal reservation");
}
void Interleaved() {
    Fixture f(true);NativePoseTestBinding binding(*f.app,f.history);auto a=f.Read(0),b=f.Read(1),aa=a,bb=b;aa[0]=L"toon06.bmp";bb[0]=L"toon08.bmp";
    Commit(f,aa,0);
    camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.model-toon";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register serializer between native Toon edits");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);std::uint64_t revision=0;MmsHandle tx=0;
    auto participant=plugin.Participant(Text(client.id));
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("Toon plugin"),revision,&tx)==MMS_OK&&api.join(api.context,tx,&participant)==MMS_OK&&
        api.prepare(api.context,tx)==MMS_OK&&api.commit(api.context,tx)==MMS_OK,"real plugin transaction joins the same Toon history");
    Commit(f,bb,1);
    Check(f.history.UndoCount()==3&&f.history.History(false)==MMS_OK&&f.Read(1)==b&&f.Read(0)==aa&&plugin.value==42&&
        f.history.History(false)==MMS_OK&&plugin.value==0&&f.history.History(false)==MMS_OK&&f.Read(0)==a,"undo traverses second model, plugin and first model in one order");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.Read(0)==aa&&f.Read(1)==bb,
        "redo traverses both models and the plugin independently");
    {ObjectModalProbe confirmation(f,true);f.Command(true);Check(confirmation.seen==1&&!f.app->ModelSlot(0),"delete Toon-edited model through the real command");}
    Check(f.history.History(false)==MMS_OK&&f.Read(0)==aa&&f.history.History(false)==MMS_OK&&f.Read(1)==b&&
        f.history.History(false)==MMS_OK&&plugin.value==0&&f.history.History(false)==MMS_OK&&f.Read(0)==a,
        "object restoration renews generation while the older Toon node keeps exact stable identity and resources");
    Check(f.history.Reset()==MMS_OK,"release interleaved Toon/plugin history");f.history.Collect();
    Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister Toon test serializer after its history");
}
struct ModalProbe {
    static thread_local ModalProbe* current;
    Fixture& fixture;DLGPROC procedure=nullptr;LPARAM parameter=0;
    bool initialized=false,accept=true,noop=false,stale=false,truncate=false,armed=false,readFailure=false;
    int reject=0;unsigned calls=0,rejections=0;INT_PTR response=-1;
    static LRESULT CALLBACK Fault(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
        auto& s=*reinterpret_cast<ModalProbe*>(data);
        if(s.armed&&((msg==WM_SETTEXT&&!s.readFailure)||(msg==WM_GETTEXT&&s.readFailure))) {
            s.armed=false;++s.rejections;
            if(msg==WM_GETTEXT)return 0;
            if(!s.truncate)return FALSE;
            std::array<wchar_t,100> text{};const auto* value=reinterpret_cast<const wchar_t*>(lp);const auto n=wcsnlen_s(value,text.size());
            if(n&&n<text.size())std::memcpy(text.data(),value,(n-1)*sizeof(wchar_t));
            DefSubclassProc(h,msg,wp,reinterpret_cast<LPARAM>(text.data()));return TRUE;
        }
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,Fault,id);
        return DefSubclassProc(h,msg,wp,lp);
    }
    static INT_PTR CALLBACK Window(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
        auto& s=*current;
        if(msg==WM_INITDIALOG) {
            Check(IsWindowUnicode(h)!=FALSE,"actual 261 uses a Unicode modal window");
            for(int id=709;id<=718;++id) {
                const auto edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,240,20,h,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
                Check(edit&&IsWindowUnicode(edit),"real Unicode Toon EDIT created");
                if(id==s.reject){Check(SetWindowSubclass(edit,Fault,0x4d545046,reinterpret_cast<DWORD_PTR>(&s)),"install real EDIT failure boundary");s.armed=true;}
            }
            for(int id:{IDOK,IDCANCEL})Check(CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,60,20,h,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr),"create original Toon confirmation buttons");
            const auto result=s.procedure(h,msg,wp,s.parameter);PostMessageW(h,s.reject?WM_COMMAND:WM_APP+61,s.reject?IDOK:0,0);return result;
        }
        if(msg==WM_APP+61) {
            const auto undo=s.fixture.history.UndoCount();
            if(!s.noop) {
                auto edit=GetDlgItem(h,709);Check(SetWindowTextW(edit,L"toon07.bmp"),"enter a valid cross-slot Toon name");
                SendMessageW(edit,WM_KEYDOWN,VK_RETURN,0);
                Check(s.fixture.history.UndoCount()==undo&&std::strcmp(mdl::Mdl(s.fixture.app->ModelSlot(0))->pmdToonFileNames[0],"toon01.bmp")==0,
                    "native Enter only retains a private draft and leaves real metadata/history unchanged");
                const auto japanese=GetDlgItem(h,710);SetWindowTextW(japanese,L"");
                for(const auto unit:std::wstring(L"陰影.bmp"))SendMessageW(japanese,WM_CHAR,unit,1);
                wchar_t typed[100]{};Check(GetWindowTextW(japanese,typed,100)==6&&std::wcscmp(typed,L"陰影.bmp")==0,"real UTF16 EDIT character input retains CP932 names independent of ACP");
                // Keep the real menu success independent of D3DX/fake device;
                // custom IO/resource semantics are covered by injected plans.
                SetWindowTextW(japanese,L"toon02.bmp");
            }
            if(s.stale)mdl::Mdl(s.fixture.app->ModelSlot(0))->modelDirectory[200]=L'!';
            SendMessageW(h,WM_COMMAND,s.accept?IDOK:IDCANCEL,0);return TRUE;
        }
        return s.procedure(h,msg,wp,lp);
    }
    static INT_PTR Open(HINSTANCE,LPCWSTR resource,HWND owner,DLGPROC proc,LPARAM parameter) {
        auto& s=*current;++s.calls;s.procedure=proc;s.parameter=parameter;
        Check(ULONG_PTR(resource)==681||ULONG_PTR(resource)==682,"real menu 261 selects the original Toon resource template");
        struct Template {DLGTEMPLATE dialog{};WORD menu=0,klass=0,title=0;} data;
        data.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;data.dialog.cx=260;data.dialog.cy=260;
        return s.response=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&data.dialog,owner,Window,0);
    }
    explicit ModalProbe(Fixture& f):fixture(f) {
        Check(!current,"one Toon modal probe");current=this;const auto init=MH_Initialize();initialized=init==MH_OK;
        Check(initialized||init==MH_ERROR_ALREADY_INITIALIZED,"initialize Toon resource-template interception");
        Check(MH_CreateHook(reinterpret_cast<void*>(&UiDialogBoxParamW),reinterpret_cast<void*>(&Open),nullptr)==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&UiDialogBoxParamW))==MH_OK,"intercept only actual 261 W resource boundary");
    }
    ~ModalProbe(){MH_DisableHook(reinterpret_cast<void*>(&UiDialogBoxParamW));MH_RemoveHook(reinterpret_cast<void*>(&UiDialogBoxParamW));if(initialized)MH_Uninitialize();current=nullptr;}
    void Run(){CmdViewMenu(fixture.app.get(),fixture.app->MainWindow(),261,0);}
};
thread_local ModalProbe* ModalProbe::current=nullptr;
void ActualMenu() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly})for(unsigned action=0;action<4;++action) {
        Fixture f(true);NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Bytes();ModalProbe probe(f);
        Check(f.history.SetMode(mode)==MMS_OK,"actual Toon menu honors either history recording mode");
        probe.accept=action!=0;probe.noop=action==1;probe.stale=action==3;probe.Run();
        Check(probe.calls==1&&f.Unmoved()&&!f.app->FrameRangeDialog(),"actual 261 leaves no legacy writer or altered frame/pose");
        if(action<2)Check(!f.history.UndoCount()&&f.Bytes()==before&&!f.app->SceneModified()&&!f.app->EnhancedModelDirty(),"native cancel/noop does not dirty the model or add history");
        if(action==2)Check(f.history.UndoCount()==1&&f.Resolved()==f.app->ToonTexture(7)&&RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&
            f.Bytes()==before&&RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&f.Resolved()==f.app->ToonTexture(7),"actual native confirmation/history routes one complete shared Toon publication");
        if(action==3)Check(!f.history.UndoCount()&&!f.app->EnhancedModelDirty()&&f.Bytes()==before,"modal source replacement is rejected instead of recaptured");
    }
    for(bool truncate:{false,true})for(int field=709;field<=718;++field) {
        Fixture f(true);NativePoseTestBinding binding(*f.app,f.history);auto value=f.Read();value[0]=L"toon03.bmp";Commit(f,value);
        Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"retain real redo before native Toon field refusal");const auto before=f.Bytes();
        const std::uint8_t dirty=f.app->SceneModified(),modelDirty=f.app->EnhancedModelDirty();
        ModalProbe probe(f);probe.reject=field;probe.truncate=truncate;probe.Run();
        Check(probe.response==IDCANCEL&&probe.rejections==1&&f.Bytes()==before&&!f.history.UndoCount()&&f.history.RedoCount()==1&&
            f.app->SceneModified()==dirty&&f.app->EnhancedModelDirty()==modelDirty,"refused or short accepted native text cancels without touching any model/cache/history state");
    }
    {
        Fixture f(true);NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Bytes();ModalProbe probe(f);probe.reject=713;probe.readFailure=true;probe.Run();
        Check(probe.response==IDCANCEL&&probe.rejections==1&&f.Bytes()==before&&!f.history.UndoCount(),"failed real EDIT WM_GETTEXT cancels rather than accepting a partial Toon snapshot");
    }
}
}
void NativeModelToonChecks() {
    std::puts("RUN model Toon roundtrip");model_toon_checks::RoundTrip();
    std::puts("RUN model Toon offline/ownership");model_toon_checks::OfflineAndOwnership();
    std::puts("RUN model Toon unresolved before");model_toon_checks::BeforeCacheMiss();
    std::puts("RUN model Toon validation/resources");model_toon_checks::ValidationAndResources();
    std::puts("RUN model Toon stale/budget/allocation");model_toon_checks::StaleAndBudget();
    std::puts("RUN model Toon cross-model/plugin/object");model_toon_checks::Interleaved();
    std::puts("RUN model Toon actual native menu");model_toon_checks::ActualMenu();
}
