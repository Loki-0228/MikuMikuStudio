// Include after native_camera_follow_checks.inc and shared object/plugin helpers.
#include "../src/plugins/native_camera_parent_history.hpp"
#include "mikumikustudio/bone_combo.hpp"
#include "mikumikustudio/autosave.hpp"
#include "mikumikustudio/save_compatibility.hpp"
#include "mikumikustudio/pmm_extension.hpp"
#include "mikumikustudio/d3d_wrapper.hpp"
#include <MinHook.h>
#include <functional>
#include <fcntl.h>
#include <io.h>
namespace camera_parent_checks {
using namespace plugins;
using Mode = SharedHistory::RecordingMode;
struct Scene : NumpadFixture {
    std::array<mdl::BoneRecord,4> bones{};
    std::array<unsigned char,4> dirty{}, selected{};
    HWND models = nullptr, combo = nullptr;
    unsigned notifications = 0,captureNotifications=0;
    HWND notifiedCapture = nullptr;
    bool forward = false;
    static LRESULT CALLBACK Window(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto& s=*reinterpret_cast<Scene*>(data);
        if(message==WM_COMMAND&&LOWORD(w)==450&&HIWORD(w)==CBN_SELCHANGE&&s.forward){
            ++s.notifications;if(const auto capture=GetCapture()){++s.captureNotifications;s.notifiedCapture=capture;}
            CommandDispatch(reinterpret_cast<HWND>(l),w);return 0;}
        return DefSubclassProc(window,message,w,l);
    }
    Scene() {
        first.m->comboSelIndex=1;second.m->comboSelIndex=2;
        first.m->boneTable=bones.data();first.m->boneCount=4;first.m->bonePhysicsState=dirty.data();first.m->boneSelection=selected.data();
        for(unsigned i=0;i<bones.size();++i){auto& b=bones[i];b.parent=-1;b.rotQuat[3]=1;b.type=mdl::BoneType::RotateMove;
            strcpy_s(b.name,i==3?"last":"duplicate");for(unsigned n=0;n<4;++n)b.matInit[n*5]=1;
            b.position[0]=float(i)+1.25f;b.position[1]=float(i)*2.5f;b.matInit[12]=float(i)*11.125f;
            b.trans[0]=float(i)+.371f;}
        bones[1].type=mdl::BoneType::InertTip;bones[3].type=mdl::BoneType::FixedAxis;
        first.m->selectedBone=0;selected[0]=1;
        for(int id:{449,450}){const auto c=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,id==449?0:26,190,150,
            app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            Check(c!=nullptr,"create actual camera parent model and filtered bone combos");(id==449?models:combo)=c;}
        Check(SetWindowSubclass(app->MainWindow(),Window,151,reinterpret_cast<DWORD_PTR>(this)),"observe native ComboLBox notifications without global input");
        app->CameraParentModel()=0;app->CameraParentBone()=0;app->CameraMode()=1;app->EnglishUI()=1;
        app->state.timeNowHigh=0x72436815;app->PhysicsResetPending()=app->SceneModified()=app->RecordedFrameCountByte1()=0;
        Lists();
    }
    ~Scene(){ReleaseCapture();history.Reset();history.Collect();RemoveWindowSubclass(app->MainWindow(),Window,151);
        first.m->boneTable=first.bones.data();first.m->boneCount=2;first.m->bonePhysicsState=first.dirty.data();first.m->boneSelection=first.selected.data();}
    void Lists(){SendMessageW(models,CB_RESETCONTENT,0,0);
        for(const auto& row:{std::pair<const wchar_t*,int>{L"non",-1},{L"model A",0},{L"model B",1}}){const auto index=SendMessageW(models,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.first));SendMessageW(models,CB_SETITEMDATA,index,row.second);}
        SendMessageW(models,CB_SETCURSEL,1,0);FillBoneCombo(combo,first.m.get(),true);SelectBoneCombo(combo,app->CameraParentBone());}
    int Selection()const{const auto row=SendMessageW(combo,CB_GETCURSEL,0,0);return row<0?-1:int(SendMessageW(combo,CB_GETITEMDATA,row,0));}
    void Choose(int bone){SelectBoneCombo(combo,bone);auto* old=g_Block;g_Block=app.get();CommandDispatch(combo,MAKEWPARAM(450,CBN_SELCHANGE));g_Block=old;}
    std::shared_ptr<HistoryChange> Plan(int bone){std::shared_ptr<HistoryChange> change;
        Check(PlanCameraParentBone(*app,bone,change)==MMS_OK&&change,"prepare only the current camera parent bone privately");return change;}
};
struct Source {
    Scene& f;NumpadView view;
    std::array<mdl::BoneRecord,4> bones;
    std::array<mdl::BoneRecord,2> other;
    std::array<unsigned char,4> dirty,selected;
    std::vector<mdl::CameraKey> keys;
    std::uint64_t ring,otherRing;
    const int frame;const std::uint32_t clock;
    const unsigned char reset,follow,camera;
    const unsigned slot;
    const CameraAttachmentReference reference;
    const int firstBone,secondBone;
    explicit Source(Scene& s):f(s),view(NumpadRead(*s.app)),bones(s.bones),other(s.second.bones),dirty(s.dirty),selected(s.selected),
        keys(GlobalRegistrationKeys(s.app->CameraKeys())),ring(PoseRingDigest(s.first)),otherRing(PoseRingDigest(s.second)),
        frame(s.app->CurrentFrame()),clock(s.app->state.timeNowHigh),reset(s.app->PhysicsResetPending()),follow(s.app->state.followCameraEnabled),camera(s.app->CameraMode()),
        slot(s.app->SelectedModelSlot()),reference(s.app->CameraReferenceMode()),firstBone(s.first.m->selectedBone),secondBone(s.second.m->selectedBone){}
    void Unchanged(int bone,int currentFrame=-1)const{auto expected=view;expected.parentBone=bone;
        Check(NumpadSame(NumpadRead(*f.app),expected)&&!std::memcmp(bones.data(),f.bones.data(),sizeof bones)&&
            !std::memcmp(other.data(),f.second.bones.data(),sizeof other)&&dirty==f.dirty&&selected==f.selected&&
            GlobalRegistrationSame(f.app->CameraKeys(),keys)&&PoseRingDigest(f.first)==ring&&PoseRingDigest(f.second)==otherRing&&
            f.app->CurrentFrame()==(currentFrame<0?frame:currentFrame)&&f.app->state.timeNowHigh==clock&&
            f.app->PhysicsResetPending()==reset&&f.app->state.followCameraEnabled==follow&&!f.app->cameraProjectionRefreshPending&&
            f.app->CameraMode()==camera&&f.app->SelectedModelSlot()==slot&&f.app->CameraReferenceMode()==reference&&
            f.first.m->selectedBone==firstBone&&f.second.m->selectedBone==secondBone,
            "parent index owns no pose, key, camera projection/basis/offset, clock or physics reset");}
};
void ModesAndRealHistory() {
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(bool camera:{false,true}){
        Scene f;NativePoseTestBinding binding(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"set parent adjustment history mode");
        f.app->CameraMode()=camera;f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);f.accessory.rowSelected=1;Source before(f);
        const auto draft=FollowPendingEdit(*f.app,panel::kReadoutPosXEdit);
        f.Choose(2);
        Check(f.app->CameraParentBone()==2&&f.Selection()==2&&f.history.UndoCount()==(mode==Mode::AllOperations?1:0)&&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera)&&!f.accessory.rowSelected&&FollowHasPendingEdit(draft),
            "real 450 notification uses filtered item data and one adjustment, preserving numeric drafts");before.Unchanged(2);
        Check(f.app->SceneModified()&&f.app->RecordedFrameCountByte1()&&!f.history.LastPublicationRequiresPhysicsReset(),"parent publication marks authored state without physics reset");
        if(mode==Mode::AllOperations){
            f.app->CurrentFrame()=53;f.app->SelectedModelSlot()=1;f.second.bones[1].trans[2]=17.25f;
            f.app->CameraPositionX()=39.75f;f.app->CameraFov()=58.125f;Source later(f);
            f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);f.accessory.rowSelected=1;
            Check(TryBoundHistoryCommand(f.app.get(),false)&&f.Selection()==0&&f.app->CameraParentBone()==0,
                "actual common undo restores the canonical item data in camera and model modes");later.Unchanged(0);
            Check(f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&!f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera)&&f.accessory.rowSelected&&FollowHasPendingEdit(draft),
                "undo readout never repeats initial camera-track selection or overwrites a pending draft");
            Check(TryBoundHistoryCommand(f.app.get(),true)&&f.Selection()==2&&FollowHasPendingEdit(draft),"common redo synchronizes 450 without registering or seeking");later.Unchanged(2);
            Check(f.history.History(false)==MMS_OK&&f.history.RedoCount()==1,"seed parent redo before same-value action");
            f.app->SceneModified()=f.app->RecordedFrameCountByte1()=0;f.Choose(0);
            Check(!f.history.UndoCount()&&f.history.RedoCount()==1&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1(),"same-value accepted parent selection retains redo and dirty flags");
        }
    }
}
void FocusAndMapping() {
    Scene f;NativePoseTestBinding binding(*f.app,f.history);Source before(f);
    Check(SendMessageW(f.combo,CB_GETCOUNT,0,0)==3&&SendMessageW(f.combo,CB_GETITEMDATA,1,0)==2,
        "filtered bone rows have noncontiguous indices and duplicate labels");
    float initial[3]{},after[3]{};Check(ResolveFrameFocus(f.app.get(),0,initial)==FrameFocusSource::AttachmentBone,"renderer resolves original parent from current bone matrix");
    f.Choose(2);Check(ResolveFrameFocus(f.app.get(),0,after)==FrameFocusSource::AttachmentBone&&after[0]==f.bones[2].position[0]+f.bones[2].matInit[12]&&after[0]!=initial[0],
        "renderer focus changes to the live new bone rather than compensating the camera eye");before.Unchanged(2);
    f.bones[2].matInit[12]+=19.25f;Check(ResolveFrameFocus(f.app.get(),0,after)==FrameFocusSource::AttachmentBone&&after[0]==f.bones[2].position[0]+f.bones[2].matInit[12],
        "subsequent live unregistered parent matrices remain authoritative without a sampled pose snapshot");
    // A valid permutation remains addressed by item data, never by its row.
    SendMessageW(f.combo,CB_RESETCONTENT,0,0);for(int index:{3,0,2}){const auto name=text_encoding::BoneLabel(f.bones[index],true);
        const auto row=SendMessageW(f.combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(f.combo,CB_SETITEMDATA,row,index);}
    f.Choose(3);Check(f.app->CameraParentBone()==3&&f.Selection()==3&&f.history.UndoCount()==2,"valid reordered bone rows retain exact item-data authoring");
    Check(f.history.History(false)==MMS_OK,"undo before stale list repair");SendMessageW(f.combo,CB_SETITEMDATA,0,2);
    RefreshBoundCameraFollowReadout(*f.app);
    Check(f.Selection()==2&&SendMessageW(f.combo,CB_GETITEMDATA,2,0)==3,"common readout repairs duplicate/stale item data before selecting canonical index");
    const auto nodes=f.history.UndoCount(),redo=f.history.RedoCount();
    for(unsigned corruption=0;corruption<6;++corruption){f.Lists();f.app->CameraParentBone()=2;SelectBoneCombo(f.combo,3);
        if(corruption==0)SendMessageW(f.combo,CB_SETITEMDATA,2,2);
        if(corruption==1)SendMessageW(f.combo,CB_SETITEMDATA,2,1);
        if(corruption==2)SendMessageW(f.combo,CB_SETITEMDATA,2,-1);
        if(corruption==3)SendMessageW(f.models,CB_SETCURSEL,2,0);
        if(corruption==4)SendMessageW(f.models,CB_SETITEMDATA,2,0);
        if(corruption==5)SendMessageW(f.combo,CB_SETCURSEL,-1,0);
        f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);f.accessory.rowSelected=1;
        CommandDispatch(f.combo,MAKEWPARAM(450,CBN_SELCHANGE));
        Check(f.app->CameraParentBone()==2&&f.history.UndoCount()==nodes&&f.history.RedoCount()==redo&&f.Selection()==2&&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&f.accessory.rowSelected,
            "invalid/duplicate/filtered bone and stale parent-model mapping consume 450 without legacy fallback or track selection");
    }
}
void CandidatesOwnersAndBudgets() {
    for(unsigned mutation=0;mutation<8;++mutation){Scene f;NativePoseTestBinding binding(*f.app,f.history);auto change=f.Plan(2);std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK&&candidate->IsCurrent(),"prepare a short parent candidate against exact live source");
        const auto oldCount=f.first.m->boneCount;auto* oldTable=f.first.m->boneTable;
        if(mutation==0)++f.app->CurrentFrame();if(mutation==1)f.app->CameraParentBone()=3;
        if(mutation==2){NativePoseTestBinding nested(*f.app,f.history);}
        if(mutation==3)f.app->CameraParentModel()=1;
        if(mutation==4)f.first.m->boneCount=3;
        if(mutation==5)f.first.m->boneTable=f.first.bones.data();
        if(mutation==6)f.app->AdvanceSceneGeneration();
        if(mutation==7){auto owner=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);const auto id=owner->Id();
            auto remove=owner->PreparePresence(false);Check(remove->IsCurrent(),"prepare real retained parent removal");remove->Publish();remove.reset();
            Check(!candidate->IsCurrent()&&!f.app->ModelSlot(0),"absent owner rejects old candidate before old table access");
            auto restore=owner->PreparePresence(true);Check(restore->IsCurrent(),"prepare exact retained parent restoration");restore->Publish();restore.reset();
            Check(SceneObjectPresent(f.first.raw(),id),"retained restoration keeps the original stable identity");}
        Check(!candidate->IsCurrent()&&!f.history.UndoCount(),"frame/index/binding/parent/table/count/scene or delete-restore expires a short candidate");
        candidate.reset();f.first.m->boneCount=oldCount;f.first.m->boneTable=oldTable;
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Choose(2);auto owner=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);
        auto park=owner->PreparePresence(false);Check(park->IsCurrent(),"park current parent after a committed index edit");park->Publish();park.reset();
        Check(f.history.History(false)==MMS_E_STALE&&f.app->CameraParentBone()==2,"absent parent blocks history before dereferencing retained model");
        auto restore=owner->PreparePresence(true);Check(restore->IsCurrent(),"restore parent before long-term index replay");restore->Publish();restore.reset();
        Check(f.history.History(false)==MMS_OK&&f.app->CameraParentBone()==0&&f.history.History(true)==MMS_OK&&f.app->CameraParentBone()==2,
            "new replay candidates survive legitimate same-owner delete and restore runtime generation changes");
        const auto cap=mdl::KeyPoolCapacity(f.app->CameraKeys());auto* grown=mdl::KeyPoolCreate<mdl::CameraKey>(cap+9);
        std::memcpy(grown,f.app->CameraKeys(),cap*sizeof(mdl::CameraKey));mdl::KeyPoolCommitGrowth(f.app->CameraKeys(),grown);
        Check(f.history.History(false)==MMS_OK&&f.history.History(true)==MMS_OK,"logical camera pool growth preserves parent history identity");
        mdl::KeyPoolDestroy(f.app->CameraKeys());f.app->CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(cap+9);
        Check(f.history.History(false)==MMS_E_STALE&&f.app->CameraParentBone()==2&&f.history.UndoCount()==1,"replaced camera root cannot inherit old index history");
    }
    for(unsigned replacement=0;replacement<4;++replacement){Scene f;NativePoseTestBinding binding(*f.app,f.history);f.Choose(2);auto* table=f.first.m->boneTable;
        if(replacement==0){ForgetSceneObject(f.first.raw());SceneObjectIdentity(f.first.raw());}
        if(replacement==1)f.app->ModelSlot(0)=f.second.raw();
        if(replacement==2)f.first.m->boneTable=f.first.bones.data();
        if(replacement==3)f.first.m->boneCount=3;
        Check(f.history.History(false)==MMS_E_STALE&&f.app->CameraParentBone()==2&&f.history.UndoCount()==1,"replacement owner, membership, bone storage or count refuses long-term replay");
        f.app->ModelSlot(0)=f.first.raw();f.first.m->boneTable=table;f.first.m->boneCount=4;
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);auto change=f.Plan(2);std::unique_ptr<PreparedHistoryChange> candidate;
        Check(change->Prepare(true,candidate)==MMS_OK,"measure the real retained parent ledger");std::uint64_t target=0;
        Check(candidate->TargetRetainedBytes(target)&&target>change->RetainedBytes(),"parent budget includes actual owner bookkeeping rather than four-byte estimate");candidate.reset();change.reset();
        Check(f.history.Configure(Mode::AllOperations,128,target-1)==MMS_OK,"set budget below complete owner charge");Source before(f);f.Choose(2);
        Check(f.app->CameraParentBone()==0&&!f.history.UndoCount()&&f.Selection()==0,"budget rejection restores control without partial scalar publication");before.Unchanged(0);
        Check(f.history.Configure(Mode::AllOperations,128,target+1024*1024)==MMS_OK,"raise budget to fit owner payload");f.Choose(2);
        Check(f.app->CameraParentBone()==2&&f.history.UndoCount()==1,"the same operation succeeds with complete retained-resource budget");
    }
}
void AllocationAndCancellation() {
    bool complete=false;
    for(int failure=0;failure<512;++failure){Scene f;NativePoseTestBinding binding(*f.app,f.history);Source before(f);
        SelectBoneCombo(f.combo,2);failAfter=failure;CommandDispatch(f.combo,MAKEWPARAM(450,CBN_SELCHANGE));const auto remaining=failAfter;failAfter=-1;
        if(f.app->CameraParentBone()==2){Check(f.history.UndoCount()==1,"accepted UI notification faults never roll back or duplicate a committed index");before.Unchanged(2);
            if(remaining>0){complete=true;break;}continue;}
        Check(f.app->CameraParentBone()==0&&!f.history.UndoCount()&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&f.Selection()==0,
            "every allocation failure before acceptance preserves scalar, dirty flags, history and canonical selection");before.Unchanged(0);
        Check(failure!=511,"entire parent allocation sweep eventually reaches a successful actual command");
    }
    Check(complete,"allocation sweep covers accepted notification work and reaches one uninjected complete actual command");
    Scene f;NativePoseTestBinding binding(*f.app,f.history);auto change=f.Plan(2);Source before(f);
    for(unsigned i=0;i<8;++i){std::unique_ptr<PreparedHistoryChange> candidate;Check(change->Prepare(!(i%2),candidate)==MMS_OK&&candidate->IsCurrent(),"prepare alternating parent direction privately");
        failAfter=0;const bool current=candidate->IsCurrent();if(current)candidate->Publish();failAfter=-1;
        Check(current,"parent source ledger verification and final scalar publication allocate nothing");before.Unchanged(i%2?0:2);}
    auto journal=f.Plan(2);std::unique_ptr<SharedHistory::NativeTransaction> transaction;
    Check(f.history.PrepareNative("Camera parent bone",journal,transaction,SharedHistory::NativeIntent::Adjustment)==MMS_OK,"reserve actual parent transaction");transaction.reset();
    Check(f.app->CameraParentBone()==0&&!f.history.UndoCount(),"cancelling a prepared transaction does not change camera parent");
    Check(f.history.PrepareNative("Camera parent bone",journal,transaction,SharedHistory::NativeIntent::Adjustment)==MMS_OK,"reserve parent again after cancellation");
    failAfter=0;const auto result=f.history.CommitNative(std::move(transaction));failAfter=-1;
    Check(result==MMS_OK&&f.history.UndoCount()==1&&f.app->CameraParentBone()==2,"complete history publication performs no C++ allocation");
}
void BusyAndMixedPlugin() {
    Scene f;NativePoseTestBinding binding(*f.app,f.history);
    const auto commit=[&](unsigned slot,const char* text){f.app->SelectedModelSlot()=std::uint8_t(slot);f.app->CameraMode()=0;
        const auto edit=f.Edit(panel::kReadoutPosYEdit);const auto nodes=f.history.UndoCount();
        const auto admission=f.history.CheckNativeEdit();const float before=mdl::Mdl(f.app->ModelSlot(slot))->boneTable[0].trans[1];
        f.Commit(panel::kReadoutPosYEdit,text);
        const float after=mdl::Mdl(f.app->ModelSlot(slot))->boneTable[0].trans[1];
        if(f.history.UndoCount()!=nodes+1||after!=std::strtof(text,nullptr))std::printf("  parent model seed slot=%u admission=%d before=%g after=%g expected=%s nodes=%zu->%zu modified=%lld selectedBone=%d camera=%u capture=%p\n",
            slot,int(admission),double(before),double(after),text,nodes,f.history.UndoCount(),static_cast<long long>(SendMessageW(edit,EM_GETMODIFY,0,0)),mdl::Mdl(f.app->ModelSlot(slot))->selectedBone,unsigned(f.app->CameraMode()),GetCapture());
        Check(admission==MMS_OK&&after==std::strtof(text,nullptr)&&f.history.UndoCount()==nodes+1,"real user-modified model field commits its exact value as one chronological node");};
    commit(0,"3.25");Check(f.bones[0].trans[1]==3.25f&&f.history.UndoCount()==1,"seed model A pose before camera parent");f.app->CameraMode()=1;f.Choose(2);
    camera_multiply_checks::PluginState plugin;StorageClient storage;storage.id="test.camera-parent";f.storage.Api(storage);auto serializer=plugin.Serializer();MmsHandle provider=0;
    Check(f.storage.Register(storage,Text(storage.id),&serializer,sizeof serializer,&provider)==MMS_OK,"register actual camera-parent mixed serializer");
    EditStore edits(f.history,f.storage);EditClient client;client.id=storage.id;const auto api=edits.Api(client);std::uint64_t revision=0,next=0;MmsHandle tx=0;
    Check(f.history.Revision(revision)==MMS_OK&&api.begin(api.context,Text("parent plugin"),revision,&tx)==MMS_OK,"start real plugin pending transaction");
    f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);f.accessory.rowSelected=1;const auto count=f.history.UndoCount();f.Choose(3);
    Check(f.app->CameraParentBone()==2&&f.Selection()==2&&f.history.UndoCount()==count&&edits.HasPending()&&
        f.history.Revision(next)==MMS_OK&&next==revision&&f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&f.accessory.rowSelected,
        "pending plugin transaction consumes 450 without fallback, selection change or canceling somebody else's transaction");
    const auto participant=plugin.Participant(Text(client.id));Check(api.join(api.context,tx,&participant)==MMS_OK&&api.prepare(api.context,tx)==MMS_OK&&api.commit(api.context,tx)==MMS_OK,
        "the same pending plugin transaction remains valid and commits after refusal");
    commit(1,"8.5");f.app->CameraMode()=1;f.Choose(3);Check(f.history.UndoCount()==5&&plugin.value==42,"two model edits, plugin and two parent edits share chronology");
    f.bones[3].trans[2]=19.25f;f.app->CurrentFrame()=79;
    Check(TryBoundHistoryCommand(f.app.get(),false)&&f.app->CameraParentBone()==2&&f.Selection()==2&&f.bones[3].trans[2]==19.25f,
        "common history first undoes only the latest parent while preserving unregistered pose");
    Check(f.history.History(false)==MMS_OK&&f.second.bones[0].trans[1]==0&&f.history.History(false)==MMS_OK&&plugin.value==0&&
        f.history.History(false)==MMS_OK&&f.app->CameraParentBone()==0&&f.history.History(false)==MMS_OK&&f.bones[0].trans[1]==0,
        "mixed parent history unwinds model/plugin/index edits in exact reverse order");
    for(unsigned i=0;i<5;++i)Check(f.history.History(true)==MMS_OK,"redo one actual mixed camera-parent history node");
    Check(f.app->CameraParentBone()==3&&plugin.value==42&&f.bones[0].trans[1]==3.25f&&f.second.bones[0].trans[1]==8.5f&&f.bones[3].trans[2]==19.25f&&f.app->CurrentFrame()==79,
        "mixed redo needs no source file and preserves current clock and unregistered pose");
    Check(f.history.Reset()==MMS_OK,"release camera-parent records before plugin provider");f.history.Collect();Check(f.storage.Unregister(storage,provider)==MMS_OK,"unregister mixed serializer");
    for(unsigned busy=0;busy<4;++busy){Scene s;NativePoseTestBinding other(*s.app,s.history);s.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);s.accessory.rowSelected=1;
        if(busy==0)s.app->PlaybackActive()=1;if(busy==1)s.app->FrameStepPlayback()=1;if(busy==2)s.app->state.recordPlaybackStartPending=1;if(busy==3)SetCapture(s.Edit(panel::kReadoutPosXEdit));
        s.Choose(2);Check(s.app->CameraParentBone()==0&&!s.history.UndoCount()&&s.Selection()==0&&s.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&s.accessory.rowSelected,
            "busy playback and foreign capture consume 450 without immediate legacy write");ReleaseCapture();}
}
void ActualComboList() {
    Scene f;NativePoseTestBinding binding(*f.app,f.history);f.forward=true;
    SendMessageW(f.combo,CB_SHOWDROPDOWN,TRUE,0);COMBOBOXINFO info{sizeof(info)};
    Check(GetComboBoxInfo(f.combo,&info)&&info.hwndList&&IsWindow(info.hwndList),"retrieve actual native ComboLBox for parent selection");
    RECT item{};Check(SendMessageW(info.hwndList,LB_GETITEMRECT,1,reinterpret_cast<LPARAM>(&item))!=LB_ERR,"query filtered target row in real dropdown list");
    const auto point=MAKELPARAM((item.left+item.right)/2,(item.top+item.bottom)/2);
    SendMessageW(info.hwndList,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(info.hwndList,WM_LBUTTONUP,0,point);
    SendMessageW(f.combo,CB_SHOWDROPDOWN,FALSE,0);ReleaseCapture();
    std::printf("  parent ComboLBox notifications=%u capturedNotifications=%u capture=%p combo=%p list=%p\n",f.notifications,f.captureNotifications,f.notifiedCapture,f.combo,info.hwndList);
    Check(f.notifications&&f.app->CameraParentBone()==2&&f.history.UndoCount()==1&&f.Selection()==2,
        "real ComboLBox mouse notification commits exactly one parent edit");
    if(f.captureNotifications)Check(f.notifiedCapture&&(f.notifiedCapture==info.hwndList||f.notifiedCapture==f.combo),
        "captured real native notifications belong to this exact combo or ComboLBox");
    else std::puts("  ordinary ComboLBox notification arrived after native capture release; it does not prove capture admission");
    for(bool list:{false,true}){
        Scene held;NativePoseTestBinding heldBinding(*held.app,held.history);SelectBoneCombo(held.combo,2);
        // Keep the popup closed: taking capture away from an open native
        // dropdown sends WM_CAPTURECHANGED, whose cancellation may release
        // the newly assigned capture too. This separately tests the admission
        // gate with a real owned handle, not a mouse-open dropdown gesture.
        COMBOBOXINFO native{sizeof(native)};
        Check(GetComboBoxInfo(held.combo,&native)&&native.hwndList,"capture admission uses an actual native ComboLBox handle");
        const auto target=list?native.hwndList:held.combo;SetCapture(target);
        Check(GetCapture()==target&&GetCapture()!=nullptr,"explicit own-window native capture is nonnull at the controlled notification entry");
        CommandDispatch(held.combo,MAKEWPARAM(450,CBN_SELCHANGE));
        Check(held.app->CameraParentBone()==2&&held.history.UndoCount()==1&&held.Selection()==2&&
            GetPropW(held.combo,L"MMS.Native.CameraParentCapture.V1")==nullptr,
            "the exact native combo/list capture route commits once and removes its short authorization token");
        ReleaseCapture();SendMessageW(held.combo,CB_SHOWDROPDOWN,FALSE,0);
    }
}
struct UiProbe {
    Scene& f;HWND window;unsigned writes=0,textReads=0;bool failAdd=false;UINT trigger=0;
    std::function<void(UINT)> callback;
    static LRESULT CALLBACK Observe(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data){auto& p=*reinterpret_cast<UiProbe*>(data);
        if(message==CB_RESETCONTENT||message==CB_ADDSTRING||message==CB_INSERTSTRING||message==CB_DELETESTRING||message==CB_SETITEMDATA||message==CB_SETCURSEL)++p.writes;
        if(message==CB_GETLBTEXT)++p.textReads;
        if(p.callback&&(!p.trigger||p.trigger==message)){auto work=std::move(p.callback);work(message);}
        if(message==CB_ADDSTRING&&p.failAdd)return CB_ERRSPACE;
        return DefSubclassProc(window,message,w,l);}
    explicit UiProbe(Scene& source,HWND target=nullptr):f(source),window(target?target:source.combo){Check(SetWindowSubclass(window,Observe,152,reinterpret_cast<DWORD_PTR>(this)),"observe only this fixture's combo/native write calls");}
    ~UiProbe(){if(IsWindow(window))RemoveWindowSubclass(window,Observe,152);}
};
struct MemoryPanel {
    MMDApp& app;std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    D3DRenderer* previousRenderer;HDC previousDc,dc=nullptr;HBITMAP previousBitmap,previousSpare,bitmap=nullptr,spare=nullptr;
    HGDIOBJ previousObject=nullptr;void* pixels=nullptr;const int previousWidth;
    explicit MemoryPanel(MMDApp& value):app(value),previousRenderer(value.Renderer()),previousDc(value.PanelDC()),
        previousBitmap(value.PanelBitmap()),previousSpare(value.PanelSpareBitmap()),previousWidth(value.SidebarWidth()) {
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=240;info.bmiHeader.biHeight=-160;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        dc=CreateCompatibleDC(nullptr);bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);void* sparePixels=nullptr;
        spare=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&sparePixels,nullptr,0);
        Check(dc&&bitmap&&spare&&pixels&&sparePixels,"allocate real memory PanelDC and both 32-bit GDI bitmaps");
        std::memset(pixels,0,240*160*4);std::memset(sparePixels,0,240*160*4);previousObject=SelectObject(dc,bitmap);
        renderer->screenWidth=240;renderer->screenHeight=160;app.Renderer()=renderer.get();app.PanelDC()=dc;
        app.PanelBitmap()=bitmap;app.PanelSpareBitmap()=spare;app.SidebarWidth()=210;
        app.state.themeColors[33]=RGB(60,70,80);app.state.themeColors[34]=RGB(220,230,240);
        Check(!renderer->device&&!renderer->d3d9,"panel highlight fixture owns a real wrapper without a GPU device or fake COM resource");
    }
    ~MemoryPanel(){app.Renderer()=previousRenderer;app.PanelDC()=previousDc;app.PanelBitmap()=previousBitmap;app.PanelSpareBitmap()=previousSpare;app.SidebarWidth()=previousWidth;
        SelectObject(dc,previousObject);DeleteObject(bitmap);DeleteObject(spare);DeleteDC(dc);}
    std::uint64_t CameraPixels()const{GdiFlush();std::uint64_t digest=14695981039346656037ull;const auto* image=static_cast<const unsigned char*>(pixels);
        for(unsigned y=16;y<30;++y)for(unsigned x=0;x<91*4;++x){digest^=image[y*240*4+x];digest*=1099511628211ull;}return digest;}
};
void AcceptedPanelHighlight() {
    for(bool camera:{false,true}){
        Scene f;NativePoseTestBinding binding(*f.app,f.history);MemoryPanel paint(*f.app);
        f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);f.accessory.rowSelected=1;
        PostLanguageSweep(f.app.get());const auto pixels=paint.CameraPixels();
        Check(!f.app->PanelRowFlags()[0]&&f.app->PanelRowFlags()[1],"real pre-edit panel bitmap highlights the light track");
        f.app->CameraMode()=camera;
        if(!camera){f.first.m->selectedBone=3;f.selected[3]=0;f.selected[0]=1;}
        const auto draft=FollowPendingEdit(*f.app,panel::kReadoutPosXEdit);Source source(f);f.Choose(2);
        Check(f.app->CameraParentBone()==2&&f.history.UndoCount()==1&&FollowHasPendingEdit(draft),"initial accepted parent edit preserves draft around optional GDI panel painting");source.Unchanged(2);
        if(camera)Check(f.app->PanelRowFlags()[0]&&!f.app->PanelRowFlags()[1]&&paint.CameraPixels()!=pixels,
            "accepted camera-mode selection immediately repaints the actual PanelDC highlight without another input");
        else Check(!f.app->PanelRowFlags()[0]&&f.app->PanelRowFlags()[1]&&paint.CameraPixels()==pixels&&f.first.m->selectedBone==3,
            "model-mode acceptance skips the broad label sweep and preserves its unselected active-bone target");
        f.app->PanelRowFlags()[0]=7;f.app->PanelRowFlags()[1]=9;f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)=1;f.accessory.rowSelected=1;
        const auto repeatedPixels=paint.CameraPixels();f.Choose(3);
        Check(f.app->CameraParentBone()==3&&f.history.UndoCount()==2&&f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera)&&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&f.accessory.rowSelected&&f.app->PanelRowFlags()[0]==7&&f.app->PanelRowFlags()[1]==9&&paint.CameraPixels()==repeatedPixels,
            "a second accepted edit keeps the already-active camera multi-selection and does not run the initial highlighter twice");source.Unchanged(3);
        f.app->CameraMode()=1;
        Check(TryBoundHistoryCommand(f.app.get(),false)&&f.app->CameraParentBone()==2&&f.app->PanelRowFlags()[0]==7&&f.app->PanelRowFlags()[1]==9&&FollowHasPendingEdit(draft)&&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&f.accessory.rowSelected,
            "common undo remains a readout and never repeats initial track highlighter work");
        Check(TryBoundHistoryCommand(f.app.get(),false)&&f.app->CameraParentBone()==0&&f.app->PanelRowFlags()[0]==7&&f.app->PanelRowFlags()[1]==9&&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&f.accessory.rowSelected,
            "undo of the initial accepted action also preserves current track selection and the existing panel presentation");
    }
}
void CacheAndLifecycle() {
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);
        std::vector<mdl::BoneRecord> many(4000);std::vector<unsigned char> flags(4000),selection(4000);
        for(unsigned i=0;i<many.size();++i){many[i].parent=-1;many[i].rotQuat[3]=1;many[i].type=mdl::BoneType::RotateMove;
            strcpy_s(many[i].name,"long native bone");strcpy_s(many[i].nameEn,"long english bone");}
        f.first.m->boneTable=many.data();f.first.m->boneCount=unsigned(many.size());f.first.m->bonePhysicsState=flags.data();f.first.m->boneSelection=selection.data();
        f.app->CameraParentBone()=3071;RefreshBoundCameraFollowReadout(*f.app);
        Check(f.Selection()==3071&&SendMessageW(f.combo,CB_GETCOUNT,0,0)==4000,"warm common readout with a real 4000-bone nonempty parent list");
        UiProbe probe(f);constexpr long allowance=1000000;failAfter=allowance;
        for(unsigned i=0;i<12;++i)RefreshBoundCameraFollowReadout(*f.app);
        const auto allocations=allowance-failAfter;failAfter=-1;
        std::printf("  parent stable 4000 bones: refreshes=12 allocations=%ld writes=%u labelReads=%u\n",allocations,probe.writes,probe.textReads);
        Check(!allocations&&!probe.writes&&!probe.textReads&&f.Selection()==3071,"stable common parent readout performs zero C++ allocation, list writes or label-message reads");
        strcpy_s(many[7].nameEn,"changed source bone");RefreshBoundCameraFollowReadout(*f.app);
        Check(probe.writes&&f.Selection()==3071,"live name bytes invalidate the cached source even when indices and language stay unchanged");
        probe.writes=probe.textReads=0;SendMessageW(f.combo,CB_SETITEMDATA,31,7);probe.writes=0;RefreshBoundCameraFollowReadout(*f.app);
        Check(probe.writes&&f.Selection()==3071&&SendMessageW(f.combo,CB_GETITEMDATA,31,0)==31,"arbitrary native item-data mutation invalidates the window-owned cache");
        many[8].type=mdl::BoneType::InertTip;RefreshBoundCameraFollowReadout(*f.app);
        Check(SendMessageW(f.combo,CB_GETCOUNT,0,0)==3999&&f.Selection()==3071,"live filtering changes rebuild the same cache without row/index confusion");
        f.app->EnglishUI()=0;probe.writes=0;RefreshBoundCameraFollowReadout(*f.app);Check(probe.writes&&f.Selection()==3071,"language changes invalidate actual row labels");
        f.first.m->boneTable=f.bones.data();f.first.m->boneCount=4;f.first.m->bonePhysicsState=f.dirty.data();f.first.m->boneSelection=f.selected.data();f.app->CameraParentBone()=0;
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);RefreshBoundCameraFollowReadout(*f.app);
        f.app->CameraParentModel()=-1;RefreshBoundCameraFollowReadout(*f.app);
        Check(SendMessageW(f.combo,CB_GETCOUNT,0,0)==0&&f.Selection()==-1&&SendMessageW(f.models,CB_GETCURSEL,0,0)==0,
            "canonical no-parent readout clears the old source rather than showing a stale bone selection");
        f.app->CameraParentModel()=0;RefreshBoundCameraFollowReadout(*f.app);Check(f.Selection()==0,"restored parent readout rebuilds from canonical source");
        f.app->ModelSlot(0)=nullptr;RefreshBoundCameraFollowReadout(*f.app);
        Check(SendMessageW(f.combo,CB_GETCOUNT,0,0)==0&&SendMessageW(f.models,CB_GETCURSEL,0,0)==CB_ERR,"missing parent clears old list without dereferencing an old owner");f.app->ModelSlot(0)=f.first.raw();
        RefreshBoundCameraFollowReadout(*f.app);UiProbe probe(f);f.app->CameraParentBone()=2;
        probe.trigger=CB_SETCURSEL;probe.callback=[&](UINT){f.app->CameraParentBone()=3;};
        RefreshBoundCameraFollowReadout(*f.app);
        // The callback is one-shot; the native list must converge to the new
        // canonical scalar, never publish its old readout as authoritative.
        probe.callback={};RefreshBoundCameraFollowReadout(*f.app);
        Check(f.app->CameraParentBone()==3&&f.Selection()==3&&!f.history.UndoCount(),"native selection reentry cannot cache a stale scalar or create an edit");
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);RefreshBoundCameraFollowReadout(*f.app);UiProbe probe(f);
        strcpy_s(f.bones[2].name,"changed source");probe.failAdd=true;bool changed=false;
        Check(ApplyBoundCameraParentBone(*f.app,2,changed)==MMS_OK&&changed&&f.app->CameraParentBone()==2&&f.history.UndoCount()==1,
            "native list insertion failure does not turn an accepted scalar/history publication into failure");
        probe.failAdd=false;const auto start=GetTickCount();
        while(f.Selection()!=2&&GetTickCount()-start<500){MSG message{};if(PeekMessageW(&message,f.combo,WM_TIMER,WM_TIMER,PM_REMOVE))DispatchMessageW(&message);else Sleep(1);}
        Check(f.Selection()==2&&f.history.UndoCount()==1,"the fixture's real deferred timer repairs UI without repeating a data commit");
        strcpy_s(f.bones[2].name,"another change");probe.failAdd=true;RefreshBoundCameraFollowReadout(*f.app);const auto old=f.combo;
        DestroyWindow(old);f.combo=CreateWindowExW(0,L"COMBOBOX",L"replacement",WS_CHILD|CBS_DROPDOWNLIST,0,26,190,150,f.app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(450)),GetModuleHandleW(nullptr),nullptr);
        Check(f.combo!=nullptr,"recreate a foreign 450 HWND after pending repair");DWORD_PTR data=0;
        Check(!GetWindowSubclass(f.combo,UiProbe::Observe,152,&data),"destroyed combo does not transfer old observer/refdata to a replacement window");
        MSG message{};while(PeekMessageW(&message,old,WM_TIMER,WM_TIMER,PM_REMOVE))DispatchMessageW(&message);
        Check(SendMessageW(f.combo,CB_GETCOUNT,0,0)==0&&f.history.UndoCount()==1&&f.app->CameraParentBone()==2,
            "late old timer messages cannot edit a replacement window or repeat committed history");
        probe.window=nullptr;
    }
    {
        Scene f;MmsObjectRef live{},sentinel{{0x1234,0x5678},91};auto output=sentinel;
        const void* unknown=reinterpret_cast<const void*>(std::uintptr_t(1));failAfter=0;
        const bool missing=FindSceneObjectIdentity(unknown,output);const bool found=FindSceneObjectIdentity(f.first.raw(),live);failAfter=-1;
        Check(!missing&&output.id.lo==sentinel.id.lo&&output.id.hi==sentinel.id.hi&&output.generation==sentinel.generation&&found&&live.generation,
            "pure identity lookup never registers unknown input, keeps failure output and succeeds live with allocation disabled");
        auto owner=RetainSceneObject(*f.app,MMS_OBJECT_MODEL,0);auto remove=owner->PreparePresence(false);Check(remove->IsCurrent(),"prepare absent lookup source");remove->Publish();remove.reset();
        output=sentinel;failAfter=0;const bool absent=FindSceneObjectIdentity(f.first.raw(),output);failAfter=-1;
        Check(!absent&&output.id.lo==sentinel.id.lo&&output.id.hi==sentinel.id.hi&&output.generation==sentinel.generation,"absent identity lookup remains no-allocation and leaves output untouched");
        auto restore=owner->PreparePresence(true);Check(restore->IsCurrent(),"restore lookup fixture before owner cleanup");restore->Publish();
    }
    {
        auto f=std::make_unique<Scene>();auto binding=std::make_unique<NativePoseTestBinding>(*f->app,f->history);
        RefreshBoundCameraFollowReadout(*f->app);UiProbe probe(*f);strcpy_s(f->bones[2].name,"orphan pending UI");probe.failAdd=true;
        RefreshBoundCameraFollowReadout(*f->app);const auto main=f->app->MainWindow(),combo=f->combo;
        Check(SendMessageW(combo,CB_GETCOUNT,0,0)==0,"leave a genuine failed UI repair queued before app teardown");
        binding.reset();RemoveWindowSubclass(main,Scene::Window,151);f->app->MainWindow()=nullptr;f.reset();
        // The owned test root deliberately outlives the now-unbound app.
        // RepairProc must compare the binding before dereferencing refdata.
        const auto start=GetTickCount();bool delivered=false;
        while(GetTickCount()-start<250&&!delivered){MSG message{};if(PeekMessageW(&message,combo,WM_TIMER,WM_TIMER,PM_REMOVE)){DispatchMessageW(&message);delivered=true;}else Sleep(1);}
        Check(delivered&&SendMessageW(combo,CB_GETCOUNT,0,0)==0,"late repair after binding/app teardown cancels without dereferencing the retired app");
        DestroyWindow(main);probe.window=nullptr;
    }
}
void ReentrantNotificationAndRawValue() {
    {
        Scene f;auto history=std::make_unique<SharedHistory>(f.storage);auto binding=std::make_unique<NativePoseTestBinding>(*f.app,*history);
        auto nextHistory=std::make_unique<SharedHistory>(f.storage);std::unique_ptr<NativePoseTestBinding> replacement;
        UiProbe probe(f,f.Edit(panel::kUndoButton));probe.trigger=WM_ENABLE;
        probe.callback=[&](UINT){replacement=std::make_unique<NativePoseTestBinding>(*f.app,*nextHistory);history.reset();};
        f.Choose(2);
        Check(!history&&replacement&&BoundNativePoseHistory(f.app.get())==nextHistory.get()&&f.app->CameraParentBone()==2&&!nextHistory->UndoCount(),
            "WM_ENABLE can retire the old journal after acceptance without late history access or committing into its replacement");
        replacement.reset();binding.reset();
    }
    {
        Scene f;NativePoseTestBinding binding(*f.app,f.history);constexpr std::int32_t raw=INT32_MIN+173;
        f.app->CameraParentBone()=raw;Source source(f);f.Choose(2);
        Check(f.app->CameraParentBone()==2&&f.history.UndoCount()==1,"a valid new selection keeps the complete noncanonical old int32 in history");source.Unchanged(2);
        Check(f.history.History(false)==MMS_OK&&f.app->CameraParentBone()==raw,"undo restores all original parent-index bytes rather than normalizing them");
        RefreshBoundCameraFollowReadout(*f.app);Check(f.Selection()==-1,"nonlisted canonical source restores no selection without inventing a row");source.Unchanged(raw);
    }
}
void DirectDraftSourceGuards() {
    for(UINT phase:{UINT(EM_GETMODIFY),UINT(WM_GETTEXT),UINT(EM_GETSEL)})
    for(unsigned mutation=0;mutation<4;++mutation)for(bool noop:{false,true}){
        Scene f;NativePoseTestBinding binding(*f.app,f.history);
        // Both old and replacement parent models accept index 1, so a
        // missing source guard cannot hide behind argument validation.
        f.bones[1].type=mdl::BoneType::RotateMove;
        const auto draft=FollowPendingEdit(*f.app,panel::kReadoutPosXEdit);
        f.app->SelectGlobalTimelineTrack(GlobalTimelineTrack::Light);f.accessory.rowSelected=1;Source before(f);
        auto replacement=f.bones;auto* const originalTable=f.first.m->boneTable;
        const auto scene=f.app->SceneGeneration();const auto identity=SceneObjectIdentity(f.first.raw());
        std::uint64_t revision=0,afterRevision=UINT64_MAX;Check(f.history.Revision(revision)==MMS_OK,"capture direct-apply draft guard revision");
        UiProbe probe(f,draft);probe.trigger=phase;unsigned callbacks=0;
        probe.callback=[&](UINT){++callbacks;
            if(mutation==0)f.app->CameraParentModel()=1;
            if(mutation==1)f.app->AdvanceSceneGeneration();
            if(mutation==2)f.first.m->boneTable=replacement.data();
            if(mutation==3){ForgetSceneObject(f.first.raw());SceneObjectIdentity(f.first.raw());}
        };
        bool changed=true;const auto result=ApplyBoundCameraParentBone(*f.app,noop?0:1,changed);
        if(result!=MMS_E_STALE||changed||callbacks!=1||f.app->CameraParentBone()!=0||f.history.UndoCount())
            std::printf("  parent direct draft phase=%u mutation=%u noop=%u result=%d changed=%u callbacks=%u parent=%d bone=%d nodes=%zu\n",
                unsigned(phase),mutation,unsigned(noop),int(result),unsigned(changed),callbacks,f.app->CameraParentModel(),f.app->CameraParentBone(),f.history.UndoCount());
        Check(callbacks==1&&result==MMS_E_STALE&&!changed&&f.app->CameraParentBone()==0&&!f.history.UndoCount()&&!f.history.RedoCount()&&
            f.history.Revision(afterRevision)==MMS_OK&&afterRevision==revision&&!f.app->SceneModified()&&!f.app->RecordedFrameCountByte1()&&
            f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)&&!f.app->GlobalTrackSelected(GlobalTimelineTrack::Camera)&&f.accessory.rowSelected&&FollowHasPendingEdit(draft),
            "direct apply rejects a source changed during native draft reads, including no-op, without publication or initial selection effects");
        Check((mutation!=0||f.app->CameraParentModel()==1)&&(mutation!=1||f.app->SceneGeneration()!=scene)&&
            (mutation!=2||f.first.m->boneTable==replacement.data())&&
            (mutation!=3||!SceneObjectPresent(f.first.raw(),identity.id)),
            "rejection preserves the callback's own source change instead of silently rolling it back");
        // Restore only the mutations made explicitly by the test callback;
        // all other camera, pose, key, clock and physics fields must match.
        f.app->CameraParentModel()=0;f.first.m->boneTable=originalTable;before.Unchanged(0);
    }
}
void FollowAndRegistrationOrder() {
    FollowFixture f;f.Context(true,false,0,CameraAttachmentReference::None);NativePoseTestBinding binding(*f.app,f.history);
    const auto sample=[](const std::vector<mdl::CameraKey>& keys,int frame){
        // Independent unbound native seek: registration replay owns its
        // evaluated camera pose as well as the restored key data.
        FollowFixture oracle;oracle.Context(true,true,0,CameraAttachmentReference::None);oracle.app->CurrentFrame()=frame;
        Check(mdl::KeyPoolEnsure(oracle.app->CameraKeys(),keys.size()),"size the independent parent-registration seek oracle");
        std::memcpy(oracle.app->CameraKeys(),keys.data(),keys.size()*sizeof(mdl::CameraKey));
        ReloadModels(oracle.app.get());return NumpadRead(*oracle.app);
    };
    const auto matches=[&](const NumpadView& expected){const auto actual=NumpadRead(*f.app);
        return !std::memcmp(actual.values.data(),expected.values.data(),7*sizeof(float))&&actual.values[9]==expected.values[9]&&
            actual.perspective==expected.perspective&&actual.parentModel==expected.parentModel&&actual.parentBone==expected.parentBone;};
    f.Legacy(true,2);Check(f.history.UndoCount()==1&&f.app->CameraParentBone()==1,"seed actual follow menu node with a stable parent");
    const auto originalKeys=GlobalRegistrationKeys(f.app->CameraKeys());bool changed=false;
    Check(ApplyBoundCameraParentBone(*f.app,0,changed)==MMS_OK&&changed&&f.history.UndoCount()==2&&GlobalRegistrationSame(f.app->CameraKeys(),originalKeys),
        "parent adjustment follows a complete follow transition without registering the camera track");
    Check(TryRegisterBoundCamera(f.app.get(),f.app->MainWindow())&&f.history.UndoCount()==3&&f.app->CameraParentBone()==0,
        "actual camera registration shares the next node and stores the selected parent bone");
    const auto registeredKeys=GlobalRegistrationKeys(f.app->CameraKeys());const auto initialFrame=f.app->CurrentFrame();
    const auto originalSample=sample(originalKeys,initialFrame);
    const auto keyUndo=f.history.History(false);
    std::printf("  parent registration undo: result=%d keysMatch=%u parentBone=%d undo=%zu redo=%zu\n",
        int(keyUndo),unsigned(GlobalRegistrationSame(f.app->CameraKeys(),originalKeys)),f.app->CameraParentBone(),f.history.UndoCount(),f.history.RedoCount());
    Check(keyUndo==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),originalKeys)&&matches(originalSample)&&
        f.history.UndoCount()==2&&f.history.RedoCount()==1&&f.app->CurrentFrame()==initialFrame,
        "camera-key undo restores and evaluates the exact original track while retaining the earlier parent history node");
    f.app->CurrentFrame()=59;f.first.bones[0].trans[2]=13.75f;
    Check(f.history.History(false)==MMS_OK&&f.app->CameraParentBone()==1&&f.history.UndoCount()==1&&f.history.RedoCount()==2,
        "parent adjustment unwinds as its own earlier history node");
    Check(f.history.History(false)==MMS_OK&&!f.app->state.followCameraEnabled&&f.history.UndoCount()==0&&f.history.RedoCount()==3,
        "follow transition remains the first independent history node");
    Check(f.history.History(true)==MMS_OK&&f.app->state.followCameraEnabled&&f.app->CameraParentBone()==1&&f.history.UndoCount()==1,
        "redo follow restores its frozen parent before the later adjustment");
    Check(f.history.History(true)==MMS_OK&&f.app->CameraParentBone()==0&&f.history.UndoCount()==2,
        "redo the scalar parent adjustment independently of registration");
    const auto registeredSample=sample(registeredKeys,59);
    Check(f.history.History(true)==MMS_OK&&GlobalRegistrationSame(f.app->CameraKeys(),registeredKeys)&&matches(registeredSample)&&
        f.history.UndoCount()==3&&f.history.RedoCount()==0&&f.app->CurrentFrame()==59&&f.first.bones[0].trans[2]==13.75f,
        "registration redo evaluates its exact target track at the current frame and preserves the later model pose");
}
struct PmmFiles {
    std::vector<std::wstring> paths;
    std::wstring Next(){wchar_t directory[MAX_PATH]{},file[MAX_PATH]{};Check(GetTempPathW(MAX_PATH,directory)&&GetTempFileNameW(directory,L"cpb",0,file),"allocate private PMM fixture path");
        DeleteFileW(file);paths.push_back(std::wstring(file)+L".pmm");return paths.back();}
    ~PmmFiles(){for(const auto& path:paths)DeleteFileW(path.c_str());}
};
std::vector<unsigned char> StandardPrefix(const std::wstring& path) {
    const int fd=_wopen(path.c_str(),_O_BINARY|_O_RDONLY);Check(fd>=0,"open actual parent-index PMM copy");
    pmm_extension::Document document;std::uint64_t standard=0;const auto result=pmm_extension::Read(fd,document,standard);
    Check(result==pmm_extension::ReadResult::Loaded||result==pmm_extension::ReadResult::Absent,"parse actual PMM trailer boundary");
    if(result==pmm_extension::ReadResult::Absent)standard=std::uint64_t(_filelengthi64(fd));
    std::vector<unsigned char> bytes(std::size_t(standard),0);Check(_lseeki64(fd,0,SEEK_SET)==0&&_read(fd,bytes.data(),unsigned(bytes.size()))==bytes.size(),"read unchanged standard PMM prefix only");_close(fd);return bytes;
}
struct PmmDialogs {
    inline static unsigned count=0;
    static int WINAPI W(HWND,LPCWSTR,LPCWSTR,UINT){++count;return IDOK;}
    static int WINAPI A(HWND,LPCSTR,LPCSTR,UINT){++count;return IDOK;}
};
void StandardPmmBytes() {
    Scene f;NativePoseTestBinding binding(*f.app,f.history);PmmFiles files;
    // Exercise the real serializer without trying to reopen nonexistent model
    // source files. The parsed config2 field is the standard four-byte scalar;
    // this is not a complete PMM scene-loader roundtrip or a GPU import test.
    for(auto* model:{f.first.m.get(),f.second.m.get()}){model->ikChainCount=0;model->boneOrderCount=0;}
    f.app->AccessorySlot(7)=nullptr;
    for(int id:{panel::kMainComboModel,panel::kAccessoryCombo}){const auto combo=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,10,10,
        f.app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);Check(combo!=nullptr,"create PMM model/accessory count controls");
        if(id==panel::kMainComboModel)for(const auto* name:{L"camera",L"model A",L"model B"})SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));}
    for(int id:{panel::kPlayStartFrameEdit,panel::kPlayStopFrameEdit,panel::kGotoFrameEdit}){const auto edit=CreateWindowExW(0,L"EDIT",id==panel::kPlayStopFrameEdit?L"99":L"0",WS_CHILD,0,0,10,10,
        f.app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);Check(edit!=nullptr,"create valid PMM bound-readback controls");}
    f.app->LightKeys()=mdl::KeyPoolCreate<mdl::LightKey>(1);f.app->ShadowKeys()=mdl::KeyPoolCreate<mdl::SelfShadowKey>(1);f.app->GravityKeys()=mdl::KeyPoolCreate<mdl::GravityKey>(1);
    const auto report=save_compatibility::InspectPmm(*f.app);Check(!report.invalidData&&!report.needsNewPmm,"parent fixture stays representable by the existing standard PMM layout");
    // Any unexpected refusal must fail the assertion rather than leave an
    // unobserved native modal running on the inherited desktop.
    PmmDialogs::count=0;
    const auto initialized=MH_Initialize();Check(initialized==MH_OK||initialized==MH_ERROR_ALREADY_INITIALIZED,"initialize exact PMM refusal observers");
    Check(MH_CreateHook(reinterpret_cast<void*>(&MessageBoxW),reinterpret_cast<void*>(&PmmDialogs::W),nullptr)==MH_OK&&
        MH_CreateHook(reinterpret_cast<void*>(&MessageBoxA),reinterpret_cast<void*>(&PmmDialogs::A),nullptr)==MH_OK&&
        MH_EnableHook(reinterpret_cast<void*>(&MessageBoxW))==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(&MessageBoxA))==MH_OK,
        "intercept unexpected PMM refusal dialogs within this fixture");
    const auto first=files.Next();const bool savedBefore=SaveSceneCopy(f.app.get(),first.c_str());
    f.Choose(2);const auto second=files.Next();const bool savedAfter=SaveSceneCopy(f.app.get(),second.c_str());
    MH_DisableHook(reinterpret_cast<void*>(&MessageBoxW));MH_DisableHook(reinterpret_cast<void*>(&MessageBoxA));
    MH_RemoveHook(reinterpret_cast<void*>(&MessageBoxW));MH_RemoveHook(reinterpret_cast<void*>(&MessageBoxA));
    Check(savedBefore&&savedAfter&&!PmmDialogs::count,"real PMM copies save both sides without a refusal or hidden modal");
    const auto before=StandardPrefix(first),after=StandardPrefix(second);
    Check(before.size()==after.size()&&before.size()>86&&!std::memcmp(before.data(),"Polygon Movie maker 0002",24),"actual parent edit preserves standard PMM v2 header and byte length");
    const auto offset=before.size()-86;std::int32_t source=-1,target=-1;
    std::memcpy(&source,before.data()+offset,4);std::memcpy(&target,after.data()+offset,4);
    Check(source==0&&target==2,"standard config2 serializes the same signed four-byte camera-parent bone field");
    bool exact=true;for(std::size_t i=0;i<before.size();++i)if((i<offset||i>=offset+4)&&before[i]!=after[i])exact=false;
    Check(exact,"only the existing parent-index field changes in the entire standard PMM prefix");
    mdl::KeyPoolDestroy(f.app->LightKeys());mdl::KeyPoolDestroy(f.app->ShadowKeys());mdl::KeyPoolDestroy(f.app->GravityKeys());
    f.app->LightKeys()=nullptr;f.app->ShadowKeys()=nullptr;f.app->GravityKeys()=nullptr;f.app->AccessorySlot(7)=&f.accessory;
    for(auto* model:{f.first.m.get(),f.second.m.get()}){model->ikChainCount=1;model->boneOrderCount=1;}
}
}
namespace {
void NativeCameraParentHistoryChecks(){using namespace camera_parent_checks;
    std::puts("  camera parent: recording modes, actual 450/common history and current pose");ModesAndRealHistory();
    std::puts("  camera parent: renderer focus and filtered/reordered/stale combo identity");FocusAndMapping();
    std::puts("  camera parent: stable owners, short generations, logical roots and budgets");CandidatesOwnersAndBudgets();
    std::puts("  camera parent: full allocation sweep, cancellation and no-allocation publication");AllocationAndCancellation();
    std::puts("  camera parent: pending plugin, cross-model chronology and admission");BusyAndMixedPlugin();
    std::puts("  camera parent: native ComboLBox selection and capture");ActualComboList();
    std::puts("  camera parent: real GDI panel highlight and draft/model-mode isolation");AcceptedPanelHighlight();
    std::puts("  camera parent: 4000-bone cache, invalidation, native failure repair and lifetime");CacheAndLifecycle();
    std::puts("  camera parent: accepted-notification retirement and exact old int32");ReentrantNotificationAndRawValue();
    std::puts("  camera parent: direct apply keeps its pre-draft source, including no-op");DirectDraftSourceGuards();
    std::puts("  camera parent: real follow and camera-registration history order");FollowAndRegistrationOrder();
    std::puts("  camera parent: actual standard PMM prefix serialization");StandardPmmBytes();
}
}
