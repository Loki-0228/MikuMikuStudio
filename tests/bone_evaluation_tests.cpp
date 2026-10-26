// Internal frozen bone frames compared with the real native frame pipeline.
#define NOMINMAX
#include "mikumikustudio/bone_evaluation.hpp"
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
struct Expected {std::vector<Pose> bones;uint8_t visibility=0;};
Expected Native(const Model& source,int frame,const std::vector<Dependency>& deps={},bool compressLastLayer=false){
    Model oracle;MirrorFresh(source,oracle);
    if(compressLastLayer){for(auto& b:oracle.bones)if(b.layer==INT32_MAX)b.layer=2;oracle.record->maxBoneLayer=2;}
    SeekModelFrame(oracle.raw(),frame,0);
    Expected result;result.bones.resize(oracle.record->boneCount);result.visibility=oracle.record->loadComplete;
    // These real native proxy models intentionally retain nonzero raw slot and
    // bone indices; the implementation's compact remap is not used as oracle.
    std::array<std::unique_ptr<mdl::ModelRecord>,7> targets;
    std::array<std::array<mdl::BoneRecord,5>,7> targetBones{};std::array<unsigned char*,7> slots{};
    for(const auto& dep:deps){CHECK(dep.modelIndex>=0&&dep.modelIndex<7&&dep.boneIndex>=0&&dep.boneIndex<5);const auto slot=unsigned(dep.modelIndex);
        if(!targets[slot]){targets[slot]=std::make_unique<mdl::ModelRecord>();targets[slot]->boneCount=5;targets[slot]->boneTable=targetBones[slot].data();slots[slot]=reinterpret_cast<unsigned char*>(targets[slot].get());}}
    for(int phase=0;phase<2;++phase){
        for(const auto& dep:deps){auto& b=targetBones[unsigned(dep.modelIndex)][unsigned(dep.boneIndex)];std::memcpy(b.position,dep.restPosition.data(),12);std::memcpy(b.matExtra,dep.attachment[phase].data(),64);}
        ModelApplyMorphs(oracle.raw());SetPhysicsMode(oracle.raw(),phase,slots.data(),0);
        for(unsigned b=0;b<result.bones.size();++b)std::memcpy(result.bones[b].attachment[phase].data(),oracle.bones[b].matExtra,64);
    }
    for(unsigned b=0;b<result.bones.size();++b)std::memcpy(result.bones[b].rigid.data(),oracle.bones[b].matInit,64);
    bs::ApplyTransforms(oracle.raw());
    for(unsigned b=0;b<result.bones.size();++b){auto& expected=result.bones[b];std::memcpy(expected.scaled.data(),oracle.bones[b].matInit,64);
        for(unsigned c=0;c<3;++c){double value=expected.scaled[12+c];for(unsigned k=0;k<3;++k)value+=double(oracle.bones[b].position[k])*expected.scaled[k*4+c];expected.jointPosition[c]=float(value);}}
    return result;
}
bool Equal(const Pose& a,const Pose& b){return Bits(a.rigid,b.rigid)&&Bits(a.scaled,b.scaled)&&Bits(a.attachment,b.attachment)&&Bits(a.jointPosition,b.jointPosition);}
void CheckFrame(const Frame& frame,const Expected& expected,double time){
    CHECK(frame.BoneCount()==expected.bones.size()&&frame.Visibility()==expected.visibility&&frame.Frame()==time&&frame.RetainedBytes()>0);
    for(unsigned b=0;b<expected.bones.size();++b){Pose actual;failAfter=0;const bool ok=frame.BoneAt(b,actual);failAfter=-1;CHECK(ok);
        CHECK(Bits(actual.rigid,expected.bones[b].rigid)&&Bits(actual.scaled,expected.bones[b].scaled)&&Bits(actual.attachment,expected.bones[b].attachment));
        for(unsigned c=0;c<3;++c)CHECK(std::isfinite(actual.jointPosition[c])&&std::abs(double(actual.jointPosition[c])-expected.bones[b].jointPosition[c])<2e-5);}
    Pose sentinel;std::memset(&sentinel,0xa5,sizeof(sentinel));auto out=sentinel;CHECK(!frame.BoneAt(frame.BoneCount(),out)&&Bits(out,sentinel));
}
void NativePipeline(const BoneTransformContext& context){
    currentCase="fresh native seek morph two-sweep and scale parity";
    for(unsigned variant=0;variant<3;++variant){Model model;if(variant==0)FreeRoot(model);if(variant==1){model.record->physicsMode=0;std::memcpy(model.bones[2].name,"\x8d\xb6\x82\xd0\x82\xb4",6);}
        if(variant==2){model.bones[2].twistEnable=197;for(unsigned axis=0;axis<3;++axis){model.bones[2].ikLimitMin[axis]=-.04f;model.bones[2].ikLimitMax[axis]=.04f;}}
        const State before(model);const auto source=Source::Capture(model.raw());CHECK(source->BoneCount()==10&&source->SelectorCount()==3);
        for(int time:{0,5,12,20,25,frame_limits::maximum}){const auto expected=Native(model,time);const auto frame=source->Evaluate(time,context);CheckFrame(*frame,expected,time);}
        before.Check(model);
    }
    Model enabled,disabled;disabled.enabled[0][0]=disabled.enabled[1][0]=0;
    const auto onSource=Source::Capture(enabled.raw()),offSource=Source::Capture(disabled.raw());
    const auto on=onSource->Evaluate(5,context),off=offSource->Evaluate(5,context);Pose a,b;CHECK(on->BoneAt(2,a)&&off->BoneAt(2,b));CHECK(!Bits(a.rigid,b.rigid));
    CheckFrame(*off,Native(disabled,5),5); // Disabling IK changes a genuinely moving chain.
}
void LayerAndDerivedRules(const BoneTransformContext& context){
    currentCase="derived fresh pose negative layers and sparse maximum";
    Model model;model.bones[8].type=mdl::BoneType::InertTip;model.record->boneKeys[8].position[0]=123;model.record->boneKeys[18].position[0]=456;
    model.record->boneKeys[6].position[0]=321;model.record->boneKeys[16].position[0]=654;
    model.bones[5].layer=-2;model.bones[9].layer=3;
    auto source=Source::Capture(model.raw());auto frame=source->Evaluate(5,context);CheckFrame(*frame,Native(model,5),5);
    Pose skipped;CHECK(frame->BoneAt(5,skipped));const bs::Transform zero{};CHECK(Bits(skipped.rigid,zero)&&Bits(skipped.attachment[0],zero)&&Bits(skipped.attachment[1],zero));
    CHECK(frame->BoneAt(9,skipped)&&Bits(skipped.rigid,zero));
    model.bones[5].layer=1;model.bones[9].layer=INT32_MAX;model.record->maxBoneLayer=INT32_MAX;
    source=Source::Capture(model.raw());frame=source->Evaluate(5,context);CheckFrame(*frame,Native(model,5,{},true),5);
    model.record->maxBoneLayer=-1;source=Source::Capture(model.raw());frame=source->Evaluate(5,context);CheckFrame(*frame,Native(model,5),5);
}
std::vector<Dependency> Dependencies(Model& m){
    // A root after-physics bone retains its local attachment in phase E;
    // non-root phase E intentionally follows its parent instead.
    m.bones[7].parent=-1;
    std::vector<Dependency> values(3);
    for(unsigned i=0;i<3;++i){auto& d=values[i];d.selector=i;d.modelIndex=i?6:3;d.boneIndex=i?4:2;d.restPosition={1,2,3};
        d.attachment[0]=d.attachment[1]=Identity();d.attachment[0][12]=i?3:2;d.attachment[0][13]=4;
        d.attachment[1][0]=d.attachment[1][5]=0;d.attachment[1][1]=1;d.attachment[1][4]=-1;d.attachment[1][12]=i?9:10;d.attachment[1][13]=-2;
        for(auto& row:m.parents)row[i]={d.modelIndex,d.boneIndex};}
    return values;
}
void ExplicitDependencies(const BoneTransformContext& context){
    currentCase="global and local explicit per-phase attachment inputs";
    Model model;auto deps=Dependencies(model);const auto depsBefore=deps;const State before(model);const auto source=Source::Capture(model.raw());
    for(int time:{0,5,12,20}){const auto frame=source->Evaluate(time,context,deps.data(),uint32_t(deps.size()));CheckFrame(*frame,Native(model,time,deps),time);}
    CHECK(Bits(deps,depsBefore));before.Check(model);
    const auto original=source->Evaluate(5,context,deps.data(),3);deps[0].attachment[1][12]+=5;deps[1].attachment[1][12]+=5;deps[2].attachment[1][12]+=5;
    const auto changed=source->Evaluate(5,context,deps.data(),3);Pose a,b;CHECK(original->BoneAt(7,a)&&changed->BoneAt(7,b)&&!Bits(a.rigid,b.rigid));CheckFrame(*changed,Native(model,5,deps),5);
    auto reject=[&](const Dependency* p,uint32_t n){CHECK(Throws<std::invalid_argument>([&]{source->Evaluate(5,context,p,n);}));};
    reject(nullptr,0);reject(nullptr,3);reject(deps.data(),2);
    for(unsigned variant=0;variant<8;++variant){auto invalid=deps;
        if(variant==0)invalid[2].selector=1;if(variant==1)invalid[0].selector=3;if(variant==2)++invalid[1].modelIndex;if(variant==3)++invalid[1].boneIndex;
        if(variant==4)invalid[0].restPosition[1]=std::numeric_limits<float>::quiet_NaN();
        if(variant==5)invalid[1].attachment[0][6]=std::numeric_limits<float>::infinity();
        if(variant==6)invalid[1].attachment[1][14]=std::numeric_limits<float>::quiet_NaN();
        if(variant==7){invalid.push_back(invalid[0]);invalid.back().selector=99;}
        reject(invalid.data(),uint32_t(invalid.size()));}
    // Selectors 1 and 2 refer to the same raw target. The matching pair above
    // is legal, but each part of its supplied target data must agree.
    for(unsigned conflict=0;conflict<3;++conflict){auto inconsistent=deps;
        if(conflict==0)inconsistent[2].restPosition[0]+=1;
        if(conflict==1)inconsistent[2].attachment[0][12]+=1;
        if(conflict==2)inconsistent[2].attachment[1][12]+=1;
        reject(inconsistent.data(),uint32_t(inconsistent.size()));}
    auto signedZeros=deps;
    signedZeros[1].restPosition[0]=0.f;signedZeros[2].restPosition[0]=-0.f;
    for(unsigned phase=0;phase<2;++phase){signedZeros[1].attachment[phase][2]=0.f;signedZeros[2].attachment[phase][2]=-0.f;}
    const auto equivalent=source->Evaluate(5,context,signedZeros.data(),3);
    CHECK(equivalent->BoneCount()==10&&equivalent->Frame()==5);
    Model inactive;const auto noParents=Source::Capture(inactive.raw());CHECK(Throws<std::invalid_argument>([&]{noParents->Evaluate(5,context,deps.data(),3);}));
    CheckFrame(*original,Native(model,5,depsBefore),5);before.Check(model);
}
void BudgetsAndFailures(const BoneTransformContext& context){
    currentCase="capture evaluation budgets and failure atomicity";
    CHECK(Throws<std::invalid_argument>([]{Source::Capture(nullptr);}));
    Model model;FreeRoot(model);const State before(model);const auto source=Source::Capture(model.raw());const auto expected=Native(model,5);
    const auto existing=source->Evaluate(5,context);const auto sourceBytes=source->RetainedBytes(),frameBytes=existing->RetainedBytes();
    CHECK(Throws<std::length_error>([&]{Source::Capture(model.raw(),sourceBytes-1);}));CHECK(Source::Capture(model.raw(),sourceBytes)->RetainedBytes()==sourceBytes);
    CHECK(Throws<std::length_error>([&]{source->Evaluate(5,context,nullptr,0,frameBytes-1);}));CheckFrame(*source->Evaluate(5,context,nullptr,0,frameBytes),expected,5);
    CHECK(Throws<std::length_error>([&]{source->Evaluate(5,context,nullptr,0,frameBytes,0);}));
    // Discover the actual workspace boundary without embedding private sizes.
    std::uint64_t low=0,high=128ull*1024*1024;
    while(high-low>1){const auto mid=low+(high-low)/2;try{const auto frame=source->Evaluate(5,context,nullptr,0,frameBytes,mid);high=mid;}catch(const std::length_error&){low=mid;}}
    CheckFrame(*source->Evaluate(5,context,nullptr,0,frameBytes,high),expected,5);
    CHECK(Throws<std::length_error>([&]{source->Evaluate(5,context,nullptr,0,frameBytes,high-1);}));
    for(bool capture:{true,false}){bool completed=false;unsigned failures=0;
        for(int allowance=0;allowance<1024;++allowance){bool failed=false,unexpected=false;std::unique_ptr<Source> s;std::unique_ptr<Frame> f;failAfter=allowance;
            try{if(capture)s=Source::Capture(model.raw());else f=source->Evaluate(5,context);}catch(const std::bad_alloc&){failed=true;}catch(...){unexpected=true;}
            failAfter=-1;CHECK(!unexpected);before.Check(model);CheckFrame(*existing,expected,5);
            if(!failed){if(capture)CheckFrame(*s->Evaluate(5,context),expected,5);else CheckFrame(*f,expected,5);completed=true;break;}++failures;CHECK(!s&&!f);}
        CHECK(completed&&failures>0);std::printf("bone frame capture=%d allocation failures=%u\n",int(capture),failures);}
    for(double time:{-1.,-.125,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),double(frame_limits::maximum)+.5})CHECK(Throws<std::invalid_argument>([&]{source->Evaluate(time,context);}));
    before.Check(model);
    Model overflow;overflow.record->morphCount=0;overflow.enabled[0][0]=overflow.enabled[1][0]=0;bs::Forget(overflow.raw());
    for(unsigned slot:{0u,1u,10u,11u})overflow.record->boneKeys[slot].position[0]=(std::numeric_limits<float>::max)();
    const auto excessive=Source::Capture(overflow.raw());CHECK(Throws<std::overflow_error>([&]{excessive->Evaluate(0,context);}));
    // Both endpoint values are finite, but native float subtraction for the
    // midpoint is not. This is a sampling overflow, not a malformed request.
    Model delta;delta.record->boneKeys[0].position[0]=-(std::numeric_limits<float>::max)();
    delta.record->boneKeys[10].position[0]=(std::numeric_limits<float>::max)();
    const State deltaBefore(delta);const auto deltaSource=Source::Capture(delta.raw());
    CHECK(Throws<std::overflow_error>([&]{deltaSource->Evaluate(10,context);}));deltaBefore.Check(delta);
    Model morphDelta;morphDelta.record->morphKeys[0].value=-(std::numeric_limits<float>::max)();
    morphDelta.record->morphKeys[2].value=(std::numeric_limits<float>::max)();
    const State morphBefore(morphDelta);const auto morphSource=Source::Capture(morphDelta.raw());
    CHECK(Throws<std::overflow_error>([&]{morphSource->Evaluate(10,context);}));morphBefore.Check(morphDelta);
    // Finite double curve handles may overshoot float range even though both
    // mirrored native key values are zero and therefore fully representable.
    Model freeMorph;ac::Key first,last;last.frame=20;first.tangent=last.tangent=ac::Tangent::Free;first.outSlope=1e40;
    ac::NamedTrack named;named.id=mc::StableId(0,freeMorph.morphs[0].name);named.track.keys={first,last};
    const double peak=ac::Evaluate(named.track,5).value;
    CHECK(std::isfinite(peak)&&peak>double((std::numeric_limits<float>::max)()));
    std::string error;CHECK(mc::Write(freeMorph.raw(),{named},error));
    const State freeBefore(freeMorph);const auto freeSource=Source::Capture(freeMorph.raw());
    CHECK(Throws<std::overflow_error>([&]{freeSource->Evaluate(5,context);}));freeBefore.Check(freeMorph);
    CheckFrame(*existing,expected,5);
}
void EmptyAndFractional(const BoneTransformContext& context){
    currentCase="empty source and fractional registered frames";
    Model empty;empty.record->boneCount=empty.record->morphCount=empty.record->ikChainCount=empty.record->boneOrderCount=0;empty.record->boneMorphCount=0;
    const auto source=Source::Capture(empty.raw());const auto frame=source->Evaluate(.25,context);CHECK(frame->BoneCount()==0&&frame->Frame()==.25&&frame->Visibility()==255);
    Pose p;std::memset(&p,0xa5,sizeof(p));const auto sentinel=p;CHECK(!frame->BoneAt(0,p)&&Bits(p,sentinel));
    Model simple;simple.record->boneCount=1;simple.record->morphCount=simple.record->ikChainCount=0;simple.record->boneOrderCount=1;simple.record->maxBoneLayer=0;simple.record->boneMorphCount=0;bs::Forget(simple.raw());
    simple.record->boneKeys[0].position[0]=0;simple.record->boneKeys[10].position[0]=20;
    const auto continuous=Source::Capture(simple.raw());
    for(double time:{.125,5.5,19.75}){const auto value=continuous->Evaluate(time,context);CHECK(value->BoneAt(0,p));CHECK(std::abs(p.rigid[12]-time)<1e-6&&Bits(p.rigid,p.scaled));}
}
void LifetimeAndConcurrentEvaluation(){
    currentCase="frozen source and independent frame lifetime";
    std::unique_ptr<Frame> retained;Expected oldExpected;std::unique_ptr<Source> frozen;
    {
        auto model=std::make_unique<Model>();auto deps=Dependencies(*model);auto context=BoneTransformContext::Create();CHECK(bool(context));
        oldExpected=Native(*model,5,deps);frozen=Source::Capture(model->raw());retained=frozen->Evaluate(5,*context,deps.data(),3);
        model->record->boneKeys[10].position[0]+=7;model->boneMorphs[0].translation[0]+=2;
        const auto changed=Source::Capture(model->raw());const auto newFrame=changed->Evaluate(5,*context,deps.data(),3);Pose a,b;CHECK(retained->BoneAt(0,a)&&newFrame->BoneAt(0,b)&&!Bits(a.rigid,b.rigid));
        model.reset();CheckFrame(*frozen->Evaluate(5,*context,deps.data(),3),oldExpected,5);
    }
    // The result outlives original model, context and dependency arrays.
    frozen.reset();CheckFrame(*retained,oldExpected,5);
    Model model;FreeRoot(model);const auto source=Source::Capture(model.raw());
    std::array<Expected,4> expected;const int times[]{0,5,12,20};for(unsigned i=0;i<4;++i)expected[i]=Native(model,times[i]);const State before(model);
    const auto context=BoneTransformContext::Create();CHECK(bool(context));std::atomic<unsigned> failures{0};std::array<std::thread,4> workers;
    try{for(unsigned i=0;i<4;++i)workers[i]=std::thread([&,i,lease=context]{try{for(unsigned repeat=0;repeat<12;++repeat){const auto f=source->Evaluate(times[i],*lease);if(f->BoneCount()!=10||f->Visibility()!=expected[i].visibility){++failures;continue;}
        for(unsigned bone=0;bone<10;++bone){Pose p;failAfter=0;const bool ok=f->BoneAt(bone,p);failAfter=-1;if(!ok||!Bits(p.rigid,expected[i].bones[bone].rigid)||!Bits(p.scaled,expected[i].bones[bone].scaled)||!Bits(p.attachment,expected[i].bones[bone].attachment))++failures;}}}catch(...){failAfter=-1;++failures;}});
    }catch(...){for(auto& worker:workers)if(worker.joinable())worker.join();throw;}
    for(auto& worker:workers)worker.join();CHECK(failures==0);before.Check(model);
}
}
int main(){try{const auto context=BoneTransformContext::Create();CHECK(bool(context));NativePipeline(*context);LayerAndDerivedRules(*context);ExplicitDependencies(*context);BudgetsAndFailures(*context);EmptyAndFractional(*context);LifetimeAndConcurrentEvaluation();std::printf("PASS %u bone evaluation checks\n",checks);return 0;}
    catch(const std::exception& error){failAfter=-1;std::fprintf(stderr,"FAIL %s unexpected exception: %s\n",currentCase,error.what());return 1;}}
