#include "mikumikustudio/mesh_definition_snapshot.hpp"
#include "mikumikustudio/mesh_scene_evaluation.hpp"
#include "mikumikustudio/bone_transform_context.hpp"
#include "mikumikustudio/morph_definition_snapshot.hpp"
#include "mikumikustudio/morph_evaluation.hpp"
#include "mikumikustudio/morph_curve.hpp"
#include "mikumikustudio/gpu_model.hpp"
#include <array>
#include <tuple>

// Include after bone_evaluation_host_checks.inc. Its proof observes native
// pose, keys, authoring, playback and shared history; this adds mesh indices.
#if !MMD_ORIGINAL_PROFILE
namespace mesh_definition_probe {
using namespace mikumikustudio::plugins;
namespace {
template<class T> bool Bits(const T& a,const T& b) { return std::memcmp(&a,&b,sizeof(T))==0; }
template<class T,size_t N> void Copy(std::array<T,N>& out,const T* source) {
    std::memcpy(out.data(),source,N*sizeof(T));
}
struct Expected {
    uint8_t mode=0,uvCount=0;
    uint32_t bones=0;
    std::vector<MeshDefinitionVertex> vertices;
    std::vector<MeshDefinitionTriangle> triangles;
    std::vector<MeshDefinitionMaterial> materials;
};
struct BindLease {
    MmsSceneApiV1 api{};MmsHandle handle=0;
    ~BindLease() { if(handle)api.release(api.context,handle); }
};
bool Id(MmsId a,MmsId b) { return a.lo==b.lo&&a.hi==b.hi; }
Expected ReadBind(MMDApp* app,const mdl::ModelRecord& model) {
    auto store=CreateSceneStore(app);SceneClient client;auto api=store->Api(client);
    MmsSceneQueryV1 query{};query.h=Header<MmsSceneQueryV1>();query.stage=MMS_STAGE_BIND;
    query.flags=MMS_SCENE_CURRENT_TIME;query.channels=MMS_CHANNEL_MESH|MMS_CHANNEL_BINDING|MMS_CHANNEL_BONES|MMS_CHANNEL_MATERIALS;
    MmsSnapshotV1 snapshot{};Check(api.acquire(api.context,&query,&snapshot)==MMS_OK,"real BIND geometry succeeds");
    BindLease lease{api,snapshot.handle};MmsSpan objects{},meshSpan{},bindingSpan{},boneSpan{},materialSpan{};
    Check(api.objects(api.context,lease.handle,&objects)==MMS_OK&&objects.count==1,"one real mesh object in BIND");
    const auto object=static_cast<const MmsObjectV1*>(objects.data)[0].object;
    Check(api.readChannel(api.context,lease.handle,object,MMS_CHANNEL_MESH,&meshSpan)==MMS_OK&&meshSpan.count==1&&meshSpan.stride==sizeof(MmsMeshV1),"read real mesh record");
    Check(api.readChannel(api.context,lease.handle,object,MMS_CHANNEL_BINDING,&bindingSpan)==MMS_OK&&bindingSpan.count==1&&bindingSpan.stride==sizeof(MmsBindingV1),"read real binding record");
    Check(api.readChannel(api.context,lease.handle,object,MMS_CHANNEL_BONES,&boneSpan)==MMS_OK&&boneSpan.count==model.boneCount&&boneSpan.stride==sizeof(MmsBoneV1),"read BIND bone identity map");
    Check(api.readChannel(api.context,lease.handle,object,MMS_CHANNEL_MATERIALS,&materialSpan)==MMS_OK&&materialSpan.count==model.materialCount&&materialSpan.stride==sizeof(MmsMaterialV1),"read BIND material identity map");
    const auto& mesh=*static_cast<const MmsMeshV1*>(meshSpan.data);
    const auto& binding=*static_cast<const MmsBindingV1*>(bindingSpan.data);
    Check(mesh.vertices.count==model.vertexCount&&mesh.vertices.stride==sizeof(MmsVertexV1)&&
        binding.weights.count==model.vertexCount&&binding.weights.stride==sizeof(MmsWeightV1)&&
        mesh.triangles.count*3==model.indexCount&&mesh.triangles.stride==sizeof(MmsTriangleV1),"BIND extents match loaded native tables");
    const auto* vertices=static_cast<const MmsVertexV1*>(mesh.vertices.data);
    const auto* weights=static_cast<const MmsWeightV1*>(binding.weights.data);
    const auto* bones=static_cast<const MmsBoneV1*>(boneSpan.data);
    const auto* materials=static_cast<const MmsMaterialV1*>(materialSpan.data);
    const auto* triangles=static_cast<const MmsTriangleV1*>(mesh.triangles.data);
    const bool pmx=model.physicsMode==2;
    Expected result;result.mode=model.physicsMode;result.uvCount=pmx?model.pmxAdditionalUvCount:0;result.bones=model.boneCount;
    for(uint32_t i=0;i<model.vertexCount;++i) {
        MeshDefinitionVertex vertex;const auto& source=vertices[i];const auto& weight=weights[i];
        Copy(vertex.position,source.position);Copy(vertex.normal,source.normal);Copy(vertex.uv,source.uv);
        for(unsigned group=0;group<4;++group)Copy(vertex.additionalUv[group],source.additionalUv+group*4);
        vertex.binding=static_cast<MeshBindingType>(weight.type);Copy(vertex.weights,weight.weights);
        for(unsigned lane=0;lane<4;++lane) {
            if(!weight.bones[lane].lo&&!weight.bones[lane].hi)continue;
            bool found=false;
            for(uint32_t bone=0;bone<model.boneCount;++bone)if(Id(weight.bones[lane],bones[bone].id)){vertex.bones[lane]=int32_t(bone);found=true;break;}
            Check(found,"every active BIND bone maps to the original loaded index");
        }
        Copy(vertex.sdefCenter,weight.sdefCenter);Copy(vertex.sdefR0Offset,weight.sdefR0);Copy(vertex.sdefR1Offset,weight.sdefR1);
        vertex.edgeScale=source.edgeScale;
        if(pmx) {vertex.pmxEdgeData=model.pmxVertices[i].hasPmxEdgeData;vertex.materialIndex=model.pmxVertices[i].materialIndex;}
        else {vertex.pmdWeightPercent=model.rawVertices[i].weightPercent;vertex.pmdEdgeDisabled=model.rawVertices[i].edgeDisabled;}
        result.vertices.push_back(vertex);
    }
    for(uint32_t i=0;i<mesh.triangles.count;++i) {
        MeshDefinitionTriangle triangle;Copy(triangle.vertices,triangles[i].vertex);bool found=false;
        for(uint32_t material=0;material<model.materialCount;++material)if(Id(triangles[i].material,materials[material].id)){triangle.materialIndex=material;found=true;break;}
        Check(found,"triangle material ID maps to native material order");result.triangles.push_back(triangle);
    }
    uint32_t first=0;const auto* rawMaterials=static_cast<const mdl::ModelMaterialRecord*>(model.materials);
    for(uint32_t material=0;material<model.materialCount;++material) {
        Check(rawMaterials[material].faceVertexCount>=0,"loaded material range is nonnegative");
        const auto count=uint32_t(rawMaterials[material].faceVertexCount);result.materials.push_back({first,count});first+=count;
    }
    Check(first==model.indexCount,"all native material ranges partition the index buffer");
    return result;
}
bool SameVertex(const MeshDefinitionVertex& a,const MeshDefinitionVertex& b) {
    return Bits(a.position,b.position)&&Bits(a.normal,b.normal)&&Bits(a.uv,b.uv)&&Bits(a.additionalUv,b.additionalUv)&&
        a.binding==b.binding&&a.bones==b.bones&&Bits(a.weights,b.weights)&&Bits(a.sdefCenter,b.sdefCenter)&&
        Bits(a.sdefR0Offset,b.sdefR0Offset)&&Bits(a.sdefR1Offset,b.sdefR1Offset)&&Bits(a.edgeScale,b.edgeScale)&&
        a.pmdWeightPercent==b.pmdWeightPercent&&a.pmdEdgeDisabled==b.pmdEdgeDisabled&&a.pmxEdgeData==b.pmxEdgeData&&a.materialIndex==b.materialIndex;
}
bool Matches(const MeshDefinitionSnapshot& snapshot,const Expected& expected) {
    if(snapshot.PhysicsMode()!=expected.mode||snapshot.AdditionalUvCount()!=expected.uvCount||snapshot.BoneCount()!=expected.bones||
        snapshot.VertexCount()!=expected.vertices.size()||snapshot.TriangleCount()!=expected.triangles.size()||
        snapshot.IndexCount()!=expected.triangles.size()*3||snapshot.MaterialCount()!=expected.materials.size()||!snapshot.RetainedBytes())return false;
    for(uint32_t i=0;i<expected.vertices.size();++i){MeshDefinitionVertex vertex;if(!snapshot.VertexAt(i,vertex)||!SameVertex(vertex,expected.vertices[i]))return false;}
    for(uint32_t i=0;i<expected.triangles.size();++i) {
        MeshDefinitionTriangle triangle;if(!snapshot.TriangleAt(i,triangle)||triangle.vertices!=expected.triangles[i].vertices||triangle.materialIndex!=expected.triangles[i].materialIndex)return false;
        for(uint32_t lane=0;lane<3;++lane){uint32_t index=999;if(!snapshot.IndexAt(i*3+lane,index)||index!=triangle.vertices[lane])return false;}
    }
    for(uint32_t i=0;i<expected.materials.size();++i){MeshDefinitionMaterial material;if(!snapshot.MaterialAt(i,material)||material.firstIndex!=expected.materials[i].firstIndex||material.indexCount!=expected.materials[i].indexCount)return false;}
    return true;
}
struct Proof {
    bone_evaluation_probe::Proof native;
    const mdl::ModelRecord& model;
    std::vector<unsigned char> indices;
    Proof(MMDApp* app,unsigned char* raw):native(app,raw),model(*mdl::Mdl(raw)) {
        const auto* bytes=static_cast<const unsigned char*>(model.indices);indices.assign(bytes,bytes+size_t(model.indexCount)*4);
    }
    void Verify() const {native.Verify();Check(std::memcmp(indices.data(),model.indices,indices.size())==0,"capture leaves all four bytes of every native index unchanged");}
};
void Workers(const MeshDefinitionSnapshot& a,const Expected& x,const MeshDefinitionSnapshot& b,const Expected& y) {
    std::array<bool,4> passed{{true,true,true,true}};std::array<std::thread,4> workers;
    try {for(unsigned i=0;i<workers.size();++i)workers[i]=std::thread([&,i]{for(unsigned repeat=0;repeat<64;++repeat)passed[i]=Matches(a,x)&&Matches(b,y)&&passed[i];});}
    catch(...){for(auto& worker:workers)if(worker.joinable())worker.join();throw;}
    for(auto& worker:workers)worker.join();for(bool ok:passed)Check(ok,"concurrent getters preserve independent owned mesh revisions");
}
void InvalidReads(const MeshDefinitionSnapshot& snapshot) {
    MeshDefinitionVertex vertex;std::memset(&vertex,0x5a,sizeof(vertex));std::array<unsigned char,sizeof(vertex)> bytes{};std::memcpy(bytes.data(),&vertex,sizeof(vertex));
    Check(!snapshot.VertexAt(snapshot.VertexCount(),vertex)&&std::memcmp(bytes.data(),&vertex,sizeof(vertex))==0,"invalid vertex read leaves every output byte unchanged");
    uint32_t index=12345;Check(!snapshot.IndexAt(snapshot.IndexCount(),index)&&index==12345,"invalid index read is unchanged");
    MeshDefinitionTriangle triangle{{{1,2,3}},77};const auto oldTriangle=triangle;
    Check(!snapshot.TriangleAt(snapshot.TriangleCount(),triangle)&&triangle.vertices==oldTriangle.vertices&&triangle.materialIndex==77,"invalid triangle read is unchanged");
    MeshDefinitionMaterial material{123,456};Check(!snapshot.MaterialAt(snapshot.MaterialCount(),material)&&material.firstIndex==123&&material.indexCount==456,"invalid material read is unchanged");
}
void UnsupportedTypes(MMDApp* app,unsigned char* raw) {
    auto& model=*mdl::Mdl(raw);if(model.physicsMode!=2)return;
    for(unsigned type:{4u,197u,255u}) {
        auto& vertex=model.pmxVertices[4];const auto original=vertex.weightType;
        bool rejected=false,unchanged=false;
        try {
            vertex.weightType=static_cast<mdl::PmxWeightType>(type);
            const Proof pending(app,raw);
            try {auto value=MeshDefinitionSnapshot::Capture(raw);(void)value;}catch(const MeshDefinitionUnsupported&){rejected=true;}
            // Compare before restoring the deliberately injected field.
            unchanged=vertex.weightType==static_cast<mdl::PmxWeightType>(type);pending.Verify();
        } catch(...) {vertex.weightType=original;throw;}
        vertex.weightType=original;
        Check(rejected&&unchanged,"runtime-only QDEF or unknown type is explicitly rejected without changing the model");
    }
    std::puts("COVERAGE runtime QDEF/unknown injection only; this is not PMX 2.1 file support");
}
}
void RunImpl(MMDApp* app) {
    wchar_t path[32768]{};const auto length=GetEnvironmentVariableW(L"MMS_SCENE_TEST_MODEL",path,32768);
    Check(length&&length<32768,"original mesh definition fixture supplied");LoadModelFile(app,path,false);
    auto* raw=app->ModelSlot(0);Check(raw&&!app->ModelSlot(1),"one real PMX or PMD loaded in slot zero");
    auto& model=*mdl::Mdl(raw);const bool pmx=model.physicsMode==2;
    Check(model.vertexCount==(pmx?5u:4u)&&model.indexCount==6&&model.materialCount==2&&model.morph0Count,"fixture has two material segments and native position restore data");
    app->CurrentFrame()=7;app->SceneModified()=0;const auto original=ReadBind(app,model);
    std::unique_ptr<MeshDefinitionSnapshot> old;
    {const Proof proof(app,raw);old=MeshDefinitionSnapshot::Capture(raw);Check(old&&Matches(*old,original),"all loaded bind vertices active influences UVs triangles and material segments match BIND");InvalidReads(*old);proof.Verify();}
    auto context=BoneTransformContext::Create();Check(bool(context),"private CPU math context is available");
    EvaluationTime time;Check(EvaluationTime::FromFrame(5.25,time),"fractional geometry frame is valid");
    BoneSceneModelInput input{0,{123,456,1},raw};
    std::unique_ptr<MeshSceneSource> scene;
    std::unique_ptr<MeshSceneFrame> geometry;
    std::vector<MeshEvaluationVertex> frozenGeometry;
    {
        const Proof proof(app,raw);
        scene=MeshSceneSource::Capture(&input,1);
        geometry=scene->EvaluateAt(time,*context);
        auto mesh=MeshDefinitionSnapshot::Capture(raw);
        auto definitions=MorphDefinitionSnapshot::Capture(raw);
        auto tracks=morph_curve::SamplingState::Capture(raw);
        std::vector<float> weights(tracks->MorphCount());
        for(uint32_t i=0;i<weights.size();++i)Check(tracks->Sample(i,time.Frame(),weights[i]),"sample real model morph track");
        auto morph=MorphEvaluationFrame::Evaluate(*definitions,weights.empty()?nullptr:weights.data(),uint32_t(weights.size()));
        auto separate=MeshEvaluationFrame::Evaluate(*mesh,*morph,*geometry->Bones().ModelFrame(0),*context);
        Check(geometry->ModelCount()==1&&geometry->ModelFrame(0)&&geometry->ModelFrame(0)->VertexCount()==model.vertexCount,"real file gives one complete private vertex frame");
        for(uint32_t i=0;i<model.vertexCount;++i){MeshEvaluationVertex a,b;Check(geometry->ModelFrame(0)->VertexAt(i,a)&&separate->VertexAt(i,b),"read real model private vertex");
            Check(Bits(a.position,b.position)&&Bits(a.normal,b.normal)&&Bits(a.uv,b.uv)&&Bits(a.additionalUv,b.additionalUv),"coherent source matches separately sampled frozen model inputs");frozenGeometry.push_back(a);}
        proof.Verify();
    }
    Check(original.materials[0].firstIndex==0&&original.materials[0].indexCount==3&&original.materials[1].firstIndex==3&&original.materials[1].indexCount==3&&
        original.triangles[0].materialIndex==0&&original.triangles[1].materialIndex==1,"material transition occurs exactly between the two triangles");
    if(pmx) {
        Check(original.uvCount==1&&model.uvMorphCounts.byFamily[0]>0&&model.uvMorphCounts.byFamily[1]>0,"real PMX has base UV and first additional-UV restore families");
        for(unsigned i=0;i<4;++i)Check(unsigned(original.vertices[i].binding)==i,"real PMX loader exercises BDEF1 BDEF2 BDEF4 and SDEF");
        Check(original.vertices[3].sdefCenter==std::array<float,3>{{1,2,3}}&&original.vertices[3].sdefR0Offset==std::array<float,3>{{-3,-3,-3}}&&
            original.vertices[3].sdefR1Offset==std::array<float,3>{{1,1,1}},"SDEF loader weighted-pivot offsets are retained without a second rebase");
        Check(original.vertices[0].additionalUv[0]==std::array<float,4>{{1,2,3,4}}&&original.vertices[0].additionalUv[1]==std::array<float,4>{},"active UV group is transposed correctly and absent groups stay zero");
        std::puts("COVERAGE PMX 2.0 BDEF1/BDEF2/BDEF4/SDEF and one additional UV group loaded from file");
    } else {
        const uint32_t expected[]{0,1,2,2,1,3};for(unsigned i=0;i<6;++i){uint32_t index=999;Check(old->IndexAt(i,index)&&index==expected[i],"PMD four-byte host stride preserves original 16-bit numeric index");}
        Check(original.vertices[1].pmdWeightPercent==25&&original.vertices[3].pmdWeightPercent==-59&&original.vertices[3].weights[0]<0,"PMD raw signed percentage is not normalized or clamped");
    }
    UnsupportedTypes(app,raw);

    // Change the actual current pose and ask the existing CPU preparation path
    // to apply vertex/UV morphs. No GPU dispatch or alternate oracle is used.
    for(uint32_t i=0;i<model.morphCount;++i)model.morphs[i].value=0;
    for(uint32_t i=0;i<model.morphCount;++i)if(pmx?(model.morphs[i].type==1||model.morphs[i].type==3||model.morphs[i].type==4):i==1)model.morphs[i].value=.75f;
    model.boneTable[0].trans[0]=3.25f;model.boneTable[0].rotQuat[0]=0;model.boneTable[0].rotQuat[1]=0;
    model.boneTable[0].rotQuat[2]=.6f;model.boneTable[0].rotQuat[3]=.8f;
    ModelApplyMorphs(raw);SetPhysicsMode(raw,0,app->ModelSlots(),0);SetPhysicsMode(raw,1,app->ModelSlots(),0);
    const float identity[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};GpuModelInput packed;
    Check(PrepareGpuModelInput(app,raw,identity,packed,true),"real CPU preparation applies current vertex and UV morph values");
    Check(pmx?model.pmxVertices[0].position[0]!=original.vertices[0].position[0]:model.rawVertices[0].position[0]!=original.vertices[0].position[0],"working position actually differs from its bind value");
    if(pmx)Check(model.pmxVertices[0].uv[0]!=original.vertices[0].uv[0]&&model.pmxVertices[0].additionalUvByComponent[0][0]!=original.vertices[0].additionalUv[0][0],"both working UV0 and UV1 actually contain surface morph output");
    std::unique_ptr<MeshDefinitionSnapshot> morphed;
    {const Proof proof(app,raw);morphed=MeshDefinitionSnapshot::Capture(raw);Check(morphed&&Matches(*morphed,original),"capture restores only its private position UV0 and UV1 copy to bind values");Workers(*old,original,*morphed,original);proof.Verify();}

    auto& normal=pmx?model.pmxVertices[0].normal[0]:model.rawVertices[0].normal[0];const float previous=normal;
    std::unique_ptr<MeshDefinitionSnapshot> newer;Expected edited;
    try {normal=previous+.25f;edited=ReadBind(app,model);const Proof proof(app,raw);newer=MeshDefinitionSnapshot::Capture(raw);proof.Verify();}
    catch(...){normal=previous;throw;}
    normal=previous;Check(newer&&Matches(*newer,edited)&&!Matches(*old,edited)&&!Matches(*newer,original),"later static edit yields an independent mesh revision");
    Workers(*old,original,*newer,edited);

    // Save clean current state. Static definitions remain in the original model
    // file; PMM reloading must not invalidate any previously captured revision.
    for(uint32_t i=0;i<model.morphCount;++i)model.morphs[i].value=0;
    SeekModelFrame(raw,0,0);ModelApplyMorphs(raw);Check(PrepareGpuModelInput(app,raw,identity,packed,true),"restore native surface arrays before saving");
    app->CurrentFrame()=0;const auto project=std::filesystem::absolute(L"mesh-definition.pmm");
    Check(SaveSceneCopy(app,project.c_str()),"save real model in PMM");
    ResetAppState(app);Check(!app->ModelSlot(0),"delete original source model");Workers(*old,original,*newer,edited);
    auto detached=scene->EvaluateAt(time,*context);scene.reset();
    Check(detached->ModelFrame(0)->VertexCount()==frozenGeometry.size(),"frozen scene evaluates after real source deletion");
    for(uint32_t i=0;i<frozenGeometry.size();++i){MeshEvaluationVertex vertex;Check(detached->ModelFrame(0)->VertexAt(i,vertex),"read detached real vertex");
        Check(Bits(vertex.position,frozenGeometry[i].position)&&Bits(vertex.normal,frozenGeometry[i].normal)&&Bits(vertex.uv,frozenGeometry[i].uv)&&Bits(vertex.additionalUv,frozenGeometry[i].additionalUv),"source deletion preserves independent vertex result");}
    geometry.reset();context.reset();
    LoadModelFile(app,path,false);Check(app->ModelSlot(0)&&!app->ModelSlot(1),"a later load reuses source slot zero");
    auto& replacement=*mdl::Mdl(app->ModelSlot(0));
    (pmx?replacement.pmxVertices[0].normal[0]:replacement.rawVertices[0].normal[0])+=1.f;
    const auto replacementExpected=ReadBind(app,replacement);auto replacementSnapshot=MeshDefinitionSnapshot::Capture(app->ModelSlot(0));
    Check(replacementSnapshot&&Matches(*replacementSnapshot,replacementExpected)&&!Matches(*old,replacementExpected),"slot replacement does not retarget an old immutable snapshot");
    Workers(*old,original,*replacementSnapshot,replacementExpected);ResetAppState(app);
    wcscpy_s(app->state.envFileName,project.c_str());LoadSceneFile();Check(app->ModelSlot(0)&&!app->ModelSlot(1),"reload saved PMM into a fresh native model");
    const auto reloadedExpected=ReadBind(app,*mdl::Mdl(app->ModelSlot(0)));std::unique_ptr<MeshDefinitionSnapshot> reloaded;
    {const Proof proof(app,app->ModelSlot(0));reloaded=MeshDefinitionSnapshot::Capture(app->ModelSlot(0));Check(reloaded&&Matches(*reloaded,reloadedExpected)&&Matches(*reloaded,original),"PMM reload preserves original bind mesh definition");proof.Verify();}
    Workers(*old,original,*newer,edited);ResetAppState(app);
    Workers(*reloaded,reloadedExpected,*replacementSnapshot,replacementExpected);
    Check(Matches(*morphed,original),"capture made from a morphed working array also survives all source destruction");
    std::puts("Frozen mesh definitions: real BIND geometry, active skinning inputs, surface restore, read-only capture and independent lifetime passed");
}
void Run(MMDApp* app) {
    try {RunImpl(app);}
    catch(const std::exception& error) {std::fprintf(stderr,"mesh definition host exception: %s\n",error.what());Check(false,"mesh definition host completed without unexpected exception");}
    catch(...) {Check(false,"mesh definition host completed without nonstandard exception");}
}
}
#endif
