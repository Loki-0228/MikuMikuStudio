// Independent CPU mesh frame against the real production CPU worker.
// The worker uses owned COM buffers; no D3D device, window or GPU is created.
#define main inherited_bone_evaluation_tests_main
#include "bone_evaluation_tests.cpp"
#undef main
#include "mikumikustudio/mesh_evaluation.hpp"
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
int main(){try{auto ctx=BoneTransformContext::Create();CHECK(bool(ctx));mesh_probe::Case(false,0,*ctx);for(unsigned u=0;u<=4;++u)mesh_probe::Case(true,u,*ctx);mesh_probe::EmptyAndLifetime();std::printf("PASS %u checks: CPU oracle PMD and PMX UV 0-4\n",checks);return 0;}catch(const std::exception& e){failAfter=-1;std::fprintf(stderr,"FAIL %s exception: %s\n",currentCase,e.what());return 1;}}
