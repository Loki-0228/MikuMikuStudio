// Include after native_model_physics_checks.inc. Actual menu/resource windows,
// EDIT subclasses and the production per-frame viewport axis entry points.
#include <commctrl.h>
#include <MinHook.h>
#include "mikumikustudio/update_coordinator.hpp"
#include "../src/plugins/native_model_hud_history.hpp"
namespace mikumikustudio { void ReleasePhysicsEditScratch(MMDApp*); }
namespace model_physics_ui_checks {
using namespace plugins;
struct HiddenDialogs {
    HHOOK hook = nullptr;
    static LRESULT CALLBACK Hide(int code, WPARAM w, LPARAM l) {
        if (code == HCBT_CREATEWND) {
            auto* value = reinterpret_cast<CBT_CREATEWNDW*>(l)->lpcs;
            if (!(value->style & WS_CHILD)) { value->style &= ~WS_VISIBLE; value->x = value->y = -32000; }
        }
        if (code == HCBT_ACTIVATE) return 1;
        return CallNextHookEx(nullptr, code, w, l);
    }
    HiddenDialogs() { hook = SetWindowsHookExW(WH_CBT, Hide, nullptr, GetCurrentThreadId()); Check(hook != nullptr,"hide actual modeless physics resource windows"); }
    ~HiddenDialogs() { if (hook) UnhookWindowsHookEx(hook); }
};
std::wstring Text(HWND field) {
    const int n = GetWindowTextLengthW(field); std::wstring value(static_cast<std::size_t>(n) + 1,L'\0');
    const int count = GetWindowTextW(field,value.data(),n+1); value.resize(static_cast<std::size_t>(count)); return value;
}
bool Same(const ModelPhysicsDraft& a,const ModelPhysicsDraft& b) {
    return a.rigids.size()==b.rigids.size()&&a.joints.size()==b.joints.size()&&a.rigidNames==b.rigidNames&&a.jointNames==b.jointNames&&
        (a.rigids.empty()||!std::memcmp(a.rigids.data(),b.rigids.data(),a.rigids.size()*sizeof(mdl::RigidRecord)))&&
        (a.joints.empty()||!std::memcmp(a.joints.data(),b.joints.data(),a.joints.size()*sizeof(mdl::JointRecord)));
}
struct Fixture : model_physics_checks::Fixture {
    HiddenDialogs hidden;
    ~Fixture() { if (auto h=app->state.frameCopyDialog) SendMessageW(h,WM_CLOSE,0,0); }
    HWND Open() {
        CommandDispatch(nullptr,MAKEWPARAM(262,0)); const auto h=app->state.frameCopyDialog;
        Check(h&&IsWindow(h)&&BoundPhysicsModelDraft(app.get()),"menu 262 opens the actual private modeless physics window");
        Check(IsWindowUnicode(h)&&GetDlgItem(h,705)&&GetDlgItem(h,740)&&GetDlgItem(h,IDOK)&&GetDlgItem(h,IDCANCEL),
            "original physics resource exposes real Unicode controls and confirmation buttons");
        Check(!Text(GetDlgItem(h,709)).empty()&&!Text(GetDlgItem(h,710)).empty()&&!Text(GetDlgItem(h,711)).empty(),
            "private physics initializes numeric captions for hidden shape dimensions as well as visible radius");
        ShowWindow(h,SW_HIDE); return h;
    }
    void Closed(HWND h) {
        Check(!IsWindow(h)&&!app->state.frameCopyDialog&&!app->state.rigidScratchArray&&!app->state.jointScratchArray&&
            !BoundPhysicsModelDraft(app.get()),"closing removes the private view and both scratch arrays");
    }
    mdl::RigidRecord* Bodies() { return static_cast<mdl::RigidRecord*>(app->state.rigidScratchArray); }
    mdl::JointRecord* Joints() { return static_cast<mdl::JointRecord*>(app->state.jointScratchArray); }
};
struct Source {
    Fixture& fixture;
    ModelPhysicsDraft draft;
    std::array<mdl::BoneRecord,2> bones;
    std::vector<btCollisionObject*> objects;
    std::vector<btTypedConstraint*> constraints;
    mdl::RigidRecord* rigids;
    mdl::JointRecord* joints;
    std::uint32_t clock;
    unsigned char dirty,enhanced,reset,recorded;
    explicit Source(Fixture& f) : fixture(f),draft(f.Read()),rigids(mdl::Mdl(f.app->ModelSlot(0))->rigidTable),
        joints(mdl::Mdl(f.app->ModelSlot(0))->jointTable),clock(f.app->state.timeNowHigh),dirty(f.app->SceneModified()),
        enhanced(f.app->EnhancedModelDirty()),reset(f.app->PhysicsResetPending()),recorded(f.app->RecordedFrameCountByte1()) {
        std::memcpy(bones.data(),mdl::Mdl(f.app->ModelSlot(0))->boneTable,sizeof(bones));
        auto& world=f.physics.world;
        for(int i=0;i<world.getNumCollisionObjects();++i)objects.push_back(world.getCollisionObjectArray()[i]);
        for(int i=0;i<world.getNumConstraints();++i)constraints.push_back(world.getConstraint(i));
    }
    void Unchanged(bool compareDraft=true) const {
        auto& f=fixture;const auto& m=*mdl::Mdl(f.app->ModelSlot(0));
        Check(m.rigidTable==rigids&&m.jointTable==joints&&(!compareDraft||Same(f.Read(),draft)),"private controls never replace or edit formal physics tables before OK");
        bool world=f.physics.world.getNumCollisionObjects()==int(objects.size())&&f.physics.world.getNumConstraints()==int(constraints.size());
        for(std::size_t i=0;world&&i<objects.size();++i)world=f.physics.world.getCollisionObjectArray()[int(i)]==objects[i];
        for(std::size_t i=0;world&&i<constraints.size();++i)world=f.physics.world.getConstraint(int(i))==constraints[i];
        Check(world,"draft editing and cancellation preserve exact live Bullet member order");
        Check(!std::memcmp(m.boneTable,bones.data(),sizeof(bones))&&f.Stable()&&f.app->state.timeNowHigh==clock&&
            f.app->SceneModified()==dirty&&f.app->EnhancedModelDirty()==enhanced&&f.app->PhysicsResetPending()==reset&&
            f.app->RecordedFrameCountByte1()==recorded,"open/cancel/close do not seek, reset, dirty, alter author bones or invalidate time");
    }
};
void Command(HWND h,int id) { SendMessageW(h,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(h,id))); }
void Select(HWND h,int id,int row) {
    SendDlgItemMessageW(h,id,CB_SETCURSEL,row,0);
    SendMessageW(h,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(h,id)));
}
void Enter(HWND h,int id,const wchar_t* value) {
    Check(SetWindowTextW(GetDlgItem(h,id),value)!=FALSE,"type through actual physics EDIT");
    SendDlgItemMessageW(h,id,WM_KEYDOWN,VK_RETURN,1);
    SendDlgItemMessageW(h,id,WM_CHAR,L'\r',1);
    SendDlgItemMessageW(h,id,WM_KEYUP,VK_RETURN,1);
}
// Observe the real queue adapter, forwarding every call. The hidden, renderer-
// free fixture deliberately never completes a FrameDriver/SafePoint draw.
struct PreviewRequests {
    using Request = void (*)(MMDApp*,bool);
    static thread_local PreviewRequests* current;
    Fixture& fixture;
    Request original = nullptr;
    HWND wake = nullptr;
    unsigned calls = 0, geometryChanges = 0, publishedCalls = 0;
    bool initialized = false;
    static void Forward(MMDApp* app,bool geometry) {
        auto& self=*current;
        if(app==self.fixture.app.get()) {
            ++self.calls;if(geometry)++self.geometryChanges;
            if(self.fixture.history.UndoCount())++self.publishedCalls;
        }
        self.original(app,geometry);
    }
    explicit PreviewRequests(Fixture& f) : fixture(f) {
        Check(!current,"one real physics preview request observer");current=this;
        const auto init=MH_Initialize();initialized=init==MH_OK;
        Check(initialized||init==MH_ERROR_ALREADY_INITIALIZED,"initialize preview request forwarding observation");
        Check(MH_CreateHook(reinterpret_cast<void*>(&updates::RequestViewport),reinterpret_cast<void*>(&Forward),
            reinterpret_cast<void**>(&original))==MH_OK&&
            MH_EnableHook(reinterpret_cast<void*>(&updates::RequestViewport))==MH_OK,"observe and forward the production viewport request adapter");
        Check(!IsWindowVisible(f.app->MainWindow())&&!f.app->Renderer(),"queue fixture has a hidden main window and no renderer");
        Check(updates::Initialize(f.app.get()),"initialize the real preview coordinator for the hidden owner");
        wake=FindWindowExW(HWND_MESSAGE,nullptr,L"MMS.UpdateCoordinator.V1",nullptr);
        Check(wake!=nullptr,"real coordinator creates its message-only wake window");
        Check(Drain()==1,"consume the initial real coordinator wake while its owner is hidden");
        calls=geometryChanges=publishedCalls=0;
    }
    ~PreviewRequests() {
        if(auto h=fixture.app->state.frameCopyDialog)SendMessageW(h,WM_CLOSE,0,0);
        updates::Shutdown(fixture.app.get());
        MH_DisableHook(reinterpret_cast<void*>(&updates::RequestViewport));
        MH_RemoveHook(reinterpret_cast<void*>(&updates::RequestViewport));
        if(initialized)MH_Uninitialize();current=nullptr;
    }
    unsigned Drain() const {
        unsigned count=0;MSG msg{};
        while(PeekMessageW(&msg,wake,WM_APP+0x73,WM_APP+0x73,PM_REMOVE)) {++count;DispatchMessageW(&msg);}
        return count;
    }
    bool Pending() const { MSG msg{};return PeekMessageW(&msg,wake,WM_APP+0x73,WM_APP+0x73,PM_NOREMOVE)!=FALSE; }
};
thread_local PreviewRequests* PreviewRequests::current=nullptr;
void PreviewQueueAndPassiveMessages() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);PreviewRequests preview(f);
        Check(f.history.SetMode(mode)==MMS_OK,"preview requests are independent of history recording mode");
        Source before(f);const auto h=f.Open();const auto field=GetDlgItem(h,718);
        Check(preview.calls>0&&preview.Pending()&&!preview.geometryChanges,"opening private physics queues a real drawing-only viewport wake");
        const auto opened=preview.calls;
        wchar_t value[128]{};SendMessageW(field,WM_GETTEXT,128,reinterpret_cast<LPARAM>(value));
        SendMessageW(field,WM_GETTEXTLENGTH,0,0);SendMessageW(field,WM_GETDLGCODE,VK_RETURN,0);
        const auto dc=GetDC(field);Check(dc!=nullptr,"obtain a real hidden field DC for passive colour queries");
        SendMessageW(h,WM_CTLCOLOREDIT,reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(field));ReleaseDC(field,dc);
        for(auto notice:{EN_SETFOCUS,EN_KILLFOCUS,EN_CHANGE})
            SendMessageW(h,WM_COMMAND,MAKEWPARAM(718,notice),reinterpret_cast<LPARAM>(field));
        Check(SetWindowTextW(field,L"2.75")!=FALSE,"stage native physics text without accepting Enter");
        f.app->InteractionDragMode()=ViewportDragMode::None;ModeTranslate(f.app.get(),0);
        Check(preview.calls==opened,"text reads, colour, focus, field fills and idle axis polling do not request previews");
        SendMessageW(field,WM_IME_STARTCOMPOSITION,0,0);SendMessageW(field,WM_KEYDOWN,VK_RETURN,1);Command(h,IDOK);
        Check(IsWindow(h)&&f.Bodies()[0].mass==1&&preview.calls==opened,"composition Enter and OK do not accept a field or enqueue drawing");
        SendMessageW(field,WM_IME_ENDCOMPOSITION,0,0);
        Check(preview.calls==opened,"composition end itself is not a physics edit");
        SendMessageW(field,WM_KEYDOWN,VK_RETURN,1);
        Check(f.Bodies()[0].mass==2.75f&&preview.calls==opened+1,"fresh non-IME Enter requests drawing after accepting only the private field");
        auto prior=preview.calls;Command(h,727);
        Check(f.Bodies()[0].shape==1&&preview.calls==prior+1,"an accepted shape command requests the private geometry preview");
        prior=preview.calls;Select(h,704,1);
        Check(f.app->state.selectedRigidIndex==1&&preview.calls==prior+1,"real body selection requests the newly selected helper drawing");
        Select(h,704,0);
        for(int operation:{4,10}) {
            prior=preview.calls;f.app->InteractionDragMode()=static_cast<ViewportDragMode>(operation);
            f.app->PreviousMouseY()=100;f.app->MouseY()=104;
            const auto old=operation==4?f.Bodies()[0].rotation[0]:f.Bodies()[0].position[0];ModeTranslate(f.app.get(),0);
            Check((operation==4?f.Bodies()[0].rotation[0]:f.Bodies()[0].position[0])!=old&&preview.calls==prior+1,
                "a real rigid rotation or translation requests drawing once after the draft value changes");
            prior=preview.calls;f.app->PreviousMouseY()=f.app->MouseY();ModeTranslate(f.app.get(),0);
            Check(preview.calls==prior,"zero-delta rigid axis echoes do not repeatedly request drawing");
        }
        prior=preview.calls;Command(h,801);
        Check(f.app->state.physicsEditorJointPage==1&&preview.calls==prior+1,"changing to the joint page requests its helper drawing");
        for(int operation:{4,10}) {
            prior=preview.calls;f.app->InteractionDragMode()=static_cast<ViewportDragMode>(operation);
            f.app->PreviousMouseY()=100;f.app->MouseY()=104;
            const auto old=operation==4?f.Joints()[0].rotation[0]:f.Joints()[0].position[0];ModePhysicsBody(f.app.get(),0);
            Check((operation==4?f.Joints()[0].rotation[0]:f.Joints()[0].position[0])!=old&&preview.calls==prior+1,
                "a real joint rotation or translation requests drawing once after the draft value changes");
            prior=preview.calls;f.app->PreviousMouseY()=f.app->MouseY();ModePhysicsBody(f.app.get(),0);
            Check(preview.calls==prior,"zero-delta joint axis echoes do not repeatedly request drawing");
        }
        before.Unchanged();
        Check(preview.Drain()==1&&!preview.Pending()&&!preview.geometryChanges,
            "accepted preview requests coalesce into one real wake without marking geometry or advancing the hidden scene");
        before.Unchanged();prior=preview.calls;Command(h,IDCANCEL);f.Closed(h);
        Check(preview.calls==prior+1&&preview.Pending(),"cancellation clears auxiliary drawing with one request across all destroy notifications");
        before.Unchanged();Check(!f.history.UndoCount(),"preview requests and cancellation create no common-history records");
    }
}
void CancelNoopAndLifetime() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        for(unsigned action=0;action<5;++action) {
            Fixture f;NativePoseTestBinding bound(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"real physics UI recording mode");
            f.app->state.timeNowHigh=0x23456789;f.app->state.physicsEditorJointPage=7;Source before(f);
            const auto h=f.Open();before.Unchanged();
            Check(f.Bodies()[0].jpText!=mdl::Mdl(f.app->ModelSlot(0))->rigidTable[0].jpText,
                "private scratch names never borrow source model-owned UTF16 allocations");
            if(action!=1)Enter(h,718,L"2.75");
            before.Unchanged();Check(!f.history.UndoCount(),"editing and Enter create no common-history node");
            if(action==0)Command(h,IDCANCEL);
            else if(action==1)Command(h,IDOK);
            else if(action==2)SendMessageW(h,WM_CLOSE,0,0);
            else if(action==3)DestroyWindow(h);
            else ReleasePhysicsEditScratch(f.app.get());
            f.Closed(h);before.Unchanged();
            Check(!f.history.UndoCount()&&f.app->state.physicsEditorJointPage==7,"all teardown paths restore prior page state without a history node");
        }
    }
}
void ConfirmAndReplay() {
    for(auto mode:{SharedHistory::RecordingMode::AllOperations,SharedHistory::RecordingMode::ExplicitOnly}) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);Check(f.history.SetMode(mode)==MMS_OK,"explicit real physics confirmation in either mode");
        auto& initial=*mdl::Mdl(f.app->ModelSlot(0));
        initial.rigidTable[0].size[1]=.31415927f;initial.rigidTable[0].size[2]=.27182818f;
        Source before(f);const auto h=f.Open();
        Enter(h,718,L"2.75");
        std::fprintf(stderr,"PHYSICS_UI stage=mass mode=%u live_window=%d scratch=%p mass=%.9g damping=%.9g undo=%u\n",
            unsigned(mode),int(IsWindow(h)),static_cast<void*>(f.Bodies()),f.Bodies()?double(f.Bodies()[0].mass):-1.,
            f.Bodies()?double(f.Bodies()[0].linearDamping):-1.,unsigned(f.history.UndoCount()));
        Check(IsWindow(h)&&f.Bodies()&&f.Bodies()[0].mass==2.75f,"Enter keeps an accepted mass in the live private physics draft");
        Enter(h,705,L"新的物理名称😀");
        Check(IsWindow(h)&&f.Bodies()&&f.Bodies()[0].mass==2.75f,"Unicode rename retains the private window and the previously edited mass");
        before.Unchanged();
        Command(h,IDOK);f.Closed(h);auto after=f.Read();auto& m=*mdl::Mdl(f.app->ModelSlot(0));
        std::fprintf(stderr,"PHYSICS_UI stage=confirmed mode=%u mass=%.9g damping=%.9g expected_damping=%.9g undo=%u stable=%d frame=%d reset=%u bone_x=%.9g camera_x=%.9g distance=%.9g light_track=%u peer_light=%.9g peer_morph=%.9g\n",
            unsigned(mode),double(after.rigids[0].mass),double(after.rigids[0].linearDamping),double(before.draft.rigids[0].linearDamping),
            unsigned(f.history.UndoCount()),int(f.Stable()),f.app->CurrentFrame(),unsigned(f.app->PhysicsResetPending()),double(m.boneTable[0].trans[0]),
            double(f.app->CameraPositionX()),double(f.app->CameraDistance()),unsigned(f.app->GlobalTrackSelected(GlobalTimelineTrack::Light)),
            double(mdl::Mdl(f.app->ModelSlot(1))->lightDir[0]),double(mdl::Mdl(f.app->ModelSlot(1))->morphKeys[0].value));
        Check(after.rigids[0].mass==2.75f,"one real OK publishes the edited physics mass");
        Check(after.rigids[0].linearDamping==before.draft.rigids[0].linearDamping,"one real OK preserves untouched float precision");
        Check(after.rigids[0].size[1]==before.draft.rigids[0].size[1]&&after.rigids[0].size[2]==before.draft.rigids[0].size[2],
            "one real OK preserves exact source dimensions that the sphere editor keeps hidden");
        Check(f.history.UndoCount()==1,"one real OK creates exactly one common-history node");
        Check(f.Stable(),"one real OK preserves author pose, playback position, peer model and physics reset state");
        Check(after.rigidNames[0][0]==std::vector<wchar_t>({L'新',L'的',L'物',L'理',L'名',L'称',0xd83d,0xde00,0})&&
            after.rigidNames[1]==before.draft.rigidNames[1]&&after.jointNames==before.draft.jointNames,
            "real name EDIT preserves Unicode edits and unchanged allocations including tails after NUL");
        auto* replacement=m.rigidTable;auto* body=m.rigidTable[0].body;
        Check(f.history.History(false)==MMS_OK&&Same(f.Read(),before.draft)&&m.rigidTable==before.rigids&&f.Stable(),"real-window physics edit undoes the entire old resource version without a file");
        const auto redo=f.history.RedoCount();const auto unchanged=f.Open();Command(unchanged,IDOK);f.Closed(unchanged);
        Check(!f.history.UndoCount()&&f.history.RedoCount()==redo&&Same(f.Read(),before.draft),"unchanged OK preserves redo and exact original numeric/name data");
        Check(f.history.History(true)==MMS_OK&&Same(f.Read(),after)&&m.rigidTable==replacement&&m.rigidTable[0].body==body&&f.Stable(),
            "redo restores the exact replacement body identity after modeless UI has been destroyed");
    }
}
void PagesAndViewport() {
    Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source before(f);const auto h=f.Open();
    f.app->InteractionDragMode()=static_cast<ViewportDragMode>(4);f.app->PreviousMouseY()=100;f.app->MouseY()=105;
    const auto rotation=f.Bodies()[0].rotation[0];ModeTranslate(f.app.get(),0);
    Check(f.Bodies()[0].rotation[0]!=rotation&&!ModelHudGestureActive(*f.app),"production axis update changes private rigid rotation before the HUD history gate");
    const auto precise=f.Bodies()[0].rotation[0];Select(h,704,1);Select(h,704,0);
    Check(f.Bodies()[0].rotation[0]==precise,"host numeric echoes do not round a private viewport drag during selection changes");
    Command(h,801);Check(f.app->state.physicsEditorJointPage==1,"joint page drives the same page identity consumed by viewport tools");
    f.app->InteractionDragMode()=static_cast<ViewportDragMode>(10);f.app->PreviousMouseY()=100;f.app->MouseY()=104;
    const auto joint=f.Joints()[0].position[0],rigid=f.Bodies()[0].position[0];ModePhysicsBody(f.app.get(),0);
    Check(f.Joints()[0].position[0]!=joint&&f.Bodies()[0].position[0]==rigid,"joint-page translation changes only the selected private joint");
    Command(h,800);Check(f.app->state.physicsEditorJointPage==0,"body page restores private rigid routing");
    before.Unchanged();Command(h,IDCANCEL);f.Closed(h);before.Unchanged();
}
void StaleAndUnbound() {
    {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source before(f);const auto h=f.Open();Enter(h,718,L"2.75");
        auto& m=*mdl::Mdl(f.app->ModelSlot(0));m.rigidTable[0].mass=4;
        Command(h,IDOK);f.Closed(h);Check(m.rigidTable==before.rigids&&m.rigidTable[0].mass==4&&!f.history.UndoCount(),"source edits invalidate the private window without overwriting the new source");
        m.rigidTable[0].mass=before.draft.rigids[0].mass;before.Unchanged();
    }
    {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source before(f);const auto h=f.Open();
        f.app->AdvanceSceneGeneration();Command(h,IDOK);f.Closed(h);
        before.Unchanged();Check(!f.history.UndoCount(),"a changed project generation cancels instead of committing an old draft");
    }
    {
        Fixture f;Source before(f);const auto h=f.Open();Enter(h,718,L"3.25");before.Unchanged();Command(h,IDOK);f.Closed(h);
        Check(mdl::Mdl(f.app->ModelSlot(0))->rigidTable[0].mass==3.25f&&!f.history.UndoCount()&&f.Stable(),"ordinary full-profile window shares private resource publication without a bound journal");
        const auto second=f.Open();Enter(second,718,L"4.25");Command(second,IDOK);f.Closed(second);
        Check(mdl::Mdl(f.app->ModelSlot(0))->rigidTable[0].mass==4.25f,"ordinary full-profile reopening never reaches legacy destructive ownership");
    }
}
struct DestroyDuringFill {
    HWND dialog=nullptr;bool fired=false;
    static LRESULT CALLBACK Proc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR value) {
        auto& self=*reinterpret_cast<DestroyDuringFill*>(value);
        if(msg==WM_SETTEXT&&!self.fired) {self.fired=true;DestroyWindow(self.dialog);return FALSE;}
        if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,Proc,id);
        return DefSubclassProc(h,msg,w,l);
    }
};
void NativeReentrantClose() {
    Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source before(f);const auto h=f.Open();DestroyDuringFill close{h};
    Check(SetWindowSubclass(GetDlgItem(h,709),DestroyDuringFill::Proc,0x4d505549,reinterpret_cast<DWORD_PTR>(&close))!=FALSE,"install a real field-message reentrant close");
    Select(h,704,1);f.Closed(h);Check(close.fired&&!f.history.UndoCount(),"a nested native field fill may destroy the window without submitting an edit");before.Unchanged();
}
void PreviewTeardownQueue() {
    for(unsigned action=0;action<6;++action) {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);PreviewRequests preview(f);Source before(f);
        const auto h=f.Open();Check(preview.Drain()==1,"consume the opening preview wake before a teardown case");
        if(action==5) {Enter(h,718,L"2.75");Check(preview.Drain()==1,"consume accepted input wake before real confirmation");}
        const auto prior=preview.calls;
        // These teardown fixtures start with an empty journal. Retirement is
        // before publication; only a changed OK creates the first record and
        // requests the separate committed-scene redraw. Do not use this
        // attribution rule for fixtures with pre-existing history entries.
        Check(!f.history.UndoCount()&&!f.history.RedoCount()&&!preview.publishedCalls,
            "teardown request attribution starts without any published history");
        const auto priorPublished=preview.publishedCalls;
        if(action==0)DestroyWindow(h);
        else if(action==1)Enter(h,718,L"NaN");
        else if(action==2) {f.app->AdvanceSceneGeneration();SendMessageW(h,WM_GETTEXTLENGTH,0,0);}
        else if(action==3) {
            DestroyDuringFill close{h};
            Check(SetWindowSubclass(GetDlgItem(h,709),DestroyDuringFill::Proc,0x4d505552,
                reinterpret_cast<DWORD_PTR>(&close))!=FALSE,"install reentrant close while observing the real preview queue");
            Select(h,704,1);Check(close.fired,"real selection fill reentrantly destroys the preview dialog");
        } else Command(h,IDOK);
        f.Closed(h);
        const unsigned expectedPublished=action==5?1u:0u;
        Check(preview.publishedCalls==priorPublished+expectedPublished&&
            preview.calls==prior+1+expectedPublished&&!preview.geometryChanges&&preview.Pending(),
            "each teardown retires preview exactly once and only changed OK separately requests its published-scene redraw");
        Check(preview.Drain()==1&&!preview.Pending(),"teardown leaves one coalesced real wake and no repeated closing requests");
        if(action<5) {before.Unchanged();Check(!f.history.UndoCount(),"nonpublishing preview cleanup does not create history");}
        else {
            Check(f.Read().rigids[0].mass==2.75f&&f.history.UndoCount()==1&&f.Stable(),
                "consumed-plan confirmation clears preview and still publishes exactly one real physics edit");
        }
    }
}
void ImeAndInvalidText() {
    {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source before(f);const auto h=f.Open();const auto field=GetDlgItem(h,718);
        SendMessageW(field,WM_IME_STARTCOMPOSITION,0,0);Enter(h,718,L"2.75");Command(h,IDOK);
        Check(IsWindow(h)&&f.Bodies()[0].mass==1&&!f.history.UndoCount(),"IME-marked Enter and OK retain the composing field without accepting a physics draft");before.Unchanged();
        SendMessageW(field,WM_IME_ENDCOMPOSITION,0,0);SendMessageW(field,WM_KEYDOWN,VK_RETURN,1);
        Check(f.Bodies()[0].mass==2.75f,"a fresh Enter after composition updates only the private record");Command(h,IDCANCEL);f.Closed(h);before.Unchanged();
    }
    {
        Fixture f;NativePoseTestBinding bound(*f.app,f.history);Source before(f);const auto h=f.Open();Enter(h,718,L"NaN");
        f.Closed(h);before.Unchanged();Check(!f.history.UndoCount(),"nonfinite native numeric text fails before any model or history publication");
    }
}
void DisabledJointPreservation() {
    Fixture f;NativePoseTestBinding bound(*f.app,f.history);auto& m=*mdl::Mdl(f.app->ModelSlot(0));
    auto* replacement=static_cast<mdl::JointRecord*>(std::calloc(3,sizeof(mdl::JointRecord)));
    Check(replacement!=nullptr,"allocate disabled-joint UI fixture");replacement[0]=m.jointTable[0];
    for(unsigned i=1;i<3;++i) {
        auto& j=replacement[i];j.constraint=-1;j.rigidA=i==1?0:2;j.rigidB=1;
        std::strcpy(j.name,i==1?"disabled-valid":"disabled-missing");
        j.jpText=model_physics_checks::Text({L'無',L'効',wchar_t(L'0'+i),0,L'尾',0});
    }
    std::free(m.jointTable);m.jointTable=replacement;m.jointCount=3;
    const auto before=f.Read();auto h=f.Open();
    Check(SendDlgItemMessageW(h,736,CB_GETCOUNT,0,0)==1,"disabled native joints stay preserved outside the editable active-joint list");
    Enter(h,718,L"2.75");Command(h,IDOK);f.Closed(h);auto after=f.Read();
    Check(after.joints.size()==3&&after.joints[1].constraint==-1&&after.joints[2].constraint==-1&&
        after.joints[1].rigidA==0&&after.joints[2].rigidA==2&&after.jointNames==before.jointNames,
        "an unrelated rigid edit preserves disabled joint endpoints and complete name allocations");
    Check(f.history.History(false)==MMS_OK&&Same(f.Read(),before),"undo restores the full active-and-disabled joint table");
    h=f.Open();Command(h,706);Command(h,685);Command(h,685);Command(h,IDOK);f.Closed(h);after=f.Read();
    Check(after.rigids.size()==3&&after.joints.size()==2&&after.joints[0].constraint==-1&&after.joints[1].constraint==-1&&
        after.joints[0].rigidA==-1&&after.joints[0].rigidB==1&&after.joints[1].rigidA==-1&&
        after.jointNames[0]==before.jointNames[1]&&after.jointNames[1]==before.jointNames[2],
        "deleted or formerly missing disabled endpoints cannot acquire a newly inserted body through slot reuse");
}
}
void NativeModelPhysicsUiChecks() {
    model_physics_ui_checks::CancelNoopAndLifetime();model_physics_ui_checks::ConfirmAndReplay();
    model_physics_ui_checks::PagesAndViewport();model_physics_ui_checks::StaleAndUnbound();model_physics_ui_checks::NativeReentrantClose();
    model_physics_ui_checks::ImeAndInvalidText();model_physics_ui_checks::DisabledJointPreservation();
    model_physics_ui_checks::PreviewQueueAndPassiveMessages();model_physics_ui_checks::PreviewTeardownQueue();
}
