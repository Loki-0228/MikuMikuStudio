// Real production window messages, independent legacy arithmetic and the
// plugin menu's guarded common-history dispatch. No desktop input is injected.
namespace model_viewport_host_checks {
using Mode=plugins::SharedHistory::RecordingMode;
struct View {
    std::array<float,26> values{};std::uint8_t suppressed=0,perspective=0;int parent=-1,bone=0;
};
View Read(MMDApp& app){
    View value;for(unsigned i=0;i<3;++i){value.values[i]=app.CameraPositionAxis(i);value.values[i+3]=app.CameraRotation()[i];}
    value.values[6]=app.CameraDistance();value.values[7]=app.CameraFov();value.values[8]=app.ViewOffsetX();value.values[9]=app.ViewOffsetY();
    std::memcpy(value.values.data()+10,&app.CameraAttachmentBasis(),16*sizeof(float));
    value.suppressed=app.CameraAttachmentTransformSuppressed();value.perspective=app.CameraPerspective();value.parent=app.CameraParentModel();value.bone=app.CameraParentBone();return value;
}
void Write(MMDApp& app,const View& value){
    for(unsigned i=0;i<3;++i){app.CameraPositionAxis(i)=value.values[i];app.CameraRotation()[i]=value.values[i+3];}
    app.CameraDistance()=value.values[6];app.CameraFov()=value.values[7];app.ViewOffsetX()=value.values[8];app.ViewOffsetY()=value.values[9];
    std::memcpy(&app.CameraAttachmentBasis(),value.values.data()+10,16*sizeof(float));
    app.CameraAttachmentTransformSuppressed()=value.suppressed;app.CameraPerspective()=value.perspective;app.CameraParentModel()=value.parent;app.CameraParentBone()=value.bone;
}
bool Same(const View& a,const View& b){return !std::memcmp(a.values.data(),b.values.data(),sizeof a.values)&&a.suppressed==b.suppressed&&a.perspective==b.perspective&&a.parent==b.parent&&a.bone==b.bone;}
bool SameFloat(float a,float b){return !std::memcmp(&a,&b,sizeof a);}
std::uint32_t FloatBits(float value){std::uint32_t bits=0;std::memcpy(&bits,&value,sizeof bits);return bits;}
void DiagnoseUndo(MMDApp& app,plugins::SharedHistory& history,int value,int expectedValue,unsigned selected,
    const View& before,const View& after,const View& expected,unsigned branch,unsigned index,bool detached){
    const auto actual=Read(app);std::uint32_t mask=0;
    for(unsigned i=0;i<before.values.size();++i)if(!SameFloat(before.values[i],after.values[i]))mask|=1u<<i;
    if(before.suppressed!=after.suppressed)mask|=1u<<26;if(before.perspective!=after.perspective)mask|=1u<<27;
    if(before.parent!=after.parent)mask|=1u<<28;if(before.bone!=after.bone)mask|=1u<<29;
    std::printf("DIAG model drag undo branch=%u action=%u detached=%u historyMode=%u undo=%zu expectedUndo=1 redo=%zu expectedRedo=2 value=%d expectedValue=%d frame=%d expectedFrame=53 cameraMode=%u expectedCameraMode=1 selected=%u expectedSelected=%u mask=%08x capture=%p uiPending=%u\n",
        branch,index,unsigned(detached),unsigned(history.Mode()),history.UndoCount(),history.RedoCount(),value,expectedValue,app.CurrentFrame(),unsigned(app.CameraMode()),unsigned(app.SelectedModelSlot()),selected,
        unsigned(mask),static_cast<void*>(GetCapture()),unsigned(plugins::NativeObjectUiPending(&app)));
    const char* names[]={"position.x","position.y","position.z","pitch","yaw","roll","distance","fov","offset.x","offset.y"};
    for(unsigned i=0;i<actual.values.size();++i)if(!SameFloat(actual.values[i],expected.values[i]))
        std::printf("DIAG model view field=%s index=%u actual=%+.9g[%08x] expected=%+.9g[%08x] before=%+.9g[%08x] after=%+.9g[%08x] changed=%u\n",
            i<10?names[i]:"basis",i,double(actual.values[i]),unsigned(FloatBits(actual.values[i])),double(expected.values[i]),unsigned(FloatBits(expected.values[i])),
            double(before.values[i]),unsigned(FloatBits(before.values[i])),double(after.values[i]),unsigned(FloatBits(after.values[i])),unsigned((mask>>i)&1));
    if(actual.suppressed!=expected.suppressed)std::printf("DIAG model view suppressed actual=%u expected=%u before=%u after=%u\n",unsigned(actual.suppressed),unsigned(expected.suppressed),unsigned(before.suppressed),unsigned(after.suppressed));
    if(actual.perspective!=expected.perspective)std::printf("DIAG model view perspective actual=%u expected=%u before=%u after=%u\n",unsigned(actual.perspective),unsigned(expected.perspective),unsigned(before.perspective),unsigned(after.perspective));
    if(actual.parent!=expected.parent)std::printf("DIAG model view parent actual=%d expected=%d before=%d after=%d\n",actual.parent,expected.parent,before.parent,after.parent);
    if(actual.bone!=expected.bone)std::printf("DIAG model view bone actual=%d expected=%d before=%d after=%d\n",actual.bone,expected.bone,before.bone,after.bone);
    std::fflush(stdout);
}
template<class T>std::vector<std::uint8_t> Bytes(const T* p,std::size_t count){
    if(!count)return {};const auto* bytes=reinterpret_cast<const std::uint8_t*>(p);return {bytes,bytes+count*sizeof(T)};
}
bool Matches(const std::vector<std::uint8_t>& bytes,const void* p){return bytes.empty()||!std::memcmp(bytes.data(),p,bytes.size());}
struct Model {
    unsigned slot;mdl::ModelRecord* model;const mdl::BoneRecord* table;const unsigned count;const int selected;
    const mdl::BoneKey* bonePool;const mdl::MorphKey* morphPool;const mdl::DisplayKey* displayPool;
    std::vector<std::uint8_t> bones,physics,selection,boneKeys,morphKeys,displayKeys,rings,ringState;
    const int undoDirty,redoDirty;
    explicit Model(MMDApp& app,unsigned index):slot(index),model(mdl::Mdl(app.ModelSlot(index))),table(model->boneTable),count(model->boneCount),selected(model->selectedBone),
        bonePool(model->boneKeys),morphPool(model->morphKeys),displayPool(model->displayKeys),
        bones(Bytes(table,count)),physics(Bytes(model->bonePhysicsState,count)),selection(Bytes(model->boneSelection,count)),
        boneKeys(Bytes(model->boneKeys,mdl::BoneKeyCapacity(model->boneKeys))),morphKeys(Bytes(model->morphKeys,mdl::MorphKeyCapacity(model->morphKeys))),
        displayKeys(Bytes(model->displayKeys,mdl::KeyPoolCapacity(model->displayKeys))),rings(Bytes(model->undoRings,2)),ringState(Bytes(model->undoState,2)),
        undoDirty(model->undoDirty),redoDirty(model->redoDirty){}
    bool SameNative(MMDApp& app)const{
        return mdl::Mdl(app.ModelSlot(slot))==model&&model->boneTable==table&&model->boneCount==count&&model->selectedBone==selected&&
            model->boneKeys==bonePool&&model->morphKeys==morphPool&&model->displayKeys==displayPool&&
            mdl::BoneKeyCapacity(model->boneKeys)*sizeof(mdl::BoneKey)==boneKeys.size()&&mdl::MorphKeyCapacity(model->morphKeys)*sizeof(mdl::MorphKey)==morphKeys.size()&&
            mdl::KeyPoolCapacity(model->displayKeys)*sizeof(mdl::DisplayKey)==displayKeys.size()&&
            Matches(bones,table)&&Matches(physics,model->bonePhysicsState)&&Matches(selection,model->boneSelection)&&
            Matches(boneKeys,model->boneKeys)&&Matches(morphKeys,model->morphKeys)&&Matches(displayKeys,model->displayKeys)&&
            Matches(rings,model->undoRings)&&Matches(ringState,model->undoState)&&model->undoDirty==undoDirty&&model->redoDirty==redoDirty;
    }
};
struct Original {
    MMDApp& app;plugins::SharedHistory& history;View view;mdl::CameraKey* pool;const std::uint64_t identity;
    curve_host::AuthoringTable authoring,verification,workingAuthoring;HWND focus,floating;RECT viewport;std::vector<Model> models;
    std::vector<std::uint8_t> cameraBytes,globalSelection;std::uint64_t workingIdentity=0;
    const int frame,last,cursor,active,modified,recorded,messageSeen,physicsReset,mouseX,mouseY,previousX,previousY,sidebar;
    const unsigned shift,ctrl,right,middle;const std::uint8_t camera,slot,follow,reference,input,playback,step,pending,dirty,trace,layout;
    const float readoutX,readoutY,readoutZ;const bool projection;const Mode mode;const std::size_t maxEntries;const std::uint64_t maxBytes;
    Original(MMDApp& a,plugins::SharedHistory& h):app(a),history(h),view(Read(a)),pool(a.CameraKeys()),identity(mdl::KeyPoolIdentity(pool)),focus(GetFocus()),floating(a.FloatingWindow()),viewport(a.ViewportRect()),
        frame(a.CurrentFrame()),last(a.LastRegisteredFrame()),cursor(a.CameraTrackCursor()),active(a.CameraTrackActive()),modified(a.SceneModified()),recorded(a.RecordedFrameCountByte1()),
        messageSeen(a.MessageSeen()),physicsReset(a.PhysicsResetPending()),mouseX(a.MouseX()),mouseY(a.MouseY()),previousX(a.PreviousMouseX()),previousY(a.PreviousMouseY()),sidebar(a.SidebarWidth()),
        shift(a.ShiftModifierState()),ctrl(a.CtrlModifierState()),right(a.state.rightMouseButtonState),middle(a.state.middleMouseButtonState),camera(a.CameraMode()),slot(a.SelectedModelSlot()),
        follow(a.state.followCameraEnabled),reference(static_cast<std::uint8_t>(a.CameraReferenceMode())),input(a.ViewportInputActive()),playback(a.PlaybackActive()),step(a.FrameStepPlayback()),pending(a.state.recordPlaybackStartPending),
        dirty(a.state.viewDirty),trace(a.state.b6568482),layout(a.WindowLayoutReady()),readoutX(a.BoneRotationEditDegreesX()),readoutY(a.BoneRotationEditDegreesY()),readoutZ(a.BoneRotationEditDegreesZ()),
        projection(a.cameraProjectionRefreshPending),mode(h.Mode()),maxEntries(h.MaxEntries()),maxBytes(h.MaxBytes()){
        std::wstring error;Check(curve_host::CaptureAuthoring(app,verification,error),"capture model-view fixture's original global authoring");curve_host::SwapAuthoring(app,authoring);
        app.CameraKeys()=mdl::KeyPoolCreate<mdl::CameraKey>(4);app.CameraKeys()[0].parentModel=-1;app.CameraKeys()[0].distance=-45;app.CameraKeys()[0].fov=45;
        workingIdentity=mdl::KeyPoolIdentity(app.CameraKeys());cameraBytes=Bytes(app.CameraKeys(),mdl::KeyPoolCapacity(app.CameraKeys()));
        globalSelection=Bytes(app.state.globalTrackSelected,4);
        Check(curve_host::CaptureAuthoring(app,workingAuthoring,error),"capture the private model-view fixture's unchanged authoring baseline");
        for(unsigned i=0;i<kModelSlotCount;++i)if(auto* raw=app.ModelSlot(i)){auto* model=mdl::Mdl(raw);if(model->boneCount&&model->boneTable&&model->bonePhysicsState&&model->boneSelection)models.emplace_back(app,i);}
        app.PlaybackActive()=app.FrameStepPlayback()=0;app.state.recordPlaybackStartPending=0;app.ViewportRect()={0,0,800,600};app.SidebarWidth()=200;
    }
    ~Original(){
        plugins::CancelViewportCameraGesture(app);Check(history.Reset()==MMS_OK,"release real model-view records before restoring their pools and authoring");history.Collect();
        Invariants();
        curve_host::AuthoringTable discarded;curve_host::SwapAuthoring(app,discarded);auto* temporary=app.CameraKeys();app.CameraKeys()=pool;mdl::KeyPoolDestroy(temporary);curve_host::SwapAuthoring(app,authoring);
        Write(app,view);app.CurrentFrame()=frame;app.LastRegisteredFrame()=last;app.CameraTrackCursor()=cursor;app.CameraTrackActive()=active;app.SceneModified()=modified;app.RecordedFrameCountByte1()=recorded;
        app.MessageSeen()=messageSeen;app.PhysicsResetPending()=physicsReset;app.MouseX()=mouseX;app.MouseY()=mouseY;app.PreviousMouseX()=previousX;app.PreviousMouseY()=previousY;app.SidebarWidth()=sidebar;
        app.ShiftModifierState()=shift;app.CtrlModifierState()=ctrl;app.state.rightMouseButtonState=right;app.state.middleMouseButtonState=middle;
        app.CameraMode()=camera;app.SelectedModelSlot()=slot;app.state.followCameraEnabled=follow;app.CameraReferenceMode()=static_cast<CameraAttachmentReference>(reference);app.ViewportInputActive()=input;
        app.PlaybackActive()=playback;app.FrameStepPlayback()=step;app.state.recordPlaybackStartPending=pending;app.state.viewDirty=dirty;app.state.b6568482=trace;app.WindowLayoutReady()=layout;
        app.BoneRotationEditDegreesX()=readoutX;app.BoneRotationEditDegreesY()=readoutY;app.BoneRotationEditDegreesZ()=readoutZ;app.cameraProjectionRefreshPending=projection;app.FloatingWindow()=floating;app.ViewportRect()=viewport;
        if(focus&&IsWindow(focus))SetFocus(focus);else SetFocus(nullptr);
        Check(history.Configure(mode,maxEntries,maxBytes)==MMS_OK&&mdl::KeyPoolIdentity(pool)==identity&&curve_host::AuthoringUnchanged(app,verification),
            "real model-view fixture restores complete view, input, original pool, authoring and recording configuration");
    }
    void Invariants(){
        Check(mdl::KeyPoolIdentity(app.CameraKeys())==workingIdentity&&mdl::KeyPoolCapacity(app.CameraKeys())*sizeof(mdl::CameraKey)==cameraBytes.size()&&Matches(cameraBytes,app.CameraKeys())&&
            Matches(globalSelection,app.state.globalTrackSelected)&&curve_host::AuthoringUnchanged(app,workingAuthoring),
            "model viewport input preserves the private camera pool, global selection and all authored curves exactly");
        for(const auto& model:models)Check(model.SameNative(app),"model viewport input does not author bones, keys, selection or old undo rings");
    }
};
struct Windows {
    MMDApp& app;HWND previous,detached;WNDPROC procedure;
    explicit Windows(MMDApp& a):app(a),previous(a.FloatingWindow()),detached(CreateWindowExW(0,L"STATIC",L"ABI model viewport message fixture",WS_OVERLAPPED,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr)),procedure(nullptr){
        Check(detached!=nullptr,"create an owned hidden model-viewport message window");
        procedure=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(detached,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(MicWndProc)));
        Check(procedure!=nullptr,"install the production detached window procedure on the owned fixture");
    }
    ~Windows(){plugins::CancelViewportCameraGesture(app);app.FloatingWindow()=previous;SetWindowLongPtrW(detached,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(procedure));DestroyWindow(detached);}
    HWND Select(bool floating){app.FloatingWindow()=floating?detached:nullptr;return floating?detached:app.MainWindow();}
};
struct Action {UINT button;WPARAM modifiers;};
unsigned OtherSelection(const Original& original){
    if(original.models.size()>1)return original.models[1].slot;
    // RefreshObjectLists resolves a missing model to slot zero even in camera
    // mode. Keep the real selection when no second owner exists; the CPU
    // fixture covers replay across two distinct valid model owners.
    return original.models[0].slot;
}
void LegacyDrag(View& value,const Action& action,int dx,int dy){
    const bool shift=(action.modifiers&MK_SHIFT)!=0,ctrl=(action.modifiers&MK_CONTROL)!=0;
    if(action.button==WM_MBUTTONDOWN){const float scale=shift?.5f:ctrl?.005f:.05f;value.values[9]+=float(dy)*scale;value.values[8]-=float(dx)*scale;}
    else if(shift){value.values[9]+=float(dy)*.1f;value.values[8]-=float(dx)*.1f;}
    else if(ctrl)value.values[6]-=float(dy)*.1f;
    else {value.values[4]-=float(dx)*.01f;value.values[3]-=float(dy)*.01f;}
}
void Build(MMDApp& app,unsigned selected,unsigned other,unsigned branch,int reference){
    app.CameraMode()=0;app.SelectedModelSlot()=selected;app.state.followCameraEnabled=branch>=2;
    app.CameraParentModel()=branch==0||branch==2?-1:branch==3?int(other):int(selected);app.CameraParentBone()=0;
    app.CameraReferenceMode()=static_cast<CameraAttachmentReference>(reference);app.ViewportInputActive()=1;app.CurrentFrame()=7;
    app.CameraPositionX()=3.125f;app.CameraPositionY()=-2.25f;app.CameraPositionZ()=6.5f;app.CameraPitch()=.23f;app.CameraYaw()=-.57f;app.CameraRoll()=.13f;
    app.CameraDistance()=-71.25f;app.CameraFov()=45;app.ViewOffsetX()=2.75f;app.ViewOffsetY()=-3.5f;app.CameraPerspective()=0;app.CameraAttachmentTransformSuppressed()=1;
    for(unsigned i=0;i<16;++i)app.CameraAttachmentBasis().m[i/4][i%4]=float(i+1)*.125f;
    app.SceneModified()=app.RecordedFrameCountByte1()=app.PhysicsResetPending()=0;app.cameraProjectionRefreshPending=false;
    app.state.viewDirty=0;app.state.b6568482=0;app.WindowLayoutReady()=1;app.ShiftModifierState()=app.CtrlModifierState()=0;
    app.state.rightMouseButtonState=app.state.middleMouseButtonState=0;app.MouseX()=app.PreviousMouseX()=300;app.MouseY()=app.PreviousMouseY()=100;
}
View Replay(const View& before,const View& after,const View& current,bool forward){
    auto result=current;for(unsigned i=0;i<result.values.size();++i)if(!SameFloat(before.values[i],after.values[i]))result.values[i]=forward?after.values[i]:before.values[i];return result;
}
void Unrelated(MMDApp& app,const View& before,const View& after){
    auto current=Read(app);for(unsigned i=0;i<current.values.size();++i)if(SameFloat(before.values[i],after.values[i]))current.values[i]+=float(i+1)*.03125f;
    current.suppressed^=1;current.perspective^=1;Write(app,current);
}
void Mouse(HWND owner,UINT message,const Action& action,int x,int y){
    const bool up=message==WM_RBUTTONUP||message==WM_MBUTTONUP;
    const WPARAM button=action.button==WM_RBUTTONDOWN?MK_RBUTTON:MK_MBUTTON;
    SendMessageW(owner,message,action.modifiers|(up?0:button),MAKELPARAM(x,y));
}
void RunDrags(MMDApp* app,plugins::SharedHistory& history,int(*value)(),Original& original,Windows& windows){
    using edit_history_host_checks::Menu;const auto initial=value();const auto selected=original.models[0].slot,other=OtherSelection(original);
    const Action actions[]={{WM_RBUTTONDOWN,0},{WM_RBUTTONDOWN,MK_SHIFT},{WM_RBUTTONDOWN,MK_CONTROL},{WM_RBUTTONDOWN,MK_SHIFT|MK_CONTROL},
        {WM_MBUTTONDOWN,0},{WM_MBUTTONDOWN,MK_SHIFT},{WM_MBUTTONDOWN,MK_CONTROL},{WM_MBUTTONDOWN,MK_SHIFT|MK_CONTROL}};
    // The CPU suite owns the full reference Cartesian product. Here every
    // modifier/context/window/policy combination runs through real dispatch;
    // rotating the three references keeps expensive plugin/UI work bounded.
    for(bool detached:{false,true})for(unsigned branch=0;branch<(original.models.size()>1?4u:3u);++branch)
    for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(unsigned index=0;index<8;++index){
        const auto& action=actions[index];const int reference=int((branch+index)%3);
        Check(history.SetMode(mode)==MMS_OK,"configure real model-drag recording policy");Build(*app,selected,other,branch,reference);const auto owner=windows.Select(detached);
        const auto before=Read(*app);const auto up=action.button==WM_RBUTTONDOWN?WM_RBUTTONUP:WM_MBUTTONUP;
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe prepared");
        Mouse(owner,action.button,action,300,100);Mouse(owner,WM_MOUSEMOVE,action,311,106);Mouse(owner,up,action,322,110);
        Check(Same(Read(*app),before)&&GetCapture()!=owner&&history.UndoCount()==1&&value()==initial+1,
            "pending plugin transaction rejects real model drag without capture, preview or legacy fallback");Menu(app,L"Edit probe cancel");
        Mouse(owner,action.button,action,300,100);Check(GetCapture()==owner,"real model drag owns its main or detached capture");
        auto expected=before;Mouse(owner,WM_MOUSEMOVE,action,311,106);LegacyDrag(expected,action,11,6);
        Mouse(owner,WM_MOUSEMOVE,action,319,108);LegacyDrag(expected,action,8,2);
        Check(Same(Read(*app),expected)&&history.UndoCount()==1,"real model drag applies original per-move float arithmetic without per-move history nodes");
        Mouse(owner,up,action,322,110);LegacyDrag(expected,action,3,2);const auto after=Read(*app);
        Check(Same(after,expected)&&GetCapture()!=owner&&history.UndoCount()==(mode==Mode::AllOperations?2:1)&&app->CurrentFrame()==7,
            "real model release includes its final delta and makes one adjustment or an explicit-only preview");original.Invariants();
        Menu(app,L"Edit probe once");app->CurrentFrame()=53;app->CameraMode()=1;app->SelectedModelSlot()=other;app->state.followCameraEnabled=0;app->CameraReferenceMode()=CameraAttachmentReference::None;
        Unrelated(*app,before,after);const auto current=Read(*app),expectedUndo=Replay(before,after,current,false),expectedRedo=Replay(before,after,current,true);
        Menu(app,L"Edit probe undo");Check(value()==initial+1&&Same(Read(*app),current),"later plugin edit remains separately undoable after the model drag");
        if(mode==Mode::AllOperations){Menu(app,L"Edit probe undo");
            const bool restored=Same(Read(*app),expectedUndo)&&value()==initial+1&&app->CurrentFrame()==53&&app->CameraMode()==1&&app->SelectedModelSlot()==other;
            if(!restored)DiagnoseUndo(*app,history,value(),initial+1,other,before,after,expectedUndo,branch,index,detached);
            Check(restored,"real model-drag undo crosses frame and mode while preserving the valid selection and restoring only changed view fields");}
        Menu(app,L"Edit probe undo");Check(value()==initial,"preceding plugin edit remains separately undoable before the model drag");
        Menu(app,L"Edit probe redo");if(mode==Mode::AllOperations)Menu(app,L"Edit probe redo");
        Check(value()==initial+1&&Same(Read(*app),expectedRedo)&&app->CurrentFrame()==53,"real model-drag redo restores frozen arithmetic while preserving later independent view edits");
        Menu(app,L"Edit probe redo");Check(value()==initial+2,"following plugin edit remains independently redoable after model drag replay");
        const unsigned nodes=mode==Mode::AllOperations?3:2;for(unsigned i=0;i<nodes;++i)Menu(app,L"Edit probe undo");
        Check(value()==initial&&history.Reset()==MMS_OK,"real model-drag case restores plugin value before releasing its records");history.Collect();original.Invariants();
    }
}
void RunWheel(MMDApp* app,plugins::SharedHistory& history,int(*value)(),Original& original,Windows& windows){
    using edit_history_host_checks::Menu;const auto initial=value();const auto selected=original.models[0].slot,other=OtherSelection(original);
    for(bool detached:{false,true})for(unsigned branch=0;branch<5;++branch)for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly}){
        if(branch==3&&original.models.size()<2)continue;
        Check(history.SetMode(mode)==MMS_OK,"configure real model-wheel recording policy");Build(*app,selected,other,branch,2);const auto owner=windows.Select(detached);
        // Float bits c2680311 with -120 produces different last bits for the
        // main window's float multiply and the detached window's double chain.
        app->CameraDistance()=-58.002994537353516f;const auto before=Read(*app);
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe prepared");SendMessageW(owner,WM_MOUSEWHEEL,MAKEWPARAM(0,120),0);
        Check(Same(Read(*app),before)&&history.UndoCount()==1,"pending plugin transaction rejects real model wheel atomically");Menu(app,L"Edit probe cancel");
        auto expected=before;const bool hintOnly=!detached&&branch==4;
        if(!hintOnly){if(detached)expected.values[6]=static_cast<float>(-120.0*0.05000000074505806+static_cast<double>(expected.values[6]));else expected.values[6]=-120.f*.05f+expected.values[6];}
        SendMessageW(owner,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-120)),0);const auto after=Read(*app);
        const unsigned nativeNodes=!hintOnly&&mode==Mode::AllOperations?1:0;
        Check(Same(after,expected)&&history.UndoCount()==1+nativeNodes&&app->CurrentFrame()==7&&app->state.viewDirty==unsigned(hintOnly)&&
            app->state.b6568482==unsigned(hintOnly)&&app->WindowLayoutReady()==1,
            "real model wheel preserves main float/detached double arithmetic or exact native matched-follow hints without an empty journal node");original.Invariants();
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");Check(value()==initial+1,"plugin edit following model wheel is independently undoable");
        app->CurrentFrame()=61;app->CameraMode()=1;app->SelectedModelSlot()=other;Unrelated(*app,before,after);const auto current=Read(*app);
        if(nativeNodes){Menu(app,L"Edit probe undo");Check(Same(Read(*app),Replay(before,after,current,false))&&app->CurrentFrame()==61&&value()==initial+1,
            "real model-wheel undo changes only distance across mode, frame and selection");}
        Menu(app,L"Edit probe undo");Check(value()==initial,"plugin edit preceding model wheel remains independently undoable");
        const auto redo=history.RedoCount();const auto unchanged=Read(*app);app->SceneModified()=0;app->CameraMode()=0;app->state.followCameraEnabled=0;
        SendMessageW(owner,WM_MOUSEWHEEL,MAKEWPARAM(0,0),0);
        Check(Same(Read(*app),unchanged)&&history.RedoCount()==redo&&!app->SceneModified(),"zero model wheel preserves later independent edits, dirty state and existing redo");
        app->CameraMode()=1;app->state.followCameraEnabled=branch>=2;
        Menu(app,L"Edit probe redo");if(nativeNodes)Menu(app,L"Edit probe redo");Check(value()==initial+1&&Same(Read(*app),Replay(before,after,current,true)),
            "real model-wheel redo uses its exact frozen result and leaves unrelated fields intact");
        Menu(app,L"Edit probe redo");Check(value()==initial+2,"plugin edit following model wheel is independently redoable");
        for(unsigned i=0;i<2+nativeNodes;++i)Menu(app,L"Edit probe undo");Check(value()==initial&&history.Reset()==MMS_OK,"real model-wheel case restores plugin baseline");history.Collect();
    }
}
void RunCancelAndSpecial(MMDApp* app,plugins::SharedHistory& history,int(*value)(),Original& original,Windows& windows){
    using edit_history_host_checks::Menu;const auto selected=original.models[0].slot,other=OtherSelection(original);const auto initial=value();
    for(bool detached:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(UINT button:{WM_RBUTTONDOWN,WM_MBUTTONDOWN})for(unsigned cancellation=0;cancellation<5;++cancellation){
        Check(history.SetMode(mode)==MMS_OK,"configure real model cancellation recording policy");Build(*app,selected,other,0,2);const auto owner=windows.Select(detached);const auto before=Read(*app);const Action action{button,0};
        Menu(app,L"Edit probe once");Menu(app,L"Edit probe undo");const auto redo=history.RedoCount();app->SceneModified()=0;
        if(cancellation==0){
            const auto up=button==WM_RBUTTONDOWN?WM_RBUTTONUP:WM_MBUTTONUP;
            Mouse(owner,button,action,300,100);Mouse(owner,up,action,300,100);
            Check(Same(Read(*app),before)&&GetCapture()!=owner&&!history.UndoCount()&&history.RedoCount()==redo&&!app->SceneModified(),
                "zero-displacement real model drag releases capture without pruning redo or marking the scene");
            app->PlaybackActive()=1;Mouse(owner,button,action,300,100);Mouse(owner,WM_MOUSEMOVE,action,313,107);Mouse(owner,up,action,319,111);app->PlaybackActive()=0;
            Check(Same(Read(*app),before)&&GetCapture()!=owner&&!history.UndoCount()&&history.RedoCount()==redo&&!app->SceneModified(),
                "playback refuses real model drag before preview and cannot fall through to legacy edits");
        }
        Mouse(owner,button,action,300,100);Mouse(owner,WM_MOUSEMOVE,action,313,107);Check(!Same(Read(*app),before)&&GetCapture()==owner,"real model cancellation fixture has an unfinished changed preview");
        if(cancellation==0)SendMessageW(owner,WM_CANCELMODE,0,0);
        else if(cancellation==1)SetFocus(detached?app->MainWindow():windows.detached);
        else if(cancellation==2){SetCapture(detached?app->MainWindow():windows.detached);ReleaseCapture();}
        else if(cancellation==3)SendMessageW(owner,WM_KEYDOWN,VK_ESCAPE,0);
        else SendMessageW(owner,WM_ACTIVATEAPP,FALSE,0);
        Check(Same(Read(*app),before)&&GetCapture()!=owner&&!history.UndoCount()&&history.RedoCount()==redo&&!app->SceneModified()&&
            app->state.rightMouseButtonState==0&&app->state.middleMouseButtonState==0,
            "real model cancel, focus loss, capture theft, Escape or deactivation restores preview without journal or stuck button state");
        Menu(app,L"Edit probe redo");Menu(app,L"Edit probe undo");Check(value()==initial&&history.Reset()==MMS_OK,"cancelled model preview keeps the original plugin redo valid");history.Collect();original.Invariants();
    }
    for(bool detached:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(UINT button:{WM_RBUTTONDOWN,WM_MBUTTONDOWN}){
        Check(history.SetMode(mode)==MMS_OK,"configure matched-follow hint and pan recording policy");Build(*app,selected,other,4,2);const auto owner=windows.Select(detached);const auto before=Read(*app);const Action action{button,MK_SHIFT};
        Mouse(owner,button,action,300,100);Mouse(owner,WM_MOUSEMOVE,action,313,107);
        auto expected=before;const bool pan=button==WM_MBUTTONDOWN;if(pan)LegacyDrag(expected,action,13,7);
        Check(Same(Read(*app),expected)&&GetCapture()==owner&&app->state.viewDirty==1&&!history.UndoCount(),
            "real matched-follow held input uses only the original RMB hint or MMB offsets before final publication");
        Mouse(owner,button==WM_RBUTTONDOWN?WM_RBUTTONUP:WM_MBUTTONUP,action,319,111);if(pan)LegacyDrag(expected,action,6,4);
        const unsigned nativeNodes=pan&&mode==Mode::AllOperations?1:0;
        Check(Same(Read(*app),expected)&&GetCapture()!=owner&&history.UndoCount()==nativeNodes&&!history.RedoCount()&&
            bool(app->SceneModified())==pan&&bool(app->RecordedFrameCountByte1())==pan&&!app->PhysicsResetPending()&&
            !app->state.viewDirty&&!app->state.b6568482&&app->WindowLayoutReady()==1,
            "matched-follow release clears transient hint and records only a whole numeric pan under the configured policy");original.Invariants();
        if(nativeNodes){
            app->CameraMode()=1;app->SelectedModelSlot()=other;app->CurrentFrame()=61;app->state.followCameraEnabled=0;app->PhysicsResetPending()=0;
            Menu(app,L"Edit probe undo");Check(Same(Read(*app),before)&&app->CurrentFrame()==61&&!app->PhysicsResetPending(),
                "real followed-pan undo crosses model and frame without a physics settle");
            Menu(app,L"Edit probe redo");Check(Same(Read(*app),expected)&&app->CurrentFrame()==61&&!app->PhysicsResetPending(),
                "real followed-pan redo restores the exact sampled offsets without scene reevaluation");
            Menu(app,L"Edit probe undo");
        }
        Check(history.Reset()==MMS_OK,"matched-follow host case releases its private journal records");history.Collect();original.Invariants();
    }
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    Check(!history.UndoCount()&&!history.RedoCount(),"real model-view suite starts with an empty common journal");
    Original original(*app,history);Check(!original.models.empty(),"real model-view suite has a loaded model owner for its pose and history invariants");
    if(original.models.size()<2)std::puts("INFO model-view host has one model; independent-parent and cross-model combinations run in the CPU matrix, real replay retains its valid selected model");
    Windows windows(*app);RunDrags(app,history,value,original,windows);RunWheel(app,history,value,original,windows);RunCancelAndSpecial(app,history,value,original,windows);
}
}
