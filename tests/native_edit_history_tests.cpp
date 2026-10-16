// CPU-only regression of the production native history prepare/publish path.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikudancestudio/native_edit_history.hpp"
#include "mikudancestudio/model.hpp"
#include "mikudancestudio/bone_key_pool.hpp"
#include "mikudancestudio/key_pool.hpp"
#include "mikudancestudio/bone_scale.hpp"
#include "mikudancestudio/bone_curve.hpp"
#include "mikudancestudio/morph_curve.hpp"
#include "mikudancestudio/sequence_scatter.hpp"
#include "mikudancestudio/mmd_app.hpp"
#include "mikudancestudio/ported_funcs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <array>

namespace { thread_local long failAfter=-1; }
void* operator new(std::size_t size) {
    if(failAfter==0)throw std::bad_alloc();
    if(failAfter>0)--failAfter;
    if(auto p=std::malloc(size?size:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

using namespace mikudancestudio;
namespace mikudancestudio { void CmdControl400(MMDApp*, HWND, std::uint16_t, std::uint16_t); }
namespace nh=native_history;
namespace {
unsigned checks=0;
void Check(bool ok,const char* what){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",what);std::exit(1);}}
struct Fixture {
    std::unique_ptr<mdl::ModelRecord> m=std::make_unique<mdl::ModelRecord>();
    std::array<mdl::BoneRecord,2> bones{};
    std::array<mdl::MorphRecord,1> morphs{};
    std::array<unsigned char,2> dirty{},selected{};
    Fixture(){
        m->boneCount=2;m->boneTable=bones.data();m->bonePhysicsState=dirty.data();m->boneSelection=selected.data();
        m->boneKeys=mdl::BoneKeyPoolCreate(32);m->morphKeys=mdl::MorphKeyPoolCreate(16);m->displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(4);
        m->morphs=morphs.data();m->morphCount=1;std::memcpy(morphs[0].name,"face",5);morphs[0].panel=static_cast<mdl::MorphPanel>(1);
        m->ikChainCount=1;m->boneOrderCount=1;
        for(unsigned i=0;i<4;++i){m->displayKeys[i].ikStates=new unsigned char[1]{1};m->displayKeys[i].selectorStates=new mdl::BoneReference[1]{};}
        for(unsigned i=0;i<2;++i){bones[i].rotQuat[3]=1;bones[i].parent=-1;m->boneKeys[i].rotation[3]=1;}
        m->undoState[0]=m->undoState[1]=1;m->undoDirty=1;m->maxFrame=99;m->searchCursor=5;
    }
    unsigned char* raw(){return reinterpret_cast<unsigned char*>(m.get());}
    ~Fixture(){bone_scale::Forget(raw());morph_curve::Forget(raw());
        for(auto& ring:m->undoRings)for(auto& entry:ring.slots){std::free(entry.bonePose);std::free(entry.auxiliaryPose);}
        for(unsigned i=0;i<4;++i){delete[] m->displayKeys[i].ikStates;delete[] m->displayKeys[i].selectorStates;}
        mdl::BoneKeyPoolDestroy(m->boneKeys);mdl::MorphKeyPoolDestroy(m->morphKeys);mdl::KeyPoolDestroy(m->displayKeys);
    }
    void Entry(unsigned slot,int type,float value){
        auto& e=m->undoRings[0].slots[slot];e={};e.operation=type;e.dirty=1;e.frame=7;
        const auto count=type==1||type==3?1u:2u;
        e.bonePose=static_cast<mdl::BonePoseSnapshot*>(std::calloc(count,sizeof(mdl::BonePoseSnapshot)));
        for(unsigned i=0;i<count;++i){e.bonePose[i].boneIndex=int(i);e.bonePose[i].position[0]=value;e.bonePose[i].rotation[3]=1;}
        if(type==2||type==4||type==5){auto* key=static_cast<mdl::BoneKeySnapshot*>(std::calloc(1,sizeof(mdl::BoneKeySnapshot)));
            key->keyIndex=3;key->key=m->boneKeys[3];key->key.position[0]=value;e.auxiliaryPose=key;}
    }
    std::uint64_t Digest(){
        std::uint64_t h=14695981039346656037ull;
        const auto add=[&](const void* data,std::size_t n){auto p=static_cast<const unsigned char*>(data);while(n--){h^=*p++;h*=1099511628211ull;}};
        add(m.get(),sizeof(*m));add(bones.data(),sizeof(bones));add(dirty.data(),sizeof(dirty));
        add(m->boneKeys,32*sizeof(mdl::BoneKey));add(m->morphKeys,16*sizeof(mdl::MorphKey));add(m->displayKeys,4*sizeof(mdl::DisplayKey));
        for(unsigned i=0;i<4;++i){add(m->displayKeys[i].ikStates,1);add(m->displayKeys[i].selectorStates,sizeof(mdl::BoneReference));}
        return h;
    }
};
bone_scale::Matrix Scale(float value){bone_scale::Matrix m;m.v[0]=value;return m;}
bool PublishNoAllocation(nh::ModelEdit& edit,int& frame){
    failAfter=0;const bool result=edit.Publish(frame);failAfter=-1;return result;
}
void FailureSweep(Fixture& f,int frame,bool redo){
    const auto before=f.Digest();nh::ModelEdit baseline;
    Check(baseline.Prepare(f.raw(),frame,redo)==nh::Result::Ready,"failure fixture is valid");
    unsigned failures=0;
    for(long limit=0;limit<4096;++limit){nh::ModelEdit edit;failAfter=limit;
        const auto result=edit.Prepare(f.raw(),frame,redo);failAfter=-1;
        Check(f.Digest()==before&&baseline.IsCurrent(frame),"failed preparation preserves native and side-table state");
        if(result==nh::Result::Ready){Check(failures>2,"allocation failures reached multiple preparation stages");return;}
        Check(result==nh::Result::Memory,"allocation failure reported as memory");++failures;
    }
    Check(false,"allocation sweep terminates");
}
void PoseAndStale(){
    for(int type:{1,3}){Fixture f;f.Entry(1,type,2);f.bones[0].trans[0]=9;f.dirty[0]=1;
        bone_scale::SetPose(f.raw(),0,Scale(2));bone_scale::CaptureUndo(f.raw());bone_scale::SetPose(f.raw(),0,Scale(3));
        int frame=20;FailureSweep(f,frame,false);nh::ModelEdit edit;
        Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Ready,"prepare pose undo");
        f.bones[0].trans[0]=10;const auto stale=f.Digest();Check(!PublishNoAllocation(edit,frame)&&f.Digest()==stale,"stale pose refuses publication");
        f.bones[0].trans[0]=9;bone_scale::SetPose(f.raw(),0,Scale(4));Check(!edit.IsCurrent(frame),"stale scale side table refuses publication");
        bone_scale::SetPose(f.raw(),0,Scale(3));
        Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"pose undo publishes without allocation");
        Check(f.bones[0].trans[0]==2&&f.dirty[0]==0&&bone_scale::Pose(f.raw(),0)==Scale(2),"pose and scale restored together");
        Check(frame==(type==3?7:20)&&f.m->undoState[0]==0,"pose undo frame and cursor");
        Check(!PublishNoAllocation(edit,frame),"publication consumed once");
        FailureSweep(f,frame,true);Check(edit.Prepare(f.raw(),frame,true)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"pose redo publishes without allocation");
        Check(f.bones[0].trans[0]==9&&f.dirty[0]==1&&bone_scale::Pose(f.raw(),0)==Scale(3),"redo restores displaced pose and scale");
    }
}
void KeysAndChain(){
    for(bool wrap:{false,true}){Fixture f;const unsigned latest=wrap?0:2,older=wrap?29:1;
        f.Entry(older,4,1);f.Entry(latest,2,2);f.m->undoState[0]=f.m->undoState[1]=latest;f.m->boneKeys[3].position[0]=3;
        int frame=30;FailureSweep(f,frame,false);nh::ModelEdit edit;
        Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"whole type-4 chain publishes together");
        Check(f.m->boneKeys[3].position[0]==1&&f.m->undoState[0]==(older+29)%30,"chain undo includes wrapped ring");
        Check(edit.Prepare(f.raw(),frame,true)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"whole chain redo publishes together");
        Check(f.m->boneKeys[3].position[0]==3&&f.m->undoState[0]==latest,"redo captures each intermediate chain state");
    }
    Fixture f;f.Entry(1,2,2);auto* key=static_cast<mdl::BoneKeySnapshot*>(f.m->undoRings[0].slots[1].auxiliaryPose);
    key->keyIndex=32;const auto before=f.Digest();nh::ModelEdit edit;
    Check(edit.Prepare(f.raw(),0,false)==nh::Result::Invalid&&f.Digest()==before,"out-of-bounds key rejected before mutation");
    key->keyIndex=3;Check(edit.Prepare(f.raw(),0,false)==nh::Result::Ready,"valid replacement prepares");
    key->key.position[0]=19;int frame=0;Check(!PublishNoAllocation(edit,frame),"in-place source snapshot change is stale");
}
void Bake(){
    Fixture f;f.Entry(1,5,2);auto& e=f.m->undoRings[0].slots[1];e.reserved0=21;e.reserved1=6;e.bonePose[0].reserved[0]=1;
    f.m->boneKeys[3].position[0]=8;bone_scale::SetKey(f.raw(),3,Scale(2));bone_scale::CaptureUndo(f.raw());bone_scale::SetKey(f.raw(),3,Scale(4));
    int frame=30;FailureSweep(f,frame,false);nh::ModelEdit edit;
    Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"bake undo publishes");
    Check(f.m->maxFrame==21&&f.m->searchCursor==6&&f.bones[0].physicsDisabled==1&&bone_scale::Key(f.raw(),3)==Scale(2),"bake metadata physics and scale restore together");
    Check(edit.Prepare(f.raw(),frame,true)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"bake redo publishes");
    Check(f.m->maxFrame==99&&f.m->searchCursor==5&&f.bones[0].physicsDisabled==0&&f.m->boneKeys[3].position[0]==8&&bone_scale::Key(f.raw(),3)==Scale(4),"bake redo restores all displaced data");
}
void Deletion(){
    Fixture f;f.m->morphKeys[0].next=1;f.m->morphKeys[1].frame=10;f.m->morphKeys[1].value=.75f;f.m->morphKeys[1].allocated=1;
    f.m->displayKeys[0].next=1;f.m->displayKeys[1].frame=10;f.m->displayKeys[1].allocated=1;f.m->displayKeys[1].ikStates[0]=0;
    f.m->displayKeys[1].selectorStates[0].modelIndex=7;f.m->displayKeys[1].selectorStates[0].boneIndex=1;
    DeleteMarkedModelKeys(f.raw(),10);Check(f.m->undoRings[0].slots[f.m->undoState[0]].operation==7,"real deletion produces type-7 history");
    int frame=15;FailureSweep(f,frame,false);nh::ModelEdit edit;
    Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"deletion undo publishes");
    Check(f.m->morphKeys[1].value==.75f&&f.m->morphKeys[1].frame==10&&f.m->displayKeys[1].frame==10&&f.m->displayKeys[1].ikStates[0]==0&&f.m->displayKeys[1].selectorStates[0].modelIndex==7,"morph display IK and selector snapshots restored");
    auto* ik=f.m->displayKeys[1].ikStates;auto* selectors=f.m->displayKeys[1].selectorStates;
    FailureSweep(f,frame,true);Check(edit.Prepare(f.raw(),frame,true)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"deletion redo publishes");
    Check(f.m->morphKeys[1].frame==0&&f.m->displayKeys[1].frame==0&&f.m->displayKeys[1].ikStates==ik&&f.m->displayKeys[1].selectorStates==selectors,"redo preserves live payload ownership");
}
void EmptyAndCycle(){
    Fixture f;int frame=9;const auto before=f.Digest();
    Check(!TryUndoModelEdit(f.raw(),&frame)&&f.Digest()==before,"empty native undo preserves dirty and availability flags");
    for(unsigned i=0;i<30;++i)f.Entry(i,4,float(i));
    nh::ModelEdit edit;const auto cyclic=f.Digest();
    Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Invalid&&f.Digest()==cyclic,"cyclic chain rejected as one unit");
}
void FreeCurves(){
    Fixture f;bone_curve::Tracks tracks;std::string error;
    for(unsigned c=0;c<9;++c){animation_curve::Key a,b;b.frame=10;a.value=b.value=c>=6?1:0;
        a.tangent=b.tangent=animation_curve::Tangent::Free;if(c==0){b.value=10;a.outSlope=2;}
        tracks[c].keys={a,b};}
    Check(bone_curve::Write(f.raw(),0,tracks,error),"real free curve writer prepares source");
    f.Entry(1,2,0);auto& e=f.m->undoRings[0].slots[1];std::free(e.auxiliaryPose);e.dirty=32;
    auto* saved=static_cast<mdl::BoneKeySnapshot*>(std::calloc(32,sizeof(mdl::BoneKeySnapshot)));e.auxiliaryPose=saved;
    for(int i=0;i<32;++i)saved[i]={i,f.m->boneKeys[i]};
    bone_scale::CaptureUndo(f.raw());tracks[0].keys[1].value=18;
    Check(bone_curve::Write(f.raw(),0,tracks,error),"real free curve writer produces edited state");
    int frame=10;FailureSweep(f,frame,false);nh::ModelEdit edit;
    Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"free curve undo publishes without allocation");
    bone_curve::Tracks read;Check(bone_curve::Read(f.raw(),0,read)&&read[0].keys.back().value==10,"undo restores authored curve and native mirror together");
    Check(edit.Prepare(f.raw(),frame,true)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"free curve redo publishes without allocation");
    Check(bone_curve::Read(f.raw(),0,read)&&read[0].keys.back().value==18,"redo restores authored curve");
}
void Scatter(){
    Fixture f;f.m->boneCount=0;f.m->ikChainCount=0;f.m->boneOrderCount=0;
    animation_curve::Track source;animation_curve::Key a,b;b.frame=10;b.value=1;a.outSlope=.2;b.inSlope=0;
    a.tangent=b.tangent=animation_curve::Tangent::Free;source.keys={a,b};std::string error;
    const auto id=morph_curve::StableId(0,f.morphs[0].name);
    Check(morph_curve::Write(f.raw(),{{id,source}},error),"real morph writer creates scatter source");
    const auto oldPool=std::vector<mdl::MorphKey>(f.m->morphKeys,f.m->morphKeys+16);
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};app->ModelSlot(0)=f.raw();app->SelectedModelSlot()=0;
    app->MainWindow()=CreateWindowExW(0,L"STATIC",L"history test",WS_OVERLAPPED,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(app->MainWindow()!=nullptr,"hidden scatter owner");
    sequence_scatter::Source selected;sequence_scatter::Plan plan;sequence_scatter::Settings settings;
    settings.first=20;settings.last=45;settings.gapMean=5;settings.gapVariance=0;settings.speedVariance=0;
    Check(sequence_scatter::Capture(f.raw(),selected,error),"real scatter reads selected source");
    Check(sequence_scatter::Prepare(selected,settings,plan,error)&&plan.copies>0,"real scatter builds placements");
    Check(sequence_scatter::Apply(*app,selected,plan,error),"real scatter creates type-6 history");
    Check(f.m->undoRings[0].slots[f.m->undoState[0]].operation==6,"scatter publishes native ring entry");
    const auto placed=std::vector<mdl::MorphKey>(f.m->morphKeys,f.m->morphKeys+16);
    int frame=app->CurrentFrame();FailureSweep(f,frame,false);nh::ModelEdit edit;
    Check(edit.Prepare(f.raw(),frame,false)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"scatter undo publishes without allocation");
    Check(!std::memcmp(f.m->morphKeys,oldPool.data(),oldPool.size()*sizeof(mdl::MorphKey)),"scatter undo restores whole source pool");
    animation_curve::Track read;Check(morph_curve::Read(f.raw(),0,read,error)&&read.keys.size()==2&&read.keys.front().outSlope==.2,"scatter undo restores original authored tangent");
    FailureSweep(f,frame,true);Check(edit.Prepare(f.raw(),frame,true)==nh::Result::Ready&&PublishNoAllocation(edit,frame),"scatter redo publishes without allocation");
    Check(!std::memcmp(f.m->morphKeys,placed.data(),placed.size()*sizeof(mdl::MorphKey)),"scatter redo restores whole placed pool");
    Check(morph_curve::Read(f.raw(),0,read,error)&&read.keys.size()>2,"scatter redo restores placed curves");
    DestroyWindow(app->MainWindow());app->MainWindow()=nullptr;app->ModelSlot(0)=nullptr;
}
void FailedNativeCommands(){
    Fixture f;auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};
    app->ModelSlot(0)=f.raw();app->SelectedModelSlot()=0;app->CurrentFrame()=9;
    app->MainWindow()=CreateWindowExW(0,L"STATIC",L"history commands",WS_OVERLAPPED,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(app->MainWindow()!=nullptr,"hidden native command owner");
    for(unsigned command:{400u,401u}){const auto before=f.Digest();
        CmdControl400(app.get(),app->MainWindow(),static_cast<std::uint16_t>(command),0);
        Check(f.Digest()==before&&app->SceneModified()==0&&app->CurrentFrame()==9,"empty undo/redo command preserves scene dirty and history flags");
    }
    f.Entry(1,1,2);const auto before=f.Digest();
    failAfter=0;CmdControl400(app.get(),app->MainWindow(),400,0);failAfter=-1;
    Check(f.Digest()==before&&app->SceneModified()==0&&app->CurrentFrame()==9,"allocation failure in real undo command leaves scene unmodified");
    DestroyWindow(app->MainWindow());app->MainWindow()=nullptr;app->ModelSlot(0)=nullptr;
}
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);PoseAndStale();KeysAndChain();Bake();Deletion();EmptyAndCycle();FreeCurves();Scatter();FailedNativeCommands();std::printf("PASS native history: %u checks\n",checks);}
