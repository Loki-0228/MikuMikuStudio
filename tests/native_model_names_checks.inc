// Include after ObjectCommandFixture and camera_multiply_checks::PluginState.
#include "../src/plugins/native_model_names.hpp"
#include "mikumikustudio/display_capacity.hpp"
#include "mikumikustudio/extended_names.hpp"
#include "mikumikustudio/owned_text.hpp"
#include "mikumikustudio/ui_translation.hpp"
#include <MinHook.h>
#include <type_traits>
namespace model_names_checks {
using namespace plugins;
static_assert(!std::is_copy_constructible_v<NativeModelNamesPlan>);
static_assert(!std::is_copy_assignable_v<NativeModelNamesPlan>);
struct Fixture : ObjectCommandFixture {
    Fixture() {
        app->HInstance()=GetModuleHandleW(nullptr);
        for(unsigned slot=0;slot<2;++slot) {
            auto& m=*mdl::Mdl(app->ModelSlot(slot));
            m.boneCount=2;m.boneTable=static_cast<mdl::BoneRecord*>(std::calloc(2,sizeof(mdl::BoneRecord)));
            m.morphCount=2;m.morphs=static_cast<mdl::MorphRecord*>(std::calloc(2,sizeof(mdl::MorphRecord)));
            m.boneSelection=static_cast<unsigned char*>(std::calloc(2,1));m.bonePhysicsState=static_cast<unsigned char*>(std::calloc(2,1));
            m.groupNames=std::calloc(4,sizeof(mdl::DisplayGroup));m.displayFrames=static_cast<mdl::FrameGroup*>(std::calloc(1,sizeof(mdl::FrameGroup)));
            m.rbGroups=std::calloc(2,sizeof(mdl::FrameGroup));m.rigidBodyCount=2;
            Check(m.boneTable&&m.morphs&&m.boneSelection&&m.bonePhysicsState&&m.groupNames&&m.displayFrames&&m.rbGroups,"allocate real name-bearing native tables");
            mdl::SetDisplayCapacity(m,4,1);m.selectedBone=-1;
            std::strcpy(m.nameEn,slot?"English-B":"English-A");std::strcpy(m.commentEn,"old comment\nline");
            Check(extended_names::Assign(m.pmxTextBuffers[1],slot?L"English-B":L"English-A")&&
                extended_names::Assign(m.pmxTextBuffers[3],L"old comment\nline"),"owned model Unicode names and comment");
            for(unsigned i=0;i<2;++i) {
                auto& b=m.boneTable[i];b.parent=-1;b.rotQuat[3]=1;b.trans[0]=float(7+slot+i);
                std::strcpy(b.name,i?"bone-jp-1":"bone-jp-0");std::strcpy(b.nameEn,i?"bone-english-1":"bone-english-0");
                Check(extended_names::Assign(b.enText,i?L"bone-english-1":L"bone-english-0"),"owned bone English name");
                auto& morph=m.morphs[i];std::strcpy(morph.name,i?"face-jp":"base-jp");std::strcpy(morph.nameEn,i?"face-en":"base-en");
                Check(extended_names::Assign(morph.enText,i?L"face-en":L"base-en"),"owned morph English name");
                auto& row=static_cast<mdl::FrameGroup*>(m.rbGroups)[i];mdl::SetFrameIndices(row,3,i);std::memcpy(row.nameEn,b.nameEn,20);row.selected=BYTE(i+4);
            }
            mdl::SetFrameIndices(m.displayFrames[0],1,1);std::memcpy(m.displayFrames[0].nameEn,m.morphs[1].nameEn,20);m.displayFrames[0].selected=9;
            auto* groups=mdl::DisplayGroups(app->ModelSlot(slot));
            for(unsigned i=0;i<4;++i){std::strcpy(groups[i].name,"native group");std::strcpy(groups[i].nameEn,"English group");groups[i].flags=BYTE(i+2);}
            text_encoding::SetDisplayGroupName(m,groups[3],L"原始显示组",L"English group");
        }
    }
    ModelNamesDraft Read(unsigned slot=0) {
        NativeModelNamesPlan plan;ModelNamesDraft out;
        Check(plan.Begin(*app,slot)==MMS_OK&&plan.Read(out)==MMS_OK,"capture complete names from a fixed native identity");return out;
    }
    std::shared_ptr<HistoryChange> Change(const ModelNamesDraft& draft,unsigned slot=0) {
        NativeModelNamesPlan plan;std::shared_ptr<HistoryChange> out;
        Check(plan.Begin(*app,slot)==MMS_OK&&plan.Set(draft)==MMS_OK&&plan.Take(out)==MMS_OK,"stage all English-name fields privately");return out;
    }
    bool Stable() {
        const auto& m=*mdl::Mdl(app->ModelSlot(0));
        return Unmoved()&&m.boneTable[0].trans[0]==7&&m.boneTable[1].trans[0]==8&&m.selectedBone==-1&&
            static_cast<mdl::FrameGroup*>(m.rbGroups)[1].selected==5&&m.displayFrames[0].selected==9&&
            mdl::DisplayGroups(app->ModelSlot(0))[3].flags==5;
    }
};
ModelNamesDraft Renamed(ModelNamesDraft value) {
    value.model=L"new English model";value.comment=L"new comment\n第二行😀";
    value.bones[0]=L"new bone zero";value.bones[1]=L"很长的骨骼英文名，保留完整 Unicode 😀";
    value.morphs[1]=L"new expression 😀";value.groups[3]=L"新的完整显示组 😀";return value;
}
bool Same(const ModelNamesDraft& a,const ModelNamesDraft& b) {
    return a.model==b.model&&a.comment==b.comment&&a.bones==b.bones&&a.morphs==b.morphs&&a.groups==b.groups;
}
void Commit(Fixture& f,const ModelNamesDraft& value,unsigned slot=0) {
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("model names",f.Change(value,slot),token,SharedHistory::NativeIntent::ExplicitCommit)==MMS_OK,
        "reserve a complete name metadata publication");
    failAfter=0;const auto result=f.history.CommitNative(std::move(token));failAfter=-1;
    Check(result==MMS_OK,"name pointer, fixed mirror, sidecar and frame-row publication allocates nothing");
}
void RoundTripAndNoop() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"names recording mode");
        const auto original=f.Read(),after=Renamed(original);auto* object=f.app->ModelSlot(0);const auto id=SceneObjectIdentity(object).id;
        auto& m=*mdl::Mdl(object);std::array<char,20> oldLongMirror{};std::memcpy(oldLongMirror.data(),m.boneTable[1].nameEn,20);
        const auto ui=NativeModelNamesUiRevision();Commit(f,after);
        Check(f.history.UndoCount()==1&&Same(f.Read(),after)&&f.Stable()&&NativeModelNamesUiRevision()!=ui&&f.app->EnhancedModelDirty(),
            "one explicit metadata node retains all Unicode names and unregistered poses in both modes");
        Check(!std::memcmp(m.boneTable[1].nameEn,oldLongMirror.data(),20)&&
            !std::memcmp(static_cast<mdl::FrameGroup*>(m.rbGroups)[1].nameEn,m.boneTable[1].nameEn,20)&&
            !std::memcmp(m.displayFrames[0].nameEn,m.morphs[1].nameEn,20),
            "unrepresentable Unicode keeps byte-exact standard mirrors and consistent display rows");
        m.boneTable[0].trans[2]=.375f;
        Check(f.history.History(false)==MMS_OK&&Same(f.Read(),original)&&f.history.RedoCount()==1&&m.boneTable[0].trans[2]==.375f&&f.app->EnhancedModelDirty(),
            "undo restores complete original names without restoring the unregistered editing pose");
        {auto noChange=f.Change(original);Check(!noChange&&!f.history.UndoCount()&&f.history.RedoCount()==1,"same-value name confirmation preserves redo");}
        {NativeModelNamesPlan cancel;ModelNamesDraft privateDraft;Check(cancel.Begin(*f.app,0)==MMS_OK&&cancel.Read(privateDraft)==MMS_OK&&
            cancel.Set(after)==MMS_OK&&Same(f.Read(),original)&&f.history.RedoCount()==1,"discarding a changed private draft never writes native metadata");}
        Check(f.history.History(true)==MMS_OK&&Same(f.Read(),after)&&f.app->ModelSlot(0)==object&&SceneObjectPresent(object,id)&&
            m.boneTable[0].trans[2]==.375f&&!f.history.LastPublicationRequiresPhysicsReset(),"redo needs no original file and preserves identity, live pose and physics state");
    }
}
void RawTextAndStale() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);auto& m=*mdl::Mdl(f.app->ModelSlot(0));
    owned_text::Free(m.pmxTextBuffers[1]);
    const std::array<wchar_t,8> raw{L'A',0,L'尾',L'部',0,L'Z',L'!',0};
    m.pmxTextBuffers[1]=static_cast<wchar_t*>(std::malloc(sizeof(raw)));Check(m.pmxTextBuffers[1]!=nullptr,"allocate early-NUL raw text");
    std::memcpy(m.pmxTextBuffers[1],raw.data(),sizeof(raw));owned_text::Remember(m.pmxTextBuffers[1],sizeof(raw));
    auto original=f.Read();auto after=Renamed(original);Commit(f,after);
    Check(f.history.History(false)==MMS_OK,"undo restores raw Unicode text allocation");
    owned_text::Allocation allocation;
    Check(owned_text::Find(m.pmxTextBuffers[1],allocation)&&allocation.bytes==sizeof(raw)&&!std::memcmp(m.pmxTextBuffers[1],raw.data(),sizeof(raw)),
        "undo restores original capacity and UTF16 bytes after an early NUL");
    Check(f.history.Reset()==MMS_OK,"clear early-NUL history before stale-source tests");f.history.Collect();
    auto change=f.Change(after);std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("short source",change,token)==MMS_OK&&extended_names::Assign(m.pmxTextBuffers[1],L"x"),"replace a prepared source with a shorter owned allocation");
    Check(f.history.CommitNative(std::move(token))==MMS_E_STALE&&!f.history.UndoCount()&&std::wcscmp(m.pmxTextBuffers[1],L"x")==0,
        "shorter current Unicode source refuses safely before any old-length memory read");
    {auto value=f.Read();value.model=L"aliased replacement";change=f.Change(value);
        const auto originalOther=m.pmxTextBuffers[0];m.pmxTextBuffers[0]=m.pmxTextBuffers[1];
        Check(f.history.PrepareNative("aliased source",change,token)==MMS_E_STALE&&!token&&std::wcscmp(m.pmxTextBuffers[1],L"x")==0,
            "shared native owned-text aliases cannot be freed by a name edit");m.pmxTextBuffers[0]=originalOther;}
    {NativeModelNamesPlan plan;Check(plan.Begin(*f.app,0)==MMS_OK,"capture before binding replacement");
        {NativePoseTestBinding temporary(*f.app,f.history);}
        Check(plan.Set(after)==MMS_E_STALE,"modal name source does not attach to a replacement binding at identical pointers");}
}
void Boundaries() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);const auto original=f.Read();
    for(unsigned invalid=0;invalid<4;++invalid) {
        auto value=original;NativeModelNamesPlan plan;
        Check(plan.Begin(*f.app,0)==MMS_OK,"capture name schema before invalid private input");
        if(invalid==0)value.model=std::wstring(1,wchar_t(0xd800));
        if(invalid==1)value.bones.pop_back();
        if(invalid==2)value.morphs[0]=L"uneditable base morph";
        if(invalid==3)value.groups[0]=L"uneditable reserved display group";
        std::shared_ptr<HistoryChange> out;
        Check(plan.Set(value)==MMS_E_ARGUMENT&&plan.Take(out)==MMS_E_NOT_READY&&!out&&Same(f.Read(),original),
            "invalid UTF16, array shape and reserved name fields never stage partial metadata");
    }
    auto value=original;value.model=std::wstring(19,L'a');Commit(f,value);auto& model=*mdl::Mdl(f.app->ModelSlot(0));
    Check(std::strlen(model.nameEn)==19&&model.nameEn[19]==0,"nineteen CP932 bytes fit the original twenty-byte model field");
    std::array<char,20> mirror{};std::memcpy(mirror.data(),model.nameEn,20);
    value.model=std::wstring(20,L'b');Commit(f,value);
    Check(!std::memcmp(mirror.data(),model.nameEn,20)&&text_encoding::ModelName(model,true)==value.model,
        "twenty-byte English name uses existing Unicode storage while preserving the standard byte-field boundary");
}
void BudgetAndFailure() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Read(),after=Renamed(before);
    auto change=f.Change(after);std::unique_ptr<PreparedHistoryChange> candidate;
    Check(change->Prepare(true,candidate)==MMS_OK,"measure actual prepared name resources");std::uint64_t bytes=0;
    Check(candidate->TargetRetainedBytes(bytes)&&bytes>change->RetainedBytes(),"name budget includes prepared Unicode owners and group sidecar nodes");candidate.reset();
    Check(f.history.Configure(SharedHistory::RecordingMode::ExplicitOnly,128,bytes)==MMS_OK,"set name budget below complete native-plus-journal charge");
    std::unique_ptr<SharedHistory::NativeTransaction> token;
    Check(f.history.PrepareNative("names budget",change,token)==MMS_E_MEMORY&&!token&&!f.history.UndoCount()&&Same(f.Read(),before),"budget failure cannot leave a partially named model");
    Check(f.history.Configure(SharedHistory::RecordingMode::AllOperations,128,256ull*1024*1024)==MMS_OK,"restore name budget");
    bool success=false;unsigned failures=0;
    for(long limit=0;limit<1024&&!success;++limit) {
        NativeModelNamesPlan plan;std::shared_ptr<HistoryChange> next;
        failAfter=limit;
        // Include by-value argument construction, especially the deliberately
        // non-SSO journal label, in the same boundary as the production caller.
        const auto r=Boundary([&]() -> MmsResult {
            auto result=plan.Begin(*f.app,0);if(!result)result=plan.Set(after);if(!result)result=plan.Take(next);
            return result?result:f.history.PrepareNative("names allocation",next,token);
        });
        failAfter=-1;
        Check(Same(f.Read(),before)&&!f.history.UndoCount()&&!f.app->SceneModified()&&!f.app->EnhancedModelDirty(),"allocation failure preserves all native/Unicode/sidecar fields, save reminder and journal");
        if(!r){token.reset();success=true;}else{++failures;Check(r==MMS_E_MEMORY&&!token,"private name preparation reports allocation refusal");}
    }
    Check(success&&failures>16,"name allocation sweep reaches capture, private draft, owner, Unicode, sidecar and journal stages");
}
void Interleaved() {
    Fixture f;NativePoseTestBinding binding(*f.app,f.history);const auto a=f.Read(0),b=f.Read(1);
    const auto aa=Renamed(a),bb=Renamed(b);Commit(f,aa,0);
    camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.model-names";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register real serializer between model name edits");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;auto api=edits.Api(client);std::uint64_t revision=0;MmsHandle tx=0;
    auto participant=plugin.Participant(Text(client.id));
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("name plugin"),revision,&tx)==MMS_OK&&
        api.join(api.context,tx,&participant)==MMS_OK&&api.prepare(api.context,tx)==MMS_OK&&api.commit(api.context,tx)==MMS_OK,"publish a real plugin edit into the same history");
    Commit(f,bb,1);
    Check(f.history.UndoCount()==3&&f.history.History(false)==MMS_OK&&Same(f.Read(1),b)&&Same(f.Read(0),aa)&&plugin.value==42,
        "common undo first restores the second model only");
    Check(f.history.History(false)==MMS_OK&&plugin.value==0&&Same(f.Read(0),aa)&&f.history.History(false)==MMS_OK&&Same(f.Read(0),a),
        "common history traverses plugin and first-model names in original order");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&
        Same(f.Read(0),aa)&&Same(f.Read(1),bb)&&plugin.value==42,"cross-model and plugin redo restores all independent metadata");
    {ObjectModalProbe confirmation(f,true);f.Command(true);Check(confirmation.seen==1&&!f.app->ModelSlot(0),"delete a renamed model with the real command");}
    Check(f.history.History(false)==MMS_OK&&Same(f.Read(0),aa),"undo deletion restores owned name buffers and sidecars without reopening files");
    Check(f.history.Reset()==MMS_OK,"release named model/plugin records before serializer teardown");f.history.Collect();
    Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister name-interleaved serializer after history cleanup");
}
constexpr wchar_t modalModelName[]=L"modal model \U0001f600 \U0001d11e";
struct ModalProbe {
    static thread_local ModalProbe* current;
    Fixture& fixture;DLGPROC procedure=nullptr;LPARAM parameter=0;
    bool accept=true,noop=false,stale=false;unsigned calls=0;bool initialized=false;
    int rejectInitField=0;bool rejectSwitch=false,rejectArmed=false,truncateWrite=false,shortWriteApplied=false;unsigned rejectedWrites=0;
    INT_PTR response=-1,initResult=-1;int rejectedField=0;
    LRESULT forwardedWriteResult=0;
    std::size_t undoAtOpen=0;std::uint8_t sceneDirtyAtOpen=0;
    static LRESULT CALLBACK RejectText(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
        auto& self=*reinterpret_cast<ModalProbe*>(data);
        if(msg==WM_SETTEXT&&self.rejectArmed) {
            self.rejectArmed=false;++self.rejectedWrites;self.rejectedField=GetDlgCtrlID(h);
            if(self.truncateWrite) {
                // No allocation or exception crosses this native callback.
                // The real EDIT accepts a shorter value while its subclass
                // reports success for the original, unabridged WM_SETTEXT.
                std::array<wchar_t,256> shortened{};const auto* input=reinterpret_cast<const wchar_t*>(lp);
                const auto length=input?wcsnlen_s(input,shortened.size()):0;
                if(length&&length<shortened.size())std::memcpy(shortened.data(),input,(length-1)*sizeof(wchar_t));
                self.forwardedWriteResult=DefSubclassProc(h,msg,wp,reinterpret_cast<LPARAM>(shortened.data()));
                self.shortWriteApplied=length>0&&length<shortened.size()&&self.forwardedWriteResult!=FALSE&&
                    GetWindowTextLengthW(h)==int(length-1);
                return TRUE;
            }
            return FALSE;
        }
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,RejectText,id);
        return DefSubclassProc(h,msg,wp,lp);
    }
    static INT_PTR CALLBACK Window(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
        auto& self=*current;
        if(msg==WM_INITDIALOG) {
            Check(IsWindowUnicode(h)!=FALSE,"actual259 test boundary receives a native Unicode dialog");
            for(int id:{IDOK,667,668,669,670,671,672,673,674,675,676,677,678,679,680}) {
                const bool combo=id==669||id==673||id==677;const bool edit=id==667||id==668||id==672||id==676||id==680;
                Check(CreateWindowExW(0,combo?L"COMBOBOX":edit?L"EDIT":L"BUTTON",L"",WS_CHILD|WS_VISIBLE|
                    (combo?CBS_DROPDOWNLIST:edit?(ES_AUTOHSCROLL|(id==668?ES_MULTILINE:0)):BS_PUSHBUTTON),0,0,100,80,h,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr)!=nullptr,"create real hidden English-name dialog controls");
                Check(IsWindowUnicode(GetDlgItem(h,id))!=FALSE,"native English-name controls retain Unicode window procedures");
                if(id==self.rejectInitField||(self.rejectSwitch&&id==672)) {
                    Check(SetWindowSubclass(GetDlgItem(h,id),RejectText,0x4d4e5446,reinterpret_cast<DWORD_PTR>(&self)),
                        "install one real native EDIT text-write refusal");
                    if(id==self.rejectInitField)self.rejectArmed=true;
                }
            }
            const auto result=self.procedure(h,msg,wp,self.parameter);self.initResult=result;
            // A correct initialization refusal has already ended the dialog.
            // If it were ignored, this queued OK would expose its bad draft
            // without leaving an unattended modal loop behind.
            if(self.rejectInitField)PostMessageW(h,WM_COMMAND,IDOK,0);
            else PostMessageW(h,WM_APP+51,0,0);
            return result;
        }
        if(msg==WM_APP+51) {
            auto& f=self.fixture;auto& m=*mdl::Mdl(f.app->ModelSlot(0));
            Check(GetDlgItem(h,IDCANCEL)&&IsWindowEnabled(GetDlgItem(h,IDCANCEL)),"controlled original template gains a real cancellation button");
            if(!self.noop) {
                auto edit=GetDlgItem(h,667);SetWindowTextW(edit,L"modal model ");
                const auto end=GetWindowTextLengthW(edit);SendMessageW(edit,EM_SETSEL,end,end);
                for(const wchar_t unit:L"\U0001f600 \U0001d11e")if(unit)SendMessageW(edit,WM_CHAR,unit,1);
                std::wstring typed(std::size_t(GetWindowTextLengthW(edit))+1,L'\0');
                const int copied=GetWindowTextW(edit,typed.data(),int(typed.size()));typed.resize(copied);
                Check(typed==modalModelName,"native EDIT character input and GetWindowTextW retain surrogate pairs outside legacy ACPs");
                std::string ansi;
                if(text_encoding::EncodeExact(modalModelName,GetACP(),ansi))
                    std::puts("INFO names ACP represents the Unicode sample; native UTF16 roundtrip remains mandatory");
                else {
                    BOOL replaced=FALSE;const auto cp=GetACP();
                    const int size=WideCharToMultiByte(cp,0,modalModelName,-1,nullptr,0,nullptr,&replaced);
                    Check(size>0,"query the real ACP conversion of the Unicode EDIT input");
                    std::string lossy(std::size_t(size),'\0');
                    Check(WideCharToMultiByte(cp,0,modalModelName,-1,lossy.data(),size,nullptr,&replaced)==size,
                        "perform the real ACP conversion independently of the wide control");
                    std::wstring decoded;
                    Check(text_encoding::Decode(lossy.c_str(),cp,decoded)&&decoded!=modalModelName,
                        "the native EDIT retains characters which the current ACP roundtrip actually loses");
                }
                SetDlgItemTextW(h,672,L"modal bone zero");
                SendMessageW(GetDlgItem(h,672),WM_KEYDOWN,VK_RETURN,0);
                Check(f.history.UndoCount()==self.undoAtOpen&&f.app->SceneModified()==self.sceneDirtyAtOpen&&
                    std::wcscmp(m.boneTable[0].enText,L"bone-english-0")==0,
                    "Enter stores only private text and does not close or publish the dialog");
                if(self.rejectSwitch)self.rejectArmed=true;
                SendDlgItemMessageW(h,669,CB_SETCURSEL,1,0);SendMessageW(h,WM_COMMAND,MAKEWPARAM(669,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(h,669)));
                if(self.rejectSwitch) {PostMessageW(h,WM_COMMAND,IDOK,0);return TRUE;}
                SetDlgItemTextW(h,672,L"modal bone one");SetDlgItemTextW(h,676,L"modal expression");SetDlgItemTextW(h,680,L"modal group 😀");
                SetDlgItemTextW(h,668,L"modal comment\r\nnext");
            }
            if(self.stale)Check(extended_names::Assign(m.pmxTextBuffers[1],L"foreign"),"replace fixed modal source before confirmation");
            SendMessageW(h,WM_COMMAND,self.accept?IDOK:IDCANCEL,0);return TRUE;
        }
        return self.procedure(h,msg,wp,lp);
    }
    static INT_PTR Open(HINSTANCE,LPCWSTR resource,HWND owner,DLGPROC proc,LPARAM parameter) {
        auto& self=*current;++self.calls;self.procedure=proc;self.parameter=parameter;
        Check(ULONG_PTR(resource)==665||ULONG_PTR(resource)==666,"actual259 uses the original native English-name template selection");
        struct Template {DLGTEMPLATE dialog{};WORD menu=0,klass=0,title=0;} data;
        data.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;data.dialog.cx=226;data.dialog.cy=228;
        self.response=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&data.dialog,owner,Window,0);
        return self.response;
    }
    explicit ModalProbe(Fixture& f):fixture(f),undoAtOpen(f.history.UndoCount()),sceneDirtyAtOpen(f.app->SceneModified()) {
        Check(!current,"one native name modal probe");current=this;const auto init=MH_Initialize();initialized=init==MH_OK;
        Check(initialized||init==MH_ERROR_ALREADY_INITIALIZED,"initialize native name dialog interception");
        Check(MH_CreateHook(reinterpret_cast<void*>(&UiDialogBoxParamW),reinterpret_cast<void*>(&Open),nullptr)==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&UiDialogBoxParamW))==MH_OK,"intercept only the Unicode resource-template boundary of actual259");
    }
    ~ModalProbe(){MH_DisableHook(reinterpret_cast<void*>(&UiDialogBoxParamW));MH_RemoveHook(reinterpret_cast<void*>(&UiDialogBoxParamW));if(initialized)MH_Uninitialize();current=nullptr;}
    void Run(){CmdViewMenu(fixture.app.get(),fixture.app->MainWindow(),259,0);}
};
thread_local ModalProbe* ModalProbe::current=nullptr;
void ActualMenu() {
    for(unsigned action=0;action<4;++action) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Read();ModalProbe probe(f);
        probe.accept=action!=0;probe.noop=action==1;probe.stale=action==3;probe.Run();
        Check(probe.calls==1&&!f.app->FrameRangeDialog()&&f.Stable(),"actual259 controlled route leaves no legacy modeless writer or altered edit pose");
        if(action<2)Check(!f.history.UndoCount()&&Same(f.Read(),before)&&!f.app->SceneModified()&&!f.app->EnhancedModelDirty(),"native cancel and same-value OK produce no edit, model save reminder or journal node");
        if(action==2) {
            auto after=f.Read();Check(f.history.UndoCount()==1&&after.model==modalModelName&&after.bones[0]==L"modal bone zero"&&
                after.bones[1]==L"modal bone one"&&after.morphs[1]==L"modal expression"&&after.groups[3]==L"modal group 😀"&&
                after.comment==L"modal comment\nnext","one native OK submits every field visited through private combo switching");
            Check(RunBoundObjectHistory(*f.app,f.history,false)==MMS_OK&&Same(f.Read(),before)&&
                RunBoundObjectHistory(*f.app,f.history,true)==MMS_OK&&Same(f.Read(),after),"actual history routing refreshes native name edits in both directions");
        }
        if(action==3)Check(!f.history.UndoCount()&&text_encoding::ModelName(*mdl::Mdl(f.app->ModelSlot(0)),true)==L"foreign"&&
            f.Read().bones==before.bones,"modal source replacement cannot be silently recaptured as an undo baseline");
    }
}
void DialogTextWriteFailures() {
    for(bool truncateWrite:{false,true}) for(bool keepRedo:{false,true}) for(int field:{667,668,672,676,680,0}) {
        Fixture f;NativePoseTestBinding binding(*f.app,f.history);const auto before=f.Read();
        const auto after=Renamed(before);
        if(keepRedo) {
            Commit(f,after);
            Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1&&Same(f.Read(),before),
                "preserve a real redo entry before a native name field failure");
        }
        const auto undo=f.history.UndoCount(),redo=f.history.RedoCount();
        const std::uint8_t sceneDirty=f.app->SceneModified(),modelDirty=f.app->EnhancedModelDirty();
        std::uint64_t revision=0,current=0;
        Check(f.history.Revision(revision)==MMS_OK,"capture journal revision before refusing native name text");
        {
            ModalProbe probe(f);probe.rejectInitField=field;probe.rejectSwitch=field==0;probe.truncateWrite=truncateWrite;probe.Run();
            const bool cancelled=probe.calls==1&&probe.rejectedWrites==1&&!probe.rejectArmed&&probe.response==IDCANCEL&&
                (!truncateWrite||probe.shortWriteApplied);
            if(!cancelled)
                std::fprintf(stderr,"MODEL_NAMES_TEXT_REFUSAL truncate=%u redo=%u field=%d rejectedField=%d calls=%u rejectedWrites=%u armed=%u shortWriteApplied=%u forwardedWriteResult=%lld initResult=%lld response=%lld\n",
                    unsigned(truncateWrite),unsigned(keepRedo),field,probe.rejectedField,probe.calls,probe.rejectedWrites,unsigned(probe.rejectArmed),
                    unsigned(probe.shortWriteApplied),static_cast<long long>(probe.forwardedWriteResult),
                    static_cast<long long>(probe.initResult),static_cast<long long>(probe.response));
            Check(cancelled,"refused or incomplete accepted native text cancels the private dialog exactly once");
        }
        Check(Same(f.Read(),before)&&f.Stable()&&!f.app->FrameRangeDialog()&&
            f.history.UndoCount()==undo&&f.history.RedoCount()==redo&&f.history.Revision(current)==MMS_OK&&current==revision&&
            f.app->SceneModified()==sceneDirty&&f.app->EnhancedModelDirty()==modelDirty,
            "failed native text writes preserve every model field, live pose, journal, redo and save reminder");
        if(keepRedo)Check(f.history.History(true)==MMS_OK&&Same(f.Read(),after),
            "the prior name redo remains usable after the modal field write failed");
    }
}
}
void NativeModelNamesChecks() {
    std::printf("RUN model names roundtrip\n");
    model_names_checks::RoundTripAndNoop();
    std::printf("RUN model names raw/stale\n");
    model_names_checks::RawTextAndStale();
    std::printf("RUN model names boundaries\n");
    model_names_checks::Boundaries();
    std::printf("RUN model names budget/failure\n");
    model_names_checks::BudgetAndFailure();
    std::printf("RUN model names interleaved\n");
    model_names_checks::Interleaved();
    std::printf("RUN model names actual menu\n");
    model_names_checks::ActualMenu();
    std::printf("RUN model names dialog text failures\n");
    model_names_checks::DialogTextWriteFailures();
}
