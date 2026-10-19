#pragma once
#include "mikumikustudio/accessory_layout.hpp"
#include "mikumikustudio/plugin_scene.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/animation_curve_motion.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/plugin_runtime.hpp"
#include "../src/plugins/native_object_commands.hpp"
#include "../src/plugins/edit_store.hpp"
#include "../src/plugins/native_curve_commands.hpp"
#include "../src/plugins/native_global_commands.hpp"
#include <commctrl.h>
#include <cwchar>

namespace bound_curve_history_checks {
namespace p= mikumikustudio::plugins;
constexpr int sourceCombo=1002,canvasId=1020,channelTree=1015,frameField=1041,valueField=1043;
constexpr int addButton=1056,deleteButton=1057,applyButton=1058,undoButton=1059,redoButton=1060;
constexpr curve_host::Kind kinds[]={curve_host::Kind::Camera,curve_host::Kind::Light,
    curve_host::Kind::Shadow,curve_host::Kind::Gravity,curve_host::Kind::Accessory};
int Lane(curve_host::Kind kind){return kind==curve_host::Kind::Shadow?1:0;}
struct PluginState {
    int value=0,candidate=0,before=0,after=0,commits=0,cancels=0;
    bool failRestore=false;
    MmsSerializerV1 Serializer(){
        MmsSerializerV1 s{p::Header<MmsSerializerV1>(),this};
        s.prepareLoad=[](void* context,MmsBytes bytes,std::uint32_t schema,MmsHandle* out)->MmsResult{
            auto& state=*static_cast<PluginState*>(context);
            if(state.failRestore)return MMS_E_NOT_READY;
            if(schema&&(schema!=7||!bytes.data||bytes.bytes!=sizeof(int)))return MMS_E_ARGUMENT;
            state.candidate=0;if(schema)std::memcpy(&state.candidate,bytes.data,sizeof(int));*out=1;return MMS_OK;
        };
        s.commitLoad=[](void* context,MmsHandle){auto& s=*static_cast<PluginState*>(context);s.value=s.candidate;};
        s.cancelLoad=[](void*,MmsHandle){};
        s.save=[](void* context,MmsBytes* bytes,std::uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& s=*static_cast<PluginState*>(context);*bytes={&s.value,sizeof(int)};*schema=7;*lease=2;return MMS_OK;
        };
        s.releaseBytes=[](void*,MmsHandle){};return s;
    }
    MmsTransactionParticipantV1 Participant(MmsString id){
        MmsTransactionParticipantV1 result{p::Header<MmsTransactionParticipantV1>(),this,id};
        result.prepare=[](void* context)->MmsResult{const auto& s=*static_cast<PluginState*>(context);return s.value==s.before?MMS_OK:MMS_E_STALE;};
        result.commit=[](void* context){auto& s=*static_cast<PluginState*>(context);s.value=s.after;++s.commits;};
        result.cancel=[](void* context){++static_cast<PluginState*>(context)->cancels;};
        result.getUndoState=[](void* context,MmsBytes* before,MmsBytes* after,std::uint32_t* schema,MmsHandle* lease)->MmsResult{
            auto& s=*static_cast<PluginState*>(context);*before={&s.before,sizeof(int)};*after={&s.after,sizeof(int)};*schema=7;*lease=3;return MMS_OK;
        };
        result.releaseUndoState=[](void*,MmsHandle){};return result;
    }
};
struct Fixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    p::StorageStore storage;
    p::SharedHistory history;
    std::unique_ptr<mdl::AccessoryRecord> accessory=std::make_unique<mdl::AccessoryRecord>();
    std::unique_ptr<p::NativePoseTestBinding> binding;
    std::unique_ptr<p::EditStore> edits;
    p::StorageClient storageClient;
    p::EditClient editClient;
    PluginState plugin;
    MmsHandle provider=0;
    HWND editor=nullptr;
    const char* stage="fixture";
    unsigned addAttempt=0;
    HWND historyFocusBefore=nullptr;
    SHORT historyControl=0,historyShift=0,historyMenu=0;
    BOOL historyUndoEnabled=FALSE,historyRedoEnabled=FALSE;
    std::uint64_t historyRevisionBefore=0,historyRevisionAfter=0;
    MmsResult historyRevisionResultBefore=MMS_OK,historyRevisionResultAfter=MMS_OK;
    explicit Fixture(p::SharedHistory::RecordingMode mode=p::SharedHistory::RecordingMode::AllOperations,
                     bool nativeCamera=false,std::uint64_t budget=64ull*1024*1024)
        :history(storage,[this]()->MmsResult{
            if(p::NativeObjectUiPending(app.get()))return MMS_E_BUSY;
            if((p::NativeObjectOperationActive(app.get())||app->SceneMutationInProgress())&&
               !p::NativeObjectPublicationAuthorized(app.get()))return MMS_E_BUSY;
            if(app->PlaybackActive()||app->FrameStepPlayback()||app->state.recordPlaybackStartPending)return MMS_E_BUSY;
            return MMS_OK;
        },128,budget){
        ::new(static_cast<void*>(&app->state))MMDAppState{};app->EnglishUI()=2;app->CameraMode()=1;
        app->CameraParentModel()=-1;app->WindowLayoutReady()=1;app->SidebarWidth()=400;
        WNDCLASSW type{};type.lpfnWndProc=CurveTransportHost;type.hInstance=GetModuleHandleW(nullptr);
        type.lpszClassName=L"MMS.BoundCurveHistory.Host";RegisterClassW(&type);
        app->MainWindow()=CreateWindowW(type.lpszClassName,L"bound curve history",WS_OVERLAPPED,0,0,800,600,nullptr,nullptr,type.hInstance,nullptr);
        Check(app->MainWindow()!=nullptr,"bound curve hidden native owner");
        SetWindowLongPtrW(app->MainWindow(),GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app.get()));
        for(int id:{panel::kCurrentFrameEdit,panel::kUndoButton,panel::kRedoButton,panel::kReadoutPosXEdit,436,471,443}){
            const bool combo=id==436||id==471||id==443;
            Check(CreateWindowW(combo?L"COMBOBOX":id==panel::kCurrentFrameEdit||id==panel::kReadoutPosXEdit?L"EDIT":L"BUTTON",L"0",
                WS_CHILD|(combo?CBS_DROPDOWNLIST:0),0,0,100,100,app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),type.hInstance,nullptr)!=nullptr,
                "bound curve owner readout and history controls");
        }
        app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(2);
        app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(2);
        app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(2);
        app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(2);
        app->AccessoryKeys(0)=mdl::KeyPoolCreate<mdl::AccessoryKey>(2);
        Check(app->CameraKeys()&&app->LightKeys()&&app->ShadowKeys()&&app->GravityKeys()&&app->AccessoryKeys(0),"bound curve managed pools");
        app->CameraKeys()[0].fov=45;app->CameraKeys()[0].distance=-30;app->CameraKeys()[0].parentModel=-1;
        app->CameraFov()=45;app->CameraDistance()=-30;
        app->LightKeys()[0].direction[1]=-1;
        for(auto& color:app->LightKeys()[0].color)color=1;
        app->LightDirection()[1]=-1;for(int i=0;i<3;++i)app->LightColor()[i]=1;
        app->ShadowKeys()[0].mode=1;app->ShadowKeys()[0].distance=.02f;
        app->ShadowMode()=1;app->ShadowDistance()=.02f;
        app->GravityKeys()[0].acceleration=9.8f;app->GravityKeys()[0].direction[1]=-1;
        app->GravityMagnitude()=9.8f;app->GravityDirection()[1]=-1;
        std::strcpy(accessory->name,"bound-prop");accessory->parentModel=-1;
        accessory->scale=accessory->opacity=1;accessory->visible=1;
        app->AccessorySlot(0)=accessory.get();app->SelectedAccessorySlot()=0;app->AccessoryRenderSplitOrder()=1;
        auto& accessoryKey=app->AccessoryKeys(0)[0];accessoryKey.parentModel=-1;
        accessoryKey.scale=accessoryKey.opacity=1;accessoryKey.visible=1;
        p::SceneObjectIdentity(accessory.get());
        if(nativeCamera){
            auto& first=app->CameraKeys()[0];auto& last=app->CameraKeys()[1];last=first;last.frame=20;last.eye[0]=10;
            first.next=1;last.previous=0;
            for(auto* key:{&first,&last})for(int lane=0;lane<6;++lane){
                key->interpolation[0][lane]=20;key->interpolation[1][lane]=5;
                key->interpolation[2][lane]=110;key->interpolation[3][lane]=120;
            }
            app->LastRegisteredFrame()=20;
        }
        Check(history.SetMode(mode)==MMS_OK,"bound curve history mode configured");
        binding=std::make_unique<p::NativePoseTestBinding>(*app,history);
        curve_host::Open(app.get(),false);
        EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM context)->BOOL{
            auto& f=*reinterpret_cast<Fixture*>(context);wchar_t name[96]{};GetClassNameW(window,name,96);
            if(GetWindow(window,GW_OWNER)==f.app->MainWindow()&&!std::wcscmp(name,L"MikuMikuStudioCurveEditorWindow"))f.editor=window;
            return TRUE;
        },reinterpret_cast<LPARAM>(this));
        Check(editor&&GetDlgItem(editor,canvasId),"real scene curve editor opens with bound production history");
        Select(0,0);
    }
    ~Fixture(){
        ce::CloseOwned(app->MainWindow());editor=nullptr;binding.reset();edits.reset();
        Check(history.Reset()==MMS_OK,"bound curve history releases before native resources");history.Collect();
        if(provider)Check(storage.Unregister(storageClient,provider)==MMS_OK,"bound curve serializer unregisters");
        p::ForgetBoundObjectUi(app.get());curve_host::Reset(app.get());p::ForgetSceneObject(accessory.get());
        mdl::KeyPoolDestroy(app->CameraKeys());mdl::KeyPoolDestroy(app->LightKeys());mdl::KeyPoolDestroy(app->ShadowKeys());
        mdl::KeyPoolDestroy(app->GravityKeys());mdl::KeyPoolDestroy(app->AccessoryKeys(0));
        app->CameraKeys()=nullptr;app->LightKeys()=nullptr;app->ShadowKeys()=nullptr;app->GravityKeys()=nullptr;app->AccessoryKeys(0)=nullptr;
        app->AccessorySlot(0)=nullptr;DestroyWindow(app->MainWindow());app->MainWindow()=nullptr;
    }
    void Click(int id){const auto button=GetDlgItem(editor,id);Check(button!=nullptr,"real bound curve button exists");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(button));}
    static HTREEITEM Leaf(HWND tree,HTREEITEM first,int index){
        for(auto item=first;item;item=TreeView_GetNextSibling(tree,item)){
            TVITEMW value{};value.hItem=item;value.mask=TVIF_PARAM;TreeView_GetItem(tree,&value);
            if(value.lParam==index+1)return item;
            if(auto child=Leaf(tree,TreeView_GetChild(tree,item),index))return child;
        }return nullptr;
    }
    void Select(int source,int lane){
        const auto combo=GetDlgItem(editor,sourceCombo);
        Check(SendMessageW(combo,CB_SETCURSEL,source,0)==source,"bound curve source selected");
        SendMessageW(editor,WM_COMMAND,MAKEWPARAM(sourceCombo,CBN_SELCHANGE),reinterpret_cast<LPARAM>(combo));
        const auto tree=GetDlgItem(editor,channelTree);const auto item=Leaf(tree,TreeView_GetRoot(tree),lane);
        Check(item!=nullptr,"real bound curve channel leaf");TreeView_EnsureVisible(tree,item);RECT row{};
        Check(TreeView_GetItemRect(tree,item,&row,TRUE)!=FALSE,"bound channel label rectangle");
        POINT point{row.left+1,(row.top+row.bottom)/2};TVHITTESTINFO hit{};hit.pt=point;
        Check(TreeView_HitTest(tree,&hit)==item&&(hit.flags&TVHT_ONITEMLABEL),"bound channel label hit is independent of checkbox");
        SendMessageW(tree,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        SendMessageW(tree,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
    }
    void Add(int frame,double value){
        const auto text=std::to_wstring(frame);wchar_t number[64]{};std::swprintf(number,64,L"%.17g",value);
        SetDlgItemTextW(editor,frameField,text.c_str());SetDlgItemTextW(editor,valueField,number);
        const auto field=GetDlgItem(editor,valueField);const auto enabledBefore=IsWindowEnabled(editor);++addAttempt;
        SetLastError(ERROR_SUCCESS);const auto previousFocus=SetFocus(field);const auto focusError=GetLastError();
        const auto focusBefore=GetFocus();
        const auto diagnose=[&](const char* phase){
            const auto actual=GetFocus();char actualClass[96]{},expectedClass[96]{};
            if(actual)GetClassNameA(actual,actualClass,96);if(field)GetClassNameA(field,expectedClass,96);
            std::fprintf(stderr,"DIAG bound Add phase=%s stage=%s attempt=%u source=%lld frame=%d value=%.17g undo=%zu redo=%zu\n"
                "  focus previous=%p before-command=%p current=%p(id=%d,class=%s) expected=%p(id=%d,class=%s) SetFocus-error=%lu\n"
                "  editor=%p enabled-before=%d enabled-now=%d visible=%d main=%p enabled=%d visible=%d field-enabled=%d field-visible=%d active=%p foreground=%p capture=%p ui-locked=%d\n",
                phase,stage,addAttempt,static_cast<long long>(SendDlgItemMessageW(editor,sourceCombo,CB_GETCURSEL,0,0)),frame,value,
                history.UndoCount(),history.RedoCount(),static_cast<void*>(previousFocus),static_cast<void*>(focusBefore),static_cast<void*>(actual),
                actual?GetDlgCtrlID(actual):0,actualClass,static_cast<void*>(field),field?GetDlgCtrlID(field):0,expectedClass,
                static_cast<unsigned long>(focusError),static_cast<void*>(editor),int(enabledBefore),int(IsWindowEnabled(editor)),int(IsWindowVisible(editor)),
                static_cast<void*>(app->MainWindow()),int(IsWindowEnabled(app->MainWindow())),int(IsWindowVisible(app->MainWindow())),
                int(IsWindowEnabled(field)),int(IsWindowVisible(field)),static_cast<void*>(GetActiveWindow()),static_cast<void*>(GetForegroundWindow()),
                static_cast<void*>(GetCapture()),int(bake::UiLocked()));
        };
        if(!enabledBefore||focusBefore!=field)diagnose("before-command");
        Check(enabledBefore&&focusBefore==field,"bound curve Add starts with an enabled editor and actual text focus");
        Click(addButton);
        if(!IsWindowEnabled(editor)||GetFocus()!=field)diagnose("after-command");
        Check(IsWindowEnabled(editor)&&GetFocus()==field,"bound curve publication keeps the modeless editor enabled and its text focus");
    }
    void History(bool redo){
        BYTE previous[256]{},keys[256]{};Check(GetKeyboardState(previous)!=FALSE,"read synthetic shortcut state");
        std::memcpy(keys,previous,sizeof keys);keys[VK_CONTROL]=keys[VK_LCONTROL]=0x80;keys[VK_MENU]=keys[VK_SHIFT]=0;
        Check(SetKeyboardState(keys)!=FALSE,"set synthetic bound Ctrl shortcut");
        SetFocus(GetDlgItem(editor,canvasId));historyFocusBefore=GetFocus();
        historyControl=GetKeyState(VK_CONTROL);historyShift=GetKeyState(VK_SHIFT);historyMenu=GetKeyState(VK_MENU);
        historyUndoEnabled=IsWindowEnabled(GetDlgItem(editor,undoButton));historyRedoEnabled=IsWindowEnabled(GetDlgItem(editor,redoButton));
        historyRevisionResultBefore=history.Revision(historyRevisionBefore);
        SendMessageW(GetDlgItem(editor,canvasId),WM_KEYDOWN,redo?'Y':'Z',1);
        historyRevisionResultAfter=history.Revision(historyRevisionAfter);
        Check(SetKeyboardState(previous)!=FALSE,"restore shortcut state");
    }
    void DiagnoseHistory(){
        wchar_t status[1024]{};char statusUtf8[4096]{};GetDlgItemTextW(editor,1080,status,1024);
        WideCharToMultiByte(CP_UTF8,0,status,-1,statusUtf8,4096,nullptr,nullptr);
        std::uint64_t revision=0;const auto revisionResult=history.Revision(revision);
        const auto focus=GetFocus();const auto* label=history.NextLabel(false);
        std::fprintf(stderr,"  editor-status=%s\n"
            "  history-shortcut focus-before=%p(id=%d) ctrl=%04x shift=%04x alt=%04x undo-enabled-before=%d redo-enabled-before=%d revision-before=%llu(result=%ld) after=%llu(result=%ld)\n"
            "  windows focus=%p(id=%d) capture=%p editor-enabled=%d undo-enabled-now=%d redo-enabled-now=%d main-enabled=%d main-thread=%lu current-thread=%lu\n"
            "  binding same-history=%d generation=%llu revision=%llu(result=%ld) navigation-admission=%ld next-undo=%s storage-prepared=%d storage-callback=%d\n"
            "  scene input-lock=%d plugin-callback=%d native-operation=%d native-ui-pending=%d native-publication=%d mutation=%d save=%d playback=%d frame-step=%d record-start=%d\n"
            "  modal frame-range=%p frame-copy=%p recording=%p gravity=%p shadow=%p accessory-order=%p accessory-edit=%d edge=%p\n",
            statusUtf8,static_cast<void*>(historyFocusBefore),historyFocusBefore?GetDlgCtrlID(historyFocusBefore):0,
            unsigned(static_cast<unsigned short>(historyControl)),unsigned(static_cast<unsigned short>(historyShift)),unsigned(static_cast<unsigned short>(historyMenu)),
            int(historyUndoEnabled),int(historyRedoEnabled),static_cast<unsigned long long>(historyRevisionBefore),long(historyRevisionResultBefore),
            static_cast<unsigned long long>(historyRevisionAfter),long(historyRevisionResultAfter),static_cast<void*>(focus),focus?GetDlgCtrlID(focus):0,
            static_cast<void*>(GetCapture()),int(IsWindowEnabled(editor)),int(IsWindowEnabled(GetDlgItem(editor,undoButton))),int(IsWindowEnabled(GetDlgItem(editor,redoButton))),
            int(IsWindowEnabled(app->MainWindow())),static_cast<unsigned long>(GetWindowThreadProcessId(app->MainWindow(),nullptr)),static_cast<unsigned long>(GetCurrentThreadId()),
            int(p::BoundNativePoseHistory(app.get())==&history),static_cast<unsigned long long>(p::BoundNativePoseGeneration(app.get())),
            static_cast<unsigned long long>(revision),long(revisionResult),long(history.CheckNativeNavigation()),label?label->c_str():"<none>",int(storage.Prepared()),int(storage.InCallback()),
            int(bake::UiLocked()),int(p::InCallback()),int(p::NativeObjectOperationActive(app.get())),int(p::NativeObjectUiPending(app.get())),
            int(p::NativeObjectPublicationAuthorized(app.get())),int(app->SceneMutationInProgress()),int(SceneSaveInProgress()),int(app->PlaybackActive()),int(app->FrameStepPlayback()),int(app->state.recordPlaybackStartPending),
            static_cast<void*>(app->FrameRangeDialog()),static_cast<void*>(app->state.frameCopyDialog),static_cast<void*>(app->RecordingWindow()),static_cast<void*>(app->GravitySettingDialog()),
            static_cast<void*>(app->GroundShadowColorDialog()),app->AccessoryOrderArray(),int(app->state.accessoryEditDialogOpen),static_cast<void*>(app->state.edgeThicknessDialog));
    }
    std::vector<ce::Channel> Read(curve_host::Kind kind){std::vector<ce::Channel> channels;std::wstring error;
        Check(curve_host::Read(*app,kind,0,channels,error),"read actual bound authoring");return channels;}
    bool Has(curve_host::Kind kind,int frame){const auto channels=Read(kind);const auto& keys=channels[Lane(kind)].track.keys;
        return std::any_of(keys.begin(),keys.end(),[&](const auto& key){return key.frame==frame;});}
    void PluginEdit(int value,bool commit=true){
        if(!edits){
            storageClient.id="example.bound-curve";storage.Api(storageClient);const auto serializer=plugin.Serializer();
            Check(storage.Register(storageClient,p::Text(storageClient.id),&serializer,sizeof serializer,&provider)==MMS_OK,"real bound plugin serializer registers");
            edits=std::make_unique<p::EditStore>(history,storage);editClient.id=storageClient.id;edits->Api(editClient);
        }
        auto api=edits->Api(editClient);std::uint64_t revision=0;Check(history.Revision(revision)==MMS_OK,"read common revision for plugin edit");
        plugin.before=plugin.value;plugin.after=value;const auto participant=plugin.Participant(p::Text(storageClient.id));MmsHandle transaction=0;
        Check(api.begin(api.context,p::Text(std::string("bound plugin edit")),revision,&transaction)==MMS_OK&&
              api.join(api.context,transaction,&participant)==MMS_OK&&api.prepare(api.context,transaction)==MMS_OK,
              "real plugin transaction prepares beside bound curve nodes");
        if(commit)Check(api.commit(api.context,transaction)==MMS_OK,"real plugin transaction commits to the common journal");
        else api.cancel(api.context,transaction);
    }
};

void FiveSourcesAndPlugin(){
    for(auto mode:{p::SharedHistory::RecordingMode::AllOperations,p::SharedHistory::RecordingMode::ExplicitOnly}){
        Fixture f(mode);const double values[]={4,3,.05,12.5,7};
        f.stage=mode==p::SharedHistory::RecordingMode::AllOperations?"FiveSourcesAndPlugin/all":"FiveSourcesAndPlugin/explicit";
        for(int source=0;source<5;++source){f.Select(source,Lane(kinds[source]));const auto count=f.history.UndoCount();
            f.Add(10,values[source]);Check(f.history.UndoCount()==count+1&&f.Has(kinds[source],10),"each real source Add commits one shared history node in both modes");
            f.Add(10,values[source]);Check(f.history.UndoCount()==count+1,"Add at an existing key is only selection");}
        f.PluginEdit(7);Check(f.plugin.value==7&&f.history.UndoCount()==6,"plugin transaction shares chronology with five native sources");
        f.Select(0,0);f.Add(20,8);Check(f.history.UndoCount()==7&&f.Has(kinds[0],20),"camera edit follows plugin transaction");
        f.Select(1,0);SetDlgItemTextW(f.editor,1024,L"37");f.Click(1025);
        Check(f.app->CurrentFrame()==37,"bound editor seeks before cross-source history");
        f.History(false);Check(f.history.UndoCount()==6&&!f.Has(kinds[0],20)&&f.app->CurrentFrame()==37,"Ctrl Z in light source undoes latest camera edit without seeking");
        f.plugin.failRestore=true;f.History(false);
        Check(f.history.UndoCount()==6&&f.plugin.value==7&&f.app->CurrentFrame()==37,"failed real plugin restore keeps history cursor and playhead");
        f.plugin.failRestore=false;f.History(false);
        Check(f.history.UndoCount()==5&&f.plugin.value==0,"Ctrl Z in light source restores the interleaved plugin node");
        for(int source=4;source>=0;--source){f.History(false);
            Check(f.history.UndoCount()==std::size_t(source)&&!f.Has(kinds[source],10)&&f.app->CurrentFrame()==37,
                  "one source shortcut traverses every native source in global order");}
        for(int source=0;source<5;++source){f.History(true);
            Check(f.history.UndoCount()==std::size_t(source+1)&&f.Has(kinds[source],10)&&f.app->CurrentFrame()==37,
                  "Ctrl Y replays native sources without moving the playhead");}
        f.History(true);f.History(true);
        Check(f.plugin.value==7&&f.history.UndoCount()==7&&f.Has(kinds[0],20),"redo restores plugin and later camera edits in one chronology");
        f.PluginEdit(99,false);Check(f.plugin.value==7&&f.history.UndoCount()==7&&f.plugin.cancels==1,"cancelled plugin preparation adds no history");
        const auto before=f.history.UndoCount();Check(f.storage.Prepare({})==MMS_OK,"prepare real serializer gate during curve command");
        f.Select(1,0);f.Add(30,9);
        Check(f.history.UndoCount()==before&&!f.Has(kinds[1],30),"prepared serializer prevents actual window edits before publication");f.storage.Cancel();
        f.Add(10,values[1]);const auto deleteCount=f.history.UndoCount();f.Click(deleteButton);
        Check(f.history.UndoCount()==deleteCount+1&&!f.Has(kinds[1],10),"real source Delete uses the same common journal");
        f.History(false);Check(f.history.UndoCount()==deleteCount&&f.Has(kinds[1],10),"Ctrl Z restores the real deleted light curve key");
        f.Select(0,0);f.Add(10,values[0]);const auto shapeCount=f.history.UndoCount();
        SetDlgItemTextW(f.editor,1049,L"0.1");SetDlgItemTextW(f.editor,1051,L"0.25");f.Click(applyButton);
        Check(f.history.UndoCount()==shapeCount+1,"actual curve slope fields commit through the shared journal");
        f.History(false);Check(f.history.UndoCount()==shapeCount,"Ctrl Z traverses a real curve handle-field edit");
    }
}
void CameraShapeDragAndBudget(){
    Fixture f(p::SharedHistory::RecordingMode::AllOperations,true);
    f.stage="CameraShapeDragAndBudget";
    const auto original=f.Read(curve_host::Kind::Camera);const auto capacity=mdl::KeyPoolCapacity(f.app->CameraKeys());
    const auto point=SendMessageW(f.editor,WM_APP+301,10,0);
    SendMessageW(GetDlgItem(f.editor,canvasId),WM_LBUTTONDBLCLK,MK_LBUTTON,point);
    Check(f.history.UndoCount()==1&&f.Has(curve_host::Kind::Camera,10),"real canvas native-preserving double click commits once");
    Check(mdl::KeyPoolCapacity(f.app->CameraKeys())>capacity,"actual curve insertion grows its managed native pool");
    auto inserted=f.Read(curve_host::Kind::Camera);
    Check(!inserted[0].preservedCameraIntervals.empty()&&inserted[0].track.keys.size()==3&&inserted[1].track.keys.size()==2,
          "shared native rows retain sparse authored peer lanes and exact camera metadata");
    const auto checkShape=[&](const auto& channels){for(int tick=0;tick<=160;++tick){const double frame=tick/8.0;
        Check(channels[0].sample(channels[0],frame).value==original[0].sample(original[0],frame).value,
              "real bound insertion keeps original native float values at fractional frames");}};
    checkShape(inserted);f.History(false);Check(f.history.UndoCount()==0&&!f.Has(curve_host::Kind::Camera,10),"shared undo removes only the inserted authored key");
    f.History(true);inserted=f.Read(curve_host::Kind::Camera);checkShape(inserted);
    {
        pmm_extension::Document document;curve_host::Capture(*f.app,document);
        Check(pmm_extension::Representable(document)&&pmm_extension::Find(document,"org.mikumikustudio.camera_intervals.v1"),
              "real bound edit produces a serializable separate exact-camera PMM record");
        auto restored=std::make_unique<MMDApp>();::new(static_cast<void*>(&restored->state))MMDAppState{};
        restored->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(mdl::KeyPoolCapacity(f.app->CameraKeys()));
        std::memcpy(restored->CameraKeys(),f.app->CameraKeys(),mdl::KeyPoolCapacity(f.app->CameraKeys())*sizeof(mdl::CameraKey));
        curve_host::Load(*restored,document);std::vector<ce::Channel> channels;std::wstring error;
        Check(curve_host::Read(*restored,curve_host::Kind::Camera,0,channels,error),"rehydrate PMM authoring from the actual bound edit on another application");
        checkShape(channels);curve_host::Reset(restored.get());mdl::KeyPoolDestroy(restored->CameraKeys());restored->CameraKeys()=nullptr;
        std::vector<ce::Channel> prepared;Check(curve_host::PrepareMotionChannels(inserted,prepared,error),"prepare VMD from actual bound exact-camera edit");
        curve_motion::Motion motion;motion.kind=curve_motion::Kind::Camera;
        for(const auto& channel:prepared)motion.tracks.push_back({channel.id,channel.track});
        std::vector<std::uint8_t> bytes;std::string detail;curve_motion::Motion decoded;
        Check(curve_motion::Encode(motion,bytes,detail)&&curve_motion::Decode(bytes.data(),bytes.size(),motion.kind,decoded,detail),
              "VMD transport accepts the bound editor's exact-camera export candidates");
        const auto x=std::find_if(decoded.tracks.begin(),decoded.tracks.end(),[](const auto& channel){return channel.id=="x";});
        Check(x!=decoded.tracks.end(),"bound camera VMD retains X track");
        for(int frame=0;frame<=20;++frame)Check(ac::Evaluate(x->track,frame).value==original[0].sample(original[0],frame).value,
              "bound-camera VMD exact extension retains every original native integer value");
        Check(f.history.UndoCount()==1&&f.history.RedoCount()==0,"PMM capture and VMD encode do not append common history");
    }
    const auto redoCount=f.history.RedoCount(),undoCount=f.history.UndoCount();
    // Real key drag cancellation cannot turn its preview into a native write.
    const auto key=SendMessageW(f.editor,WM_APP+300,1,0);const auto target=MAKELPARAM(LOWORD(key)+20,HIWORD(key)-10);
    auto canvas=GetDlgItem(f.editor,canvasId);SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,key);
    SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,target);SendMessageW(canvas,WM_KEYDOWN,VK_ESCAPE,1);
    Check(!GetCapture()&&f.history.UndoCount()==undoCount&&f.history.RedoCount()==redoCount,"Esc cancels a real curve preview without a shared node");checkShape(f.Read(curve_host::Kind::Camera));
    // The final mouse-up must release capture before invoking the bound bridge.
    SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,key);SendMessageW(canvas,WM_MOUSEMOVE,MK_LBUTTON,target);
    SendMessageW(canvas,WM_LBUTTONUP,0,target);
    Check(!GetCapture()&&f.history.UndoCount()==undoCount+1,"real curve drag publishes once after releasing mouse capture");
    f.History(false);checkShape(f.Read(curve_host::Kind::Camera));
    // Lowering the configured budget may evict old nodes. The subsequent
    // rejected write must leave the remaining cursor and exact curve intact.
    Check(f.history.Configure(f.history.Mode(),128,1)==MMS_OK,"set a deliberately insufficient native history budget");
    const auto count=f.history.UndoCount(),redo=f.history.RedoCount();f.Add(15,5);
    Check(f.history.UndoCount()==count&&f.history.RedoCount()==redo&&!f.Has(curve_host::Kind::Camera,15),"window write refuses insufficient history budget without authoring or pruning redo");
    checkShape(f.Read(curve_host::Kind::Camera));
}
// Keep the actual native registrar between two edits of the same camera.
// A second curve write must neither invalidate the older native node nor make
// an older snapshot erase the later node when the common journal traverses it.
struct MixedCameraCheckpoint {
    std::vector<ce::Channel> channels;
    std::vector<mdl::CameraKey> native;
    pmm_extension::Document authoring;
    std::uint32_t last=0;
    explicit MixedCameraCheckpoint(Fixture& f):channels(f.Read(curve_host::Kind::Camera)),last(f.app->LastRegisteredFrame()){
        const auto* keys=f.app->CameraKeys();unsigned slot=0;
        for(std::size_t hops=0;;++hops){
            Check(slot<mdl::KeyPoolCapacity(keys)&&hops<mdl::KeyPoolCapacity(keys),"mixed camera snapshot follows a bounded native chain");
            auto key=keys[slot];key.selected=0;native.push_back(key);
            if(!key.next)break;slot=key.next;
        }
        curve_host::Capture(*f.app,authoring);
    }
    void Match(Fixture& f,const char* direction,int step,int frame)const{
        const auto lastBeforeRead=f.app->LastRegisteredFrame();
        const MixedCameraCheckpoint current(f);
        const auto diagnose=[&](const char* reason){
            std::fprintf(stderr,"DIAG mixed camera reason=%s direction=%s step=%d seek=%d actual-playhead=%d undo=%zu redo=%zu\n"
                "  channels expected=%zu actual=%zu native-rows expected=%zu actual=%zu LastRegisteredFrame expected=%u before-read=%u snapshot=%u after-capture=%u pool-capacity=%zu\n",
                reason,direction,step,frame,f.app->CurrentFrame(),f.history.UndoCount(),f.history.RedoCount(),channels.size(),current.channels.size(),
                native.size(),current.native.size(),unsigned(last),unsigned(lastBeforeRead),unsigned(current.last),unsigned(f.app->LastRegisteredFrame()),
                std::size_t(mdl::KeyPoolCapacity(f.app->CameraKeys())));
            f.DiagnoseHistory();
            const auto rows=[](const char* label,const auto& keys){std::fprintf(stderr,"  %s native frame(previous,next):",label);
                for(const auto& key:keys)std::fprintf(stderr," %u(%u,%u)",unsigned(key.frame),unsigned(key.previous),unsigned(key.next));std::fputc('\n',stderr);};
            rows("expected",native);rows("actual",current.native);
            const auto lanes=[](const char* label,const auto& source){for(const auto& lane:source){std::fprintf(stderr,"  %s lane=%s count=%zu frames:",label,lane.id.c_str(),lane.track.keys.size());
                for(const auto& key:lane.track.keys)std::fprintf(stderr," %d",int(key.frame));std::fprintf(stderr," raw=%zu geometry=%zu preserved=%zu\n",lane.nativeControls.size(),lane.controlGeometry.size(),lane.preservedCameraIntervals.size());}};
            lanes("expected",channels);lanes("actual",current.channels);
        };
        if(current.channels.size()!=channels.size()||current.native.size()!=native.size()||current.last!=last)diagnose("lane/native/horizon count");
        Check(current.channels.size()==channels.size()&&current.native.size()==native.size()&&current.last==last,
              "mixed curve/native traversal restores authored lane count, native rows and frame horizon");
        for(std::size_t i=0;i<channels.size();++i){const auto& a=channels[i];const auto& b=current.channels[i];
            const bool exact=a.id==b.id&&Same(a.track,b.track)&&a.base==b.base&&a.minimum==b.minimum&&a.maximum==b.maximum&&
                  a.interpolationGroup==b.interpolationGroup&&a.nativeControls==b.nativeControls&&a.controlGeometry==b.controlGeometry&&
                  a.preservedCameraIntervals==b.preservedCameraIntervals&&bool(a.sample)==bool(b.sample)&&a.sample.target_type()==b.sample.target_type();
            if(!exact){diagnose("author lane/metadata");std::fprintf(stderr,"  mismatch lane=%zu id expected=%s actual=%s\n",i,a.id.c_str(),b.id.c_str());}
            Check(exact,
                  "mixed history restores exact sparse authors, raw-control metadata and native sampler identity");
            for(int tick=0;tick<=240;++tick){const double frame=tick/8.0;
                const auto left=a.sample?a.sample(a,frame):ac::Evaluate(a.track,frame);
                const auto right=b.sample?b.sample(b,frame):ac::Evaluate(b.track,frame);
                if(left.value!=right.value||left.derivative!=right.derivative){diagnose("fractional sample");
                    std::fprintf(stderr,"  lane=%s sample-frame=%.17g expected-value=%.17g actual-value=%.17g expected-derivative=%.17g actual-derivative=%.17g\n",
                        a.id.c_str(),frame,left.value,right.value,left.derivative,right.derivative);}
                Check(left.value==right.value&&left.derivative==right.derivative,"mixed history restores exact fractional camera samples");}
        }
        for(std::size_t i=0;i<native.size();++i){const bool exact=!std::memcmp(&native[i],&current.native[i],sizeof(mdl::CameraKey));
            if(!exact){diagnose("native raw bytes");std::fprintf(stderr,"  native-row=%zu frame=%u\n",i,unsigned(native[i].frame));}
            Check(exact,"mixed history round-trips native camera values, raw interpolation and chain links byte for byte apart from selection");}
        if(authoring.records.size()!=current.authoring.records.size())diagnose("serialized authoring record count");
        Check(authoring.records.size()==current.authoring.records.size(),"mixed history restores the same serialized authoring records");
        for(std::size_t i=0;i<authoring.records.size();++i){const bool exact=authoring.records[i].key==current.authoring.records[i].key&&
            authoring.records[i].value==current.authoring.records[i].value;
            if(!exact){diagnose("serialized authoring bytes");std::fprintf(stderr,"  record=%zu expected-key=%s actual-key=%s expected-bytes=%zu actual-bytes=%zu\n",
                i,authoring.records[i].key.c_str(),current.authoring.records[i].key.c_str(),authoring.records[i].value.size(),current.authoring.records[i].value.size());}
            Check(exact,"mixed history restores exact PMM authoring and camera metadata bytes");}
    }
    float Position(int lane,int frame)const{
        const auto& channel=channels[lane];return float(channel.sample?channel.sample(channel,frame).value:ac::Evaluate(channel.track,frame).value);
    }
};
void MixedNativeRegistration(){
    Fixture f(p::SharedHistory::RecordingMode::AllOperations,true);const MixedCameraCheckpoint pristine(f);
    f.stage="MixedNativeRegistration";
    const auto seek=[&](int frame){SetDlgItemTextW(f.editor,1024,std::to_wstring(frame).c_str());f.Click(1025);
        Check(f.app->CurrentFrame()==frame,"mixed history changes time through the real curve transport");};
    f.Select(0,0);f.Add(10,4);const MixedCameraCheckpoint x(f);
    Check(f.history.UndoCount()==1,"first real camera X edit records one common node");
    seek(12);f.app->CameraPositionAxis(0)=6.5f;f.app->CameraPositionAxis(1)=-3.25f;f.app->CameraPositionAxis(2)=2.5f;
    f.app->LightDirection()[0]=67.125f;
    Check(p::TryRegisterBoundCamera(f.app.get(),f.app->MainWindow())&&f.history.UndoCount()==2,
          "real native camera registrar records between the two window curve edits");
    const MixedCameraCheckpoint registered(f);
    Check(std::all_of(registered.channels.begin(),registered.channels.end(),[](const auto& channel){
        return std::any_of(channel.track.keys.begin(),channel.track.keys.end(),[](const auto& key){return key.frame==12;});}),
        "native explicit registration authors frame twelve on every camera lane");
    const auto nativeKey=std::find_if(registered.native.begin(),registered.native.end(),[](const auto& key){return key.frame==12;});
    Check(nativeKey!=registered.native.end()&&nativeKey->eye[0]==6.5f&&nativeKey->eye[1]==-3.25f&&nativeKey->eye[2]==2.5f&&
          f.app->LightDirection()[0]==67.125f,"native record stores the real live camera pose without overwriting another family's unregistered pose");
    f.Select(0,1);seek(16);f.app->CameraPositionAxis(0)=91.5f;f.app->CameraPositionAxis(2)=71.25f;
    f.app->LightDirection()[0]=68.125f;f.Add(30,-8);const MixedCameraCheckpoint y(f);
    Check(f.history.UndoCount()==3&&f.app->CameraPositionAxis(0)==91.5f&&f.app->CameraPositionAxis(2)==71.25f&&
          f.app->LightDirection()[0]==68.125f,"later Y edit owns only its lane and leaves camera and foreign-family peer poses unregistered");
    const MixedCameraCheckpoint* states[]={&pristine,&x,&registered,&y};
    // Different frames before the second and third undo/redo catch records
    // that accidentally retain their initial publication frame or cursor.
    const int undoFrames[]={5,17,3},redoFrames[]={9,23,14};
    for(int step=0;step<3;++step){const int frame=undoFrames[step];seek(frame);
        const bool curve=step!=1;const int owned=step==0?1:0,peer=owned==0?1:0;
        const float peerValue=110.25f+step,zValue=210.5f+step,foreign=310.75f+step;
        if(curve){f.app->CameraPositionAxis(peer)=peerValue;f.app->CameraPositionAxis(2)=zValue;}
        f.app->LightDirection()[0]=foreign;f.History(false);const auto& expected=*states[2-step];expected.Match(f,"undo",step+1,frame);
        Check(f.history.UndoCount()==std::size_t(2-step)&&f.history.RedoCount()==std::size_t(step+1)&&f.app->CurrentFrame()==frame&&
              f.app->LightDirection()[0]==foreign,"three real common undos retain the current time and another family's unregistered pose");
        if(curve)Check(f.app->CameraPositionAxis(owned)==expected.Position(owned,frame)&&f.app->CameraPositionAxis(peer)==peerValue&&
            f.app->CameraPositionAxis(2)==zValue,"curve undo samples only its owned axis at the current frame and preserves unregistered peers");
        else for(int lane=0;lane<3;++lane)Check(f.app->CameraPositionAxis(lane)==expected.Position(lane,frame),
            "native registration undo samples its restored camera authors at the newly selected time");
    }
    for(int step=0;step<3;++step){const int frame=redoFrames[step];seek(frame);
        const bool curve=step!=1;const int owned=step==0?0:1,peer=owned==0?1:0;
        const float peerValue=410.25f+step,zValue=510.5f+step,foreign=610.75f+step;
        if(curve){f.app->CameraPositionAxis(peer)=peerValue;f.app->CameraPositionAxis(2)=zValue;}
        f.app->LightDirection()[0]=foreign;f.History(true);const auto& expected=*states[step+1];expected.Match(f,"redo",step+1,frame);
        Check(f.history.UndoCount()==std::size_t(step+1)&&f.history.RedoCount()==std::size_t(2-step)&&f.app->CurrentFrame()==frame&&
              f.app->LightDirection()[0]==foreign,"three real common redos retain the current time and another family's unregistered pose");
        if(curve)Check(f.app->CameraPositionAxis(owned)==expected.Position(owned,frame)&&f.app->CameraPositionAxis(peer)==peerValue&&
            f.app->CameraPositionAxis(2)==zValue,"curve redo samples only its owned axis at the current frame and preserves unregistered peers");
        else for(int lane=0;lane<3;++lane)Check(f.app->CameraPositionAxis(lane)==expected.Position(lane,frame),
            "native registration redo samples its restored camera authors at the newly selected time");
    }
}
struct ReadoutReentry {
    Fixture& fixture;std::vector<ce::Channel> channels;bool observed=false,locked=false,accepted=true;
    static LRESULT CALLBACK Window(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR context){
        auto& probe=*reinterpret_cast<ReadoutReentry*>(context);
        if(message==WM_SETTEXT&&!probe.observed){
            probe.observed=true;probe.locked=bake::UiLocked();std::wstring error;
            probe.accepted=p::WriteBoundGlobalCurves(*probe.fixture.app,curve_host::Kind::Light,0,probe.channels,error);
        }
        return DefSubclassProc(window,message,wp,lp);
    }
    explicit ReadoutReentry(Fixture& f):fixture(f),channels(f.Read(curve_host::Kind::Light)){
        channels[0].track.keys[0].value=3;
        Check(SetWindowSubclass(GetDlgItem(f.app->MainWindow(),panel::kReadoutPosXEdit),Window,710,reinterpret_cast<DWORD_PTR>(this))!=FALSE,
              "install native readout reentry observation");
    }
    ~ReadoutReentry(){RemoveWindowSubclass(GetDlgItem(fixture.app->MainWindow(),panel::kReadoutPosXEdit),Window,710);}
};
void ReadoutPublicationGate(){
    Fixture f;f.stage="ReadoutPublicationGate";ReadoutReentry probe(f);f.Add(10,4);
    Check(probe.observed&&probe.locked&&!probe.accepted&&f.history.UndoCount()==1,
          "actual post-commit WM_SETTEXT keeps the input gate and refuses a nested native curve write");
    Check(f.Read(curve_host::Kind::Light)[0].track.keys[0].value==0,"readout callback cannot modify an unrelated source");
}
void Run(){FiveSourcesAndPlugin();CameraShapeDragAndBudget();MixedNativeRegistration();ReadoutPublicationGate();}
} // namespace bound_curve_history_checks
