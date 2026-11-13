// MAINTENANCE: regression-keep; unmatched legacy pose rows never become bone 0.
// Both profiles exercise the real unbound VPD loader, paste command and native
// undo/redo implementation. Original reset checks create hidden native controls;
// no renderer device or plugin runtime starts.
#define NOMINMAX
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/panel_controls.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include "mikumikustudio/vpd_scale_io.hpp"
#if !MMD_ORIGINAL_PROFILE
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/native_edit_history.hpp"
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace allocation_fault {
thread_local long remaining=-1;
// Stop fault injection after the real target pose begins publication. This
// sweep tests before-images and side-table preparation, not later UI refresh.
thread_local const float* targetX=nullptr;
thread_local float beforeX=0;
void* Allocate(std::size_t bytes){
    if(remaining>=0&&targetX&&*targetX!=beforeX)remaining=-1;
    if(remaining==0)throw std::bad_alloc();
    if(remaining>0)--remaining;
    if(auto* result=std::malloc(bytes?bytes:1))return result;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t bytes){return allocation_fault::Allocate(bytes);}
void* operator new[](std::size_t bytes){return allocation_fault::Allocate(bytes);}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete[](void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}
void operator delete[](void* value,std::size_t)noexcept{std::free(value);}

namespace mikumikustudio { void CmdControl450(MMDApp*, HWND, std::uint16_t, std::uint16_t); }
using namespace mikumikustudio;
namespace {
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value){std::printf("FAIL %s\n",message);std::exit(1);}}
struct PoseCopy { char name[20]{};float position[3]{},rotation[4]{0,0,0,1}; };
static_assert(sizeof(PoseCopy)==48,"native pose clipboard stride");
PoseCopy Copy(const char* name,float x){PoseCopy result;strcpy_s(result.name,name);result.position[0]=x;
    result.position[1]=2;result.position[2]=-3;result.rotation[0]=.125f;result.rotation[1]=.25f;
    result.rotation[2]=-.375f;result.rotation[3]=.875f;return result;}
