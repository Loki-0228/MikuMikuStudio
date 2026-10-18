// Include after accessory_hud_host_checks.inc inside edit_history_host_checks.
// The aggregate includes native_model_hud_history.hpp at top level. Call Run
// before removing the loaded models/X objects. No simulation or frame is run.
namespace model_hud_host_checks {
using Mode=plugins::SharedHistory::RecordingMode;
using model_viewport_host_checks::Bytes;
using model_viewport_host_checks::Matches;
using camera_hud_host_checks::ClientCursor;
using camera_hud_host_checks::MoveCursor;
using camera_hud_host_checks::DispatchQueued;
struct Pose {
    std::vector<std::array<float,7>> values;std::vector<unsigned char> dirty;bone_scale::Values scale;
    explicit Pose(unsigned char* raw){const auto& m=*mdl::Mdl(raw);values.resize(m.boneCount);dirty.assign(m.bonePhysicsState,m.bonePhysicsState+m.boneCount);scale=bone_scale::PoseValues(raw);
        for(unsigned i=0;i<m.boneCount;++i){std::memcpy(values[i].data(),m.boneTable[i].trans,3*sizeof(float));std::memcpy(values[i].data()+3,m.boneTable[i].rotQuat,4*sizeof(float));}}
    bool Same(unsigned char* raw)const{const auto& m=*mdl::Mdl(raw);if(values.size()!=m.boneCount||!bone_scale::PoseValuesEqual(raw,scale)||
            std::memcmp(dirty.data(),m.bonePhysicsState,dirty.size()))return false;
        for(unsigned i=0;i<m.boneCount;++i)if(std::memcmp(values[i].data(),m.boneTable[i].trans,3*sizeof(float))||std::memcmp(values[i].data()+3,m.boneTable[i].rotQuat,4*sizeof(float)))return false;
        return true;}
};
struct ModelFixture {
    MMDApp& app;plugins::SharedHistory& history;unsigned char* raw;mdl::ModelRecord& model;const unsigned bone;
    model_viewport_host_checks::Model native;model_paste_host_checks::Snapshot keys;
    std::vector<mdl::BoneRecord> originalBones,rigidBones;std::vector<unsigned char> originalDirty,originalSelection;
    bone_scale::Snapshot current;std::unique_ptr<bone_curve::AuthoringState> authors;bone_scale::AuthoringState scaleAuthors;
    std::unique_ptr<mdl::ModelRecord> parked=std::make_unique<mdl::ModelRecord>();
    const MmsObjectRef identity;const std::uint64_t scaleRevision;const bool scaleApplied;
    bool exchanged=false;
    static unsigned Bone(const mdl::ModelRecord& model){
        Check(model.boneCount&&model.boneTable&&model.bonePhysicsState&&model.boneSelection,"real imported model has complete native HUD pose storage");
        for(unsigned i=0;i<model.boneCount;++i)if((model.boneTable[i].type==mdl::BoneType::Move||model.boneTable[i].type==mdl::BoneType::Ik)&&model.boneTable[i].parent==-1)return i;
        for(unsigned i=0;i<model.boneCount;++i)if(model.boneTable[i].type==mdl::BoneType::Move||model.boneTable[i].type==mdl::BoneType::Ik)return i;
        Check(false,"actual imported PMX/Chinese-renamed PMD must contain a Move/Ik bone for real model HUD acceptance; no silent fixture skip");return 0;
    }
    unsigned char* Park()const{return reinterpret_cast<unsigned char*>(parked.get());}
    ModelFixture(MMDApp& a,plugins::SharedHistory& h):app(a),history(h),raw(a.ModelSlot(0)),model(*mdl::Mdl(raw)),bone(Bone(model)),native(a,0),keys(model),
        originalBones(model.boneTable,model.boneTable+model.boneCount),rigidBones(originalBones),originalDirty(model.bonePhysicsState,model.bonePhysicsState+model.boneCount),
        originalSelection(model.boneSelection,model.boneSelection+model.boneCount),current(bone_scale::Capture(raw)),authors(bone_curve::AuthoringState::Capture(raw)),
        scaleAuthors(bone_scale::AuthoringState::Capture(raw)),identity(plugins::SceneObjectIdentity(raw)),scaleRevision(bone_scale::Revision(raw)),scaleApplied(bone_scale::TransformsApplied(raw)){
        Check(model.boneTable&&model.bonePhysicsState&&model.boneSelection&&model.boneCount,"real imported model owns complete HUD author pose arrays");
        // Obtain the original rigid matrix baseline without touching its real
        // applied-cache flag or synchronizing any original free-curve hash.
        *parked=model;parked->boneTable=rigidBones.data();bone_scale::CopyPoseRuntime(raw,Park());bone_scale::RestoreTransforms(Park());bone_scale::Forget(Park());
        parked->boneTable=model.boneTable;bone_scale::ExchangeState(raw,Park());exchanged=true;
        Check(bone_scale::Restore(raw,current),"install isolated current scale authoring while original full runtime/rings remain parked");
        auto restore=bone_curve::AuthoringRestore::Prepare(raw,*authors);Check(restore->Publish(),"clone original free author values without source reconciliation for model HUD fixture");
        Check(scaleAuthors.Equals(bone_scale::AuthoringState::Capture(raw))&&authors->Equals(*bone_curve::AuthoringState::Capture(raw)),"isolated real model fixture preserves every original key and free author");
    }
    void Stable()const{
        Check(app.ModelSlot(0)==raw&&model.boneTable==native.table&&model.boneCount==native.count&&plugins::SceneObjectPresent(raw,identity.id)&&
            plugins::SceneObjectIdentity(raw).generation==identity.generation&&model.boneKeys==native.bonePool&&model.morphKeys==native.morphPool&&model.displayKeys==native.displayPool&&
            keys.Matches(model)&&Matches(native.rings,model.undoRings)&&Matches(native.ringState,model.undoState)&&model.undoDirty==native.undoDirty&&model.redoDirty==native.redoDirty&&
            authors->Equals(*bone_curve::AuthoringState::Capture(raw))&&scaleAuthors.Equals(bone_scale::AuthoringState::Capture(raw)),
            "real model HUD retains the imported resource identity, complete native keys/deep IK/display rows, all free/scale authors and legacy rings");
    }
    void Reset(){plugins::CancelModelHudGesture(app);Check(history.Reset()==MMS_OK,"retire actual model HUD journal before resetting isolated pose");history.Collect();
        Check(bone_scale::Restore(raw,current),"reset isolated live model scale pose without touching parked original cache/rings");
        std::memcpy(model.boneTable,rigidBones.data(),rigidBones.size()*sizeof(mdl::BoneRecord));
        std::memcpy(model.bonePhysicsState,originalDirty.data(),originalDirty.size());std::memset(model.boneSelection,0,model.boneCount);
        model.selectedBone=int(bone);model.boneSelection[bone]=1;auto& b=model.boneTable[bone];
        b.trans[0]=3.125f;b.trans[1]=-1.875f;b.trans[2]=6.3125f;b.rotQuat[0]=0;b.rotQuat[1]=.6f;b.rotQuat[2]=0;b.rotQuat[3]=.8f;model.bonePhysicsState[bone]=0;
        // Controlled derived basis makes global X's unchanged Y channel an
        // exact independent author field. The actual imported bone/pool stays.
        for(unsigned i=0;i<16;++i)b.matWorld[i]=(i%5==0)?1.f:0.f;
        bone_scale::Matrix scale;scale.v[0]=1.125f;Check(bone_scale::SetPose(raw,int(bone),scale),"set deliberately unregistered model HUD scale without registering keys");Stable();
    }
    ~ModelFixture(){plugins::CancelModelHudGesture(app);Check(history.Reset()==MMS_OK,"release real model HUD records before restoring original runtime");history.Collect();Stable();
        if(exchanged){bone_scale::ExchangeState(raw,Park());bone_scale::Forget(Park());}
        std::memcpy(model.boneTable,originalBones.data(),originalBones.size()*sizeof(mdl::BoneRecord));
        std::memcpy(model.bonePhysicsState,originalDirty.data(),originalDirty.size());std::memcpy(model.boneSelection,originalSelection.data(),originalSelection.size());model.selectedBone=native.selected;
        Check(native.SameNative(app)&&authors->Equals(*bone_curve::AuthoringState::Capture(raw))&&scaleAuthors.Equals(bone_scale::AuthoringState::Capture(raw))&&
            bone_scale::PoseValuesEqual(raw,current.pose)&&bone_scale::Revision(raw)==scaleRevision&&bone_scale::TransformsApplied(raw)==scaleApplied,
            "real model HUD fixture restores original complete bones/dirty/selection, scale runtime revision/applied cache/free authors and every original key/ring before later probes");
    }
};
struct InputRestore {
    MMDApp& app;POINT cursor{};bool cursorValid;const ViewportEditMode edit;const int coordinate,left,drag,operation,hovered,tool,box;
    const TimelineSelectionRow pending;const std::uint8_t jumped,separate,authored;
    explicit InputRestore(MMDApp& a):app(a),cursorValid(GetCursorPos(&cursor)!=FALSE),edit(a.EditMode()),coordinate(a.state.coordinateSystem),left(a.LeftMouseButtonState()),
        drag(int(a.InteractionDragMode())),operation(int(a.ViewToolDragOperation())),hovered(a.ViewportToolHovered()),tool(int(a.ViewportToolOperation())),box(a.BoneBoxSelectionActive()),
        pending(a.PendingTimelineSelectionRow()),jumped(a.state.mouseJumped),separate(a.state.separateWindowMouseSeen),authored(a.state.a06B5){}
    ~InputRestore(){plugins::CancelModelHudGesture(app);if(GetCapture()==app.MainWindow()||GetCapture()==app.FloatingWindow())ReleaseCapture();app.EditMode()=edit;app.state.coordinateSystem=coordinate;
        app.LeftMouseButtonState()=left;app.InteractionDragMode()=static_cast<ViewportDragMode>(drag);app.ViewToolDragOperation()=static_cast<ViewportToolAction>(operation);
        app.ViewportToolHovered()=hovered;app.ViewportToolOperation()=static_cast<ViewportToolAction>(tool);app.BoneBoxSelectionActive()=box;app.PendingTimelineSelectionRow()=pending;
        app.state.mouseJumped=jumped;app.state.separateWindowMouseSeen=separate;app.state.a06B5=authored;if(cursorValid)SetCursorPos(cursor.x,cursor.y);}
};
void Build(MMDApp& app,int operation){app.CameraMode()=0;app.SelectedModelSlot()=0;app.state.followCameraEnabled=0;app.CameraParentModel()=-1;
    app.EditMode()=operation==23?ViewportEditMode::Scale:ViewportEditMode::Bone;app.state.coordinateSystem=1;app.ViewportInputActive()=1;
    app.CurrentFrame()=7;app.LeftMouseButtonState()=0;app.ViewToolDragOperation()=ViewportToolAction::None;app.InteractionDragMode()=ViewportDragMode::None;
    app.ShiftModifierState()=app.CtrlModifierState()=0;app.state.a06B5=app.state.mouseJumped=0;app.BoneBoxSelectionActive()=0;
    app.SceneModified()=app.RecordedFrameCountByte1()=app.PhysicsResetPending()=0;app.ViewportRect()=FrameOverlayRect(&app);
    plugins::RefreshBoundPoseReadout(app);}
POINT Hit(MMDApp& app,int operation){const auto hud=viewport_tool_layout::Make(FrameOverlayRect(&app),FrameOverlayScale(&app),viewport_corner_settings::Current());
    POINT point{LONG(hud.x-110.f*hud.scale),LONG(hud.y-(operation==12?65.f:25.f)*hud.scale)};
    app.MouseX()=point.x;app.MouseY()=point.y;Check(ViewportToolAtPointForTest(&app)==operation,"real displayed model move-X/scale-X HUD hits the original imported bone target");return point;}
POINT Press(MMDApp& app,HWND owner,int operation){const auto hit=Hit(app,operation);MoveCursor(owner,hit);SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(hit.x,hit.y));
    const auto point=ClientCursor(owner);const auto view=app.ViewportRect();Check(plugins::ModelHudGestureActive(app)&&GetCapture()==owner&&
        int(app.InteractionDragMode())==(operation==12?10:20)&&int(app.ViewToolDragOperation())==operation&&point.x==(view.left+view.right)/2&&point.y==(view.top+view.bottom)/2&&
        app.MouseX()==point.x&&app.MouseY()==point.y&&app.PreviousMouseX()==point.x&&app.PreviousMouseY()==point.y,
        "actual main/Mic model HUD press reserves history, selects original axis mode and warps OS/current/previous cursor together");return hit;}
