// Multi-model frozen frames compared with real native scene sweeps.
#define NOMINMAX
#include "mikumikustudio/bone_scene_evaluation.hpp"
#include <algorithm>
#include "mikumikustudio/bone_transform_context.hpp"
#include "mikumikustudio/bone_curve.hpp"
#include "mikumikustudio/bone_key_pool.hpp"
#include "mikumikustudio/frame_limits.hpp"
#include "mikumikustudio/key_pool.hpp"
#include "mikumikustudio/model.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/ported_funcs.hpp"
#include "mikumikustudio/undo_storage.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace { thread_local int failAfter=-1; }
void* operator new(std::size_t bytes){if(failAfter==0)throw std::bad_alloc();if(failAfter>0)--failAfter;if(auto* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mikumikustudio;
namespace bs=bone_scale;namespace bc=bone_curve;namespace mc=morph_curve;namespace ac=animation_curve;
using Source=BoneEvaluationSource;using Frame=BoneEvaluationFrame;using Pose=BoneEvaluationPose;using Dependency=BoneEvaluationDependency;
namespace {
unsigned checks=0;const char* currentCase="setup";
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s line %d: %s\n",currentCase,__LINE__,#x);std::exit(1);}}while(false)
template<class T>bool Bits(const T& a,const T& b){return !std::memcmp(&a,&b,sizeof(T));}
template<class T>bool Bits(const std::vector<T>& a,const std::vector<T>& b){return a.size()==b.size()&&(a.empty()||!std::memcmp(a.data(),b.data(),a.size()*sizeof(T)));}
template<class E,class F>bool Throws(F&& f){try{f();}catch(const E&){return true;}return false;}
bs::Transform Identity(){bs::Transform t{};t[0]=t[5]=t[10]=t[15]=1;return t;}
using Bones=std::array<mdl::BoneRecord,10>;
struct Model {
    std::unique_ptr<mdl::ModelRecord> record=std::make_unique<mdl::ModelRecord>();
    Bones bones{};std::array<uint8_t,10> selected{},physics{};
    mdl::IkChain chain{};std::array<uint16_t,2> links{2,1};
    std::array<mdl::BoneOrderEntry,3> selectors{};
    std::array<std::array<mdl::BoneReference,3>,2> parents{};
    std::array<std::array<uint8_t,1>,2> enabled{{{197},{197}}};
    std::array<mdl::MorphRecord,2> morphs{};
    std::array<mdl::PmxBoneMorphEntry,2> boneMorphs{};
    std::array<mdl::PmxGroupMorphEntry,2> group{{{0,.5f},{0,-.25f}}};
    std::array<mdl::BoneMorphOffsetRecord,10> morphWork{};
    Model(){
        record->physicsMode=2;record->boneCount=10;record->boneTable=bones.data();
        record->boneKeys=mdl::BoneKeyPoolCreate(64);record->morphKeys=mdl::MorphKeyPoolCreate(8);record->displayKeys=mdl::KeyPoolCreate<mdl::DisplayKey>(2);
        record->boneSelection=selected.data();record->bonePhysicsState=physics.data();
        record->boneOrderCount=3;record->boneOrderTable=selectors.data();record->ikChainCount=1;record->ikChains=&chain;
        record->morphCount=2;record->morphs=morphs.data();record->boneMorphCount=10;record->boneMorphTable=morphWork.data();
        record->maxBoneLayer=1;record->maxFrame=20;record->searchCursor=555;record->displayKeyCursor=1;
        for(unsigned i=0;i<10;++i){
            auto& b=bones[i];b.parent=i?0:-1;b.slotIndex=-1;b.tailBone=b.tailIdx=-1;b.type=mdl::BoneType::RotateMove;
            b.flags=mdl::kBoneFlagVisible;std::snprintf(b.name,20,"frozen-%u",i);
            b.trans[0]=700+float(i);b.rotQuat[2]=.25f;b.rotQuat[3]=1;b.physicsOffset[1]=900;
            for(unsigned j=0;j<16;++j){b.matInit[j]=float(j+500);b.matLocal[j]=float(j+600);b.matWorld[j]=float(j+700);b.matExtra[j]=float(j+800);}
            auto& first=record->boneKeys[i];first.allocated=1;first.rotation[3]=1;first.next=10+i;
            auto& last=record->boneKeys[10+i];last=first;last.previous=i;last.next=0;last.frame=20;
            last.position[0]=float(i%3)*.125f;last.physicsDisabled=uint8_t(i%2);
            morphWork[i].boneIndex=int(i);morphWork[i].translation[0]=1234;morphWork[i].rotation[3]=1;
        }
        record->boneKeys[0].position[0]=.5f;record->boneKeys[10].position[0]=1.25f;
        record->boneKeys[4].position[0]=.125f;record->boneKeys[4].position[1]=.25f;record->boneKeys[4].position[2]=-.5f;
        record->boneKeys[14].position[0]=.75f;record->boneKeys[14].position[1]=-.25f;record->boneKeys[14].position[2]=.5f;
        bones[1].type=bones[2].type=mdl::BoneType::UnderIk;bones[1].position[1]=1;bones[2].parent=1;bones[2].position[1]=2;
        bones[3].parent=2;bones[3].position[1]=3;bones[3].type=mdl::BoneType::Effector;
        bones[4].position[0]=1;bones[4].position[1]=2;bones[4].position[2]=1;bones[4].type=mdl::BoneType::Ik;
        chain.boneIndex=4;chain.targetBone=3;chain.linkCount=2;chain.links=links.data();chain.iterations=8;chain.maxAngle=.35f;chain.enabled=231;
        for(unsigned i=5;i<10;++i){bones[i].layer=1;bones[i].position[0]=float(i-3);bones[i].position[1]=float(i%3);}
        bones[5].type=mdl::BoneType::RotateGrant;bones[5].tailIdx=1;bones[5].hasFlag=1;
        bones[6].type=mdl::BoneType::CoRotate;bones[6].tailBone=2;bones[6].tailIdx=75;bones[6].hasFlag=1;
        bones[7].flags|=mdl::kBoneFlagInheritMask|mdl::kBoneFlagAfterPhysics;bones[7].tailIdx=1;bones[7].inheritRatio=-.375f;bones[7].hasFlag=1;bones[7].slotIndex=2;
        bones[8].flags|=mdl::kBoneFlagInheritMask;bones[8].tailIdx=2;bones[8].inheritRatio=.625f;
        bones[9].parent=-1;bones[9].type=mdl::BoneType::RotateGrant;bones[9].tailIdx=1;bones[9].hasFlag=1;bones[9].slotIndex=1;
        selectors[0].boneIndex=-1;selectors[1].boneIndex=9;selectors[2].boneIndex=7;
        for(auto& s:selectors){s.linkedModel=777;s.linkedBone=888;s.windowStart=444;s.windowEnd=999;}
        for(unsigned i=0;i<2;++i){
            auto& d=record->displayKeys[i];d.frame=i?12:0;d.previous=0;d.next=i?0:1;d.allocated=1;d.visible=i?197:255;
            d.ikStates=enabled[i].data();d.selectorStates=parents[i].data();for(auto& p:parents[i])p={-1,0};
            auto& key=record->morphKeys[i];key.allocated=1;key.next=2+i;key.value=i?-.5f:.25f;
            record->morphKeys[2+i]=key;record->morphKeys[2+i].previous=i;record->morphKeys[2+i].next=0;record->morphKeys[2+i].frame=20;record->morphKeys[2+i].value=i?1.25f:.75f;
            std::snprintf(morphs[i].name,20,"bone-morph-%u",i);morphs[i].panel=mdl::MorphPanel::other;morphs[i].value=789;
        }
        boneMorphs={{{0,{.125f,-.25f,.5f},{0,.70710677f,0,.70710677f}},{7,{.25f,.5f,-.125f},{.38268343f,0,0,.9238795f}}}};
        morphs[0].type=2;morphs[0].boneCount=2;morphs[0].boneEntries=boneMorphs.data();
        morphs[1].type=0;morphs[1].groupCount=2;morphs[1].groupEntries=group.data();
        bs::Matrix scale;scale.v={1.5f,.125f,0,0,.75f,0,0,0,2};bs::SetKey(raw(),10,scale);
        bs::SetKeyCurve(raw(),10,{10,90,90,115});
    }
    unsigned char* raw()const{return reinterpret_cast<unsigned char*>(record.get());}
    ~Model(){bc::Forget(raw());mc::Forget(raw());bs::Forget(raw());mdl::BoneKeyPoolDestroy(record->boneKeys);mdl::MorphKeyPoolDestroy(record->morphKeys);mdl::KeyPoolDestroy(record->displayKeys);
        for(auto& ring:record->undoRings)for(auto& e:ring.slots){undo_storage::Free(e.bonePose);undo_storage::Free(e.auxiliaryPose);}}
};
void FreeRoot(Model& m){
    bc::Tracks tracks;for(unsigned lane=0;lane<9;++lane){ac::Key a,b;b.frame=20;a.tangent=b.tangent=ac::Tangent::Free;a.interpolation=b.interpolation=ac::Interpolation::Linear;
        a.value=b.value=lane>=6?1:0;if(lane==0){a.value=.25;b.value=1.25;}if(lane==4)b.value=45;if(lane==6)b.value=1.5;tracks[lane].keys={a,b};}
    std::string error;CHECK(bc::Write(m.raw(),0,tracks,error));
}
void FreshBone(mdl::BoneRecord& b,bool pmx){
    std::memset(b.trans,0,sizeof(b.trans));std::memset(b.rotQuat,0,sizeof(b.rotQuat));b.rotQuat[3]=1;
    std::memset(b.rotQuat2,0,sizeof(b.rotQuat2));b.rotQuat2[3]=1;
    std::memset(b.physicsOffset,0,sizeof(b.physicsOffset));std::memset(b.physicsQuat,0,sizeof(b.physicsQuat));
    std::memset(b.ikBackup,0,sizeof(b.ikBackup));std::memset(b.ikWorkingPos,0,sizeof(b.ikWorkingPos));std::memset(b.ikWorkingQuat,0,sizeof(b.ikWorkingQuat));
    std::memset(b.matLocal,0,sizeof(b.matLocal));std::memset(b.matInit,0,sizeof(b.matInit));std::memset(b.matExtra,0,sizeof(b.matExtra));std::memset(b.matWorld,0,sizeof(b.matWorld));
    b.matWorld[10]=b.matWorld[15]=1;if(pmx)b.matWorld[0]=b.matWorld[5]=1;b.physicsDisabled=0;b.rigidIdx=-296;
}
void MirrorFresh(const Model& source,Model& oracle){
    oracle.record->physicsMode=source.record->physicsMode;oracle.record->boneCount=source.record->boneCount;oracle.record->morphCount=source.record->morphCount;
    oracle.record->boneOrderCount=source.record->boneOrderCount;oracle.record->ikChainCount=source.record->ikChainCount;oracle.record->maxBoneLayer=source.record->maxBoneLayer;
    oracle.bones=source.bones;for(auto& b:oracle.bones)FreshBone(b,source.record->physicsMode==2);
    oracle.chain=source.chain;oracle.chain.links=oracle.links.data();oracle.links=source.links;
    oracle.selectors=source.selectors;oracle.parents=source.parents;oracle.enabled=source.enabled;
    oracle.morphs=source.morphs;oracle.boneMorphs=source.boneMorphs;oracle.group=source.group;
    oracle.morphs[0].boneEntries=oracle.boneMorphs.data();oracle.morphs[1].groupEntries=oracle.group.data();
    const auto capacity=mdl::BoneKeyCapacity(source.record->boneKeys);CHECK(mdl::BoneKeyPoolEnsure(*oracle.record,capacity));
    std::memcpy(oracle.record->boneKeys,source.record->boneKeys,capacity*sizeof(mdl::BoneKey));
    std::memcpy(oracle.record->morphKeys,source.record->morphKeys,8*sizeof(mdl::MorphKey));
    for(unsigned i=0;i<2;++i){oracle.record->displayKeys[i]=source.record->displayKeys[i];oracle.record->displayKeys[i].ikStates=oracle.enabled[i].data();oracle.record->displayKeys[i].selectorStates=oracle.parents[i].data();}
    for(std::size_t i=0;i<capacity;++i){bs::SetKey(oracle.raw(),int(i),bs::Key(source.raw(),int(i)));bs::SetKeyCurve(oracle.raw(),int(i),bs::KeyCurve(source.raw(),int(i)));}
    bc::CopyState(source.raw(),oracle.raw());mc::CopyState(source.raw(),oracle.raw());
}
struct State {
    std::unique_ptr<mdl::ModelRecord> record;Bones bones;mdl::IkChain chain;decltype(Model::links)links;decltype(Model::selectors)selectors;
    decltype(Model::parents)parents;decltype(Model::enabled)enabled;decltype(Model::morphs)morphs;decltype(Model::boneMorphs)boneMorphs;decltype(Model::group)group;decltype(Model::morphWork)work;
    decltype(Model::selected)selected,physics;std::vector<mdl::BoneKey> keys;std::array<mdl::MorphKey,8> morphKeys;std::array<mdl::DisplayKey,2> display;
    bs::Snapshot scale;std::uint64_t revision;bool applied;std::unique_ptr<bc::AuthoringState> author;mc::AuthoredLayers morphAuthor;
    explicit State(const Model& m):record(std::make_unique<mdl::ModelRecord>(*m.record)),bones(m.bones),chain(m.chain),links(m.links),selectors(m.selectors),parents(m.parents),enabled(m.enabled),morphs(m.morphs),boneMorphs(m.boneMorphs),group(m.group),work(m.morphWork),selected(m.selected),physics(m.physics),keys(m.record->boneKeys,m.record->boneKeys+mdl::BoneKeyCapacity(m.record->boneKeys)),scale(bs::Capture(m.raw())),revision(bs::Revision(m.raw())),applied(bs::TransformsApplied(m.raw())),author(bc::AuthoringState::Capture(m.raw())){
        std::memcpy(morphKeys.data(),m.record->morphKeys,sizeof(morphKeys));std::memcpy(display.data(),m.record->displayKeys,sizeof(display));std::string error;CHECK(mc::CaptureLayers(m.raw(),morphAuthor,error));}
    void Check(const Model& m)const{
        CHECK(Bits(*record,*m.record)&&Bits(bones,m.bones)&&Bits(chain,m.chain)&&Bits(links,m.links)&&Bits(selectors,m.selectors));
        CHECK(Bits(parents,m.parents)&&Bits(enabled,m.enabled)&&Bits(morphs,m.morphs)&&Bits(boneMorphs,m.boneMorphs)&&Bits(group,m.group)&&Bits(work,m.morphWork)&&Bits(selected,m.selected)&&Bits(physics,m.physics));
        CHECK(keys.size()==mdl::BoneKeyCapacity(m.record->boneKeys)&&!std::memcmp(keys.data(),m.record->boneKeys,keys.size()*sizeof(mdl::BoneKey)));
        CHECK(!std::memcmp(morphKeys.data(),m.record->morphKeys,sizeof(morphKeys))&&!std::memcmp(display.data(),m.record->displayKeys,sizeof(display)));
        const auto now=bs::Capture(m.raw());CHECK(now.pose==scale.pose&&now.keys==scale.keys&&now.curves==scale.curves&&revision==bs::Revision(m.raw())&&applied==bs::TransformsApplied(m.raw()));
        CHECK(bc::AuthoringState::Capture(m.raw())->Equals(*author)&&mc::LayersUnchanged(m.raw(),morphAuthor));
    }
};
using SceneSource=BoneSceneSource;
using SceneFrame=BoneSceneFrame;
constexpr std::uint64_t ample=512ull*1024*1024;
struct Scene {
    std::array<std::unique_ptr<Model>,3> models;
    const std::array<unsigned,3> slots{11,7,2};
    const std::array<unsigned,3> inputOrder{0,2,1};
    std::array<BoneSceneModelInput,3> inputs{};
    Scene(){
        for(unsigned i=0;i<3;++i){
            models[i]=std::make_unique<Model>();
            models[i]->record->comboSelIndex2=0;
            // The after-physics driver rotates BEFORE-physics links. Another
            // model consumes those links, making the scene sweep order visible.
            models[i]->bones[4].flags|=mdl::kBoneFlagAfterPhysics;
            models[i]->record->boneKeys[0].position[0]+=3*float(i);
            models[i]->record->boneKeys[10].position[0]+=3*float(i);
            for(unsigned key=0;key<2;++key){
                models[i]->parents[key][0]=i<2?mdl::BoneReference{int(slots[i+1]),2}:mdl::BoneReference{-1,0};
            }
        }
        FreeRoot(*models[0]);
        for(unsigned input=0;input<3;++input){const auto i=inputOrder[input];inputs[input]={slots[i],{100+i,900+i,7+i},models[i]->raw()};}
    }
    Model& InputModel(unsigned index)const{return *models[inputOrder[index]];}
};
struct ExpectedModel {std::vector<Pose> bones;uint8_t visibility=0;};
using ExpectedScene=std::vector<ExpectedModel>;
ExpectedScene Native(const Scene& scene,int time,bool wronglyFinishEachModel=false){
    std::array<std::unique_ptr<Model>,3> copies;
    std::array<unsigned char*,12> slots{};
    std::array<unsigned,3> order{0,1,2};
    ExpectedScene result(3);
    for(unsigned i=0;i<3;++i){copies[i]=std::make_unique<Model>();MirrorFresh(scene.InputModel(i),*copies[i]);
        copies[i]->record->comboSelIndex2=scene.InputModel(i).record->comboSelIndex2;
        slots[scene.inputs[i].slot]=copies[i]->raw();SeekModelFrame(copies[i]->raw(),time,0);
        result[i].bones.resize(copies[i]->record->boneCount);result[i].visibility=copies[i]->record->loadComplete;}
    std::sort(order.begin(),order.end(),[&](unsigned a,unsigned b){
        const auto left=copies[a]->record->comboSelIndex2,right=copies[b]->record->comboSelIndex2;
        return left==right?scene.inputs[a].slot<scene.inputs[b].slot:left<right;});
    const auto sweep=[&](unsigned index,unsigned phase){auto& model=*copies[index];
        ModelApplyMorphs(model.raw());SetPhysicsMode(model.raw(),static_cast<unsigned char>(phase),slots.data(),0);
        for(unsigned b=0;b<result[index].bones.size();++b)std::memcpy(result[index].bones[b].attachment[phase].data(),model.bones[b].matExtra,64);
    };
    // Deliberately wrong schedule is only a sensitivity check, never the oracle.
    if(wronglyFinishEachModel){for(const auto i:order){sweep(i,0);sweep(i,1);}}
    else {for(unsigned phase=0;phase<2;++phase)for(const auto i:order)sweep(i,phase);}
    for(unsigned i=0;i<3;++i){auto& model=*copies[i];
        for(unsigned b=0;b<result[i].bones.size();++b)std::memcpy(result[i].bones[b].rigid.data(),model.bones[b].matInit,64);
        bs::ApplyTransforms(model.raw());
        for(unsigned b=0;b<result[i].bones.size();++b){auto& pose=result[i].bones[b];std::memcpy(pose.scaled.data(),model.bones[b].matInit,64);
            for(unsigned c=0;c<3;++c){double value=pose.scaled[12+c];for(unsigned k=0;k<3;++k)value+=double(model.bones[b].position[k])*pose.scaled[k*4+c];pose.jointPosition[c]=float(value);}}
    }
    return result;
}
bool SameMatrices(const ExpectedScene& left,const ExpectedScene& right){
    if(left.size()!=right.size())return false;
    for(unsigned i=0;i<left.size();++i){if(left[i].bones.size()!=right[i].bones.size()||left[i].visibility!=right[i].visibility)return false;
        for(unsigned b=0;b<left[i].bones.size();++b){const auto& a=left[i].bones[b];const auto& z=right[i].bones[b];
            if(!Bits(a.rigid,z.rigid)||!Bits(a.scaled,z.scaled)||!Bits(a.attachment,z.attachment))return false;}}
    return true;
}
std::array<BoneSceneModelInfo,3> Infos(const Scene& scene){
    std::array<BoneSceneModelInfo,3> result{};
    for(unsigned i=0;i<3;++i)result[i]={scene.inputs[i].slot,scene.InputModel(i).record->comboSelIndex2,scene.InputModel(i).record->boneCount,scene.inputs[i].identity};
    return result;
}
bool SameInfo(const BoneSceneModelInfo& a,const BoneSceneModelInfo& b){return a.slot==b.slot&&a.calculationOrder==b.calculationOrder&&a.boneCount==b.boneCount&&a.identity.lo==b.identity.lo&&a.identity.hi==b.identity.hi&&a.identity.generation==b.identity.generation;}
bool Matches(const SceneFrame& frame,const ExpectedScene& expected,double time,const std::array<BoneSceneModelInfo,3>& infos){
    if(frame.ModelCount()!=expected.size()||frame.Frame()!=time)return false;
    for(unsigned i=0;i<expected.size();++i){BoneSceneModelInfo info{};if(!frame.ModelAt(i,info)||!SameInfo(info,infos[i]))return false;
        const auto* child=frame.ModelFrame(i);if(!child||child->BoneCount()!=expected[i].bones.size()||child->Visibility()!=expected[i].visibility||child->Frame()!=time)return false;
        for(unsigned b=0;b<expected[i].bones.size();++b){Pose actual;if(!child->BoneAt(b,actual))return false;const auto& e=expected[i].bones[b];
            if(!Bits(actual.rigid,e.rigid)||!Bits(actual.scaled,e.scaled)||!Bits(actual.attachment,e.attachment))return false;
            for(unsigned c=0;c<3;++c)if(!std::isfinite(actual.jointPosition[c])||std::abs(double(actual.jointPosition[c])-e.jointPosition[c])>2e-5)return false;}
    }
    return true;
}
void CheckFrame(const SceneFrame& frame,const ExpectedScene& expected,double time,const std::array<BoneSceneModelInfo,3>& infos){
    failAfter=0;const bool matches=Matches(frame,expected,time,infos);failAfter=-1;CHECK(matches&&frame.RetainedBytes()>0);
    BoneSceneModelInfo out;std::memset(&out,0xa5,sizeof(out));const auto original=out;
    CHECK(!frame.ModelAt(frame.ModelCount(),out)&&Bits(out,original));CHECK(frame.ModelFrame(frame.ModelCount())==nullptr);
    for(unsigned i=0;i<frame.ModelCount();++i){Pose pose;std::memset(&pose,0xb6,sizeof(pose));const auto before=pose;const auto* child=frame.ModelFrame(i);
        CHECK(!child->BoneAt(child->BoneCount(),pose)&&Bits(pose,before));}
}
void CheckSourceInfo(const SceneSource& source,const std::array<BoneSceneModelInfo,3>& infos){
    CHECK(source.ModelCount()==3);for(unsigned i=0;i<3;++i){BoneSceneModelInfo info{};failAfter=0;const bool ok=source.ModelAt(i,info);failAfter=-1;CHECK(ok&&SameInfo(info,infos[i]));}
    BoneSceneModelInfo out;std::memset(&out,0xce,sizeof(out));const auto before=out;CHECK(!source.ModelAt(3,out)&&Bits(out,before));
}
void SceneSweeps(const BoneTransformContext& context){
    currentCase="native scene sweep order, transitive attachments and active IK";
    Scene scene;const auto correct=Native(scene,5),wrong=Native(scene,5,true);
    CHECK(!SameMatrices(correct,wrong)); // This fixture must detect the original scheduling hazard.
    const auto on=correct;for(auto& model:scene.models)model->enabled[0][0]=model->enabled[1][0]=0;
    const auto off=Native(scene,5);CHECK(!SameMatrices(on,off));
    CheckFrame(*SceneSource::Capture(scene.inputs.data(),3)->Evaluate(5,context),off,5,Infos(scene));
    for(auto& model:scene.models)model->enabled[0][0]=model->enabled[1][0]=197;
    const std::array<std::array<unsigned char,3>,4> orders{{{0,0,0},{0,127,255},{255,127,0},{255,255,255}}};
    for(const auto& order:orders){for(unsigned i=0;i<3;++i)scene.models[i]->record->comboSelIndex2=order[i];
        const State a(*scene.models[0]),b(*scene.models[1]),c(*scene.models[2]);const auto source=SceneSource::Capture(scene.inputs.data(),3);const auto infos=Infos(scene);CheckSourceInfo(*source,infos);
        for(int time:{0,5,11,12,20,25,frame_limits::maximum})CheckFrame(*source->Evaluate(time,context),Native(scene,time),time,infos);
        a.Check(*scene.models[0]);b.Check(*scene.models[1]);c.Check(*scene.models[2]);
    }
    // Reverse only enumeration, preserving actual slots and calculation order.
    auto reversed=scene.inputs;std::reverse(reversed.begin(),reversed.end());const auto backwards=SceneSource::Capture(reversed.data(),3)->Evaluate(5,context);
    const auto forward=SceneSource::Capture(scene.inputs.data(),3)->Evaluate(5,context);
    for(unsigned i=0;i<3;++i){BoneSceneModelInfo info{};CHECK(backwards->ModelAt(2-i,info)&&SameInfo(info,Infos(scene)[i]));
        for(unsigned bone=0;bone<10;++bone){Pose p,q;CHECK(forward->ModelFrame(i)->BoneAt(bone,p)&&backwards->ModelFrame(2-i)->BoneAt(bone,q));CHECK(Bits(p.rigid,q.rigid)&&Bits(p.scaled,q.scaled)&&Bits(p.attachment,q.attachment));}}
}
void CyclesAndFutureKeys(const BoneTransformContext& context){
    currentCase="self and cyclic dependencies use current native workspace";
    for(unsigned kind=0;kind<2;++kind){Scene scene;
        for(unsigned i=0;i<3;++i)for(auto& key:scene.models[i]->parents)key[0]={int(scene.slots[kind?i:(i+1)%3]),2};
        const auto source=SceneSource::Capture(scene.inputs.data(),3);for(int time:{0,5,20})CheckFrame(*source->Evaluate(time,context),Native(scene,time),time,Infos(scene));
    }
    currentCase="future selector keys switch real targets including local selectors";
    Scene scene;scene.models[0]->parents[1][0]={int(scene.slots[2]),1};
    for(auto& key:scene.models[0]->parents){key[1]={int(scene.slots[2]),2};key[2]={int(scene.slots[2]),2};}
    // Both explicit selectors intentionally share one real target; global changes.
    const auto source=SceneSource::Capture(scene.inputs.data(),3);const State before(*scene.models[0]);
    for(int time:{0,11,12,13,20})CheckFrame(*source->Evaluate(time,context),Native(scene,time),time,Infos(scene));
    before.Check(*scene.models[0]);
    const auto early=source->Evaluate(11.75,context),late=source->Evaluate(12.0,context);CHECK(early->Frame()==11.75&&late->Frame()==12);
}
void GroundSelectors(const BoneTransformContext& context){
    currentCase="ground -2 preserves native local-versus-inherited parent semantics";
    Scene scene;
    // Root 9 uses selector 1. The ordinary -1 fallback inherits the moving
    // global target; explicit ground -2 keeps its own local root transform.
    const auto inherited=Native(scene,5);
    for(auto& key:scene.models[0]->parents)key[1]={-2,0};
    const auto grounded=Native(scene,5);
    CHECK(!Bits(inherited[0].bones[9].rigid,grounded[0].bones[9].rigid));
    CheckFrame(*SceneSource::Capture(scene.inputs.data(),3)->Evaluate(5,context),grounded,5,Infos(scene));
    // Keep the native raw sentinel through both kinds of future-key windows.
    for(unsigned selector:{0u,1u}){Scene future;future.models[0]->parents[1][selector]={-2,0};
        const State before(*future.models[0]);const auto source=SceneSource::Capture(future.inputs.data(),3);
        for(int time:{0,11,12,13,20})CheckFrame(*source->Evaluate(time,context),Native(future,time),time,Infos(future));
        before.Check(*future.models[0]);
    }
}
void SelfLocalDependency(const BoneTransformContext& context){
    currentCase="self local selector reads an already evaluated same-workspace target";
    Scene scene;for(auto& model:scene.models)for(auto& key:model->parents)for(auto& parent:key)parent={-1,0};
    // Earlier bone 2 has a nonzero world transform when later root 9 uses it.
    for(auto& key:scene.models[0]->parents)key[1]={int(scene.slots[0]),2};
    const auto expected=Native(scene,5);const auto source=SceneSource::Capture(scene.inputs.data(),3);const auto actual=source->Evaluate(5,context);
    CheckFrame(*actual,expected,5,Infos(scene));Pose self{};CHECK(actual->ModelFrame(0)->BoneAt(9,self));
    CHECK(self.rigid[15]==1&&(self.rigid[12]!=0||self.rigid[13]!=0||self.rigid[14]!=0));
    // A tempting but incorrect coordinator freezes the target's initial zero
    // matrix as an external dependency instead of wiring the working model.
    Dependency initial{};initial.selector=1;initial.modelIndex=int(scene.slots[0]);initial.boneIndex=2;
    std::memcpy(initial.restPosition.data(),scene.models[0]->bones[2].position,12);
    const auto isolated=Source::Capture(scene.models[0]->raw());const auto stale=isolated->Evaluate(5,context,&initial,1);Pose wrong{};
    CHECK(stale->BoneAt(9,wrong)&&!Bits(self.rigid,wrong.rigid));
}
void InvalidInputs(const BoneTransformContext& context){
    currentCase="scene capture rejects ambiguous identities and unresolved future keys";
    Scene scene;const State a(*scene.models[0]),b(*scene.models[1]),c(*scene.models[2]);
    CHECK(Throws<std::invalid_argument>([]{SceneSource::Capture(nullptr,1);}));
    for(unsigned mutation=0;mutation<6;++mutation){auto inputs=scene.inputs;
        if(mutation==0)inputs[0].model=nullptr;
        if(mutation==1)inputs[1].slot=inputs[0].slot;
        if(mutation==2)inputs[1].model=inputs[0].model;
        if(mutation==3){inputs[1].identity=inputs[0].identity;++inputs[1].identity.generation;}
        if(mutation==4)inputs[0].identity.lo=inputs[0].identity.hi=0;
        if(mutation==5)inputs[0].identity.generation=0;
        CHECK(Throws<std::invalid_argument>([&]{SceneSource::Capture(inputs.data(),3);}));
    }
    // Existing current key resolves correctly: rejecting only at Evaluate(12)
    // would let an incomplete source escape and fails this capture-time test.
    const auto saved=scene.models[0]->parents[1][0];
    for(const auto bad:std::array<mdl::BoneReference,4>{{{3,1},{int(scene.slots[2]),10},{int(scene.slots[2]),-1},{-3,0}}}){
        scene.models[0]->parents[1][0]=bad;CHECK(Throws<std::invalid_argument>([&]{SceneSource::Capture(scene.inputs.data(),3);}));
    }
    scene.models[0]->parents[1][0]=saved;
    CHECK(Throws<std::invalid_argument>([&]{SceneSource::Capture(scene.inputs.data(),2);})); // Missing middle target.
    a.Check(*scene.models[0]);b.Check(*scene.models[1]);c.Check(*scene.models[2]);
    const auto source=SceneSource::Capture(scene.inputs.data(),3);const auto good=source->Evaluate(5,context);const auto expected=Native(scene,5);
    for(double time:{-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),double(frame_limits::maximum)+1})
        CHECK(Throws<std::invalid_argument>([&]{source->Evaluate(time,context);}));
    CheckFrame(*good,expected,5,Infos(scene));
    currentCase="finite source motion with nonfinite intermediate result publishes no scene";
    // Model 2 has no authored override: a float delta of 2*FLT_MAX overflows.
    auto& extreme=*scene.models[2];extreme.record->boneKeys[0].position[0]=-std::numeric_limits<float>::max();
    extreme.record->boneKeys[10].position[0]=std::numeric_limits<float>::max();const State extremeBefore(extreme);
    const auto overflow=SceneSource::Capture(scene.inputs.data(),3);CHECK(Throws<std::overflow_error>([&]{overflow->Evaluate(5,context);}));
    extremeBefore.Check(extreme);CheckFrame(*good,expected,5,Infos(scene));
}
std::uint64_t MinimumWorkspace(const SceneSource& source,const BoneTransformContext& context){
    std::uint64_t lo=0,hi=1024;
    while(Throws<std::length_error>([&]{source.Evaluate(5,context,ample,hi);})){lo=hi;hi*=2;CHECK(hi<=ample);}
    while(lo+1<hi){const auto mid=lo+(hi-lo)/2;if(Throws<std::length_error>([&]{source.Evaluate(5,context,ample,mid);}))lo=mid;else hi=mid;}
    return hi;
}
void BudgetsAndAllocation(const BoneTransformContext& context){
    currentCase="aggregate source, frame and simultaneous workspace budgets";
    Scene scene;for(auto& model:scene.models)for(auto& key:model->parents)for(auto& parent:key)parent={-1,0};
    const State a(*scene.models[0]),b(*scene.models[1]),c(*scene.models[2]);
    const auto source=SceneSource::Capture(scene.inputs.data(),3);const auto retained=source->RetainedBytes();CHECK(retained>0);
    CHECK(SceneSource::Capture(scene.inputs.data(),3,retained)->RetainedBytes()==retained);
    CHECK(Throws<std::length_error>([&]{SceneSource::Capture(scene.inputs.data(),3,retained-1);}));
    const auto single=SceneSource::Capture(scene.inputs.data(),1);CHECK(single->RetainedBytes()<retained);
    CHECK(Throws<std::length_error>([&]{SceneSource::Capture(scene.inputs.data(),3,single->RetainedBytes());}));
    const auto stable=source->Evaluate(5,context);const auto expected=Native(scene,5);const auto bytes=stable->RetainedBytes();CHECK(bytes>0);
    CheckFrame(*source->Evaluate(5,context,bytes),expected,5,Infos(scene));
    CHECK(Throws<std::length_error>([&]{source->Evaluate(5,context,bytes-1);}));
    const auto singleFrame=single->Evaluate(5,context);CHECK(singleFrame->RetainedBytes()<bytes);
    CHECK(Throws<std::length_error>([&]{source->Evaluate(5,context,singleFrame->RetainedBytes());}));
    const auto work=MinimumWorkspace(*source,context),singleWork=MinimumWorkspace(*single,context);CHECK(work>singleWork&&singleWork>0);
    CheckFrame(*source->Evaluate(5,context,ample,work),expected,5,Infos(scene));
    CHECK(Throws<std::length_error>([&]{source->Evaluate(5,context,ample,work-1);}));
    CHECK(Throws<std::length_error>([&]{source->Evaluate(5,context,ample,singleWork);}));
    a.Check(*scene.models[0]);b.Check(*scene.models[1]);c.Check(*scene.models[2]);
    currentCase="capture and scene evaluation allocation failures are atomic";
    for(unsigned operation=0;operation<2;++operation){bool completed=false;unsigned failed=0;
        for(int allowance=0;allowance<4096&&!completed;++allowance){bool allocationFailed=false,unexpected=false;
            failAfter=allowance;try{if(operation==0){auto candidate=SceneSource::Capture(scene.inputs.data(),3);completed=bool(candidate);}else{auto candidate=source->Evaluate(5,context);completed=bool(candidate);}}
            catch(const std::bad_alloc&){allocationFailed=true;}catch(...){unexpected=true;}failAfter=-1;
            CHECK(!unexpected);if(allocationFailed)++failed;
            a.Check(*scene.models[0]);b.Check(*scene.models[1]);c.Check(*scene.models[2]);CheckFrame(*stable,expected,5,Infos(scene));
        }
        CHECK(completed&&failed>0);
    }
}
void LifetimeAndConcurrency(){
    currentCase="scene captures target motion and retains identity despite slot reuse";
    std::unique_ptr<SceneSource> frozen;std::unique_ptr<SceneFrame> retained;ExpectedScene expected;std::array<BoneSceneModelInfo,3> infos{};
    {
        auto scene=std::make_unique<Scene>();auto context=BoneTransformContext::Create();CHECK(bool(context));
        expected=Native(*scene,5);infos=Infos(*scene);frozen=SceneSource::Capture(scene->inputs.data(),3);retained=frozen->Evaluate(5,*context);
        scene->models[2]->record->boneKeys[10].position[0]+=7;
        const auto now=SceneSource::Capture(scene->inputs.data(),3);const auto newExpected=Native(*scene,5);CHECK(!SameMatrices(expected,newExpected));
        // The changed terminal target must propagate through the intermediate
        // model to the first input, not merely change its own isolated output.
        CHECK(!Bits(expected[0].bones[0].rigid,newExpected[0].bones[0].rigid));
        CheckFrame(*now->Evaluate(5,*context),newExpected,5,infos);CheckFrame(*frozen->Evaluate(5,*context),expected,5,infos);
        // Replace the target at the exact old slot with a new identity and body.
        scene->models[2]=std::make_unique<Model>();scene->models[2]->record->boneKeys[0].position[0]=40;
        scene->inputs[1].model=scene->models[2]->raw();scene->inputs[1].identity={55,66,77};
        const auto reused=SceneSource::Capture(scene->inputs.data(),3);BoneSceneModelInfo newInfo{};
        CHECK(reused->ModelAt(1,newInfo)&&newInfo.slot==infos[1].slot&&newInfo.identity.generation==77);
        CheckFrame(*frozen->Evaluate(5,*context),expected,5,infos);scene.reset();
        CheckFrame(*frozen->Evaluate(5,*context),expected,5,infos);
    }
    // Every original model, input array and context has now been destroyed.
    frozen.reset();CheckFrame(*retained,expected,5,infos);
    currentCase="concurrent fresh scene evaluations and allocation-free immutable reads";
    Scene scene;const auto source=SceneSource::Capture(scene.inputs.data(),3);const auto context=BoneTransformContext::Create();CHECK(bool(context));
    const int times[]{0,5,12,20};std::array<ExpectedScene,4> reference;for(unsigned i=0;i<4;++i)reference[i]=Native(scene,times[i]);const auto identity=Infos(scene);
    const State a(*scene.models[0]),b(*scene.models[1]),c(*scene.models[2]);std::atomic<unsigned> failures{0};std::array<std::thread,4> workers;
    try{for(unsigned i=0;i<4;++i)workers[i]=std::thread([&,i,lease=context]{try{
        for(unsigned repeat=0;repeat<8;++repeat){const auto frame=source->Evaluate(times[i],*lease);failAfter=0;
            const bool ok=Matches(*frame,reference[i],times[i],identity)&&Matches(*retained,expected,5,infos);failAfter=-1;if(!ok)++failures;}
    }catch(...){failAfter=-1;++failures;}});}catch(...){for(auto& worker:workers)if(worker.joinable())worker.join();throw;}
    for(auto& worker:workers)worker.join();CHECK(failures==0);a.Check(*scene.models[0]);b.Check(*scene.models[1]);c.Check(*scene.models[2]);
}
void Empty(const BoneTransformContext& context){
    currentCase="empty captured scene is immutable and preserves frame time";
    const auto source=SceneSource::Capture(nullptr,0);CHECK(source->ModelCount()==0);
    for(double time:{0.0,.5,double(frame_limits::maximum)}){const auto frame=source->Evaluate(time,context);CHECK(frame->ModelCount()==0&&frame->Frame()==time&&frame->ModelFrame(0)==nullptr);
        BoneSceneModelInfo out;std::memset(&out,0x7c,sizeof(out));const auto before=out;CHECK(!source->ModelAt(0,out)&&Bits(before,out));CHECK(!frame->ModelAt(0,out)&&Bits(before,out));}
    CHECK(Throws<std::invalid_argument>([&]{source->Evaluate(-.5,context);}));
}
bool ExactSceneNear(const SceneFrame& actual,const SceneFrame& expected){
    if(actual.ModelCount()!=expected.ModelCount())return false;
    const auto closeValues=[](const auto& a,const auto& b){for(std::size_t i=0;i<a.size();++i)
        if(!std::isfinite(a[i])||!std::isfinite(b[i])||std::abs(a[i]-b[i])>5e-5f*(1.f+std::abs(b[i])))return false;return true;};
    for(uint32_t m=0;m<actual.ModelCount();++m){BoneSceneModelInfo aInfo{},bInfo{};
        if(!actual.ModelAt(m,aInfo)||!expected.ModelAt(m,bInfo)||!SameInfo(aInfo,bInfo))return false;
        const auto* a=actual.ModelFrame(m);const auto* b=expected.ModelFrame(m);
        if(!a||!b||a->BoneCount()!=b->BoneCount()||a->Visibility()!=b->Visibility())return false;
        for(uint32_t i=0;i<a->BoneCount();++i){Pose p,q;if(!a->BoneAt(i,p)||!b->BoneAt(i,q)||
            !closeValues(p.rigid,q.rigid)||!closeValues(p.scaled,q.scaled)||!closeValues(p.attachment[0],q.attachment[0])||
            !closeValues(p.attachment[1],q.attachment[1])||!closeValues(p.jointPosition,q.jointPosition))return false;}}
    return true;
}
void ExactSceneTime(const BoneTransformContext& context){
    currentCase="scene exact rational selector ground IK and visibility boundaries";
    EvaluationTime beforeKey;CHECK(EvaluationTime::FromSeconds(108086391056891904LL,270215977642229761ULL,beforeKey));
    CHECK(beforeKey.Frame()==12&&beforeKey.DiscreteFrame()==11);const double previous=std::nextafter(12.0,0.0);
    EvaluationTime ordinary;CHECK(EvaluationTime::FromSeconds(11,60,ordinary)&&ordinary.Frame()==5.5&&ordinary.DiscreteFrame()==5);
    // Isolate each discrete cause. Otherwise a working IK switch could hide a
    // broken parent switch behind the same visible matrix difference.
    for(unsigned scenario=0;scenario<3;++scenario){Scene scene;
        if(scenario==0)scene.models[0]->parents[1][0]={-2,91}; // global MODEL -> GROUND
        if(scenario==1){scene.models[0]->parents[0][1]={int(scene.slots[2]),0};scene.models[0]->parents[1][1]={-2,92};}
        if(scenario==2)scene.models[2]->enabled[1][0]=0;
        const State a(*scene.models[0]),b(*scene.models[1]),c(*scene.models[2]);
        const auto source=SceneSource::Capture(scene.inputs.data(),3);
        const auto expected=source->Evaluate(previous,context),exact=source->EvaluateAt(beforeKey,context),rounded=source->Evaluate(12,context);
        CHECK(exact->Frame()==12&&ExactSceneNear(*exact,*expected));
        for(uint32_t i=0;i<3;++i)CHECK(exact->ModelFrame(i)->Frame()==12&&exact->ModelFrame(i)->Visibility()==255&&rounded->ModelFrame(i)->Visibility()==197);
        const unsigned row=scenario==2?1:0,bone=scenario==2?2:9;Pose before,after;
        CHECK(exact->ModelFrame(row)->BoneAt(bone,before)&&rounded->ModelFrame(row)->BoneAt(bone,after));
        CHECK(!Bits(before.rigid,after.rigid)); // Nondegenerate parent/CCD output, independently of visibility.
        const auto fractional=source->EvaluateAt(ordinary,context),legacy=source->Evaluate(5.5,context),quantized=source->Evaluate(5,context);
        CHECK(fractional->Frame()==5.5&&ExactSceneNear(*fractional,*legacy));
        Pose moving,integer;CHECK(fractional->ModelFrame(0)->BoneAt(0,moving)&&quantized->ModelFrame(0)->BoneAt(0,integer));
        CHECK(!Bits(moving.rigid,integer.rigid)); // Continuous free/native motion remains at 5.5, not floor 5.
        a.Check(*scene.models[0]);b.Check(*scene.models[1]);c.Check(*scene.models[2]);
    }
    std::shared_ptr<const SceneSource> frozen;std::unique_ptr<SceneFrame> expected,retained;
    auto math=BoneTransformContext::Create();CHECK(bool(math));
    {Scene scene;scene.models[0]->parents[1][1]={-2,0};scene.models[2]->enabled[1][0]=0;
        frozen=SceneSource::Capture(scene.inputs.data(),3);expected=frozen->Evaluate(previous,*math);retained=frozen->EvaluateAt(beforeKey,*math);}
    std::atomic<unsigned> failures{0};std::array<std::thread,2> readers;
    try{for(auto& reader:readers)reader=std::thread([&,source=frozen,lease=math]{try{for(unsigned n=0;n<6;++n){const auto f=source->EvaluateAt(beforeKey,*lease);
        if(f->Frame()!=12||!ExactSceneNear(*f,*expected))++failures;}}catch(...){++failures;}});}
    catch(...){for(auto& reader:readers)if(reader.joinable())reader.join();throw;}
    frozen.reset();math.reset();for(auto& reader:readers)reader.join();CHECK(failures==0);
    CHECK(retained->Frame()==12&&ExactSceneNear(*retained,*expected));
}
}
int main(){try{const auto context=BoneTransformContext::Create();CHECK(bool(context));SceneSweeps(*context);CyclesAndFutureKeys(*context);GroundSelectors(*context);SelfLocalDependency(*context);InvalidInputs(*context);BudgetsAndAllocation(*context);LifetimeAndConcurrency();Empty(*context);ExactSceneTime(*context);
    std::printf("PASS %u bone scene evaluation checks\n",checks);return 0;
}catch(const std::exception& error){failAfter=-1;std::fprintf(stderr,"FAIL %s unexpected exception: %s\n",currentCase,error.what());return 1;}}
