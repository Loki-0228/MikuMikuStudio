// Include after Fixture/Scale in native_edit_history_tests.cpp; call
// NativePoseScopeChecks(). Uses hidden EDIT controls, no renderer or messages
// pumped through the user's MMS process.
#include "../src/plugins/native_pose_scope.hpp"
#include "mikudancestudio/panel_controls.hpp"
#include "mikudancestudio/d3dx_dyn.hpp"
namespace mikudancestudio { LRESULT CALLBACK EditSubclassProc(HWND,UINT,WPARAM,LPARAM); }
namespace {
struct PoseNumericFixture {
    Fixture first,second;
    mdl::AccessoryRecord accessory{};
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    plugins::StorageStore storage;
    plugins::SharedHistory history{storage};
    MMDApp* previous=g_Block;
    PoseNumericFixture() {
        // MMDApp's native constructor intentionally leaves state untouched.
        // Value-initialize in place, without a large stack temporary.
        ::new(static_cast<void*>(&app->state)) MMDAppState{};
        app->ModelSlot(0)=first.raw();app->ModelSlot(1)=second.raw();
        app->SelectedModelSlot()=0;app->WindowLayoutReady()=1;
        app->state.coordinateSystem=1;app->state.globalTrackSelected[1]=1;
        accessory.rowSelected=1;app->AccessorySlot(7)=&accessory;
        for(auto* f:{&first,&second}) {
            f->m->selectedBone=0;f->selected[0]=1;
            for(auto& bone:f->bones)for(int n=0;n<4;++n)bone.matInit[n*5]=1;
            plugins::SceneObjectIdentity(f->raw());
        }
        app->MainWindow()=CreateWindowExA(0,"STATIC","native pose numeric test",WS_OVERLAPPED,
            0,0,360,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(app->MainWindow()!=nullptr,"hidden numeric test parent created");
        for(int id:{panel::kReadoutPosXEdit,panel::kReadoutPosYEdit,panel::kReadoutPosZEdit,
            panel::kReadoutRotXEdit,panel::kReadoutRotYEdit,panel::kReadoutRotZEdit,
            panel::kReadoutScaleXEdit,panel::kReadoutScaleYEdit,panel::kReadoutScaleZEdit}) {
            const HWND edit=CreateWindowExA(0,"EDIT","0",WS_CHILD,0,0,80,20,
                app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            Check(edit!=nullptr,"hidden numeric EDIT created");
            app->OriginalEditProc()=reinterpret_cast<WNDPROC>(GetWindowLongPtrA(edit,GWLP_WNDPROC));
        }
        for(int id:{panel::kUndoButton,panel::kRedoButton}) {
            const HWND button=CreateWindowExA(0,"BUTTON","history",WS_CHILD,0,0,80,20,
                app->MainWindow(),reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            Check(button!=nullptr,"hidden numeric history button created");EnableWindow(button,FALSE);
        }
        g_Block=app.get();
    }
    ~PoseNumericFixture() {
        history.Reset();history.Collect();
        plugins::ForgetSceneObject(first.raw());plugins::ForgetSceneObject(second.raw());
        app->ModelSlot(0)=app->ModelSlot(1)=nullptr;app->AccessorySlot(7)=nullptr;
        DestroyWindow(app->MainWindow());app->MainWindow()=nullptr;g_Block=previous;
    }
    HWND Edit(int id)const{return GetDlgItem(app->MainWindow(),id);}
    void Type(int id,const char* value) {
        SetWindowTextA(Edit(id),value);SendMessageA(Edit(id),EM_SETMODIFY,TRUE,0);
    }
    void Commit(int id,const char* value) {Type(id,value);CommitEditControlTail(app.get(),Edit(id));}
    void LoseFocus(int id) {EditSubclassProc(Edit(id),WM_KILLFOCUS,reinterpret_cast<WPARAM>(app->MainWindow()),0);}
    bool SelectionUnchanged()const {
        return app->state.globalTrackSelected[0]==0&&app->state.globalTrackSelected[1]==1&&
            app->state.globalTrackSelected[2]==0&&app->state.globalTrackSelected[3]==0&&accessory.rowSelected==1;
    }
};
std::uint64_t PoseRingDigest(const Fixture& f) {
    std::uint64_t result=14695981039346656037ull;
    const auto add=[&](const void* data,std::size_t n){auto p=static_cast<const unsigned char*>(data);while(n--){result^=*p++;result*=1099511628211ull;}};
    add(f.m->undoRings,sizeof(f.m->undoRings));add(f.m->undoState,sizeof(f.m->undoState));
    add(&f.m->undoDirty,sizeof(f.m->undoDirty));add(&f.m->redoDirty,sizeof(f.m->redoDirty));
    return result;
}
void PoseScopeBudgetAndCancellation() {
    {
        Fixture f;nh::PoseEditCapture capture;std::unique_ptr<nh::OwnedPoseEdit> record;
        Check(capture.MaximumRetainedBytes()==0,"inactive pose capture has no retained reservation");
        Check(capture.Begin(f.raw())==nh::Result::Ready,"preallocated pose capture begins");
        const auto reserved=capture.MaximumRetainedBytes();
        for(int bone=0;bone<2;++bone){f.bones[bone].trans[0]=float(bone+5);bone_scale::SetPose(f.raw(),bone,Scale(float(bone+2)));}
        Check(capture.Finish(1,record)==nh::Result::Ready&&record->RetainedBytes()==reserved,
            "full pose and scale deltas fit actual preallocated payload capacities");
        failAfter=0;const bool restored=capture.Rollback();failAfter=-1;
        Check(restored&&capture.MaximumRetainedBytes()==0&&f.bones[0].trans[0]==0&&bone_scale::Identity(bone_scale::Pose(f.raw(),1)),
            "sealed pose rollback needs no allocation and releases reservation estimate");
    }
    PoseNumericFixture f;const auto original=f.first.Digest();const auto ring=PoseRingDigest(f.first);
    {
        plugins::NativePoseScope scope;
        Check(scope.Begin(*f.app,f.history,0,"cancel pose")==MMS_OK,"scope reserves before authored mutation");
        Check(f.history.Enter()==MMS_E_BUSY,"active scope excludes journal reentry");
        f.first.bones[0].trans[0]=8;f.first.dirty[0]=1;
        failAfter=0;const bool restored=scope.Cancel();failAfter=-1;
        Check(restored&&f.first.Digest()==original&&f.history.UndoCount()==0,"explicit cancellation restores all authored state without allocation");
        Check(f.history.Enter()==MMS_OK,"cancellation releases journal lock");f.history.Leave();
    }
    {
        plugins::NativePoseScope scope;bool changed=true;
        Check(scope.Begin(*f.app,f.history,0,"failed pose")==MMS_OK,"failed pose scope begins");
        f.first.bones[0].trans[0]=8;f.first.dirty[0]=1;
        failAfter=0;const auto result=scope.Commit(changed);failAfter=-1;
        Check(result==MMS_E_MEMORY&&!changed&&f.first.Digest()==original&&f.history.UndoCount()==0,
            "inverse preparation allocation failure rolls authored pose back before unlocking");
    }
    {
        plugins::NativePoseScope scope;
        Check(scope.Begin(*f.app,f.history,0,"uncommitted pose")==MMS_OK,"RAII cancellation scope begins");
        f.first.bones[0].rotQuat[1]=.6f;f.first.bones[0].rotQuat[3]=.8f;
    }
    Check(f.first.Digest()==original&&PoseRingDigest(f.first)==ring,"scope destruction restores quaternion and leaves native ring untouched");
    {
        plugins::NativePoseScope scope;bool changed=true;
        Check(scope.Begin(*f.app,f.history,0,"no-op")==MMS_OK&&scope.Commit(changed)==MMS_OK&&!changed,
            "unchanged scope does not append history");
    }
    bone_scale::SetPose(f.first.raw(),0,Scale(2));
    float before[3]{};Check(SelectedBoneScaleReadout(f.app.get(),before)&&before[0]==2,"nonuniform scale fixture readout");
    {
        plugins::NativePoseScope scope;bool changed=true;
        Check(scope.Begin(*f.app,f.history,0,"failed scale")==MMS_OK,"nonuniform scale capture begins");
        Check(SetSelectedBoneScaleAxis(f.app.get(),0,3),"real scale author changes existing nonuniform pose");
        failAfter=0;const auto result=scope.Commit(changed);failAfter=-1;
        Check(result==MMS_E_MEMORY&&!changed&&bone_scale::Pose(f.first.raw(),0)==Scale(2),"Finish allocation failure restores existing scale authoring");
        float after[3]{};Check(SelectedBoneScaleReadout(f.app.get(),after)&&after[0]==before[0]&&after[1]==before[1]&&after[2]==before[2],
            "failed scale readout rebuilds the original nonuniform transform");
        Check(f.first.bones[0].matInit[0]==2&&f.first.bones[0].matInit[5]==1&&f.first.bones[0].matInit[10]==1,
            "rollback scale cache rebuild matches original authored matrix");
    }
    Check(PoseRingDigest(f.first)==ring&&f.history.UndoCount()==0&&f.app->SceneModified()==0,
        "cancelled scopes preserve ring journal and UI dirty metadata");
}
void PoseNumericEntryChecks() {
    {
        PoseNumericFixture f;
        Check(plugins::BoundNativePoseHistory(f.app.get())==nullptr,"numeric production path remains unbound by default");
        const auto cursor=f.first.m->undoState[0];f.Commit(panel::kReadoutPosXEdit,"5");
        Check(f.first.bones[0].trans[0]==5&&f.first.m->undoState[0]==(cursor+1)%30&&f.history.UndoCount()==0,
            "default real numeric entry still writes legacy ring only");
    }
    PoseNumericFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto ring=PoseRingDigest(f.first),otherRing=PoseRingDigest(f.second);
    f.Type(panel::kReadoutPosXEdit,"5.1234");f.LoseFocus(panel::kReadoutPosXEdit);
    const auto precise=f.first.bones[0].trans[0];
    Check(precise==5.1234f&&f.first.dirty[0]==1&&f.history.UndoCount()==1,"real loss-of-focus commits position through common journal");
    Check(f.app->SceneModified()==1&&f.app->RecordedFrameCountByte1()==1&&IsWindowEnabled(f.Edit(panel::kUndoButton)),
        "successful numeric publication updates dirty flags and undo availability");
    Check(f.SelectionUnchanged(),"numeric commit preserves global track and accessory row selection");
    char displayed[32]{};GetWindowTextA(f.Edit(panel::kReadoutPosXEdit),displayed,sizeof(displayed));
    Check(std::strcmp(displayed,"5.12")==0,"position readout uses its ordinary display precision");
    f.LoseFocus(panel::kReadoutPosXEdit);CommitEditControlTail(f.app.get(),f.Edit(panel::kReadoutPosXEdit));
    Check(f.first.bones[0].trans[0]==precise&&f.history.UndoCount()==1,"repeated unmodified focus and commit retain full precision and one history node");
    f.Commit(panel::kReadoutPosXEdit,"6.2345");
    Check(f.first.bones[0].trans[0]==6.2345f&&f.history.UndoCount()==2,"new user typing after formatted echo creates a new edit");
    Check(f.history.History(false)==MMS_OK&&f.first.bones[0].trans[0]==precise,"numeric journal undoes latest position");
    f.Commit(panel::kReadoutPosXEdit,"5.1234");
    Check(f.history.UndoCount()==1&&f.history.RedoCount()==1,"same authored value preserves existing redo");
    for(const char* invalid:{"nan","inf","5junk","","  ","1e1000"})f.Commit(panel::kReadoutPosXEdit,invalid);
    Check(f.first.bones[0].trans[0]==precise&&f.history.UndoCount()==1&&f.history.RedoCount()==1,"invalid numeric inputs preserve pose and both history branches");
    f.app->SelectedModelSlot()=1;f.Commit(panel::kReadoutPosYEdit,"9");
    Check(f.history.UndoCount()==2&&f.history.RedoCount()==0&&f.second.bones[0].trans[1]==9,"second model numeric author branches common history");
    Check(f.history.History(false)==MMS_OK&&f.second.bones[0].trans[1]==0&&f.first.bones[0].trans[0]==precise,
        "common numeric undo restores last edited model independently of selected model");
    Check(f.history.History(false)==MMS_OK&&f.first.bones[0].trans[0]==0,"earlier model numeric edit undoes second");
    Check(f.history.History(true)==MMS_OK&&f.history.History(true)==MMS_OK&&f.second.bones[0].trans[1]==9,"cross-model numeric edits redo in original order");
    Check(PoseRingDigest(f.first)==ring&&PoseRingDigest(f.second)==otherRing,"common numeric edits undo and redo never mutate either native ring");
    f.app->SelectedModelSlot()=0;const auto count=f.history.UndoCount();
    f.Commit(panel::kReadoutScaleXEdit,"2.3456");const auto scale=bone_scale::Pose(f.first.raw(),0);
    Check(scale.v[0]==2.3456f&&f.history.UndoCount()==count+1,"real scale numeric edit enters common journal");
    f.LoseFocus(panel::kReadoutScaleXEdit);CommitEditControlTail(f.app.get(),f.Edit(panel::kReadoutScaleXEdit));
    Check(bone_scale::Pose(f.first.raw(),0)==scale&&f.history.UndoCount()==count+1,"rounded scale echo does not create a second edit");
    Check(f.history.History(false)==MMS_OK&&bone_scale::Identity(bone_scale::Pose(f.first.raw(),0)),"numeric scale undo restores identity authoring");
    Check(f.history.History(true)==MMS_OK&&bone_scale::Pose(f.first.raw(),0)==scale,"numeric scale redo restores precise authored value");
    // Rotation requires the same optional D3DX math DLL as the real editor.
    if(d3dx::Get().Load()) {
        const auto rotations=f.history.UndoCount();f.Commit(panel::kReadoutRotXEdit,"0.01");
        std::array<float,4> quaternion{};std::memcpy(quaternion.data(),f.first.bones[0].rotQuat,sizeof(quaternion));
        Check(f.history.UndoCount()==rotations+1&&quaternion[0]!=0,"real rotation numeric edit retains sub-display precision");
        f.LoseFocus(panel::kReadoutRotXEdit);CommitEditControlTail(f.app.get(),f.Edit(panel::kReadoutRotXEdit));
        Check(!std::memcmp(quaternion.data(),f.first.bones[0].rotQuat,sizeof(quaternion))&&f.history.UndoCount()==rotations+1,
            "rounded rotation echo does not alter quaternion or append history");
        f.Commit(panel::kReadoutRotYEdit,"13.37");f.Commit(panel::kReadoutRotZEdit,"-7.321");
        std::memcpy(quaternion.data(),f.first.bones[0].rotQuat,sizeof(quaternion));
        for(int id:{panel::kReadoutRotXEdit,panel::kReadoutRotYEdit,panel::kReadoutRotZEdit})f.LoseFocus(id);
        Check(f.history.UndoCount()==rotations+3&&!std::memcmp(quaternion.data(),f.first.bones[0].rotQuat,sizeof(quaternion)),
            "unmodified multi-axis rotation readouts preserve quaternion and transaction count");
        std::puts("PASS native numeric rotation with D3DX math");
    } else std::puts("SKIP native numeric rotation: D3DX math library unavailable");
    Check(f.SelectionUnchanged()&&PoseRingDigest(f.first)==ring,"all numeric kinds preserve selection and legacy ring");
}
void PoseNumericFailureChecks() {
    PoseNumericFixture f;plugins::NativePoseTestBinding binding(*f.app,f.history);
    const auto before=f.first.Digest();unsigned failures=0;bool succeeded=false;
    for(long limit=0;limit<256;++limit) {
        f.Type(panel::kReadoutPosXEdit,"7.1234");failAfter=limit;
        f.LoseFocus(panel::kReadoutPosXEdit);failAfter=-1;
        if(f.history.UndoCount()) {succeeded=true;break;}
        ++failures;
        Check(f.first.Digest()==before&&f.app->SceneModified()==0&&f.app->RecordedFrameCountByte1()==0&&
            !IsWindowEnabled(f.Edit(panel::kUndoButton))&&!IsWindowEnabled(f.Edit(panel::kRedoButton)),
            "numeric allocation failure preserves native ring authored pose and UI metadata");
        Check(f.history.Enter()==MMS_OK,"numeric failure releases reservation after rollback");f.history.Leave();
        Check(f.SelectionUnchanged(),"failed numeric commit preserves scene selection");
    }
    Check(succeeded&&failures>8&&f.first.bones[0].trans[0]==7.1234f,"numeric allocation sweep reaches capture reservation and inverse preparation before success");
    Check(f.history.History(false)==MMS_OK&&f.first.bones[0].trans[0]==0,"first successful numeric edit after failures remains undoable");
    const auto redo=f.history.RedoCount();f.app->SceneModified()=0;f.app->RecordedFrameCountByte1()=0;
    f.Type(panel::kReadoutPosXEdit,"8");failAfter=0;f.LoseFocus(panel::kReadoutPosXEdit);failAfter=-1;
    Check(f.history.UndoCount()==0&&f.history.RedoCount()==redo&&f.first.bones[0].trans[0]==0&&f.app->SceneModified()==0,
        "failed numeric branch preserves earlier redo and UI dirty state");
}
}
void NativePoseScopeChecks() {PoseScopeBudgetAndCancellation();PoseNumericEntryChecks();PoseNumericFailureChecks();}