struct Pose {
    std::array<float,3> position{};std::array<float,4> rotation{};unsigned char dirty=0;
    bone_scale::Matrix scale;
    bool operator==(const Pose& other)const{return position==other.position&&rotation==other.rotation&&dirty==other.dirty&&scale==other.scale;}
};
constexpr unsigned boneCount=5;
struct Fixture {
    std::unique_ptr<MMDApp> app=std::make_unique<MMDApp>();
    std::unique_ptr<D3DRenderer> renderer=std::make_unique<D3DRenderer>();
    std::unique_ptr<mdl::ModelRecord> model=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,boneCount> bones{};
    std::array<unsigned char,boneCount> selected{},dirty{};
    std::array<std::uint32_t,boneCount> boneRows{};std::array<std::uint32_t,1> morphRows{};
    MMDApp* previous=g_Block;
    Fixture(){
        app->state=MMDAppState{};std::memset(renderer.get(),0,sizeof(*renderer));app->Renderer()=renderer.get();
        app->ModelSlot(0)=Raw();app->EnglishUI()=1;app->CurrentFrame()=13;app->PhysicsResetPending()=0;
        model->boneCount=boneCount;model->boneTable=bones.data();model->boneSelection=selected.data();model->bonePhysicsState=dirty.data();
        model->boneKeyIndices=boneRows.data();model->morphKeyIndices=morphRows.data();model->morphCount=0;
        model->boneKeys=mdl::BoneKeyPoolCreate(32);model->morphKeys=mdl::KeyPoolCreate<mdl::MorphKey>(1);
        model->displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(1);model->physicsMode=2;
        const char* names[]={"sentinel","match","\x8d\xb6" "arm","\x89\x45" "arm","spare"};
        for(unsigned i=0;i<boneCount;++i){auto& bone=bones[i];strcpy_s(bone.name,names[i]);bone.slotIndex=-1;
            bone.trans[0]=float(i)+3.5f;bone.trans[1]=float(i)-8.75f;bone.trans[2]=float(i)*2.125f;
            bone.rotQuat[0]=.125f;bone.rotQuat[1]=-.25f;bone.rotQuat[2]=.375f;bone.rotQuat[3]=.875f;
            dirty[i]=static_cast<unsigned char>(7+i);model->boneKeys[i].rotation[3]=1;}
        bones[0].trans[0]=12.5f;selected[0]=1;model->selectedBone=0;g_Block=app.get();
        bone_scale::CopyClipboard(app.get(),0,{},true);
    }
    ~Fixture(){
        g_Block=previous;bone_scale::Forget(Raw());
        for(auto& ring:model->undoRings)for(auto& entry:ring.slots){undo_storage::Free(entry.bonePose);undo_storage::Free(entry.auxiliaryPose);}
        mdl::BoneKeyPoolDestroy(model->boneKeys);mdl::KeyPoolDestroy(model->morphKeys);mdl::KeyPoolDestroy(model->displayKeys);
    }
    unsigned char* Raw(){return reinterpret_cast<unsigned char*>(model.get());}
    Pose Get(unsigned index)const{Pose value;std::memcpy(value.position.data(),bones[index].trans,12);
        std::memcpy(value.rotation.data(),bones[index].rotQuat,16);value.dirty=dirty[index];value.scale=bone_scale::Pose(reinterpret_cast<const unsigned char*>(model.get()),int(index));return value;}
    std::array<Pose,boneCount> Poses()const{std::array<Pose,boneCount> value;for(unsigned i=0;i<boneCount;++i)value[i]=Get(i);return value;}
    PoseCopy Existing(unsigned index,bool mirror=false)const{PoseCopy value;strcpy_s(value.name,bones[index].name);
        std::memcpy(value.position,bones[index].trans,12);std::memcpy(value.rotation,bones[index].rotQuat,16);
        if(mirror){value.position[0]=-value.position[0];value.rotation[1]=-value.rotation[1];value.rotation[2]=-value.rotation[2];}return value;}
    void Paste(std::vector<PoseCopy>& source,unsigned command){app->BoneCopyRecords()=source.data();app->state.copiedBoneCount=int(source.size());CmdControl450(app.get(),nullptr,std::uint16_t(command),0);}
    void Undo(){const auto cursor=model->undoState[0];UndoModelEdit(Raw(),&app->CurrentFrame());
        Check(model->undoState[0]==(cursor+29)%30&&app->CurrentFrame()==13,"real legacy pose undo consumes one record without seeking");}
    void Redo(){const auto cursor=model->undoState[0];RedoModelEdit(Raw(),&app->CurrentFrame());
        Check(model->undoState[0]==(cursor+1)%30&&app->CurrentFrame()==13,"real legacy pose redo reapplies one record without seeking");}
    void SnapshotTargets(const std::vector<int>& expected){const auto& entry=model->undoRings[0].slots[model->undoState[0]];
        Check(entry.operation==1&&entry.dirty==int(expected.size()),"pose history contains exactly the unique matched targets");
        for(std::size_t i=0;i<expected.size();++i)Check(entry.bonePose[i].boneIndex==expected[i],"dense pose snapshots contain no zero-filled unmatched row");}
};
struct State {
    std::vector<unsigned char> bytes;std::array<Pose,boneCount> poses;int physicsReset=0;
    explicit State(Fixture& f):poses(f.Poses()),physicsReset(f.app->PhysicsResetPending()){
        const auto append=[&](const void* data,std::size_t size){if(!size)return;const auto* begin=static_cast<const unsigned char*>(data);bytes.insert(bytes.end(),begin,begin+size);};
        append(f.model.get(),sizeof(*f.model));append(f.selected.data(),f.selected.size());append(f.dirty.data(),f.dirty.size());
        for(const auto& ring:f.model->undoRings)for(const auto& entry:ring.slots)if(entry.bonePose&&entry.dirty>0)
            append(entry.bonePose,std::size_t(entry.dirty)*sizeof(mdl::BonePoseSnapshot));
    }
    void Unchanged(Fixture& f,const char* why)const{const State now(f);Check(bytes==now.bytes&&poses==now.poses&&physicsReset==now.physicsReset,why);}
};
Pose Applied(const PoseCopy& source,bool mirror){Pose target;std::memcpy(target.position.data(),source.position,12);std::memcpy(target.rotation.data(),source.rotation,16);
    if(mirror){target.position[0]=-target.position[0];target.rotation[1]=-target.rotation[1];target.rotation[2]=-target.rotation[2];}target.dirty=1;return target;}
