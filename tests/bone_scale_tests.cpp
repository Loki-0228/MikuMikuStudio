// MAINTENANCE: regression-keep — native bone scale behavior.
// Native scale math, hierarchy, legacy key registration and undo integration.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "mikumikustudio/bone_scale.hpp"
#include "mikumikustudio/bone_scale_io.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <filesystem>
#include <fstream>
#include <vector>
using namespace mikumikustudio;
namespace bs=bone_scale;
namespace {
void Check(bool ok,const char* message) { if(!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); } }
void Near(float a,float b,const char* message) { Check(std::abs(a-b)<0.001f,message); }
void Near(const bs::Matrix& a,const bs::Matrix& b,const char* message) { for(int i=0;i<9;++i) Near(a.v[i],b.v[i],message); }
bs::Matrix Diagonal(float x,float y,float z) { return {{x,0,0,0,y,0,0,0,z}}; }
void Identity(float* m) { std::memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }
std::array<char,20> FixedName(const char* name) {
    std::array<char,20> field{};
    const int size=static_cast<int>(field.size());
    for(int i=0;i<size&&name[i]!='\0';++i) field[i]=name[i];
    return field;
}
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> record=std::make_unique<mdl::ModelRecord>();
    std::vector<mdl::BoneRecord> bones;
    std::vector<mdl::BoneKey> keys=std::vector<mdl::BoneKey>(mdl::kBoneKeyCapacity);
    std::vector<mdl::MorphKey> morphs=std::vector<mdl::MorphKey>(mdl::kMorphKeyCapacity);
    std::vector<mdl::DisplayKey> displays=std::vector<mdl::DisplayKey>(mdl::kDisplayKeyCapacity);
    std::vector<unsigned char> selected,dirty;
    explicit Fixture(int count=10):bones(count),selected(count),dirty(count) {
        record->boneCount=count; record->boneTable=bones.data(); record->boneKeys=keys.data();
        record->morphKeys=morphs.data(); record->displayKeys=displays.data();
        record->boneSelection=selected.data(); record->bonePhysicsState=dirty.data();
        record->selectedBone=0; record->loadComplete=1;
        for(int i=0;i<count;++i) {
            bones[i].parent=-1; bones[i].slotIndex=-1; bones[i].type=static_cast<mdl::BoneType>(i%10);
            bones[i].rotQuat[3]=1; keys[i].rotation[3]=1;
            Identity(bones[i].matInit); Identity(bones[i].matWorld);
            std::snprintf(bones[i].name,sizeof(bones[i].name),"bone%d",i);
        }
    }
    unsigned char* data() { return reinterpret_cast<unsigned char*>(record.get()); }
    ~Fixture() {
        bs::Forget(data());
        for(auto& ring:record->undoRings) for(auto& slot:ring.slots) {
            std::free(slot.bonePose); std::free(slot.auxiliaryPose);
        }
    }
};
void MathAndAxes() {
    Fixture f;
    const float c=std::sqrt(0.5f);
    const bs::Matrix rotation{{c,c,0,-c,c,0,0,0,1}};
    const float worldX[3]={1,0,0},localX[3]={c,c,0};
    Check(bs::Scale(f.data(),0,rotation,worldX,2,false),"world X scale accepted");
    Near(bs::Multiply(bs::Pose(f.data(),0),rotation),bs::Multiply(rotation,Diagonal(2,1,1)),"world scale conjugates rotated bone");
    Check(std::abs(bs::Pose(f.data(),0).v[1])>0.4f,"world scale retains off-diagonal shear");
    bs::SetPose(f.data(),0,{});
    Check(bs::Scale(f.data(),0,rotation,localX,2,false),"local X scale accepted");
    Near(bs::Pose(f.data(),0),Diagonal(2,1,1),"local axis follows rotation");
    bs::SetPose(f.data(),0,{});
    const float viewAxis[3]={0,0.6f,0.8f};
    Check(bs::Scale(f.data(),0,rotation,viewAxis,1.5f,false),"view axis scale accepted");
    const auto before=bs::Pose(f.data(),0);
    Check(bs::Scale(f.data(),0,rotation,viewAxis,2,true),"uniform accepted");
    Near(bs::Pose(f.data(),0),bs::Multiply(before,Diagonal(2,2,2)),"uniform independent of coordinate basis");
    Check(!bs::SetPose(f.data(),0,Diagonal(0,1,1)),"reject zero scale");
    Check(!bs::SetPose(f.data(),0,Diagonal(-1,1,1)),"reject reflection");
    Check(!bs::SetPose(f.data(),0,Diagonal(std::numeric_limits<float>::quiet_NaN(),1,1)),"reject NaN");
    Check(!bs::Scale(f.data(),0,{},worldX,-1,true),"reject negative drag factor");
    for(int i=0;i<10;++i) Check(bs::SetPose(f.data(),i,Diagonal(1.2f,1.3f,1.4f)),"all ten legacy bone types accept scale");
    const bs::Matrix r180{{-1,0,0,0,-1,0,0,0,1}};
    for(int i=0;i<=100;++i) Check(bs::Valid(bs::Interpolate({},bs::Multiply(Diagonal(2,3,4),r180),i/100.f)),"polar interpolation never collapses rotated scale");
    Near(bs::Interpolate({},Diagonal(3,5,7),0.5f),Diagonal(2,3,4),"linear stretch interpolation");
}
void Hierarchy() {
    Fixture f(4); auto& root=f.bones[0]; auto& child=f.bones[1]; auto& grand=f.bones[2];
    f.bones[3].matInit[0]=.1234567f;
    float untouched[16]; std::memcpy(untouched,f.bones[3].matInit,64);
    root.position[0]=1; child.parent=0; child.position[0]=3; grand.parent=1; grand.position[0]=4;
    root.matInit[12]=child.matInit[12]=grand.matInit[12]=5;
    bs::SetPose(f.data(),0,Diagonal(2,2,2)); bs::ApplyTransforms(f.data());
    Near(root.matInit[0],2,"root scaled");
    Check(std::memcmp(untouched,f.bones[3].matInit,64)==0,"unrelated bones remain byte identical");
    Near(root.position[0]*root.matInit[0]+root.matInit[12],6,"root pivot fixed");
    Near(child.position[0]*child.matInit[0]+child.matInit[12],10,"child pivot follows parent scale");
    Near(grand.position[0]*grand.matInit[0]+grand.matInit[12],12,"grandchild inherits once");
    const auto first=bs::Linear(child.matInit); bs::ApplyTransforms(f.data()); Near(bs::Linear(child.matInit),first,"repeated apply is idempotent");
    bs::RestoreTransforms(f.data()); Near(root.matInit[0],1,"restore before IK and physics"); Near(child.matInit[12],5,"restore rigid translation");
    bs::SetPose(f.data(),1,Diagonal(3,1,1)); bs::ApplyTransforms(f.data());
    Near(child.position[0]*child.matInit[0]+child.matInit[12],10,"own scale preserves inherited pivot");
    Near(grand.position[0]*grand.matInit[0]+grand.matInit[12],16,"child scale composes down hierarchy");
    bs::RestoreTransforms(f.data()); root.parent=2; bs::ApplyTransforms(f.data());
    Check(std::isfinite(root.matInit[0]),"malformed parent cycle terminates");
}
bs::Matrix AxisRotation(int axis,float angle) {
    bs::Matrix r;
    const int a=(axis+1)%3,b=(axis+2)%3;
    r.v[a*3+a]=r.v[b*3+b]=std::cos(angle);
    r.v[a*3+b]=std::sin(angle); r.v[b*3+a]=-std::sin(angle);
    return r;
}
using Point=std::array<float,3>;
Point OffsetPoint(const Point& origin,const Point& offset,const bs::Matrix& rotation) {
    Point result=origin;
    for(int c=0;c<3;++c)for(int k=0;k<3;++k)result[c]+=offset[k]*rotation.v[k*3+c];
    return result;
}
void RigidAt(mdl::BoneRecord& bone,const bs::Matrix& rotation,const Point& pivot) {
    Identity(bone.matInit);
    for(int c=0;c<3;++c) {
        bone.matInit[12+c]=pivot[c];
        for(int k=0;k<3;++k) {
            bone.matInit[k*4+c]=rotation.v[k*3+c];
            bone.matInit[12+c]-=bone.position[k]*rotation.v[k*3+c];
        }
    }
}
void CheckPivot(const mdl::BoneRecord& bone,const bs::Transform& transform,const Point& expected) {
    for(int c=0;c<3;++c) {
        float value=transform[12+c];
        for(int k=0;k<3;++k)value+=bone.position[k]*transform[k*4+c];
        Near(value,expected[c],"rotated child stays attached to scaled parent");
    }
}
void CheckShape(const bs::Transform& transform,const bs::Matrix& shape) {
    // Pairwise lengths and angles must survive rotation, including authored shear.
    for(int a=0;a<3;++a)for(int b=0;b<3;++b) {
        float actual=0,expected=0;
        for(int k=0;k<3;++k) {
            actual+=transform[a*4+k]*transform[b*4+k];
            expected+=shape.v[a*3+k]*shape.v[b*3+k];
        }
        Near(actual,expected,"child rotation preserves inherited shape");
    }
}
void RotatedHierarchy() {
    Fixture f(4);
    const Point origin{5,-3,2},offset{2,3,-1},tipOffset{-1,2,2};
    const Point positions[3]={{1,-2,.5f},{3,1,-.5f},{2,3,1.5f}};
    for(int i=0;i<3;++i) {
        f.bones[i].parent=i-1;
        std::copy(positions[i].begin(),positions[i].end(),f.bones[i].position);
    }
    bs::SetPose(f.data(),0,Diagonal(4,2,.5f));
    bs::SetPose(f.data(),1,Diagonal(1,3,2));
    for(int axis=0;axis<3;++axis)for(int degrees=0;degrees<=180;degrees+=15) {
        const float angle=degrees*std::acos(-1.f)/180.f;
        const auto parent=AxisRotation((axis+1)%3,.37f);
        const auto child=bs::Multiply(AxisRotation(axis,angle),parent);
        const auto grand=bs::Multiply(AxisRotation((axis+2)%3,-.61f),child);
        const auto childPivot=OffsetPoint(origin,offset,parent);
        RigidAt(f.bones[0],parent,origin);
        RigidAt(f.bones[1],child,childPivot);
        RigidAt(f.bones[2],grand,OffsetPoint(childPivot,tipOffset,child));
        const auto expectedChild=OffsetPoint(origin,{8,6,-.5f},parent);
        const auto expectedGrand=OffsetPoint(expectedChild,{-4,12,2},child);
        std::vector<bs::Transform> baseline(4),out;
        for(int i=0;i<4;++i)std::memcpy(baseline[i].data(),f.bones[i].matInit,64);
        bs::BuildTransforms(f.data(),bs::PoseValues(f.data()),out);
        CheckShape(out[1],Diagonal(4,6,1)); CheckShape(out[2],Diagonal(4,6,1));
        CheckPivot(f.bones[1],out[1],expectedChild); CheckPivot(f.bones[2],out[2],expectedGrand);
        Check(out[3]==baseline[3],"unrelated branch remains byte identical");
        bs::ApplyTransforms(f.data()); bs::ApplyTransforms(f.data());
        std::vector<bs::Transform> cached,explicitBaseline;
        bs::BuildTransforms(f.data(),bs::PoseValues(f.data()),cached);
        bs::BuildTransforms(f.data(),bs::PoseValues(f.data()),explicitBaseline,&baseline);
        Check(out==cached&&out==explicitBaseline,"cached and explicit baselines do not repeat scaling");
        bs::RestoreTransforms(f.data());
        // Physics must use the same inheritance rule and reproduce all target matrices.
        const std::map<int,bs::Transform> physical{{0,out[0]},{1,out[1]}};
        for(int repeat=0;repeat<3;++repeat) {
            bs::ResolvePhysics(f.data(),physical);
            bs::BuildTransforms(f.data(),bs::PoseValues(f.data()),cached);
            for(int i=0;i<3;++i)for(int k=0;k<16;++k)
                Near(cached[i][k],out[i][k],"physics round trip preserves scaled hierarchy");
        }
        bs::SetPose(f.data(),0,Diagonal(4,2,.5f));
        bs::SetPose(f.data(),1,Diagonal(1,3,2));
    }
    bs::SetPose(f.data(),1,{});
    const bs::Matrix shear{{4,.4f,.2f,0,2,.3f,0,0,.5f}};
    bs::SetPose(f.data(),0,shear);
    std::vector<bs::Transform> out;
    bs::BuildTransforms(f.data(),bs::PoseValues(f.data()),out);
    CheckShape(out[1],shear); CheckShape(out[2],shear);
    bs::SetPose(f.data(),0,{});
    bs::BuildTransforms(f.data(),{},out);
    for(int i=0;i<4;++i)Check(std::memcmp(out[i].data(),f.bones[i].matInit,64)==0,
                             "unit scale preserves exact legacy transforms");
}
void KeysAndUndo() {
    Fixture f; auto* m=f.data(); f.selected[7]=1; f.record->selectedBone=7;
    bs::SetPose(m,7,Diagonal(1,1,1)); RegisterSelectedBoneKeys(m,0,0);
    bs::SetPose(m,7,Diagonal(3,5,7)); RegisterSelectedBoneKeys(m,10,0);
    const int index=f.keys[7].next; Check(index>=10,"inert tip gets a native scale key");
    Near(bs::Key(m,index),Diagonal(3,5,7),"registration captures scale");
    bs::Evaluate(m,5); Near(bs::Pose(m,7),Diagonal(2,3,4),"scale key sampled for inert tip");
    bs::Evaluate(m,20); Near(bs::Pose(m,7),Diagonal(3,5,7),"last scale key holds");
    const auto revision=bs::Revision(m);
    bs::Evaluate(m,5); bs::Evaluate(m,20);
    bs::SetPose(m,7,Diagonal(3,5,7)); bs::SetKey(m,index,Diagonal(3,5,7));
    Check(bs::Revision(m)==revision,"seeking and no-op writes preserve extension edit revision");
    int frame=10; UndoModelEdit(m,&frame); Check(bs::Identity(bs::Key(m,index)),"undo registration restores key sidecar");
    RedoModelEdit(m,&frame); Near(bs::Key(m,index),Diagonal(3,5,7),"redo registration restores scale key");
    for(auto& key:f.keys) key.allocated=0; f.keys[index].allocated=1;
    DeleteMarkedModelKeys(m,10); Check(bs::Identity(bs::Key(m,index)),"deletion erases scale of freed key slot");
    UndoModelEdit(m,&frame); Near(bs::Key(m,index),Diagonal(3,5,7),"undo deletion restores scale");
    bs::CopyKey(m,index+1,index); Near(bs::Key(m,index+1),Diagonal(3,5,7),"frame merge copy preserves scale");
    { bs::Registration outer(Diagonal(2,3,4)); { bs::Registration inner({}); Check(bs::Identity(bs::RegistrationValue()),"nested ordinary import uses identity"); } Near(bs::RegistrationValue(),Diagonal(2,3,4),"registrar restores scoped scale"); }
    Check(bs::Identity(bs::RegistrationValue()),"no scale leaks into VMD imports");
    for(int i=0;i<65;++i) { f.record->undoState[0]=i%30; bs::CaptureUndo(m); bs::SetPose(m,7,Diagonal(2+i,1,1)); }
    bs::Undo(m,4); Near(bs::Pose(m,7).v[0],65,"wrapped undo slot carries current snapshot");
    bs::Redo(m,4); Near(bs::Pose(m,7).v[0],66,"wrapped redo works");
    bs::Forget(m); Check(bs::Identity(bs::Pose(m,7))&&bs::Identity(bs::Key(m,index)),"model disposal forgets pointer state");
}
void DragAndAvailability() {
    Fixture f(3); auto app=std::make_unique<MMDApp>(); app->state={};
    app->state.modelSlots[0]=f.data(); app->state.slotIdx=0; app->PlaybackActive()=0;
    f.selected[0]=f.selected[1]=1; f.bones[1].parent=0; f.bones[1].position[0]=2;
    for(int type=0;type<10;++type) { f.bones[0].type=static_cast<mdl::BoneType>(type); Check(bs::Enabled(app.get()),"HUD enables every bone type"); }
    app->EditMode()=ViewportEditMode::Light; Check(bs::Enabled(app.get()),"bone move cursor still allows scale");
    app->EditMode()=ViewportEditMode::Camera; Check(bs::Enabled(app.get()),"bone rotate cursor still allows scale");
    app->state.optflag[0]=1; Check(!bs::Enabled(app.get()),"camera and accessory HUD disabled"); app->state.optflag[0]=0;
    app->PlaybackActive()=1; Check(!bs::Enabled(app.get()),"playback disables editing"); app->PlaybackActive()=0;
    app->PreviousMouseY()=100; app->MouseY()=90; app->state.coordinateSystem=1;
    bs::Drag(app.get(),-1); Near(bs::Pose(f.data(),0).v[0],std::exp(0.1f),"drag upward grows uniformly");
    Check(bs::Identity(bs::Pose(f.data(),1)),"selected descendant is not scaled a second time");
    Check(f.dirty[0]!=0,"scale marks bone pose dirty for registration");
    bs::Matrix shear{{2,.4f,.3f,.2f,3,.1f,.5f,.6f,4}};
    bs::CopyClipboard(app.get(),0,shear,true); const auto mirrored=bs::Clipboard(app.get(),0,true,true);
    Near(mirrored.v[1],-.4f,"mirror paste reflects XY shear"); Near(mirrored.v[5],.1f,"mirror paste preserves YZ shear");
    bs::CopyClipboard(app.get(),0,{},true); Check(bs::Identity(bs::Clipboard(app.get(),0,true)),"new ordinary copy clears stale scale");
    app->state.modelSlots[0]=nullptr;
}
void PersistenceIntegration() {
    Fixture f(3); auto app=std::make_unique<MMDApp>(); app->state={};
    auto* m=f.data(); app->state.modelSlots[0]=m; f.selected[1]=1;
    bs::SetPose(m,1,Diagonal(2,3,4)); RegisterSelectedBoneKeys(m,0,0);
    bs::SetPose(m,1,Diagonal(4,5,6)); RegisterSelectedBoneKeys(m,10,0);
    const int oldSlot=f.keys[1].next;
    namespace io=bone_scale_io;
    io::ResetPmm(); pmm_extension::Document saved;
    io::CapturePmm(*app,saved);
    Check(pmm_extension::Find(saved,io::PmmKey(0))!=nullptr,"PMM captures actual model scale state");
    f.keys[400]=f.keys[oldSlot]; f.keys[1].next=400; f.keys[oldSlot]={};
    bs::Forget(m); io::LoadPmm(*app,saved);
    Near(bs::Key(m,400),Diagonal(4,5,6),"PMM remaps persisted frame to a different key pool slot");
    Near(bs::Pose(m,1),Diagonal(4,5,6),"PMM restores unregistered current pose");
    bs::Evaluate(m,5); auto seekSave=saved; io::CapturePmm(*app,seekSave);
    bs::Forget(m); io::LoadPmm(*app,seekSave);
    Near(bs::Pose(m,1),Diagonal(3,4,5),"known PMM record saves the current evaluated pose");
    pmm_extension::Document future;
    const std::string futureKey="org.mikudancestudio.model.0.boneScale.v42";
    pmm_extension::Set(future,futureKey,{1,2,3,4});
    bs::Forget(m); io::LoadPmm(*app,future); bs::Evaluate(m,5);
    auto unchanged=future; io::CapturePmm(*app,unchanged);
    Check(pmm_extension::Find(unchanged,futureKey)!=nullptr,"frame seeking preserves unknown future data");
    bs::SetPose(m,1,Diagonal(7,8,9));
    auto discarded=future; io::CapturePmm(*app,discarded);
    auto retried=future; io::CapturePmm(*app,retried);
    Check(pmm_extension::Find(retried,futureKey)==nullptr,"failed save retry still replaces obsolete future data");
    Check(pmm_extension::Find(retried,io::PmmKey(0))!=nullptr,"failed save retry retains actual new scale");
    pmm_extension::Set(retried,futureKey,{4,3,2,1});
    bs::SetPose(m,1,Diagonal(8,9,10)); io::CapturePmm(*app,retried);
    Check(pmm_extension::Find(retried,futureKey)==nullptr,"editing a mixed known and future record remains safe");
    app->state.modelSlots[0]=nullptr; io::CapturePmm(*app,retried);
    Check(pmm_extension::Find(retried,io::PmmKey(0))==nullptr,"deleting model removes its old scale record");
    io::ResetPmm();
}
// A PMM record that decodes but cannot be applied to the loaded model (a bone
// name that no longer matches, or a scaled key frame that is not in the bone's
// track) is refused by LoadPmm.  While the slot still holds the same model and
// the user has not edited the scale, CapturePmm must keep that record byte for
// byte instead of rewriting or clearing it from the empty model state.  A real
// scale edit and a model change stay allowed to replace or clear it.
void PmmRefusedRecordPreservation() {
    namespace io=bone_scale_io;
    auto app=std::make_unique<MMDApp>(); app->state={};
    // Refused by bone identity: the record names a bone this model does not have.
    {
        Fixture f(3); f.selected[1]=1;
        app->state.modelSlots[0]=f.data(); app->state.slotIdx=0;
        bs::Forget(f.data()); io::ResetPmm();
        io::Samples wanted; io::PoseSample pose;
        pose.bone=1; pose.name=FixedName("not this model"); pose.scale=Diagonal(2,3,4);
        wanted.pose.push_back(pose);
        const std::vector<std::uint8_t> payload=io::EncodePmm(wanted);
        const std::string future="org.mikudancestudio.model.0.boneScale.v42";
        pmm_extension::Document document;
        pmm_extension::Set(document,io::PmmKey(0),payload);
        pmm_extension::Set(document,future,{1,2,3,4});
        io::LoadPmm(*app,document);
        Check(bs::Identity(bs::Pose(f.data(),1)),"a mismatched bone name applies no scale");
        pmm_extension::Document kept=document; io::CapturePmm(*app,kept);
        const pmm_extension::Record* record=pmm_extension::Find(kept,io::PmmKey(0));
        Check(record!=nullptr,"a record refused by bone identity survives a capture without an edit");
        Check(record!=nullptr&&record->value==payload,"a record refused by bone identity keeps its exact bytes");
        Check(pmm_extension::Find(kept,future)!=nullptr,"a refused record leaves a future version alone");
        // A real scale edit still replaces the refused record and drops the future one.
        Check(bs::SetPose(f.data(),1,Diagonal(5,5,5)),"edit the slot whose record was refused");
        pmm_extension::Document edited=kept; io::CapturePmm(*app,edited);
        const pmm_extension::Record* replaced=pmm_extension::Find(edited,io::PmmKey(0));
        Check(replaced!=nullptr,"a real scale edit writes a record again");
        io::Samples rewritten;
        Check(replaced!=nullptr&&io::DecodePmm(replaced->value.data(),replaced->value.size(),rewritten)==io::PmmDecode::Ok,
              "the record written after a real edit decodes");
        Check(rewritten.pose.size()==1&&!bs::Identity(rewritten.pose[0].scale)&&bs::Valid(rewritten.pose[0].scale),
              "the record written after a real edit carries the edited scale");
        Check(pmm_extension::Find(edited,future)==nullptr,"a real scale edit drops the obsolete future version");
        app->state.modelSlots[0]=nullptr;
    }
    // Refused by key frame: the record's frame is not a key of that bone's track.
    {
        Fixture f(3); f.selected[1]=1;
        app->state.modelSlots[0]=f.data(); app->state.slotIdx=0;
        bs::Forget(f.data()); io::ResetPmm();
        f.keys[1].frame=10; f.keys[1].allocated=1;   // the only real key of bone 1
        io::Samples wanted; io::KeySample key;
        key.bone=1; key.name=FixedName("bone1"); key.frame=99; key.scale=Diagonal(2,2,2);
        wanted.keys.push_back(key);
        const std::vector<std::uint8_t> payload=io::EncodePmm(wanted);
        pmm_extension::Document document;
        pmm_extension::Set(document,io::PmmKey(0),payload);
        io::LoadPmm(*app,document);
        Check(bs::Capture(f.data()).keys.empty(),"a missing key frame applies no scale");
        pmm_extension::Document kept=document; io::CapturePmm(*app,kept);
        const pmm_extension::Record* record=pmm_extension::Find(kept,io::PmmKey(0));
        Check(record!=nullptr,"a record refused by key frame survives a capture without an edit");
        Check(record!=nullptr&&record->value==payload,"a record refused by key frame keeps its exact bytes");
        app->state.modelSlots[0]=nullptr;
    }
    // An applicable empty record is still cleared: nothing is over-preserved.
    {
        Fixture f(3);
        app->state.modelSlots[0]=f.data(); app->state.slotIdx=0;
        bs::Forget(f.data()); io::ResetPmm();
        pmm_extension::Document document;
        pmm_extension::Set(document,io::PmmKey(0),io::EncodePmm({}));
        io::LoadPmm(*app,document); io::CapturePmm(*app,document);
        Check(pmm_extension::Find(document,io::PmmKey(0))==nullptr,
              "an applicable empty record is still cleared by a capture");
        app->state.modelSlots[0]=nullptr;
    }
    // Replacing the model in the slot still drops the refused record.
    {
        Fixture a(3),b(4);
        app->state.modelSlots[0]=a.data(); app->state.slotIdx=0;
        bs::Forget(a.data()); bs::Forget(b.data()); io::ResetPmm();
        io::Samples wanted; io::PoseSample pose;
        pose.bone=1; pose.name=FixedName("gone"); pose.scale=Diagonal(2,2,2);
        wanted.pose.push_back(pose);
        pmm_extension::Document document;
        pmm_extension::Set(document,io::PmmKey(0),io::EncodePmm(wanted));
        io::LoadPmm(*app,document);
        app->state.modelSlots[0]=b.data(); io::CapturePmm(*app,document);
        Check(pmm_extension::Find(document,io::PmmKey(0))==nullptr,
              "replacing the model in the slot drops the refused record");
        app->state.modelSlots[0]=nullptr;
    }
    io::ResetPmm();
}
void VmdProductionRoundTrip() {
    Fixture source(3), destination(3);
    std::strcpy(source.record->name,"bone-scale-test");
    std::strcpy(destination.record->name,"bone-scale-test");
    source.selected[1]=1;
    bs::SetPose(source.data(),1,Diagonal(2,3,4)); RegisterSelectedBoneKeys(source.data(),0,0);
    bs::SetPose(source.data(),1,Diagonal(4,5,6)); RegisterSelectedBoneKeys(source.data(),10,0);
    source.keys[1].allocated=1; source.keys[source.keys[1].next].allocated=1;
    // MMS addition: the frame-0 key (the allocated slot 1; slot 0 is the
    // track's unallocated head) carries a non-linear scale curve, so the
    // export must select the version 2 tail and the import must put the curve
    // back next to its matrix.  The tuple is not linear under
    // bone_scale::Linear (x1 != y1).
    const bs::Curve exportedCurve={20,20,20,107};
    bs::SetKeyCurve(source.data(),1,exportedCurve);
    auto app=std::make_unique<MMDApp>(); app->state={};
    auto renderer=std::make_unique<D3DRenderer>(); app->Renderer()=renderer.get();
    app->state.modelSlots[0]=source.data(); app->state.slotIdx=0;
    auto* previous=g_Block; g_Block=app.get();
    wchar_t directory[MAX_PATH]{},scaled[MAX_PATH]{},plain[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH,directory)&&GetTempFileNameW(directory,L"bss",0,scaled)&&
          GetTempFileNameW(directory,L"bsp",0,plain),"create owned VMD roundtrip files");
    std::puts("VMD: export scaled motion"); SaveVmdFile(scaled);
    bone_scale_io::VmdExtension sidecar;
    Check(bone_scale_io::LoadVmdExtension(scaled,sidecar)==bone_scale_io::ExtensionLoad::Loaded,
          "actual VMD save writes a matching tail extension");
    Check(sidecar.entries.size()==2,"actual VMD exports the two selected scale keys");
    // Entries follow the bone track, so the curved key is found by its frame:
    // the frame-0 entry carries the slot 1 curve and is not linear, while the
    // frame-10 entry stays on the linear default.
    Check(!bs::Linear(sidecar.entries[0].curve)&&
          sidecar.entries[0].frame==0&&
          sidecar.entries[0].curve==exportedCurve,
          "actual VMD export carries the key curve next to its matrix");
    Check(bs::Linear(sidecar.entries[1].curve)&&
          sidecar.entries[1].frame==10,
          "actual VMD export keeps the curve-free key on the linear default");
    std::puts("VMD: export ordinary motion"); bs::Restore(source.data(),{}); SaveVmdFile(plain);
    const auto read=[](const wchar_t* path) {
        std::ifstream stream(std::filesystem::path(path),std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(stream),{});
    };
    const auto a=read(scaled),b=read(plain);
    Check(a.size()>b.size()&&sidecar.prefixBytes==b.size()&&std::equal(b.begin(),b.end(),a.begin()),
          "single-file extension keeps standard VMD prefix bytes identical");
    Check(!std::filesystem::exists(std::wstring(scaled)+L".mdsx"),"single-file export creates no companion");
    app->state.modelSlots[0]=destination.data(); app->state.currentFrame=37;
    std::puts("VMD: import scaled motion"); LoadVmdFile(scaled);
    // The import creates its own slots, so the curve is located by frame: the
    // source's frame-0 key (the curved one) becomes the frame-37 key, which is
    // slot 3, while the source's linear frame-10 key becomes frame 47 / slot 4.
    {
        const auto snapshot=bs::Capture(destination.data());
        Check(snapshot.curves.size()==1&&snapshot.curves.count(3)==1&&
              snapshot.curves.at(3)==exportedCurve,
              "actual VMD import restores the key curve next to its matrix");
        Check(snapshot.curves.count(4)==0,
              "actual VMD import leaves the curve-free key linear");
    }
    bs::Evaluate(destination.data(),37); Near(bs::Pose(destination.data(),1),Diagonal(2,3,4),"real VMD import restores scale at offset");
    bs::Evaluate(destination.data(),42); Near(bs::Pose(destination.data(),1),Diagonal(3,4,5),"real VMD scale keys interpolate after offset import");
    std::puts("VMD: undo and redo import"); int frame=37; UndoModelEdit(destination.data(),&frame);
    Check(bs::Capture(destination.data()).keys.empty(),"undo VMD import removes its scale keys");
    RedoModelEdit(destination.data(),&frame); bs::Evaluate(destination.data(),47);
    Near(bs::Pose(destination.data(),1),Diagonal(4,5,6),"redo VMD import restores its scale keys");
    std::puts("VMD: import ordinary motion"); LoadVmdFile(plain); bs::Evaluate(destination.data(),37);
    Check(bs::Identity(bs::Pose(destination.data(),1)),"ordinary VMD overwrites matching scale with identity");
    app->state.modelSlots[0]=nullptr; g_Block=previous;
    std::filesystem::remove(scaled); std::filesystem::remove(plain);
}
}
int main() { std::setvbuf(stdout,nullptr,_IONBF,0);
 MathAndAxes(); Hierarchy(); RotatedHierarchy(); KeysAndUndo(); DragAndAvailability(); VmdProductionRoundTrip(); PersistenceIntegration(); PmmRefusedRecordPreservation(); std::puts("PASS: native bone scale math, hierarchy, keys, undo, clipboard, drag and PMM/VMD integration"); }