void Release(MMDApp& app,HWND owner,POINT hit){auto final=ClientCursor(owner);final.y+=7;MoveCursor(owner,final);
    SendMessageW(owner,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(hit.x,hit.y));Check(PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(hit.x,hit.y))!=FALSE,"queue original stale HUD release coordinates after moving the real OS cursor");
    const bool restored=GetForegroundWindow()==owner;DispatchQueued(owner,WM_LBUTTONUP);const auto actual=ClientCursor(owner),expected=restored?hit:final;
    Check(actual.x==expected.x&&actual.y==expected.y&&!plugins::ModelHudGestureActive(app)&&GetCapture()!=owner&&app.InteractionDragMode()==ViewportDragMode::None,
        "actual model HUD release uses final OS pixels and restores the owner button cursor only when that viewport owns foreground/capture");}
void Preview(MMDApp& app,int operation){app.MouseY()=app.PreviousMouseY()+3;
    if(operation==12)ModePhysicsBody(&app,0);else Check(plugins::ApplyModelHudSample(app,operation)==MMS_OK,"actual private model scale preview uses its owned gesture");}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    Check(app&&app->ModelSlot(0)&&!history.UndoCount()&&!history.RedoCount()&&!GetCapture()&&plugins::BoundNativePoseHistory(app)==&history,
        "real model HUD starts with actual imported PMX/Chinese-renamed PMD, empty journal and the manager binding");const auto initial=value();
    {InputRestore input(*app);model_viewport_host_checks::Original original(*app,history);model_viewport_host_checks::Windows windows(*app);ModelFixture model(*app,history);
        const auto stable=[&]{model.Stable();};const auto plugin=[&](const wchar_t* label){Menu(app,label);stable();};
        const auto command=[&](unsigned id){SendMessageW(app->MainWindow(),WM_COMMAND,id,0);stable();};
        for(bool detached:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation:{12,23}){
            model.Reset();const auto owner=windows.Select(detached);Build(*app,operation);Check(history.SetMode(mode)==MMS_OK,"configure real model HUD recording mode");
            const Pose before(model.raw);const auto point=Hit(*app,operation);MoveCursor(owner,point);plugin(L"Edit probe prepared");
            SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));SendMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
            Check(!plugins::ModelHudGestureActive(*app)&&before.Same(model.raw)&&!history.UndoCount()&&value()==initial,
                "real pending plugin edit blocks model HUD before any author write or legacy ring fallback");plugin(L"Edit probe cancel");
            Build(*app,operation);plugin(L"Edit probe once");const auto pressed=Press(*app,owner,operation);Release(*app,owner,pressed);const Pose after(model.raw);
            Check(!before.Same(model.raw),"real model HUD changes actual imported bone pose or scale");
            if(operation==12){const float expected=before.values[model.bone][0]+float(-7.0*0.05000000074505806);
                Check(model.model.boneTable[model.bone].trans[0]==expected&&model.model.boneTable[model.bone].trans[1]==before.values[model.bone][1]&&
                    !std::memcmp(model.model.boneTable[model.bone].rotQuat,before.values[model.bone].data()+3,4*sizeof(float)),
                    "real imported global-X HUD uses original world-to-parent float math and preserves independent unregistered Y/quaternion");}
            else Check(after.scale!=before.scale&&after.values==before.values,"real imported scale-X HUD changes actual scale matrices while preserving unregistered translation and quaternion");
            Check(history.UndoCount()==(mode==Mode::AllOperations?2u:1u)&&value()==initial+1,"real model HUD joins the same plugin/native journal once under its selected adjustment policy");stable();
            plugin(L"Edit probe once");command(400);Check(value()==initial+1&&history.RedoCount()==1,"native undo reverses later plugin transaction before the real model HUD node");
            Build(*app,operation);const auto empty=Hit(*app,operation);MoveCursor(owner,empty);
            Check(PostMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(empty.x,empty.y))&&PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(empty.x,empty.y)),"queue actual empty model HUD down/up before its centre warp");
            DispatchQueued(owner,WM_LBUTTONDOWN);DispatchQueued(owner,WM_LBUTTONUP);
            Check(after.Same(model.raw)&&history.RedoCount()==1&&history.UndoCount()==(mode==Mode::AllOperations?2u:1u),"real empty HUD centre-warp click preserves later plugin redo and every unregistered pose value");
            const float foreign=model.model.boneTable[model.bone].trans[1]+8.125f;model.model.boneTable[model.bone].trans[1]=foreign;model.model.boneTable[model.bone].rotQuat[2]=.125f;
            Pose expectedBefore=before,expectedAfter=after;for(auto* expected:{&expectedBefore,&expectedAfter}){expected->values[model.bone][1]=foreign;expected->values[model.bone][5]=.125f;}
            app->CurrentFrame()=53;app->CameraMode()=1;app->state.coordinateSystem=2;app->SelectedModelSlot()=std::uint8_t(model_viewport_host_checks::OtherSelection(original));
            if(mode==Mode::AllOperations){plugin(L"Edit probe undo");Check(expectedBefore.Same(model.raw)&&value()==initial+1,"real plugin undo writes only its captured HUD field and preserves external Y and unregistered quaternion after frame/editor/model switches");}
            plugin(L"Edit probe undo");Check(value()==initial&&!history.UndoCount(),"earlier real plugin node remains independently undoable before model HUD");
            plugin(L"Edit probe redo");if(mode==Mode::AllOperations)command(401);
            Check(expectedAfter.Same(model.raw)&&app->CurrentFrame()==53&&app->CameraMode()==1&&value()==initial+1,"native model HUD redo owns sampled bone/scale values independently from current editor/playhead and external pose fields");
            plugin(L"Edit probe redo");Check(value()==initial+2,"following plugin record redoes after model HUD without merging transactions");
            command(400);if(mode==Mode::AllOperations)plugin(L"Edit probe undo");command(400);
            Check(value()==initial&&!history.UndoCount()&&history.Reset()==MMS_OK,"return real plugin baseline before retiring complete model HUD history");history.Collect();stable();
            model.Reset();Build(*app,operation);const Pose cancelled(model.raw);Press(*app,owner,operation);Preview(*app,operation);
            auto expected=cancelled;model.model.boneTable[model.bone].trans[1]+=4;expected.values[model.bone][1]=model.model.boneTable[model.bone].trans[1];expected.dirty[model.bone]=1;
            SendMessageW(owner,WM_CANCELMODE,0,0);
            Check(!plugins::ModelHudGestureActive(*app)&&expected.Same(model.raw)&&!history.UndoCount()&&GetCapture()!=owner,
                "real model cancellation restores only still-owned pose/scale, preserves external Y and the foreign writer's dirty flag");stable();
            model.Reset();Build(*app,operation);const Pose captureBefore(model.raw);const auto old=Press(*app,owner,operation);Preview(*app,operation);
            Check(PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(old.x,old.y))!=FALSE,"queue real old-owner model release before capture takeover");const auto stealer=detached?app->MainWindow():windows.detached;SetCapture(stealer);
            Check(!plugins::ModelHudGestureActive(*app)&&captureBefore.Same(model.raw)&&GetCapture()==stealer,"actual main/Mic capture transfer cancels model HUD without keeping an unrecorded preview");
            DispatchQueued(owner,WM_LBUTTONUP);Check(GetCapture()==stealer&&captureBefore.Same(model.raw)&&!history.UndoCount(),"queued old model release cannot release new capture or commit cancelled history");ReleaseCapture();stable();
            std::printf("TRACE real model HUD owner=%s operation=%d bone=%u historyMode=%u final-native-delta=7\n",detached?"MicWndProc":"MainWndProc",operation,model.bone,unsigned(mode));
        }
    }
    Check(value()==initial&&!history.UndoCount()&&!history.RedoCount(),"real model HUD restores original imported model/runtime/authoring/input and plugin history before later probes");
    std::puts("PASS actual imported PMX/PMD model HUD move-X/scale-X, main/Mic cursor, native/plugin shared history, unrelated unregistered pose, cancellation and capture in both recording modes");
}
}