void PasteMatches(){
    for(unsigned command:{497u,498u})for(bool duplicates:{false,true}){
        Fixture f;const auto before=f.Poses();std::vector<PoseCopy> copied{Copy("missing",99),Copy("match",7),Copy("absent",-99)};
        if(duplicates)copied.push_back(Copy("match",11));const auto expected=Applied(copied[duplicates?3:1],command==498);
        f.Paste(copied,command);f.SnapshotTargets({1});
        Check(f.Get(0)==before[0]&&f.Get(1)==expected,"partial ordinary/mirror paste edits its real target and leaves bone zero intact");
        for(unsigned i=2;i<boneCount;++i)Check(f.Get(i)==before[i],"partial pose paste leaves every other unmatched bone intact");
        for(int replay=0;replay<2;++replay){f.Undo();Check(f.Poses()==before,"duplicate input and missing rows undo to the original pose once per target");
            f.Redo();Check(f.Get(0)==before[0]&&f.Get(1)==expected,"redo restores the final matching pose without touching bone zero");}
    }
    for(unsigned command:{497u,498u}){
        Fixture f;const auto before=f.Poses();std::vector<PoseCopy> copied{Copy("missing",99),Copy(f.bones[2].name,7),Copy(f.bones[3].name,11)};
        const auto left=Applied(copied[command==498?2:1],command==498),right=Applied(copied[command==498?1:2],command==498);
        f.Paste(copied,command);f.SnapshotTargets({2,3});
        Check(f.Get(2)==left&&f.Get(3)==right&&f.Get(0)==before[0],"mirror paste keeps native left/right resolution and sign changes");
        f.Undo();Check(f.Poses()==before,"left/right pose undo restores both actual targets");f.Redo();
        Check(f.Get(2)==left&&f.Get(3)==right,"left/right pose redo restores mirrored or ordinary target values");
    }
}
void PasteNoop(){
    for(unsigned command:{497u,498u})for(int kind=0;kind<4;++kind){
        Fixture f;const auto before=f.Poses();std::vector<PoseCopy> changed{Copy("match",7)};f.Paste(changed,command);f.Undo();
        Check(f.Poses()==before,"prepare a real redo entry before a no-op paste");
        std::vector<PoseCopy> copied;
        if(kind==0)copied={Copy("missing",3),Copy("absent",4)};
        else if(kind==1)copied={f.Existing(1,command==498)};
        else if(kind==2)copied={Copy("match",99),f.Existing(1,command==498)};
        // kind 3 is an empty clipboard; every no-op preserves redo ownership.
        const State saved(f);f.Paste(copied,command);
        saved.Unchanged(f,"unmatched, identical, net-zero or empty paste preserves native ring, redo, selection and pose");
        f.Redo();Check(f.Get(1)==Applied(changed[0],command==498)&&f.Get(0)==before[0],"redo remains usable after a no-op ordinary/mirror paste");
    }
    for(unsigned command:{497u,498u}){
        Fixture f;const State saved(f);std::vector<PoseCopy> copied{Copy("missing",7)};f.Paste(copied,command);
        saved.Unchanged(f,"fully unmatched paste creates no history entry in an initially empty ring");
    }
}
struct Vpd {
    std::wstring path;
    explicit Vpd(const std::vector<PoseCopy>& rows){wchar_t folder[MAX_PATH]{},name[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH,folder)!=0&&GetTempFileNameW(folder,L"mlp",0,name)!=0,"create a private legacy VPD test path");path=name;
        FILE* file=nullptr;Check(_wfopen_s(&file,path.c_str(),L"wb")==0&&file,"open private VPD fixture");
        std::fprintf(file,"Vocaloid Pose Data file\n\nother.osm;\n%zu;\n\n",rows.size());
        for(std::size_t i=0;i<rows.size();++i){const auto& row=rows[i];
            std::fprintf(file,"Bone%zu{%s\n  %.9g,%.9g,%.9g;\n  %.9g,%.9g,%.9g,%.9g;\n}\n\n",i,row.name,
                double(row.position[0]),double(row.position[1]),double(row.position[2]),double(row.rotation[0]),double(row.rotation[1]),double(row.rotation[2]),double(row.rotation[3]));}
        Check(std::fclose(file)==0,"finish complete standard VPD fixture");}
    ~Vpd(){Check(DeleteFileW(path.c_str())!=FALSE,"remove only the owned legacy VPD fixture");}
    void Load(){LoadVpdFile(path.c_str());}
};
void VpdMatches(){
    for(bool duplicateNames:{false,true})for(bool repeated:{false,true}){
        Fixture f;if(duplicateNames)strcpy_s(f.bones[4].name,"match");const auto before=f.Poses();
        std::vector<PoseCopy> rows{Copy("missing",99),Copy("match",7),Copy("absent",-99)};
        if(repeated)rows.push_back(Copy("match",11));const auto expected=Applied(rows[repeated?3:1],false);
        Vpd pose(rows);pose.Load();f.SnapshotTargets(duplicateNames?std::vector<int>{1,4}:std::vector<int>{1});
        Check(f.Get(1)==expected&&f.Get(0)==before[0],"partial VPD matches actual target while preserving bone-zero sentinel");
        if(duplicateNames)Check(f.Get(4)==expected,"VPD retains the original rule that every same-named model bone receives the pose");
        for(int replay=0;replay<2;++replay){f.Undo();Check(f.Poses()==before,"VPD undo restores unique targets before all repeated input rows");
            f.Redo();Check(f.Get(1)==expected&&f.Get(0)==before[0]&&(!duplicateNames||f.Get(4)==expected),"VPD redo restores all actual matching targets without empty snapshots");}
    }
    Fixture f;strcpy_s(f.bones[4].name,"match");const auto before=f.Poses();Vpd one({Copy("match",7)});one.Load();
    f.SnapshotTargets({1,4});f.Undo();Check(f.Poses()==before,"one VPD row matching several model bones has enough unique undo storage");
}
void VpdNoop(){
    for(int kind=0;kind<4;++kind){
        Fixture f;const auto before=f.Poses();Vpd changed({Copy("match",7)});changed.Load();f.Undo();
        std::vector<PoseCopy> rows;
        if(kind==0)rows={Copy("missing",3),Copy("absent",4)};
        else if(kind==1)rows={f.Existing(1)};
        else if(kind==2)rows={Copy("match",99),f.Existing(1)};
        Vpd pose(rows);const State saved(f);pose.Load();
        saved.Unchanged(f,"unmatched, identical, net-zero or zero-count VPD preserves native redo and all bone state");
        f.Redo();Check(f.Get(1)==Applied(Copy("match",7),false)&&f.Get(0)==before[0],"redo remains usable after a no-op VPD import");
    }
    Fixture f;Vpd pose({Copy("missing",7)});const State saved(f);pose.Load();
    saved.Unchanged(f,"fully unmatched VPD creates no empty history entry or selection change");
}
#if !MMD_ORIGINAL_PROFILE
void PreparationFailures(){
    for(unsigned operation=0;operation<3;++operation){
        Fixture f;bone_curve::Tracks authored;
        for(unsigned c=0;c<authored.size();++c){animation_curve::Key first,last;first.frame=0;last.frame=20;
            first.value=c<3?f.bones[1].trans[c]:c<6?0:1;last.value=first.value+(c==0?8:0);
            first.tangent=last.tangent=animation_curve::Tangent::Free;
            first.outSlope=.013;last.inSlope=-.017;
            if(c>=6)first.outSlope=last.inSlope=0;
            authored[c].keys={first,last};}
        std::string message;Check(bone_curve::Write(f.Raw(),1,authored,message),"seed actual Free authoring before testing undo-side-table allocation failures");
        bone_scale::Matrix scale;scale.v[0]=1.25f;
        Check(bone_scale::SetPose(f.Raw(),1,scale),"seed a nonempty scale before-image for allocation-failure sweep");
        const auto before=f.Poses();Vpd first({Copy("match",7)});first.Load();f.Undo();
        Check(f.Poses()==before,"allocation-failure fixture has a real available redo with Free and Scale side tables");
        std::vector<PoseCopy> copied{Copy("missing",99),Copy("match",9)};Vpd second(copied);
        unsigned failures=0;bool completed=false;
        for(long allowance=0;allowance<2048;++allowance){
            const State saved(f);
            auto scaleWitness=bone_scale::PrepareHistory(f.Raw(),{});
            auto curveWitness=bone_curve::PrepareHistory(f.Raw(),{});
            Check(scaleWitness->IsCurrent()&&curveWitness->IsCurrent(),"capture complete before/undo/redo side-table witnesses without publishing them");
            bool failed=false;
            allocation_fault::targetX=&f.bones[1].trans[0];allocation_fault::beforeX=f.bones[1].trans[0];
            allocation_fault::remaining=allowance;
            try{if(operation==2)second.Load();else f.Paste(copied,operation?498:497);}
            catch(const std::bad_alloc&){failed=true;}
            catch(...){allocation_fault::remaining=-1;allocation_fault::targetX=nullptr;Check(false,"legacy pose preparation reports allocation failures without a different exception");}
            allocation_fault::remaining=-1;allocation_fault::targetX=nullptr;
            if(!failed){completed=true;f.SnapshotTargets({1});break;}
            ++failures;saved.Unchanged(f,"failed pose preparation preserves native undo/redo buffers, pose, selection and cursor");
            Check(scaleWitness->IsCurrent()&&curveWitness->IsCurrent(),"failed preparation preserves complete Scale and Free current/undo/redo side tables");
            f.Redo();Check(f.Get(1)==Applied(Copy("match",7),false),"previous native redo remains executable after each preparation allocation failure");
            f.Undo();Check(f.Poses()==before,"previous native undo still restores its original Scale pose after allocation failure");
        }
        Check(completed&&failures>8,"allocation sweep reaches the nonempty Free/Scale preparation and then a successful pose publication");
        f.Undo();Check(f.Poses()==before,"successful publication after allocation failures still owns a valid original pose snapshot");
        std::printf("legacy pose preparation operation=%u failed-allocations=%u\n",operation,failures);
    }
}
void ScaleMatching(){
    bone_scale::Matrix scale;scale.v={1.25f,.125f,0,0,1.5f,.25f,0,0,.75f};
    for(unsigned command:{497u,498u}){
        Fixture f;const auto before=f.Poses();std::vector<PoseCopy> copied{f.Existing(1,command==498)};
        bone_scale::CopyClipboard(f.app.get(),0,scale,true);f.Paste(copied,command);f.SnapshotTargets({1});
        const auto expected=bone_scale::Clipboard(f.app.get(),0,true,command==498);
        Check(f.Get(1).position==before[1].position&&f.Get(1).rotation==before[1].rotation&&f.Get(1).scale==expected,
            "scale-only ordinary/mirror paste remains a real edit even when translation and quaternion match");
        f.Undo();Check(f.Poses()==before,"scale-only paste undo restores native pose and its original scale");f.Redo();
        Check(f.Get(1).scale==expected,"scale-only paste redo retains the clipboard scale and mirror policy");
    }
    {
        Fixture f;Check(bone_scale::SetPose(f.Raw(),1,scale),"seed an existing scale before ordinary VPD");const auto before=f.Poses();
        Vpd pose({f.Existing(1)});pose.Load();f.SnapshotTargets({1});
        Check(bone_scale::Identity(f.Get(1).scale),"standard VPD without a tail still resets residual scale even when all scalar fields match");
        f.Undo();Check(f.Poses()==before,"standard VPD undo restores the old residual scale");f.Redo();
        Check(bone_scale::Identity(f.Get(1).scale),"standard VPD redo restores the scale reset");
    }
    {
        Fixture f;Check(bone_scale::SetPose(f.Raw(),1,scale),"seed an existing scale for VPD tail no-op");Vpd pose({f.Existing(1)});
        vpd_scale_io::Entry entry;entry.bone=1;std::memcpy(entry.name.data(),f.bones[1].name,20);entry.scale=scale;
        Check(vpd_scale_io::AppendTail(pose.path.c_str(),{entry})==vpd_scale_io::TailSave::Written,"append an unchanged scale using the production VPD tail codec");
        const State saved(f);pose.Load();saved.Unchanged(f,"matching VPD tail with identical pose and scale creates no history or transient scale reset");
    }
}
#endif
#if MMD_ORIGINAL_PROFILE
void OriginalReset() {
    Fixture f;
    HWND owner = CreateWindowW(L"STATIC", L"reset fixture", WS_OVERLAPPED, 0, 0, 300, 100,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "create reset owner");
    f.app->Hwnd() = owner;
    std::array<HWND, 6> edits{};
    for (int i = 0; i < 6; ++i) {
        edits[i] = CreateWindowW(L"EDIT", L"draft", WS_CHILD, 0, 0, 20, 20, owner,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kReadoutPosXEdit + i)), GetModuleHandleW(nullptr), nullptr);
        Check(edits[i] != nullptr, "create reset draft field");
        SendMessageW(edits[i], EM_SETMODIFY, TRUE, 0);
    }
    f.selected = {1, 1, 1, 0, 0};
    f.bones[1].parent = -1;
    f.bones[2].parent = 1;
    bone_scale::Matrix scaled{}; scaled.v[0] = 2.0f; scaled.v[4] = 0.5f;
    // The original provider deliberately owns no scale side tables. Verify
    // this capability boundary instead of requiring full-profile scale state.
    for (unsigned i : {0u, 2u}) {
        Check(!bone_scale::SetPose(f.Raw(), int(i), scaled) && bone_scale::Identity(f.Get(i).scale),
              "original profile rejects non-identity selected scale and retains identity");
    }
    std::array<unsigned char, sizeof(mdl::BoneKey) * boneCount> originalKeys{};
    std::memcpy(originalKeys.data(), f.model->boneKeys, originalKeys.size());
    std::array<bone_scale::Matrix, boneCount> originalScaleKeys{};
    for (unsigned i = 0; i < boneCount; ++i) originalScaleKeys[i] = bone_scale::Key(f.Raw(), int(i));
    const auto originalRows = f.boneRows;
    bone_scale::SetKey(f.Raw(), 1, scaled);
    Check(bone_scale::Key(f.Raw(), 1) == originalScaleKeys[1] && bone_scale::Identity(originalScaleKeys[1]),
          "original profile does not publish non-identity scale keys");
    const auto checkKeys = [&] {
        Check(!std::memcmp(f.model->boneKeys, originalKeys.data(), originalKeys.size()) && f.boneRows == originalRows,
              "reset and history preserve all five keyframe records and track indices");
        for (unsigned i = 0; i < boneCount; ++i)
            Check(bone_scale::Key(f.Raw(), int(i)) == originalScaleKeys[i],
                  "reset and history preserve each original scale key");
    };
    checkKeys();
    const auto before = f.Poses();
    f.app->SceneModified() = 0;
    CmdControl450(f.app.get(), owner, 495, 0);
    Check(f.app->SceneModified() == 1, "reset marks project modified");
    for (auto edit : edits) Check(!SendMessageW(edit, EM_GETMODIFY, 0, 0), "reset cancels edit draft");
    for (unsigned i : {0u, 1u, 2u}) {
        Check(f.Get(i).position == std::array<float,3>{0,0,0}, "selected bone positions reset");
        Check(f.Get(i).rotation == std::array<float,4>{0,0,0,1}, "selected bone rotations reset");
        Check(f.dirty[i] == 1 && bone_scale::Identity(f.Get(i).scale), "selected bone marked and original scale remains identity");
    }
    Check(f.Get(3) == before[3] && f.Get(4) == before[4], "unselected bones remain unchanged");
    checkKeys();
    f.Undo(); Check(f.Poses() == before, "native undo restores reset pose");
    checkKeys();
    f.Redo(); Check(f.Get(0).position == std::array<float,3>{0,0,0} &&
        f.Get(1).rotation == std::array<float,4>{0,0,0,1} && bone_scale::Identity(f.Get(0).scale) &&
        bone_scale::Identity(f.Get(2).scale), "native redo reapplies reset pose and preserves original identity scales");
    checkKeys();
    f.app->Hwnd() = nullptr;
    DestroyWindow(owner);
}
void OriginalResetGuardsAndFloating() {
    Fixture f;
    HWND main = CreateWindowW(L"STATIC", L"main fixture", WS_OVERLAPPED, 0, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND floating = CreateWindowW(L"STATIC", L"floating fixture", WS_OVERLAPPED, 0, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(main && floating, "create main and floating owners");
    const auto makeEdit = [](HWND owner) { return CreateWindowW(L"EDIT", L"draft", WS_CHILD, 0, 0, 20, 20, owner,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(panel::kReadoutPosXEdit)), GetModuleHandleW(nullptr), nullptr); };
    HWND mainEdit = makeEdit(main), floatingEdit = makeEdit(floating);
    Check(mainEdit && floatingEdit, "create main and floating drafts");
    f.app->MainWindow() = main; f.app->FloatingWindow() = floating;
    SendMessageW(mainEdit, EM_SETMODIFY, TRUE, 0); SendMessageW(floatingEdit, EM_SETMODIFY, TRUE, 0);
    f.selected = {1, 0, 0, 0, 0};
    CmdControl450(f.app.get(), main, 495, 0);
    Check(SendMessageW(mainEdit, EM_GETMODIFY, 0, 0) && !SendMessageW(floatingEdit, EM_GETMODIFY, 0, 0),
        "reset clears floating owner draft without clearing main owner draft");
    f.selected = {};
    const auto before = f.Poses(); const auto cursor = f.model->undoState[0];
    CmdControl450(f.app.get(), main, 495, 0);
    Check(f.Poses() == before && f.model->undoState[0] == cursor, "empty selection preserves pose and undo cursor");
    SendMessageW(floatingEdit, EM_SETMODIFY, TRUE, 0); f.app->SceneModified() = 0;
    auto* bones = f.model->boneTable; auto* selection = f.model->boneSelection; auto* dirty = f.model->bonePhysicsState;
    for (int missing = 0; missing < 3; ++missing) {
        if (missing == 0) f.model->boneTable = nullptr;
        if (missing == 1) f.model->boneSelection = nullptr;
        if (missing == 2) f.model->bonePhysicsState = nullptr;
        CmdControl450(f.app.get(), main, 495, 0);
        Check(f.model->undoState[0] == cursor && f.app->SceneModified() == 0 && SendMessageW(floatingEdit, EM_GETMODIFY, 0, 0),
            "missing bone array returns before undo or draft mutation");
        f.model->boneTable = bones; f.model->boneSelection = selection; f.model->bonePhysicsState = dirty;
    }
    f.app->FloatingWindow() = nullptr; f.app->MainWindow() = nullptr;
    DestroyWindow(floating); DestroyWindow(main);
}
#endif
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);
    std::puts("legacy pose partial and duplicate paste");PasteMatches();
    std::puts("legacy pose paste no-op and redo");PasteNoop();
    std::puts("legacy VPD partial and duplicate matches");VpdMatches();
    std::puts("legacy VPD no-op and redo");VpdNoop();
#if !MMD_ORIGINAL_PROFILE
    std::puts("legacy pose scale matching");ScaleMatching();
    std::puts("legacy pose private preparation allocation failures");PreparationFailures();
#endif
#if MMD_ORIGINAL_PROFILE
    std::puts("original reset and drafts"); OriginalReset(); OriginalResetGuardsAndFloating();
#endif
    std::printf("PASS legacy pose match profile=%s checks=%u\n",MMD_ORIGINAL_PROFILE?"original":"full",checks);return 0;}
