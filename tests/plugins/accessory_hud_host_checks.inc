// Include after camera_hud_host_checks.inc, inside edit_history_host_checks.
// The aggregate includes native_accessory_hud_history.hpp at top level.
// Run while both real X objects still exist, before their removal fixture.
namespace accessory_hud_host_checks {
using Mode=plugins::SharedHistory::RecordingMode;
using camera_hud_host_checks::ClientCursor;
using camera_hud_host_checks::MoveCursor;
using camera_hud_host_checks::Hit;
using camera_hud_host_checks::DispatchQueued;
using camera_hud_host_checks::NativePreview;
using model_viewport_host_checks::Read;
using model_viewport_host_checks::Same;
float Field(const mdl::AccessoryRecord& a,unsigned field){return field<3?a.position[field]:a.rotation[field-3];}
void Field(mdl::AccessoryRecord& a,unsigned field,float value){if(field<3)a.position[field]=value;else a.rotation[field-3]=value;}
bool SameObject(const mdl::AccessoryRecord& a,const mdl::AccessoryRecord& b){return !std::memcmp(&a,&b,sizeof(a));}
struct InputRestore {
    MMDApp& app;std::array<mdl::AccessoryRecord*,2> objects;std::array<mdl::AccessoryRecord,2> values;
    POINT cursor{};bool cursorValid;const int coordinate,left,drag,operation,hovered,tool,box;
    const ViewportEditMode edit;const TimelineSelectionRow pending;const std::uint8_t selected,jumped,separate,authored;
    std::array<std::uint8_t,4> rows{};std::array<std::uint8_t,kModelSlotCount> accessoryRows{};
    explicit InputRestore(MMDApp& a):app(a),objects{a.AccessorySlot(0),a.AccessorySlot(1)},values{*objects[0],*objects[1]},cursorValid(GetCursorPos(&cursor)!=FALSE),
        coordinate(a.state.coordinateSystem),left(a.LeftMouseButtonState()),drag(int(a.InteractionDragMode())),operation(int(a.ViewToolDragOperation())),
        hovered(a.ViewportToolHovered()),tool(int(a.ViewportToolOperation())),box(a.BoneBoxSelectionActive()),edit(a.EditMode()),pending(a.PendingTimelineSelectionRow()),
        selected(a.SelectedAccessorySlot()),jumped(a.state.mouseJumped),separate(a.state.separateWindowMouseSeen),authored(a.state.a06B5){
        std::memcpy(rows.data(),a.state.globalTrackSelected,rows.size());
        for(unsigned i=0;i<kModelSlotCount;++i)if(auto* object=a.AccessorySlot(i))accessoryRows[i]=object->rowSelected;
        // Both rows are already selected, so pressing either accessory keeps
        // the model-view invariant fixture's empty global selection unchanged.
        a.ClearGlobalTimelineTrackSelection();objects[0]->rowSelected=objects[1]->rowSelected=1;
    }
    void ResetObjects()const{for(unsigned i=0;i<2;++i){Check(app.AccessorySlot(i)==objects[i],"real accessory HUD fixture retains both object addresses");
        *objects[i]=values[i];objects[i]->rowSelected=1;}}
    ~InputRestore(){plugins::CancelAccessoryHudGesture(app);if(GetCapture()==app.MainWindow()||GetCapture()==app.FloatingWindow())ReleaseCapture();
        for(unsigned i=0;i<2;++i)*objects[i]=values[i];app.SelectedAccessorySlot()=selected;
        app.state.coordinateSystem=coordinate;app.LeftMouseButtonState()=left;app.InteractionDragMode()=static_cast<ViewportDragMode>(drag);
        app.ViewToolDragOperation()=static_cast<ViewportToolAction>(operation);app.ViewportToolHovered()=hovered;
        app.ViewportToolOperation()=static_cast<ViewportToolAction>(tool);app.BoneBoxSelectionActive()=box;app.EditMode()=edit;app.PendingTimelineSelectionRow()=pending;
        app.state.mouseJumped=jumped;app.state.separateWindowMouseSeen=separate;app.state.a06B5=authored;
        std::memcpy(app.state.globalTrackSelected,rows.data(),rows.size());
        for(unsigned i=0;i<kModelSlotCount;++i)if(auto* object=app.AccessorySlot(i))object->rowSelected=accessoryRows[i];
        // Original restores readout caches and dirty/input bookkeeping first;
        // do not recompute them after that exact state restoration.
        if(cursorValid)SetCursorPos(cursor.x,cursor.y);
    }
};
template<class K>struct Pool {
    K* pointer;std::uint64_t identity;std::vector<K> records;
    explicit Pool(K* p):pointer(p),identity(mdl::KeyPoolIdentity(p)),records(p,p+mdl::KeyPoolCapacity(p)){}
    bool SameAs(K* p)const{return pointer==p&&identity==mdl::KeyPoolIdentity(p)&&records.size()==mdl::KeyPoolCapacity(p)&&
        !std::memcmp(p,records.data(),records.size()*sizeof(K));}
};
struct Resources {
    MMDApp& app;model_viewport_host_checks::View camera;
    std::array<mdl::AccessoryRecord*,2> objects;std::array<mdl::AccessoryRecord,2> values;std::array<MmsObjectRef,2> ids;
    Pool<mdl::LightKey> lights;Pool<mdl::SelfShadowKey> shadows;Pool<mdl::GravityKey> gravity;
    std::array<Pool<mdl::AccessoryKey>,2> keys;std::array<std::uint32_t,2> cursors;std::array<std::uint8_t,2> active;
    explicit Resources(MMDApp& a):app(a),camera(Read(a)),objects{a.AccessorySlot(0),a.AccessorySlot(1)},values{*objects[0],*objects[1]},
        ids{plugins::SceneObjectIdentity(objects[0]),plugins::SceneObjectIdentity(objects[1])},lights(a.LightKeys()),shadows(a.ShadowKeys()),gravity(a.GravityKeys()),
        keys{Pool<mdl::AccessoryKey>(a.AccessoryKeys(0)),Pool<mdl::AccessoryKey>(a.AccessoryKeys(1))},
        cursors{a.AccessoryTrackCursor(0),a.AccessoryTrackCursor(1)},active{a.AccessoryTrackActive(0),a.AccessoryTrackActive(1)}{}
    void CheckStable()const{
        Check(Same(camera,Read(app))&&lights.SameAs(app.LightKeys())&&shadows.SameAs(app.ShadowKeys())&&gravity.SameAs(app.GravityKeys()),
            "real accessory HUD and plugin history preserve the complete camera pose and all unrelated global key pools");
        for(unsigned i=0;i<2;++i){const auto* a=app.AccessorySlot(i);Check(a==objects[i]&&plugins::SceneObjectPresent(a,ids[i].id)&&
                plugins::SceneObjectIdentity(a).generation==ids[i].generation&&a->mesh==values[i].mesh&&a->materials==values[i].materials&&
                a->texturePaths==values[i].texturePaths&&a->textureTypes==values[i].textureTypes&&a->materialCount==values[i].materialCount&&
                keys[i].SameAs(app.AccessoryKeys(i))&&app.AccessoryTrackCursor(i)==cursors[i]&&app.AccessoryTrackActive(i)==active[i],
                "real accessory HUD retains each loaded X mesh/material/texture, stable scene identity, exact keys and sampling cursor");}
    }
};
void Build(MMDApp& a,unsigned slot){
    a.CameraMode()=1;a.SelectedAccessorySlot()=std::uint8_t(slot);a.state.coordinateSystem=2;a.EditMode()=ViewportEditMode::Camera;a.ViewportInputActive()=1;
    a.LeftMouseButtonState()=0;a.InteractionDragMode()=ViewportDragMode::None;a.ViewToolDragOperation()=ViewportToolAction::None;
    a.ShiftModifierState()=a.CtrlModifierState()=0;a.state.a06B5=0;a.state.mouseJumped=0;a.BoneBoxSelectionActive()=0;
    a.CurrentFrame()=7;a.SceneModified()=a.RecordedFrameCountByte1()=a.PhysicsResetPending()=0;a.ViewportRect()=FrameOverlayRect(&a);
    plugins::RefreshBoundPoseReadout(a);plugins::RefreshBoundGlobalReadout(a);
}
POINT Press(MMDApp& a,HWND owner,int operation){
    const auto point=Hit(a,operation);MoveCursor(owner,point);SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
    const auto centered=ClientCursor(owner);const auto& view=a.ViewportRect();
    Check(plugins::AccessoryHudGestureActive(a)&&GetCapture()==owner&&int(a.ViewToolDragOperation())==operation&&int(a.InteractionDragMode())==operation-2&&
        centered.x==(view.left+view.right)/2&&centered.y==(view.top+view.bottom)/2&&a.MouseX()==centered.x&&a.PreviousMouseX()==centered.x&&
        a.MouseY()==centered.y&&a.PreviousMouseY()==centered.y,"real main/Mic accessory HUD press owns one gesture and warps OS/native current/previous cursor to the viewport center");return point;
}
void EmptyClick(MMDApp& a,HWND owner,int operation){
    const auto point=Hit(a,operation);MoveCursor(owner,point);
    Check(PostMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y))&&PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y)),
        "queue real accessory down/up together with matching old button coordinates before the center warp");
    DispatchQueued(owner,WM_LBUTTONDOWN);Check(plugins::AccessoryHudGestureActive(a)&&GetCapture()==owner,"queued real accessory press enters its selected target");
    DispatchQueued(owner,WM_LBUTTONUP);
    Check(!plugins::AccessoryHudGestureActive(a)&&GetCapture()!=owner&&a.InteractionDragMode()==ViewportDragMode::None&&a.ViewToolDragOperation()==ViewportToolAction::None,
        "queued main/Mic accessory release completes native cursor and interaction housekeeping");
}
void Run(MMDApp* app,plugins::SharedHistory& history,int(*value)()){
    using edit_history_host_checks::Menu;
    Check(app&&app->ModelSlot(0)&&app->AccessorySlot(0)&&app->AccessorySlot(1)&&!history.UndoCount()&&!history.RedoCount()&&!GetCapture(),
        "real accessory HUD starts with loaded PMX/PMD, both real X objects, the bound empty journal and no foreign capture");
    Check(plugins::BoundNativePoseHistory(app)==&history,"real accessory HUD uses the manager's existing journal binding");const auto initial=value();
    {
        InputRestore input(*app);model_viewport_host_checks::Original original(*app,history);model_viewport_host_checks::Windows windows(*app);Resources resources(*app);
        const auto stable=[&]{original.Invariants();resources.CheckStable();};
        const auto plugin=[&](const wchar_t* label){Menu(app,label);stable();};
        const auto command=[&](unsigned id){SendMessageW(app->MainWindow(),WM_COMMAND,id,0);stable();};
        for(bool detached:{false,true})for(auto mode:{Mode::AllOperations,Mode::ExplicitOnly})for(int operation=15;operation<=20;++operation){
            const unsigned slot=unsigned(operation-15)%2,other=1-slot;input.ResetObjects();const auto owner=windows.Select(detached);Build(*app,slot);
            // Include actual attachment to the already loaded model without
            // editing/evaluating that model or rewriting accessory keys.
            app->AccessorySlot(slot)->parentModel=operation>=18?0:-1;app->AccessorySlot(slot)->parentBone=0;
            const auto before=*app->AccessorySlot(slot),unrelated=*app->AccessorySlot(other);Check(history.SetMode(mode)==MMS_OK,"configure real accessory HUD adjustment recording mode");
            auto point=Hit(*app,operation);MoveCursor(owner,point);plugin(L"Edit probe prepared");
            SendMessageW(owner,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));SendMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
            Check(!plugins::AccessoryHudGestureActive(*app)&&SameObject(*app->AccessorySlot(slot),before)&&SameObject(*app->AccessorySlot(other),unrelated)&&
                !history.UndoCount()&&value()==initial&&!app->SceneModified()&&!app->RecordedFrameCountByte1(),"real prepared plugin transaction blocks accessory HUD capture authoring without old-writer or partial history fallback");stable();plugin(L"Edit probe cancel");
            plugin(L"Edit probe once");const auto pressed=Press(*app,owner,operation);auto final=ClientCursor(owner);final.y+=7;MoveCursor(owner,final);
            // A pending move and release still carry the pre-warp point.
            SendMessageW(owner,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(pressed.x,pressed.y));
            Check(PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(pressed.x,pressed.y)),"queue real accessory release with stale HUD coordinates after actual OS cursor movement");DispatchQueued(owner,WM_LBUTTONUP);
            auto after=before;const unsigned field=unsigned(operation-15);const double scale=operation<18?0.05000000074505806:0.019999999552965164;
            Field(after,field,float(double(Field(before,field))+(operation<18?-7.0:7.0)*scale));
            Check(SameObject(*app->AccessorySlot(slot),after)&&SameObject(*app->AccessorySlot(other),unrelated)&&!plugins::AccessoryHudGestureActive(*app)&&GetCapture()!=owner&&
                history.UndoCount()==(mode==Mode::AllOperations?2u:1u)&&value()==initial+1,"real main/Mic final OS cursor delta edits only the chosen loaded accessory and joins the plugin journal once under its recording policy");stable();
            plugin(L"Edit probe once");command(400);const auto undoCount=history.UndoCount();
            Check(value()==initial+1&&history.RedoCount()==1,"native undo reverses the later real plugin edit before accessory HUD history");
            app->SceneModified()=app->RecordedFrameCountByte1()=app->PhysicsResetPending()=0;Build(*app,slot);EmptyClick(*app,owner,operation);
            Check(SameObject(*app->AccessorySlot(slot),after)&&history.UndoCount()==undoCount&&history.RedoCount()==1&&
                !app->SceneModified()&&!app->RecordedFrameCountByte1()&&!app->PhysicsResetPending(),"real empty accessory HUD gesture preserves later plugin redo and has no dirty/history side effects");stable();
            const unsigned independent=(field+1)%6;auto expectedBefore=before,expectedAfter=after;
            const float foreign=Field(*app->AccessorySlot(slot),independent)+8.125f;
            for(auto* object:{app->AccessorySlot(slot),&expectedBefore,&expectedAfter}){Field(*object,independent,foreign);object->scale=2.375f;object->opacity=.73125f;}
            app->CurrentFrame()=53;app->CameraMode()=0;app->state.coordinateSystem=0;app->SelectedAccessorySlot()=std::uint8_t(other);app->SelectedModelSlot()=0;
            if(mode==Mode::AllOperations){plugin(L"Edit probe undo");Check(SameObject(*app->AccessorySlot(slot),expectedBefore)&&value()==initial+1&&history.UndoCount()==1,
                "real plugin undo restores only the HUD axis and preserves another unregistered axis, scale and opacity on its original accessory");}
            plugin(L"Edit probe undo");Check(value()==initial&&!history.UndoCount(),"earlier real plugin node remains separately undoable before accessory HUD");
            plugin(L"Edit probe redo");if(mode==Mode::AllOperations)command(401);
            Check(SameObject(*app->AccessorySlot(slot),expectedAfter)&&SameObject(*app->AccessorySlot(other),unrelated)&&app->CurrentFrame()==53&&app->CameraMode()==0&&
                app->state.coordinateSystem==0&&app->SelectedAccessorySlot()==other&&value()==initial+1,
                "real native accessory HUD redo or explicit-only preview retains later frame/mode/object selection and every independent authored field");
            plugin(L"Edit probe redo");Check(value()==initial+2&&history.UndoCount()==(mode==Mode::AllOperations?3u:2u),"following real plugin edit remains independently redoable after accessory HUD");
            command(400);if(mode==Mode::AllOperations)plugin(L"Edit probe undo");command(400);
            Check(value()==initial&&!history.UndoCount()&&history.Reset()==MMS_OK,"restore real accessory HUD plugin baseline before retiring complete adjustment history");history.Collect();stable();
            if(operation==15||operation==18){
                input.ResetObjects();Build(*app,slot);const auto cancelled=*app->AccessorySlot(slot);Press(*app,owner,operation);NativePreview(*app,operation);
                auto expected=cancelled;Field(*app->AccessorySlot(slot),independent,Field(*app->AccessorySlot(slot),independent)+4.f);
                Field(expected,independent,Field(*app->AccessorySlot(slot),independent));app->SelectedAccessorySlot()=std::uint8_t(other);
                SendMessageW(owner,WM_CANCELMODE,0,0);
                Check(!plugins::AccessoryHudGestureActive(*app)&&SameObject(*app->AccessorySlot(slot),expected)&&app->SelectedAccessorySlot()==other&&!history.UndoCount()&&GetCapture()!=owner,
                    "real accessory cancellation restores its still-owned axis while preserving an independent pose author and the newly selected other accessory");stable();
                input.ResetObjects();Build(*app,slot);const auto captureBefore=*app->AccessorySlot(slot);const auto stolen=Press(*app,owner,operation);NativePreview(*app,operation);
                Check(PostMessageW(owner,WM_LBUTTONUP,0,MAKELPARAM(stolen.x,stolen.y)),"queue old-owner accessory mouse-up before actual capture takeover");
                const auto stealer=detached?app->MainWindow():windows.detached;SetCapture(stealer);
                Check(!plugins::AccessoryHudGestureActive(*app)&&SameObject(*app->AccessorySlot(slot),captureBefore)&&GetCapture()==stealer,"real accessory capture takeover cancels native preview and preserves the replacement window owner");
                DispatchQueued(owner,WM_LBUTTONUP);Check(GetCapture()==stealer&&SameObject(*app->AccessorySlot(slot),captureBefore)&&!history.UndoCount(),
                    "old queued main/Mic accessory mouse-up cannot steal replacement capture or republish cancelled pose");ReleaseCapture();stable();
            }
            std::printf("TRACE accessory HUD host owner=%s slot=%u operation=%d historyMode=%u final=(%ld,%ld) native-delta=7\n",detached?"MicWndProc":"MainWndProc",slot,operation,unsigned(mode),final.x,final.y);
        }
        input.ResetObjects();stable();
    }
    Check(value()==initial&&!history.UndoCount()&&!history.RedoCount(),"real accessory HUD fixture restores original object/authoring/input/history state for subsequent host probes");
    std::puts("PASS real main/Mic accessory HUD, both loaded X targets, centered/final cursor, plugin interleave, independent pose fields, cancellation and capture ownership in both recording modes");
}
}
