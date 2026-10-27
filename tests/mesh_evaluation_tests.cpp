// Independent CPU mesh frame against the real production CPU worker.
// The worker uses owned COM buffers; no D3D device, window or GPU is created.
#define main inherited_bone_evaluation_tests_main
#include "bone_evaluation_tests.cpp"
#undef main
#include "mikumikustudio/mesh_evaluation.hpp"
#include "mikumikustudio/mesh_scene_evaluation.hpp"
#include "mikumikustudio/mesh_definition_snapshot.hpp"
#include "mikumikustudio/morph_evaluation.hpp"
#include "mikumikustudio/mmd_app.hpp"
#include "mikumikustudio/model_vertex_cache.hpp"
#include <d3d9.h>
#include <type_traits>
namespace mikumikustudio { bool UpdateModelVertexBuffersCpu(MMDApp*, unsigned char*, const float[16]); }
namespace mesh_probe {
using namespace mikumikustudio;
using Mesh = MeshEvaluationFrame;
struct CpuBuffer final : IDirect3DVertexBuffer9 {
    std::vector<float> values;
    bool locked = false;
    explicit CpuBuffer(unsigned count): values(count, -999.f) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** p) override { if(p)*p=nullptr;return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override {return 2;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override {return E_NOTIMPL;}
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override {return 0;}
    DWORD STDMETHODCALLTYPE GetPriority() override {return 0;}
    void STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {return D3DRTYPE_VERTEXBUFFER;}
    HRESULT STDMETHODCALLTYPE Lock(UINT offset,UINT size,void** out,DWORD flags) override {
        if(locked || !out || offset || size!=values.size()*sizeof(float) || flags) return E_FAIL;
        locked=true;*out=values.data();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { if(!locked)return E_FAIL;locked=false;return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC*) override {return E_NOTIMPL;}
};
static_assert(!std::is_abstract<CpuBuffer>::value,"test buffer implements the COM interface");
struct Geometry {
    static constexpr unsigned count=8;
    std::array<mdl::PmxVertex,count> pmx{};
    std::array<mdl::PmdVertex,count> pmd{};
    std::array<mdl::ModelMaterialRecord,1> materials{};
    std::array<mdl::MaterialMorphPool,1> materialBase{};
    std::array<mdl::MorphRecord,7> morphs{};
    std::array<mdl::PmdVertexMorphEntry,3> base{{{0,{1,2,3}},{1,{4,5,6}},{0,{7,8,9}}}};
    std::array<mdl::PmdVertexMorphEntry,2> offsets{{{0,{.5f,-.25f,2}},{1,{-1,3,.25f}}}};
    std::array<mdl::PmxUvMorphEntry,5> baseUv{},deltaUv{};
    std::array<mdl::PmxGroupMorphEntry,2> group{{{1,.5f},{1,-.25f}}};
    std::array<float,7> weights{{.625f,-.75f,.25f,.5f,-.25f,1.25f,.75f}};
    explicit Geometry(Model& source,bool isPmx,unsigned uvCount){
        auto& r=*source.record;r.physicsMode=isPmx?2:0;r.pmxAdditionalUvCount=isPmx?uvCount:0;
        r.vertexCount=count;r.indexCount=0;r.materialCount=1;r.materials=materials.data();
        r.pmxVertices=pmx.data();r.rawVertices=pmd.data();r.morphCount=isPmx?7:2;r.morphs=morphs.data();
        r.morph0Count=unsigned(base.size());r.morph0Table=base.data();r.materialMorphPools.base=materialBase.data();
        r.loadComplete=1;r.edgeScale=1;materials[0].edgeSize=1;
        for(unsigned i=0;i<count;++i){
            auto& x=pmx[i];auto& d=pmd[i];
            for(unsigned k=0;k<3;++k){x.position[k]=d.position[k]=float(i+k+1)*.37f;x.normal[k]=d.normal[k]=k==1?1.f:0.f;}
            x.uv[0]=d.uv[0]=float(i)*.125f;x.uv[1]=d.uv[1]=float(i)*.25f;
            for(unsigned u=0;u<uvCount;++u)for(unsigned k=0;k<4;++k)x.additionalUvByComponent[k][u]=float(i+10*u+k)*.125f;
            x.materialIndex=0;x.edgeScale=1;x.hasPmxEdgeData=1;x.weightType=static_cast<mdl::PmxWeightType>(i%4);
            x.bone[0]=0;x.bone[1]=i==3?-1:1;x.bone[2]=i==6?999:2;x.bone[3]=3;
            x.weight[0]=i==2?-.25f:.375f;x.weight[1]=.125f;x.weight[2]=.5f;x.weight[3]=.75f;
            x.sdef.center[0]=.25f;x.sdef.center[1]=-.5f;x.sdef.r0Offset[0]=.75f;x.sdef.r1Offset[2]=-.375f;
            d.bone[0]=0;d.bone[1]=i==3?-1:1;d.weightPercent=i==2?-59:75;d.edgeDisabled=1;
        }
        morphs[0].type=0;morphs[0].value=weights[0];morphs[0].groupCount=isPmx?2:0;morphs[0].groupEntries=group.data();
        morphs[1].type=isPmx?1:0;morphs[1].value=weights[1];morphs[1].offsetCount=unsigned(offsets.size());morphs[1].vertexEntries=offsets.data();
        for(unsigned f=0;f<5;++f){
            // Native morphs may address any of the four additional families,
            // even if this model's output buffer does not expose the family.
            r.uvMorphCounts.byFamily[f]=isPmx?1:0;r.uvMorphTables.byFamily[f]=&baseUv[f];
            baseUv[f].vertexIndex=deltaUv[f].vertexIndex=0;
            for(unsigned k=0;k<4;++k){baseUv[f].offset[k]=float(f*4+k)*.125f;deltaUv[f].offset[k]=float(k+1)*.0625f;}
            morphs[f+2].type=std::uint8_t(f+3);morphs[f+2].value=weights[f+2];
            morphs[f+2].uvCounts[f]=isPmx?1:0;morphs[f+2].uvEntries[f]=&deltaUv[f];
        }
    }
};
void Case(bool isPmx,unsigned uvCount,const BoneTransformContext& context){
    currentCase=isPmx?"PMX CPU oracle":"PMD CPU oracle";
    Model model;
    model.record->physicsMode=isPmx?2:0;
    auto skeletonSource=BoneEvaluationSource::Capture(model.raw());
    auto bones=skeletonSource->Evaluate(5,context);
    Geometry geometry(model,isPmx,uvCount);
    for(unsigned i=0;i<bones->BoneCount();++i){BoneEvaluationPose pose;CHECK(bones->BoneAt(i,pose));std::memcpy(model.bones[i].matInit,pose.scaled.data(),64);}
    auto mesh=MeshDefinitionSnapshot::Capture(model.raw());
    auto definitions=MorphDefinitionSnapshot::Capture(model.raw());
    auto morph=MorphEvaluationFrame::Evaluate(*definitions,geometry.weights.data(),isPmx?7:2);
    auto evaluated=Mesh::Evaluate(*mesh,*morph,*bones,context);
    CHECK(evaluated->VertexCount()==Geometry::count && evaluated->AdditionalUvCount()==(isPmx?uvCount:0));
    const unsigned stride=8+(isPmx?uvCount*4:0);
    CpuBuffer body(Geometry::count*stride),edge(Geometry::count*4);
    model.record->vertexBuffer=&body;model.record->vertexBuffer2=&edge;
    auto app=std::make_unique<MMDApp>();app->state=MMDAppState{};app->CameraDistance()=10;app->CameraFov()=30;
    const float world[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    CHECK(UpdateModelVertexBuffersCpu(app.get(),model.raw(),world));CHECK(!body.locked&&!edge.locked);
    for(unsigned i=0;i<Geometry::count;++i){
        MeshEvaluationVertex v;CHECK(evaluated->VertexAt(i,v));const float* n=body.values.data()+i*stride;
        CHECK(!std::memcmp(v.position.data(),n,12));CHECK(!std::memcmp(v.normal.data(),n+3,12));CHECK(!std::memcmp(v.uv.data(),n+6,8));
        for(unsigned u=0;u<(isPmx?uvCount:0);++u)CHECK(!std::memcmp(v.additionalUv[u].data(),n+8+u*4,16));
        for(unsigned u=(isPmx?uvCount:0);u<4;++u)CHECK((v.additionalUv[u]==std::array<float,4>{}));
    }
    MeshEvaluationVertex unchanged{};unchanged.position[0]=13;CHECK(!evaluated->VertexAt(Geometry::count,unchanged));CHECK(unchanged.position[0]==13);
    CHECK(Throws<std::length_error>([&]{Mesh::Evaluate(*mesh,*morph,*bones,context,evaluated->RetainedBytes()-1);}));
    CHECK(Mesh::Evaluate(*mesh,*morph,*bones,context,evaluated->RetainedBytes())->RetainedBytes()==evaluated->RetainedBytes());
    CHECK(Throws<std::length_error>([&]{Mesh::Evaluate(*mesh,*morph,*bones,context,evaluated->RetainedBytes(),0);}));
    std::uint64_t low=0,high=128ull*1024*1024;
    while(high-low>1){const auto mid=low+(high-low)/2;try{Mesh::Evaluate(*mesh,*morph,*bones,context,evaluated->RetainedBytes(),mid);high=mid;}catch(const std::length_error&){low=mid;}}
    CHECK(Mesh::Evaluate(*mesh,*morph,*bones,context,evaluated->RetainedBytes(),high)->VertexCount()==Geometry::count);
    CHECK(Throws<std::length_error>([&]{Mesh::Evaluate(*mesh,*morph,*bones,context,evaluated->RetainedBytes(),high-1);}));
    auto wrong=std::make_unique<mdl::ModelRecord>();wrong->physicsMode=isPmx?0:2;wrong->boneCount=bones->BoneCount();
    auto wrongDefinitions=MorphDefinitionSnapshot::Capture(reinterpret_cast<unsigned char*>(wrong.get()));
    auto wrongMorph=MorphEvaluationFrame::Evaluate(*wrongDefinitions,nullptr,0);
    CHECK(Throws<std::invalid_argument>([&]{Mesh::Evaluate(*mesh,*wrongMorph,*bones,context);}));
    const auto oldBoneCount=model.record->boneCount;model.record->boneCount=oldBoneCount-1;
    auto wrongMesh=MeshDefinitionSnapshot::Capture(model.raw());model.record->boneCount=oldBoneCount;
    CHECK(Throws<std::invalid_argument>([&]{Mesh::Evaluate(*wrongMesh,*morph,*bones,context);}));
    bool eventually=false;unsigned failures=0;
    for(int point=0;point<100&&!eventually;++point){failAfter=point;try{auto result=Mesh::Evaluate(*mesh,*morph,*bones,context);failAfter=-1;CHECK(result->VertexCount()==Geometry::count);eventually=true;}
        catch(const std::bad_alloc&){failAfter=-1;++failures;}catch(...){failAfter=-1;throw;}}
    CHECK(eventually&&failures>0);
    std::atomic<unsigned> errors{0};std::array<std::thread,4> threads;
    for(auto& t:threads)t=std::thread([&]{try{for(unsigned n=0;n<8;++n){auto frame=Mesh::Evaluate(*mesh,*morph,*bones,context);for(unsigned i=0;i<Geometry::count;++i){MeshEvaluationVertex a,b;if(!evaluated->VertexAt(i,a)||!frame->VertexAt(i,b)||std::memcmp(&a,&b,sizeof(a)))++errors;}}}catch(...){++errors;}});
    for(auto& t:threads)t.join();CHECK(errors==0);
    model_vertex_cache::Forget(model.raw());
}
void EmptyAndLifetime(){
    currentCase="empty mesh and frame ownership";
    std::unique_ptr<Mesh> retained;
    MeshEvaluationVertex expected{};
    {
        auto context=BoneTransformContext::Create();CHECK(bool(context));
        auto model=std::make_unique<Model>();
        auto skeletonSource=BoneEvaluationSource::Capture(model->raw());
        auto bones=skeletonSource->Evaluate(5,*context);
        Geometry geometry(*model,true,2);
        auto mesh=MeshDefinitionSnapshot::Capture(model->raw());
        auto definitions=MorphDefinitionSnapshot::Capture(model->raw());
        auto morph=MorphEvaluationFrame::Evaluate(*definitions,geometry.weights.data(),7);
        retained=Mesh::Evaluate(*mesh,*morph,*bones,*context);
        CHECK(retained->VertexAt(3,expected));

        model->record->vertexCount=0;model->record->materialCount=0;
        model->record->morphCount=0;model->record->morph0Count=0;
        for(auto& count:model->record->uvMorphCounts.byFamily)count=0;
        auto emptyMesh=MeshDefinitionSnapshot::Capture(model->raw());
        auto emptyDefinitions=MorphDefinitionSnapshot::Capture(model->raw());
        auto emptyMorph=MorphEvaluationFrame::Evaluate(*emptyDefinitions,nullptr,0);
        auto empty=Mesh::Evaluate(*emptyMesh,*emptyMorph,*bones,*context);
        CHECK(empty->VertexCount()==0&&empty->AdditionalUvCount()==2);
        auto unchanged=expected;CHECK(!empty->VertexAt(0,unchanged));CHECK(Bits(unchanged,expected));
    }
    MeshEvaluationVertex actual{};CHECK(retained->VertexAt(3,actual));CHECK(Bits(actual,expected));
    std::atomic<unsigned> failures{0};std::array<std::thread,4> threads;
    for(auto& thread:threads)thread=std::thread([&]{for(unsigned n=0;n<100;++n){MeshEvaluationVertex value;if(!retained->VertexAt(3,value)||!Bits(value,expected))++failures;}});
    for(auto& thread:threads)thread.join();CHECK(failures==0);
}
}
namespace mesh_probe {
void SceneCapture(const BoneTransformContext& context) {
    currentCase="coherent mesh scene";
    std::unique_ptr<MeshSceneSource> source;
    std::unique_ptr<MeshSceneFrame> old;
    MeshEvaluationVertex expected{};
    EvaluationTime time; CHECK(EvaluationTime::FromFrame(5.25, time));
    {
        auto model=std::make_unique<Model>();
        Geometry geometry(*model,true,2);
        model->record->morphCount=2;
        BoneSceneModelInput input{0,{123,456,1},model->raw()};
        source=MeshSceneSource::Capture(&input,1);
        CHECK(source->ModelCount()==1);
        CHECK(source->MeshAt(0) && !source->MeshAt(1));
        MeshDefinitionVertex frozenBind{};CHECK(source->MeshAt(0)->VertexAt(3,frozenBind));
        CHECK(Throws<std::length_error>([&]{MeshSceneSource::Capture(&input,1,source->RetainedBytes()-1);}));
        CHECK(MeshSceneSource::Capture(&input,1,source->RetainedBytes())->RetainedBytes()==source->RetainedBytes());
        old=source->EvaluateAt(time,context);
        CHECK(old->ModelCount()==1 && old->ModelFrame(0) && !old->ModelFrame(1));
        BoneSceneModelInfo info{}; CHECK(old->Bones().ModelAt(0,info));
        CHECK(info.identity.lo==123 && info.identity.hi==456 && info.identity.generation==1);
        CHECK(old->ModelFrame(0)->VertexAt(3,expected));
        auto mesh=MeshDefinitionSnapshot::Capture(model->raw());
        auto definitions=MorphDefinitionSnapshot::Capture(model->raw());
        auto sampling=morph_curve::SamplingState::Capture(model->raw());
        std::array<float,2> sampled{};
        for(unsigned i=0;i<sampled.size();++i) CHECK(sampling->Sample(i,time.Frame(),sampled[i]));
        auto morph=MorphEvaluationFrame::Evaluate(*definitions,sampled.data(),unsigned(sampled.size()));
        auto separate=Mesh::Evaluate(*mesh,*morph,*old->Bones().ModelFrame(0),context);
        for(unsigned i=0;i<Geometry::count;++i){MeshEvaluationVertex a,b;CHECK(old->ModelFrame(0)->VertexAt(i,a)&&separate->VertexAt(i,b));CHECK(Bits(a,b));}
        CHECK(Throws<std::length_error>([&]{source->EvaluateAt(time,context,old->RetainedBytes()-1);}));
        CHECK(source->EvaluateAt(time,context,old->RetainedBytes())->RetainedBytes()==old->RetainedBytes());
        CHECK(Throws<std::length_error>([&]{source->EvaluateAt(time,context,old->RetainedBytes(),0);}));
        BoneSceneModelInput duplicate[]{input,input};
        CHECK(Throws<std::invalid_argument>([&]{MeshSceneSource::Capture(duplicate,2);}));
        Model second; Geometry other(second,true,1); second.record->morphCount=2;
        other.pmx[3].position[0]+=10;
        BoneSceneModelInput pair[]{input,{1,{789,123,2},second.raw()}};
        auto pairSource=MeshSceneSource::Capture(pair,2);
        auto pairFrame=pairSource->EvaluateAt(time,context);
        BoneSceneModelInfo secondInfo{}; CHECK(pairFrame->Bones().ModelAt(1,secondInfo));
        CHECK(secondInfo.identity.lo==789 && secondInfo.identity.generation==2);
        MeshEvaluationVertex firstVertex{},secondVertex{};
        CHECK(pairFrame->ModelFrame(0)->VertexAt(3,firstVertex) && pairFrame->ModelFrame(1)->VertexAt(3,secondVertex));
        CHECK(Bits(firstVertex,expected) && !Bits(firstVertex,secondVertex));
        bool succeeded=false;unsigned failed=0;
        for(int n=0;n<300&&!succeeded;++n){failAfter=n;try{auto result=source->EvaluateAt(time,context);failAfter=-1;CHECK(result->ModelCount()==1);succeeded=true;}catch(const std::bad_alloc&){failAfter=-1;++failed;}catch(...){failAfter=-1;throw;}}
        CHECK(succeeded&&failed>0);
        geometry.pmx[3].position[0]=999;
        MeshDefinitionVertex unchangedBind{};CHECK(source->MeshAt(0)->VertexAt(3,unchangedBind));CHECK(Bits(frozenBind.position,unchangedBind.position));
    }
    auto fresh=source->EvaluateAt(time,context);
    MeshEvaluationVertex actual{}; CHECK(fresh->ModelFrame(0)->VertexAt(3,actual)); CHECK(Bits(actual,expected));
    MeshDefinitionVertex sourceAfterDelete{};CHECK(source->MeshAt(0)->VertexAt(3,sourceAfterDelete));CHECK(sourceAfterDelete.position[0]!=999);
    std::atomic<unsigned> evalErrors{0};std::array<std::thread,4> evaluations;
    for(auto& worker:evaluations)worker=std::thread([&]{try{for(unsigned n=0;n<4;++n){auto frame=source->EvaluateAt(time,context);MeshEvaluationVertex value;if(!frame->ModelFrame(0)->VertexAt(3,value)||!Bits(value,expected))++evalErrors;}}catch(...){++evalErrors;}});
    for(auto& worker:evaluations)worker.join();CHECK(evalErrors==0);
    source.reset(); CHECK(old->ModelFrame(0)->VertexAt(3,actual)); CHECK(Bits(actual,expected));
    std::atomic<unsigned> errors{0};std::array<std::thread,4> workers;
    for(auto& worker:workers) worker=std::thread([&]{for(unsigned j=0;j<100;++j){MeshEvaluationVertex v;if(!old->ModelFrame(0)->VertexAt(3,v)||!Bits(v,expected))++errors;}});
    for(auto& worker:workers)worker.join();CHECK(errors==0);
    auto empty=MeshSceneSource::Capture(nullptr,0); auto emptyFrame=empty->EvaluateAt(time,context);
    CHECK(empty->ModelCount()==0 && !empty->MeshAt(0) && emptyFrame->ModelCount()==0 && !emptyFrame->ModelFrame(0));
    EvaluationTime boundary;CHECK(EvaluationTime::FromSeconds(18014398509481983LL,540431955284459520ULL,boundary));
    CHECK(boundary.Frame()==1.0 && boundary.DiscreteFrame()==0);
    auto boundaryFrame=empty->EvaluateAt(boundary,context);
    CHECK(boundaryFrame->Time().Frame()==1.0 && boundaryFrame->Time().DiscreteFrame()==0);
    CHECK(old->Time().Frame()==5.25 && old->Time().DiscreteFrame()==5);
}

mdl::MaterialMorphChannels MaterialChannels(float seed) {
    mdl::MaterialMorphChannels value{};
    for(unsigned i=0;i<4;++i){value.diffuse[i]=seed+.01f*float(i);value.edgeColor[i]=seed+.05f*float(i);
        value.textureTint[i]=seed+.09f*float(i);value.sphereTint[i]=seed+.13f*float(i);value.toonTint[i]=seed+.17f*float(i);}
    for(unsigned i=0;i<3;++i){value.specular[i]=seed+.02f*float(i);value.ambient[i]=seed+.03f*float(i);}
    value.specularPower=seed+.21f;value.edgeSize=seed+.23f;return value;
}
struct MaterialFixture {
    Model& model;
    Geometry geometry;
    std::array<mdl::ModelMaterialRecord,2> materials{};
    std::array<mdl::MaterialMorphPool,2> base{},add{},mul{};
    std::array<mdl::PmxMaterialMorphEntry,2> additive{},multiply{};
    std::array<mdl::PmxGroupMorphEntry,3> group{{{1,.5f},{2,-.25f},{1,-.375f}}};
    explicit MaterialFixture(Model& input,bool pmx):model(input),geometry(input,pmx,0) {
        auto& r=*model.record;r.materialCount=2;r.materials=materials.data();
        r.materialMorphPools={base.data(),add.data(),mul.data()};
        for(unsigned i=0;i<2;++i){auto& m=materials[i];base[i].channels=MaterialChannels(float(i+1)*.5f);
            for(unsigned c=0;c<3;++c){m.diffuse[c]=float(10+i+c);m.diffuseMirror[c]=float(20+i+c);m.specular[c]=float(30+i+c);m.ambient[c]=float(40+i+c);}
            m.diffuse[3]=.6f+float(i)*.1f;for(unsigned c=0;c<4;++c)m.edgeColor[c]=float(i+c+1)*.2f;
            m.specularPower=2+float(i);m.edgeSize=.25f+float(i);}
        if(!pmx)return;
        r.morphCount=3;
        for(unsigned i=0;i<3;++i){geometry.morphs[i]={};std::snprintf(geometry.morphs[i].name,20,"material-%u",i);geometry.morphs[i].panel=mdl::MorphPanel::other;}
        additive[0].materialIndex=-1;additive[0].operation=1;additive[0].channels=MaterialChannels(.125f);
        additive[1].materialIndex=0;additive[1].operation=197;additive[1].channels=MaterialChannels(-.2f);
        multiply[0].materialIndex=-1;multiply[0].operation=0;multiply[0].channels=MaterialChannels(.5f);
        multiply[1].materialIndex=1;multiply[1].operation=0;multiply[1].channels=MaterialChannels(1.25f);
        geometry.morphs[0].type=0;geometry.morphs[0].groupCount=3;geometry.morphs[0].groupEntries=group.data();
        geometry.morphs[1].type=8;geometry.morphs[1].materialCount=2;geometry.morphs[1].materialEntries=additive.data();
        geometry.morphs[2].type=8;geometry.morphs[2].materialCount=2;geometry.morphs[2].materialEntries=multiply.data();
        CHECK(mdl::MorphKeyPoolEnsure(r,6));
        const float first[]{-1.5f,2.25f,-.75f},last[]{2.5f,-1.25f,1.75f};
        for(unsigned i=0;i<3;++i){r.morphKeys[i]={};r.morphKeys[3+i]={};r.morphKeys[i].allocated=1;r.morphKeys[i].next=3+i;r.morphKeys[i].value=first[i];
            r.morphKeys[3+i].allocated=1;r.morphKeys[3+i].frame=10;r.morphKeys[3+i].previous=i;r.morphKeys[3+i].value=last[i];}
    }
};
bool MaterialMatches(const MeshSceneMaterial& actual,const mdl::ModelMaterialRecord& native,
                     const mdl::MaterialMorphChannels* add,const mdl::MaterialMorphChannels* mul,bool pmx) {
    if(actual.physicsMode!=(pmx?2:0))return false;
    for(unsigned i=0;i<3;++i)if(!Bits(actual.nativeDiffuse[i],native.diffuse[i])||
        !Bits(actual.diffuseMirrorAndAlpha[i],native.diffuseMirror[i])||!Bits(actual.specular[i],native.specular[i])||!Bits(actual.ambient[i],native.ambient[i]))return false;
    if(!Bits(actual.diffuseMirrorAndAlpha[3],native.diffuse[3])||!Bits(actual.specularPower,native.specularPower)||!Bits(actual.edgeSize,native.edgeSize))return false;
    for(unsigned i=0;i<4;++i){if(!Bits(actual.edgeColor[i],native.edgeColor[i]))return false;
        if(pmx){if(!Bits(actual.textureAdd[i],add->textureTint[i])||!Bits(actual.textureMultiply[i],mul->textureTint[i])||
            !Bits(actual.sphereAdd[i],add->sphereTint[i])||!Bits(actual.sphereMultiply[i],mul->sphereTint[i])||
            !Bits(actual.toonAdd[i],add->toonTint[i])||!Bits(actual.toonMultiply[i],mul->toonTint[i]))return false;}
        else if(actual.textureAdd[i]!=0||actual.sphereAdd[i]!=0||actual.toonAdd[i]!=0||
            actual.textureMultiply[i]!=1||actual.sphereMultiply[i]!=1||actual.toonMultiply[i]!=1)return false;
    }return true;
}
bool SameMaterial(const MeshSceneMaterial& a,const MeshSceneMaterial& b){
    return a.physicsMode==b.physicsMode && Bits(a.nativeDiffuse,b.nativeDiffuse) && Bits(a.specular,b.specular) && Bits(a.ambient,b.ambient) &&
        Bits(a.diffuseMirrorAndAlpha,b.diffuseMirrorAndAlpha) && Bits(a.edgeColor,b.edgeColor) &&
        Bits(a.textureAdd,b.textureAdd) && Bits(a.textureMultiply,b.textureMultiply) && Bits(a.sphereAdd,b.sphereAdd) &&
        Bits(a.sphereMultiply,b.sphereMultiply) && Bits(a.toonAdd,b.toonAdd) && Bits(a.toonMultiply,b.toonMultiply) &&
        Bits(a.specularPower,b.specularPower) && Bits(a.edgeSize,b.edgeSize);
}
void SceneMaterial(const BoneTransformContext& context,bool pmx){
    currentCase=pmx?"coherent PMX materials native oracle":"coherent PMD static materials";
    std::unique_ptr<MeshSceneFrame> retained;std::array<MeshSceneMaterial,2> expected{};
    {
        Model model;MaterialFixture fixture(model,pmx);
        BoneSceneModelInput input{0,{991,992,1},model.raw()};
        auto legacy=MeshSceneSource::Capture(&input,1);
        auto source=MeshSceneSource::CaptureWithMaterials(&input,1);
        CHECK(!legacy->HasMaterials()&&source->HasMaterials()&&source->RetainedBytes()>legacy->RetainedBytes());
        CHECK(Throws<std::length_error>([&]{MeshSceneSource::Capture(&input,1,legacy->RetainedBytes()-1);}));
        CHECK(MeshSceneSource::Capture(&input,1,legacy->RetainedBytes())->RetainedBytes()==legacy->RetainedBytes());
        CHECK(Throws<std::length_error>([&]{MeshSceneSource::CaptureWithMaterials(&input,1,source->RetainedBytes()-1);}));
        CHECK(MeshSceneSource::CaptureWithMaterials(&input,1,source->RetainedBytes())->RetainedBytes()==source->RetainedBytes());
        auto sampling=morph_curve::SamplingState::Capture(model.raw());
        for(double f:{0.,5.,10.}){
            EvaluationTime time;CHECK(EvaluationTime::FromFrame(f,time));
            const auto beforeMaterials=fixture.materials;const auto beforeAdd=fixture.add;const auto beforeMul=fixture.mul;
            auto frame=source->EvaluateAt(time,context);
            auto legacyFrame=legacy->EvaluateAt(time,context);
            CHECK(!legacyFrame->HasMaterials()&&frame->HasMaterials()&&legacyFrame->MaterialCount(0)==0&&frame->RetainedBytes()>legacyFrame->RetainedBytes());
            MeshSceneMaterial oldOutput;std::memset(&oldOutput,0xa5,sizeof(oldOutput));std::array<unsigned char,sizeof(oldOutput)> oldBytes{};
            std::memcpy(oldBytes.data(),&oldOutput,sizeof(oldOutput));
            CHECK(!legacyFrame->MaterialAt(0,0,oldOutput)&&std::memcmp(oldBytes.data(),&oldOutput,sizeof(oldOutput))==0);
            CHECK(Throws<std::length_error>([&]{legacy->EvaluateAt(time,context,legacyFrame->RetainedBytes()-1);}));
            CHECK(legacy->EvaluateAt(time,context,legacyFrame->RetainedBytes())->RetainedBytes()==legacyFrame->RetainedBytes());
            for(unsigned vertex=0;vertex<Geometry::count;++vertex){MeshEvaluationVertex a,b;
                CHECK(frame->ModelFrame(0)->VertexAt(vertex,a)&&legacyFrame->ModelFrame(0)->VertexAt(vertex,b));CHECK(Bits(a,b));}
            CHECK(Bits(beforeMaterials,fixture.materials)&&Bits(beforeAdd,fixture.add)&&Bits(beforeMul,fixture.mul));
            if(pmx){for(unsigned m=0;m<3;++m){float value=0;CHECK(sampling->Sample(int(m),f,value));fixture.geometry.morphs[m].value=value;}ModelApplyMorphs(model.raw());}
            CHECK(frame->MaterialCount(0)==2 && frame->MaterialCount(1)==0);
            for(unsigned material=0;material<2;++material){MeshSceneMaterial value{};CHECK(frame->MaterialAt(0,material,value));
                CHECK(MaterialMatches(value,fixture.materials[material],pmx?&fixture.add[material].channels:nullptr,pmx?&fixture.mul[material].channels:nullptr,pmx));expected[material]=value;}
            CHECK(expected[0].nativeDiffuse[0]!=expected[0].diffuseMirrorAndAlpha[0]);
            auto unchanged=expected[0];std::array<unsigned char,sizeof(unchanged)> before{};std::memcpy(before.data(),&unchanged,sizeof(unchanged));
            CHECK(!frame->MaterialAt(1,0,unchanged)&&std::memcmp(before.data(),&unchanged,sizeof(unchanged))==0);
            CHECK(!frame->MaterialAt(0,2,unchanged)&&std::memcmp(before.data(),&unchanged,sizeof(unchanged))==0);
            CHECK(Throws<std::length_error>([&]{source->EvaluateAt(time,context,frame->RetainedBytes()-1);}));
            CHECK(source->EvaluateAt(time,context,frame->RetainedBytes())->RetainedBytes()==frame->RetainedBytes());
            retained=std::move(frame);
        }
        EvaluationTime time;CHECK(EvaluationTime::FromFrame(10,time));bool succeeded=false;unsigned failures=0;
        for(int point=0;point<300&&!succeeded;++point){failAfter=point;try{auto value=source->EvaluateAt(time,context);failAfter=-1;CHECK(value->MaterialCount(0)==2);succeeded=true;}
            catch(const std::bad_alloc&){failAfter=-1;++failures;}catch(...){failAfter=-1;throw;}}
        CHECK(succeeded&&failures>0);
        std::atomic<unsigned> errors{0};std::array<std::thread,4> workers;
        for(auto& worker:workers)worker=std::thread([&]{try{for(unsigned n=0;n<4;++n){auto frame=source->EvaluateAt(time,context);for(unsigned i=0;i<2;++i){MeshSceneMaterial value{};if(!frame->MaterialAt(0,i,value)||!SameMaterial(value,expected[i]))++errors;}}}catch(...){++errors;}});
        for(auto& worker:workers)worker.join();CHECK(errors==0);
    }
    for(unsigned i=0;i<2;++i){MeshSceneMaterial value{};CHECK(retained->MaterialAt(0,i,value)&&SameMaterial(value,expected[i]));}
    std::atomic<unsigned> errors{0};std::array<std::thread,4> readers;
    for(auto& reader:readers)reader=std::thread([&]{for(unsigned n=0;n<100;++n)for(unsigned i=0;i<2;++i){MeshSceneMaterial value{};if(!retained->MaterialAt(0,i,value)||!SameMaterial(value,expected[i]))++errors;}});
    for(auto& reader:readers)reader.join();CHECK(errors==0);
}
void NonfiniteMaterialPreservesMesh(const BoneTransformContext& context){
    currentCase="unavailable material does not reject existing mesh";
    for(bool pmx:{false,true}){
        Model model;MaterialFixture fixture(model,pmx);
        fixture.materials[0].diffuse[0]=std::numeric_limits<float>::quiet_NaN();
        BoneSceneModelInput input{0,{881,882,1},model.raw()};
        auto legacy=MeshSceneSource::Capture(&input,1);
        auto source=MeshSceneSource::CaptureWithMaterials(&input,1);EvaluationTime time;
        CHECK(EvaluationTime::FromFrame(5,time));auto frame=source->EvaluateAt(time,context);
        auto legacyFrame=legacy->EvaluateAt(time,context);
        CHECK(!legacyFrame->HasMaterials()&&legacyFrame->MaterialCount(0)==0&&legacyFrame->ModelFrame(0));
        CHECK(frame->ModelFrame(0) && frame->ModelFrame(0)->VertexCount()==Geometry::count && frame->MaterialCount(0)==2);
        MeshSceneMaterial output;std::memset(&output,0xa5,sizeof(output));std::array<unsigned char,sizeof(output)> before{};
        std::memcpy(before.data(),&output,sizeof(output));
        CHECK(!frame->MaterialAt(0,0,output)&&std::memcmp(before.data(),&output,sizeof(output))==0);
        CHECK(frame->MaterialAt(0,1,output));
    }
}
}
int main(){try{auto ctx=BoneTransformContext::Create();CHECK(bool(ctx));mesh_probe::Case(false,0,*ctx);for(unsigned u=0;u<=4;++u)mesh_probe::Case(true,u,*ctx);mesh_probe::EmptyAndLifetime();mesh_probe::SceneCapture(*ctx);mesh_probe::SceneMaterial(*ctx,true);mesh_probe::SceneMaterial(*ctx,false);mesh_probe::NonfiniteMaterialPreservesMesh(*ctx);std::printf("PASS %u checks: CPU oracle PMD and PMX UV 0-4 and coherent scene\n",checks);return 0;}catch(const std::exception& e){failAfter=-1;std::fprintf(stderr,"FAIL %s exception: %s\n",currentCase,e.what());return 1;}}
